# Bazel Integration for `vycor-cpp` (`megascope` & `check`)

This directory provides out-of-the-box **Bazel** rules ([`bazel/vycor.bzl`](vycor.bzl)), build-configuration flags ([`bazel/BUILD.bazel`](BUILD.bazel) & [`bazel/settings.bzl`](settings.bzl)), and zero-config **Bazel Aspects** ([`bazel/aspects.bzl`](aspects.bzl)) for running `vycor-cpp` inside a Bazel action graph (Bzlmod and legacy `WORKSPACE`, Bazel 7/8/9+).

## Architectural Overview: Hierarchical Map-Reduce Indexing

Traditional Clang LibTooling workflows require generating a monolithic `compile_commands.json` over the entire repository and running a single large indexing process. In Bazel, `vycor-cpp` instead decomposes `megascope` call-graph and control-flow indexing into three cacheable, remotely executable action tiers — mirroring [`buck2/vycor.bzl`](../buck2/vycor.bzl):

```
  [ src/a.cpp ] ──(vycor_tu_index)──► [ tu/libfoo/a.cpp.vycs ] ─┐
                                                                ├──(megascope merge)──► [ libfoo.vycs ] ─┐
  [ src/b.cpp ] ──(vycor_tu_index)──► [ tu/libfoo/b.cpp.vycs ] ─┘                                        │
                                                                                                         ├──(megascope merge)──► [ app.global.vycs ]
  [ src/c.cpp ] ──(vycor_tu_index)──► [ tu/libbar/c.cpp.vycs ] ────(single-TU copy)───► [ libbar.vycs ] ─┤
                                                                                                         │
  [ src/main.cpp ]─(vycor_tu_index)─► [ tu/app/main.cpp.vycs ] ────(single-TU copy)───► [ app.vycs ] ────┘
```

### 1. Per-Translation-Unit Artifacts (`.vycs` / `.mgsc` / `.shard.json` per TU)
Each C/C++ source file is indexed in its own hermetic Bazel action (`VycorTuIndex` or `VycorTuPluginIndex`) using either:

1. **Standalone `vycor-cpp megascope index` (`FixedCompilationDatabase` mode, `-- <compile_flags>`)**, requiring **no `compile_commands.json` file**:
   ```bash
   vycor-cpp megascope index \
     --threads 1 --no-wait \
     --index <out/tu.vycs> \
     --source <src.cpp> \
     -- -std=c++17 -Iinclude -DFOO=1
   ```
2. **In-compilation Clang Frontend Plugin (`libvycor_megascope_plugin.so`)**, via `vycor_plugin` / `vycor_plugin_path` / `@vycor//bazel:vycor_plugin` or injected directly into `cc_library` / `cc_binary` `copts` via `vycor_plugin_copts`:
   ```bash
   clang++ -fsyntax-only \
     -fplugin=/path/to/libvycor_megascope_plugin.so \
     -Xclang -plugin-arg-vycor-megascope -Xclang --output=<out/tu.vycs> \
     -Xclang -plugin-arg-vycor-megascope -Xclang --no-wait \
     -std=c++17 -Iinclude -DFOO=1 <src.cpp>
   ```

