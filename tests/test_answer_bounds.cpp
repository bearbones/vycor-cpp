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

// test_answer_bounds.cpp — package K (docs/result-contract.md, "Unknown
// identities", "Paging", "Parameter aliases"): an unknown function is
// not_found with suggestions on every identity-taking tool, an endpoint
// known only by name still resolves, and every list tool pages its
// canonical order without gaps or repeats.

#include "vycor/callgraph/CallGraph.h"
#include "vycor/callgraph/ChannelIndex.h"
#include "vycor/callgraph/ControlFlowIndex.h"
#include "vycor/callgraph/ControlFlowOracle.h"
#include "vycor/query/Identity.h"
#include "vycor/query/Tools.h"

#include "llvm/Support/JSON.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <initializer_list>
#include <string>
#include <vector>

using namespace vycor;

namespace {

constexpr int kN = 7; // records per paged list

ToolHandler findHandler(llvm::StringRef name) {
  for (auto &t : getRegisteredTools())
    if (t.name == name)
      return t.handler;
  return {};
}

CallGraphEdge edge(const std::string &from, const std::string &to,
                   EdgeKind kind, const std::string &site,
                   ExecutionContext ec = ExecutionContext::Synchronous) {
  return {from, to, kind, Confidence::Proven, site, 0, ec};
}

// main -> Server::handle -> Server::process_request, handle -> ext_log (an
// external callee: an edge endpoint without a node). For paging: callers
// caller_0..6 of hub (caller_0 from three sites), hub calling callee_0..6,
// a function-pointer target cb_i and a thread target worker_i per caller,
// seven derived classes, and seven channels.
struct Fixture {
  CallGraph graph;
  ControlFlowIndex cfIndex;
  ChannelIndex channels;
  std::vector<std::string> eps;
  ControlFlowOracle oracle{graph, cfIndex};
  ToolContext ctx{graph, oracle, cfIndex, eps, &channels};

  Fixture() {
    graph.addNode({"main", "m.cpp", 1, true, false, ""});
    graph.addNode({"Server::handle", "s.cpp", 1, false, false, "Server"});
    graph.addNode(
        {"Server::process_request", "s.cpp", 9, false, false, "Server"});
    graph.addEdge(edge("main", "Server::handle", EdgeKind::DirectCall,
                       "m.cpp:2:3"));
    graph.addEdge(edge("Server::handle", "Server::process_request",
                       EdgeKind::DirectCall, "s.cpp:2:3"));
    graph.addEdge(
        edge("Server::handle", "ext_log", EdgeKind::DirectCall, "s.cpp:3:3"));

    graph.addNode({"hub", "h.cpp", 1, false, false, ""});
    for (int i = 0; i < kN; ++i) {
      std::string n = std::to_string(i);
      graph.addNode({"caller_" + n, "c.cpp", 10u + i, false, false, ""});
      graph.addNode({"callee_" + n, "e.cpp", 10u + i, false, false, ""});
      graph.addEdge(edge("caller_" + n, "hub", EdgeKind::DirectCall,
                         "c.cpp:" + n + ":1"));
      graph.addEdge(edge("hub", "callee_" + n, EdgeKind::DirectCall,
                         "h.cpp:" + n + ":1"));
      graph.addEdge(edge("caller_" + n, "cb_" + n, EdgeKind::FunctionPointer,
                         "c.cpp:" + n + ":5"));
      graph.addEdge(edge("caller_" + n, "worker_" + n, EdgeKind::ThreadEntry,
                         "c.cpp:" + n + ":9", ExecutionContext::ThreadSpawn));
      graph.addDerivedClass("Base", "Derived_" + n);
      eps.push_back("caller_" + n);

      ChannelSite site;
      site.channelId = "chan_" + n;
      site.channelTypeName = "Queue";
      site.category = "queue";
      site.siteFunctionUsr = "hub";
      site.siteFunctionDisplay = "hub";
      site.callSite = "h.cpp:" + n + ":7";
      channels.addSite(site);
      // chan_0 gets all the producers, and three consumers.
      site.channelId = "chan_0";
      site.siteFunctionUsr = site.siteFunctionDisplay = "caller_" + n;
      site.callSite = "c.cpp:" + n + ":7";
      channels.addSite(site);
      if (i < 3) {
        site.op = ChannelOperation::Consume;
        site.callSite = "c.cpp:" + n + ":8";
        channels.addSite(site);
      }
    }
    // caller_0 calls hub from two more sites (distinct collapses them).
    graph.addEdge(edge("caller_0", "hub", EdgeKind::DirectCall, "c.cpp:0:2"));
    graph.addEdge(edge("caller_0", "hub", EdgeKind::DirectCall, "c.cpp:0:3"));
    ctx.facts.channelsIndexed = true;
  }

