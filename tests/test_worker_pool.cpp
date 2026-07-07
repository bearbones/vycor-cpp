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

// test_worker_pool.cpp — F12 subprocess worker isolation:
//   (a) CallGraph/ControlFlowIndex::absorb merge semantics (disjoint absorb
//       equals a single combined build; identical edges dedup across shards
//       with accumulating refs; removeTU provenance survives the merge),
//   (b) the dispatcher's crash/bisect bookkeeping through the fake-runner
//       seam (clean batch, crash-with-marker, markerless split, single-TU
//       poison, retry bound).

#include "vycor/callgraph/CallGraph.h"
#include "vycor/callgraph/ControlFlowIndex.h"
#include "vycor/callgraph/Snapshot.h"
#include "vycor/callgraph/WorkerPool.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

#include <catch2/catch_test_macros.hpp>
#include <clang/Tooling/CompilationDatabase.h>

#include <algorithm>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

using namespace vycor;

namespace {

// ---------------------------------------------------------------------------
// Canonicalization helpers: order-independent comparison of two indexes.
// ---------------------------------------------------------------------------

std::vector<std::string> nodeNames(const CallGraph &g) {
  std::vector<std::string> v;
  for (const auto *n : g.allNodes())
    v.push_back(n->qualifiedName);
  std::sort(v.begin(), v.end());
  return v;
}

std::vector<std::string> canonCallees(const CallGraph &g,
                                      const std::string &name) {
  std::vector<std::string> v;
  for (const auto &e : g.calleesOf(name))
    v.push_back(e.callerName + "|" + e.calleeName + "|" +
                std::to_string(static_cast<int>(e.kind)) + "|" +
                std::to_string(static_cast<int>(e.confidence)) + "|" +
                e.callSite + "|" + std::to_string(e.indirectionDepth) + "|" +
                std::to_string(static_cast<int>(e.execContext)));
  std::sort(v.begin(), v.end());
  return v;
}

std::vector<std::string> canonContexts(const ControlFlowIndex &idx) {
  std::vector<std::string> v;
  for (const auto &c : idx.allContexts())
    v.push_back(c.callerName + "|" + c.calleeName + "|" + c.callSite + "|" +
                c.tuPath + "|" +
                std::to_string(static_cast<int>(c.callerNoexcept)) + "|" +
                (c.insideCatchBlock ? "1" : "0") + "|" +
                std::to_string(c.enclosingTryCatches.size()) + "|" +
                std::to_string(c.enclosingGuards.size()) + "|" +
                std::to_string(c.liveRaiiLocals.size()));
  std::sort(v.begin(), v.end());
  return v;
}

void checkGraphsEqual(const CallGraph &a, const CallGraph &b) {
  CHECK(a.nodeCount() == b.nodeCount());
  CHECK(a.edgeCount() == b.edgeCount());
  auto names = nodeNames(a);
  REQUIRE(names == nodeNames(b));
  for (const auto &n : names) {
    INFO("calleesOf(" << n << ")");
    CHECK(canonCallees(a, n) == canonCallees(b, n));
  }
}

// ---------------------------------------------------------------------------
// Synthetic shard builders.
// ---------------------------------------------------------------------------

CallSiteContext makeContext(const std::string &caller,
                            const std::string &callee,
                            const std::string &site, const std::string &tu,
                            bool withScope = false) {
  CallSiteContext ctx;
  ctx.callerName = caller;
  ctx.calleeName = callee;
  ctx.callSite = site;
  ctx.tuPath = tu;
  if (withScope) {
    TryCatchScope scope;
    scope.tryLocation = site + ":try";
    scope.enclosingFunction = caller;
    CatchHandlerInfo handler;
    handler.caughtType = "std::exception";
    handler.location = site + ":catch";
    scope.handlers.push_back(handler);
    ctx.enclosingTryCatches.push_back(scope);
    RaiiLocal lock;
    lock.typeName = "std::lock_guard<std::mutex>";
    lock.varName = "g";
    lock.declLocation = site + ":lock";
    lock.kind = RaiiKind::Lock;
    ctx.liveRaiiLocals.push_back(lock);
  }
  return ctx;
}

// ---------------------------------------------------------------------------
// deep_chains fixture bake (same fixture as test_deep_chains.cpp).
// ---------------------------------------------------------------------------

std::vector<std::string> deepChainsFiles(const std::string &base) {
  return {
      base + "main.cpp",           base + "pipeline.cpp",
      base + "stage1_ingest.cpp",  base + "stage2_parse.cpp",
      base + "stage3_transform.cpp", base + "stage4_dispatch.cpp",
      base + "stage5_sink.cpp",    base + "plugins.cpp",
      base + "workers.cpp",        base + "tokenizer.cpp",
      base + "scheduler.cpp",      base + "callbacks.cpp",
      base + "async_workers.cpp",  base + "lambda_callbacks.cpp",
  };
}

BakedIndexes bakeFixture(const std::vector<std::string> &files) {
  const std::string base =
      std::string(PROJECT_SOURCE_DIR) + "/examples/deep_chains/";
  clang::tooling::FixedCompilationDatabase compDb(".",
                                                  {"-std=c++17", "-I" + base});
  // Serial bake: keeps node-field backfill order deterministic so absorbed
  // and combined builds are comparable field-for-field.
  return bakeIndexes(compDb, files, {}, /*threadCount=*/1);
}

// ---------------------------------------------------------------------------
// Dispatcher-seam helpers.
// ---------------------------------------------------------------------------

std::string makeShardDir() {
  llvm::SmallString<128> base;
  llvm::sys::path::system_temp_directory(/*ErasedOnReboot=*/true, base);
  llvm::sys::path::append(base, "vycor-worker-test");
  llvm::SmallString<128> dir;
  REQUIRE(!llvm::sys::fs::createUniqueDirectory(base, dir));
  return std::string(dir);
}

/// Write a plausible shard for `batch`: one function node per TU, an edge
/// from it to a shared "common" callee (dedup fodder across shards), and
/// one call-site context per TU.
void writeShard(const std::string &shardPath,
                const std::vector<std::string> &batch) {
  CallGraph g;
  ControlFlowIndex cf;
  for (const auto &tu : batch) {
    std::string fn = "fn@" + tu;
    g.addNode({fn, tu, 1, false, false, ""}, tu);
    g.addNode({"common", "/tu/common.h", 1, false, false, ""}, tu);
    g.addEdge({fn, "common", EdgeKind::DirectCall, Confidence::Proven,
               tu + ":2:3", 0, ExecutionContext::Synchronous},
              tu);
    cf.addCallSiteContext(makeContext(fn, "common", tu + ":2:3", tu));
  }
  SnapshotMeta meta;
  REQUIRE(SnapshotIO::save(shardPath, g, cf, meta));
}

void writeMarkers(const std::string &stderrPath,
                  const std::vector<std::string> &tus) {
  std::ofstream out(stderrPath);
  for (const auto &tu : tus)
    out << "WORKER-TU " << tu << "\n";
}

struct RunnerLog {
  std::mutex mu;
  std::vector<std::vector<std::string>> batches;
  void record(const std::vector<std::string> &batch) {
    std::lock_guard<std::mutex> lock(mu);
    batches.push_back(batch);
  }
  size_t calls() {
    std::lock_guard<std::mutex> lock(mu);
    return batches.size();
  }
  size_t dispatchesOf(const std::string &tu) {
    std::lock_guard<std::mutex> lock(mu);
    size_t n = 0;
    for (const auto &b : batches)
      n += std::count(b.begin(), b.end(), tu);
    return n;
  }
};

std::vector<std::string> poisonedFiles(const BuildStats &stats) {
  std::vector<std::string> v;
  for (const auto &t : stats.tuStats)
    if (t.toolStatus == -1)
      v.push_back(t.file);
  std::sort(v.begin(), v.end());
  return v;
}

} // namespace

