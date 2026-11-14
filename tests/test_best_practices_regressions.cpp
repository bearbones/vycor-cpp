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

#include "vycor/callgraph/CallGraph.h"
#include "vycor/callgraph/ChannelIndex.h"
#include "vycor/callgraph/CollapseFilter.h"
#include "vycor/callgraph/ControlFlowIndex.h"
#include "vycor/callgraph/ControlFlowOracle.h"
#include "vycor/callgraph/PathSearch.h"
#include "vycor/callgraph/StringInterner.h"
#include "vycor/check/TypeNormalize.h"
#include "vycor/query/Tools.h"

#include "llvm/Support/JSON.h"

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <utility>
#include <vector>

using namespace vycor;

namespace {

const ToolEntry *findRegisteredTool(llvm::StringRef name) {
  static const std::vector<ToolEntry> tools = getRegisteredTools();
  for (const auto &t : tools) {
    if (t.name == name)
      return &t;
  }
  return nullptr;
}

} // namespace

TEST_CASE("CollapseFilter ignores empty and slash-only patterns",
          "[callgraph][collapse]") {
  CollapseFilter emptyFilter({"", "/", "///", ""});
  CHECK(emptyFilter.empty());
  CHECK_FALSE(emptyFilter.isCollapsed("/workspace/src/main.cpp"));
  CHECK_FALSE(emptyFilter.isCollapsed("src/main.cpp"));

  CollapseFilter mixed({"", "third_party/fmt", "//vendor/zlib//"});
  CHECK_FALSE(mixed.empty());
  CHECK(mixed.isCollapsed("/repo/third_party/fmt/format.cc"));
  CHECK(mixed.isCollapsed("/repo/vendor/zlib/inflate.c"));
  CHECK_FALSE(mixed.isCollapsed("/repo/src/engine.cpp"));
}

TEST_CASE("StringInterner move and self-move preserve invariants",
          "[callgraph][interner]") {
  StringInterner a;
  auto id0 = a.intern("short");
  auto id1 = a.intern("a_much_longer_string_exceeding_sso_buffer_capacity_12345");
  REQUIRE(a.size() == 2);

  // Self-move must not deadlock or corrupt state.
  StringInterner *selfPtr = &a;
  a = std::move(*selfPtr);
  REQUIRE(a.size() == 2);
  CHECK(a.resolve(id0) == "short");
  CHECK(a.find("short") == id0);
  CHECK(a.find("a_much_longer_string_exceeding_sso_buffer_capacity_12345") ==
        id1);

  // Move construction clears moved-from index so moved-from find/intern cannot
  // dereference dangling string_views or return out-of-bounds ids.
  StringInterner b(std::move(a));
  CHECK(a.size() == 0);
  CHECK_FALSE(a.find("short").has_value());
  auto freshId = a.intern("short");
  CHECK(freshId == 0);
  CHECK(a.resolve(freshId) == "short");

  CHECK(b.size() == 2);
  CHECK(b.resolve(id0) == "short");
  CHECK(b.resolve(id1) ==
        "a_much_longer_string_exceeding_sso_buffer_capacity_12345");
}

