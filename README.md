# vycor-cpp

A Clang LibTooling backend for safe, AST-aware C++ refactoring and static
analysis. It exposes four features as subcommands:

- **anneal** — detect problems such as fragile ADL/CTAD resolutions across translation units. `clang-tidy` wishes.
- **morph** — apply rule-driven, multi-pass AST matcher transformations
- **prism** — query control flow, exception handling, and call site guard context from the command line
- **megascope** — start an MCP (Model Context Protocol) server for interactive call graph queries, designed for LLM-assisted code analysis

Designed as a backend for external systems (e.g. a Python script translating
a custom DSL, or an LLM agent performing security audits via the MCP server).

---

## Features

### anneal — ADL/CTAD Fragility Analysis

ADL (Argument-Dependent Lookup) and CTAD (Class Template Argument Deduction)
can silently resolve to different declarations depending on which headers are
included in a given translation unit. This is a portability and correctness
hazard that standard compilers do not diagnose.

`anneal` runs a two-phase analysis:

1. **Index phase** — walks every translation unit and records all function
   overloads and deduction guides found in any header, building a
   project-wide `GlobalIndex`.
2. **Analysis phase** — re-walks each translation unit, comparing resolved
   call sites and CTAD usages against the global index. Any overload or
   deduction guide that exists globally but is invisible in the current TU
   (because its header is not included) is flagged as a fragile resolution.

Output is a list of diagnostics with source locations and human-readable
messages indicating which header to include or how to qualify the call.

### morph — AST-Based Source Transformations

`morph` parses matcher expressions from a JSON rules file and runs them
against source files using Clang's dynamic AST matcher API. It supports
multi-pass pipelines where each pass can build on the results of the
previous one.

Rules are specified as JSON objects containing:
- A matcher expression string (Clang's dynamic AST matcher DSL)
- A bind ID for the root matched node
- An action name (resolved to a built-in callback)

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

### anneal — Analyze for Fragile ADL/CTAD

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

### morph — Apply Transformations

```bash
./build/vycor-cpp morph \
  --rules-json rules.json \
  --build-path /path/to/compile_commands_dir \
  --source file1.cpp file2.cpp \
  --dry-run
```

The `--dry-run` flag collects replacements without writing them to disk,
which is useful for previewing changes.

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
the collapsed region).

### megascope — Interactive Call Graph MCP Server

Starts a persistent MCP server that pre-bakes a call graph and control flow
index from multiple source files, then serves interactive queries via
JSON-RPC over stdio.

```bash
./build/vycor-cpp megascope \
  --build-path /path/to/compile_commands_dir \
  --source file1.cpp --source file2.cpp --source file3.cpp \
  --entry-point "main" \
  --collapse-paths Client/Math \
  --collapse-paths Client/Core
```

**MCP tools exposed**: `lookup_function`, `get_callees`, `get_callers`,
`find_call_chain`, `query_exception_safety`, `query_call_site_context`,
`analyze_dead_code`, `get_class_hierarchy`

**Key difference from prism**: `megascope` indexes all specified sources
into a unified cross-TU call graph held in memory. `prism` parses per
invocation and is limited to single-TU context. For security audits or
multi-file analysis, always use `megascope`.

---

## Project Structure

