# Call-site-accurate path analysis

Status: implemented on `main` (snapshot format v11). Owner: package B of
`docs/plans/2026-09-next/`. This page is the contract for the shared
path search (`callgraph/PathSearch.h`), the exception oracle built on it
(`callgraph/ControlFlowOracle.h`), and the path tools that consume both
(`find_call_chain`, `query_exception_safety`, `query_throw_propagation`,
`query_all_path_contexts`, `query_nearest_catches`, `query_locks_held`,
`query_same_lock`).

## The problem it fixes

```cpp
void target() { throw 1; }
int main(int argc, char**) {
  if (argc > 1) { try { target(); } catch (...) {} }
  else { target(); }
}
```

Before this package `query_exception_safety --function target` reported
`never_caught` with two uncaught paths. The oracle enumerated the two
edges but joined each of them to the control-flow context by *caller
name*, taking the first context stored for `main` whichever call site the
edge came from. Every consumer had its own traversal with its own
depth units and its own idea of what a cut-off search meant, and none of
them could say whether "no path" meant "no path exists" or "the search
stopped". Now the fixture reports `sometimes_caught` with one caught path
through the call at `4:25` and one uncaught path through the call at
`5:10`, and every path tool reports how its search ended.

## The shared search: `findCallerPaths`

```cpp
PathSearchResult findCallerPaths(const CallGraph &graph,
                                 const std::string &target,
                                 const std::vector<std::string> &starts,
                                 const SearchLimits &limits,
                                 CycleRule cycles = CycleRule::SimpleNodes,
                                 EdgePredicate filter = nullptr);
```

A reverse depth-first search from the target over caller edges
(`callerRefsOf`: stored edges plus the query-time virtual-dispatch and
function-pointer-through-return expansions), collecting every path from
a start to the target within the limits.

**Identity.** `target` and each start may be a USR or a display name. A
display name resolves through `usrsForName` to every node that carries
it, so a query for an overloaded name searches every overload and the
result says which one each path reaches (`PathHop::calleeUsr`). A USR
resolves to exactly one node. `targetKnown` / `startKnown` say whether
anything resolved; a result with either false has no paths and no stop
reasons, because nothing was searched.

**Hops.** Every path is a sequence of `PathHop`s in call direction, each
carrying the exact edge: `callerUsr`, `calleeUsr`, the display names,
`callSite`, `kind`, `confidence`, `execContext`, `indirectionDepth`.
Parallel edges through the same function pair at different call sites
are distinct paths. A start that *is* the target is not a path (no
zero-length paths).

**Depth** is counted in edges: a chain of N functions has N-1 edges, and
for a reverse walk an edge is a frame above the target. `maxDepth = 20`
admits up to 20 frames above the target. Every consumer now uses this
unit (`query_locks_held` previously counted nodes, so its `max_depth`
admitted one frame fewer).

**Limits** (`SearchLimits`, 0 = unlimited except where noted):

| Limit | Default | Meaning |
|---|---|---|
| `maxPaths` | 100 | paths to collect; a consumer may override (locks: 512, `find_call_chain`: 10) |
| `maxDepth` | 20 | edges per path |
| `maxFanIn` | 1000 | a non-target node whose *stored* in-degree exceeds this is not expanded; 0 disables |
| `maxWork` | 2,000,000 | DFS node expansions |

**Cycle rules.** `SimpleNodes`: no function repeats on a path (the
`find_call_chain` and exception-oracle rule). `SimpleEdges`: no edge
(caller, callee, call site) repeats, so a function may recur through
different edges (the lock tools' rule: a lock acquired inside a recursive
cycle is still observed).

**Stop reasons** (`PathSearchResult::stops`, a bitmask; spelled
`path_limit`, `depth_limit`, `work_budget`, `hub_pruned` in payloads):

| Reason | Set when | Paths missed? |
|---|---|---|
| `PathLimit` | `maxPaths` paths were collected and unexplored branches remained. Collecting exactly `maxPaths` paths with nothing left is not a stop. | yes |
| `DepthLimit` | a branch was cut at `maxDepth` and a walk longer than `maxDepth` from a start through the cut node to the target exists | only paths longer than `maxDepth` |
| `WorkBudget` | `maxWork` expansions were spent | yes |
| `HubPruned` | at least one node's ancestry was skipped for fan-in (listed in `skippedHubs` with name, usr, in-degree) | yes |

`complete()` = no `PathLimit`, `WorkBudget`, or `HubPruned`: every simple
path within `maxDepth` was enumerated. `exhaustive()` = `complete()` and
no `DepthLimit`: the paths are all the paths. `exhaustive` is the
precondition for any unconditional always/never claim over paths.

**Canonical order.** A node's callers are expanded sorted by (caller
usr, call site, kind, confidence, execution context, indirection depth),
so the order of the paths, and under a path limit the *subset* kept, do
not depend on TU input order or index insertion order. Skipped hubs are
sorted by usr. Multiple targets (an overloaded name) are searched in usr
order.