// ============================================================================
// absorb: synthetic graphs
// ============================================================================

TEST_CASE("CallGraph::absorb merges disjoint shards to the combined result",
          "[worker_pool][absorb]") {
  auto addShardA = [](CallGraph &g) {
    g.addNode({"main", "/src/a.cpp", 10, true, false, ""}, "/src/a.cpp");
    g.addNode({"helper", "/src/a.cpp", 3, false, false, ""}, "/src/a.cpp");
    g.addEdge({"main", "helper", EdgeKind::DirectCall, Confidence::Proven,
               "/src/a.cpp:11:3", 0, ExecutionContext::Synchronous},
              "/src/a.cpp");
    g.addFunctionReturn("factory", "helper");
  };
  auto addShardB = [](CallGraph &g) {
    g.addNode({"Base::run", "/src/b.cpp", 5, false, true, "Base"},
              "/src/b.cpp");
    g.addNode({"Derived::run", "/src/b.cpp", 15, false, true, "Derived"},
              "/src/b.cpp");
    g.addEdge({"main", "Base::run", EdgeKind::VirtualDispatch,
               Confidence::Plausible, "/src/b.cpp:12:3", 0,
               ExecutionContext::Synchronous},
              "/src/b.cpp");
    g.addDerivedClass("Base", "Derived");
    g.addMethodOverride("Base::run", "Derived::run");
    g.addEffectiveImpl("Derived", "Derived::run");
  };

  CallGraph combined;
  addShardA(combined);
  addShardB(combined);

  CallGraph a, b;
  addShardA(a);
  addShardB(b);
  a.absorb(b);

  checkGraphsEqual(a, combined);
  // Virtual-dispatch expansion works through the merged override relation.
  auto callees = a.calleesOf("main");
  bool sawExpanded = false;
  for (const auto &e : callees)
    if (e.calleeName == "Derived::run")
      sawExpanded = true;
  CHECK(sawExpanded);
  CHECK(a.getOverrides("Base::run") == combined.getOverrides("Base::run"));
  CHECK(a.getFunctionReturns("factory") ==
        combined.getFunctionReturns("factory"));
  CHECK(a.getAllDerivedClasses("Base") ==
        combined.getAllDerivedClasses("Base"));

  // Node backfill union: main's entry-point flag and Base::run's class
  // survive the merge.
  REQUIRE(a.findNode("main") != nullptr);
  CHECK(a.findNode("main")->isEntryPoint);
  REQUIRE(a.findNode("Base::run") != nullptr);
  CHECK(a.findNode("Base::run")->enclosingClass == "Base");
}