  llvm::json::Value run(llvm::StringRef tool, llvm::json::Object args) const {
    auto h = findHandler(tool);
    REQUIRE(h);
    return h(args, ctx);
  }
  llvm::json::Value
  run(llvm::StringRef tool,
      std::initializer_list<llvm::json::Object::KV> args) const {
    return run(tool, llvm::json::Object(args));
  }
};

const llvm::json::Object &objectOf(const llvm::json::Value &v) {
  auto *o = v.getAsObject();
  REQUIRE(o != nullptr);
  return *o;
}

std::vector<std::string> suggestionNames(const llvm::json::Value &v) {
  std::vector<std::string> out;
  if (auto *arr = objectOf(v).getArray("didYouMean"))
    for (auto &s : *arr)
      if (auto *o = s.getAsObject())
        out.push_back(o->getString("qualifiedName")->str());
  return out;
}

bool contains(const std::vector<std::string> &v, const std::string &s) {
  return std::find(v.begin(), v.end(), s) != v.end();
}

} // namespace

TEST_CASE("an unknown function is not_found with suggestions on every "
          "identity-taking tool",
          "[identity][tools]") {
  Fixture f;
  struct Call {
    const char *tool;
    const char *param;
  };
  for (Call c : {Call{"lookup_function", "name"}, {"get_callers", "name"},
                 {"get_callees", "name"}, {"find_call_chain", "to"},
                 {"query_exception_safety", "function"},
                 {"query_throw_propagation", "function"},
                 {"query_all_path_contexts", "function"},
                 {"query_nearest_catches", "function"},
                 {"query_locks_held", "function"}}) {
    INFO(c.tool);
    auto result = f.run(c.tool, {{c.param, "Server::procss_request"}});
    CHECK(statusOf(result) == ResultStatus::NotFound);
    CHECK(objectOf(result).getString("parameter") == c.param);
    CHECK(objectOf(result).getString("name") == "Server::procss_request");
    // The misspelling suggests the intended function.
    CHECK(contains(suggestionNames(result), "Server::process_request"));
  }

  SECTION("each identity of a two-identity tool is checked") {
    auto result = f.run("query_same_lock",
                        {{"fn_a", "Server::handle"}, {"fn_b", "nope"}});
    CHECK(statusOf(result) == ResultStatus::NotFound);
    CHECK(objectOf(result).getString("parameter") == "fn_b");
    result = f.run("find_call_chain",
                   {{"from", "mian"}, {"to", "Server::handle"}});
    CHECK(statusOf(result) == ResultStatus::NotFound);
    CHECK(objectOf(result).getString("parameter") == "from");
    CHECK(contains(suggestionNames(result), "main"));
  }

  SECTION("an unknown usr is not_found too") {
    auto result = f.run("get_callers", {{"usr", "c:@F@nope#"}});
    CHECK(statusOf(result) == ResultStatus::NotFound);
    CHECK(objectOf(result).getString("parameter") == "usr");
  }

  SECTION("a wrong qualifier suggests the function by its own name") {
    auto result =
        f.run("get_callers", {{"name", "Client::process_request"}});
    CHECK(statusOf(result) == ResultStatus::NotFound);
    auto names = suggestionNames(result);
    REQUIRE_FALSE(names.empty());
    CHECK(names.front() == "Server::process_request");
  }

  SECTION("nothing close suggests nothing, and says so") {
    auto result = f.run("get_callers", {{"name", "zzzzzzzzzzzz"}});
    CHECK(statusOf(result) == ResultStatus::NotFound);
    CHECK(suggestionNames(result).empty());
    REQUIRE(objectOf(result).getArray("didYouMean") != nullptr);
  }

  SECTION("suggestions are capped") {
    auto result = f.run("get_callers", {{"name", "caller_"}});
    CHECK(statusOf(result) == ResultStatus::NotFound);
    CHECK(suggestionNames(result).size() == kMaxSuggestions);
  }

  SECTION("a string the interner holds is not a function") {
    // Call-site and file strings are interned; they are not identities.
    auto result = f.run("get_callers", {{"name", "s.cpp:2:3"}});
    CHECK(statusOf(result) == ResultStatus::NotFound);
  }

  SECTION("query_channels_for_function: unknown is not_found, a known "
          "function without sites is an empty answer") {
    auto unknown = f.run("query_channels_for_function", {{"function", "hbu"}});
    CHECK(statusOf(unknown) == ResultStatus::NotFound);
    CHECK(contains(suggestionNames(unknown), "hub"));
    auto none =
        f.run("query_channels_for_function", {{"function", "Server::handle"}});
    CHECK(statusOf(none) == ResultStatus::Ok);
    CHECK(objectOf(none).getInteger("count") == 0);
  }
}

