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
#include "vycor/callgraph/ChannelIndex.h"
#include "vycor/callgraph/ControlFlowIndex.h"
#include "vycor/callgraph/Snapshot.h"

#include "llvm/ADT/SmallString.h"
#include "clang/Tooling/CompilationDatabase.h"
#include "llvm/Support/FileSystem.h"

#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <cstdio>
#include <iterator>
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
  meta.channelTypes = {{"Queue", {"push"}, {"pop"}, "queue"}};
  meta.files = {{"/src/a.cpp", 1234567890ull, 2048ull},
                {"/src/b.cpp", 987654321ull, 4096ull}};
  meta.deps = {{"/src/a.h", 1000000000ull, 10ull},
               {"/src/shared.h", 2000000000ull, 20ull}};
  meta.tuDeps = {{0, 1}, {1}};
  return meta;
}

/// A channel site contributed by two TUs (dedup -> refs=2, mirroring
/// makeGraph's inlineFn) plus one single-TU consumer, so the round-trip
/// test exercises both the common case and the multi-contributor path.
ChannelIndex makeChannelIndex() {
  ChannelIndex ch;
  ChannelSite produce;
  produce.channelId = "c:eventQueue_";
  produce.channelTypeName = "Queue";
  produce.category = "queue";
  produce.op = ChannelOperation::Produce;
  produce.siteFunctionUsr = "fn:Shared::send";
  produce.siteFunctionDisplay = "Shared::send";
  produce.callSite = "/src/shared.h:10:5";
  ConditionalGuard guard;
  guard.conditionText = "streaming";
  guard.location = "/src/shared.h:9:3";
  guard.inTrueBranch = true;
  guard.isAssertion = false;
  produce.enclosingGuards.push_back(guard);
  produce.tuPath = "/src/a.cpp";
  ch.addSite(produce);
  produce.tuPath = "/src/b.cpp";
  ch.addSite(produce); // same key -> refs becomes 2

  ChannelSite consume;
  consume.channelId = "c:eventQueue_";
  consume.channelTypeName = "Queue";
  consume.category = "queue";
  consume.op = ChannelOperation::Consume;
  consume.siteFunctionUsr = "fn:Client::drain";
  consume.siteFunctionDisplay = "Client::drain";
  consume.callSite = "/src/client.cpp:20:5";
  consume.tuPath = "/src/b.cpp";
  ch.addSite(consume);

  return ch;
}

} // namespace

