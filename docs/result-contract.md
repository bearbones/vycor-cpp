# Shared result and completeness contract

Status: implemented on `main`. Owner: package C of
`docs/plans/2026-09-next/`. This page is the contract every megascope
transport (the CLI verbs, `batch`, and the MCP server) follows for a
tool result, and the interface the diff/impact tools (package E) and the
validation corpus (package D) build on.

Modules: `include/vycor/query/Tools.h` (`ResultStatus`, `IndexFacts`,
`ToolContext::facts`, the typed error constructors, `runTool`,
`completeResult`), `src/query/Registry.cpp` (the implementation),
`src/cli/MegascopeCli.cpp` (`exitCodeFor`, `emitToolResult`, `batch`,
`info`), `src/mcp/McpServer.cpp` (`wrapToolResult`).

## The problem this fixes

Before this contract a tool payload said what it found but not what
kind of answer it was. The CLI classified an error as "usage" or "empty"
by the first word of its message (`Missing`, `Requires`, `Invalid`), so
rewording a message changed an exit code; the MCP adapter reduced every
error to `isError: true` with the bare message. Nothing in a payload
said which index it came from, whether that index covered every
requested TU, or whether anyone had checked it against the sources. A
`never_caught` verdict over an index whose failed TUs held the only
handler read exactly like a proof.

## Contract

Every tool payload, on every transport, carries two members in addition
to the tool's own fields. Both are additive: no existing member was
removed or renamed, and every existing member keeps its meaning.

### `status`

One of five strings. Machine status is read from here, never from
prose.

| `status` | Meaning | Payload shape |
|---|---|---|
| `ok` | answered; the record list may be empty | the tool's payload |
| `ambiguous` | the identity names several functions or call sites; pick one and re-query | `ambiguous: true`, `candidates`, the tool's disambiguation fields |
| `usage_error` | the arguments are malformed: a parameter is missing, has the wrong type, or an invalid value | `error` |
| `not_found` | the arguments are well-formed and name something the index does not contain (a function, a call site, a channel, a channel site) | `error` |
| `unavailable` | the facts the tool needs are not loaded or were never indexed (the channel tools over a bake that registered no channel types; `reindex_tu` without a compilation database) | `error` |

An empty, complete answer is `ok` with an empty record list, not an
error. `not_found` is for a named thing that is absent; the message
says which. Handlers build errors with the typed constructors
(`usageError`, `notFoundError`, `unavailableError`, or
`errorResult(ResultStatus, message)`); the message text is free-form
and changing it changes nothing else. An error payload that reaches an
adapter without a `status` (an extension handler that bypassed the
constructors) is treated as `not_found`, the historical default.

In C++: `ResultStatus statusOf(const llvm::json::Value &payload)`,
`resultStatusName`, `parseResultStatus`, `isErrorStatus`.

### `indexScope`

Where the answer came from and how much of the requested scope the
index holds. Compact by design (one bake reference and five counts):
the per-TU rows stay behind `megascope info --files`.

```json
"indexScope": {
  "bake": "e3b0c442…@1788213045123456789",
  "freshness": "unchecked",
  "requested": 938, "indexed": 936, "partial": 1, "failed": 1,
  "complete": false
}
```

| Member | Meaning |
|---|---|
| `bake` | `<environment fingerprint>@<bake_start_ns>`: identifies the bake that wrote the index (`docs/index-provenance.md`). Absent when there is no saved bake to cite (the ephemeral `--source` mode, a `serve` that baked without an index file). `megascope info` reports the same string as `provenance.bake`. |
| `freshness` | `unchecked`: a saved index was loaded as-is and nobody compared it with the sources. `baked`: the facts were baked from the sources by this process (ephemeral mode; `serve`, whose warm start re-indexes dirty TUs before serving and whose `reindex_tu` re-parses on request). `unknown`: the adapter did not say (a handler called directly, as the unit tests do). No query verb validates an index; `megascope index` does. |
| `requested`, `indexed`, `partial`, `failed` | `IndexCoverage`: the selected TU set at the last save and its partition by parse outcome (`docs/index-provenance.md`). `requested == indexed + partial + failed`. |
| `complete` | `requested == indexed`: every requested TU parsed cleanly. |

### What "complete" means

Three different facts are reported, and none of them is whole-program
proof:

- `indexScope.complete`: every requested TU parsed cleanly. A partial
  TU contributed facts from an errored AST; a failed TU contributed
  none. A function that lives only in a failed TU is `not_found` to
  every identity-taking tool (see "Unknown identities" below), and a
  handler that lives there is invisible to every path tool.
- `complete` / `exhaustive` / `stopReasons` / `skippedHubs` on the path
  tools: the search facts of `docs/path-analysis.md`. `exhaustive`
  means every path within the indexed graph was enumerated; it says
  nothing about TUs the index does not hold.
