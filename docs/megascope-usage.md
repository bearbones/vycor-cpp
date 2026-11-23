# Using megascope

`vycor-cpp megascope` indexes a C++ project's call graph, symbol references,
and the control flow around call sites. Build the index once to answer questions
such as who calls a function, which paths can reach it, and which locks or
exception handlers are in scope along the way.

- [Build and query an index](#quick-start)
- [Select translation units](#file-selection) or [query files in memory](#query-selected-files-in-memory)
- [Find a tool](#query-tools) or [use a shell recipe](#shell-recipes)
- [Merge Bazel or Buck2 shards and index during compilation](#build-system-integration)

## Quick start

### 1. Get the CLI

The commands below use `vycor-cpp` on your PATH. For a local CMake build,
the executable is `build/src/vycor-cpp`. [Install and build instructions](building.md)

### 2. Generate a compilation database

For a CMake project, enable `CMAKE_EXPORT_COMPILE_COMMANDS` with the Ninja
generator. Build the targets you want to index so that generated headers
exist when Clang parses the sources.

```sh
cmake -S . -B build -G Ninja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build
```

For a runnable example from this repository, use the bundled call-chain
fixture. It includes a script to generate its compilation database and a
query that traces a call from `main` to `stage5_sink`.
[Call-chain walkthrough](../examples/deep_chains/README.md#how-to-analyze)

### 3. Build the index

```bash
vycor-cpp megascope index \
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

The initial bake parses each TU once to collect declarations, call edges, and
control-flow context. Cross-TU joins such as virtual dispatch happen at query
time. Re-running `index` is a warm start: it loads the file, compares per-file
mtime+size stamps of every TU **and of every header each TU's parse
opened**, re-parses only the dirty TUs (in parallel, through the same
bake as a cold build; drops TUs removed from the selection), and skips
the re-save when nothing changed:

```
megascope: re-indexing 4 TU(s): 4 for changed headers, 0 for changed compile inputs, 0 retried after a failed parse...
megascope: warm start from /path/to/build/.vycor/megascope.vycs (4 TU(s) re-indexed, 0 dropped, ...)
```

A TU is dirty when its source, any file its parse opened, or its
effective compile inputs (compile command, working directory, extra
args, sysroot, PCH, toolchain) changed since it was indexed. When
nothing is dirty, `index` reads only the meta section and exits
without decoding the graph (0.03 s on a 938-TU index). When more than
half of the selection changed it runs the cold bake instead; `--force`
rebuilds regardless. The index is rebuilt from scratch when `--collapse-paths`,
`--lock-types`, or the channel-type registrations differ from the run
that produced it, when the bake environment differs, when the format
version changes, or when the file fails to decode. Deleting it is
always safe.

Every TU's parse outcome is recorded (`megascope info` reports
`coverage`, `info --files` the per-TU `status`): a TU whose parse
failed is never cached as healthy. Failed TUs are re-parsed alongside
any refresh that rewrites the index, or on `--retry-failed`; a refresh
that would otherwise touch nothing leaves them as recorded and stays
meta-only. [Index provenance and refresh rules](index-provenance.md)

#### Indexing options

| Option | When to use it |
|---|---|
| `--index <file>` | Choose a snapshot path for later refreshes. `--snapshot` is the older spelling. |
| `--no-wait` | Fail immediately if another writer holds `<index>.lock`. |
| `--threads N`, `--pch-dir <dir>` | Set parse parallelism or choose a cache directory for precompiled headers. |
| `--isolate-workers`, `--workers N` | Parse in subprocesses. Isolation is the default when `--threads` differs from 1 and `--pch-dir` is unset. Use `--isolate-workers=false` for an in-process bake. |
| `--worker-timeout <seconds>`, `--worker-memory-limit <MiB>` | Bound each worker. The timeout defaults to 600 seconds without starting a new TU. A timed-out TU is recorded as `timeout`. Zero disables the timeout. |
| `--stats-json <file>` | Write per-phase and per-TU timings, parse outcomes, graph sizes, and peak memory use after saving the index. |
| `-v` | Show progress on stderr. Verbose queries also report the index load time by section. |

### 4. Query it

Query verbs find the index using `--index`, then `$VYCOR_INDEX`, then
`<build-path>/.vycor/megascope.vycs` or `./.vycor/megascope.vycs`.
Every tool is a verb. Its flags come from the tool's input schema (the
names `batch` takes as JSON keys, hyphens or underscores), so
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

Every payload carries `status` (`ok`, `ambiguous`, `usage_error`,
`not_found`, `unavailable`) and `indexScope` (which bake the index came
from, whether it was checked against the sources, and how many of the
requested TUs it holds cleanly) — see [result contract](result-contract.md). Exit
codes derive from `status` and are the contract to branch on:

| Code | Meaning |
|---|---|
| 0 | answered, results |
| 1 | answered, empty (no callers, no paths, no dead code), or `not_found` (a named function, call site, or channel the index lacks) |
| 2 | usage or argument error (`usage_error`) |
| 3 | index missing, wrong format version, unreadable, or lacking the facts the tool needs (`unavailable`) |
| 4 | ambiguous identity — candidates on stdout; re-run with `--usr` |

An `indexScope.complete` of `false` means some requested TU failed to
parse; a function that lives there is `not_found`, and the exception
tools report `observed_*` instead of a universal verdict.

For many related queries, `megascope batch` reads NDJSON requests from
stdin and answers each on one line, in order, on one loaded index:

```bash
printf '%s\n' \
  '{"id":1,"tool":"get_callers","args":{"name":"Foo::bar"}}' \
  '{"id":2,"tool":"query_exception_safety","args":{"function":"Foo::bar"}}' \
  | vycor-cpp megascope batch
# {"exit":0,"id":1,"result":{...},"status":"ok","tool":"get_callers"}
# {"exit":0,"id":2,"result":{...},"status":"ok","tool":"query_exception_safety"}
```

`exit` carries the same code the one-shot verb would have returned and
`status` mirrors `result.status`.

Beyond calls, the index records which function references which
function, global variable, or class, and how (`call`, `address_of`,
`read`, `write`, `vtable_use`, `typeinfo_use`, and `lock` /
`lock_shared` for a mutex acquisition), with each symbol's
linkage and defining TU:

```bash
vycor-cpp megascope lookup-symbol --name ns::counter          # linkage, defining TU, referrers per role
vycor-cpp megascope get-references --name ns::counter --roles write   # who writes it
vycor-cpp megascope get-references --name main --direction from      # what main references
```

The reference describes the payloads, coverage, and indexing cost.
[Symbol references](symbol-references.md)

## Query tools

Run `vycor-cpp megascope tools` for the live list. Use
`vycor-cpp megascope <tool> --help` for a tool's arguments. The CLI uses hyphens in verb names.
Batch requests use the identifiers below.

| Question | Tool identifiers |
|---|---|
| Which function, symbol, or class is this? | `search_functions`, `lookup_function`, `lookup_symbol`, `get_class_hierarchy` |
| What calls or references it? | `get_callers`, `get_callees`, `find_call_chain`, `get_references`, `list_entry_points`, `graph_summary`, `analyze_dead_code` |
| What happens on a path or at a call site? | `query_call_site_context`, `query_all_path_contexts`, `query_raii_scopes_at_callsite`, `query_locks_held`, `query_same_lock`, `query_path_effects` |
| Where can an exception propagate? | `query_exception_safety`, `query_throw_propagation`, `query_nearest_catches` |
| Which callbacks, threads, and channels interact? | `list_callback_sites`, `list_concurrency_entry_points`, `list_channels`, `query_channel`, `query_channels_for_function`, `explain_ordering` |
| What does a change or link dependency affect? | `impact_of_change`, `audit_dependencies`, `explain_dependency` |
| What can I measure? | `metrics` |

Use `megascope diff --before A --after B` to compare two saved indexes.
The reference guides explain how to interpret each kind of evidence.
[Change impact](change-impact.md) · [Link dependencies](dependency-audit.md) · [Metrics](metrics/README.md)

## Shell recipes

Run these from the index's build directory or set `VYCOR_INDEX` to its path.
`megascope dump` streams call-site contexts and channel sites as NDJSON.
Use `dump --format json` for one JSON document.

```bash
# Unique callers of a function, walking one level up
vycor-cpp megascope get-callers --name Foo::bar --format ndjson | \
  jq -r 'select(.callerName) | .callerName' | sort -u

# Print each returned path from main as a sequence of function names
vycor-cpp megascope find-call-chain --from main --to Foo::bar --format ndjson | \
  jq -r 'select(type=="array") | [.[0].fromName] + map(.toName) | join(" -> ")'

# Unprotected call sites of the whole index (no enclosing try/catch)
vycor-cpp megascope dump | \
  jq -c 'select(.kind=="call_site" and .enclosingTryCatches==[]) | {callerName, calleeName, callSite}'

# Dead functions under one directory, as a file:line list
vycor-cpp megascope analyze-dead-code --format ndjson | \
  jq -r 'select(.file? and (.file|test("/src/net/"))) | "\(.file):\(.line) \(.name)"'

# Exit codes drive scripts: 1 is "answered, empty", 4 is "ambiguous name"
vycor-cpp megascope get-callers --name bar --format ndjson; case $? in
  4) echo "pick a --usr from the candidates above";; esac
```

## Query selected files in memory

Give a query `--source`, `--source-list`, or `--source-re` to build its input in
memory. Supply the project's compilation database with `--build-path`, or pass
compiler flags after `--`. Only the selected TUs contribute evidence, including
callers. This mode also supports `--skip-paths`, `--collapse-paths`, `--threads`,
and `--org-config`. It replaces the former `prism` subcommand.

```sh
vycor-cpp megascope query-nearest-catches --build-path build \
  --source SecurityModule.cpp --function onClientChallengeResponse
vycor-cpp megascope dump --build-path build --source SecurityModule.cpp | \
  jq -c 'select(.kind=="call_site" and .enclosingTryCatches==[])'
```

---

## File selection

This is the most consequential decision. The index only covers functions
**defined** in the TUs you select. Functions in headers are indexed only
when their definition is compiled in a TU you include.

With no `--source`, `--source-list`, or `--source-re`, `megascope index`
takes the TU set recorded in the existing index — a bare `index`
refreshes what was indexed and never widens a narrow
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
refuses to bake an empty selection. A `compile_flags.txt` database
cannot be enumerated, so it needs `--source` or `--source-list`.

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

In `megascope batch` the same call is
`{"tool":"get_callers","args":{"name":"..."}}`; every step below maps
the same way (tool name with underscores, arguments as a JSON object).

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
  | jq -r '.protection, .exhaustive, .summary'   # never_caught / sometimes_caught / always_caught / observed_*

# The same question with every path spelled out (scopes, guards, where
# the throw would be caught):
vycor-cpp megascope query-throw-propagation \
  --function RBX::Network::deserializeUnsignedVarint \
  --exception-type std::exception --format ndjson \
  | jq -r 'select(.callChain) | .outcome + " " + (.callChain | join(" -> ")) + " @" + .hops[-1].callSite'
```

`protection: never_caught` means every path from the entry points to the
target function is unprotected, and `exhaustive: true` that every path
was enumerated. For functions that consume untrusted input this is the
primary signal of interest. Each path is one exact chain of call sites
(`hops`), so two calls to the same function from the same caller — one
under a try, one not — are two paths with two outcomes. When the search
stopped early (`stopReasons` non-empty, `exhaustive: false`) the verdict
is `observed_caught` / `observed_uncaught`: a statement about the paths
examined, not about all of them. See [path analysis](path-analysis.md).

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
not indexed: ...", "status": "not_found"}`, exit code 1): the TU that
contains it may not have been included (check `indexScope`), or the
path spelling differs from the compilation database's.

### Step 5 — Trace specific paths with `find_call_chain`

```bash
vycor-cpp megascope find-call-chain \
  --from RBX::Network::Replicator::readItem \
  --to RBX::Network::deserializeUnsignedVarint \
  --max-depth 8 --format ndjson \
  | jq -r 'select(type=="array") | map("[\(.kind)] \(.from) -> \(.to) @\(.callSite)") | join("\n  ")'
```

`max_depth` counts edges (not nodes). If every chain is longer than
`max_depth`, the result is empty (exit code 1) with
`stopReasons: ["depth_limit"]` — increase `max_depth`. `path_limit`
means more chains exist than `max_paths`; `hub_pruned` lists the
high-fan-in functions whose ancestry was skipped (`skippedHubs`, raise
`max_fan_in` or query them with `get_callers`).

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

## Auditing link dependencies

`audit-dependencies` and `explain-dependency` join the index with a
target map (which target compiles which source, and what each target
says it links) and report, per declared dependency, how many of the
provider's symbols the consumer references and how much of the provider
that drags in:

```bash
scripts/cmake-target-map.py --build-dir build --output targets.json
vycor-cpp megascope audit-dependencies --index proj.vycs \
    --target-map targets.json --finding thin-dependency --sort gain
vycor-cpp megascope explain-dependency --index proj.vycs \
    --target-map targets.json --consumer app --provider bigdep
```

The evidence is the index's symbol reference facts (an index baked
before them falls back to call edges; bake that one without
`--collapse-paths`). Every payload says what it cannot see
(`completeness`) and no finding is a proof; [the dependency audit reference](dependency-audit.md) has the target-map
schema, the attribution rules, and how to check a result against the
built objects.

---

## Effects on hot paths

`query-path-effects` lists the allocations, lock acquisitions, I/O and
throws reachable from hot roots, one record per effect call with a
witness path from a hot root (the nearest one, or with a profile the
root of the heaviest path):

```bash
vycor-cpp megascope query-path-effects --roots 'myorg::Solver::step' \
    --effects allocation --effects lock --format ndjson

# With a coverage profile (llvm-cov export JSON) of a real run: paths are
# weighted by their rarest call, observed ones first, latent ones last;
# --min-count takes every function entered that often as a root.
vycor-cpp megascope query-path-effects --profile run.json --min-count 10000
```

Roots and accepted functions also come from the org config's `hotPaths`
key. The same analysis runs as the `hot-path-effects` check. Reference,
including the profile join and what a count does and does not prove:
[checks/hot-path-effects.md](checks/hot-path-effects.md).

## Build system integration

### Merge translation units and build targets

Build adapters can capture the compiler flags for each translation unit
directly from a Bazel or Buck2 action. Generate a shard for each TU, merge a
target's shards, then merge the targets into a project index.

```sh
# Index one TU with its compiler flags. Repeat for the other TUs in the target.
mkdir -p out/tu
vycor-cpp megascope index --threads 1 --no-wait \
  --index out/tu/a.cpp.vycs --source src/a.cpp -- -std=c++17 -Iinclude

# Combine the TU shards for a library or binary.
vycor-cpp megascope merge --no-wait --output out/libcore.vycs \
  out/tu/a.cpp.vycs out/tu/b.cpp.vycs

# Merge target shards, one path per line in target_shards.list.
vycor-cpp megascope merge --no-wait --output out/megascope.global.vycs \
  --input-list out/target_shards.list --entry-point main
```

`megascope merge` validates build configuration and environment compatibility
across shards (overridable with `--allow-mismatch`) and deduplicates
overlapping TUs idempotently so diamond dependencies in the target graph never
double-count edges or call-site contexts.

Both integrations include a runnable multi-target project.

- **Buck2** supports Starlark rules and BXL scripts. [Buck2 guide](../buck2/README.md) · [Example project](../examples/buck2_megascope/)
- **Bazel** supports rules and aspects with Bzlmod or `WORKSPACE`. [Bazel guide](../bazel/README.md) · [Example project](../examples/bazel_megascope/)

### Index during compilation with the Clang plugin

Load `libvycor_megascope_plugin.so` into the normal `clang++ -c` invocation
to emit a per-TU shard alongside the object file. The plugin walks the AST
after the main compiler action, reusing its parsed headers and templates.
For example, compile with the same Clang major as the plugin.

```bash
# Emits foo.vycs next to foo.o by default (or pass --output=foo.vycs / foo.mgsc / foo.shard.json)
clang++ -c foo.cpp -o foo.o \
  -fplugin=/path/to/lib/libvycor_megascope_plugin.so \
  -fplugin-arg-vycor-megascope---output=foo.vycs \
  -fplugin-arg-vycor-megascope---entry-point=main \
  -fplugin-arg-vycor-megascope---no-wait
```

Plugin flags (`--output=<path>`, `--entry-point=<fn>`, `--collapse-paths=<p>`,
`--org-config=<file>`, `--lock-types=<t>`, `--channel-types-json=<file>`,
`--no-wait`) can be passed via `-fplugin-arg-vycor-megascope-<flag>` or
`-Xclang -plugin-arg-vycor-megascope -Xclang <flag>`.

### Share an index with the Rust core and Python tools

The native snapshot format is `.vycs`. To exchange data with the Rust
megascope engine, choose an output format with `index --index` or the Clang
plugin's `--output`. [Rust megascope](https://github.com/bearbones/megascope) · [C++ bridge](../include/vycor/callgraph/MegascopeBridge.h)

- **Export JSON shards.** A path ending in `.shard.json` or `.shards.json`
  exports functions, call edges, exception handlers, RAII and lock scopes,
  conditional guards, class hierarchies, and channels for `megascope ingest`.
- **Use binary snapshots.** Linking the optional `libmegascope` lets the CLI
  build, merge, compare, and query `.mgsc` snapshots. [Build options](building.md#dependencies)
- **Combine C++ and Python.** With `libmegascope` linked,
  `vycor-cpp megascope merge --output combined.mgsc cpp.vycs python.pyrx`
  merges a C++ index and a Python snapshot from pyRx. It also accepts C++
  `.mgsc` and JSON shards. [pyRx](https://github.com/bearbones/pyRx)

---

## Known gotchas

**Function names are exact-match only.** Partial names, namespaces
without the full path, and operator spellings all answer `not_found`
(exit 1) on every tool that takes
a function, with up to five `didYouMean` suggestions (the
`search_functions` ranking, then near spellings). Re-query with a
suggestion's `usr`, or mine real names from `search_functions` or a
`megascope dump` first. An empty `ok` answer means the function is in
the index and the list really is empty.

**Duplicate edges in `get_callers`/`get_callees`.** The same function can
appear multiple times with different `callSite` values. Pass
`distinct: true` (`--distinct`) when you only need unique caller/callee
relationships: one record per function, with `siteCount`.

**`new` and `delete` are calls.** A new-expression is an edge to the
allocation function it selects (`operator new`, `operator new[]`, a
placement overload, a class's own), a delete-expression to the
deallocation function, each at the expression (`kind: DirectCall`). So
`operator new` has callers, and its call sites carry the try scopes
around them: `query-throw-propagation --usr 'c:@F@operator new#l#'
--exception-type std::bad_alloc` answers where an allocation failure
would be caught. Ask by `usr`: the name `operator new` is nearly always
`ambiguous` (every overload `<new>` declares shares it), and the
candidate list gives each one's. `c:@F@operator new#l#` is the plain
form on a 64-bit Linux or macOS target, the one ending `#*v#` the
placement form. A `new` whose type depends on a template parameter has
no edge in the template's definition; each instantiation has its own.

**Template instantiations have their own calls.** The bake reads the
body of every function template instantiation, member of a class
template instantiation, and generic-lambda specialization a TU uses,
system headers included, and records its calls under the
instantiation's identity, with calls that depend on a template
parameter resolved for it. So `get_callees` on `registerType<Codec>`
answers from that instantiation's own edges, `T::name()` included,
while `get_callees` on the template's definition answers with the calls
written there that depend on no parameter. Display names drop a
function template's arguments (`registerType`), so ask for an
instantiation by `usr`. A call inside an instantiation is sited where
the template spells it, often in a header; it carries no control-flow
context, so the exception tools answer `unknown` for a path through one.

**Calls the compiler writes are edges.** A constructor the compiler
writes (default, copy, move, inherited) calls the member and base
constructors and runs the default member initializers; a destructor,
written or not, calls the member and base destructors after its body.
A member the compiler constructs is sited at the member's declaration
(a base, at the constructor).
A local's destructor is a `DestructorCall` sited at its declaration, a
temporary's at the expression that creates it, and a
delete-expression's at the expression (`VirtualDispatch`, Plausible,
through a virtual destructor). An implicit trivial constructor and a
trivial destructor run no code and have no edge. A lambda's closure has
no identity of its own besides its call operator: copying or destroying
one is the copy or destruction of its captures, at that site, and a
capture's initializer runs in the function that creates the closure.
A lambda written in a function template is one node whose edges are
every instantiation's calls; a generic lambda's specializations share it.

**A `throw` is a call.** Each `throw` expression is a `DirectCall`
edge to `__cxa_throw` (a bare `throw;` to `__cxa_rethrow`) at the
expression, so `get-callers --name __cxa_throw` lists every function
that throws, and the effect tools see throws as they see allocations.

**List tools are paged.** `get_callers`, `get_callees`,
`list_callback_sites`, `list_concurrency_entry_points`, and the other
list tools return at most their default `limit` (200 for callers and
callees) and say `truncated: true` with a `nextOffset` when there is
more; pass `offset` to continue ([paging contract](result-contract.md#paging)).

**Graph scope is limited to the selected TUs.** If `get_callers` returns
fewer callers than expected, the missing callers are in TUs you didn't
include. The graph is not a lie — it's scoped.

**`handleX_deprecated` callers are live.** In the Replicator code,
`handleServerSecurityMessage_deprecated` and
`handleClientSecurityMessage_deprecated` are still called from
`OnReceive_deprecated`. These are not dead code — they're active code
paths for older protocol versions. Exception safety audits must include them.

**Build generated headers first.** A missing header can leave a translation
unit only partially indexed. Check `megascope info --files` for per-file parse
status. After generating the missing inputs, run `megascope index --retry-failed`
to retry incomplete parses. [Coverage and refresh rules](index-provenance.md)

**Measuring index time.** Pass `--stats-json timings.json` to a bake to see
which translation units took the longest to parse. The index command prints a
JSON summary when it finishes.

---

## Interpreting results

| `protection` value | Meaning |
|---|---|
| `always_caught` | Every path from the entry points has a handler that catches the type (search exhaustive, every outcome known) |
| `never_caught` | No path is protected — a throw propagates uncaught to an entry point on at least one, and on none is it caught (exhaustive) |
| `sometimes_caught` | Mixed — a caught witness and an uncaught or terminating witness; the response lists both |
| `noexcept_barrier` | Every path terminates at a `noexcept` function or a spawned thread's entry: a throw would `std::terminate` (exhaustive) |
| `observed_caught` / `observed_uncaught` | The same as `always` / `never`, over the paths examined only: the search stopped early (`stopReasons`) or some path's outcome is `unknown` |
| `unknown` | No paths found from the given entry points (wrong entry points, unreachable in the indexed graph, or everything pruned), or every path's outcome is unknown (no context indexed, async boundary) |

Per-path `outcome` (`query_throw_propagation`): `caught` (with
`caughtAt` / `caughtBy`, and `rethrownAt` for handlers that rethrew on
the way), `uncaught` (escapes the entry point), `terminates` (noexcept
or thread boundary, `stopAt` says where), `unknown` (`note` says why).
[path analysis](path-analysis.md) has the propagation order and the limits of the
model.

For security-sensitive functions called on data-plane paths (packet
deserialization, authentication message handlers), `never_caught` from
a packet-processing entry point is the primary finding. Combined with the
function handling untrusted input, it means an adversary can trigger
`std::terminate` by sending malformed data.

`sometimes_caught` is worth deeper investigation: the uncaught path summary
in the response identifies exactly which call chain is unprotected.
