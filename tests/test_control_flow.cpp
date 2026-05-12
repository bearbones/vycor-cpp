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

// test_control_flow.cpp — Tests for the control flow query system.

#include "vycor/callgraph/CallGraph.h"
#include "vycor/callgraph/CallGraphBuilder.h"
#include "vycor/callgraph/ControlFlowIndex.h"
#include "vycor/callgraph/ControlFlowOracle.h"

#include <catch2/catch_test_macros.hpp>
#include <clang/Tooling/Tooling.h>
#include <string>

using namespace vycor;

// ============================================================================
// ControlFlowIndex unit tests
// ============================================================================

TEST_CASE("ControlFlowIndex stores and queries contexts",
          "[prism][index]") {
  ControlFlowIndex index;

  SECTION("empty index returns no results") {
    CHECK(index.size() == 0);
    CHECK(index.contextAtSite("test.cpp:10:5") == nullptr);
    CHECK(index.contextsForCallee("foo").empty());
    CHECK(index.protectedCallsTo("foo").empty());
    CHECK(index.unprotectedCallsTo("foo").empty());
  }

  SECTION("unprotected call is queryable") {
    CallSiteContext ctx;
    ctx.callerName = "main";
    ctx.calleeName = "dangerous";
    ctx.callSite = "test.cpp:10:5";
    index.addCallSiteContext(std::move(ctx));

    CHECK(index.size() == 1);
    auto *result = index.contextAtSite("test.cpp:10:5");
    REQUIRE(result != nullptr);
    CHECK(result->callerName == "main");
    CHECK(result->calleeName == "dangerous");
    CHECK(result->enclosingTryCatches.empty());

    CHECK(index.unprotectedCallsTo("dangerous").size() == 1);
    CHECK(index.protectedCallsTo("dangerous").empty());
  }

  SECTION("protected call is queryable") {
    CallSiteContext ctx;
    ctx.callerName = "safe_caller";
    ctx.calleeName = "dangerous";
    ctx.callSite = "test.cpp:20:7";
    TryCatchScope scope;
    scope.tryLocation = "test.cpp:18:5";
    scope.enclosingFunction = "safe_caller";
    scope.nestingDepth = 0;
    CatchHandlerInfo handler;
    handler.caughtType = "std::exception";
    handler.location = "test.cpp:22:5";
    scope.handlers.push_back(std::move(handler));
    ctx.enclosingTryCatches.push_back(std::move(scope));
    index.addCallSiteContext(std::move(ctx));

    CHECK(index.protectedCallsTo("dangerous").size() == 1);
    CHECK(index.unprotectedCallsTo("dangerous").empty());
  }

  SECTION("mixed protection is queryable") {
    // Protected call.
    {
      CallSiteContext ctx;
      ctx.callerName = "safe_caller";
      ctx.calleeName = "dangerous";
      ctx.callSite = "test.cpp:20:7";
      TryCatchScope scope;
      scope.tryLocation = "test.cpp:18:5";
      scope.enclosingFunction = "safe_caller";
      CatchHandlerInfo handler;
      handler.caughtType = "std::exception";
      handler.location = "test.cpp:22:5";
      scope.handlers.push_back(std::move(handler));
      ctx.enclosingTryCatches.push_back(std::move(scope));
      index.addCallSiteContext(std::move(ctx));
    }
    // Unprotected call.
    {
      CallSiteContext ctx;
      ctx.callerName = "unsafe_caller";
      ctx.calleeName = "dangerous";
      ctx.callSite = "test.cpp:30:5";
      index.addCallSiteContext(std::move(ctx));
    }

    CHECK(index.contextsForCallee("dangerous").size() == 2);
    CHECK(index.protectedCallsTo("dangerous").size() == 1);
    CHECK(index.unprotectedCallsTo("dangerous").size() == 1);
  }

  SECTION("allContexts returns everything") {
    CallSiteContext ctx1;
    ctx1.callerName = "a";
    ctx1.calleeName = "b";
    ctx1.callSite = "1:1:1";
    index.addCallSiteContext(std::move(ctx1));

    CallSiteContext ctx2;
    ctx2.callerName = "c";
    ctx2.calleeName = "d";
    ctx2.callSite = "2:2:2";
    index.addCallSiteContext(std::move(ctx2));

    auto all = index.allContexts();
    CHECK(all.size() == 2);
  }
}

