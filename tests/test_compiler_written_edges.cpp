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

// test_compiler_written_edges.cpp — The calls no source line spells: the
// bodies of template instantiations (calls that depend on a template
// parameter, resolved per instantiation), the code the compiler writes
// for implicit members and default member initializers, and the
// destructors that run at a scope's end, after a full-expression, after a
// destructor's body and in a delete-expression.

#include "vycor/callgraph/CallGraph.h"
#include "vycor/callgraph/CallGraphBuilder.h"
#include "vycor/callgraph/ControlFlowIndex.h"
#include "vycor/callgraph/ControlFlowOracle.h"
#include "vycor/query/Tools.h"

#include <catch2/catch_test_macros.hpp>
#include <clang/Tooling/Tooling.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/JSON.h>

#include <string>
#include <vector>

using namespace vycor;

namespace {

// No standard header: a unique_ptr and make_unique of the test's own, so
// the edges do not depend on the host's standard library.
const char *const kPrelude = R"(
using size_t = decltype(sizeof 0);
namespace std {
template <class T> struct default_delete {
  void operator()(T *p) const { delete p; }
};
template <class T, class D = default_delete<T>> struct unique_ptr {
  T *p = nullptr;
  explicit unique_ptr(T *q) : p(q) {}
  ~unique_ptr() { if (p) D()(p); }
};
template <class T> unique_ptr<T> make_unique() { return unique_ptr<T>(new T()); }
}
int compute();
)";

void build(CallGraph &graph, const std::string &code,
           const char *file = "test.cpp") {
  CallGraphBuilderFactory factory(graph);
  REQUIRE(clang::tooling::runToolOnCodeWithArgs(
      factory.create(), std::string(kPrelude) + code, {"-std=c++17"}, file));
}

// The edges from `caller` (a display name or a usr) to functions
// displayed as `callee`.
std::vector<CallGraphEdge> calleesNamed(const CallGraph &graph,
                                        const std::string &caller,
                                        const std::string &callee) {
  std::vector<CallGraphEdge> out;
  for (const CallGraphEdge &edge : graph.calleesOf(caller))
    if (edge.calleeName == callee)
      out.push_back(edge);
  return out;
}

CallGraphEdge only(const CallGraph &graph, const std::string &caller,
                   const std::string &callee) {
  std::vector<CallGraphEdge> edges = calleesNamed(graph, caller, callee);
  INFO(caller << " -> " << callee);
  REQUIRE(edges.size() == 1);
  return edges.front();
}

} // namespace

TEST_CASE("an instantiation holds its own calls, dependent ones resolved",
          "[callgraph][builder][templates]") {
  CallGraph graph;
  build(graph, R"(
struct Payload { static int *make() { return new int(7); } };
template <class T> int *inner() { return T::make(); }
template <class T> int *outer() { return inner<T>(); }
int *start() { return outer<Payload>(); }

template <class T> int twice(T) { return compute(); }
template int twice<long>(long);
)");

  // start -> outer<Payload> -> inner<Payload> -> Payload::make -> new: each
  // hop under the instantiation's own identity.
  const CallGraphEdge toOuter = only(graph, "start", "outer");
  const CallGraphEdge toInner = only(graph, toOuter.calleeUsr, "inner");
  CHECK(toInner.kind == EdgeKind::DirectCall);
  const CallGraphEdge toMake =
      only(graph, toInner.calleeUsr, "Payload::make");
  CHECK(toMake.confidence == Confidence::Proven);
  CHECK(only(graph, toMake.calleeUsr, "operator new").kind ==
        EdgeKind::DirectCall);

  // The dependent call is no edge of the template's definition.
  bool patternCallsMake = false;
  graph.forEachStoredEdge([&](const CallGraph::EdgeRef &edge) {
    if (graph.displayNameOf(edge.callee) == "Payload::make" &&
        llvm::StringRef(graph.interner().resolve(edge.caller))
            .contains("@FT@"))
      patternCallsMake = true;
  });
  CHECK_FALSE(patternCallsMake);

  // A body read is marked as such; the instantiation has a node.
  const CallGraphNode *outer = graph.findNode(toOuter.calleeUsr);
  REQUIRE(outer != nullptr);
  CHECK(outer->hasBody);

  SECTION("an explicit instantiation definition is read uncalled") {
    std::vector<CallGraphEdge> fromTwice;
    for (const CallGraphNode *node : graph.allNodes())
      if (node->qualifiedName == "twice")
        for (const CallGraphEdge &edge : graph.calleesOf(node->usr))
          fromTwice.push_back(edge);
    REQUIRE(fromTwice.size() == 1);
    CHECK(fromTwice[0].calleeName == "compute");
  }

  SECTION("get_callees on an instantiation answers from its own edges") {
    ControlFlowIndex cfIndex;
    ControlFlowOracle oracle(graph, cfIndex);
    std::vector<std::string> eps;
    ToolContext ctx{graph, oracle, cfIndex, eps};
    ToolHandler handler;
    for (const ToolEntry &tool : getRegisteredTools())
      if (tool.name == "get_callees")
        handler = tool.handler;
    REQUIRE(handler);
    llvm::json::Object args{{"usr", toOuter.calleeUsr}};
    llvm::json::Value result = handler(args, ctx);
    const llvm::json::Object *out = result.getAsObject();
    REQUIRE(out != nullptr);
    const llvm::json::Array *callees = out->getArray("callees");
    REQUIRE(callees != nullptr);
    REQUIRE(callees->size() == 1);
    CHECK((*callees)[0].getAsObject()->getString("calleeName").value_or("") ==
          "inner");
  }
}

