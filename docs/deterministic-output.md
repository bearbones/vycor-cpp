# Deterministic output

Status: implemented on `main`. Owner: package D of
`docs/plans/2026-09-next/`. This page says, for every list a megascope
tool emits, whether its order is part of the contract and what key
orders it, and which outputs are sets whose order a consumer must not
rely on. `docs/result-contract.md` is the envelope; `docs/path-analysis.md`
is the path engine's order; this page is the rest.

## The rule

Every list a tool emits is in a documented order that depends only on
the facts in the index, never on the order the facts were indexed in.
Two bakes of the same sources — cold or warm, one thread or many, TUs
in any order — answer every query byte for byte the same. Where a list
is cut (`limit`, `offset`, `max_paths`), the cut falls in the same place
for the same reason.

Storage order is not that: `CallGraph::calleesOf` / `callersOf` return
edges in insertion order, `allNodes()` walks a hash map, and the
hierarchy, override, and channel maps follow insertion order, all of
which follow the bake's TU order and thread scheduling. Handlers that
emit from storage sort first (`sortEdgesCanonically`, `sortedNodes`,
`sortChannelSites` in `src/query/`); the path tools take the engine's
canonical order.

**Canonical edge order** (`canonicalEdgeLess`, `query/Serialize.h`):
caller usr, callee usr, call site, kind, confidence, execution context,
indirection depth. With one end fixed (`get_callees`, `get_callers`)
this is the other end's usr, then the call site. It is the key the path
search expands callers by (`docs/path-analysis.md`, "Canonical order"),
applied to a whole edge.

## Per tool

| Tool | List | Order |
|---|---|---|
| `search_functions` | `results` | tier (exact unqualified name, prefix, substring), qualified-name length, qualified name, usr; `limit` cuts after ordering |
| `lookup_function` | — | scalar |
| `get_callees` | `callees` | canonical edge order (callee usr, call site, ...) |
| `get_callers` | `callers` | canonical edge order (caller usr, call site, ...) |
| `find_call_chain` | `paths` | engine canonical order; under `max_paths` the kept subset is the canonical prefix (`docs/path-analysis.md`) |
| `query_exception_safety`, `query_throw_propagation`, `query_all_path_contexts` | `paths` | engine canonical order; `catches` by (frames from target, path segment, call site) |
| `query_nearest_catches` | `catches` | (frames from target, path segment, call site) |
| `query_call_site_context` | `enclosingScopes`, `guards` | innermost first, as recorded at the site |
| `query_raii_scopes_at_callsite` | `locals` | as recorded at the site: enclosing scopes innermost first, locals in declaration order |
| `query_locks_held`, `query_same_lock` | `paths` | engine canonical order; locks on a path innermost frame first; `skippedHubs` by usr |
| `analyze_dead_code` | `dead`, `optimistic` | (file, line, usr); `offset` / `limit` page over that order |
| `get_class_hierarchy` | `derivedClasses`, `overrides` | class names in name order; base methods in usr order, each method's overrides in name order |
| `list_entry_points` | `entryPoints` | as recorded by the bake (`--entry-point` order) or as passed to `serve` |
| `graph_summary` | `topFanoutCallers`, `topFanoutCallees` | count descending, then name; the histograms are objects |
| `list_callback_sites` | `targets`, `sites` | target name; sites within a target in canonical edge order |
| `list_concurrency_entry_points` | `entries` | canonical edge order (spawner usr, target usr, call site, ...) |
| `list_channels` | `channels` | channel id |
| `query_channel`, `query_channels_for_function` | `producers`, `consumers`, `sites` | (call site, channel id, function usr) |
| `explain_ordering` | — | scalar |
| `impact_of_change` | `changed`, `unknown`, `affected`, `entryPointsAffected`, `skippedHubs`, `unmapped` | changed and hubs by usr, unknown by name; affected by (depth, usr) — a function's `path` is its shallowest chain, ties by the callers' canonical edge order; entry points in affected order; unmapped ranges by (file, first line) |
| `diff` | `changes`, `moves`, `identity.ambiguous`, `identity.renameCandidates`, `routes.added`/`removed`/`unchanged`, `impact` | change kind (`function_removed`, `function_added`, `call_removed`, `call_added`, `call_changed`, `context_changed`), then usr, then (caller usr, callee usr); sites within a change by call site; moves and rename candidates by key; ambiguous groups by group; routes in path order (`docs/path-analysis.md`); `impact` as `impact_of_change` |
| any tool, `ambiguous` | `candidates` | by usr (a name), by caller usr (a call site) |
| `batch` | response lines | request order |
| `tools` | tools | registration order (`query/Registry.cpp`) |

`indexScope`, `status`, and every scalar member are order-free. JSON
object keys are sorted by the serializer on every transport.

## Sets

These outputs are complete but their order is **not** a contract; a
consumer that compares them compares as multisets:

- `megascope dump`: every call-site context and channel site in stored
  order (`ControlFlowIndex::forEachContext`: a loaded index walks its
  file's records, which a v12 snapshot keeps by call-site id, insertion
  order within a site). A warm-refreshed index and a clean rebuild hold
  the same records in different orders; `scripts/warm-refresh-check.py`
  compares them sorted.
- `megascope info --files`: the requested TUs in the selection's
  canonical order (absolute paths, deduped, sorted), which is stable,
  but the per-file rows are provenance, not a query answer.

## Ties

Where a key can tie, the tie-break is part of the order above, so a
cut at a `limit` boundary is reproducible:

- overloads share a qualified name: `search_functions` breaks the tie
  on usr;
- equal fan-in or fan-out: `graph_summary` breaks the tie on name;
- two functions at one location (a macro): `analyze_dead_code` breaks
  the tie on usr;
- two calls from one caller to one callee: the call site, then kind,
  confidence, execution context, indirection depth.

## Checks

- `tests/test_determinism.cpp` (`[determinism]`): the same fixture
  baked with one thread and with four, with the TUs in forward and in
  reversed order, and warm-refreshed (one TU removed and re-absorbed)
  answers a battery of every list-emitting tool byte for byte the same.
  The tie cases above are exercised on hand-built graphs inserted in
  both orders. These compare raw serialized payloads, not sorted lines.
- `scripts/warm-refresh-check.py` (ctest `warm_refresh`): after every
  refresh scenario, a set of tool queries over the warm index equals
  the same queries over a clean rebuild, raw stdout compared, and
  `dump` compared as a multiset.
- `scripts/cli-golden.py` (ctest `cli_golden`): the end-to-end shapes,
  compared as sorted normalized lines so the goldens hold across hosts
  and standard-library versions. It is the shape check, not the order
  check.
- `scripts/corpus-run.py` (ctest `corpus`): `docs/validation.md`; every
  expectation names the record it wants, in the order above.
