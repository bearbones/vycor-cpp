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

// test_disambiguation.cpp — F8 PR C: the MCP disambiguation contract
// (docs/design-f8-usr-identity.md §4), end-to-end against the
// examples/precision fixtures.
//
// The contract, uniform across identity-taking tools:
//   - name resolving to one function -> normal response (plus its "usr");
//   - name shared by several functions -> a NON-error {ambiguous:true,
//     candidates:[...]} response (never a silent union, never a hard error);
//   - explicit "usr" -> that function exactly, bypassing name resolution.
// Call-site tools disambiguate a macro-shared spelling through the optional
// "caller" parameter the same way.

#include "vycor/callgraph/CallGraph.h"
#include "vycor/callgraph/CallGraphBuilder.h"
#include "vycor/callgraph/ControlFlowIndex.h"
#include "vycor/callgraph/ControlFlowOracle.h"
#include "vycor/mcp/McpTools.h"

#include "llvm/Support/JSON.h"

#include <catch2/catch_test_macros.hpp>
#include <clang/Tooling/CompilationDatabase.h>

#include <set>
#include <string>
#include <vector>

using namespace vycor;

namespace {

std::string fixtureDir() {
  return std::string(PROJECT_SOURCE_DIR) + "/examples/precision/";
}

CallGraph buildPrecisionGraph(const std::vector<std::string> &files) {
  clang::tooling::FixedCompilationDatabase compDb(
      ".", {"-std=c++17", "-I" + fixtureDir()});
  std::vector<std::string> paths;
  paths.reserve(files.size());
  for (const auto &f : files)
    paths.push_back(fixtureDir() + f);
  return buildCallGraph(compDb, paths);
}

McpToolHandler findHandler(llvm::StringRef name) {
  for (auto &t : getRegisteredTools()) {
    if (t.name == name)
      return t.handler;
  }
  return {};
}

// Extract the JSON payload from an MCP tool result (same shape helper as
// test_mcp.cpp).
llvm::json::Object parseToolResult(const llvm::json::Value &result) {
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

bool isErrorResult(const llvm::json::Value &result) {
  auto *obj = result.getAsObject();
  if (!obj)
    return false;
  if (auto b = obj->getBoolean("isError"))
    return *b;
  return false;
}

// Pull the sorted candidate usr list out of an ambiguous response, asserting
// the full response shape along the way.
std::vector<std::string> requireAmbiguousUsrs(const llvm::json::Object &obj,
                                              const std::string &param,
                                              const std::string &name) {
  CHECK(obj.getBoolean("ambiguous") == true);
  CHECK(obj.getString("parameter") == param);
  CHECK(obj.getString("name") == name);
  CHECK(obj.getString("note").has_value());
  auto *candidates = obj.getArray("candidates");
  REQUIRE(candidates != nullptr);
  std::vector<std::string> usrs;
  for (auto &v : *candidates) {
    auto *cand = v.getAsObject();
    REQUIRE(cand != nullptr);
    auto usr = cand->getString("usr");
    REQUIRE(usr.has_value());
    usrs.push_back(usr->str());
    CHECK(cand->getString("qualifiedName") == name);
    CHECK(cand->getString("file").has_value());
    CHECK(cand->getInteger("line").has_value());
  }
  return usrs;
}

} // namespace

TEST_CASE("get_callers disambiguates overloads by name and answers "
          "precisely by usr",
          "[mcp][disambiguation]") {
  auto graph = buildPrecisionGraph({"overloads.cpp"});
  ControlFlowIndex cfIndex;
  ControlFlowOracle oracle(graph, cfIndex);
  std::vector<std::string> eps = {"precision::fromCString",
                                  "precision::fromDouble"};
  McpToolContext ctx{graph, oracle, cfIndex, eps};

  auto handler = findHandler("get_callers");
  REQUIRE(handler);

  // Ambiguous display name: a NON-error candidates response, not a merged
  // caller list.
  llvm::json::Object args;
  args["name"] = "precision::process";
  auto result = handler(args, ctx);
  CHECK_FALSE(isErrorResult(result));
  auto obj = parseToolResult(result);
  auto usrs = requireAmbiguousUsrs(obj, "name", "precision::process");
  REQUIRE(usrs.size() == 2);
  CHECK(usrs[0] != usrs[1]);
  // No result payload rode along with the disambiguation.
  CHECK(obj.getArray("callers") == nullptr);

  // The end-to-end precision payoff: each candidate usr yields that
  // overload's OWN callers, and the two sets are disjoint.
  std::set<std::string> allCallers;
  for (const auto &usr : usrs) {
    llvm::json::Object precise;
    precise["usr"] = usr;
    auto presult = handler(precise, ctx);
    CHECK_FALSE(isErrorResult(presult));
    auto pobj = parseToolResult(presult);
    CHECK_FALSE(pobj.getBoolean("ambiguous").has_value());
    CHECK(pobj.getInteger("callerCount") == 1);
    CHECK(pobj.getString("usr") == usr);
    auto *callers = pobj.getArray("callers");
    REQUIRE(callers != nullptr);
    REQUIRE(callers->size() == 1);
    auto *edge = (*callers)[0].getAsObject();
    REQUIRE(edge != nullptr);
    auto callerName = edge->getString("callerName");
    REQUIRE(callerName.has_value());
    // Disjointness: no caller appears for both overloads.
    CHECK(allCallers.insert(callerName->str()).second);
  }
  CHECK(allCallers ==
        std::set<std::string>{"precision::fromCString",
                              "precision::fromDouble"});
}

TEST_CASE("lookup_function disambiguates and resolves by usr",
          "[mcp][disambiguation]") {
  auto graph = buildPrecisionGraph({"overloads.cpp"});
  ControlFlowIndex cfIndex;
  ControlFlowOracle oracle(graph, cfIndex);
  std::vector<std::string> eps = {"precision::fromCString"};
  McpToolContext ctx{graph, oracle, cfIndex, eps};

  auto handler = findHandler("lookup_function");
  REQUIRE(handler);

  SECTION("ambiguous name returns candidates") {
    llvm::json::Object args;
    args["name"] = "precision::process";
    auto result = handler(args, ctx);
    CHECK_FALSE(isErrorResult(result));
    auto obj = parseToolResult(result);
    auto usrs = requireAmbiguousUsrs(obj, "name", "precision::process");
    CHECK(usrs.size() == 2);

    // Re-query with each usr lands on that node exactly.
    for (const auto &usr : usrs) {
      llvm::json::Object precise;
      precise["usr"] = usr;
      auto presult = handler(precise, ctx);
      CHECK_FALSE(isErrorResult(presult));
      auto pobj = parseToolResult(presult);
      CHECK_FALSE(pobj.getBoolean("ambiguous").has_value());
      CHECK(pobj.getString("qualifiedName") == "precision::process");
      CHECK(pobj.getString("usr") == usr);
    }
  }

  SECTION("unique name behaves exactly as before") {
    llvm::json::Object args;
    args["name"] = "precision::fromCString";
    auto result = handler(args, ctx);
    CHECK_FALSE(isErrorResult(result));
    auto obj = parseToolResult(result);
    CHECK_FALSE(obj.getBoolean("ambiguous").has_value());
    CHECK(obj.getString("qualifiedName") == "precision::fromCString");
    // The resolved identity is included.
    auto usr = obj.getString("usr");
    REQUIRE(usr.has_value());
    CHECK_FALSE(usr->empty());
  }

  SECTION("missing both name and usr is still an error") {
    llvm::json::Object args;
    auto result = handler(args, ctx);
    CHECK(isErrorResult(result));
  }
}

TEST_CASE("get_callers with a unique name is unchanged",
          "[mcp][disambiguation]") {
  auto graph = buildPrecisionGraph({"overloads.cpp"});
  ControlFlowIndex cfIndex;
  ControlFlowOracle oracle(graph, cfIndex);
  std::vector<std::string> eps = {"precision::fromCString"};
  McpToolContext ctx{graph, oracle, cfIndex, eps};

  auto handler = findHandler("get_callers");
  REQUIRE(handler);

  llvm::json::Object args;
  args["name"] = "precision::fromCString";
  auto result = handler(args, ctx);
  CHECK_FALSE(isErrorResult(result));
  auto obj = parseToolResult(result);
  CHECK_FALSE(obj.getBoolean("ambiguous").has_value());
  CHECK(obj.getString("function") == "precision::fromCString");
  CHECK(obj.getInteger("callerCount").has_value());
}

TEST_CASE("call-site tools disambiguate a macro-shared spelling by caller",
          "[mcp][disambiguation]") {
  // Two functions expand CALL_GUARDED, so their calls to precision::guarded
  // share one spelling (the macro definition line). Bare-spelling queries
  // must surface BOTH contexts as candidates; the `caller` parameter routes
  // to the precise compound-key lookup.
  clang::tooling::FixedCompilationDatabase compDb(
      ".", {"-std=c++17", "-I" + fixtureDir()});
  std::vector<std::string> paths{fixtureDir() + "macro_sites.cpp"};
  auto graph = buildCallGraph(compDb, paths);
  auto cfIndex = buildControlFlowIndex(compDb, paths, graph);
  ControlFlowOracle oracle(graph, cfIndex);
  std::vector<std::string> eps = {"precision::userOne", "precision::userTwo"};
  McpToolContext ctx{graph, oracle, cfIndex, eps};

  auto contexts = cfIndex.contextsForCallee("precision::guarded");
  REQUIRE(contexts.size() == 2);
  const std::string spelling = contexts[0].callSite;
  REQUIRE(contexts[1].callSite == spelling); // the macro collision itself

  SECTION("query_call_site_context") {
    auto handler = findHandler("query_call_site_context");
    REQUIRE(handler);

    // Bare spelling: non-error candidates response listing both callers.
    llvm::json::Object args;
    args["call_site"] = spelling;
    auto result = handler(args, ctx);
    CHECK_FALSE(isErrorResult(result));
    auto obj = parseToolResult(result);
    CHECK(obj.getBoolean("ambiguous") == true);
    CHECK(obj.getString("parameter") == "caller");
    CHECK(obj.getString("callSite") == spelling);
    auto *candidates = obj.getArray("candidates");
    REQUIRE(candidates != nullptr);
    REQUIRE(candidates->size() == 2);
    std::set<std::string> callerNames, callerUsrs;
    for (auto &v : *candidates) {
      auto *cand = v.getAsObject();
      REQUIRE(cand != nullptr);
      CHECK(cand->getString("callSite") == spelling);
      if (auto n = cand->getString("callerName"))
        callerNames.insert(n->str());
      if (auto u = cand->getString("callerUsr"))
        callerUsrs.insert(u->str());
    }
    CHECK(callerNames == std::set<std::string>{"precision::userOne",
                                               "precision::userTwo"});
    CHECK(callerUsrs.size() == 2); // distinct identities

    // Caller-qualified: each caller gets its OWN context.
    for (const auto &caller :
         {std::string("precision::userOne"), std::string("precision::userTwo")}) {
      llvm::json::Object precise;
      precise["call_site"] = spelling;
      precise["caller"] = caller;
      auto presult = handler(precise, ctx);
      CHECK_FALSE(isErrorResult(presult));
      auto pobj = parseToolResult(presult);
      CHECK_FALSE(pobj.getBoolean("ambiguous").has_value());
      CHECK(pobj.getString("caller") == caller);
      CHECK(pobj.getString("callee") == "precision::guarded");
    }
  }

  SECTION("query_raii_scopes_at_callsite") {
    auto handler = findHandler("query_raii_scopes_at_callsite");
    REQUIRE(handler);

    llvm::json::Object args;
    args["call_site"] = spelling;
    auto result = handler(args, ctx);
    CHECK_FALSE(isErrorResult(result));
    auto obj = parseToolResult(result);
    CHECK(obj.getBoolean("ambiguous") == true);
    auto *candidates = obj.getArray("candidates");
    REQUIRE(candidates != nullptr);
    CHECK(candidates->size() == 2);

    llvm::json::Object precise;
    precise["call_site"] = spelling;
    precise["caller"] = "precision::userTwo";
    auto presult = handler(precise, ctx);
    CHECK_FALSE(isErrorResult(presult));
    auto pobj = parseToolResult(presult);
    CHECK_FALSE(pobj.getBoolean("ambiguous").has_value());
    CHECK(pobj.getString("caller") == "precision::userTwo");
    CHECK(pobj.getArray("locals") != nullptr);
  }
}

// ============================================================================
// Candidate cap + site/filter refinement (the node-growth-policy follow-up:
// generic utilities can have hundreds of instantiations under one display
// name; the response must stay agent-loop-cheap).
// ============================================================================

namespace {

// A graph with `n` same-display-name nodes (distinct usrs, two files) plus
// one caller wired to a single instantiation through both the edge set and
// a call-site context.
struct BigOverloadFixture {
  CallGraph graph;
  ControlFlowIndex cfIndex;

