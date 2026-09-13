# Validation corpus

Status: implemented on `main`. Owner: package D of
`docs/plans/2026-09-next/`. This page is the contract for
`corpus/cases/`, `scripts/corpus-run.py`, and the report it writes. The
order contract the expectations rely on is `docs/deterministic-output.md`;
the envelope is `docs/result-contract.md`; the path facts are
`docs/path-analysis.md`.

## What it measures

Each case is a small source tree whose answers are known by
construction. Its `case.json` says, query by query, which records must
be present (**witnesses**), which must be absent (**negatives**), which
scalar fields must hold, and in what relative order records must come.
The runner bakes the case with the built binary, runs the queries, and
reports:

- **quality**: every check, expected against observed, with failures
  sorted into *missing findings* (a witness, count, or required line
  not there), *false findings* (a negative present), *field mismatches*
  (exit, status, scalar, regex), *order mismatches*, and *unparseable
  output*; separately, what the tool itself **declared** — a search
  that said `complete: false`, an index that said
  `indexScope.complete: false`, a verdict downgraded to `observed_*`,
  a `not_found` or `unavailable` status — so a reader can tell a wrong
  answer from an honestly scoped one;
- **cost**: per query, repeated wall-clock latency (median, min, max
  over `--reps`) and the child's own peak RSS from `wait4`; per bake,
  wall and RSS.

The two never combine into one score. Rates are over the corpus's own
queries only; the report says so in `summary.quality.scope`.

## Layout

```
corpus/
  VERSION                  # bump when a case's expectations change
  cases/<name>/
    case.json              # sources, flags, phases, queries, expectations
    src/                   # the sources (compile_commands.json is generated)
    <overlay>/             # files a later phase copies over src/ (patch pair)
```

The runner copies `src/` into a scratch directory, backdates every file
(the bake re-checks files stamped in its own second), writes a
`compile_commands.json` of `clang++ -std=c++17 -I<dir> <flags> -c
<file>` entries, bakes `index.vycs` with `megascope index --threads 2`
and the case's `entry_points` (default `main`), and runs each query
with `--index` appended. A query whose first word is `anneal` runs the
`anneal` subcommand instead, with `--build-path` and every source
appended, and is checked as text.

### case.json

```json
{
  "schema": 1,
  "title": "one sentence: what the sources set up and what must hold",
  "sources": ["a.cpp", "b.cpp"],
  "flags": {"*": ["-std=c++17"], "b.cpp": ["-DMODE"]},
  "entry_points": ["main"],
  "queries": [ ... ]                  // single phase, or:
  "phases": [
    {"name": "before", "index": {"mode": "cold"},
     "save_index_as": "before.vycs", "queries": [ ... ]},
    {"name": "after", "overlay": "after", "flags": {"a.cpp": ["-DX"]},
     "sources": ["a.cpp", "b.cpp", "c.cpp"],
     "index": {"mode": "warm", "refreshed": 1, "refreshed_for_headers": 1},
     "queries": [ ... ]}
  ]
}
```

A phase may copy an **overlay** directory over the sources (only the
copied files are re-stamped, so only the TUs they reach look edited),
change per-file **flags** (the compile command changes, nothing else),
or change the **sources** list (a TU joins or leaves the selection).
Every phase runs `megascope index` again on the same index file — a
warm refresh — and `index` names fields of the one-line summary that
must hold (`mode`, `refreshed`, `refreshed_for_headers`,
`refreshed_for_inputs`, `indexed`, `partial`, `failed`, ...).
`save_index_as` copies the index, after the phase's refresh, to a file
in the scratch directory, so a later phase can compare against it.

An `argv` may use `{index}` (the index file), `{dir}` (the scratch
directory), `{case}` (the case directory, for a patch file kept beside
`case.json`), and `{saved:NAME}` (a file a `save_index_as` wrote). The
runner appends `--index` unless the query is a `diff` or names
`--index` itself.