TEST_CASE("CallGraph, ControlFlowIndex, and ChannelIndex move invariants",
          "[callgraph][move]") {
  SECTION("CallGraph move resets moved-from counters and handles self-move") {
    CallGraph g;
    g.addNode({"main", "main.cpp", 1, true, false, "", "c:@F@main"}, "main.cpp");
    g.addNode({"helper", "main.cpp", 5, false, false, "", "c:@F@helper"},
              "main.cpp");
    g.addEdge({"c:@F@main", "c:@F@helper", EdgeKind::DirectCall,
               Confidence::Proven, "main.cpp:2:3"},
              "main.cpp");
    REQUIRE(g.nodeCount() == 2);
    REQUIRE(g.edgeCount() == 1);

    CallGraph *self = &g;
    g = std::move(*self);
    CHECK(g.nodeCount() == 2);
    CHECK(g.edgeCount() == 1);

    CallGraph dst;
    dst = std::move(g);
    CHECK(g.nodeCount() == 0);
    CHECK(g.edgeCount() == 0);
    CHECK(dst.nodeCount() == 2);
    CHECK(dst.edgeCount() == 1);
  }

  SECTION("ControlFlowIndex moved-from instance stays valid for new inserts") {
    ControlFlowIndex cf;
    CallSiteContext ctx;
    ctx.callerName = "main";
    ctx.calleeName = "helper";
    ctx.callSite = "main.cpp:2:3";
    cf.addCallSiteContext(ctx);
    REQUIRE(cf.size() == 1);

    ControlFlowIndex *self = &cf;
    cf = std::move(*self);
    CHECK(cf.size() == 1);

    ControlFlowIndex moved(std::move(cf));
    CHECK(cf.size() == 0);
    CHECK(moved.size() == 1);

    // Moved-from ControlFlowIndex must still have sentinel empty set 0 so
    // subsequent addCallSiteContext + protectedCallsTo/unprotectedCallsTo work.
    CallSiteContext ctx2;
    ctx2.callerName = "f";
    ctx2.calleeName = "g";
    ctx2.callSite = "f.cpp:10:5";
    cf.addCallSiteContext(ctx2);
    CHECK(cf.size() == 1);
    CHECK(cf.protectedCallsTo("g").empty());
    CHECK(cf.unprotectedCallsTo("g").size() == 1);
  }

  SECTION("ChannelIndex move and siteAtCallSite lookup across removeTU/compact") {
    ChannelIndex ch;
    ChannelSite s1;
    s1.channelId = "q1";
    s1.channelTypeName = "Queue";
    s1.category = "queue";
    s1.op = ChannelOperation::Produce;
    s1.siteFunctionUsr = "c:@F@prod";
    s1.siteFunctionDisplay = "prod";
    s1.callSite = "a.cpp:10:3";
    s1.tuPath = "a.cpp";
    ch.addSite(s1);

    ChannelSite s2 = s1;
    s2.op = ChannelOperation::Consume;
    s2.siteFunctionUsr = "c:@F@cons";
    s2.siteFunctionDisplay = "cons";
    s2.callSite = "b.cpp:20:3";
    s2.tuPath = "b.cpp";
    ch.addSite(s2);

    REQUIRE(ch.siteAtCallSite("a.cpp:10:3").has_value());
    REQUIRE(ch.siteAtCallSite("b.cpp:20:3").has_value());

    ChannelIndex *self = &ch;
    ch = std::move(*self);
    REQUIRE(ch.size() == 2);

    ChannelIndex dst;
    dst = std::move(ch);
    CHECK(ch.size() == 0);
    CHECK(dst.size() == 2);

    dst.removeTU("a.cpp");
    CHECK_FALSE(dst.siteAtCallSite("a.cpp:10:3").has_value());
    CHECK(dst.siteAtCallSite("b.cpp:20:3").has_value());

    dst.compact();
    auto remaining = dst.siteAtCallSite("b.cpp:20:3");
    REQUIRE(remaining.has_value());
    CHECK(remaining->siteFunctionUsr == "c:@F@cons");
  }
}

TEST_CASE("queryNearestCatches reports targetKnown=false after removeTU drops TU",
          "[oracle][warm-refresh]") {
  CallGraph g;
  ControlFlowIndex cf;
  g.addNode({"main", "main.cpp", 1, true, false, "", "main"}, "main.cpp");
  g.addNode({"vanishedFn", "old.cpp", 4, false, false, "", "vanishedFn"},
            "old.cpp");
  g.addEdge({"main", "vanishedFn", EdgeKind::DirectCall, Confidence::Proven,
             "old.cpp:8:3"},
            "old.cpp");

  ControlFlowOracle oracle(g, cf);
  REQUIRE(oracle.queryNearestCatches("vanishedFn", 10).targetKnown);

  // Drop old.cpp; StringInterner still holds "vanishedFn", but the graph no
  // longer references it.
  g.removeTU("old.cpp");
  auto res = oracle.queryNearestCatches("vanishedFn", 10);
  CHECK_FALSE(res.targetKnown);
  CHECK(res.catches.empty());
}

