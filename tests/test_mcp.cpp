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

// test_mcp.cpp — Tests for the MCP call graph server.

#include "vycor/callgraph/CallGraph.h"
#include "vycor/callgraph/ControlFlowIndex.h"
#include "vycor/callgraph/ControlFlowOracle.h"
#include "vycor/mcp/McpProtocol.h"
#include "vycor/mcp/McpTools.h"

#include "llvm/Support/JSON.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <cstring>
#include <string>

using namespace vycor;

// ============================================================================
// Helper: build a Chain-C-shaped graph for callback/concurrency tool tests
//
// Mirrors what the real builder produces for examples/deep_chains/runChainC
// without pulling in a ClangTool invocation. Keeps the MCP tool tests purely
// unit-level; the end-to-end edge-production behavior lives in
// test_deep_chains.cpp.
//
// Edges exercised:
//   main      -> runChainC                 DirectCall, Synchronous
//   runChainC -> worker_thread_entry       ThreadEntry, ThreadSpawn
//   runChainC -> compute_hash              ThreadEntry, AsyncTask
//   runChainC -> <lambda-node>             ThreadEntry, ThreadSpawn
//   runChainC -> <lambda-node>             LambdaCall,  Synchronous
//   runChainC -> registerValueCallback     DirectCall,  Synchronous
//   runChainC -> cbs::startupHook          FunctionPointer, Synchronous (Plausible)
// ============================================================================

static CallGraph buildChainCGraph() {
  CallGraph g;

  g.addNode({"main", "main.cpp", 75, true, false, ""});
  g.addNode({"runChainC", "main.cpp", 38, false, false, ""});
  g.addNode({"worker_thread_entry", "async_workers.cpp", 10, false, false, ""});
  g.addNode({"compute_hash", "async_workers.cpp", 20, false, false, ""});
  g.addNode({"registerValueCallback", "lambda_callbacks.cpp", 11, false, false, ""});
  g.addNode({"cbs::startupHook", "callbacks.cpp", 5, false, false, "cbs"});

  const std::string lam1 = "lambda#main.cpp:49:15#runChainC";
  const std::string lam2 = "lambda#main.cpp:58:7#runChainC";
  g.addNode({lam1, "main.cpp", 49, false, false, ""});
  g.addNode({lam2, "main.cpp", 58, false, false, ""});

  // main -> runChainC: direct call.
  g.addEdge({"main", "runChainC", EdgeKind::DirectCall,
             Confidence::Proven, "main.cpp:91:3", 0,
             ExecutionContext::Synchronous});

  // runChainC -> concurrency targets.
  g.addEdge({"runChainC", "worker_thread_entry", EdgeKind::ThreadEntry,
             Confidence::Proven, "main.cpp:40:3", 0,
             ExecutionContext::ThreadSpawn});
  g.addEdge({"runChainC", "compute_hash", EdgeKind::ThreadEntry,
             Confidence::Proven, "main.cpp:43:14", 0,
             ExecutionContext::AsyncTask});
  g.addEdge({"runChainC", lam1, EdgeKind::ThreadEntry,
             Confidence::Proven, "main.cpp:49:15", 0,
             ExecutionContext::ThreadSpawn});

  // runChainC -> callback registrations (lambda body + direct call).
  g.addEdge({"runChainC", lam2, EdgeKind::LambdaCall,
             Confidence::Proven, "main.cpp:58:7", 1,
             ExecutionContext::Synchronous});
  g.addEdge({"runChainC", "registerValueCallback", EdgeKind::DirectCall,
             Confidence::Proven, "main.cpp:57:3", 0,
             ExecutionContext::Synchronous});

  // runChainC -> function-pointer address-take (Plausible).
  g.addEdge({"runChainC", "cbs::startupHook", EdgeKind::FunctionPointer,
             Confidence::Plausible, "main.cpp:45:21", 0,
             ExecutionContext::Synchronous});

  return g;
}

// ============================================================================
// Helper: build a simple test graph
//
//   main -> processFile -> readData
//                       -> writeLog
//   main -> cleanup
//
//   Base (virtual: doWork) -> Derived (override: doWork)
//   main -> Base::doWork (VirtualDispatch)
// ============================================================================

static CallGraph buildTestGraph() {
  CallGraph g;

  g.addNode({"main", "main.cpp", 10, true, false, ""});
  g.addNode({"processFile", "process.cpp", 20, false, false, ""});
  g.addNode({"readData", "io.cpp", 30, false, false, ""});
  g.addNode({"writeLog", "log.cpp", 40, false, false, ""});
  g.addNode({"cleanup", "main.cpp", 50, false, false, ""});
  g.addNode({"Base::doWork", "base.cpp", 60, false, true, "Base"});
  g.addNode({"Derived::doWork", "derived.cpp", 70, false, true, "Derived"});
  g.addNode({"orphanFunc", "orphan.cpp", 80, false, false, ""});

  g.addEdge({"main", "processFile", EdgeKind::DirectCall,
             Confidence::Proven, "main.cpp:11:3", 0});
  g.addEdge({"main", "cleanup", EdgeKind::DirectCall,
             Confidence::Proven, "main.cpp:12:3", 0});
  g.addEdge({"processFile", "readData", EdgeKind::DirectCall,
             Confidence::Proven, "process.cpp:21:5", 0});
  g.addEdge({"processFile", "writeLog", EdgeKind::DirectCall,
             Confidence::Plausible, "process.cpp:22:5", 0});
  g.addEdge({"main", "Base::doWork", EdgeKind::VirtualDispatch,
             Confidence::Plausible, "main.cpp:13:3", 0});

  g.addDerivedClass("Base", "Derived");
  g.addMethodOverride("Base::doWork", "Derived::doWork");

  return g;
}

// Helper: build a test control flow index with a protected and unprotected call.
static ControlFlowIndex buildTestCfIndex() {
  ControlFlowIndex idx;

  // processFile -> readData is inside a try/catch.
  {
    CallSiteContext ctx;
    ctx.callerName = "processFile";
    ctx.calleeName = "readData";
    ctx.callSite = "process.cpp:21:5";
    TryCatchScope scope;
    scope.tryLocation = "process.cpp:20:3";
    scope.enclosingFunction = "processFile";
    scope.nestingDepth = 0;
    CatchHandlerInfo handler;
    handler.caughtType = "std::exception";
    handler.location = "process.cpp:25:3";
    scope.handlers.push_back(std::move(handler));
    ctx.enclosingTryCatches.push_back(std::move(scope));
    idx.addCallSiteContext(std::move(ctx));
  }

  // main -> processFile is NOT inside a try/catch.
  {
    CallSiteContext ctx;
    ctx.callerName = "main";
    ctx.calleeName = "processFile";
    ctx.callSite = "main.cpp:11:3";
    idx.addCallSiteContext(std::move(ctx));
  }

  return idx;
}

// Helper: extract the text from an MCP tool result.
static llvm::json::Object parseToolResult(const llvm::json::Value &result) {
  auto *obj = result.getAsObject();
  REQUIRE(obj != nullptr);
  auto *content = obj->getArray("content");
  REQUIRE(content != nullptr);
  REQUIRE(content->size() >= 1);
  auto *first = (*content)[0].getAsObject();
  REQUIRE(first != nullptr);
  auto text = first->getString("text");
  REQUIRE(text.has_value());

  auto parsed = llvm::json::parse(*text);
  REQUIRE(static_cast<bool>(parsed));
  auto *parsedObj = parsed->getAsObject();
  REQUIRE(parsedObj != nullptr);
  return std::move(*parsedObj);
}