// ============================================================================
// ControlFlowOracle unit tests with hand-crafted graphs
// ============================================================================

TEST_CASE("Oracle determines exception protection across paths",
          "[prism][oracle]") {
  // Graph: main -> processFile -> readChunk -> allocateBuffer
  //        main -> directAlloc -> allocateBuffer
  // processFile calls readChunk inside a try/catch(std::exception).
  // directAlloc calls allocateBuffer without protection.

  CallGraph graph;
  graph.addNode({"main", "main.cpp", 1, true, false, ""});
  graph.addNode({"processFile", "proc.cpp", 1, false, false, ""});
  graph.addNode({"readChunk", "read.cpp", 1, false, false, ""});
  graph.addNode({"allocateBuffer", "alloc.cpp", 1, false, false, ""});
  graph.addNode({"directAlloc", "direct.cpp", 1, false, false, ""});

  graph.addEdge({"main", "processFile", EdgeKind::DirectCall,
                 Confidence::Proven, "main.cpp:5:3", 0});
  graph.addEdge({"main", "directAlloc", EdgeKind::DirectCall,
                 Confidence::Proven, "main.cpp:6:3", 0});
  graph.addEdge({"processFile", "readChunk", EdgeKind::DirectCall,
                 Confidence::Proven, "proc.cpp:10:5", 0});
  graph.addEdge({"readChunk", "allocateBuffer", EdgeKind::DirectCall,
                 Confidence::Proven, "read.cpp:5:5", 0});
  graph.addEdge({"directAlloc", "allocateBuffer", EdgeKind::DirectCall,
                 Confidence::Proven, "direct.cpp:3:5", 0});

  ControlFlowIndex cfIndex;

  // processFile -> readChunk is inside a try/catch.
  {
    CallSiteContext ctx;
    ctx.callerName = "processFile";
    ctx.calleeName = "readChunk";
    ctx.callSite = "proc.cpp:10:5";
    TryCatchScope scope;
    scope.tryLocation = "proc.cpp:8:3";
    scope.enclosingFunction = "processFile";
    CatchHandlerInfo handler;
    handler.caughtType = "std::exception";
    handler.location = "proc.cpp:14:3";
    scope.handlers.push_back(std::move(handler));
    ctx.enclosingTryCatches.push_back(std::move(scope));
    cfIndex.addCallSiteContext(std::move(ctx));
  }

  // All other edges have no try/catch context.
  {
    CallSiteContext ctx;
    ctx.callerName = "main";
    ctx.calleeName = "processFile";
    ctx.callSite = "main.cpp:5:3";
    cfIndex.addCallSiteContext(std::move(ctx));
  }
  {
    CallSiteContext ctx;
    ctx.callerName = "main";
    ctx.calleeName = "directAlloc";
    ctx.callSite = "main.cpp:6:3";
    cfIndex.addCallSiteContext(std::move(ctx));
  }
  {
    CallSiteContext ctx;
    ctx.callerName = "readChunk";
    ctx.calleeName = "allocateBuffer";
    ctx.callSite = "read.cpp:5:5";
    cfIndex.addCallSiteContext(std::move(ctx));
  }
  {
    CallSiteContext ctx;
    ctx.callerName = "directAlloc";
    ctx.calleeName = "allocateBuffer";
    ctx.callSite = "direct.cpp:3:5";
    cfIndex.addCallSiteContext(std::move(ctx));
  }

  ControlFlowOracle oracle(graph, cfIndex);

  SECTION("exception protection is sometimes_caught") {
    auto result = oracle.queryExceptionProtection(
        "allocateBuffer", "std::bad_alloc", {"main"});

    CHECK(result.protection == Protection::SometimesCaught);
    CHECK(result.paths.size() == 2);

    // At least one path should be caught, one not.
    bool hasCaught = false, hasUncaught = false;
    for (const auto &p : result.paths) {
      if (p.isCaught)
        hasCaught = true;
      else
        hasUncaught = true;
    }
    CHECK(hasCaught);
    CHECK(hasUncaught);
  }

  SECTION("summary mentions the function and exception type") {
    auto result = oracle.queryExceptionProtection(
        "allocateBuffer", "std::bad_alloc", {"main"});
    CHECK(result.summary.find("allocateBuffer") != std::string::npos);
    CHECK(result.summary.find("std::bad_alloc") != std::string::npos);
  }

  SECTION("queryCallSite returns protection info") {
    auto info = oracle.queryCallSite("proc.cpp:10:5");
    CHECK(info.isUnderTryCatch == true);
    CHECK(info.caller == "processFile");
    CHECK(info.callee == "readChunk");
    CHECK(info.enclosingScopes.size() == 1);
  }

  SECTION("queryCallSite for unprotected site") {
    auto info = oracle.queryCallSite("direct.cpp:3:5");
    CHECK(info.isUnderTryCatch == false);
    CHECK(info.caller == "directAlloc");
  }

  SECTION("queryNearestCatches finds the catch in processFile") {
    auto catches = oracle.queryNearestCatches("allocateBuffer");
    // Should find at least one catch (processFile's try/catch, 2 frames up).
    bool foundProcessFile = false;
    for (const auto &c : catches) {
      if (c.scope.enclosingFunction == "processFile")
        foundProcessFile = true;
    }
    CHECK(foundProcessFile);
  }

  SECTION("JSON output is valid") {
    auto result = oracle.queryExceptionProtection(
        "allocateBuffer", "std::bad_alloc", {"main"});
    auto json = ControlFlowOracle::toJson(result, "exception-protection",
                                          "allocateBuffer", "std::bad_alloc");
    CHECK(json.find("\"protection\"") != std::string::npos);
    CHECK(json.find("\"sometimes_caught\"") != std::string::npos);
    CHECK(json.find("\"summary\"") != std::string::npos);
    CHECK(json.find("\"paths\"") != std::string::npos);
  }
}