A two-phase case is a **patch pair**: `src/` is *before*, the overlay
(or flag change) is the patch, and the expectations of both phases say
what the patch must add and remove. Package E's before/after fixtures
use this layout (`header_change`, `compile_flag_invalidation`,
`mixed_exception_protection`).

### A query

```json
{"id": "callers-of-f-int",
 "argv": ["get-callers", "--usr", "c:@F@f#I#"],
 "expect": {
   "exit": 0,
   "status": "ok",
   "fields": {"complete": true, "indexScope": {"complete": true}},
   "regex": {"summary": "caught on 1 of 2 path"},
   "count": {"callers": 2},
   "witnesses": [{"in": "callers", "match": {"callerName": "g"}}],
   "negatives": [{"in": "callers", "match": {"callerName": "h"}}],
   "sequence": [{"in": "callers",
                 "match": [{"callerName": "g"}, {"callerName": "main"}]}]
 }}
```

`match` is a **subset**: every key it names must be present in the
record with an equal value; nested objects are subsets too; a list
must have the same length and match element-wise (so a path is a list
of hop subsets, and `"stopReasons": []` demands an empty list). A field
that is absent never matches — missing output is a failure, not a
pass. `sequence` demands the matches appear in that relative order,
not necessarily adjacent. Text queries take `"lines": {"present":
[regex...], "absent": [regex...]}`; give every text query at least one
`present` line, or empty output passes it.

## Cases

| Case | Sets up | Shows |
|---|---|---|
| `overload_identity` | `f(int)` and `f(double)`, called from `g`, `h`, and `main` | a name is `ambiguous` (exit 4) for callers and chains; a usr is exact; callers, callees, search cuts, and chains order the overloads by usr |
| `mixed_exception_protection` | `risky()` called under a try in one caller and bare in another; `guarded()` only under a try; then a TU that fails to parse joins the selection | `sometimes_caught` with 1 of 2 paths, `always_caught` with complete coverage; after the partial TU, `observed_caught` with `indexScope.complete: false` and the reason in `summary` |
| `bounded_path_search` | four two-hop routes from `main` to `target` | unbounded: 4 paths, `complete`, `exhaustive`; `max_paths 2`: the canonical prefix (`a1`, `a2`), `complete: false`, `path_limit`; `max_depth 1`: `complete` but not `exhaustive`; unknown target: neither |
| `header_change` | an inline function in a header calls `gamma`; the patch makes it call `delta` | warm refresh re-parses the one including TU (`refreshed_for_headers: 1`); afterwards `gamma`, which only the old header ever named, is **unknown** (`complete: false`), not "proven unreachable" |
| `compile_flag_invalidation` | `entry()` calls `alpha` or `beta` under `#ifdef`; the patch adds `-DNEW_TARGET` to one compile command | `refreshed_for_inputs: 1`; the callee follows the flag; `alpha` stays a known node with no callers (the TU declares it), so its chain is `complete` with 0 paths |
| `change_impact` | `process` calls `helper` bare, `main` calls `process`; the patch wraps the call in a try/catch, adds `fresh` (calls `helper`, called by `main`), and drops `caller_of_retired`'s call; a whitespace-only overlay in between | `diff` of the whitespace pair is empty (exit 1, `changeCount` 0); the patch pair yields exactly `function_added`, `call_removed`, two `call_added`, `context_changed` in that order; `--to helper` adds one route and removes none; `impact-of-change --changed helper` lists `fresh`, `process`, `main` by depth; `--patch-file` maps three functions by call site and reports `ops.cpp`, whose functions are declared in `ops.h`, as unmapped |
| `odr_divergence` | `anneal --odr-diag` over the fixture from `tests/test_anneal_odr.cpp`: a `-D`-dependent inline body, one function defined by two headers, a class defined twice, an identical copy | the three violations are reported; the identical copy and the flagged class's own method are not; without the check nothing is |

