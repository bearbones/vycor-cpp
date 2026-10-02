// Copyright (c) 2026 The vycor-cpp Authors
// Original author: Alex Mason
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// test_anneal_checkpoint.cpp — anneal worker-pool parallelism and the
// per-TU checkpoint journal (kill-resume), over an on-disk scratch fixture
// with a genuinely fragile ADL resolution.

#include "vycor/anneal/Analyzer.h"
#include "vycor/anneal/Checkpoint.h"
#include "vycor/anneal/GlobalIndex.h"
#include "vycor/anneal/Indexer.h"
#include "vycor/compat/ToolAdjusters.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Path.h"

#include <catch2/catch_test_macros.hpp>
#include <clang/Tooling/CompilationDatabase.h>
#include <clang/Tooling/JSONCompilationDatabase.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace vycor;

namespace {

// A fresh directory named after `dir` (dir-XXXXXX), stored back into `dir`:
// ctest runs test cases as parallel processes, and a fixed name let one
// case's cleanup delete another's sources mid-run.
[[maybe_unused]] bool makeUniqueFixtureDir(std::string &dir) {
  llvm::SmallString<128> made;
  if (llvm::sys::fs::createUniqueDirectory(dir, made))
    return false;
  dir = std::string(made);
  return true;
}

// On-disk scratch project with a genuinely fragile TU (written fresh per
// test case, removed on destruction). user_a.cpp and user_c.cpp call
// scale(v, 3.14) with only core.hpp included, so they resolve to the int
// overload while ext.hpp's double overload — indexed globally via
// user_b.cpp — is invisible to them: an ADL fallback diagnostic each.
// (The examples/adl_fallback fixture can't serve here: both its TUs
// include every header, so nothing is globally-visible-but-not-included.)
// No system headers involved, so no host-toolchain dependence.
struct ScratchFixture {
  std::string dir = "anneal_ckpt_fixture";

  ScratchFixture() {
    REQUIRE(makeUniqueFixtureDir(dir));
    write("core.hpp", R"cpp(
#pragma once
namespace MathLib {
struct Vector { int x = 0; };
inline void scale(Vector, int) {}
}
)cpp");
    write("ext.hpp", R"cpp(
#pragma once
#include "core.hpp"
namespace MathLib {
inline void scale(Vector, double) {}
}
)cpp");
    const char *fragileUser = R"cpp(
#include "core.hpp"
void use_a() {
  MathLib::Vector v;
  scale(v, 3.14);
}
)cpp";
    write("user_a.cpp", fragileUser);
    write("user_b.cpp", R"cpp(
#include "core.hpp"
#include "ext.hpp"
void use_b() {
  MathLib::Vector v;
  scale(v, 3.14);
}
)cpp");
    write("user_c.cpp", fragileUser);
  }

  ~ScratchFixture() {
    for (const char *f : {"core.hpp", "ext.hpp", "user_a.cpp", "user_b.cpp",
                          "user_c.cpp"})
      std::remove((dir + "/" + f).c_str());
    llvm::sys::fs::remove(dir);
  }

  void write(const std::string &name, const std::string &content) const {
    std::ofstream out(dir + "/" + name, std::ios::binary | std::ios::trunc);
    REQUIRE(out.good());
    out << content;
  }

  std::string path(const std::string &name) const {
    llvm::SmallString<256> abs;
    REQUIRE(!llvm::sys::fs::real_path(dir, abs));
    llvm::sys::path::append(abs, name);
    return std::string(abs.str());
  }

  // user_b (the ext.hpp contributor) deliberately in the middle.
  std::vector<std::string> files() const {
    return {path("user_a.cpp"), path("user_b.cpp"), path("user_c.cpp")};
  }

  clang::tooling::FixedCompilationDatabase db() const {
    return clang::tooling::FixedCompilationDatabase(".", {"-std=c++17"});
  }
};

// Comparable, order-insensitive projection of a diagnostics list.
std::vector<std::string> sortedKeys(const std::vector<Diagnostic> &diags) {
  std::vector<std::string> keys;
  keys.reserve(diags.size());
  for (const auto &d : diags)
    keys.push_back(std::to_string(static_cast<int>(d.kind)) + "|" +
                   d.callLocation + "|" + d.message);
  std::sort(keys.begin(), keys.end());
  return keys;
}

uint64_t fileSize(const std::string &path) {
  uint64_t size = 0;
  REQUIRE(!llvm::sys::fs::file_size(path, size));
  return size;
}

// "file|status|detail" per TU, in report order.
std::vector<std::string> outcomeRows(const AnalysisReport &report) {
  std::vector<std::string> rows;
  for (const auto &[file, outcome] : report.tus)
    rows.push_back(llvm::sys::path::filename(file).str() + "|" +
                   tuStatusName(outcome.status) + "|" + outcome.detail);
  return rows;
}

struct CheckpointFileGuard {
  std::string path;
  explicit CheckpointFileGuard(std::string p) : path(std::move(p)) {
    std::remove(path.c_str());
  }
  ~CheckpointFileGuard() { std::remove(path.c_str()); }
};

} // namespace

