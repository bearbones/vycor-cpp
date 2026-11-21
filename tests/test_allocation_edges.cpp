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

// test_allocation_edges.cpp — The edges a new-expression and a
// delete-expression contribute: a DirectCall to the allocation or
// deallocation function overload resolution selected, which no CallExpr
// spells. Without them "does this path allocate" sees only allocations
// written as calls (`malloc`, an explicit `operator new(n)`).

#include "vycor/callgraph/CallGraph.h"
#include "vycor/callgraph/CallGraphBuilder.h"
#include "vycor/callgraph/Effects.h"

#include <catch2/catch_test_macros.hpp>
#include <clang/Tooling/Tooling.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace vycor;

namespace {

// No standard header: the declarations <new> would bring, spelled out, so
// the test does not depend on the host's standard library.
const char *const kPrelude = R"(
using size_t = decltype(sizeof 0);
namespace std {
struct nothrow_t {};
enum class align_val_t : size_t {};
}
void *operator new(size_t, void *) noexcept;
void *operator new[](size_t, void *) noexcept;
void *operator new(size_t, const std::nothrow_t &) noexcept;
void *operator new(size_t, std::align_val_t);
struct Widget { Widget(); ~Widget(); int x; };
)";

void build(CallGraph &graph, const std::string &code,
           const char *standard = "-std=c++17") {
  CallGraphBuilderFactory factory(graph);
  REQUIRE(clang::tooling::runToolOnCodeWithArgs(
      factory.create(), std::string(kPrelude) + code, {standard},
      "test.cpp"));
}

std::vector<CallGraphEdge> calleesNamed(const CallGraph &graph,
                                        const std::string &caller,
                                        const std::string &callee) {
  std::vector<CallGraphEdge> out;
  for (const CallGraphEdge &edge : graph.calleesOf(caller))
    if (edge.calleeName == callee)
      out.push_back(edge);
  return out;
}

// The one edge from `caller` to a function displayed as `callee`.
CallGraphEdge only(const CallGraph &graph, const std::string &caller,
                   const std::string &callee) {
  std::vector<CallGraphEdge> edges = calleesNamed(graph, caller, callee);
  REQUIRE(edges.size() == 1);
  return edges.front();
}

bool allocates(const CallGraphEdge &edge) {
  static const EffectTable table = EffectTable::builtin();
  return table.effectsOf(edge.calleeName, edge.calleeUsr) &
         effectBit(EffectKind::Allocation);
}

} // namespace

TEST_CASE("a new-expression is a call of its allocation function",
          "[callgraph][builder][allocation]") {
  CallGraph graph;
  build(graph, R"(
    Widget *plain() { return new Widget; }
    int *array(int n) { return new int[n]; }
    Widget *quiet() { return new (std::nothrow_t{}) Widget; }
    struct alignas(64) Wide { int x; };
    Wide *wide() { return new Wide; }
  )");

  SECTION("`new T` calls operator new and the constructor") {
    CallGraphEdge edge = only(graph, "plain", "operator new");
    CHECK(edge.kind == EdgeKind::DirectCall);
    CHECK(edge.confidence == Confidence::Proven);
    CHECK(edge.callSite.find("test.cpp:") != std::string::npos);
    CHECK(allocates(edge));
    CHECK(only(graph, "plain", "Widget::Widget").kind ==
          EdgeKind::ConstructorCall);
  }

  SECTION("`new T[n]` calls operator new[]") {
    CallGraphEdge edge = only(graph, "array", "operator new[]");
    CHECK(edge.kind == EdgeKind::DirectCall);
    CHECK(allocates(edge));
  }

  SECTION("the nothrow and aligned forms allocate too") {
    CHECK(allocates(only(graph, "quiet", "operator new")));
    CHECK(allocates(only(graph, "wide", "operator new")));
  }

  SECTION("the allocating functions are callers of operator new") {
    std::vector<std::string> callers;
    for (const CallGraphEdge &edge : graph.callersOf("operator new"))
      callers.push_back(edge.callerName);
    std::sort(callers.begin(), callers.end());
    CHECK(callers == std::vector<std::string>{"plain", "quiet", "wide"});
  }
}

TEST_CASE("a placement new calls the placement overload and does not "
          "allocate",
          "[callgraph][builder][allocation]") {
  CallGraph graph;
  build(graph, R"(
    struct Arena {};
    void *operator new(size_t, Arena &);
    Widget *placed(void *buf) { return new (buf) Widget; }
    int *placedArray(void *buf) { return new (buf) int[4]; }
    Widget *fromArena(Arena &a) { return new (a) Widget; }
  )");

  CallGraphEdge placed = only(graph, "placed", "operator new");
  CHECK(placed.kind == EdgeKind::DirectCall);
  CHECK(isPlacementNewUsr(placed.calleeUsr));
  CHECK_FALSE(allocates(placed));

  CallGraphEdge placedArray = only(graph, "placedArray", "operator new[]");
  CHECK(isPlacementNewUsr(placedArray.calleeUsr));
  CHECK_FALSE(allocates(placedArray));

  // A program's own placement overload is ordinary code: it allocates
  // when its body does, not by its name.
  CallGraphEdge arena = only(graph, "fromArena", "operator new");
  CHECK(isPlacementNewUsr(arena.calleeUsr));
  CHECK_FALSE(allocates(arena));
  CHECK(arena.calleeUsr != placed.calleeUsr);
}

