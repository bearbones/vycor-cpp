# vycor-cpp

vycor-cpp uses Clang to analyze and rewrite C++ with your project's compiler
flags. It can compare definitions across translation units, apply a rewrite
you describe in JSON, or trace a call across library boundaries.

## Commands

| Command | What can I do with it? | Guide |
|---|---|---|
| `check` | Find defects that span files, such as conflicting definitions, inconsistent declarations, and lock-order inversions. Use findings to gate a pull request. | [Run checks and use them in CI](docs/check-usage.md) |
| `morph` | Describe a migration with AST matchers and replacement templates. Preview the edits, then apply a sequence of rules to source files or macro invocations. | [Write and run a rewrite rule](docs/morph.md) |
| `megascope` | Index a project's call graph and control flow. Find callers, trace exception paths, or query the same index repeatedly from a script. | [Build and query an index](docs/megascope-usage.md) |
| `metrics` | Measure work reachable before `main` and group the results by translation unit, directory, or link target. | [Run and interpret metrics](docs/metrics/README.md) |

## Installing

On macOS, install the CLI and its LLVM runtime with Homebrew.

```sh
brew tap bearbones/vycor-cpp
brew install vycor-cpp
```

Prebuilt archives are available for Linux x86-64 and macOS arm64.
[Release downloads](https://github.com/bearbones/vycor-cpp/releases) · [Installation instructions](docs/building.md#install-a-binary)

## Building

With supported LLVM and Clang development packages installed, build the CLI
from the repository root.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DVYCOR_BUILD_TESTS=OFF
cmake --build build --target vycor-cpp
build/src/vycor-cpp --version
```

For tests, LLVM version selection, and hermetic builds, see the
[build guide](docs/building.md#build-from-source) and [compatibility matrix](COMPATIBILITY.md#support-matrix).

## For contributors

- Explore the [example projects](examples/) and [validation corpus](docs/validation.md).
- Find the source layout and extension points in the [developer guide](AGENTS.md#feature-layout).
- Add organization-specific checks, metrics, lock types, or feature flags with the [extension guide](docs/EXTENDING.md).
