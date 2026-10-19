# Buck2 Integration for `vycor-cpp` (`megascope` & `check`)

This directory provides out-of-the-box **Buck2** rules ([`buck2/vycor.bzl`](vycor.bzl)) and zero-config **BXL** scripts ([`buck2/vycor.bxl`](vycor.bxl)) for running `vycor-cpp` inside a Buck2 action graph.

## Architectural Overview: Hierarchical Map-Reduce Indexing

Traditional Clang LibTooling workflows require generating a monolithic `compile_commands.json` over the entire repository and running a single large indexing process. In Buck2, `vycor-cpp` instead decomposes `megascope` call-graph and control-flow indexing into three cacheable, remotely executable action tiers:

```
  [ src/a.cpp ] ──(vycor_tu_index)──► [ tu/libfoo/a.cpp.vycs ] ─┐
                                                                ├──(megascope merge)──► [ libfoo.vycs ] ─┐
  [ src/b.cpp ] ──(vycor_tu_index)──► [ tu/libfoo/b.cpp.vycs ] ─┘                                        │
                                                                                                         ├──(megascope merge)──► [ app.global.vycs ]
  [ src/c.cpp ] ──(vycor_tu_index)──► [ tu/libbar/c.cpp.vycs ] ────(single-TU copy)───► [ libbar.vycs ] ─┤
                                                                                                         │
  [ src/main.cpp ]─(vycor_tu_index)─► [ tu/app/main.cpp.vycs ] ────(single-TU copy)───► [ app.vycs ] ────┘
```

### 1. Per-Translation-Unit Artifacts (`.vycs` per TU)
Each C/C++ source file is indexed in its own hermetic Buck2 action using either:

1. **Standalone `vycor-cpp megascope index` (`FixedCompilationDatabase` mode, `-- <compile_flags>`)**, requiring **no `compile_commands.json` file**:
   ```bash
   vycor-cpp megascope index \
     --threads 1 --no-wait \
     --index <out/tu.vycs> \
     --source <src.cpp> \
     -- -std=c++17 -Iinclude -DFOO=1
   ```
2. **In-compilation Clang Frontend Plugin (`libvycor_megascope_plugin.so`)**, piggybacking on `clang++ -c` so the AST is parsed once for both object codegen (`.o`) and `.vycs` shard emission (or via `vycor_plugin` / `vycor_plugin_path` / `vycor_plugin_flags` in `buck2/vycor.bzl`):
   ```bash
   clang++ -c <src.cpp> -o <out/src.o> \
     -fplugin=/path/to/libvycor_megascope_plugin.so \
     -Xclang -plugin-arg-vycor-megascope -Xclang --output=<out/tu.vycs> \
     -Xclang -plugin-arg-vycor-megascope -Xclang --no-wait \
     -std=c++17 -Iinclude -DFOO=1
   ```