static bool isErrorResult(const llvm::json::Value &result) {
  auto *obj = result.getAsObject();
  if (!obj)
    return false;
  if (auto b = obj->getBoolean("isError"))
    return *b;
  return false;
}

// ============================================================================
// Protocol tests
// ============================================================================

TEST_CASE("McpRequest isNotification", "[mcp][protocol]") {
  McpRequest req;

  SECTION("null id is a notification") {
    req.id = nullptr;
    CHECK(req.isNotification());
  }

  SECTION("integer id is not a notification") {
    req.id = 1;
    CHECK_FALSE(req.isNotification());
  }

  SECTION("string id is not a notification") {
    req.id = "abc";
    CHECK_FALSE(req.isNotification());
  }
}

TEST_CASE("readRequest parses Content-Length framed messages",
          "[mcp][protocol]") {
  SECTION("valid request") {
    std::string input =
        "Content-Length: 58\r\n"
        "\r\n"
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}";

    FILE *f = fmemopen(const_cast<char *>(input.data()), input.size(), "r");
    REQUIRE(f != nullptr);

    auto req = readRequest(f, llvm::errs());
    std::fclose(f);

    REQUIRE(req.has_value());
    CHECK(req->method == "initialize");
    auto id = req->id.getAsInteger();
    REQUIRE(id.has_value());
    CHECK(*id == 1);
  }

  SECTION("EOF returns nullopt") {
    std::string input;
    FILE *f = fmemopen(const_cast<char *>(input.data()), 0, "r");
    // fmemopen with size 0 may return NULL on some platforms.
    if (!f) {
      // Just verify we handle it.
      CHECK(true);
      return;
    }
    auto req = readRequest(f, llvm::errs());
    std::fclose(f);
    CHECK_FALSE(req.has_value());
  }

  SECTION("notification has null id") {
    std::string input =
        "Content-Length: 54\r\n"
        "\r\n"
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}";

    FILE *f = fmemopen(const_cast<char *>(input.data()), input.size(), "r");
    REQUIRE(f != nullptr);

    auto req = readRequest(f, llvm::errs());
    std::fclose(f);

    REQUIRE(req.has_value());
    CHECK(req->method == "notifications/initialized");
    CHECK(req->isNotification());
  }

  SECTION("Content-Length framing sets ContentLength write mode") {
    setActiveFraming(McpFraming::Newline);
    std::string input =
        "Content-Length: 58\r\n"
        "\r\n"
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}";

    FILE *f = fmemopen(const_cast<char *>(input.data()), input.size(), "r");
    REQUIRE(f != nullptr);
    auto req = readRequest(f, llvm::errs());
    std::fclose(f);

    REQUIRE(req.has_value());
    CHECK(activeFraming() == McpFraming::ContentLength);
    setActiveFraming(McpFraming::Newline);
  }
}

TEST_CASE("readRequest parses newline-delimited messages (MCP stdio framing)",
          "[mcp][protocol]") {
  SECTION("single request") {
    std::string input =
        "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/list\",\"params\":{}}\n";

    FILE *f = fmemopen(const_cast<char *>(input.data()), input.size(), "r");
    REQUIRE(f != nullptr);

    auto req = readRequest(f, llvm::errs());
    std::fclose(f);

    REQUIRE(req.has_value());
    CHECK(req->method == "tools/list");
    auto id = req->id.getAsInteger();
    REQUIRE(id.has_value());
    CHECK(*id == 7);
    CHECK(activeFraming() == McpFraming::Newline);
  }

  SECTION("two requests back to back") {
    std::string input =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\"}\n"
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}\n";

    FILE *f = fmemopen(const_cast<char *>(input.data()), input.size(), "r");
    REQUIRE(f != nullptr);

    auto first = readRequest(f, llvm::errs());
    auto second = readRequest(f, llvm::errs());
    std::fclose(f);

    REQUIRE(first.has_value());
    CHECK(first->method == "initialize");
    REQUIRE(second.has_value());
    CHECK(second->method == "tools/list");
  }

  SECTION("blank lines between messages are tolerated") {
    std::string input =
        "\n\r\n"
        "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/list\"}\n"
        "\n";

    FILE *f = fmemopen(const_cast<char *>(input.data()), input.size(), "r");
    REQUIRE(f != nullptr);

    auto req = readRequest(f, llvm::errs());
    std::fclose(f);

    REQUIRE(req.has_value());
    CHECK(req->method == "tools/list");
  }

  SECTION("CRLF line ending is stripped") {
    std::string input =
        "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/list\"}\r\n";

    FILE *f = fmemopen(const_cast<char *>(input.data()), input.size(), "r");
    REQUIRE(f != nullptr);

    auto req = readRequest(f, llvm::errs());
    std::fclose(f);

    REQUIRE(req.has_value());
    CHECK(req->method == "tools/list");
  }

  SECTION("trailing newline missing (EOF terminates the line)") {
    std::string input =
        "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/list\"}";

    FILE *f = fmemopen(const_cast<char *>(input.data()), input.size(), "r");
    REQUIRE(f != nullptr);

    auto req = readRequest(f, llvm::errs());
    std::fclose(f);

    REQUIRE(req.has_value());
    CHECK(req->method == "tools/list");
  }

  SECTION("malformed line is skipped, next message still read") {
    std::string input =
        "{\"jsonrpc\":\"2.0\",,,garbage\n"
        "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"tools/list\"}\n";

    FILE *f = fmemopen(const_cast<char *>(input.data()), input.size(), "r");
    REQUIRE(f != nullptr);

    auto req = readRequest(f, llvm::errs());
    std::fclose(f);

    REQUIRE(req.has_value());
    CHECK(req->method == "tools/list");
    auto id = req->id.getAsInteger();
    REQUIRE(id.has_value());
    CHECK(*id == 6);
  }

  SECTION("framing can alternate between messages") {
    std::string input =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\"}\n"
        "Content-Length: 46\r\n"
        "\r\n"
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}";

    FILE *f = fmemopen(const_cast<char *>(input.data()), input.size(), "r");
    REQUIRE(f != nullptr);

    auto first = readRequest(f, llvm::errs());
    REQUIRE(first.has_value());
    CHECK(activeFraming() == McpFraming::Newline);

    auto second = readRequest(f, llvm::errs());
    std::fclose(f);
    REQUIRE(second.has_value());
    CHECK(second->method == "tools/list");
    CHECK(activeFraming() == McpFraming::ContentLength);
    setActiveFraming(McpFraming::Newline);
  }
}

// ============================================================================
// Tool registration tests
// ============================================================================

TEST_CASE("getRegisteredTools returns all 16 tools", "[mcp][tools]") {
  auto tools = getRegisteredTools();
  CHECK(tools.size() == 16);

  // Verify tool names.
  std::set<std::string> names;
  for (auto &t : tools)
    names.insert(t.name);

  CHECK(names.count("lookup_function") == 1);
  CHECK(names.count("get_callees") == 1);
  CHECK(names.count("get_callers") == 1);
  CHECK(names.count("find_call_chain") == 1);
  CHECK(names.count("query_exception_safety") == 1);
  CHECK(names.count("query_call_site_context") == 1);
  CHECK(names.count("query_raii_scopes_at_callsite") == 1);
  CHECK(names.count("query_locks_held") == 1);
  CHECK(names.count("query_same_lock") == 1);
  CHECK(names.count("analyze_dead_code") == 1);
  CHECK(names.count("get_class_hierarchy") == 1);
  CHECK(names.count("list_entry_points") == 1);
  CHECK(names.count("graph_summary") == 1);
  CHECK(names.count("list_callback_sites") == 1);
  CHECK(names.count("list_concurrency_entry_points") == 1);
  CHECK(names.count("reindex_tu") == 1);
}

