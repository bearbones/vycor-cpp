# Toolchain Portability Plan

How to make vycor-cpp easy to fork and integrate with an organization's
internal toolchains, package management, and CI — while keeping a "just works"
default for standalone use.

## Problem

The current build has two hardcoded dependency acquisition paths:

1. **LLVM/Clang**: `find_package()` for system installs → `extern/llvm-project` git submodule fallback
2. **Catch2**: `FetchContent` from GitHub (requires network at configure time)

Both break in typical enterprise environments:
- Network restrictions block `FetchContent` and submodule pulls
- LLVM is distributed via internal artifact repos (Artifactory, Conan, etc.),
  not system packages
- Organizations maintain their own LLVM forks with custom patches
- Hermetic toolchains ship LLVM under a project-relative path
  (e.g., `Client/dependencies/clang/`)

## Design Goals

1. **Zero-config default**: `cmake -B build` works on a dev laptop with homebrew
   LLVM or the submodule, just like today
2. **Fork-friendly**: An org forks the repo and adds their package resolution
   without touching the core `CMakeLists.txt`
3. **No network at configure time**: All dependencies resolvable from local disk
4. **Build artifact portability**: The built binary should work against compilation
   databases from the org's toolchain, not just the one it was built with

## Proposed Approach: CMake Presets + Override Include

### Layer 1: `CMakeUserPresets.json` (org-specific, gitignored upstream)

CMake presets let each organization define named configurations that set
`CMAKE_PREFIX_PATH`, `FETCHCONTENT_SOURCE_DIR_*`, and custom variables without
modifying `CMakeLists.txt`.

```jsonc
// CMakeUserPresets.json (checked into org fork, gitignored in upstream)
{
  "version": 6,
  "configurePresets": [
    {
      "name": "roblox",
      "inherits": "default",
      "cacheVariables": {
        // Point at our internal LLVM fork
        "CMAKE_PREFIX_PATH": "${sourceDir}/../game-engine/Client/dependencies/clang",
        // Use our vendored Catch2 instead of FetchContent
        "FETCHCONTENT_SOURCE_DIR_CATCH2": "${sourceDir}/../Catch2",
        // Disable submodule fallback since we have a package
        "VYCOR_USE_SUBMODULE": "OFF",
        // Set the clang binary for PCH compilation
        "VYCOR_DEFAULT_CLANG": "${sourceDir}/../game-engine/Client/dependencies/clang/bin/clang++"
      }
    }
  ]
}
```

Usage: `cmake --preset roblox -B build`

**Why presets**: They're the CMake-native mechanism for this. They don't require
any changes to `CMakeLists.txt`. They compose (inherit from a `default` preset).
And `CMakeUserPresets.json` is gitignored by convention, so org forks can commit
their own without conflicting with upstream.

### Layer 2: Optional override include in `CMakeLists.txt`

For organizations that need more control than cache variables (e.g., custom
`find_package` logic, target aliasing, or patching compiler flags), add a
single hook point:

```cmake
# In CMakeLists.txt, before the LLVM resolution block:
include(cmake/org-overrides.cmake OPTIONAL)
```

This file is:
- **Not present** in upstream → no-op, default behavior
- **Present in org fork** → can override any variable, add custom
  `find_package` calls, define wrapper targets, etc.
- **gitignored in upstream**, committed in org fork

Example `cmake/org-overrides.cmake` for an organization using Artifactory:

```cmake
# Pull LLVM from our internal Conan/Artifactory package
find_package(LLVM CONFIG REQUIRED
  PATHS "${CMAKE_SOURCE_DIR}/../game-engine/Client/dependencies/clang/lib/cmake/llvm"
  NO_DEFAULT_PATH)
find_package(Clang CONFIG REQUIRED
  HINTS "${LLVM_INSTALL_PREFIX}/lib/cmake/clang")

# Our Catch2 is pre-installed
set(FETCHCONTENT_SOURCE_DIR_CATCH2
    "${CMAKE_SOURCE_DIR}/../Catch2" CACHE PATH "" FORCE)

# Default --clang for PCH compilation
set(VYCOR_DEFAULT_CLANG
    "${CMAKE_SOURCE_DIR}/../game-engine/Client/dependencies/clang/bin/clang++"
    CACHE PATH "")
```

