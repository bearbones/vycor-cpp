# LLVM/Clang Compatibility

`vycor-cpp` is built against the Clang LibTooling C++ API, which
changes shape between major LLVM releases. This document records which LLVM
major versions are supported, what API differences exist between them, and
how to add or remove a supported version.

It also documents the cherry-pick / backport policy for downstream release
branches, so contributors know what to expect when a fix lands on `main`.

---

## Support matrix

| LLVM major | Status | CI | Notes |
|---|---|---|---|
| 18 | supported | yes | Oldest supported. Ubuntu 24.04 archive default. |
| 19 | not supported | — | Skipped (no current demand). Intermediate between 18 and 20. |
| 20 | supported | yes | Mid-range stable target. |
| 21 | supported | yes | Current. |
| 22+ | not supported | — | Will be added on demand. |

`CMakeLists.txt` declares the canonical list as
`VYCOR_SUPPORTED_LLVM_VERSIONS` (currently `21 20 18`). The CI matrix in
`.github/workflows/ci.yml` mirrors this list. Both must stay in sync.

To pin a specific LLVM at configure time:

```bash
cmake -B build -G Ninja -DCMAKE_PREFIX_PATH=/usr/lib/llvm-NN
```

---

## Known API differences

### LLVM 19+: `llvm::DefaultThreadPool`

**Used in:**
- `src/callgraph/CallGraphBuilder.cpp` — parallel call graph construction.
- `src/callgraph/ControlFlowContextVisitor.cpp` — parallel control flow indexing.

**Background.** LLVM 19 split `llvm::ThreadPool` into a polymorphic interface.
The old concrete class was renamed to `llvm::StdThreadPool`, and a new
abstract base named `llvm::ThreadPool` was introduced. `llvm::DefaultThreadPool`
is the recommended user-facing typedef from 19 onward.

**Resolution.** Both call sites are guarded with `VYCOR_LLVM_AT_LEAST(19)`:

```cpp
#if VYCOR_LLVM_AT_LEAST(19)
    llvm::DefaultThreadPool pool(llvm::hardware_concurrency(threadCount));
#else
    llvm::ThreadPool pool(llvm::hardware_concurrency(threadCount));
#endif
```

The `.async()` and `.wait()` methods are identical on both types, so only the
type spelling needs guarding.

---

## Writing version-conditional code

### The macros

`include/vycor/compat/ClangVersion.h` exposes:

```cpp
#define VYCOR_LLVM_AT_LEAST(major)         (LLVM_VERSION_MAJOR >= (major))
#define VYCOR_LLVM_VERSION_IN_RANGE(lo,hi)  (LLVM_VERSION_MAJOR >= (lo) && LLVM_VERSION_MAJOR <= (hi))
```

### When to use them

Use these macros when:

- An LLVM API was renamed, removed, or added a required parameter between
  supported majors.
- A type's namespace or header location changed.
- A behavioral default changed in a way that requires explicit opt-in.

Do **not** use them to conditionally enable features that only some users
care about — that belongs in CMake options, not in `#if`s.

### Pattern

When a single API call differs by spelling but has identical surrounding
logic, prefer inline `#if` at the call site:

```cpp
#include "vycor/compat/ClangVersion.h"

void buildSomething() {
#if VYCOR_LLVM_AT_LEAST(N)
    NewSpelling x(args);
#else
    OldSpelling x(args);
#endif
    x.commonMethod();
}
```

If the same difference appears at three or more call sites, factor it into a
small helper in `compat/` (header-only, namespace `vycor::compat`).
Avoid premature extraction — two call sites with `#if` guards remain simpler
than a wrapper, in our experience.

### Always cover all supported majors

Every `#if VYCOR_LLVM_AT_LEAST(N)` must also have an `#else` arm that
works on every supported major below `N`. CI exercises every major in the
matrix on every commit; if your `#else` doesn't compile against LLVM 18, CI
will tell you immediately.

---

## Adding a new supported LLVM major

1. Append the version (newest first) to `VYCOR_SUPPORTED_LLVM_VERSIONS`
   in `CMakeLists.txt`.
