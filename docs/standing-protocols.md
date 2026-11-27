---
repo: vycor-cpp
domain: static-analysis
languages: [cpp, python]
primary_oracle: "timeout 600s cmake --build build && timeout 600s ctest --test-dir build --output-on-failure"
---

# Standing Autonomous Protocols — `vycor-cpp`

Durable reference for unsupervised, monotonic-value workflows in `vycor-cpp`.
General loop mechanics, dead-end ledgers, and multi-agent procedures live in
the `standing-tasks` skill (`~/.claude/skills/standing-tasks/`).

## Protocol: libFuzzer & Parser Corpus Campaigns
- **id**: `vycor-cpp/fuzz-own`
- **category**: `fuzzing`
- **scope**: `own-code`
- **metric**: `crashes-fixed | corpus-edges`
- **default_stop**: `time=20m/target | crashes>=1 | indefinite`
- **tier**: `pr-ready`
- **oracle**: `timeout 600s cmake --build build-fuzz --target fuzz && timeout 600s ctest --test-dir build --output-on-failure`
- **summary**: Grow libFuzzer corpora across snapshot, checkpoint, shard, and batch parsers under ASan+UBSan and expand harnesses to uncovered parsers.

### Targets & Commands
Existing Clang libFuzzer targets (`fuzz/README.md`, `fuzz/CMakeLists.txt`):
- `fuzz_snapshot` (`SnapshotIO::load`, `ControlFlowIndex`, query/dump/save round-trips)
- `fuzz_checkpoint` (`CheckCheckpoint::open`, replay, append, reopen)
- `fuzz_shard` (`readCheckIndexShard`, `readCheckDiagShard`, `readGlobalIndexFile`)
- `fuzz_batch` (`runBatch` JSON request stream & response contract)

```bash
cmake -B build-fuzz -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DVYCOR_FUZZ=ON -DVYCOR_BUILD_TESTS=OFF
timeout 600s cmake --build build-fuzz --target fuzz
build-fuzz/fuzz/fuzz_seeds /tmp/vycor-fuzz-seeds
timeout 1260s build-fuzz/fuzz/fuzz_snapshot \
  ~/.cache/standing-tasks/fuzz/vycor-cpp/snapshot /tmp/vycor-fuzz-seeds/snapshot \
  -max_total_time=1200 -timeout=30 -rss_limit_mb=4096 \
  -artifact_prefix=/tmp/vycor-fuzz-artifacts/snapshot-
```

### Expansion Candidates & Invariants
- Once existing targets plateau, consider adding harnesses for `morph` rule JSON
  specs and `check` config/suppression parsing.
- Every finding MUST land with a standalone unit regression test that fails
  before the fix without running libFuzzer (matching `fuzz/README.md` Findings).
- Preserve `docs/deterministic-output.md` and `tests/test_utf8_strings.cpp`
  exact-escape invariants.

## Protocol: Upstream Dependency Surface Fuzzing
- **id**: `vycor-cpp/fuzz-upstream`
- **category**: `fuzzing`
- **scope**: `upstream-deps`
- **metric**: `minimized-upstream-bugs`
- **default_stop**: `time=30m/dep | bugs>=1 | indefinite`
- **tier**: `issue-only`
- **oracle**: `isolated scratch harness under ASan+UBSan`
- **summary**: Fuzz exact API surfaces vycor-cpp calls in megascope C FFI, Clang LibTooling/ASTMatchers, llvm::json, and Catch2.

### Priority Upstream Surfaces
1. **`megascope` C FFI (`../megascope/include/megascope.h`)**: malformed shard
   JSON/binary buffers, non-UTF8 paths, empty/null slices, concurrent FFI calls.
2. **LLVM / Clang (`clang::tooling`, `clang::ast_matchers`, `llvm::json`)**:
   deeply nested C++20/23 templates, malformed PCH/compile_commands entries,
   non-UTF8 source/path inputs across supported LLVM 18/20/21/22.
