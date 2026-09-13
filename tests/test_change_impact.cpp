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

// Semantic diff, impact search, patch mapping, and the CLI `diff` /
// `impact-of-change` adapters (docs/change-impact.md).

#include "vycor/callgraph/CallGraph.h"
#include "vycor/callgraph/ControlFlowIndex.h"
#include "vycor/callgraph/ControlFlowOracle.h"
#include "vycor/callgraph/Snapshot.h"
#include "vycor/cli/MegascopeCli.h"
#include "vycor/impact/ImpactSearch.h"
#include "vycor/impact/PatchMapping.h"
#include "vycor/impact/SemanticDiff.h"
#include "vycor/query/Tools.h"

#include "clang/Tooling/CompilationDatabase.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

using namespace vycor;

namespace {

// ---- hand-built graphs ------------------------------------------------------

void node(CallGraph &g, const std::string &name, const std::string &file,
          unsigned line, const std::string &usr = "", bool entry = false) {
  g.addNode({name, file, line, entry, false, "", usr}, file);
}

void edge(CallGraph &g, const std::string &from, const std::string &to,
          const std::string &site, EdgeKind kind = EdgeKind::DirectCall,
          Confidence conf = Confidence::Proven) {
  g.addEdge({from, to, kind, conf, site, 0, ExecutionContext::Synchronous},
            site.substr(0, site.find(':')));
}

SemanticDiffResult diffOf(const CallGraph &a, const CallGraph &b,
                          SemanticDiffOptions opts = {},
                          const ControlFlowIndex *cfA = nullptr,
                          const ControlFlowIndex *cfB = nullptr,
                          const SnapshotMeta *metaA = nullptr,
                          const SnapshotMeta *metaB = nullptr) {
  if (!cfA || !cfB)
    opts.compareContexts = false;
  return semanticDiff({a, cfA, metaA}, {b, cfB, metaB}, opts);
}

std::vector<std::string> kinds(const SemanticDiffResult &d) {
  std::vector<std::string> out;
  for (const auto &c : d.changes)
    out.push_back(changeKindName(c.change));
  return out;
}

const std::string kClosureA = "c:@N@std@S@function>#FI(#I)@F@function<#$main."
                              "cpp@2118@F@runChainC#@Sa#v>#&&S1_#";
const std::string kClosureB = "c:@N@std@S@function>#FI(#I)@F@function<#$main."
                              "cpp@2132@F@runChainC#@Sa#v>#&&S1_#";
const std::string kThreadA =
    "c:@N@std@S@thread@F@thread<#$main.cpp@1856@F@runChainC#@Sa#p0#v>#&&S0_#";
const std::string kThreadB =
    "c:@N@std@S@thread@F@thread<#$main.cpp@1870@F@runChainC#@Sa#p0#v>#&&S0_#";

// runChainC with two lambdas (a thread body and a std::function callback)
// at the given lines and closure offsets.
CallGraph chainGraph(unsigned l1, unsigned l2, const std::string &closure,
                     const std::string &thread) {
  CallGraph g;
  node(g, "main", "main.cpp", 75, "c:@F@main#", true);
  node(g, "runChainC", "main.cpp", 38, "c:@F@runChainC#");
  std::string lam1 = "vycor-lambda:lambda#main.cpp:" + std::to_string(l1) +
                     ":19#runChainC";
  std::string lam2 = "vycor-lambda:lambda#main.cpp:" + std::to_string(l2) +
                     ":7#runChainC";
  node(g, "runChainC::(lambda)", "main.cpp", l1, lam1);
  node(g, "runChainC::(lambda)", "main.cpp", l2, lam2);
  node(g, "std::thread::thread", "std_thread.h", 154, thread);
  node(g, "std::function<int (int)>::function", "std_function.h", 435,
       closure);
  edge(g, "c:@F@main#", "c:@F@runChainC#", "main.cpp:76:3");
  edge(g, "c:@F@runChainC#", lam1,
       "main.cpp:" + std::to_string(l1) + ":19", EdgeKind::ThreadEntry,
       Confidence::Plausible);
  edge(g, "c:@F@runChainC#", lam2, "main.cpp:" + std::to_string(l2) + ":7",
       EdgeKind::LambdaCall, Confidence::Plausible);
  edge(g, "c:@F@runChainC#", thread,
       "main.cpp:" + std::to_string(l1) + ":15");
  edge(g, "c:@F@runChainC#", closure,
       "main.cpp:" + std::to_string(l2) + ":3");
  return g;
}

} // namespace

// ============================================================================
// Identity
// ============================================================================

TEST_CASE("identity: lambda ordinals and closure offsets survive a line "
          "shift",
          "[impact][identity]") {
  CallGraph a = chainGraph(49, 58, kClosureA, kThreadA);
  CallGraph b = chainGraph(53, 62, kClosureB, kThreadB);
  IdentityTable ia = IdentityTable::build(a), ib = IdentityTable::build(b);

  CHECK(ia.keyOf("vycor-lambda:lambda#main.cpp:49:19#runChainC") ==
        "vycor-lambda:main.cpp#runChainC#0");
  CHECK(ib.keyOf("vycor-lambda:lambda#main.cpp:53:19#runChainC") ==
        "vycor-lambda:main.cpp#runChainC#0");
  CHECK(ia.keyOf("vycor-lambda:lambda#main.cpp:58:7#runChainC") ==
        ib.keyOf("vycor-lambda:lambda#main.cpp:62:7#runChainC"));
  CHECK(ia.keyOf(kClosureA) == ib.keyOf(kClosureB));
  CHECK(ia.keyOf(kClosureA).find("@%1@F@runChainC#@Sa") !=
        std::string::npos);
  CHECK(ia.keyOf(kThreadA) == ib.keyOf(kThreadB));
  CHECK(ia.keyOf(kThreadA).find("@%0@F@runChainC#@Sa") != std::string::npos);
  CHECK(ia.keyOf("c:@F@main#") == "c:@F@main#");
  CHECK(ia.usrOf(ia.keyOf(kClosureA)) == kClosureA);
  CHECK(ia.groupOf(ia.keyOf(kClosureA)) ==
        "closure:main.cpp@F@runChainC#@Sa");
  CHECK(ia.groupOf("c:@F@main#").empty());

  SemanticDiffResult d = diffOf(a, b);
  CHECK(d.changes.empty());
  CHECK(d.ambiguous.empty());
  CHECK(d.movedFunctions == 2); // the lambdas' lines moved
  CHECK(d.functionsBefore == d.functionsAfter);
  CHECK(d.relationshipsBefore == d.relationshipsAfter);
}