2. Append it to the `llvm-version` matrix in `.github/workflows/ci.yml`.
3. Build locally against the new version. For each error:
   - If it's a renamed/added/removed API, add a `VYCOR_LLVM_AT_LEAST(N)`
     guard at the affected call site(s) and document the difference in the
     "Known API differences" section above.
   - If it's a behavioral change without an obvious workaround, decide
     whether to drop support or write an adapter; document the decision.
4. Update the support matrix table at the top of this document.
5. Update `AGENTS.md`'s "External Dependencies" line to include the new
   version if it changes the supported range.

## Removing a supported LLVM major

1. Remove from `VYCOR_SUPPORTED_LLVM_VERSIONS` and the CI matrix.
2. Search for `VYCOR_LLVM_AT_LEAST(N)` guards whose `#else` arm only
   existed to support the dropped version. Remove those `#if/#else/#endif`
   blocks; keep just the modern path.
3. Update the support matrix and "Known API differences" sections.
4. Open a deprecation issue at least one release before the drop, listing
   the affected release branches if any are active.

---

## Release branches and cherry-pick policy

The project plans to cut long-lived release branches per supported LLVM
major (`release/llvm-18`, `release/llvm-20`, `release/llvm-21`) so
organizations can pin to the LLVM their internal toolchain ships. These
branches do not yet exist; this section describes the policy that will
apply once they do.

### Branch model

- `main` — newest supported LLVM (21). New features land here first.
- `release/llvm-NN` — frozen against LLVM major NN. Receives bug fixes and
  security fixes. Refuses features unless the feature is genuinely
  cross-version-clean.

### Cherry-pick policy (Phase 1: manual)

When a PR merges to `main`:

- **Bug fixes** that apply cleanly should be cherry-picked to every active
  release branch where they're applicable. Convention: maintainer applies a
  `backport/llvm-NN` label to the original PR before merging.
- **Features** are not backported by default. Backport requires explicit
  request from a downstream consumer of the affected release branch.
- **API-skewed code** (anything wrapped in `VYCOR_LLVM_AT_LEAST(N)`)
  cherry-picks cleanly into branches at version N or above. For branches
  below N, the cherry-pick may need a hand-rewritten patch — this is
  expected, not a bug in the policy.

### Cherry-pick policy (Phase 2: automation, future)

Once manual backports become a recurring cost, the project will adopt a
label-driven backport bot (e.g. `korthout/backport-action` or similar):

- PR labeled `backport/llvm-NN` -> bot opens cherry-pick PR against
  `release/llvm-NN`.
- Cherry-pick PR auto-merges only if both (a) the cherry-pick applied
  cleanly and (b) CI is green on the release branch.
- Conflicts or test failures leave the PR open for manual resolution.

We deliberately do not auto-port every commit that *could* apply cleanly:
the social signal of "a human chose to send this back" is the cheapest
defense against semantic conflicts that text-merge can't catch.

### What downstream consumers should expect

- A release branch will not change CMake-visible interfaces or CLI flags
  except for security fixes.
- Pinning to the tip of a release branch is supported. Pinning to a tag
  (versioned releases) is also supported — see "Tagging and releases" below.
- Organizations using internal mirrors should mirror `release/llvm-NN`
  alongside `main`.

---

## Tagging and releases

`vycor-cpp` carries its own SemVer, independent of the LLVM major it's built
against, in the root `VERSION` file. `CMakeLists.txt` reads it into
`project(vycor-cpp VERSION ...)`, and `vycor-cpp --version` reports it
alongside the embedded LLVM version.

**Bump policy:** MAJOR = breaking CLI flag / output-format / MCP-protocol
change; MINOR = new subcommand or flag, backward compatible; PATCH = bug fix
only. `main` and each `release/llvm-NN` branch accumulate **independent**
version histories once they diverge — `v0.3.0` on `release/llvm-18` is not
guaranteed to contain the same changes as `v0.3.0` on `main`, only to follow
the same bump semantics from whatever `VERSION` value the branch was frozen
at.

### Tag grammar

