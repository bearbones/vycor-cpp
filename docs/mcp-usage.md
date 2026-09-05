# vycor-cpp MCP Server — Usage Guide

This guide covers practical usage of `megascope` based on real analysis runs
against game-engine codebases. It focuses on workflow, gotchas, and effective
query patterns rather than repeating what's in AGENTS.md.

For API critique and improvement proposals see `docs/mcp-review.md`.

---

## Quick start

### 1. Build the tool

```bash
export PATH="/path/to/llvm/bin:$PATH"
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=$(which clang++)
cmake --build build --target vycor-cpp
```

### 2. Generate a compilation database

Point `--build-path` at a directory containing `compile_commands.json`.
For game-engine targets this is `build.ninja/<target>/optimized/`.
The **build must be complete** — generated headers (OpenAPI stubs, Protobuf
outputs, reflection registration files) must exist on disk or ClangTool will
crash while parsing TUs that include them.

### 3. Build the index

```bash
./build/src/vycor-cpp megascope index \
  --build-path /path/to/build \
  --source /path/to/file1.cpp \
  --source /path/to/file2.cpp \
  --threads 8 \
  --collapse-paths Client/dependencies \
  --collapse-paths Client/ThirdParty
```

`--source` is optional: with no selection flag, every C/C++ entry of the
compilation database is indexed the first time, and the TUs recorded in
the index on later runs (see [File selection](#file-selection) for
`--source-re` and `--source-list`). Progress goes to stderr; when the
bake finishes the index is written to
`<build-path>/.vycor/megascope.vycs` (override with `--index <file>`) and
one JSON summary line goes to stdout:

```
megascope: baking call graph + control flow index (68 files, 8 threads)...
megascope: indexes built (59893 nodes, 412051 edges, 1098163 call sites)
megascope: index saved to /path/to/build/.vycor/megascope.vycs
{"call_sites":1098163,"edges":412051,"files":68,"index":"/path/to/build/.vycor/megascope.vycs","mode":"cold","nodes":59893,...}
```

Re-running `index` is a warm start: it loads the file, compares per-file
mtime+size stamps of every TU **and of every header each TU's parse
opened**, re-parses only the dirty TUs (in parallel, through the same
bake as a cold build; drops TUs removed from the selection), and skips
the re-save when nothing changed:

```
megascope: re-indexing 4 TU(s), 4 for changed headers...
megascope: warm start from /path/to/build/.vycor/megascope.vycs (4 TU(s) re-indexed, 0 dropped, ...)
```

When nothing is dirty, `index` reads only the meta section and exits
without decoding the graph (0.03 s on a 938-TU index). When more than
half of the selection is dirty it runs the cold bake instead; `--force`
rebuilds regardless. The index is rebuilt from scratch when `--collapse-paths`,
`--lock-types`, or the channel-type registrations differ from the run
that produced it, when the format version changes, or when the file fails
to decode. Deleting it is always safe.

### 4. Query it

Every tool is a verb. Its flags come from the tool's input schema (the
same names an MCP client passes, hyphens or underscores), so
`megascope <tool> --help` is the reference:

```bash
cd /path/to/build                     # or pass --build-path / --index
vycor-cpp megascope get-callers --name Foo::bar
vycor-cpp megascope find-call-chain --to Foo::bar --max-depth 6 --format ndjson
vycor-cpp megascope search-functions --query Replicator --format tsv | cut -f4
vycor-cpp megascope analyze-dead-code --limit 50 --pretty
vycor-cpp megascope call get_callers --args '{"name":"Foo::bar","min_confidence":"Plausible"}'
vycor-cpp megascope info --files       # what the index holds
vycor-cpp megascope tools              # the tool list
```

stdout is the payload and nothing else: compact JSON by default,
`--pretty` to indent, `--format ndjson` for one record per line (after a
`{"_summary":...}` line carrying the scalar fields, so `head`, `grep`,
and `jq -c` work without loading the whole result), `--format tsv` for
the flat tables (sorted columns, header first). stderr carries only error
messages unless `-v`.