TEST_CASE("identity: a changed lambda count makes the group ambiguous",
          "[impact][identity]") {
  CallGraph a = chainGraph(49, 58, kClosureA, kThreadA);
  CallGraph b = chainGraph(49, 58, kClosureA, kThreadA);
  // A third lambda in runChainC, and a third closure offset.
  node(b, "runChainC::(lambda)", "main.cpp", 70,
       "vycor-lambda:lambda#main.cpp:70:3#runChainC");
  edge(b, "c:@F@runChainC#", "vycor-lambda:lambda#main.cpp:70:3#runChainC",
       "main.cpp:70:3", EdgeKind::LambdaCall, Confidence::Plausible);
  std::string extra = "c:@N@std@S@function>#FI(#I)@F@function<#$main."
                      "cpp@2500@F@runChainC#@Sa#v>#&&S1_#";
  node(b, "std::function<int (int)>::function", "std_function.h", 435,
       extra);
  edge(b, "c:@F@runChainC#", extra, "main.cpp:71:3");

  SemanticDiffResult d = diffOf(a, b);
  REQUIRE(d.ambiguous.size() == 2);
  CHECK(d.ambiguous[0].group == "closure:main.cpp@F@runChainC#@Sa");
  // Both lambdas of runChainC share the group: the thread's and the
  // std::function's constructors, plus the new one.
  CHECK(d.ambiguous[0].beforeUsrs.size() == 2);
  CHECK(d.ambiguous[0].afterUsrs.size() == 3);
  CHECK(d.ambiguous[1].group == "main.cpp#runChainC");
  CHECK(d.ambiguous[1].beforeUsrs.size() == 2);
  CHECK(d.ambiguous[1].afterUsrs.size() == 3);
  CHECK(d.ambiguous[1].edgesWithheldBefore == 2);
  CHECK(d.ambiguous[1].edgesWithheldAfter == 3);
  // Nothing ambiguous is reported as added or removed; the thread group
  // (one offset on both sides) still compares.
  CHECK(d.changes.empty());
}

// ============================================================================
// Change kinds
// ============================================================================

namespace {

CallGraph baseGraph() {
  CallGraph g;
  node(g, "main", "a.cpp", 1, "", true);
  node(g, "a", "a.cpp", 10);
  node(g, "b", "b.cpp", 20);
  edge(g, "main", "a", "a.cpp:2:3");
  edge(g, "a", "b", "a.cpp:11:3");
  return g;
}

} // namespace

TEST_CASE("each change kind, in contract order", "[impact][diff]") {
  CallGraph before = baseGraph();
  CallGraph after;
  node(after, "main", "a.cpp", 1, "", true);
  node(after, "a", "a.cpp", 10);
  node(after, "c", "c.cpp", 30);
  // main -> a became Plausible; a -> b is gone with b; main -> c is new.
  edge(after, "main", "a", "a.cpp:2:3", EdgeKind::DirectCall,
       Confidence::Plausible);
  edge(after, "main", "c", "a.cpp:3:3");

  SemanticDiffResult d = diffOf(before, after);
  CHECK(kinds(d) == std::vector<std::string>{"function_removed",
                                             "function_added", "call_removed",
                                             "call_added", "call_changed"});
  CHECK(d.changes[0].function.name == "b");
  CHECK(d.changes[1].function.name == "c");
  CHECK(d.changes[2].callerName == "a");
  CHECK(d.changes[2].calleeName == "b");
  REQUIRE(d.changes[2].before);
  CHECK_FALSE(d.changes[2].after);
  CHECK(d.changes[3].calleeName == "c");
  REQUIRE(d.changes[3].after);
  CHECK(d.changes[3].after->sites.size() == 1);
  CHECK(d.changes[3].after->sites[0].callSite == "a.cpp:3:3");
  REQUIRE(d.changes[4].before);
  REQUIRE(d.changes[4].after);
  CHECK(d.changes[4].before->attributes ==
        std::vector<std::string>{"Proven/Synchronous/0"});
  CHECK(d.changes[4].after->attributes ==
        std::vector<std::string>{"Plausible/Synchronous/0"});
  CHECK(d.count(ChangeKind::CallChanged) == 1);
  CHECK(d.functionsBefore == 3);
  CHECK(d.functionsAfter == 3);
  CHECK(d.relationshipsBefore == 2);
  CHECK(d.relationshipsAfter == 2);
}

TEST_CASE("a second call site to the same callee is call_changed, not "
          "call_added",
          "[impact][diff]") {
  CallGraph before = baseGraph();
  CallGraph after = baseGraph();
  edge(after, "main", "a", "a.cpp:5:3");
  SemanticDiffResult d = diffOf(before, after);
  CHECK(kinds(d) == std::vector<std::string>{"call_changed"});
  CHECK(d.changes[0].before->attributes.size() == 1);
  CHECK(d.changes[0].after->attributes.size() == 2);
}

