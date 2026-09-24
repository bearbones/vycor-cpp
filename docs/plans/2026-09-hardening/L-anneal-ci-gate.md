# L — anneal as a CI gate

## Outcome

`vycor-cpp anneal` can gate a pull request: it fails the build on new
findings, reports in a format code-scanning tools ingest, can be adopted on
an existing codebase without fixing every historical finding first, and
never reports "no issues found" over TUs it could not parse.

## Evidence and starting points

- Output and exit code: findings print as plain `loc: message` lines with no
  check name or severity, and the command returns 0 whether or not it found
  anything (`src/main.cpp:855-863`).
- Parse failures are invisible: `tool.run(&factory)` results are ignored in
  both phases (`src/anneal/Analyzer.cpp:1407`, `1415`), so a TU that failed
  to parse still ends in "anneal: no issues found."
- No SARIF, JSON, baseline, or suppression support (a search for `sarif`,
  `NOLINT`, and `baseline` finds nothing). `docs/change-impact.md:322` notes
  that anneal "has no stable finding identity".
- Source selection: anneal requires an explicit `--source` list
  (`main.cpp:600`); it lacks megascope's `--source-list`, `--source-re`, and
  default-all selection. (O owns unifying flags across subcommands; this
  package only needs `--source-list` so CI can pass a changed-file list.)

## Work

1. Per-TU outcomes: record each TU's parse result in both phases (and from
   isolated workers and checkpoint replay), and print a summary line
   (`N analyzed, M failed`), naming the failed TUs. Parse failures make the
   run exit 3, or with `--allow-parse-failures`, a warning.
2. Finding identity: a fingerprint of check name + the USR (or qualified name)
   of the entities involved + the file path relative to the project root, with
   line numbers excluded so it survives unrelated edits. Document it in
   `docs/checks/README.md`.
3. Output: `--format text|json|sarif` (`--output <file>`). Text gains the
   check name (`file:line:col: [check] message`). JSON is one object per
   finding with check, severity, location(s), message, fingerprint. SARIF
   2.1.0 with one `rule` per named check (`helpUri` pointing at
   `docs/checks/<name>.md`) and `partialFingerprints` from step 2. Validate
   the SARIF against the schema in a test.
4. Exit codes: 0 clean, 1 findings at or above `--fail-on` severity (default:
   any), 2 usage, 3 parse failures. Document them next to megascope's in
   `docs/result-contract.md`.
5. Baseline: `--write-baseline <file>` records current fingerprints;
   `--baseline <file>` suppresses them and reports only new findings, plus a
   count of baseline entries no longer present (so the baseline can shrink).
6. Inline suppression: `// vycor: ignore[check-name]` on the finding's line or
   the line above; `ignore[*]` for all checks. Report unused suppressions under
   `-v`.
7. Changed-lines mode: `--patch-file` / `--git-base` reusing
   `src/impact/PatchMapping.cpp` to report only findings whose location
   falls in changed hunks.
8. A worked GitHub Actions example in the README uploading SARIF with
   `github/codeql-action/upload-sarif`.

## Ownership and boundaries

Own anneal's output, exit code, finding identity, baseline, and
suppression. Do not change what the checks detect. Organization checks
(`ext/`) inherit fingerprints and SARIF rules through `name()`; cover one in
a test.

## Acceptance

- Tests for each exit code, including a fixture TU that fails to parse.
- Fingerprints are unchanged when unrelated lines are inserted above a
  finding, and change when the entities involved change.
- Baseline round trip: write, rerun (exit 0), add a new finding (exit 1, only
  the new one reported), fix a baselined one (reported as stale).
- SARIF validates against the 2.1.0 schema; a sample upload is shown in the
  PR (a dry run or a fork is enough).
- Checkpoint and isolated-worker runs produce byte-identical JSON to the
  in-process run.
- Supported LLVM matrix passes.

## Deliverables

Exit codes, per-TU outcome reporting, JSON and SARIF output, fingerprints,
baselines, suppressions, changed-lines mode, tests, and a CI example.
