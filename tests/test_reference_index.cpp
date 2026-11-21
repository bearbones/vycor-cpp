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

// test_reference_index.cpp — symbol reference facts (ReferenceIndex.h,
// ReferenceVisitor.h; docs/symbol-references.md): one test per role over
// real sources, attribution (inline bodies, lambdas, initializers, file
// scope), linkage and declaration facts, and the per-TU bookkeeping
// (shared-header deduplication, removeTU, re-absorb).

#include "vycor/callgraph/ControlFlowIndex.h"
#include "vycor/callgraph/ReferenceIndex.h"

#include <catch2/catch_test_macros.hpp>
#include <clang/Tooling/CompilationDatabase.h>

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

using namespace vycor;

namespace {

// A scratch source tree: headers and TUs by name, baked together.
struct Project {
  std::string dir;
  std::vector<std::string> tus;
  std::vector<std::string> flags = {"-std=c++17"};

  Project() {
    llvm::SmallString<128> tmp;
    REQUIRE_FALSE(llvm::sys::fs::createUniqueDirectory("vycor_refs", tmp));
    llvm::SmallString<128> real;
    REQUIRE_FALSE(llvm::sys::fs::real_path(tmp, real));
    dir = std::string(real.str());
  }
  Project(const Project &) = delete;
  Project &operator=(const Project &) = delete;
  ~Project() { llvm::sys::fs::remove_directories(dir); }

  std::string path(const std::string &name) const { return dir + "/" + name; }

  void header(const std::string &name, const std::string &code) const {
    std::ofstream out(path(name));
    out << code;
  }
  std::string tu(const std::string &name, const std::string &code) {
    header(name, code);
    tus.push_back(path(name));
    return tus.back();
  }

  BakedIndexes bake(std::vector<std::string> files, unsigned threads = 1,
                    bool references = true) const {
    std::vector<std::string> args = flags;
    args.push_back("-I" + dir);
    clang::tooling::FixedCompilationDatabase compDb(dir, args);
    return bakeIndexes(compDb, files, {}, threads, nullptr, "", {}, nullptr,
                       nullptr, {}, references);
  }
  BakedIndexes bake() const { return bake(tus); }
};

BakedIndexes bakeOne(Project &p, const std::string &code) {
  p.tu("tu.cpp", code);
  return p.bake();
}

// "read,write": the roles `referrer` references `target` with, in role
// order. Names are qualified display names.
std::string roles(const ReferenceIndex &ix, const std::string &referrer,
                  const std::string &target) {
  std::string out;
  for (const ReferenceInfo &r : ix.allReferences()) {
    if (r.referrerName != referrer || r.targetName != target)
      continue;
    if (!out.empty())
      out += ',';
    out += refRoleName(r.role);
  }
  return out;
}

ReferenceInfo reference(const ReferenceIndex &ix, const std::string &referrer,
                        const std::string &target, RefRole role) {
  for (const ReferenceInfo &r : ix.allReferences())
    if (r.referrerName == referrer && r.targetName == target && r.role == role)
      return r;
  FAIL("no " << refRoleName(role) << " reference " << referrer << " -> "
             << target);
  return {};
}

SymbolInfo symbolNamed(const ReferenceIndex &ix, const std::string &name) {
  std::vector<std::string> usrs = ix.usrsNamed(name);
  INFO("symbol " << name);
  REQUIRE(usrs.size() == 1);
  auto info = ix.symbol(usrs.front());
  REQUIRE(info.has_value());
  return *info;
}

bool hasSymbol(const ReferenceIndex &ix, const std::string &name) {
  return !ix.usrsNamed(name).empty();
}

// Everything a query can observe, as text.
std::string describe(const ReferenceIndex &ix) {
  std::string out;
  for (const SymbolInfo &s : ix.allSymbols()) {
    out += "S " + s.usr + " " + s.name + " " + symbolKindName(s.kind) + " " +
           linkageClassName(s.linkage) + " f" + std::to_string(s.flags) +
           " z" + std::to_string(s.size) + " d" +
           std::to_string(s.definingTuCount) + " " + s.firstDefiningTu + " v" +
           s.vtableHomeTu + "\n";
    for (const SymbolDeclInfo &d : s.declarations)
      out += "  D " + d.file + ":" + std::to_string(d.line) +
             (d.definition ? " def" : "") + (d.inHeader ? " hdr" : "") + " n" +
             std::to_string(d.tuCount) + " " + d.firstTu + "\n";
  }
  for (const ReferenceInfo &r : ix.allReferences())
    out += "R " + r.referrerUsr + " -> " + r.targetUsr + " " +
           refRoleName(r.role) + " x" + std::to_string(r.count) + " " + r.file +
           ":" + std::to_string(r.line) + ":" + std::to_string(r.col) + " n" +
           std::to_string(r.tuCount) + " " + r.firstTu + "\n";
  return out;
}

} // namespace

// ============================================================================
// Roles
// ============================================================================

TEST_CASE("references: call and address_of of functions", "[references]") {
  Project p;
  auto ix = bakeOne(p, R"cpp(
void callee();
void taken();
void both();
typedef void (*Fn)();
void caller() { callee(); (*both)(); }
Fn pick() { return taken; }
Fn pick2() { Fn f = &both; return f; }
void indirect(Fn f) { f(); }
)cpp");
  const ReferenceIndex &refs = ix.references;
  CHECK(roles(refs, "caller", "callee") == "call");
  CHECK(roles(refs, "caller", "both") == "call");
  CHECK(roles(refs, "pick", "taken") == "address_of");
  CHECK(roles(refs, "pick2", "both") == "address_of");
  // A call through a parameter references no symbol.
  CHECK(refs.referencesFrom(symbolNamed(refs, "caller").usr).size() == 2);
  CHECK_FALSE(hasSymbol(refs, "f"));
}

TEST_CASE("references: read and write of globals", "[references]") {
  Project p;
  auto ix = bakeOne(p, R"cpp(
int g;
int h;
int arr[4];
struct Pt { int x; int y; };
Pt pt;
int reader() { return g; }
void writer() { g = 1; }
void compound() { g += 2; ++h; }
int element(int i) { return arr[i]; }
void store(int i) { arr[i] = 0; }
int field() { return pt.x; }
void setField() { pt.y = 3; }
int *escape() { return &g; }
int *decay() { return arr; }
int &alias() { return h; }
void take(const int &);
void byConstRef() { take(g); }
void viaConstRef() { const int &r = h; (void)r; }
)cpp");
  const ReferenceIndex &refs = ix.references;
  CHECK(roles(refs, "reader", "g") == "read");
  CHECK(roles(refs, "writer", "g") == "write");
  CHECK(roles(refs, "compound", "g") == "read,write");
  CHECK(roles(refs, "compound", "h") == "read,write");
  CHECK(roles(refs, "element", "arr") == "read");
  CHECK(roles(refs, "store", "arr") == "write");
  CHECK(roles(refs, "field", "pt") == "read");
  CHECK(roles(refs, "setField", "pt") == "write");
  CHECK(roles(refs, "escape", "g") == "address_of");
  CHECK(roles(refs, "decay", "arr") == "address_of");
  CHECK(roles(refs, "alias", "h") == "address_of");
  CHECK(roles(refs, "byConstRef", "g") == "read");
  CHECK(roles(refs, "viaConstRef", "h") == "read");
}

TEST_CASE("references: member calls on a global object read or write it",
          "[references]") {
  Project p;
  auto ix = bakeOne(p, R"cpp(
struct Counter {
  int n = 0;
  int get() const { return n; }
  void bump() { ++n; }
  Counter &operator+=(int d) { n += d; return *this; }
  int operator[](int) const { return n; }
};
Counter c;
int observe() { return c.get(); }
void mutate() { c.bump(); }
void add() { c += 2; }
int index() { return c[0]; }
)cpp");
  const ReferenceIndex &refs = ix.references;
  CHECK(roles(refs, "observe", "c") == "read");
  CHECK(roles(refs, "mutate", "c") == "write");
  CHECK(roles(refs, "add", "c") == "write");
  CHECK(roles(refs, "index", "c") == "read");
  CHECK(roles(refs, "observe", "Counter::get") == "call");
  CHECK(roles(refs, "add", "Counter::operator+=") == "call");
}