- **`vX.Y.Z`** — cut on `main`. Triggers a full release: `.github/workflows/release.yml`
  reads `VYCOR_SUPPORTED_LLVM_VERSIONS` out of `CMakeLists.txt` **at the
  tagged commit** and builds every listed major, for every OS in the matrix.
- **`vX.Y.Z-llvmNN`** — cut on the corresponding `release/llvm-NN` branch,
  once that branch has diverged from `main` (i.e. a backport bump changed
  its `VERSION`). Triggers a single-major release: only LLVM major `NN`,
  every OS.

The tag string alone is enough for the release workflow to compute its build
matrix — it never needs to look up which branch a tag came from.

### What a release produces

Pushing a matching tag runs `.github/workflows/release.yml`, which for each
(OS × LLVM major) in the resolved matrix:

1. Builds `vycor-cpp` in Release mode and runs the full test suite as a
   release gate (a release is never cut from a build that fails `ctest`).
2. Stages it via `cmake --install` and packages it as
   `vycor-cpp-vX.Y.Z-<os-label>-llvmNN.tar.gz` (`os-label` is one of
   `linux-x86_64`, `macos-x86_64`, `macos-arm64`), attached to a GitHub
   Release on this repo.
3. On a non-test, non-dry-run tag, updates the Homebrew tap
   `bearbones/homebrew-vycor-cpp` (`.github/workflows/scripts/update-tap-formula.sh`)
   with a `Formula/vycor-cpp@NN.rb` per released major, plus a
   `Formula/vycor-cpp.rb` alias tracking whichever major is currently newest.
   This is a self-hosted tap, not a `homebrew/core` submission — anyone can
   consume it directly with `brew tap bearbones/vycor-cpp && brew install
   vycor-cpp`, with no popularity/notability bar to clear.

Linux runs on GitHub-hosted `ubuntu-24.04`; macOS runs on GitHub-hosted
`macos-13` (Intel) and `macos-14` (Apple Silicon) — both come with Xcode /
Apple Clang preinstalled, so no third-party cloud-Mac vendor is needed for
this pipeline. Note: as of this writing, Homebrew has no `llvm@{18,20,21}`
bottle for `macos-13`/Ventura (only `sonoma`+), so that leg compiles LLVM
from source and is slower — budgeted via a longer timeout in the workflow,
not a bug.

A macOS release tarball is **not** fully self-contained: Homebrew's
`llvm@NN` formulas link `libLLVM.dylib`/`libclang-cpp.dylib` dynamically, so
a `vycor-cpp` binary built against one needs that same Homebrew keg present
at runtime. This is why the tap (whose formulas `depends_on "llvm@NN"`) is
the primary supported macOS install path; a raw tarball works too but
requires the matching `brew install llvm@NN` first. Out of scope for now:
code signing/notarization (use `xattr -d com.apple.quarantine` on a
downloaded tarball if Gatekeeper objects) and re-linking tarballs into
fully self-contained binaries.

### Cutting a release

```bash
# On main, for a full release:
echo "0.2.0" > VERSION
git commit -am "Release 0.2.0"
git push origin main
git tag v0.2.0
git push origin v0.2.0

# On a release branch, for a single-major backport release:
git checkout release/llvm-18
echo "0.1.1" > VERSION
git commit -am "release/llvm-18: 0.1.1"
git push origin release/llvm-18
git tag v0.1.1-llvm18
git push origin v0.1.1-llvm18
```

Test the workflow itself via `workflow_dispatch` with `dry_run: true`
(uploads build artifacts, creates no Release, touches no tap) before relying
on a real tag push.

---

## Reference

- `include/vycor/compat/ClangVersion.h` — version-check macros.
- `CMakeLists.txt` — `VYCOR_SUPPORTED_LLVM_VERSIONS`, LLVM detection,
  `VERSION` wiring.
- `VERSION` — the project's own SemVer, independent of LLVM major.
- `.github/workflows/ci.yml` — matrix CI per supported major (Linux only).
- `.github/workflows/release.yml` — tag-triggered release builds
  (Linux + macOS), packaging, and the Homebrew tap update.
- `bearbones/homebrew-vycor-cpp` — the self-hosted Homebrew tap.
- `AGENTS.md` — top-level developer guide; cross-references this document.