### Layer 3: Dependency resolution changes in `CMakeLists.txt`

Minimal changes to the existing CMakeLists.txt to support the above:

1. **Add the optional include hook** (one line):
   ```cmake
   include(cmake/org-overrides.cmake OPTIONAL)
   ```

2. **Guard the submodule fallback** behind `VYCOR_USE_SUBMODULE`:
   ```cmake
   option(VYCOR_USE_SUBMODULE "Fall back to extern/llvm-project submodule" ON)
   
   if(NOT LLVM_FOUND AND VYCOR_USE_SUBMODULE AND EXISTS "${LLVM_SUBMODULE_DIR}/CMakeLists.txt")
     # ... existing submodule logic
   endif()
   ```

3. **Support `FETCHCONTENT_SOURCE_DIR_CATCH2`** (already works — CMake's
   `FetchContent` automatically uses this variable when set, skipping the
   network fetch)

4. **Add `VYCOR_DEFAULT_CLANG` cache variable** that sets the default
   for `--clang` when not specified on the command line:
   ```cmake
   set(VYCOR_DEFAULT_CLANG "clang++" CACHE PATH
       "Default clang++ binary for PCH compilation")
   target_compile_definitions(vycor_lib PRIVATE
     "VYCOR_DEFAULT_CLANG=\"${VYCOR_DEFAULT_CLANG}\"")
   ```

5. **Add a `default` preset** to `CMakePresets.json` (committed upstream):
   ```jsonc
   {
     "version": 6,
     "configurePresets": [
       {
         "name": "default",
         "binaryDir": "${sourceDir}/build",
         "generator": "Ninja",
         "cacheVariables": {
           "CMAKE_BUILD_TYPE": "Release"
         }
       }
     ]
   }
   ```

### What a fork looks like

An organization forks vycor-cpp and:

1. Adds `CMakeUserPresets.json` (or `CMakePresets.json` with org presets)
2. Optionally adds `cmake/org-overrides.cmake` for advanced customization
3. Removes or ignores `extern/llvm-project` (not needed with internal LLVM)
4. Sets up their CI to configure with `cmake --preset <org-name>`
5. Publishes the built binary to their artifact repo

The upstream `CMakeLists.txt` changes are minimal (2-3 lines) and don't
affect the default behavior.

## Alternative Approaches Considered

### vcpkg / Conan manifest

A `vcpkg.json` or `conanfile.py` could declare LLVM and Catch2 as dependencies,
and each organization's package manager resolves them. This is more
"standard C++" but:
- LLVM is not well-supported in vcpkg (it's enormous and version-sensitive)
- Adds a mandatory package manager dependency for all users
- The game-engine already has its own package manager (gobot), not vcpkg/Conan

**Verdict**: Too much infrastructure for a tool that most users build once.

### Git submodule pinning with shallow clone

Keep the submodule but make it shallow and version-pinned. Forks override
`.gitmodules` to point at their internal mirror.
- Simple but couples the LLVM version to a git URL
- Doesn't help with the "no network at configure time" requirement for Catch2

**Verdict**: Only solves LLVM, not Catch2. Less flexible than presets.

### Nix / Bazel external dependencies

Hermetic dependency resolution via Nix flakes or Bazel's `http_archive`.
- Maximum reproducibility
- But requires Nix or Bazel — heavy dependency for a small tool

**Verdict**: Overkill. Reserve for if the tool grows to need it.

## Recommended Implementation Order

1. Add `CMakePresets.json` with a `default` preset (upstream)
2. Add `include(cmake/org-overrides.cmake OPTIONAL)` to `CMakeLists.txt`
3. Guard submodule fallback behind `VYCOR_USE_SUBMODULE`
4. Add `VYCOR_DEFAULT_CLANG` cache variable
5. Gitignore `CMakeUserPresets.json` and `cmake/org-overrides.cmake`
6. Document in CLAUDE.md: "Forking for your organization" section

Total CMakeLists.txt changes: ~10 lines. No behavioral change for existing users.
