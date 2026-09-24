# P — CI hardening: sanitizers and fuzzing

## Outcome

Memory errors, undefined behavior, and parser bugs reachable from files on
disk or bytes on stdin are found by CI instead of by users, and the Release
build that ships is the build that is tested.

## Evidence and starting points

- `.github/workflows/ci.yml` builds and tests Debug only (`:46`), across the
  LLVM 18/20/21 matrix. There is no ASan, UBSan, or TSan job and no Release
  test run, although releases ship Release builds, where asserts
  (including `CallGraph`'s read-only guards) compile out.
- No fuzz targets exist. Four parsers read untrusted or corruptible bytes:
  the snapshot loader (`src/callgraph/Snapshot.cpp`, including the mapped
  v12 control-flow section in `ControlFlowIndex.cpp:339-461`), the anneal
  checkpoint journal and worker shards (`src/anneal/Checkpoint.cpp`), and the
  MCP framing reader (`src/mcp/McpProtocol.cpp`).
- Corruption coverage is thin: `tests/test_snapshot.cpp:493` covers bad
  magic, half-truncation, and version mismatch only. There are no tests for
  torn concurrent writes, garbage or oversized MCP input, worker hangs, or a
  failed `reindex_tu` (H, I, and J each add their own reproductions).
- The thread pool bake and the concurrent `CallGraph`/`ControlFlowIndex`
  inserts have never run under TSan.

## Work

1. CI jobs (newest LLVM only, to bound cost):
   - `asan-ubsan`: Debug with `-fsanitize=address,undefined
     -fno-sanitize-recover=undefined`, full ctest.
   - `tsan`: the thread-pool tests and a multi-threaded bake of
     `examples/deep_chains` and one corpus case.
   - `release`: `CMAKE_BUILD_TYPE=Release` build plus the full ctest,
     `cli_golden`, and `corpus`.
   Add a `VYCOR_SANITIZE` CMake option so the same flags work locally, and
   document it in AGENTS.md.
2. Fuzz targets (libFuzzer, built only with `-DVYCOR_FUZZ=ON` under clang):
   snapshot load in both `Mutable` and `ReadOnly` modes followed by a few
   queries; checkpoint journal replay; worker shard read; MCP
   `readRequest` over an in-memory stream. Seed corpora from the test
   fixtures (a small saved index, a journal, a shard, recorded MCP
   sessions).
3. Run each fuzz target for a fixed short budget in CI (for example 60 s
   each, on pull requests that touch the corresponding file), and longer on
   a nightly schedule; upload crashing inputs as artifacts.
4. Triage: fix what the sanitizers and fuzzers find, or file each finding
   with a reproducer if it belongs to H, I, or J and they are in flight.
   Add every crashing input as a regression test.

## Ownership and boundaries

Own `ci.yml`, the sanitizer and fuzz CMake options, the fuzz targets, and
their seed corpora. H, I, and J own fixes in their areas; this package
reports to them rather than fixing in parallel while they are open.

## Acceptance

- All three new CI jobs are green on `main` (after the triage fixes), and
  their runtime is recorded in the PR.
- Each fuzz target runs at least 10 minutes locally without a finding after
  triage, with the command documented.
- Every sanitizer or fuzz finding has a regression test.
- A final pass after H, I, and J merge reruns the fuzzers against their
  changes.

## Deliverables

Sanitizer and Release CI jobs, four fuzz targets with seeds, the nightly
schedule, the triage fixes or filed reproducers, and AGENTS.md build notes.
