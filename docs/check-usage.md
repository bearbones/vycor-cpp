# Running check

`vycor-cpp check` compares C++ translation units to find defects that depend on
what other files declare, define, or call. Each diagnostic names the check that
produced it. The catalog lists every built-in check, its default severity, and
the evidence behind a finding. [Check catalog](checks/README.md)

## Try a cross-file check

From the repository root, run the one-definition rule (ODR) check on two files
from the validation corpus. Both compile separately. Their headers supply
different definitions for the same class and inline function.

```sh
vycor-cpp check --checks=odr-violations \
  --source corpus/cases/odr_divergence/src/tu1.cpp \
  --source corpus/cases/odr_divergence/src/tu2.cpp -- -std=c++17
```

Look for findings tagged `[odr-violations]` naming `Cfg` and `dup`. The summary
on stderr reports two translation units analyzed, and the command exits 1
because it found defects. For a local source build, use `build/src/vycor-cpp`.
[Example sources](../corpus/cases/odr_divergence/src) · [ODR check](checks/odr-violations.md)

## Run on your project

Give `--build-path` the directory containing `compile_commands.json` or the path
to the database file itself. Repeat `--source` for individual translation units.
To supply many, use `--source-list tus.txt` (one path per line), or
`--source-list -` for stdin. Explicit files and list entries are combined.
For standalone inputs, pass compiler flags after `--` as in the example above.

```sh
vycor-cpp check --build-path build --source src/reader.cpp --source src/writer.cpp
vycor-cpp check --list-checks
```

Text output gives the source location, check name in brackets, and diagnostic.
The summary names any translation units that failed to parse. Use
`--project-root` to choose the directory for relative paths and fingerprints,
or `--absolute-paths` for full paths.

### Select checks

Configure the checks with `--checks=all,-noisy` or a `.vycor-check.json`
discovered by walking up from the working directory. For example, this
configuration excludes coverage checks and the compute-heavy group.

```json
{ "checks": ["all", "-coverage-properties", "-compute-heavy"] }
```

| Investigation | Checks and options |
|---|---|
| Overload and deduction-guide visibility | The first pass indexes overloads and deduction guides across the selection. The second compares call sites and class template argument deductions with that index. `--warn-same-score` includes ties and `--model-convertibility` expands viability testing. [ADL](checks/adl-visibility.md) · [CTAD](checks/ctad-visibility.md) |
| Definitions that differ between files | `odr-violations` compares definition hashes, including inline bodies changed by preprocessor flags. `abi-layout-divergence` compares computed sizes and offsets under the same target ABI, including packing differences in identical declarations. [ODR](checks/odr-violations.md) · [ABI layout](checks/abi-layout-divergence.md) |
| Linkage and exception specifications | `could-be-internal` finds externally linked symbols with no references from other translation units. `noexcept-candidate` identifies functions proven unable to throw. Move operations and virtual functions can be selected separately. [Linkage](checks/could-be-internal.md) · [Exception specifications](checks/noexcept-candidate.md) |
| Concurrency across files | `unsynchronized-shared-global` reports accesses from different threads with no common lock. `lock-order-inversion` reports acquisition cycles with a witness for each edge. Both qualify their findings when program coverage is incomplete. [Shared state](checks/unsynchronized-shared-global.md) · [Lock ordering](checks/lock-order-inversion.md) |

Legacy flags `--odr-diag`, `--coverage-diag`, and `--dead-code` also enable
their named checks. Use `--entry-point` for dead-code reachability and
`--org-config` for organization-specific settings. For worked examples of
how header order changes overload selection or deduction, see the
[ADL fixture](../examples/adl_fallback) and [CTAD fixture](../examples/ctad_fallback).

## Parallel and resumable runs

Both analysis phases distribute translation units over the worker pool.
For a long run, supply a checkpoint to resume completed work after interruption.

```sh
vycor-cpp check --build-path build --source-list tus.txt \
  --threads 0 --checkpoint /tmp/check.vycj
```