Exit codes are the contract to branch on:

| Code | Meaning |
|---|---|
| 0 | answered, results |
| 1 | answered, empty (not found, no paths, no dead code) |
| 2 | usage or argument error |
| 3 | index missing, wrong format version, or unreadable |
| 4 | ambiguous identity — candidates on stdout; re-run with `--usr` |

For many related queries, `megascope batch` reads NDJSON requests from
stdin and answers each on one line, in order, on one loaded index:

```bash
printf '%s\n' \
  '{"id":1,"tool":"get_callers","args":{"name":"Foo::bar"}}' \
  '{"id":2,"tool":"query_exception_safety","args":{"function":"Foo::bar"}}' \
  | vycor-cpp megascope batch
# {"exit":0,"id":1,"result":{...},"tool":"get_callers"}
# {"exit":0,"id":2,"result":{...},"tool":"query_exception_safety"}
```

`exit` carries the same code the one-shot verb would have returned.

### 5. Serve over MCP

The same tools are available over MCP stdio for clients that speak it:

```bash
./build/src/vycor-cpp megascope serve --build-path /path/to/build \
  --source /path/to/file1.cpp --source /path/to/file2.cpp
```

`serve` warm-starts from the same default index (saving it after a
bake), prints progress to stderr, and then blocks on stdin waiting for
JSON-RPC requests:

```
megascope: warm start from /path/to/build/.vycor/megascope.vycs (2 TU(s) re-indexed, 0 dropped, ...)
megascope: server started, waiting for requests...
```

Do not send requests until "server started" appears — the index is not
ready before that point. Per-request logging is off unless `-v`.
`reindex_tu` is only available here (it mutates the live indexes).

The pre-verb form `megascope --build-path ... --source ... [--snapshot F]`
still works and means `serve`; it only touches an index file when
`--snapshot`/`--index` is given.

---

## File selection

This is the most consequential decision. The index only covers functions
**defined** in the TUs you select. Functions in headers are indexed only
when their definition is compiled in a TU you include.

With no `--source`, `--source-list`, or `--source-re`, `megascope index`
and `megascope serve` take the TU set recorded in the existing index —
a bare `serve` refreshes what was indexed and never widens a narrow
index by accident — or, when there is no index yet, every C/C++ entry
of the compilation database that still exists on disk (assembly and
resource entries are skipped; `--source-re .` re-selects the whole
database later). Narrow the database with:

- `--source-re <regex>` — keep paths matching a POSIX extended regex
  (searched, not anchored), e.g. `--source-re '/Network/(src|tests)/'`;
- `--skip-paths <pattern>` — drop paths under a directory component
  (`--skip-paths ThirdParty`);
- `--source-list <file>` — one path per line, `-` for stdin, so shell
  pipelines compose:

```bash
jq -r '.[].file' build/compile_commands.json | grep /Network/ | sort -u |
  vycor-cpp megascope index --build-path build --source-list -
```

`--source <file>` (repeatable) still names TUs explicitly; explicit files
and list entries are unioned, and `--source-re`/`--skip-paths` narrow
whatever the base set is. Any of `--source`, `--source-list`, or
`--source-re` resolves against the compilation database, not the index.
Paths are made absolute with `.`/`..` removed before deduplication, so a
relative list entry matches the database spelling and the index stamps.
`megascope index` reports how many TUs each filter dropped on stderr and
refuses to bake an empty selection. Under `serve`, `--source-list` must
be a regular file (stdin, pipes, and devices would be drained before the
first MCP request); a `compile_flags.txt` database cannot be enumerated,
so it needs `--source` or `--source-list`.

**Rule of thumb:** include both the implementation files you want to analyze
and their test files. The test TUs often pull in the concrete class
definitions the implementation files only forward-declare.

**For a focused security/exception-safety audit** of a subsystem, select
the subsystem's `src/` and `tests/` TUs with `--source-re` and add
`--collapse-paths` for dependency and third-party directories so internal
edges within those trees are suppressed (boundary edges from your code
into them are kept).

