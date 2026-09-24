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

// test_write_integrity.cpp — index and checkpoint-journal writes cannot be
// torn, mixed, or emptied, and a damaged file is refused on load. Each
// failure mode is reproduced as it happened before the fix: concurrent
// saves sharing one temp file, a write error on a full disk, a corrupt
// journal record swallowing every later append, and a record length that
// wrapped the bounds check.

#include "vycor/anneal/Checkpoint.h"
#include "vycor/callgraph/CallGraph.h"
#include "vycor/callgraph/ControlFlowIndex.h"
#include "vycor/callgraph/AtomicFile.h"
#include "vycor/callgraph/Snapshot.h"
#include "vycor/callgraph/WorkerPool.h"

#include "SnapshotBytes.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <csignal>
#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

using namespace vycor;

namespace {

/// A fresh directory per test, removed on destruction, so temp-file
/// leftovers can be counted exactly.
struct ScratchDir {
  std::string path;
  ScratchDir() {
    llvm::SmallString<128> p;
    REQUIRE(!llvm::sys::fs::createUniqueDirectory("vycor-write-integrity",
                                                  p));
    path = std::string(p.str());
  }
  ~ScratchDir() { llvm::sys::fs::remove_directories(path); }
  std::string file(const char *name) const {
    llvm::SmallString<128> p(path);
    llvm::sys::path::append(p, name);
    return std::string(p.str());
  }
  std::vector<std::string> entries() const {
    std::vector<std::string> out;
    std::error_code ec;
    for (llvm::sys::fs::directory_iterator it(path, ec), end;
         !ec && it != end; it.increment(ec))
      out.push_back(std::string(llvm::sys::path::filename(it->path())));
    std::sort(out.begin(), out.end());
    return out;
  }
};

std::string readBytes(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

void writeBytes(const std::string &path, const std::string &bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << bytes;
}

/// A chain of `n` functions, each calling the next, with one call-site
/// context per call: enough bytes that a save takes a measurable time.
struct Indexes {
  CallGraph graph;
  ControlFlowIndex cf;
};

Indexes chainOf(int n, const std::string &tag) {
  Indexes ix;
  const std::string tu = "/src/" + tag + ".cpp";
  for (int i = 0; i < n; ++i)
    ix.graph.addNode({tag + "_fn" + std::to_string(i), tu,
                      static_cast<unsigned>(i + 1), false, false, ""},
                     tu);
  for (int i = 0; i + 1 < n; ++i) {
    std::string site = tu + ":" + std::to_string(i + 1) + ":3";
    ix.graph.addEdge({tag + "_fn" + std::to_string(i),
                      tag + "_fn" + std::to_string(i + 1),
                      EdgeKind::DirectCall, Confidence::Proven, site, 0,
                      ExecutionContext::Synchronous},
                     tu);
    CallSiteContext ctx;
    ctx.callerName = tag + "_fn" + std::to_string(i);
    ctx.calleeName = tag + "_fn" + std::to_string(i + 1);
    ctx.callSite = site;
    ctx.tuPath = tu;
    ix.cf.addCallSiteContext(std::move(ctx));
  }
  return ix;
}

SnapshotMeta metaFor(const std::string &tag) {
  SnapshotMeta meta;
  FileStamp s;
  s.path = "/src/" + tag + ".cpp";
  s.mtimeNs = 1;
  s.size = 1;
  meta.files.push_back(s);
  return meta;
}

bool saveChain(const std::string &path, const Indexes &ix,
               const std::string &tag) {
  return SnapshotIO::save(path, ix.graph, ix.cf, metaFor(tag));
}

} // namespace

// ---------------------------------------------------------------------------
// Index saves
// ---------------------------------------------------------------------------

TEST_CASE("A writer mid-write cannot tear the index another writer "
          "published",
          "[write-integrity]") {
  ScratchDir dir;
  const std::string path = dir.file("index.vycs");
  const std::string ref = dir.file("reference.vycs");
  Indexes a = chainOf(200, "a");
  REQUIRE(saveChain(ref, a, "a"));
  const std::string expected = readBytes(ref);

  // Writer B has opened its temp file and is still writing when writer A
  // saves. Before the fix every writer used `<index>.tmp`: A truncated
  // and renamed B's open file, so B's remaining bytes landed inside the
  // index A had just published.
  const std::string sharedTemp = path + ".tmp";
  std::ofstream writerB(sharedTemp, std::ios::binary);
  writerB << std::string(64, 'B');
  writerB.flush();

  REQUIRE(saveChain(path, a, "a"));

  writerB.seekp(static_cast<std::streamoff>(expected.size() / 2));
  writerB << std::string(64, 'B');
  writerB.close();

  CHECK(readBytes(path) == expected);
  auto loaded = SnapshotIO::load(path);
  REQUIRE(loaded);
  CHECK(loaded->graph.nodeCount() == 200);
}

TEST_CASE("Concurrent saves to one index publish one whole index each",
          "[write-integrity]") {
  ScratchDir dir;
  const std::string path = dir.file("index.vycs");
  Indexes a = chainOf(6000, "a");
  Indexes b = chainOf(9000, "b");
  REQUIRE(saveChain(path, a, "a"));

  constexpr int kRounds = 10;
  std::atomic<int> failedSaves{0};
  std::atomic<bool> done{false};
  auto writer = [&](const Indexes &ix, const char *tag) {
    for (int i = 0; i < kRounds; ++i)
      if (!saveChain(path, ix, tag))
        ++failedSaves;
  };
  // A reader loading throughout must only ever see A or B, whole.
  std::atomic<int> badLoads{0}, loads{0};
  std::thread reader([&] {
    while (!done) {
      auto snap = SnapshotIO::load(path, nullptr, LoadMode::ReadOnly);
      ++loads;
      if (!snap || (snap->graph.nodeCount() != 6000 &&
                    snap->graph.nodeCount() != 9000))
        ++badLoads;
    }
  });
  std::thread ta(writer, std::cref(a), "a");
  std::thread tb(writer, std::cref(b), "b");
  ta.join();
  tb.join();
  done = true;
  reader.join();

  CHECK(failedSaves == 0);
  CHECK(badLoads == 0);
  CHECK(loads > 0);
  auto last = SnapshotIO::load(path);
  REQUIRE(last);
  const size_t nodes = last->graph.nodeCount();
  CHECK((nodes == 6000 || nodes == 9000));
  // Nothing left behind but the index itself.
  CHECK(dir.entries() == std::vector<std::string>{"index.vycs"});
}

#ifndef _WIN32
TEST_CASE("A full disk fails the save and keeps the previous index",
          "[write-integrity]") {
  ScratchDir dir;
  const std::string path = dir.file("index.vycs");
  Indexes small = chainOf(3, "small");
  REQUIRE(saveChain(path, small, "small"));
  const std::string before = readBytes(path);

  // RLIMIT_FSIZE makes every write past the limit fail with EFBIG (with
  // SIGXFSZ ignored): the same stream error a full disk produces.
  struct rlimit old;
  REQUIRE(getrlimit(RLIMIT_FSIZE, &old) == 0);
  auto oldHandler = std::signal(SIGXFSZ, SIG_IGN);
  struct rlimit capped = old;
  capped.rlim_cur = 4096;
  REQUIRE(setrlimit(RLIMIT_FSIZE, &capped) == 0);

  Indexes big = chainOf(2000, "big");
  const bool saved = saveChain(path, big, "big");

  REQUIRE(setrlimit(RLIMIT_FSIZE, &old) == 0);
  std::signal(SIGXFSZ, oldHandler);

  CHECK_FALSE(saved);
  CHECK(readBytes(path) == before);
  CHECK(dir.entries() == std::vector<std::string>{"index.vycs"});
}
#endif

TEST_CASE("A damaged index is refused on load", "[write-integrity]") {
  ScratchDir dir;
  const std::string path = dir.file("index.vycs");
  Indexes a = chainOf(40, "a");
  REQUIRE(saveChain(path, a, "a"));
  const std::string original = readBytes(path);

  SECTION("one flipped byte anywhere") {
    // Before the fix most flips inside a string decoded cleanly into a
    // different, wrong index.
    // A few hundred positions spread over the whole file (a stride
    // prime to the record sizes), so the header and every section are
    // hit.
    const size_t stride = std::max<size_t>(1, original.size() / 397) | 1;
    for (size_t at = 0; at < original.size(); at += stride) {
      std::string bytes = original;
      bytes[at] = static_cast<char>(bytes[at] ^ 0x20);
      writeBytes(path, bytes);
      INFO("flipped byte " << at << " of " << original.size());
      REQUIRE_FALSE(SnapshotIO::load(path, nullptr, LoadMode::Mutable));
      REQUIRE_FALSE(SnapshotIO::load(path, nullptr, LoadMode::ReadOnly));
    }
  }

  SECTION("the error names the damaged section") {
    const char *names[] = {"meta", "graph", "control_flow", "channels"};
    for (size_t i = 0; i < 4; ++i) {
      const size_t at = 44 + i * 25;
      const uint8_t kind = static_cast<uint8_t>(original[at]);
      REQUIRE(kind < 4);
      const uint64_t offset = testing::snapshotU64At(original, at + 1);
      const uint64_t length = testing::snapshotU64At(original, at + 9);
      if (length == 0)
        continue;
      std::string bytes = original;
      bytes[offset + length / 2] ^= 0x01;
      writeBytes(path, bytes);
      const std::string expected =
          std::string("section '") + names[kind] + "' checksum mismatch";
      INFO("damaged " << names[kind]);
      SnapshotLoadStats stats;
      CHECK_FALSE(SnapshotIO::load(path, &stats, LoadMode::ReadOnly));
      CHECK(stats.error.find(expected) != std::string::npos);
      std::string why;
      CHECK_FALSE(SnapshotIO::verify(path, &why));
      CHECK(why.find(expected) != std::string::npos);
      // A load that leaves the damaged section undecoded does not check
      // it (meta is always decoded).
      if (kind != 0)
        CHECK(SnapshotIO::load(path, nullptr, LoadMode::ReadOnly, 0));
    }
    std::string bytes = original;
    bytes[8] ^= 0x01; // the summary: covered by the header checksum
    writeBytes(path, bytes);
    SnapshotLoadStats stats;
    CHECK_FALSE(SnapshotIO::load(path, &stats, LoadMode::ReadOnly, 0));
    CHECK(stats.error == "header checksum mismatch");
    writeBytes(path, original);
    CHECK(SnapshotIO::verify(path));
  }

  SECTION("truncated at any length") {
    for (size_t len = 0; len < original.size(); len += 7) {
      writeBytes(path, original.substr(0, len));
      INFO("truncated to " << len);
      CHECK_FALSE(SnapshotIO::load(path, nullptr, LoadMode::ReadOnly, 0));
    }
  }
}

TEST_CASE("The index write lock admits one writer at a time",
          "[write-integrity]") {
  ScratchDir dir;
  const std::string path = dir.file("index.vycs");
  std::string error;
  auto first = IndexWriteLock::acquire(path, /*wait=*/false, &error);
  REQUIRE(first);
  CHECK(first->lockPath() == path + ".lock");

  bool busy = false;
  auto second =
      IndexWriteLock::acquire(path, /*wait=*/false, &error, &busy);
  CHECK_FALSE(second);
  CHECK(busy);
  CHECK(error.find(path + ".lock") != std::string::npos);

  // A waiting writer is told, and gets the lock once it is released.
  std::atomic<bool> waited{false}, acquired{false};
  std::thread waiter([&] {
    std::string e;
    auto lock =
        IndexWriteLock::acquire(path, /*wait=*/true, &e, nullptr,
                                [&](const std::string &) { waited = true; });
    acquired = lock != nullptr;
  });
  while (!waited)
    std::this_thread::yield();
  CHECK_FALSE(acquired);
  first.reset();
  waiter.join();
  CHECK(acquired);
}

TEST_CASE("Stale temp files of killed writers are recognized",
          "[write-integrity]") {
  ScratchDir dir;
  const std::string path = dir.file("index.vycs");
  writeBytes(path, "index");
  writeBytes(path + ".tmp-a1b2c3", "torn");
  writeBytes(path + ".tmp-x", "not ours");
  writeBytes(dir.file("other.vycs.tmp-a1b2c3"), "another index's");
  CHECK(removeStaleAtomicTemps(path) == 1);
  CHECK(dir.entries() ==
        std::vector<std::string>{"index.vycs", "index.vycs.tmp-x",
                                 "other.vycs.tmp-a1b2c3"});
}

TEST_CASE("A bake that parsed nothing is not publishable",
          "[write-integrity]") {
  const std::vector<std::string> tus{"/src/a.cpp", "/src/b.cpp"};
  TuOutcomes none;
  CHECK_FALSE(SnapshotIO::unpublishableBake(tus, none).empty());
  TuOutcomes skipped{{"/src/a.cpp", {TuStatus::Skipped, "why"}},
                     {"/src/b.cpp", {TuStatus::Skipped, "why"}}};
  CHECK(SnapshotIO::unpublishableBake(tus, skipped).find("why") !=
        std::string::npos);
  // A TU that was parsed, however it ended, is a real result.
  TuOutcomes crashed = skipped;
  crashed["/src/b.cpp"] = {TuStatus::Crashed, "signal 11"};
  CHECK(SnapshotIO::unpublishableBake(tus, crashed).empty());
  TuOutcomes indexed{{"/src/a.cpp", {TuStatus::Indexed, ""}}};
  CHECK(SnapshotIO::unpublishableBake(tus, indexed).empty());
  CHECK(SnapshotIO::unpublishableBake({}, none).empty());
}

#ifndef _WIN32
TEST_CASE("An isolated bake without a shard directory is not an empty "
          "success",
          "[write-integrity]") {
  ScratchDir dir;
  const char *old = std::getenv("TMPDIR");
  const std::string saved = old ? old : "";
  // The shard directory goes under the system temp dir: point it nowhere.
  ::setenv("TMPDIR", dir.file("no-such-dir").c_str(), 1);
  const std::vector<std::string> files{"/src/a.cpp", "/src/b.cpp"};
  BakedIndexes baked = bakeIsolated("/no/such/exe", McpBakeConfig{}, files,
                                    2, nullptr);
  if (old)
    ::setenv("TMPDIR", saved.c_str(), 1);
  else
    ::unsetenv("TMPDIR");

  CHECK(baked.graph.nodeCount() == 0);
  REQUIRE(baked.outcomes.size() == 2);
  for (const auto &f : files)
    CHECK(baked.outcomes[f].status == TuStatus::Skipped);
  CHECK_FALSE(SnapshotIO::unpublishableBake(files, baked.outcomes).empty());
}
#endif

// ---------------------------------------------------------------------------
// Checkpoint journal
// ---------------------------------------------------------------------------

namespace {

FileStamp stampOf(const std::string &tu, uint64_t mtime) {
  FileStamp s;
  s.path = tu;
  s.mtimeNs = mtime;
  s.size = 10;
  return s;
}

void appendRaw(const std::string &path, const std::string &bytes) {
  std::ofstream f(path, std::ios::binary | std::ios::app);
  f << bytes;
}

std::string u32le(uint32_t v) {
  std::string s(4, '\0');
  for (int i = 0; i < 4; ++i)
    s[i] = static_cast<char>((v >> (8 * i)) & 0xFF);
  return s;
}

} // namespace

TEST_CASE("A corrupt journal record does not swallow later progress",
          "[write-integrity][AnnealCheckpoint]") {
  ScratchDir dir;
  const std::string path = dir.file("anneal.vycj");
  const uint64_t fp = 0x1234;
  const FileStamp a = stampOf("/src/a.cpp", 1);
  const FileStamp b = stampOf("/src/b.cpp", 2);

  {
    auto ckpt = AnnealCheckpoint::open(path, fp);
    REQUIRE(ckpt);
    ckpt->recordAttempt(AnnealCheckpoint::kPhaseIndex, a.path, a);
  }
  // A torn tail: what a kill mid-append (or a bad sector) leaves.
  appendRaw(path, "\x02garbage-not-a-record");

  // Resume 1 keeps what precedes the damage and appends new progress: a
  // second attempt at a (it died again) and b's completed phase 1.
  {
    auto ckpt = AnnealCheckpoint::open(path, fp);
    REQUIRE(ckpt);
    CHECK(ckpt->attempts(AnnealCheckpoint::kPhaseIndex, a.path, a) == 1);
    ckpt->recordAttempt(AnnealCheckpoint::kPhaseIndex, a.path, a);
    ckpt->recordPhase1(b.path, b, AnnealIndexPayload{});
  }

  // Resume 2 must see resume 1's appends. Before the fix they landed
  // after the garbage, where every later load stopped: b re-ran forever
  // and a was never recognized as poisoned.
  auto ckpt = AnnealCheckpoint::open(path, fp);
  REQUIRE(ckpt);
  CHECK(ckpt->attempts(AnnealCheckpoint::kPhaseIndex, a.path, a) == 2);
  GlobalIndex into;
  CHECK(ckpt->replayPhase1(b.path, b, into));
}

TEST_CASE("A journal record length near 4 GiB cannot read past the file",
          "[write-integrity][AnnealCheckpoint]") {
  ScratchDir dir;
  const std::string path = dir.file("anneal.vycj");
  const uint64_t fp = 0x1234;
  const FileStamp a = stampOf("/src/a.cpp", 1);
  {
    auto ckpt = AnnealCheckpoint::open(path, fp);
    REQUIRE(ckpt);
    ckpt->recordPhase1(a.path, a, AnnealIndexPayload{});
  }
  // kind, then a length whose `len + 4` wraps to 2 in uint32_t: the old
  // bounds check passed and `pos += len` walked 4 GiB past the buffer.
  appendRaw(path, std::string("\x02", 1) + u32le(0xFFFFFFFEu) + "xx");

  auto ckpt = AnnealCheckpoint::open(path, fp);
  REQUIRE(ckpt);
  GlobalIndex into;
  CHECK(ckpt->replayPhase1(a.path, a, into));
}

#ifndef _WIN32
TEST_CASE("A journal append on a full disk stops journaling, not the run",
          "[write-integrity][AnnealCheckpoint]") {
  ScratchDir dir;
  const std::string path = dir.file("anneal.vycj");
  const uint64_t fp = 0x1234;
  const FileStamp a = stampOf("/src/a.cpp", 1);
  uint64_t sizeBefore = 0;
  {
    auto ckpt = AnnealCheckpoint::open(path, fp);
    REQUIRE(ckpt);
    ckpt->recordPhase1(a.path, a, AnnealIndexPayload{});
    REQUIRE(!llvm::sys::fs::file_size(path, sizeBefore));

    struct rlimit old;
    REQUIRE(getrlimit(RLIMIT_FSIZE, &old) == 0);
    auto oldHandler = std::signal(SIGXFSZ, SIG_IGN);
    struct rlimit capped = old;
    capped.rlim_cur = sizeBefore + 16;
    REQUIRE(setrlimit(RLIMIT_FSIZE, &capped) == 0);
    // Too big for the space left: the append fails part way (before the
    // fix the stream error aborted the process at destruction).
    ckpt->recordAttempt(AnnealCheckpoint::kPhaseIndex,
                        std::string(4096, 'x'), a);
    ckpt->recordAttempt(AnnealCheckpoint::kPhaseIndex, a.path, a);
    REQUIRE(setrlimit(RLIMIT_FSIZE, &old) == 0);
    std::signal(SIGXFSZ, oldHandler);
  }
  // The torn append is truncated away on resume; what preceded it stays.
  auto ckpt = AnnealCheckpoint::open(path, fp);
  REQUIRE(ckpt);
  GlobalIndex into;
  CHECK(ckpt->replayPhase1(a.path, a, into));
  uint64_t sizeAfter = 0;
  REQUIRE(!llvm::sys::fs::file_size(path, sizeAfter));
  CHECK(sizeAfter == sizeBefore);
}
#endif

TEST_CASE("A shard entry length near 4 GiB is refused",
          "[write-integrity][AnnealCheckpoint]") {
  ScratchDir dir;
  const std::string path = dir.file("diag.shard");
  REQUIRE(writeAnnealDiagShard(path, {{"/src/a.cpp", {}}}));
  std::string bytes = readBytes(path);
  // magic(4) version(4) count(4) tu(len 4 + 10 bytes) payloadLen(4)...
  const size_t lenAt = 12 + 4 + std::strlen("/src/a.cpp");
  REQUIRE(bytes.size() > lenAt + 4);
  bytes.replace(lenAt, 4, u32le(0xFFFFFFFEu));
  writeBytes(path, bytes);
  bool called = false;
  CHECK_FALSE(readAnnealDiagShard(
      path, [&](const std::string &, std::vector<Diagnostic>) {
        called = true;
      }));
  CHECK_FALSE(called);
}
