# Architecture Review: Call Graph Indexing and the megascope MCP Server

Date: 2026-06-09
Scope: `include/vycor/callgraph/`, `src/callgraph/`, `include/vycor/mcp/`,
`src/mcp/`, and the `megascope` wiring in `src/main.cpp`.

This document records the findings of an architecture review focused on
efficiency and real-world practicality, and the recommended sequence for
addressing them. Items are checked off as they land.

---

## Summary

The layering is sound: graph data structure → builders → oracle → protocol →
tools. The two-phase build (index before edges), per-TU provenance with
tombstone-based incremental reindex, edge confidence tiers, execution-context
tagging, crash isolation, PCH caching, and edge collapse are all the right
ideas. The remaining work is less about architecture and more about closing
the gap between "works on a demo project" and "works on a 5,000-TU codebase,"
plus one client-interop issue that blocks off-the-shelf MCP clients entirely.

---

## Findings

### F1. stdio framing is LSP-style, not MCP-style (interop blocker)

`McpProtocol.cpp` reads and writes `Content-Length`-framed JSON-RPC. The MCP
specification's stdio transport is **newline-delimited JSON** — one compact
JSON-RPC message per line, no headers. Standard clients (Claude Desktop, the
official MCP SDKs, Claude Code) send a bare JSON line and wait; megascope
waits for a header that never comes. A custom orchestrator that happens to
speak LSP framing works, but nothing off-the-shelf does.

**Fix:** default to newline-delimited framing; autodetect `Content-Length`
on the first byte of each message (`{` ⇒ newline mode, `C`/`c` ⇒ header
mode) and reply in the framing the client used. Also echo the client's
`protocolVersion` in `initialize` when it is one we support, rather than
hardcoding `2024-11-05`.

### F2. Every TU is parsed three times

Phase 1 (node index), Phase 2 (edges), and Phase 3 (control-flow contexts)
each run a fresh `ClangTool` — i.e. a full frontend parse — over every file.
Parsing dominates wall-clock (seconds per TU); visitor traversal is
milliseconds. Phases 2 and 3 have identical preconditions (a completed
Phase 1 index), so they can share one parse the way
`CallGraphBuilderConsumer::HandleTranslationUnit` already does for the
single-TU path. That is 3x → 2x. Getting to 1x requires deferring virtual
dispatch fan-out out of AST time (see F3).

### F3. Virtual dispatch expansion is baked at build time

`handleVirtualDispatch` materializes Plausible edges to all overrides known
at build time. Consequences:

- **Incremental drift:** when `reindexTU` indexes a TU that adds a new
  override, existing `VirtualDispatch` call sites in other TUs silently miss
  it. The incremental graph diverges from a full rebuild.
- **Edge bloat:** every virtual call site fans out to every override.
- **Blocks the 1-parse pipeline (F2):** Phase 2 needs the full override map
  before it can expand, which is why it is a separate pass.

**Fix:** store one edge to the static target, expand through
`methodOverrides_` at query time in `calleesOf`/`callersOf`. Incremental
updates become correct by construction, and Phases 1+2+3 can run in a single
parse per TU.

### F4. No edge deduplication

`addEdge` appends unconditionally. Inline functions defined in project
headers are traversed once per including TU, so a header-defined function
with 200 includers produces ~200 copies of each edge — all returned
separately by `get_callers`. Bloats memory and pollutes every tool response.
Dedup at insert (keyed on caller/callee/kind/callSite) or at query time.

### F5. Edges store full strings despite the interner

`CallGraphEdge` holds `callerName`, `calleeName`, `callSite` as
`std::string`; the `StringInterner` is only used for index keys. Three heap
strings per edge across millions of edges is gigabytes. Store `SId`s in
edges, resolve at serialization. Traversal-heavy tools (lock DFS, call
chains) also stop re-hashing strings on every hop.

Similarly, `CallSiteContext` deep-copies the whole `TryCatchScope` vector
(including handler body summaries) into every call site under the try; scopes
are per-function and should be shared records referenced by ID.