TEST_CASE("CallGraph::absorb dedups identical cross-shard edges by refs",
          "[worker_pool][absorb]") {
  // The same header-inlined edge registered by two TUs, one per shard.
  CallGraphEdge inlineEdge{"inlineFn", "target", EdgeKind::DirectCall,
                           Confidence::Proven, "/src/common.h:4:5", 0,
                           ExecutionContext::Synchronous};
  CallGraph a;
  a.addNode({"inlineFn", "/src/common.h", 2, false, false, ""}, "/src/a.cpp");
  a.addNode({"target", "/src/common.h", 8, false, false, ""}, "/src/a.cpp");
  a.addEdge(inlineEdge, "/src/a.cpp");

  CallGraph b;
  b.addNode({"inlineFn", "/src/common.h", 2, false, false, ""}, "/src/b.cpp");
  b.addNode({"target", "/src/common.h", 8, false, false, ""}, "/src/b.cpp");
  b.addEdge(inlineEdge, "/src/b.cpp");

  a.absorb(b);
  CHECK(a.nodeCount() == 2);
  CHECK(a.edgeCount() == 1); // deduped, not duplicated

  // Provenance: the edge survives losing one contributor, dies with both —
  // exactly as if both TUs had been baked in-process.
  CHECK(a.removeTU("/src/a.cpp") == 0);
  CHECK(a.edgeCount() == 1);
  CHECK(a.nodeCount() == 2); // nodes still contributed by /src/b.cpp
  CHECK(a.removeTU("/src/b.cpp") == 1);
  CHECK(a.edgeCount() == 0);
  CHECK(a.nodeCount() == 0);
}