TEST_CASE("snapshot round-trips graph, CF index, and meta",
          "[snapshot]") {
  auto path = tempSnapshotPath("roundtrip");
  CallGraph g = makeGraph();
  ControlFlowIndex cf = makeCfIndex();
  SnapshotMeta meta = makeMeta();

  REQUIRE(SnapshotIO::save(path, g, cf, meta));

  SnapshotLoadStats loadStats;
  auto loaded = SnapshotIO::load(path, &loadStats);
  std::remove(path.c_str());
  REQUIRE(loaded.has_value());

  SECTION("load stats account for every section of the file") {
    std::vector<std::string> names;
    uint64_t bytes = 0;
    for (const auto &sec : loadStats.sections) {
      names.push_back(sec.name);
      bytes += sec.bytes;
      CHECK(sec.ms >= 0);
    }
    CHECK(names == std::vector<std::string>{
                       "meta", "graph_interner", "nodes", "edges",
                       "graph_relations", "cf_interner", "cf_set_tables",
                       "cf_contexts", "channels"});
    // Everything after the fixed header is a section.
    CHECK(bytes + SnapshotIO::kHeaderBytes == loadStats.fileBytes);
    CHECK(loadStats.totalMs >= 0);
  }

  SECTION("meta") {
    CHECK(loaded->meta.collapsePaths == meta.collapsePaths);
    CHECK(loaded->meta.lockAllowlist == meta.lockAllowlist);
    CHECK(loaded->meta.lockBuiltins == meta.lockBuiltins);
    CHECK(loaded->meta.channelTypes == meta.channelTypes);
    REQUIRE(loaded->meta.files.size() == 2);
    CHECK(loaded->meta.files[0] == meta.files[0]);
    CHECK(loaded->meta.files[1] == meta.files[1]);
    CHECK(loaded->meta.deps == meta.deps);
    CHECK(loaded->meta.tuDeps == meta.tuDeps);
  }

  SECTION("channels defaults empty when save() isn't given a ChannelIndex") {
    CHECK(loaded->channels.size() == 0);
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
    // 2 stored callees of main, plus the synthesized virtual-dispatch
    // expansion Base::run -> Derived::run (the override relation also
    // round-tripped through the snapshot).
    auto callees = loaded->graph.calleesOf("main");
    REQUIRE(callees.size() == 3);

    auto callers = loaded->graph.callersOf("helper");
    REQUIRE(callers.size() == 2);

    auto spawns = loaded->graph.calleesOf("helper");
    REQUIRE(spawns.size() == 1);
    CHECK(spawns[0].kind == EdgeKind::ThreadEntry);
    CHECK(spawns[0].execContext == ExecutionContext::ThreadSpawn);
    CHECK(spawns[0].indirectionDepth == 1);
    CHECK(spawns[0].callSite == "/src/a.cpp:4:5");
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

    const auto ctx = loaded->cfIndex.contextAtSite("/src/a.cpp:11:3");
    REQUIRE(ctx.has_value());
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

    const auto bare = loaded->cfIndex.contextAtSite("/src/b.cpp:16:3");
    REQUIRE(bare.has_value());
    CHECK(bare->callerNoexcept == NoexceptSpec::Noexcept);
  }
}

TEST_CASE("a sectioned load decodes only the requested sections",
          "[snapshot]") {
  auto path = tempSnapshotPath("sections");
  CallGraph g = makeGraph();
  ControlFlowIndex cf = makeCfIndex();
  SnapshotMeta meta = makeMeta();
  meta.entryPoints = {"main", "worker_entry"};
  REQUIRE(SnapshotIO::save(path, g, cf, meta));

  SECTION("meta only: header counts and entry points, empty indexes") {
    SnapshotLoadStats stats;
    auto snap = SnapshotIO::load(path, &stats, LoadMode::ReadOnly, 0);
    REQUIRE(snap.has_value());
    CHECK(snap->loaded == 0);
    CHECK(snap->summary.nodes == g.nodeCount());
    CHECK(snap->summary.edges == g.edgeCount());
    CHECK(snap->summary.callSites == cf.size());
    CHECK(snap->summary.channelSites == 0);
    CHECK(snap->meta.entryPoints ==
          std::vector<std::string>{"main", "worker_entry"});
    CHECK(snap->graph.nodeCount() == 0);
    CHECK(snap->cfIndex.size() == 0);
    std::vector<std::string> names;
    size_t skipped = 0;
    for (const auto &sec : stats.sections) {
      names.push_back(sec.name);
      skipped += sec.skipped;
      if (sec.skipped)
        CHECK(sec.bytes > 0);
    }
    CHECK(names == std::vector<std::string>{"meta", "graph", "control_flow",
                                            "channels"});
    CHECK(skipped == 3);
  }

  SECTION("graph only: queries work, control flow stays empty") {
    auto snap =
        SnapshotIO::load(path, nullptr, LoadMode::ReadOnly, kSectionGraph);
    REQUIRE(snap.has_value());
    CHECK(snap->loaded == kSectionGraph);
    CHECK(snap->graph.calleesOf("main").size() == 3);
    CHECK(snap->cfIndex.size() == 0);
    CHECK(snap->summary.callSites == 2);
  }

  SECTION("control flow only") {
    auto snap = SnapshotIO::load(path, nullptr, LoadMode::ReadOnly,
                                 kSectionControlFlow);
    REQUIRE(snap.has_value());
    CHECK(snap->loaded == kSectionControlFlow);
    CHECK(snap->graph.nodeCount() == 0);
    CHECK(snap->cfIndex.contextAtSite("/src/a.cpp:11:3").has_value());
  }

  SECTION("a section table pointing past the file is rejected") {
    std::string bytes;
    {
      std::ifstream in(path, std::ios::binary);
      bytes.assign(std::istreambuf_iterator<char>(in), {});
    }
    // Corrupt the graph entry's length (kind byte at 4+4+32+4, then the
    // meta entry: 17 bytes; graph entry length lives 9 bytes into it).
    size_t graphLen = 4 + 4 + 32 + 4 + 17 + 1 + 8;
    REQUIRE(bytes.size() > graphLen + 8);
    for (int i = 0; i < 8; ++i)
      bytes[graphLen + i] = static_cast<char>(0xff);
    {
      std::ofstream out(path, std::ios::binary | std::ios::trunc);
      out << bytes;
    }
    CHECK_FALSE(SnapshotIO::load(path).has_value());
  }
  std::remove(path.c_str());
}

