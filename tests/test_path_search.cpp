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

// test_path_search.cpp — the shared bounded reverse path search
// (callgraph/PathSearch.h): exact hops, canonical order, cycle rules,
// and the stop-reason contract every path consumer relies on.

#include "vycor/callgraph/CallGraph.h"
#include "vycor/callgraph/PathSearch.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace vycor;

namespace {

void node(CallGraph &g, const std::string &name, bool entry = false,
          const std::string &usr = "") {
  g.addNode({name, name + ".cpp", 1, entry, false, "", usr});
}

void edge(CallGraph &g, const std::string &from, const std::string &to,
          const std::string &site,
          Confidence confidence = Confidence::Proven,
          ExecutionContext exec = ExecutionContext::Synchronous) {
  g.addEdge({from, to, EdgeKind::DirectCall, confidence, site, 0, exec});
}

// "main>a>target" for a found path (display names).
std::string chainOf(const CallPath &p) {
  std::string s = p.hops.front().caller;
  for (const auto &h : p.hops)
    s += ">" + h.callee;
  return s;
}

std::vector<std::string> chainsOf(const PathSearchResult &r) {
  std::vector<std::string> out;
  for (const auto &p : r.paths)
    out.push_back(chainOf(p));
  return out;
}

// Same paths, including every hop's call site: the identity a consumer
// joins control-flow context on.
std::vector<std::string> sitesOf(const PathSearchResult &r) {
  std::vector<std::string> out;
  for (const auto &p : r.paths) {
    std::string s;
    for (const auto &h : p.hops)
      s += h.callerUsr + "@" + h.callSite + ">" + h.calleeUsr + ";";
    out.push_back(s);
  }
  return out;
}

// main -> a -> target, main -> b -> target, main -> target. Edges are
// inserted in an order unrelated to the canonical one.
CallGraph diamond(bool reversedInsertion = false) {
  CallGraph g;
  std::vector<std::string> names = {"target", "b", "a", "main"};
  if (reversedInsertion)
    names = {"main", "a", "b", "target"};
  for (const auto &n : names)
    node(g, n, n == "main");
  struct E {
    const char *from, *to, *site;
  };
  std::vector<E> edges = {{"b", "target", "b.cpp:4:3"},
                          {"main", "target", "main.cpp:9:3"},
                          {"main", "b", "main.cpp:8:3"},
                          {"a", "target", "a.cpp:4:3"},
                          {"main", "a", "main.cpp:7:3"}};
  if (reversedInsertion)
    std::reverse(edges.begin(), edges.end());
  for (const auto &e : edges)
    edge(g, e.from, e.to, e.site);
  return g;
}

} // namespace

TEST_CASE("findCallerPaths enumerates simple paths with exact hops in "
          "canonical order",
          "[pathsearch]") {
  auto g = diamond();
  auto r = findCallerPaths(g, "target", {"main"}, SearchLimits{});
  CHECK(r.targetKnown);
  CHECK(r.startKnown);
  CHECK(r.stops == 0);
  CHECK(r.complete());
  CHECK(r.exhaustive());
  // Callers of a node are expanded sorted by usr: a, b, main.
  CHECK(chainsOf(r) == std::vector<std::string>{"main>a>target",
                                                "main>b>target",
                                                "main>target"});
  REQUIRE(r.paths[0].hops.size() == 2);
  const auto &h0 = r.paths[0].hops[0];
  CHECK(h0.callerUsr == "main");
  CHECK(h0.calleeUsr == "a");
  CHECK(h0.caller == "main");
  CHECK(h0.callee == "a");
  CHECK(h0.callSite == "main.cpp:7:3");
  CHECK(h0.kind == EdgeKind::DirectCall);
  CHECK(h0.confidence == Confidence::Proven);
  CHECK(h0.execContext == ExecutionContext::Synchronous);
  CHECK(r.paths[0].hops[1].callSite == "a.cpp:4:3");
  CHECK(r.expansions > 0);
}

