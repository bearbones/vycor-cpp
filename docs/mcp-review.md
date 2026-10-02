# MCP Server Review — Improvements from an LLM-Consumer Perspective

> **Historical.** The MCP server (`megascope serve`, `src/mcp/`,
> `reindex_tu`, `scripts/mcp-smoke.py`) was removed on 2026-10-02; the
> CLI verbs and `megascope batch` answer the same tools
> (`docs/megascope-usage.md`). This review is kept as a record.

All findings below are grounded in the run captured under
`scripts/mcp-smoke-out/` against the `examples/deep_chains/` fixture
(call graph: 1696 nodes, 66 edges, 34 call sites). See
`scripts/mcp-smoke-out/summary.md` for the request/response index.

The implementation is otherwise functional — every request was answered,
protocol framing is correct, and JSON-RPC error codes are reasonable.
This document focuses on rough edges that will bite an LLM consumer.

---

## Per-tool findings

### 1. `lookup_function` (`src/mcp/McpTools.cpp:142`)

Works. Returns `qualifiedName`, `file`, `line`, `enclosingClass`,
`isVirtual`, `isEntryPoint`. Missing-function case returns
`isError: true` with a clear message (`11-lookup_function.json`).

Gaps:
- No disambiguation when a name matches multiple overloads — the node
  lookup uses qualified name as a single key.
- No source snippet or signature. An LLM that wants to reason about
  parameters needs to open the file itself.
- `isEntryPoint` is redundant with a hypothetical `list_entry_points`
  tool that doesn't exist today.

### 2. `get_callees` (`src/mcp/McpTools.cpp:170`)

Works. Response includes `callSite`, `kind`, `confidence` per edge
(`04-get_callees.json`, `16-get_callees.json`) — exactly the shape
needed for navigation.

**Semantic trap:** `min_confidence` is ordered by
`confidenceRank` (`Unknown < Plausible < Proven`). Setting
`min_confidence: Plausible` on `Pipeline::run` returned *both* the
Plausible `FunctionPointer` and the Proven `DirectCall`. For an LLM,
"minimum Plausible" reads as "only Plausible" — the intent is usually
"show me only the uncertain ones I need to audit". The description
at `src/mcp/McpTools.cpp:624` should either rename this to
`max_uncertainty` / `only_confidence`, or explicitly document the
inclusive semantics.

No pagination. On a 1000-callee fan-out this becomes a problem.

### 3. `get_callers` (`src/mcp/McpTools.cpp:210`)

Symmetric to `get_callees`; same `min_confidence` semantic trap
(`src/mcp/McpTools.cpp:646`).

### 4. `find_call_chain` (`src/mcp/McpTools.cpp:246`)

**Biggest LLM-ergonomics issue in the whole server.** Response is a
list of lists of function names (`06-find_call_chain.json`,
`17-find_call_chain.json`):

```json
"paths": [["main", "Pipeline::run", "stage1_ingest", "stage2_parse",
           "stage3_transform", "stage4_dispatch", "stage5_sink",
           "Registry::invoke"]]
```

Every hop's kind and confidence are dropped. For a chain built to
stress exactly this — six layers, each mixing Proven and Plausible —
the caller learns the path exists but not where the uncertainty
actually sits. To answer "is the path from `main` to `Registry::invoke`
entirely Proven, or does one hop go through a virtual dispatch?" the
LLM has to issue one `get_callees` call per node on the path.

Concrete proposal: annotate each hop. At `src/mcp/McpTools.cpp:310-316`,
emit an array of edges instead of an array of names:

```json
"paths": [[
  {"from": "main", "to": "Pipeline::run",
   "kind": "DirectCall", "confidence": "Proven",
   "callSite": ".../main.cpp:10:3"},
  ...
]]
```

Also: the DFS walks reverse edges (`callersOf`) but there's no way to
restrict the walk to a confidence floor — a deep-chain LLM can't say
"only show me Proven paths". Add `min_confidence` and `edge_kinds`
filters consistent with `get_callees`.

Finally: a tight `max_depth` silently truncates. Step 13 asked
`max_depth=2` and got one path of length 3 (`runAsync → Scheduler::schedule
→ cbs::startupHook`). This is `max_depth=2` in the code but the path
has 3 nodes / 2 edges; the off-by-one ambiguity in the description
("Maximum chain length", `src/mcp/McpTools.cpp:669`) should clarify
whether it counts nodes or edges.

