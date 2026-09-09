# Review: megascope as a CLI-first tool (usability and performance)

Date: 2026-09-02
Scope: `src/main.cpp` (megascope/prism wiring), `src/mcp/`, `src/callgraph/`
(Snapshot, bake, WorkerPool), docs and scripts that describe the agent
workflow. Anneal and morph are out of scope except where they share code.

Companion documents: `docs/callgraph-mcp-review.md` (2026-06, architecture
and scale; every item there is landed) and `docs/mcp-review.md` (LLM-consumer
ergonomics of the tool responses). This review starts from the operator
decision that the MCP server should stop being the primary entry point for
agents: a JSON-RPC tool result lands in the agent's context window whole,
where a CLI result can be piped through `jq`, `grep`, `head`, `sort -u` and
only the filtered residue enters the context.

---

## Summary

The move is cheaper than it looks. Every tool handler is already a pure
function `(json::Object args, McpToolContext ctx) -> json::Value` over
in-memory indexes; the only MCP-specific part is the `makeTextResult`
envelope that stringifies the payload into a `content[0].text` block. The
snapshot (`--snapshot`, format v7) already persists everything a query
needs and already does incremental refresh. A CLI is a second thin adapter
over the same handlers, and the MCP server becomes the first one.

Two things are not in place and decide whether the CLI is pleasant:

1. **The per-invocation load tax.** A one-shot command pays snapshot
   decode on every call: 2.3–2.6 s at the 938-TU testbed (322 MB, 6.37M
   call sites), of which the control-flow index is the majority and is
   not needed by most graph queries. Sectioned loading (skip the CF index
   unless the query needs it; skip mutation-only indexes) is the cheap
   first fix; a frozen mmap-able layout or an auto-spawned daemon are the
   escalations if a target codebase still measures slow.
2. **Output shape for pipes.** Compact JSON payload on stdout with nothing
   else, NDJSON for list-shaped results, meaningful exit codes, quiet
   stderr by default. Today stderr logs every request and stdout is a
   JSON-RPC envelope around a JSON string.

The rest of this document is the concrete plan, plus a handful of defects
and doc drift found while reading.

---

## Part 1 — What already makes this cheap

| Fact | Where | Why it matters for a CLI |
|---|---|---|
| Handlers are transport-neutral | `McpToolHandler` in `include/vycor/mcp/McpTools.h`; 21 entries in `getRegisteredTools()` | The CLI calls the same functions. No query logic is rewritten. |
| Each tool carries a JSON Schema | `inputSchema` per `McpToolEntry` | CLI flags, `--help` text, and validation can be generated from the schema — one source of truth for both transports. |
| Persisted index with stamps | `SnapshotIO` (v7), `stampFiles`, config-match check in `main.cpp` | "Index once, query many times" is the CLI model, and it already exists under the name `--snapshot`. |
| Query-time joins | virtual dispatch and function-return expansion in `calleesOf`/`callersOf` | Incremental `index` runs produce the same answers as a full rebuild; no phase barrier to re-run per query. |
| Whole-graph result cache | `QueryCache` | Irrelevant to one-shot use (nothing to cache across processes) but needed unchanged by the daemon option (Part 3). |
| Test coverage against handlers, not the wire | `tests/test_mcp.cpp` (1.7k lines) constructs `McpToolContext` directly | Tests carry over to the CLI unchanged. |

The identity contract (USR-keyed nodes, `ambiguous:true` responses with
candidates, `*_usr` / `*_filter` / `*_site` refinements) also carries over
verbatim; the CLI only needs a distinct exit code for the ambiguous case so
a script can branch without parsing.

---

## Part 2 — CLI design

### 2.1 Split the subcommand into `index` and query verbs

Today `megascope` is one action: bake (or warm start), optionally save,
then block on stdin. Make it a verb-taking subcommand:

```
vycor-cpp megascope index   --build-path B [--source ...|--source-list F|--source-re RE] [--index I] [--threads N] [--isolate-workers] ...
vycor-cpp megascope <tool>  --index I [tool flags...]
vycor-cpp megascope batch   --index I            # NDJSON requests on stdin, one response per line
vycor-cpp megascope serve   --index I [--mcp]    # the current stdio server, now one adapter among several
vycor-cpp megascope info    --index I            # meta only: config, file count, stamps, format version
vycor-cpp megascope tools                        # list tools with one-line descriptions
```