TEST_CASE("a line shift of every call site is no change", "[impact][diff]") {
  CallGraph before = baseGraph();
  CallGraph after;
  node(after, "main", "a.cpp", 4, "", true);
  node(after, "a", "a.cpp", 13);
  node(after, "b", "b.cpp", 20);
  edge(after, "main", "a", "a.cpp:5:5");
  edge(after, "a", "b", "a.cpp:14:3");
  SemanticDiffOptions opts;
  opts.includeMoves = true;
  SemanticDiffResult d = diffOf(before, after, opts);
  CHECK(d.changes.empty());
  CHECK(d.movedFunctions == 2);
  REQUIRE(d.moves.size() == 2);
  CHECK(d.moves[0].name == "a");
  CHECK(d.moves[0].beforeLine == 10);
  CHECK(d.moves[0].afterLine == 13);
}

TEST_CASE("context_changed when a call site's protection changes",
          "[impact][diff]") {
  CallGraph before = baseGraph();
  CallGraph after = baseGraph();
  ControlFlowIndex cfBefore, cfAfter;
  {
    CallSiteContext ctx;
    ctx.callerName = ctx.callerUsr = "main";
    ctx.calleeName = ctx.calleeUsr = "a";
    ctx.callSite = "a.cpp:2:3";
    cfBefore.addCallSiteContext(ctx);
    TryCatchScope scope;
    scope.tryLocation = "a.cpp:1:20";
    scope.enclosingFunction = "main";
    CatchHandlerInfo h;
    h.caughtType = "std::exception";
    scope.handlers.push_back(h);
    ctx.enclosingTryCatches.push_back(scope);
    cfAfter.addCallSiteContext(ctx);
  }
  SemanticDiffResult d =
      diffOf(before, after, {}, &cfBefore, &cfAfter);
  REQUIRE(kinds(d) == std::vector<std::string>{"context_changed"});
  CHECK(d.changes[0].callerName == "main");
  CHECK(d.changes[0].before->signatures ==
        std::vector<std::string>{
            "protected=0;handlers=;noexcept=none;inCatch=0;locks=;guards="});
  CHECK(d.changes[0].after->signatures ==
        std::vector<std::string>{"protected=1;handlers=std::exception;"
                                 "noexcept=none;inCatch=0;locks=;guards="});
  // The site with no recorded context is "absent", distinct from
  // "unprotected".
  CHECK(d.changes[0].before->sites[0].signature != "absent");

  SemanticDiffOptions noCtx;
  noCtx.compareContexts = false;
  SemanticDiffResult quiet =
      semanticDiff({before, &cfBefore, nullptr}, {after, &cfAfter, nullptr},
                   noCtx);
  CHECK(quiet.changes.empty());
  CHECK_FALSE(quiet.comparability.contextsCompared);
}

TEST_CASE("moves and rename candidates are never merged", "[impact][diff]") {
  CallGraph before, after;
  node(before, "work", "a.cpp", 10, "c:@F@old#");
  node(before, "stable", "a.cpp", 30, "c:@F@stable#");
  node(after, "work", "a.cpp", 10, "c:@F@new#");
  node(after, "stable", "b.cpp", 30, "c:@F@stable#");
  SemanticDiffOptions opts;
  opts.includeMoves = true;
  SemanticDiffResult d = diffOf(before, after, opts);
  CHECK(kinds(d) ==
        std::vector<std::string>{"function_removed", "function_added"});
  REQUIRE(d.renameCandidates.size() == 1);
  CHECK(d.renameCandidates[0].name == "work");
  CHECK(d.renameCandidates[0].beforeKey == "c:@F@old#");
  CHECK(d.renameCandidates[0].afterKey == "c:@F@new#");
  REQUIRE(d.moves.size() == 1);
  CHECK(d.moves[0].name == "stable");
  CHECK(d.moves[0].beforeFile == "a.cpp");
  CHECK(d.moves[0].afterFile == "b.cpp");
}

TEST_CASE("changedFunctionsAfter names the after-side callers and added "
          "functions",
          "[impact][diff]") {
  CallGraph before = baseGraph();
  CallGraph after = baseGraph();
  node(after, "c", "c.cpp", 30);
  edge(after, "a", "c", "a.cpp:12:3");
  IdentityTable ia, ib;
  SemanticDiffResult d = semanticDiff({before, nullptr, nullptr},
                                      {after, nullptr, nullptr}, {}, &ia, &ib);
  CHECK(changedFunctionsAfter(d, ib) == std::vector<std::string>{"a", "c"});
}

// ============================================================================
// Comparability
// ============================================================================

namespace {

SnapshotMeta metaFor(std::vector<std::string> tus,
                     std::vector<TuStatus> statuses = {}) {
  SnapshotMeta m;
  for (size_t i = 0; i < tus.size(); ++i) {
    m.files.push_back({tus[i], 1, 2});
    TuOutcome o;
    o.status = i < statuses.size() ? statuses[i] : TuStatus::Indexed;
    m.outcomes.push_back(o);
  }
  m.provenance.analyzer = "vycor-cpp test";
  m.provenance.toolchain = "LLVM test";
  m.provenance.bakeStartNs = 1;
  return m;
}

} // namespace