TEST_CASE("a class's own allocation function is the callee",
          "[callgraph][builder][allocation]") {
  CallGraph graph;
  build(graph, R"(
    void *malloc(size_t);
    void free(void *);
    struct Pooled {
      static void *operator new(size_t n) { return malloc(n); }
      static void operator delete(void *p) { free(p); }
    };
    Pooled *make() { return new Pooled; }
    void drop(Pooled *p) { delete p; }
  )");

  CHECK(only(graph, "make", "Pooled::operator new").kind ==
        EdgeKind::DirectCall);
  CHECK(calleesNamed(graph, "make", "operator new").empty());
  // Its allocation shows through what it calls.
  CHECK(only(graph, "Pooled::operator new", "malloc").kind ==
        EdgeKind::DirectCall);
  CHECK(only(graph, "drop", "Pooled::operator delete").confidence ==
        Confidence::Proven);
}

TEST_CASE("a delete-expression is a call of its deallocation function",
          "[callgraph][builder][allocation]") {
  CallGraph graph;
  build(graph, R"(
    struct Poly { virtual ~Poly(); };
    void drop(Widget *w) { delete w; }
    void dropArray(int *p) { delete[] p; }
    void dropPoly(Poly *p) { delete p; }
  )");

  CallGraphEdge drop = only(graph, "drop", "operator delete");
  CHECK(drop.kind == EdgeKind::DirectCall);
  CHECK(drop.confidence == Confidence::Proven);
  CHECK_FALSE(allocates(drop));

  CHECK(only(graph, "dropArray", "operator delete[]").confidence ==
        Confidence::Proven);

  // Through a virtual destructor the deallocation function is the dynamic
  // type's: the one resolved here is likely, not certain.
  CHECK(only(graph, "dropPoly", "operator delete").confidence ==
        Confidence::Plausible);
}

TEST_CASE("only a plain delete through a virtual destructor is uncertain",
          "[callgraph][builder][allocation]") {
  // The forms that settle the deallocation function at compile time even
  // when the destructor is virtual.
  CallGraph graph;
  build(graph, R"(
    namespace std {
    struct destroying_delete_t { explicit destroying_delete_t() = default; };
    }
    struct Poly { virtual ~Poly(); };
    struct Dest {
      virtual ~Dest();
      static void operator delete(Dest *, std::destroying_delete_t);
    };
    void dropPolyArray(Poly *p) { delete[] p; }
    void dropPolyGlobal(Poly *p) { ::delete p; }
    void dropDest(Dest *d) { delete d; }
  )",
        "-std=c++20");

  CHECK(only(graph, "dropPolyArray", "operator delete[]").confidence ==
        Confidence::Proven);
  CHECK(only(graph, "dropPolyGlobal", "operator delete").confidence ==
        Confidence::Proven);
  CHECK(only(graph, "dropDest", "Dest::operator delete").confidence ==
        Confidence::Proven);
}

TEST_CASE("a program's own placement overloads are not the allocation "
          "function",
          "[callgraph][builder][allocation]") {
  // Two shapes whose identity is not the plain `c:@F@operator new#...`: a
  // parameter type with internal linkage, and a function template.
  CallGraph graph;
  build(graph, R"(
    namespace { struct Arena { char buf[64]; }; Arena arena; }
    void *operator new(size_t, Arena &a) noexcept { return a.buf; }
    struct Bump { char buf[64]; };
    template <class A> void *operator new(size_t, A *a) noexcept;
    int *viaArena() { return new (arena) int(1); }
    int *viaBump(Bump *b) { return new (b) int(2); }
    int *viaHeap() { return new int(3); }
  )");

  CallGraphEdge arena = only(graph, "viaArena", "operator new");
  CallGraphEdge bump = only(graph, "viaBump", "operator new");
  CallGraphEdge heap = only(graph, "viaHeap", "operator new");
  CHECK(arena.calleeUsr != heap.calleeUsr);
  CHECK(bump.calleeUsr != heap.calleeUsr);
  CHECK_FALSE(allocates(arena));
  CHECK_FALSE(allocates(bump));
  CHECK(allocates(heap));
}

