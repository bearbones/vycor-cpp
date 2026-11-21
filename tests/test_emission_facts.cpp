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

// test_emission_facts.cpp — the bake's emission facts
// (docs/symbol-references.md "Emission facts"): which vague-linkage and
// header-internal function bodies each TU's code generation emits, their
// persistence and per-TU bookkeeping, and the redundant-instantiation
// metric over them (docs/metrics/redundant-instantiation.md).

#include "vycor/callgraph/ControlFlowIndex.h"
#include "vycor/callgraph/ControlFlowOracle.h"
#include "vycor/callgraph/ReferenceIndex.h"
#include "vycor/metrics/Metric.h"
#include "vycor/metrics/MetricSet.h"
#include "vycor/query/Tools.h"

#include <catch2/catch_test_macros.hpp>
#include <clang/Tooling/CompilationDatabase.h>

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace vycor;

namespace {

struct Project {
  std::string dir;
  std::vector<std::string> tus;
  std::vector<std::string> flags = {"-std=c++17"};

  Project() {
    llvm::SmallString<128> tmp;
    REQUIRE_FALSE(llvm::sys::fs::createUniqueDirectory("vycor_emit", tmp));
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

struct Emitted {
  std::string symbol; // the pattern's or the function's USR, "" if none
  std::string file;
  uint32_t line = 0;
  uint32_t size = 0;
  uint8_t flags = 0;
  std::vector<std::string> tus;
};

// Mangled name -> what the index says about it. One record per name in
// these fixtures.
std::map<std::string, Emitted> emissions(const ReferenceIndex &ix) {
  std::map<std::string, Emitted> out;
  ix.forEachEmissionRecord([&](const ReferenceIndex::EmissionRecord &rec,
                               llvm::ArrayRef<ReferenceIndex::Id> tus) {
    Emitted e;
    if (rec.symbol != ReferenceIndex::kNoId)
      e.symbol = ix.textOf(rec.symbol);
    e.file = ix.textOf(rec.file);
    e.line = rec.line;
    e.size = rec.size;
    e.flags = rec.flags;
    for (ReferenceIndex::Id tu : tus)
      e.tus.push_back(ix.textOf(tu));
    const std::string name = ix.textOf(rec.name);
    INFO("emission " << name);
    CHECK(out.count(name) == 0);
    out[name] = e;
  });
  return out;
}

// Everything the emission facts say, as text: the persistence tests'
// observable.
std::string describe(const ReferenceIndex &ix) {
  std::string out;
  for (const auto &[name, e] : emissions(ix)) {
    out += "E " + name + " " + e.symbol + " " + e.file + ":" +
           std::to_string(e.line) + " z" + std::to_string(e.size) + " f" +
           std::to_string(e.flags);
    for (const std::string &tu : e.tus)
      out += " " + tu;
    out += "\n";
  }
  for (ReferenceIndex::Id tu : ix.emissionTus())
    out += "T " + ix.textOf(tu) + "\n";
  return out;
}

const char *kHeader = R"cpp(
inline int used(int x) { return x + 1; }
inline int unused(int x) { return x - 1; }
template <class T> T twice(T x) { return x + x; }
static int helper(int x) { return x * 3; }
static int idle(int x) { return x * 5; }
int dead(int);
inline int folded(int x) {
  if (false)
    return dead(x);
  if constexpr (sizeof(int) == 0)
    return dead(x + 1);
  return x;
}
struct Box {
  int get() const { return v; }
  int v = 0;
};
)cpp";

struct TwoTus {
  Project p;
  std::string a, b;
  TwoTus() {
    p.header("h.h", kHeader);
    a = p.tu("a.cpp", R"cpp(
#include "h.h"
int fa(int x) { return used(x) + twice(x) + helper(x) + folded(x); }
)cpp");
    b = p.tu("b.cpp", R"cpp(
#include "h.h"
int fb(int x) { Box box; return used(x) + twice(x) + int(twice(1.0)) +
                                helper(x) + box.get(); }
)cpp");
  }
};

/// The metrics tool over baked indexes, as an adapter would run it.
struct Surface {
  ControlFlowOracle oracle;
  std::vector<std::string> entryPoints{"main"};
  std::vector<ToolEntry> tools = getRegisteredTools();
  ToolContext ctx;