// ============================================================================
// Tool handler tests
// ============================================================================

TEST_CASE("lookup_function tool", "[mcp][tools]") {
  auto graph = buildTestGraph();
  ControlFlowIndex cfIndex;
  ControlFlowOracle oracle(graph, cfIndex);
  std::vector<std::string> eps = {"main"};
  McpToolContext ctx{graph, oracle, cfIndex, eps};

  auto tools = getRegisteredTools();
  McpToolHandler handler;
  for (auto &t : tools) {
    if (t.name == "lookup_function") {
      handler = t.handler;
      break;
    }
  }
  REQUIRE(handler);

  SECTION("existing function") {
    llvm::json::Object args;
    args["name"] = "processFile";
    auto result = handler(args, ctx);
    CHECK_FALSE(isErrorResult(result));

    auto obj = parseToolResult(result);
    CHECK(obj.getString("qualifiedName") == "processFile");
    CHECK(obj.getString("file") == "process.cpp");
    CHECK(obj.getInteger("line") == 20);
  }

  SECTION("missing function") {
    llvm::json::Object args;
    args["name"] = "nonexistent";
    auto result = handler(args, ctx);
    CHECK(isErrorResult(result));
  }

  SECTION("missing parameter") {
    llvm::json::Object args;
    auto result = handler(args, ctx);
    CHECK(isErrorResult(result));
  }
}

TEST_CASE("get_callees tool", "[mcp][tools]") {
  auto graph = buildTestGraph();
  ControlFlowIndex cfIndex;
  ControlFlowOracle oracle(graph, cfIndex);
  std::vector<std::string> eps = {"main"};
  McpToolContext ctx{graph, oracle, cfIndex, eps};

  auto tools = getRegisteredTools();
  McpToolHandler handler;
  for (auto &t : tools) {
    if (t.name == "get_callees") {
      handler = t.handler;
      break;
    }
  }
  REQUIRE(handler);

  SECTION("main has 3 callees") {
    llvm::json::Object args;
    args["name"] = "main";
    auto result = handler(args, ctx);
    auto obj = parseToolResult(result);
    CHECK(obj.getInteger("calleeCount") == 3);
  }

  SECTION("filter by edge kind") {
    llvm::json::Object args;
    args["name"] = "main";
    llvm::json::Array kinds;
    kinds.push_back("DirectCall");
    args["edge_kinds"] = std::move(kinds);
    auto result = handler(args, ctx);
    auto obj = parseToolResult(result);
    CHECK(obj.getInteger("calleeCount") == 2); // processFile, cleanup
  }

  SECTION("filter by min confidence") {
    llvm::json::Object args;
    args["name"] = "processFile";
    args["min_confidence"] = "Proven";
    auto result = handler(args, ctx);
    auto obj = parseToolResult(result);
    CHECK(obj.getInteger("calleeCount") == 1); // only readData is Proven
  }
}

TEST_CASE("get_callers tool", "[mcp][tools]") {
  auto graph = buildTestGraph();
  ControlFlowIndex cfIndex;
  ControlFlowOracle oracle(graph, cfIndex);
  std::vector<std::string> eps = {"main"};
  McpToolContext ctx{graph, oracle, cfIndex, eps};

  auto tools = getRegisteredTools();
  McpToolHandler handler;
  for (auto &t : tools) {
    if (t.name == "get_callers") {
      handler = t.handler;
      break;
    }
  }
  REQUIRE(handler);

  SECTION("processFile has 1 caller") {
    llvm::json::Object args;
    args["name"] = "processFile";
    auto result = handler(args, ctx);
    auto obj = parseToolResult(result);
    CHECK(obj.getInteger("callerCount") == 1);
  }

  SECTION("orphanFunc has 0 callers") {
    llvm::json::Object args;
    args["name"] = "orphanFunc";
    auto result = handler(args, ctx);
    auto obj = parseToolResult(result);
    CHECK(obj.getInteger("callerCount") == 0);
  }
}

TEST_CASE("find_call_chain tool", "[mcp][tools]") {
  auto graph = buildTestGraph();
  ControlFlowIndex cfIndex;
  ControlFlowOracle oracle(graph, cfIndex);
  std::vector<std::string> eps = {"main"};
  McpToolContext ctx{graph, oracle, cfIndex, eps};

  auto tools = getRegisteredTools();
  McpToolHandler handler;
  for (auto &t : tools) {
    if (t.name == "find_call_chain") {
      handler = t.handler;
      break;
    }
  }
  REQUIRE(handler);

  SECTION("chain from main to readData carries edge metadata") {
    llvm::json::Object args;
    args["to"] = "readData";
    auto result = handler(args, ctx);
    auto obj = parseToolResult(result);
    auto pathCount = obj.getInteger("pathCount");
    REQUIRE(pathCount.has_value());
    CHECK(*pathCount >= 1);

    auto *paths = obj.getArray("paths");
    REQUIRE(paths != nullptr);
    // First path is main -> processFile -> readData: 2 hops.
    auto *firstPath = (*paths)[0].getAsArray();
    REQUIRE(firstPath != nullptr);
    CHECK(firstPath->size() == 2);

    auto *firstHop = (*firstPath)[0].getAsObject();
    REQUIRE(firstHop != nullptr);
    CHECK(firstHop->getString("from") == "main");
    CHECK(firstHop->getString("to") == "processFile");
    CHECK(firstHop->getString("kind") == "DirectCall");
    CHECK(firstHop->getString("confidence") == "Proven");
    CHECK(firstHop->getString("callSite") == "main.cpp:11:3");

    auto *secondHop = (*firstPath)[1].getAsObject();
    REQUIRE(secondHop != nullptr);
    CHECK(secondHop->getString("from") == "processFile");
    CHECK(secondHop->getString("to") == "readData");
  }

  SECTION("no chain to orphan") {
    llvm::json::Object args;
    args["to"] = "orphanFunc";
    auto result = handler(args, ctx);
    auto obj = parseToolResult(result);
    CHECK(obj.getInteger("pathCount") == 0);
  }

  SECTION("explicit from parameter") {
    llvm::json::Object args;
    args["from"] = "processFile";
    args["to"] = "readData";
    auto result = handler(args, ctx);
    auto obj = parseToolResult(result);
    CHECK(obj.getInteger("pathCount") == 1);
  }

  SECTION("min_confidence=Proven prunes chains with Plausible hops") {
    // processFile -> writeLog is Plausible. Requiring Proven should block
    // every chain through writeLog.
    llvm::json::Object args;
    args["to"] = "writeLog";
    args["min_confidence"] = "Proven";
    auto result = handler(args, ctx);
    auto obj = parseToolResult(result);
    CHECK(obj.getInteger("pathCount") == 0);
  }
}

