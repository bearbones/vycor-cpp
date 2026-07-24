# AGENTS.md — Developer Guide for AI Assistants

This file is the canonical reference for AI coding assistants working in
this repository. Read it before making changes.

---

## What This Repository Does

`vycor-cpp` is a **Clang LibTooling backend** for C++ static analysis
and source-to-source transformation. It uses Clang's AST (Abstract Syntax
Tree) infrastructure to:

1. **Detect fragile ADL/CTAD resolutions** (`anneal`) — header-order-sensitive
   name lookups that silently resolve to different declarations depending on
   what is included.
2. **Apply AST matcher transformations** (`morph`) — rule-driven rewrites of
   C++ source code using Clang's dynamic matcher DSL.
3. **Query control flow and exception context** (`prism`) — one-shot CLI
   queries for call site guards, try/catch scopes, and exception safety.
4. **Interactive call graph MCP server** (`megascope`) — pre-bakes a multi-TU
   call graph and control flow index, then serves interactive queries via
   JSON-RPC for LLM-assisted code analysis (security audits, dead code, etc.).

It is designed to be called by external tooling (e.g. a Python orchestrator,
or an LLM agent using the MCP server for security vulnerability analysis).

---

## Naming

Glass and optics. **vycor** is the Corning glass family the C++ tool draws
its name from (sibling to Pyrex, more demanding spec); the `-cpp` suffix
leaves the bare `vycor` for a future Python sibling. **anneal** relieves
latent stress — ADL/CTAD fragility. **morph** reshapes amorphous structure —
AST transforms. **prism** decomposes one point into its components — call-site
control flow. **megascope** is the persistent far-seeing instrument — the
MCP server.

---

## Feature Layout

The codebase is split into four feature areas:

### `anneal` — ADL/CTAD Analysis

**Headers:** `include/vycor/anneal/`
**Sources:** `src/anneal/`

| File | Purpose |
|---|---|
| `GlobalIndex.h/.cpp` | In-memory database of all function overloads and deduction guides discovered across the project |
| `Indexer.h/.cpp` | Phase-1 AST visitor: walks every translation unit and populates `GlobalIndex` |
| `Analyzer.h/.cpp` | Phase-2 AST visitor: compares each TU's resolved names against `GlobalIndex`; emits `Diagnostic` entries for fragile resolutions |
| `Checkpoint.h/.cpp` | `--checkpoint` journal: append-only per-TU record of phase-1 index contributions and phase-2 diagnostics, so a killed run resumes without re-parsing finished TUs |

The main entry point is `vycor::runAnalysis(compDb, files)` defined in
`Analyzer.h`. It orchestrates both phases and returns a `vector<Diagnostic>`.

Both phases run per-TU on a worker pool (`AnalysisOptions::threadCount`,
same 0=hardware/1=serial semantics as `bakeIndexes`; per-file diagnostic
slots keep output order deterministic). With
`AnalysisOptions::checkpointPath` set (CLI: `--checkpoint`), per-TU
progress is journaled and replayed on resume: phase-1 records carry the
TU's full index contribution (no re-parse on resume), phase-2 records are
keyed on a hash of the whole contributing file set (any phase-1 change
invalidates them all — one TU's diagnostics depend on every other TU's
declarations), stamps are mtime+size (`FileStamp`), an options-fingerprint
mismatch discards the journal wholesale, and a TU whose parse fatally died
twice (attempt records, no completion) is skipped as poisoned instead of
re-killing every resume. See `anneal/Checkpoint.h`.

`--odr-diag` adds cross-TU ODR violation detection: phase 1 records a
`clang::ODRHash` per vague-linkage definition (inline functions, in-class
method bodies, class definitions — external linkage, non-template,
non-system-header), and an index-only pass compares sites project-wide.
`ODR_DivergentDefinition` = one site whose body hashes differently across
TUs (preprocessor-dependent definition); `ODR_DuplicateDefinition` = the
same entity defined at multiple sites with differing content.
Token-identical copies at different sites are deliberately not flagged
(benign vendoring), and method-level echoes of an already-flagged class
are suppressed. This is the ODR class ordinary builds cannot see: linkers
error on duplicate strong symbols but silently keep one arbitrary copy of
mismatched weak/COMDAT definitions. OdrEntries ride checkpoint payloads
and worker shards (journal/shard format v2).

