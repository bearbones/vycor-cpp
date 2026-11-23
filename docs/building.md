# Installing and building vycor-cpp

## Install a binary

On macOS, the Homebrew tap installs the matching LLVM runtime. The versioned
formula lets you choose the LLVM major.

```sh
brew tap bearbones/vycor-cpp
brew install vycor-cpp
# To select LLVM 20, use: brew install vycor-cpp@20
vycor-cpp --version
```

Release archives are named `vycor-cpp-vX.Y.Z-<os>-llvmNN.tar.gz`, with builds
for Linux x86-64 and macOS arm64. Match the archive's LLVM major to your
installed runtime. For example, after downloading the Linux LLVM 22 archive
and installing `libllvm22` from apt.llvm.org, run the following.
[Release downloads](https://github.com/bearbones/vycor-cpp/releases)

```sh
tar xzf vycor-cpp-v0.3.0-linux-x86_64-llvm22.tar.gz
./vycor-cpp-v0.3.0-linux-x86_64-llvm22/bin/vycor-cpp --version
```

For a macOS archive, install the matching `llvm@NN` formula first. If Gatekeeper
quarantines the downloaded binary, use `xattr -d com.apple.quarantine` on the
extracted executable. The version output identifies the tool, LLVM, and the
host compiler. [Platform and runtime details](../COMPATIBILITY.md#macos-runner-and-apple-clang)

## Build from source

A source build lets you compile organization extensions, select your toolchain,
or build for another platform. Use CMake 3.20 or newer, a C++17 compiler, and
Ninja for the commands below. Start with a supported LLVM and Clang development
installation. [LLVM support matrix](../COMPATIBILITY.md#support-matrix)

```sh
git clone https://github.com/bearbones/vycor-cpp.git
cd vycor-cpp
git submodule update --init extern/Catch2
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target vycor-cpp
build/src/vycor-cpp --version
```

To run the tests, build the test target and run CTest.

```sh
cmake --build build --target vycor_tests
ctest --test-dir build --output-on-failure
```

For a CLI-only build, configure with `-DVYCOR_BUILD_TESTS=OFF` and skip the
Catch2 submodule step.

### Dependencies

| Dependency | Resolution |
|---|---|
| LLVM and Clang | CMake probes supported system packages, newest first (22, 21, 20, 18), then the `extern/llvm-project` submodule. Pin a system installation with `-DCMAKE_PREFIX_PATH=/usr/lib/llvm-20`. |
| Catch2 (tests) | CMake uses an existing `Catch2::Catch2WithMain` target, then tries `find_package(Catch2 3.10 CONFIG)`, then the pinned `extern/Catch2` submodule. Set `Catch2_DIR` or `CMAKE_PREFIX_PATH` for a packaged installation. |
| libmegascope (optional) | With `VYCOR_USE_LIBMEGASCOPE=ON`, the build looks in `MEGASCOPE_ROOT` (default `../megascope`) for the C FFI. Linking it enables direct binary `.mgsc` support. The native tool also exports JSON shards. [megascope core](https://github.com/bearbones/megascope) |

To build LLVM from source, initialize `extern/llvm-project` before configuring
vycor-cpp. For a sparse LLVM checkout, include the directories below. The
submodule build includes the X86 and AArch64 targets.

```sh
git submodule update --init extern/llvm-project
git -C extern/llvm-project sparse-checkout set llvm clang cmake third-party
```

### Hermetic and internal-package-manager builds

Dependency resolution uses local packages or initialized submodules during
configuration. For an internal build, choose the source that fits your setup.

- **Mirrors.** Point the URLs in `.gitmodules` at the internal mirrors and initialize the submodules at their existing paths.
- **Package managers.** Set `CMAKE_PREFIX_PATH` to make the LLVM, Clang, and Catch2 package configurations visible to CMake. This works with installations supplied by Spack, Conan, or vcpkg.
- **Parent projects.** Provide `Catch2::Catch2WithMain` before adding vycor-cpp as a subdirectory. Supply the LLVM and Clang package locations from the parent or toolchain file. Forks can also provide dependency overrides through the CMake hook. [Extension guide](EXTENDING.md)