- The model: paths are lexical, guards are reported but not evaluated,
  a `Plausible` edge is a candidate, virtual and pointer dispatch are
  over-approximated, asynchronous retrieval is unknown. See
  `docs/path-analysis.md` and `docs/mcp-usage.md`.

A universal verdict (`always_caught`, `never_caught`,
`noexcept_barrier`) requires an exhaustive search **and**
`indexScope.complete`. When either is missing the verdict is the
observed one (`observed_caught`, `observed_uncaught`) or `unknown`, the
counts and witnesses are reported as found, and the summary says which
condition failed. `sometimes_caught` needs two witnesses and is never
demoted. A capped search and an index missing a requested TU therefore
cannot emit an unconditional claim. The gate fails closed: a handler
whose adapter stated no facts (`freshness: unknown`) is treated as
incomplete, and a handler called directly gets an observed verdict.

The channel tools have their own precondition: a bake that registered
channel types. The adapters always hand handlers a channel index, so an
index baked without channel types is an empty one, and an empty answer
over it would claim there are no channels. `IndexFacts::channelsIndexed`
(from `SnapshotMeta::channelTypes`) is what the four channel tools
check; without it every one of them is `unavailable`.

### Unknown identities

A function identity (`name`/`function`, `usr`, `to`/`to_usr`,
`from`/`from_usr`, `fn_a`/`fn_b` and their `*_usr` twins) must name
something the index holds: a function node, or the endpoint of at least
one edge. An identity that names neither is `not_found`, on every tool
that takes one, because `resolveIdentity` (`query/Identity.h`) decides
it before any handler runs:

```json
{"status": "not_found", "parameter": "name", "name": "stage3_transfrom",
 "error": "Function not found: 'stage3_transfrom' names no function and no call-graph edge in this index (did you mean 'stage3_transform'?). ...",
 "didYouMean": [{"qualifiedName": "stage3_transform", "usr": "c:@F@stage3_transform#...",
                 "file": ".../stage3_transform.hpp", "line": 15}],
 "indexScope": {...}}
```

- `parameter` is the argument as it was spelled (an alias stays the
  alias); `name` is its value.
- `didYouMean` holds up to 5 functions (`kMaxSuggestions`), in this
  order and deduplicated: the `search_functions` ranking of the whole
  value, the same ranking of its unqualified tail (a wrong
  namespace or class), then the nearest unqualified names by edit
  distance, a transposition counting as one edit, within a third of
  the tail's length (at least 1, at most 3). It is present and empty
  when nothing is close. Deterministic, like every list here.
- Before this rule, an unknown name resolved to itself and the by-name
  tools answered it: `get_callers` with `ok` and zero callers, the
  path tools with an empty, incomplete search. Over MCP, `ok` with zero
  callers reads as "nothing calls this"; a typo became a dead-code or
  attack-surface conclusion. The exit code is 1 either way; the
  `status` is what changed.
- An **edge endpoint without a node** (an external or unresolved
  callee: some edge names it, no node does) still resolves. Its
  identity string is all the index holds for it, and in a real bake
  that string is a USR (`c:@F@ext_write#I#`), so it is reached by
  that USR, as `usr` or spelled as the name; a display name nothing
  recorded for it is `not_found`. The payload says the identity
  resolved to a bare endpoint with `resolvedAs: "name"` where a
  node's `usr` would be (`targetResolvedAs` beside `targetUsr`,
  `fn_a_resolvedAs` beside `fn_a_usr`). `lookup_function` still
  answers `not_found` for one, since it reports node metadata.
- The string table is not the test: it never forgets a string, and call
  sites and file paths live in it too. A name only a removed TU knew
  is `not_found`, as it is to a clean bake.
- `query_channels_for_function` applies the same test when a function
  has no channel sites: a known function without sites is an empty
  `ok`; an unknown one is `not_found` with `didYouMean`.

`suggestFunctions` and `rankFunctionMatches` (`query/Identity.h`) are
the shared helpers; `search_functions` ranks with the latter.

### Paging

Every tool that returns a record list without its own bound pages it
through one contract (`src/query/Paging.h`):

| Argument | Meaning |
|---|---|
| `limit` | records to return, at least 1; each tool has a default cap |
| `offset` | records to skip first, at least 0 (default 0) |

| Member | Meaning |
|---|---|
| `total` | records in the whole (filtered) list |
| `offset`, `limit` | the window applied |
| `returned` | records in this page |
| `truncated` | records exist past this page |
| `nextOffset` | the offset of the next page; present only when `truncated` |