TEST_CASE("Oracle handles always-caught and never-caught cases",
          "[prism][oracle]") {
  SECTION("always caught") {
    CallGraph graph;
    graph.addNode({"main", "m.cpp", 1, true, false, ""});
    graph.addNode({"risky", "r.cpp", 1, false, false, ""});
    graph.addEdge({"main", "risky", EdgeKind::DirectCall,
                   Confidence::Proven, "m.cpp:5:3", 0});

    ControlFlowIndex cfIndex;
    {
      CallSiteContext ctx;
      ctx.callerName = "main";
      ctx.calleeName = "risky";
      ctx.callSite = "m.cpp:5:3";
      TryCatchScope scope;
      scope.tryLocation = "m.cpp:3:3";
      scope.enclosingFunction = "main";
      CatchHandlerInfo handler;
      handler.isCatchAll = true;
      handler.location = "m.cpp:7:3";
      scope.handlers.push_back(std::move(handler));
      ctx.enclosingTryCatches.push_back(std::move(scope));
      cfIndex.addCallSiteContext(std::move(ctx));
    }

    ControlFlowOracle oracle(graph, cfIndex);
    auto result = oracle.queryExceptionProtection(
        "risky", "std::exception", {"main"});
    CHECK(result.protection == Protection::AlwaysCaught);
  }

  SECTION("never caught") {
    CallGraph graph;
    graph.addNode({"main", "m.cpp", 1, true, false, ""});
    graph.addNode({"risky", "r.cpp", 1, false, false, ""});
    graph.addEdge({"main", "risky", EdgeKind::DirectCall,
                   Confidence::Proven, "m.cpp:5:3", 0});

    ControlFlowIndex cfIndex;
    {
      CallSiteContext ctx;
      ctx.callerName = "main";
      ctx.calleeName = "risky";
      ctx.callSite = "m.cpp:5:3";
      cfIndex.addCallSiteContext(std::move(ctx));
    }

    ControlFlowOracle oracle(graph, cfIndex);
    auto result = oracle.queryExceptionProtection(
        "risky", "std::exception", {"main"});
    CHECK(result.protection == Protection::NeverCaught);
  }

  SECTION("unknown when no paths exist") {
    CallGraph graph;
    graph.addNode({"main", "m.cpp", 1, true, false, ""});
    graph.addNode({"unreachable", "u.cpp", 1, false, false, ""});

    ControlFlowIndex cfIndex;
    ControlFlowOracle oracle(graph, cfIndex);
    auto result = oracle.queryExceptionProtection(
        "unreachable", "std::exception", {"main"});
    CHECK(result.protection == Protection::Unknown);
  }
}

