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

// The mapped control-flow index (format v12 loaded ReadOnly) against the
// resident one (the same file loaded Mutable): every query, every
// control-flow tool, the whole-index walks, the file's lifetime, and the
// index's behaviour over a damaged file.

#include "vycor/callgraph/CallGraphBuilder.h"
#include "vycor/callgraph/ControlFlowIndex.h"
#include "vycor/callgraph/ControlFlowOracle.h"
#include "vycor/callgraph/Snapshot.h"
#include "vycor/impact/SemanticDiff.h"
#include "vycor/query/Tools.h"

#include "SnapshotBytes.h"

#include "clang/Tooling/CompilationDatabase.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace vycor;

namespace {

const std::string kBase =
    std::string(PROJECT_SOURCE_DIR) + "/examples/deep_chains/";

std::vector<std::string> deepChainsFiles() {
  std::vector<std::string> files;
  for (const char *f :
       {"main.cpp", "pipeline.cpp", "stage1_ingest.cpp", "stage2_parse.cpp",
        "stage3_transform.cpp", "stage4_dispatch.cpp", "stage5_sink.cpp",
        "plugins.cpp", "workers.cpp", "tokenizer.cpp", "scheduler.cpp",
        "callbacks.cpp", "async_workers.cpp", "lambda_callbacks.cpp"})
    files.push_back(kBase + f);
  return files;
}

std::string tempPath(const char *tag) {
  llvm::SmallString<128> p;
  llvm::sys::fs::createUniquePath(std::string("vycor-mapped-") + tag +
                                      "-%%%%%%.vycs",
                                  p, /*MakeAbsolute=*/true);
  return p.str().str();
}

std::string readFile(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

void writeFile(const std::string &path, const std::string &bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

uint32_t u32At(const std::string &bytes, size_t at) {
  REQUIRE(at + 4 <= bytes.size());
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i)
    v |= static_cast<uint32_t>(static_cast<uint8_t>(bytes[at + i])) << (8 * i);
  return v;
}

uint64_t u64At(const std::string &bytes, size_t at) {
  REQUIRE(at + 8 <= bytes.size());
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i)
    v |= static_cast<uint64_t>(static_cast<uint8_t>(bytes[at + i])) << (8 * i);
  return v;
}

void putU32At(std::string &bytes, size_t at, uint32_t v) {
  REQUIRE(at + 4 <= bytes.size());
  for (int i = 0; i < 4; ++i)
    bytes[at + i] = static_cast<char>((v >> (8 * i)) & 0xff);
}

std::string serialized(const llvm::json::Value &v) {
  std::string s;
  llvm::raw_string_ostream os(s);
  os << v;
  os.flush();
  return s;
}

const ToolEntry &toolNamed(const std::vector<ToolEntry> &tools,
                           llvm::StringRef name) {
  for (const auto &t : tools)
    if (t.name == name)
      return t;
  FAIL("no tool named " << name.str());
  return tools.front();
}

// Every field of a context, as one comparable string.
std::string key(const CallSiteContext &c) {
  std::string k = c.callerName + "|" + c.calleeName + "|" + c.callSite + "|" +
                  c.callerUsr + "|" + c.calleeUsr + "|" + c.tuPath + "|" +
                  std::to_string(static_cast<int>(c.callerNoexcept)) + "|" +
                  (c.insideCatchBlock ? "catch" : "-") + "|";
  for (const auto &s : c.enclosingTryCatches) {
    k += "try(" + s.tryLocation + "," + s.enclosingFunction + "," +
         std::to_string(s.nestingDepth);
    for (const auto &h : s.handlers)
      k += ";" + h.caughtType + (h.isCatchAll ? "*" : "") +
           (h.rethrows ? "^" : "") + "@" + h.location + ":" + h.bodySummary;
    k += ")";
  }
  k += "|";
  for (const auto &g : c.enclosingGuards)
    k += "guard(" + g.conditionText + "," + g.location +
         (g.inTrueBranch ? ",T" : ",F") + (g.isAssertion ? ",A" : "") + ")";
  k += "|";
  for (const auto &l : c.liveRaiiLocals)
    k += "raii(" + l.typeName + "," + l.varName + "," + l.declLocation + "," +
         std::to_string(static_cast<int>(l.kind)) + ")";
  return k;
}

std::string key(const std::optional<CallSiteContext> &c) {
  return c ? key(*c) : std::string("<none>");
}

std::vector<std::string> keys(const std::vector<CallSiteContext> &v) {
  std::vector<std::string> out;
  for (const auto &c : v)
    out.push_back(key(c));
  return out;
}

// A saved bake, loaded both ways. Two of them: deep_chains (paths,
// callbacks, virtual dispatch) and the exception fixtures (try/catch
// scopes, catch bodies, guards, RAII locals, macro-shared call sites).
struct Fixture {
  std::string name;
  std::string path;
  SnapshotData eager;
  SnapshotData mapped;
  std::vector<std::string> sites, callerUsrs, callerNames, calleeUsrs,
      calleeNames, functions;