**Cross-TU coverage:** callers in TUs you didn't include won't appear in
`get_callers` results. If a function shows 0 callers but you know it's
called from outside the subsystem, that's expected — the graph is scoped to
the files you provided.

**Generated files:** the `Generated/src/` files (reflection registration,
schema enums) are needed for a complete class hierarchy. Including them adds
noise to the function list but ensures `get_class_hierarchy` is accurate.

---

## Protocol

The server speaks JSON-RPC 2.0 over stdio with MCP-standard
**newline-delimited framing**: one compact JSON message per line. Framing is
autodetected per message, so legacy clients that send LSP-style
`Content-Length` headers continue to work; responses always use the framing
of the most recent request.

**Request format** (standard MCP clients — Claude Desktop, MCP SDKs — do
this automatically):
```
{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"<tool>","arguments":{...}}}\n
```

**Legacy request format** (still accepted):
```
Content-Length: <byte count>\r\n
\r\n
{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"<tool>","arguments":{...}}}
```

**Initialization sequence** (required before any `tools/call`):
```json
{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"my-client","version":"1"}}}
```
Read the response, then send the notification:
```json
{"jsonrpc":"2.0","method":"notifications/initialized","params":{}}
```
Do not send `tools/call` before the `initialize` response arrives.

**Response format:** tool results arrive as a `content` array with a single
`text` item containing JSON-encoded output. You must JSON-parse the `text`
value to get the actual result object.

**Minimal Python client skeleton:**
```python
import subprocess, json

def send(proc, msg):
    proc.stdin.write(json.dumps(msg).encode() + b"\n")
    proc.stdin.flush()

def recv(proc):
    return json.loads(proc.stdout.readline())

def call(proc, req_id, tool, params):
    send(proc, {"jsonrpc":"2.0","id":req_id,"method":"tools/call",
                "params":{"name":tool,"arguments":params}})
    r = recv(proc)
    text = r["result"]["content"][0]["text"]
    return json.loads(text)
```

---

## Function identity and disambiguation

Nodes are identified by their **Clang USR** (unique, signature-encoding);
the qualified **name** is display-only. Two overloads of
`MyClass::process`, or two specializations of `parse<T>`, are distinct
nodes that share one display name. Every tool that takes a function
identity therefore accepts two parameters:

- `name` (or `from`/`to`, `function`, `fn_a`/`fn_b`) — the human-readable
  qualified name. Resolved server-side.
- `usr` (or `from_usr`/`to_usr`, `fn_a_usr`/`fn_b_usr`) — the exact USR.
  Bypasses name resolution entirely; wins when both are given.

**Resolution rules per identity parameter:**

1. A name matching exactly one function behaves exactly as before — and
   the response includes the resolved `"usr"` so you can pin it for later
   queries.
2. A name matching **several** functions returns a non-error
   **disambiguation response** instead of the normal result. It is never a
   silent union of the candidates, and never a hard error:

```json
{"ambiguous": true, "parameter": "name", "name": "precision::process",
 "candidates": [
   {"usr": "c:@N@precision@F@process#*1C#", "qualifiedName": "precision::process",
    "file": "/src/overloads.cpp", "line": 5},
   {"usr": "c:@N@precision@F@process#d#", "qualifiedName": "precision::process",
    "file": "/src/overloads.cpp", "line": 6}],
 "note": "Multiple functions share this name. Re-run with the 'usr' parameter of the intended candidate."}
```

3. Pick the intended candidate and re-run the same call with its `usr` —
   one extra round-trip, then every result is precise to that overload.

**Recommended agent flow:** `search_functions` first (each match carries
its `usr`), pick the `usr`, then drive the precise queries with it:

```python
r = call(proc, 1, "search_functions", {"query": "process"})
usr = r["matches"][0]["usr"]
callers = call(proc, 2, "get_callers", {"usr": usr})
```