TEST_CASE("GlobalIndex absorb merges shard entries and type relations",
          "[AnnealCheckpoint]") {
  GlobalIndex shard;
  FunctionOverloadEntry fn;
  fn.qualifiedName = "MathLib::scale";
  fn.headerPath = "Extension.hpp";
  fn.paramTypes = {"MathLib::Vector", "double"};
  fn.returnType = "MathLib::Vector";
  fn.sourceLine = 12;
  shard.addFunctionOverload(fn);

  DeductionGuideEntry guide;
  guide.templateName = "Container";
  guide.headerPath = "Guide.hpp";
  guide.paramTypes = {"const char *"};
  guide.deducedType = "Container<std::string>";
  shard.addDeductionGuide(guide);

  shard.mutableTypeRelations().addBase("Derived", "Base");
  shard.mutableTypeRelations().addCtorEdge("Wrapper", "int");

  GlobalIndex master;
  master.absorb(shard);

  REQUIRE(master.overloadCount() == 1);
  REQUIRE(master.guideCount() == 1);
  auto found = master.findOverloads("MathLib::scale");
  REQUIRE(found.size() == 1);
  CHECK(found[0]->headerPath == "Extension.hpp");
  CHECK(found[0]->paramTypes.size() == 2);
  CHECK(master.typeRelations().isBaseOrSelf("Derived", "Base"));
  CHECK(master.typeRelations().isConvertible("int", "Wrapper"));
}

TEST_CASE("Parallel anneal produces the same diagnostics as serial",
          "[AnnealCheckpoint]") {
  ScratchFixture fx;
  auto compDb = fx.db();
  auto files = fx.files();

  AnalysisOptions serial;
  serial.threadCount = 1;
  auto serialDiags = runAnalysis(compDb, files, serial);
  REQUIRE(!serialDiags.empty()); // fixture is designed to trip the analyzer

  AnalysisOptions parallel;
  parallel.threadCount = 0; // all hardware threads
  auto parallelDiags = runAnalysis(compDb, files, parallel);

  CHECK(sortedKeys(serialDiags) == sortedKeys(parallelDiags));
}

TEST_CASE("Checkpoint warm resume replays both phases without new records",
          "[AnnealCheckpoint]") {
  CheckpointFileGuard ckpt("anneal_ckpt_warm.vycj");
  ScratchFixture fx;
  auto compDb = fx.db();
  auto files = fx.files();

  AnalysisOptions opts;
  opts.threadCount = 1;
  opts.checkpointPath = ckpt.path;

  auto coldDiags = runAnalysis(compDb, files, opts);
  REQUIRE(llvm::sys::fs::exists(ckpt.path));
  const uint64_t coldSize = fileSize(ckpt.path);

  auto warmDiags = runAnalysis(compDb, files, opts);
  CHECK(sortedKeys(coldDiags) == sortedKeys(warmDiags));
  // Everything replayed from the journal: nothing was appended.
  CHECK(fileSize(ckpt.path) == coldSize);

  // And the checkpointed run matches a checkpoint-free run.
  AnalysisOptions plain;
  plain.threadCount = 1;
  CHECK(sortedKeys(warmDiags) == sortedKeys(runAnalysis(compDb, files, plain)));
}

TEST_CASE("Partial journal (killed run) is completed on resume",
          "[AnnealCheckpoint]") {
  CheckpointFileGuard ckpt("anneal_ckpt_partial.vycj");
  ScratchFixture fx;
  auto compDb = fx.db();
  auto files = fx.files();

  AnalysisOptions opts;
  opts.threadCount = 1;
  opts.checkpointPath = ckpt.path;

  // Simulate a run that died after finishing only user_a: the journal
  // holds its records and nothing about the other TUs.
  runAnalysis(compDb, {files[0]}, opts);
  REQUIRE(llvm::sys::fs::exists(ckpt.path));

  // Resume over the full file set: user_a's phase-1 record replays; the
  // others are parsed fresh; phase 2 re-runs everywhere (the contributing
  // set — and with it the validity hash — changed).
  auto resumed = runAnalysis(compDb, files, opts);

  AnalysisOptions plain;
  plain.threadCount = 1;
  CHECK(sortedKeys(resumed) == sortedKeys(runAnalysis(compDb, files, plain)));
}