TEST_CASE("references: a global passed or bound by reference escapes",
          "[references]") {
  Project p;
  auto ix = bakeOne(p, R"cpp(
int g;
struct Box { int v; static int count; static void reset(); };
Box gb;
void fill(int &);
void peek(const int &);
void move(int &&);
struct Holder { Holder(int &); };
void byRef() { fill(g); }
void byConstRef() { peek(g); }
void construct() { Holder h(g); (void)h; }
void bind() { int &r = g; r = 1; }
void bindField() { int &r = gb.v; r = 1; }
// A static member reached through an object names the object, no more.
void viaObject() { gb.count = 1; gb.reset(); }
)cpp");
  const ReferenceIndex &refs = ix.references;
  CHECK(roles(refs, "byRef", "g") == "address_of");
  CHECK(roles(refs, "byConstRef", "g") == "read");
  CHECK(roles(refs, "construct", "g") == "address_of");
  CHECK(roles(refs, "bind", "g") == "address_of");
  CHECK(roles(refs, "bindField", "gb") == "address_of");
  CHECK(roles(refs, "viaObject", "gb") == "read");
  CHECK(roles(refs, "viaObject", "Box::count") == "write");
  CHECK(roles(refs, "viaObject", "Box::reset") == "call");
}

// In a template body an expression that depends on a parameter is kept
// unresolved. What it does to a global is read off the operator it spells;
// where that says nothing the global is recorded as escaping, never as
// merely read.
TEST_CASE("references: template bodies do not turn writes into reads",
          "[references]") {
  Project p;
  auto ix = bakeOne(p, R"cpp(
int g;
int arr[4];
struct Box {
  int v;
  void put(int);
  void put(long);
  int get() const;
  int get(int) const;
};
Box gb;
template <class T> void tWrite(T v) { g = v; }
template <class T> void tCompound(T v) { g += v; }
template <class T> void tIncrement(T) { ++g; }
template <class T> T tRead(T v) { return g + v; }
template <class T> T tReturn() { return g; }
template <class T> auto tAuto(T) { return g; }
template <class T> decltype(auto) tAlias(T) { return (g); }
template <class T> T tCast() { return static_cast<T>(g); }
template <class T> bool tCompare(T v) { return v < g; }
template <class T> T tNegate(T) { return -g; }
template <class T> void tStore(T i) { arr[i] = 1; }
template <class T> int tLoad(T i) { return arr[i]; }
template <class T> int tLocal(T i) { int v = arr[i]; auto w = arr[i]; return v + w; }
template <class T> void tPass(T i) { consume(arr[i]); }
template <class T> int &tElement(T i) { return arr[i]; }
template <class T> void tField(T v) { gb.v = v; }
template <class T> void tPut(T v) { gb.put(v); }
template <class T> int tGet(T v) { return gb.get(v); }
template <class T> void tMember(T v) { v.take(g); }
template <class T> void tFree(T v) { consume(v, g); }
template <class T> void tStream(T &out) { out << g; }
template <class T> void tInsert(T v) { g << v; }
template <class T> void tBind(T) { int &r = g; r = 1; }
template <class T> void tAddress(T v) { v = &g; }
template <class T> void tPlain(T) { g = 2; }
template <class T> int tPlainRead(T) { return g; }
)cpp");
  const ReferenceIndex &refs = ix.references;
  CHECK(roles(refs, "tWrite", "g") == "write");
  CHECK(roles(refs, "tCompound", "g") == "read,write");
  CHECK(roles(refs, "tIncrement", "g") == "read,write");
  CHECK(roles(refs, "tRead", "g") == "read");
  CHECK(roles(refs, "tReturn", "g") == "read");
  CHECK(roles(refs, "tAuto", "g") == "read");
  CHECK(roles(refs, "tAlias", "g") == "address_of");
  CHECK(roles(refs, "tCast", "g") == "read");
  CHECK(roles(refs, "tCompare", "g") == "read");
  CHECK(roles(refs, "tNegate", "g") == "read");
  CHECK(roles(refs, "tStore", "arr") == "write");
  CHECK(roles(refs, "tLoad", "arr") == "read");
  CHECK(roles(refs, "tLocal", "arr") == "read");
  CHECK(roles(refs, "tPass", "arr") == "address_of");
  CHECK(roles(refs, "tElement", "arr") == "address_of");
  CHECK(roles(refs, "tField", "gb") == "write");
  CHECK(roles(refs, "tPut", "gb") == "write");
  CHECK(roles(refs, "tGet", "gb") == "read");
  CHECK(roles(refs, "tMember", "g") == "address_of");
  CHECK(roles(refs, "tFree", "g") == "address_of");
  CHECK(roles(refs, "tStream", "g") == "read");
  CHECK(roles(refs, "tInsert", "g") == "address_of");
  CHECK(roles(refs, "tBind", "g") == "address_of");
  CHECK(roles(refs, "tAddress", "g") == "address_of");
  CHECK(roles(refs, "tPlain", "g") == "write");
  CHECK(roles(refs, "tPlainRead", "g") == "read");
}

TEST_CASE("references: a C function called without a declaration is a symbol",
          "[references]") {
  Project p;
  p.flags = {"-std=gnu89", "-Wno-implicit-function-declaration"};
  p.tu("a.c", R"c(int caller(void) { return __builtin_expect(undeclared(3), 0); }
)c");
  p.tu("b.c", R"c(int undeclared(int v) { return v; }
)c");
  auto ix = p.bake();
  const ReferenceIndex &refs = ix.references;
  CHECK(roles(refs, "caller", "undeclared") == "call");
  SymbolInfo sym = symbolNamed(refs, "undeclared");
  CHECK(sym.linkage == LinkageClass::External);
  CHECK(sym.definingTuCount == 1);
  CHECK(sym.firstDefiningTu == p.path("b.c"));
  CHECK_FALSE(sym.declaredInHeader);
  // The call site in a.c stands in for the missing prototype.
  REQUIRE(sym.declarations.size() == 2);
  CHECK(sym.declarations[0].file == p.path("a.c"));
  CHECK_FALSE(sym.declarations[0].definition);
  CHECK(sym.declarations[1].file == p.path("b.c"));
  CHECK(sym.declarations[1].definition);
  // A compiler builtin is not.
  CHECK_FALSE(hasSymbol(refs, "__builtin_expect"));
}

TEST_CASE("references: an inherited constructor is a call of the base's",
          "[references]") {
  Project p;
  auto ix = bakeOne(p, R"cpp(
struct Base { Base(int); };
struct Mid : Base { using Base::Base; };
struct Leaf : Mid { using Mid::Mid; };
void make() { Leaf leaf(1); (void)leaf; }
)cpp");
  CHECK(roles(ix.references, "make", "Base::Base") == "call");
}