TEST_CASE("allocation edges belong to the function that evaluates the "
          "expression",
          "[callgraph][builder][allocation]") {
  CallGraph graph;
  build(graph, R"(
    int *viaLambda() {
      auto make = [] { return new int(1); };
      return make();
    }
    template <class T> T *makeDependent() { return new T; }
    template <class T> int *makeFixed(T) { return new int(2); }
    Widget *instantiate() { return makeDependent<Widget>(); }
  )");

  // Stored edges into `operator new`, by the caller's identity: a
  // template's definition has edges but no node of its own, so it is not
  // found by name.
  auto newCalledFrom = [&graph](llvm::StringRef callerUsrPart) {
    unsigned count = 0;
    graph.forEachStoredEdge([&](const CallGraph::EdgeRef &edge) {
      if (graph.displayNameOf(edge.callee) == "operator new" &&
          llvm::StringRef(graph.interner().resolve(edge.caller))
              .contains(callerUsrPart))
        ++count;
    });
    return count;
  };

  SECTION("a lambda's new is the lambda's") {
    CHECK(calleesNamed(graph, "viaLambda", "operator new").empty());
    CHECK(newCalledFrom("vycor-lambda:") == 1);
  }

  SECTION("a dependent new is resolved in the instantiation") {
    // With `T` unknown the template's definition calls nothing yet; the
    // body of makeDependent<Widget> calls the allocation function.
    CHECK(newCalledFrom("makeDependent") == 1);
  }

  SECTION("a non-dependent new in a template is resolved in its "
          "definition") {
    CHECK(newCalledFrom("makeFixed") == 1);
  }
}

TEST_CASE("operator new keeps its node when a header redeclares it",
          "[callgraph][builder][allocation]") {
  // <new> redeclares the functions the compiler declares implicitly; the
  // callee then resolves to the redeclaration. The node, and with it the
  // display name the effect table matches, must not depend on that.
  CallGraph graph;
  build(graph, R"(
    void *operator new(size_t);
    void operator delete(void *) noexcept;
    Widget *plain() { return new Widget; }
    void drop(Widget *w) { delete w; }
    void *raw() { return ::operator new(8); }
  )");

  CallGraphEdge viaExpr = only(graph, "plain", "operator new");
  CallGraphEdge viaCall = only(graph, "raw", "operator new");
  CHECK(viaExpr.calleeUsr == viaCall.calleeUsr);
  CHECK(allocates(viaExpr));
  CHECK(only(graph, "drop", "operator delete").kind == EdgeKind::DirectCall);

  CallGraph bare;
  build(bare, "Widget *plain() { return new Widget; }");
  const CallGraphNode *withHeader = graph.findNode(viaExpr.calleeUsr);
  const CallGraphNode *without =
      bare.findNode(only(bare, "plain", "operator new").calleeUsr);
  REQUIRE(withHeader != nullptr);
  REQUIRE(without != nullptr);
  CHECK(withHeader->usr == without->usr);
  CHECK(withHeader->file == without->file);
  CHECK(withHeader->line == without->line);
}

TEST_CASE("placement operator new is told apart by its parameters",
          "[metrics][effects][allocation]") {
  // The replaceable allocation functions: size, then only alignment and
  // nothrow.
  CHECK_FALSE(isPlacementNewUsr("c:@F@operator new#l#"));
  CHECK_FALSE(isPlacementNewUsr("c:@F@operator new[]#l#"));
  CHECK_FALSE(isPlacementNewUsr("c:@F@operator new#l#$@N@std@E@align_val_t#"));
  CHECK_FALSE(isPlacementNewUsr("c:@F@operator new#l#&1$@N@std@S@nothrow_t#"));
  CHECK_FALSE(isPlacementNewUsr(
      "c:@F@operator new[]#l#$@N@std@E@align_val_t#&1$@N@std@S@nothrow_t#"));

  CHECK(isPlacementNewUsr("c:@F@operator new#l#*v#"));
  CHECK(isPlacementNewUsr("c:@F@operator new[]#l#*v#"));
  CHECK(isPlacementNewUsr("c:@F@operator new#l#&$@S@Arena#"));
  CHECK(isPlacementNewUsr(
      "c:@F@operator new#l#$@N@std@E@align_val_t#&$@S@Arena#"));

  // With a file prefix (a parameter type of internal linkage), and as an
  // instantiation of a function template, which is never replaceable.
  CHECK(isPlacementNewUsr("c:a.cpp@F@operator new#l#&$@aN@S@Arena#"));
  CHECK(isPlacementNewUsr("c:@F@operator new<#$@S@Bump>#l#*S0_#"));

  // Not a global operator new at all.
  CHECK_FALSE(isPlacementNewUsr("c:a.cpp@aN@S@Pooled@F@operator new#l#S"));
  CHECK_FALSE(isPlacementNewUsr("c:"));
  CHECK_FALSE(isPlacementNewUsr("c:@S@Pooled@F@operator new#l#*v#S"));
  CHECK_FALSE(isPlacementNewUsr("c:@F@operator delete#*v#"));
  CHECK_FALSE(isPlacementNewUsr("c:@F@operator newish#l#*v#"));
  CHECK_FALSE(isPlacementNewUsr("operator new"));
  CHECK_FALSE(isPlacementNewUsr(""));

  EffectTable table = EffectTable::builtin();
  const uint8_t allocation = effectBit(EffectKind::Allocation);
  CHECK(table.effectsOf("operator new", "c:@F@operator new#l#") == allocation);
  CHECK(table.effectsOf("operator new", "c:@F@operator new#l#*v#") == 0);
  // A name-only identity (a hand-built graph) is judged by name.
  CHECK(table.effectsOf("operator new", "operator new") == allocation);
  CHECK(table.effectsOf("malloc", "c:@F@malloc") == allocation);
}