TEST_CASE("ControlFlowIndex::absorb merges contexts and dedups set tables",
          "[worker_pool][absorb]") {
  ControlFlowIndex combined, a, b;
  // Identical scope/RAII sets in both shards (header-inlined call site
  // pattern) must land in one table entry after the merge.
  for (ControlFlowIndex *idx : {&combined, &a})
    idx->addCallSiteContext(
        makeContext("main", "helper", "/src/a.cpp:11:3", "/src/a.cpp",
                    /*withScope=*/true));
  for (ControlFlowIndex *idx : {&combined, &b}) {
    idx->addCallSiteContext(
        makeContext("worker", "helper", "/src/b.cpp:7:3", "/src/b.cpp",
                    /*withScope=*/true));
    idx->addCallSiteContext(
        makeContext("worker", "logger", "/src/b.cpp:9:3", "/src/b.cpp"));
  }

  a.absorb(b);
  CHECK(a.size() == combined.size());
  CHECK(canonContexts(a) == canonContexts(combined));
  CHECK(a.protectedCallsTo("helper").size() ==
        combined.protectedCallsTo("helper").size());
  CHECK(a.unprotectedCallsTo("logger").size() ==
        combined.unprotectedCallsTo("logger").size());
  auto at = a.contextAtSite("/src/b.cpp:7:3");
  REQUIRE(at.has_value());
  CHECK(at->callerName == "worker");
  REQUIRE(at->enclosingTryCatches.size() == 1);
  CHECK(at->enclosingTryCatches[0].handlers.size() == 1);
  REQUIRE(at->liveRaiiLocals.size() == 1);
  CHECK(at->liveRaiiLocals[0].typeName == "std::lock_guard<std::mutex>");

  // removeTU provenance survives the merge.
  CHECK(a.removeTU("/src/b.cpp") == 2);
  CHECK(combined.removeTU("/src/b.cpp") == 2);
  CHECK(canonContexts(a) == canonContexts(combined));
}

// ============================================================================
// absorb: real bakes over the deep_chains fixture
// ============================================================================

TEST_CASE("disjoint deep_chains shards absorb to the single combined build",
          "[worker_pool][absorb][deep_chains]") {
  const std::string base =
      std::string(PROJECT_SOURCE_DIR) + "/examples/deep_chains/";
  auto files = deepChainsFiles(base);
  auto combined = bakeFixture(files);

  std::vector<std::string> half1(files.begin(), files.begin() + 7);
  std::vector<std::string> half2(files.begin() + 7, files.end());
  auto shard1 = bakeFixture(half1);
  auto shard2 = bakeFixture(half2);

  shard1.graph.absorb(shard2.graph);
  shard1.cfIndex.absorb(shard2.cfIndex);

  checkGraphsEqual(shard1.graph, combined.graph);
  CHECK(shard1.cfIndex.size() == combined.cfIndex.size());
  CHECK(canonContexts(shard1.cfIndex) == canonContexts(combined.cfIndex));

  // removeTU after absorb behaves identically to the combined build
  // (provenance carried through the merge).
  const std::string victim = base + "stage3_transform.cpp";
  size_t removedAbsorbed = shard1.graph.removeTU(victim);
  size_t removedCombined = combined.graph.removeTU(victim);
  CHECK(removedAbsorbed == removedCombined);
  CHECK(removedAbsorbed > 0);
  CHECK(shard1.cfIndex.removeTU(victim) == combined.cfIndex.removeTU(victim));
  checkGraphsEqual(shard1.graph, combined.graph);
  CHECK(canonContexts(shard1.cfIndex) == canonContexts(combined.cfIndex));
}

TEST_CASE("overlapping deep_chains shards dedup edges; removeTU of the "
          "shared TU restores combined-build equality",
          "[worker_pool][absorb][deep_chains]") {
  const std::string base =
      std::string(PROJECT_SOURCE_DIR) + "/examples/deep_chains/";
  auto files = deepChainsFiles(base);
  auto combined = bakeFixture(files);

  const std::string shared = files[6]; // in both shards
  std::vector<std::string> half1(files.begin(), files.begin() + 7);
  std::vector<std::string> half2(files.begin() + 6, files.end());
  auto shard1 = bakeFixture(half1);
  auto shard2 = bakeFixture(half2);

  shard1.graph.absorb(shard2.graph);
  shard1.cfIndex.absorb(shard2.cfIndex);

  // Unique node/edge sets match the combined build even though the shared
  // TU was baked twice: the dedup probe merged its edges (refs 2), and
  // node contributor sets absorbed idempotently.
  checkGraphsEqual(shard1.graph, combined.graph);

  // Dropping the doubly-baked TU removes both registrations at once; from
  // here the whole state (graph AND contexts) matches the combined build
  // minus that TU.
  shard1.graph.removeTU(shared);
  combined.graph.removeTU(shared);
  shard1.cfIndex.removeTU(shared);
  combined.cfIndex.removeTU(shared);
  checkGraphsEqual(shard1.graph, combined.graph);
  CHECK(shard1.cfIndex.size() == combined.cfIndex.size());
  CHECK(canonContexts(shard1.cfIndex) == canonContexts(combined.cfIndex));
}