TEST_CASE("query_call_site_context tool", "[mcp][tools]") {
  auto graph = buildTestGraph();
  auto cfIndex = buildTestCfIndex();
  ControlFlowOracle oracle(graph, cfIndex);
  std::vector<std::string> eps = {"main"};
  McpToolContext ctx{graph, oracle, cfIndex, eps};

  auto tools = getRegisteredTools();
  McpToolHandler handler;
  for (auto &t : tools) {
    if (t.name == "query_call_site_context") {
      handler = t.handler;
      break;
    }
  }
  REQUIRE(handler);

  SECTION("protected call site") {
    llvm::json::Object args;
    args["call_site"] = "process.cpp:21:5";
    auto result = handler(args, ctx);
    auto obj = parseToolResult(result);
    CHECK(obj.getBoolean("isUnderTryCatch") == true);
    CHECK(obj.getString("caller") == "processFile");
    CHECK(obj.getString("callee") == "readData");
  }

  SECTION("unprotected call site") {
    llvm::json::Object args;
    args["call_site"] = "main.cpp:11:3";
    auto result = handler(args, ctx);
    auto obj = parseToolResult(result);
    CHECK(obj.getBoolean("isUnderTryCatch") == false);
  }
}

TEST_CASE("analyze_dead_code tool", "[mcp][tools]") {
  auto graph = buildTestGraph();
  ControlFlowIndex cfIndex;
  ControlFlowOracle oracle(graph, cfIndex);
  std::vector<std::string> eps = {"main"};
  McpToolContext ctx{graph, oracle, cfIndex, eps};

  auto tools = getRegisteredTools();
  McpToolHandler handler;
  for (auto &t : tools) {
    if (t.name == "analyze_dead_code") {
      handler = t.handler;
      break;
    }
  }
  REQUIRE(handler);

  SECTION("orphanFunc is dead") {
    llvm::json::Object args;
    auto result = handler(args, ctx);
    auto obj = parseToolResult(result);

    auto deadCount = obj.getInteger("deadCount");
    REQUIRE(deadCount.has_value());
    CHECK(*deadCount >= 1);

    // Verify orphanFunc is in the dead list.
    auto *deadArr = obj.getArray("dead");
    REQUIRE(deadArr != nullptr);
    bool foundOrphan = false;
    for (auto &entry : *deadArr) {
      if (auto *entryObj = entry.getAsObject()) {
        if (auto name = entryObj->getString("name")) {
          if (*name == "orphanFunc")
            foundOrphan = true;
        }
      }
    }
    CHECK(foundOrphan);
  }
}

TEST_CASE("get_class_hierarchy tool", "[mcp][tools]") {
  auto graph = buildTestGraph();
  ControlFlowIndex cfIndex;
  ControlFlowOracle oracle(graph, cfIndex);
  std::vector<std::string> eps = {"main"};
  McpToolContext ctx{graph, oracle, cfIndex, eps};

  auto tools = getRegisteredTools();
  McpToolHandler handler;
  for (auto &t : tools) {
    if (t.name == "get_class_hierarchy") {
      handler = t.handler;
      break;
    }
  }
  REQUIRE(handler);

  SECTION("Base has Derived") {
    llvm::json::Object args;
    args["class_name"] = "Base";
    auto result = handler(args, ctx);
    auto obj = parseToolResult(result);
    CHECK(obj.getInteger("derivedClassCount") == 1);
  }

  SECTION("with overrides") {
    llvm::json::Object args;
    args["class_name"] = "Base";
    args["include_overrides"] = true;
    auto result = handler(args, ctx);
    auto obj = parseToolResult(result);

    auto *overrides = obj.getArray("virtualMethodOverrides");
    REQUIRE(overrides != nullptr);
    CHECK(overrides->size() >= 1);
  }

  SECTION("nonexistent class returns empty") {
    llvm::json::Object args;
    args["class_name"] = "NoSuchClass";
    auto result = handler(args, ctx);
    auto obj = parseToolResult(result);
    CHECK(obj.getInteger("derivedClassCount") == 0);
  }
}

// ============================================================================
// include_confidences filter (regression for inclusive-min surprise)
// ============================================================================

TEST_CASE("get_callees with include_confidences selects exact tiers",
          "[mcp][tools]") {
  auto graph = buildTestGraph();
  ControlFlowIndex cfIndex;
  ControlFlowOracle oracle(graph, cfIndex);
  std::vector<std::string> eps = {"main"};
  McpToolContext ctx{graph, oracle, cfIndex, eps};

  auto tools = getRegisteredTools();
  McpToolHandler handler;
  for (auto &t : tools) {
    if (t.name == "get_callees") {
      handler = t.handler;
      break;
    }
  }
  REQUIRE(handler);

  SECTION("only Plausible excludes Proven") {
    llvm::json::Object args;
    args["name"] = "processFile";
    llvm::json::Array confs;
    confs.push_back("Plausible");
    args["include_confidences"] = std::move(confs);
    auto result = handler(args, ctx);
    auto obj = parseToolResult(result);
    CHECK(obj.getInteger("calleeCount") == 1); // only writeLog (Plausible)
  }

  SECTION("only Proven excludes Plausible") {
    llvm::json::Object args;
    args["name"] = "processFile";
    llvm::json::Array confs;
    confs.push_back("Proven");
    args["include_confidences"] = std::move(confs);
    auto result = handler(args, ctx);
    auto obj = parseToolResult(result);
    CHECK(obj.getInteger("calleeCount") == 1); // only readData (Proven)
  }

  SECTION("invalid value yields error") {
    llvm::json::Object args;
    args["name"] = "processFile";
    llvm::json::Array confs;
    confs.push_back("NotATier");
    args["include_confidences"] = std::move(confs);
    auto result = handler(args, ctx);
    CHECK(isErrorResult(result));
  }
}

// ============================================================================
// analyze_dead_code filtering and pagination
// ============================================================================

TEST_CASE("analyze_dead_code filters system headers and paginates",
          "[mcp][tools]") {
  auto graph = buildTestGraph();
  // Add synthetic dead nodes in system and project locations.
  graph.addNode({"std::sys_dead", "/usr/include/fake.h", 10, false, false, ""});
  graph.addNode({"project_dead_a", "src/a.cpp", 11, false, false, ""});
  graph.addNode({"project_dead_b", "src/b.cpp", 12, false, false, ""});
  ControlFlowIndex cfIndex;
  ControlFlowOracle oracle(graph, cfIndex);
  std::vector<std::string> eps = {"main"};
  McpToolContext ctx{graph, oracle, cfIndex, eps};

  auto tools = getRegisteredTools();
  McpToolHandler handler;
  for (auto &t : tools) {
    if (t.name == "analyze_dead_code") {
      handler = t.handler;
      break;
    }
  }
  REQUIRE(handler);

  auto countNamed = [](const llvm::json::Array &arr, const std::string &name) {
    int n = 0;
    for (auto &v : arr) {
      if (auto *o = v.getAsObject()) {
        if (auto s = o->getString("name"))
          if (*s == name)
            ++n;
      }
    }
    return n;
  };

  SECTION("system functions excluded by default") {
    llvm::json::Object args;
    auto result = handler(args, ctx);
    auto obj = parseToolResult(result);
    auto *dead = obj.getArray("dead");
    REQUIRE(dead != nullptr);
    CHECK(countNamed(*dead, "std::sys_dead") == 0);
    CHECK(countNamed(*dead, "project_dead_a") == 1);
  }

  SECTION("include_system surfaces system functions") {
    llvm::json::Object args;
    args["include_system"] = true;
    auto result = handler(args, ctx);
    auto obj = parseToolResult(result);
    auto *dead = obj.getArray("dead");
    REQUIRE(dead != nullptr);
    CHECK(countNamed(*dead, "std::sys_dead") == 1);
  }

  SECTION("limit and offset paginate") {
    llvm::json::Object args;
    args["limit"] = 1;
    auto result = handler(args, ctx);
    auto obj = parseToolResult(result);
    auto totalDead = obj.getInteger("totalDead");
    auto deadCount = obj.getInteger("deadCount");
    REQUIRE(totalDead.has_value());
    REQUIRE(deadCount.has_value());
    CHECK(*totalDead >= 2);
    CHECK(*deadCount == 1);
    CHECK(obj.getBoolean("truncated") == true);
  }

  SECTION("name_prefix filters results") {
    llvm::json::Object args;
    args["name_prefix"] = "project_dead_";
    auto result = handler(args, ctx);
    auto obj = parseToolResult(result);
    auto *dead = obj.getArray("dead");
    REQUIRE(dead != nullptr);
    CHECK(countNamed(*dead, "project_dead_a") == 1);
    CHECK(countNamed(*dead, "project_dead_b") == 1);
    CHECK(countNamed(*dead, "orphanFunc") == 0);
  }
}

