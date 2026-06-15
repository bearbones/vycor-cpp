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
  (versioned releases) is not yet provided.
- Organizations using internal mirrors should mirror `release/llvm-NN`
  alongside `main`.

---

## Reference

- `include/vycor/compat/ClangVersion.h` — version-check macros.
- `CMakeLists.txt` — `VYCOR_SUPPORTED_LLVM_VERSIONS`, LLVM detection.
- `.github/workflows/ci.yml` — matrix CI per supported major.
- `AGENTS.md` — top-level developer guide; cross-references this document.