  explicit Surface(const BakedIndexes &baked)
      : oracle(baked.graph, baked.cfIndex),
        ctx{baked.graph, oracle, baked.cfIndex, entryPoints} {
    ctx.references = &baked.references;
  }
  llvm::json::Object
  run(std::initializer_list<llvm::json::Object::KV> kvs) {
    for (const auto &t : tools)
      if (t.name == "metrics") {
        llvm::json::Value v = runTool(t, llvm::json::Object(kvs), ctx);
        const auto *obj = v.getAsObject();
        REQUIRE(obj != nullptr);
        return *obj;
      }
    FAIL("no metrics tool registered");
    return {};
  }
};

std::string serialized(const llvm::json::Value &v) {
  std::string out;
  llvm::raw_string_ostream os(out);
  os << v;
  return out;
}

} // namespace

// ============================================================================
// What a TU emits
// ============================================================================

TEST_CASE("emission facts: what each TU's code generation emits",
          "[emission]") {
  TwoTus t;
  auto ix = t.p.bake();
  REQUIRE(ix.references.hasEmissionFacts());
  const auto e = emissions(ix.references);

  // An inline function emitted code calls: emitted by both TUs, at its
  // definition, with a size.
  REQUIRE(e.count("_Z4usedi"));
  CHECK(e.at("_Z4usedi").tus == std::vector<std::string>{t.a, t.b});
  CHECK(e.at("_Z4usedi").file == t.p.path("h.h"));
  CHECK(e.at("_Z4usedi").line == 2);
  CHECK(e.at("_Z4usedi").size > 0);
  CHECK(e.at("_Z4usedi").flags == 0);
  CHECK_FALSE(e.at("_Z4usedi").symbol.empty());

  // One nobody calls is no code anywhere.
  CHECK(e.count("_Z6unusedi") == 0);
  CHECK(e.count("_ZL4idlei") == 0);

  // Instantiations are records of their own, attributed to the pattern.
  REQUIRE(e.count("_Z5twiceIiET_S0_"));
  REQUIRE(e.count("_Z5twiceIdET_S0_"));
  CHECK(e.at("_Z5twiceIiET_S0_").tus == std::vector<std::string>{t.a, t.b});
  CHECK(e.at("_Z5twiceIdET_S0_").tus == std::vector<std::string>{t.b});
  CHECK(e.at("_Z5twiceIiET_S0_").symbol ==
        e.at("_Z5twiceIdET_S0_").symbol);
  CHECK(e.at("_Z5twiceIiET_S0_").line == 4);

  // A header's internal function: one copy per TU, flagged internal.
  REQUIRE(e.count("_ZL6helperi"));
  CHECK(e.at("_ZL6helperi").flags == kEmitInternal);
  CHECK(e.at("_ZL6helperi").tus.size() == 2);

  // An in-class member function, and the code a folded condition
  // discards: `dead` is never named by emitted code (and has no body).
  CHECK(e.count("_ZNK3Box3getEv"));
  CHECK(e.at("_ZNK3Box3getEv").tus == std::vector<std::string>{t.b});
  REQUIRE(e.count("_Z6foldedi"));
  CHECK(e.count("_Z4deadi") == 0);

  // Strong definitions of the TU's own source are not vague linkage.
  CHECK(e.count("_Z2fai") == 0);
  CHECK(e.count("_Z2fbi") == 0);

  CHECK(ix.references.emissionTus().size() == 2);
}

TEST_CASE("emission facts: constant data emits the functions it points to",
          "[emission]") {
  Project p;
  p.header("cb.h", R"cpp(
inline void viaTable() {}
inline void viaLocal() {}
inline void viaMember() {}
inline void viaUnused() {}
inline void viaLiteral() {}
struct Ops { void (*run)(); int id; };
inline void (*unusedTable[])() = {viaUnused};
)cpp");
  p.tu("t.cpp", R"cpp(
#include "cb.h"
void (*table[])() = {viaTable};
Ops ops = {viaMember, 7};
void callLocal() {
  constexpr void (*local)() = &viaLocal;
  local();
  static const Ops literal = {viaLiteral, 1};
  literal.run();
}
)cpp");
  auto ix = p.bake();
  const auto e = emissions(ix.references);
  CHECK(e.count("_Z8viaTablev"));
  CHECK(e.count("_Z9viaMemberv"));
  CHECK(e.count("_Z8viaLocalv"));
  CHECK(e.count("_Z10viaLiteralv"));
  // An inline variable nothing references is not emitted, nor what it
  // points to.
  CHECK(e.count("_Z9viaUnusedv") == 0);
}

TEST_CASE("emission facts: a C table of function pointers emits its "
          "header's static inline functions",
          "[emission]") {
  Project p;
  p.flags = {"-std=c11"};
  p.header("ops.h", R"c(
static inline int hop(void) { return 1; }
static inline int skip(void) { return 2; }
static inline int spare(void) { return 3; }
struct ops { int (*run)(void); int id; };
struct pair { struct ops first; struct ops second; };
)c");
  p.tu("t.c", R"c(
#include "ops.h"
struct ops g = { .run = hop, .id = 1 };
struct pair table[] = { { { skip, 2 }, { 0, 3 } } };
)c");
  auto ix = p.bake();
  const auto e = emissions(ix.references);
  // C names are not mangled; a header's static inline function is a
  // private copy per TU.
  REQUIRE(e.count("hop"));
  CHECK(e.at("hop").flags == kEmitInternal);
  REQUIRE(e.count("skip"));
  CHECK(e.count("spare") == 0);
}

TEST_CASE("emission facts: an explicit instantiation declaration leaves an "
          "available_externally body",
          "[emission]") {
  Project p;
  p.header("x.h", R"cpp(
template <class T> struct Holder {
  T get() const { return v; }
  T v{};
};
extern template struct Holder<int>;
)cpp");
  p.tu("u.cpp", R"cpp(
#include "x.h"
int use() { Holder<int> h; Holder<long> l; return h.get() + int(l.get()); }
)cpp");
  auto ix = p.bake();
  const auto e = emissions(ix.references);
  REQUIRE(e.count("_ZNK6HolderIiE3getEv"));
  CHECK(e.at("_ZNK6HolderIiE3getEv").flags == kEmitAvailableExternally);
  REQUIRE(e.count("_ZNK6HolderIlE3getEv"));
  CHECK(e.at("_ZNK6HolderIlE3getEv").flags == 0);
}

TEST_CASE("emission facts: system headers and a bake without references "
          "record none",
          "[emission]") {
  Project p;
  p.tu("s.cpp", R"cpp(
#include <vector>
int sum() { std::vector<int> v{1, 2}; return v[0] + v[1]; }
)cpp");
  auto ix = p.bake();
  REQUIRE(ix.references.hasEmissionFacts());
  for (const auto &[name, e] : emissions(ix.references))
    CHECK(e.file.find("/usr/") == std::string::npos);

  auto off = p.bake(p.tus, 1, /*references=*/false);
  CHECK_FALSE(off.references.hasEmissionFacts());
  CHECK(off.references.emissionCount() == 0);
}

// ============================================================================
// Persistence and per-TU bookkeeping
// ============================================================================

namespace {

std::string encoded(const ReferenceIndex &ix) {
  std::string bytes;
  ix.encode(bytes);
  return bytes;
}

} // namespace

TEST_CASE("emission facts: encode, decode, remove and absorb are exact",
          "[emission][snapshot]") {
  TwoTus t;
  auto ix = t.p.bake();
  const std::string want = describe(ix.references);
  REQUIRE_FALSE(want.empty());
  const std::string bytes = encoded(ix.references);

  ReferenceIndex mutableCopy;
  REQUIRE(mutableCopy.decode(bytes.data(), bytes.data() + bytes.size(),
                             /*readOnly=*/false));
  CHECK(describe(mutableCopy) == want);
  CHECK(encoded(mutableCopy) == bytes);
  ReferenceIndex readOnlyCopy;
  REQUIRE(readOnlyCopy.decode(bytes.data(), bytes.data() + bytes.size(),
                              /*readOnly=*/true));
  CHECK(describe(readOnlyCopy) == want);
  CHECK(readOnlyCopy.hasEmissionFacts());

  // Removing a TU drops what only it emitted and its name from what both
  // emitted; absorbing it again restores the index byte for byte.
  mutableCopy.removeTU(t.b);
  auto after = emissions(mutableCopy);
  CHECK(after.count("_Z5twiceIdET_S0_") == 0);
  REQUIRE(after.count("_Z4usedi"));
  CHECK(after.at("_Z4usedi").tus == std::vector<std::string>{t.a});
  CHECK(mutableCopy.emissionTus().size() == 1);
  auto again = t.p.bake({t.b});
  mutableCopy.absorb(again.references);
  CHECK(describe(mutableCopy) == want);
  CHECK(encoded(mutableCopy) == bytes);

  mutableCopy.removeTU(t.a);
  mutableCopy.compact();
  auto aAgain = t.p.bake({t.a});
  mutableCopy.absorb(aAgain.references);
  CHECK(encoded(mutableCopy) == bytes);

  // Truncated anywhere, the encoding is refused rather than misread.
  for (size_t cut : {bytes.size() - 1, bytes.size() / 2}) {
    ReferenceIndex broken;
    CHECK_FALSE(broken.decode(bytes.data(), bytes.data() + cut, false));
  }
}

TEST_CASE("emission facts: the encoding does not depend on bake order",
          "[emission][snapshot]") {
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
}

TEST_CASE("emission facts: a TU recorded with none still counts as recorded",
          "[emission]") {
  ReferenceIndex ix;
  const auto tu = ix.internText("/src/empty.cpp");
  CHECK_FALSE(ix.hasEmissionFacts());
  ix.addTuEmissions(tu, {});
  CHECK(ix.hasEmissionFacts());
  CHECK(ix.emissionCount() == 0);
  REQUIRE(ix.emissionTus().size() == 1);
  CHECK(ix.textOf(ix.emissionTus()[0]) == "/src/empty.cpp");

  std::string bytes;
  ix.encode(bytes);
  ReferenceIndex copy;
  REQUIRE(copy.decode(bytes.data(), bytes.data() + bytes.size(), true));
  CHECK(copy.hasEmissionFacts());
  // Not named: its path would read as a TU with reference facts.
  CHECK(copy.emissionTus().empty());
  CHECK_FALSE(copy.strings().find("/src/empty.cpp").has_value());
  ReferenceIndex merged;
  ReferenceIndex mutableCopy;
  REQUIRE(mutableCopy.decode(bytes.data(), bytes.data() + bytes.size(),
                             false));
  merged.absorb(mutableCopy);
  CHECK(merged.hasEmissionFacts());
}

TEST_CASE("emission facts: a TU with no facts at all leaves the string table "
          "as without them",
          "[emission]") {
  // The dependency audit reads a TU path in the string table as a TU with
  // reference facts; recording emission facts must not change that.
  auto encodeWith = [](bool emissions) {
    ReferenceIndex ix;
    SymbolFact f;
    f.usr = "c:@F@f#";
    f.name = "f";
    f.file = "/src/a.cpp";
    f.line = 1;
    ix.addSymbol(f, "/src/a.cpp");
    if (emissions) {
      ix.addTuEmissions(ix.internText("/src/a.cpp"), {});
      ix.addTuEmissions(ix.internText("/src/empty.cpp"), {});
    }
    std::string bytes;
    ix.encode(bytes);
    ReferenceIndex copy;
    REQUIRE(copy.decode(bytes.data(), bytes.data() + bytes.size(), true));
    return copy.strings().size();
  };
  CHECK(encodeWith(true) == encodeWith(false));
}

// ============================================================================
// redundant-instantiation
// ============================================================================

TEST_CASE("redundant-instantiation is a built-in metric", "[emission][metrics]") {
  bool found = false;
  for (const Metric *m : allMetrics())
    if (m->name() == "redundant-instantiation") {
      found = true;
      CHECK(m->unit() == "statements");
    }
  CHECK(found);
}

TEST_CASE("redundant-instantiation: copies times statements per function "
          "two or more TUs emit",
          "[emission][metrics]") {
  TwoTus t;
  auto ix = t.p.bake();
  const auto e = emissions(ix.references);
  Surface s(ix);
  auto payload = s.run({{"metrics", "redundant-instantiation"}});
  REQUIRE(payload.getString("status").value_or("") == "ok");
  const auto *records = payload.getArray("records");
  REQUIRE(records != nullptr);

  std::map<std::string, const llvm::json::Object *> byUsr;
  for (const auto &r : *records) {
    const auto *obj = r.getAsObject();
    REQUIRE(obj);
    const auto *entity = obj->getObject("entity");
    REQUIRE(entity);
    byUsr[entity->getString("usr").value_or("").str()] = obj;
  }
  // Emitted once: no redundancy, no record.
  CHECK(byUsr.count("_Z5twiceIdET_S0_") == 0);
  CHECK(byUsr.count("_ZNK3Box3getEv") == 0);
  REQUIRE(byUsr.count("_Z4usedi"));
  const auto *used = byUsr.at("_Z4usedi");
  CHECK(used->getObject("entity")->getString("name").value_or("") ==
        "used(int)");
  CHECK(used->getObject("entity")->getString("kind").value_or("") ==
        "function");
  const auto *components = used->getObject("components");
  REQUIRE(components);
  CHECK(components->getInteger("copies").value_or(0) == 2);
  CHECK(components->getInteger("statements").value_or(0) ==
        e.at("_Z4usedi").size);
  CHECK(used->getInteger("value").value_or(0) ==
        2 * static_cast<int64_t>(e.at("_Z4usedi").size));
  REQUIRE(byUsr.count("_ZL6helperi"));
  CHECK(byUsr.at("_ZL6helperi")
            ->getObject("components")
            ->getInteger("internal")
            .value_or(0) == 1);

  // The template roll-up groups the instantiations of one pattern.
  auto rolled = s.run({{"metrics", "redundant-instantiation"},
                       {"rollup", "template"}});
  REQUIRE(rolled.getString("status").value_or("") == "ok");
  bool sawTwice = false;
  for (const auto &r : *rolled.getArray("records")) {
    const auto *entity = r.getAsObject()->getObject("entity");
    CHECK(entity->getString("kind").value_or("") == "template");
    if (entity->getString("name").value_or("") == "twice") {
      sawTwice = true;
      CHECK(entity->getString("usr").value_or("") ==
            e.at("_Z5twiceIiET_S0_").symbol);
    }
  }
  CHECK(sawTwice);

  // The TU roll-up counts each record in every TU that emits it.
  auto byTu = s.run({{"metrics", "redundant-instantiation"},
                     {"rollup", "tu"}});
  REQUIRE(byTu.getArray("records")->size() == 2);
}

TEST_CASE("redundant-instantiation: two headers' internal namesakes are "
          "two functions",
          "[emission][metrics]") {
  Project p;
  p.header("one.h", "static int helper(int x) { return x * 3; }\n");
  p.header("two.h", "static int helper(int x) { return x + x * x; }\n");
  p.tu("a1.cpp", "#include \"one.h\"\nint a1(int x) { return helper(x); }\n");
  p.tu("a2.cpp", "#include \"one.h\"\nint a2(int x) { return helper(x); }\n");
  p.tu("b1.cpp", "#include \"two.h\"\nint b1(int x) { return helper(x); }\n");
  auto ix = p.bake();
  Surface s(ix);
  auto payload = s.run({{"metrics", "redundant-instantiation"}});
  REQUIRE(payload.getString("status").value_or("") == "ok");
  const auto *records = payload.getArray("records");
  REQUIRE(records != nullptr);
  // one.h's helper has two copies; two.h's has one, so no record, and
  // neither adds to the other.
  REQUIRE(records->size() == 1);
  const auto *only = (*records)[0].getAsObject();
  CHECK(only->getObject("entity")->getString("usr").value_or("") ==
        "_ZL6helperi");
  CHECK(only->getObject("entity")->getString("file").value_or("") ==
        p.path("one.h"));
  CHECK(only->getObject("components")->getInteger("copies").value_or(0) == 2);
}

TEST_CASE("redundant-instantiation: deterministic across bake order and "
          "threads",
          "[emission][metrics]") {
  TwoTus t;
  auto forward = t.p.bake({t.a, t.b});
  auto reversed = t.p.bake({t.b, t.a}, /*threads=*/2);
  Surface f(forward), r(reversed);
  for (const char *level : {"symbol", "template", "tu", "file"}) {
    INFO(level);
    CHECK(serialized(llvm::json::Value(f.run(
              {{"metrics", "redundant-instantiation"}, {"rollup", level}}))) ==
          serialized(llvm::json::Value(r.run(
              {{"metrics", "redundant-instantiation"}, {"rollup", level}}))));
  }
}

TEST_CASE("redundant-instantiation is unavailable without emission facts",
          "[emission][metrics]") {
  TwoTus t;
  auto off = t.p.bake(t.p.tus, 1, /*references=*/false);
  Surface s(off);
  auto payload = s.run({{"metrics", "redundant-instantiation"}});
  CHECK(payload.getString("status").value_or("") == "unavailable");
}