TEST_CASE("normalizeTypeForMatching single-pass edge cases",
          "[check][typenormalize]") {
  CHECK(normalizeTypeForMatching("const Vector &") == "Vector");
  CHECK(normalizeTypeForMatching("const volatile int * const &") == "int*");
  CHECK(normalizeTypeForMatching("Foo::const_iterator &") ==
        "Foo::const_iterator");
  CHECK(normalizeTypeForMatching("my_volatile_state") == "my_volatile_state");
  CHECK(normalizeTypeForMatching("std::function<void(const int &)> &") ==
        "std::function<void(int&)>");
  CHECK(normalizeTypeForMatching("unsigned   long\tlong  int") ==
        "unsignedlonglongint");
  CHECK(isArithmeticNormalizedType(
      normalizeTypeForMatching("unsigned long long int")));
  CHECK_FALSE(isArithmeticNormalizedType("std::string"));
}

TEST_CASE("query_same_lock emits sharedLocks in deterministic sorted order",
          "[query][locks][determinism]") {
  CallGraph g;
  ControlFlowIndex cf;
  g.addNode({"entry", "a.cpp", 1, true, false, "", "entry"});
  g.addNode({"fnA", "a.cpp", 10, false, false, "", "fnA"});
  g.addNode({"fnB", "a.cpp", 20, false, false, "", "fnB"});
  g.addEdge({"entry", "fnA", EdgeKind::DirectCall, Confidence::Proven,
             "a.cpp:3:3"});
  g.addEdge({"entry", "fnB", EdgeKind::DirectCall, Confidence::Proven,
             "a.cpp:4:3"});

  CallSiteContext ctxA;
  ctxA.callerName = "entry";
  ctxA.callerUsr = "entry";
  ctxA.calleeName = "fnA";
  ctxA.calleeUsr = "fnA";
  ctxA.callSite = "a.cpp:3:3";
  ctxA.liveRaiiLocals = {
      {"std::lock_guard<std::mutex>", "zetaLock", "a.cpp:2:3", RaiiKind::Lock},
      {"absl::MutexLock", "alphaLock", "a.cpp:2:10", RaiiKind::Lock},
      {"std::lock_guard<std::mutex>", "betaLock", "a.cpp:2:20", RaiiKind::Lock},
  };
  cf.addCallSiteContext(ctxA);

  CallSiteContext ctxB = ctxA;
  ctxB.calleeName = "fnB";
  ctxB.calleeUsr = "fnB";
  ctxB.callSite = "a.cpp:4:3";
  cf.addCallSiteContext(ctxB);

  ControlFlowOracle oracle(g, cf);
  std::vector<std::string> eps = {"entry"};
  ToolContext tctx{g, oracle, cf, eps};
  tctx.facts.freshness = IndexFreshness::Baked;
  tctx.facts.coverage = {1, 1, 0, 0};

  const ToolEntry *tool = findRegisteredTool("query_same_lock");
  REQUIRE(tool != nullptr);

  llvm::json::Object args;
  args["fn_a"] = "fnA";
  args["fn_b"] = "fnB";
  auto res = runTool(*tool, args, tctx);
  auto *obj = res.getAsObject();
  REQUIRE(obj != nullptr);
  auto *shared = obj->getArray("sharedLocks");
  REQUIRE(shared != nullptr);
  REQUIRE(shared->size() == 3);

  CHECK((*shared)[0].getAsObject()->getString("typeName") == "absl::MutexLock");
  CHECK((*shared)[0].getAsObject()->getString("varName") == "alphaLock");
  CHECK((*shared)[1].getAsObject()->getString("varName") == "betaLock");
  CHECK((*shared)[2].getAsObject()->getString("varName") == "zetaLock");
}

