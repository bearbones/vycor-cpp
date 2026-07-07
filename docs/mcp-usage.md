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

### 3. Launch the server

```bash
./build/src/vycor-cpp megascope \
  --build-path /path/to/build \
  --source /path/to/file1.cpp \
  --source /path/to/file2.cpp \
  --threads 8 \
  --collapse-paths Client/dependencies \
  --collapse-paths Client/ThirdParty
```

`--source` must be repeated once per file — it does not accept globs.
The server prints progress to stderr and then blocks on stdin waiting for
JSON-RPC requests:

```
megascope: baking call graph + control flow index (68 files, 8 threads)...
megascope: indexes built (59893 nodes, 412051 edges, 1098163 call sites)
megascope: server started, waiting for requests...
```

Do not send requests until "server started" appears — the index is not
ready before that point.

### 4. Warm starts with `--snapshot`

Pass `--snapshot <file>` to persist the baked graph and control-flow index
across launches:

```bash
./build/src/vycor-cpp megascope --build-path ... --source ... \
  --snapshot /tmp/myproject.vycs
```

On the first run the server builds normally and saves the snapshot. On later
runs it loads the snapshot, compares per-file mtime+size stamps, and
re-parses **only the TUs that changed** (plus drops TUs removed from the
`--source` set). For a large file set this turns minutes of startup into
seconds:

```
megascope: warm start from /tmp/myproject.vycs (2 TU(s) re-indexed, 0 dropped, ...)
```

The snapshot is invalidated wholesale (full rebuild) when `--collapse-paths`
or `--lock-types` differ from the run that produced it, when the format
version changes, or when the file fails to decode. It is a cache, never a
source of truth — deleting it is always safe.

---

## File selection

This is the most consequential decision. The server only indexes functions
**defined** in the files you pass. Functions in headers are indexed only when
their definition is compiled in a TU you include.

**Rule of thumb:** include both the implementation files you want to analyze
and their test files. The test TUs often pull in the concrete class
definitions the implementation files only forward-declare.

**For a focused security/exception-safety audit** of a subsystem:
1. Extract all TUs from `compile_commands.json` whose path matches the
   subsystem directory.
2. Add `--collapse-paths` for dependency and third-party directories so
   internal edges within those trees are suppressed (boundary edges from
   your code into them are kept).

```python
import json
with open("compile_commands.json") as f:
    db = json.load(f)
files = [e["file"] for e in db if "/Network/src/" in e["file"]
                                or "/Network/tests/" in e["file"]]
```

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

For bulk name mining outside the server, a `prism` dump also works:

```bash
# Get a prism dump (faster than megascope for discovery)
vycor-cpp prism \
  --build-path /path/to/build \
  --source file1.cpp --source file2.cpp \
  --mode dump --threads 8 > dump.json

# Find callers of interest
python3 -c "
import json
data = json.load(open('dump.json'))
callers = {s['callerName'] for s in data['callSites']
           if 'Replicat' in s['callerName']}
print('\n'.join(sorted(callers)[:50]))
"
```

Use this to populate `lookup_function` calls and `entry_points` lists
rather than guessing qualified names from memory.

### Step 2 — Discover real entry points via `get_callers`

Start from a known function deep in the call tree and walk upward:

```python
# Find what calls handleServerSecurityMessageV2
r = call(proc, 1, "get_callers",
         {"name": "RBX::Network::ClientReplicator::handleServerSecurityMessageV2",
          "max_depth": 3})
# Each unique callerName with zero callers of its own is a real entry point
```

Identical edges from multiple TUs are deduplicated server-side; distinct
call sites for the same caller still appear as separate entries (that is
real information, not duplication).

### Step 3 — Check exception safety with real entry points

```python
r = call(proc, 2, "query_exception_safety", {
    "function": "RBX::Network::deserializeUnsignedVarint",
    "exception_type": "std::exception",
    "entry_points": ["RBX::Network::Replicator::readItem",
                     "RBX::Network::ServerReplicator::readItem"]
})
print(r["protection"])   # never_caught / sometimes_caught / always_caught
print(r["summary"])
for path in r["paths"][:5]:
    print("CAUGHT" if path["isCaught"] else "UNCAUGHT",
          " -> ".join(path["callChain"]))
```

`protection: never_caught` means every path from the entry points to the
target function is unprotected. For functions that consume untrusted input
this is the primary signal of interest.

### Step 4 — Check call site context for specific callers

When `query_exception_safety` surfaces an uncaught path, drill into
individual call sites to see whether they have `noexcept`, guards, or
try/catch blocks:

```python
# Get callers of the target
callers = call(proc, 3, "get_callers",
               {"name": "RBX::Network::SecurityChannel::sendCounter",
                "max_depth": 1})

# Check each unique call site
seen = set()
for edge in callers["callers"]:
    if edge["callerName"] in seen: continue
    seen.add(edge["callerName"])
    ctx = call(proc, 4, "query_call_site_context",
               {"call_site": edge["callSite"]})
    print(edge["callerName"],
          "protected:", ctx.get("isUnderTryCatch"),
          "noexcept:", ctx.get("callerNoexcept"),
          "guards:", [g["conditionText"] for g in ctx.get("enclosingGuards",[])][:2])
```

**Note:** `query_call_site_context` silently returns an empty result for
unknown sites (no `isError`). If `caller` and `callee` are both empty
strings in the response, the site was not indexed — the TU that contains
it may not have been included.

### Step 5 — Trace specific paths with `find_call_chain`

```python
r = call(proc, 5, "find_call_chain", {
    "from": "RBX::Network::Replicator::readItem",
    "to":   "RBX::Network::deserializeUnsignedVarint",
    "max_depth": 8
})
if r["found"]:
    for hop in r["hops"]:
        print(f"  [{hop['kind']}] {hop['from']} -> {hop['to']}  @{hop['callSite']}")
```

`max_depth` counts edges (not nodes). If the chain is longer than
`max_depth`, `found: false` is returned with no partial result — increase
`max_depth` if you suspect a longer path.

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
with `isError: true`. Mine real names from a prism dump first.

**Duplicate edges in `get_callers`/`get_callees`.** The same function can
appear multiple times with different `callSite` values. Deduplicate on
`callerName`/`calleeName` in client code when you only need unique
caller/callee relationships rather than site-level detail.

**Graph scope is limited to `--source` files.** If `get_callers` returns
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
`N TU(s) crashed and were skipped` to stderr. Run a `prism --mode dump`
pass first to confirm your crash count before a long `megascope` session.

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
