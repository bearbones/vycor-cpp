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

// test_dead_code.cpp — Tests for the call graph library and dead code analyzer.

#include "vycor/callgraph/CallGraph.h"
#include "vycor/callgraph/CallGraphBuilder.h"
#include "vycor/anneal/DeadCodeAnalyzer.h"

#include <catch2/catch_test_macros.hpp>
#include <clang/Tooling/CompilationDatabase.h>
#include <clang/Tooling/Tooling.h>
#include <fstream>
#include <sstream>
#include <string>

using namespace vycor;

static std::string readFile(const std::string &path) {
  std::ifstream in(path);
  if (!in.is_open())
    return {};
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// ============================================================================
// Smoke tests: verify the example project files are loadable
// ============================================================================

TEST_CASE("Dead code example files are loadable", "[dead_code][smoke]") {
  const std::string base =
      std::string(PROJECT_SOURCE_DIR) + "/examples/dead_code/";

  SECTION("main.cpp exists") {
    auto content = readFile(base + "main.cpp");
    CHECK_FALSE(content.empty());
  }

  SECTION("all headers exist") {
    CHECK_FALSE(readFile(base + "api.hpp").empty());
    CHECK_FALSE(readFile(base + "shapes.hpp").empty());
    CHECK_FALSE(readFile(base + "callbacks.hpp").empty());
    CHECK_FALSE(readFile(base + "internal.hpp").empty());
  }

  SECTION("all implementation files exist") {
    CHECK_FALSE(readFile(base + "api.cpp").empty());
    CHECK_FALSE(readFile(base + "shapes.cpp").empty());
    CHECK_FALSE(readFile(base + "callbacks.cpp").empty());
    CHECK_FALSE(readFile(base + "internal.cpp").empty());
  }

  SECTION("expected_liveness.json exists and is non-empty") {
    auto content = readFile(base + "expected_liveness.json");
    CHECK_FALSE(content.empty());
    CHECK(content.find("\"entry_points\"") != std::string::npos);
    CHECK(content.find("\"pessimistic\"") != std::string::npos);
    CHECK(content.find("\"optimistic\"") != std::string::npos);
  }
}

// ============================================================================
// CallGraph data structure unit tests
// ============================================================================

TEST_CASE("CallGraph stores nodes and edges", "[dead_code][callgraph]") {
  CallGraph graph;

  SECTION("empty graph has no nodes") {
    CHECK(graph.nodeCount() == 0);
    CHECK(graph.edgeCount() == 0);
  }

  SECTION("add nodes and query") {
    graph.addNode({"main", "main.cpp", 10, true, false, ""});
    graph.addNode({"foo", "foo.cpp", 5, false, false, ""});
    CHECK(graph.nodeCount() == 2);
    CHECK(graph.findNode("main") != nullptr);
    CHECK(graph.findNode("bar") == nullptr);
  }

  SECTION("add edges and query adjacency") {
    graph.addNode({"main", "main.cpp", 10, true, false, ""});
    graph.addNode({"foo", "foo.cpp", 5, false, false, ""});
    graph.addEdge({"main", "foo", EdgeKind::DirectCall, Confidence::Proven,
                   "main.cpp:12:5", 0});
    auto callees = graph.calleesOf("main");
    REQUIRE(callees.size() == 1);
    CHECK(callees[0]->calleeName == "foo");

    auto callers = graph.callersOf("foo");
    REQUIRE(callers.size() == 1);
    CHECK(callers[0]->callerName == "main");
  }

  SECTION("virtual dispatch edges have Plausible confidence") {
    graph.addNode(
        {"print_shape_info", "shapes.cpp", 80, false, false, ""});
    graph.addNode(
        {"Circle::area", "shapes.cpp", 14, false, true, "Circle"});
    graph.addNode(
        {"Square::area", "shapes.cpp", 54, false, true, "Square"});
    graph.addEdge({"print_shape_info", "Circle::area",
                   EdgeKind::VirtualDispatch, Confidence::Proven,
                   "shapes.cpp:82:20", 0});
    graph.addEdge({"print_shape_info", "Square::area",
                   EdgeKind::VirtualDispatch, Confidence::Plausible,
                   "shapes.cpp:82:20", 0});

    auto callees = graph.calleesOf("print_shape_info");
    REQUIRE(callees.size() == 2);

    bool hasProven = false, hasPlausible = false;
    for (const auto *e : callees) {
      if (e->confidence == Confidence::Proven)
        hasProven = true;
      if (e->confidence == Confidence::Plausible)
        hasPlausible = true;
    }
    CHECK(hasProven);
    CHECK(hasPlausible);
  }

  SECTION("duplicate nodes are merged") {
    graph.addNode({"foo", "foo.cpp", 5, false, false, ""});
    graph.addNode({"foo", "foo2.cpp", 10, true, false, ""});
    CHECK(graph.nodeCount() == 1);
    auto *node = graph.findNode("foo");
    REQUIRE(node != nullptr);
    CHECK(node->isEntryPoint == true); // Updated from second add.
  }
}

// ============================================================================
// Liveness propagation unit tests
// ============================================================================

TEST_CASE("Liveness propagation from entry points",
          "[dead_code][liveness]") {
  // Build a small hand-crafted graph:
  //   main -> foo (Proven)
  //   main -> bar (Plausible, FunctionPointer)
  //   foo  -> baz (Proven)
  //   (qux has no incoming edges)
  CallGraph graph;
  graph.addNode({"main", "main.cpp", 1, true, false, ""});
  graph.addNode({"foo", "foo.cpp", 1, false, false, ""});
  graph.addNode({"bar", "bar.cpp", 1, false, false, ""});
  graph.addNode({"baz", "baz.cpp", 1, false, false, ""});
  graph.addNode({"qux", "qux.cpp", 1, false, false, ""});

  graph.addEdge({"main", "foo", EdgeKind::DirectCall, Confidence::Proven,
                 "main.cpp:5:3", 0});
  graph.addEdge({"main", "bar", EdgeKind::FunctionPointer,
                 Confidence::Plausible, "main.cpp:6:3", 0});
  graph.addEdge(
      {"foo", "baz", EdgeKind::DirectCall, Confidence::Proven,
       "foo.cpp:3:3", 0});

  SECTION("pessimistic mode: only proven paths") {
    DeadCodeAnalyzer analyzer(graph, {"main"});
    analyzer.analyzePessimistic();
    analyzer.analyzeOptimistic();
    auto results = analyzer.getResults();

    CHECK(results["main"] == Liveness::Alive);
    CHECK(results["foo"] == Liveness::Alive);
    CHECK(results["baz"] == Liveness::Alive);
    CHECK(results["bar"] == Liveness::OptimisticallyAlive);
    CHECK(results["qux"] == Liveness::Dead);
  }

  SECTION("optimistic mode: proven + plausible paths") {
    DeadCodeAnalyzer analyzer(graph, {"main"});
    analyzer.analyzePessimistic();
    analyzer.analyzeOptimistic();
    auto results = analyzer.getResults();

    // bar is OptimisticallyAlive (reachable via Plausible edge)
    CHECK(results.at("bar") == Liveness::OptimisticallyAlive);
    // qux is Dead
    CHECK(results.at("qux") == Liveness::Dead);
  }

  SECTION("cascading plausibility") {
    // main -> A (Proven) -> B (Plausible, FunctionPointer) -> C (Proven)
    CallGraph g2;
    g2.addNode({"main", "m.cpp", 1, true, false, ""});
    g2.addNode({"A", "a.cpp", 1, false, false, ""});
    g2.addNode({"B", "b.cpp", 1, false, false, ""});
    g2.addNode({"C", "c.cpp", 1, false, false, ""});

    g2.addEdge({"main", "A", EdgeKind::DirectCall, Confidence::Proven,
                "m.cpp:2:1", 0});
    g2.addEdge({"A", "B", EdgeKind::FunctionPointer, Confidence::Plausible,
                "a.cpp:2:1", 0});
    g2.addEdge(
        {"B", "C", EdgeKind::DirectCall, Confidence::Proven, "b.cpp:2:1", 0});

    DeadCodeAnalyzer analyzer(g2, {"main"});
    analyzer.analyzePessimistic();
    analyzer.analyzeOptimistic();
    auto results = analyzer.getResults();

    CHECK(results["main"] == Liveness::Alive);
    CHECK(results["A"] == Liveness::Alive);
    CHECK(results["B"] == Liveness::OptimisticallyAlive);
    CHECK(results["C"] == Liveness::OptimisticallyAlive);
  }
}

// ============================================================================
// Integration tests using in-memory compilation
// ============================================================================

TEST_CASE("CallGraphBuilder indexes direct calls",
          "[dead_code][builder][integration]") {
  std::string code = R"(
    void bar() {}
    void foo() { bar(); }
    int main() { foo(); return 0; }
  )";

  CallGraph graph;
  CallGraphBuilderFactory factory(graph);
  REQUIRE(clang::tooling::runToolOnCodeWithArgs(factory.create(), code,
                                                {"-std=c++17"}, "test.cpp"));

  CHECK(graph.findNode("bar") != nullptr);
  CHECK(graph.findNode("foo") != nullptr);
  CHECK(graph.findNode("main") != nullptr);

  auto callees = graph.calleesOf("main");
  bool callsFoo = false;
  for (const auto *e : callees) {
    if (e->calleeName == "foo" && e->kind == EdgeKind::DirectCall &&
        e->confidence == Confidence::Proven)
      callsFoo = true;
  }
  CHECK(callsFoo);

  auto fooCallees = graph.calleesOf("foo");
  bool callsBar = false;
  for (const auto *e : fooCallees) {
    if (e->calleeName == "bar" && e->kind == EdgeKind::DirectCall)
      callsBar = true;
  }
  CHECK(callsBar);
}