TEST_CASE("comparability labels and refusal", "[impact][comparability]") {
  SnapshotMeta a = metaFor({"/src/a.cpp", "/src/b.cpp"});
  SnapshotMeta b = metaFor({"/src/a.cpp", "/src/b.cpp"});

  SECTION("identical bakes compare fully") {
    Comparability c = checkComparability(&a, &b, true);
    CHECK(c.comparable);
    CHECK(c.absenceReliable);
    CHECK(c.configSame);
    CHECK(c.contextsCompared);
    CHECK(c.reasons.empty());
    CHECK(c.before.coverage.indexed == 2);
  }
  SECTION("a partial TU makes absence unreliable, not incomparable") {
    b = metaFor({"/src/a.cpp", "/src/b.cpp"},
                {TuStatus::Indexed, TuStatus::Partial});
    Comparability c = checkComparability(&a, &b, false);
    CHECK(c.comparable);
    CHECK_FALSE(c.absenceReliable);
    CHECK_FALSE(c.contextsCompared);
    CHECK(c.partialAfter == std::vector<std::string>{"/src/b.cpp"});
    CHECK_FALSE(c.reasons.empty());
  }
  SECTION("a different TU selection is listed on the side that has it") {
    b = metaFor({"/src/a.cpp"});
    Comparability c = checkComparability(&a, &b, true);
    CHECK(c.comparable);
    CHECK_FALSE(c.absenceReliable);
    CHECK(c.tusOnlyBefore == std::vector<std::string>{"/src/b.cpp"});
    CHECK(c.tusOnlyAfter.empty());
  }
  SECTION("a failed TU is listed") {
    a = metaFor({"/src/a.cpp", "/src/b.cpp"},
                {TuStatus::Crashed, TuStatus::Indexed});
    Comparability c = checkComparability(&a, &b, true);
    CHECK(c.failedBefore == std::vector<std::string>{"/src/a.cpp"});
    CHECK_FALSE(c.absenceReliable);
  }
  SECTION("different analyzers are labelled, not refused") {
    b.provenance.analyzer = "vycor-cpp other";
    Comparability c = checkComparability(&a, &b, true);
    CHECK(c.comparable);
    CHECK_FALSE(c.analyzerSame);
    CHECK(c.toolchainSame);
  }
  SECTION("a different bake configuration is refused unless allowed") {
    b.collapsePaths = {"third_party"};
    Comparability c = checkComparability(&a, &b, true);
    CHECK_FALSE(c.comparable);
    CHECK_FALSE(c.configSame);
    CallGraph g = baseGraph();
    SemanticDiffResult refused = diffOf(g, g, {}, nullptr, nullptr, &a, &b);
    CHECK(refused.refused);
    CHECK(refused.changes.empty());
    SemanticDiffOptions allow;
    allow.allowMismatch = true;
    SemanticDiffResult forced =
        diffOf(g, g, allow, nullptr, nullptr, &a, &b);
    CHECK_FALSE(forced.refused);
    CHECK_FALSE(forced.comparability.comparable);
  }
  SECTION("no metadata: comparable, absence unreliable") {
    Comparability c = checkComparability(nullptr, nullptr, false);
    CHECK(c.comparable);
    CHECK_FALSE(c.absenceReliable);
    CHECK_FALSE(c.reasons.empty());
  }
}

TEST_CASE("a function whose TU is absent or failed on one side is "
          "explained",
          "[impact][comparability]") {
  CallGraph before = baseGraph();
  CallGraph after;
  node(after, "main", "a.cpp", 1, "", true);
  node(after, "a", "a.cpp", 10);
  edge(after, "main", "a", "a.cpp:2:3");
  SnapshotMeta ma = metaFor({"/src/a.cpp", "/src/b.cpp"});
  SECTION("absent TU") {
    SnapshotMeta mb = metaFor({"/src/a.cpp"});
    SemanticDiffResult d =
        diffOf(before, after, {}, nullptr, nullptr, &ma, &mb);
    REQUIRE(d.changes.size() == 2);
    CHECK(d.changes[0].function.name == "b");
    CHECK(d.changes[0].explanation == "tu_absent_after");
    CHECK(d.changes[1].change == ChangeKind::CallRemoved);
  }
  SECTION("failed TU") {
    SnapshotMeta mb = metaFor({"/src/a.cpp", "/src/b.cpp"},
                              {TuStatus::Indexed, TuStatus::Crashed});
    SemanticDiffResult d =
        diffOf(before, after, {}, nullptr, nullptr, &ma, &mb);
    REQUIRE(d.changes.size() == 2);
    CHECK(d.changes[0].explanation == "tu_failed_after");
  }
  SECTION("partial TU") {
    SnapshotMeta mb = metaFor({"/src/a.cpp", "/src/b.cpp"},
                              {TuStatus::Indexed, TuStatus::Partial});
    SemanticDiffResult d =
        diffOf(before, after, {}, nullptr, nullptr, &ma, &mb);
    REQUIRE(d.changes.size() == 2);
    CHECK(d.changes[0].explanation == "tu_partial_after");
  }
}

// ============================================================================
// Routes
// ============================================================================

TEST_CASE("route diff compares key sequences", "[impact][routes]") {
  CallGraph before = baseGraph();
  CallGraph after = baseGraph();
  node(after, "c", "c.cpp", 30);
  edge(after, "main", "c", "a.cpp:3:3");
  edge(after, "c", "b", "c.cpp:31:3");
  IdentityTable ia = IdentityTable::build(before);
  IdentityTable ib = IdentityTable::build(after);
  SearchLimits limits;
  RouteDiff r = diffRoutes({before, nullptr, nullptr},
                           {after, nullptr, nullptr}, ia, ib, "b", {"main"},
                           {"main"}, limits);
  CHECK(r.unchanged == 1);
  REQUIRE(r.added.size() == 1);
  CHECK(r.removed.empty());
  REQUIRE(r.added[0].hops.size() == 2);
  CHECK(r.added[0].hops[0].callee == "c");
  CHECK(r.complete);
  CHECK(r.exhaustive);
  CHECK(r.before.targetKnown);
}

// ============================================================================
// Impact
// ============================================================================

namespace {

// main -> p1 -> p2 -> leaf, q -> leaf, hub -> leaf with three callers.
CallGraph impactGraph() {
  CallGraph g;
  node(g, "main", "m.cpp", 1, "", true);
  for (const char *n : {"p1", "p2", "leaf", "q", "hub", "r1", "r2", "r3"})
    node(g, n, "m.cpp", 10);
  edge(g, "main", "p1", "m.cpp:2:3");
  edge(g, "p1", "p2", "m.cpp:11:3");
  edge(g, "p2", "leaf", "m.cpp:12:3");
  edge(g, "q", "leaf", "m.cpp:13:3");
  edge(g, "hub", "leaf", "m.cpp:14:3");
  edge(g, "r1", "hub", "m.cpp:15:3");
  edge(g, "r2", "hub", "m.cpp:16:3");
  edge(g, "r3", "hub", "m.cpp:17:3");
  return g;
}

std::vector<std::string> names(const ImpactResult &r) {
  std::vector<std::string> out;
  for (const auto &a : r.affected)
    out.push_back(a.name + "@" + std::to_string(a.depth));
  return out;
}

} // namespace