### 5. `query_exception_safety` (`src/mcp/McpTools.cpp:340`)

Works. Returned `protection: never_caught` with `caughtPaths: 0`,
`totalPaths: 1` for `stage4_dispatch` (`07-query_exception_safety.json`).

The `summary` string has a stray double-space: `"stage4_dispatch
throwing  is NOT caught ..."` — the `exception_type` parameter was
absent and got interpolated as empty, leaving `"throwing  is"`. See
the summary builder where `exception_type` is blank.

No breakdown per-path: which specific call chains are uncaught? An LLM
asked "where does this leak out" can't answer from the response.

### 6. `query_call_site_context` (`src/mcp/McpTools.cpp:388`)

**Silent-failure mode.** Asked with `call_site: "nope"` (step 14) the
response was a success with every field zero/empty
(`14-query_call_site_context.json`):

```json
{"callSite":"nope", "callee":"", "caller":"",
 "enclosingGuardCount":0, "enclosingScopeCount":0,
 "enclosingScopes":[], "isUnderTryCatch":false, ...}
```

Likewise, a syntactically valid but unknown site
(`stage4_dispatch.cpp:21:5`, step 8) returned the same shape with no
indication the site wasn't found. The parse/lookup at
`src/mcp/McpTools.cpp:395` should:

1. Validate the `file:line:col` format and return `isError: true` with
   a message on malformed input.
2. Distinguish "no such call site indexed" from "call site found, no
   enclosing try" — e.g. return `isError` or add a `resolved: false`
   field.

Current behavior makes debugging exception-safety workflows painful:
the LLM gets a plausible-looking empty answer with no hint that the
site was never indexed.

### 7. `analyze_dead_code` (`src/mcp/McpTools.cpp:443`)

Functionally correct but response shape is actively hostile to LLM
use. From the deep_chains run:

- `totalFunctions: 1696`
- `aliveCount: 26`
- `optimisticallyAliveCount: 9`
- `deadCount: 1661` — and the entire `dead` array is returned inline.

The JSON payload is **≈120KB** (`09-analyze_dead_code.json`). The bulk
is stdlib template instantiations
(`std::basic_string_view::operator[]`, `std::vector::_M_fill_insert`,
…). An LLM pulling this through a context window will burn most of its
tokens on boilerplate.

Proposals, anchored at `src/mcp/McpTools.cpp:490-498`:
- **Filter out system-header functions by default** (file path starts
  with a standard include dir). Add `include_system: false` flag.
- **Paginate**: add `limit` + `offset` params. Return
  `{count, totalDead, truncated}` metadata.
- **Allow filtering** by `name_prefix` or `file_prefix` so an LLM can
  scope to project code.

As-is, the tool returns useful data but is unusable for anything but a
post-hoc script.

### 8. `get_class_hierarchy` (`src/mcp/McpTools.cpp:502`)

Works cleanly (`10-get_class_hierarchy.json`) — `derivedClasses`,
`virtualMethodOverrides` structured neatly.

Gap: doesn't surface base classes. `include_transitive` only goes
down, not up. An LLM asking "what's in `Plugin`'s lineage" gets
descendants only.

Gap: no node locations. `Plugin` is described by name only — no file
or line anchor.

---

## Cross-cutting

### Introspection gaps

- **No `list_entry_points`.** The server already has `entryPoints_`
  (`src/mcp/McpServer.cpp:28`) but exposes it only implicitly through
  `lookup_function.isEntryPoint`. An LLM that doesn't know what to
  ask about has no starting point.
- **No `graph_summary`.** Node/edge totals, top-fanout callers/callees,
  confidence histogram — invaluable for orientation, currently only
  visible on stderr (`call graph built (1696 nodes, 66 edges)`,
  `control flow index built (34 call sites)`).
- **No `get_neighbors` / iterative exploration.** An LLM navigating
  an unfamiliar codebase needs "show me K hops around this node",
  which is currently six round trips of `get_callees` / `get_callers`.

### Response shape

- **`find_call_chain` drops edge metadata.** Already covered above —
  this is the single highest-impact change in the critique.
- **`analyze_dead_code` response size.** Covered above. Pagination is
  the minimum fix; stdlib filtering is the right default.
- **Every tool wraps JSON in a `text` content block.** The payload is
  a JSON string inside a JSON response (e.g.
  `"text": "{\"calleeCount\":2,...}"`, `04-get_callees.json:20`). MCP
  allows `type: "json"` content blocks for this exact reason; the
  current double-encoding forces the client to `JSON.parse` twice. See
  `makeTextResult` at `src/mcp/McpTools.cpp:31`.