  Fixture(const char *tag, const std::vector<std::string> &files,
          const std::vector<std::string> &flags)
      : name(tag), path(tempPath(tag)) {
    clang::tooling::FixedCompilationDatabase compDb(".", flags);
    BakedIndexes baked = bakeIndexes(compDb, files, {}, 2);
    SnapshotMeta meta;
    REQUIRE(SnapshotIO::save(path, baked.graph, baked.cfIndex, meta,
                             baked.channels));
    auto e = SnapshotIO::load(path, nullptr, LoadMode::Mutable);
    auto m = SnapshotIO::load(path, nullptr, LoadMode::ReadOnly);
    REQUIRE(e);
    REQUIRE(m);
    eager = std::move(*e);
    mapped = std::move(*m);
    std::set<std::string> s, cu, cn, eu, en;
    eager.cfIndex.forEachContext([&](const CallSiteContext &c) {
      s.insert(c.callSite);
      cu.insert(c.callerUsr);
      cn.insert(c.callerName);
      eu.insert(c.calleeUsr);
      en.insert(c.calleeName);
    });
    sites.assign(s.begin(), s.end());
    callerUsrs.assign(cu.begin(), cu.end());
    callerNames.assign(cn.begin(), cn.end());
    calleeUsrs.assign(eu.begin(), eu.end());
    calleeNames.assign(en.begin(), en.end());
    for (const CallGraphNode *n : eager.graph.allNodes())
      functions.push_back(n->qualifiedName);
  }
  ~Fixture() { std::remove(path.c_str()); }