// Syntax the compiler rewrites into calls the source does not spell.
TEST_CASE("references: range-for and delete and cleanup call what they "
          "desugar to",
          "[references]") {
  Project p;
  auto ix = bakeOne(p, R"cpp(
struct Iter {
  int *at;
  int &operator*() const;
  Iter &operator++();
};
bool operator!=(Iter, Iter);
struct Span { int data[3]; };
Iter begin(Span &);
Iter end(Span &);
Span gs;
int total() {
  int sum = 0;
  for (int v : gs)
    sum += v;
  return sum;
}
template <class R> int totalOf(R &range) {
  int sum = 0;
  for (int v : range)
    sum += v;
  return sum;
}

struct Node { ~Node(); };
struct Plain { int v; };
void drop(Node *n, Plain *p) { delete n; delete p; }

void release(int *);
void scoped() { __attribute__((cleanup(release))) int handle = 0; (void)handle; }
)cpp");
  const ReferenceIndex &refs = ix.references;
  CHECK(roles(refs, "total", "begin") == "call");
  CHECK(roles(refs, "total", "end") == "call");
  CHECK(roles(refs, "total", "operator!=") == "call");
  CHECK(roles(refs, "total", "Iter::operator++") == "call");
  CHECK(roles(refs, "total", "Iter::operator*") == "call");
  CHECK(reference(refs, "total", "begin", RefRole::Call).count == 1);
  CHECK(roles(refs, "total", "gs") == "read");
  // Nothing is known about the range of a template's loop.
  CHECK(roles(refs, "totalOf", "begin").empty());

  CHECK(roles(refs, "drop", "Node::~Node") == "call");
  CHECK_FALSE(hasSymbol(refs, "Plain::~Plain"));
  CHECK(roles(refs, "scoped", "release") == "call");
}

TEST_CASE("references: structured bindings", "[references]") {
  Project p;
  auto ix = bakeOne(p, R"cpp(
namespace std {
template <class T> struct tuple_size;
template <unsigned long I, class T> struct tuple_element;
}
struct Pair { int a; int b; };
struct Rec { int x; long y; };
template <unsigned long I> int field(const Rec &);
namespace std {
template <> struct tuple_size<Rec> { static const unsigned long value = 2; };
template <unsigned long I> struct tuple_element<I, Rec> { using type = int; };
}
template <unsigned long I> int get(const Rec &r) { return field<I>(r); }

Pair gp;
auto [first, second] = gp;
int readBinding() { return first; }
void writeBinding() { second = 2; }

Rec gr;
int unpack() {
  auto [x, y] = gr;
  return x + y;
}
)cpp");
  const ReferenceIndex &refs = ix.references;
  // A namespace-scope binding names its hidden variable.
  bool bindingRead = false, bindingWritten = false;
  for (const ReferenceInfo &r : refs.allReferences()) {
    if (r.referrerName == "readBinding" && r.role == RefRole::Read)
      bindingRead = true;
    if (r.referrerName == "writeBinding" && r.role == RefRole::Write)
      bindingWritten = true;
  }
  CHECK(bindingRead);
  CHECK(bindingWritten);
  // The tuple protocol calls get<I>() once per name.
  CHECK(reference(refs, "unpack", "get", RefRole::Call).count == 2);
}

TEST_CASE("references: a rewritten comparison calls the operator it uses",
          "[references]") {
  Project p;
  p.flags = {"-std=c++20"};
  auto ix = bakeOne(p, R"cpp(
struct Key {
  int v;
  bool operator==(const Key &) const;
  int operator<=>(const Key &) const;
};
Key ga, gb;
bool differ() { return ga != gb; }
bool before() { return ga < gb; }
)cpp");
  const ReferenceIndex &refs = ix.references;
  CHECK(roles(refs, "differ", "Key::operator==") == "call");
  CHECK(roles(refs, "before", "Key::operator<=>") == "call");
  CHECK(roles(refs, "differ", "ga") == "read");
  CHECK(roles(refs, "differ", "gb") == "read");
}

TEST_CASE("references: a file has one name however it was included",
          "[references]") {
  Project p;
  REQUIRE_FALSE(llvm::sys::fs::create_directory(p.path("inc")));
  REQUIRE_FALSE(llvm::sys::fs::create_directory(p.path("src")));
  p.header("inc/api.h", R"cpp(#pragma once
void shared();
inline int twice(int v) { return v * 2; }
static int table[] = {twice(1)};
)cpp");
  p.tu("src/a.cpp", R"cpp(#include "../inc/api.h"
void shared() {}
int a() { return twice(table[0]); }
)cpp");
  p.tu("src/b.cpp", R"cpp(#include "../src/../inc/./api.h"
int b() { shared(); return twice(table[0]); }
)cpp");
  p.tu("c.cpp", R"cpp(#include "inc/api.h"
int c() { shared(); return twice(2); }
)cpp");
  auto ix = p.bake();
  const ReferenceIndex &refs = ix.references;
  const std::string header = p.path("inc/api.h");

  SymbolInfo shared = symbolNamed(refs, "shared");
  REQUIRE(shared.declarations.size() == 2);
  CHECK(shared.declarations[0].file == header);
  CHECK(shared.declarations[0].tuCount == 3);
  SymbolInfo twice = symbolNamed(refs, "twice");
  REQUIRE(twice.declarations.size() == 1);
  CHECK(twice.declarations[0].file == header);
  for (const SymbolInfo &s : refs.allSymbols())
    for (const SymbolDeclInfo &d : s.declarations) {
      INFO(s.name << " declared in " << d.file);
      CHECK(d.file.find("..") == std::string::npos);
      CHECK(llvm::sys::path::is_absolute(d.file));
    }
  for (const ReferenceInfo &r : refs.allReferences()) {
    INFO(r.referrerName << " -> " << r.targetName << " at " << r.file);
    CHECK(r.file.find("/.") == std::string::npos);
  }
}

TEST_CASE("references: the visibility flag means explicitly exported",
          "[references]") {
  Project p;
  auto ix = bakeOne(p, R"cpp(
#define EXPORT __attribute__((visibility("default")))
#define HIDDEN __attribute__((visibility("hidden")))
EXPORT void onDecl() {}
HIDDEN void hidden() {}
void plain() {}
namespace api EXPORT { void inNamespace() {} }
struct EXPORT Api { void method(); static int counter; };
void Api::method() {}
int Api::counter = 0;
#pragma GCC visibility push(default)
void byPragma() {}
#pragma GCC visibility pop
#pragma GCC visibility push(hidden)
void hiddenByPragma() {}
#pragma GCC visibility pop
EXPORT static void internalExport() {}
void use() { internalExport(); }
)cpp");
  const ReferenceIndex &refs = ix.references;
  const auto exported = [&](const char *name) {
    return (symbolNamed(refs, name).flags & kSymVisibility) != 0;
  };
  CHECK(exported("onDecl"));
  CHECK(exported("api::inNamespace"));
  CHECK(exported("Api::method"));
  CHECK(exported("Api::counter"));
  CHECK(exported("byPragma"));
  CHECK_FALSE(exported("hidden"));
  CHECK_FALSE(exported("hiddenByPragma"));
  CHECK_FALSE(exported("plain"));
}

TEST_CASE("references: one record per referrer and target and role with a "
          "count and the first site",
          "[references]") {
  Project p;
  std::string tu = p.tu("tu.cpp", R"cpp(int g;
void callee();
void f() {
  g = 1;
  callee();
  g = 2;
  callee();
  callee();
}
)cpp");
  auto ix = p.bake();
  const ReferenceIndex &refs = ix.references;
  ReferenceInfo write = reference(refs, "f", "g", RefRole::Write);
  CHECK(write.count == 2);
  CHECK(write.file == tu);
  CHECK(write.line == 4);
  CHECK(write.col == 3);
  CHECK(write.tuCount == 1);
  CHECK(write.firstTu == tu);
  ReferenceInfo call = reference(refs, "f", "callee", RefRole::Call);
  CHECK(call.count == 3);
  CHECK(call.line == 5);
  CHECK(refs.referenceCount() == 2);
}

