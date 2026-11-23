# vycor-cpp

A Clang LibTooling backend for safe, AST-aware C++ refactoring and static
analysis. It exposes three subcommands (plus a zero-reparse Clang frontend
plugin, `libvycor_megascope_plugin`):

- **check** — detect cross-TU structural and ODR defects such as fragile ADL/CTAD/template-specialization resolutions, divergent default arguments or exception specs, static initialization order hazards, and uncaught exception escapes. Like if `clang-tidy` went Super Saiyan.
- **morph** — apply rule-driven, multi-pass AST matcher transformations
- **megascope** — index a multi-TU call graph and control-flow context graph once (`.vycs`, `.mgsc`, or `.shard.json`), then query it from the shell (`megascope get-callers --name f`) or in batches; backed by both the native C++ engine and the language-agnostic **[`megascope`](https://github.com/bearbones/megascope)** Rust core (shared with **[`pyRx`](https://github.com/bearbones/pyRx)** for Python)

Designed as a backend for external systems (e.g. a Python script translating
a custom DSL, or an LLM agent performing security audits via the megascope
verbs),
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
sudo apt-get install libllvm22

tar xzf vycor-cpp-v0.3.0-linux-x86_64-llvm22.tar.gz
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
| LLVM/Clang | 18, 20, 21, or 22 | system package preferred; submodule fallback |
| [`libmegascope`](https://github.com/bearbones/megascope) | optional (`VYCOR_USE_LIBMEGASCOPE=ON`) | probed at `MEGASCOPE_ROOT` (`../megascope` or release prefix); enables direct `.mgsc` binary snapshot baking/merging/querying via C FFI (`megascope.h`), while `.shard.json` export works even without it |
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

The build tries each supported LLVM major version in turn (22, then 21,
then 20, then 18) by probing well-known install paths, then falls back to the
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

All subcommands accept compile commands from:
1. A directory containing `compile_commands.json` (`--build-path <dir>`),
2. A direct path to a compilation database JSON file (`--build-path <path/to/compdb.json>`), or
3. Compiler flags passed directly after `--` (`vycor-cpp <subcommand> ... -- -std=c++17 -Iinclude`), enabling hermetic per-TU and per-target actions in build systems such as **Buck2** and **Bazel** without generating `compile_commands.json`.

### check — Analyze for Fragile ADL/CTAD

ADL (Argument-Dependent Lookup) and CTAD (Class Template Argument
Deduction) can silently resolve to different declarations depending on
which headers a translation unit includes — a correctness hazard standard
compilers do not diagnose. `check` indexes all overloads and deduction
guides across the project (phase 1), then re-walks each TU comparing
resolved call sites against the global index (phase 2).

```bash
./build/vycor-cpp check \
  --build-path /path/to/compile_commands_dir \
  --source file1.cpp --source file2.cpp
```

Output example (paths relative to `--project-root`, default the working
directory; one finding per line, the check name in brackets):

```
src/logic.cpp:42:5: [adl-visibility] Fragile ADL resolution: MathLib::scale(Vector, double)
    exists in Extension.hpp but is not visible here. The current call resolves to
    MathLib::scale(Vector, int). Include Extension.hpp or explicitly qualify the call.
```

A summary goes to stderr: `N TU(s): A analyzed, F failed`, naming every
TU that did not parse cleanly. A run with a failed TU never prints
`check: no issues found.`.

**ODR violation detection** (`--odr-diag`): linkers reject duplicate
*strong* symbols, but mismatched *vague-linkage* definitions — inline
functions, in-class method bodies, class definitions — are silently
merged, with the linker keeping one arbitrary copy. `check` hashes every
such definition during indexing (`clang::ODRHash`) and compares across
the whole project, catching both the two-headers-define-the-same-thing
case and the subtler one-header-whose-body-depends-on-`-D`-flags case:

```
limits.hpp:2: [odr-violations] ODR violation: 'limits' at ./limits.hpp:2 compiles to 2 different
    definitions across TUs — its body depends on preprocessor state that differs
    between compile commands. Every TU must see an identical definition.
```

Token-identical copies at different paths are not flagged (vendored
duplicates are benign), so the signal stays clean.

Every check analysis is a **named check** with its own documentation
page — see [docs/checks/README.md](docs/checks/README.md) for the full
list, defaults, and groups. Select checks clang-tidy-style with
`--checks=all,-noisy` or a `.vycor-check.json` discovered from the
working directory upward:

```json
{ "checks": ["all", "-coverage-properties", "-compute-heavy"] }
```

**ABI layout divergence** (`--checks=abi-layout-divergence`) compares computed
record sizes, alignments, field and base offsets across TUs with the same target
ABI. It can find packing-flag mismatches even when declaration text is identical.
The check is opt-in, produces one witness per record/target, and uses fresh parses;
see [the check reference](docs/checks/abi-layout-divergence.md).

**Could be internal** (`--checks=could-be-internal`) lists functions and
variables with external linkage that no other TU references, from the
megascope reference facts: no header declares them (a warning), a header
does (a note), or only tests use them from elsewhere (a note). The
verdict needs every TU of the compilation database; with fewer it is a
note that says why. See [the check reference](docs/checks/could-be-internal.md).

**noexcept candidates** (`--checks=noexcept-candidate`, with
`noexcept-candidate-move` and `noexcept-candidate-virtual` selected
separately) reports functions that provably cannot throw and are not declared
`noexcept`, proven across TUs from the leaves up. A callee with no indexed
definition is unknown, never "cannot throw", and proofs that cross TUs are
withheld when any TU failed to index. Opt-in, severity note, most useful
first; see [the check reference](docs/checks/noexcept-candidate.md).

**Unsynchronized shared globals** (`--checks=unsynchronized-shared-global`)
reports globals, static members and function-local statics that one
thread writes and another accesses with no lock in common, from the
megascope call graph and the lock sets the bake records at each
reference. Each finding names both accesses, the lock set and thread
entry path of each; it is a warning only when both thread searches were
exhaustive and the facts cover the whole program, else a note that says
why. See [the check reference](docs/checks/unsynchronized-shared-global.md).

**Lock order inversions** (`--checks=lock-order-inversion`) reports locks
acquired in opposite orders on different paths across TUs (and a
non-recursive mutex acquired again while held): an edge `A -> B`
whenever `B` is acquired, directly or through a call chain, while `A`
is held, and every cycle of those edges with one witness per edge.
`std::scoped_lock` / `std::lock`, try-locks and recursive mutexes form
no edge or self-cycle; it is a warning only when every witness is
proven and exclusive and the facts cover the whole program. See
[the check reference](docs/checks/lock-order-inversion.md).

Other optional flags: `--warn-same-score`, `--model-convertibility`, the
legacy check toggles (`--coverage-diag`, `--odr-diag`, `--dead-code` with
`--entry-point`), and `--org-config` (organization checks/config — see
below).

Both analysis phases run per-TU on a worker pool (`--threads`, same
semantics as megascope). For long runs, `--checkpoint <file>`
journals per-TU progress: a run killed partway (OOM, Ctrl-C, CI timeout)
resumes where it left off instead of restarting, source edits invalidate
exactly the affected entries, and a TU whose parse fatally died twice is
skipped with a warning instead of re-killing every resume. One run per
journal: a second run given the same `--checkpoint` while the first is
running continues without it (the journal is locked through
`<file>.lock`).
`--isolate-workers` (with `--workers N`) additionally runs the parses in
subprocess workers, megascope-style: a TU that crashes its worker costs
only that TU, and the parent survives. `--worker-timeout <s>` (default
600, 0 = off) kills a worker that starts no new TU for that long and
drops the TU it was parsing; `--worker-memory-limit <MiB>` caps each
worker's heap. Ctrl-C or SIGTERM kills the workers and removes their
scratch directory. All of these compose.

```bash
./build/vycor-cpp check \
  --build-path /path/to/compile_commands_dir \
  --source file1.cpp --source file2.cpp \
  --threads 0 --checkpoint /tmp/check.vycj
```

#### check as a CI gate

| Flag | Effect |
|---|---|
| `--source-list <file\|->` | TUs one per line (`#` comments), unioned with `--source` |
| `--format text\|json\|sarif` | `file:line:col: [check] message` lines, a JSON report, or SARIF 2.1.0 |
| `--output <file>` | write the report there instead of stdout |
| `--fail-on note\|warning\|error\|none` | the severity that fails the run (default `note`: any finding) |
| `--allow-parse-failures` | a TU that fails to parse is a warning instead of exit 3 |
| `--write-baseline <file>` | record the current findings' fingerprints (the run exits 0) |
| `--baseline <file>` | report only findings not in the baseline, and count the stale entries |
| `--patch-file <file\|->`, `--git-base <rev> [--git-head <rev>]` | report only findings on changed lines |
| `--project-root <dir>` | what paths and fingerprints are relative to (default: the working directory) |
| `-v` | also list inline suppressions that suppressed nothing |

Exit codes: **0** clean, **1** findings at or above `--fail-on`, **2**
usage or configuration error, **3** a requested TU did not parse cleanly
(it takes precedence over 1). Details next to megascope's in
[docs/result-contract.md](docs/result-contract.md#check-exit-codes).

Each finding has a **fingerprint** (check name + the entities involved +
the file relative to the project root, never a line number), so it
survives unrelated edits — see
[docs/checks/README.md](docs/checks/README.md#finding-identity). Adopt
check on an existing codebase by recording today's findings once:

```bash
vycor-cpp check --build-path build --source-list tus.txt \
  --write-baseline .vycor-check-baseline.json
# later runs fail only on new findings, and say which baseline entries
# are fixed so the file can shrink:
vycor-cpp check --build-path build --source-list tus.txt \
  --baseline .vycor-check-baseline.json
```

Silence one finding in the source with a comment on its line, or alone
on the line above: `// vycor: ignore[adl-visibility]` (several:
`ignore[adl-visibility, odr-violations]`; every check: `ignore[*]`).

A GitHub Actions job that gates pull requests and uploads the findings
to code scanning (the SARIF has one rule per check, with a link to its
page, and a `partialFingerprints` entry per result so alerts follow the
code across edits):

```yaml
name: check
on:
  pull_request:
  push:
    branches: [main]

permissions:
  contents: read
  security-events: write # upload-sarif

jobs:
  check:
    runs-on: ubuntu-24.04
    steps:
      - uses: actions/checkout@v4

      - name: Install vycor-cpp
        env:
          VYCOR: vycor-cpp-v0.2.0-linux-x86_64-llvm18
        run: |
          sudo apt-get install -y libclang-cpp18 libllvm18 clang-18
          curl -sSfL "https://github.com/bearbones/vycor-cpp/releases/download/v0.2.0/$VYCOR.tar.gz" | tar xz
          echo "$PWD/$VYCOR/bin" >> "$GITHUB_PATH"

      - name: Configure (writes compile_commands.json)
        run: cmake -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

      - name: check
        run: |
          jq -r '.[].file' build/compile_commands.json | sort -u > tus.txt
          vycor-cpp check --build-path build --source-list tus.txt \
            --baseline .vycor-check-baseline.json \
            --format sarif --output check.sarif

      - name: Upload SARIF
        if: always() && hashFiles('check.sarif') != ''
        uses: github/codeql-action/upload-sarif@v3
        with:
          sarif_file: check.sarif
          category: vycor-check
```

The `check` step fails the job on a new finding (exit 1) or a TU that
did not parse (exit 3); the upload runs either way. To gate only on the
lines a pull request touches instead of a baseline, check out with
`fetch-depth: 0` and pass `--git-base origin/${{ github.base_ref }}`.
check's checks are cross-TU, so analyze the whole compilation database
(as above) rather than only the changed files: `--source-list` with a
changed-file list misses findings whose evidence is in unchanged TUs.

### morph — Apply Transformations

Write a C++ rewrite as JSON and run it with the existing binary. For example,
the bundled rule splits compound `ASSERT` calls while leaving an `&&` nested
inside an `||` alone. Preview it from the repository root.

```sh
build/src/vycor-cpp morph --rules-json examples/macro_split/rules.json \
  --source examples/macro_split/input.cpp --dry-run -- -std=c++17
```

One reported edit changes `ASSERT(a > 0 && b > 0 && c > 0)` to
`ASSERT_ALL(a > 0, b > 0, c > 0)`. The [morph guide](docs/morph.md)
walks through applying and checking the example, writing a first rule,
and extending the implementation.

### megascope — Cross-TU Call Graph Index and Query Tools

Bakes a unified cross-TU call graph and control-flow index into an index
file, then answers queries from the shell (one process per query, or a
batch on one loaded index).

```bash
# Build the index once (default location: <build-path>/.vycor/megascope.vycs).
# No selection flag means every C/C++ TU in compile_commands.json (or,
# once an index exists, the TUs it recorded); narrow with --source-re /
# --skip-paths, name TUs with --source, or pipe a list into --source-list -.
./build/vycor-cpp megascope index \
  --build-path /path/to/compile_commands_dir \
  --source-re '/Client/(Network|Core)/' --skip-paths ThirdParty \
  --collapse-paths Client/Math

# Query it. Flags come from each tool's schema; `megascope <tool> --help`.
./build/vycor-cpp megascope get-callers --name Foo::bar --build-path /path/to/compile_commands_dir
./build/vycor-cpp megascope find-call-chain --to Foo::bar --format ndjson
./build/vycor-cpp megascope search-functions --query Foo --format tsv | cut -f4
./build/vycor-cpp megascope info            # what the index holds
./build/vycor-cpp megascope tools           # the 30 tools

# Many queries, one load: NDJSON requests in, one response per line out.
printf '{"id":1,"tool":"get_callers","args":{"name":"Foo::bar"}}\n' | \
  ./build/vycor-cpp megascope batch

# No index: --source/--source-list/--source-re bake the selected TUs in
# memory and answer from that (quick single-file investigations).
./build/vycor-cpp megascope query-nearest-catches --build-path /path/to/compile_commands_dir \
  --source SecurityModule.cpp --function onClientChallengeResponse
./build/vycor-cpp megascope dump --build-path /path/to/compile_commands_dir \
  --source SecurityModule.cpp | jq -c 'select(.kind=="call_site" and .enclosingTryCatches==[])'
```

**Recipes** (`--format ndjson` puts one record per line after a
`{"_summary":...}` line; `--format tsv` is `cut`/`awk` fodder):

```bash
# Unique callers of a function, walking one level up
./build/vycor-cpp megascope get-callers --name Foo::bar --format ndjson | \
  jq -r 'select(.callerName) | .callerName' | sort -u

# Every path from main to a function, one line each
./build/vycor-cpp megascope find-call-chain --from main --to Foo::bar --format ndjson | \
  jq -r 'select(type=="array") | [.[0].from] + map(.to) | join(" -> ")'

# Unprotected call sites of the whole index (no enclosing try/catch)
./build/vycor-cpp megascope dump | \
  jq -c 'select(.kind=="call_site" and .enclosingTryCatches==[]) | {callerName, calleeName, callSite}'

# Dead functions under one directory, as a file:line list
./build/vycor-cpp megascope analyze-dead-code --format ndjson | \
  jq -r 'select(.file? and (.file|test("/src/net/"))) | "\(.file):\(.line) \(.name)"'

# Exit codes drive scripts: 1 is "answered, empty", 4 is "ambiguous name"
./build/vycor-cpp megascope get-callers --name bar --format ndjson; case $? in
  4) echo "pick a --usr from the candidates above";; esac
```

Query verbs find the index via `--index`, `$VYCOR_INDEX`,
`<build-path>/.vycor/megascope.vycs`, or `./.vycor/megascope.vycs`, so a
query run from the build directory needs no path at all. Output is compact
JSON (`--pretty`, `--format ndjson|tsv`); exit codes are the contract:
0 results, 1 empty, 2 usage, 3 index missing/unreadable, 4 ambiguous name
(candidates on stdout — re-run with `--usr`). `megascope dump` streams
every call-site context (caller/callee with USRs, try/catch scopes,
guards, live RAII locals) and channel site as NDJSON — one record per
line after a `{"_summary":...}` line — or as one JSON document with
`--format json`; the stream never holds the materialized index, so it
scales to the largest indexes.

**30 tools**: `search_functions`, `lookup_function`, `get_callees`,
`get_callers`, `find_call_chain`, `query_exception_safety`,
`query_call_site_context`, `query_raii_scopes_at_callsite`,
`query_throw_propagation`, `query_all_path_contexts`,
`query_nearest_catches`,
`query_locks_held`, `query_same_lock`, `analyze_dead_code`,
`get_class_hierarchy`, `list_entry_points`, `graph_summary`,
`list_callback_sites`, `list_concurrency_entry_points`, `list_channels`,
`query_channel`, `query_channels_for_function`, `explain_ordering`,
`impact_of_change`, `audit_dependencies`, `explain_dependency`, `metrics`,
`lookup_symbol`, `get_references`,
`query_path_effects`.
See `docs/megascope-usage.md`. `megascope diff --before A --after B` compares
two saved indexes (`docs/change-impact.md`). `audit_dependencies` and
`explain_dependency` compare the build's declared link dependencies with
the symbol references the index holds, given a target map written by a build adapter
(`scripts/cmake-target-map.py`): which symbols each dependency is needed
for, what they drag along, and which dependencies are thin, undeclared, or
not seen in use (`docs/dependency-audit.md`). `lookup_symbol` and
`get_references` answer from the symbol reference facts — who reads,
writes, calls, or takes the address of a function or global, its
linkage, and its defining TU (`docs/symbol-references.md`).

Bake flags (`index`): `--source`/`--source-list`/`--source-re`/
`--skip-paths` (TU selection; with none, the TUs recorded in the existing
index, else the whole compilation database),
`--index` (warm starts — only TUs whose source, any opened header, or
compile command changed are re-indexed, in parallel; `--snapshot` is
the old spelling), `--force` (rebuild regardless), `--retry-failed`
(re-parse the TUs whose last parse failed even when nothing changed;
see `docs/index-provenance.md`), `--no-wait` (fail instead of waiting
when another `index` holds the index's write lock,
`<index>.lock`), `--threads`,
`--pch-dir`, `--isolate-workers`/`--workers` (subprocess baking: a
crashing or hanging TU costs only that TU; the default whenever
`--threads` is not 1 and `--pch-dir` is unset, `--isolate-workers=false`
bakes in-process), `--worker-timeout <s>` (default 600: a worker that
starts no new TU for that long is killed and the TU recorded `timeout`;
0 = off), `--worker-memory-limit <MiB>`, `--stats-json` (bake timings plus the
per-section index load split), `-v`. Query verbs also take `-v`, which
reports the load time section by section.

**Ephemeral mode**: a query verb given `--source`/`--source-list`/
`--source-re` (with `--build-path` or `-- <compile-flags...>`) bakes those
TUs in memory and answers, reading and writing no index — the former
`prism` subcommand. `--skip-paths`, `--collapse-paths`, `--threads`, and
`--org-config` qualify the bake as they do for `index`. The bake sees only
the selected TUs, so callers in other files are invisible; for security
audits or multi-file analysis, index the project once and query that.

#### Hierarchical Map-Reduce Indexing — Buck2 & Bazel Integration (`megascope merge`)

For hermetic build systems like **Buck2** and **Bazel**, `megascope` supports a map-reduce
workflow that produces `.vycs` (or `.mgsc` / `.shard.json`) call-graph artifacts at three granularities
without needing a monolithic `compile_commands.json`:

1. **Per-TU (`.vycs` per translation unit)**:
   ```bash
   vycor-cpp megascope index --threads 1 --no-wait \
     --index out/tu/a.cpp.vycs --source src/a.cpp \
     -- -std=c++17 -Iinclude
   ```
2. **Per-library / per-binary target (`.vycs` per target)**:
   ```bash
   vycor-cpp megascope merge --no-wait \
     --output out/libcore.vycs \
     out/tu/a.cpp.vycs out/tu/b.cpp.vycs
   ```
3. **Global index (`.vycs` across targets)**:
   ```bash
   vycor-cpp megascope merge --no-wait \
     --output out/megascope.global.vycs \
     --input-list out/target_shards.list \
     --entry-point main
   ```

`megascope merge` validates build configuration and environment compatibility
across shards (overridable with `--allow-mismatch`) and deduplicates
overlapping TUs idempotently so diamond dependencies in the target graph never
double-count edges or call-site contexts.

Out-of-the-box integrations and complete multi-target examples are shipped in
this repository for both build systems:
- **Buck2**: Starlark rules ([`buck2/vycor.bzl`](buck2/vycor.bzl)), zero-config
  **BXL** scripts ([`buck2/vycor.bxl`](buck2/vycor.bxl)), and example project
  ([`examples/buck2_megascope/`](examples/buck2_megascope/)) — see
  **[buck2/README.md](buck2/README.md)**.
- **Bazel** (Bzlmod & `WORKSPACE`): Starlark rules ([`bazel/vycor.bzl`](bazel/vycor.bzl)),
  zero-config **Bazel Aspects** ([`bazel/aspects.bzl`](bazel/aspects.bzl)), build
  flags ([`bazel/BUILD.bazel`](bazel/BUILD.bazel)), and example project
  ([`examples/bazel_megascope/`](examples/bazel_megascope/)) — see
  **[bazel/README.md](bazel/README.md)**.

#### Zero-Reparse Clang Frontend Plugin (`libvycor_megascope_plugin.so`)

Instead of invoking `vycor-cpp megascope index` as a separate compilation pass,
you can load `libvycor_megascope_plugin.so` directly into your normal
`clang++ -c` compile commands. The plugin runs via
`PluginASTAction::AddAfterMainAction`, walking the already-parsed `ASTContext`
once to emit a per-TU shard alongside the `.o` object file with zero extra
header or template parsing:

```bash
# Emits foo.vycs next to foo.o by default (or pass --output=foo.vycs / foo.mgsc / foo.shard.json)
clang++ -c foo.cpp -o foo.o \
  -fplugin=/path/to/lib/libvycor_megascope_plugin.so \
  -fplugin-arg-vycor-megascope---output=foo.vycs \
  -fplugin-arg-vycor-megascope---entry-point=main \
  -fplugin-arg-vycor-megascope---no-wait
```

Plugin flags (`--output=<path>`, `--entry-point=<fn>`, `--collapse-paths=<p>`,
`--org-config=<file>`, `--lock-types=<t>`, `--channel-types-json=<file>`,
`--no-wait`) can be passed via `-fplugin-arg-vycor-megascope-<flag>` or
`-Xclang -plugin-arg-vycor-megascope -Xclang <flag>`.

#### Standalone `megascope` Core & Polyglot (`C++` + `Python`) Indexing

In addition to `vycor-cpp`'s native `.vycs` snapshot format, [`MegascopeBridge`](include/vycor/callgraph/MegascopeBridge.h)
connects `vycor-cpp` to the language-agnostic **[`megascope`](https://github.com/bearbones/megascope)**
Rust engine (`brew tap bearbones/megascope && brew install megascope`):

- **`.shard.json` / `.shards.json` export**: Passing an `--index` (or plugin
  `--output`) path ending in `.shard.json` or `.shards.json` lowers C++
  functions, call edges, `try`/`catch` scopes, RAII/lock scopes, conditional
  guards, class hierarchies, and channels into `megascope::ModuleShard` JSON
  for ingestion by `megascope ingest`.
- **`.mgsc` binary snapshots (via `libmegascope` C FFI)**: When built with
  `libmegascope` (`VYCOR_HAS_LIBMEGASCOPE`), passing an `--index` / `--output`
  path ending in `.mgsc` bakes, merges, diffs, and queries native `.mgsc`
  binary snapshots directly through `libmegascope`.
- **Polyglot C++ + Python merging**: `vycor-cpp megascope merge` (or the
  standalone `megascope merge` CLI) can merge C++ shards (`.mgsc`, `.vycs`,
  `.shard.json`) with Python `.pyrx` snapshots produced by
  **[`pyRx`](https://github.com/bearbones/pyRx)** (`brew tap bearbones/pyrx && brew install pyrx`)
  into a single unified `.mgsc` index.

---

### metrics — Measurements

Some findings are measurements, not warnings. `vycor-cpp metrics`
reports named metrics, selected the way checks are, as records with one
shape and no pass/fail:

```bash
./build/vycor-cpp metrics --list-metrics

# Work done before main, largest reach first
./build/vycor-cpp metrics --metrics=static-init-cost \
  --build-path /path/to/compile_commands_dir --source-re 'src/.*\.cpp$'

# The same, per TU / directory / link target
./build/vycor-cpp metrics --rollup tu --format ndjson \
  --build-path /path/to/compile_commands_dir --source-re 'src/.*\.cpp$'
```

| Metric | Measures |
|---|---|
| [static-init-cost](docs/metrics/static-init-cost.md) | Per dynamic initializer and constructor function: the functions it can reach before `main`, and whether that reach allocates, locks or does I/O |

It is the megascope tool `metrics` under a top-level name, so the output
formats, paging, exit codes and `batch` are megascope's. Record shape,
roll-ups and organization metrics: **[docs/metrics/README.md](docs/metrics/README.md)**.

---

## Customizing for your organization

Organizations forking/mirroring this repo can add their own semantics
without editing upstream files (so upstream merges stay conflict-free):

- **Org config JSON** (`--org-config vycor.org.json`) — declarative:
  in-house lock types for the RAII/lock queries, channel/queue types,
  **feature-flag patterns** (regexes over guard conditions; matching
  guards are annotated with the flag name in query output, so "this
  path only runs with `FFlag::NewNav` on" becomes queryable), collapse
  paths, and per-check kill switches.
- **`ext/` slot-in directory** — compiled hooks: custom **check checks**
  (full AST access, registered with `VYCOR_REGISTER_CHECK`),
  custom **metrics** (`VYCOR_REGISTER_METRIC`),
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
  check/                       ADL/CTAD analysis (GlobalIndex, Indexer,
                                Analyzer, DeadCodeAnalyzer)
  morph/                        Transform pipeline (MatcherEngine,
                                RulesParser, TemplateEngine, TransformPipeline)
  callgraph/                    Call graph + control flow (CallGraph,
                                CallGraphBuilder, ControlFlowIndex,
                                ControlFlowOracle, ChannelIndex, Snapshot,
                                MegascopeBridge, CollapseFilter, WorkerPool)
  query/                        Transport-neutral megascope tools (Tools.h:
                                ToolEntry/ToolContext/result contract,
                                Identity, Serialize)
  cli/                          megascope verbs: schema-derived flags,
                                output contract, exit codes (MegascopeCli)
  metrics/                      Named measurements (Metric, MetricSet,
                                TargetOwners)
  ext/                          Organization extension API (Extensions.h,
                                OrgConfig.h)
  compat/                       LLVM-version compat, PCH cache, tool adjusters

src/
  main.cpp                      CLI entry point (check/morph/megascope/metrics)
  plugin/MegascopePlugin.cpp    Clang frontend plugin (libvycor_megascope_plugin)
  check/ morph/ callgraph/ query/ cli/ impact/ metrics/ ext/ compat/   Implementations

ext/                            Organization slot-in (fork-owned; globbed
                                into the build — see ext/README.md)
  examples/                     Reference extension + org config (not built)

tests/                          Catch2 test suite
docs/                           Design notes, megascope usage, EXTENDING.md
examples/                       ADL/CTAD fragility and transform examples
scripts/                        Benchmarking and smoke-test scripts
```

---

## Architecture

### Two-Phase Analysis (check)

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
      → then: registered organization CheckSpec (ext/)
      → emit Diagnostic entries
```

### Single-parse bake

`bakeIndexes()` runs the declaration/hierarchy index, edge building, and
control-flow context extraction over **one** frontend parse per TU.
Cross-TU joins (virtual-dispatch fan-out, function-pointer-through-return)
are resolved at query time, which also keeps warm refreshes of a saved
index consistent with full rebuilds.

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
`check` detects this without requiring compilation of both orderings.

### CTAD Fragility (`examples/ctad_fallback/`)

`Container c("hello")` deduces differently depending on whether `Guide.hpp`
(which contains an explicit `Container(const char*) -> Container<std::string>`
deduction guide) is included. `check` flags the case where the explicit guide
is absent.

### Boolean Macro Split (`examples/macro_split/`)

The directory contains a runnable JSON rules file and the exact expected
output. The transform integration test checks both preview and apply against it.
See the [morph walkthrough](docs/morph.md#try-the-macro-example).

### Builder to Struct (`examples/builder_to_struct/`)

This illustrative before/after pair turns setter calls into field assignments
and moves the lambda-based apply operation onto the struct. A contributor
implementing this migration would need to supply the transformation rules.

---

## Naming

Glass and optics. **vycor** is the Corning glass family the C++ tool draws
its name from (sibling to Pyrex, more demanding spec); the `-cpp` suffix
just hints at what it's about. **check** surfaces latent stress — ADL/CTAD
fragility — in order to relieve it. **morph** reshapes amorphous structure —
AST transforms. **megascope** is the persistent far-seeing instrument — the
cross-TU index and its query verbs (its ephemeral mode absorbed **prism**,
which decomposed one point into its components — call-site control flow).