TEST_CASE("the compiler's constructors and default member initializers "
          "are read",
          "[callgraph][builder][templates]") {
  CallGraph graph;
  build(graph, R"(
struct Logger { Logger(); };
struct Holder { Logger log; };
void makeHolder() { Holder h; }

struct Seeded { int *p = new int(3); Seeded(); };
Seeded::Seeded() {}
struct Overridden { int *p = new int(3); Overridden(); };
Overridden::Overridden() : p(nullptr) {}
)");

  // Holder's implicit constructor default-constructs its member.
  CHECK(only(graph, "makeHolder", "Holder::Holder").kind ==
        EdgeKind::ConstructorCall);
  CHECK(only(graph, "Holder::Holder", "Logger::Logger").kind ==
        EdgeKind::ConstructorCall);

  // A default member initializer runs in a constructor that leaves the
  // member out of its initializer list, and only there.
  CHECK(calleesNamed(graph, "Seeded::Seeded", "operator new").size() == 1);
  CHECK(calleesNamed(graph, "Overridden::Overridden", "operator new").empty());
}

TEST_CASE("a default member initializer runs where its object is "
          "initialized",
          "[callgraph][builder]") {
  CallGraph graph;
  build(graph, R"(
struct Dmi { int v = compute(); Dmi(); Dmi(int); };
Dmi::Dmi() {}
Dmi::Dmi(int x) : v(x) {}
struct Agg { int v = compute(); };
void useAgg() { Agg a; (void)a; }
void useAgg2() { Agg a{}; (void)a; }
)");
  CHECK(calleesNamed(graph, "Dmi::Dmi", "compute").size() == 1);
  // The overload that initializes the member itself does not call it.
  CHECK(calleesNamed(graph, "c:@S@Dmi@F@Dmi#I#", "compute").empty());
  CHECK(calleesNamed(graph, "c:@S@Dmi@F@Dmi#", "compute").size() == 1);
  // Default-initialized: the aggregate's implicit constructor runs it.
  only(graph, "useAgg", "Agg::Agg");
  CHECK(only(graph, "Agg::Agg", "compute").kind == EdgeKind::DirectCall);
  // Aggregate-initialized: the initializer runs in the caller.
  CHECK(only(graph, "useAgg2", "compute").kind == EdgeKind::DirectCall);
}