TEST_CASE("impact: shallowest depth, canonical order, witness paths",
          "[impact][search]") {
  CallGraph g = impactGraph();
  ImpactLimits limits;
  ImpactResult r = findImpact(g, {"leaf"}, limits);
  CHECK(r.changed == std::vector<std::string>{"leaf"});
  CHECK(r.unknown.empty());
  CHECK(names(r) == std::vector<std::string>{"hub@1", "p2@1", "q@1", "p1@2",
                                             "r1@2", "r2@2", "r3@2",
                                             "main@3"});
  CHECK(r.complete());
  CHECK(r.exhaustive());
  const AffectedFunction &m = r.affected.back();
  REQUIRE(m.path.size() == 3);
  CHECK(m.path[0].caller == "main");
  CHECK(m.path[0].callee == "p1");
  CHECK(m.path[2].callee == "leaf");
  CHECK(m.changedUsr == "leaf");

  SECTION("seed order does not matter") {
    ImpactResult ab = findImpact(g, {"leaf", "q"}, limits);
    ImpactResult ba = findImpact(g, {"q", "leaf"}, limits);
    CHECK(names(ab) == names(ba));
    CHECK(ab.changed == std::vector<std::string>{"leaf", "q"});
    // q is a seed, so it is not affected; its callers are none.
    CHECK(names(ab) == std::vector<std::string>{"hub@1", "p2@1", "p1@2",
                                                "r1@2", "r2@2", "r3@2",
                                                "main@3"});
  }
  SECTION("unknown seeds are reported and searched for nothing") {
    ImpactResult u = findImpact(g, {"nope", "leaf", "nope"}, limits);
    CHECK(u.unknown == std::vector<std::string>{"nope"});
    CHECK(u.affected.size() == 8);
  }
  SECTION("depth limit") {
    limits.maxDepth = 1;
    ImpactResult d = findImpact(g, {"leaf"}, limits);
    CHECK(names(d) == std::vector<std::string>{"hub@1", "p2@1", "q@1"});
    CHECK(d.stops & static_cast<unsigned>(StopReason::DepthLimit));
    CHECK(d.complete()); // a depth limit is a bound, not a budget
    CHECK_FALSE(d.exhaustive());
  }
  SECTION("hub pruning") {
    limits.maxFanIn = 2;
    ImpactResult h = findImpact(g, {"leaf"}, limits);
    CHECK(names(h) == std::vector<std::string>{"hub@1", "p2@1", "q@1",
                                               "p1@2", "main@3"});
    REQUIRE(h.skippedHubs.size() == 1);
    CHECK(h.skippedHubs[0].name == "hub");
    CHECK(h.skippedHubs[0].inDegree == 3);
    CHECK_FALSE(h.complete());
  }
  SECTION("work budget") {
    limits.maxWork = 1;
    ImpactResult w = findImpact(g, {"leaf"}, limits);
    CHECK(w.stops & static_cast<unsigned>(StopReason::WorkBudget));
    CHECK_FALSE(w.complete());
    CHECK(w.affected.size() == 3);
  }
  SECTION("edge filter") {
    ImpactResult f = findImpact(
        g, {"leaf"}, limits,
        [&](const CallGraph::EdgeRef &e) {
          return g.interner().resolve(e.caller) != "p2";
        });
    CHECK(names(f) == std::vector<std::string>{"hub@1", "q@1", "r1@2",
                                               "r2@2", "r3@2"});
  }
}

// ============================================================================
// Patches
// ============================================================================

TEST_CASE("unified diff parsing", "[impact][patch]") {
  const char *text = "diff --git a/src/x.cpp b/src/x.cpp\n"
                     "--- a/src/x.cpp\n"
                     "+++ b/src/x.cpp\n"
                     "@@ -10,2 +12,3 @@ int f()\n"
                     "+  a();\n"
                     "@@ -30 +33,0 @@\n"
                     "-  gone();\n"
                     "--- a/src/old.cpp\n"
                     "+++ /dev/null\n"
                     "@@ -1,5 +0,0 @@\n"
                     "--- ./y.cpp\t2026-09-01 10:00:00\n"
                     "+++ ./y.cpp\t2026-09-02 10:00:00\n"
                     "@@ -1 +1 @@\n";
  auto ranges = parseUnifiedDiff(text);
  REQUIRE(ranges.size() == 4);
  CHECK(ranges[0].file == "src/x.cpp");
  CHECK(ranges[0].firstLine == 12);
  CHECK(ranges[0].lastLine == 14);
  CHECK_FALSE(ranges[0].deletionOnly);
  CHECK(ranges[1].firstLine == 33);
  CHECK(ranges[1].lastLine == 34);
  CHECK(ranges[1].deletionOnly);
  CHECK(ranges[2].file == "src/old.cpp");
  CHECK(ranges[2].deletionOnly);
  CHECK(ranges[2].firstLine == 1);
  CHECK(ranges[3].file == "y.cpp");
  CHECK(ranges[3].firstLine == 1);
  CHECK(ranges[3].lastLine == 1);
  CHECK(parseUnifiedDiff("not a patch\n").empty());
}