Every case carries expected negatives; `header_change` is the
regression for the stale-name bug the raw warm-refresh comparison found
(`docs/path-analysis.md`, "Identity").

## Running

```bash
scripts/corpus-run.py --binary build/src/vycor-cpp                 # check
scripts/corpus-run.py --binary build/src/vycor-cpp --out report.json
scripts/corpus-run.py --binary build/src/vycor-cpp --self-check    # gate
scripts/corpus-run.py --binary build/src/vycor-cpp --case header_change -v --keep
```

`--reps N` (default 3) repeats each query for the latency figures; the
first run's output is the one checked, and `stable_across_reps` records
whether every repetition printed the same bytes. `--keep` leaves the
scratch trees behind.

ctest registers two gates (`tests/CMakeLists.txt`, label `corpus`):

- `corpus`: the cases pass (`--reps 1`);
- `corpus_selfcheck`: `--self-check` — the clean run passes, and under
  each `--inject-fault` (`drop-first-record`, `inject-record`,
  `drop-fields`, `empty-stdout`, applied to every observed result before
  checking) the run fails **in the right category** (missing findings,
  false findings, field mismatches, unparseable outputs). A corpus
  whose checks cannot fail is not a gate.

Gates are small and deterministic and need no network: every fixture
lives in the repository. Benchmarks over external trees
(`scripts/bench.py`, the llvm-project testbed) are explicitly invoked,
never part of ctest.

## Report

One JSON document (`report_version: 1`):

| Field | Holds |
|---|---|
| `revision` | `git rev-parse HEAD`, branch, whether the tree was dirty |
| `toolchain` | `vycor-cpp --version` (vycor-cpp and the embedded LLVM) |
| `binary`, `host`, `started`, `finished`, `reps`, `fault_injected` | provenance of the run |
| `corpus.version`, `corpus.cases` | `corpus/VERSION` and the cases run |
| `cases[].commands` | every command run, with wall and RSS for the bakes |
| `cases[].phases[].index_summary`, `index_checks` | the `megascope index` summary and the checks on it |
| `cases[].phases[].queries[]` | `argv`, `expected`, `observed` (payload, or raw stdout when it is not JSON), `declared`, `checks[]` (kind, ok, detail), `ok`, `stable_across_reps`, `latency_ms {reps, median, min, max}`, `peak_rss_kb` |
| `summary.quality` | counts: queries, passed, failed, checks, missing_findings, false_findings, field_mismatches, order_mismatches, unparseable_outputs, index_check_failures, declared_incomplete, declared_unknown, unstable_across_reps, case_errors, and `scope` |
| `summary.cost` | per-bake and per-query latency and RSS, plus the median of query medians |
| `ok` | every case passed |

A baseline for this revision is `corpus/reports/baseline.json`,
produced by the commands above on the host it names; regenerate it
when the corpus or a contract changes, and compare `summary.quality`
first, `summary.cost` second. Keep reports over large external trees
out of git.

## Adding a case

1. Write the smallest sources that make the answer obvious by reading
   them; declare-only callees and `#ifdef`s are fine.
2. Run the queries by hand over a scratch index and read the payloads;
   copy the *witnesses* you can justify from the sources, never from
   the output alone, and add at least one negative per list.
3. Pin orders only where `docs/deterministic-output.md` promises them.
4. Run `--self-check`; a case whose expectations survive
   `drop-first-record` has no witness worth the name.
5. Bump `corpus/VERSION`.

## Limitations

- Expectations are hand-written; the corpus finds regressions in what
  it names and nothing else.
- Latency figures come from a few repetitions of tiny fixtures and say
  nothing about scale; use `scripts/bench.py` for that.
- Peak RSS is `ru_maxrss` of the child process (kilobytes on Linux,
  converted from bytes on macOS).
- The runner drives the CLI only; the MCP transport is covered by
  `scripts/mcp-smoke.py` and the result-contract tests.
