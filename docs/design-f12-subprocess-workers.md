# Design: Subprocess Worker Isolation (review F12)

Date: 2026-07-07
Status: approved for implementation (operator decision: F12 before F8)
Scope: `bakeIndexes` multi-TU build path, `src/main.cpp` megascope wiring,
new worker mode. Queries, snapshot format, and the MCP surface are
untouched.

## Motivation

All TU parses run as threads in one process. Crash protection is a
`siglongjmp` out of the SIGSEGV/SIGBUS handler (`runCfToolGuarded`),
which works — the 938-TU llvm testbed bake survives 1 crash with it —
but is structurally unsound:

- Jumping out of a signal handler mid-`ClangTool::run` skips every
  destructor on the stack. If the crashing thread holds the `CallGraph`
  mutex, the next `addEdge` deadlocks the entire bake. If it holds the
  interner's `shared_mutex`, likewise.
- Leaked frontend memory accumulates per crash; survivable at 1, not at
  50 on a hostile codebase.
- Peak bake RSS is O(threads × largest AST) — measured 8.1 GB at 12
  threads (pre-interning; 4.5 GB after) — with no way to bound it.

Subprocess workers give true isolation (a poisoned TU costs exactly that
TU), reliable memory reclamation (worker exit returns AST memory to the
OS), and open the distributed-indexing path (a worker on another machine
is the same protocol).

## Architecture

```
parent (megascope --isolate-workers)
  ├─ splits files into batches
  ├─ spawns ≤ --workers concurrent workers:
  │     vycor-cpp megascope --bake-worker --worker-out DIR/shard-N.snap
  │                --build-path ... --source f1 --source f2 ...
  │     (worker = this same binary; bakes its batch with the EXISTING
  │      in-process pipeline at --threads 1..k, writes a v5 snapshot
  │      shard via SnapshotIO::save, exits 0)
  ├─ as each worker exits: load shard (SnapshotIO::load) and ABSORB it
  │     into the master indexes (id-remapped merge, see below)
  └─ crash handling: nonzero worker exit → bisect the batch (see below)
```

Key insight that makes this cheap: **the v5 id-preserving snapshot IS the
worker wire format.** Workers reuse `SnapshotIO::save` verbatim; the
parent reuses `SnapshotIO::load`. No new serialization code. Shards go
through the filesystem (a temp dir), which is simple, debuggable, and
retry-friendly; pipes can come later if shard I/O ever measures hot.

### Batching and scheduling

- Batch size: `max(1, files / (workers * 4))` capped at 32 — small
  enough for pipelined merging and cheap retry, large enough to amortize
  process spawn (~50 ms) and shard write.
- The parent keeps `--workers` (default: the `--threads` value) workers
  in flight; as one exits, its shard is absorbed on the parent thread
  while the next batch launches. Absorb cost is far below bake cost, so
  the parent never becomes the bottleneck (see Merge).
- Workers run the existing thread pool internally with a small thread
  count (`--threads` split across workers: `threads / workers`, min 1).
  Simplest correct initial version: workers are single-threaded and
  `--workers` = the old `--threads`; revisit only if spawn overhead
  measures high.

### Worker mode

