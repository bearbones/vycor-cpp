// test_precision_identity.cpp — F8 identity-precision gate
// (docs/design-f8-usr-identity.md).
//
// These tests assert the PRECISE identity semantics the USR core (PR B)
// delivers, against the examples/precision fixtures. They were authored in
// PR A as [!shouldfail]-tagged red tests; PR B (nodes keyed by USR, the
// compound (site, caller) context key) removed the tags in the same change
// that made the assertions pass.

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
          "[precision][identity]") {
  auto g = buildPrecisionGraph({"overloads.cpp"});

  // Two overloads of precision::process exist in the fixture with
  // disjoint caller sets. Name-keyed identity stores ONE node for both;
  // USR identity must store TWO (sharing a display name).
  CHECK(nodesNamed(g, "precision::process") == 2);
}

TEST_CASE("template specializations are distinct nodes",
          "[precision][identity]") {
  auto g = buildPrecisionGraph({"templates.cpp"});

  // parse<Json> and parse<Yaml> are explicit specializations with
  // disjoint callers; both spell their qualified name "precision::parse".
  CHECK(nodesNamed(g, "precision::parse") == 2);
}

TEST_CASE("macro-shared call-site spellings keep distinct contexts",
          "[precision][identity]") {
  // Two functions expand CALL_GUARDED, so their calls to
  // precision::guarded share a SPELLING location (the macro definition
  // line). Both contexts must be stored, reachable by callee, and — the
  // PR-B compound (site, caller) key — retrievable INDIVIDUALLY by
  // caller-qualified site lookup.
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

  // Compound-key retrieval: the shared spelling plus a caller resolves to
  // that caller's specific context.
  const std::string &spelling = contexts[0].callSite;
  auto one = cf.contextAtSite(spelling, "precision::userOne");
  auto two = cf.contextAtSite(spelling, "precision::userTwo");
  REQUIRE(one.has_value());
  REQUIRE(two.has_value());
  CHECK(one->callerName == "precision::userOne");
  CHECK(two->callerName == "precision::userTwo");
  CHECK(one->calleeName == "precision::guarded");
  CHECK(two->calleeName == "precision::guarded");
  CHECK(one->callerUsr != two->callerUsr);
  // The spelling-only lookup still answers (first match, documented).
  CHECK(cf.contextAtSite(spelling).has_value());
}