### F6. Dangling-pointer trap: `edges_` is a `std::vector`

`calleesOf`/`callersOf` return `const CallGraphEdge *` into `edges_`. The
header documents invalidation on `compact()`, but any `addEdge` reallocation
also invalidates them. Masked today by the single-threaded serve loop;
becomes a real bug the moment queries interleave with `reindexTU`. Use
`std::deque` (the interner already does, for exactly this reason).

### F7. Undocumented thread-safety contract in `CallGraph`

Writes lock `mutex_`; reads (`calleesOf`, `getOverrides`, …) do not. Correct
today only because of phase barriers: Phase 2 reads only maps written during
Phase 1, after `pool.wait()`. One refactor away from a data race. Document
the invariant or move to `std::shared_mutex`.

### F8. Qualified names are not stable node identities

`getQualifiedNameAsString()` is the universal key:

- **Overloads collapse** into one node — callers of `process(const char*)`
  merge with callers of `process(std::string)`. For security-audit queries
  this is a precision loss that can produce wrong conclusions.
- **Template specializations collapse** similarly.
- **Macro expansion** can give multiple call sites the same spelling
  `file:line:col`; `ControlFlowIndex::bySite_` keeps one context per site
  string (last wins).

**Fix:** Clang USRs (`clang::index::generateUSRForDecl`) as canonical IDs,
with a secondary name → USRs index so humans/LLMs can query by readable name
and receive disambiguation candidates.

### F9. No persistence — cold start on every launch