TEST_CASE("an endpoint known only by name still resolves, marked",
          "[identity][tools]") {
  Fixture f;
  auto result = f.run("get_callers", {{"name", "ext_log"}});
  REQUIRE(statusOf(result) == ResultStatus::Ok);
  const auto &obj = objectOf(result);
  CHECK(obj.getInteger("callerCount") == 1);
  CHECK(obj.getString("resolvedAs") == "name");
  CHECK(obj.get("usr") == nullptr);

  // A registered node carries its usr and no marker.
  auto known = f.run("get_callers", {{"name", "Server::process_request"}});
  CHECK(objectOf(known).getString("usr") == "Server::process_request");
  CHECK(objectOf(known).get("resolvedAs") == nullptr);

  // find_call_chain's marker follows its key.
  auto chain = f.run("find_call_chain", {{"from", "main"}, {"to", "ext_log"}});
  REQUIRE(statusOf(chain) == ResultStatus::Ok);
  CHECK(objectOf(chain).getString("targetResolvedAs") == "name");
  CHECK(objectOf(chain).getInteger("pathCount") == 1);
}

TEST_CASE("name and function are aliases of the target parameter",
          "[identity][tools]") {
  Fixture f;
  auto viaFunction =
      f.run("get_callers", {{"function", "Server::process_request"}});
  REQUIRE(statusOf(viaFunction) == ResultStatus::Ok);
  CHECK(objectOf(viaFunction).getInteger("callerCount") == 1);
  CHECK(objectOf(viaFunction).getString("function") ==
        "Server::process_request");

  auto viaName =
      f.run("query_nearest_catches", {{"name", "Server::process_request"}});
  REQUIRE(statusOf(viaName) == ResultStatus::Ok);
  CHECK(objectOf(viaName).getString("function") == "Server::process_request");

  auto locks = f.run("query_locks_held", {{"name", "Server::handle"}});
  CHECK(statusOf(locks) == ResultStatus::Ok);

  // The canonical spelling wins when both are present.
  auto both = f.run("get_callees", {{"name", "Server::handle"},
                                    {"function", "main"}});
  REQUIRE(statusOf(both) == ResultStatus::Ok);
  CHECK(objectOf(both).getString("function") == "Server::handle");

  // An alias that names nothing is reported under the spelling given.
  auto unknown = f.run("get_callers", {{"function", "nope"}});
  CHECK(statusOf(unknown) == ResultStatus::NotFound);
  CHECK(objectOf(unknown).getString("parameter") == "function");
}