- Hidden megascope options: `--bake-worker` (bool) + `--worker-out PATH`.
  In worker mode, megascope: loads the compilation DB, bakes its
  `--source` list via `bakeIndexes` (crash guard **still enabled** —
  first line of defense stays in-process so a benign recoverable fault
  doesn't cost a whole batch), writes the snapshot, prints nothing to
  stdout, exits 0. Exits nonzero on unrecoverable crash (default signal
  disposition kills the worker — that's the isolation working).
- Worker stderr: before parsing each TU it prints `WORKER-TU <path>` —
  the parent captures stderr per worker; on a crashed worker the last
  `WORKER-TU` line identifies the poison TU directly, so bisection is
  usually one step: re-dispatch the batch minus that TU, record the TU
  as crashed (same accounting as today's crash counter / BuildStats
  toolStatus -1).

### Crash/bisect protocol

1. Worker exits nonzero (or is killed by a signal).
2. Parent reads the worker's stderr log; the TU named by the last
   `WORKER-TU` marker is presumed poisoned.
3. Re-dispatch the batch minus that TU as a new batch; record the
   poisoned TU in BuildStats (`toolStatus = -1`) and the crash counter.
4. If a batch fails with no marker (spawn failure), split it in half and
   re-dispatch both halves; a single-TU batch that fails is recorded as
   poisoned. Bounded: each TU is retried at most twice.

### Merge (absorb) — id-remapped, reuses v5 machinery

New methods, mirroring what `SnapshotIO::load` already does internally:

- `CallGraph::absorb(const CallGraph &shard)` — build a remap
  `shardStringId -> masterStringId` by interning each shard string once
  (`shard.interner_.forEachString`), then:
  - nodes: merge with the existing addNode union semantics (field
    backfill, contributor-set union, tuNodes_ append);
  - edges: remapped `StoredEdge`s go through the `edgeIndex_` dedup
    probe so header-inlined edges dedup ACROSS shards exactly as they
    do across TUs today (refs add, tuEdges_ append);
  - relations: remapped id pairs inserted with the existing
    forward+reverse dedup semantics.
- `ControlFlowIndex::absorb(const ControlFlowIndex &shard)` — same remap
  for the cf interner; set tables dedup through the existing key maps
  (rebuild key per shard entry — tables are small); contexts append with
  remapped ids through `insertStored`.

Cost estimate at the 938-TU scale: interning ~370k unique strings once
(~100 ms total across all shards; shard interners overlap so later
shards are mostly hash-hits) + 6.37M context inserts at the measured
~0.4 µs `insertStored` cost (~2.5 s across the whole bake, pipelined
against ~150 s of parsing). Not a bottleneck.

`absorb` is also independently useful: it is the merge step for any
future distributed or incremental-shard scheme.

### What stays the same

- Non-isolated mode (the default, `--isolate-workers` off) is
  byte-for-byte today's in-process pipeline. The flag is opt-in until it
  has soak time; flipping the default is a separate, later decision.
- The snapshot the parent saves at the end, warm starts, `reindex_tu`
  (single-TU, stays in-process), all queries, all tools: unchanged.
- The crash guard stays compiled in both modes.

## Testing

- Unit: `absorb` correctness — two graphs/indexes built from disjoint
  and from overlapping TU sets absorb to exactly the result of a single
  combined build (node/edge/context counts, dedup refcounts, query
  results, provenance: removeTU after absorb behaves identically).
- Integration: worker round-trip on `examples/deep_chains` — isolated
  bake result equals in-process bake result (same assertions as the
  existing scenario tests).
- Crash path: a fixture TU that deterministically crashes the frontend
  is hard to make portable; instead unit-test the bisect bookkeeping
  with an injected fake worker runner (function pointer / std::function
  seam in the dispatcher), covering: clean batch, crash-with-marker,
  crash-without-marker split, single-TU poison, retry bound.
- Scale (harness): `bench.py` cold bake with `--isolate-workers` on the
  938-TU testbed — expect wall parity ±10% with in-process (parse
  dominates), peak parent RSS far below in-process (parent holds only
  indexes + one shard), worker RSS bounded per batch.

## Acceptance

1. 111+ tests green, both modes.
2. Isolated 938-TU bake: identical node/edge/call-site counts to
   in-process, wall within ±10%, parent peak RSS < 2.5 GB.
3. Poisoned-TU simulation loses exactly that TU.
4. Docs: review doc item row + AGENTS.md workflow note.

## Non-goals (this round)

- Distributed workers (protocol is ready for it; not built).
- Flipping `--isolate-workers` to default.
- Replacing the in-process crash guard.
- Pipe/shared-memory transport (filesystem shards until measured hot).