The window is cut from the list's canonical order
(`docs/deterministic-output.md`), so consecutive pages continue that
order with no gap and no repeat, and the same arguments cut in the same
place on any bake of the same sources. An offset past the end is an
empty, untruncated page (`ok`, exit 1). `limit` below 1, a negative
`offset`, or a non-integer for either is a `usage_error`. The tool's
own count member (`callerCount`, `calleeCount`, `count`,
`targetCount`, `derivedClassCount`, `totalMatches`) keeps meaning the
whole list, not the page.

| Tool | Paged list | Default `limit` | Notes |
|---|---|---|---|
| `get_callers`, `get_callees` | `callers`, `callees` | 200 | `distinct: true` (CLI `--distinct`) keeps one record per function at the other end, the first site in canonical order, with `siteCount`; `callerCount` / `calleeCount` and `total` then count functions, and the payload carries `distinct: true` |
| `search_functions` | `matches` | 25 | `offset` is new; `limit` below 1 was clamped to 1 and is now a usage error |
| `list_callback_sites` | `targets` | 100 | each target lists at most `site_limit` sites (default 50) in canonical edge order; `siteCount` is the full count and `sitesTruncated` says whether the list was cut; `siteLimit` echoes the cap |
| `list_concurrency_entry_points` | `entries` | 200 | |
| `list_entry_points` | `entryPoints` | 200 | |
| `get_class_hierarchy` | `derivedClasses` | 200 | `virtualMethodOverrides` is not paged (bounded by the class's own methods) |
| `list_channels` | `channels` | 200 | |
| `query_channels_for_function` | `sites` | 200 | |
| `query_channel` | `producers`, `consumers` | 200 | one window pages both lists: `producerTotal` and `consumerTotal` replace `total` and `returned`, and `truncated` is set when either list continues |
| `analyze_dead_code` | `dead`, `optimisticallyAlive` | 500 | already paged (`totalDead`, `deadCount`, `offset`, `limit`); the same window now pages `optimisticallyAlive` too (`optimisticTotal`, `optimisticTruncated`). `total` and `returned` describe `dead`; `truncated` and `nextOffset` cover both lists, so following `nextOffset` reaches every record of either. Its `limit` keeps its historical range: 0 is counts only, which returns no records and no `nextOffset` (`truncated` still says whether records exist) |

Not paged, because another argument bounds them: the path tools
(`find_call_chain`, the exception path tools, `query_locks_held`,
`query_same_lock`) by `max_paths` / `max_depth` / `max_fan_in`
(`docs/path-analysis.md`), whose semantics this contract does not
change; `impact_of_change` by `max_results`; `query_nearest_catches` by
`max_depth`; `explain_ordering` by its walk bound; the call-site tools
by the site; `graph_summary` by its top-5.

### Parameter aliases

The schema lists each parameter once, under its canonical name; these
aliases are accepted too and keep working:

| Canonical | Alias | Tools |
|---|---|---|
| `name` | `function` | `lookup_function`, `get_callers`, `get_callees` |
| `function` | `name` | `query_exception_safety`, `query_throw_propagation`, `query_all_path_contexts`, `query_nearest_catches`, `query_locks_held`, `query_channels_for_function` |
| `max_results` | `limit` | `impact_of_change` (0 = all, its historical meaning) |

The canonical spelling wins when both are present. An error names the
spelling the request used (`parameter`). Aliases are argument names,
not schema properties: they work wherever arguments are JSON (MCP,
`batch`, `call --args`), while the CLI's `--flags` are derived from
the schema and take the canonical names. No schema lists an aliased
parameter as `required` (`query_channels_for_function` reports a
missing `function` itself), because the CLI's flag check and MCP
clients that enforce `required` would reject the alias before the
handler runs.

### Exit codes

The exit code is derived from `status` and the record list, never from
message text.

| Condition | Exit |
|---|---|
| `ok`, record list non-empty (or a scalar payload) | 0 |
| `ok`, record list empty | 1 |
| `not_found` | 1 |
| `usage_error` | 2 |
| `unavailable` | 3 |
| `ambiguous` | 4 |

The meanings are unchanged (0 results, 1 empty, 2 usage, 3 index, 4
ambiguous). One mapping changed: an `unavailable` error exited 1 before
(it was an error with no reserved prefix); it is an index problem and
now exits 3.

### Transports

- **CLI, `--format json`**: the payload, with `status` and
  `indexScope` inside it.
- **`--format ndjson`**: the leading `{"_summary": ...}` line carries
  every non-record member, so `status` and `indexScope` appear once,
  not per record.
- **`--format tsv`**: record rows are unchanged (records never carry the
  envelope). A scalar payload row gains `indexScope` and `status`
  columns (`indexScope` as compact JSON, the existing nested-value
  policy). An error prints a two-column block, `error` and `status`.
- **`batch`**: each response line is `{"id"?, "tool", "status", "exit",
  "result"}`; `result` is the payload as above and `status` mirrors
  `result.status`. A request the batch loop rejects itself (unparseable
  line, unknown tool) answers `{"id"?, "tool"?, "error", "status":
  "usage_error", "exit": 2}`.
- **MCP `tools/call`**: `content[0].text` is the JSON payload for every
  result, `isError: true` when `status` is an error status. Before this
  contract an error's text was the bare message; a client that parsed
  it reads `.error` now. `reindex_tu` answers a JSON payload
  (`{"status": "ok", "file", "edgesRemoved", "edgesAfter",
  "contextsRemoved", "contextsAfter", "indexScope"}`) instead of prose;
  its `indexScope` still describes the bake the server started from,
  because the single-TU re-parse does not report an outcome
  (`docs/index-provenance.md`, follow-ups).
- **`megascope info`**: adds `provenance.bake` and `freshness`
  (`unchecked`: info never compares the index with the sources).

`tools` and `dump` are not tool results and are unchanged.

### Compatibility

Existing scripts keep working: every member they read is still there
with the same meaning, `error` messages are unchanged, and the exit
codes for `ok`, `not_found`, `usage_error`, and `ambiguous` are what
they were. Two things to check:

- a script that treated an MCP error's text as the message reads
  `.error` of the JSON text now;
- a script that expected exit 1 from `query_channel` or
  `explain_ordering`, or an empty `ok` from `list_channels` or
  `query_channels_for_function`, over an index baked without channel
  types gets `unavailable`, exit 3.

Scripts that classified errors by message prefix should read `status`;
the prefixes still happen to hold for the current messages but are no
longer a contract.

Unknown identities, paging, and aliases (package K of
`docs/plans/2026-09-hardening/`) changed these; the exit codes did not:

- an unknown function name or usr is `not_found` (exit 1, as before)
  where it was `ok` with an empty record list, on every
  identity-taking tool; a script that read `callers` from such an
  answer finds `error` and `didYouMean` instead. The `lookup_function`
  message for an unknown name is reworded (it now comes from
  `resolveIdentity`);
- the list tools above return at most their default `limit`; a script
  that relied on the whole list past it follows `nextOffset` or passes
  a larger `limit`. No list in the goldens or the corpus reached a
  default cap;
- `search_functions` with `limit` below 1 is a `usage_error` (it was
  clamped to 1); `analyze_dead_code`'s `optimisticallyAlive` is now
  windowed by `offset`/`limit` like `dead`.

## Implementation

`ToolContext::facts` (`IndexFacts {coverage, bake, freshness,
channelsIndexed}`) is set by the adapter that owns the index: the query
verbs from the loaded meta (`freshness: unchecked`), the ephemeral bake
from its in-memory meta (`baked`, no `bake`), `serve` from the meta it
saved or kept (`baked`). `IndexFacts::of(meta, freshness)` builds it;
`coversRequested()` is the verdict gate (stated and complete).

`runTool(entry, args, ctx)` runs the handler and passes the payload
through `completeResult(payload, ctx)`, which stamps `status` (from the
payload's shape and typed error status) and `indexScope` (from
`ctx.facts`). Every adapter calls `runTool`; a test that calls a handler
directly sees the raw payload. `statusOf` reads the stamped status back
for `exitCodeFor` and `wrapToolResult`.

The coverage gate is `ControlFlowOracle::queryExceptionProtection` and
`queryThrowPropagation`'s `indexComplete` argument: `verdictExhaustive
= search.exhaustive && unknownCount == 0 && indexComplete`. The
exception tools pass `ctx.facts.coversRequested()`.

## Tests

`tests/test_answer_bounds.cpp`: an unknown name (and usr, and each
identity of a two-identity tool) is `not_found` with a suggestion that
includes the intended function on every identity-taking tool; an
edge endpoint without a node resolves by its USR with `resolvedAs`;
the aliases, and no schema requiring an aliased parameter;
every paged tool's pages stitched back together equal one uncut page,
`analyze_dead_code`'s two lists included, and its counts-only `limit: 0`
without a next page;
`distinct`; the per-target site cap; the validation errors.

`tests/test_result_contract.cpp`: one case per acceptance row (success,
complete empty, ambiguity, usage error, index failure, partial bake,
truncated search, absent semantic information), the exit-code table,
"a message change cannot change an exit code", the ndjson summary, the
TSV policy, batch per-request status, and the MCP mapping.
`examples/deep_chains/cli-golden/` records the shapes end to end;
`tests/test_megascope_cli.cpp` and `tests/test_mcp_server.cpp` cover
the adapters.
