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

// test_path_oracle.cpp — call-site-accurate exception path analysis over
// real sources (docs/path-analysis.md): duplicate protected/unprotected
// calls, overload identity, typed handlers and inheritance, rethrows,
// noexcept termination, thread and async boundaries, calls inside
// handlers, search cutoffs, and insertion-order invariance. Plus the tool
// payloads that carry the verdicts and the search facts.

#include "vycor/callgraph/CallGraph.h"
#include "vycor/callgraph/ControlFlowIndex.h"
#include "vycor/callgraph/ControlFlowOracle.h"
#include "vycor/callgraph/PathSearch.h"
#include "vycor/query/Tools.h"

#include <catch2/catch_test_macros.hpp>
#include <clang/Tooling/CompilationDatabase.h>

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"

#include <fstream>
#include <map>
#include <string>
#include <unistd.h>
#include <vector>

using namespace vycor;

namespace {

// Several TUs baked together, in the given order (order matters for the
// insertion-order tests). Temp files are removed on destruction.
struct Baked {
  BakedIndexes ix;
  std::vector<std::string> paths;
  Baked() = default;
  Baked(Baked &&) = default;
  Baked &operator=(Baked &&) = default;
  ~Baked() {
    for (const auto &p : paths)
      llvm::sys::fs::remove(p);
  }
};

std::string writeTemp(const std::string &code) {
  llvm::SmallString<128> tmp;
  int fd = -1;
  auto ec = llvm::sys::fs::createTemporaryFile("vycor_paths", "cpp", fd, tmp);
  REQUIRE_FALSE(ec);
  ::close(fd);
  std::ofstream out(std::string(tmp.str()));
  out << code;
  return std::string(tmp.str());
}

Baked bake(const std::vector<std::string> &sources) {
  Baked b;
  for (const auto &s : sources)
    b.paths.push_back(writeTemp(s));
  clang::tooling::FixedCompilationDatabase compDb(".", {"-std=c++17"});
  b.ix = bakeIndexes(compDb, b.paths, {}, /*threadCount=*/1);
  return b;
}

// The line:col suffix of a "file:line:col" location.
std::string lineCol(const std::string &loc) {
  auto colon = loc.rfind(':');
  REQUIRE(colon != std::string::npos);
  auto colon2 = loc.rfind(':', colon - 1);
  REQUIRE(colon2 != std::string::npos);
  return loc.substr(colon2 + 1);
}

// "main>mid>target".
std::string chainOf(const PathInfo &p) {
  std::string s;
  for (size_t i = 0; i < p.callChain.size(); ++i)
    s += (i ? ">" : "") + p.callChain[i];
  return s;
}

const PathInfo &pathVia(const ExceptionPathResult &r,
                        const std::string &secondFrame) {
  for (const auto &p : r.paths)
    if (p.callChain.size() > 1 && p.callChain[1] == secondFrame)
      return p;
  FAIL("no path through " << secondFrame);
  return r.paths.front();
}

ToolHandler findHandler(llvm::StringRef name) {
  for (auto &t : getRegisteredTools())
    if (t.name == name)
      return t.handler;
  return {};
}

llvm::json::Object payloadOf(const llvm::json::Value &result) {
  auto *obj = result.getAsObject();
  REQUIRE(obj != nullptr);
  REQUIRE_FALSE(isErrorResult(result));
  return *obj;
}

std::vector<std::string> stringsOf(const llvm::json::Object &o,
                                   llvm::StringRef key) {
  std::vector<std::string> out;
  auto *arr = o.getArray(key);
  REQUIRE(arr != nullptr);
  for (const auto &v : *arr)
    out.push_back(v.getAsString()->str());
  return out;
}

const char *kPlanFixture = R"cpp(
void target() { throw 1; }
int main(int argc, char**) {
  if (argc > 1) { try { target(); } catch (...) {} }
  else { target(); }
}
)cpp";

} // namespace

// ============================================================================
// The plan's fixture: one protected and one unprotected call to the same
// callee from the same caller.
// ============================================================================

TEST_CASE("duplicate calls: each call site is its own path with its own "
          "outcome",
          "[paths][oracle]") {
  auto b = bake({kPlanFixture});
  ControlFlowOracle oracle(b.ix.graph, b.ix.cfIndex);

  auto r = oracle.queryExceptionProtection("target", "", {"main"});
  CHECK(r.protection == Protection::SometimesCaught);
  CHECK(r.verdictExhaustive);
  CHECK(r.search.exhaustive);
  CHECK(r.search.stops == 0);
  REQUIRE(r.paths.size() == 2);
  CHECK(r.caughtCount == 1);
  CHECK(r.uncaughtCount == 1);

  // Both witnesses, joined on their exact call sites (line 4 column 25 is
  // the guarded call, line 5 column 10 the bare one; the fixture's first
  // line is blank).
  std::map<std::string, const PathInfo *> bySite;
  for (const auto &p : r.paths) {
    REQUIRE(p.hops.size() == 1);
    bySite[lineCol(p.hops[0].callSite)] = &p;
  }
  REQUIRE(bySite.count("4:25"));
  REQUIRE(bySite.count("5:10"));
  const auto &guarded = *bySite["4:25"];
  CHECK(guarded.outcome == PathOutcome::Caught);
  CHECK(guarded.isCaught);
  CHECK(guarded.caughtBy == "...");
  CHECK(lineCol(guarded.caughtAt) == "4:19");
  CHECK(guarded.tryCatchesOnPath.size() == 1);
  const auto &bare = *bySite["5:10"];
  CHECK(bare.outcome == PathOutcome::Uncaught);
  CHECK_FALSE(bare.isCaught);
  CHECK(bare.stopAt == "main");
  CHECK(bare.tryCatchesOnPath.empty());
  CHECK(guarded.hops[0].callerUsr.rfind("c:@F@main#", 0) == 0);
  CHECK(guarded.hops[0].calleeUsr == "c:@F@target#");

  SECTION("the tool payload carries both counts and the search facts") {
    std::vector<std::string> eps = {"main"};
    ToolContext ctx{b.ix.graph, oracle, b.ix.cfIndex, eps};
    llvm::json::Object args;
    args["function"] = "target";
    auto obj = payloadOf(findHandler("query_exception_safety")(args, ctx));
    CHECK(obj.getString("protection") == "sometimes_caught");
    CHECK(obj.getInteger("totalPaths") == 2);
    CHECK(obj.getInteger("caughtPaths") == 1);
    CHECK(obj.getInteger("uncaughtPaths") == 1);
    CHECK(obj.getInteger("terminatingPaths") == 0);
    CHECK(obj.getInteger("unknownPaths") == 0);
    CHECK(obj.getBoolean("complete") == true);
    CHECK(obj.getBoolean("exhaustive") == true);
    CHECK(stringsOf(obj, "stopReasons").empty());
    CHECK_FALSE(obj.get("skippedHubs"));
  }
}