TEST_CASE("CallGraphBuilder tracks virtual dispatch",
          "[dead_code][builder][virtual]") {
  std::string code = R"(
    struct Base {
      virtual ~Base() = default;
      virtual int value() const = 0;
    };
    struct Derived : Base {
      int value() const override { return 42; }
    };
    int use(const Base& b) { return b.value(); }
    int main() { Derived d; return use(d); }
  )";

  CallGraph graph;
  CallGraphBuilderFactory factory(graph);
  REQUIRE(clang::tooling::runToolOnCodeWithArgs(factory.create(), code,
                                                {"-std=c++17"}, "test.cpp"));

  // use() should have a VirtualDispatch edge to Derived::value.
  auto useCallees = graph.calleesOf("use");
  bool hasDerivedEdge = false;
  for (const auto *e : useCallees) {
    if (e->calleeName == "Derived::value" &&
        e->kind == EdgeKind::VirtualDispatch)
      hasDerivedEdge = true;
  }
  CHECK(hasDerivedEdge);

  // main should have "concrete type knowledge" edges for Derived.
  auto mainCallees = graph.calleesOf("main");
  bool mainHasDerivedValue = false;
  for (const auto *e : mainCallees) {
    if (e->calleeName == "Derived::value" &&
        e->confidence == Confidence::Proven)
      mainHasDerivedValue = true;
  }
  CHECK(mainHasDerivedValue);
}