Source edits invalidate affected entries. A TU that repeatedly crashes can
be marked poisoned and skipped on resume. The journal holds a lock at
`<file>.lock`. A second run that encounters the lock proceeds without the
journal. Individual checks document when they require fresh parses.

Use `--isolate-workers --workers N` to run parses in subprocesses.
`--worker-timeout <seconds>` kills a worker that starts no new TU for that
long (default 600, zero disables it). `--worker-memory-limit <MiB>` sets a
per-worker memory limit. Interrupting the parent terminates workers and removes
their scratch directory.

## Use check in CI

| Option | Effect |
|---|---|
| `--format text\|json\|sarif`, `--output <file>` | Emit text findings, a JSON report, or SARIF 2.1.0. Reports go to stdout unless an output file is specified. |
| `--fail-on note\|warning\|error\|none` | Choose the lowest severity that fails the job. The default is `note`. |
| `--allow-parse-failures` | Report failed TUs on stderr and use the findings to determine the exit code. |
| `--write-baseline <file>`, `--baseline <file>` | Record existing findings, then report new ones and count obsolete baseline entries. Fingerprints survive line-number changes. |
| `--patch-file <file\|->`, `--git-base <rev>` with optional `--git-head <rev>` | Report findings on changed lines. Analyze the full TU set so the checks retain cross-file evidence. |
| `-v` | Also list inline suppressions that matched no finding. |

Exit **0** means clean or below threshold, **1** means findings at or above
threshold, **2** means usage or configuration error, and **3** means a requested
TU failed to parse. An incomplete analysis takes precedence over findings.
[Exit-code details](result-contract.md#check-exit-codes)

Record a baseline once, then use it in subsequent runs. A successful
`--write-baseline` accepts the current findings and exits 0.

```sh
vycor-cpp check --build-path build --source-list tus.txt --write-baseline .vycor-check-baseline.json
vycor-cpp check --build-path build --source-list tus.txt --baseline .vycor-check-baseline.json
```

If you enable `odr-violations`, a parallel run can report the same conflicting
definitions at a different header, changing the finding's fingerprint. Use
`--threads 1` and the same source order for both baseline commands when you
need reproducible results for that check.

To suppress an individual finding, put `// vycor: ignore[adl-visibility]` on
its line or alone on the preceding line. A comma-separated list names several
checks and `*` covers all checks. [Fingerprints](checks/README.md#finding-identity) · [Suppression syntax](checks/README.md#suppressing-a-finding)

### GitHub Actions with SARIF

This example assumes a CMake project and a committed baseline. The check step
gates the job, and the upload step sends the SARIF report to code scanning
even when the check finds a defect. Each result has a fingerprint so alerts
can follow the code across edits.

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
      - uses: actions/checkout@v6

      - name: Install vycor-cpp
        env:
          VYCOR: vycor-cpp-v0.3.0-linux-x86_64-llvm18
        run: |
          sudo apt-get install -y libclang-cpp18 libllvm18 clang-18
          curl -sSfL "https://github.com/bearbones/vycor-cpp/releases/download/v0.3.0/$VYCOR.tar.gz" | tar xz
          echo "$PWD/$VYCOR/bin" >> "$GITHUB_PATH"

      - name: Configure (writes compile_commands.json)
        run: cmake -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

      - name: check
        run: |
          jq -r '.[] | if (.file | startswith("/")) then .file else .directory + "/" + .file end' \
            build/compile_commands.json | sort -u > tus.txt
          vycor-cpp check --build-path build --source-list tus.txt \
            --baseline .vycor-check-baseline.json \
            --format sarif --output check.sarif

      - name: Upload SARIF
        if: always() && hashFiles('check.sarif') != ''
        uses: github/codeql-action/upload-sarif@v4
        with:
          sarif_file: check.sarif
          category: vycor-check
```

To gate on changed lines, use `fetch-depth: 0` on checkout and pass
`--git-base origin/${{ github.base_ref }}` on pull requests in place of
`--baseline`. Keep the full source list in both runs.
