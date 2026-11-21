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

// test_static_init_facts.cpp — the facts the static-init-cost metric
// reduces: the effect table (vycor/callgraph/Effects.h) and the
// static-initialization roots the bake collects
// (vycor/callgraph/StaticInit.h).

#include "vycor/callgraph/CallGraph.h"
#include "vycor/callgraph/ControlFlowIndex.h"
#include "vycor/callgraph/Effects.h"
#include "vycor/callgraph/StaticInit.h"

#include <catch2/catch_test_macros.hpp>
#include <clang/Tooling/CompilationDatabase.h>

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"

#include <algorithm>
#include <fstream>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

using namespace vycor;

namespace {

struct Baked {
  BakedIndexes ix;
  std::vector<std::string> paths;
  Baked() = default;
  Baked(Baked &&) = default;
  Baked &operator=(Baked &&) = default;
  ~Baked() {
    for (const std::string &path : paths)
      llvm::sys::fs::remove(path);
  }
};

std::string writeTemp(const std::string &code, const char *suffix) {
  llvm::SmallString<128> tmp;
  int fd = -1;
  auto ec = llvm::sys::fs::createTemporaryFile("vycor_static_init", suffix, fd,
                                               tmp);
  REQUIRE_FALSE(ec);
  ::close(fd);
  std::ofstream out(std::string(tmp.str()));
  out << code;
  return std::string(tmp.str());
}

Baked bake(const std::vector<std::string> &sources) {
  Baked b;
  for (const std::string &code : sources)
    b.paths.push_back(writeTemp(code, "cpp"));
  clang::tooling::FixedCompilationDatabase compDb(".", {"-std=c++17"});
  b.ix = bakeIndexes(compDb, b.paths, {}, /*threadCount=*/1);
  return b;
}

const StaticInitRoot *rootNamed(const StaticInitFacts &facts,
                                llvm::StringRef name) {
  for (const StaticInitRoot &root : facts.roots)
    if (root.name == name)
      return &root;
  return nullptr;
}

std::vector<std::string> calleeNames(const StaticInitRoot &root) {
  std::vector<std::string> out;
  for (const StaticInitCallee &callee : root.callees)
    out.push_back(callee.display);
  std::sort(out.begin(), out.end());
  return out;
}

} // namespace

// ============================================================================
// Effect table
// ============================================================================

TEST_CASE("effect lookup names drop template arguments and implementation "
          "namespaces",
          "[metrics][effects]") {
  CHECK(effectLookupName("malloc") == "malloc");
  CHECK(effectLookupName("std::make_unique<Foo>") == "std::make_unique");
  CHECK(effectLookupName("std::__1::lock_guard<std::__1::mutex>::lock_guard") ==
        "std::lock_guard::lock_guard");
  CHECK(effectLookupName("std::__cxx11::basic_string<char>::basic_string") ==
        "std::basic_string::basic_string");
  CHECK(effectLookupName("std::vector<std::pair<int, int>>::push_back") ==
        "std::vector::push_back");

  SECTION("an operator spelling keeps its angle brackets") {
    CHECK(effectLookupName("operator new") == "operator new");
    CHECK(effectLookupName("operator new[]") == "operator new[]");
    CHECK(effectLookupName("operator<<") == "operator<<");
    CHECK(effectLookupName("ns::Box<int>::operator<") == "ns::Box::operator<");
    CHECK(effectLookupName("ns::Box<int>::operator->") ==
          "ns::Box::operator->");
  }

  SECTION("a name that only contains the word is not an operator") {
    CHECK(effectLookupName("ns::myoperator<int>") == "ns::myoperator");
    CHECK(effectLookupName("ns::operator_table<int>::get") ==
          "ns::operator_table::get");
  }

  SECTION("a reserved name is kept when it is the function's own") {
    CHECK(effectLookupName("__libc_malloc") == "__libc_malloc");
    CHECK(effectLookupName("std::__detail::__helper") == "std::__helper");
  }
}

