// test_deferred_edges.cpp — Query-time expansion of deferred
// function-pointer-through-return edges (EdgeKind::FunctionPointerReturn).
//
// The builder records `auto h = pick(); run(h);` as ONE stored edge whose
// callee is `pick` (the returning function); calleesOf/callersOf join it
// through the functionReturns relation at query time. This keeps edge
// building free of cross-TU reads (single-parse bake pipeline) and makes
// incremental indexing order-independent: returns recorded after the call
// site still become visible.

#include "vycor/callgraph/CallGraph.h"
#include "vycor/callgraph/CallGraphBuilder.h"
#include "vycor/callgraph/Snapshot.h"

#include <catch2/catch_test_macros.hpp>
#include <clang/Tooling/Tooling.h>

#include <cstdio>
#include <string>

using namespace vycor;

namespace {

bool hasEdge(const std::vector<CallGraphEdge> &edges, const std::string &from,
             const std::string &to, EdgeKind kind) {
  for (const auto &e : edges) {
    if (e.callerName == from && e.calleeName == to && e.kind == kind)
      return true;
  }
  return false;
}

bool hasCalleeRow(const std::vector<CallGraphEdge> &edges,
                  const std::string &to) {
  for (const auto &e : edges)
    if (e.calleeName == to)
      return true;
  return false;
}

} // namespace

TEST_CASE("deferred fn-ptr-return edges expand at query time",
          "[callgraph][deferred][builder]") {
  std::string code = R"(
    int fa(int x) { return x + 1; }
    int fb(int x) { return x + 2; }
    using Fn = int (*)(int);
    Fn pick(bool b) {
      if (b) return fa;
      return fb;
    }
    void run(Fn f);
    void driver() {
      auto h = pick(true);
      run(h);
    }
  )";

  CallGraph graph;
  CallGraphBuilderFactory factory(graph);
  REQUIRE(clang::tooling::runToolOnCodeWithArgs(factory.create(), code,
                                                {"-std=c++17"}, "test.cpp"));

  SECTION("calleesOf(driver) synthesizes edges to both returned targets") {
    auto callees = graph.calleesOf("driver");
    CHECK(hasEdge(callees, "driver", "fa", EdgeKind::FunctionPointer));
    CHECK(hasEdge(callees, "driver", "fb", EdgeKind::FunctionPointer));
    // The raw deferred row (callee = pick, kind = FunctionPointerReturn)
    // must never be materialized.
    for (const auto &e : callees)
      CHECK(e.kind != EdgeKind::FunctionPointerReturn);
  }

  SECTION("callersOf(fa) sees driver through the deferred join") {
    auto callers = graph.callersOf("fa");
    bool found = false;
    for (const auto &e : callers) {
      if (e.callerName == "driver" && e.kind == EdgeKind::FunctionPointer)
        found = true;
      CHECK(e.kind != EdgeKind::FunctionPointerReturn);
    }
    CHECK(found);
  }

  SECTION("callersOf(pick) shows the direct call, not the deferred row") {
    auto callers = graph.callersOf("pick");
    CHECK(hasEdge(callers, "driver", "pick", EdgeKind::DirectCall));
    for (const auto &e : callers)
      CHECK(e.kind != EdgeKind::FunctionPointerReturn);
  }
}

TEST_CASE("deferred edges are order-independent across TUs",
          "[callgraph][deferred][incremental]") {
  // TU 1 consumes the pointer; it only sees pick's DECLARATION, so at its
  // parse time nothing is known about what pick returns. The historical
  // build resolved the join during TU 1's parse and silently found nothing
  // unless TU 2 had been indexed first.
  std::string tu1 = R"(
    using Fn = int (*)(int);
    Fn pick(bool b);
    void run(Fn f);
    void driver() {
      auto h = pick(true);
      run(h);
    }
  )";
  // TU 2 defines pick and the returned functions.
  std::string tu2 = R"(
    int fa(int x) { return x + 1; }
    using Fn = int (*)(int);
    Fn pick(bool b) { return fa; }
  )";

  CallGraph graph;
  {
    CallGraphBuilderFactory factory(graph);
    REQUIRE(clang::tooling::runToolOnCodeWithArgs(
        factory.create(), tu1, {"-std=c++17"}, "tu1.cpp"));
  }
  {
    CallGraphBuilderFactory factory(graph);
    REQUIRE(clang::tooling::runToolOnCodeWithArgs(
        factory.create(), tu2, {"-std=c++17"}, "tu2.cpp"));
  }

  // Even though TU 1 (the consumer) was indexed BEFORE TU 2 (the definer),
  // the query-time join sees the returns.
  auto callees = graph.calleesOf("driver");
  CHECK(hasEdge(callees, "driver", "fa", EdgeKind::FunctionPointer));

  auto callers = graph.callersOf("fa");
  CHECK(hasEdge(callers, "driver", "fa", EdgeKind::FunctionPointer));
}