`llvm::cl::SubCommand` is single-level, so the verb should be peeled off
`argv` before `ParseCommandLineOptions` (or parsed as a leading
`cl::Positional` on the megascope subcommand); either way, `main.cpp`
grows a small dispatcher rather than 21 hand-written option blocks.

**Rename `--snapshot` to `--index`** (keep the old spelling as an alias).
The word "snapshot" and the header comment "a cache, never a source of
truth" describe the server's warm-start optimization. Under the CLI model
the file *is* the product: `index` writes it, every query reads it. The
format can stay exactly what it is; only the story changes.

**Default the index path** from the build path (for example
`<build-path>/.vycor/megascope.vycs`, overridable by `--index` or a
`VYCOR_INDEX` environment variable) so that a query needs only
`--build-path` or nothing at all when run from the build directory. Every
flag an agent has to remember is a flag it will get wrong.

**Ephemeral mode.** A query verb given `--build-path` and sources but no
index bakes in memory and answers — this is exactly what `prism --mode
query` does today (see 4.1), and it lets the same verbs serve quick
single-file investigations without a persisted index.

### 2.2 Generate per-tool flags from the schema

For a chosen tool, walk its `inputSchema.properties` and map each property
to `--<name>` (underscores to hyphens): strings take a value, integers are
parsed, booleans are bare flags, arrays repeat. Unknown flags error with
the valid list; `--help` prints each property's description (the same text
the MCP client sees). This keeps 21 tools' worth of flags in one place and
guarantees the CLI never drifts from the MCP surface.

Keep a generic escape hatch for full fidelity and for scripts:

```
vycor-cpp megascope call get_callers --args '{"usr":"c:@N@x@F@f#","min_confidence":"Plausible"}'
```

Spelling: expose tool names with hyphens on the command line
(`get-callers`) and accept the underscore form too.

### 2.3 Output contract

- **stdout is the payload, nothing else.** Unwrap `makeTextResult`: emit
  the JSON object the handler built, compact by default, `--pretty` on
  request. `llvm::json::Value`'s `operator<<` is already compact.
- **`--format json|ndjson|tsv`.** `json` is the current object. `ndjson`
  emits one record per line for list-shaped results (callers, callees,
  search matches, dead functions, channel sites, call chains as one path
  per line, dump records) with the scalar summary fields as a final or
  first `{"_summary":...}` line — so `grep`, `head -20`, `wc -l`, and
  `jq -c` work without loading the whole result. `tsv` for the flat
  tables (search, callers/callees, dead code) so `cut`/`awk` work; the
  `--dump-nodes` TSV is precedent and its "tabs cannot appear in USRs or
  qualified names" argument holds.
- **Stream, do not materialize.** NDJSON output should be written record
  by record to `raw_ostream`. `ControlFlowOracle::dumpIndexToJson` builds
  the entire index as one `std::ostringstream` string — at 6.37M call
  sites, a few hundred bytes per record puts that on the order of
  gigabytes of transient heap for a dump nobody can read in one piece
  anyway.
- **Exit codes:** 0 = answered with results; 1 = answered, empty (not
  found, no paths, no dead code) — the `grep` convention; 2 = usage or
  argument error; 3 = index missing, stale (config mismatch, format
  version), or unreadable; 4 = ambiguous identity (candidates on stdout).
  Agents branch on exit codes far more reliably than on parsing an
  `isError` field.
- **stderr is quiet by default.** `-v` restores the progress and bake
  chatter; `serve` keeps its current logging under `-v` only. The
  per-request `megascope: received method:` line in `McpServer::run` is
  noise even for the server.
- **Determinism.** Most results are already sorted (search hits, ambiguity
  candidates, dead-code entries). Make it a stated contract of the CLI
  output so diffing two runs is meaningful; add a sort anywhere a result
  currently follows hash-map order (some `callers`/`callees` lists follow
  insertion order — fine for one index file, unstable across re-indexes).

### 2.4 Source selection

Every document in this repo tells the user to write a Python snippet to
pull file lists out of `compile_commands.json`, and `scripts/bench.py`
implements `--source-re` for itself. Fold that into the tool:

- No `--source` at all means every entry of the compilation database
  (`CompilationDatabase::getAllFiles()`; the TODO in AGENTS.md).