TEST_CASE("CallGraphBuilder tracks function pointer indirection",
          "[dead_code][builder][fnptr]") {
  std::string code = R"(
    double transform(double x) { return x * 2; }
    double noop(double x) { return x; }
    using Fn = double(*)(double);
    double apply(Fn f, double x) { return f(x); }
    int main() {
      double r = apply(transform, 1.0);
      Fn unused = noop;
      (void)unused;
      (void)r;
      return 0;
    }
  )";

  CallGraph graph;
  CallGraphBuilderFactory factory(graph);
  REQUIRE(clang::tooling::runToolOnCodeWithArgs(factory.create(), code,
                                                {"-std=c++17"}, "test.cpp"));

  // transform should be reachable from main via FunctionPointer Proven.
  auto mainCallees = graph.calleesOf("main");
  bool hasTransformProven = false;
  bool hasNoopPlausible = false;
  for (const auto *e : mainCallees) {
    if (e->calleeName == "transform" && e->kind == EdgeKind::FunctionPointer &&
        e->confidence == Confidence::Proven)
      hasTransformProven = true;
    if (e->calleeName == "noop" && e->kind == EdgeKind::FunctionPointer &&
        e->confidence == Confidence::Plausible)
      hasNoopPlausible = true;
  }
  CHECK(hasTransformProven);
  CHECK(hasNoopPlausible);

  // Full liveness analysis.
  DeadCodeAnalyzer analyzer(graph, {"main"});
  analyzer.analyzePessimistic();
  analyzer.analyzeOptimistic();
  auto results = analyzer.getResults();

  CHECK(results["transform"] == Liveness::Alive);
  CHECK(results["noop"] == Liveness::OptimisticallyAlive);
}

// ============================================================================
// Integration tests using example files
// ============================================================================

// Simple JSON key-value extractor (no external JSON library needed).
// Extracts string values for keys inside "functions" -> funcName -> key.
static std::string extractJsonValue(const std::string &json,
                                    const std::string &funcName,
                                    const std::string &key) {
  // Find the function entry.
  auto funcPos = json.find("\"" + funcName + "\"");
  if (funcPos == std::string::npos)
    return "";

  // Find the key within the function entry's object.
  auto braceStart = json.find('{', funcPos);
  if (braceStart == std::string::npos)
    return "";
  auto braceEnd = json.find('}', braceStart);
  if (braceEnd == std::string::npos)
    return "";

  std::string block = json.substr(braceStart, braceEnd - braceStart + 1);
  auto keyPos = block.find("\"" + key + "\"");
  if (keyPos == std::string::npos)
    return "";

  auto colonPos = block.find(':', keyPos);
  if (colonPos == std::string::npos)
    return "";

  auto valStart = block.find('"', colonPos + 1);
  if (valStart == std::string::npos)
    return "";
  auto valEnd = block.find('"', valStart + 1);
  if (valEnd == std::string::npos)
    return "";

  return block.substr(valStart + 1, valEnd - valStart - 1);
}

