# K — Answer honesty and bounded results

## Outcome

A query about a function the index does not know says so. No list a tool
returns can grow without a cap, and every capped list says it was cut.

## Evidence and starting points

- `resolveIdentity` (`src/query/Identity.cpp:219-221`) falls back to
  `return name->str()` when a name matches no node. The tool then answers
  about a name nothing refers to: `megascope get-callers --name does_not_exist`
  returns `{"callerCount":0,"callers":[],"status":"ok",...}` with exit 1, and
  `examples/deep_chains/cli-golden/not-found.txt` locks this in. Over MCP
  the `status` is what an agent reads, and `ok` with zero callers reads as
  "nobody calls this": a false dead-code or attack-surface conclusion from a
  typo.
- `get_callers`, `get_callees` (`src/query/GraphTools.cpp:190-263`),
  `list_callback_sites` (`:541-593`), and `list_concurrency_entry_points` have
  no `limit`/`offset` and no `truncated` field. Only `search_functions`
  (`:90-181`) and `analyze_dead_code` are capped. One hub function (a logger,
  an allocator) can flood an agent's context.
- Parameter names differ: the target function is `name` in lookup, callers,
  and callees (`GraphTools.cpp:649`, `675`, `713`) but `function` in the
  exception, lock, and channel tools; caps are `limit`, `max_results`
  (`ImpactTools.cpp:470`), or `max_paths`.
- `megascope-cli-review.md` §4.4 (`--distinct`) and `mcp-review.md`
  (paging on callers/callees, `did_you_mean`) are still open.

## Work

1. Unknown names: when a name matches no node and no edge endpoint, return
   `notFoundError` with `didYouMean` (up to 5 candidates, reusing
   `search_functions`' ranking at `GraphTools.cpp:83-181`). Keep the
   name-only fallback only where a name legitimately appears as an edge
   endpoint without a node (external or unresolved callees), and mark that
   case in the payload (`resolvedAs: "name"`). Apply in `resolveIdentity`
   so every tool gets it.
2. Paging contract, defined once in `src/query/` and documented in
   `docs/result-contract.md`: `limit` (default cap per tool, e.g. 200),
   `offset`, `total`, `truncated`; ordering is the existing canonical order
   (`docs/deterministic-output.md`). Apply to every list tool that lacks it.
   `--distinct` for callers/callees collapses multiple call sites per
   function and reports `siteCount`.
3. Aliases: accept `function` wherever `name` is the target parameter and
   vice versa, and `limit` as an alias for `max_results`; keep the old names.
   The schema lists the canonical name; the alias is documented.
4. Refresh the CLI goldens and corpus witnesses; the exit code for the
   not-found case stays 1 but the `status` becomes `not_found`.

## Ownership and boundaries

Own `Identity.cpp`, the paging helper, and the per-tool schema changes in
`Registry.cpp`. J adds MCP annotations to the same registrations; coordinate
the file. Do not change path-search semantics (the `max_paths` family stays
as is).

## Acceptance

- Unit tests: a misspelled name returns `not_found` with a suggestion that
  includes the intended function; an external callee known only by name
  still resolves.
- Every list-returning tool has a test showing the cap, `truncated: true`,
  and a second page that continues the canonical order without gaps or
  repeats.
- Updated goldens reviewed line by line in the PR; corpus passes;
  supported LLVM matrix passes.

## Deliverables

`not_found` with suggestions, the shared paging contract, aliases, updated
goldens and `docs/result-contract.md`.