TEST_CASE("overloads: the verdict is per identity, not per display name",
          "[paths][oracle]") {
  auto b = bake({R"cpp(
void f(int) { throw 1; }
void f(double) { throw 2.0; }
int main() {
  try { f(1); } catch (...) {}
  f(1.0);
}
)cpp"});
  ControlFlowOracle oracle(b.ix.graph, b.ix.cfIndex);
  auto usrs = b.ix.graph.usrsForName("f");
  REQUIRE(usrs.size() == 2);

  std::map<Protection, std::string> verdicts;
  for (const auto &usr : usrs) {
    auto r = oracle.queryExceptionProtection(usr, "", {"main"});
    REQUIRE(r.paths.size() == 1);
    CHECK(r.paths[0].hops[0].calleeUsr == usr);
    CHECK(r.verdictExhaustive);
    verdicts[r.protection] = usr;
  }
  REQUIRE(verdicts.count(Protection::AlwaysCaught));
  REQUIRE(verdicts.count(Protection::NeverCaught));
  CHECK(lineCol(oracle
                    .queryExceptionProtection(
                        verdicts[Protection::AlwaysCaught], "", {"main"})
                    .paths[0]
                    .hops[0]
                    .callSite) == "5:9");
  CHECK(lineCol(oracle
                    .queryExceptionProtection(
                        verdicts[Protection::NeverCaught], "", {"main"})
                    .paths[0]
                    .hops[0]
                    .callSite) == "6:3");

  // By display name the query spans every overload: one path each.
  auto byName = oracle.queryExceptionProtection("f", "", {"main"});
  CHECK(byName.protection == Protection::SometimesCaught);
  CHECK(byName.paths.size() == 2);
}

// ============================================================================
// Propagation order
// ============================================================================

TEST_CASE("typed handlers: source order, catch-all, std and user "
          "inheritance",
          "[paths][oracle]") {
  auto b = bake({R"cpp(
#include <stdexcept>
struct Base : std::exception {};
struct Derived : Base {};
void target() { throw Derived(); }
void viaBase() { try { target(); } catch (const Base &) {} }
void viaStd() { try { target(); } catch (const std::exception &) {} }
void viaRuntime() { try { target(); } catch (const std::runtime_error &) {} }
void viaOrder() { try { target(); } catch (int) {} catch (...) {} }
int main() { viaBase(); viaStd(); viaRuntime(); viaOrder(); }
)cpp"});
  ControlFlowOracle oracle(b.ix.graph, b.ix.cfIndex);

  auto r = oracle.queryThrowPropagation("target", "Derived", {"main"});
  REQUIRE(r.paths.size() == 4);
  CHECK(r.protection == Protection::SometimesCaught);
  CHECK(pathVia(r, "viaBase").outcome == PathOutcome::Caught);
  CHECK(pathVia(r, "viaStd").outcome == PathOutcome::Caught);
  const auto rt = pathVia(r, "viaRuntime");
  CHECK(rt.outcome == PathOutcome::Uncaught);
  // The non-matching scope is still reported as being on the path.
  CHECK(rt.tryCatchesOnPath.size() == 1);
  const auto order = pathVia(r, "viaOrder");
  CHECK(order.outcome == PathOutcome::Caught);
  CHECK(order.caughtBy == "...");

  SECTION("an unrelated type is caught only by the catch-alls") {
    auto other = oracle.queryThrowPropagation("target", "int", {"main"});
    CHECK(pathVia(other, "viaBase").outcome == PathOutcome::Uncaught);
    CHECK(pathVia(other, "viaStd").outcome == PathOutcome::Uncaught);
    CHECK(pathVia(other, "viaOrder").outcome == PathOutcome::Caught);
    CHECK(pathVia(other, "viaOrder").caughtBy != "...");
  }
  SECTION("an empty type matches every handler") {
    auto any = oracle.queryThrowPropagation("target", "", {"main"});
    CHECK(any.protection == Protection::AlwaysCaught);
    CHECK(any.verdictExhaustive);
  }
}

TEST_CASE("rethrows hand the exception to the next enclosing scope",
          "[paths][oracle]") {
  auto b = bake({R"cpp(
void target() { throw 1; }
void mid() { try { target(); } catch (...) { throw; } }
void outer() { try { mid(); } catch (...) {} }
int main() { outer(); mid(); }
)cpp"});
  ControlFlowOracle oracle(b.ix.graph, b.ix.cfIndex);
  auto r = oracle.queryThrowPropagation("target", "int", {"main"});
  CHECK(r.protection == Protection::SometimesCaught);
  REQUIRE(r.paths.size() == 2);
  const auto viaOuter = pathVia(r, "outer");
  CHECK(viaOuter.outcome == PathOutcome::Caught);
  CHECK(viaOuter.callChain ==
        std::vector<std::string>{"main", "outer", "mid", "target"});
  REQUIRE(viaOuter.rethrownAt.size() == 1);
  CHECK(lineCol(viaOuter.rethrownAt[0]) == "3:32");
  CHECK(lineCol(viaOuter.caughtAt) == "4:16");
  const auto direct = pathVia(r, "mid");
  CHECK(direct.outcome == PathOutcome::Uncaught);
  REQUIRE(direct.rethrownAt.size() == 1);
  // The rethrowing scope is on the path but does not count as protection.
  CHECK(direct.tryCatchesOnPath.size() == 1);
  CHECK_FALSE(direct.isCaught);
}

TEST_CASE("noexcept boundaries terminate; a handler inside the noexcept "
          "function still catches first",
          "[paths][oracle]") {
  SECTION("mixed") {
    auto b = bake({R"cpp(
void target() { throw 1; }
void barrier() noexcept { target(); }
void guarded() noexcept { try { target(); } catch (...) {} }
int main() { barrier(); guarded(); }
)cpp"});
    ControlFlowOracle oracle(b.ix.graph, b.ix.cfIndex);
    auto r = oracle.queryThrowPropagation("target", "int", {"main"});
    CHECK(r.protection == Protection::SometimesCaught);
    CHECK(r.terminatesCount == 1);
    CHECK(r.caughtCount == 1);
    const auto term = pathVia(r, "barrier");
    CHECK(term.outcome == PathOutcome::Terminates);
    CHECK(term.stopAt == "barrier");
    CHECK_FALSE(term.isCaught);
    CHECK(pathVia(r, "guarded").outcome == PathOutcome::Caught);
  }
  SECTION("only barriers: noexcept_barrier, exhaustive") {
    auto b = bake({R"cpp(
void target() { throw 1; }
void barrier() noexcept { target(); }
int main() { try { barrier(); } catch (...) {} }
)cpp"});
    ControlFlowOracle oracle(b.ix.graph, b.ix.cfIndex);
    auto r = oracle.queryThrowPropagation("target", "int", {"main"});
    // main's handler is outside the barrier: it never sees the exception.
    CHECK(r.protection == Protection::NoexceptBarrier);
    CHECK(r.verdictExhaustive);
    REQUIRE(r.paths.size() == 1);
    CHECK(r.paths[0].outcome == PathOutcome::Terminates);
    CHECK(r.paths[0].tryCatchesOnPath.size() == 1);
  }
}

TEST_CASE("a noexcept target terminates before any handler",
          "[paths][oracle]") {
  auto b = bake({R"cpp(
void helper() {}
void target() noexcept { helper(); throw 1; }
int main() { try { target(); } catch (...) {} }
)cpp"});
  ControlFlowOracle oracle(b.ix.graph, b.ix.cfIndex);
  auto r = oracle.queryThrowPropagation("target", "int", {"main"});
  REQUIRE(r.paths.size() == 1);
  CHECK(r.paths[0].outcome == PathOutcome::Terminates);
  CHECK(r.paths[0].stopAt == "target");
  CHECK(r.protection == Protection::NoexceptBarrier);
  CHECK(r.verdictExhaustive);
}

TEST_CASE("an entry point reached from another entry point contributes "
          "both paths",
          "[paths][oracle]") {
  auto b = bake({R"cpp(
void target() { throw 1; }
void api_b() { target(); }
void api_a() { try { api_b(); } catch (...) {} }
int main() { api_a(); }
)cpp"});
  ControlFlowOracle oracle(b.ix.graph, b.ix.cfIndex);
  auto one = oracle.queryExceptionProtection("target", "", {"main"});
  CHECK(one.protection == Protection::AlwaysCaught);
  auto all = oracle.queryExceptionProtection("target", "",
                                             {"main", "api_a", "api_b"});
  CHECK(all.paths.size() == 3);
  CHECK(all.verdictExhaustive);
  // The bare path from api_b is real once api_b is declared an entry;
  // the guarded path through api_a is still enumerated.
  CHECK(all.protection == Protection::SometimesCaught);
  CHECK(all.caughtCount == 2);
  CHECK(all.uncaughtCount == 1);
}

TEST_CASE("a call inside a handler body is not protected by that try",
          "[paths][oracle]") {
  auto b = bake({R"cpp(
void other() {}
void target() { throw 1; }
int main() { try { other(); } catch (...) { target(); } }
)cpp"});
  ControlFlowOracle oracle(b.ix.graph, b.ix.cfIndex);
  auto r = oracle.queryThrowPropagation("target", "int", {"main"});
  CHECK(r.protection == Protection::NeverCaught);
  REQUIRE(r.paths.size() == 1);
  CHECK(r.paths[0].outcome == PathOutcome::Uncaught);
  CHECK(r.paths[0].tryCatchesOnPath.empty());
}

TEST_CASE("calls in an if condition, init, or condition variable have a "
          "context and are not under the guard",
          "[paths][oracle]") {
  auto b = bake({R"cpp(
bool target() { throw 1; }
int main() {
  if (target()) return 1;
  try { if (auto ok = target()) return 2; } catch (...) {}
  if (bool r = target(); r) return 3;
  return 0;
}
)cpp"});
  ControlFlowOracle oracle(b.ix.graph, b.ix.cfIndex);
  auto r = oracle.queryThrowPropagation("target", "int", {"main"});
  REQUIRE(r.paths.size() == 3);
  CHECK(r.unknownCount == 0);
  CHECK(r.caughtCount == 1);
  CHECK(r.uncaughtCount == 2);
  CHECK(r.protection == Protection::SometimesCaught);
  CHECK(r.verdictExhaustive);
  for (const auto &p : r.paths) {
    INFO(p.hops[0].callSite);
    // The condition is evaluated before the branch is taken.
    CHECK(p.guardsOnPath.empty());
  }
}

TEST_CASE("thread and async boundaries separate the callee from the "
          "caller's stack",
          "[paths][oracle]") {
  SECTION("std::thread: the exception terminates the process") {
    auto b = bake({R"cpp(
#include <thread>
void target() { throw 1; }
int main() { try { std::thread t(target); t.join(); } catch (...) {} }
)cpp"});
    ControlFlowOracle oracle(b.ix.graph, b.ix.cfIndex);
    auto r = oracle.queryThrowPropagation("target", "int", {"main"});
    REQUIRE(r.paths.size() == 1);
    CHECK(r.paths[0].hops.back().execContext ==
          ExecutionContext::ThreadSpawn);
    CHECK(r.paths[0].outcome == PathOutcome::Terminates);
    CHECK(r.protection == Protection::NoexceptBarrier);
    CHECK(r.verdictExhaustive);
  }
  SECTION("std::invoke is synchronous: the caller's handler catches") {
    auto b = bake({R"cpp(
#include <functional>
void target() { throw 1; }
int main() { try { std::invoke(target); } catch (...) {} }
)cpp"});
    ControlFlowOracle oracle(b.ix.graph, b.ix.cfIndex);
    auto r = oracle.queryThrowPropagation("target", "int", {"main"});
    REQUIRE(r.paths.size() == 1);
    CHECK(r.paths[0].hops.back().execContext == ExecutionContext::Invoke);
    CHECK(r.paths[0].outcome == PathOutcome::Caught);
    CHECK(r.protection == Protection::AlwaysCaught);
  }
  SECTION("std::async: retrieval is not modeled, so the outcome is unknown") {
    auto b = bake({R"cpp(
#include <future>
void target() { throw 1; }
int main() { try { auto f = std::async(std::launch::async, target); f.get(); } catch (...) {} }
)cpp"});
    ControlFlowOracle oracle(b.ix.graph, b.ix.cfIndex);
    auto r = oracle.queryThrowPropagation("target", "int", {"main"});
    REQUIRE(r.paths.size() == 1);
    CHECK(r.paths[0].hops.back().execContext == ExecutionContext::AsyncTask);
    CHECK(r.paths[0].outcome == PathOutcome::Unknown);
    CHECK(r.unknownCount == 1);
    CHECK(r.protection == Protection::Unknown);
    CHECK_FALSE(r.verdictExhaustive);
  }
}

TEST_CASE("a hop without an indexed context is unknown, not unprotected",
          "[paths][oracle]") {
  CallGraph graph;
  graph.addNode({"main", "main.cpp", 1, true, false, ""});
  graph.addNode({"target", "t.cpp", 1, false, false, ""});
  graph.addEdge({"main", "target", EdgeKind::DirectCall, Confidence::Proven,
                 "main.cpp:2:3", 0});
  ControlFlowIndex cf; // nothing indexed
  ControlFlowOracle oracle(graph, cf);
  auto r = oracle.queryExceptionProtection("target", "", {"main"});
  REQUIRE(r.paths.size() == 1);
  CHECK(r.paths[0].outcome == PathOutcome::Unknown);
  CHECK(r.paths[0].note.find("no call-site context") != std::string::npos);
  CHECK(r.protection == Protection::Unknown);
  CHECK_FALSE(r.verdictExhaustive);

  SECTION("one unknown hop demotes an otherwise universal verdict") {
    graph.addNode({"guard", "g.cpp", 1, false, false, ""});
    graph.addEdge({"main", "guard", EdgeKind::DirectCall, Confidence::Proven,
                   "main.cpp:3:3", 0});
    graph.addEdge({"guard", "target", EdgeKind::DirectCall,
                   Confidence::Proven, "g.cpp:2:5", 0});
    CallSiteContext c;
    c.callerName = "guard";
    c.calleeName = "target";
    c.callSite = "g.cpp:2:5";
    TryCatchScope scope;
    scope.tryLocation = "g.cpp:2:1";
    scope.enclosingFunction = "guard";
    CatchHandlerInfo h;
    h.isCatchAll = true;
    scope.handlers.push_back(h);
    c.enclosingTryCatches.push_back(scope);
    cf.addCallSiteContext(c);
    CallSiteContext m;
    m.callerName = "main";
    m.calleeName = "guard";
    m.callSite = "main.cpp:3:3";
    cf.addCallSiteContext(m);
    auto r2 = oracle.queryExceptionProtection("target", "", {"main"});
    REQUIRE(r2.paths.size() == 2);
    CHECK(r2.caughtCount == 1);
    CHECK(r2.unknownCount == 1);
    CHECK(r2.protection == Protection::ObservedCaught);
    CHECK_FALSE(r2.verdictExhaustive);
  }
}

// ============================================================================
// Cutoffs: unexplored paths never yield an unconditional verdict
// ============================================================================

TEST_CASE("search cutoffs demote universal verdicts to observed ones",
          "[paths][oracle]") {
  // m1 guards its call; m2 and m3 do not. Canonical order is by usr, so a
  // one-path enumeration sees m1 only.
  auto b = bake({R"cpp(
void target() { throw 1; }
void m1() { try { target(); } catch (...) {} }
void m2() { target(); }
void m3() { target(); }
int main() { m1(); m2(); m3(); }
)cpp"});
  ControlFlowOracle oracle(b.ix.graph, b.ix.cfIndex);

  SECTION("no cutoff: sometimes_caught, exhaustive") {
    auto r = oracle.queryExceptionProtection("target", "", {"main"});
    CHECK(r.protection == Protection::SometimesCaught);
    CHECK(r.paths.size() == 3);
    CHECK(r.verdictExhaustive);
  }
  SECTION("path limit 1 sees only the guarded path: observed_caught") {
    SearchLimits lim;
    lim.maxPaths = 1;
    auto r = oracle.queryExceptionProtection("target", "", {"main"}, lim);
    REQUIRE(r.paths.size() == 1);
    CHECK(r.paths[0].callChain[1] == "m1");
    CHECK(r.protection == Protection::ObservedCaught);
    CHECK_FALSE(r.verdictExhaustive);
    CHECK_FALSE(r.search.complete);
    CHECK(r.search.stops == static_cast<unsigned>(StopReason::PathLimit));
    CHECK(r.summary.find("not exhaustive") != std::string::npos);
    CHECK(r.summary.find("path_limit") != std::string::npos);
  }
  SECTION("path limit 2 has both witnesses: sometimes_caught stands") {
    SearchLimits lim;
    lim.maxPaths = 2;
    auto r = oracle.queryExceptionProtection("target", "", {"main"}, lim);
    CHECK(r.protection == Protection::SometimesCaught);
    CHECK_FALSE(r.verdictExhaustive);
  }
  SECTION("path limit equal to the count is exhaustive") {
    SearchLimits lim;
    lim.maxPaths = 3;
    auto r = oracle.queryExceptionProtection("target", "", {"main"}, lim);
    CHECK(r.verdictExhaustive);
  }
  SECTION("a hub cutoff below the target's own fan-in still expands the "
          "target") {
    SearchLimits lim;
    lim.maxFanIn = 2; // target has three callers
    auto full = oracle.queryExceptionProtection("target", "", {"main"}, lim);
    CHECK(full.paths.size() == 3);
    CHECK(full.verdictExhaustive);
    CHECK(full.search.skippedHubs.empty());
  }
  SECTION("query_exception_safety spells the observed verdict") {
    std::vector<std::string> eps = {"main"};
    ToolContext ctx{b.ix.graph, oracle, b.ix.cfIndex, eps};
    llvm::json::Object args;
    args["function"] = "target";
    args["max_paths"] = 1;
    auto obj = payloadOf(findHandler("query_exception_safety")(args, ctx));
    CHECK(obj.getString("protection") == "observed_caught");
    CHECK(obj.getBoolean("complete") == false);
    CHECK(obj.getBoolean("exhaustive") == false);
    CHECK(stringsOf(obj, "stopReasons") ==
          std::vector<std::string>{"path_limit"});
    args["max_paths"] = 0;
    CHECK(isErrorResult(findHandler("query_exception_safety")(args, ctx)));
  }
}

TEST_CASE("depth cutoff: paths within the bound are complete, the verdict "
          "is bounded",
          "[paths][oracle]") {
  auto b = bake({R"cpp(
void target() { throw 1; }
void deep2() { target(); }
void deep1() { deep2(); }
int main() { deep1(); try { target(); } catch (...) {} }
)cpp"});
  ControlFlowOracle oracle(b.ix.graph, b.ix.cfIndex);
  SearchLimits lim;
  lim.maxDepth = 2;
  auto r = oracle.queryExceptionProtection("target", "", {"main"}, lim);
  REQUIRE(r.paths.size() == 1);
  CHECK(r.paths[0].outcome == PathOutcome::Caught);
  CHECK(r.search.complete);
  CHECK_FALSE(r.search.exhaustive);
  CHECK(r.search.stops == static_cast<unsigned>(StopReason::DepthLimit));
  CHECK(r.protection == Protection::ObservedCaught);

  lim.maxDepth = 3;
  auto full = oracle.queryExceptionProtection("target", "", {"main"}, lim);
  CHECK(full.protection == Protection::SometimesCaught);
  CHECK(full.verdictExhaustive);
}

TEST_CASE("hub cutoff: skipped ancestry is reported and the verdict is "
          "not universal",
          "[paths][oracle]") {
  auto b = bake({R"cpp(
void target() { throw 1; }
void hub() { target(); }
void x() { hub(); }
void y() { hub(); }
int main() { hub(); x(); y(); }
)cpp"});
  ControlFlowOracle oracle(b.ix.graph, b.ix.cfIndex);
  SearchLimits lim;
  lim.maxFanIn = 2;
  auto r = oracle.queryExceptionProtection("target", "", {"main"}, lim);
  CHECK(r.paths.empty());
  CHECK(r.protection == Protection::Unknown);
  CHECK(r.search.stops == static_cast<unsigned>(StopReason::HubPruned));
  REQUIRE(r.search.skippedHubs.size() == 1);
  CHECK(r.search.skippedHubs[0].name == "hub");
  CHECK(r.search.skippedHubs[0].inDegree == 3);
  CHECK(r.summary.find("hub_pruned") != std::string::npos);

  std::vector<std::string> eps = {"main"};
  ToolContext ctx{b.ix.graph, oracle, b.ix.cfIndex, eps};
  llvm::json::Object args;
  args["function"] = "target";
  args["max_fan_in"] = 2;
  auto obj = payloadOf(findHandler("query_throw_propagation")(args, ctx));
  CHECK(obj.getString("protection") == "unknown");
  auto *hubs = obj.getArray("skippedHubs");
  REQUIRE(hubs != nullptr);
  REQUIRE(hubs->size() == 1);
  CHECK((*hubs)[0].getAsObject()->getString("name") == "hub");
  CHECK((*hubs)[0].getAsObject()->getString("usr") == "c:@F@hub#");
  CHECK((*hubs)[0].getAsObject()->getInteger("inDegree") == 3);
}

// ============================================================================
// Insertion-order invariance
// ============================================================================

TEST_CASE("reordering the TUs changes neither the classification nor the "
          "path order",
          "[paths][oracle]") {
  const std::string decl = "void target();\n";
  const std::string tuTarget = "void target() { throw 1; }\n";
  const std::string tuA =
      decl + "void pa() { try { target(); } catch (...) {} }\n";
  const std::string tuB = decl + "void pb() { target(); }\n";
  const std::string tuMain =
      "void pa(); void pb();\nint main() { pb(); pa(); }\n";

  auto describe = [](const Baked &b) {
    ControlFlowOracle oracle(b.ix.graph, b.ix.cfIndex);
    auto r = oracle.queryExceptionProtection("target", "", {"main"});
    std::vector<std::string> out;
    out.push_back(protectionName(r.protection));
    for (const auto &p : r.paths) {
      std::string s = chainOf(p) + "=" + pathOutcomeName(p.outcome);
      for (const auto &h : p.hops)
        s += "|" + lineCol(h.callSite);
      out.push_back(s);
    }
    auto nc = oracle.queryNearestCatches("target");
    for (const auto &c : nc.catches)
      out.push_back("catch:" + c.scope.enclosingFunction + "@" +
                    lineCol(c.callSite));
    return out;
  };

  auto forward = describe(bake({tuTarget, tuA, tuB, tuMain}));
  auto backward = describe(bake({tuMain, tuB, tuA, tuTarget}));
  CHECK(forward == backward);
  REQUIRE(forward.size() >= 3);
  CHECK(forward[0] == "sometimes_caught");
  // Canonical order: pa before pb (by usr), whatever main's call order.
  CHECK(forward[1].rfind("main>pa>target=caught", 0) == 0);
  CHECK(forward[2].rfind("main>pb>target=uncaught", 0) == 0);
}

// ============================================================================
// Nearest catches
// ============================================================================

TEST_CASE("nearest catches reports every chain's first handler with its "
          "call site and hops",
          "[paths][oracle]") {
  auto b = bake({R"cpp(
void target() { throw 1; }
void inner() { target(); }
void a() { try { inner(); } catch (...) {} }
void b() { try { inner(); } catch (int) {} }
void bare() { inner(); }
int main() { a(); b(); bare(); }
)cpp"});
  ControlFlowOracle oracle(b.ix.graph, b.ix.cfIndex);
  auto r = oracle.queryNearestCatches("target");
  CHECK(r.targetKnown);
  CHECK(r.complete);
  CHECK(r.stops == 0);
  // Two chains reach a handler (through a and through b); the chain
  // through bare ends at main without one. Each handler is reported once
  // even though inner is shared.
  REQUIRE(r.catches.size() == 2);
  CHECK(r.catches[0].scope.enclosingFunction == "a");
  CHECK(r.catches[1].scope.enclosingFunction == "b");
  CHECK(r.catches[0].framesFromTarget == 2);
  CHECK(r.catches[0].pathSegment ==
        std::vector<std::string>{"a", "inner", "target"});
  REQUIRE(r.catches[0].hops.size() == 2);
  CHECK(r.catches[0].hops[0].caller == "a");
  CHECK(lineCol(r.catches[0].callSite) == "4:18");
  CHECK(r.catches[0].hops[0].callSite == r.catches[0].callSite);

  SECTION("a depth bound below the handler reports the cutoff") {
    auto shallow = oracle.queryNearestCatches("target", 1);
    CHECK(shallow.catches.empty());
    CHECK_FALSE(shallow.complete);
    CHECK(shallow.stops == static_cast<unsigned>(StopReason::DepthLimit));
  }
  SECTION("query_nearest_catches payload") {
    std::vector<std::string> eps = {"main"};
    ToolContext ctx{b.ix.graph, oracle, b.ix.cfIndex, eps};
    llvm::json::Object args;
    args["function"] = "target";
    auto obj = payloadOf(findHandler("query_nearest_catches")(args, ctx));
    CHECK(obj.getInteger("maxDepth") == 20);
    CHECK(obj.getBoolean("complete") == true);
    auto *catches = obj.getArray("catches");
    REQUIRE(catches != nullptr);
    REQUIRE(catches->size() == 2);
    auto *first = (*catches)[0].getAsObject();
    CHECK(first->getArray("hops")->size() == 2);
    CHECK(first->getString("callSite").has_value());
    args["max_depth"] = 1;
    auto cut = payloadOf(findHandler("query_nearest_catches")(args, ctx));
    CHECK(stringsOf(cut, "stopReasons") ==
          std::vector<std::string>{"depth_limit"});
  }
}

// ============================================================================
// Tool payloads: per-path detail and find_call_chain facts
// ============================================================================

TEST_CASE("query_throw_propagation paths carry outcome, hops, and rethrow "
          "locations",
          "[paths][tools]") {
  auto b = bake({R"cpp(
void target() { throw 1; }
void mid() { try { target(); } catch (...) { throw; } }
int main() { try { mid(); } catch (...) {} }
)cpp"});
  ControlFlowOracle oracle(b.ix.graph, b.ix.cfIndex);
  std::vector<std::string> eps = {"main"};
  ToolContext ctx{b.ix.graph, oracle, b.ix.cfIndex, eps};
  // A universal verdict needs stated coverage (docs/result-contract.md).
  ctx.facts.freshness = IndexFreshness::Baked;
  llvm::json::Object args;
  args["function"] = "target";
  args["exception_type"] = "int";
  auto obj = payloadOf(findHandler("query_throw_propagation")(args, ctx));
  CHECK(obj.getString("protection") == "always_caught");
  CHECK(obj.getBoolean("exhaustive") == true);
  auto *paths = obj.getArray("paths");
  REQUIRE(paths != nullptr);
  REQUIRE(paths->size() == 1);
  auto *p = (*paths)[0].getAsObject();
  CHECK(p->getString("outcome") == "caught");
  CHECK(p->getBoolean("isCaught") == true);
  CHECK(p->getString("caughtBy") == "...");
  CHECK(stringsOf(*p, "callChain") ==
        std::vector<std::string>{"main", "mid", "target"});
  auto *hops = p->getArray("hops");
  REQUIRE(hops != nullptr);
  REQUIRE(hops->size() == 2);
  auto *h0 = (*hops)[0].getAsObject();
  CHECK(h0->getString("from") == "main");
  CHECK(h0->getString("to") == "mid");
  CHECK(h0->getString("fromUsr") == "c:@F@main#");
  CHECK(h0->getString("toUsr") == "c:@F@mid#");
  CHECK(h0->getString("kind") == "DirectCall");
  CHECK(h0->getString("callSite").has_value());
  CHECK_FALSE(h0->get("executionContext"));
  CHECK(stringsOf(*p, "rethrownAt").size() == 1);
  CHECK_FALSE(p->get("stopAt"));

  SECTION("query_all_path_contexts lists hops and scopes, no outcome") {
    auto all = payloadOf(findHandler("query_all_path_contexts")(args, ctx));
    CHECK(all.getInteger("maxDepth") == 20);
    CHECK(all.getBoolean("exhaustive") == true);
    auto *ps = all.getArray("paths");
    REQUIRE(ps != nullptr);
    REQUIRE(ps->size() == 1);
    auto *q = (*ps)[0].getAsObject();
    CHECK_FALSE(q->get("outcome"));
    CHECK(q->getArray("hops")->size() == 2);
    CHECK(q->getArray("tryCatchesOnPath")->size() == 2);
  }
}

TEST_CASE("find_call_chain reports the search facts and display names",
          "[paths][tools]") {
  CallGraph graph;
  graph.addNode({"main", "main.cpp", 1, true, false, ""});
  graph.addNode({"a", "a.cpp", 1, false, false, ""});
  graph.addNode({"b", "b.cpp", 1, false, false, ""});
  graph.addNode({"target", "t.cpp", 1, false, false, ""});
  graph.addEdge({"main", "b", EdgeKind::DirectCall, Confidence::Proven,
                 "main.cpp:3:3", 0});
  graph.addEdge({"main", "a", EdgeKind::DirectCall, Confidence::Proven,
                 "main.cpp:2:3", 0});
  graph.addEdge({"a", "target", EdgeKind::DirectCall, Confidence::Proven,
                 "a.cpp:2:3", 0});
  graph.addEdge({"b", "target", EdgeKind::DirectCall, Confidence::Proven,
                 "b.cpp:2:3", 0});
  ControlFlowIndex cf;
  ControlFlowOracle oracle(graph, cf);
  std::vector<std::string> eps = {"main"};
  ToolContext ctx{graph, oracle, cf, eps};
  auto handler = findHandler("find_call_chain");

  llvm::json::Object args;
  args["to"] = "target";
  auto obj = payloadOf(handler(args, ctx));
  CHECK(obj.getInteger("pathCount") == 2);
  CHECK(obj.getBoolean("exhaustive") == true);
  CHECK(stringsOf(obj, "stopReasons").empty());
  auto *paths = obj.getArray("paths");
  REQUIRE(paths != nullptr);
  auto *first = (*paths)[0].getAsArray();
  REQUIRE(first != nullptr);
  auto *hop = (*first)[0].getAsObject();
  CHECK(hop->getString("from") == "main");
  CHECK(hop->getString("fromName") == "main");
  CHECK(hop->getString("to") == "a");
  CHECK(hop->getString("toName") == "a");

  args["max_paths"] = 1;
  auto cut = payloadOf(handler(args, ctx));
  CHECK(cut.getInteger("pathCount") == 1);
  CHECK(cut.getBoolean("complete") == false);
  CHECK(stringsOf(cut, "stopReasons") ==
        std::vector<std::string>{"path_limit"});

  args["max_paths"] = 0;
  CHECK(isErrorResult(handler(args, ctx)));
  args["max_paths"] = 1;
  args["max_depth"] = -1;
  CHECK(isErrorResult(handler(args, ctx)));

  SECTION("an unknown target is not_found, not an empty search") {
    llvm::json::Object none;
    none["to"] = "nope";
    auto result = handler(none, ctx);
    CHECK(statusOf(result) == ResultStatus::NotFound);
    auto *obj = result.getAsObject();
    REQUIRE(obj != nullptr);
    CHECK(obj->getString("parameter") == "to");
    CHECK(obj->getArray("didYouMean") != nullptr);
    CHECK(obj->get("pathCount") == nullptr);
  }
}

TEST_CASE("query_locks_held lists locks innermost frame first",
          "[paths][tools][concurrency]") {
  // main holds `outer` around mid(); mid holds `inner` around leaf().
  CallGraph graph;
  graph.addNode({"main", "m.cpp", 1, true, false, ""});
  graph.addNode({"mid", "d.cpp", 1, false, false, ""});
  graph.addNode({"leaf", "l.cpp", 1, false, false, ""});
  graph.addEdge({"main", "mid", EdgeKind::DirectCall, Confidence::Proven,
                 "m.cpp:5:3", 0});
  graph.addEdge({"mid", "leaf", EdgeKind::DirectCall, Confidence::Proven,
                 "d.cpp:4:3", 0});
  ControlFlowIndex cf;
  auto add = [&](const char *caller, const char *callee, const char *site,
                 const char *var) {
    CallSiteContext c;
    c.callerName = caller;
    c.calleeName = callee;
    c.callSite = site;
    RaiiLocal l;
    l.typeName = "std::lock_guard<std::mutex>";
    l.varName = var;
    l.kind = RaiiKind::Lock;
    c.liveRaiiLocals.push_back(l);
    cf.addCallSiteContext(c);
  };
  add("main", "mid", "m.cpp:5:3", "outer");
  add("mid", "leaf", "d.cpp:4:3", "inner");
  ControlFlowOracle oracle(graph, cf);
  std::vector<std::string> eps = {"main"};
  ToolContext ctx{graph, oracle, cf, eps};
  llvm::json::Object args;
  args["function"] = "leaf";
  auto obj = payloadOf(findHandler("query_locks_held")(args, ctx));
  auto *paths = obj.getArray("paths");
  REQUIRE(paths != nullptr);
  REQUIRE(paths->size() == 1);
  auto *locks = (*paths)[0].getAsObject()->getArray("locksHeld");
  REQUIRE(locks != nullptr);
  REQUIRE(locks->size() == 2);
  CHECK((*locks)[0].getAsObject()->getString("varName") == "inner");
  CHECK((*locks)[0].getAsObject()->getString("heldAt") == "d.cpp:4:3");
  CHECK((*locks)[1].getAsObject()->getString("varName") == "outer");
}

// ============================================================================
// Locks: SimpleEdges recursion and the exact-edge context join
// ============================================================================

TEST_CASE("query_locks_held observes a lock inside a recursive cycle and "
          "counts max_depth in frames",
          "[paths][tools][concurrency]") {
  // entry -> rec (lock held), rec -> rec, rec -> leaf.
  CallGraph graph;
  graph.addNode({"entry", "e.cpp", 1, true, false, ""});
  graph.addNode({"rec", "r.cpp", 1, false, false, ""});
  graph.addNode({"leaf", "l.cpp", 1, false, false, ""});
  graph.addEdge({"entry", "rec", EdgeKind::DirectCall, Confidence::Proven,
                 "e.cpp:3:3", 0});
  graph.addEdge({"rec", "rec", EdgeKind::DirectCall, Confidence::Proven,
                 "r.cpp:3:3", 0});
  graph.addEdge({"rec", "leaf", EdgeKind::DirectCall, Confidence::Proven,
                 "r.cpp:4:3", 0});
  ControlFlowIndex cf;
  {
    CallSiteContext c;
    c.callerName = "rec";
    c.calleeName = "rec";
    c.callSite = "r.cpp:3:3";
    RaiiLocal l;
    l.typeName = "std::lock_guard<std::mutex>";
    l.varName = "g";
    l.kind = RaiiKind::Lock;
    c.liveRaiiLocals.push_back(l);
    cf.addCallSiteContext(c);
  }
  ControlFlowOracle oracle(graph, cf);
  std::vector<std::string> eps = {"entry"};
  ToolContext ctx{graph, oracle, cf, eps};
  auto handler = findHandler("query_locks_held");

  llvm::json::Object args;
  args["function"] = "leaf";
  auto obj = payloadOf(handler(args, ctx));
  CHECK(obj.getInteger("pathCount") == 2);
  CHECK(obj.getBoolean("truncated") == false);
  CHECK(obj.getBoolean("exhaustive") == true);
  auto *paths = obj.getArray("paths");
  REQUIRE(paths != nullptr);
  bool sawRecursive = false;
  for (const auto &v : *paths) {
    auto *p = v.getAsObject();
    auto chain = stringsOf(*p, "path");
    if (chain == std::vector<std::string>{"entry", "rec", "rec", "leaf"}) {
      sawRecursive = true;
      CHECK(p->getArray("locksHeld")->size() == 1);
    } else {
      CHECK(chain == std::vector<std::string>{"entry", "rec", "leaf"});
      CHECK(p->getArray("locksHeld")->empty());
    }
  }
  CHECK(sawRecursive);

  // max_depth counts edges: 2 admits entry->rec->leaf only.
  args["max_depth"] = 2;
  auto cut = payloadOf(handler(args, ctx));
  CHECK(cut.getInteger("pathCount") == 1);
  CHECK(cut.getBoolean("truncated") == false);
  CHECK(cut.getBoolean("complete") == true);
  CHECK(cut.getBoolean("exhaustive") == false);
  CHECK(stringsOf(cut, "stopReasons") ==
        std::vector<std::string>{"depth_limit"});
}

TEST_CASE("contextForEdge joins on the exact caller and callee at a "
          "shared call site",
          "[paths][cfindex]") {
  // Two contexts stored at one call-site spelling (a macro expansion, or
  // two TUs' views of a header): only the caller identity tells them
  // apart, and the lock is live in one of them.
  ControlFlowIndex cf;
  auto add = [&](const std::string &caller, bool withLock) {
    CallSiteContext c;
    c.callerName = caller;
    c.calleeName = "callee";
    c.callSite = "h.h:5:3";
    if (withLock) {
      RaiiLocal l;
      l.typeName = "std::mutex";
      l.varName = "g";
      l.kind = RaiiKind::Lock;
      c.liveRaiiLocals.push_back(l);
    }
    cf.addCallSiteContext(c);
  };
  add("locked", true);
  add("plain", false);

  auto locked = cf.contextForEdge("h.h:5:3", "locked", "callee");
  REQUIRE(locked.has_value());
  CHECK(locked->liveRaiiLocals.size() == 1);
  auto plain = cf.contextForEdge("h.h:5:3", "plain", "callee");
  REQUIRE(plain.has_value());
  CHECK(plain->liveRaiiLocals.empty());
  CHECK_FALSE(cf.contextForEdge("h.h:5:3", "other", "callee").has_value());
  CHECK_FALSE(cf.contextForEdge("h.h:9:9", "locked", "callee").has_value());

  // The lock walk joins on the hop's caller: the plain caller's path
  // carries no lock even though the site has a locked context.
  CallGraph graph;
  graph.addNode({"locked", "locked.cpp", 1, true, false, ""});
  graph.addNode({"plain", "plain.cpp", 1, true, false, ""});
  graph.addNode({"callee", "c.cpp", 1, false, false, ""});
  graph.addEdge({"locked", "callee", EdgeKind::DirectCall,
                 Confidence::Proven, "h.h:5:3", 0});
  graph.addEdge({"plain", "callee", EdgeKind::DirectCall, Confidence::Proven,
                 "h.h:5:3", 0});
  ControlFlowOracle oracle(graph, cf);
  std::vector<std::string> eps = {"locked", "plain"};
  ToolContext ctx{graph, oracle, cf, eps};
  llvm::json::Object args;
  args["function"] = "callee";
  auto obj = payloadOf(findHandler("query_locks_held")(args, ctx));
  auto *paths = obj.getArray("paths");
  REQUIRE(paths != nullptr);
  REQUIRE(paths->size() == 2);
  for (const auto &v : *paths) {
    auto *p = v.getAsObject();
    bool isLocked = p->getString("entryPoint") == "locked";
    CHECK(p->getArray("locksHeld")->size() == (isLocked ? 1u : 0u));
  }
}
