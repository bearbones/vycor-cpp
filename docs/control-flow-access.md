# Control-flow access for one-shot queries

Status: implemented on `main` (snapshot format v12). Owner: package F of
`docs/plans/2026-09-next/` (`F-control-flow-performance.md`). This page
is the benchmark report, the go/no-go decision, the layout of the v12
control-flow section, and the reproduction commands.

## The problem

Every `megascope` query verb is one process: start, load the index,
answer, exit. The graph section loads in a third of a second on the
938-TU llvm-project testbed, and a graph-only tool such as
`get-callers` answers in about half a second. The eight control-flow
tools (`query_call_site_context`, `query_exception_safety`,
`query_raii_scopes_at_callsite`, `query_throw_propagation`,
`query_all_path_contexts`, `query_nearest_catches`, `query_locks_held`,
`query_same_lock`) also need the control-flow section, and until v12 a
load of that section decoded all of its 7.06 million call-site
contexts into the resident index — hash maps by site, caller, and
callee, one million interned strings — to answer for one call site.
That decode was 2.3 of the 2.8 seconds such a query took, and 1.3 GB
of its 1.6 GB peak.

The plan's gate: a prototype earns its way into production only with
at least 2x lower median one-shot exact-site latency and 2x lower peak
RSS on the large fixture, identical normalized answers, and no
bake / refresh / full-dump regression above 10%.

## What changed

The control-flow section is now laid out so a read-only load can leave
it in the mapped file:

- The interner table is length-prefixed, so a reader steps over it.
- The set tables (try/catch scopes, guards, RAII locals) follow as
  before; they are small (13 MB decoded on the testbed) and are decoded
  by both kinds of load.
- The context records — 38 bytes each, the same nine u32 fields and
  two bytes as the resident form — are written stably sorted by call
  site id, so a site's contexts are one bisection of the record array
  and the resident form's insertion order within a site is kept.
- After the records: each string's offset in the interner table (random
  access by id), the string ids in string order (a name or spelling
  becomes an id by bisection), record positions ordered by caller id
  and by callee id, and the positions whose display name differs from
  the usr, ordered by display id (the by-name fallback the resident
  form keeps in its `*Display_` maps).

`SnapshotIO::load` with `LoadMode::ReadOnly` — the query verbs and
`diff` — validates the array lengths against the section and hands the
region to `ControlFlowIndex::attachMapped`; the `llvm::MemoryBuffer`
that maps the file is shared into the index and outlives the load. A
`Mutable` load — `index`, `serve`, the worker shards — decodes the
records into the resident form exactly as before and skips the orders,
so bake, refresh, and `reindex_tu` are unchanged in behaviour.

Every `ControlFlowIndex` query is written once over positions:
contexts_ indices in the resident form, record positions in the mapped
one. `contextsAtSite` is a bisection of the records; `contextsForCallee`
and `contextsForCaller` bisect the usr order and fall back to the
display order; `forEachContext` and `forEachContextRecord` stream the
records sequentially, so `dump` and the semantic diff's context
signatures read the file once, front to back. Nothing in the file is
trusted: every offset, id, and position is bounds-checked where it is
read, a record whose ids or set indices are out of range reads as
dead, and a region shorter than its counts is refused at load.
Mutation of a mapped index is a contract violation (asserted; a no-op
in release builds).

`ControlFlowIndex::interner()` is gone; `stringOf(id)` resolves a
`ContextRecord` field in either form.

## Measurements

All numbers are from one host (12 cores, NVMe, Linux 7.0), warm page
cache, both binaries built Release against the same LLVM 20 with the
same flags. The baseline binary is the F branch's merge base (`main`
after package E, format v11); the mapped binary is this change (format
v12). Both bake the same 938 TUs (7,056,195 call-site contexts). Query
numbers are the median of nine one-shot runs (three for the dump) run
back to back, baseline then mapped, at a load average of about four
(a game and a browser were open; the machine was not otherwise quiet,
and the two runs of each pair share the same conditions). The bake
numbers are single runs and carry a few percent of noise (a repeat of
the v11 bake during a period of heavy other load took 299 s and was
discarded); the one-TU refresh is the median of three interleaved
pairs.