TEST_CASE("explain_ordering concurrency boundary BFS tracks caller USRs cleanly",
          "[query][channels]") {
  CallGraph g;
  ControlFlowIndex cf;
  ChannelIndex ch;

  g.addNode({"spawner", "s.cpp", 1, true, false, "", "c:@F@spawner#"});
  // Two overloaded `work` functions with distinct USRs: only `work(int)` is
  // spawned on a background thread; `work(double)` runs synchronously.
  g.addNode({"work", "s.cpp", 10, false, false, "", "c:@F@work#I#"});
  g.addNode({"work", "s.cpp", 20, false, false, "", "c:@F@work#d#"});
  g.addNode({"sendA", "s.cpp", 30, false, false, "", "c:@F@sendA#"});
  g.addNode({"sendB", "s.cpp", 40, false, false, "", "c:@F@sendB#"});

  g.addEdge({"c:@F@spawner#", "c:@F@work#I#", EdgeKind::ThreadEntry,
             Confidence::Proven, "s.cpp:3:3", 0,
             ExecutionContext::ThreadSpawn});
  g.addEdge({"c:@F@spawner#", "c:@F@work#d#", EdgeKind::DirectCall,
             Confidence::Proven, "s.cpp:4:3", 0,
             ExecutionContext::Synchronous});
  // Recursive self-call on work(double) + call to sendB: must not conflate
  // work(double)'s display name "work" with work(int)'s ThreadSpawn!
  g.addEdge({"c:@F@work#d#", "c:@F@work#d#", EdgeKind::DirectCall,
             Confidence::Proven, "s.cpp:21:3"});
  g.addEdge({"c:@F@work#d#", "c:@F@sendB#", EdgeKind::DirectCall,
             Confidence::Proven, "s.cpp:22:3"});
  g.addEdge({"c:@F@work#I#", "c:@F@sendA#", EdgeKind::DirectCall,
             Confidence::Proven, "s.cpp:12:3"});

  ChannelSite siteA;
  siteA.channelId = "chanA";
  siteA.channelTypeName = "Queue";
  siteA.category = "queue";
  siteA.op = ChannelOperation::Produce;
  siteA.siteFunctionUsr = "c:@F@sendA#";
  siteA.siteFunctionDisplay = "sendA";
  siteA.callSite = "s.cpp:31:3";
  ch.addSite(siteA);

  ChannelSite siteB = siteA;
  siteB.channelId = "chanB";
  siteB.siteFunctionUsr = "c:@F@sendB#";
  siteB.siteFunctionDisplay = "sendB";
  siteB.callSite = "s.cpp:41:3";
  ch.addSite(siteB);

  ControlFlowOracle oracle(g, cf);
  std::vector<std::string> eps = {"spawner"};
  ToolContext tctx{g, oracle, cf, eps, &ch};
  tctx.facts.freshness = IndexFreshness::Baked;
  tctx.facts.channelsIndexed = true;
  tctx.facts.coverage = {1, 1, 0, 0};

  const ToolEntry *tool = findRegisteredTool("explain_ordering");
  REQUIRE(tool != nullptr);

  llvm::json::Object args;
  args["call_site_a"] = "s.cpp:31:3";
  args["call_site_b"] = "s.cpp:41:3";
  auto res = runTool(*tool, args, tctx);
  auto *obj = res.getAsObject();
  REQUIRE(obj != nullptr);
  REQUIRE(obj->getObject("siteA") != nullptr);
  REQUIRE(obj->getObject("siteB") != nullptr);

  // sendA is called by work(int), which has a ThreadSpawn boundary;
  // sendB is called only by work(double), which is purely synchronous!
  auto *boundsA = obj->getArray("concurrencyBoundariesA");
  auto *boundsB = obj->getArray("concurrencyBoundariesB");
  REQUIRE(boundsA != nullptr);
  REQUIRE(boundsB != nullptr);
  CHECK(boundsA->size() == 1);
  CHECK(boundsB->empty());
}