  explicit BigOverloadFixture(int n) {
    for (int i = 0; i < n; ++i) {
      CallGraphNode node;
      node.qualifiedName = "big::process";
      node.file = (i % 2 == 0) ? "even.h" : "odd.h";
      node.line = 10 + i;
      node.usr = "c:@N@big@F@process#inst" + pad(i) + "#";
      graph.addNode(std::move(node), "tu.cpp");
    }
    CallGraphNode caller;
    caller.qualifiedName = "big::caller";
    caller.file = "caller.cpp";
    caller.line = 1;
    caller.usr = "c:@N@big@F@caller#";
    graph.addNode(std::move(caller), "tu.cpp");

    graph.addEdge({"c:@N@big@F@caller#", "c:@N@big@F@process#inst" + pad(7) + "#",
                   EdgeKind::DirectCall, Confidence::Proven,
                   "caller.cpp:5:3"},
                  "tu.cpp");

    CallSiteContext site;
    site.callerName = "big::caller";
    site.calleeName = "big::process";
    site.callSite = "caller.cpp:5:3";
    site.callerUsr = "c:@N@big@F@caller#";
    site.calleeUsr = "c:@N@big@F@process#inst" + pad(7) + "#";
    site.tuPath = "tu.cpp";
    cfIndex.addCallSiteContext(std::move(site));
  }