  static Fixture &deepChains() {
    static Fixture f("deep-chains", deepChainsFiles(),
                     {"-std=c++17", "-I" + kBase});
    return f;
  }
  static Fixture &exceptions() {
    const std::string root = std::string(PROJECT_SOURCE_DIR) + "/";
    const std::string precision = root + "examples/precision/";
    const std::string mixed =
        root + "corpus/cases/mixed_exception_protection/src/";
    static Fixture f("exceptions",
                     {root + "examples/exception_context/main.cpp",
                      precision + "macro_sites.cpp",
                      precision + "overloads.cpp", precision + "templates.cpp",
                      mixed + "main.cpp", mixed + "lib.cpp", mixed + "pad1.cpp",
                      mixed + "pad2.cpp"},
                     {"-std=c++17", "-I" + precision, "-I" + mixed});
    return f;
  }
  static std::vector<Fixture *> all() { return {&deepChains(), &exceptions()}; }
};

// Runs one tool over a loaded snapshot with its own oracle and cache.
std::string run(SnapshotData &snap, const std::vector<ToolEntry> &tools,
                llvm::StringRef tool, llvm::json::Object args) {
  ControlFlowOracle oracle(snap.graph, snap.cfIndex);
  std::vector<std::string> entryPoints{"main"};
  QueryCache cache;
  ToolContext ctx{snap.graph,  oracle,         snap.cfIndex,
                  entryPoints, &snap.channels, &cache};
  ctx.facts.freshness = IndexFreshness::Baked;
  return serialized(runTool(toolNamed(tools, tool), args, ctx));
}

// Where the v12 control-flow section and its record region sit in the
// file, derived from the header's section table and the resident index
// (docs/control-flow-access.md gives the layout).
struct Layout {
  size_t sectionStart, sectionEnd, count, stringCount, countAt, recordsAt,
      stringOffsetsAt, sortedIdsAt, byCallerAt, byCalleeAt,
      callerDisplayCountAt;
};

Layout layoutOf(const std::string &bytes, const ControlFlowIndex &eager) {
  Layout l{};
  // magic(4) version(4) summary(32) table count(4), then
  // {kind u8, offset u64, length u64, checksum u64} entries.
  REQUIRE(u32At(bytes, 4) == SnapshotIO::kFormatVersion);
  const uint32_t entries = u32At(bytes, 40);
  bool found = false;
  for (uint32_t i = 0; i < entries; ++i) {
    const size_t at = 44 + i * 25;
    if (static_cast<uint8_t>(bytes[at]) != 2)
      continue;
    l.sectionStart = static_cast<size_t>(u64At(bytes, at + 1));
    l.sectionEnd = l.sectionStart + static_cast<size_t>(u64At(bytes, at + 9));
    found = true;
  }
  REQUIRE(found);
  REQUIRE(l.sectionEnd <= bytes.size());
  l.stringCount = u32At(bytes, l.sectionStart + 4);
  l.count = eager.size();
  size_t callerDisplays = 0, calleeDisplays = 0;
  eager.forEachContext([&](const CallSiteContext &c) {
    if (c.callerName != c.callerUsr)
      ++callerDisplays;
    if (c.calleeName != c.calleeUsr)
      ++calleeDisplays;
  });
  const size_t arrays = 8 * l.stringCount + 8 * l.count + 8 +
                        4 * callerDisplays + 4 * calleeDisplays;
  l.stringOffsetsAt = l.sectionEnd - arrays;
  l.recordsAt = l.stringOffsetsAt - 38 * l.count;
  l.countAt = l.recordsAt - 4;
  l.sortedIdsAt = l.stringOffsetsAt + 4 * l.stringCount;
  l.byCallerAt = l.sortedIdsAt + 4 * l.stringCount;
  l.byCalleeAt = l.byCallerAt + 4 * l.count;
  l.callerDisplayCountAt = l.byCalleeAt + 4 * l.count;
  // The derived layout must agree with the file's own counts.
  REQUIRE(u32At(bytes, l.countAt) == l.count);
  REQUIRE(u32At(bytes, l.callerDisplayCountAt) == callerDisplays);
  return l;
}

// Every query over every name the fixture knows; the answers are not
// checked, the point is that a damaged file cannot make one crash.
void exerciseEverything(const Fixture &f, const ControlFlowIndex &cf) {
  for (const auto &s : f.sites) {
    (void)cf.contextAtSite(s);
    (void)cf.contextsAtSite(s);
    for (const auto &c : f.callerUsrs)
      (void)cf.contextAtSite(s, c);
  }
  for (const auto &n : f.calleeUsrs) {
    (void)cf.contextsForCallee(n);
    (void)cf.protectedCallsTo(n);
    (void)cf.unprotectedCallsTo(n);
  }
  for (const auto &n : f.calleeNames)
    (void)cf.contextsForCallee(n);
  for (const auto &n : f.callerUsrs) {
    (void)cf.contextsForCaller(n);
    (void)cf.callerNoexceptOf(n);
  }
  for (const auto &n : f.callerNames)
    (void)cf.contextsForCaller(n);
  size_t seen = 0;
  cf.forEachContext([&](const CallSiteContext &) { ++seen; });
  std::vector<ControlFlowIndex::ContextRecord> records;
  cf.forEachContextRecord(
      [&](const ControlFlowIndex::ContextRecord &r) { records.push_back(r); });
  for (const auto &r : records) {
    (void)cf.stringOf(r.callSite);
    (void)cf.contextOfShape(r.shape);
  }
  (void)ContextSignatures::build(cf);
}

} // namespace