TEST_CASE("Dead code analysis on example project",
          "[dead_code][integration][example]") {
  const std::string base =
      std::string(PROJECT_SOURCE_DIR) + "/examples/dead_code/";

  // Load expected results.
  std::string expectedJson = readFile(base + "expected_liveness.json");
  REQUIRE_FALSE(expectedJson.empty());

  // Create a compilation database for the example files.
  std::vector<std::string> sourceFiles = {
      base + "api.cpp", base + "shapes.cpp", base + "callbacks.cpp",
      base + "internal.cpp", base + "main.cpp"};

  clang::tooling::FixedCompilationDatabase compDb(
      ".", {"-std=c++17", "-I" + base});

  // Build the call graph.
  auto graph = buildCallGraph(compDb, sourceFiles);

  // Run dead code analysis.
  DeadCodeAnalyzer analyzer(graph, {"main"});
  analyzer.analyzePessimistic();
  analyzer.analyzeOptimistic();
  auto results = analyzer.getResults();

  // List of functions to check (from expected_liveness.json).
  struct Expected {
    std::string name;
    std::string pessimistic;
    std::string optimistic;
  };

  std::vector<Expected> expectedFuncs = {
      {"main", "alive", "alive"},
      {"mathutil::sum", "alive", "alive"},
      {"mathutil::mean", "dead", "dead"},
      {"mathutil::normalize", "alive", "alive"},
      {"mathutil::obscure_transform", "dead", "dead"},
      {"Circle::area", "alive", "alive"},
      {"Circle::name", "alive", "alive"},
      {"Circle::debug_print", "alive", "alive"},
      {"Circle::circumference", "alive", "alive"},
      {"Triangle::area", "alive", "alive"},
      {"Triangle::name", "alive", "alive"},
      {"Triangle::debug_print", "alive", "alive"},
      {"Triangle::hypotenuse", "dead", "dead"},
      {"Square::area", "dead", "alive"},
      {"Square::name", "dead", "alive"},
      {"Hexagon::area", "dead", "dead"},
      {"Hexagon::name", "dead", "dead"},
      {"Hexagon::debug_print", "dead", "dead"},
      {"print_shape_info", "alive", "alive"},
      {"make_shape", "alive", "alive"},
      {"double_it", "alive", "alive"},
      {"triple_it", "alive", "alive"},
      {"negate_it", "dead", "alive"},
      {"square_it", "dead", "dead"},
      {"apply_once", "alive", "alive"},
      {"apply_chain", "dead", "dead"},
      {"select_transform", "alive", "alive"},
      {"internal::accumulate_helper", "alive", "alive"},
      {"internal::old_normalize", "dead", "dead"},
      {"internal::log_value", "dead", "dead"},
  };

  SECTION("pessimistic mode matches expected") {
    for (const auto &exp : expectedFuncs) {
      auto it = results.find(exp.name);
      if (it == results.end()) {
        // Function not in graph; it should be dead.
        if (exp.pessimistic != "dead") {
          FAIL_CHECK("Function " << exp.name
                                 << " not found in results but expected "
                                 << exp.pessimistic);
        }
        continue;
      }

      bool expectedAlive = (exp.pessimistic == "alive");
      bool actualAlive = (it->second == Liveness::Alive);

      if (expectedAlive != actualAlive) {
        FAIL_CHECK("Pessimistic mismatch for "
                   << exp.name << ": expected "
                   << (expectedAlive ? "alive" : "dead") << " but got "
                   << (it->second == Liveness::Alive
                           ? "alive"
                           : (it->second == Liveness::OptimisticallyAlive
                                  ? "optimistically_alive"
                                  : "dead")));
      }
    }
  }

  SECTION("optimistic mode matches expected") {
    for (const auto &exp : expectedFuncs) {
      auto it = results.find(exp.name);
      if (it == results.end()) {
        if (exp.optimistic != "dead") {
          FAIL_CHECK("Function " << exp.name
                                 << " not found in results but expected "
                                 << exp.optimistic);
        }
        continue;
      }

      bool expectedAlive = (exp.optimistic == "alive");
      bool actualAlive = (it->second == Liveness::Alive ||
                          it->second == Liveness::OptimisticallyAlive);

      if (expectedAlive != actualAlive) {
        FAIL_CHECK("Optimistic mismatch for "
                   << exp.name << ": expected "
                   << (expectedAlive ? "alive" : "dead") << " but got "
                   << (it->second == Liveness::Alive
                           ? "alive"
                           : (it->second == Liveness::OptimisticallyAlive
                                  ? "optimistically_alive"
                                  : "dead")));
      }
    }
  }
}
