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