**Preserved optimizations.** The corridor prune (one forward BFS from
the starts, unbounded, recording the fewest edges from any start to each
node; the DFS expands only callers that can still complete a path within
`maxDepth`) and the dead-end memo (a node whose ancestry was exhausted
from depth d without reaching a start is not re-explored at depth >= d;
failures caused by the per-path exclusion, the path limit, or the work
budget are search-state-dependent and never memoized). The BFS is
unbounded so that a `DepthLimit` can be reported honestly: a caller
reachable from a start but too far to complete a path is a cut branch,
not a dead end.

## The exception oracle

`queryExceptionProtection` / `queryThrowPropagation` search with
`SimpleNodes`, then decide each path's outcome by walking it from the
target outward, in propagation order:

1. On a `ThreadSpawn` edge the exception never unwinds into the caller:
   `terminates` (an exception escaping a thread entry calls
   `std::terminate`). On an `AsyncTask` / `PackagedTask` edge it is stored
   and rethrown at a retrieval site the model does not follow: `unknown`.
   `Invoke` is synchronous.
2. The hop's control-flow context is joined on the exact (call site,
   caller USR), preferring the context recorded for the hop's callee
   (`ControlFlowIndex::contextForEdge`). No context indexed: `unknown`
   ("no call-site context indexed for ..."), never "unprotected".
3. The caller's try scopes enclosing the call, innermost first. Within a
   scope the first handler in source order that matches decides: a
   catch-all matches anything; an empty query type matches any handler;
   a typed handler matches the thrown type, a standard-library base of it,
   or a base recorded in the graph's class hierarchy. A matching handler
   whose body rethrows (`throw;` anywhere in the handler) is recorded in
   `rethrownAt` and the walk continues with the next enclosing scope.
   A non-rethrowing match: `caught`, with `caughtAt` / `caughtBy`.
4. Leaving the caller's body: `noexcept` / `throw()` callers give
   `terminates`; an unevaluated specification gives `unknown`; otherwise
   the next hop.
5. Past the entry point: `uncaught`.

A call inside a handler body is not protected by that handler's try (the
index pops the try scope before traversing handler bodies; it used to
list the try as enclosing). A call in an `if` condition, its init
statement, or its condition variable now has a context (the visitor
used to skip all three, so `if (f())` had no call-site context at all
and, on the LLVM testbed, most real paths ended `unknown`); it is not
under the `if`'s guard, since the condition runs before the branch is
chosen. `tryCatchesOnPath` / `guardsOnPath` still list every scope and
guard on every hop, matched or not.

**Verdicts** (`protection`):

| Value | Meaning | Requires |
|---|---|---|
| `always_caught` | every path is caught | exhaustive search, no unknown path |
| `never_caught` | no path is caught, at least one is uncaught | exhaustive search, no unknown path |
| `noexcept_barrier` | every path terminates at a noexcept or thread boundary | exhaustive search, no unknown path |
| `sometimes_caught` | a caught witness and an uncaught or terminating witness | two witnesses; never demoted |
| `observed_caught` | caught on every path examined, but the search stopped early or some path is unknown | |
| `observed_uncaught` | uncaught on some path, caught on none examined, same qualification | |
| `unknown` | no paths (target or entry points not in the graph, nothing reachable, everything pruned), or every path's outcome is unknown | |

`verdictExhaustive` (payload `exhaustive`) is `search.exhaustive && no
unknown paths`. The summary string appends `Search stopped: <reasons>.`
when the search did not run to completion.

`query_all_path_contexts` runs the same search and annotation with no
propagation walk (no `outcome` per path).

`query_nearest_catches` is a breadth-first walk over caller edges (callers
in canonical order, each edge examined once, node-level visited set for
enqueueing only), stopping each chain at the first edge whose context
has a try/catch; it reports the handler, the call site it encloses, the
canonical shortest chain and its hops, `maxDepth`, and a `depth_limit`
stop when callers remained beyond it. Previously a global visited set
skipped the check on later edges into an already-seen caller, so the
set of catches depended on edge order.

## Payload changes

Common to every path tool (`attachSearchFacts`): `complete`,
`exhaustive`, `stopReasons` (array, possibly empty), and `skippedHubs`
`[{name, usr, inDegree}]` when any hub was pruned. Package C owns the
common result contract; these are the fields it consumes.

