# N — Query-time algorithms and bake timing

## Outcome

Path, exception, lock, dead-code, and impact queries do work proportional to
the part of the graph they answer about, not the whole program, with
byte-identical output. Bake statistics show where bake time goes.

## Evidence and starting points

- Corridor BFS: `findCallerPaths` (`src/callgraph/PathSearch.cpp:305-325`)
  runs a forward BFS from every start over every reachable node, calling
  `calleeRefsOf` (which builds a vector, a set, and override closures) for
  each, on every query. `find_call_chain`, the exception oracle
  (`ControlFlowOracle.cpp:342`, `:395`), and both lock tools pay this.
- Dead code: `src/anneal/DeadCodeAnalyzer.cpp:25-150` walks in string space:
  `calleesOf(string)` copies each reachable edge into five `std::string`s,
  the alive set is a `std::set<std::string>` (`:39`), there are two full
  passes, and deferred virtual edges are pushed without dedup and rescanned
  every round through `getClassesForImpl` (a vector of strings) and
  `findNode`. The one-shot CLI recomputes this on every call because
  `QueryCache` lives only inside one process.
- `callerRefsOf` (`src/callgraph/CallGraph.cpp`, around `:299`) inserts every
  direct edge into a `std::set<pair>` even when the callee has no override
  bases and no function-pointer-return joins. `resolveIds`' `referenced`
  check (`PathSearch.cpp:80-91`) builds full expanded caller and callee lists
  only to test emptiness.
- `findImpact` (`src/impact/ImpactSearch.cpp`, around `:95-114`) computes
  `callerRefsOf` and the canonical string sort before the hub and work-budget
  checks that may discard them.
- Bake timing: `BuildStats` has no split between frontend parse and visitor
  time, and each TU gets three full `TraverseDecl` passes
  (`ControlFlowContextVisitor.cpp:1041-1046`). The 188 s cold bake on the
  testbed cannot currently be attributed.

## Work

1. Baseline: time `find_call_chain`, `query_throw_propagation`,
   `query_locks_held`, `analyze_dead_code`, and `impact_of_change` on the
   largest available index with `scripts/bench.py --cli`, recording binary
   revision and workload.
2. Corridor: compute the target's ancestor set first (reverse closure under
   the same edge filter, filling `callersMemo`, which the DFS reuses), then
   run the forward BFS only inside it. Every shortest path from a start to an
   ancestor of the target runs through ancestors only, so `minFromStart` is
   unchanged wherever the DFS reads it. State this invariant in a comment and
   in `docs/path-analysis.md`.
3. Dead code: walk ids via `calleeRefsOf`, keep a bitmap for the alive set,
   and index deferred virtual edges by class so each is released once when
   its class becomes constructed. Target O(V+E).
4. Build the `callerRefsOf` dedup set only when an expansion is possible;
   make `referenced` check `inEdges_`/`outEdges_` directly; move the impact
   hub/work checks before the caller materialization.
5. Bake timing: add parse and per-visitor time to `BuildStats` (summed
   across threads) and print it in `megascope index`'s JSON summary under a
   `timing` key; decide from the data whether merging the three traversals
   is worth a follow-up, and record the decision.

## Ownership and boundaries

Own `PathSearch.cpp`, `DeadCodeAnalyzer.cpp`, `ImpactSearch.cpp`, the
`CallGraph` query helpers named above, and `BuildStats`. Do not change
search semantics, limits, or stop reasons; B's contract in
`docs/path-analysis.md` stands.

## Acceptance

- Raw tool output is byte-identical before and after on the corpus, the
  CLI goldens, `tests/test_determinism.cpp`, and a scripted comparison over
  the largest available index.
- Before/after latencies from step 1 on the same workload, reported per
  tool; no tool slower beyond noise.
- The timing split appears in the index summary and is described in
  `docs/index-provenance.md` or the bench docs.
- Supported LLVM matrix passes.

## Deliverables

The algorithm changes, measurements, the bake timing split, and the
recorded decision on merging traversals.