- `--source-re <regex>` and the existing `--skip-paths` filter that set.
- `--source-list <file>` (one path per line, `-` for stdin) so
  `jq -r '.[].file' compile_commands.json | grep /Network/ | vycor-cpp megascope index --source-list -`
  composes.
- `--source` stays for explicit lists.

### 2.5 `batch` mode

A one-shot CLI pays the load tax per call; a daemon adds hidden state. The
middle ground is nearly free: `megascope batch` reads NDJSON requests
(`{"tool":"get_callers","args":{...}}`) from stdin and writes one NDJSON
response per line, in order, sharing one loaded index and one
`QueryCache`. It is the existing serve loop minus JSON-RPC framing. An
agent that wants twenty related queries writes them to a file and pays one
load; a shell pipeline can generate the requests. This covers most of the
"interactive session" value of the server without a resident process.

### 2.6 MCP becomes an adapter

`megascope serve --mcp` keeps the current stdio protocol unchanged for
clients that want it. Nothing is removed. `src/mcp/McpServer.cpp` shrinks
to the framing loop plus `initialize`/`tools/list`/`tools/call`; the tool
table, handlers, `QueryCache`, and identity resolution move to a
transport-neutral home (see 4.2).

---

## Part 3 — Performance for one-shot use

### 3.1 The load tax is the whole story

Measured in `docs/callgraph-mcp-review.md` on the 938-TU testbed: snapshot
load 2.3–2.6 s after the v5 id-preserving format, warm start 11.6 s total
(the remainder is stamping and re-save decisions), file 322 MB. Queries
themselves are 0.02–3 ms. For a one-shot CLI, load is 99% of latency, so
it is the only thing worth optimizing.

The cost is decode, not I/O: `MemoryBuffer::getFile` mmaps the file, then
`SnapshotIO::load` walks every record and inserts into `unordered_map`s
(`nodes_`, `edgeIndex_`, `outEdges_`/`inEdges_`, `byName_`, `nodeContributors_`,
`tuNodes_`, and the six CF-index maps). Recommendations in order of cost
to build:

**3.1.1 Sectioned load (small format change, v8).** Write a section table
in the header: byte offset and length for meta, graph interner, nodes,
edges, hierarchy/returns, CF interner, CF contexts, channels. Then:

- Load the CF index only for tools that read it. From the handler
  bodies: `query_exception_safety` (via the oracle),
  `query_call_site_context`, `query_raii_scopes_at_callsite`,
  `query_locks_held`, `query_same_lock`, and the four channel tools
  (`list_channels`, `query_channel`, `query_channels_for_function`,
  `explain_ordering`, which read the channel section) need it.
  `get_callers`, `get_callees`, `search_functions`, `lookup_function`,
  `find_call_chain`, `analyze_dead_code`, `get_class_hierarchy`,
  `list_entry_points`, `list_callback_sites`,
  `list_concurrency_entry_points` touch only the graph; `graph_summary`
  reads only the CF index's size, which the section table can carry.
  Contexts were the 6.1 s of an 11.1 s load before v5 and are still the
  bulk; expect graph-only queries to load in well under a second at this
  scale. Record the per-section split with `--stats-json` first so the
  estimate is replaced by a number.

  **Measured 2026-09-05** (938-TU llvm testbed, format v7, 404 MB, 94,788
  nodes, 352,639 edges, 6.37M call sites; `megascope graph-summary -v`,
  five runs, Release build, 12-core box):

  | section | ms | MB |
  |---|---|---|
  | meta | 0.1 | 0.1 |
  | graph_interner | 285–370 | 41.5 |
  | nodes | 1180–1280 | 47.3 |
  | edges | 290–360 | 28.5 |
  | graph_relations | 2 | 0.2 |
  | cf_interner | 230–250 | 34.0 |
  | cf_set_tables | 43–49 | 10.0 |
  | cf_contexts | 2950–3530 | 242.2 |
  | **load total** | **5100–5600** | **404** |
  | one-shot wall | 6200–6900 | (RSS 2.1 GB) |

  So the three control-flow sections are 3.3–3.8 s of a 5.1–5.6 s load:
  a graph-only tool would load in about 1.8 s with a sectioned layout,
  which confirms 3.1.1. The nodes section is the surprise — 1.2 s for
  95k records is ~12 µs per node, almost all of it the per-node
  `nodeContributors_` set and `tuNodes_` rebuild (mutation-only state,
  3.1.2), so a read-only load should bring graph-only loads under a
  second. Wall exceeded load by 2–3.5 s before the one-shot verbs
  stopped destroying the indexes at exit (now leaked deliberately; the
  remaining ~1 s is kernel time faulting in and unmapping 2 GB of heap,
  which only a smaller working set — 3.1.2, 3.1.5 — reduces).

  **3.1.2 measured** (same testbed, `LoadMode::ReadOnly` — the one-shot
  verbs skip `edgeIndex_`, `tuEdges_`, `nodeContributors_`, `tuNodes_`,
  the CF set-key maps and `byTu_`, the channel `byTu_`): nodes 1200 →
  130 ms, edges 300 → 65 ms, cf_contexts 3000–3500 → 2250–2360 ms, load
  total 5.1–5.6 → 2.9–3.0 s, one-shot wall 6.2–6.9 → 3.9–4.1 s, RSS 2.1
  → 1.5 GB. The remaining load is 75% `cf_contexts`; a graph-only tool
  under a sectioned layout (3.1.1) would load in about 0.65 s.

  **3.1.1 measured** (same testbed re-baked as v8, 404 MB; read-only,
  sections per `ToolEntry::needs`):

  | verb | sections decoded | load | one-shot wall | RSS |
  |---|---|---|---|---|
  | `info` | meta | 0.1 ms | 0.01 s | 19 MB |
  | `get-callers` | graph | 520 ms | 0.87 s | 326 MB |
  | `graph-summary` | graph (header counts) | 540–700 ms | 1.6 s | 326 MB |
  | `list-channels` | graph + channels | 420 ms | 0.67 s | 326 MB |
  | `query-call-site-context` | graph + control flow | 2.75–2.93 s | 3.3–3.5 s | 1.5 GB |

  A graph query's wall time went from 7.4–9.1 s (v7, mutable load,
  destructors at exit) to 0.87 s across #59, #60, and the v8 layout. The
  control-flow tools still pay for `cf_contexts` (2.2–2.4 s of decode
  for 6.37M contexts); that is the case for 3.1.5 if it matters, and it
  is now isolated to the five tools that read contexts.
- Declare per-tool needs in `McpToolEntry` (a small `Needs` bitmask) so
  the CLI dispatcher knows what to load, and `batch`/`serve` load
  everything.
- `info` reads the meta section only.

**3.1.2 Skip mutation-only indexes on read-only loads.** `edgeIndex_` (the
dedup key → edge map) and `nodeContributors_`/`tuNodes_`/`tuEdges_`
(per-TU provenance for `removeTU`) are needed only when the index will be
mutated. A `LoadMode::ReadOnly` that skips them saves hash inserts
proportional to edge count. `byName_` (display → USRs) is rebuilt on load
and is needed by every name-taking query; it should stay, or be
serialized so it is a bulk install like the interner.

**3.1.3 Parallelize the warm-start refresh.** In `main.cpp` the dirty-TU
loop calls `bakeTU` serially, one TU at a time, even though `bakeIndexes`
runs the same work on a pool and the worker-pool `absorb()` merge already
knows how to fold a baked shard into a live index. For the CLI `index`
verb, "re-run after editing a few files" is the hot path; a 20-TU refresh
at 12 threads should take roughly one TU's parse, not twenty. Run the
dirty set through `bakeIndexes` (or `bakeIsolated`) and absorb.

**3.1.4 Header dependency stamps (correctness, not speed).** Stamps cover
only the TU paths passed in (`stampFiles(files)`). Editing a header that a
TU includes does not invalidate that TU, so an incremental `index` after
a header edit silently serves stale edges for every includer. The server
had the same gap, but a CLI run repeatedly during development will hit
it constantly. Fix: record, per TU, the set of files the frontend opened
(`SourceManager` file entries at end of `HandleTranslationUnit`, or the
`PPCallbacks::InclusionDirective` hook) with their stamps; a TU is dirty
if any of them changed. Snapshot size grows by one path-id list per TU
(paths interned). Offer `--force` to rebuild regardless.