3. **`extern/Catch2`**: matcher/reporter edge cases exercised by `tests/`.

Build harnesses outside the git tree under
`~/.cache/standing-tasks/upstream-fuzz/vycor-cpp/`. Follow
`references/fuzzing-upstream-and-own.md` §2.

## Protocol: Adversarial Blind Mutation Testing
- **id**: `vycor-cpp/mutation-blind`
- **category**: `mutation`
- **scope**: `own-code`
- **metric**: `killed-mutants`
- **default_stop**: `cycles=5 | killed-mutants>=3 | indefinite`
- **tier**: `pr-ready`
- **oracle**: `timeout 600s cmake --build build && timeout 600s ctest --test-dir build --output-on-failure`
- **summary**: Dual-worktree adversarial blind mutation testing (Mull excluded due to missing && <-> ||) across check, callgraph, impact, and morph.

### Tooling Note
- **Mull is explicitly excluded** (`mull-project/mull` removed `cxx_logical`
  `&& <-> ||` mutators in v0.27.0 / #1115). Optional `dextool mutate --mutant lcr`
  may be used if installed; otherwise run the Adversarial Blind Mutation loop
  (`references/adversarial-mutation.md` §2).

### High-Value Target Modules
- `src/check/` (`Analyzer.cpp`, `GlobalIndex.cpp`, `Checkpoint.cpp` — ODR, ADL,
  CTAD, callback-under-lock, lock-order-inversion, unsynchronized-shared-global,
  ABI/ISA divergence, hot-path-effects, and exception-escape boolean guard chains).
- `src/callgraph/` (`CallGraph.cpp`, `ControlFlowOracle.cpp`, `PathSearch.cpp`,
  `Snapshot.cpp`, `AtomicFile.cpp`).
- `src/impact/` and `src/query/` (disambiguation, dependency audit, paging,
  exit-code contracts).
- `src/morph/` (composable multi-pass AST matcher and statement-subset rewrite guards).

## Protocol: Sanitizer Sweeps (ASan, UBSan, LSan, TSan)
- **id**: `vycor-cpp/sanitizers`
- **category**: `sanitizers`
- **scope**: `own-code`
- **metric**: `sanitizer-findings-fixed`
- **default_stop**: `findings>=1 | sweep=complete`
- **tier**: `pr-ready`
- **oracle**: `timeout 900s ctest --test-dir build-san --output-on-failure`
- **summary**: Run full ctest, cli_golden, and corpus suites under ASan+UBSan+LSan and separately under TSan for multi-threaded indexing and check workers.

### Commands
```bash
# ASan + UBSan + LSan (UB aborts; RetainUntilExit.h tracks intentional globals)
cmake -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DVYCOR_SANITIZE=address,undefined -DVYCOR_WERROR=ON
timeout 600s cmake --build build-asan
timeout 900s ctest --test-dir build-asan --output-on-failure

# TSan (cannot combine with address; exercises parallel indexing & check shards)
cmake -B build-tsan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DVYCOR_SANITIZE=thread -DVYCOR_WERROR=ON
timeout 600s cmake --build build-tsan
timeout 900s ctest --test-dir build-tsan --output-on-failure
```

Also exercise `scripts/corpus-run.py`, `scripts/warm-refresh-check.py`, and
`scripts/write-integrity-check.py` under sanitized binaries.

## Protocol: Indexing, Check & Query Performance Ratchet
- **id**: `vycor-cpp/perf`
- **category**: `perf`
- **scope**: `own-code`
- **metric**: `bench-wall-ms | peak-rss-mb`
- **default_stop**: `speedup>=5% | cycles=5 | indefinite`
- **tier**: `draft-pr`
- **oracle**: `timeout 600s cmake --build build && timeout 600s ctest --test-dir build --output-on-failure`
- **summary**: Profile and optimize callgraph indexing, check passes, and control-flow queries using scripts/bench.py, cf-access-bench.py, and ~/git/megascope-demos.

### Benchmarks & Invariants
- Local micro/macro scripts: `scripts/bench.py`, `scripts/cf-access-bench.py`.
- Real-world C++ corpus: `~/git/megascope-demos` (`fmt`, `spdlog`, `json`,
  `pybind11`, `chymera`, `vycor-cpp`).
- Build Release (`cmake --preset default`) for timing; verify byte-for-bit
  determinism (`tests/test_determinism.cpp`, `ctest -R '^(cli_golden|corpus)$'`).

## Protocol: Static Analysis Algorithm Bake-Offs
- **id**: `vycor-cpp/bakeoff`
- **category**: `bakeoff`
- **scope**: `own-code`
- **metric**: `index-wall-ms | query-p95-ms | peak-rss-mb`
- **default_stop**: `candidates=3 | indefinite`
- **tier**: `scratch-ledger`
- **oracle**: `timeout 600s ctest --test-dir build --output-on-failure`
- **summary**: Benchmark competing data layouts and traversal algorithms (string interning, SCC/path search, shard merge, AST visitor dispatch) in isolated worktrees.

### Candidate Topics
- `StringInterner` hash table / flat arena layouts vs current implementation.
- `PathSearch` / `ControlFlowOracle` bitset reachability vs bidirectional BFS.
- `RecursiveASTVisitor` direct dispatch vs `ASTMatchers` overhead in hot checks.
- Binary shard/snapshot compression and mmap layout compaction.

## Protocol: Stricter C++ Compiler Warning & clang-tidy Ratchet
- **id**: `vycor-cpp/warnings`
- **category**: `warnings`
- **scope**: `own-code`
- **metric**: `warning-flags-locked | clang-tidy-checks-clean`
- **default_stop**: `flags>=1 | warnings=0 | indefinite`
- **tier**: `pr-ready`
- **oracle**: `timeout 600s cmake --build build-warn && timeout 600s ctest --test-dir build-warn --output-on-failure`
- **summary**: Escalate VYCOR_WARNING_FLAGS beyond -Wall -Wextra toward -Wshadow, -Wconversion, -Wsign-conversion, -Wnon-virtual-dtor, and clang-tidy checks under -DVYCOR_WERROR=ON.

### Escalation Ladder
`CMakeLists.txt` currently sets `-Wall -Wextra -Wno-missing-field-initializers`
(MSVC `/W4`) with `-DVYCOR_WERROR=ON`. Ratchet one flag or `clang-tidy` group
per PR across `src/` and `include/` (never `extern/`):
1. `-Wshadow`, `-Wnon-virtual-dtor`, `-Woverloaded-virtual`
2. `-Wsign-conversion`, `-Wconversion` (module-by-module)
3. `clang-tidy -p build` checks: `bugprone-*`, `performance-*`, `modernize-use-override`
4. Verify compatibility across installed Clang and GCC toolchains.

## Protocol: Static Analysis & C++ Soundness Research Pulls
- **id**: `vycor-cpp/research`
- **category**: `research`
- **scope**: `theory`
- **metric**: `verified-writeups`
- **default_stop**: `notes=1 | indefinite`
- **tier**: `research-doc`
- **oracle**: `web-verified citations + local docs/checks/ grounding`
- **summary**: Pull verified compiler/static-analysis theory on cross-TU C++ ODR/ABI/ADL checks, scalable call-graph construction, and deterministic AST refactoring.

### Research Queue
- Demand-driven vs whole-program call-graph & points-to precision for C++
  virtual/function-pointer dispatch (`src/callgraph/`).
- Formal semantics & edge cases of C++20 modules/concepts for ADL, CTAD, and
  specialization-visibility checks (`docs/checks/`).
- Incremental summary-based interprocedural analysis for lock-order and
  exception-escape checks (`callback-under-lock.md`, `exception-escape.md`).