// ============================================================================
// Dispatcher bookkeeping through the fake-runner seam
// ============================================================================

TEST_CASE("dispatcher absorbs clean batches", "[worker_pool][dispatcher]") {
  std::string dir = makeShardDir();
  std::vector<std::string> files = {"/tu/a.cpp", "/tu/b.cpp", "/tu/c.cpp",
                                    "/tu/d.cpp"};
  RunnerLog log;
  WorkerRunner runner = [&](const std::vector<std::string> &batch,
                            const std::string &shardPath,
                            const std::string &stderrPath) {
    log.record(batch);
    writeMarkers(stderrPath, batch);
    writeShard(shardPath, batch);
    return 0;
  };

  BuildStats stats;
  auto out = bakeIsolatedWithRunner(runner, files, /*workers=*/2, &stats, dir,
                                    /*expected=*/nullptr,
                                    /*batchSizeOverride=*/2);

  CHECK(log.calls() == 2);
  for (const auto &tu : files) {
    INFO(tu);
    CHECK(out.graph.findNode("fn@" + tu) != nullptr);
    auto callees = out.graph.calleesOf("fn@" + tu);
    REQUIRE(callees.size() == 1);
    CHECK(callees[0].calleeName == "common");
  }
  CHECK(out.graph.nodeCount() == files.size() + 1); // + shared "common"
  CHECK(out.graph.edgeCount() == files.size());     // distinct call sites
  CHECK(out.cfIndex.size() == files.size());
  CHECK(stats.tuStats.size() == files.size());
  CHECK(stats.crashCount() == 0);
  for (const auto &t : stats.tuStats)
    CHECK(t.toolStatus == 0);

  llvm::sys::fs::remove_directories(dir);
}

TEST_CASE("crash with marker poisons exactly the marked TU and re-dispatches "
          "the rest",
          "[worker_pool][dispatcher]") {
  std::string dir = makeShardDir();
  std::vector<std::string> files = {"/tu/a.cpp", "/tu/b.cpp",
                                    "/tu/poison.cpp"};
  RunnerLog log;
  WorkerRunner runner = [&](const std::vector<std::string> &batch,
                            const std::string &shardPath,
                            const std::string &stderrPath) {
    log.record(batch);
    bool hasPoison = std::find(batch.begin(), batch.end(),
                               "/tu/poison.cpp") != batch.end();
    if (hasPoison) {
      // Crashed mid-batch: markers up to and including the poison TU, no
      // shard written (the worker died before saving).
      writeMarkers(stderrPath, batch);
      return 1;
    }
    writeMarkers(stderrPath, batch);
    writeShard(shardPath, batch);
    return 0;
  };

  BuildStats stats;
  auto out = bakeIsolatedWithRunner(runner, files, /*workers=*/1, &stats, dir,
                                    /*expected=*/nullptr,
                                    /*batchSizeOverride=*/3);

  CHECK(log.calls() == 2); // failed [a,b,poison], then clean [a,b]
  CHECK(out.graph.findNode("fn@/tu/a.cpp") != nullptr);
  CHECK(out.graph.findNode("fn@/tu/b.cpp") != nullptr);
  CHECK(out.graph.findNode("fn@/tu/poison.cpp") == nullptr);
  CHECK(stats.crashCount() == 1);
  CHECK(poisonedFiles(stats) ==
        std::vector<std::string>{"/tu/poison.cpp"});

  llvm::sys::fs::remove_directories(dir);
}