**3.1.5 Frozen layout (conditional).** If a target codebase still shows
one-shot load above about a second after 3.1.1–3.1.2, the structural fix
is a read-only representation that needs no decode: CSR adjacency
(offset array + neighbor array, both `u32`), a sorted or hashed name
table with offsets into a string blob, contexts as fixed-width records
with set-table indices. `calleesOf`/`callersOf` and the path searches
would run against an abstract graph interface with two implementations
(mutable `CallGraph` for `index`/`serve`, frozen view for one-shot). This
is a large change — every query touches the graph API — so it should be
gated on a measurement, not done speculatively.

**3.1.6 Auto-spawned daemon (conditional, alternative to 3.1.5).** The
`sccache`/`gopls` pattern: the CLI looks for a Unix socket keyed on the
index path (under `$XDG_RUNTIME_DIR`), spawns `megascope serve --socket`
detached if absent, forwards the request, and the server exits after an
idle timeout. First call pays the load, subsequent calls are
milliseconds, `reindex_tu` and `QueryCache` keep working, and the agent
still sees a plain CLI with `jq`-able stdout. Costs: hidden state, a
version handshake so an upgraded binary does not talk to an old server,
and single-threaded serialization of concurrent callers. Given the
operator's direction away from a resident server, treat this as the
fallback if 3.1.1–3.1.2 do not get the one-shot latency where it needs
to be; do not build it first.

### 3.2 Bake

The bake is already single-parse, pooled, and optionally
subprocess-isolated; the measured levers (PCH: 1.06–1.75× and
deliberately not built) are documented. Two gaps:

- `--isolate-workers` does not thread `channelCfg` through
  `McpBakeConfig`, so channel tracking is silently empty under
  isolation (the CLI prints a warning). Add `channelTypesJson` to the
  worker argv and `SnapshotMeta.channelTypes` already covers the shard
  config check.
- Progress reporting: the bake prints one line at start and one at end.
  For an `index` run of minutes, a `-v` per-TU line (or a counter every N
  TUs) on stderr tells the operator it is alive; `BuildStats` already has
  the per-TU records.

---

## Part 4 — Usability beyond the CLI surface

### 4.1 Fold `prism --mode query` into megascope; keep `dump`

`prism`'s five query types overlap the tool set: `exception-protection`
is `query_exception_safety` (both call `queryExceptionProtection`),
`call-site-context` is `query_call_site_context` (but prism's version
emits only counts, not the scopes and guards), and `throw-propagation`,
`nearest-catches`, and `all-path-contexts` have no MCP twin — the oracle
methods behind them are reachable only through prism today and belong in
the tool table. Prism's query output is hand-built
JSON in `main.cpp` (`nearest-catches`, `call-site-context`) with no string
escaping — a function or location containing `"` or `\` (user-defined
literal operators, some lambda spellings) produces invalid JSON. Under
the ephemeral mode of 2.1, `prism --mode query` is `megascope <tool>`
without an index, and the duplicate code goes away. Keep `prism --mode
dump` as `megascope dump --format ndjson` (streamed, see 2.3). README and
AGENTS.md then lose the "prism vs megascope" section, which today exists
mostly to warn people off prism for anything cross-TU.

### 4.2 Code organization

`src/mcp/McpTools.cpp` is 3,021 lines: 21 handlers, their schemas, the
identity resolver, edge filters, JSON serializers. Proposed:

```
include/vycor/query/   Tools.h (ToolEntry, ToolContext, QueryCache, Needs), Identity.h, Serialize.h
src/query/             tools_graph.cpp (lookup, search, callers, callees, chain, hierarchy, summary)
                       tools_exceptions.cpp, tools_locks.cpp, tools_channels.cpp, tools_deadcode.cpp
                       registry.cpp (getRegisteredTools), identity.cpp, serialize.cpp