TEST_CASE("a read-only load of a v12 snapshot maps the control flow",
          "[mapped][snapshot]") {
  for (Fixture *fp : Fixture::all()) {
    Fixture &f = *fp;
    INFO(f.name);
    CHECK(f.mapped.cfIndex.isMapped());
    CHECK_FALSE(f.eager.cfIndex.isMapped());
    CHECK(f.mapped.loaded == f.eager.loaded);
    REQUIRE(f.eager.cfIndex.size() > 0);
    CHECK(f.mapped.cfIndex.size() == f.eager.cfIndex.size());
    REQUIRE(f.sites.size() > 10);
  }
  // The exception fixture is there for what deep_chains lacks.
  const ControlFlowIndex &x = Fixture::exceptions().eager.cfIndex;
  size_t scoped = 0, guarded = 0, raii = 0, shared = 0;
  std::map<std::string, size_t> perSite;
  x.forEachContext([&](const CallSiteContext &c) {
    scoped += !c.enclosingTryCatches.empty();
    guarded += !c.enclosingGuards.empty();
    raii += !c.liveRaiiLocals.empty();
    ++perSite[c.callSite];
  });
  for (const auto &[site, n] : perSite)
    shared += n > 1;
  CHECK(scoped > 0);
  CHECK(guarded > 0);
  CHECK(raii > 0);
  CHECK(shared > 0);
}

