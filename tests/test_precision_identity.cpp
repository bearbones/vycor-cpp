// test_precision_identity.cpp — F8 identity-precision gate (PR A of
// docs/design-f8-usr-identity.md).
//
// These tests assert the PRECISE identity semantics the USR core (PR B)
// must deliver, against the examples/precision fixtures. Until PR B
// lands, name-keyed identity collapses the fixture nodes, so the
// assertions are genuinely red — the [!shouldfail] tag keeps the suite
// green while pinning the redness: if identity precision arrives (or the
// fixtures rot), the tag makes the unexpected PASS a suite failure,
// flagging that the tag must be removed.
//
// PR B removes the [!shouldfail] tags in the same change that makes the
// assertions pass.

#include "vycor/callgraph/CallGraph.h"
#include "vycor/callgraph/CallGraphBuilder.h"
#include "vycor/callgraph/ControlFlowIndex.h"

#include <catch2/catch_test_macros.hpp>
#include <clang/Tooling/CompilationDatabase.h>

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

size_t nodesNamed(const CallGraph &g, const std::string &displayName) {
  size_t n = 0;
  for (const auto *node : g.allNodes()) {
    if (node->qualifiedName == displayName)
      ++n;
  }
  return n;
}

} // namespace

TEST_CASE("overloads are distinct nodes",
          "[precision][identity][!shouldfail]") {
  auto g = buildPrecisionGraph({"overloads.cpp"});

  // Two overloads of precision::process exist in the fixture with
  // disjoint caller sets. Name-keyed identity stores ONE node for both;
  // USR identity must store TWO (sharing a display name).
  CHECK(nodesNamed(g, "precision::process") == 2);
}

TEST_CASE("template specializations are distinct nodes",
          "[precision][identity][!shouldfail]") {
  auto g = buildPrecisionGraph({"templates.cpp"});

  // parse<Json> and parse<Yaml> are explicit specializations with
  // disjoint callers; both spell their qualified name "precision::parse".
  CHECK(nodesNamed(g, "precision::parse") == 2);
}

TEST_CASE("macro-shared call-site spellings keep distinct contexts",
          "[precision][identity]") {
  // Two functions expand CALL_GUARDED, so their calls to
  // precision::guarded share a SPELLING location (the macro definition
  // line). Both contexts are stored and reachable by callee today; the
  // still-missing precision (PR B: compound (site, caller) key) is
  // site-keyed retrieval of a SPECIFIC one — that assertion requires the
  // PR-B API and is added there. This test pins the fixture's premise so
  // it cannot rot silently: exactly two guarded-contexts, one per caller,
  // sharing one spelling.
  clang::tooling::FixedCompilationDatabase compDb(
      ".", {"-std=c++17", "-I" + fixtureDir()});
  std::vector<std::string> paths{fixtureDir() + "macro_sites.cpp"};
  auto graph = buildCallGraph(compDb, paths);
  auto cf = buildControlFlowIndex(compDb, paths, graph);

  auto contexts = cf.contextsForCallee("precision::guarded");
  REQUIRE(contexts.size() == 2);
  CHECK(contexts[0].callerName != contexts[1].callerName);
  // The macro collision itself: one spelling for both sites.
  CHECK(contexts[0].callSite == contexts[1].callSite);
}