TEST_CASE("markerless crash bisects down to the single poisoned TU",
          "[worker_pool][dispatcher]") {
  std::string dir = makeShardDir();
  std::vector<std::string> files = {"/tu/a.cpp", "/tu/b.cpp", "/tu/c.cpp",
                                    "/tu/d.cpp"};
  RunnerLog log;
  WorkerRunner runner = [&](const std::vector<std::string> &batch,
                            const std::string &shardPath,
                            const std::string &stderrPath) {
    log.record(batch);
    bool hasD =
        std::find(batch.begin(), batch.end(), "/tu/d.cpp") != batch.end();
    if (hasD)
      return 1; // spawn-style failure: no stderr, no shard
    writeMarkers(stderrPath, batch);
    writeShard(shardPath, batch);
    return 0;
  };

  BuildStats stats;
  auto out = bakeIsolatedWithRunner(runner, files, /*workers=*/1, &stats, dir,
                                    /*expected=*/nullptr,
                                    /*batchSizeOverride=*/4);

  // [a,b,c,d] fails -> [a,b] clean, [c,d] fails -> [c] clean, [d] poisoned.
  CHECK(log.calls() == 5);
  CHECK(out.graph.findNode("fn@/tu/a.cpp") != nullptr);
  CHECK(out.graph.findNode("fn@/tu/b.cpp") != nullptr);
  CHECK(out.graph.findNode("fn@/tu/c.cpp") != nullptr);
  CHECK(out.graph.findNode("fn@/tu/d.cpp") == nullptr);
  CHECK(stats.crashCount() == 1);
  CHECK(poisonedFiles(stats) == std::vector<std::string>{"/tu/d.cpp"});

  llvm::sys::fs::remove_directories(dir);
}

TEST_CASE("markerless single-TU failure is poisoned without retry",
          "[worker_pool][dispatcher]") {
  std::string dir = makeShardDir();
  RunnerLog log;
  WorkerRunner runner = [&](const std::vector<std::string> &batch,
                            const std::string &, const std::string &) {
    log.record(batch);
    return 1;
  };

  BuildStats stats;
  auto out = bakeIsolatedWithRunner(runner, {"/tu/x.cpp"}, /*workers=*/1,
                                    &stats, dir, /*expected=*/nullptr,
                                    /*batchSizeOverride=*/1);
  CHECK(log.calls() == 1);
  CHECK(out.graph.nodeCount() == 0);
  CHECK(stats.crashCount() == 1);
  CHECK(poisonedFiles(stats) == std::vector<std::string>{"/tu/x.cpp"});

  llvm::sys::fs::remove_directories(dir);
}

TEST_CASE("each TU is re-dispatched at most twice",
          "[worker_pool][dispatcher]") {
  std::string dir = makeShardDir();
  std::vector<std::string> files = {"/tu/a.cpp", "/tu/b.cpp", "/tu/c.cpp",
                                    "/tu/d.cpp"};
  RunnerLog log;
  // Pathological worker: always crashes, always blaming the LAST TU of the
  // batch, so every survivor keeps getting re-dispatched.
  WorkerRunner runner = [&](const std::vector<std::string> &batch,
                            const std::string &,
                            const std::string &stderrPath) {
    log.record(batch);
    writeMarkers(stderrPath, batch);
    return 1;
  };

  BuildStats stats;
  auto out = bakeIsolatedWithRunner(runner, files, /*workers=*/1, &stats, dir,
                                    /*expected=*/nullptr,
                                    /*batchSizeOverride=*/4);

  // [a,b,c,d] poisons d; [a,b,c] poisons c; [a,b] poisons b and a's third
  // re-dispatch is over the bound, so a is poisoned without another run.
  CHECK(log.calls() == 3);
  CHECK(log.dispatchesOf("/tu/a.cpp") == 3); // initial + 2 retries, no more
  CHECK(out.graph.nodeCount() == 0);
  CHECK(stats.crashCount() == 4);
  CHECK(poisonedFiles(stats) ==
        std::vector<std::string>{"/tu/a.cpp", "/tu/b.cpp", "/tu/c.cpp",
                                 "/tu/d.cpp"});

  llvm::sys::fs::remove_directories(dir);
}

TEST_CASE("clean exit with an unreadable shard is retried as a failure",
          "[worker_pool][dispatcher]") {
  std::string dir = makeShardDir();
  RunnerLog log;
  WorkerRunner runner = [&](const std::vector<std::string> &batch,
                            const std::string &, const std::string &) {
    log.record(batch);
    return 0; // "clean" exit, but no shard was ever written
  };

  BuildStats stats;
  auto out = bakeIsolatedWithRunner(runner, {"/tu/x.cpp"}, /*workers=*/1,
                                    &stats, dir, /*expected=*/nullptr,
                                    /*batchSizeOverride=*/1);
  CHECK(out.graph.nodeCount() == 0);
  CHECK(stats.crashCount() == 1);
  CHECK(poisonedFiles(stats) == std::vector<std::string>{"/tu/x.cpp"});

  llvm::sys::fs::remove_directories(dir);
}