TEST_CASE("implicit destructor calls are edges",
          "[callgraph][builder][destructors]") {
  CallGraph graph;
  build(graph, R"(
struct Res { ~Res(); };
Res make();
struct Base { virtual ~Base(); };
struct Derived : Base { Res r; ~Derived(); };
Derived::~Derived() {}
struct Thing { ~Thing(); };
struct Owner { std::unique_ptr<Thing> thing; };
struct Plain { int x; };

void scope() {
  Res r;
}
void temporary() { make(); }
void del(Res *p) { delete p; }
void delBase(Base *b) { delete b; }
void own(Owner *o) { delete o; }
void trivial() { Plain p; Plain q = Plain(); (void)p; (void)q; }
)");

  SECTION("a local at its scope's end, sited at its declaration") {
    const CallGraphEdge edge = only(graph, "scope", "Res::~Res");
    CHECK(edge.kind == EdgeKind::DestructorCall);
    CHECK(edge.confidence == Confidence::Proven);
    // Line 12 of the snippet, after the prelude's lines.
    const unsigned line =
        12 + static_cast<unsigned>(llvm::StringRef(kPrelude).count('\n'));
    CHECK(llvm::StringRef(edge.callSite)
              .ends_with(":" + std::to_string(line) + ":7"));
  }

  SECTION("a temporary at the end of its full-expression") {
    CHECK(only(graph, "temporary", "Res::~Res").kind ==
          EdgeKind::DestructorCall);
  }

  SECTION("a destructor destroys its members and bases") {
    CHECK(only(graph, "Derived::~Derived", "Res::~Res").kind ==
          EdgeKind::DestructorCall);
    CHECK(only(graph, "Derived::~Derived", "Base::~Base").kind ==
          EdgeKind::DestructorCall);
  }

  SECTION("a delete-expression runs the destructor") {
    const CallGraphEdge exact = only(graph, "del", "Res::~Res");
    CHECK(exact.kind == EdgeKind::DestructorCall);
    CHECK(exact.confidence == Confidence::Proven);
    // Through a virtual destructor: whichever the dynamic type has.
    const CallGraphEdge virt = only(graph, "delBase", "Base::~Base");
    CHECK(virt.kind == EdgeKind::VirtualDispatch);
    CHECK(virt.confidence == Confidence::Plausible);
  }

  SECTION("a unique_ptr member's destructor reaches the pointee's") {
    // Owner's implicit destructor -> unique_ptr<Thing>::~unique_ptr ->
    // default_delete<Thing>::operator() -> Thing::~Thing.
    only(graph, "own", "Owner::~Owner");
    const CallGraphEdge toPtr =
        only(graph, "Owner::~Owner", "std::unique_ptr<Thing>::~unique_ptr");
    const CallGraphEdge toDeleter = only(
        graph, toPtr.calleeUsr, "std::default_delete<Thing>::operator()");
    CHECK(only(graph, toDeleter.calleeUsr, "Thing::~Thing").kind ==
          EdgeKind::DestructorCall);
  }

  SECTION("a trivial destructor is no call") {
    CHECK(graph.calleesOf("trivial").empty());
  }
}

TEST_CASE("a qualified call to a virtual method is a direct call",
          "[callgraph][builder][virtual]") {
  CallGraph graph;
  build(graph, R"(
struct V { virtual void f(); };
struct W : V { void f() override; };
void W::f() { V::f(); }
void viaPointer(V *v) { v->f(); }
)");
  // `V::f()` names the function: no dispatch, no other override.
  const std::vector<CallGraphEdge> edges = graph.calleesOf("W::f");
  REQUIRE(edges.size() == 1);
  CHECK(edges[0].calleeName == "V::f");
  CHECK(edges[0].kind == EdgeKind::DirectCall);
  CHECK(edges[0].confidence == Confidence::Proven);
  // An unqualified call still dispatches.
  CHECK(only(graph, "viaPointer", "V::f").kind == EdgeKind::VirtualDispatch);
}

TEST_CASE("make_unique of same-named internal classes keeps them apart",
          "[callgraph][builder][templates]") {
  CallGraph graph;
  build(graph, R"(
namespace { struct Widget { Widget() { compute(); } }; }
void first() { std::make_unique<Widget>(); }
)",
        "a.cpp");
  build(graph, R"(
namespace { struct Widget { Widget() {} }; }
void second() { std::make_unique<Widget>(); }
)",
        "b.cpp");

  for (const CallGraphNode *node : graph.allNodes())
    CHECK_FALSE(llvm::StringRef(node->usr).starts_with("vycor-synth:"));

  // Each make_unique<Widget> constructs its own TU's Widget.
  auto widgetCtorUnder = [&graph](const std::string &caller) {
    const CallGraphEdge mk = only(graph, caller, "std::make_unique");
    return only(graph, mk.calleeUsr, "(anonymous namespace)::Widget::Widget")
        .calleeUsr;
  };
  const std::string a = widgetCtorUnder("first");
  const std::string b = widgetCtorUnder("second");
  CHECK(a != b);
  CHECK(calleesNamed(graph, a, "compute").size() == 1);
  CHECK(graph.calleesOf(b).empty());
}