namespace {

// Page through `tool` `limit` records at a time and check the paging
// contract: total, returned, truncated/nextOffset on every page, and the
// concatenated pages equal to one uncut page (canonical order, no gaps, no
// repeats).
void checkPaging(const Fixture &f, llvm::StringRef tool,
                 std::initializer_list<llvm::json::Object::KV> kv,
                 llvm::StringRef key, size_t expectedTotal,
                 int64_t limit = 3) {
  INFO(tool.str());
  const llvm::json::Object args(kv);
  llvm::json::Object all = args;
  all["limit"] = 1000;
  auto whole = f.run(tool, std::move(all));
  REQUIRE(statusOf(whole) == ResultStatus::Ok);
  const auto *wholeList = objectOf(whole).getArray(key);
  REQUIRE(wholeList != nullptr);
  REQUIRE(wholeList->size() == expectedTotal);
  CHECK(objectOf(whole).getBoolean("truncated") == false);
  CHECK(objectOf(whole).get("nextOffset") == nullptr);

  llvm::json::Array stitched;
  int64_t offset = 0;
  int pages = 0;
  for (;;) {
    llvm::json::Object page = args;
    page["limit"] = limit;
    if (offset)
      page["offset"] = offset;
    auto result = f.run(tool, std::move(page));
    REQUIRE(statusOf(result) == ResultStatus::Ok);
    const auto &obj = objectOf(result);
    const auto *list = obj.getArray(key);
    REQUIRE(list != nullptr);
    CHECK(obj.getInteger("offset") == offset);
    CHECK(obj.getInteger("limit") == limit);
    if (obj.get("total"))
      CHECK(obj.getInteger("total") == static_cast<int64_t>(expectedTotal));
    CHECK(list->size() <= static_cast<size_t>(limit));
    for (const auto &v : *list)
      stitched.push_back(v);
    ++pages;
    bool truncated = *obj.getBoolean("truncated");
    CHECK(truncated == (offset + static_cast<int64_t>(list->size()) <
                        static_cast<int64_t>(expectedTotal)));
    if (!truncated) {
      CHECK(obj.get("nextOffset") == nullptr);
      break;
    }
    REQUIRE(obj.getInteger("nextOffset") ==
            offset + static_cast<int64_t>(list->size()));
    offset = *obj.getInteger("nextOffset");
    REQUIRE(pages < 100);
  }
  CHECK(pages == static_cast<int>((expectedTotal + limit - 1) / limit));
  CHECK(llvm::json::Value(std::move(stitched)) ==
        llvm::json::Value(llvm::json::Array(*wholeList)));
}

} // namespace

TEST_CASE("every list tool pages its canonical order", "[paging][tools]") {
  Fixture f;
  // hub's callers: caller_0 has three sites.
  checkPaging(f, "get_callers", {{"name", "hub"}}, "callers", kN + 2);
  checkPaging(f, "get_callees", {{"name", "hub"}}, "callees", kN);
  checkPaging(f, "search_functions", {{"query", "caller_"}}, "matches", kN);
  checkPaging(f, "list_callback_sites", {}, "targets", kN);
  checkPaging(f, "list_concurrency_entry_points", {}, "entries", kN);
  checkPaging(f, "list_entry_points", {}, "entryPoints", kN);
  checkPaging(f, "get_class_hierarchy", {{"class_name", "Base"}},
              "derivedClasses", kN);
  checkPaging(f, "list_channels", {}, "channels", kN);
  checkPaging(f, "query_channels_for_function", {{"function", "hub"}}, "sites",
              kN);
  // Dead code pages over its own (file, line, usr) order; main's side of
  // the fixture is unreachable from the configured entry points.
  auto dead = f.run("analyze_dead_code", {{"limit", 1000}});
  auto totalDead = objectOf(dead).getInteger("totalDead");
  REQUIRE(totalDead);
  REQUIRE(*totalDead >= 3);
  checkPaging(f, "analyze_dead_code", {}, "dead",
              static_cast<size_t>(*totalDead), 2);
}