`--isolate-workers [--workers N]` runs the per-TU parses in subprocess
workers (megascope's model): the parent spawns `anneal --index-worker` /
`--analyze-worker` batches through the generic `dispatchIsolated`
dispatcher (`callgraph/WorkerPool.h` — batching, WORKER-TU poison markers,
crash/bisect, shared with the megascope bake), phase-1 workers write
per-TU index shards, and phase-2 workers read the merged index from a
handoff file (`--global-index`) and write diagnostics shards. Composes
with `--checkpoint`: shard results are journaled per TU as they land.
`AnalysisOptions::isolatedRunner` is the test seam (in-process fake
workers, no spawning).

### `morph` — AST-Based Transformations

**Headers:** `include/vycor/morph/`
**Sources:** `src/morph/`

| File | Purpose |
|---|---|
| `MatcherEngine.h/.cpp` | Parses Clang dynamic matcher expression strings; registers callbacks with `MatchFinder`; runs `ClangTool` and accumulates `Replacement` objects |
| `TransformPipeline.h/.cpp` | Orchestrates multiple passes of `MatcherEngine`; merges replacements across passes; optionally writes to disk |

The main entry point is `vycor::TransformPipeline::execute(buildPath, files, dryRun)`.

### `callgraph` — Call Graph and Control Flow Analysis

**Headers:** `include/vycor/callgraph/`
**Sources:** `src/callgraph/`

| File | Purpose |
|---|---|
| `CallGraph.h/.cpp` | Graph data structure: nodes (functions), edges (calls), class hierarchy, virtual overrides |
| `CallGraphBuilder.h/.cpp` | Two-phase AST visitor: Phase 1 indexes nodes/hierarchy, Phase 2 builds edges |
| `CollapseFilter.h/.cpp` | Path-based edge collapse — skips internal edges in specified directories |
| `ControlFlowIndex.h/.cpp` | Per-call-site record of enclosing try/catch scopes and conditional guards |
| `ControlFlowContextVisitor.cpp` | Phase 3 AST visitor: snapshots exception/guard context at each call site |
| `ControlFlowOracle.h/.cpp` | Query engine: exception safety, path analysis, call site context |

**Single-parse build** (used by both `prism` and `megascope`):
`bakeIndexes(compDb, files, ...)` runs all three visitor phases —
declaration/hierarchy index, edge building, CF context — over ONE frontend
parse per TU. No phase barrier is needed because edge building has no
cross-TU reads: the two joins that used to require completed Phase-1 data
(virtual-dispatch fan-out and the function-pointer-through-return join,
`EdgeKind::FunctionPointerReturn`) are performed at query time in
`CallGraph::calleesOf`/`callersOf`. Measured on llvm-project lib/Support
(149 TUs, 12 threads): cold bake 17.1s → 9.9s vs the old two-parse bake,
identical query results. `buildCallGraph` / `buildControlFlowIndex` remain
for callers that need only one of the two indexes (also single-parse).

**Virtual dispatch** is stored as ONE Plausible `VirtualDispatch` edge to the
static target per call site. `CallGraph::calleesOf`/`callersOf` expand it
through the transitive override map at query time, so overrides indexed
later (other TUs, incremental reindex) are visible to existing call sites.
Proven `VirtualDispatch` edges (concrete type known) are never expanded.

**Edge collapse**: When `collapsePaths` is non-empty, edges where BOTH caller and callee
are in collapsed paths are skipped. Boundary edges (non-collapsed caller → collapsed callee)
are preserved. This reduces noise from utility/math headers while keeping entry points visible.

### `mcp` — MCP Server

**Headers:** `include/vycor/mcp/`
**Sources:** `src/mcp/`

| File | Purpose |
|---|---|
| `McpServer.h/.cpp` | JSON-RPC dispatch loop, holds call graph + CF index in memory |
| `McpProtocol.h/.cpp` | MCP stdio framing: newline-delimited JSON, with Content-Length autodetect for legacy clients |
| `McpTools.h/.cpp` | Tool implementations: lookup, callers, callees, call chains, exception safety, dead code, class hierarchy |

**17 MCP tools**: `search_functions`, `lookup_function`, `get_callees`,
`get_callers`, `find_call_chain`, `query_exception_safety`,
`query_call_site_context`, `query_raii_scopes_at_callsite`,
`query_locks_held`, `query_same_lock`, `analyze_dead_code`,
`get_class_hierarchy`, `list_entry_points`, `graph_summary`,
`list_callback_sites`, `list_concurrency_entry_points`, `reindex_tu`.

Identical edges registered by multiple TUs (header-inlined code) are
**deduplicated at insert** with per-TU refcounting, so `removeTU` only drops
an edge when its last contributing TU is removed. Edge records store
interned string IDs internally; public queries materialize `CallGraphEdge`
with resolved strings. Path-walking tools (`find_call_chain`,
`query_locks_held`, `query_same_lock`) accept `max_fan_in` to skip
high-fan-in hubs and report them in the response.

### `ext` — Organization Extension Points

**Headers:** `include/vycor/ext/`
**Sources:** `src/ext/` (core), top-level `ext/` (fork-owned slot-in)

| File | Purpose |
|---|---|
| `Extensions.h/.cpp` | `ExtensionRegistry` (process-wide): custom `AnnealCheck`s, lock/channel type registration, guard classifiers (feature flags). Macros `VYCOR_REGISTER_ANNEAL_CHECK` / `VYCOR_EXTENSION_SETUP` |
| `OrgConfig.h/.cpp` | `--org-config` JSON: declarative lock/channel types, feature-flag patterns, collapse paths, disabled checks |

The scheme exists so organization forks never edit upstream files (clean
merges): declarative config goes in an org JSON passed as `--org-config`;
compiled hooks go in top-level `ext/*.cpp`, which CMake globs **directly
into the `vycor-cpp` and `vycor_tests` executables** (NOT `vycor_lib` —
static-init registrars in an archive would be dropped by the linker).
`ext/tests/*.cpp` is globbed into `vycor_tests`. `ext/examples/` is not
built. Full guide: `docs/EXTENDING.md`.

Key semantics:
- Custom anneal checks run per TU **after** the built-in `AnalyzerVisitor`
  (in `AnalyzerConsumer::HandleTranslationUnit`); one fresh instance per
  TU; emit `Diagnostic::Custom` with `checkName`.
- Registry lock/channel types are merged into the CLI-built
  `LockTypeConfig`/`ChannelTypeConfig` in `main.cpp`
  (`mergeExtensionConfig`, CLI-first order, deduped) — so they participate
  in snapshot config-match/invalidation automatically.
- Guard classification (feature flags) happens at **query/serialization
  time** (`classifyGuard` in prism dump, `query_call_site_context`,
  channel-site listings) — never at index time, so changing patterns does
  not invalidate snapshots. Annotation shape:
  `{"annotation": {"kind": "feature-flag", "name": "<flag>"}}` plus the
  existing `inTrueBranch` for on/off.

---

## CLI Entry Point

`src/main.cpp` uses LLVM's `CommandLine` library with four `cl::SubCommand`
objects:

```
vycor-cpp anneal     --build-path <dir> --source <files...> [--list-checks] [--checks <spec>] [--checks-config <file>] [--threads <n>] [--checkpoint <file>] [--isolate-workers [--workers <n>]] [--org-config <file>]
vycor-cpp morph     --rules-json <file> --build-path <dir> --source <files...> [--dry-run]
vycor-cpp prism    --build-path <dir> --source <files...> --mode <dump|query> [--collapse-paths <pattern>...] [--org-config <file>]
vycor-cpp megascope  --build-path <dir> --source <files...> [--entry-point <name>...] [--collapse-paths <pattern>...] [--snapshot <file>] [--org-config <file>]
```

`--org-config` (anneal/prism/megascope) loads the organization config JSON
(see `include/vycor/ext/OrgConfig.h` and `docs/EXTENDING.md`); its
lock/channel types and collapse paths merge with the equivalent CLI flags.

**Named checks (anneal):** every analysis is a named check with a page
under `docs/checks/` (see `docs/checks/README.md` for the list, groups,
and defaults). Selection sources in order (later wins): a
`.vycor-anneal.json` found walking up from the working directory (or
`--checks-config <file>`), the `--checks=<spec>` flag (clang-tidy style:
`all,-noisy`, `-name` disables, groups expand), then the legacy toggles
(`--odr-diag`, `--coverage-diag`, `--dead-code`) as appended enables. The
resolver lives in `anneal/CheckSet.h/.cpp`; unknown names are a hard
error; main.cpp maps the resolved set onto `AnalysisOptions` booleans and
forwards it to isolated workers as `--checks=-all,<resolved...>`.
Organization checks (per-TU `AnnealCheck` and merged-index `IndexCheck`,
`ext/Extensions.h`) join the same namespace via `name()`; org groups via
`registry.addCheckGroup`.

Each subcommand has its own scoped options (declared with `llvm::cl::sub(...)`).
To add a new subcommand, follow the pattern in `main.cpp`:
1. Declare a `static llvm::cl::SubCommand MyCmd("name", "description")`.
2. Declare option variables with `llvm::cl::sub(MyCmd)`.
3. Add an `if (MyCmd) { ... }` branch in `main()`.

### prism vs megascope

- **prism**: One-shot CLI tool. Parses AST per invocation, outputs JSON to stdout. Best for quick single-file investigations.
- **megascope**: Persistent server. Pre-bakes a unified cross-TU call graph at startup, then serves interactive queries via JSON-RPC over stdio. **Always use megascope for security audits or multi-file analysis** — prism is single-TU and cannot see cross-file callers.

### Edge Collapse (--collapse-paths)

Both `prism` and `megascope` accept `--collapse-paths` to reduce noise from
header-inlined utility code. Patterns are path component substrings:

```bash
--collapse-paths Client/Math --collapse-paths Client/Core
```

This matches paths containing `/Client/Math/` or `/Client/Core/` as directory
components. Internal edges (both endpoints in collapsed paths) are skipped;
boundary edges (non-collapsed caller → collapsed callee) are preserved.

---

## Build System

`CMakeLists.txt` (top-level):
- Requires CMake 3.20+, C++17.
- Reads the project's own SemVer from the root `VERSION` file into
  `project(vycor-cpp VERSION ...)`, and `configure_file`s
  `include/vycor/Version.h.in` into `${CMAKE_BINARY_DIR}/generated/vycor/Version.h`
  (`VYCOR_VERSION_STRING` etc.) — `main.cpp` uses it to make `vycor-cpp
  --version` report both the vycor-cpp version and the embedded LLVM
  version. See `COMPATIBILITY.md` "Tagging and releases" for the version/tag
  scheme this feeds.
- Iterates `VYCOR_SUPPORTED_LLVM_VERSIONS` (currently `21 20 18`,
  newest first) via `find_package()`. The CI matrix in
  `.github/workflows/ci.yml` mirrors this list.
- Falls back to `extern/llvm-project` submodule if no system package matches.
- Disables RTTI (`-fno-rtti`) to match LLVM's default.
- A lightweight compatibility header (`include/vycor/compat/ClangVersion.h`)
  provides `VYCOR_LLVM_AT_LEAST(major)` for version-conditional code.
- See `COMPATIBILITY.md` for the support matrix, known API differences
  between supported LLVM majors, and the procedure for adding/removing a
  supported version.
- `install(TARGETS vycor-cpp RUNTIME DESTINATION ...)` in `src/CMakeLists.txt`
  supports `cmake --install` staging for release tarballs (no CPack — a
  single binary doesn't need it). Used by `.github/workflows/release.yml`.

`src/CMakeLists.txt`:
- Builds `vycor_lib` from `anneal/*.cpp`, `morph/*.cpp`, `callgraph/*.cpp`, `mcp/*.cpp`, and `ext/*.cpp`.
- Builds `vycor-cpp` executable from `main.cpp` plus a `CONFIGURE_DEPENDS`
  glob of top-level `ext/*.cpp` (organization slot-in — attached to the
  executable, not the archive, so static registrars survive linking).
- Links against: `clangTooling`, `clangDynamicASTMatchers`, `clangASTMatchers`,
  `clangAST`, `clangBasic`, `clangFrontend`, `clangSema`, `clangSerialization`,
  `clangRewrite`, `clangToolingCore`, `LLVMSupport`.

`tests/CMakeLists.txt`:
- Resolves Catch2 v3.10+ in three tiers (no network at configure time):
  1. Pre-existing `Catch2::Catch2WithMain` imported target.
  2. `find_package(Catch2 3.10 CONFIG)` — system / vcpkg / Conan / Spack.
  3. `extern/Catch2` submodule (pinned to v3.10.0 upstream by default).
- Defines `PROJECT_SOURCE_DIR` for example file paths.

---

## Build and Test Commands

```bash
# Configure (Release) — picks the newest supported LLVM automatically
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release

# Configure against a specific LLVM version (e.g. 20)
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/usr/lib/llvm-20

# Configure (Debug — faster iteration)
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug

# Build the CLI tool
cmake --build build --target vycor-cpp

# Build tests
cmake --build build --target vycor_tests

# Run tests
cd build && ctest --output-on-failure

# Run a specific test tag
cd build && ctest -R "GlobalIndex" --output-on-failure
```

Run tests from the project root, or ensure `PROJECT_SOURCE_DIR` is set
correctly (the CMake build sets it automatically via a compile definition).

---

## Adding a New Analysis to anneal

1. **If new data is needed globally**, extend `GlobalIndex` (`GlobalIndex.h`
   and `GlobalIndex.cpp`) with a new entry struct and lookup method.

2. **Collect data** by extending `IndexerVisitor` in `Indexer.cpp`:
   - Add a `VisitXxxDecl(clang::XxxDecl *decl)` method.
   - Populate the new `GlobalIndex` fields.

3. **Detect fragility** by extending `AnalyzerVisitor` in `Analyzer.cpp`:
   - Add a `VisitXxxExpr` or `VisitXxxDecl` method.
   - Query `GlobalIndex` and push `Diagnostic` entries.

4. **Add tests** in `tests/test_adl_ctad.cpp` following the existing pattern
   of constructing in-memory code strings and running `runToolOnCodeWithArgs`.

---

## Adding a New Transform to morph

1. Create a `TransformRule` struct with:
   - `matcherExpression`: a Clang dynamic matcher string
     (e.g. `"functionDecl(hasName(\"foo\"))"`)
   - `bindId`: the bind ID for the root node (e.g. `"fn"`)
   - `callback`: a `ReplacementCallback` lambda that receives the
     `MatchFinder::MatchResult` and returns `vector<Replacement>`

2. Add the rule to a pipeline pass:
   ```cpp
   TransformPipeline pipeline;
   pipeline.addPass({ rule1, rule2 });
   pipeline.execute(buildPath, sourceFiles, dryRun);
   ```

3. Test using the example files in `examples/` as TDD targets.

The JSON rules format for the `morph` CLI subcommand is not yet implemented
(see TODOs below). For now, rules are added programmatically.

---

## Key Design Patterns

### Two-Phase Analysis

`runAnalysis()` runs two separate `ClangTool` passes over the same file set:

```
Pass 1: IndexerActionFactory → IndexerConsumer → IndexerVisitor
  → fills GlobalIndex with all declarations across all TUs

Pass 2: AnalyzerActionFactory → AnalyzerConsumer → AnalyzerVisitor
  → reads GlobalIndex, walks each TU, emits Diagnostics
```

The two-pass design is necessary because a single TU cannot know what
declarations exist in other TUs that are not included.

### RecursiveASTVisitor

All AST traversal uses Clang's `RecursiveASTVisitor<Derived>` CRTP base.
Override `VisitXxxDecl` or `VisitXxxExpr` methods; return `true` to continue
traversal, `false` to stop.

### FrontendActionFactory

`ClangTool::run()` takes a `FrontendActionFactory*`. The pattern used here:

```
XxxActionFactory → creates XxxAction per TU
XxxAction → creates XxxConsumer
XxxConsumer::HandleTranslationUnit → drives XxxVisitor
```

### MatchFinder Callbacks

`MatcherEngine` uses Clang's `MatchFinder` with a nested `CallbackAdapter`
class that bridges between `MatchFinder::MatchCallback` and the user-supplied
`ReplacementCallback` function objects.

---

## Development Workflow

Changes land on `main` via pull requests. Feature branches use a topical
prefix (`fix/`, `feat/`, `docs/`, etc.) and a short description, e.g.
`fix/llvm-18-threadpool-compat`.

Commit convention: descriptive imperative messages, no ticket numbers required.

Long-lived `release/llvm-18`, `release/llvm-20`, `release/llvm-21` branches
track each supported LLVM major; bug fixes are cherry-picked from `main` to
each applicable release branch via the `backport/llvm-NN` PR label. See
`COMPATIBILITY.md` for the full cherry-pick policy.

---

## Cutting a Release

A pushed tag matching `vX.Y.Z` (on `main`) or `vX.Y.Z-llvmNN` (on a
`release/llvm-NN` branch) triggers `.github/workflows/release.yml`, which
builds Linux + macOS binaries for the relevant LLVM major(s), attaches them
as tarballs to a GitHub Release, and updates the `bearbones/homebrew-vycor-cpp`
tap. Bump the root `VERSION` file, commit, then tag and push — see
`COMPATIBILITY.md` "Tagging and releases" for the exact command sequence and
the full tag grammar. Test with `workflow_dispatch`'s `dry_run` input before
relying on a real tag push.

---

## Current TODOs

| Area | TODO |
|---|---|
| `morph` CLI | Parse `--rules-json` and populate `TransformPipeline` passes |
| `TransformPipeline` | Apply replacements to disk between passes (not just collect) |
| `TransformPipeline` | Write final replacements to disk when `!dryRun` |
| Tests | Implement full integration tests in `test_transforms.cpp` (currently stubs) |
| Matcher DSL | Define and document the JSON schema for rules files |
| `megascope` | Add `--source-glob` flag for directory-based source selection |
| `find_call_chain` | Annotate path edges with try/catch depth and caught types |
| `call-site-context` | Include catch handler type and body summary in response |
| Concurrency | `query_raii_scopes_at_callsite` — list RAII objects live at a call site |
| Concurrency | `query_locks_held(function)` — trace callers to find implicit locks |
| Concurrency | `query_same_lock(fn_a, fn_b)` — confirm two functions share a lock |

---

## Namespace

All types and functions live in `namespace vycor`. There are no nested
namespaces per feature — the feature folders are a filesystem organization
only, not a namespace boundary.

---

## External Dependencies

- **LLVM/Clang 18, 20, or 21** — system package preferred (`find_package`),
  git submodule at `extern/llvm-project` as fallback. Selectable via
  `CMAKE_PREFIX_PATH=/usr/lib/llvm-NN`.
- **Catch2 v3.10+** (tests only) — resolved in three tiers: pre-existing
  imported target -> `find_package` -> `extern/Catch2` submodule.
  Network is not required at configure time. See README.md "Hermetic
  and internal-package-manager builds" for organizations using
  internal mirrors or alternate package managers.
- No other runtime dependencies.
- **macOS install path**: the self-hosted Homebrew tap
  `bearbones/homebrew-vycor-cpp` (`brew tap bearbones/vycor-cpp && brew
  install vycor-cpp`), not `homebrew/core` — see `COMPATIBILITY.md`
  "Tagging and releases". Each formula `depends_on "llvm@NN"`: Homebrew's
  LLVM formulas link Clang/LLVM as shared libraries, so that dependency is
  required at runtime, not just at build time.