TEST_CASE("a read-only load answers the same queries as a mutable one",
          "[snapshot]") {
  auto path = tempSnapshotPath("readonly");
  CallGraph g = makeGraph();
  ControlFlowIndex cf = makeCfIndex();
  REQUIRE(SnapshotIO::save(path, g, cf, makeMeta()));

  auto full = SnapshotIO::load(path);
  auto ro = SnapshotIO::load(path, nullptr, LoadMode::ReadOnly);
  std::remove(path.c_str());
  REQUIRE(full.has_value());
  REQUIRE(ro.has_value());

  CHECK(ro->graph.nodeCount() == full->graph.nodeCount());
  CHECK(ro->graph.edgeCount() == full->graph.edgeCount());
  for (const char *name : {"main", "helper", "Base::run", "Derived::run"}) {
    auto a = full->graph.calleesOf(name), b = ro->graph.calleesOf(name);
    REQUIRE(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i) {
      CHECK(a[i].calleeName == b[i].calleeName);
      CHECK(a[i].callSite == b[i].callSite);
    }
    CHECK(full->graph.callersOf(name).size() ==
          ro->graph.callersOf(name).size());
  }
  CHECK(ro->graph.getOverrides("Base::run") ==
        full->graph.getOverrides("Base::run"));

  CHECK(ro->cfIndex.size() == full->cfIndex.size());
  const auto ctx = ro->cfIndex.contextAtSite("/src/a.cpp:11:3");
  REQUIRE(ctx.has_value());
  CHECK(ctx->callerName == "main");
  CHECK(ctx->tuPath == "/src/a.cpp");
  REQUIRE(ctx->enclosingTryCatches.size() == 1);
  CHECK(ro->cfIndex.contextsForCallee("helper").size() ==
        full->cfIndex.contextsForCallee("helper").size());
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

TEST_CASE("snapshot round-trips ChannelIndex with multi-TU refs and guards",
          "[snapshot][ChannelIndex]") {
  auto path = tempSnapshotPath("channels");
  CallGraph g = makeGraph();
  ControlFlowIndex cf = makeCfIndex();
  ChannelIndex ch = makeChannelIndex();

  REQUIRE(SnapshotIO::save(path, g, cf, makeMeta(), ch));
  auto loaded = SnapshotIO::load(path);
  std::remove(path.c_str());
  REQUIRE(loaded.has_value());

  CHECK(loaded->channels.size() == 2);

  auto producers = loaded->channels.producersOf("c:eventQueue_");
  REQUIRE(producers.size() == 1); // refs=2 dedups to one live site
  CHECK(producers[0].siteFunctionDisplay == "Shared::send");
  REQUIRE(producers[0].enclosingGuards.size() == 1);
  CHECK(producers[0].enclosingGuards[0].conditionText == "streaming");
  CHECK(producers[0].enclosingGuards[0].inTrueBranch);

  auto consumers = loaded->channels.consumersOf("c:eventQueue_");
  REQUIRE(consumers.size() == 1);
  CHECK(consumers[0].siteFunctionDisplay == "Client::drain");

  // The refs=2 producer site survives removing just one of its two
  // contributing TUs...
  CHECK(loaded->channels.removeTU("/src/a.cpp") == 0);
  CHECK(loaded->channels.producersOf("c:eventQueue_").size() == 1);

  // ...and is fully removed once the second (and last) TU is dropped, while
  // the unrelated single-TU consumer on the same TU also drops.
  CHECK(loaded->channels.removeTU("/src/b.cpp") == 2);
  CHECK(loaded->channels.producersOf("c:eventQueue_").empty());
  CHECK(loaded->channels.consumersOf("c:eventQueue_").empty());
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

namespace {

std::string writeText(const std::string &path, const std::string &text) {
  std::ofstream(path) << text;
  return path;
}

/// What the frontend records for a file it opened: whole-second mtime.
FileStamp parsedStamp(const std::string &path) {
  auto fs = SnapshotIO::stampFiles({path})[0];
  fs.mtimeNs -= fs.mtimeNs % 1000000000ull;
  return fs;
}

} // anonymous namespace

TEST_CASE("dependency stamps dirty the TUs that opened a changed header",
          "[snapshot]") {
  llvm::SmallString<128> dir;
  REQUIRE(!llvm::sys::fs::createUniqueDirectory("vycor-deps", dir));
  const std::string base = std::string(dir) + "/";
  auto a = writeText(base + "a.cpp", "int a();\n");
  auto b = writeText(base + "b.cpp", "int b();\n");
  auto shared = writeText(base + "shared.h", "// shared\n");
  auto priv = writeText(base + "priv.h", "// private\n");

  SnapshotMeta meta;
  meta.files = SnapshotIO::stampFiles({a, b});
  TuDependencies deps;
  deps[a] = {parsedStamp(shared), parsedStamp(priv)};
  deps[b] = {parsedStamp(shared)};
  deps[base + "ghost.cpp"] = {parsedStamp(priv)}; // not a recorded TU
  SnapshotIO::recordDependencies(meta, deps);

  auto dirtyNow = [&](std::vector<std::string> tus, size_t &via) {
    SnapshotIO::DirtyReport why;
    auto dirty =
        SnapshotIO::dirtyTUs(meta, SnapshotIO::stampFiles(tus), nullptr, &why);
    via = why.viaDeps;
    return dirty;
  };
  size_t via = 99;

  SECTION("recordDependencies interns each file once, per recorded TU") {
    REQUIRE(meta.deps.size() == 2);
    REQUIRE(meta.tuDeps.size() == 2);
    CHECK(meta.tuDeps[0].size() == 2);
    REQUIRE(meta.tuDeps[1].size() == 1);
    CHECK(meta.deps[meta.tuDeps[1][0]].path == shared);
    auto expanded = SnapshotIO::dependenciesOf(meta);
    REQUIRE(expanded.size() == 2);
    CHECK(expanded[a] == deps[a]);
    CHECK(expanded[b] == deps[b]);
  }

  SECTION("nothing changed: nothing dirty") {
    CHECK(dirtyNow({a, b}, via) == std::vector<bool>{false, false});
    CHECK(via == 0);
  }

  SECTION("a changed private header dirties only its includer") {
    writeText(priv, "// private, edited\n");
    CHECK(dirtyNow({a, b}, via) == std::vector<bool>{true, false});
    CHECK(via == 1);
  }

  SECTION("a changed shared header dirties every includer") {
    writeText(shared, "// shared, edited\n");
    CHECK(dirtyNow({a, b}, via) == std::vector<bool>{true, true});
    CHECK(via == 2);
  }

  SECTION("a deleted header dirties its includer") {
    llvm::sys::fs::remove(priv);
    CHECK(dirtyNow({a, b}, via) == std::vector<bool>{true, false});
    CHECK(via == 1);
  }

  SECTION("an unrecorded TU and an edited TU are dirty on their own") {
    auto c = writeText(base + "c.cpp", "int c();\n");
    writeText(a, "int a();\nint aa();\n");
    CHECK(dirtyNow({a, b, c}, via) == std::vector<bool>{true, false, true});
    CHECK(via == 0);
  }

  SECTION("two parses of one header keep the older stamp") {
    FileStamp older = parsedStamp(shared);
    older.mtimeNs -= 5 * 1000000000ull;
    TuDependencies two;
    two[a] = {older};
    two[b] = {parsedStamp(shared)};
    SnapshotIO::recordDependencies(meta, two);
    REQUIRE(meta.deps.size() == 1);
    CHECK(meta.deps[0] == older);
    // The file on disk is newer than the older stamp: both are dirty.
    CHECK(dirtyNow({a, b}, via) == std::vector<bool>{true, true});
    CHECK(via == 2);
  }

  SECTION("the tables survive a save/load") {
    std::string path = base + "snap.vycs";
    REQUIRE(SnapshotIO::save(path, CallGraph(), ControlFlowIndex(), meta));
    auto loaded = SnapshotIO::load(path, nullptr, LoadMode::ReadOnly, 0);
    REQUIRE(loaded);
    CHECK(loaded->meta.deps == meta.deps);
    CHECK(loaded->meta.tuDeps == meta.tuDeps);
    CHECK(SnapshotIO::dependenciesOf(loaded->meta) ==
          SnapshotIO::dependenciesOf(meta));
  }

  llvm::sys::fs::remove_directories(dir);
}

TEST_CASE("fingerprints and outcomes dirty TUs the stamps would keep",
          "[snapshot]") {
  llvm::SmallString<128> dir;
  REQUIRE(!llvm::sys::fs::createUniqueDirectory("vycor-prov", dir));
  const std::string base = std::string(dir) + "/";
  auto a = writeText(base + "a.cpp", "int a();\n");
  auto b = writeText(base + "b.cpp", "int b();\n");
  auto shared = writeText(base + "shared.h", "// shared\n");
  // Files written just now sit inside the unstable-stamp window; the
  // tests below want stable stamps, so age them by a minute.
  const uint64_t kNs = 1000000000ull;
  auto ageStamp = [&](FileStamp fs) {
    fs.mtimeNs -= 60 * kNs;
    return fs;
  };

  SnapshotMeta meta;
  meta.files = SnapshotIO::stampFiles({a, b});
  meta.fingerprints = {"fp-a", "fp-b"};
  TuDependencies deps;
  deps[a] = {parsedStamp(shared)};
  deps[b] = {parsedStamp(shared)};
  SnapshotIO::recordDependencies(meta, deps);
  TuOutcomes outcomes;
  outcomes[a] = {TuStatus::Indexed, ""};
  outcomes[b] = {TuStatus::Indexed, ""};
  SnapshotIO::recordOutcomes(meta, outcomes);
  meta.provenance = {"vycor-cpp test", "LLVM test", "env-1", 12345};

  auto current = SnapshotIO::stampFiles({a, b});
  std::vector<std::string> fps = {"fp-a", "fp-b"};
  SnapshotIO::DirtyReport why;

  SECTION("nothing changed: nothing dirty, nothing counted") {
    CHECK(SnapshotIO::dirtyTUs(meta, current, &fps, &why) ==
          std::vector<bool>{false, false});
    CHECK(why.viaInputs == 0);
    CHECK(why.viaDeps == 0);
    CHECK(why.retried == 0);
    using R = SnapshotIO::DirtyReason;
    CHECK(why.reasons == std::vector<R>{R::Clean, R::Clean});
  }

  SECTION("a changed fingerprint dirties only that TU") {
    fps[1] = "fp-b-with-DNEW_TARGET";
    CHECK(SnapshotIO::dirtyTUs(meta, current, &fps, &why) ==
          std::vector<bool>{false, true});
    CHECK(why.viaInputs == 1);
    CHECK(why.viaDeps == 0);
    using R = SnapshotIO::DirtyReason;
    CHECK(why.reasons == std::vector<R>{R::Clean, R::Inputs});
  }

  SECTION("no fingerprints given: inputs are not checked") {
    fps[1] = "other";
    CHECK(SnapshotIO::dirtyTUs(meta, current, nullptr, &why) ==
          std::vector<bool>{false, false});
  }

  SECTION("a meta without recorded fingerprints is dirty through inputs") {
    meta.fingerprints.clear();
    CHECK(SnapshotIO::dirtyTUs(meta, current, &fps, &why) ==
          std::vector<bool>{true, true});
    CHECK(why.viaInputs == 2);
  }

  SECTION("a non-Indexed outcome is retried") {
    outcomes[a] = {TuStatus::Partial, "parse errors"};
    outcomes[b] = {TuStatus::Poisoned, "worker crashed"};
    SnapshotIO::recordOutcomes(meta, outcomes);
    CHECK(SnapshotIO::dirtyTUs(meta, current, &fps, &why) ==
          std::vector<bool>{true, true});
    CHECK(why.retried == 2);
    CHECK(why.viaInputs == 0);
    CHECK(why.viaDeps == 0);
  }

  SECTION("a requested TU with no outcome is recorded Skipped and retried") {
    outcomes.erase(b);
    SnapshotIO::recordOutcomes(meta, outcomes);
    REQUIRE(meta.outcomes.size() == 2);
    CHECK(meta.outcomes[1] ==
          TuOutcome{TuStatus::Skipped, "no outcome recorded"});
    CHECK(SnapshotIO::dirtyTUs(meta, current, &fps, &why) ==
          std::vector<bool>{false, true});
    CHECK(why.retried == 1);
    auto back = SnapshotIO::outcomesOf(meta);
    REQUIRE(back.size() == 2);
    CHECK(back[a].status == TuStatus::Indexed);
    CHECK(back[b].status == TuStatus::Skipped);
  }

  SECTION("each TU is counted once, inputs before deps before retry") {
    writeText(shared, "// shared, edited\n");
    fps[0] = "fp-a2";
    outcomes[b] = {TuStatus::Crashed, "signal 11"};
    SnapshotIO::recordOutcomes(meta, outcomes);
    CHECK(SnapshotIO::dirtyTUs(meta, current, &fps, &why) ==
          std::vector<bool>{true, true});
    CHECK(why.viaInputs == 1); // a: fingerprint wins
    CHECK(why.viaDeps == 1);   // b: the header wins over the outcome
    CHECK(why.retried == 0);
    using R = SnapshotIO::DirtyReason;
    CHECK(why.reasons == std::vector<R>{R::Inputs, R::Deps});
    writeText(b, "int b(); // edited\n");
    outcomes[a] = {TuStatus::Partial, "parse errors"};
    SnapshotIO::recordOutcomes(meta, outcomes);
    fps[0] = meta.fingerprints[0];
    writeText(shared, "// shared\n");
    CHECK(SnapshotIO::dirtyTUs(meta, SnapshotIO::stampFiles({a, b}), &fps,
                               &why) == std::vector<bool>{true, true});
    CHECK(why.reasons == std::vector<R>{R::Retry, R::Stamp});
    CHECK(why.retried == 1);
  }

  SECTION("coverage partitions the requested scope by outcome") {
    auto c = coverageOf(meta);
    CHECK(c.requested == 2);
    CHECK(c.indexed == 2);
    CHECK(c.complete());
    outcomes[a] = {TuStatus::Partial, "parse errors"};
    outcomes[b] = {TuStatus::Skipped, "no compile command"};
    SnapshotIO::recordOutcomes(meta, outcomes);
    c = coverageOf(meta);
    CHECK(c.indexed == 0);
    CHECK(c.partial == 1);
    CHECK(c.failed == 1);
    CHECK(!c.complete());
    meta.outcomes.clear(); // an older bake path: nothing recorded
    CHECK(coverageOf(meta).failed == 2);
  }

  SECTION("status names are the payload spellings") {
    CHECK(std::string(tuStatusName(TuStatus::Indexed)) == "indexed");
    CHECK(std::string(tuStatusName(TuStatus::Partial)) == "partial");
    CHECK(std::string(tuStatusName(TuStatus::Crashed)) == "crashed");
    CHECK(std::string(tuStatusName(TuStatus::Poisoned)) == "poisoned");
    CHECK(std::string(tuStatusName(TuStatus::Skipped)) == "skipped");
  }

  SECTION("unstable stamps: files touched since the bake started are "
          "recorded as unknown and dirty exactly once") {
    // Pretend the bake started a minute ago: the current stamps (files
    // written seconds ago) are all younger than that.
    const uint64_t bakeStart = current[0].mtimeNs - 60 * kNs;
    CHECK(SnapshotIO::markUnstableStamps(meta, bakeStart) == 3); // a, b, shared
    CHECK(meta.files[0].mtimeNs == 0);
    CHECK(meta.files[0].size != 0);
    CHECK(meta.deps[0].mtimeNs == 0);
    CHECK(SnapshotIO::dirtyTUs(meta, current, &fps, &why) ==
          std::vector<bool>{true, true});
    // Marking is idempotent and a bake that started later marks nothing.
    CHECK(SnapshotIO::markUnstableStamps(meta, bakeStart) == 0);
    SnapshotMeta later;
    later.files = {ageStamp(current[0])};
    later.deps = {ageStamp(parsedStamp(shared))};
    CHECK(SnapshotIO::markUnstableStamps(later, current[0].mtimeNs + kNs) ==
          0);
    // The window is whole seconds: a bake starting in the same second as
    // the file's mtime still marks it.
    SnapshotMeta same;
    same.files = {current[0]};
    CHECK(SnapshotIO::markUnstableStamps(
              same, current[0].mtimeNs - current[0].mtimeNs % kNs + 1) == 1);
  }

  SECTION("v10 fields survive a save/load; info needs the meta alone") {
    std::string path = base + "snap.vycs";
    REQUIRE(SnapshotIO::save(path, CallGraph(), ControlFlowIndex(), meta));
    auto loaded = SnapshotIO::load(path, nullptr, LoadMode::ReadOnly, 0);
    REQUIRE(loaded);
    CHECK(loaded->loaded == 0);
    CHECK(loaded->meta.fingerprints == meta.fingerprints);
    CHECK(loaded->meta.outcomes == meta.outcomes);
    CHECK(loaded->meta.provenance == meta.provenance);
    CHECK(SnapshotIO::dirtyTUs(loaded->meta, current, &fps, &why) ==
          std::vector<bool>{false, false});
  }

  SECTION("a meta saved without fingerprints or outcomes loads as unknown "
          "inputs and Skipped outcomes") {
    SnapshotMeta bare;
    bare.files = meta.files;
    std::string path = base + "bare.vycs";
    REQUIRE(SnapshotIO::save(path, CallGraph(), ControlFlowIndex(), bare));
    auto loaded = SnapshotIO::load(path, nullptr, LoadMode::ReadOnly, 0);
    REQUIRE(loaded);
    REQUIRE(loaded->meta.fingerprints == std::vector<std::string>{"", ""});
    REQUIRE(loaded->meta.outcomes.size() == 2);
    CHECK(loaded->meta.outcomes[0].status == TuStatus::Skipped);
    CHECK(SnapshotIO::dirtyTUs(loaded->meta, current, &fps, &why) ==
          std::vector<bool>{true, true});
    CHECK(why.viaInputs == 2);
  }

  SECTION("an outcome byte outside the enum is corruption") {
    std::string path = base + "corrupt.vycs";
    REQUIRE(SnapshotIO::save(path, CallGraph(), ControlFlowIndex(), meta));
    std::ifstream in(path, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
    in.close();
    // The first TU's outcome byte follows its fingerprint "fp-a".
    auto pos = bytes.find("fp-a");
    REQUIRE(pos != std::string::npos);
    bytes[pos + 4] = 9;
    std::ofstream(path, std::ios::binary) << bytes;
    CHECK(!SnapshotIO::load(path, nullptr, LoadMode::ReadOnly, 0));
  }

  llvm::sys::fs::remove_directories(dir);
}

TEST_CASE("snapshot round-trips multi-contributor deduped edges",
          "[snapshot]") {
  auto path = tempSnapshotPath("multicontrib");
  CallGraph g;
  g.addNode({"inlineCaller", "common.h", 2, false, false, ""}, "/src/a.cpp");
  g.addNode({"inlineCaller", "common.h", 2, false, false, ""}, "/src/b.cpp");
  g.addNode({"target", "t.cpp", 5, false, false, ""}, "/src/t.cpp");
  g.addEdge({"inlineCaller", "target", EdgeKind::DirectCall,
             Confidence::Proven, "common.h:3:5", 0}, "/src/a.cpp");
  g.addEdge({"inlineCaller", "target", EdgeKind::DirectCall,
             Confidence::Proven, "common.h:3:5", 0}, "/src/b.cpp");
  REQUIRE(g.edgeCount() == 1);

  ControlFlowIndex cf;
  REQUIRE(SnapshotIO::save(path, g, cf, makeMeta()));
  auto loaded = SnapshotIO::load(path);
  std::remove(path.c_str());
  REQUIRE(loaded.has_value());

  CHECK(loaded->graph.edgeCount() == 1);
  CHECK(loaded->graph.callersOf("target").size() == 1);

  // Both contributors must survive the round-trip: removing one TU keeps
  // the edge, removing both drops it.
  CHECK(loaded->graph.removeTU("/src/a.cpp") == 0);
  CHECK(loaded->graph.edgeCount() == 1);
  CHECK(loaded->graph.removeTU("/src/b.cpp") == 1);
  CHECK(loaded->graph.edgeCount() == 0);
}

TEST_CASE("dependencies are recorded by the path the file system opened",
          "[snapshot]") {
  // A GCC toolchain found through /../lib (a bare `clang++` in the
  // compile command on a merged-/usr distribution) spells every
  // libstdc++ header as /../lib/gcc/<triple>/N/../../../../include/c++/N.
  // Lexically that is /include/c++/N; on disk /lib is a symlink into
  // /usr/lib, so the `..` climb out of it lands in /usr. A dependency
  // recorded by the lexical spelling names a file that does not exist,
  // and every warm start after that rebuilds cold. The same shape, in
  // a scratch tree: `link` is a symlink into `real/sub`, and the
  // include path climbs out of it.
  llvm::SmallString<128> dir;
  REQUIRE(!llvm::sys::fs::createUniqueDirectory("vycor-symlink-dep", dir));
  const std::string base = std::string(dir) + "/";
  REQUIRE(!llvm::sys::fs::create_directories(base + "real/inc"));
  REQUIRE(!llvm::sys::fs::create_directories(base + "real/sub"));
  REQUIRE(!llvm::sys::fs::create_link(base + "real/sub", base + "link"));
  auto header = writeText(base + "real/inc/h.h", "int h();\n");
  auto tu = writeText(base + "tu.cpp", "#include \"h.h\"\nint m() { return h(); }\n");

  llvm::SmallString<256> realHeader;
  REQUIRE(!llvm::sys::fs::real_path(header, realHeader));
  clang::tooling::FixedCompilationDatabase compDb(
      std::string(dir), {"-std=c++17", "-I", base + "link/../inc"});
  auto baked = bakeIndexes(compDb, {tu}, {}, /*threadCount=*/1);

  REQUIRE(baked.deps.count(tu) == 1);
  std::vector<std::string> paths;
  for (const auto &dep : baked.deps[tu])
    paths.push_back(dep.path);
  INFO("recorded: " << [&] {
    std::string s;
    for (const auto &p : paths)
      s += p + "\n";
    return s;
  }());
  CHECK(std::find(paths.begin(), paths.end(), std::string(realHeader)) !=
        paths.end());
  CHECK(std::find(paths.begin(), paths.end(), base + "inc/h.h") ==
        paths.end());
  for (const auto &p : paths)
    CHECK(llvm::sys::fs::exists(p));
  llvm::sys::fs::remove_directories(dir);
}