TEST_CASE("findCallerPaths: a start that is the target is not a path, and "
          "unknown names search nothing",
          "[pathsearch]") {
  auto g = diamond();
  SECTION("start == target") {
    auto r = findCallerPaths(g, "target", {"target"}, SearchLimits{});
    CHECK(r.targetKnown);
    CHECK(r.startKnown);
    CHECK(r.paths.empty());
    CHECK(r.stops == 0);
  }
  SECTION("unknown target: nothing searched, so nothing is complete") {
    auto r = findCallerPaths(g, "nope", {"main"}, SearchLimits{});
    CHECK_FALSE(r.targetKnown);
    CHECK(r.paths.empty());
    CHECK(r.stops == 0);
    CHECK_FALSE(r.complete());
    CHECK_FALSE(r.exhaustive());
  }
  SECTION("unknown start") {
    auto r = findCallerPaths(g, "target", {"nope"}, SearchLimits{});
    CHECK(r.targetKnown);
    CHECK_FALSE(r.startKnown);
    CHECK(r.paths.empty());
    CHECK(r.stops == 0);
  }
  SECTION("no starts") {
    auto r = findCallerPaths(g, "target", {}, SearchLimits{});
    CHECK_FALSE(r.startKnown);
    CHECK(r.paths.empty());
  }
}

TEST_CASE("findCallerPaths expands past a start that another start "
          "reaches",
          "[pathsearch]") {
  // main -> api -> target; both main and api are starts.
  CallGraph g;
  node(g, "main", true);
  node(g, "api", true);
  node(g, "target");
  edge(g, "main", "api", "main.cpp:2:3");
  edge(g, "api", "target", "api.cpp:2:3");
  auto r = findCallerPaths(g, "target", {"main", "api"}, SearchLimits{});
  // The nearer start is recorded first, then its callers are walked.
  CHECK(chainsOf(r) ==
        std::vector<std::string>{"api>target", "main>api>target"});
  CHECK(r.exhaustive());

  SECTION("a path limit reached at a start still reports the cut") {
    SearchLimits lim;
    lim.maxPaths = 1;
    auto cut = findCallerPaths(g, "target", {"main", "api"}, lim);
    CHECK(chainsOf(cut) == std::vector<std::string>{"api>target"});
    CHECK(cut.stopped(StopReason::PathLimit));
  }
}

TEST_CASE("findCallerPaths: parallel edges through one pair at different "
          "call sites are distinct paths",
          "[pathsearch]") {
  CallGraph g;
  node(g, "main", true);
  node(g, "target");
  edge(g, "main", "target", "main.cpp:4:10");
  edge(g, "main", "target", "main.cpp:3:25");
  auto r = findCallerPaths(g, "target", {"main"}, SearchLimits{});
  REQUIRE(r.paths.size() == 2);
  // Ordered by call site.
  CHECK(r.paths[0].hops[0].callSite == "main.cpp:3:25");
  CHECK(r.paths[1].hops[0].callSite == "main.cpp:4:10");
  CHECK(r.exhaustive());
}