// ============================================================================
// Integration tests: visitor + in-memory compilation
// ============================================================================

// Helper: build a CallGraph and ControlFlowIndex from in-memory source code.
static std::pair<CallGraph, ControlFlowIndex>
buildFromCode(const std::string &code) {
  CallGraph graph;
  CallGraphBuilderFactory graphFactory(graph);
  clang::tooling::runToolOnCodeWithArgs(graphFactory.create(), code,
                                        {"-std=c++17"}, "test_input.cpp");

  ControlFlowIndex cfIndex;
  // We need to build the control flow index using the same code.
  // buildControlFlowIndex requires a CompilationDatabase, but for in-memory
  // tests we need a simpler approach. We'll use the fact that the
  // ControlFlowContextVisitor is in a .cpp file with no public header,
  // so we test it end-to-end via buildControlFlowIndex with a fixed DB,
  // or we test the integration via the Oracle with hand-crafted data.
  //
  // For true integration, we test the full pipeline via the example files.
  // For unit tests, we use hand-crafted CallGraph + ControlFlowIndex.
  return {std::move(graph), std::move(cfIndex)};
}

TEST_CASE("CallGraph integration for exception context scenarios",
          "[prism][integration]") {
  // This test verifies the call graph structure for code with try/catch,
  // which the oracle will then reason about.
  std::string code = R"(
    namespace std {
      class exception {};
      class runtime_error : public exception {};
    }
    void dangerous() {}
    void safe_caller() {
      try {
        dangerous();
      } catch (const std::exception& e) {
      }
    }
    void unsafe_caller() {
      dangerous();
    }
    int main() {
      safe_caller();
      unsafe_caller();
      return 0;
    }
  )";

  CallGraph graph;
  CallGraphBuilderFactory factory(graph);
  REQUIRE(clang::tooling::runToolOnCodeWithArgs(factory.create(), code,
                                                {"-std=c++17"},
                                                "test_input.cpp"));

  // Verify the call graph structure.
  CHECK(graph.findNode("main") != nullptr);
  CHECK(graph.findNode("safe_caller") != nullptr);
  CHECK(graph.findNode("unsafe_caller") != nullptr);
  CHECK(graph.findNode("dangerous") != nullptr);

  // Both safe_caller and unsafe_caller should call dangerous.
  auto safeCallees = graph.calleesOf("safe_caller");
  bool safeCallsDangerous = false;
  for (const auto *e : safeCallees) {
    if (e->calleeName == "dangerous")
      safeCallsDangerous = true;
  }
  CHECK(safeCallsDangerous);

  auto unsafeCallees = graph.calleesOf("unsafe_caller");
  bool unsafeCallsDangerous = false;
  for (const auto *e : unsafeCallees) {
    if (e->calleeName == "dangerous")
      unsafeCallsDangerous = true;
  }
  CHECK(unsafeCallsDangerous);
}

// ============================================================================
// JSON dump tests
// ============================================================================