src/mcp/               McpProtocol.cpp, McpServer.cpp  (adapter only)
src/cli/               megascope.cpp (verb dispatch, schema→flags, formats, exit codes)
```

Rename `McpToolContext`/`McpToolEntry`/`McpToolHandler` to drop the
prefix. `tests/test_mcp.cpp` becomes `test_query_tools.cpp` with the
same bodies; add a small `test_cli_format.cpp` for the schema→flag
mapping and NDJSON/TSV emitters.

### 4.3 Documentation and discoverability

- README and AGENTS.md describe megascope as "start MCP server"; the CLI
  synopsis at the top of `main()` says the same. Rewrite around
  `index` + query verbs with three or four `jq` recipes
  (`... get-callers --name X --format ndjson | jq -r .callerName | sort -u`).
- `docs/mcp-usage.md`'s Python client skeleton becomes a shell section;
  the analysis workflow (Steps 1–5) translates one-to-one to CLI calls
  and is worth keeping — it is the best agent-facing prose in the repo.
- Ship `--help` per tool from the schema descriptions, and an
  `examples` subsection in each. Agents read `--help`; they do not read
  `docs/`.
- `scripts/bench.py` drives the MCP loop for query latency; add a CLI
  mode (one-shot latency is the number that matters now) and a
  `--sections` breakdown of load time from 3.1.1.
- Add an end-to-end CLI test in CI: run the built binary over
  `examples/deep_chains/`, compare NDJSON output against goldens. Unit
  tests cover handlers; nothing today exercises the binary's output
  contract, and output is about to become the API.

### 4.4 Small ergonomic wins

- `--distinct` on callers/callees to collapse call-site granularity to
  unique function pairs (the documented gotcha "deduplicate on
  callerName in client code").
- `get_callers`/`get_callees` accept no `max_depth`, but `docs/mcp-usage.md`
  passes `max_depth: 3` to `get_callers` in two examples (silently
  ignored). Either implement bounded transitive expansion (with the same
  `max_fan_in` hub cutoff the path tools use) or fix the docs.
- Ambiguity responses return up to 25 candidates with a by-file summary;
  in NDJSON mode emit candidates one per line so `grep <file>` picks the
  overload.

---

## Part 5 — Defects and drift found in passing

| # | Item | Where |
|---|---|---|
| D1 | `serverInfo.version` is hard-coded `"0.1.0"`; `VERSION` is 0.2.0 and `VYCOR_VERSION_STRING` exists | `src/mcp/McpServer.cpp` `handleInitialize` |
| D2 | README and AGENTS.md say "17 MCP tools"; 21 are registered (`list_channels`, `query_channel`, `query_channels_for_function`, `explain_ordering` are missing from the lists) | README.md, AGENTS.md |
| D3 | Prism query output is hand-concatenated JSON without escaping | `src/main.cpp` prism `CfqCallSiteContext`, `CfqNearestCatches` |
| D4 | `dumpIndexToJson` materializes the whole dump in one string | `src/callgraph/ControlFlowOracle.cpp` |
| D5 | Header edits do not dirty dependent TUs on warm start | `SnapshotIO::stampFiles` callers in `src/main.cpp` |
| D6 | Warm-start dirty refresh is serial | `src/main.cpp` megascope branch, `bakeTU` loop |
| D7 | `--channel-types-json` dropped under `--isolate-workers` | `McpBakeConfig` (`WorkerPool.h`), worker argv in `bakeIsolated` |
| D8 | `docs/mcp-usage.md` passes `max_depth` to `get_callers`, which ignores it | docs vs `handleGetCallers` schema |
| D9 | Per-request stderr logging in the serve loop | `McpServer::run` |

---

## Recommended sequence

| # | Item | Depends on | Size |
|---|---|---|---|
| 1 | Move tools to `src/query/`, MCP becomes an adapter; unwrap the text envelope at the adapter (4.2, 2.6) | — | medium, mechanical |
| 2 | `megascope index` / `<tool>` / `tools` / `info` verbs; schema→flags; `--index` alias for `--snapshot`; default index path (2.1, 2.2) | 1 | medium |
| 3 | Output contract: compact JSON, `--format ndjson|tsv`, streaming emitters, exit codes, quiet stderr (2.3, D4, D9) | 2 | small–medium |
| 4 | Source selection: default-all, `--source-re`, `--source-list` (2.4) | — | small |
| 5 | `batch` verb (2.5) | 2, 3 | small |
| 6 | Sectioned snapshot load + per-tool `Needs` + read-only load mode (3.1.1, 3.1.2); measure with `--stats-json` | 2 | medium; bump format to v8 |
| 7 | Parallel warm refresh; header dependency stamps (3.1.3, 3.1.4, D5, D6) | — | medium |
| 8 | Fold prism query mode into megascope; `dump` as NDJSON (4.1, D3) | 2, 3 | small–medium |
| 9 | Docs and CI: README/AGENTS rewrite, `--help` recipes, CLI golden test, bench CLI mode (4.3) | 2–5 | small, ongoing |
| 10 | Frozen layout or auto-daemon (3.1.5 / 3.1.6) | 6 measured and still slow | large; only if needed |
| — | D1, D2, D7, D8 | — | trivial, any time |

Status: item 1 and defects D1, D2, D7, D8 landed in #56 (2026-09-03).
Items 2, 3 (except D4, which belongs to item 8), 5, and D9 landed in #57
(2026-09-05): verbs, schema→flags, `--index`, default index path, output
contract, exit codes, `batch`. Item 4 (default-all, `--source-re`,
`--source-list`; `src/cli/SourceSelection.cpp`) landed in #58 the same
day, with the rule that a bare `index`/`serve` refreshes the TU set the
index already records rather than widening it to the database. Item 7's
parallel warm refresh is still open; #58 only falls back to the cold
bake when more than half the selection is dirty. Item 6's measurement
landed next (`SnapshotLoadStats`: `--stats-json` `snapshot.load_sections`
and the query verbs' `-v`; numbers under 3.1.1) together with the
leak-at-exit for one-shot verbs (#59), then the read-only load mode
(3.1.2, numbers under 3.1.1 too), then the sectioned v8 layout with
per-tool `ToolEntry::needs`, `info` on the meta section alone, and the
bake's entry points recorded in the v8 meta (measurements under 3.1.1),
which closes item 6. Item 7 (3.1.3, 3.1.4; D5, D6) landed next: the
dirty set goes through `bakeIndexes`/`bakeIsolated` and is absorbed, and
the v9 meta records per TU the files its parse opened, stamped by the
frontend's own stat, so a header edit dirties its includers; `--force`
rebuilds regardless. Along the way: `index` loads the meta section first
and an unchanged index exits without decoding the graph, and the drop +
dirty set is removed with one batched `removeTUs` per index. Measured on
the 938-TU testbed (12 threads; cold bake 150–205 s): unchanged `index`
0.03 s / 29 MB (was 6.9 s / 2.1 GB); a header with 3 includers 8–12 s
wall (load 2.6–3.8, bake 2.7–4.2, save 1.1–1.4); a header with 74
includers 44–49 s wall (remove 3.9 s after batching, from 17.7 s; bake
25–31 s; absorb 0.6–1.0 s).
Item 8 (4.1, D3, D4) and the ephemeral mode of 2.1 landed next: the
query verbs given `--source`/`--source-list`/`--source-re` (with
`--build-path`) bake the selected TUs in memory and answer from that
(`src/cli/MegascopeCli.cpp` `bakeEphemeral`; the bake-config helpers
moved out of `main.cpp` into `src/cli/BakeConfig.cpp`); the three
oracle queries prism alone exposed are tools (`query_throw_propagation`,
`query_all_path_contexts`, `query_nearest_catches`, all with per-path
scopes and guards through the shared `serializeTryCatchScope`); `megascope
dump` streams every call-site context and channel site as ndjson (or one
json document through `llvm::json::OStream`) via the new
`ControlFlowIndex::forEachContext`, never holding the materialized index;
and the `prism` subcommand, its hand-built JSON, `toJson`, and
`dumpIndexToJson` are gone (`vycor-cpp prism` prints the megascope
equivalents and exits 2).
Item 9 landed last: `scripts/cli-golden.py` runs the built binary over
`examples/deep_chains/` (an `index` then 30 queries covering every verb,
the ndjson/tsv/json shapes, `batch`, ephemeral mode, and each exit code)
and compares exit codes plus sorted, path- and USR-normalized output
lines against `examples/deep_chains/cli-golden/`; it is the `cli_golden`
ctest and refreshes with `--update`. Each tool's `--help` ends with
worked examples, the README carries jq recipes, docs/mcp-usage.md's
walkthrough is shell, `scripts/bench.py --cli [--sections]` measures the
one-shot latency with the per-section load split (4.3), and
`graph_summary`'s top-N lists break count ties by name so the answer no
longer depends on hash-map order.

Items 1–5 make the CLI real and can land in one or two PRs. Item 6 is the
one performance change the CLI model actually needs, and it is gated on a
measurement the tooling already produces. Item 10 exists so the decision
is written down, not so it gets built.