// ============================================================================
// query_call_site_context validation and not-found surfacing
// ============================================================================

TEST_CASE("query_call_site_context surfaces malformed and unindexed input",
          "[mcp][tools]") {
  auto graph = buildTestGraph();
  auto cfIndex = buildTestCfIndex();
  ControlFlowOracle oracle(graph, cfIndex);
  std::vector<std::string> eps = {"main"};
  McpToolContext ctx{graph, oracle, cfIndex, eps};

  auto tools = getRegisteredTools();
  McpToolHandler handler;
  for (auto &t : tools) {
    if (t.name == "query_call_site_context") {
      handler = t.handler;
      break;
    }
  }
  REQUIRE(handler);

  SECTION("malformed call_site returns isError") {
    llvm::json::Object args;
    args["call_site"] = "nope";
    auto result = handler(args, ctx);
    CHECK(isErrorResult(result));
  }

  SECTION("valid format but unindexed site returns isError") {
    llvm::json::Object args;
    args["call_site"] = "src/unknown.cpp:99:9";
    auto result = handler(args, ctx);
    CHECK(isErrorResult(result));
  }

  SECTION("non-numeric line/col returns isError") {
    llvm::json::Object args;
    args["call_site"] = "foo.cpp:abc:5";
    auto result = handler(args, ctx);
    CHECK(isErrorResult(result));
  }
}

// ============================================================================
// list_entry_points and graph_summary introspection tools
// ============================================================================

TEST_CASE("list_entry_points returns configured entries", "[mcp][tools]") {
  auto graph = buildTestGraph();
  ControlFlowIndex cfIndex;
  ControlFlowOracle oracle(graph, cfIndex);
  std::vector<std::string> eps = {"main"};
  McpToolContext ctx{graph, oracle, cfIndex, eps};

  auto tools = getRegisteredTools();
  McpToolHandler handler;
  for (auto &t : tools) {
    if (t.name == "list_entry_points") {
      handler = t.handler;
      break;
    }
  }
  REQUIRE(handler);

  llvm::json::Object args;
  auto result = handler(args, ctx);
  auto obj = parseToolResult(result);
  CHECK(obj.getInteger("count") == 1);
  auto *entries = obj.getArray("entryPoints");
  REQUIRE(entries != nullptr);
  REQUIRE(entries->size() == 1);
  auto *first = (*entries)[0].getAsObject();
  REQUIRE(first != nullptr);
  CHECK(first->getString("name") == "main");
  CHECK(first->getString("file") == "main.cpp");
}

TEST_CASE("graph_summary produces histograms and top-N fanout",
          "[mcp][tools]") {
  auto graph = buildTestGraph();
  ControlFlowIndex cfIndex;
  ControlFlowOracle oracle(graph, cfIndex);
  std::vector<std::string> eps = {"main"};
  McpToolContext ctx{graph, oracle, cfIndex, eps};

  auto tools = getRegisteredTools();
  McpToolHandler handler;
  for (auto &t : tools) {
    if (t.name == "graph_summary") {
      handler = t.handler;
      break;
    }
  }
  REQUIRE(handler);

  llvm::json::Object args;
  auto result = handler(args, ctx);
  auto obj = parseToolResult(result);

  CHECK(obj.getInteger("nodeCount") == 8);
  CHECK(obj.getInteger("edgeCount") == 5);
  CHECK(obj.getInteger("entryPointCount") == 1);

  auto *confHist = obj.getObject("confidenceHistogram");
  REQUIRE(confHist != nullptr);
  CHECK(confHist->getInteger("Proven") == 3);
  CHECK(confHist->getInteger("Plausible") == 2);
  CHECK(confHist->getInteger("Unknown") == 0);

  auto *kindHist = obj.getObject("edgeKindHistogram");
  REQUIRE(kindHist != nullptr);
  CHECK(kindHist->getInteger("DirectCall") == 4);
  CHECK(kindHist->getInteger("VirtualDispatch") == 1);

  auto *topCallers = obj.getArray("topFanoutCallers");
  REQUIRE(topCallers != nullptr);
  REQUIRE(topCallers->size() >= 1);
  auto *topCaller = (*topCallers)[0].getAsObject();
  REQUIRE(topCaller != nullptr);
  CHECK(topCaller->getString("qualifiedName") == "main");
  CHECK(topCaller->getInteger("count") == 3);
}

// ============================================================================
// Callback/concurrency MCP tool tests
//
// Unit-level coverage for the lambda and thread-entry edges that
// CallGraphBuilder emits for the deep_chains fixture's Chain C. These tests
// exercise the MCP tool surface directly against a synthetic graph; the
// builder-side integration tests live in test_deep_chains.cpp.
// ============================================================================

namespace {
McpToolHandler findHandler(const std::vector<McpToolEntry> &tools,
                           llvm::StringRef name) {
  for (auto &t : tools)
    if (t.name == name)
      return t.handler;
  return {};
}
} // namespace

