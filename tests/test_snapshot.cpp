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

// test_snapshot.cpp — round-trip tests for SnapshotIO binary persistence.

#include "vycor/callgraph/CallGraph.h"
#include "vycor/callgraph/ControlFlowIndex.h"
#include "vycor/callgraph/Snapshot.h"

#include "llvm/Support/FileSystem.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <fstream>
#include <string>

using namespace vycor;

namespace {

std::string tempSnapshotPath(const char *tag) {
  llvm::SmallString<128> path;
  llvm::sys::fs::createUniquePath(
      llvm::Twine("vycor-snapshot-") + tag + "-%%%%%%.bin", path,
      /*MakeAbsolute=*/true);
  return std::string(path.str());
}

/// Build a small graph with every feature the snapshot must preserve:
/// nodes with TU provenance, edges of several kinds, hierarchy, overrides,
/// effective impls, and function returns.
CallGraph makeGraph() {
  CallGraph g;
  g.addNode({"main", "/src/a.cpp", 10, true, false, ""}, "/src/a.cpp");
  g.addNode({"helper", "/src/a.cpp", 3, false, false, ""}, "/src/a.cpp");
  g.addNode({"Base::run", "/src/b.cpp", 5, false, true, "Base"},
            "/src/b.cpp");
  g.addNode({"Derived::run", "/src/b.cpp", 15, false, true, "Derived"},
            "/src/b.cpp");
  // Node contributed by two TUs (e.g. header-inlined function).
  g.addNode({"inlineFn", "/src/common.h", 2, false, false, ""}, "/src/a.cpp");
  g.addNode({"inlineFn", "/src/common.h", 2, false, false, ""}, "/src/b.cpp");

  g.addEdge({"main", "helper", EdgeKind::DirectCall, Confidence::Proven,
             "/src/a.cpp:11:3", 0, ExecutionContext::Synchronous},
            "/src/a.cpp");
  g.addEdge({"main", "Base::run", EdgeKind::VirtualDispatch,
             Confidence::Plausible, "/src/a.cpp:12:3", 0,
             ExecutionContext::Synchronous},
            "/src/a.cpp");
  g.addEdge({"helper", "inlineFn", EdgeKind::ThreadEntry, Confidence::Proven,
             "/src/a.cpp:4:5", 1, ExecutionContext::ThreadSpawn},
            "/src/a.cpp");
  g.addEdge({"Derived::run", "helper", EdgeKind::DirectCall,
             Confidence::Proven, "/src/b.cpp:16:3", 0,
             ExecutionContext::Synchronous},
            "/src/b.cpp");

  g.addDerivedClass("Base", "Derived");
  g.addMethodOverride("Base::run", "Derived::run");
  g.addEffectiveImpl("Derived", "Derived::run");
  g.addFunctionReturn("factory", "helper");
  return g;
}

ControlFlowIndex makeCfIndex() {
  ControlFlowIndex idx;

  CallSiteContext ctx;
  ctx.callerName = "main";
  ctx.calleeName = "helper";
  ctx.callSite = "/src/a.cpp:11:3";
  ctx.tuPath = "/src/a.cpp";
  TryCatchScope scope;
  scope.tryLocation = "/src/a.cpp:10:1";
  scope.enclosingFunction = "main";
  scope.nestingDepth = 0;
  CatchHandlerInfo handler;
  handler.caughtType = "std::runtime_error";
  handler.isCatchAll = false;
  handler.location = "/src/a.cpp:14:3";
  handler.bodySummary = "log(e.what());";
  scope.handlers.push_back(handler);
  ctx.enclosingTryCatches.push_back(scope);
  ConditionalGuard guard;
  guard.conditionText = "ptr != nullptr";
  guard.location = "/src/a.cpp:11:1";
  guard.inTrueBranch = true;
  guard.isAssertion = false;
  ctx.enclosingGuards.push_back(guard);
  ctx.callerNoexcept = NoexceptSpec::None;
  ctx.insideCatchBlock = false;
  RaiiLocal lock;
  lock.typeName = "std::lock_guard<std::mutex>";
  lock.varName = "g";
  lock.declLocation = "/src/a.cpp:10:5";
  lock.kind = RaiiKind::Lock;
  ctx.liveRaiiLocals.push_back(lock);
  idx.addCallSiteContext(std::move(ctx));

  CallSiteContext bare;
  bare.callerName = "Derived::run";
  bare.calleeName = "helper";
  bare.callSite = "/src/b.cpp:16:3";
  bare.tuPath = "/src/b.cpp";
  bare.callerNoexcept = NoexceptSpec::Noexcept;
  idx.addCallSiteContext(std::move(bare));

  return idx;
}

SnapshotMeta makeMeta() {
  SnapshotMeta meta;
  meta.collapsePaths = {"Client/Math"};
  meta.lockAllowlist = {"RBX::Arbiter"};
  meta.lockBuiltins = true;
  meta.files = {{"/src/a.cpp", 1234567890ull, 2048ull},
                {"/src/b.cpp", 987654321ull, 4096ull}};
  return meta;
}

} // namespace