TEST_CASE("patch mapping precisions", "[impact][patch]") {
  CallGraph g;
  node(g, "f", "/src/x.cpp", 10);
  node(g, "g", "/src/x.cpp", 30);
  node(g, "h", "/src/y.cpp", 100); // declared here, bodies elsewhere
  node(g, "k", "/src/y.cpp", 101);
  edge(g, "f", "g", "/src/x.cpp:12:3");
  edge(g, "g", "f", "/src/x.cpp:33:3");
  edge(g, "h", "f", "/src/body.cpp:5:3");
  edge(g, "k", "f", "/src/body.cpp:9:3");

  auto via = [](const PatchMapping &m, const std::string &usr) {
    for (const auto &f : m.functions)
      if (f.usr == usr)
        return std::string(mappingViaName(f.via));
    return std::string("none");
  };
  auto one = [&](const std::string &file, unsigned a, unsigned b,
                 llvm::StringRef root = "") {
    return mapRangesToFunctions(g, {{file, a, b, false}}, root);
  };

  CHECK(via(one("x.cpp", 12, 12), "f") == "call_site");
  CHECK(one("x.cpp", 12, 12).viaCallSite == 1);
  CHECK(via(one("x.cpp", 30, 30), "g") == "definition");
  CHECK(via(one("src/x.cpp", 20, 20), "f") == "extent"); // after f's site
  CHECK(via(one("src/x.cpp", 20, 20), "g") == "none");   // g's decl follows
  {
    PatchMapping top = one("/src/x.cpp", 3, 3);
    CHECK(via(top, "f") == "file");
    CHECK(via(top, "g") == "file");
    CHECK(top.viaFile == 2);
  }
  {
    // Between two call sites of different functions with no declaration
    // between them: both are candidates.
    PatchMapping both = one("body.cpp", 7, 7);
    CHECK(via(both, "h") == "extent");
    CHECK(via(both, "k") == "extent");
    CHECK(both.viaExtent == 2);
  }
  {
    PatchMapping none = one("nowhere.cpp", 1, 1);
    CHECK(none.functions.empty());
    REQUIRE(none.unmapped.size() == 1);
    CHECK(none.unmapped[0].reason ==
          "no function or call site indexed in this file");
  }
  {
    PatchMapping root = one("x.cpp", 12, 12, "/src");
    CHECK(via(root, "f") == "call_site");
    PatchMapping wrong = one("x.cpp", 12, 12, "/elsewhere");
    REQUIRE(wrong.unmapped.size() == 1);
  }
  {
    // A basename shared by two indexed files is ambiguous without a root.
    node(g, "f2", "/other/x.cpp", 10);
    PatchMapping amb = one("x.cpp", 10, 10);
    REQUIRE(amb.unmapped.size() == 1);
    CHECK(amb.unmapped[0].reason.find("matches 2 indexed files") !=
          std::string::npos);
    CHECK(via(one("x.cpp", 10, 10, "/other"), "f2") == "definition");
  }
  {
    // The most precise attribution wins per function.
    PatchMapping two = mapRangesToFunctions(
        g, {{"/src/x.cpp", 20, 20, false}, {"/src/x.cpp", 12, 12, false}},
        "");
    CHECK(via(two, "f") == "call_site");
  }
}

// ============================================================================
// Real bakes
// ============================================================================

namespace {

const std::string kBase =
    std::string(PROJECT_SOURCE_DIR) + "/examples/deep_chains/";

std::vector<std::string> deepChainsFiles() {
  std::vector<std::string> files;
  for (const char *f :
       {"main.cpp", "pipeline.cpp", "stage1_ingest.cpp", "stage2_parse.cpp",
        "stage3_transform.cpp", "stage4_dispatch.cpp", "stage5_sink.cpp",
        "plugins.cpp", "workers.cpp", "tokenizer.cpp", "scheduler.cpp",
        "callbacks.cpp", "async_workers.cpp", "lambda_callbacks.cpp"})
    files.push_back(kBase + f);
  return files;
}

BakedIndexes bakeDeepChains(std::vector<std::string> files,
                            unsigned threads) {
  clang::tooling::FixedCompilationDatabase compDb(
      ".", {"-std=c++17", "-I" + kBase});
  return bakeIndexes(compDb, files, {}, threads);
}

} // namespace

TEST_CASE("equivalent bakes of deep_chains diff to zero changes",
          "[impact][bake]") {
  std::vector<std::string> files = deepChainsFiles();
  BakedIndexes one = bakeDeepChains(files, 1);
  std::reverse(files.begin(), files.end());
  BakedIndexes four = bakeDeepChains(files, 4);
  REQUIRE(one.graph.nodeCount() == four.graph.nodeCount());

  SemanticDiffOptions opts;
  opts.includeMoves = true;
  SemanticDiffResult d = semanticDiff({one.graph, &one.cfIndex, nullptr},
                                      {four.graph, &four.cfIndex, nullptr},
                                      opts);
  CHECK(d.changes.empty());
  CHECK(d.moves.empty());
  CHECK(d.ambiguous.empty());
  CHECK(d.renameCandidates.empty());
  CHECK(d.comparability.contextsCompared);
  CHECK(d.functionsBefore == d.functionsAfter);

  // The one-pass signature table answers exactly what the per-edge
  // lookup would, for every edge of the bake.
  ContextSignatures sigs = ContextSignatures::build(one.cfIndex);
  CHECK(sigs.contextCount() > 0);
  CHECK(sigs.shapeCount() > 1);
  size_t edges = 0, present = 0;
  for (const CallGraphNode *n : one.graph.allNodes()) {
    for (const CallGraphEdge &e : one.graph.calleesOf(n->usr)) {
      ++edges;
      auto ctx =
          one.cfIndex.contextForEdge(e.callSite, e.callerUsr, e.calleeUsr);
      if (ctx)
        ++present;
      const std::string want = ctx ? contextSignature(*ctx) : "absent";
      CHECK(sigs.forEdge(e.callSite, e.callerUsr, e.calleeUsr) == want);
    }
  }
  CHECK(edges > 0);
  CHECK(present > 0);
  CHECK(sigs.forEdge("nowhere.cpp:1:1", "c:@F@x#", "c:@F@y#") == "absent");
  CHECK(d.relationshipsBefore == d.relationshipsAfter);
  CHECK(d.relationshipsBefore > 0);

  // The whole fixture, both ways round, is its own impact set.
  IdentityTable ia = IdentityTable::build(one.graph);
  IdentityTable ib = IdentityTable::build(four.graph);
  CHECK(changedFunctionsAfter(d, ib).empty());
  (void)ia;
}