TEST_CASE("mapped and resident indexes answer every query alike",
          "[mapped][parity]") {
  Fixture &f = *GENERATE(from_range(Fixture::all()));
  INFO(f.name);
  const ControlFlowIndex &e = f.eager.cfIndex;
  const ControlFlowIndex &m = f.mapped.cfIndex;

  SECTION("whole-index walks keep the stored order") {
    std::vector<std::string> ek, mk;
    e.forEachContext([&](const CallSiteContext &c) { ek.push_back(key(c)); });
    m.forEachContext([&](const CallSiteContext &c) { mk.push_back(key(c)); });
    REQUIRE(ek.size() == e.size());
    CHECK(ek == mk);
    CHECK(keys(e.allContexts()) == keys(m.allContexts()));

    std::vector<std::string> er, mr;
    // The walk holds the index's lock, so the shapes are resolved after
    // it (as ContextSignatures::build does).
    auto record = [](const ControlFlowIndex &cf,
                     std::vector<std::string> &out) {
      std::vector<ControlFlowIndex::ContextRecord> records;
      cf.forEachContextRecord([&](const ControlFlowIndex::ContextRecord &r) {
        records.push_back(r);
      });
      for (const auto &r : records)
        out.push_back(
            cf.stringOf(r.callSite) + "|" + cf.stringOf(r.callerUsr) + "|" +
            cf.stringOf(r.callerName) + "|" + cf.stringOf(r.calleeUsr) + "|" +
            (r.tuPath == ControlFlowIndex::kNoString ? std::string("<no tu>")
                                                     : cf.stringOf(r.tuPath)) +
            "|" + key(cf.contextOfShape(r.shape)));
    };
    record(e, er);
    record(m, mr);
    CHECK(er == mr);
    // The two forms number their strings differently; only the strings
    // themselves must agree, and an id neither holds resolves to "".
    CHECK(m.stringOf(UINT32_MAX - 1).empty());
    CHECK(e.stringOf(UINT32_MAX - 1).empty());
  }

  SECTION("site lookups") {
    for (const auto &s : f.sites) {
      INFO("site " << s);
      CHECK(key(e.contextAtSite(s)) == key(m.contextAtSite(s)));
      CHECK(keys(e.contextsAtSite(s)) == keys(m.contextsAtSite(s)));
      for (const auto &c : f.callerUsrs)
        CHECK(key(e.contextAtSite(s, c)) == key(m.contextAtSite(s, c)));
      for (const auto &c : f.callerNames)
        CHECK(key(e.contextAtSite(s, c)) == key(m.contextAtSite(s, c)));
    }
    CHECK_FALSE(m.contextAtSite("no/such/file.cpp:1:1"));
    CHECK(m.contextsAtSite("").empty());
    CHECK_FALSE(m.contextAtSite(f.sites.front(), "no such caller"));
  }

  SECTION("edge contexts and signatures") {
    ContextSignatures es = ContextSignatures::build(e);
    ContextSignatures ms = ContextSignatures::build(m);
    CHECK(es.contextCount() == ms.contextCount());
    CHECK(es.shapeCount() == ms.shapeCount());
    size_t edges = 0;
    for (const CallGraphNode *n : f.eager.graph.allNodes()) {
      for (const CallGraphEdge &edge : f.eager.graph.calleesOf(n->usr)) {
        ++edges;
        INFO("edge " << edge.callSite << " " << edge.callerUsr << " -> "
                     << edge.calleeUsr);
        CHECK(key(e.contextForEdge(edge.callSite, edge.callerUsr,
                                   edge.calleeUsr)) ==
              key(m.contextForEdge(edge.callSite, edge.callerUsr,
                                   edge.calleeUsr)));
        CHECK(key(e.contextForEdge(edge.callSite, edge.callerName,
                                   edge.calleeUsr)) ==
              key(m.contextForEdge(edge.callSite, edge.callerName,
                                   edge.calleeUsr)));
        CHECK(es.forEdge(edge.callSite, edge.callerUsr, edge.calleeUsr) ==
              ms.forEdge(edge.callSite, edge.callerUsr, edge.calleeUsr));
      }
    }
    CHECK(edges > 20);
  }

  SECTION("callee lookups, by usr and by display name") {
    auto both = [&](const std::string &n) {
      INFO("callee " << n);
      CHECK(keys(e.contextsForCallee(n)) == keys(m.contextsForCallee(n)));
      CHECK(keys(e.protectedCallsTo(n)) == keys(m.protectedCallsTo(n)));
      CHECK(keys(e.unprotectedCallsTo(n)) == keys(m.unprotectedCallsTo(n)));
    };
    size_t nonEmpty = 0, protectedSeen = 0;
    for (const auto &n : f.calleeUsrs) {
      both(n);
      nonEmpty += !m.contextsForCallee(n).empty();
      protectedSeen += !m.protectedCallsTo(n).empty();
    }
    for (const auto &n : f.calleeNames)
      both(n);
    for (const auto &n : f.functions)
      both(n);
    both("no such callee");
    CHECK(nonEmpty == f.calleeUsrs.size());
    if (f.name == "exceptions")
      CHECK(protectedSeen > 0);
  }

  SECTION("caller lookups, by usr and by display name") {
    auto both = [&](const std::string &n) {
      INFO("caller " << n);
      CHECK(keys(e.contextsForCaller(n)) == keys(m.contextsForCaller(n)));
      CHECK(e.callerNoexceptOf(n) == m.callerNoexceptOf(n));
    };
    for (const auto &n : f.callerUsrs)
      both(n);
    for (const auto &n : f.callerNames)
      both(n);
    for (const auto &n : f.functions)
      both(n);
    both("no such caller");
    CHECK_FALSE(m.callerNoexceptOf("no such caller"));
  }
}

TEST_CASE("every control-flow tool answers alike over the mapped index",
          "[mapped][tools]") {
  Fixture &f = *GENERATE(from_range(Fixture::all()));
  INFO(f.name);
  const auto tools = getRegisteredTools();
  using O = llvm::json::Object;
  std::vector<std::pair<std::string, O>> battery;
  for (const auto &fn : f.functions) {
    battery.push_back(
        {"query_exception_safety",
         O{{"function", fn}, {"exception_type", "std::exception"}}});
    battery.push_back({"query_nearest_catches", O{{"function", fn}}});
    battery.push_back({"query_locks_held", O{{"function", fn}}});
    battery.push_back(
        {"query_throw_propagation", O{{"function", fn},
                                      {"exception_type", "std::exception"},
                                      {"max_paths", 3}}});
    battery.push_back(
        {"query_all_path_contexts", O{{"function", fn}, {"max_paths", 3}}});
  }
  for (const auto &s : f.sites) {
    battery.push_back({"query_call_site_context", O{{"call_site", s}}});
    battery.push_back({"query_raii_scopes_at_callsite", O{{"call_site", s}}});
    for (const auto &c : f.callerNames)
      battery.push_back(
          {"query_call_site_context", O{{"call_site", s}, {"caller", c}}});
  }
  battery.push_back({"query_same_lock",
                     O{{"fn_a", "stage5_sink"}, {"fn_b", "stage4_dispatch"}}});
  battery.push_back(
      {"query_same_lock", O{{"fn_a", "Pipeline::run"}, {"fn_b", "main"}}});
  size_t withRecords = 0;
  for (const auto &[tool, args] : battery) {
    const std::string ea = run(f.eager, tools, tool, args);
    const std::string ma = run(f.mapped, tools, tool, args);
    INFO(tool << " " << serialized(llvm::json::Value(O(args))));
    CHECK(ea == ma);
    withRecords += ea.find("\"status\":\"ok\"") != std::string::npos;
  }
  CHECK(withRecords > battery.size() / 4);
}