TEST_CASE("Corrupt or truncated journal tail is dropped, not fatal",
          "[AnnealCheckpoint]") {
  ScratchFixture fx;
  auto compDb = fx.db();
  auto files = fx.files();

  AnalysisOptions opts;
  opts.threadCount = 1;

  AnalysisOptions plain = opts;
  auto expected = sortedKeys(runAnalysis(compDb, files, plain));

  SECTION("garbage appended after valid records") {
    CheckpointFileGuard ckpt("anneal_ckpt_garbage.vycj");
    opts.checkpointPath = ckpt.path;
    runAnalysis(compDb, files, opts);
    {
      std::ofstream f(ckpt.path, std::ios::binary | std::ios::app);
      f << "\x07garbage-not-a-record";
    }
    CHECK(sortedKeys(runAnalysis(compDb, files, opts)) == expected);
  }

  SECTION("journal truncated mid-record") {
    CheckpointFileGuard ckpt("anneal_ckpt_truncated.vycj");
    opts.checkpointPath = ckpt.path;
    runAnalysis(compDb, files, opts);
    const uint64_t size = fileSize(ckpt.path);
    REQUIRE(size > 32);
    // Chop into the last record's payload: exactly what a kill mid-append
    // leaves behind.
    {
      std::ifstream in(ckpt.path, std::ios::binary);
      std::string bytes((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
      in.close();
      REQUIRE(bytes.size() == size);
      bytes.resize(bytes.size() - 17);
      std::ofstream out(ckpt.path, std::ios::binary | std::ios::trunc);
      out << bytes;
    }
    CHECK(sortedKeys(runAnalysis(compDb, files, opts)) == expected);
  }
}

TEST_CASE("Options change invalidates the whole journal",
          "[AnnealCheckpoint]") {
  CheckpointFileGuard ckpt("anneal_ckpt_options.vycj");
  ScratchFixture fx;
  auto compDb = fx.db();
  auto files = fx.files();

  AnalysisOptions base;
  base.threadCount = 1;
  base.checkpointPath = ckpt.path;
  runAnalysis(compDb, files, base);

  // Same journal path, different analysis options: the fingerprint
  // mismatch discards it, and the run must match a fresh no-checkpoint run
  // with those options (not replay stale diagnostics).
  AnalysisOptions changed = base;
  changed.warnSameScore = true;
  auto withCkpt = runAnalysis(compDb, files, changed);

  AnalysisOptions plainChanged;
  plainChanged.threadCount = 1;
  plainChanged.warnSameScore = true;
  CHECK(sortedKeys(withCkpt) ==
        sortedKeys(runAnalysis(compDb, files, plainChanged)));
}

TEST_CASE("A TU whose parse died kMaxAttempts times is skipped on resume",
          "[AnnealCheckpoint]") {
  CheckpointFileGuard ckpt("anneal_ckpt_poison.vycj");
  ScratchFixture fx;
  auto compDb = fx.db();
  auto files = fx.files();

  AnalysisOptions opts;
  opts.threadCount = 1;
  opts.checkpointPath = ckpt.path;

  // Forge the aftermath of two runs killed during user_c's phase-1 parse:
  // two attempt records, no completion.
  auto stamps = SnapshotIO::stampFiles({files[2]});
  REQUIRE(stamps.size() == 1);
  {
    auto journal =
        AnnealCheckpoint::open(ckpt.path, annealOptionsFingerprint(opts));
    REQUIRE(journal);
    journal->recordAttempt(AnnealCheckpoint::kPhaseIndex, files[2],
                          stamps[0]);
    journal->recordAttempt(AnnealCheckpoint::kPhaseIndex, files[2],
                          stamps[0]);
  }

  // Resume: user_c is poisoned and skipped entirely (both phases); the run
  // degrades to a user_a + user_b analysis — user_a's fragile-ADL
  // diagnostic still fires — instead of dying a third time.
  AnalysisReport report;
  auto resumed = runAnalysis(compDb, files, opts, nullptr, &report);
  REQUIRE(!resumed.empty());
  CHECK(outcomeRows(report).back() ==
        "user_c.cpp|crashed|phase-1 parse died 2 times (checkpoint)");

  AnalysisOptions plain;
  plain.threadCount = 1;
  CHECK(sortedKeys(resumed) ==
        sortedKeys(runAnalysis(compDb, {files[0], files[1]}, plain)));
}

TEST_CASE("Attempt counters reset on success and on stamp change",
          "[AnnealCheckpoint]") {
  CheckpointFileGuard ckpt("anneal_ckpt_attempts.vycj");

  FileStamp stampA;
  stampA.path = "x.cpp";
  stampA.mtimeNs = 111;
  stampA.size = 10;
  FileStamp stampB = stampA;
  stampB.mtimeNs = 222; // edited file

  {
    auto journal = AnnealCheckpoint::open(ckpt.path, 42);
    REQUIRE(journal);
    journal->recordAttempt(AnnealCheckpoint::kPhaseIndex, "x.cpp", stampA);
    journal->recordAttempt(AnnealCheckpoint::kPhaseIndex, "x.cpp", stampA);
    CHECK(journal->attempts(AnnealCheckpoint::kPhaseIndex, "x.cpp", stampA) ==
          2);
    // A different stamp doesn't inherit the poison.
    CHECK(journal->attempts(AnnealCheckpoint::kPhaseIndex, "x.cpp", stampB) ==
          0);
    // Success clears the counter.
    GlobalIndex empty;
    journal->recordPhase1("x.cpp", stampA, empty);
    CHECK(journal->attempts(AnnealCheckpoint::kPhaseIndex, "x.cpp", stampA) ==
          0);
  }

  // Same story after a reload from disk.
  auto reloaded = AnnealCheckpoint::open(ckpt.path, 42);
  REQUIRE(reloaded);
  CHECK(reloaded->attempts(AnnealCheckpoint::kPhaseIndex, "x.cpp", stampA) ==
        0);
}

// ============================================================================
// Worker isolation (--isolate-workers seam)
// ============================================================================

namespace {

// In-process stand-in for the CLI's subprocess workers: same per-TU loop,
// same WORKER-TU markers, same shard files — everything except the spawn.
// `crashOn`, when non-empty, makes the runner die (nonzero exit) right
// after writing that TU's marker, exactly like a parse crashing a worker.
AnnealWorkerRunner
makeInProcessRunner(const clang::tooling::CompilationDatabase &compDb,
                    AnalysisOptions workerOpts,
                    std::atomic<unsigned> *invocations = nullptr,
                    std::string crashOn = "") {
  return [&compDb, workerOpts, invocations,
          crashOn](uint8_t phase, const std::string &globalIndexPath,
                   const std::vector<std::string> &batch,
                   const std::string &shardPath,
                   const std::string &stderrPath) -> int {
    if (invocations)
      ++*invocations;
    std::ofstream err(stderrPath);
    if (phase == AnnealCheckpoint::kPhaseIndex) {
      std::vector<std::pair<std::string, AnnealIndexPayload>> shards;
      std::vector<TuOutcome> outcomes;
      for (const auto &f : batch) {
        err << "WORKER-TU " << f << "\n";
        err.flush();
        if (f == crashOn)
          return 139; // "crashed" mid-parse
        GlobalIndex shard;
        auto tool = makeClangTool(compDb, {f});
        IndexerActionFactory factory(shard);
        outcomes.push_back(outcomeForToolStatus(tool.run(&factory)));
        shards.emplace_back(f, AnnealIndexPayload::capture(shard));
      }
      return writeAnnealIndexShard(shardPath, shards, outcomes) ? 0 : 1;
    }
    GlobalIndex index;
    if (!readGlobalIndexFile(globalIndexPath, index))
      return 1;
    std::vector<std::pair<std::string, std::vector<Diagnostic>>> perTu;
    std::vector<TuOutcome> outcomes;
    for (const auto &f : batch) {
      err << "WORKER-TU " << f << "\n";
      err.flush();
      if (f == crashOn)
        return 139;
      std::vector<Diagnostic> local;
      auto tool = makeClangTool(compDb, {f});
      AnalyzerActionFactory factory(index, local, workerOpts);
      outcomes.push_back(outcomeForToolStatus(tool.run(&factory)));
      perTu.emplace_back(f, std::move(local));
    }
    return writeAnnealDiagShard(shardPath, perTu, outcomes) ? 0 : 1;
  };
}

} // namespace

TEST_CASE("Shard files round-trip index payloads, diagnostics, and the "
          "merged index",
          "[AnnealIsolated]") {
  CheckpointFileGuard shard("anneal_shard_roundtrip.bin");

  SECTION("index shard") {
    GlobalIndex src;
    FunctionOverloadEntry fn;
    fn.qualifiedName = "NS::f";
    fn.headerPath = "h.hpp";
    fn.paramTypes = {"int"};
    src.addFunctionOverload(fn);
    src.mutableTypeRelations().addBase("D", "B");

    REQUIRE(writeAnnealIndexShard(
        shard.path, {{"tu1.cpp", AnnealIndexPayload::capture(src)}},
        {AnnealCheckpoint::kCleanParse}));

    GlobalIndex dst;
    unsigned seen = 0;
    REQUIRE(readAnnealIndexShard(
        shard.path, [&](const std::string &tu, const AnnealIndexPayload &p) {
          CHECK(tu == "tu1.cpp");
          p.applyTo(dst);
          ++seen;
        }));
    CHECK(seen == 1);
    CHECK(dst.overloadCount() == 1);
    CHECK(dst.findOverloads("NS::f").size() == 1);
    CHECK(dst.typeRelations().isBaseOrSelf("D", "B"));
  }

  SECTION("diagnostics shard") {
    Diagnostic d;
    d.kind = Diagnostic::ADL_Fallback;
    d.callLocation = "a.cpp:1:1";
    d.message = "msg";
    d.scope = "c:@F@use#";
    REQUIRE(writeAnnealDiagShard(shard.path, {{"a.cpp", {d}}},
                                 {AnnealCheckpoint::kCleanParse}));

    unsigned seen = 0;
    REQUIRE(readAnnealDiagShard(
        shard.path, [&](const std::string &tu, std::vector<Diagnostic> diags) {
          CHECK(tu == "a.cpp");
          REQUIRE(diags.size() == 1);
          CHECK(diags[0].kind == Diagnostic::ADL_Fallback);
          CHECK(diags[0].message == "msg");
          CHECK(diags[0].scope == "c:@F@use#");
          ++seen;
        }));
    CHECK(seen == 1);
  }

  SECTION("merged index file") {
    GlobalIndex src;
    DeductionGuideEntry g;
    g.templateName = "Box";
    g.deducedType = "Box<int>";
    src.addDeductionGuide(g);
    REQUIRE(writeGlobalIndexFile(shard.path, src));

    GlobalIndex dst;
    REQUIRE(readGlobalIndexFile(shard.path, dst));
    CHECK(dst.guideCount() == 1);
    CHECK(dst.findDeductionGuides("Box").size() == 1);
  }

  SECTION("garbage is rejected, not fatal") {
    {
      std::ofstream f(shard.path, std::ios::binary | std::ios::trunc);
      f << "not a shard at all";
    }
    GlobalIndex dst;
    CHECK_FALSE(readAnnealIndexShard(shard.path,
                                     [](const std::string &,
                                        const AnnealIndexPayload &) {}));
    CHECK_FALSE(readGlobalIndexFile(shard.path, dst));
  }
}

namespace {

void putU32At(std::string &b, size_t at, uint32_t v) {
  for (int i = 0; i < 4; ++i)
    b[at + i] = static_cast<char>((v >> (8 * i)) & 0xff);
}

uint32_t u32At(const std::string &b, size_t at) {
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i)
    v |= static_cast<uint32_t>(static_cast<uint8_t>(b[at + i])) << (8 * i);
  return v;
}

std::string slurp(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

} // namespace

// fuzz_checkpoint finding (UBSan): a diagnostic kind outside
// Diagnostic::Kind was cast into the enum, undefined behavior the first
// time the kind was read. Shards and journal records now refuse it.
TEST_CASE("A diagnostic kind outside the enum is refused and not loaded",
          "[AnnealIsolated][AnnealCheckpoint]") {
  Diagnostic d;
  d.kind = Diagnostic::ADL_Fallback;
  d.callLocation = "ZZloc.cpp:1:1";
  d.message = "msg";

  SECTION("diagnostics shard") {
    CheckpointFileGuard shard("anneal_shard_badkind.bin");
    REQUIRE(writeAnnealDiagShard(shard.path, {{"a.cpp", {d}}}));
    std::string bytes = slurp(shard.path);
    // magic, version, count; tu "a.cpp"; payload length; then the
    // payload: diagnostic count, kind, ...; the checksum follows it.
    const size_t lenAt = 12 + 4 + 5, payload = lenAt + 4;
    const uint32_t len = u32At(bytes, lenAt);
    REQUIRE(u32At(bytes, payload) == 1);
    putU32At(bytes, payload + 4, 32);
    putU32At(bytes, payload + len,
             annealRecordChecksum(bytes.data() + payload, len));
    std::ofstream(shard.path, std::ios::binary | std::ios::trunc) << bytes;
    CHECK_FALSE(readAnnealDiagShard(
        shard.path, [](const std::string &, std::vector<Diagnostic>) {}));
  }

  SECTION("journal phase-2 record") {
    CheckpointFileGuard ckpt("anneal_ckpt_badkind.bin");
    FileStamp stamp;
    stamp.path = "a.cpp";
    stamp.mtimeNs = 1;
    stamp.size = 2;
    {
      auto journal = AnnealCheckpoint::open(ckpt.path, 7);
      REQUIRE(journal);
      journal->recordPhase2("a.cpp", stamp, 99, {d});
    }
    std::string bytes = slurp(ckpt.path);
    // Header (16), then the one record: kind u8, length u32, payload,
    // checksum. The diagnostic's kind sits just before its location.
    const size_t payload = 16 + 5;
    const uint32_t len = u32At(bytes, 17);
    const size_t loc = bytes.find("ZZloc.cpp");
    REQUIRE(loc != std::string::npos);
    putU32At(bytes, loc - 8, 32);
    putU32At(bytes, payload + len,
             annealRecordChecksum(bytes.data() + payload, len));
    std::ofstream(ckpt.path, std::ios::binary | std::ios::trunc) << bytes;
    auto journal = AnnealCheckpoint::open(ckpt.path, 7);
    REQUIRE(journal);
    std::vector<Diagnostic> out;
    CHECK_FALSE(journal->replayPhase2("a.cpp", stamp, 99, out));
    CHECK(out.empty());    journal.reset();
    std::remove((ckpt.path + ".lock").c_str());
  }
}

TEST_CASE("Isolated anneal produces the same diagnostics as in-process",
          "[AnnealIsolated]") {
  ScratchFixture fx;
  auto compDb = fx.db();
  auto files = fx.files();

  AnalysisOptions plain;
  plain.threadCount = 1;
  auto expected = runAnalysis(compDb, files, plain);
  REQUIRE(!expected.empty());

  AnalysisOptions isolated;
  isolated.threadCount = 1;
  isolated.workerCount = 2;
  isolated.isolatedRunner = makeInProcessRunner(compDb, plain);
  auto got = runAnalysis(compDb, files, isolated);

  CHECK(sortedKeys(got) == sortedKeys(expected));
}

TEST_CASE("Isolation composes with the checkpoint: shard results are "
          "journaled and replayed",
          "[AnnealIsolated]") {
  CheckpointFileGuard ckpt("anneal_ckpt_isolated.vycj");
  ScratchFixture fx;
  auto compDb = fx.db();
  auto files = fx.files();

  AnalysisOptions plain;
  plain.threadCount = 1;

  std::atomic<unsigned> invocations{0};
  AnalysisOptions isolated;
  isolated.threadCount = 1;
  isolated.workerCount = 2;
  isolated.checkpointPath = ckpt.path;
  isolated.isolatedRunner = makeInProcessRunner(compDb, plain, &invocations);

  auto cold = runAnalysis(compDb, files, isolated);
  REQUIRE(llvm::sys::fs::exists(ckpt.path));
  REQUIRE(invocations.load() > 0);

  // Warm resume: everything replays from the journal — no worker runs.
  invocations = 0;
  auto warm = runAnalysis(compDb, files, isolated);
  CHECK(invocations.load() == 0);
  CHECK(sortedKeys(warm) == sortedKeys(cold));
  CHECK(sortedKeys(warm) == sortedKeys(runAnalysis(compDb, files, plain)));
}

TEST_CASE("A TU that crashes its worker is poisoned; the rest complete",
          "[AnnealIsolated]") {
  ScratchFixture fx;
  auto compDb = fx.db();
  auto files = fx.files();

  AnalysisOptions plain;
  plain.threadCount = 1;

  AnalysisOptions isolated;
  isolated.threadCount = 1;
  isolated.workerCount = 2;
  isolated.isolatedRunner =
      makeInProcessRunner(compDb, plain, nullptr, /*crashOn=*/files[2]);

  // user_c kills every worker that touches it: the bisect protocol poisons
  // exactly user_c and the run degrades to a user_a + user_b analysis.
  auto got = runAnalysis(compDb, files, isolated);
  REQUIRE(!got.empty());
  CHECK(sortedKeys(got) ==
        sortedKeys(runAnalysis(compDb, {files[0], files[1]}, plain)));
}

TEST_CASE("A TU the isolated dispatcher poisoned is skipped on resume",
          "[AnnealIsolated]") {
  // Without an attempt record, every resume dispatched the poisoned TU
  // again (and, for a hung TU, waited out the worker timeout again).
  CheckpointFileGuard ckpt("anneal_ckpt_isolated_poison.vycj");
  ScratchFixture fx;
  auto compDb = fx.db();
  auto files = fx.files();

  AnalysisOptions plain;
  plain.threadCount = 1;

  std::atomic<unsigned> invocations{0};
  AnalysisOptions isolated;
  isolated.threadCount = 1;
  isolated.workerCount = 2;
  isolated.checkpointPath = ckpt.path;
  isolated.isolatedRunner = makeInProcessRunner(compDb, plain, &invocations,
                                                /*crashOn=*/files[2]);

  AnalysisReport coldReport, warmReport;
  auto cold = runAnalysis(compDb, files, isolated, nullptr, &coldReport);
  REQUIRE(invocations.load() > 0);

  // Resume: user_a and user_b replay from the journal, user_c is skipped
  // as poisoned — no worker runs at all.
  invocations = 0;
  auto warm = runAnalysis(compDb, files, isolated, nullptr, &warmReport);
  CHECK(invocations.load() == 0);
  CHECK(sortedKeys(warm) == sortedKeys(cold));
  // ... and reported the way the run that poisoned it reported it, not as
  // an in-process parse that died.
  CHECK(outcomeRows(coldReport) ==
        std::vector<std::string>{"user_a.cpp|indexed|", "user_b.cpp|indexed|",
                                 "user_c.cpp|poisoned|worker crashed"});
  CHECK(outcomeRows(warmReport) == outcomeRows(coldReport));
}

// ---- per-TU outcomes (anneal as a CI gate) ---------------------------------

namespace {

// The scratch fixture plus a TU whose parse fails (a missing include).
struct BrokenTuFixture : ScratchFixture {
  BrokenTuFixture() {
    write("broken.cpp", "#include \"no-such-header.hpp\"\nint broken();\n");
  }
  ~BrokenTuFixture() { std::remove((dir + "/broken.cpp").c_str()); }
  std::vector<std::string> filesWithBroken() const {
    auto f = files();
    f.push_back(path("broken.cpp"));
    return f;
  }
};

} // namespace

TEST_CASE("runAnalysis reports every TU's parse outcome in source order",
          "[AnnealOutcomes]") {
  BrokenTuFixture fx;
  auto compDb = fx.db();
  auto files = fx.filesWithBroken();

  AnalysisOptions opts;
  opts.threadCount = 2;
  AnalysisReport report;
  auto diags = runAnalysis(compDb, files, opts, nullptr, &report);
  CHECK(!diags.empty()); // the clean TUs are still analyzed
  CHECK(outcomeRows(report) ==
        std::vector<std::string>{"user_a.cpp|indexed|", "user_b.cpp|indexed|",
                                 "user_c.cpp|indexed|",
                                 "broken.cpp|partial|parse errors"});
}

TEST_CASE("Outcomes are identical in-process, resumed from a checkpoint, and "
          "from isolated workers",
          "[AnnealOutcomes]") {
  BrokenTuFixture fx;
  auto compDb = fx.db();
  auto files = fx.filesWithBroken();

  AnalysisOptions plain;
  plain.threadCount = 1;
  AnalysisReport expected;
  auto expectedDiags = runAnalysis(compDb, files, plain, nullptr, &expected);

  SECTION("checkpoint: cold, then replayed without a parse") {
    CheckpointFileGuard ckpt("anneal_ckpt_outcomes.vycj");
    AnalysisOptions opts = plain;
    opts.checkpointPath = ckpt.path;
    AnalysisReport cold, warm;
    auto coldDiags = runAnalysis(compDb, files, opts, nullptr, &cold);
    auto warmDiags = runAnalysis(compDb, files, opts, nullptr, &warm);
    CHECK(outcomeRows(cold) == outcomeRows(expected));
    CHECK(outcomeRows(warm) == outcomeRows(expected));
    CHECK(sortedKeys(warmDiags) == sortedKeys(expectedDiags));
  }

  SECTION("isolated workers, with and without the checkpoint") {
    CheckpointFileGuard ckpt("anneal_ckpt_outcomes_iso.vycj");
    AnalysisOptions opts = plain;
    opts.workerCount = 2;
    opts.isolatedRunner = makeInProcessRunner(compDb, plain);
    AnalysisReport isolated;
    runAnalysis(compDb, files, opts, nullptr, &isolated);
    CHECK(outcomeRows(isolated) == outcomeRows(expected));

    opts.checkpointPath = ckpt.path;
    AnalysisReport cold, warm;
    runAnalysis(compDb, files, opts, nullptr, &cold);
    runAnalysis(compDb, files, opts, nullptr, &warm);
    CHECK(outcomeRows(cold) == outcomeRows(expected));
    CHECK(outcomeRows(warm) == outcomeRows(expected));
  }
}

TEST_CASE("A TU that crashes its worker is reported poisoned",
          "[AnnealOutcomes]") {
  ScratchFixture fx;
  auto compDb = fx.db();
  auto files = fx.files();

  AnalysisOptions plain;
  plain.threadCount = 1;
  AnalysisOptions isolated = plain;
  isolated.workerCount = 2;
  isolated.isolatedRunner =
      makeInProcessRunner(compDb, plain, nullptr, /*crashOn=*/files[2]);
  AnalysisReport report;
  runAnalysis(compDb, files, isolated, nullptr, &report);
  CHECK(outcomeRows(report) ==
        std::vector<std::string>{"user_a.cpp|indexed|", "user_b.cpp|indexed|",
                                 "user_c.cpp|poisoned|worker crashed"});
}

TEST_CASE("Shards carry each TU's outcome", "[AnnealOutcomes]") {
  CheckpointFileGuard shard("anneal_shard_outcomes.bin");
  Diagnostic d;
  d.kind = Diagnostic::Custom;
  d.checkName = "org-check";
  d.entities = {"ns::f(int)", "ns::g()"};
  REQUIRE(writeAnnealDiagShard(shard.path, {{"a.cpp", {d}}, {"b.cpp", {}}},
                               {{TuStatus::Indexed, ""},
                                {TuStatus::Partial, "parse errors"}}));
  std::vector<std::string> seen;
  REQUIRE(readAnnealDiagShard(
      shard.path,
      [&](const std::string &tu, std::vector<Diagnostic> diags) {
        if (tu == "a.cpp") {
          REQUIRE(diags.size() == 1);
          CHECK(diags[0].entities == d.entities);
        }
      },
      [&](const std::string &tu, const TuOutcome &outcome) {
        seen.push_back(tu + "|" + tuStatusName(outcome.status));
      }));
  CHECK(seen == std::vector<std::string>{"a.cpp|indexed", "b.cpp|partial"});

  // An entry the outcomes do not cover reads as skipped, never as clean.
  REQUIRE(writeAnnealIndexShard(shard.path, {{"c.cpp", {}}}, {}));
  seen.clear();
  REQUIRE(readAnnealIndexShard(
      shard.path, [](const std::string &, const AnnealIndexPayload &) {},
      [&](const std::string &tu, const TuOutcome &outcome) {
        seen.push_back(tu + "|" + tuStatusName(outcome.status));
      }));
  CHECK(seen == std::vector<std::string>{"c.cpp|skipped"});
}

// ---- review follow-ups -------------------------------------------------------

TEST_CASE("A TU whose parse failed is parsed again on resume, so fixing it "
          "clears the failure",
          "[AnnealOutcomes]") {
  CheckpointFileGuard ckpt("anneal_ckpt_refix.vycj");
  BrokenTuFixture fx;
  auto compDb = fx.db();
  auto files = fx.filesWithBroken();

  AnalysisOptions opts;
  opts.threadCount = 1;
  opts.checkpointPath = ckpt.path;
  AnalysisReport first;
  runAnalysis(compDb, files, opts, nullptr, &first);
  CHECK(outcomeRows(first).back() == "broken.cpp|partial|parse errors");

  // The missing header appears (a generated file). broken.cpp's own stamp
  // is unchanged, but a failed parse is never a cache entry.
  fx.write("no-such-header.hpp", "#pragma once\n");
  AnalysisReport second;
  runAnalysis(compDb, files, opts, nullptr, &second);
  std::remove((fx.dir + "/no-such-header.hpp").c_str());
  CHECK(outcomeRows(second).back() == "broken.cpp|indexed|");
}

TEST_CASE("Declarations a re-parsed TU gains invalidate the other TUs' "
          "phase-2 records",
          "[AnnealOutcomes]") {
  CheckpointFileGuard ckpt("anneal_ckpt_regen.vycj");
  ScratchFixture fx;
  // gen.cpp reaches ext.hpp's better overload only through gen.hpp, which
  // does not exist yet: until it does, user_a has no fragile call.
  fx.write("gen.cpp", "#include \"gen.hpp\"\nvoid gen();\n");
  auto compDb = fx.db();
  const std::vector<std::string> files = {fx.path("user_a.cpp"),
                                          fx.path("gen.cpp")};
  AnalysisOptions opts;
  opts.threadCount = 1;
  opts.checkpointPath = ckpt.path;
  CHECK(runAnalysis(compDb, files, opts).empty());

  fx.write("gen.hpp", "#pragma once\n#include \"ext.hpp\"\n");
  AnalysisReport report;
  auto resumed = runAnalysis(compDb, files, opts, nullptr, &report);
  std::remove((fx.dir + "/gen.hpp").c_str());
  std::remove((fx.dir + "/gen.cpp").c_str());
  CHECK(outcomeRows(report) ==
        std::vector<std::string>{"user_a.cpp|indexed|", "gen.cpp|indexed|"});
  REQUIRE(resumed.size() == 1);
  CHECK(resumed[0].kind == Diagnostic::ADL_Fallback);
}

TEST_CASE("Locations are absolute against the TU's compile directory",
          "[AnnealOutcomes]") {
  // A Meson-style database: `directory` is the build directory and `file`
  // is relative to it. Every recorded path must name the real file, not
  // one resolved against wherever anneal runs.
  ScratchFixture fx;
  const std::string buildDir = fx.dir + "/build";
  REQUIRE(!llvm::sys::fs::create_directory(buildDir));
  llvm::json::Array entries;
  for (const char *f : {"user_a.cpp", "user_b.cpp"})
    entries.push_back(llvm::json::Object{
        {"directory", fx.path("build")},
        {"file", std::string("../") + f},
        {"arguments", llvm::json::Array{"clang++", "-std=c++17", "-c",
                                        std::string("../") + f}}});
  std::string error;
  auto compDb = clang::tooling::JSONCompilationDatabase::loadFromBuffer(
      llvm::formatv("{0}", llvm::json::Value(std::move(entries))).str(),
      error, clang::tooling::JSONCommandLineSyntax::AutoDetect);
  REQUIRE(compDb);

  AnalysisOptions opts;
  opts.threadCount = 1;
  auto diags = runAnalysis(*compDb,
                           {fx.path("user_a.cpp"), fx.path("user_b.cpp")},
                           opts);
  llvm::sys::fs::remove(buildDir);
  REQUIRE(diags.size() == 1);
  CHECK(diags[0].callLocation == fx.path("user_a.cpp") + ":5:3");
  CHECK(diags[0].missingHeader == fx.path("ext.hpp"));
}

TEST_CASE("An attempt record carries the outcome that used it up",
          "[AnnealCheckpoint]") {
  CheckpointFileGuard ckpt("anneal_ckpt_attempt_outcome.vycj");
  ScratchFixture fx;
  auto compDb = fx.db();
  auto files = fx.files();

  AnalysisOptions opts;
  opts.threadCount = 1;
  opts.checkpointPath = ckpt.path;

  // What the isolated dispatcher journals for a TU whose worker timed out.
  auto stamps = SnapshotIO::stampFiles({files[2]});
  REQUIRE(stamps.size() == 1);
  const TuOutcome timedOut{TuStatus::TimedOut, "worker timed out"};
  {
    auto journal =
        AnnealCheckpoint::open(ckpt.path, annealOptionsFingerprint(opts));
    REQUIRE(journal);
    for (unsigned i = 0; i < AnnealCheckpoint::kMaxAttempts; ++i)
      journal->recordAttempt(AnnealCheckpoint::kPhaseIndex, files[2],
                             stamps[0], timedOut);
  }
  {
    auto journal =
        AnnealCheckpoint::open(ckpt.path, annealOptionsFingerprint(opts));
    REQUIRE(journal);
    std::optional<TuOutcome> why;
    CHECK(journal->attempts(AnnealCheckpoint::kPhaseIndex, files[2],
                            stamps[0], &why) == AnnealCheckpoint::kMaxAttempts);
    REQUIRE(why);
    CHECK(*why == timedOut);
  }

  AnalysisReport report;
  runAnalysis(compDb, files, opts, nullptr, &report);
  CHECK(outcomeRows(report) ==
        std::vector<std::string>{"user_a.cpp|indexed|", "user_b.cpp|indexed|",
                                 "user_c.cpp|timeout|worker timed out"});
}