Megascope re-parses the world on every start. A versioned binary snapshot of
graph + CF index + interner, keyed on a hash of `compile_commands.json` plus
per-file mtimes, gives instant warm starts. Combined with `reindexTU`, that
is a real daemon: load snapshot → reindex dirty TUs → serve. This is the
difference between "tool you run for an audit" and "tool that is always on."
(Already on the TODO list as "incremental indexing with binary
serialization".)

### F10. Exact-name lookup is hostile to LLM clients

`lookup_function` requires the exact qualified name; an agent usually knows
only `execute` or `TransformPipeline`. There is no search tool, so the first
interaction with the server often fails. Add
`search_functions(substring/regex, limit)` returning candidates.

### F11. Hub-node blowup in path queries

`find_call_chain` and `query_locks_held` cap *output* (`maxPaths`,
`maxDepth`) but not *work*: a target whose ancestry contains a hub (logger
with 10k callers) grinds before the cap hits. Add an in-degree cutoff
(reported in the response — useful signal for the LLM) and/or a node-visit
budget.

### F12. `siglongjmp` crash guard can poison locks

Jumping out of a SIGSEGV handler skips destructors mid-`ClangTool::run`. If
the crash occurs while a thread holds the `CallGraph` mutex, the next
`addEdge` deadlocks the build. Subprocess workers (fork a batch of TUs,
merge) give true isolation and a path to distributed indexing; at minimum the
mutex-poisoning case needs a guard.

### F13a. ControlFlowIndex::removeTU matched on the wrong path (found during F9 work)

`removeTU` matched contexts whose `callSite` string starts with
`tuPath + ":"`, but call sites are recorded with the *compile-command
spelling* of the file (often relative, e.g. `foo.cpp:11:3`) while callers
pass the TU path they know (often absolute). The prefix never matched, so
stale contexts survived re-indexing — this silently broke both the
`reindex_tu` MCP tool and snapshot warm starts. Fixed by recording an
explicit `tuPath` on every `CallSiteContext` (threaded through the Phase 3
factory chain, like the CallGraph builders already do) and matching on it
exactly; contexts without provenance fall back to the old prefix match.

### F13. Minor serve-loop items

- `handleToolsCall` keeps a function-local `static` handler map — harmless
  single-threaded, a landmine otherwise. Make it a member.
- `analyze_dead_code` reruns full liveness per call; cache keyed on the
  entry-point set.
- `megascope` requires an explicit `--source` list; default to all files in
  `compile_commands.json` (existing TODO).

---

## Recommended sequence

| # | Item | Findings | Status |
|---|------|----------|--------|
| 1 | Newline-delimited stdio framing with Content-Length autodetect | F1 | ✅ done |
| 2 | Quick wins: `deque` for `edges_`, handler map as member, locking contract docs | F6, F7, F13 | ✅ done |
| 3 | Snapshot persistence + warm start | F9, F13a | ✅ done |
| 4 | Merge Phase 2+3 into one parse | F2 | ✅ done |
| 4b | Single-parse pipeline (defer the function-return join like F3; all phases share one parse) | F2, F3 | ✅ done — measured −42% cold bake on llvm lib/Support, 149 TUs (17.1s → 9.9s, 12 threads), identical query results |
| 5 | Query-time virtual dispatch expansion | F3 | ✅ done |
| 6 | Edge dedup + interned IDs in edges | F4, F5 | ✅ done (edges; CallSiteContext scope-sharing from F5 still open) |
| 7 | `search_functions` tool; in-degree cutoffs in path queries | F10, F11 | ✅ done |
| 7b | Whole-graph query cache (graph_summary, dead-code liveness) + id-space path search with corridor pruning | F11, F13 | ✅ done — see scale table below |
| 8 | USR-based node identity | F8 | |
| 9 | Subprocess worker isolation | F12 | |
| 10 | ControlFlowIndex string interning (F5 completion) | F5 | open — at 6.37M call sites the per-context string copies (callerName/calleeName/callSite/tuPath, the last never deduplicated) dominate warm RSS (4.95 GB) and the 301 MB snapshot |
| 11 | Snapshot bulk-load fast path | F9 | open — warm-start load replays 6.37M contexts through per-call locking + interning: 8.3s of the 12.2s warm start |

---

## Scale measurements (2026-07-06)

Testbed: llvm-project (submodule), 938 TUs across lib/{Support,IR,MC,
Object,Analysis,Transforms/{Utils,Scalar},…}, Release build, 12 threads,
measured with scripts/bench.py and a warm-snapshot query driver. Graph:
57k nodes, 345k stored edges, 6.37M call sites, 31 MB interned strings.

| metric | before this round | after |
|---|---|---|
| cold bake (149-TU Support subset) | 17.1 s (two parses/TU) | 9.9 s (single parse) |
| cold bake (938 TUs) | — | 149.6 s |
| warm start (938 TUs) | — | 12.2 s (8.3 s snapshot load) |
| peak RSS cold / warm | — | 8.1 / 4.95 GB |
| graph_summary | 11 ms (whole-graph scan per call) | 0.04 ms (cached) |
| analyze_dead_code | 23 ms (liveness per call) | 0.8–1.2 ms (cached liveness + JSON) |
| search_functions | 14.2 ms (re-lowercase per query) | 3.0 ms (cached lowered index) |
| find_call_chain | 5,448 ms (string-space DFS over ancestor cone) | 0.02 ms no-path / 2.5 ms deep 5-path (id-space + corridor prune) |

### Auto-PCH: measured ceiling, deliberately not built

A shared PCH synthesized from the most common direct includes was
prototyped externally to size the prize before building a feature:

- llvm lib/Support TUs (lean headers): 350 → 200 ms per parse, **1.75×**,
  PCH build cost 0.8 s.
- vycor-cpp's own TUs (clang-tooling heavy): 1879 → 1776 ms, **1.06×** —
  these TUs are template-instantiation-bound (RecursiveASTVisitor), which
  a PCH does not skip.

Verdict: workload-dependent 1.1–1.75×, plus correctness caveats (flag
grouping, macro-before-include ordering). Not worth the complexity while
bigger loss-free levers remain (items 10–11 above). Revisit if a target
codebase is measured header-bound.

Rationale for the order: 1 gates all real-world MCP use and is small. 3 is
the largest UX win per line of code. 4–6 compound: the single-parse pipeline
falls out of query-time expansion, and dedup/interning shrink what the
snapshot in 3 has to serialize (doing 3 first is still fine — bump the
snapshot format version). 8 touches every layer, so it goes last among the
semantic changes.