**Call-site tools** (`query_call_site_context`,
`query_raii_scopes_at_callsite`) have the same contract on a different
axis: a macro expanded in several functions gives multiple call sites one
`file:line:col` spelling. A bare spelling that matches several contexts
returns `{"ambiguous": true, "candidates": [{callSite, callerUsr,
callerName}...]}` — re-query with the optional `caller` parameter
(qualified name or USR of the enclosing function).

**Deliberate exceptions:** `entry_points` arrays (`analyze_dead_code`,
the lock tools) keep **union** semantics — entry points are reachability
seeds, so a shared name seeding all its overloads is the intended
behavior. `get_class_hierarchy` is display-keyed by design and untouched.

---

## Effective analysis workflow

### Step 1 — Mine function names before querying

`lookup_function` requires an **exact qualified name**. Do not guess —
use `search_functions` first:

```python
r = call(proc, 1, "search_functions", {"query": "handleServerSecurity"})
# -> ranked candidates with qualifiedName, file, line
```

For bulk name mining outside the server, `megascope dump` streams every
call-site context as NDJSON (from the saved index, or from an in-memory
bake of a few files):

```bash
# Every call-site record of two TUs, baked in memory (no index needed)
vycor-cpp megascope dump \
  --build-path /path/to/build \
  --source file1.cpp --source file2.cpp --threads 8 > dump.ndjson

# Find callers of interest
jq -r 'select(.kind=="call_site" and (.callerName|test("Replicat"))) | .callerName' \
  dump.ndjson | sort -u | head -50
```

Use this to populate `lookup_function` calls and `entry_points` lists
rather than guessing qualified names from memory.

### Step 2 — Discover real entry points via `get_callers`

Start from a known function deep in the call tree and walk upward:

```bash
# Find what calls handleServerSecurityMessageV2
vycor-cpp megascope get-callers \
  --name RBX::Network::ClientReplicator::handleServerSecurityMessageV2 \
  --format ndjson | jq -r 'select(.callerName) | .callerName' | sort -u
# get_callers is one level deep (there is no max_depth); repeat on each
# unique callerName to walk upward. A caller with zero callers of its own
# (exit code 1) is a real entry point.
```

Over MCP the same call is `call(proc, 1, "get_callers", {"name": ...})`;
every step below maps the same way (tool name with underscores, arguments
as a JSON object).

Identical edges from multiple TUs are deduplicated server-side; distinct
call sites for the same caller still appear as separate entries (that is
real information, not duplication).

### Step 3 — Check exception safety with real entry points

```bash
vycor-cpp megascope query-exception-safety \
  --function RBX::Network::deserializeUnsignedVarint \
  --exception-type std::exception \
  --entry-points RBX::Network::Replicator::readItem \
  --entry-points RBX::Network::ServerReplicator::readItem \
  | jq -r '.protection, .summary'   # never_caught / sometimes_caught / always_caught

# The same question with every path spelled out (scopes, guards, where
# the throw would be caught):
vycor-cpp megascope query-throw-propagation \
  --function RBX::Network::deserializeUnsignedVarint \
  --exception-type std::exception --format ndjson \
  | jq -r 'select(.callChain) | (if .isCaught then "CAUGHT  " else "UNCAUGHT" end) + " " + (.callChain | join(" -> "))'
```

`protection: never_caught` means every path from the entry points to the
target function is unprotected. For functions that consume untrusted input
this is the primary signal of interest.

### Step 4 — Check call site context for specific callers

When `query_exception_safety` surfaces an uncaught path, drill into
individual call sites to see whether they have `noexcept`, guards, or
try/catch blocks:

```bash
# Each call site of the target, with its context
vycor-cpp megascope get-callers \
  --name RBX::Network::SecurityChannel::sendCounter --format ndjson \
  | jq -r 'select(.callSite) | .callSite' | sort -u | while read -r site; do
    vycor-cpp megascope query-call-site-context --call-site "$site" \
      | jq -c '{caller, isUnderTryCatch, wouldTerminateIfThrows, guards: [.enclosingGuards[].conditionText]}'
  done

# Or every unprotected call site in the index at once:
vycor-cpp megascope dump \
  | jq -c 'select(.kind=="call_site" and .enclosingTryCatches==[] and (.calleeName|test("sendCounter")))'
```