TEST_CASE("a mapped index outlives the load that produced it",
          "[mapped][lifetime]") {
  Fixture &f = Fixture::deepChains();
  std::optional<SnapshotData> snap =
      SnapshotIO::load(f.path, nullptr, LoadMode::ReadOnly);
  REQUIRE(snap);
  ControlFlowIndex moved = std::move(snap->cfIndex);
  snap.reset();
  ControlFlowIndex assigned;
  assigned = std::move(moved);
  REQUIRE(assigned.isMapped());
  CHECK(assigned.size() == f.eager.cfIndex.size());
  CHECK(keys(assigned.allContexts()) == keys(f.eager.cfIndex.allContexts()));
  for (const auto &s : f.sites)
    CHECK(key(assigned.contextAtSite(s)) ==
          key(f.eager.cfIndex.contextAtSite(s)));
}

TEST_CASE("a mapped index reads only the control-flow section it needs",
          "[mapped][sections]") {
  Fixture &f = Fixture::deepChains();
  SnapshotLoadStats stats;
  auto snap = SnapshotIO::load(f.path, &stats, LoadMode::ReadOnly,
                               kSectionGraph | kSectionControlFlow);
  REQUIRE(snap);
  CHECK(snap->cfIndex.isMapped());
  CHECK(snap->loaded == (kSectionGraph | kSectionControlFlow));
  bool sawContexts = false;
  for (const auto &sec : stats.sections)
    if (std::strcmp(sec.name, "cf_contexts") == 0) {
      sawContexts = true;
      CHECK_FALSE(sec.skipped);
    }
  CHECK(sawContexts);
  auto graphOnly =
      SnapshotIO::load(f.path, nullptr, LoadMode::ReadOnly, kSectionGraph);
  REQUIRE(graphOnly);
  CHECK_FALSE(graphOnly->cfIndex.isMapped());
  CHECK(graphOnly->cfIndex.size() == 0);
}