TEST_CASE("JSON dump serialization", "[prism][json]") {
  ControlFlowIndex index;

  CallSiteContext ctx;
  ctx.callerName = "caller";
  ctx.calleeName = "callee";
  ctx.callSite = "test.cpp:10:5";
  TryCatchScope scope;
  scope.tryLocation = "test.cpp:8:3";
  scope.enclosingFunction = "caller";
  CatchHandlerInfo handler;
  handler.caughtType = "std::exception";
  handler.location = "test.cpp:12:3";
  scope.handlers.push_back(std::move(handler));
  ctx.enclosingTryCatches.push_back(std::move(scope));

  ConditionalGuard guard;
  guard.conditionText = "size > 0";
  guard.location = "test.cpp:7:5";
  guard.inTrueBranch = true;
  ctx.enclosingGuards.push_back(std::move(guard));

  index.addCallSiteContext(std::move(ctx));

  auto json = ControlFlowOracle::dumpIndexToJson(index);

  CHECK(json.find("\"totalCallSites\": 1") != std::string::npos);
  CHECK(json.find("\"callerName\": \"caller\"") != std::string::npos);
  CHECK(json.find("\"calleeName\": \"callee\"") != std::string::npos);
  CHECK(json.find("\"std::exception\"") != std::string::npos);
  CHECK(json.find("\"size > 0\"") != std::string::npos);
  CHECK(json.find("\"inTrueBranch\": true") != std::string::npos);
}

// ============================================================================
// Exception type matching tests
// ============================================================================

TEST_CASE("Exception type matching through hierarchy",
          "[prism][oracle][matching]") {
  // catch(std::exception) should catch std::bad_alloc.
  CallGraph graph;
  graph.addNode({"main", "m.cpp", 1, true, false, ""});
  graph.addNode({"thrower", "t.cpp", 1, false, false, ""});
  graph.addEdge({"main", "thrower", EdgeKind::DirectCall,
                 Confidence::Proven, "m.cpp:5:3", 0});

  ControlFlowIndex cfIndex;
  {
    CallSiteContext ctx;
    ctx.callerName = "main";
    ctx.calleeName = "thrower";
    ctx.callSite = "m.cpp:5:3";
    TryCatchScope scope;
    scope.tryLocation = "m.cpp:3:3";
    scope.enclosingFunction = "main";
    CatchHandlerInfo handler;
    handler.caughtType = "std::exception";
    handler.location = "m.cpp:7:3";
    scope.handlers.push_back(std::move(handler));
    ctx.enclosingTryCatches.push_back(std::move(scope));
    cfIndex.addCallSiteContext(std::move(ctx));
  }

  ControlFlowOracle oracle(graph, cfIndex);

  SECTION("std::bad_alloc caught by std::exception handler") {
    auto result = oracle.queryExceptionProtection(
        "thrower", "std::bad_alloc", {"main"});
    CHECK(result.protection == Protection::AlwaysCaught);
  }

  SECTION("std::runtime_error caught by std::exception handler") {
    auto result = oracle.queryExceptionProtection(
        "thrower", "std::runtime_error", {"main"});
    CHECK(result.protection == Protection::AlwaysCaught);
  }

  SECTION("std::out_of_range caught by std::exception handler") {
    auto result = oracle.queryExceptionProtection(
        "thrower", "std::out_of_range", {"main"});
    CHECK(result.protection == Protection::AlwaysCaught);
  }

  SECTION("catch-all catches anything") {
    // Replace with catch-all handler.
    ControlFlowIndex cfIndex2;
    {
      CallSiteContext ctx;
      ctx.callerName = "main";
      ctx.calleeName = "thrower";
      ctx.callSite = "m.cpp:5:3";
      TryCatchScope scope;
      scope.tryLocation = "m.cpp:3:3";
      scope.enclosingFunction = "main";
      CatchHandlerInfo handler;
      handler.isCatchAll = true;
      handler.location = "m.cpp:7:3";
      scope.handlers.push_back(std::move(handler));
      ctx.enclosingTryCatches.push_back(std::move(scope));
      cfIndex2.addCallSiteContext(std::move(ctx));
    }

    ControlFlowOracle oracle2(graph, cfIndex2);
    auto result = oracle2.queryExceptionProtection(
        "thrower", "int", {"main"}); // Even non-std types.
    CHECK(result.protection == Protection::AlwaysCaught);
  }
}