TEST_CASE("paging details", "[paging][tools]") {
  Fixture f;

  SECTION("the default cap applies without a limit") {
    auto result = f.run("get_callers", {{"name", "hub"}});
    const auto &obj = objectOf(result);
    CHECK(obj.getInteger("limit") == 200);
    CHECK(obj.getInteger("offset") == 0);
    CHECK(obj.getInteger("total") == kN + 2);
    CHECK(obj.getInteger("returned") == kN + 2);
    CHECK(obj.getBoolean("truncated") == false);
  }

  SECTION("the count member is the whole list, not the page") {
    auto result = f.run("get_callers", {{"name", "hub"}, {"limit", 2}});
    CHECK(objectOf(result).getInteger("callerCount") == kN + 2);
    CHECK(objectOf(result).getInteger("returned") == 2);
  }

  SECTION("an offset past the end is an empty, untruncated page") {
    auto result = f.run("get_callees", {{"name", "hub"}, {"offset", 50}});
    REQUIRE(statusOf(result) == ResultStatus::Ok);
    CHECK(objectOf(result).getArray("callees")->empty());
    CHECK(objectOf(result).getInteger("returned") == 0);
    CHECK(objectOf(result).getBoolean("truncated") == false);
  }

  SECTION("limit and offset are validated") {
    CHECK(statusOf(f.run("get_callers", {{"name", "hub"}, {"limit", 0}})) ==
          ResultStatus::UsageError);
    CHECK(statusOf(f.run("get_callers", {{"name", "hub"}, {"limit", -4}})) ==
          ResultStatus::UsageError);
    CHECK(statusOf(f.run("get_callers", {{"name", "hub"}, {"offset", -1}})) ==
          ResultStatus::UsageError);
    CHECK(statusOf(f.run("get_callers",
                         {{"name", "hub"}, {"limit", "ten"}})) ==
          ResultStatus::UsageError);
    CHECK(statusOf(f.run("list_channels", {{"limit", 0}})) ==
          ResultStatus::UsageError);
    CHECK(statusOf(f.run("list_callback_sites", {{"site_limit", 0}})) ==
          ResultStatus::UsageError);
  }

  SECTION("distinct collapses call sites per function with siteCount") {
    auto result = f.run("get_callers", {{"name", "hub"}, {"distinct", true}});
    REQUIRE(statusOf(result) == ResultStatus::Ok);
    const auto &obj = objectOf(result);
    CHECK(obj.getBoolean("distinct") == true);
    CHECK(obj.getInteger("callerCount") == kN);
    const auto *callers = obj.getArray("callers");
    REQUIRE(callers->size() == static_cast<size_t>(kN));
    const auto *first = (*callers)[0].getAsObject();
    CHECK(first->getString("callerName") == "caller_0");
    CHECK(first->getInteger("siteCount") == 3);
    // The first site in canonical order stands for the function.
    CHECK(first->getString("callSite") == "c.cpp:0:1");
    CHECK((*callers)[1].getAsObject()->getInteger("siteCount") == 1);
    checkPaging(f, "get_callers", {{"name", "hub"}, {"distinct", true}},
                "callers", kN);
  }

  SECTION("query_channel pages both lists by one window") {
    llvm::json::Object args{{"channel_id", "chan_0"}, {"limit", 2}};
    auto result = f.run("query_channel", args);
    REQUIRE(statusOf(result) == ResultStatus::Ok);
    const auto &obj = objectOf(result);
    // chan_0: hub's own produce site plus the seven callers'.
    CHECK(obj.getInteger("producerTotal") == kN + 1);
    CHECK(obj.getInteger("consumerTotal") == 3);
    CHECK(obj.getArray("producers")->size() == 2);
    CHECK(obj.getArray("consumers")->size() == 2);
    CHECK(obj.getBoolean("truncated") == true);
    CHECK(obj.getInteger("nextOffset") == 2);
    args["offset"] = 2;
    auto next = f.run("query_channel", args);
    // Consumers run out on the second page; producers continue.
    CHECK(objectOf(next).getArray("consumers")->size() == 1);
    CHECK(objectOf(next).getArray("producers")->size() == 2);
    CHECK(objectOf(next).getBoolean("truncated") == true);
  }

  SECTION("callback sites are capped per target") {
    // One target with many sites: every caller_i registers cb_shared.
    for (int i = 0; i < kN; ++i)
      f.graph.addEdge(edge("caller_" + std::to_string(i), "cb_shared",
                           EdgeKind::FunctionPointer,
                           "c.cpp:" + std::to_string(i) + ":6"));
    auto result = f.run("list_callback_sites",
                        {{"target_prefix", "cb_shared"}, {"site_limit", 2}});
    REQUIRE(statusOf(result) == ResultStatus::Ok);
    const auto *targets = objectOf(result).getArray("targets");
    REQUIRE(targets->size() == 1);
    const auto *t = (*targets)[0].getAsObject();
    CHECK(t->getInteger("siteCount") == kN);
    CHECK(t->getBoolean("sitesTruncated") == true);
    CHECK(t->getArray("sites")->size() == 2);
  }

  SECTION("impact_of_change accepts limit for max_results") {
    auto viaLimit =
        f.run("impact_of_change",
              {{"changed", llvm::json::Array{"hub"}}, {"limit", 2}});
    auto viaMax =
        f.run("impact_of_change",
              {{"changed", llvm::json::Array{"hub"}}, {"max_results", 2}});
    REQUIRE(statusOf(viaLimit) == ResultStatus::Ok);
    CHECK(viaLimit == viaMax);
    CHECK(objectOf(viaLimit).getArray("affected")->size() == 2);
  }
}
