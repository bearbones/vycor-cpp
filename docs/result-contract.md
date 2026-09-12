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
| `unavailable` | the facts the tool needs are not loaded or were never indexed (the channel index when the adapter was started without channel types) | `error` |

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
  none. A function that lives in a failed TU is `not_found`, and a
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
cannot emit an unconditional claim.

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
- a script that expected exit 1 from a channel tool run without a
  channel index gets exit 3.

Scripts that classified errors by message prefix should read `status`;
the prefixes still happen to hold for the current messages but are no
longer a contract.

## Implementation

`ToolContext::facts` (`IndexFacts {coverage, bake, freshness}`) is set
by the adapter that owns the index: the query verbs from the loaded
meta (`freshness: unchecked`), the ephemeral bake from its in-memory
meta (`baked`, no `bake`), `serve` from the meta it saved or kept
(`baked`). `IndexFacts::of(meta, freshness)` builds it.

`runTool(entry, args, ctx)` runs the handler and passes the payload
through `completeResult(payload, ctx)`, which stamps `status` (from the
payload's shape and typed error status) and `indexScope` (from
`ctx.facts`). Every adapter calls `runTool`; a test that calls a handler
directly sees the raw payload. `statusOf` reads the stamped status back
for `exitCodeFor` and `wrapToolResult`.

The coverage gate is `ControlFlowOracle::queryExceptionProtection` and
`queryThrowPropagation`'s `indexComplete` argument: `verdictExhaustive
= search.exhaustive && unknownCount == 0 && indexComplete`. The
exception tools pass `ctx.facts.coverage.complete()`.

## Tests

`tests/test_result_contract.cpp`: one case per acceptance row (success,
complete empty, ambiguity, usage error, index failure, partial bake,
truncated search, absent semantic information), the exit-code table,
"a message change cannot change an exit code", the ndjson summary, the
TSV policy, batch per-request status, and the MCP mapping.
`examples/deep_chains/cli-golden/` records the shapes end to end;
`tests/test_megascope_cli.cpp` and `tests/test_mcp_server.cpp` cover
the adapters.