  // Zero-pad so lexicographic candidate order matches numeric order.
  static std::string pad(int i) {
    std::string s = std::to_string(i);
    return std::string(2 - std::min<size_t>(2, s.size()), '0') + s;
  }
};

} // namespace

TEST_CASE("ambiguous candidate list is capped with a by-file summary",
          "[mcp][disambiguation][cap]") {
  BigOverloadFixture fx(30);
  ControlFlowOracle oracle(fx.graph, fx.cfIndex);
  std::vector<std::string> eps = {"main"};
  McpToolContext ctx{fx.graph, oracle, fx.cfIndex, eps};
  auto handler = findHandler("lookup_function");
  REQUIRE(handler);

  llvm::json::Object args;
  args["name"] = "big::process";
  auto result = handler(args, ctx);
  CHECK_FALSE(isErrorResult(result));
  auto obj = parseToolResult(result);
  CHECK(obj.getBoolean("ambiguous") == true);
  CHECK(obj.getInteger("total_candidates") == 30);
  CHECK(obj.getBoolean("truncated") == true);
  auto *candidates = obj.getArray("candidates");
  REQUIRE(candidates != nullptr);
  CHECK(candidates->size() == 25);

  // By-file summary covers ALL candidates, not just the emitted prefix.
  auto *byFile = obj.getArray("candidates_by_file");
  REQUIRE(byFile != nullptr);
  int64_t sum = 0;
  for (const auto &f : *byFile)
    sum += f.getAsObject()->getInteger("count").value_or(0);
  CHECK(sum == 30);

  // The note teaches the refinement parameters.
  auto note = obj.getString("note");
  REQUIRE(note.has_value());
  CHECK(note->contains("site"));
  CHECK(note->contains("filter"));
}

TEST_CASE("small ambiguous sets are not truncated",
          "[mcp][disambiguation][cap]") {
  BigOverloadFixture fx(3);
  ControlFlowOracle oracle(fx.graph, fx.cfIndex);
  std::vector<std::string> eps = {"main"};
  McpToolContext ctx{fx.graph, oracle, fx.cfIndex, eps};
  auto handler = findHandler("lookup_function");
  REQUIRE(handler);

  llvm::json::Object args;
  args["name"] = "big::process";
  auto result = handler(args, ctx);
  auto obj = parseToolResult(result);
  CHECK(obj.getBoolean("ambiguous") == true);
  CHECK(obj.getInteger("total_candidates") == 3);
  CHECK_FALSE(obj.getBoolean("truncated").has_value());
  CHECK(obj.getArray("candidates")->size() == 3);
}

TEST_CASE("filter narrows an ambiguous name", "[mcp][disambiguation][filter]") {
  BigOverloadFixture fx(30);
  ControlFlowOracle oracle(fx.graph, fx.cfIndex);
  std::vector<std::string> eps = {"main"};
  McpToolContext ctx{fx.graph, oracle, fx.cfIndex, eps};
  auto handler = findHandler("lookup_function");
  REQUIRE(handler);

  SECTION("unique survivor resolves") {
    llvm::json::Object args;
    args["name"] = "big::process";
    args["filter"] = "inst07";
    auto result = handler(args, ctx);
    CHECK_FALSE(isErrorResult(result));
    auto obj = parseToolResult(result);
    CHECK_FALSE(obj.getBoolean("ambiguous").has_value());
    CHECK(obj.getString("usr") == "c:@N@big@F@process#inst07#");
  }

  SECTION("multi-match filter narrows the candidate list") {
    llvm::json::Object args;
    args["name"] = "big::process";
    args["filter"] = "even.h"; // matches candidate file
    auto result = handler(args, ctx);
    auto obj = parseToolResult(result);
    CHECK(obj.getBoolean("ambiguous") == true);
    CHECK(obj.getInteger("total_candidates") == 15);
    CHECK(obj.getString("note")->contains("narrowed by filter"));
  }

  SECTION("no-match filter falls back to the unfiltered set") {
    llvm::json::Object args;
    args["name"] = "big::process";
    args["filter"] = "no_such_thing";
    auto result = handler(args, ctx);
    auto obj = parseToolResult(result);
    CHECK(obj.getBoolean("ambiguous") == true);
    CHECK(obj.getInteger("total_candidates") == 30);
    CHECK(obj.getString("note")->contains("matched no candidate"));
  }
}

TEST_CASE("site resolves an ambiguous name to the instantiation called there",
          "[mcp][disambiguation][site]") {
  BigOverloadFixture fx(30);
  ControlFlowOracle oracle(fx.graph, fx.cfIndex);
  std::vector<std::string> eps = {"main"};
  McpToolContext ctx{fx.graph, oracle, fx.cfIndex, eps};
  auto handler = findHandler("lookup_function");
  REQUIRE(handler);

  SECTION("site alone resolves") {
    llvm::json::Object args;
    args["site"] = "caller.cpp:5:3";
    auto result = handler(args, ctx);
    CHECK_FALSE(isErrorResult(result));
    auto obj = parseToolResult(result);
    CHECK(obj.getString("usr") == "c:@N@big@F@process#inst07#");
  }

  SECTION("site plus agreeing name resolves") {
    llvm::json::Object args;
    args["name"] = "big::process";
    args["site"] = "caller.cpp:5:3";
    auto result = handler(args, ctx);
    CHECK_FALSE(isErrorResult(result));
    auto obj = parseToolResult(result);
    CHECK(obj.getString("usr") == "c:@N@big@F@process#inst07#");
  }

  SECTION("site plus disagreeing name is a clear error") {
    llvm::json::Object args;
    args["name"] = "big::caller"; // the site calls big::process, not this
    args["site"] = "caller.cpp:5:3";
    auto result = handler(args, ctx);
    CHECK(isErrorResult(result));
  }

  SECTION("unknown site is a clear error") {
    llvm::json::Object args;
    args["name"] = "big::process";
    args["site"] = "nowhere.cpp:1:1";
    auto result = handler(args, ctx);
    CHECK(isErrorResult(result));
  }

  SECTION("explicit usr still wins over site") {
    llvm::json::Object args;
    args["usr"] = "c:@N@big@F@process#inst03#";
    args["site"] = "caller.cpp:5:3";
    auto result = handler(args, ctx);
    auto obj = parseToolResult(result);
    CHECK(obj.getString("usr") == "c:@N@big@F@process#inst03#");
  }
}

TEST_CASE("macro-shared site with distinct callees lists the small set",
          "[mcp][disambiguation][site]") {
  BigOverloadFixture fx(4);
  // A second context at the SAME spelling calling a DIFFERENT instantiation
  // (macro expanded in two functions).
  CallSiteContext other;
  other.callerName = "big::caller2";
  other.calleeName = "big::process";
  other.callSite = "caller.cpp:5:3";
  other.callerUsr = "c:@N@big@F@caller2#";
  other.calleeUsr = "c:@N@big@F@process#inst01#";
  other.tuPath = "tu.cpp";
  fx.cfIndex.addCallSiteContext(std::move(other));

  ControlFlowOracle oracle(fx.graph, fx.cfIndex);
  std::vector<std::string> eps = {"main"};
  McpToolContext ctx{fx.graph, oracle, fx.cfIndex, eps};
  auto handler = findHandler("lookup_function");
  REQUIRE(handler);

  llvm::json::Object args;
  args["site"] = "caller.cpp:5:3";
  auto result = handler(args, ctx);
  CHECK_FALSE(isErrorResult(result));
  auto obj = parseToolResult(result);
  CHECK(obj.getBoolean("ambiguous") == true);
  CHECK(obj.getInteger("total_candidates") == 2);
}