TEST_CASE("references: vtable_use at constructors and construction sites",
          "[references]") {
  Project p;
  std::string tu = p.tu("tu.cpp", R"cpp(
struct Base {
  virtual ~Base();
  virtual void f();
};
struct Derived : Base {
  Derived();
  void f() override;
};
struct Plain { virtual void v() {} };
struct Pod { int x; };
Derived::Derived() {}
Base::~Base() {}
void make() { Plain p; (void)p; Pod q{}; (void)q; }
Derived *heap() { return new Derived(); }
)cpp");
  auto ix = p.bake();
  const ReferenceIndex &refs = ix.references;
  // A user-provided constructor or destructor stores the vtable pointer.
  CHECK(roles(refs, "Derived::Derived", "Derived") == "vtable_use");
  CHECK(roles(refs, "Base::~Base", "Base") == "vtable_use");
  // An implicit constructor is inlined at the construction site.
  CHECK(roles(refs, "make", "Plain") == "vtable_use");
  CHECK(roles(refs, "make", "Pod").empty());
  // Constructing through a user-provided constructor is a call, not a
  // vtable use.
  CHECK(roles(refs, "heap", "Derived::Derived") == "call");
  CHECK(roles(refs, "heap", "Derived").empty());

  // ~Base is Base's key function: this TU is the vtable home.
  SymbolInfo base = symbolNamed(refs, "Base");
  CHECK(base.kind == SymbolKind::Class);
  CHECK(base.linkage == LinkageClass::External);
  CHECK((base.flags & kSymDynamicClass) != 0);
  CHECK((base.flags & kSymVtableHome) != 0);
  CHECK(base.vtableHomeTu == tu);
  // Derived::f, its key function, is not defined here.
  SymbolInfo derived = symbolNamed(refs, "Derived");
  CHECK(derived.linkage == LinkageClass::External);
  CHECK((derived.flags & kSymVtableHome) == 0);
  CHECK(derived.vtableHomeTu.empty());
  // No key function: vague.
  CHECK(symbolNamed(refs, "Plain").linkage == LinkageClass::Vague);
}

TEST_CASE("references: typeinfo_use from dynamic_cast and typeid and throw "
          "and catch",
          "[references]") {
  Project p;
  auto ix = bakeOne(p, R"cpp(
namespace std { class type_info { public: virtual ~type_info(); }; }
struct Base { virtual ~Base(); };
struct Derived : Base { Derived(); };
struct Err { int code; };
bool is(Base *b) { return dynamic_cast<Derived *>(b) != nullptr; }
Base *up(Derived *d) { return dynamic_cast<Base *>(d); }
const std::type_info &name() { return typeid(Derived); }
const std::type_info &dyn(Base &b) { return typeid(b); }
void thrower() { throw Err{1}; }
void catcher() { try { thrower(); } catch (const Err &) {} catch (...) {} }
unsigned long size() { return sizeof(typeid(Base)); }
)cpp");
  const ReferenceIndex &refs = ix.references;
  CHECK(roles(refs, "is", "Derived") == "typeinfo_use");
  CHECK(roles(refs, "is", "Base") == "typeinfo_use");
  // An upcast spelled dynamic_cast needs no typeinfo.
  CHECK(roles(refs, "up", "Base").empty());
  CHECK(roles(refs, "name", "Derived") == "typeinfo_use");
  CHECK(roles(refs, "dyn", "Base") == "typeinfo_use");
  CHECK(roles(refs, "thrower", "Err") == "typeinfo_use");
  CHECK(roles(refs, "catcher", "Err") == "typeinfo_use");
  CHECK(roles(refs, "size", "Base").empty());
}

// ============================================================================
// Attribution
// ============================================================================

TEST_CASE("references: an inline body in a header belongs to its function "
          "in every TU that includes it",
          "[references]") {
  Project p;
  p.header("lib.h", R"cpp(#pragma once
extern int counter;
void sink(int);
inline void bump() { ++counter; sink(counter); }
template <class T> T twice(T x) { sink(counter); return x + x; }
)cpp");
  std::string a = p.tu("a.cpp", R"cpp(#include "lib.h"
int counter = 0;
void fromA() { bump(); }
)cpp");
  std::string b = p.tu("b.cpp", R"cpp(#include "lib.h"
void sink(int) {}
int fromB() { return twice(2); }
)cpp");
  auto ix = p.bake();
  const ReferenceIndex &refs = ix.references;

  // Attributed to bump, not to a.cpp or b.cpp; one record, two TUs.
  ReferenceInfo write = reference(refs, "bump", "counter", RefRole::Write);
  CHECK(write.file == p.path("lib.h"));
  CHECK(write.line == 4);
  CHECK(write.tuCount == 2);
  CHECK(write.firstTu == a);
  CHECK(roles(refs, "bump", "counter") == "read,write");
  CHECK(roles(refs, "bump", "sink") == "call");
  CHECK(roles(refs, "fromA", "bump") == "call");
  CHECK(roles(refs, "fromA", "counter").empty());
  CHECK(reference(refs, "fromA", "bump", RefRole::Call).tuCount == 1);

  // A template body is read as written; a use of an instantiation is a
  // reference to the template.
  CHECK(roles(refs, "twice", "sink") == "call");
  CHECK(roles(refs, "twice", "counter") == "read");
  CHECK(roles(refs, "fromB", "twice") == "call");
  CHECK(symbolNamed(refs, "twice").linkage ==
        LinkageClass::TemplateInstantiation);

  SymbolInfo bump = symbolNamed(refs, "bump");
  CHECK(bump.linkage == LinkageClass::Vague);
  CHECK(bump.declaredInHeader);
  CHECK(bump.definingTuCount == 2);
  CHECK(bump.firstDefiningTu == a);

  SymbolInfo counter = symbolNamed(refs, "counter");
  CHECK(counter.kind == SymbolKind::Variable);
  CHECK(counter.linkage == LinkageClass::External);
  CHECK(counter.declaredInHeader);
  CHECK(counter.definingTuCount == 1);
  CHECK(counter.firstDefiningTu == a);
  REQUIRE(counter.declarations.size() == 2);
  CHECK(counter.declarations[0].file == a);
  CHECK(counter.declarations[0].definition);
  CHECK_FALSE(counter.declarations[0].inHeader);
  CHECK(counter.declarations[1].file == p.path("lib.h"));
  CHECK_FALSE(counter.declarations[1].definition);
  CHECK(counter.declarations[1].inHeader);
  CHECK(counter.declarations[1].tuCount == 2);

  SymbolInfo sink = symbolNamed(refs, "sink");
  CHECK(sink.definingTuCount == 1);
  CHECK(sink.firstDefiningTu == b);
}

TEST_CASE("references: static data members", "[references]") {
  Project p;
  auto ix = bakeOne(p, R"cpp(
struct Registry {
  static int count;
  static const int limit = 8;
  static void add();
  int local = 0;
  int size() const { return count + local; }
};
int Registry::count = 0;
void Registry::add() { ++count; }
int peek() { return Registry::count; }
int viaObject(Registry &r) { return r.count; }
int cap() { return Registry::limit; }
)cpp");
  const ReferenceIndex &refs = ix.references;
  CHECK(roles(refs, "Registry::add", "Registry::count") == "read,write");
  CHECK(roles(refs, "Registry::size", "Registry::count") == "read");
  CHECK(roles(refs, "peek", "Registry::count") == "read");
  CHECK(roles(refs, "viaObject", "Registry::count") == "read");
  // A constant read by value is a read with no symbol reference emitted by
  // the compiler; the fact is still recorded.
  CHECK(roles(refs, "cap", "Registry::limit") == "read");
  // A non-static member is not a symbol.
  CHECK_FALSE(hasSymbol(refs, "Registry::local"));

  SymbolInfo count = symbolNamed(refs, "Registry::count");
  CHECK(count.kind == SymbolKind::Variable);
  CHECK(count.linkage == LinkageClass::External);
  CHECK((count.flags & kSymMember) != 0);
  CHECK(count.definingTuCount == 1);
  CHECK(count.size == sizeof(int));
  SymbolInfo add = symbolNamed(refs, "Registry::add");
  CHECK((add.flags & kSymMember) != 0);
  CHECK(add.linkage == LinkageClass::External);
  CHECK(symbolNamed(refs, "Registry::size").linkage == LinkageClass::Vague);
}

