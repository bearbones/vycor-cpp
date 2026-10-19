# Changelog

All notable changes to `vycor-cpp` will be documented in this file.

## [0.3.0] - 2026-10-19

### Added
- **Shared `megascope` Core & `.mgsc` Binary Snapshot Integration (`MegascopeBridge`)**:
  - Added C FFI integration (`libmegascope.a` / `include/megascope.h`) and `ModuleShard` JSON lowering (`--emit-shard`, `.mgsc` v1 snapshots, and cross-language C++ + Python `megascope merge`).
  - Added `vycor_megascope_plugin` Clang frontend plugin (`-fplugin=libvycor_megascope_plugin.so`) and Buck2 map-reduce rules (`buck2/vycor.bzl`, `buck2/vycor.bxl`) for zero-reparse per-TU shard emission.
  - Added C++ RAII resource classification in `MegascopeBridge` so `query-context-managers` surfaces lock guards, mutexes, transactions, and scoped resources while excluding standard value containers and smart pointers (`std::string`, `std::vector`, `std::unique_ptr`, `std::shared_ptr`).
- **LLVM / Clang 22 Support**:
  - Added LLVM 22 to `VYCOR_SUPPORTED_LLVM_VERSIONS` (`22 21 20 18`), CI matrix, and `release/llvm-22` branch compatibility (`clang::NestedNameSpecifier` value semantics and removed `ElaboratedType` wrappers in LLVM 22).
- **`check` CLI & Diagnostic Improvements (formerly `anneal`)**:
  - Renamed the cross-TU static analysis subcommand and library namespace from `anneal` to `check` (`vycor-cpp check`, `.vycor-check.json`, `// vycor-disable-next-line`).
  - Defaulted diagnostic paths to project-root-relative paths (`inferProjectRoot` with out-of-tree build directory & symlink canonicalization); added `--absolute-paths` opt-in.
  - Added `--show-call-sites` to `exception-escape` diagnostics to include `file:line` call-site coordinates along the throw propagation chain.
  - Enhanced `odr-violation` diagnostics to list representative source files from `compile_commands.json` for each distinct definition.
  - Enhanced `specialization-visibility` diagnostics to show the resolved explicit specialization signature/kind and concise visibility guidance.
  - Refined `header-static-duplication` diagnostic wording and moved detailed remediation patterns to `docs/checks/header-static-duplication.md`.

### Fixed
- Fixed `adl-visibility`, `ctad-visibility`, and `specialization-visibility` false positives on TU-local (`static` or anonymous-namespace) helpers and template arguments.
- Fixed `odr-violation` false positives on function-local classes and lambdas across overloaded functions by keying `odrQualifiedName` on the enclosing function signature (`FunctionDecl::getType()`).
- Fixed $O(V + E)$ per-TU shard partitioning in `MegascopeBridge::toShardsArray` and filtered lambda `enclosingClass` synthesized class stubs.