TEST_CASE("unknown returns synthesize nothing", "[callgraph][deferred]") {
  // pick is never defined anywhere: the deferred row's join is empty, so
  // driver's callees contain no trace of it (matching the historical
  // behavior, where an empty getFunctionReturns produced no edge).
  std::string code = R"(
    using Fn = int (*)(int);
    Fn pick(bool b);
    void run(Fn f);
    void driver() {
      auto h = pick(true);
      run(h);
    }
  )";

  CallGraph graph;
  CallGraphBuilderFactory factory(graph);
  REQUIRE(clang::tooling::runToolOnCodeWithArgs(factory.create(), code,
                                                {"-std=c++17"}, "test.cpp"));

  auto callees = graph.calleesOf("driver");
  CHECK(hasCalleeRow(callees, "pick")); // the direct call to pick()
  for (const auto &e : callees) {
    CHECK(e.kind != EdgeKind::FunctionPointerReturn);
    // No phantom FunctionPointer edge to an unknown target.
    if (e.kind == EdgeKind::FunctionPointer)
      CHECK(e.calleeName != "pick");
  }
}

TEST_CASE("deferred edges survive a snapshot round-trip",
          "[callgraph][deferred][snapshot]") {
  CallGraph graph;
  graph.addNode({"driver", "a.cpp", 1, false, false, ""}, "a.cpp");
  graph.addNode({"pick", "b.cpp", 1, false, false, ""}, "b.cpp");
  graph.addNode({"fa", "b.cpp", 5, false, false, ""}, "b.cpp");
  graph.addEdge({"driver", "pick", EdgeKind::FunctionPointerReturn,
                 Confidence::Proven, "a.cpp:3:5", 2,
                 ExecutionContext::Synchronous},
                "a.cpp");
  graph.addFunctionReturn("pick", "fa", "b.cpp");

  ControlFlowIndex cfIndex;
  SnapshotMeta meta;
  std::string path = std::string("deferred_edges_test.snapshot");
  REQUIRE(SnapshotIO::save(path, graph, cfIndex, meta));

  auto loaded = SnapshotIO::load(path);
  std::remove(path.c_str());
  REQUIRE(loaded.has_value());

  auto callees = loaded->graph.calleesOf("driver");
  CHECK(hasEdge(callees, "driver", "fa", EdgeKind::FunctionPointer));
  // The reverse join (returnedBy) must be rebuilt by the loader replay.
  auto callers = loaded->graph.callersOf("fa");
  CHECK(hasEdge(callers, "driver", "fa", EdgeKind::FunctionPointer));
}

TEST_CASE("deferred spawner args expand to ThreadEntry",
          "[callgraph][deferred][concurrency]") {
  // A pointer produced by pick() handed to std::thread: the deferred row
  // records the spawner context, and expansion synthesizes ThreadEntry.
  std::string code = R"(
    namespace std {
      class thread {
      public:
        template <typename F, typename... A> thread(F &&f, A &&...a);
        ~thread();
        void join();
      };
    }
    int fa(int x) { return x + 1; }
    using Fn = int (*)(int);
    Fn pick(bool b) { return fa; }
    void driver() {
      auto h = pick(true);
      std::thread t(h, 1);
      t.join();
    }
  )";

  CallGraph graph;
  CallGraphBuilderFactory factory(graph);
  REQUIRE(clang::tooling::runToolOnCodeWithArgs(factory.create(), code,
                                                {"-std=c++17"}, "test.cpp"));

  auto callees = graph.calleesOf("driver");
  bool threadEntry = false;
  for (const auto &e : callees) {
    if (e.calleeName == "fa" && e.kind == EdgeKind::ThreadEntry &&
        e.execContext == ExecutionContext::ThreadSpawn)
      threadEntry = true;
  }
  CHECK(threadEntry);
}