TEST_CASE("the effect table answers by normalized display name",
          "[metrics][effects]") {
  EffectTable table = EffectTable::builtin();
  CHECK_FALSE(table.empty());
  CHECK(table.effectsOf("malloc") == effectBit(EffectKind::Allocation));
  CHECK(table.effectsOf("operator new[]") ==
        effectBit(EffectKind::Allocation));
  CHECK(table.effectsOf("std::make_shared<Widget>") ==
        effectBit(EffectKind::Allocation));
  CHECK(table.effectsOf("std::__1::mutex::lock") ==
        effectBit(EffectKind::Lock));
  CHECK(table.effectsOf("std::lock_guard<std::mutex>::lock_guard") ==
        effectBit(EffectKind::Lock));
  CHECK(table.effectsOf("puts") == effectBit(EffectKind::Io));
  CHECK(table.effectsOf("std::basic_ofstream<char>::basic_ofstream") ==
        effectBit(EffectKind::Io));

  // An effect that depends on the arguments is not in the table.
  CHECK(table.effectsOf("std::vector<int>::push_back") == 0);
  CHECK(table.effectsOf("helper") == 0);

  SECTION("additions are normalized like lookups and may carry several "
          "kinds") {
    table.add(EffectKind::Io, "myorg::log<T>");
    table.add(EffectKind::Lock, "myorg::log");
    CHECK(table.effectsOf("myorg::log<int>") ==
          (effectBit(EffectKind::Io) | effectBit(EffectKind::Lock)));
    table.add(EffectKind::Allocation, "");
    CHECK(table.effectsOf("") == 0);
  }

  SECTION("names are listed sorted, per kind") {
    EffectTable own;
    own.add(EffectKind::Lock, "zeta::lock");
    own.add(EffectKind::Lock, "alpha::lock");
    own.add(EffectKind::Io, "alpha::write");
    CHECK(own.names(EffectKind::Lock) ==
          std::vector<std::string>{"alpha::lock", "zeta::lock"});
    CHECK(own.names(EffectKind::Io) ==
          std::vector<std::string>{"alpha::write"});
    CHECK(own.names(EffectKind::Allocation).empty());
  }

  SECTION("kind names round-trip") {
    for (EffectKind kind : kEffectKinds)
      CHECK(effectKindFromName(effectKindName(kind)) == kind);
    CHECK_FALSE(effectKindFromName("network").has_value());
  }
}

// ============================================================================
// Static-initialization roots
// ============================================================================