TEST_CASE("get_callees surfaces ThreadEntry edges with execution context",
          "[mcp][tools][concurrency]") {
  auto graph = buildChainCGraph();
  ControlFlowIndex cfIndex;
  ControlFlowOracle oracle(graph, cfIndex);
  std::vector<std::string> eps = {"main"};
  McpToolContext ctx{graph, oracle, cfIndex, eps};

  auto tools = getRegisteredTools();
  auto handler = findHandler(tools, "get_callees");
  REQUIRE(handler);

  SECTION("filter by ThreadEntry returns all three concurrency targets") {
    llvm::json::Object args;
    args["name"] = "runChainC";
    llvm::json::Array kinds;
    kinds.push_back("ThreadEntry");
    args["edge_kinds"] = std::move(kinds);
    auto result = handler(args, ctx);
    auto obj = parseToolResult(result);

    auto calleeCount = obj.getInteger("calleeCount");
    REQUIRE(calleeCount.has_value());
    CHECK(*calleeCount == 3);

    auto *callees = obj.getArray("callees");
    REQUIRE(callees != nullptr);

    std::set<std::string> targets;
    std::set<std::string> contexts;
    for (auto &v : *callees) {
      auto *o = v.getAsObject();
      REQUIRE(o != nullptr);
      CHECK(o->getString("kind") == "ThreadEntry");
      if (auto t = o->getString("calleeName"))
        targets.insert(t->str());
      if (auto c = o->getString("executionContext"))
        contexts.insert(c->str());
    }
    CHECK(targets.count("worker_thread_entry") == 1);
    CHECK(targets.count("compute_hash") == 1);
    // At least one synthetic lambda node among the ThreadEntry targets.
    bool anyLambda = false;
    for (auto &t : targets)
      if (llvm::StringRef(t).starts_with("lambda#"))
        anyLambda = true;
    CHECK(anyLambda);

    CHECK(contexts.count("ThreadSpawn") == 1);
    CHECK(contexts.count("AsyncTask") == 1);
  }

  SECTION("execution_contexts filter narrows to AsyncTask") {
    llvm::json::Object args;
    args["name"] = "runChainC";
    llvm::json::Array ctxs;
    ctxs.push_back("AsyncTask");
    args["execution_contexts"] = std::move(ctxs);
    auto result = handler(args, ctx);
    auto obj = parseToolResult(result);

    CHECK(obj.getInteger("calleeCount") == 1);
    auto *callees = obj.getArray("callees");
    REQUIRE(callees != nullptr);
    REQUIRE(callees->size() == 1);
    auto *e0 = (*callees)[0].getAsObject();
    REQUIRE(e0 != nullptr);
    CHECK(e0->getString("calleeName") == "compute_hash");
    CHECK(e0->getString("executionContext") == "AsyncTask");
  }

  SECTION("invalid execution_contexts value yields error") {
    llvm::json::Object args;
    args["name"] = "runChainC";
    llvm::json::Array ctxs;
    ctxs.push_back("GreenThreadSomething");
    args["execution_contexts"] = std::move(ctxs);
    auto result = handler(args, ctx);
    CHECK(isErrorResult(result));
  }

  SECTION("synchronous edges omit executionContext from the response") {
    // DirectCall from main -> runChainC is Synchronous; edgeToJson must not
    // emit an "executionContext" key for it (byte-compat with older clients).
    llvm::json::Object args;
    args["name"] = "main";
    auto result = handler(args, ctx);
    auto obj = parseToolResult(result);
    auto *callees = obj.getArray("callees");
    REQUIRE(callees != nullptr);
    REQUIRE(callees->size() >= 1);
    auto *first = (*callees)[0].getAsObject();
    REQUIRE(first != nullptr);
    CHECK(first->getString("calleeName") == "runChainC");
    CHECK_FALSE(first->getString("executionContext").has_value());
  }
}

TEST_CASE("list_callback_sites groups callback edges by target",
          "[mcp][tools][callbacks]") {
  auto graph = buildChainCGraph();
  ControlFlowIndex cfIndex;
  ControlFlowOracle oracle(graph, cfIndex);
  std::vector<std::string> eps = {"main"};
  McpToolContext ctx{graph, oracle, cfIndex, eps};

  auto tools = getRegisteredTools();
  auto handler = findHandler(tools, "list_callback_sites");
  REQUIRE(handler);

  SECTION("returns FunctionPointer and LambdaCall targets grouped by callee") {
    llvm::json::Object args;
    auto result = handler(args, ctx);
    auto obj = parseToolResult(result);

    auto targetCount = obj.getInteger("targetCount");
    REQUIRE(targetCount.has_value());
    CHECK(*targetCount >= 2);

    auto *targets = obj.getArray("targets");
    REQUIRE(targets != nullptr);

    std::set<std::string> names;
    std::set<std::string> kindsSeen;
    for (auto &v : *targets) {
      auto *o = v.getAsObject();
      REQUIRE(o != nullptr);
      if (auto t = o->getString("target"))
        names.insert(t->str());
      auto *sites = o->getArray("sites");
      REQUIRE(sites != nullptr);
      for (auto &s : *sites) {
        if (auto *so = s.getAsObject())
          if (auto k = so->getString("kind"))
            kindsSeen.insert(k->str());
      }
    }
    CHECK(names.count("cbs::startupHook") == 1);
    bool anyLambda = false;
    for (auto &n : names)
      if (llvm::StringRef(n).starts_with("lambda#"))
        anyLambda = true;
    CHECK(anyLambda);
    CHECK(kindsSeen.count("FunctionPointer") == 1);
    CHECK(kindsSeen.count("LambdaCall") == 1);
    // ThreadEntry is concurrency, not a callback-site kind — not listed here.
    CHECK(kindsSeen.count("ThreadEntry") == 0);
  }

  SECTION("target_prefix narrows results to lambda nodes") {
    llvm::json::Object args;
    args["target_prefix"] = "lambda#";
    auto result = handler(args, ctx);
    auto obj = parseToolResult(result);
    auto *targets = obj.getArray("targets");
    REQUIRE(targets != nullptr);
    REQUIRE(targets->size() >= 1);
    for (auto &v : *targets) {
      auto *o = v.getAsObject();
      REQUIRE(o != nullptr);
      auto t = o->getString("target");
      REQUIRE(t.has_value());
      CHECK(llvm::StringRef(*t).starts_with("lambda#"));
    }
  }
}

TEST_CASE("list_concurrency_entry_points enumerates ThreadEntry edges",
          "[mcp][tools][concurrency]") {
  auto graph = buildChainCGraph();
  ControlFlowIndex cfIndex;
  ControlFlowOracle oracle(graph, cfIndex);
  std::vector<std::string> eps = {"main"};
  McpToolContext ctx{graph, oracle, cfIndex, eps};

  auto tools = getRegisteredTools();
  auto handler = findHandler(tools, "list_concurrency_entry_points");
  REQUIRE(handler);

  SECTION("lists all spawners with correct execution contexts") {
    llvm::json::Object args;
    auto result = handler(args, ctx);
    auto obj = parseToolResult(result);

    auto count = obj.getInteger("count");
    REQUIRE(count.has_value());
    CHECK(*count == 3);

    auto *entries = obj.getArray("entries");
    REQUIRE(entries != nullptr);

    bool sawLambda = false;
    std::set<std::string> contexts;
    for (auto &v : *entries) {
      auto *o = v.getAsObject();
      REQUIRE(o != nullptr);
      CHECK(o->getString("spawner") == "runChainC");
      if (auto t = o->getString("target"))
        if (llvm::StringRef(*t).starts_with("lambda#"))
          sawLambda = true;
      if (auto c = o->getString("executionContext"))
        contexts.insert(c->str());
    }
    CHECK(sawLambda);
    CHECK(contexts.count("ThreadSpawn") == 1);
    CHECK(contexts.count("AsyncTask") == 1);
  }

  SECTION("execution_contexts filter restricts to AsyncTask") {
    llvm::json::Object args;
    llvm::json::Array ctxs;
    ctxs.push_back("AsyncTask");
    args["execution_contexts"] = std::move(ctxs);
    auto result = handler(args, ctx);
    auto obj = parseToolResult(result);

    CHECK(obj.getInteger("count") == 1);
    auto *entries = obj.getArray("entries");
    REQUIRE(entries != nullptr);
    REQUIRE(entries->size() == 1);
    auto *e0 = (*entries)[0].getAsObject();
    REQUIRE(e0 != nullptr);
    CHECK(e0->getString("target") == "compute_hash");
    CHECK(e0->getString("executionContext") == "AsyncTask");
  }

  SECTION("invalid execution_contexts value yields error") {
    llvm::json::Object args;
    llvm::json::Array ctxs;
    ctxs.push_back("NotARealContext");
    args["execution_contexts"] = std::move(ctxs);
    auto result = handler(args, ctx);
    CHECK(isErrorResult(result));
  }
}