TEST_CASE("findCallerPaths cycle rules: SimpleNodes vs SimpleEdges on "
          "recursion",
          "[pathsearch]") {
  // main -> rec -> target, rec -> rec (self-recursion), plus mutual
  // recursion rec <-> peer.
  CallGraph g;
  node(g, "main", true);
  node(g, "rec");
  node(g, "peer");
  node(g, "target");
  edge(g, "main", "rec", "main.cpp:2:3");
  edge(g, "rec", "rec", "rec.cpp:3:5");
  edge(g, "rec", "peer", "rec.cpp:4:5");
  edge(g, "peer", "rec", "peer.cpp:2:3");
  edge(g, "rec", "target", "rec.cpp:5:5");

  SECTION("SimpleNodes: no function repeats") {
    auto r = findCallerPaths(g, "target", {"main"}, SearchLimits{},
                             CycleRule::SimpleNodes);
    CHECK(chainsOf(r) == std::vector<std::string>{"main>rec>target"});
    CHECK(r.exhaustive());
  }
  SECTION("SimpleEdges: a function may recur through different edges") {
    auto r = findCallerPaths(g, "target", {"main"}, SearchLimits{},
                             CycleRule::SimpleEdges);
    // Every walk that uses each edge at most once: the self-loop and the
    // rec->peer->rec cycle each add one simple-edge path, and the two
    // cycles taken in either order add two more.
    auto chains = chainsOf(r);
    CHECK(r.exhaustive());
    REQUIRE(chains.size() == 5);
    CHECK(chains[0] == "main>rec>target");
    // The self-loop edge is used at most once per path.
    for (const auto &c : chains)
      CHECK(c.find("rec>rec>rec") == std::string::npos);
    bool sawSelf = false, sawMutual = false;
    for (const auto &c : chains) {
      if (c == "main>rec>rec>target")
        sawSelf = true;
      if (c == "main>rec>peer>rec>target")
        sawMutual = true;
    }
    CHECK(sawSelf);
    CHECK(sawMutual);
  }
  SECTION("SimpleEdges honors the depth bound in edges") {
    SearchLimits lim;
    lim.maxDepth = 2;
    auto r = findCallerPaths(g, "target", {"main"}, lim,
                             CycleRule::SimpleEdges);
    CHECK(chainsOf(r) == std::vector<std::string>{"main>rec>target"});
    CHECK(r.stopped(StopReason::DepthLimit));
    CHECK(r.complete());
    CHECK_FALSE(r.exhaustive());
  }
}

TEST_CASE("findCallerPaths path limit: PathLimit only when paths remain "
          "unexplored, and the kept subset is canonical",
          "[pathsearch]") {
  auto g = diamond();
  SECTION("limit below the count") {
    SearchLimits lim;
    lim.maxPaths = 2;
    auto r = findCallerPaths(g, "target", {"main"}, lim);
    CHECK(chainsOf(r) ==
          std::vector<std::string>{"main>a>target", "main>b>target"});
    CHECK(r.stopped(StopReason::PathLimit));
    CHECK_FALSE(r.complete());
    CHECK_FALSE(r.exhaustive());
    CHECK(stopReasonNames(r.stops) ==
          std::vector<std::string>{"path_limit"});
  }
  SECTION("limit equal to the count is not a stop") {
    SearchLimits lim;
    lim.maxPaths = 3;
    auto r = findCallerPaths(g, "target", {"main"}, lim);
    CHECK(r.paths.size() == 3);
    CHECK(r.stops == 0);
    CHECK(r.exhaustive());
  }
  SECTION("0 means unlimited") {
    SearchLimits lim;
    lim.maxPaths = 0;
    auto r = findCallerPaths(g, "target", {"main"}, lim);
    CHECK(r.paths.size() == 3);
    CHECK(r.exhaustive());
  }
}

TEST_CASE("findCallerPaths depth limit counts edges and reports cut "
          "branches",
          "[pathsearch]") {
  // main -> x -> y -> target (3 edges) and main -> target (1 edge).
  CallGraph g;
  node(g, "main", true);
  node(g, "x");
  node(g, "y");
  node(g, "target");
  edge(g, "main", "x", "main.cpp:2:3");
  edge(g, "x", "y", "x.cpp:2:3");
  edge(g, "y", "target", "y.cpp:2:3");

  SECTION("a chain longer than maxDepth is cut: no paths, DepthLimit") {
    SearchLimits lim;
    lim.maxDepth = 2;
    auto r = findCallerPaths(g, "target", {"main"}, lim);
    CHECK(r.paths.empty());
    CHECK(r.stopped(StopReason::DepthLimit));
    CHECK(r.complete());
    CHECK_FALSE(r.exhaustive());
  }
  SECTION("a chain of exactly maxDepth edges is found without a stop") {
    SearchLimits lim;
    lim.maxDepth = 3;
    auto r = findCallerPaths(g, "target", {"main"}, lim);
    CHECK(chainsOf(r) == std::vector<std::string>{"main>x>y>target"});
    CHECK(r.stops == 0);
  }
  SECTION("a short path is found while the long one is cut") {
    edge(g, "main", "target", "main.cpp:3:3");
    SearchLimits lim;
    lim.maxDepth = 2;
    auto r = findCallerPaths(g, "target", {"main"}, lim);
    CHECK(chainsOf(r) == std::vector<std::string>{"main>target"});
    CHECK(r.stopped(StopReason::DepthLimit));
    CHECK(r.complete());
    CHECK_FALSE(r.exhaustive());
  }
  SECTION("0 means unlimited") {
    SearchLimits lim;
    lim.maxDepth = 0;
    auto r = findCallerPaths(g, "target", {"main"}, lim);
    CHECK(r.paths.size() == 1);
    CHECK(r.stops == 0);
  }
}