```
CMakeLists.txt                  Top-level build configuration
extern/llvm-project/            LLVM/Clang submodule (sparse checkout)

include/vycor/
  anneal/                       Public headers — ADL/CTAD analysis feature
    GlobalIndex.h               Project-wide declaration database
    Indexer.h                   Phase-1 AST visitor (index all declarations)
    Analyzer.h                  Phase-2 AST visitor (detect fragile resolutions)
  morph/                       Public headers — transform pipeline feature
    MatcherEngine.h             Dynamic matcher parsing and execution
    TransformPipeline.h         Multi-pass transform orchestration
  callgraph/                    Public headers — call graph and control flow
    CallGraph.h                 Call graph data structure (nodes, edges, hierarchy)
    CallGraphBuilder.h          Two-phase AST visitors for graph construction
    CollapseFilter.h            Path-based edge collapse filtering
    ControlFlowIndex.h          Per-call-site try/catch and guard context
    ControlFlowOracle.h         Exception safety and path queries
  mcp/                          Public headers — MCP server
    McpServer.h                 JSON-RPC MCP server (holds graph in memory)
    McpTools.h                  MCP tool implementations
    McpProtocol.h               JSON-RPC protocol framing

src/
  main.cpp                      CLI entry point (anneal / morph / prism / megascope)
  anneal/                       anneal implementation
    GlobalIndex.cpp
    Indexer.cpp
    Analyzer.cpp
  morph/                       morph implementation
    MatcherEngine.cpp
    TransformPipeline.cpp
  callgraph/                    callgraph implementation
    CallGraph.cpp
    CallGraphBuilder.cpp        Two-phase: index nodes, then build edges
    CollapseFilter.cpp          Path-component matching for edge collapse
    ControlFlowIndex.cpp
    ControlFlowContextVisitor.cpp  Phase 3: try/catch and guard context
    ControlFlowOracle.cpp
  mcp/                          megascope implementation
    McpServer.cpp               JSON-RPC dispatch loop
    McpProtocol.cpp             Content-Length framing
    McpTools.cpp                Tool handlers (lookup, callers, callees, etc.)
  CMakeLists.txt

tests/                          Catch2 test suite
  test_matcher_engine.cpp       Unit tests for MatcherEngine::parse and addRule
  test_transforms.cpp           Integration test stubs for morph transforms
  test_adl_ctad.cpp             Unit and integration tests for anneal analysis

examples/
  adl_fallback/                 ADL fragility example (include-order sensitivity)
    Core.hpp, Extension.hpp, Logic.hpp
    order_a.cpp, order_b.cpp
  ctad_fallback/                CTAD fragility example (deduction guide visibility)
    Container.hpp, Factory.hpp, Guide.hpp
    order_a.cpp, order_b.cpp
  macro_split/                  Boolean macro splitting transform example
    input.cpp, expected.cpp
  builder_to_struct/            Builder pattern to struct conversion example
    input.cpp, expected.cpp
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
      → emit Diagnostic entries
```

### Transform Pipeline (morph)

```
For each pass:
  MatcherEngine(pass rules)
    → parse matcher expressions (Clang dynamic parser)
    → register callbacks with MatchFinder
    → ClangTool::run → collect Replacements
  merge into allReplacements_
Apply replacements to disk (when not dry-run)
```

### Key Design Patterns

| Pattern | Where used |
|---|---|
| RecursiveASTVisitor | IndexerVisitor, AnalyzerVisitor |
| FrontendActionFactory | IndexerActionFactory, AnalyzerActionFactory |
| MatchFinder + MatchCallback | MatcherEngine (via CallbackAdapter) |
| Two-phase index/analyze | runAnalysis() in Analyzer.cpp |
| Multi-pass pipeline | TransformPipeline::execute() |

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

## Current Status

| Component | Status |
|---|---|
| MatcherEngine (parse + run) | Complete |
| TransformPipeline (multi-pass) | Complete (apply-to-disk TODO) |
| GlobalIndex | Complete |
| Indexer (two-phase phase 1) | Complete |
| Analyzer (two-phase phase 2) | Complete |
| anneal CLI subcommand | Complete |
| morph CLI subcommand | Complete (JSON parsing TODO) |
| Call graph builder (two-phase) | Complete |
| Control flow index (phase 3) | Complete |
| Edge collapse filtering | Complete |
| prism CLI subcommand | Complete |
| MCP server (megascope) | Complete |
| MCP tools (8 tools) | Complete |
| Test suite (unit) | Complete |
| Test suite (integration stubs) | Stubs present, impl pending |

---

## Naming

Glass and optics. **vycor** is the Corning glass family the C++ tool draws
its name from (sibling to Pyrex, more demanding spec); the `-cpp` suffix
just hints at what it's about. **anneal** surfaceslatent stress — ADL/CTAD
fragility — in order to relieve it. **morph** reshapes amorphous structure —
AST transforms. **prism** decomposes one point into its components — call-site
control flow. **megascope** is the persistent far-seeing instrument — the
MCP server.