**Note:** an unindexed site is an error payload (`{"error": "Call site
not indexed: ..."}`, exit code 1): the TU that contains it may not have
been included, or the path spelling differs from the compilation
database's.

### Step 5 — Trace specific paths with `find_call_chain`

```bash
vycor-cpp megascope find-call-chain \
  --from RBX::Network::Replicator::readItem \
  --to RBX::Network::deserializeUnsignedVarint \
  --max-depth 8 --format ndjson \
  | jq -r 'select(type=="array") | map("[\(.kind)] \(.from) -> \(.to) @\(.callSite)") | join("\n  ")'
```

`max_depth` counts edges (not nodes). If every chain is longer than
`max_depth`, the result is empty (exit code 1) with no partial result —
increase `max_depth` if you suspect a longer path.

---

## `--collapse-paths` for large codebases

Without collapsing, a 60k-node graph from a typical subsystem includes
thousands of internal edges within `Client/dependencies` and
`Client/ThirdParty`. These pollute `get_callers` results with
non-actionable entries and slow down path-finding queries significantly.

Collapse suppresses edges where **both** caller and callee are inside a
collapsed prefix. Boundary edges (your code calling into a dependency) are
preserved. Use it for any tree you don't want to audit:

```bash
--collapse-paths Client/dependencies \
--collapse-paths Client/ThirdParty \
--collapse-paths Client/Luau
```

The collapsed tree still appears as callees in `get_callees` results —
you see the boundary edge — but the internal structure of the dependency
is hidden.

---

## Known gotchas

**`lookup_function` is exact-match only.** Partial names, namespaces
without the full path, and operator spellings will all fail silently
with `isError: true`. Mine real names from `search_functions` or a
`megascope dump` first.

**Duplicate edges in `get_callers`/`get_callees`.** The same function can
appear multiple times with different `callSite` values. Deduplicate on
`callerName`/`calleeName` in client code when you only need unique
caller/callee relationships rather than site-level detail.

**Graph scope is limited to the selected TUs.** If `get_callers` returns
fewer callers than expected, the missing callers are in TUs you didn't
include. The graph is not a lie — it's scoped.

**`handleX_deprecated` callers are live.** In the Replicator code,
`handleServerSecurityMessage_deprecated` and
`handleClientSecurityMessage_deprecated` are still called from
`OnReceive_deprecated`. These are not dead code — they're active code
paths for older protocol versions. Exception safety audits must include them.

**Build completeness matters.** TUs that include missing generated headers
(OpenAPI stubs, Protobuf outputs, reflection registration) will crash
ClangTool during parsing. The crash guard skips them and reports
`N TU(s) crashed and were skipped` to stderr. A `megascope dump
--source-re ...` pass over a sample confirms your crash count before a
long `megascope index` run.

**Index build takes time proportional to file count.** Expect roughly
4–6 seconds per 10 source files at 8 threads on ARM64. For 68 Replicator
files, build takes ~30 seconds. Nothing is ready until "server started"
appears on stderr.

---

## Interpreting results

| `protection` value | Meaning |
|---|---|
| `always_caught` | All sampled paths from entry points have a try/catch that covers the target |
| `never_caught` | No path is protected — any throw propagates uncaught to the entry point |
| `sometimes_caught` | Mixed — some paths are protected, others are not; review uncaught paths |
| `noexcept_barrier` | A `noexcept` function sits on the path; a throw would `std::terminate` |
| `unknown` | No paths found from the given entry points — either the entry points are wrong or the function is unreachable from them in the indexed graph |

For security-sensitive functions called on data-plane paths (packet
deserialization, authentication message handlers), `never_caught` from
a packet-processing entry point is the primary finding. Combined with the
function handling untrusted input, it means an adversary can trigger
`std::terminate` by sending malformed data.

`sometimes_caught` is worth deeper investigation: the uncaught path summary
in the response identifies exactly which call chain is unprotected.
