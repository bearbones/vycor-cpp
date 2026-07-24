# vycor-cpp

A Clang LibTooling backend for safe, AST-aware C++ refactoring and static
analysis. It exposes four features as subcommands:

- **anneal** — detect problems such as fragile ADL/CTAD resolutions across translation units. Like if `clang-tidy` went Super Saiyan.
- **morph** — apply rule-driven, multi-pass AST matcher transformations
- **prism** — query control flow, exception handling, and call site guard context from the command line
- **megascope** — start an MCP (Model Context Protocol) server for interactive call graph queries, designed for LLM-assisted code analysis

Designed as a backend for external systems (e.g. a Python script translating
a custom DSL, or an LLM agent performing security audits via the MCP server),
and as a **base for organization forks**: lock types, feature-flag
conventions, and custom checks slot in without touching upstream code — see
[Customizing for your organization](#customizing-for-your-organization).

---

## Installing

### Homebrew (macOS, recommended)

```bash
brew tap bearbones/vycor-cpp
brew install vycor-cpp          # newest supported LLVM major
# or pin a major: brew install vycor-cpp@20
```

The tap formulas declare `depends_on "llvm@NN"`, so the required LLVM
runtime libraries are installed and wired up automatically.

### Release tarballs (Linux and macOS)

Each [GitHub Release](https://github.com/bearbones/vycor-cpp/releases)
attaches prebuilt binaries as
`vycor-cpp-vX.Y.Z-<os>-llvmNN.tar.gz` (`linux-x86_64`, `macos-arm64`).

The binaries are **not fully self-contained**: they link LLVM dynamically
and need the matching LLVM runtime present.

```bash
# Linux: runtime .so only (lighter than the full -dev package), from apt.llvm.org
sudo apt-get install libllvm21

tar xzf vycor-cpp-v0.2.0-linux-x86_64-llvm21.tar.gz
./bin/vycor-cpp --version
```

On macOS a raw tarball works too, after `brew install llvm@NN` and (if
Gatekeeper objects) `xattr -d com.apple.quarantine` on the extracted
binary — but the tap handles all of that for you.

`vycor-cpp --version` reports the tool version, the embedded LLVM version,
and the host compiler that built it.

### From source

See [Building](#building) — required if you want an organization fork with
compiled extensions (`ext/`), a different LLVM major, or an unsupported
platform.

---

## Building

Requires CMake 3.20+, a C++17 compiler, and Ninja (recommended).

### Dependencies

| Dependency | Required | How it's resolved |
|---|---|---|
| LLVM/Clang | 18, 20, or 21 | system package preferred; submodule fallback |
| Catch2 | v3.10+ (tests only) | imported target -> `find_package` -> submodule fallback |

Neither dependency is fetched at configure time. Hermetic-build
environments and organizations using internal package managers can
satisfy each dependency at any tier without modifying this build
system — see "Hermetic and internal-package-manager builds" below.

### Quick start

```bash
# Clone with submodules (LLVM and Catch2)
git clone --recurse-submodules https://github.com/bearbones/vycor-cpp.git
cd vycor-cpp

# If you only need Catch2 (recommended — system LLVM is much faster):
# git submodule update --init extern/Catch2

# The LLVM submodule uses sparse checkout; after cloning you may need:
cd extern/llvm-project
git sparse-checkout set llvm clang cmake third-party
cd ../..

# Configure (first time is slow due to LLVM build if not using system LLVM)
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release

# Build the tool
cmake --build build --target vycor-cpp

# Build and run tests
cmake --build build --target vycor_tests
cd build && ctest --output-on-failure
```

### LLVM/Clang resolution

The build tries each supported LLVM major version in turn (21, then 20,
then 18) by probing well-known install paths, then falls back to the
`extern/llvm-project` submodule. To pin a specific version, point
`CMAKE_PREFIX_PATH` at it:

```bash
cmake -B build -G Ninja -DCMAKE_PREFIX_PATH=/usr/lib/llvm-20
```

See `COMPATIBILITY.md` for the support matrix, known API differences
between LLVM majors, and the release-branch/backport policy.

On macOS with Apple Silicon, AArch64 support is included automatically.
For Intel-only builds: `-DLLVM_TARGETS_TO_BUILD=X86`.

### Catch2 resolution

The test build tries Catch2 in this order:

1. **Pre-existing `Catch2::Catch2WithMain` imported target** — if a parent
   project, internal package manager, or CMake toolchain file has already
   defined it, we use it as-is.
2. **`find_package(Catch2 3.10 CONFIG)`** — covers system installs, vcpkg,
   Conan, Spack, and any layout that ships Catch2's CMake config files.
   Override the search with `CMAKE_PREFIX_PATH` or `Catch2_DIR`.
3. **Submodule at `extern/Catch2`** — pinned to v3.10.0 upstream by default.

Tests can be disabled entirely with `-DVYCOR_BUILD_TESTS=OFF`.

### Hermetic and internal-package-manager builds

The dependency resolution is structured so that organizations vendoring
their own dependencies (internal forks, mirror-only network, package
managers like Spack/Conan/vcpkg) do not need to modify any of this
project's build files:

- **Replace dependency sources** — point `.gitmodules` at internal
  mirrors (`url = https://internal-mirror.example.com/Catch2.git`) and
  reuse the submodule path. CMake never reads the URL.
- **Provide via package manager** — set `CMAKE_PREFIX_PATH` for
  `find_package` to find your toolchain's installs. Both LLVM and
  Catch2 are looked up with `find_package` first.
- **Provide via parent project** — if this repository is consumed as a
  subdirectory of a larger build that already exposes
  `Catch2::Catch2WithMain` (or LLVM via `LLVM_DIR`/`Clang_DIR`), no
  configuration is needed; the existing targets/packages are honored.

---

## Usage

All subcommands consume a `compile_commands.json` compilation database
(`--build-path` points at its directory).

### anneal — Analyze for Fragile ADL/CTAD

ADL (Argument-Dependent Lookup) and CTAD (Class Template Argument
Deduction) can silently resolve to different declarations depending on
which headers a translation unit includes — a correctness hazard standard
compilers do not diagnose. `anneal` indexes all overloads and deduction
guides across the project (phase 1), then re-walks each TU comparing
resolved call sites against the global index (phase 2).

```bash
./build/vycor-cpp anneal \
  --build-path /path/to/compile_commands_dir \
  --source file1.cpp file2.cpp
```

Output example:

```
src/logic.cpp:42:5: Fragile ADL resolution: MathLib::scale(Vector, double) exists in
    Extension.hpp but is not visible here. The current call resolves to
    MathLib::scale(Vector, int). Include Extension.hpp or explicitly qualify the call.
```

**ODR violation detection** (`--odr-diag`): linkers reject duplicate
*strong* symbols, but mismatched *vague-linkage* definitions — inline
functions, in-class method bodies, class definitions — are silently
merged, with the linker keeping one arbitrary copy. `anneal` hashes every
such definition during indexing (`clang::ODRHash`) and compares across
the whole project, catching both the two-headers-define-the-same-thing
case and the subtler one-header-whose-body-depends-on-`-D`-flags case:

```
./limits.hpp:2: ODR violation: 'limits' at ./limits.hpp:2 compiles to 2 different
    definitions across TUs — its body depends on preprocessor state that differs
    between compile commands. Every TU must see an identical definition.
```

Token-identical copies at different paths are not flagged (vendored
duplicates are benign), so the signal stays clean.

Every anneal analysis is a **named check** with its own documentation
page — see [docs/checks/README.md](docs/checks/README.md) for the full
list, defaults, and groups. Select checks clang-tidy-style with
`--checks=all,-noisy` or a `.vycor-anneal.json` discovered from the
working directory upward:

```json
{ "checks": ["all", "-coverage-properties", "-compute-heavy"] }
```

Other optional flags: `--warn-same-score`, `--model-convertibility`, the
legacy check toggles (`--coverage-diag`, `--odr-diag`, `--dead-code` with
`--entry-point`), and `--org-config` (organization checks/config — see
below).

Both analysis phases run per-TU on a worker pool (`--threads`, same
semantics as prism/megascope). For long runs, `--checkpoint <file>`
journals per-TU progress: a run killed partway (OOM, Ctrl-C, CI timeout)
resumes where it left off instead of restarting, source edits invalidate
exactly the affected entries, and a TU whose parse fatally died twice is
skipped with a warning instead of re-killing every resume.
`--isolate-workers` (with `--workers N`) additionally runs the parses in
subprocess workers, megascope-style: a TU that crashes its worker costs
only that TU, and the parent survives. All three flags compose.

```bash
./build/vycor-cpp anneal \
  --build-path /path/to/compile_commands_dir \
  --source file1.cpp file2.cpp \
  --threads 0 --checkpoint /tmp/anneal.vycj
```

### morph — Apply Transformations

```bash
./build/vycor-cpp morph \
  --rules-json rules.json \
  --build-path /path/to/compile_commands_dir \
  --source file1.cpp file2.cpp \
  --dry-run
```

Rules are JSON objects containing a Clang dynamic AST matcher expression,
a bind ID for the root node, and an action; passes run in sequence, each
building on the previous. `--dry-run` collects replacements without
writing them to disk.

### prism — Query Control Flow and Exception Context

One-shot CLI queries against a compilation database. Useful for quick
investigations of individual files.

```bash
# Dump all call sites with guards and try/catch context
./build/vycor-cpp prism \
  --build-path /path/to/compile_commands_dir \
  --source SecurityModule.cpp \
  --mode dump

# Query exception protection for a specific function
./build/vycor-cpp prism \
  --build-path /path/to/compile_commands_dir \
  --source SecurityModule.cpp \
  --mode query --query-type exception-protection \
  --function "RBX::Security::ServerSecurityInstance::onClientChallengeResponse"

# Query call site context (try/catch, guards) at a specific location
./build/vycor-cpp prism \
  --build-path /path/to/compile_commands_dir \
  --source ServerReplicator.cpp \
  --mode query --query-type call-site-context \
  --call-site "ServerReplicator.cpp:2342:13"
```

**Query types**: `exception-protection`, `call-site-context`, `all-path-contexts`,
`throw-propagation`, `nearest-catches`

**Edge collapse** — reduce noise from header-inlined utility code:
```bash
./build/vycor-cpp prism \
  --build-path /path/to/compile_commands_dir \
  --source SecurityModule.cpp \
  --collapse-paths Client/Math \
  --collapse-paths Client/Core \
  --mode dump
```

This skips internal edges where both caller and callee are in collapsed
paths, while preserving boundary edges (calls from non-collapsed code into
the collapsed region). `--skip-paths` excludes matching TUs entirely.

Other useful flags: `--threads`, `--pch-dir` (PCH reuse), `--lock-types`
(extra RAII lock types), `--channel-types-json` (queue/bus tracing),
`--org-config`.

### megascope — Interactive Call Graph MCP Server

Starts a persistent MCP server that pre-bakes a unified cross-TU call
graph and control flow index, then serves interactive queries via JSON-RPC
over stdio.

```bash
./build/vycor-cpp megascope \
  --build-path /path/to/compile_commands_dir \
  --source file1.cpp --source file2.cpp --source file3.cpp \
  --entry-point "main" \
  --collapse-paths Client/Math \
  --snapshot /tmp/proj.snap
```

**17 MCP tools**: `search_functions`, `lookup_function`, `get_callees`,
`get_callers`, `find_call_chain`, `query_exception_safety`,
`query_call_site_context`, `query_raii_scopes_at_callsite`,
`query_locks_held`, `query_same_lock`, `analyze_dead_code`,
`get_class_hierarchy`, `list_entry_points`, `graph_summary`,
`list_callback_sites`, `list_concurrency_entry_points`, `reindex_tu`.
See `docs/mcp-usage.md`.

Scaling flags: `--snapshot` (warm starts — only changed TUs are
re-indexed), `--threads`, `--pch-dir`, `--isolate-workers`/`--workers`
(subprocess baking: a crashing TU costs only that TU), `--stats-json`.

**Key difference from prism**: `megascope` indexes all specified sources
into a unified cross-TU call graph held in memory. `prism` parses per
invocation and is limited to single-TU context. For security audits or
multi-file analysis, always use `megascope`.

---

## Customizing for your organization

Organizations forking/mirroring this repo can add their own semantics
without editing upstream files (so upstream merges stay conflict-free):

- **Org config JSON** (`--org-config vycor.org.json`) — declarative:
  in-house lock types for the RAII/lock queries, channel/queue types,
  **feature-flag patterns** (regexes over guard conditions; matching
  guards are annotated with the flag name in prism/MCP output, so "this
  path only runs with `FFlag::NewNav` on" becomes queryable), collapse
  paths, and per-check kill switches.
- **`ext/` slot-in directory** — compiled hooks: custom **anneal checks**
  (full AST access, registered with `VYCOR_REGISTER_ANNEAL_CHECK`),
  arbitrary guard classifiers, and code-registered type lists. `ext/*.cpp`
  is auto-globbed into the build; `ext/tests/*.cpp` into the test binary.
- **CLI flags** (`--lock-types`, `--channel-types-json`,
  `--collapse-paths`) — ad-hoc, merged with the above.

Start from `ext/examples/ExampleOrgExtension.cpp` and
`ext/examples/vycor.org.json`. Full guide: **[docs/EXTENDING.md](docs/EXTENDING.md)**.

---

## Project Structure

```
CMakeLists.txt                  Top-level build configuration
COMPATIBILITY.md                LLVM support matrix, release/backport policy
extern/llvm-project/            LLVM/Clang submodule (sparse checkout)
extern/Catch2/                  Catch2 submodule (tests)

include/vycor/
  anneal/                       ADL/CTAD analysis (GlobalIndex, Indexer,
                                Analyzer, DeadCodeAnalyzer)
  morph/                        Transform pipeline (MatcherEngine,
                                RulesParser, TemplateEngine, TransformPipeline)
  callgraph/                    Call graph + control flow (CallGraph,
                                CallGraphBuilder, ControlFlowIndex,
                                ControlFlowOracle, ChannelIndex, Snapshot,
                                CollapseFilter, WorkerPool)
  mcp/                          MCP server (McpServer, McpTools, McpProtocol)
  ext/                          Organization extension API (Extensions.h,
                                OrgConfig.h)
  compat/                       LLVM-version compat, PCH cache, tool adjusters

src/
  main.cpp                      CLI entry point (anneal/morph/prism/megascope)
  anneal/ morph/ callgraph/ mcp/ ext/ compat/   Implementations

ext/                            Organization slot-in (fork-owned; globbed
                                into the build — see ext/README.md)
  examples/                     Reference extension + org config (not built)

tests/                          Catch2 test suite
docs/                           Design notes, MCP usage, EXTENDING.md
examples/                       ADL/CTAD fragility and transform examples
scripts/                        Benchmarking and smoke-test scripts
```

---

## Architecture

### Two-Phase Analysis (anneal)

```
Phase 1 — Index:
  ClangTool(all files) → IndexerActionFactory
    → for each TU: IndexerVisitor
      → VisitFunctionDecl → GlobalIndex::addFunctionOverload
      → VisitCXXDeductionGuideDecl → GlobalIndex::addDeductionGuide

Phase 2 — Analyze:
  ClangTool(all files) → AnalyzerActionFactory
    → for each TU: AnalyzerVisitor(GlobalIndex)
      → VisitCallExpr → check ADL candidates vs global index
      → VisitVarDecl  → check CTAD usages vs global index
      → then: registered organization AnnealChecks (ext/)
      → emit Diagnostic entries
```

### Single-parse bake (prism / megascope)

`bakeIndexes()` runs the declaration/hierarchy index, edge building, and
control-flow context extraction over **one** frontend parse per TU.
Cross-TU joins (virtual-dispatch fan-out, function-pointer-through-return)
are resolved at query time, which also keeps incremental reindexing
(`reindex_tu`, snapshot warm starts) consistent with full rebuilds.

### Key Design Patterns

| Pattern | Where used |
|---|---|
| RecursiveASTVisitor | IndexerVisitor, AnalyzerVisitor, CF context visitors |
| FrontendActionFactory | IndexerActionFactory, AnalyzerActionFactory |
| MatchFinder + MatchCallback | MatcherEngine (via CallbackAdapter) |
| Two-phase index/analyze | runAnalysis() in Analyzer.cpp |
| Multi-pass pipeline | TransformPipeline::execute() |
| Registry + static registrars | ExtensionRegistry (ext/ slot-in) |

---

## Examples

### ADL Fragility (`examples/adl_fallback/`)

`order_a.cpp` and `order_b.cpp` contain identical code but include headers in
different orders. Due to ADL, the same unqualified call resolves to different
overloads depending on which `scale()` overload is visible at the call site.
`anneal` detects this without requiring compilation of both orderings.

### CTAD Fragility (`examples/ctad_fallback/`)

`Container c("hello")` deduces differently depending on whether `Guide.hpp`
(which contains an explicit `Container(const char*) -> Container<std::string>`
deduction guide) is included. `anneal` flags the case where the explicit guide
is absent.

### Boolean Macro Split (`examples/macro_split/`)

`input.cpp` uses a boolean macro for a compound flag. `expected.cpp` shows the
target form after splitting into separate named parameters. The `morph`
transform for this pattern is a planned TDD target.

### Builder to Struct (`examples/builder_to_struct/`)

`input.cpp` uses a builder pattern with chained setters. `expected.cpp` shows
the equivalent aggregate initialization. Demonstrates a significant reduction
in boilerplate (1130 → 482 bytes) achievable via AST rewriting.

---

## Naming

Glass and optics. **vycor** is the Corning glass family the C++ tool draws
its name from (sibling to Pyrex, more demanding spec); the `-cpp` suffix
just hints at what it's about. **anneal** surfaces latent stress — ADL/CTAD
fragility — in order to relieve it. **morph** reshapes amorphous structure —
AST transforms. **prism** decomposes one point into its components — call-site
control flow. **megascope** is the persistent far-seeing instrument — the
MCP server.