Point queries (one call site, one function):

| workload | baseline wall | mapped wall | ratio | baseline RSS | mapped RSS | ratio | same answer |
|---|---|---|---|---|---|---|---|
| `graph_only` (get-callers) | 0.57 s | 0.58 s | 1.0x | 325 MB | 323 MB | 1.0x | yes |
| `exact_site` (query-call-site-context) | 2.96 s | 0.60 s | 4.9x | 1624 MB | 369 MB | 4.4x | yes |
| `per_function` (query-exception-safety) | 2.98 s | 0.60 s | 5.0x | 1624 MB | 424 MB | 3.8x | yes |

Path queries:

| workload | baseline wall | mapped wall | ratio | baseline RSS | mapped RSS | ratio | same answer |
|---|---|---|---|---|---|---|---|
| `path_context` (query-all-path-contexts, 20 paths) | 2.97 s | 0.61 s | 4.9x | 1624 MB | 401 MB | 4.0x | yes |

Full scans:

| workload | baseline wall | mapped wall | change | baseline RSS | mapped RSS | ratio | same answer |
|---|---|---|---|---|---|---|---|
| `full_dump` (dump --format ndjson, 7.06M lines) | 24.18 s | 21.73 s | -10% | 1322 MB | 354 MB | 3.7x | yes (as a multiset) |

Where the time went, from the CLI's `-v` load line: the baseline's
control-flow load was 2.46 s of the 2.96 s (`cf_contexts` 1.8 s,
`cf_interner` 0.17 s, `cf_set_tables` 0.01 s, the graph 0.35 s); the
mapped load is 0.38 s (`cf_contexts` and `cf_interner` 0 ms, the set
tables 0.04 s, the graph 0.35 s). A control-flow query now costs what
a graph query costs: the graph section is the floor, and the remaining
0.2 s is process start and the answer.

Bake, refresh, and size:

| cost | baseline (v11) | mapped (v12) | change |
|---|---|---|---|
| cold bake, 938 TUs, 12 threads | 188.6 s | 191.2 s | +1.4% |
| bake peak RSS | 5.64 GB | 5.68 GB | +0.6% |
| index file | 441.8 MB | 558.3 MB | +26% |
| warm refresh, nothing dirty (meta only) | 0.10 s | 0.10 s | 0 |
| warm refresh, one TU touched (median of 3) | 10.40 s | 10.73 s | +3% |

The file grows by the lookup arrays: 4 bytes per string for the
offsets and 4 for the sorted ids (8 MB), 4 bytes per record for each
of the caller and callee orders (56 MB), and 4 per record again for
each display order (52 MB — on a real bake almost every record's
display name differs from its usr, so those orders are nearly full).
The save's extra work is one counting-sort pass per order; a first
version with comparison sorts cost +5.8% on the bake and +12% on a
one-TU refresh, which is why the orders are built by counting.

## Decision

The gate asked for at least 2x on median one-shot exact-site wall and
peak RSS with identical normalized answers and no bake / refresh /
full-dump regression above 10%. Measured: 4.9x and 4.4x, answers
identical on every workload (including the two path workloads and the
dump), full dump 10% faster and at a quarter of the memory, bake +1.4%,
refresh within noise, index file +26%. Go: the mapped form is the
production read-only path.

What the decision trades: 116 MB more on disk per 7M contexts, a
save that walks the records four more times, and two code paths in
`ControlFlowIndex` (resident and mapped) that the parity tests keep
honest. What it does not change: `index`, `serve`, and the worker
shards still build and hold the resident form; no daemon, no cache
directory, no background process. A one-shot control-flow query on
the testbed is now 0.6 s and 370 MB where it was 3 s and 1.6 GB.

## Parity

`tests/test_mapped_control_flow.cpp` bakes two fixtures — deep_chains,
and the exception fixtures (`examples/exception_context`,
`examples/precision`, the corpus's `mixed_exception_protection`: try /
catch scopes, catch bodies, guards, RAII locals, macro-shared call
sites) — saves each, loads the file both ways, and checks:

- every query (`contextAtSite` both overloads, `contextsAtSite`,
  `contextForEdge` for every graph edge by usr and by display name,
  `contextsForCallee`, `contextsForCaller`, `protectedCallsTo`,
  `unprotectedCallsTo`, `callerNoexceptOf`) over every site, usr,
  display name, and function the fixture knows, plus names it does
  not, answers field for field the same;
- `forEachContext`, `allContexts`, and `forEachContextRecord` with
  `contextOfShape` produce the same sequence, and
  `ContextSignatures::build` the same signature for every edge;
- every control-flow tool, run through `runTool` over both loads for
  every function and every call site (with and without `caller`),
  serializes to the same payload;
- a mapped index survives being moved out of the `SnapshotData` that
  loaded it and the load's destruction;
- a damaged file is refused (truncation, a record count or display
  order past the section, an interner table longer than the section)
  or read safely (a record with a bad set index or string id reads as
  dead and the resident load refuses the same file; a bad string
  offset resolves to ""; bad ids in the string order and bad positions
  in the record orders are skipped; a sweep flipping bytes across the
  lookup arrays finds no fault).

The validation corpus (`scripts/corpus-run.py`, ctest `corpus`) and the
CLI goldens run through the query verbs, so they exercise the mapped
form against answers recorded from the resident one; `warm_refresh`
and the snapshot round-trip tests cover the `Mutable` path. The
determinism tests still hold: the resident form loaded from a v12 file
keeps the file's site order, which is the order the mapped form reads,
and every list a tool emits is sorted before it is emitted.

## Reproduction

The testbed is llvm-project's `lib/Support`, `IR`, `Analysis`,
`Transforms/Utils`, `Transforms/Scalar`, and the other directories the
`--source-re` below selects — 938 TUs, 94,788 functions, 352,639
relationships, 7,056,195 call-site contexts. Bake it with each binary
(the baseline is the merge base of the F branch; formats differ, so
each reads its own file):

```
RE='/lib/(Support|IR|Demangle|TableGen|BinaryFormat|Bitstream|Remarks|MC|Object|ProfileData|Analysis|AsmParser|Bitcode|TextAPI|DebugInfo|Transforms/(Utils|Scalar)|Linker|IRReader)/.*\.cpp$'
/usr/bin/time -f "%e s %M KB" $BASE megascope index --build-path $LLVM_BUILD \
    --source-re "$RE" --index llvm938-v11.vycs --force
/usr/bin/time -f "%e s %M KB" $NEW megascope index --build-path $LLVM_BUILD \
    --source-re "$RE" --index llvm938-v12.vycs --force
# no-op warm refresh, each
$BASE megascope index --build-path $LLVM_BUILD --source-re "$RE" --index llvm938-v11.vycs
$NEW  megascope index --build-path $LLVM_BUILD --source-re "$RE" --index llvm938-v12.vycs
```

Then the matched workloads, nine reps each (three for the dump), on a
warm page cache:

```
scripts/cf-access-bench.py --binary $BASE --index llvm938-v11.vycs \
    --target llvm::errs --label baseline --out cf-baseline.json
scripts/cf-access-bench.py --binary $NEW --index llvm938-v12.vycs \
    --target llvm::errs --label mapped --out cf-mapped.json
scripts/cf-access-bench.py --compare cf-baseline.json cf-mapped.json
```

The script takes a target function (`--target`; the runs above used
`llvm::errs`, a hub with thousands of callers, so the per-function and
path workloads have real fan-in to bound), its first caller's call
site, and that caller as the path workloads' entry point, records wall / user / system time,
peak RSS and page faults from `/usr/bin/time`, the per-section split
from the CLI's `-v` line, and a digest of each normalized answer
(`indexScope.bake` and the index path stripped; dump lines sorted).
`--compare` prints the ratios, whether the answers match, and the
verdict against the gate (`--gate-wall`, `--gate-rss`, `--regression`).
