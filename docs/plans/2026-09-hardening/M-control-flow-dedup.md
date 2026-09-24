# M — Control-flow context dedup

## Outcome

Each distinct call-site context is stored once, however many TUs include the
header it lives in, cutting index size, bake memory, warm refresh, and
`dump` time without changing any answer. This is measurement-gated.

## Evidence and dependencies

- `ControlFlowIndex::insertStored` (`src/callgraph/ControlFlowIndex.cpp:236-265`)
  appends every context it is given; there is no dedup key.
  `ControlFlowContextVisitor.cpp:405-410` records every call site outside
  system headers in every TU, including inline bodies in project headers.
  Edges, by contrast, are deduplicated at insert with per-TU refcounts.
- The 938-TU testbed (docs/control-flow-access.md) has 7.06M contexts
  against 352,639 deduplicated edges, and the control-flow data is about 75%
  of the 558 MB index file. Some of that ratio is legitimate (several call
  sites per edge); the rest is likely per-TU duplication of header code.
  This is inferred from the counts, not measured.
- Likely side effect: `query_call_site_context` on a header call site may
  answer `ambiguous` with N identical candidates
  (`src/query/ExceptionTools.cpp:183-187`). Unverified.
- Depends on H for the format version allocation; independent of I–L.

## Work

1. Gate measurement: on the testbed (or the largest available fixture),
   run `megascope dump`, compute the number of distinct keys
   `(caller, callee, site, scopeSet, guardSet, raiiSet, noexcept,
   insideCatch)` versus total records, and publish the ratio with the exact
   commands. Also confirm or refute the `ambiguous` side effect.
2. Go/no-go: proceed only if deduplication removes at least 40% of records
   on the representative fixture. Otherwise publish the measurement and
   stop; that is a complete outcome.
3. Implement dedup at insert: a hash of the key above to a record index, a
   per-record contributor-TU refcount (mirroring edges), `removeTU` /
   `removeTUs` decrementing and dropping at zero, `absorb` merging shard
   records into existing keys. Records that differ because of macro state
   (different guard/scope sets) stay separate by construction.
4. Storage: the v12 mapped layout keeps one record per key; per-TU
   provenance moves to a side table read only by mutable loads. Allocate the
   format version at merge (after H if H lands first). Replace the `tuPath`
   tie-break in canonical ordering (`ControlFlowIndex.cpp:609-610`) with an
   order that no longer depends on which TU contributed first.
5. Parity: the mapped and resident forms answer identically; incremental and
   clean bakes answer identically (`scripts/warm-refresh-check.py`); any TU
   order and thread count answers identically (`tests/test_determinism.cpp`).

## Ownership

Own `ControlFlowIndex.*` storage and its snapshot section. Coordinate the
header/version with H. Do not change what the visitor records.

## Acceptance

- The gate measurement is in the PR with reproduction commands, whether go
  or no-go.
- If implemented: matched before/after figures for index size, cold bake
  wall and peak RSS, one-TU warm refresh, `dump` time, and one-shot
  `query_call_site_context` latency, on the same binary revision and
  workload; any regression above 10% explained.
- All parity checks in step 5, the corpus, and CLI goldens pass (goldens
  change only where duplicate candidates disappear, and each such change is
  explained).
- Supported LLVM matrix passes.

## Deliverables

The duplication measurement and decision; if go, deduplicated storage, the
format note in `docs/control-flow-access.md`, and parity tests.