TEST_CASE("references: function-local statics", "[references]") {
  Project p;
  auto ix = bakeOne(p, R"cpp(
int seed();
int next() {
  static int n = seed();
  return ++n;
}
inline int &shared() {
  static int s = 0;
  return s;
}
int use() { return shared(); }
)cpp");
  const ReferenceIndex &refs = ix.references;
  // The initializer of a local static runs inside the function.
  CHECK(roles(refs, "next", "seed") == "call");
  CHECK(roles(refs, "next", "next::n") == "read,write");
  CHECK(roles(refs, "shared", "shared::s") == "address_of");

  SymbolInfo n = symbolNamed(refs, "next::n");
  CHECK(n.kind == SymbolKind::Variable);
  CHECK((n.flags & kSymStaticLocal) != 0);
  CHECK(n.linkage == LinkageClass::Internal);
  // A local static of an inline function is one object program-wide.
  SymbolInfo s = symbolNamed(refs, "shared::s");
  CHECK((s.flags & kSymStaticLocal) != 0);
  CHECK(s.linkage == LinkageClass::Vague);
}

TEST_CASE("references: addresses in initializer tables belong to the table",
          "[references]") {
  Project p;
  auto ix = bakeOne(p, R"cpp(
void onA();
void onB();
static void onC() {}
int value;
typedef void (*Fn)();
struct Entry { const char *name; Fn fn; int *slot; };
Fn plain[] = {onA, &onB};
static const Entry entries[] = {{"a", onA, &value}, {"c", onC, nullptr}};
Fn single = onB;
int copied = value;
void run() { entries[0].fn(); }
)cpp");
  const ReferenceIndex &refs = ix.references;
  CHECK(roles(refs, "plain", "onA") == "address_of");
  CHECK(roles(refs, "plain", "onB") == "address_of");
  CHECK(roles(refs, "entries", "onA") == "address_of");
  CHECK(roles(refs, "entries", "onC") == "address_of");
  CHECK(roles(refs, "entries", "value") == "address_of");
  CHECK(roles(refs, "single", "onB") == "address_of");
  CHECK(roles(refs, "copied", "value") == "read");
  // Calling through the table reads the table; the callee is unknown.
  CHECK(roles(refs, "run", "entries") == "read");
  CHECK(roles(refs, "run", "onA").empty());

  CHECK(symbolNamed(refs, "entries").kind == SymbolKind::Variable);
  CHECK(symbolNamed(refs, "entries").linkage == LinkageClass::Internal);
  CHECK((symbolNamed(refs, "entries").flags & kSymConst) != 0);
  CHECK(symbolNamed(refs, "onC").linkage == LinkageClass::Internal);
}

TEST_CASE("references: a lambda is its own referrer under the call graph's "
          "identity",
          "[references]") {
  Project p;
  auto ix = bakeOne(p, R"cpp(
int g;
void callee();
template <class F> void each(F f) { f(); }
void host() {
  each([] { ++g; callee(); });
}
auto atFileScope = [] { return g; };
)cpp");
  const ReferenceIndex &refs = ix.references;
  std::string lambdaUsr;
  std::string fileScopeUsr;
  for (const ReferenceInfo &r : refs.allReferences()) {
    if (r.targetName == "callee")
      lambdaUsr = r.referrerUsr;
    if (r.targetName == "g" && r.role == RefRole::Read &&
        r.referrerName.find("<tu>") != std::string::npos)
      fileScopeUsr = r.referrerUsr;
  }
  REQUIRE_FALSE(lambdaUsr.empty());
  CHECK(lambdaUsr.rfind("vycor-lambda:", 0) == 0);
  CHECK(lambdaUsr.find("#host") != std::string::npos);
  // The same node the call graph holds for the closure.
  CHECK(ix.graph.findNode(lambdaUsr) != nullptr);
  CHECK(roles(refs, "host", "g").empty());
  CHECK(roles(refs, "host", "callee").empty());
  REQUIRE_FALSE(fileScopeUsr.empty());

  auto info = refs.symbol(lambdaUsr);
  REQUIRE(info.has_value());
  CHECK(info->kind == SymbolKind::Function);
  CHECK(info->linkage == LinkageClass::Internal);
  CHECK((info->flags & kSymLambda) != 0);
}

TEST_CASE("references: file scope is the referrer outside functions and "
          "variable initializers",
          "[references]") {
  Project p;
  std::string tu = p.tu("tu.cpp", R"cpp(
int fallback();
struct Config {
  int level = fallback();
};
)cpp");
  auto ix = p.bake();
  const ReferenceIndex &refs = ix.references;
  auto to = refs.referencesTo(symbolNamed(refs, "fallback").usr);
  REQUIRE(to.size() == 1);
  CHECK(to[0].role == RefRole::Call);
  CHECK(to[0].referrerKind == SymbolKind::File);
  CHECK(to[0].referrerName == tu);
  CHECK(to[0].referrerUsr == "vycor-file:" + tu);
}

TEST_CASE("references: unevaluated operands reference nothing",
          "[references]") {
  Project p;
  auto ix = bakeOne(p, R"cpp(
int g;
int f();
struct S { static int m; };
unsigned long a() { return sizeof(g) + sizeof(f()) + alignof(S); }
decltype(f()) b();
bool c() noexcept(noexcept(f())) { return noexcept(f()); }
decltype(S::m) d = 0;
)cpp");
  const ReferenceIndex &refs = ix.references;
  CHECK(refs.referenceCount() == 0);
}

TEST_CASE("references: requirements and concepts reference nothing",
          "[references]") {
  Project p;
  p.flags = {"-std=c++20"};
  auto ix = bakeOne(p, R"cpp(
int g;
int f();
int over(int);
int over(long);
template <class T> concept Usable = requires(T t) { f(); over(t); g + t; };
template <class T> concept Sized = sizeof(over(T())) > 0;
template <class T>
bool check(T t) { return requires { over(t); }; }
)cpp");
  CHECK(ix.references.referenceCount() == 0);
}

// ============================================================================
// Symbol facts
// ============================================================================

TEST_CASE("references: linkage classes and symbol flags", "[references]") {
  Project p;
  p.header("api.h", R"cpp(#pragma once
void exported();
void headerOnly();
extern "C" void c_api(void);
)cpp");
  std::string tu = p.tu("tu.cpp", R"cpp(#include "api.h"
void exported() {}
void local() {}
static void fileStatic() {}
namespace { void anon() {} int anonVar = 0; }
inline void inl() {}
template <class T> void tmpl(T) {}
extern "C" void c_api(void) {}
__attribute__((visibility("default"))) void visible() {}
__attribute__((used)) static void kept() {}
__attribute__((weak)) void weakFn() {}
thread_local int tls = 0;
const int kLimit = 4;
extern const int kShared = 5;
int main() {
  fileStatic(); anon(); inl(); tmpl(1); headerOnly();
  return anonVar + kLimit;
}
)cpp");
  auto ix = p.bake();
  const ReferenceIndex &refs = ix.references;

  SymbolInfo exported = symbolNamed(refs, "exported");
  CHECK(exported.kind == SymbolKind::Function);
  CHECK(exported.linkage == LinkageClass::External);
  CHECK(exported.declaredInHeader);
  CHECK(exported.definingTuCount == 1);
  CHECK(exported.firstDefiningTu == tu);
  CHECK(exported.size == 1);
  CHECK(exported.flags == 0);

  // Defined here, declared nowhere else: recorded although unreferenced.
  SymbolInfo local = symbolNamed(refs, "local");
  CHECK(local.linkage == LinkageClass::External);
  CHECK_FALSE(local.declaredInHeader);
  REQUIRE(local.declarations.size() == 1);
  CHECK(local.declarations[0].line == 3);
  CHECK(local.declarations[0].definition);

  CHECK(symbolNamed(refs, "fileStatic").linkage == LinkageClass::Internal);
  CHECK(symbolNamed(refs, "(anonymous namespace)::anon").linkage ==
        LinkageClass::Internal);
  CHECK(symbolNamed(refs, "(anonymous namespace)::anonVar").linkage ==
        LinkageClass::Internal);
  CHECK(symbolNamed(refs, "inl").linkage == LinkageClass::Vague);
  CHECK(symbolNamed(refs, "tmpl").linkage ==
        LinkageClass::TemplateInstantiation);

  SymbolInfo cApi = symbolNamed(refs, "c_api");
  CHECK((cApi.flags & kSymExternC) != 0);
  CHECK(cApi.declaredInHeader);
  CHECK((symbolNamed(refs, "visible").flags & kSymVisibility) != 0);
  CHECK((symbolNamed(refs, "kept").flags & kSymUsedAttr) != 0);
  CHECK((symbolNamed(refs, "weakFn").flags & kSymWeak) != 0);
  CHECK((symbolNamed(refs, "tls").flags & kSymThreadLocal) != 0);
  CHECK((symbolNamed(refs, "main").flags & kSymMain) != 0);

  // A namespace-scope const has internal linkage unless declared extern.
  SymbolInfo limit = symbolNamed(refs, "kLimit");
  CHECK(limit.linkage == LinkageClass::Internal);
  CHECK((limit.flags & kSymConst) != 0);
  CHECK(symbolNamed(refs, "kShared").linkage == LinkageClass::External);

  // Declared in a header, never defined: known through the reference.
  SymbolInfo headerOnly = symbolNamed(refs, "headerOnly");
  CHECK(headerOnly.definingTuCount == 0);
  CHECK(headerOnly.declaredInHeader);
  CHECK(headerOnly.firstDefiningTu.empty());
}