- **Zero redundant re-interning**: Single-TU bakes (`--threads 1` with 1 clean TU, or the Clang plugin's `bakeParsedTU`) move-assign the TU's `CallGraph`, `ControlFlowIndex`, and `ChannelIndex` directly into the output snapshot in $O(1)$.
- **Fine-grained action caching**: Bazel tracks the source file and transitive `CcInfo` / `VycorIndexInfo` headers as action inputs. Editing one `.cpp` file re-runs only that single TU's action; all other TUs are served from Bazel's local or remote cache.

### 2. Per-Library / Per-Binary Target Artifacts (`.vycs` / `.mgsc` per target)
Each `vycor_target_index` target (or `vycor_index_aspect` target node) merges its per-TU shards into a target-level `<target>.vycs` artifact using `megascope merge`:

```bash
vycor-cpp megascope merge \
  --no-wait \
  --output <target.vycs> \
  --input-list <target.tu_shards.list>
```

And propagates $O(1)$ Bazel `depset`s (`VycorIndexInfo`) to downstream `deps` with zero analysis-time `depset.to_list()` flattening.

Every `vycor_target_index` exposes `OutputGroupInfo` groups for every granularity:
- `bazel build //pkg:core_vycor` or `bazel build //pkg:core_vycor --output_groups=target` — builds the per-target `core.vycs` artifact.
- `bazel build //pkg:core_vycor --output_groups=tu` — builds all per-TU `.vycs` shards for `core_vycor`.
- `bazel build //pkg:core_vycor --output_groups=tu_src/logger.cpp` (or `--output_groups=tu_src_logger_cpp`) — builds the single per-TU shard for `src/logger.cpp`.
- `bazel build //pkg:app_vycor --output_groups=global` — builds `app_vycor.global.vycs`, merging `app_vycor` and all of its transitive `deps`.

### 3. Global Index Artifact (`.vycs` / `.mgsc` across targets)
`vycor_global_index` streams the transitive `VycorIndexInfo` `depset` directly into an input-list file via `ctx.actions.args().add_all(depset)` and runs `vycor-cpp megascope merge` to produce the full cross-target call-graph and control-flow index.
- **Diamond-dependency safe**: Both Bazel `depset`s (at the target graph level) and `SnapshotIO::merge` (at the `.vycs` / `.mgsc` TU level) deduplicate overlapping shards and translation units idempotently without double-counting edge contributors or call-site contexts.
- Configurable `merge_granularity = "target"` (merges pre-merged per-target `.vycs` artifacts) or `merge_granularity = "tu"` (merges leaf per-TU `.vycs` shards directly).

---

## Workspace Setup (Bzlmod & `WORKSPACE`)

### Bzlmod (`MODULE.bazel` — Bazel 7/8/9+)

```python
bazel_dep(name = "rules_cc", version = "0.2.17")
bazel_dep(name = "vycor", version = "0.1.0")

# Point at a local checkout or git_override / archive_override:
local_path_override(
    module_name = "vycor",
    path = "/path/to/vycor-cpp",
)
```

### Legacy `WORKSPACE`

```python
local_repository(
    name = "vycor",
    path = "/path/to/vycor-cpp",
)
```

### Configuring the `vycor-cpp` Tool Binary & Build Flags

Rules and aspects locate `vycor-cpp` via `PATH` (`vycor-cpp`) by default, or via command-line / `.bazelrc` build settings in `@vycor//bazel`:

| Build Flag | Default | Description |
|---|---|---|
| `--@vycor//bazel:vycor_bin=<label>` | `@vycor//bazel:none` | Hermetic Bazel executable/file target for `vycor-cpp` (staged into sandbox & RBE automatically). |
| `--@vycor//bazel:vycor_executable=<path>` | `"vycor-cpp"` | Host path or binary name on `PATH` when `vycor_bin` is unset. |
| `--@vycor//bazel:vycor_plugin=<label>` | `@vycor//bazel:none` | Hermetic Bazel file target for `libvycor_megascope_plugin.so`. |
| `--@vycor//bazel:vycor_plugin_path=<path>` | `""` | Host path to `libvycor_megascope_plugin.so` for Clang plugin mode. |
| `--@vycor//bazel:clang_executable=<path>` | `"clang++"` | `clang++` executable used when indexing via `libvycor_megascope_plugin.so`. |
| `--@vycor//bazel:index_format=<fmt>` | `"vycs"` | Default output format: `vycs`, `mgsc`, or `shard.json`. |
| `--@vycor//bazel:cxx_standard=<std>` | `"c++17"` | Default `-std=` flag for aspect actions when `copts` omit `-std=...`. |
| `--@vycor//bazel:collapse_paths=<p1,p2>` | `[]` | Path patterns collapsed during aspect call-graph construction. |
| `--@vycor//bazel:lock_types=<t1,t2>` | `[]` | Additional qualified RAII lock types for aspect indexing. |
| `--@vycor//bazel:entry_points=<fn1,fn2>` | `["main"]` | Entry-point function names recorded in aspect-built indexes. |
| `--@vycor//bazel:checks=<spec>` | `""` | Check specification (`--checks=...`) for `vycor_check_aspect`. |
| `--@vycor//bazel:check_format=<fmt>` | `"sarif"` | Report format (`sarif`, `json`, or `text`) for `vycor_check_aspect`. |
| `--@vycor//bazel:fail_on=<sev>` | `"none"` | Severity threshold (`none`, `note`, `warning`, `error`) for `vycor_check_aspect`. |

---

## Zero-Config Bazel Aspects (`bazel/aspects.bzl`)

Just as [`buck2/vycor.bxl`](../buck2/vycor.bxl) indexes and checks existing Buck2 targets without touching `BUCK` files, [`bazel/aspects.bzl`](aspects.bzl) attaches directly to unmodified `cc_library`, `cc_binary`, and `cc_test` targets (reading `CcInfo` headers, `includes`, `quote_includes`, `system_includes`, `defines`, `local_defines`, and `copts`):

```bash
# Build per-TU shards (`tu`), per-target indexes (`target`), and the merged
# transitive index (`global`) across any cc_binary / cc_library graph:
bazel build //:app \
  --aspects=@vycor//bazel:aspects.bzl%vycor_index_aspect \
  --output_groups=global

# Also materialize all leaf TU shards (`tu_all`) and target shards (`target_all`):
bazel build //:app \
  --aspects=@vycor//bazel:aspects.bzl%vycor_index_aspect \
  --output_groups=tu_all,target_all,global

# Emit language-agnostic megascope (.mgsc) snapshots instead of .vycs:
bazel build //:app \
  --aspects=@vycor//bazel:aspects.bzl%vycor_index_aspect \
  --@vycor//bazel:index_format=mgsc \
  --output_groups=global

# Run `vycor-cpp check` across all C/C++ targets under //src/... and emit SARIF:
bazel build //src/... \
  --aspects=@vycor//bazel:aspects.bzl%vycor_check_aspect \
  --output_groups=report
```

---

## Rules Reference (`bazel/vycor.bzl`)

Load the rules in any `BUILD` or `BUILD.bazel` file:

```python
load(
    "@vycor//bazel:vycor.bzl",
    "vycor_cc_binary",
    "vycor_cc_library",
    "vycor_check",
    "vycor_global_index",
    "vycor_plugin_copts",
    "vycor_query",
    "vycor_target_index",
    "vycor_tu_index",
)
```

*(Buck2-symmetric aliases `vycor_cxx_library`, `vycor_cxx_binary`, and `vycor_plugin_flags` are also exported.)*

### `vycor_tu_index`
Produces a `.vycs`, `.mgsc`, or `.shard.json` call-graph shard for a single C/C++ translation unit.

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
Produces per-TU shards (`--output_groups=tu`, `--output_groups=tu_<src>`), a merged per-target artifact (default output and `--output_groups=target`), and an on-demand transitive `--output_groups=global` index. Accepts both `VycorIndexInfo` and native `CcInfo` (`cc_library`) targets in `deps`.

```python
vycor_target_index(
    name = "engine_vycor",
    srcs = ["src/pipeline.cpp", "src/worker.cpp"],
    exported_headers = ["include/engine/pipeline.h"],
    exported_include_dirs = ["include"],
    include_dirs = ["include"],
    deps = [":core_vycor"],
    cxx_standard = "c++17",
    out = "engine.vycs",
)
```

### `vycor_global_index`
Merges the transitive `VycorIndexInfo` closure across one or more `vycor_target_index` / `vycor_tu_index` dependencies into a single global `.vycs` or `.mgsc` artifact.

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
Runs any `vycor-cpp megascope <tool>` query (`graph-summary`, `analyze-dead-code`, `query-exception-safety`, `find-call-chain`, `info`, `dump`, etc.) against a `.vycs` or `.mgsc` target and emits the JSON/NDJSON/TSV result as a build artifact.

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
    deps = [":core"],
    include_dirs = ["include"],
    format = "sarif",
    fail_on = "warning",
    out = "app_check.sarif",
)
```

### Drop-in Macros: `vycor_cc_library` and `vycor_cc_binary`
Wrap `@rules_cc//cc:defs.bzl` `cc_library` and `cc_binary` to automatically register companion `:<name>_vycor` (`vycor_target_index`), `:<name>_vycor_global` (`vycor_global_index`, for binaries), and `:<name>_check` (`vycor_check`) targets alongside the normal C++ library or binary.

---

## Cross-Language (`C++` + `Python`) `.mgsc` & `.shard.json` Indexing

All `bazel/vycor.bzl` rules and `bazel/aspects.bzl` aspects support `index_format = "mgsc"` or `index_format = "shard.json"` (or `--@vycor//bazel:index_format=mgsc`), and `vycor_global_index` accepts cross-language shards (such as `.mgsc`, `.pyrx`, or `.shard.json` artifacts emitted by `pyRx` Python rules) via `deps` or `extra_shards`:

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

See [`examples/bazel_megascope/`](../examples/bazel_megascope/) for a complete multi-target Bzlmod C++ project (`headers_only`, `core`, `engine`, `app`) with a diamond dependency graph demonstrating zero-config aspects, drop-in wrapper macros, per-TU/per-target/global `.vycs` & `.mgsc` artifacts, and hermetic query/check reports.