// ============================================================================
// CLI
// ============================================================================

namespace {

struct Run {
  int code = -1;
  std::string out;
  std::string err;
};

Run run(std::vector<std::string> args, const std::string &stdinText = "") {
  Run r;
  llvm::raw_string_ostream out(r.out), err(r.err);
  std::istringstream in(stdinText);
  r.code = runMegascopeQueryVerb(args, out, err, in);
  out.flush();
  err.flush();
  return r;
}

llvm::json::Object parseObject(llvm::StringRef text) {
  auto v = llvm::json::parse(text);
  REQUIRE(bool(v));
  REQUIRE(v->getAsObject() != nullptr);
  return *v->getAsObject();
}

std::string tempIndexPath(llvm::StringRef tag) {
  llvm::SmallString<128> p;
  llvm::sys::fs::createUniquePath("vycor-impact-" + tag + "-%%%%%%.vycs", p,
                                  /*MakeAbsolute=*/true);
  return p.str().str();
}

struct IndexFile {
  std::string path;
  IndexFile(llvm::StringRef tag, const CallGraph &g, const SnapshotMeta &m)
      : path(tempIndexPath(tag)) {
    ControlFlowIndex cf;
    REQUIRE(SnapshotIO::save(path, g, cf, m));
  }
  ~IndexFile() { std::remove(path.c_str()); }
};

// main -> helper, and two "dup" functions with distinct USRs.
CallGraph cliBefore() {
  CallGraph g;
  node(g, "main", "/src/a.cpp", 10, "", true);
  node(g, "helper", "/src/a.cpp", 3);
  node(g, "dup", "/src/a.cpp", 20, "c:@F@dup#a");
  node(g, "dup", "/src/b.cpp", 20, "c:@F@dup#b");
  edge(g, "main", "helper", "/src/a.cpp:11:3");
  return g;
}

// ... plus helper -> extra.
CallGraph cliAfter() {
  CallGraph g = cliBefore();
  node(g, "extra", "/src/b.cpp", 5);
  edge(g, "helper", "extra", "/src/a.cpp:4:3");
  return g;
}

} // namespace

TEST_CASE("megascope diff over two saved indexes", "[impact][cli]") {
  SnapshotMeta meta = metaFor({"/src/a.cpp", "/src/b.cpp"});
  IndexFile before("before", cliBefore(), meta);
  IndexFile after("after", cliAfter(), meta);

  SECTION("changes, exit codes, envelope") {
    auto r = run({"diff", "--before", before.path, "--after", after.path});
    REQUIRE(r.code == kExitResults);
    auto o = parseObject(r.out);
    CHECK(o.getString("status") == "ok");
    CHECK(o.getObject("indexScope") != nullptr);
    auto *changes = o.getArray("changes");
    REQUIRE(changes);
    REQUIRE(changes->size() == 2);
    CHECK((*changes)[0].getAsObject()->getString("change") ==
          "function_added");
    CHECK((*changes)[0].getAsObject()->getString("name") == "extra");
    CHECK((*changes)[1].getAsObject()->getString("change") == "call_added");
    CHECK((*changes)[1].getAsObject()->getString("callerName") == "helper");
    auto *summary = o.getObject("summary");
    REQUIRE(summary);
    CHECK(summary->getInteger("changeCount") == 2);
    auto *comp = o.getObject("comparability");
    REQUIRE(comp);
    CHECK(comp->getBoolean("comparable") == true);
    CHECK(comp->getBoolean("absenceReliable") == true);

    auto same = run({"diff", "--before", before.path, "--after",
                     before.path});
    CHECK(same.code == kExitEmpty);
    CHECK(parseObject(same.out).getArray("changes")->empty());
  }
  SECTION("ndjson: one record per change after the summary line") {
    auto r = run({"diff", "--before", before.path, "--after", after.path,
                  "--format", "ndjson"});
    REQUIRE(r.code == kExitResults);
    std::vector<std::string> ls;
    std::stringstream ss(r.out);
    std::string line;
    while (std::getline(ss, line))
      ls.push_back(line);
    REQUIRE(ls.size() == 3);
    CHECK(parseObject(ls[0]).getObject("_summary") != nullptr);
    CHECK(parseObject(ls[2]).getString("change") == "call_added");
  }
  SECTION("--to: the route diff") {
    auto r = run({"diff", "--before", before.path, "--after", after.path,
                  "--to", "extra"});
    REQUIRE(r.code == kExitResults);
    auto o = parseObject(r.out);
    auto *routes = o.getObject("routes");
    REQUIRE(routes);
    CHECK(routes->getString("target") == "extra");
    CHECK(routes->getArray("added")->size() == 1);
    CHECK(routes->getArray("removed")->empty());
    CHECK(routes->getObject("before")->getBoolean("targetKnown") == false);
    CHECK(routes->getObject("after")->getBoolean("targetKnown") == true);

    auto amb = run({"diff", "--before", before.path, "--after", after.path,
                    "--to", "dup"});
    CHECK(amb.code == kExitAmbiguous);
    auto ao = parseObject(amb.out);
    CHECK(ao.getBoolean("ambiguous") == true);
    CHECK(ao.getString("side") == "after"); // ambiguous on both sides
  }
  SECTION("--impact: callers of the changed functions on the after side") {
    auto r = run({"diff", "--before", before.path, "--after", after.path,
                  "--impact"});
    REQUIRE(r.code == kExitResults);
    auto o = parseObject(r.out);
    auto *impact = o.getObject("impact");
    REQUIRE(impact);
    auto *changed = impact->getArray("changed");
    REQUIRE(changed);
    REQUIRE(changed->size() == 2); // extra (added) and helper (its caller)
    CHECK((*changed)[0].getAsObject()->getString("via") == "diff");
    auto *affected = impact->getArray("affected");
    REQUIRE(affected);
    REQUIRE(affected->size() == 1);
    CHECK((*affected)[0].getAsObject()->getString("name") == "main");
    CHECK(impact->getArray("entryPointsAffected")->size() == 1);
  }
  SECTION("usage and index errors") {
    CHECK(run({"diff", "--before", before.path}).code == kExitUsage);
    CHECK(run({"diff", "--before", before.path, "--after", after.path,
               "--bogus"})
              .code == kExitUsage);
    CHECK(run({"diff", "--before", before.path, "--after", after.path,
               "--max-depth", "x"})
              .code == kExitUsage);
    CHECK(run({"diff", "--before", before.path, "--after", after.path,
               "--index", before.path})
              .code == kExitUsage);
    auto missing = run({"diff", "--before", before.path, "--after",
                        "/nonexistent/x.vycs"});
    CHECK(missing.code == kExitIndex);
    CHECK(missing.err.find("--after") != std::string::npos);
  }
  SECTION("a configuration mismatch is refused unless allowed") {
    SnapshotMeta other = meta;
    other.collapsePaths = {"third_party"};
    IndexFile mismatched("mismatch", cliAfter(), other);
    auto r = run({"diff", "--before", before.path, "--after",
                  mismatched.path});
    CHECK(r.code == kExitIndex);
    auto o = parseObject(r.out);
    CHECK(o.getString("status") == "unavailable");
    CHECK(o.getObject("comparability")->getBoolean("configSame") == false);
    auto forced = run({"diff", "--before", before.path, "--after",
                       mismatched.path, "--allow-mismatch"});
    CHECK(forced.code == kExitResults);
    CHECK(parseObject(forced.out).getString("status") == "ok");
  }
}