TEST_CASE("findCallerPaths work budget stops the search and says so",
          "[pathsearch]") {
  auto g = diamond();
  SearchLimits lim;
  lim.maxWork = 1;
  auto r = findCallerPaths(g, "target", {"main"}, lim);
  CHECK(r.stopped(StopReason::WorkBudget));
  CHECK_FALSE(r.complete());
  CHECK(r.paths.size() < 3);
  CHECK(r.expansions <= 1);
}

TEST_CASE("findCallerPaths hub pruning skips high-fan-in ancestry but "
          "never the target",
          "[pathsearch]") {
  // main, c, d all call hub; hub calls target.
  CallGraph g;
  node(g, "main", true);
  node(g, "c");
  node(g, "d");
  node(g, "hub");
  node(g, "target");
  edge(g, "main", "hub", "main.cpp:2:3");
  edge(g, "c", "hub", "c.cpp:2:3");
  edge(g, "d", "hub", "d.cpp:2:3");
  edge(g, "hub", "target", "hub.cpp:2:3");

  SECTION("the hub's ancestry is skipped and reported") {
    SearchLimits lim;
    lim.maxFanIn = 2;
    auto r = findCallerPaths(g, "target", {"main"}, lim);
    CHECK(r.paths.empty());
    CHECK(r.stopped(StopReason::HubPruned));
    CHECK_FALSE(r.complete());
    REQUIRE(r.skippedHubs.size() == 1);
    CHECK(r.skippedHubs[0].usr == "hub");
    CHECK(r.skippedHubs[0].name == "hub");
    CHECK(r.skippedHubs[0].inDegree == 3);
  }
  SECTION("the target itself is expanded regardless of its in-degree") {
    edge(g, "main", "target", "main.cpp:3:3");
    edge(g, "c", "target", "c.cpp:3:3");
    edge(g, "d", "target", "d.cpp:3:3");
    SearchLimits lim;
    lim.maxFanIn = 2;
    auto r = findCallerPaths(g, "target", {"main"}, lim);
    CHECK(chainsOf(r) == std::vector<std::string>{"main>target"});
    CHECK(r.stopped(StopReason::HubPruned));
  }
  SECTION("0 disables the cutoff") {
    SearchLimits lim;
    lim.maxFanIn = 0;
    auto r = findCallerPaths(g, "target", {"main"}, lim);
    CHECK(chainsOf(r) == std::vector<std::string>{"main>hub>target"});
    CHECK(r.exhaustive());
  }
}

TEST_CASE("findCallerPaths is independent of insertion order, including "
          "the subset kept under a path limit",
          "[pathsearch]") {
  auto g1 = diamond(false);
  auto g2 = diamond(true);
  SECTION("full enumeration") {
    auto r1 = findCallerPaths(g1, "target", {"main"}, SearchLimits{});
    auto r2 = findCallerPaths(g2, "target", {"main"}, SearchLimits{});
    CHECK(sitesOf(r1) == sitesOf(r2));
    CHECK(r1.stops == r2.stops);
  }
  SECTION("truncated enumeration") {
    SearchLimits lim;
    lim.maxPaths = 1;
    auto r1 = findCallerPaths(g1, "target", {"main"}, lim);
    auto r2 = findCallerPaths(g2, "target", {"main"}, lim);
    CHECK(sitesOf(r1) == sitesOf(r2));
    CHECK(sitesOf(r1) ==
          std::vector<std::string>{"main@main.cpp:7:3>a;a@a.cpp:4:3>target;"});
  }
}