TEST_CASE("the bake collects dynamic initializers and constructor functions",
          "[metrics][static-init]") {
  Baked b = bake({R"cpp(
int compute();
struct Widget { Widget(); };
struct Holder { Widget inner; };

int dynamic = compute();
Widget widget;
Holder holder;

constexpr int kConstant = 4;
int constant = kConstant;
const int literal = 5;
int zero;

thread_local int perThread = compute();
int lazily() { static int local = compute(); return local; }

__attribute__((constructor)) void boot() { compute(); }

struct Registry { static int slot; };
int Registry::slot = compute();
)cpp"});
  const StaticInitFacts &facts = b.ix.staticInits;
  REQUIRE(facts.collected);

  SECTION("a variable whose initializer is not a constant expression") {
    const StaticInitRoot *root = rootNamed(facts, "dynamic");
    REQUIRE(root != nullptr);
    CHECK(root->kind == StaticInitKind::Variable);
    CHECK(root->file == b.paths[0]);
    CHECK(root->line == 6);
    CHECK(root->tus == std::vector<std::string>{b.paths[0]});
    CHECK(calleeNames(*root) == std::vector<std::string>{"compute"});
  }

  SECTION("a constructor call is a callee") {
    const StaticInitRoot *root = rootNamed(facts, "widget");
    REQUIRE(root != nullptr);
    CHECK(calleeNames(*root) == std::vector<std::string>{"Widget::Widget"});
  }

  SECTION("a compiler-generated constructor is looked through") {
    const StaticInitRoot *root = rootNamed(facts, "holder");
    REQUIRE(root != nullptr);
    CHECK(calleeNames(*root) == std::vector<std::string>{"Widget::Widget"});
  }

  SECTION("a static data member defined out of line") {
    const StaticInitRoot *root = rootNamed(facts, "Registry::slot");
    REQUIRE(root != nullptr);
    CHECK(calleeNames(*root) == std::vector<std::string>{"compute"});
  }

  SECTION("a constructor function has no recorded callees: its calls are "
          "its node's edges") {
    const StaticInitRoot *root = rootNamed(facts, "boot");
    REQUIRE(root != nullptr);
    CHECK(root->kind == StaticInitKind::ConstructorFunction);
    CHECK(root->callees.empty());
    CHECK(b.ix.graph.findNode(root->usr) != nullptr);
  }

  SECTION("constant initialization, thread-locals and function-local "
          "statics are not roots") {
    CHECK(rootNamed(facts, "constant") == nullptr);
    CHECK(rootNamed(facts, "kConstant") == nullptr);
    CHECK(rootNamed(facts, "literal") == nullptr);
    CHECK(rootNamed(facts, "zero") == nullptr);
    CHECK(rootNamed(facts, "perThread") == nullptr);
    CHECK(rootNamed(facts, "local") == nullptr);
    CHECK(facts.roots.size() == 5);
  }

  SECTION("roots are in canonical order: file, line, name") {
    std::vector<unsigned> lines;
    for (const StaticInitRoot &root : facts.roots)
      lines.push_back(root.line);
    CHECK(std::is_sorted(lines.begin(), lines.end()));
  }
}

TEST_CASE("an initializer a macro declares is located where the macro is "
          "used",
          "[metrics][static-init]") {
  Baked b = bake({R"cpp(
int compute();
#define COUNTER(name) int name = compute()
#define PASTED(stem) int stem##Slot = compute()

COUNTER(hits);
PASTED(late);
)cpp"});
  const StaticInitFacts &facts = b.ix.staticInits;
  REQUIRE(facts.roots.size() == 2);

  // A name passed as a macro argument, and one pasted together in the
  // macro body (written in no file at all): both are at the use.
  const StaticInitRoot *hits = rootNamed(facts, "hits");
  REQUIRE(hits);
  CHECK(hits->file == b.paths[0]);
  CHECK(hits->line == 6);

  const StaticInitRoot *late = rootNamed(facts, "lateSlot");
  REQUIRE(late);
  CHECK(late->file == b.paths[0]);
  CHECK(late->line == 7);
}

TEST_CASE("a lambda in an initializer is recorded under the graph's own "
          "identity for it",
          "[metrics][static-init]") {
  Baked b = bake({R"cpp(
int compute();
int viaLambda = [] { return compute(); }();
)cpp"});
  const StaticInitRoot *root = rootNamed(b.ix.staticInits, "viaLambda");
  REQUIRE(root != nullptr);
  REQUIRE(root->callees.size() == 1);
  // The collector re-derives the builder's spelling of a namespace-scope
  // lambda; if the two drift apart the lambda's body is silently lost.
  INFO(root->callees[0].usr);
  CHECK(b.ix.graph.findNode(root->callees[0].usr) != nullptr);
  CHECK(llvm::StringRef(root->callees[0].display).starts_with("lambda#"));
}

TEST_CASE("a lambda in a default argument is the graph's under the "
          "function declaring the parameter",
          "[metrics][static-init]") {
  Baked b = bake({R"cpp(
int compute();
int withDefault(int x = [] { return compute(); }()) { return x; }
int viaDefault = withDefault();
)cpp"});
  const StaticInitRoot *root = rootNamed(b.ix.staticInits, "viaDefault");
  REQUIRE(root != nullptr);
  // The function, and the lambda its default argument evaluates in the
  // initializer. Every callee is a node: a lambda named as if it were at
  // namespace scope would be a phantom leaf, counted and never entered.
  REQUIRE(root->callees.size() == 2);
  for (const StaticInitCallee &callee : root->callees) {
    INFO(callee.usr);
    CHECK(b.ix.graph.findNode(callee.usr) != nullptr);
  }
}

TEST_CASE("a braced initializer is walked in its semantic form",
          "[metrics][static-init]") {
  Baked b = bake({R"cpp(
int compute();
struct Name { Name(const char *); };
struct Row { Name name; int weight; };
struct Defaults { int a = compute(); int b = 2; };
struct Counted { Counted(); };

Name names[] = {"a", "b"};
Row rows[] = {{"a", 1}, {"b", 2}};
Row oneRow = {"z", compute()};
Defaults braced{};
Defaults assigned = {};
Counted padded[3] = {};
)cpp"});
  const StaticInitFacts &facts = b.ix.staticInits;
  auto callees = [&](const char *name) {
    const StaticInitRoot *root = rootNamed(facts, name);
    REQUIRE(root != nullptr);
    return calleeNames(*root);
  };

  // The converting constructors exist only in the semantic form of the
  // list: `{"a", "b"}` as written holds two string literals.
  CHECK(callees("names") == std::vector<std::string>{"Name::Name"});
  CHECK(callees("rows") == std::vector<std::string>{"Name::Name"});
  CHECK(callees("oneRow") ==
        std::vector<std::string>{"Name::Name", "compute"});
  // A default member initializer the list leaves to the class.
  CHECK(callees("braced") == std::vector<std::string>{"compute"});
  CHECK(callees("assigned") == std::vector<std::string>{"compute"});
  // Elements the list does not name, built by the array filler.
  CHECK(callees("padded") == std::vector<std::string>{"Counted::Counted"});
}

TEST_CASE("constant initialization is decided on the variable and not on "
          "its initializer alone",
          "[metrics][static-init]") {
  Baked b = bake({R"cpp(
int compute();
struct Owner {
  constexpr Owner() : p(nullptr) {}
  constexpr Owner(int *q) : p(q) {}
  ~Owner();
  int *p;
};
struct Literal { constexpr Literal(int x) : v(x) {} int v; };
struct Runtime { Runtime(); ~Runtime(); };

Owner empty;
Owner null(nullptr);
Literal literal(3);
Runtime runtime;
Owner late(compute() ? nullptr : nullptr);
)cpp"});
  const StaticInitFacts &facts = b.ix.staticInits;
  // A constexpr constructor and a non-trivial destructor: constant
  // initialized (nothing runs before main), though its initializer is
  // not a constant expression taken by itself. The shape of a global
  // std::unique_ptr.
  CHECK(rootNamed(facts, "empty") == nullptr);
  CHECK(rootNamed(facts, "null") == nullptr);
  CHECK(rootNamed(facts, "literal") == nullptr);
  CHECK(rootNamed(facts, "runtime") != nullptr);
  CHECK(rootNamed(facts, "late") != nullptr);
  CHECK(facts.roots.size() == 2);
}

TEST_CASE("trivial default construction is not a root",
          "[metrics][static-init]") {
  Baked b = bake({R"cpp(
int compute();
struct Pod { int a; void *p; };
struct Runtime { Runtime(); };
struct Holds { Runtime r; };
struct Seeded { int a = compute(); };

static Pod pod;
static Pod pods[4];
static struct { Pod inner; int n; } unnamed[2];
static Holds holds;
static Holds heldArray[2];
static Seeded seeded;
)cpp"});
  const StaticInitFacts &facts = b.ix.staticInits;
  // Sema does not call these constant-initialized (the members are left
  // indeterminate), and nothing runs: the shape of `static stack_t old;`
  // and `static struct sigaction prev[N];`.
  CHECK(rootNamed(facts, "pod") == nullptr);
  CHECK(rootNamed(facts, "pods") == nullptr);
  CHECK(rootNamed(facts, "unnamed") == nullptr);
  // An implicit default constructor that runs a member's is not trivial.
  REQUIRE(rootNamed(facts, "holds") != nullptr);
  CHECK(calleeNames(*rootNamed(facts, "holds")) ==
        std::vector<std::string>{"Runtime::Runtime"});
  CHECK(rootNamed(facts, "heldArray") != nullptr);
  REQUIRE(rootNamed(facts, "seeded") != nullptr);
  CHECK(calleeNames(*rootNamed(facts, "seeded")) ==
        std::vector<std::string>{"compute"});
  CHECK(facts.roots.size() == 3);
}

TEST_CASE("an inherited constructor is looked through to the base's",
          "[metrics][static-init]") {
  Baked b = bake({R"cpp(
int compute();
struct Base { Base(int); };
struct Member { Member(); };
struct Derived : Base {
  using Base::Base;
  Member member;
  int extra = compute();
};
Derived derived(1);
)cpp"});
  const StaticInitRoot *root = rootNamed(b.ix.staticInits, "derived");
  REQUIRE(root != nullptr);
  // The base constructor it forwards to, and what it initializes besides:
  // the inheriting constructor itself is not a function anyone wrote.
  CHECK(calleeNames(*root) == std::vector<std::string>{"Base::Base",
                                                       "Member::Member",
                                                       "compute"});
}

TEST_CASE("an explicit instantiation or specialization of a templated "
          "variable is a root and the template is not",
          "[metrics][static-init]") {
  Baked b = bake({R"cpp(
int compute();
template <class T> T seeded = T(compute());
template <> int seeded<int> = compute();
template long seeded<long>;
template <class T> struct Table { static int slot; };
template <class T> int Table<T>::slot = compute();
template struct Table<char>;
int use = seeded<short>;
)cpp"});
  const StaticInitFacts &facts = b.ix.staticInits;
  std::vector<std::pair<unsigned, std::string>> roots;
  for (const StaticInitRoot &root : facts.roots)
    roots.emplace_back(root.line, root.name);
  // The explicit instantiation is located at the template it
  // instantiates. Neither template is a root by itself, and neither is
  // the implicit seeded<short> that `use` reads.
  CHECK(roots == std::vector<std::pair<unsigned, std::string>>{
                     {3, "seeded<long>"},
                     {4, "seeded<int>"},
                     {7, "Table<char>::slot"},
                     {9, "use"}});
}

TEST_CASE("an instantiated callee holds its own calls in the graph",
          "[metrics][static-init]") {
  Baked b = bake({R"cpp(
int compute();
template <class T> T make() { int seed = compute(); return T(seed); }
int made = make<int>();
)cpp"});
  const StaticInitRoot *root = rootNamed(b.ix.staticInits, "made");
  REQUIRE(root != nullptr);
  REQUIRE(root->callees.size() == 1);
  const StaticInitCallee &callee = root->callees[0];
  // The bake reads the instantiation's body: make<int> calls compute
  // under its own identity.
  const auto callees = b.ix.graph.calleesOf(callee.usr);
  REQUIRE(callees.size() == 1);
  CHECK(callees[0].calleeName == "compute");
}

TEST_CASE("an initializer in a header is one root listing every TU that "
          "runs it",
          "[metrics][static-init]") {
  std::string header = writeTemp(R"cpp(
#pragma once
int compute();
inline int shared = compute();
)cpp",
                                 "hpp");
  std::string include = "#include \"" + header + "\"\n";
  Baked b = bake({include + "int first() { return shared; }\n",
                  include + "int second() { return shared; }\n"});
  b.paths.push_back(header);

  const StaticInitFacts &facts = b.ix.staticInits;
  REQUIRE(facts.roots.size() == 1);
  const StaticInitRoot &root = facts.roots[0];
  CHECK(root.name == "shared");
  CHECK(root.file == header);
  std::vector<std::string> tus = {b.paths[0], b.paths[1]};
  std::sort(tus.begin(), tus.end());
  CHECK(root.tus == tus);
  CHECK(calleeNames(root) == std::vector<std::string>{"compute"});
}

TEST_CASE("canonicalize merges and orders whatever order TUs arrived in",
          "[metrics][static-init]") {
  auto rootIn = [](const char *name, const char *file, unsigned line,
                   const char *tu, const char *callee) {
    StaticInitRoot root;
    root.name = name;
    root.usr = std::string("c:@") + name;
    root.file = file;
    root.line = line;
    root.tus = {tu};
    root.callees.push_back({std::string("c:@F@") + callee, callee});
    return root;
  };
  StaticInitFacts forward;
  forward.roots = {rootIn("shared", "/h.hpp", 3, "/a.cpp", "f"),
                   rootIn("late", "/b.cpp", 9, "/b.cpp", "g"),
                   rootIn("shared", "/h.hpp", 3, "/b.cpp", "f")};
  StaticInitFacts reversed;
  reversed.roots = {forward.roots[2], forward.roots[1], forward.roots[0]};
  forward.canonicalize();
  reversed.canonicalize();

  REQUIRE(forward.roots.size() == 2);
  CHECK(forward.roots[0].name == "late");
  CHECK(forward.roots[1].name == "shared");
  CHECK(forward.roots[1].tus == std::vector<std::string>{"/a.cpp", "/b.cpp"});
  CHECK(forward.roots[1].callees.size() == 1);

  REQUIRE(reversed.roots.size() == 2);
  for (size_t i = 0; i < 2; ++i) {
    CHECK(forward.roots[i].name == reversed.roots[i].name);
    CHECK(forward.roots[i].tus == reversed.roots[i].tus);
    CHECK(forward.roots[i].callees.size() == reversed.roots[i].callees.size());
  }

  // Idempotent.
  forward.canonicalize();
  CHECK(forward.roots.size() == 2);
  CHECK(forward.roots[1].tus.size() == 2);

  SECTION("one usr at two locations is two initializers") {
    // `static int slot = ...;` in a/util.cpp and b/util.cpp: internal
    // linkage, so both spell their usr `c:util.cpp@slot`.
    StaticInitFacts twins;
    StaticInitRoot a = rootIn("slot", "/a/util.cpp", 4, "/a/util.cpp", "f");
    StaticInitRoot b = rootIn("slot", "/b/util.cpp", 4, "/b/util.cpp", "g");
    b.usr = a.usr;
    twins.roots = {b, a};
    twins.canonicalize();
    REQUIRE(twins.roots.size() == 2);
    CHECK(twins.roots[0].file == "/a/util.cpp");
    CHECK(twins.roots[0].tus == std::vector<std::string>{"/a/util.cpp"});
    CHECK(twins.roots[0].callees[0].display == "f");
    CHECK(twins.roots[1].file == "/b/util.cpp");
    CHECK(twins.roots[1].callees[0].display == "g");
  }
}