TEST_CASE("megascope impact-of-change", "[impact][cli]") {
  SnapshotMeta meta = metaFor({"/src/a.cpp", "/src/b.cpp"});
  IndexFile idx("impact", cliAfter(), meta);

  SECTION("by name") {
    auto r = run({"impact-of-change", "--index", idx.path, "--changed",
                  "extra"});
    REQUIRE(r.code == kExitResults);
    auto o = parseObject(r.out);
    auto *affected = o.getArray("affected");
    REQUIRE(affected);
    REQUIRE(affected->size() == 2);
    CHECK((*affected)[0].getAsObject()->getString("name") == "helper");
    CHECK((*affected)[0].getAsObject()->getInteger("depth") == 1);
    CHECK((*affected)[1].getAsObject()->getString("name") == "main");
    CHECK((*affected)[1].getAsObject()->getBoolean("isEntryPoint") == true);
    CHECK((*affected)[1].getAsObject()->getArray("path")->size() == 2);
    CHECK(o.getArray("entryPointsAffected")->size() == 1);
    CHECK(o.getBoolean("complete") == true);
    CHECK((*o.getArray("changed"))[0].getAsObject()->getString("via") ==
          "argument");
    CHECK(o.getObject("mapping") == nullptr);

    auto none = run({"impact-of-change", "--index", idx.path, "--changed",
                     "main"});
    CHECK(none.code == kExitEmpty);
    auto amb = run({"impact-of-change", "--index", idx.path, "--changed",
                    "dup"});
    CHECK(amb.code == kExitAmbiguous);
    auto usr = run({"impact-of-change", "--index", idx.path,
                    "--changed-usrs", "c:@F@dup#a"});
    CHECK(usr.code == kExitEmpty);
    CHECK(parseObject(usr.out).getArray("unknown")->empty());
    auto unknown = run({"impact-of-change", "--index", idx.path, "--changed",
                        "nope"});
    CHECK(unknown.code == kExitEmpty);
    CHECK(parseObject(unknown.out).getArray("unknown")->size() == 1);
  }
  SECTION("by patch, from stdin") {
    const std::string patch = "--- a/src/a.cpp\n+++ b/src/a.cpp\n"
                              "@@ -4 +4 @@\n+  extra();\n";
    auto r = run({"impact-of-change", "--index", idx.path, "--patch-file",
                  "-"},
                 patch);
    REQUIRE(r.code == kExitResults);
    auto o = parseObject(r.out);
    auto *changed = o.getArray("changed");
    REQUIRE(changed);
    REQUIRE(changed->size() == 1);
    CHECK((*changed)[0].getAsObject()->getString("name") == "helper");
    CHECK((*changed)[0].getAsObject()->getString("via") == "call_site");
    auto *mapping = o.getObject("mapping");
    REQUIRE(mapping);
    CHECK(mapping->getInteger("call_site") == 1);
    CHECK(mapping->getBoolean("exact") == true);
    CHECK(o.getArray("unmapped")->empty());
    CHECK(o.getArray("affected")->size() == 1);

    auto bad = run({"impact-of-change", "--index", idx.path, "--patch-file",
                    "-"},
                   "nothing here\n");
    CHECK(bad.code == kExitUsage);
    CHECK(run({"impact-of-change", "--index", idx.path, "--patch-file",
               "/nonexistent.diff"})
              .code == kExitUsage);
    CHECK(run({"impact-of-change", "--index", idx.path, "--git-base",
               "HEAD~1"})
              .code == kExitUsage);
    CHECK(run({"impact-of-change", "--index", idx.path, "--patch-file", "-",
               "--git-base", "a", "--git-head", "b"},
              patch)
              .code == kExitUsage);
    CHECK(run({"impact-of-change", "--index", idx.path}).code ==
          kExitUsage);
  }
}
