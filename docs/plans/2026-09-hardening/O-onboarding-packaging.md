# O — Onboarding, packaging, and docs drift

## Outcome

A new user gets from install to a first correct answer without reading
design notes, sees progress during long bakes, can diagnose a broken setup
with one command, and the docs match the code.

## Evidence and starting points

- Progress: a bake prints one line at the start and one at the end
  (`src/main.cpp:1355`, `:1366`). The coverage warning (`:1415`) gives a count
  of failed TUs but not their names, and does not point to
  `megascope info --files`. Open since `docs/megascope-cli-review.md` §3.2.
- Onboarding: the README does not explain how to produce
  `compile_commands.json` (only `docs/mcp-usage.md` step 2 does) and has no
  troubleshooting section. `docs/pch-sdk-mismatch.md` and
  `docs/toolchain-portability.md` are design notes, not guides. A `--source`
  that is not in the compilation database is not diagnosed up front
  (`src/cli/SourceSelection.cpp`).
- Release binaries: the clang resource directory is baked as an absolute
  build-host path (`src/CMakeLists.txt:68-85`). The README asks Linux users to
  install `libllvm21` (`README.md:43`); clang's builtin headers ship in a
  separate package (`libclang-common-NN-dev` on Debian/Ubuntu), so a clean
  machine likely fails parses with "stddef.h not found". Not tested; the
  release workflow (`release.yml:130-150`) has no clean-machine smoke test.
- Flag consistency: anneal lacks `--source-list`/`--source-re`/default-all
  (`main.cpp:600`); morph's `--build-path` repeats (`:277-283`) while the
  others take one; morph lacks `--threads` and `--org-config`.
- CLI polish: `megascope --help` shows LLVM's option list rather than the
  verb help: `isMegascopeQueryVerb` (`MegascopeCli.cpp:74-77`) rejects
  anything starting with `-`, so `main.cpp`'s verb peel hands `--help` to
  `llvm::cl` (`megascope help` works); an unknown tool name
  gets no suggestion (`MegascopeCli.cpp:1147`); morph `--dry-run` prints
  "replace with '…'" rather than a diff (`TransformPipeline.cpp:119-141`).
- No MCP client configuration snippet (`claude mcp add …`, `.mcp.json`).

## Work

1. Bake progress on stderr: `[n/N] ETA mm:ss, k failed` at most once a
   second when stderr is a terminal, every N TUs otherwise, silenced by
   `-q`. At the end, list the first ten failed TUs with their outcome detail
   and point to `megascope info --files`.
2. `vycor-cpp doctor --build-path <dir> [--source <file>]`: check that the
   compilation database loads and its entries exist; that the resource dir
   and sysroot exist and contain `stddef.h`; that the chosen or first TU is
   in the database; then parse it and map the first error to a remedy
   (missing builtin headers, missing SDK, PCH mismatch, GCC install
   mismatch). Exit 0 only when the sample parses.
3. Resource directory at run time: try, in order, `--resource-dir`, the
   directory relative to the executable, the baked path, then
   `clang -print-resource-dir` on PATH; report which one was used in
   `--version -v` and `doctor`. Document the Linux runtime package
   requirement.
4. Release smoke test: a job in `release.yml` that installs the tarball on a
   clean container with only the documented runtime packages, runs
   `doctor` and a one-file ephemeral query on `examples/`, and fails the
   release otherwise. Consider publishing a container image.
5. Unify source selection: anneal and morph use `SourceSelection`
   (`--source`, `--source-list`, `--source-re`, `--skip-paths`, default-all);
   morph takes one `--build-path`, `--threads`, and `--org-config`. Keep old
   spellings working.
6. CLI polish: `megascope --help` prints the verb help; unknown tool names
   get a did-you-mean; morph `--dry-run` prints a unified diff.
7. README: a quick start that covers producing `compile_commands.json`
   (CMake, Bear, Bazel), a troubleshooting section that starts with
   `doctor`, and MCP client snippets for Claude Code and a generic
   `.mcp.json`.
8. Docs drift (verified at `8f7d68e`):
   - AGENTS.md says morph's JSON rules format "is not yet implemented" and
     lists "Parse `--rules-json`" and "Write final replacements to disk" as
     TODOs; both exist (`main.cpp:866-898`, `RulesParser.cpp`,
     `TransformPipeline.cpp:143-170`). Only applying replacements between
     passes remains (plan G).
   - AGENTS.md TODO rows for `query_raii_scopes_at_callsite`,
     `query_locks_held`, `query_same_lock`, and "catch handler type"
     (`Serialize.cpp:206-216` emits `caughtType`) are done.
   - The README's morph rules description ("matcher, bind ID, action") does
     not match the schema in `RulesParser.cpp:28-60`; add a schema doc and an
     example rules file.
   - AGENTS.md says the codebase is "split into four feature areas" and then
     lists eight; its `src/CMakeLists.txt` summary omits `impact/` and
     `compat/`.
   - `docs/toolchain-portability.md` still says Catch2 comes from
     FetchContent.
   - `docs/callgraph-mcp-review.md` item 9 says "in progress"; subprocess
     workers shipped.
   - `docs/mcp-usage.md` gotchas and `scripts/morph-batch.sh` (`RBX_CHECK`)
     contain organization-specific names and timings; generalize or move
     them under `ext/examples/`.

## Ownership and boundaries

Own README, AGENTS.md, the release workflow, `doctor`, progress output, and
the source-selection unification. Other packages update the docs for their
own changes; this package fixes the drift listed above.

## Acceptance

- The release smoke test passes on a clean container for each supported
  LLVM major, and fails when the builtin-header package is removed.
- `doctor` has tests for each diagnosed failure using fixture databases.
- Progress output does not appear on stdout (goldens unchanged) and is
  absent under `-q`.
- anneal and morph accept the unified selection flags; old spellings still
  work (tests).
- Every drift item above is resolved or explicitly deferred in the PR.
- Supported LLVM matrix passes.

## Deliverables

Progress reporting, `doctor`, run-time resource-dir resolution, the release
smoke test, unified selection flags, CLI polish, README quick start and
troubleshooting, and the drift fixes.