TEST_CASE("references: explicit instantiations are flagged on the template",
          "[references]") {
  Project p;
  auto ix = bakeOne(p, R"cpp(
template <class T> struct Box { T v; T get() const { return v; } };
template <class T> T ident(T x) { return x; }
template struct Box<int>;
template int ident<int>(int);
template <class T> struct Unused { T v; };
)cpp");
  const ReferenceIndex &refs = ix.references;
  CHECK((symbolNamed(refs, "Box").flags & kSymExplicitInstantiation) != 0);
  CHECK((symbolNamed(refs, "ident").flags & kSymExplicitInstantiation) != 0);
  CHECK((symbolNamed(refs, "Unused").flags & kSymExplicitInstantiation) == 0);
}

TEST_CASE("references: explicit specializations are symbols of their own",
          "[references]") {
  Project p;
  auto ix = bakeOne(p, R"cpp(
template <class T> int pick(T) { return 0; }
template <> int pick<int>(int) { return 1; }
template <class T> T zero = T();
template <> long zero<long> = 7;
template <class T> struct Slot { T v; };
template <> struct Slot<bool> { virtual ~Slot(); };
Slot<bool>::~Slot() {}
struct Host { template <class T> int get(T); };
template <> int Host::get<int>(int) { return 2; }
int plain() { return pick(1) + int(zero<long>); }
)cpp");
  const ReferenceIndex &refs = ix.references;
  // The specialization and the primary template are two symbols; only the
  // specialization carries the flag, and it is a strong definition.
  unsigned flagged = 0, total = 0;
  for (const std::string &usr : refs.usrsNamed("pick")) {
    auto info = refs.symbol(usr);
    REQUIRE(info);
    ++total;
    if (info->flags & kSymSpecialization) {
      ++flagged;
      CHECK(info->linkage == LinkageClass::External);
    } else {
      CHECK(info->linkage == LinkageClass::TemplateInstantiation);
    }
  }
  CHECK(total == 2);
  CHECK(flagged == 1);

  flagged = 0;
  for (const std::string &usr : refs.usrsNamed("zero"))
    if (refs.symbol(usr)->flags & kSymSpecialization)
      ++flagged;
  CHECK(flagged == 1);

  flagged = 0;
  for (const std::string &usr : refs.usrsNamed("Slot"))
    if (refs.symbol(usr)->flags & kSymSpecialization)
      ++flagged;
  CHECK(flagged == 1);

  flagged = 0;
  for (const std::string &usr : refs.usrsNamed("Host::get"))
    if (refs.symbol(usr)->flags & kSymSpecialization)
      ++flagged;
  CHECK(flagged == 1);

  CHECK((symbolNamed(refs, "plain").flags & kSymSpecialization) == 0);
  const std::vector<const char *> names =
      symbolAttributeNames(kSymLambda | kSymSpecialization);
  REQUIRE(names.size() == 2);
  CHECK(std::string(names[1]) == "explicit_specialization");
}

TEST_CASE("references: system header code is not recorded but its strong "
          "symbols are",
          "[references]") {
  Project p;
  llvm::sys::fs::create_directory(p.path("sys"));
  p.header("sys/sys.h", R"cpp(#pragma once
#pragma GCC system_header
extern "C" void *sys_alloc(unsigned long);
extern int sys_errno;
inline int sys_inline() { return sys_errno; }
template <class T> T sys_max(T a, T b) { return a < b ? b : a; }
)cpp");
  auto ix = bakeOne(p, R"cpp(#include "sys/sys.h"
void *grab() { return sys_alloc(8); }
int probe() { return sys_inline() + sys_max(1, 2) + sys_errno; }
)cpp");
  const ReferenceIndex &refs = ix.references;
  CHECK(roles(refs, "grab", "sys_alloc") == "call");
  CHECK(roles(refs, "probe", "sys_errno") == "read");
  // Inline and template code of a system library says nothing about the
  // project's link dependencies.
  CHECK_FALSE(hasSymbol(refs, "sys_inline"));
  CHECK_FALSE(hasSymbol(refs, "sys_max"));
  SymbolInfo alloc = symbolNamed(refs, "sys_alloc");
  CHECK(alloc.linkage == LinkageClass::External);
  CHECK(alloc.definingTuCount == 0);
  REQUIRE(alloc.declarations.size() == 1);
  CHECK(alloc.declarations[0].inHeader);
}

// ============================================================================
// Per-TU bookkeeping
// ============================================================================

namespace {

struct TwoTus {
  Project p;
  std::string a;
  std::string b;
  TwoTus() {
    p.header("shared.h", R"cpp(#pragma once
extern int total;
void log(int);
inline void account(int n) { total += n; log(total); }
struct Shape { virtual ~Shape(); virtual int area() const; };
)cpp");
    a = p.tu("a.cpp", R"cpp(#include "shared.h"
int total = 0;
Shape::~Shape() {}
static int onlyA() { return total; }
int fromA() { account(1); return onlyA(); }
)cpp");
    b = p.tu("b.cpp", R"cpp(#include "shared.h"
void log(int) {}
int Shape::area() const { return 0; }
int fromB(Shape *s) { account(2); return s->area(); }
void (*hook)(int) = log;
)cpp");
  }
};

} // namespace

TEST_CASE("references: removeTU releases exactly one TU's contribution",
          "[references]") {
  TwoTus t;
  auto ix = t.p.bake();
  ReferenceIndex &refs = ix.references;
  const std::string before = describe(refs);

  CHECK(reference(refs, "account", "total", RefRole::Write).tuCount == 2);
  CHECK(roles(refs, "fromA", "onlyA") == "call");
  CHECK(symbolNamed(refs, "Shape").vtableHomeTu == t.a);

  CHECK(refs.removeTU(t.a) > 0);
  // What only a.cpp said is gone.
  CHECK(roles(refs, "fromA", "onlyA").empty());
  CHECK_FALSE(hasSymbol(refs, "onlyA"));
  CHECK_FALSE(hasSymbol(refs, "fromA"));
  // What both said stays, counted once.
  ReferenceInfo write = reference(refs, "account", "total", RefRole::Write);
  CHECK(write.tuCount == 1);
  CHECK(write.firstTu == t.b);
  // total is still known (b.cpp references it) but no longer defined.
  SymbolInfo total = symbolNamed(refs, "total");
  CHECK(total.definingTuCount == 0);
  // Nothing b.cpp says involves the class itself.
  CHECK_FALSE(hasSymbol(refs, "Shape"));
  CHECK(roles(refs, "hook", "log") == "address_of");
  // Removing an unknown TU is a no-op.
  const std::string afterRemove = describe(refs);
  CHECK(refs.removeTU(t.p.path("nope.cpp")) == 0);
  CHECK(describe(refs) == afterRemove);

  // Re-absorbing the TU restores the index exactly.
  auto again = t.p.bake({t.a});
  refs.absorb(again.references);
  CHECK(describe(refs) == before);

  // And compaction changes nothing observable.
  refs.removeTU(t.b);
  refs.compact();
  auto bAgain = t.p.bake({t.b});
  refs.absorb(bAgain.references);
  CHECK(describe(refs) == before);
}