- **Zero redundant re-interning**: Single-TU bakes (`--threads 1` with 1 clean TU, or the Clang plugin's `bakeParsedTU`) move-assign the TU's `CallGraph`, `ControlFlowIndex`, and `ChannelIndex` directly into the output snapshot in $O(1)$ without re-interning strings or re-hashing maps.
- **Fine-grained action caching**: Buck2 tracks the source file and transitive exported headers as action inputs. Editing one `.cpp` file re-runs only that single TU's `vycor_tu_index` action; all other TUs are served from Buck2's local or remote cache.

### 2. Per-Library / Per-Binary Target Artifacts (`.vycs` per target)
Each `vycor_target_index` target merges its per-TU `.vycs` shards into a target-level `<target>.vycs` artifact using the `megascope merge` verb:

```bash
vycor-cpp megascope merge \
  --no-wait \
  --output <target.vycs> \
  --input-list <target.tu_shards.list>
```

And propagates a Buck2 `TransitiveSet` (`VycorIndexTSet`) to downstream `deps` in $O(1)$ per target.

Every `vycor_target_index` exposes sub-targets for every granularity:
- `buck2 build //pkg:core_vycor` or `buck2 build //pkg:core_vycor[target]` — builds the per-target `core.vycs` artifact.
- `buck2 build //pkg:core_vycor[tu]` — builds all per-TU `.vycs` shards for `core_vycor`.
- `buck2 build //pkg:core_vycor[tu/src/logger.cpp]` — builds the single per-TU `.vycs` shard for `src/logger.cpp`.
- `buck2 build //pkg:app_vycor[global]` — builds `app_vycor.global.vycs`, merging `app_vycor` and all of its transitive `deps`.

### 3. Global Index Artifact (`.vycs` across targets)
`vycor_global_index` projects the transitive `VycorIndexTSet` into an argsfile and runs `vycor-cpp megascope merge` to produce the full cross-target call-graph and control-flow index.
- **Diamond-dependency safe**: Both `VycorIndexTSet` (at the Buck2 target graph level) and `SnapshotIO::merge` (at the `.vycs` TU level) deduplicate overlapping shards and overlapping translation units idempotently without double-counting edge contributors or call-site contexts.
- Configurable `merge_granularity = "target"` (merges pre-merged per-target `.vycs` artifacts) or `merge_granularity = "tu"` (merges leaf per-TU `.vycs` shards directly).

---

## Rules Reference (`buck2/vycor.bzl`)

Load the rules in any `BUCK` file:

```python
load(
    "@vycor//buck2:vycor.bzl",
    "vycor_check",
    "vycor_cxx_binary",
    "vycor_cxx_library",
    "vycor_global_index",
    "vycor_query",
    "vycor_target_index",
    "vycor_tu_index",
)
```

### `vycor_tu_index`
Produces a `.vycs` call-graph shard for a single C/C++ translation unit.

```python
vycor_tu_index(
    name = "logger_tu",
    src = "src/logger.cpp",
    exported_headers = ["include/core/logger.h"],
    include_dirs = ["include"],
    cxx_standard = "c++17",
    out = "logger.tu.vycs",
)
```

### `vycor_target_index`
Produces per-TU `.vycs` shards (`[tu]`, `[tu/<src>]`), a merged per-target `.vycs` artifact (default output and `[target]`), and a transitive `[global]` `.vycs` sub-target.

```python
vycor_target_index(
    name = "engine_vycor",
    srcs = ["src/pipeline.cpp", "src/worker.cpp"],
    exported_headers = ["include/engine/pipeline.h"],
    exported_preprocessor_flags = ["-Iinclude"],
    include_dirs = ["include"],
    deps = [":core_vycor"],
    cxx_standard = "c++17",
    out = "engine.vycs",
)
```

### `vycor_global_index`
Merges the transitive `VycorIndexTSet` across one or more `vycor_target_index` / `vycor_tu_index` dependencies into a single global `.vycs` artifact.

```python
vycor_global_index(
    name = "global_index",
    deps = [":app_vycor"],
    entry_points = ["main"],
    merge_granularity = "target",  # or "tu"
    out = "megascope.global.vycs",
)
```

### `vycor_query`
Runs any `vycor-cpp megascope <tool>` query (`graph-summary`, `analyze-dead-code`, `query-exception-safety`, `find-call-chain`, `info`, `dump`, etc.) against a `.vycs` target and emits the JSON/NDJSON/TSV result as a build artifact.

```python
vycor_query(
    name = "dead_code_report",
    index = ":global_index",
    tool = "analyze-dead-code",
    entry_points = ["main"],
    out = "dead_code_report.json",
)
```

### `vycor_check`
Runs `vycor-cpp check` over a target's sources and produces a `.sarif`, `.json`, or `.txt` report artifact.

```python
vycor_check(
    name = "app_check",
    srcs = ["src/main.cpp", "src/pipeline.cpp", "src/worker.cpp"],
    headers = ["include/engine/pipeline.h"],
    deps = [":core_vycor"],
    include_dirs = ["include"],
    format = "sarif",
    fail_on = "warning",
    out = "app_check.sarif",
)
```

### Drop-in Macros: `vycor_cxx_library` and `vycor_cxx_binary`
Wrap `native.cxx_library` / `native.cxx_binary` to automatically register companion `:<name>_vycor` (`vycor_target_index`), `:<name>_vycor_global` (`vycor_global_index`, for binaries), and `:<name>_check` (`vycor_check`) targets alongside the normal C++ library or binary.

---

## Zero-Config BXL Usage (`buck2/vycor.bxl`)

To index or analyze existing `cxx_library` and `cxx_binary` targets in a Buck2 repository **without modifying any `BUCK` files**, run the BXL entry points:

```bash
# Build per-TU .vycs shards, per-target .vycs artifacts, and the global .vycs
# index across all C/C++ targets under //src/... and print a JSON manifest:
buck2 bxl //buck2:vycor.bxl:index -- --targets //src/...

# Emit language-agnostic megascope (.mgsc) snapshots instead of .vycs:
buck2 bxl //buck2:vycor.bxl:index -- --targets //src/... --index_format mgsc

# Run vycor-cpp check across all C/C++ targets under //src/... and emit SARIF:
buck2 bxl //buck2:vycor.bxl:check -- --targets //src/... --format sarif
```

---

## Cross-Language (`C++` + `Python`) `.mgsc` & `.shard.json` Indexing

All `buck2/vycor.bzl` rules (`vycor_tu_index`, `vycor_target_index`, `vycor_global_index`) support `index_format = "mgsc"` or `index_format = "shard.json"` (or an explicit `out = "<name>.mgsc"` / `out = "<name>.shard.json"`), and `vycor_global_index` accepts cross-language shards (such as `.mgsc`, `.pyrx`, or `.shard.json` artifacts emitted by `pyrex` Python rules) via `deps` or `extra_shards`:

```python
vycor_global_index(
    name = "polyglot_global",
    deps = [":cpp_engine_vycor"],
    extra_shards = [":py_service_shard"],
    index_format = "mgsc",
    out = "polyglot.global.mgsc",
)
```

---

## Working Example Project

See [`examples/buck2_megascope/`](../examples/buck2_megascope/) for a complete multi-target C++ project (`core`, `engine`, `app`) with a diamond dependency graph demonstrating per-TU, per-target, and global `.vycs` / `.mgsc` artifacts.