TEST_CASE("find_call_chain propagates executionContext per hop",
          "[mcp][tools][concurrency]") {
  auto graph = buildChainCGraph();
  ControlFlowIndex cfIndex;
  ControlFlowOracle oracle(graph, cfIndex);
  std::vector<std::string> eps = {"main"};
  McpToolContext ctx{graph, oracle, cfIndex, eps};

  auto tools = getRegisteredTools();
  auto handler = findHandler(tools, "find_call_chain");
  REQUIRE(handler);

  llvm::json::Object args;
  args["to"] = "compute_hash";
  auto result = handler(args, ctx);
  auto obj = parseToolResult(result);

  auto pathCount = obj.getInteger("pathCount");
  REQUIRE(pathCount.has_value());
  REQUIRE(*pathCount >= 1);

  auto *paths = obj.getArray("paths");
  REQUIRE(paths != nullptr);
  auto *firstPath = (*paths)[0].getAsArray();
  REQUIRE(firstPath != nullptr);
  REQUIRE(firstPath->size() == 2);

  // First hop: main -> runChainC, DirectCall, no executionContext field.
  auto *hop0 = (*firstPath)[0].getAsObject();
  REQUIRE(hop0 != nullptr);
  CHECK(hop0->getString("from") == "main");
  CHECK(hop0->getString("to") == "runChainC");
  CHECK(hop0->getString("kind") == "DirectCall");
  CHECK_FALSE(hop0->getString("executionContext").has_value());

  // Second hop: runChainC -> compute_hash, ThreadEntry, AsyncTask.
  auto *hop1 = (*firstPath)[1].getAsObject();
  REQUIRE(hop1 != nullptr);
  CHECK(hop1->getString("from") == "runChainC");
  CHECK(hop1->getString("to") == "compute_hash");
  CHECK(hop1->getString("kind") == "ThreadEntry");
  CHECK(hop1->getString("confidence") == "Proven");
  CHECK(hop1->getString("executionContext") == "AsyncTask");
}

// ============================================================================
// Concurrency tools: query_raii_scopes_at_callsite, query_locks_held,
// query_same_lock
// ============================================================================

// Helper: pluck a named tool's handler from getRegisteredTools().
static McpToolHandler findHandler(const std::string &name) {
  auto tools = getRegisteredTools();
  for (auto &t : tools) {
    if (t.name == name)
      return t.handler;
  }
  return {};
}

// Helper: build a three-frame chain
//   entry() -> mid() -> leaf()
// where entry() holds `std::lock_guard<std::mutex> g(m)` live at the call
// to mid(), and mid() has no locks live when it calls leaf().
//
// The query_locks_held(leaf) expectation: one path [entry, mid, leaf] with
// lock `g` of type `std::lock_guard<std::mutex>` reported at the entry->mid
// call site.
static void buildThreeFrameChain(CallGraph &g, ControlFlowIndex &cf,
                                  const std::string &entryCallSite,
                                  const std::string &midCallSite) {
  g.addNode({"entry", "entry.cpp", 1, true, false, ""});
  g.addNode({"mid", "mid.cpp", 1, false, false, ""});
  g.addNode({"leaf", "leaf.cpp", 1, false, false, ""});
  g.addEdge({"entry", "mid", EdgeKind::DirectCall, Confidence::Proven,
             entryCallSite, 0});
  g.addEdge({"mid", "leaf", EdgeKind::DirectCall, Confidence::Proven,
             midCallSite, 0});

  // entry holds a lock when it calls mid.
  {
    CallSiteContext ctx;
    ctx.callerName = "entry";
    ctx.calleeName = "mid";
    ctx.callSite = entryCallSite;
    RaiiLocal g1;
    g1.typeName = "std::lock_guard<std::mutex>";
    g1.varName = "g";
    g1.declLocation = "entry.cpp:2:29";
    g1.kind = RaiiKind::Lock;
    ctx.liveRaiiLocals.push_back(std::move(g1));
    cf.addCallSiteContext(std::move(ctx));
  }
  // mid holds no locks when it calls leaf.
  {
    CallSiteContext ctx;
    ctx.callerName = "mid";
    ctx.calleeName = "leaf";
    ctx.callSite = midCallSite;
    cf.addCallSiteContext(std::move(ctx));
  }
}

TEST_CASE("query_raii_scopes_at_callsite returns locals and respects kinds",
          "[mcp][tools][concurrency]") {
  CallGraph graph;
  graph.addNode({"caller", "a.cpp", 1, true, false, ""});
  graph.addNode({"callee", "b.cpp", 1, false, false, ""});
  graph.addEdge({"caller", "callee", EdgeKind::DirectCall,
                 Confidence::Proven, "a.cpp:5:3", 0});

  ControlFlowIndex cf;
  {
    CallSiteContext ctx;
    ctx.callerName = "caller";
    ctx.calleeName = "callee";
    ctx.callSite = "a.cpp:5:3";
    RaiiLocal lock;
    lock.typeName = "std::lock_guard<std::mutex>";
    lock.varName = "g";
    lock.declLocation = "a.cpp:4:29";
    lock.kind = RaiiKind::Lock;
    RaiiLocal ptr;
    ptr.typeName = "std::unique_ptr<int>";
    ptr.varName = "p";
    ptr.declLocation = "a.cpp:3:10";
    ptr.kind = RaiiKind::SmartPtr;
    ctx.liveRaiiLocals.push_back(std::move(lock));
    ctx.liveRaiiLocals.push_back(std::move(ptr));
    cf.addCallSiteContext(std::move(ctx));
  }

  ControlFlowOracle oracle(graph, cf);
  std::vector<std::string> eps = {"caller"};
  McpToolContext ctx{graph, oracle, cf, eps};
  auto handler = findHandler("query_raii_scopes_at_callsite");
  REQUIRE(handler);

  SECTION("returns all kinds by default") {
    llvm::json::Object args;
    args["call_site"] = "a.cpp:5:3";
    auto result = handler(args, ctx);
    REQUIRE_FALSE(isErrorResult(result));
    auto obj = parseToolResult(result);
    auto *locals = obj.getArray("locals");
    REQUIRE(locals != nullptr);
    CHECK(locals->size() == 2);
  }

  SECTION("kinds=[lock] filters to lock only") {
    llvm::json::Object args;
    args["call_site"] = "a.cpp:5:3";
    llvm::json::Array kinds;
    kinds.push_back("lock");
    args["kinds"] = std::move(kinds);
    auto result = handler(args, ctx);
    REQUIRE_FALSE(isErrorResult(result));
    auto obj = parseToolResult(result);
    auto *locals = obj.getArray("locals");
    REQUIRE(locals != nullptr);
    REQUIRE(locals->size() == 1);
    auto *only = (*locals)[0].getAsObject();
    REQUIRE(only);
    CHECK(only->getString("kind") == "lock");
    CHECK(only->getString("varName") == "g");
  }

  SECTION("unknown kind returns error") {
    llvm::json::Object args;
    args["call_site"] = "a.cpp:5:3";
    llvm::json::Array kinds;
    kinds.push_back("bogus");
    args["kinds"] = std::move(kinds);
    auto result = handler(args, ctx);
    CHECK(isErrorResult(result));
  }

  SECTION("unindexed call site returns error") {
    llvm::json::Object args;
    args["call_site"] = "nowhere.cpp:1:1";
    auto result = handler(args, ctx);
    CHECK(isErrorResult(result));
  }
}

