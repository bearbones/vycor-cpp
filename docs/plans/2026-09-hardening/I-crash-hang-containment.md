# I — Crash and hang containment

## Outcome

A crashing or hanging TU costs that TU, never the run. No bake or server
can deadlock or wait forever on one input, and an interrupted run cleans up
after itself.

## Evidence and starting points

- In-process crash recovery uses `sigsetjmp`/`siglongjmp` from a
  process-wide `std::signal` handler (`src/callgraph/ControlFlowContextVisitor.cpp:42-74`,
  the same scheme in `CallGraphBuilder.cpp:30-80`; handlers installed at
  `ControlFlowContextVisitor.cpp:1144`, `:1226`). In a thread pool, a fault
  while any thread holds `CallGraph::mutex_` (e.g. in `addEdge`) or the malloc
  lock jumps out without releasing it: the pool deadlocks, destructors are
  skipped, and the dead TU's partial nodes and edges stay in the graph. There
  is no `sigaltstack`, so a stack overflow from deep templates faults again
  inside the handler. In `serve`, `bakeTU` (`:1212-1235`) runs this on the
  server thread.
- Workers have no timeout or memory cap: `ExecuteAndWait(..., SecondsToWait=0,
  MemoryLimit=0)` in `src/callgraph/WorkerPool.cpp:359-361` and
  `src/main.cpp:813-816`. A TU that hangs blocks its dispatch thread, and the
  parent waits forever on the result queue (`WorkerPool.cpp:217`). Poison
  and bisect react only to exits, so they never trigger.
- No SIGINT/SIGTERM handling anywhere in `src/`. An interrupted isolated bake
  leaks `/tmp/vycor-workers/*` (removed only on normal return,
  `WorkerPool.cpp:389`); a killed parent orphans its workers.
- User limits are narrowed with `static_cast<unsigned>`
  (`src/query/ExceptionTools.cpp:51`, `:56`; `GraphTools.cpp:316-318`), so
  `max_depth=4294967296` becomes 0, which means unlimited. `dfs` in
  `src/callgraph/PathSearch.cpp:157` is recursive, so a long chain can
  overflow the stack.
- `CallGraph`'s read-only guards are assert-only (`CallGraph.cpp:80`, `158`,
  `583`, `710`, `796`), so Release builds would mutate silently. No
  Release-reachable mutation path was found; the guards should still hold.

## Work

1. Reproduce first: a fake worker (the `isolatedRunner` test seam) that
   sleeps forever; a TU fixture that crashes a visitor while another thread
   is mid-insert (or an injected fault inside `addEdge`); a `max_depth`
   overflow test on a long synthetic chain.
2. Worker timeout: a per-batch deadline scaled by batch size
   (`--worker-timeout`, default generous, 0 = off). On timeout, kill the
   worker and treat the batch as a crash so the existing `WORKER-TU` marker
   logic poisons or bisects it. Optional `--worker-memory-limit` via
   `RLIMIT_AS`. Apply to both anneal and megascope dispatch.
3. In-process crashes: route crash-prone parses through subprocess workers
   by default wherever threads > 1 and for `reindex_tu` (J consumes this).
   Where in-process recovery remains (single-threaded, tests), replace the
   `siglongjmp` guard with `llvm::CrashRecoveryContext`, install a
   `sigaltstack`, and `removeTU` the crashed TU's partial contributions
   before recording its outcome. Document which mode is crash-safe.
4. Signals: on SIGINT/SIGTERM, stop dispatching, terminate child workers,
   remove the shard directory and any temp index file, flush the checkpoint,
   and exit with a distinct code. Child workers die with the parent
   (`PR_SET_PDEATHSIG` on Linux; a pipe-close check elsewhere).
5. Clamp every user-supplied limit to a documented maximum before narrowing
   (a shared helper in `src/query/`), and make the path DFS iterative, or
   bound its recursion by the clamped depth.
6. Turn the read-only `CallGraph` asserts into checks that also hold in
   Release (return without mutating and log once).

## Ownership and boundaries

Own `WorkerPool.cpp` spawn/timeout, the crash guard, signal handling in
`main.cpp`, and the limit clamp helper. H owns what gets saved after a
failed bake; J owns the `reindex_tu` protocol response.

## Acceptance

- The step-1 reproductions hang, crash, or overflow before and pass after.
- A timed-out TU is recorded with a `timeout` outcome, is retried under the
  existing retry policy, and is poisoned after the same number of failures
  as a crash.
- Ctrl-C during an isolated bake leaves no worker processes and no
  `vycor-workers` directory (scripted test).
- No change to results on the corpus, CLI goldens, or
  `scripts/warm-refresh-check.py`; supported LLVM matrix passes.

## Deliverables

Worker timeouts and memory limit, the replaced crash guard, signal cleanup,
limit clamping, the reproducing tests, and a short "failure modes" section
in `docs/design-f12-subprocess-workers.md`.