TEST_CASE("a damaged v12 control-flow section is refused or read safely",
          "[mapped][corrupt]") {
  Fixture &f = Fixture::exceptions();
  const std::string original = readFile(f.path);
  const Layout l = layoutOf(original, f.eager.cfIndex);
  const std::string path = tempPath("corrupt");
  struct Cleanup {
    const std::string &p;
    ~Cleanup() { std::remove(p.c_str()); }
  } cleanup{path};

  // The damage is sealed with fresh checksums, so what is exercised is
  // the mapped reader's own validation, not the v13 integrity check.
  auto loadMapped = [&](std::string bytes) {
    testing::resealSnapshot(bytes);
    writeFile(path, bytes);
    return SnapshotIO::load(path, nullptr, LoadMode::ReadOnly);
  };

  SECTION("the untouched copy loads and matches") {
    auto snap = loadMapped(original);
    REQUIRE(snap);
    CHECK(keys(snap->cfIndex.allContexts()) ==
          keys(f.eager.cfIndex.allContexts()));
  }

  SECTION("a truncated file is refused") {
    CHECK_FALSE(loadMapped(original.substr(0, original.size() - 1)));
    CHECK_FALSE(loadMapped(original.substr(0, l.sectionEnd - 5)));
    CHECK_FALSE(loadMapped(original.substr(0, l.recordsAt + 10)));
  }

  SECTION("a record count past the section is refused") {
    std::string bytes = original;
    putU32At(bytes, l.countAt, static_cast<uint32_t>(l.count + 1));
    CHECK_FALSE(loadMapped(bytes));
    putU32At(bytes, l.countAt, UINT32_MAX);
    CHECK_FALSE(loadMapped(bytes));
  }

  SECTION("a display order longer than the records is refused") {
    std::string bytes = original;
    putU32At(bytes, l.callerDisplayCountAt, static_cast<uint32_t>(l.count + 1));
    CHECK_FALSE(loadMapped(bytes));
  }

  SECTION("an interner table longer than the section is refused") {
    std::string bytes = original;
    putU32At(bytes, l.sectionStart, UINT32_MAX);
    CHECK_FALSE(loadMapped(bytes));
    putU32At(bytes, l.sectionStart, 3);
    CHECK_FALSE(loadMapped(bytes));
  }

  SECTION("a record with a bad set index reads as dead") {
    std::string bytes = original;
    putU32At(bytes, l.recordsAt + 24, UINT32_MAX); // record 0, scopeSet
    auto snap = loadMapped(bytes);
    REQUIRE(snap);
    CHECK(snap->cfIndex.allContexts().size() == l.count - 1);
    exerciseEverything(f, snap->cfIndex);
    // The resident load checks the same field and refuses the file.
    CHECK_FALSE(SnapshotIO::load(path, nullptr, LoadMode::Mutable));
  }

  SECTION("a record with a bad string id reads as dead") {
    std::string bytes = original;
    putU32At(bytes, l.recordsAt + 38 + 16, UINT32_MAX - 1); // record 1, site
    auto snap = loadMapped(bytes);
    REQUIRE(snap);
    CHECK(snap->cfIndex.allContexts().size() == l.count - 1);
    exerciseEverything(f, snap->cfIndex);
  }

  SECTION("a bad string offset resolves to the empty string") {
    std::string bytes = original;
    putU32At(bytes, l.stringOffsetsAt, UINT32_MAX);
    putU32At(bytes, l.stringOffsetsAt + 4, static_cast<uint32_t>(l.sectionEnd));
    auto snap = loadMapped(bytes);
    REQUIRE(snap);
    CHECK(snap->cfIndex.stringOf(0).empty());
    CHECK(snap->cfIndex.stringOf(1).empty());
    exerciseEverything(f, snap->cfIndex);
  }

  SECTION("bad ids in the string order and bad positions in the record "
          "orders are skipped") {
    std::string bytes = original;
    putU32At(bytes, l.sortedIdsAt, UINT32_MAX);
    putU32At(bytes, l.sortedIdsAt + 4 * (l.stringCount / 2), UINT32_MAX);
    putU32At(bytes, l.byCallerAt, UINT32_MAX);
    putU32At(bytes, l.byCalleeAt + 4 * (l.count - 1), UINT32_MAX);
    putU32At(bytes, l.callerDisplayCountAt + 4, UINT32_MAX);
    auto snap = loadMapped(bytes);
    REQUIRE(snap);
    exerciseEverything(f, snap->cfIndex);
    // The record region itself is intact, so the sequential walk is.
    CHECK(keys(snap->cfIndex.allContexts()) ==
          keys(f.eager.cfIndex.allContexts()));
  }

  SECTION("every byte of the lookup arrays flipped, one at a time, is safe") {
    // A sweep over the arrays in steps: each load either fails or
    // answers every query without fault.
    const size_t step = (l.sectionEnd - l.stringOffsetsAt) / 24 + 1;
    for (size_t at = l.stringOffsetsAt; at < l.sectionEnd; at += step) {
      std::string bytes = original;
      bytes[at] = static_cast<char>(~bytes[at]);
      auto snap = loadMapped(bytes);
      if (snap)
        exerciseEverything(f, snap->cfIndex);
    }
  }
}