TEST_CASE("references: the merged view does not depend on bake order or "
          "threads",
          "[references]") {
  TwoTus t;
  auto forward = t.p.bake({t.a, t.b});
  auto reversed = t.p.bake({t.b, t.a});
  auto threaded = t.p.bake({t.a, t.b}, /*threads=*/2);
  const std::string want = describe(forward.references);
  CHECK_FALSE(want.empty());
  CHECK(describe(reversed.references) == want);
  CHECK(describe(threaded.references) == want);

  // Shards absorbed in either order agree as well.
  auto onlyA = t.p.bake({t.a});
  auto onlyB = t.p.bake({t.b});
  ReferenceIndex ab;
  ab.absorb(onlyA.references);
  ab.absorb(onlyB.references);
  ReferenceIndex ba;
  ba.absorb(onlyB.references);
  ba.absorb(onlyA.references);
  CHECK(describe(ab) == want);
  CHECK(describe(ba) == want);
}

TEST_CASE("references: a bake asked not to record them leaves the index "
          "empty",
          "[references]") {
  TwoTus t;
  auto off = t.p.bake({t.a, t.b}, 1, /*references=*/false);
  CHECK(off.references.empty());
  CHECK(off.references.referenceCount() == 0);
  CHECK(off.graph.nodeCount() > 0);
}

TEST_CASE("references: hand-built facts merge by content", "[references]") {
  ReferenceIndex ix;
  SymbolFact f;
  f.usr = "c:@F@f#";
  f.name = "f";
  f.file = "/src/f.cpp";
  f.line = 3;
  f.flags = kSymDefinition;
  f.size = 4;
  SymbolFact g = f;
  g.usr = "c:@g";
  g.name = "g";
  g.kind = SymbolKind::Variable;
  g.line = 1;
  ix.addSymbol(f, "/src/f.cpp");
  ix.addSymbol(g, "/src/f.cpp");
  ix.addSymbol(g, "/src/other.cpp"); // identical record from a second TU

  ReferenceFact r;
  r.referrerUsr = f.usr;
  r.targetUsr = g.usr;
  r.role = RefRole::Write;
  r.count = 2;
  r.file = "/src/f.cpp";
  r.line = 5;
  r.col = 3;
  ix.addReference(r, "/src/f.cpp");
  ix.addReference(r, "/src/f.cpp"); // the same TU again: still one
  CHECK(ix.symbolRecordCount() == 2);
  CHECK(ix.referenceCount() == 1);

  auto to = ix.referencesTo(g.usr);
  REQUIRE(to.size() == 1);
  CHECK(to[0].referrerName == "f");
  CHECK(to[0].role == RefRole::Write);
  CHECK(to[0].count == 2);
  CHECK(to[0].tuCount == 1);
  CHECK(ix.referencesFrom(g.usr).empty());
  CHECK(ix.referencesTo("c:@missing").empty());
  CHECK_FALSE(ix.symbol("c:@missing").has_value());

  auto info = ix.symbol(g.usr);
  REQUIRE(info.has_value());
  REQUIRE(info->declarations.size() == 1);
  CHECK(info->declarations[0].tuCount == 2);
  CHECK(info->declarations[0].firstTu == "/src/f.cpp");

  CHECK(ix.removeTU("/src/f.cpp") == 2); // f's record and the reference
  CHECK(ix.referenceCount() == 0);
  CHECK(ix.symbolRecordCount() == 1);
  CHECK(ix.symbol(g.usr)->declarations[0].firstTu == "/src/other.cpp");

  CHECK(refRoleFromName("vtable_use") == RefRole::VtableUse);
  CHECK_FALSE(refRoleFromName("nope").has_value());
}

// ============================================================================
// Persistence (the snapshot's references section)
// ============================================================================

namespace {

std::string encoded(const ReferenceIndex &ix) {
  std::string bytes;
  ix.encode(bytes);
  return bytes;
}

bool decodes(const std::string &bytes, bool readOnly = false) {
  ReferenceIndex ix;
  return ix.decode(bytes.data(), bytes.data() + bytes.size(), readOnly);
}

} // namespace

TEST_CASE("references: an encoded index decodes to the same facts",
          "[references][snapshot]") {
  TwoTus t;
  auto ix = t.p.bake();
  const std::string want = describe(ix.references);
  const std::string bytes = encoded(ix.references);

  ReferenceIndex mutableCopy;
  REQUIRE(mutableCopy.decode(bytes.data(), bytes.data() + bytes.size(),
                             /*readOnly=*/false));
  CHECK(describe(mutableCopy) == want);
  CHECK(mutableCopy.symbolRecordCount() == ix.references.symbolRecordCount());
  CHECK(mutableCopy.referenceCount() == ix.references.referenceCount());
  CHECK(mutableCopy.symbolCount() == ix.references.symbolCount());
  CHECK(encoded(mutableCopy) == bytes);

  ReferenceIndex readOnlyCopy;
  REQUIRE(readOnlyCopy.decode(bytes.data(), bytes.data() + bytes.size(),
                              /*readOnly=*/true));
  CHECK(describe(readOnlyCopy) == want);
  CHECK(readOnlyCopy.referenceCount() == ix.references.referenceCount());
  CHECK(reference(readOnlyCopy, "account", "total", RefRole::Write).tuCount ==
        2);
  CHECK(symbolNamed(readOnlyCopy, "Shape").vtableHomeTu == t.a);

  // The decoded index keeps its per-TU bookkeeping: a refresh over it
  // matches one over the baked index.
  ix.references.removeTU(t.a);
  mutableCopy.removeTU(t.a);
  CHECK(describe(mutableCopy) == describe(ix.references));
  CHECK(encoded(mutableCopy) == encoded(ix.references));
  auto again = t.p.bake({t.a});
  mutableCopy.absorb(again.references);
  CHECK(describe(mutableCopy) == want);
  CHECK(encoded(mutableCopy) == bytes);

  // A decode into an index that already holds facts is refused.
  CHECK_FALSE(mutableCopy.decode(bytes.data(), bytes.data() + bytes.size(),
                                 false));
  CHECK(describe(mutableCopy) == want);
}

TEST_CASE("references: the encoding does not depend on bake or absorb order",
          "[references][snapshot]") {
  TwoTus t;
  const std::string want = encoded(t.p.bake({t.a, t.b}).references);
  CHECK(encoded(t.p.bake({t.b, t.a}).references) == want);
  CHECK(encoded(t.p.bake({t.a, t.b}, /*threads=*/2).references) == want);

  auto onlyA = t.p.bake({t.a});
  auto onlyB = t.p.bake({t.b});
  ReferenceIndex ba;
  ba.absorb(onlyB.references);
  ba.absorb(onlyA.references);
  CHECK(encoded(ba) == want);

  // Records a removed TU left behind (dead slots, unused strings) are not
  // written.
  ba.removeTU(t.b);
  ba.absorb(onlyB.references);
  CHECK(encoded(ba) == want);

  // An empty index round-trips too.
  const std::string none = encoded(ReferenceIndex());
  ReferenceIndex empty;
  REQUIRE(empty.decode(none.data(), none.data() + none.size(), false));
  CHECK(empty.empty());
  CHECK(encoded(empty) == none);
}