| Tool | Added | Changed |
|---|---|---|
| `query_exception_safety` | `terminatingPaths`, `unknownPaths`, `max_paths` / `max_depth` / `max_fan_in` args | `protection` gains the observed verdicts; `uncaughtPaths` keeps its meaning of "not caught" (includes terminating and unknown) |
| `query_throw_propagation` | per path: `outcome`, `hops` (`from`/`to` display names, `fromUsr`/`toUsr`, `callSite`, `kind`, `confidence`, `executionContext` when asynchronous), `rethrownAt`, `stopAt` + `note` for non-caught outcomes; the same counts and args as above | `callChain` uses display names for every node (the target used to appear as its USR) |
| `query_all_path_contexts` | `hops`, `maxDepth`, the search facts, `max_depth` / `max_fan_in` args | `callChain` as above |
| `query_nearest_catches` | `maxDepth`, per catch `hops` and `callSite`, `max_depth` arg, the search facts | catches on later edges into a shared caller are no longer missed |
| `find_call_chain` | per hop `fromName` / `toName`, the search facts; `skippedHubs` entries gain `usr` | `from` / `to` stay USR strings |
| `query_locks_held` / `query_same_lock` | the search facts (`same_lock` combines both searches: stops OR-ed, `complete` / `exhaustive` AND-ed, hubs unioned); `skippedHubs` entries gain `usr` | `max_depth` counts frames above the target (was nodes: one frame fewer); `truncated` = the path cap cut the walk (unchanged meaning); lock contexts join on the exact edge, not the first context at the site; a lock inside a recursive cycle is observed (`SimpleEdges`, as before) |
| `query_call_site_context` and every handler record | `rethrows` on each handler | calls in `if` conditions are indexed (they were not) |
| `query_locks_held` / `query_same_lock` entry points | | display names resolve (the old walk interned entry names as if they were USRs, so a name that was not also a USR matched nothing and the answer was an empty path list) |

`examples/deep_chains/cli-golden/` records the new shapes.

## Snapshot format v11

The catch handler record gains a `rethrows` byte after `isCatchAll`.
`SnapshotIO::kFormatVersion` is 11; older files are rebuilt. This is
the one semantic fact the propagation order needed that the index did
not carry. Coordinated with package A (same owner); `docs/index-provenance.md`
notes the version.

## Limits of the model

- Paths are lexical: a path exists when the edges exist. Nothing here
  proves a path is feasible at run time (guards are reported, not
  evaluated).
- Asynchronous retrieval (`future::get`, packaged-task results) is not
  modeled: `unknown`.
- A typed handler matches through the hardcoded std hierarchy and the
  graph's recorded class hierarchy; a base class the index never saw
  defined is a miss (`uncaught`, not `unknown`).
- `rethrows` is conservative: any bare `throw;` in the handler subtree,
  including one in a nested handler or behind a condition.
- Virtual-dispatch expansions and function-pointer joins are edges like
  any other (with their `kind` / `confidence`); an indirect call with no
  callee identity is not followed.
- Hub pruning is by stored in-degree; the query-time expansions of a
  hub are not counted.

## Migration path for anneal's exception summaries

`anneal` keeps its own name-level exception facts
(`FunctionSummaryEntry` in `include/vycor/anneal/GlobalIndex.h`: merged by
qualified name, so overloads are conflated; `unguardedCalls`,
`hasUncaughtThrow`, `isNoexcept`), consumed by `analyzeExceptionEscape`
(`anneal/Analyzer.h`). The megascope side now has the USR-level facts
those summaries approximate. The path, as a follow-up (not part of this
package):

1. Key `FunctionSummaryEntry` by USR (the indexer already has the
   declaration; `clang::index::generateUSRForDecl`), merging per identity
   instead of per name, and record each unguarded call with its call site
   and callee USR (`CallSiteContext`'s join keys).
2. Replace the name-level "is this call guarded" test with
   `ControlFlowOracle::isCaughtByScope` over the same `TryCatchScope`
   records, so the handler matching (source order, catch-all, rethrow,
   inheritance) is one implementation.
3. Where anneal wants a cross-TU verdict ("can this throw escape the
   entry point"), bake the indexes it already parses (`bakeIndexes`) and
   call `queryThrowPropagation`, reading `verdictExhaustive` before
   reporting anything universal.
4. Retire `hasUncaughtThrow` in favor of `PathOutcome` counts.

## Handoff

- **C (common result contract):** the completeness fields are produced
  by `attachSearchFacts` (`query/Serialize.h`) from `PathSearchResult` /
  `PathSearchFacts`; the spelling of stop reasons is `stopReasonName`.
  Consumers should treat `exhaustive:false` as "bounded claim".
- **D (deterministic ordering):** path order is canonical inside the
  engine; `find_call_chain` and the lock tools no longer order anything
  themselves. D's remaining ordering work in `GraphTools.cpp` /
  `LockTools.cpp` is confined to the non-path tools.
- **E (anneal):** the migration path above.

## Measurements

See the package handoff (PR description) for the LLVM 938-TU numbers:
the rebaked v11 index and query latency for `find_call_chain`,
`query_exception_safety`, and `query_locks_held` against the v10 index
and the previous binary.