### Confidence filtering semantics

Same issue appears in three tools (`get_callees`, `get_callers`,
missing on `find_call_chain`): `min_confidence` with
`Unknown < Plausible < Proven` rank. For consumers the more useful
verb is usually "only show uncertain" or "hide Proven". Either rename
to `include_confidences: ["Plausible"]` (list) or add
`max_confidence`.

### Error handling

- **Protocol-level errors are well-formed** (step 15: unknown method
  returns JSON-RPC `-32601` with message, `15-unknown_method.json`).
- **Tool-level errors use `isError: true` content blocks**
  consistently (`11-lookup_function.json`, `12-get_callees.json`).
  That's the MCP-spec correct behavior; no complaint.
- **Silent no-ops are a real problem** (`query_call_site_context`,
  covered above). The pattern to break: a tool that successfully
  returns "nothing found" indistinguishably from "query was malformed".

### Protocol

- `initialize` accepts any `protocolVersion` and echoes back
  `"2024-11-05"` (`01-initialize.json`). No rejection of mismatches.
  Harmless for now but worth a TODO at
  `src/mcp/McpServer.cpp:74-88`.
- No progress notifications. `analyze_dead_code` and
  `find_call_chain` with generous `max_depth` on a real codebase could
  easily run for seconds; streaming progress would let an LLM
  UI surface "still working".
- Server startup spends time building two Clang tools before accepting
  any request (stderr: 12 file processes twice). Lazy construction of
  the control-flow index (only on first `query_call_site_context` /
  `query_exception_safety` call) would make `initialize` responsive.

### LLM ergonomics on `tools/list`

Descriptions are short (`02-tools_list.json`) and mention required
fields, but:

- `query_call_site_context` takes `"file:line:col"` but the
  description doesn't say the path must be absolute or match the
  graph's canonicalization. I had to guess.
- `get_callees` description documents `min_confidence` but not the
  `Unknown < Plausible < Proven` ordering that makes the parameter
  surprising.
- `find_call_chain`'s `max_depth` description "Maximum chain length"
  is ambiguous — nodes or edges? The DFS at
  `src/mcp/McpTools.cpp:282` compares to `depth` which starts at 0 at
  the target, so it's edges; say so.

### Errors that don't teach

`lookup_function` on an unknown name returns `"Function not found:
::does_not_exist"` (`11-lookup_function.json`). Fine for a human, but
doesn't suggest:

- Did you mean `<close match>` (fuzzy search)?
- Qualified name was expected; try without leading `::`.
- Use `tools/call` → `get_class_hierarchy` if you're looking for a
  method.

A small `did_you_mean` field on `isError` results (Levenshtein against
the node index) would dramatically cut back-and-forth for typos.

---

## Concrete proposals, ranked

1. **Annotate `find_call_chain` hops with `{kind, confidence,
   callSite}`** — `src/mcp/McpTools.cpp:310-316`. Biggest impact per
   line of code.
2. **Add `include_system: false` default + pagination to
   `analyze_dead_code`** — `src/mcp/McpTools.cpp:490-498`. Cuts
   response from ~120KB to a few KB on typical projects.
3. **Surface failure in `query_call_site_context`** —
   `src/mcp/McpTools.cpp:395`. Validate input, distinguish "site not
   indexed" from "site indexed, no try/catch".
4. **Add `list_entry_points` and `graph_summary` tools** — new entries
   in `getRegisteredTools()` (`src/mcp/McpTools.cpp:596`). One-shot
   orientation for an LLM.
5. **Rename or document `min_confidence` semantics** —
   `src/mcp/McpTools.cpp:624`, `:646`. Or add
   `include_confidences: string[]` as an alternative.
6. **Fix `query_exception_safety` summary string** to handle empty
   `exception_type` gracefully. Minor.
7. **Lazy control-flow index construction** —
   `src/mcp/McpServer.cpp:25-28`. Snappier `initialize`.
8. **Add `did_you_mean` to `lookup_function` errors** —
   `src/mcp/McpTools.cpp:151-156` (or wherever the not-found branch
   returns). Reduces round trips on typos.

Everything in the top 5 is achievable without changing the protocol
or the call-graph library; changes are localized to
`src/mcp/McpTools.cpp`.