TEST_CASE("references: damaged encodings are refused", "[references][snapshot]") {
  TwoTus t;
  const std::string bytes = encoded(t.p.bake().references);
  REQUIRE(decodes(bytes));

  // Every truncation, and trailing bytes.
  for (size_t n = 0; n < bytes.size(); ++n) {
    INFO("truncated to " << n);
    REQUIRE_FALSE(decodes(bytes.substr(0, n)));
    REQUIRE_FALSE(decodes(bytes.substr(0, n), /*readOnly=*/true));
  }
  CHECK_FALSE(decodes(bytes + '\0'));
  CHECK_FALSE(decodes(bytes + '\0', /*readOnly=*/true));

  // Any single damaged byte either fails or yields an index that is
  // self-consistent: it encodes, and the encoding decodes again.
  size_t rejected = 0;
  for (size_t i = 0; i < bytes.size(); ++i) {
    for (unsigned char flip : {0x01, 0x80, 0xFF}) {
      std::string damaged = bytes;
      damaged[i] = static_cast<char>(damaged[i] ^ flip);
      ReferenceIndex ix;
      if (!ix.decode(damaged.data(), damaged.data() + damaged.size(), false)) {
        ++rejected;
        CHECK(ix.empty());
        continue;
      }
      INFO("byte " << i << " ^ " << unsigned(flip));
      REQUIRE(decodes(encoded(ix)));
      (void)describe(ix);
      ReferenceIndex readOnly;
      (void)readOnly.decode(damaged.data(), damaged.data() + damaged.size(),
                            true);
      (void)describe(readOnly);
    }
  }
  CHECK(rejected > bytes.size());
}

// ---- limits on what a load may allocate -------------------------------------

namespace {

void appendVarint(std::string &out, uint64_t v) {
  while (v >= 0x80) {
    out.push_back(static_cast<char>(v | 0x80));
    v >>= 7;
  }
  out.push_back(static_cast<char>(v));
}

void appendU64(std::string &out, uint64_t v) {
  for (unsigned i = 0; i < 8; ++i)
    out.push_back(static_cast<char>(v >> (8 * i)));
}

// A well-formed section holding only strings: `first` bytes of 'a', then
// count - 1 strings each repeating the whole previous one plus one byte.
// Its text grows with first * count, its bytes with first + count.
std::string prefixBomb(size_t first, size_t count) {
  std::string out;
  appendVarint(out, count);
  appendVarint(out, 0);
  appendVarint(out, first);
  out.append(first, 'a');
  for (size_t i = 1; i < count; ++i) {
    appendVarint(out, first + i - 1);
    appendVarint(out, 1);
    out.push_back('b');
  }
  appendVarint(out, 0); // symbol records
  appendVarint(out, 0); // reference records
  appendU64(out, 1);
  appendVarint(out, 0); // TUs
  return out;
}

} // namespace

TEST_CASE("references: a string table may not expand without bound",
          "[references][snapshot]") {
  // The same shape, small enough: accepted.
  CHECK(decodes(prefixBomb(4096, 16)));
  CHECK(decodes(prefixBomb(4096, 16), /*readOnly=*/true));
  // 20 KiB of bytes that spell 24 MiB of text: refused, in both modes.
  const std::string bomb = prefixBomb(4096, 4000);
  REQUIRE(bomb.size() < 24 * 1024);
  CHECK_FALSE(decodes(bomb));
  CHECK_FALSE(decodes(bomb, /*readOnly=*/true));

  // The writer keeps to the same limit: USRs sharing a long prefix are
  // stored whole where sharing would break it, and still round-trip.
  ReferenceIndex ix;
  const std::string prefix(6000, 'p');
  for (unsigned i = 0; i < 3000; ++i) {
    SymbolFact f;
    f.usr = prefix + std::to_string(100000 + i);
    f.name = "f";
    f.file = "/src/f.cpp";
    f.line = i + 1;
    ix.addSymbol(f, "/src/f.cpp");
  }
  const std::string bytes = encoded(ix);
  CHECK(bytes.size() > 3000u * 6000u / 80u); // not fully front-coded
  CHECK(bytes.size() < 3000u * 6000u / 32u); // but mostly
  ReferenceIndex back;
  REQUIRE(back.decode(bytes.data(), bytes.data() + bytes.size(), false));
  CHECK(describe(back) == describe(ix));
  CHECK(encoded(back) == bytes);
}

TEST_CASE("references: a contributor run holds at most 64 records",
          "[references][snapshot]") {
  // One TU contributing 65 adjacent records: written as runs of 64 and 1.
  ReferenceIndex ix;
  for (unsigned i = 0; i < 65; ++i) {
    SymbolFact f;
    f.usr = "c:@F@f" + std::to_string(100 + i) + "#";
    f.name = "f";
    f.file = "/src/f.cpp";
    f.line = i + 1;
    ix.addSymbol(f, "/src/f.cpp");
  }
  const std::string bytes = encoded(ix);
  ReferenceIndex back;
  REQUIRE(back.decode(bytes.data(), bytes.data() + bytes.size(), false));
  CHECK(describe(back) == describe(ix));
  CHECK(encoded(back) == bytes);

  // The lists end the section: u64 length, TU count, the TU, its symbol
  // runs (2: gap 0 length 64, gap 0 length 1), its reference runs (0).
  const std::string tail = {8, 0, 0, 0, 0, 0, 0, 0, 1, bytes[bytes.size() - 7],
                            2, 0, 63, 0, 0, 0};
  REQUIRE(bytes.size() > tail.size());
  REQUIRE(bytes.substr(bytes.size() - tail.size()) == tail);
  // The same 65 records as one run: refused.
  std::string oneRun = bytes.substr(0, bytes.size() - tail.size());
  oneRun += std::string{6, 0, 0, 0, 0, 0, 0, 0, 1, tail[9], 1, 0, 64, 0};
  CHECK_FALSE(decodes(oneRun));

  // A long run in a real merge: 300 records from each of two TUs.
  ReferenceIndex big;
  for (const char *tu : {"/src/a.cpp", "/src/b.cpp"})
    for (unsigned i = 0; i < 300; ++i) {
      SymbolFact f;
      f.usr = "c:@F@g" + std::to_string(1000 + i) + "#";
      f.name = "g";
      f.file = "/src/g.h";
      f.line = i + 1;
      big.addSymbol(f, tu);
    }
  const std::string bigBytes = encoded(big);
  ReferenceIndex bigBack;
  REQUIRE(bigBack.decode(bigBytes.data(), bigBytes.data() + bigBytes.size(),
                         false));
  CHECK(describe(bigBack) == describe(big));
  CHECK(bigBack.removeTU("/src/a.cpp") == 0); // b.cpp still holds them all
  CHECK(bigBack.removeTU("/src/b.cpp") == 300);
  CHECK(bigBack.empty());
}

// A read-only load never reads the contributor lists (nothing it answers
// needs them), so it does not check them against the records either: bytes
// a mutable load refuses can load read-only. The snapshot's per-section
// checksum is what stands between a damaged file and such a load.
TEST_CASE("references: a read-only load does not check the contributor lists",
          "[references][snapshot]") {
  TwoTus t;
  auto ix = t.p.bake();
  std::string damaged = encoded(ix.references);
  damaged.back() = static_cast<char>(damaged.back() ^ 0x01);
  CHECK_FALSE(decodes(damaged));

  ReferenceIndex readOnly;
  REQUIRE(readOnly.decode(damaged.data(), damaged.data() + damaged.size(),
                          /*readOnly=*/true));
  CHECK(readOnly.readOnly());
  CHECK(describe(readOnly) == describe(ix.references));
  CHECK_FALSE(ix.references.readOnly());
}