TEST_CASE("query_locks_held finds lock two frames up",
          "[mcp][tools][concurrency]") {
  CallGraph graph;
  ControlFlowIndex cf;
  buildThreeFrameChain(graph, cf, "entry.cpp:3:3", "mid.cpp:2:3");

  ControlFlowOracle oracle(graph, cf);
  std::vector<std::string> eps = {"entry"};
  McpToolContext ctx{graph, oracle, cf, eps};
  auto handler = findHandler("query_locks_held");
  REQUIRE(handler);

  llvm::json::Object args;
  args["function"] = "leaf";
  auto result = handler(args, ctx);
  REQUIRE_FALSE(isErrorResult(result));

  auto obj = parseToolResult(result);
  CHECK(obj.getString("function") == "leaf");
  CHECK(obj.getBoolean("truncated") == false);

  auto *paths = obj.getArray("paths");
  REQUIRE(paths != nullptr);
  REQUIRE(paths->size() == 1);

  auto *first = (*paths)[0].getAsObject();
  REQUIRE(first != nullptr);
  CHECK(first->getString("entryPoint") == "entry");

  auto *path = first->getArray("path");
  REQUIRE(path != nullptr);
  REQUIRE(path->size() == 3);
  CHECK((*path)[0].getAsString() == "entry");
  CHECK((*path)[1].getAsString() == "mid");
  CHECK((*path)[2].getAsString() == "leaf");

  auto *locks = first->getArray("locksHeld");
  REQUIRE(locks != nullptr);
  REQUIRE(locks->size() == 1);
  auto *lockObj = (*locks)[0].getAsObject();
  REQUIRE(lockObj != nullptr);
  CHECK(lockObj->getString("typeName") == "std::lock_guard<std::mutex>");
  CHECK(lockObj->getString("varName") == "g");
  CHECK(lockObj->getString("heldAt") == "entry.cpp:3:3");
}

TEST_CASE("query_locks_held respects max_depth",
          "[mcp][tools][concurrency]") {
  CallGraph graph;
  ControlFlowIndex cf;
  buildThreeFrameChain(graph, cf, "entry.cpp:3:3", "mid.cpp:2:3");

  ControlFlowOracle oracle(graph, cf);
  std::vector<std::string> eps = {"entry"};
  McpToolContext ctx{graph, oracle, cf, eps};
  auto handler = findHandler("query_locks_held");
  REQUIRE(handler);

  // max_depth=1 means we walk at most 1 frame above the target; entry is
  // 2 frames above leaf, so no paths should reach entry.
  llvm::json::Object args;
  args["function"] = "leaf";
  args["max_depth"] = 1;
  auto result = handler(args, ctx);
  REQUIRE_FALSE(isErrorResult(result));

  auto obj = parseToolResult(result);
  auto *paths = obj.getArray("paths");
  REQUIRE(paths != nullptr);
  CHECK(paths->size() == 0);
}

TEST_CASE("query_same_lock intersects locks across two targets",
          "[mcp][tools][concurrency]") {
  // Graph:
  //   entry() { lock_guard g(m); leafA(); leafB(); }
  // Both leaves see the same (type, varName) lock live in the caller.
  CallGraph graph;
  graph.addNode({"entry", "e.cpp", 1, true, false, ""});
  graph.addNode({"leafA", "a.cpp", 1, false, false, ""});
  graph.addNode({"leafB", "b.cpp", 1, false, false, ""});
  graph.addEdge({"entry", "leafA", EdgeKind::DirectCall,
                 Confidence::Proven, "e.cpp:5:3", 0});
  graph.addEdge({"entry", "leafB", EdgeKind::DirectCall,
                 Confidence::Proven, "e.cpp:6:3", 0});

  ControlFlowIndex cf;
  for (auto [site, callee] : std::vector<std::pair<std::string, std::string>>{
           {"e.cpp:5:3", "leafA"}, {"e.cpp:6:3", "leafB"}}) {
    CallSiteContext ctx;
    ctx.callerName = "entry";
    ctx.calleeName = callee;
    ctx.callSite = site;
    RaiiLocal l;
    l.typeName = "std::lock_guard<std::mutex>";
    l.varName = "g";
    l.declLocation = "e.cpp:2:29";
    l.kind = RaiiKind::Lock;
    ctx.liveRaiiLocals.push_back(std::move(l));
    cf.addCallSiteContext(std::move(ctx));
  }

  ControlFlowOracle oracle(graph, cf);
  std::vector<std::string> eps = {"entry"};
  McpToolContext mctx{graph, oracle, cf, eps};
  auto handler = findHandler("query_same_lock");
  REQUIRE(handler);

  llvm::json::Object args;
  args["fn_a"] = "leafA";
  args["fn_b"] = "leafB";
  auto result = handler(args, mctx);
  REQUIRE_FALSE(isErrorResult(result));

  auto obj = parseToolResult(result);
  auto shared = obj.getInteger("shared");
  REQUIRE(shared.has_value());
  CHECK(*shared == 1);

  auto *sharedLocks = obj.getArray("sharedLocks");
  REQUIRE(sharedLocks != nullptr);
  REQUIRE(sharedLocks->size() == 1);
  auto *first = (*sharedLocks)[0].getAsObject();
  REQUIRE(first != nullptr);
  CHECK(first->getString("typeName") == "std::lock_guard<std::mutex>");
  CHECK(first->getString("varName") == "g");
  auto *pA = first->getArray("pathsA");
  auto *pB = first->getArray("pathsB");
  REQUIRE(pA != nullptr);
  REQUIRE(pB != nullptr);
  CHECK(pA->size() >= 1);
  CHECK(pB->size() >= 1);
}

TEST_CASE("query_same_lock returns empty intersection when locks differ",
          "[mcp][tools][concurrency]") {
  // leafA is under lock `ga`; leafB under lock `gb` (same type, diff name).
  CallGraph graph;
  graph.addNode({"entry", "e.cpp", 1, true, false, ""});
  graph.addNode({"leafA", "a.cpp", 1, false, false, ""});
  graph.addNode({"leafB", "b.cpp", 1, false, false, ""});
  graph.addEdge({"entry", "leafA", EdgeKind::DirectCall,
                 Confidence::Proven, "e.cpp:5:3", 0});
  graph.addEdge({"entry", "leafB", EdgeKind::DirectCall,
                 Confidence::Proven, "e.cpp:10:3", 0});

  ControlFlowIndex cf;
  auto addLocked = [&](const std::string &site, const std::string &callee,
                       const std::string &varName) {
    CallSiteContext ctx;
    ctx.callerName = "entry";
    ctx.calleeName = callee;
    ctx.callSite = site;
    RaiiLocal l;
    l.typeName = "std::lock_guard<std::mutex>";
    l.varName = varName;
    l.declLocation = "e.cpp:2:29";
    l.kind = RaiiKind::Lock;
    ctx.liveRaiiLocals.push_back(std::move(l));
    cf.addCallSiteContext(std::move(ctx));
  };
  addLocked("e.cpp:5:3", "leafA", "ga");
  addLocked("e.cpp:10:3", "leafB", "gb");

  ControlFlowOracle oracle(graph, cf);
  std::vector<std::string> eps = {"entry"};
  McpToolContext mctx{graph, oracle, cf, eps};
  auto handler = findHandler("query_same_lock");
  REQUIRE(handler);

  llvm::json::Object args;
  args["fn_a"] = "leafA";
  args["fn_b"] = "leafB";
  auto result = handler(args, mctx);
  REQUIRE_FALSE(isErrorResult(result));

  auto obj = parseToolResult(result);
  auto shared = obj.getInteger("shared");
  REQUIRE(shared.has_value());
  CHECK(*shared == 0);
}