TEST_CASE("snapshot round-trips graph, CF index, and meta",
          "[snapshot]") {
  auto path = tempSnapshotPath("roundtrip");
  CallGraph g = makeGraph();
  ControlFlowIndex cf = makeCfIndex();
  SnapshotMeta meta = makeMeta();

  REQUIRE(SnapshotIO::save(path, g, cf, meta));

  auto loaded = SnapshotIO::load(path);
  std::remove(path.c_str());
  REQUIRE(loaded.has_value());

  SECTION("meta") {
    CHECK(loaded->meta.collapsePaths == meta.collapsePaths);
    CHECK(loaded->meta.lockAllowlist == meta.lockAllowlist);
    CHECK(loaded->meta.lockBuiltins == meta.lockBuiltins);
    REQUIRE(loaded->meta.files.size() == 2);
    CHECK(loaded->meta.files[0] == meta.files[0]);
    CHECK(loaded->meta.files[1] == meta.files[1]);
  }

  SECTION("graph counts and node fields") {
    CHECK(loaded->graph.nodeCount() == g.nodeCount());
    CHECK(loaded->graph.edgeCount() == g.edgeCount());

    const auto *node = loaded->graph.findNode("Base::run");
    REQUIRE(node != nullptr);
    CHECK(node->file == "/src/b.cpp");
    CHECK(node->line == 5);
    CHECK(node->isVirtual);
    CHECK_FALSE(node->isEntryPoint);
    CHECK(node->enclosingClass == "Base");

    const auto *entry = loaded->graph.findNode("main");
    REQUIRE(entry != nullptr);
    CHECK(entry->isEntryPoint);
  }

  SECTION("edges with kind, confidence, and execution context") {
    auto callees = loaded->graph.calleesOf("main");
    REQUIRE(callees.size() == 2);

    auto callers = loaded->graph.callersOf("helper");
    REQUIRE(callers.size() == 2);

    auto spawns = loaded->graph.calleesOf("helper");
    REQUIRE(spawns.size() == 1);
    CHECK(spawns[0]->kind == EdgeKind::ThreadEntry);
    CHECK(spawns[0]->execContext == ExecutionContext::ThreadSpawn);
    CHECK(spawns[0]->indirectionDepth == 1);
    CHECK(spawns[0]->callSite == "/src/a.cpp:4:5");
  }

  SECTION("hierarchy, overrides, impls, returns") {
    CHECK(loaded->graph.getDerivedClasses("Base") ==
          std::vector<std::string>{"Derived"});
    CHECK(loaded->graph.getOverrides("Base::run") ==
          std::vector<std::string>{"Derived::run"});
    CHECK(loaded->graph.getClassesForImpl("Derived::run") ==
          std::vector<std::string>{"Derived"});
    CHECK(loaded->graph.getFunctionReturns("factory") ==
          std::set<std::string>{"helper"});
  }

  SECTION("control flow contexts") {
    CHECK(loaded->cfIndex.size() == 2);

    const auto *ctx = loaded->cfIndex.contextAtSite("/src/a.cpp:11:3");
    REQUIRE(ctx != nullptr);
    CHECK(ctx->callerName == "main");
    CHECK(ctx->tuPath == "/src/a.cpp");
    REQUIRE(ctx->enclosingTryCatches.size() == 1);
    REQUIRE(ctx->enclosingTryCatches[0].handlers.size() == 1);
    CHECK(ctx->enclosingTryCatches[0].handlers[0].caughtType ==
          "std::runtime_error");
    CHECK(ctx->enclosingTryCatches[0].handlers[0].bodySummary ==
          "log(e.what());");
    REQUIRE(ctx->enclosingGuards.size() == 1);
    CHECK(ctx->enclosingGuards[0].conditionText == "ptr != nullptr");
    REQUIRE(ctx->liveRaiiLocals.size() == 1);
    CHECK(ctx->liveRaiiLocals[0].kind == RaiiKind::Lock);

    const auto *bare = loaded->cfIndex.contextAtSite("/src/b.cpp:16:3");
    REQUIRE(bare != nullptr);
    CHECK(bare->callerNoexcept == NoexceptSpec::Noexcept);
  }
}