TEST_CASE("findCallerPaths edge predicate prunes edges without a stop "
          "reason",
          "[pathsearch]") {
  CallGraph g;
  node(g, "main", true);
  node(g, "a");
  node(g, "b");
  node(g, "target");
  edge(g, "main", "a", "main.cpp:2:3", Confidence::Plausible);
  edge(g, "main", "b", "main.cpp:3:3");
  edge(g, "a", "target", "a.cpp:2:3");
  edge(g, "b", "target", "b.cpp:2:3");
  auto r = findCallerPaths(
      g, "target", {"main"}, SearchLimits{}, CycleRule::SimpleNodes,
      [](const CallGraph::EdgeRef &e) {
        return e.confidence == Confidence::Proven;
      });
  CHECK(chainsOf(r) == std::vector<std::string>{"main>b>target"});
  CHECK(r.exhaustive());
}

TEST_CASE("findCallerPaths resolves a display name to every overload and "
          "a USR to exactly one",
          "[pathsearch]") {
  CallGraph g;
  node(g, "main", true);
  node(g, "f", false, "c:@F@f#I#");
  node(g, "f", false, "c:@F@f#d#");
  edge(g, "main", "c:@F@f#I#", "main.cpp:3:9");
  edge(g, "main", "c:@F@f#d#", "main.cpp:4:3");

  SECTION("by name: both overloads, in usr order") {
    auto r = findCallerPaths(g, "f", {"main"}, SearchLimits{});
    REQUIRE(r.paths.size() == 2);
    CHECK(r.paths[0].hops[0].calleeUsr == "c:@F@f#I#");
    CHECK(r.paths[0].hops[0].callee == "f");
    CHECK(r.paths[1].hops[0].calleeUsr == "c:@F@f#d#");
    CHECK(r.paths[1].hops[0].callSite == "main.cpp:4:3");
  }
  SECTION("by usr: one overload") {
    auto r = findCallerPaths(g, "c:@F@f#d#", {"main"}, SearchLimits{});
    REQUIRE(r.paths.size() == 1);
    CHECK(r.paths[0].hops[0].calleeUsr == "c:@F@f#d#");
    CHECK(r.paths[0].hops[0].callSite == "main.cpp:4:3");
  }
}

TEST_CASE("findCallerPaths carries execution context per hop",
          "[pathsearch]") {
  CallGraph g;
  node(g, "main", true);
  node(g, "target");
  edge(g, "main", "target", "main.cpp:2:3", Confidence::Proven,
       ExecutionContext::ThreadSpawn);
  auto r = findCallerPaths(g, "target", {"main"}, SearchLimits{});
  REQUIRE(r.paths.size() == 1);
  CHECK(r.paths[0].hops[0].execContext == ExecutionContext::ThreadSpawn);
}

TEST_CASE("stopReasonNames spells every set reason in enum order",
          "[pathsearch]") {
  CHECK(stopReasonNames(0).empty());
  unsigned all = static_cast<unsigned>(StopReason::PathLimit) |
                 static_cast<unsigned>(StopReason::DepthLimit) |
                 static_cast<unsigned>(StopReason::WorkBudget) |
                 static_cast<unsigned>(StopReason::HubPruned);
  CHECK(stopReasonNames(all) ==
        std::vector<std::string>{"path_limit", "depth_limit", "work_budget",
                                 "hub_pruned"});
  CHECK(std::string(stopReasonName(StopReason::HubPruned)) == "hub_pruned");
}