TEST_CASE("snapshot preserves TU provenance for incremental reindex",
          "[snapshot]") {
  auto path = tempSnapshotPath("provenance");
  CallGraph g = makeGraph();
  ControlFlowIndex cf = makeCfIndex();

  REQUIRE(SnapshotIO::save(path, g, cf, makeMeta()));
  auto loaded = SnapshotIO::load(path);
  std::remove(path.c_str());
  REQUIRE(loaded.has_value());

  // removeTU on the loaded graph must behave exactly as on the original:
  // /src/a.cpp contributed 3 edges and exclusively owns main + helper.
  size_t removed = loaded->graph.removeTU("/src/a.cpp");
  CHECK(removed == 3);
  CHECK(loaded->graph.edgeCount() == 1);
  CHECK(loaded->graph.findNode("main") == nullptr);
  CHECK(loaded->graph.findNode("helper") == nullptr);
  // inlineFn was contributed by both TUs and must survive.
  CHECK(loaded->graph.findNode("inlineFn") != nullptr);
  CHECK(loaded->graph.findNode("Base::run") != nullptr);

  // CF index removal matches the recorded tuPath.
  size_t cfRemoved = loaded->cfIndex.removeTU("/src/a.cpp");
  CHECK(cfRemoved == 1);
  CHECK(loaded->cfIndex.size() == 1);
}

TEST_CASE("snapshot drops tombstoned edges on save", "[snapshot]") {
  auto path = tempSnapshotPath("tombstones");
  CallGraph g = makeGraph();
  g.removeTU("/src/b.cpp");
  REQUIRE(g.edgeCount() == 3);

  REQUIRE(SnapshotIO::save(path, g, makeCfIndex(), makeMeta()));
  auto loaded = SnapshotIO::load(path);
  std::remove(path.c_str());
  REQUIRE(loaded.has_value());

  CHECK(loaded->graph.edgeCount() == 3);
  CHECK(loaded->graph.callersOf("helper").size() == 1);
}

TEST_CASE("snapshot load rejects bad input", "[snapshot]") {
  SECTION("missing file") {
    CHECK_FALSE(SnapshotIO::load("/nonexistent/vycor.bin").has_value());
  }

  SECTION("wrong magic") {
    auto path = tempSnapshotPath("badmagic");
    std::ofstream(path, std::ios::binary) << "NOPE garbage";
    CHECK_FALSE(SnapshotIO::load(path).has_value());
    std::remove(path.c_str());
  }

  SECTION("truncated file") {
    auto path = tempSnapshotPath("truncated");
    CallGraph g = makeGraph();
    ControlFlowIndex cf = makeCfIndex();
    REQUIRE(SnapshotIO::save(path, g, cf, makeMeta()));

    // Truncate to half size.
    std::ifstream in(path, std::ios::binary);
    std::string contents((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
    in.close();
    std::ofstream(path, std::ios::binary)
        << contents.substr(0, contents.size() / 2);

    CHECK_FALSE(SnapshotIO::load(path).has_value());
    std::remove(path.c_str());
  }

  SECTION("future format version") {
    auto path = tempSnapshotPath("version");
    CallGraph g = makeGraph();
    ControlFlowIndex cf = makeCfIndex();
    REQUIRE(SnapshotIO::save(path, g, cf, makeMeta()));

    // Bump the version field in place (bytes 4..7, little-endian).
    std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
    f.seekp(4);
    char v[4] = {(char)0xFF, 0, 0, 0};
    f.write(v, 4);
    f.close();

    CHECK_FALSE(SnapshotIO::load(path).has_value());
    std::remove(path.c_str());
  }
}

TEST_CASE("stampFiles flags missing files with zero stamps", "[snapshot]") {
  auto stamps = SnapshotIO::stampFiles({"/definitely/not/a/real/file.cpp"});
  REQUIRE(stamps.size() == 1);
  CHECK(stamps[0].mtimeNs == 0);
  CHECK(stamps[0].size == 0);
}
