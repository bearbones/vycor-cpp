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

// The dependency audit (docs/dependency-audit.md): the target map, the
// attribution of symbols to link targets, the per-edge closure, the
// findings, and the two tools over them.

#include "vycor/callgraph/CallGraph.h"
#include "vycor/callgraph/ControlFlowIndex.h"
#include "vycor/callgraph/ControlFlowOracle.h"
#include "vycor/callgraph/ReferenceIndex.h"
#include "vycor/callgraph/Snapshot.h"
#include "vycor/deps/DependencyAudit.h"
#include "vycor/deps/TargetMap.h"
#include "vycor/query/Tools.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

using namespace vycor;

namespace {

// ---- the target map ---------------------------------------------------------

bool parseMap(llvm::StringRef text, TargetMap &out, std::string &error,
              llvm::StringRef base = "/p") {
  auto doc = llvm::json::parse(text);
  REQUIRE(bool(doc));
  return parseTargetMap(*doc, base, out, error);
}

std::string parseError(llvm::StringRef text) {
  TargetMap map;
  std::string error;
  CHECK_FALSE(parseMap(text, map, error));
  return error;
}

// Five targets; `shared/s.cpp` is compiled into both executables.
const char *const kMap = R"({
  "schema": 1, "generator": "test", "depsForm": "direct",
  "targets": [
    {"name": "util", "kind": "static_library",
     "sources": ["util/u.cpp"], "deps": ["core"]},
    {"name": "app", "kind": "executable",
     "sources": ["app/main.cpp", "shared/s.cpp"],
     "deps": ["core", "extra", "util"]},
    {"name": "tool", "kind": "executable",
     "sources": ["tool/t.cpp", "shared/s.cpp"], "deps": ["util"]},
    {"name": "core", "kind": "static_library",
     "sources": ["core/a.cpp", "core/b.cpp"]},
    {"name": "extra", "kind": "static_library", "sources": ["extra/e.cpp"]}
  ]})";

// Target indices: the map sorts by name.
enum : uint32_t { kApp = 0, kCore = 1, kExtra = 2, kTool = 3, kUtil = 4 };

TargetMap theMap(const char *text = kMap) {
  TargetMap map;
  std::string error;
  REQUIRE(parseMap(text, map, error));
  return map;
}

// ---- a hand-built index -----------------------------------------------------

struct NodeSpec {
  const char *name;
  const char *file;
  unsigned line;
};

struct EdgeSpec {
  const char *from;
  const char *to;
  const char *site;
  EdgeKind kind = EdgeKind::DirectCall;
  Confidence confidence = Confidence::Proven;
};

// core:   clamp, core_scale, core_sum, big, Shape::area have bodies in
//         core's sources; core_leaf is declared in core.h and has no calls.
// twice:  an inline function in a header both app and util include.
// util:   util_fmt in u.cpp; priv_helper in a header only u.cpp opens.
// shared: shared_fn / shared_detail in the source both executables compile.
const std::vector<NodeSpec> kNodes = {
    {"clamp", "/p/core/a.cpp", 3},
    {"core_scale", "/p/core/core.h", 5},
    {"core_sum", "/p/core/core.h", 6},
    {"core_leaf", "/p/core/core.h", 7},
    {"Shape::area", "/p/core/core.h", 9},
    {"big", "/p/core/b.cpp", 1},
    {"twice", "/p/core/inline.h", 3},
    {"util_fmt", "/p/util/util.h", 1},
    {"priv_helper", "/p/util/detail.h", 2},
    {"extra_fn", "/p/extra/extra.h", 1},
    {"extra_helper", "/p/extra/e.cpp", 1},
    {"shared_fn", "/p/shared/shared.h", 1},
    {"shared_detail", "/p/shared/s.cpp", 2},
    {"main", "/p/app/main.cpp", 9},
    {"strlen", "/usr/include/string.h", 1},
    {"tool_run", "/p/tool/t.cpp", 1},
};

const std::vector<EdgeSpec> kEdges = {
    {"core_scale", "clamp", "/p/core/a.cpp:5:10"},
    {"core_sum", "clamp", "/p/core/a.cpp:6:10"},
    {"Shape::area", "core_leaf", "/p/core/a.cpp:9:3"},
    {"big", "core_sum", "/p/core/b.cpp:2:3"},
    {"twice", "core_scale", "/p/core/inline.h:3:20"},
    {"util_fmt", "core_sum", "/p/util/u.cpp:4:5"},
    {"util_fmt", "twice", "/p/util/u.cpp:4:20"},
    {"util_fmt", "priv_helper", "/p/util/u.cpp:4:30"},
    {"extra_fn", "extra_helper", "/p/extra/e.cpp:3:3"},
    {"shared_fn", "shared_detail", "/p/shared/s.cpp:4:3"},
    {"main", "twice", "/p/app/main.cpp:10:3"},
    {"main", "shared_fn", "/p/app/main.cpp:11:3"},
    {"main", "Shape::area", "/p/app/main.cpp:12:3", EdgeKind::VirtualDispatch,
     Confidence::Plausible},
    {"main", "strlen", "/p/app/main.cpp:13:3"},
    {"main", "util_fmt", "/p/app/main.cpp:14:3"},
    {"tool_run", "util_fmt", "/p/tool/t.cpp:2:3"},
    {"tool_run", "core_sum", "/p/tool/t.cpp:2:20"},
    {"tool_run", "shared_fn", "/p/tool/t.cpp:2:40"},
    {"tool_run", "core_leaf", "/p/tool/t.cpp:3:3"},
};

void fill(CallGraph &g, std::vector<NodeSpec> nodes,
          std::vector<EdgeSpec> edges, bool reversed = false) {
  if (reversed) {
    std::reverse(nodes.begin(), nodes.end());
    std::reverse(edges.begin(), edges.end());
  }
  for (const NodeSpec &n : nodes)
    g.addNode({n.name, n.file, n.line, false, false, "", ""}, n.file);
  for (const EdgeSpec &e : edges) {
    const std::string site = e.site;
    g.addEdge({e.from, e.to, e.kind, e.confidence, site, 0,
               ExecutionContext::Synchronous},
              site.substr(0, site.find(':')));
  }
}

struct TuSpec {
  const char *path;
  std::vector<const char *> opened;
  TuStatus status = TuStatus::Indexed;
};

const std::vector<TuSpec> kTus = {
    {"/p/app/main.cpp",
     {"/p/core/inline.h", "/p/core/core.h", "/p/shared/shared.h",
      "/p/util/util.h", "/usr/include/string.h"}},
    {"/p/shared/s.cpp", {"/p/shared/shared.h"}},
    {"/p/core/a.cpp", {"/p/core/core.h"}},
    {"/p/core/b.cpp", {"/p/core/core.h"}},
    {"/p/extra/e.cpp", {"/p/extra/extra.h"}},
    {"/p/tool/t.cpp",
     {"/p/util/util.h", "/p/core/core.h", "/p/shared/shared.h"}},
    {"/p/util/u.cpp",
     {"/p/util/util.h", "/p/core/core.h", "/p/core/inline.h",
      "/p/util/detail.h"}},
};

SnapshotMeta metaOf(const std::vector<TuSpec> &tus) {
  SnapshotMeta meta;
  for (const TuSpec &tu : tus) {
    meta.files.push_back({tu.path, 1, 2});
    meta.outcomes.push_back({tu.status, ""});
    std::vector<uint32_t> deps;
    for (const char *h : tu.opened) {
      auto it = std::find_if(meta.deps.begin(), meta.deps.end(),
                             [&](const FileStamp &s) { return s.path == h; });
      if (it == meta.deps.end()) {
        meta.deps.push_back({h, 1, 2});
        it = meta.deps.end() - 1;
      }
      deps.push_back(static_cast<uint32_t>(it - meta.deps.begin()));
    }
    meta.tuDeps.push_back(std::move(deps));
  }
  return meta;
}

uint32_t symbolNamed(const LinkFacts &lf, llvm::StringRef name) {
  for (uint32_t s = 0; s < lf.symbolCount(); ++s)
    if (lf.strings->resolve(lf.symUsr[s]) == name)
      return s;
  FAIL("no symbol named " << name.str());
  return kNoIndex;
}

std::vector<uint32_t> ownersOf(const LinkFacts &lf, llvm::StringRef name) {
  auto set = lf.targetsOf(lf.symOwner[symbolNamed(lf, name)]);
  return std::vector<uint32_t>(set.begin(), set.end());
}

SymbolClass classOf(const LinkFacts &lf, llvm::StringRef name) {
  return lf.symClass[symbolNamed(lf, name)];
}

const AuditRow &rowOf(const DependencyAudit &audit, uint32_t consumer,
                      uint32_t provider) {
  const uint32_t r = audit.find(consumer, provider);
  REQUIRE(r != kNoIndex);
  return audit.rows[r];
}

std::vector<std::string> seedNames(const LinkFacts &lf,
                                   const DependencyAudit &audit,
                                   const AuditRow &row) {
  std::vector<std::string> out;
  for (uint32_t i = row.seedBegin; i < row.seedEnd; ++i)
    out.push_back(lf.strings->resolve(lf.symUsr[audit.seedSym[i]]));
  return out;
}

std::vector<uint32_t> slice(const std::vector<uint32_t> &v, uint32_t b,
                            uint32_t e) {
  return std::vector<uint32_t>(v.begin() + b, v.begin() + e);
}

/// The fixture analysed whole: graph, map, meta, facts, audit.
struct Analysed {
  CallGraph graph;
  TargetMap map = theMap();
  SnapshotMeta meta = metaOf(kTus);
  LinkFacts facts;
  DependencyAudit audit;

  explicit Analysed(bool withMeta = true) {
    fill(graph, kNodes, kEdges);
    facts = linkFactsFromCallEdges(graph, map, withMeta ? &meta : nullptr);
    audit = auditDependencies(facts, map);
  }
};

// ---- the tools --------------------------------------------------------------

const ToolEntry &toolNamed(const std::vector<ToolEntry> &tools,
                           llvm::StringRef name) {
  for (const auto &t : tools)
    if (t.name == name)
      return t;
  FAIL("no tool named " << name.str());
  return tools.front();
}

std::string render(const llvm::json::Value &v) {
  std::string out;
  llvm::raw_string_ostream os(out);
  os << v;
  return out;
}

/// A target map written to a scratch file for the tools to load.
struct MapFile {
  llvm::SmallString<128> path;
  explicit MapFile(llvm::StringRef text) {
    int fd = -1;
    REQUIRE_FALSE(
        llvm::sys::fs::createTemporaryFile("vycor-target-map", "json", fd,
                                           path));
    llvm::raw_fd_ostream os(fd, /*shouldClose=*/true);
    os << text;
  }
  ~MapFile() { llvm::sys::fs::remove(path); }
};

// The fixture's map with an explicit root, so it resolves the same from a
// scratch directory.
std::string rootedMap() {
  std::string text = kMap;
  text.insert(text.find("\"schema\""), "\"root\": \"/p\", ");
  return text;
}

struct ToolFixture {
  CallGraph graph;
  ControlFlowIndex cf;
  ControlFlowOracle oracle;
  std::vector<std::string> entryPoints{"main"};
  QueryCache cache;
  SnapshotMeta meta = metaOf(kTus);
  std::vector<ToolEntry> tools = getRegisteredTools();
  ToolContext ctx;
  MapFile mapFile{rootedMap()};

  explicit ToolFixture(bool reversed = false)
      : oracle(graph, cf), ctx{graph, oracle, cf, entryPoints, nullptr, &cache} {
    fill(graph, kNodes, kEdges, reversed);
    ctx.meta = &meta;
    ctx.facts = IndexFacts::of(meta, IndexFreshness::Unchecked);
  }

  llvm::json::Object run(llvm::StringRef tool,
                         std::initializer_list<llvm::json::Object::KV> kvs,
                         bool withMap = true) {
    return runArgs(tool, llvm::json::Object(kvs), withMap);
  }

  llvm::json::Object runArgs(llvm::StringRef tool, llvm::json::Object args,
                             bool withMap = true) {
    if (withMap)
      args["target_map"] = mapFile.path.str().str();
    llvm::json::Value v = runTool(toolNamed(tools, tool), args, ctx);
    const auto *obj = v.getAsObject();
    REQUIRE(obj != nullptr);
    return *obj;
  }
};

const llvm::json::Object *edgeOf(const llvm::json::Object &payload,
                                 llvm::StringRef consumer,
                                 llvm::StringRef provider) {
  const auto *edges = payload.getArray("edges");
  REQUIRE(edges != nullptr);
  for (const auto &e : *edges) {
    const auto *o = e.getAsObject();
    if (o && o->getString("consumer") == consumer &&
        o->getString("provider") == provider)
      return o;
  }
  return nullptr;
}

std::vector<std::string> findingIds(const llvm::json::Object &edge) {
  std::vector<std::string> out;
  for (const auto &f : *edge.getArray("findings"))
    out.push_back(f.getAsObject()->getString("id")->str());
  return out;
}

// ---- a hand-built reference index -------------------------------------------

// Six targets under /q. `half` has a source the index never parsed, so it is
// not covered; `plug/plug.h` is opened by app and plug, so no target owns it
// outright.
const char *const kRefMap = R"({
  "schema": 1, "generator": "test", "depsForm": "direct", "root": "/q",
  "targets": [
    {"name": "app", "kind": "executable", "sources": ["app/main.cpp"],
     "deps": ["core", "extra", "plug"]},
    {"name": "base", "kind": "static_library", "sources": ["base/x.cpp"],
     "headers": ["base/base.h"]},
    {"name": "core", "kind": "static_library",
     "sources": ["core/a.cpp", "core/b.cpp"], "headers": ["core/core.h"],
     "deps": ["base"]},
    {"name": "extra", "kind": "static_library", "sources": ["extra/e.cpp"]},
    {"name": "half", "kind": "static_library",
     "sources": ["half/h1.cpp", "half/h2.cpp"], "headers": ["half/half.h"]},
    {"name": "plug", "kind": "static_library", "sources": ["plug/p.cpp"],
     "deps": ["core"]}
  ]})";

enum : uint32_t { rApp = 0, rBase = 1, rCore = 2, rExtra = 3, rHalf = 4,
                  rPlug = 5 };

const std::vector<TuSpec> kRefTus = {
    {"/q/app/main.cpp",
     {"/q/core/core.h", "/q/base/base.h", "/q/half/half.h", "/q/plug/plug.h"}},
    {"/q/base/x.cpp", {"/q/base/base.h"}},
    {"/q/core/a.cpp", {"/q/core/core.h", "/q/base/base.h"}},
    {"/q/core/b.cpp", {"/q/core/core.h"}},
    {"/q/extra/e.cpp", {}},
    {"/q/half/h1.cpp", {"/q/half/half.h"}},
    {"/q/plug/p.cpp", {"/q/plug/plug.h", "/q/core/core.h"}},
};

constexpr uint32_t kD = kSymDefinition;
constexpr uint32_t kH = kSymInHeader;
const char *const kLambdaUsr = "c:main.cpp@F@main@Sa@F@operator()#1";

struct RefSymSpec {
  const char *usr;
  const char *name;
  const char *file;
  uint32_t line;
  SymbolKind kind;
  LinkageClass linkage;
  uint32_t flags;
  std::vector<const char *> tus;
};

struct RefSpec {
  const char *from;
  const char *to;
  RefRole role;
  const char *file;
  uint32_t line, col;
  std::vector<const char *> tus;
};

constexpr SymbolKind kFn = SymbolKind::Function;
constexpr SymbolKind kVar = SymbolKind::Variable;
constexpr LinkageClass kExt = LinkageClass::External;
constexpr LinkageClass kVague = LinkageClass::Vague;

// app:   main calls core_fn, the inline core_inline, gone_fn (declared in
//        core.h, defined nowhere), half_fn (declared in half.h; half is not
//        covered), plug_init; constructs a Shape and calls its virtual area;
//        its lambda calls core_cb.
// core:  core_fn calls core_helper and reads g_table; core_other calls
//        core_helper too; core_inline (in core.h) calls base_fn; Shape's
//        vtable is homed in b.cpp.
// plug:  plug_init takes the address of the factory template make_plugin.
// extra: nothing app uses; extra_reg is a static initializer.
const std::vector<RefSymSpec> kRefSyms = {
    {"c:@F@main", "main", "/q/app/main.cpp", 5, kFn, kExt, kD | kSymMain,
     {"/q/app/main.cpp"}},
    {kLambdaUsr, "main()::(lambda)::operator()", "/q/app/main.cpp", 7, kFn,
     LinkageClass::Internal, kD | kSymLambda, {"/q/app/main.cpp"}},
    {"c:@F@core_fn", "core_fn", "/q/core/core.h", 3, kFn, kExt, kH,
     {"/q/app/main.cpp", "/q/core/a.cpp"}},
    {"c:@F@core_fn", "core_fn", "/q/core/a.cpp", 3, kFn, kExt, kD,
     {"/q/core/a.cpp"}},
    {"c:@F@core_helper", "core_helper", "/q/core/core.h", 4, kFn, kExt, kH,
     {"/q/core/a.cpp"}},
    {"c:@F@core_helper", "core_helper", "/q/core/b.cpp", 2, kFn, kExt, kD,
     {"/q/core/b.cpp"}},
    {"c:@F@core_other", "core_other", "/q/core/a.cpp", 10, kFn, kExt, kD,
     {"/q/core/a.cpp"}},
    {"c:@F@core_cb", "core_cb", "/q/core/core.h", 5, kFn, kExt, kH,
     {"/q/app/main.cpp"}},
    {"c:@F@core_cb", "core_cb", "/q/core/a.cpp", 12, kFn, kExt, kD,
     {"/q/core/a.cpp"}},
    {"c:@g_table", "g_table", "/q/core/core.h", 6, kVar, kExt, kH,
     {"/q/core/a.cpp"}},
    {"c:@g_table", "g_table", "/q/core/b.cpp", 1, kVar, kExt, kD,
     {"/q/core/b.cpp"}},
    {"c:@F@core_inline", "core_inline", "/q/core/core.h", 8, kFn, kVague,
     kD | kH, {"/q/app/main.cpp", "/q/core/a.cpp"}},
    {"c:@F@gone_fn", "gone_fn", "/q/core/core.h", 9, kFn, kExt, kH,
     {"/q/app/main.cpp"}},
    {"c:@S@Shape", "Shape", "/q/core/core.h", 12, SymbolKind::Class, kExt,
     kD | kH | kSymDynamicClass, {"/q/app/main.cpp"}},
    {"c:@S@Shape", "Shape", "/q/core/core.h", 12, SymbolKind::Class, kExt,
     kD | kH | kSymDynamicClass | kSymVtableHome, {"/q/core/b.cpp"}},
    {"c:@S@Shape@F@area#", "Shape::area", "/q/core/core.h", 13, kFn, kExt,
     kH | kSymVirtual | kSymMember, {"/q/app/main.cpp"}},
    {"c:@S@Shape@F@area#", "Shape::area", "/q/core/b.cpp", 5, kFn, kExt,
     kD | kSymVirtual | kSymMember, {"/q/core/b.cpp"}},
    {"c:@F@base_fn", "base_fn", "/q/base/base.h", 2, kFn, kExt, kH,
     {"/q/core/a.cpp"}},
    {"c:@F@base_fn", "base_fn", "/q/base/x.cpp", 2, kFn, kExt, kD,
     {"/q/base/x.cpp"}},
    {"c:@F@half_fn", "half_fn", "/q/half/half.h", 2, kFn, kExt, kH,
     {"/q/app/main.cpp"}},
    {"c:@F@extra_fn", "extra_fn", "/q/extra/e.cpp", 1, kFn, kExt, kD,
     {"/q/extra/e.cpp"}},
    {"c:@extra_reg", "extra_reg", "/q/extra/e.cpp", 5, kVar, kExt, kD,
     {"/q/extra/e.cpp"}},
    {"c:@F@plug_init", "plug_init", "/q/plug/plug.h", 2, kFn, kExt, kH,
     {"/q/app/main.cpp", "/q/plug/p.cpp"}},
    {"c:@F@plug_init", "plug_init", "/q/plug/p.cpp", 3, kFn, kExt, kD,
     {"/q/plug/p.cpp"}},
    {"c:@FT@>1#Tmake_plugin#v#", "make_plugin", "/q/plug/plug.h", 4, kFn,
     kVague, kD | kH, {"/q/plug/p.cpp"}},
    {"c:@F@plug_fn", "plug_fn", "/q/plug/plug.h", 3, kFn, kExt, kH,
     {"/q/app/main.cpp", "/q/plug/p.cpp"}},
    {"c:@F@plug_fn", "plug_fn", "/q/plug/p.cpp", 8, kFn, kExt, kD,
     {"/q/plug/p.cpp"}},
    {"c:plug.h@file", "plug.h", "/q/plug/plug.h", 0, SymbolKind::File,
     LinkageClass::None, kH, {"/q/app/main.cpp", "/q/plug/p.cpp"}},
};

const std::vector<RefSpec> kRefRefs = {
    {"c:@F@main", "c:@F@core_fn", RefRole::Call, "/q/app/main.cpp", 6, 3,
     {"/q/app/main.cpp"}},
    {"c:@F@main", "c:@F@core_inline", RefRole::Call, "/q/app/main.cpp", 7, 3,
     {"/q/app/main.cpp"}},
    {kLambdaUsr, "c:@F@core_cb", RefRole::Call, "/q/app/main.cpp", 7, 20,
     {"/q/app/main.cpp"}},
    {"c:@F@main", "c:@F@gone_fn", RefRole::Call, "/q/app/main.cpp", 8, 3,
     {"/q/app/main.cpp"}},
    {"c:@F@main", "c:@S@Shape", RefRole::VtableUse, "/q/app/main.cpp", 9, 3,
     {"/q/app/main.cpp"}},
    {"c:@F@main", "c:@S@Shape@F@area#", RefRole::Call, "/q/app/main.cpp", 10,
     3, {"/q/app/main.cpp"}},
    {"c:@F@main", "c:@F@half_fn", RefRole::Call, "/q/app/main.cpp", 11, 3,
     {"/q/app/main.cpp"}},
    {"c:@F@main", "c:@F@plug_init", RefRole::Call, "/q/app/main.cpp", 12, 3,
     {"/q/app/main.cpp"}},
    {"c:@F@core_inline", "c:@F@base_fn", RefRole::Call, "/q/core/core.h", 8,
     20, {"/q/app/main.cpp", "/q/core/a.cpp"}},
    {"c:@F@core_fn", "c:@F@core_helper", RefRole::Call, "/q/core/a.cpp", 4, 3,
     {"/q/core/a.cpp"}},
    {"c:@F@core_fn", "c:@g_table", RefRole::Read, "/q/core/a.cpp", 4, 20,
     {"/q/core/a.cpp"}},
    {"c:@F@core_other", "c:@F@core_helper", RefRole::Call, "/q/core/a.cpp",
     11, 3, {"/q/core/a.cpp"}},
    {"c:@extra_reg", "c:@F@extra_fn", RefRole::Call, "/q/extra/e.cpp", 5, 20,
     {"/q/extra/e.cpp"}},
    {"c:@F@plug_init", "c:@FT@>1#Tmake_plugin#v#", RefRole::AddressOf,
     "/q/plug/p.cpp", 4, 3, {"/q/plug/p.cpp"}},
    {"c:plug.h@file", "c:@F@plug_fn", RefRole::Read, "/q/plug/plug.h", 6, 1,
     {"/q/app/main.cpp", "/q/plug/p.cpp"}},
};

void fillRefs(ReferenceIndex &refs, bool reversed = false) {
  std::vector<RefSymSpec> syms = kRefSyms;
  std::vector<RefSpec> uses = kRefRefs;
  if (reversed) {
    std::reverse(syms.begin(), syms.end());
    std::reverse(uses.begin(), uses.end());
  }
  for (const RefSymSpec &s : syms) {
    SymbolFact f;
    f.usr = s.usr;
    f.name = s.name;
    f.file = s.file;
    f.line = s.line;
    f.kind = s.kind;
    f.linkage = s.linkage;
    f.flags = s.flags;
    for (const char *tu : s.tus)
      refs.addSymbol(f, tu);
  }
  for (const RefSpec &u : uses) {
    ReferenceFact r;
    r.referrerUsr = u.from;
    r.targetUsr = u.to;
    r.role = u.role;
    r.file = u.file;
    r.line = u.line;
    r.col = u.col;
    for (const char *tu : u.tus)
      refs.addReference(r, tu);
  }
}

TargetMap refMap() {
  TargetMap map;
  std::string error;
  REQUIRE(parseMap(kRefMap, map, error, "/q"));
  return map;
}

/// The reference fixture analysed whole.
struct RefAnalysed {
  ReferenceIndex refs;
  CallGraph graph;
  TargetMap map = refMap();
  SnapshotMeta meta = metaOf(kRefTus);
  LinkFacts facts;
  DependencyAudit audit;

  explicit RefAnalysed(bool withGraph = false) {
    fillRefs(refs);
    facts = linkFactsFromReferences(refs, withGraph ? &graph : nullptr, map,
                                    &meta);
    audit = auditDependencies(facts, map);
  }
  void rerun() {
    facts = linkFactsFromReferences(refs, &graph, map, &meta);
    audit = auditDependencies(facts, map);
  }
};

struct RefToolFixture {
  ReferenceIndex refs;
  CallGraph graph;
  ControlFlowIndex cf;
  ControlFlowOracle oracle;
  std::vector<std::string> entryPoints{"main"};
  QueryCache cache;
  SnapshotMeta meta = metaOf(kRefTus);
  std::vector<ToolEntry> tools = getRegisteredTools();
  ToolContext ctx;
  MapFile mapFile{kRefMap};

  explicit RefToolFixture(bool reversed = false)
      : oracle(graph, cf), ctx{graph, oracle, cf, entryPoints, nullptr, &cache} {
    fillRefs(refs, reversed);
    ctx.meta = &meta;
    ctx.references = &refs;
    ctx.facts = IndexFacts::of(meta, IndexFreshness::Unchecked);
  }

  llvm::json::Object run(llvm::StringRef tool,
                         std::initializer_list<llvm::json::Object::KV> kvs) {
    llvm::json::Object args(kvs);
    args["target_map"] = mapFile.path.str().str();
    llvm::json::Value v = runTool(toolNamed(tools, tool), args, ctx);
    const auto *obj = v.getAsObject();
    REQUIRE(obj != nullptr);
    return *obj;
  }
};

} // namespace

// ---------------------------------------------------------------------------
// Target map
// ---------------------------------------------------------------------------

TEST_CASE("target map: targets are sorted by name with resolved paths",
          "[dependency-audit]") {
  TargetMap map = theMap();
  REQUIRE(map.size() == 5);
  CHECK(map.names == std::vector<std::string>{"app", "core", "extra", "tool",
                                              "util"});
  CHECK(map.generator == "test");
  CHECK_FALSE(map.depsAreClosure);
  CHECK(map.kinds[kApp] == TargetKind::Executable);
  CHECK(map.kinds[kCore] == TargetKind::StaticLibrary);
  CHECK(map.sourcesOf(kApp).vec() ==
        std::vector<std::string>{"/p/app/main.cpp", "/p/shared/s.cpp"});
  CHECK(map.depsOf(kApp).vec() == std::vector<uint32_t>{kCore, kExtra, kUtil});
  CHECK(map.depsOf(kCore).empty());
  CHECK(map.find("tool") == kTool);
  CHECK_FALSE(map.find("nope").has_value());
  // Without `root` or `roots`, the base directory is the one root.
  CHECK(map.roots == std::vector<std::string>{"/p"});
}

TEST_CASE("target map: root and roots and odd spellings",
          "[dependency-audit]") {
  TargetMap map;
  std::string error;
  REQUIRE(parseMap(R"({"schema": 1, "depsForm": "closure", "root": "src",
    "roots": [".", "/build/gen/"],
    "targets": [
      {"name": "b", "kind": "shared_library",
       "sources": ["./lib/../lib/b.cpp", "/abs/x.cpp", "lib/b.cpp"],
       "headers": ["include/b.h"], "deps": ["a", "b", "a"]},
      {"name": "a", "kind": "interface_library"}
    ]})",
                   map, error, "/work"));
  CHECK(map.depsAreClosure);
  // Relative paths resolve against the root; duplicates collapse.
  CHECK(map.sourcesOf(1).vec() ==
        std::vector<std::string>{"/abs/x.cpp", "/work/src/lib/b.cpp"});
  CHECK(map.headersOf(1).vec() ==
        std::vector<std::string>{"/work/src/include/b.h"});
  // A self-dependency is dropped, a repeated one kept once.
  CHECK(map.depsOf(1).vec() == std::vector<uint32_t>{0});
  CHECK(map.kinds[0] == TargetKind::InterfaceLibrary);
  CHECK(map.roots == std::vector<std::string>{"/build/gen", "/work/src"});
  CHECK(map.underRoots("/work/src/lib/b.cpp"));
  CHECK(map.underRoots("/build/gen/x.inc"));
  CHECK_FALSE(map.underRoots("/work/srcs/b.cpp")); // a prefix, not a parent
  CHECK_FALSE(map.underRoots("/usr/include/string.h"));
}

TEST_CASE("target map: malformed documents are rejected with a reason",
          "[dependency-audit]") {
  CHECK(parseError("[]") == "the target map must be a JSON object");
  CHECK(parseError(R"({"targets": []})") == "`schema` must be 1");
  CHECK(parseError(R"({"schema": 2, "targets": []})") == "`schema` must be 1");
  CHECK(parseError(R"({"schema": 1})") == "`targets` must be an array");
  CHECK(parseError(R"({"schema": 1, "depsForm": "some", "targets": []})") ==
        "`depsForm` must be \"direct\" or \"closure\"");
  CHECK(parseError(R"({"schema": 1, "roots": [], "targets": []})") ==
        "`roots` must be a non-empty array of directories");
  CHECK(parseError(R"({"schema": 1, "targets": [{"kind": "executable"}]})") ==
        "every target needs a non-empty `name`");
  CHECK(parseError(R"({"schema": 1, "targets": [{"name": "a"}]})") ==
        "target 'a': `kind` is required");
  CHECK(parseError(
            R"({"schema": 1, "targets": [{"name": "a", "kind": "jar"}]})") ==
        "target 'a': unknown `kind` 'jar'");
  CHECK(parseError(R"({"schema": 1, "targets": [
          {"name": "a", "kind": "executable", "sources": "a.cpp"}]})") ==
        "target 'a': `sources` must be an array");
  CHECK(parseError(R"({"schema": 1, "targets": [
          {"name": "a", "kind": "executable"},
          {"name": "a", "kind": "static_library"}]})") ==
        "duplicate target name 'a'");
  CHECK(parseError(R"({"schema": 1, "targets": [
          {"name": "a", "kind": "executable", "deps": ["b"]}]})") ==
        "target 'a' depends on 'b', which the map does not declare");
}

TEST_CASE("target map: a missing or unparsable file names the file",
          "[dependency-audit]") {
  TargetMap map;
  std::string error;
  CHECK_FALSE(loadTargetMap("/nonexistent/targets.json", map, error));
  CHECK(error.find("cannot read target map '/nonexistent/targets.json'") == 0);

  MapFile broken("{not json");
  CHECK_FALSE(loadTargetMap(broken.path, map, error));
  CHECK(error.find("is not valid JSON") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Attribution
// ---------------------------------------------------------------------------

TEST_CASE("dependency audit: a symbol belongs to the target whose files "
          "hold its calls or its declaration",
          "[dependency-audit]") {
  Analysed a;
  const LinkFacts &lf = a.facts;

  // Calls written in a mapped source: the body is in that target.
  CHECK(classOf(lf, "core_scale") == SymbolClass::Defined);
  CHECK(ownersOf(lf, "core_scale") == std::vector<uint32_t>{kCore});
  CHECK(lf.files[lf.symFile[symbolNamed(lf, "core_scale")]] ==
        "/p/core/a.cpp");
  // No calls, declared in a mapped source.
  CHECK(classOf(lf, "clamp") == SymbolClass::Defined);
  CHECK(ownersOf(lf, "clamp") == std::vector<uint32_t>{kCore});
  // No calls, declared in a header only one target's TUs open.
  CHECK(classOf(lf, "priv_helper") == SymbolClass::Defined);
  CHECK(ownersOf(lf, "priv_helper") == std::vector<uint32_t>{kUtil});
  CHECK(lf.files[lf.symFile[symbolNamed(lf, "priv_helper")]] ==
        "/p/util/detail.h");

  // No calls, declared in a shared header: the owner is presumed to be
  // the target that defines the header's other functions.
  CHECK(classOf(lf, "core_leaf") == SymbolClass::Presumed);
  CHECK(ownersOf(lf, "core_leaf") == std::vector<uint32_t>{kCore});

  // Calls written in a header several targets compile: an inline body.
  CHECK(classOf(lf, "twice") == SymbolClass::Inline);
  CHECK(ownersOf(lf, "twice").empty());

  // A system header only one target includes is still nobody's.
  CHECK(classOf(lf, "strlen") == SymbolClass::Unattributed);

  CHECK(lf.counts.defined == 13);
  CHECK(lf.counts.presumed == 1);
  CHECK(lf.counts.inlined == 1);
  CHECK(lf.counts.unattributed == 1);
  CHECK(lf.counts.skippedVirtualCalls == 1);
  CHECK(lf.counts.unresolvedSites == 0);
}

TEST_CASE("dependency audit: a source compiled into two targets gives its "
          "symbols to both",
          "[dependency-audit]") {
  Analysed a;
  CHECK(ownersOf(a.facts, "shared_fn") == std::vector<uint32_t>{kApp, kTool});
  CHECK(ownersOf(a.facts, "shared_detail") ==
        std::vector<uint32_t>{kApp, kTool});
  // Each executable has its own copy: neither depends on the other.
  CHECK(a.audit.find(kApp, kTool) == kNoIndex);
  CHECK(a.audit.find(kTool, kApp) == kNoIndex);
  // The shared source counts once toward each target.
  CHECK(a.audit.providerSymbols[kApp] == 3);  // main + the two shared
  CHECK(a.audit.providerSymbols[kTool] == 3); // tool_run + the two shared
}

TEST_CASE("dependency audit: a header listed by a target is that target's",
          "[dependency-audit]") {
  // inline.h is opened by app and util; listing it under core makes the
  // inline function core's own symbol instead of a body that travels.
  std::string text = kMap;
  const std::string anchor = "\"sources\": [\"core/a.cpp\", \"core/b.cpp\"]";
  text.insert(text.find(anchor) + anchor.size(),
              ", \"headers\": [\"core/inline.h\"]");
  CallGraph g;
  fill(g, kNodes, kEdges);
  TargetMap map = theMap(text.c_str());
  SnapshotMeta meta = metaOf(kTus);
  LinkFacts lf = linkFactsFromCallEdges(g, map, &meta);
  // The call inside it is written in a shared file, so no body is seen;
  // the listing decides the presumed owner.
  CHECK(classOf(lf, "twice") == SymbolClass::Inline);

  // A function declared there without calls is presumed core's.
  CallGraph g2;
  auto nodes = kNodes;
  nodes.push_back({"inline_leaf", "/p/core/inline.h", 9});
  fill(g2, nodes, kEdges);
  LinkFacts lf2 = linkFactsFromCallEdges(g2, map, &meta);
  CHECK(classOf(lf2, "inline_leaf") == SymbolClass::Presumed);
  CHECK(ownersOf(lf2, "inline_leaf") == std::vector<uint32_t>{kCore});
  // Without the listing nothing in inline.h votes, so it stays unowned.
  LinkFacts lf3 = linkFactsFromCallEdges(g2, theMap(), &meta);
  CHECK(classOf(lf3, "inline_leaf") == SymbolClass::Unattributed);
}

TEST_CASE("dependency audit: a header named like a source that includes it "
          "is presumed to be that source's",
          "[dependency-audit]") {
  // Nothing in tokenizer.h has a visible body, so no function votes for
  // an owner; the name is the only lead.
  CallGraph g;
  fill(g,
       {{"tok_a", "/p/text/tokenizer.h", 1},
        {"tok_b", "/p/text/tokenizer.h", 2},
        {"misc", "/p/text/misc.h", 1},
        {"user", "/p/user/u.cpp", 1}},
       {{"user", "tok_a", "/p/user/u.cpp:2:3"},
        {"user", "misc", "/p/user/u.cpp:3:3"}});
  const char *text = R"({"schema": 1, "targets": [
    {"name": "idle", "kind": "static_library", "sources": ["idle/tokenizer.cpp"]},
    {"name": "text", "kind": "static_library", "sources": ["text/tokenizer.cpp"]},
    {"name": "user", "kind": "executable", "sources": ["user/u.cpp"],
     "deps": ["idle", "text"]}]})";
  TargetMap map = theMap(text);
  SnapshotMeta meta = metaOf(
      {{"/p/text/tokenizer.cpp", {"/p/text/tokenizer.h", "/p/text/misc.h"}},
       {"/p/idle/tokenizer.cpp", {}},
       {"/p/user/u.cpp", {"/p/text/tokenizer.h", "/p/text/misc.h"}}});
  LinkFacts lf = linkFactsFromCallEdges(g, map, &meta);
  CHECK(classOf(lf, "tok_a") == SymbolClass::Presumed);
  CHECK(ownersOf(lf, "tok_b") == std::vector<uint32_t>{1});
  // misc.h matches no source by name: nobody's.
  CHECK(classOf(lf, "misc") == SymbolClass::Unattributed);
  DependencyAudit audit = auditDependencies(lf, map);
  CHECK(rowOf(audit, 2, 1).uncertainSymbols == 1);
  // A provider known only through presumed symbols can be reported unused
  // by a consumer that calls none of them; one with no known symbol cannot.
  CHECK(rowFindings(audit, lf, rowOf(audit, 2, 1), {}, true) == 0);
  CHECK(rowFindings(audit, lf, rowOf(audit, 2, 0), {}, true) == 0);

  // Two same-named sources of different targets including it: no lead.
  SnapshotMeta both = metaOf(
      {{"/p/text/tokenizer.cpp", {"/p/text/tokenizer.h"}},
       {"/p/idle/tokenizer.cpp", {"/p/text/tokenizer.h"}},
       {"/p/user/u.cpp", {"/p/text/tokenizer.h"}}});
  LinkFacts lf2 = linkFactsFromCallEdges(g, map, &both);
  CHECK(classOf(lf2, "tok_a") == SymbolClass::Unattributed);

  // The consumer calls nothing of text: unused, on presumed symbols alone.
  CallGraph quiet;
  fill(quiet,
       {{"tok_a", "/p/text/tokenizer.h", 1}, {"user", "/p/user/u.cpp", 1}},
       {});
  LinkFacts lf3 = linkFactsFromCallEdges(quiet, map, &meta);
  DependencyAudit audit3 = auditDependencies(lf3, map);
  CHECK(audit3.presumedSymbols[1] == 1);
  CHECK(rowFindings(audit3, lf3, rowOf(audit3, 2, 1), {}, true) ==
        kFindingUnused);
  CHECK(rowFindings(audit3, lf3, rowOf(audit3, 2, 0), {}, true) == 0);
}

TEST_CASE("dependency audit: template code is inline whatever its USR "
          "spells after the name",
          "[dependency-audit]") {
  // Leaves declared in a header that has an owner (text lists it): only
  // the template rule keeps them from being presumed text's.
  const char *const opEq =
      "c:@N@t@F@operator==<#I>#&&$@N@t@S@Lhs>#I#I#"; // operator== <int>
  const char *const opShl =
      "c:@N@t@F@operator<<#&$@N@t@S@Out#I#"; // operator<<, no template
  const char *const opLess = "c:@N@t@F@operator<#I#I#"; // operator<
  const char *const opShlT =
      "c:@N@t@F@operator<<<#I>#&$@N@t@S@Out#I#"; // operator<< <int>
  const char *const withLambda = // gen<lambda in run()>: a later @F@
      "c:t.h@N@t@F@gen<#$@F@run#@Sa>#S0_#";
  const char *const member = "c:@N@t@S@Box>#I@F@get#"; // Box<int>::get
  const char *const pattern = "c:@N@t@FT@>1#Tmake#t0.0#";
  const char *const plain = "c:@N@t@F@plain#I#";
  const char *const localArg = // a parameter type names another function
      "c:@N@t@F@take#$@F@run#@Sa#";
  const char *const conversion = "c:@N@t@S@Out@F@operator bool#1";
  const char *const all[] = {opEq,   opShl,   opLess, opShlT,   withLambda,
                             member, pattern, plain,  localArg, conversion};
  CallGraph g;
  std::vector<NodeSpec> nodes = {{"user", "/p/user/u.cpp", 1}};
  std::vector<EdgeSpec> edges;
  for (const char *usr : all) {
    nodes.push_back({usr, "/p/text/t.h", 1});
    edges.push_back({"user", usr, "/p/user/u.cpp:2:3"});
  }
  fill(g, nodes, edges);
  const char *text = R"({"schema": 1, "targets": [
    {"name": "text", "kind": "static_library", "sources": ["text/t.cpp"],
     "headers": ["text/t.h"]},
    {"name": "user", "kind": "executable", "sources": ["user/u.cpp"],
     "deps": ["text"]}]})";
  TargetMap map = theMap(text);
  LinkFacts lf = linkFactsFromCallEdges(g, map, nullptr);
  for (const char *usr : {opEq, opShlT, withLambda, member, pattern}) {
    INFO(usr);
    CHECK(classOf(lf, usr) == SymbolClass::Inline);
  }
  for (const char *usr : {opShl, opLess, plain, localArg, conversion}) {
    INFO(usr);
    CHECK(classOf(lf, usr) == SymbolClass::Presumed);
  }
}

TEST_CASE("dependency audit: without index meta no header is private and "
          "no target is covered",
          "[dependency-audit]") {
  Analysed a(/*withMeta=*/false);
  CHECK_FALSE(a.facts.hasIndexMeta);
  // detail.h can no longer be shown to be util's alone.
  CHECK(classOf(a.facts, "priv_helper") == SymbolClass::Unattributed);
  for (uint32_t t = 0; t < a.map.size(); ++t)
    CHECK_FALSE(a.facts.targetCovered[t]);
  // The rows still exist; only the unused finding is withheld.
  const AuditRow &row = rowOf(a.audit, kApp, kExtra);
  CHECK(row.symbols == 0);
  CHECK(rowFindings(a.audit, a.facts, row, {}, /*coverageStated=*/true) == 0);
}

// ---------------------------------------------------------------------------
// Rows
// ---------------------------------------------------------------------------

TEST_CASE("dependency audit: rows are the declared dependencies and the "
          "observed ones in name order",
          "[dependency-audit]") {
  Analysed a;
  std::vector<std::pair<uint32_t, uint32_t>> pairs;
  for (const AuditRow &row : a.audit.rows)
    pairs.emplace_back(row.consumer, row.provider);
  CHECK(pairs == std::vector<std::pair<uint32_t, uint32_t>>{
                     {kApp, kCore},  {kApp, kExtra}, {kApp, kUtil},
                     {kTool, kCore}, {kTool, kUtil}, {kUtil, kCore}});
  CHECK(rowOf(a.audit, kApp, kCore).declared == DeclaredForm::Direct);
  CHECK(rowOf(a.audit, kTool, kCore).declared == DeclaredForm::Transitive);
  CHECK(a.audit.providerSymbols[kCore] == 5);
  CHECK(a.audit.presumedSymbols[kCore] == 1);
  CHECK(a.audit.providerFiles[kCore] == 2);
}

TEST_CASE("dependency audit: a call inside a shared inline body is charged "
          "to the target that uses the body",
          "[dependency-audit]") {
  Analysed a;
  // app never names a core function: it reaches core_scale through twice.
  // The call through the vtable slot is not a link reference.
  const AuditRow &row = rowOf(a.audit, kApp, kCore);
  CHECK(row.symbols == 1);
  CHECK(row.viaInline == 1);
  CHECK(row.uncertainSymbols == 0);
  CHECK(seedNames(a.facts, a.audit, row) ==
        std::vector<std::string>{"core_scale"});
  CHECK(a.audit.seedFlags[row.seedBegin] == kSeedViaInline);
  // The seed's reference is the one in the inline body; its root is the
  // consumer's own call into that body.
  const LinkFacts &lf = a.facts;
  CHECK(lf.strings->resolve(lf.refSite[a.audit.seedRef[row.seedBegin]]) ==
        "/p/core/inline.h:3:20");
  CHECK(lf.strings->resolve(lf.refSite[a.audit.seedRoot[row.seedBegin]]) ==
        "/p/app/main.cpp:10:3");

  // util names core_sum itself and reaches core_scale through twice.
  const AuditRow &util = rowOf(a.audit, kUtil, kCore);
  CHECK(util.symbols == 2);
  CHECK(util.viaInline == 1);
  CHECK(seedNames(lf, a.audit, util) ==
        std::vector<std::string>{"core_sum", "core_scale"});
}

TEST_CASE("dependency audit: the closure is what the used symbols need "
          "inside the provider",
          "[dependency-audit]") {
  Analysed a;
  const LinkFacts &lf = a.facts;

  // core_scale needs clamp; big, core_sum and Shape::area stay behind.
  const AuditRow &app = rowOf(a.audit, kApp, kCore);
  CHECK(app.closureSymbols == 2);
  CHECK(app.closureFiles == 1);
  std::vector<uint32_t> closure;
  closureSymbolsOf(lf, a.audit, app, closure);
  std::vector<std::string> names;
  for (uint32_t s : closure)
    names.push_back(lf.strings->resolve(lf.symUsr[s]));
  std::sort(names.begin(), names.end());
  CHECK(names == std::vector<std::string>{"clamp", "core_scale"});
  CHECK(app.needBegin == app.needEnd);

  // util_fmt needs priv_helper (a second file) and, leaving util, core:
  // directly and through the inline body it calls.
  const AuditRow &tool = rowOf(a.audit, kTool, kUtil);
  CHECK(tool.symbols == 1);
  CHECK(tool.closureSymbols == 2);
  CHECK(tool.closureFiles == 2);
  CHECK(slice(a.audit.needs, tool.needBegin, tool.needEnd) ==
        std::vector<uint32_t>{kCore});

  // A presumed symbol is a seed but never part of a closure.
  const AuditRow &tc = rowOf(a.audit, kTool, kCore);
  CHECK(tc.symbols == 1);
  CHECK(tc.uncertainSymbols == 1);
  CHECK(tc.closureSymbols == 2); // core_sum, clamp
  CHECK(seedNames(lf, a.audit, tc) ==
        std::vector<std::string>{"core_sum", "core_leaf"});
  CHECK(a.audit.seedFlags[tc.seedBegin + 1] == kSeedUncertain);
}

TEST_CASE("dependency audit: what a consumer would stop linking",
          "[dependency-audit]") {
  Analysed a;
  // app reaches core through util as well: cutting app -> core drops
  // nothing from app's link.
  const AuditRow &ac = rowOf(a.audit, kApp, kCore);
  CHECK(ac.onlyViaEdge == 0);
  CHECK_FALSE(ac.providerDroppable);
  CHECK(ac.droppableSymbols == 0);

  // extra is linked only through app -> extra and nothing of it is used.
  const AuditRow &ae = rowOf(a.audit, kApp, kExtra);
  CHECK(ae.onlyViaEdge == 1);
  CHECK(ae.providerDroppable);
  CHECK(slice(a.audit.droppable, ae.dropBegin, ae.dropEnd) ==
        std::vector<uint32_t>{kExtra});
  CHECK(ae.droppableSymbols == 2);

  // util -> core: core is linked only because of this edge; splitting out
  // the three symbols util needs leaves the other two behind.
  const AuditRow &uc = rowOf(a.audit, kUtil, kCore);
  CHECK(uc.closureSymbols == 3);
  CHECK(uc.onlyViaEdge == 1);
  CHECK(uc.providerDroppable);
  CHECK(uc.droppableSymbols == 2);

  // tool -> util: tool names a core function itself, so only util hangs
  // on this edge; all of util is in the closure.
  const AuditRow &tu = rowOf(a.audit, kTool, kUtil);
  CHECK(tu.onlyViaEdge == 1);
  CHECK(slice(a.audit.droppable, tu.dropBegin, tu.dropEnd) ==
        std::vector<uint32_t>{kUtil});
  CHECK(tu.droppableSymbols == 0);
}

// ---------------------------------------------------------------------------
// Findings
// ---------------------------------------------------------------------------

TEST_CASE("dependency audit: an unused finding needs stated and clean "
          "coverage of both targets",
          "[dependency-audit]") {
  Analysed a;
  const AuditRow &row = rowOf(a.audit, kApp, kExtra);
  CHECK(rowFindings(a.audit, a.facts, row, {}, true) == kFindingUnused);
  // The adapter did not state the index's coverage.
  CHECK(rowFindings(a.audit, a.facts, row, {}, false) == 0);

  // One of the provider's sources did not parse cleanly.
  auto tus = kTus;
  tus[4].status = TuStatus::Partial; // extra/e.cpp
  SnapshotMeta meta = metaOf(tus);
  LinkFacts lf = linkFactsFromCallEdges(a.graph, a.map, &meta);
  DependencyAudit audit = auditDependencies(lf, a.map);
  CHECK_FALSE(lf.targetCovered[kExtra]);
  CHECK(lf.counts.failedSources == 1);
  CHECK(rowFindings(audit, lf, rowOf(audit, kApp, kExtra), {}, true) == 0);

  // One of the consumer's sources was never indexed.
  tus = kTus;
  tus.erase(tus.begin() + 1); // shared/s.cpp
  meta = metaOf(tus);
  lf = linkFactsFromCallEdges(a.graph, a.map, &meta);
  audit = auditDependencies(lf, a.map);
  CHECK_FALSE(lf.targetCovered[kApp]);
  CHECK(lf.targetCovered[kExtra]);
  CHECK(rowFindings(audit, lf, rowOf(audit, kApp, kExtra), {}, true) == 0);
}

TEST_CASE("dependency audit: a presumed use is never reported as unused",
          "[dependency-audit]") {
  // tool's only reference into `leafy` is a function with no body seen.
  CallGraph g;
  fill(g,
       {{"leaf_fn", "/p/leafy/leafy.h", 1},
        {"leafy_other", "/p/leafy/leafy.h", 2},
        {"leafy_detail", "/p/leafy/l.cpp", 1},
        {"user", "/p/user/u.cpp", 1}},
       {{"leafy_other", "leafy_detail", "/p/leafy/l.cpp:3:3"},
        {"user", "leaf_fn", "/p/user/u.cpp:2:3"}});
  TargetMap map = theMap(R"({"schema": 1, "targets": [
    {"name": "leafy", "kind": "static_library", "sources": ["leafy/l.cpp"]},
    {"name": "user", "kind": "executable", "sources": ["user/u.cpp"],
     "deps": ["leafy"]}]})");
  SnapshotMeta meta = metaOf({{"/p/leafy/l.cpp", {"/p/leafy/leafy.h"}},
                              {"/p/user/u.cpp", {"/p/leafy/leafy.h"}}});
  LinkFacts lf = linkFactsFromCallEdges(g, map, &meta);
  DependencyAudit audit = auditDependencies(lf, map);
  const AuditRow &row = rowOf(audit, 1, 0);
  CHECK(row.symbols == 0);
  CHECK(row.uncertainSymbols == 1);
  // Thin on presumed evidence (1 of 3 symbols is over 10%, so loosen it).
  CHECK(rowFindings(audit, lf, row, {}, true) == 0);
  CHECK(rowFindings(audit, lf, row, {5, 50}, true) == kFindingThin);
}

TEST_CASE("dependency audit: undeclared and thin findings",
          "[dependency-audit]") {
  Analysed a;
  // tool names core_sum but only declares util.
  CHECK(rowFindings(a.audit, a.facts, rowOf(a.audit, kTool, kCore), {},
                    true) == kFindingUndeclared);

  // app -> core: one symbol, a closure of 2 against 5 defined + 1 presumed.
  const AuditRow &ac = rowOf(a.audit, kApp, kCore);
  CHECK(rowFindings(a.audit, a.facts, ac, {5, 10}, true) == 0);
  CHECK(rowFindings(a.audit, a.facts, ac, {5, 33}, true) == 0);
  CHECK(rowFindings(a.audit, a.facts, ac, {5, 34}, true) == kFindingThin);
  // util -> core uses two symbols.
  const AuditRow &uc = rowOf(a.audit, kUtil, kCore);
  CHECK(rowFindings(a.audit, a.facts, uc, {1, 100}, true) == 0);
  CHECK(rowFindings(a.audit, a.facts, uc, {2, 100}, true) == kFindingThin);
  // A thin finding does not need coverage: it rests on what was seen.
  CHECK(rowFindings(a.audit, a.facts, ac, {5, 34}, false) == kFindingThin);
}

TEST_CASE("dependency audit: a closure form map cannot tell direct from "
          "transitive",
          "[dependency-audit]") {
  // In closure form every target lists everything it links. util now
  // also links extra, so app and tool list it too.
  std::string text = kMap;
  auto swap = [&](const std::string &from, const std::string &to) {
    REQUIRE(text.find(from) != std::string::npos);
    text.replace(text.find(from), from.size(), to);
  };
  swap("\"direct\"", "\"closure\"");
  swap("\"deps\": [\"core\"]", "\"deps\": [\"core\", \"extra\"]");
  swap("\"deps\": [\"util\"]", "\"deps\": [\"util\", \"core\", \"extra\"]");
  CallGraph g;
  fill(g, kNodes, kEdges);
  TargetMap map = theMap(text.c_str());
  SnapshotMeta meta = metaOf(kTus);
  LinkFacts lf = linkFactsFromCallEdges(g, map, &meta);
  DependencyAudit audit = auditDependencies(lf, map);

  // A listed target no other listed target pulls in was named directly.
  CHECK(rowOf(audit, kUtil, kExtra).declared == DeclaredForm::Direct);
  CHECK(rowFindings(audit, lf, rowOf(audit, kUtil, kExtra), {}, true) ==
        kFindingUnused);
  CHECK(rowOf(audit, kTool, kUtil).declared == DeclaredForm::Direct);

  // One that a listed target pulls in may be direct or not.
  CHECK(rowOf(audit, kTool, kCore).declared == DeclaredForm::Closure);
  CHECK(rowOf(audit, kApp, kExtra).declared == DeclaredForm::Closure);
  // Listed and unused says nothing: it may be there for a dependency.
  CHECK(rowFindings(audit, lf, rowOf(audit, kApp, kExtra), {}, true) == 0);
  // Listed and used is not undeclared; it can still be thin.
  CHECK(rowFindings(audit, lf, rowOf(audit, kTool, kCore), {5, 100}, true) ==
        kFindingThin);
  // Nothing can be said about what cutting such an edge would drop.
  CHECK(rowOf(audit, kTool, kCore).onlyViaEdge == 0);
}

TEST_CASE("dependency audit: deferred and virtual edges are counted and "
          "left out",
          "[dependency-audit]") {
  CallGraph g;
  fill(g,
       {{"Base::run", "/p/lib/base.h", 1},
        {"make", "/p/lib/base.h", 2},
        {"lib_detail", "/p/lib/l.cpp", 1},
        {"user", "/p/user/u.cpp", 1}},
       {{"make", "lib_detail", "/p/lib/l.cpp:4:3"},
        {"Base::run", "lib_detail", "/p/lib/l.cpp:5:3"},
        {"user", "Base::run", "/p/user/u.cpp:2:3", EdgeKind::VirtualDispatch,
         Confidence::Plausible},
        {"user", "make", "/p/user/u.cpp:3:3",
         EdgeKind::FunctionPointerReturn}});
  TargetMap map = theMap(R"({"schema": 1, "targets": [
    {"name": "lib", "kind": "static_library", "sources": ["lib/l.cpp"]},
    {"name": "user", "kind": "executable", "sources": ["user/u.cpp"],
     "deps": ["lib"]}]})");
  SnapshotMeta meta = metaOf({{"/p/lib/l.cpp", {"/p/lib/base.h"}},
                              {"/p/user/u.cpp", {"/p/lib/base.h"}}});
  LinkFacts lf = linkFactsFromCallEdges(g, map, &meta);
  CHECK(lf.counts.skippedVirtualCalls == 1);
  CHECK(lf.counts.skippedDeferred == 1);
  DependencyAudit audit = auditDependencies(lf, map);
  const AuditRow &row = rowOf(audit, 1, 0);
  CHECK(row.symbols == 0);
  CHECK(row.uncertainSymbols == 0);
  // Neither is a reference the linker is known to need, but both touch
  // the provider: an indirect use, which is enough to withhold "unused".
  CHECK(row.indirectUses == 2);
  CHECK(rowFindings(audit, lf, row, {}, true) == 0);

  // Without them the same row is reported.
  CallGraph g0;
  fill(g0,
       {{"Base::run", "/p/lib/base.h", 1},
        {"lib_detail", "/p/lib/l.cpp", 1},
        {"user", "/p/user/u.cpp", 1},
        {"user_detail", "/p/user/u.cpp", 5}},
       {{"Base::run", "lib_detail", "/p/lib/l.cpp:5:3"},
        {"user", "user_detail", "/p/user/u.cpp:2:3"}});
  LinkFacts lf0 = linkFactsFromCallEdges(g0, map, &meta);
  DependencyAudit audit0 = auditDependencies(lf0, map);
  CHECK(rowOf(audit0, 1, 0).indirectUses == 0);
  CHECK(rowFindings(audit0, lf0, rowOf(audit0, 1, 0), {}, true) ==
        kFindingUnused);

  // A virtual call whose concrete type is known is a direct reference.
  CallGraph g2;
  fill(g2,
       {{"Base::run", "/p/lib/base.h", 1},
        {"lib_detail", "/p/lib/l.cpp", 1},
        {"user", "/p/user/u.cpp", 1}},
       {{"Base::run", "lib_detail", "/p/lib/l.cpp:5:3"},
        {"user", "Base::run", "/p/user/u.cpp:2:3", EdgeKind::VirtualDispatch,
         Confidence::Proven}});
  LinkFacts lf2 = linkFactsFromCallEdges(g2, map, &meta);
  CHECK(lf2.counts.skippedVirtualCalls == 0);
  CHECK(rowOf(auditDependencies(lf2, map), 1, 0).symbols == 1);
}

TEST_CASE("dependency audit: any call written in a source pins the body "
          "there",
          "[dependency-audit]") {
  // via_virtual's only call is a virtual one, recurse's only call is to
  // itself, and deferred's is a function pointer return. None of the three
  // is a reference, but each shows where the body is.
  CallGraph g;
  fill(g,
       {{"Base::run", "/usr/include/base.h", 1},
        {"via_virtual", "/p/lib/api.h", 1},
        {"recurse", "/p/lib/api.h", 2},
        {"deferred", "/p/lib/api.h", 3},
        {"user", "/p/user/u.cpp", 1}},
       {{"via_virtual", "Base::run", "/p/lib/l.cpp:3:3",
         EdgeKind::VirtualDispatch, Confidence::Plausible},
        {"recurse", "recurse", "/p/lib/l.cpp:5:3"},
        {"deferred", "Base::run", "/p/lib/l.cpp:7:3",
         EdgeKind::FunctionPointerReturn},
        {"user", "via_virtual", "/p/user/u.cpp:2:3"},
        {"user", "recurse", "/p/user/u.cpp:3:3"},
        {"user", "deferred", "/p/user/u.cpp:4:3"}});
  TargetMap map = theMap(R"({"schema": 1, "targets": [
    {"name": "lib", "kind": "static_library", "sources": ["lib/l.cpp"]},
    {"name": "user", "kind": "executable", "sources": ["user/u.cpp"],
     "deps": ["lib"]}]})");
  SnapshotMeta meta = metaOf({{"/p/lib/l.cpp", {"/p/lib/api.h"}},
                              {"/p/user/u.cpp", {"/p/lib/api.h"}}});
  LinkFacts lf = linkFactsFromCallEdges(g, map, &meta);
  for (const char *name : {"via_virtual", "recurse", "deferred"}) {
    INFO(name);
    CHECK(classOf(lf, name) == SymbolClass::Defined);
    CHECK(ownersOf(lf, name) == std::vector<uint32_t>{0});
  }
  DependencyAudit audit = auditDependencies(lf, map);
  const AuditRow &row = rowOf(audit, 1, 0);
  CHECK(row.symbols == 3);
  CHECK(rowFindings(audit, lf, row, {5, 100}, true) == kFindingThin);
}

TEST_CASE("dependency audit: a body in an indexed source no target lists "
          "is not carried",
          "[dependency-audit]") {
  // gen/g.cpp was indexed but belongs to no target in the map. gen_fn's
  // body is there, so it is not an inline body that travels with its
  // callers, and its reference into lib is not user's.
  CallGraph g;
  fill(g,
       {{"gen_fn", "/p/gen/g.h", 1},
        {"lib_fn", "/p/lib/lib.h", 1},
        {"lib_detail", "/p/lib/l.cpp", 1},
        {"user", "/p/user/u.cpp", 1}},
       {{"gen_fn", "lib_fn", "/p/gen/g.cpp:3:3"},
        {"lib_fn", "lib_detail", "/p/lib/l.cpp:3:3"},
        {"user", "gen_fn", "/p/user/u.cpp:2:3"}});
  TargetMap map = theMap(R"({"schema": 1, "targets": [
    {"name": "lib", "kind": "static_library", "sources": ["lib/l.cpp"]},
    {"name": "user", "kind": "executable", "sources": ["user/u.cpp"]}]})");
  SnapshotMeta meta =
      metaOf({{"/p/gen/g.cpp", {"/p/gen/g.h", "/p/lib/lib.h"}},
              {"/p/lib/l.cpp", {"/p/lib/lib.h", "/p/gen/g.h"}},
              {"/p/user/u.cpp", {"/p/gen/g.h"}}});
  LinkFacts lf = linkFactsFromCallEdges(g, map, &meta);
  CHECK(classOf(lf, "gen_fn") == SymbolClass::Unattributed);
  CHECK(lf.counts.unmappedBodyRefs == 1);
  DependencyAudit audit = auditDependencies(lf, map);
  CHECK(audit.find(1, 0) == kNoIndex);
  // Its file is one the build compiles, so it is not an unplaced use
  // either: the map says where it is not.
  CHECK(audit.unplacedUses[1] == 0);

  // The same graph when the source was never indexed: the call site is in
  // a file nothing is known about, and the body travels as before.
  SnapshotMeta partial =
      metaOf({{"/p/lib/l.cpp", {"/p/lib/lib.h", "/p/gen/g.h"}},
              {"/p/user/u.cpp", {"/p/gen/g.h"}}});
  LinkFacts lf2 = linkFactsFromCallEdges(g, map, &partial);
  CHECK(classOf(lf2, "gen_fn") == SymbolClass::Inline);
  CHECK(lf2.counts.unmappedBodyRefs == 0);
}

TEST_CASE("dependency audit: a tied header vote presumes every tied owner",
          "[dependency-audit]") {
  // both.h declares one function defined in `a` and one defined in `b`;
  // the leaf beside them could be either's.
  CallGraph g;
  fill(g,
       {{"a_fn", "/p/both/both.h", 1},
        {"b_fn", "/p/both/both.h", 2},
        {"leaf", "/p/both/both.h", 3},
        {"a_detail", "/p/a/a.cpp", 1},
        {"b_detail", "/p/b/b.cpp", 1},
        {"user", "/p/user/u.cpp", 1}},
       {{"a_fn", "a_detail", "/p/a/a.cpp:3:3"},
        {"b_fn", "b_detail", "/p/b/b.cpp:3:3"},
        {"user", "leaf", "/p/user/u.cpp:2:3"}});
  TargetMap map = theMap(R"({"schema": 1, "targets": [
    {"name": "a", "kind": "static_library", "sources": ["a/a.cpp"]},
    {"name": "b", "kind": "static_library", "sources": ["b/b.cpp"]},
    {"name": "user", "kind": "executable", "sources": ["user/u.cpp"],
     "deps": ["a", "b"]}]})");
  SnapshotMeta meta = metaOf({{"/p/a/a.cpp", {"/p/both/both.h"}},
                              {"/p/b/b.cpp", {"/p/both/both.h"}},
                              {"/p/user/u.cpp", {"/p/both/both.h"}}});
  LinkFacts lf = linkFactsFromCallEdges(g, map, &meta);
  CHECK(classOf(lf, "leaf") == SymbolClass::Presumed);
  CHECK(ownersOf(lf, "leaf") == std::vector<uint32_t>{0, 1});
  DependencyAudit audit = auditDependencies(lf, map);
  for (uint32_t provider : {0u, 1u}) {
    const AuditRow &row = rowOf(audit, 2, provider);
    CHECK(row.symbols == 0);
    CHECK(row.uncertainSymbols == 1);
    CHECK(rowFindings(audit, lf, row, {}, true) == 0);
  }
}

TEST_CASE("dependency audit: an inline body from a provider's header is an "
          "indirect use",
          "[dependency-audit]") {
  // user's only contact with lib is an inline function in a header lib
  // lists. It needs no symbol from lib, so it is not a seed; but user does
  // use lib, so the dependency is not reported as unused.
  CallGraph g;
  fill(g,
       {{"lib_inline", "/p/lib/inl.h", 1},
        {"lib_fn", "/p/lib/lib.h", 1},
        {"lib_detail", "/p/lib/l.cpp", 1},
        {"orphan", "/p/misc/orphan.h", 1},
        {"strlen", "/usr/include/string.h", 1},
        {"user", "/p/user/u.cpp", 1}},
       {{"lib_inline", "strlen", "/p/lib/inl.h:2:3"},
        {"lib_fn", "lib_detail", "/p/lib/l.cpp:3:3"},
        {"user", "lib_inline", "/p/user/u.cpp:2:3"},
        {"user", "orphan", "/p/user/u.cpp:3:3"}});
  const char *listed = R"({"schema": 1, "targets": [
    {"name": "lib", "kind": "static_library", "sources": ["lib/l.cpp"],
     "headers": ["lib/inl.h"]},
    {"name": "user", "kind": "executable", "sources": ["user/u.cpp"],
     "deps": ["lib"]}]})";
  TargetMap map = theMap(listed);
  SnapshotMeta meta =
      metaOf({{"/p/lib/l.cpp",
               {"/p/lib/lib.h", "/p/lib/inl.h", "/p/misc/orphan.h"}},
              {"/p/user/u.cpp",
               {"/p/lib/inl.h", "/p/misc/orphan.h",
                "/usr/include/string.h"}}});
  LinkFacts lf = linkFactsFromCallEdges(g, map, &meta);
  CHECK(classOf(lf, "lib_inline") == SymbolClass::Inline);
  // The header's owner is recorded as the body's home, never a link owner.
  CHECK(ownersOf(lf, "lib_inline") == std::vector<uint32_t>{0});
  DependencyAudit audit = auditDependencies(lf, map);
  const AuditRow &row = rowOf(audit, 1, 0);
  CHECK(row.symbols == 0);
  CHECK(row.uncertainSymbols == 0);
  CHECK(row.indirectUses == 1);
  CHECK(rowFindings(audit, lf, row, {}, true) == 0);

  // orphan is declared under the map's root in a header nothing places;
  // strlen is outside the build. Only the first is an unplaced use.
  CHECK(classOf(lf, "orphan") == SymbolClass::Unattributed);
  CHECK(audit.unplacedUses[1] == 1);
  CHECK(audit.unplacedUses[0] == 0);

  // Without the listing nothing places inl.h, and the row is reported.
  TargetMap bare = theMap(R"({"schema": 1, "targets": [
    {"name": "lib", "kind": "static_library", "sources": ["lib/l.cpp"]},
    {"name": "user", "kind": "executable", "sources": ["user/u.cpp"],
     "deps": ["lib"]}]})");
  LinkFacts lf2 = linkFactsFromCallEdges(g, bare, &meta);
  DependencyAudit audit2 = auditDependencies(lf2, bare);
  CHECK(lf2.targetsOf(lf2.symOwner[symbolNamed(lf2, "lib_inline")]).empty());
  CHECK(rowOf(audit2, 1, 0).indirectUses == 0);
  CHECK(rowFindings(audit2, lf2, rowOf(audit2, 1, 0), {}, true) ==
        kFindingUnused);
}

TEST_CASE("dependency audit: what a target forwards to its consumers is "
          "reached through it",
          "[dependency-audit]") {
  // hdr is header-only and forwards impl, the library its headers call.
  CallGraph g;
  fill(g,
       {{"impl_fn", "/p/impl/impl.h", 1},
        {"impl_detail", "/p/impl/i.cpp", 1},
        {"user", "/p/user/u.cpp", 1}},
       {{"impl_fn", "impl_detail", "/p/impl/i.cpp:3:3"},
        {"user", "impl_fn", "/p/user/u.cpp:2:3"}});
  SnapshotMeta meta = metaOf({{"/p/impl/i.cpp", {"/p/impl/impl.h"}},
                              {"/p/user/u.cpp", {"/p/impl/impl.h"}}});
  TargetMap map = theMap(R"({"schema": 1, "targets": [
    {"name": "hdr", "kind": "interface_library", "sources": [],
     "interfaceDeps": ["impl", "hdr", "impl"]},
    {"name": "impl", "kind": "static_library", "sources": ["impl/i.cpp"]},
    {"name": "user", "kind": "executable", "sources": ["user/u.cpp"],
     "deps": ["hdr"]}]})");
  CHECK(map.interfaceDepsOf(0) == llvm::ArrayRef<uint32_t>{1u});
  CHECK(map.depsOf(0).empty());
  LinkFacts lf = linkFactsFromCallEdges(g, map, &meta);
  DependencyAudit audit = auditDependencies(lf, map);
  const AuditRow &row = rowOf(audit, 2, 1);
  CHECK(row.declared == DeclaredForm::Transitive);
  // Still a provider user calls without naming it.
  CHECK(rowFindings(audit, lf, row, {0, 0}, true) == kFindingUndeclared);
  // The forwarding target does not link impl itself: no row of its own.
  CHECK(audit.find(0, 1) == kNoIndex);

  // Without the forwarding the same use has no declared path.
  TargetMap bare = theMap(R"({"schema": 1, "targets": [
    {"name": "hdr", "kind": "interface_library", "sources": []},
    {"name": "impl", "kind": "static_library", "sources": ["impl/i.cpp"]},
    {"name": "user", "kind": "executable", "sources": ["user/u.cpp"],
     "deps": ["hdr"]}]})");
  LinkFacts lf2 = linkFactsFromCallEdges(g, bare, &meta);
  DependencyAudit audit2 = auditDependencies(lf2, bare);
  CHECK(rowOf(audit2, 2, 1).declared == DeclaredForm::None);

  // A target named in both lists is the target's own dependency.
  TargetMap both = theMap(R"({"schema": 1, "targets": [
    {"name": "a", "kind": "static_library", "sources": ["a.cpp"],
     "deps": ["b"], "interfaceDeps": ["b", "c"]},
    {"name": "b", "kind": "static_library", "sources": ["b.cpp"]},
    {"name": "c", "kind": "static_library", "sources": ["c.cpp"]}]})");
  CHECK(both.depsOf(0) == llvm::ArrayRef<uint32_t>{1u});
  CHECK(both.interfaceDepsOf(0) == llvm::ArrayRef<uint32_t>{2u});

  CHECK(parseError(R"({"schema": 1, "targets": [
    {"name": "a", "kind": "static_library", "sources": [],
     "interfaceDeps": ["nope"]}]})") ==
        "target 'a' depends on 'nope', which the map does not declare");
  CHECK(parseError(R"({"schema": 1, "targets": [
    {"name": "a", "kind": "static_library", "sources": [],
     "interfaceDeps": "b"}]})") ==
        "target 'a': `interfaceDeps` must be an array");
}

TEST_CASE("dependency audit: an empty map and an empty graph",
          "[dependency-audit]") {
  CallGraph g;
  TargetMap empty = theMap(R"({"schema": 1, "targets": []})");
  LinkFacts lf = linkFactsFromCallEdges(g, empty, nullptr);
  DependencyAudit audit = auditDependencies(lf, empty);
  CHECK(audit.rows.empty());

  // Declared dependencies still make rows when nothing was indexed.
  TargetMap map = theMap();
  LinkFacts lf2 = linkFactsFromCallEdges(g, map, nullptr);
  DependencyAudit audit2 = auditDependencies(lf2, map);
  CHECK(audit2.rows.size() == 5);
  for (const AuditRow &row : audit2.rows) {
    CHECK(row.symbols == 0);
    CHECK(rowFindings(audit2, lf2, row, {}, true) == 0);
  }
}

// ---------------------------------------------------------------------------
// Tools
// ---------------------------------------------------------------------------

TEST_CASE("audit_dependencies: every payload states what the evidence "
          "cannot see",
          "[dependency-audit]") {
  ToolFixture fx;
  auto out = fx.run("audit_dependencies", {});
  CHECK(out.getString("status") == "ok");
  const auto *c = out.getObject("completeness");
  REQUIRE(c != nullptr);
  CHECK(c->getString("edgeSource") == "call-edges");
  CHECK(c->getBoolean("unusedIsUniversal") == false);
  CHECK(c->getBoolean("coverageStated") == true);
  REQUIRE(c->getArray("invisible") != nullptr);
  CHECK(render(llvm::json::Value(llvm::json::Array(*c->getArray("invisible")))) ==
        R"(["variable-references","vtable-and-typeinfo-references",)"
        R"("static-initializer-calls","implicit-destructor-calls",)"
        R"("calls-in-template-bodies","definitions-without-calls",)"
        R"("name-based-lookup","other-build-configurations"])");
  CHECK(c->getObject("targets")->getInteger("covered") == 5);
  CHECK(c->getObject("sources")->getInteger("mapped") == 7);
  CHECK(c->getObject("references")->getInteger("virtualDispatchSkipped") == 1);

  CHECK(out.getInteger("total") == 6);
  const auto *unused = edgeOf(out, "app", "extra");
  REQUIRE(unused != nullptr);
  CHECK(findingIds(*unused) ==
        std::vector<std::string>{"unused-link-dependency"});
  const auto *finding = (*unused->getArray("findings"))[0].getAsObject();
  CHECK(finding->getString("evidence") == "call-edges");
  CHECK(finding->getBoolean("universal") == false);
  CHECK(out.getObject("findingCounts")->getInteger("unused-link-dependency") ==
        1);
  CHECK(out.getInteger("unusedWithheld") == 0);

  // explain_dependency carries the same completeness member.
  auto why = fx.run("explain_dependency",
                    {{"consumer", "app"}, {"provider", "core"}});
  REQUIRE(why.getObject("completeness") != nullptr);
  CHECK(render(llvm::json::Value(
            llvm::json::Object(*why.getObject("completeness")))) ==
        render(llvm::json::Value(llvm::json::Object(*c))));
}

TEST_CASE("audit_dependencies: unstated coverage withholds the unused "
          "finding",
          "[dependency-audit]") {
  ToolFixture fx;
  fx.ctx.facts = IndexFacts{}; // an adapter that said nothing
  auto out = fx.run("audit_dependencies", {});
  CHECK(out.getObject("completeness")->getBoolean("coverageStated") == false);
  CHECK(findingIds(*edgeOf(out, "app", "extra")).empty());
  CHECK(out.getInteger("unusedWithheld") == 1);

  // Stated, but the handler was given no meta: nothing is covered.
  ToolFixture bare;
  bare.ctx.meta = nullptr;
  auto out2 = bare.run("audit_dependencies", {});
  CHECK(out2.getObject("completeness")->getBoolean("coverageStated") == false);
  CHECK(findingIds(*edgeOf(out2, "app", "extra")).empty());
  CHECK(out2.getInteger("unusedWithheld") == 1);
}

TEST_CASE("audit_dependencies: an index baked with collapse paths withholds "
          "the unused finding",
          "[dependency-audit]") {
  // Calls between two collapsed files were never recorded, so "no call
  // seen" is not evidence of anything.
  ToolFixture fx;
  fx.meta.collapsePaths = {"third_party"};
  auto out = fx.run("audit_dependencies", {});
  const auto *c = out.getObject("completeness");
  REQUIRE(c != nullptr);
  CHECK(c->getBoolean("edgesCollapsed") == true);
  CHECK(c->getBoolean("coverageStated") == true);
  const auto *invisible = c->getArray("invisible");
  REQUIRE(invisible != nullptr);
  REQUIRE(!invisible->empty());
  CHECK(invisible->back().getAsString() == "collapsed-call-edges");
  CHECK(findingIds(*edgeOf(out, "app", "extra")).empty());
  CHECK(out.getInteger("unusedWithheld") == 1);
  CHECK(out.getObject("findingCounts")->getInteger("unused-link-dependency") ==
        0);

  ToolFixture plain;
  auto base = plain.run("audit_dependencies", {});
  CHECK(base.getObject("completeness")->getBoolean("edgesCollapsed") == false);
}

TEST_CASE("audit_dependencies: filters and sort and paging",
          "[dependency-audit]") {
  ToolFixture fx;
  auto undeclared = fx.run("audit_dependencies",
                           {{"finding", "undeclared-link-dependency"}});
  CHECK(undeclared.getInteger("total") == 1);
  CHECK(edgeOf(undeclared, "tool", "core") != nullptr);

  auto any = fx.run("audit_dependencies", {{"finding", "any"}});
  CHECK(any.getInteger("total") == 2);

  auto ofCore = fx.run("audit_dependencies", {{"provider", "core"}});
  CHECK(ofCore.getInteger("total") == 3);
  CHECK(edgeOf(ofCore, "app", "extra") == nullptr);

  // By gain: the edge whose removal sheds the most symbols first; ties
  // keep (consumer, provider) order.
  auto gain = fx.run("audit_dependencies", {{"sort", "gain"}});
  const auto *edges = gain.getArray("edges");
  REQUIRE(edges->size() == 6);
  auto pairAt = [&](size_t i) {
    const auto *o = (*edges)[i].getAsObject();
    return o->getString("consumer")->str() + ">" +
           o->getString("provider")->str();
  };
  CHECK(pairAt(0) == "app>extra"); // 2
  CHECK(pairAt(1) == "util>core"); // 2
  CHECK(pairAt(2) == "app>core");  // 0

  auto page = fx.run("audit_dependencies", {{"limit", 2}, {"offset", 1}});
  CHECK(page.getInteger("total") == 6);
  CHECK(page.getInteger("returned") == 2);
  CHECK(page.getBoolean("truncated") == true);
  CHECK(page.getInteger("nextOffset") == 3);
  CHECK(edgeOf(page, "app", "extra") != nullptr);
  CHECK(edgeOf(page, "app", "core") == nullptr);

  auto thin = fx.run("audit_dependencies", {{"finding", "thin-dependency"},
                                            {"thin_max_closure_pct", 34}});
  CHECK(thin.getObject("thinThresholds")->getInteger("maxClosurePct") == 34);
  CHECK(edgeOf(thin, "app", "core") != nullptr);
}

TEST_CASE("audit_dependencies: argument errors are typed",
          "[dependency-audit]") {
  ToolFixture fx;
  auto noMap = fx.run("audit_dependencies", {}, /*withMap=*/false);
  CHECK(noMap.getString("status") == "usage_error");

  llvm::json::Object args;
  args["target_map"] = "/nonexistent/targets.json";
  auto missing = fx.runArgs("audit_dependencies", std::move(args), false);
  CHECK(missing.getString("status") == "usage_error");
  CHECK(missing.getString("error")->contains("cannot read target map"));

  CHECK(fx.run("audit_dependencies", {{"finding", "bogus"}})
            .getString("status") == "usage_error");
  CHECK(fx.run("audit_dependencies", {{"sort", "size"}}).getString("status") ==
        "usage_error");
  CHECK(fx.run("audit_dependencies", {{"consumer", "nope"}})
            .getString("status") == "not_found");
  CHECK(fx.run("explain_dependency", {{"consumer", "app"}})
            .getString("status") == "usage_error");
  CHECK(fx.run("explain_dependency",
               {{"consumer", "app"}, {"provider", "nope"}})
            .getString("status") == "not_found");
}

TEST_CASE("explain_dependency: seeds with their witnesses and the closure",
          "[dependency-audit]") {
  ToolFixture fx;
  auto out = fx.run("explain_dependency",
                    {{"consumer", "util"}, {"provider", "core"}});
  CHECK(out.getString("status") == "ok");
  CHECK(out.getObject("edge")->getInteger("symbols") == 2);
  const auto *seeds = out.getArray("seeds");
  REQUIRE(seeds != nullptr);
  REQUIRE(seeds->size() == 2);
  const auto *direct = (*seeds)[0].getAsObject();
  CHECK(direct->getObject("symbol")->getString("name") == "core_sum");
  CHECK(direct->getString("via") == "direct");
  CHECK(direct->getString("owner") == "defined");
  CHECK(direct->getString("site") == "/p/util/u.cpp:4:5");
  CHECK(direct->getObject("from")->getString("name") == "util_fmt");
  CHECK(direct->getObject("through") == nullptr);
  const auto *inl = (*seeds)[1].getAsObject();
  CHECK(inl->getObject("symbol")->getString("name") == "core_scale");
  CHECK(inl->getString("via") == "inline");
  CHECK(inl->getString("site") == "/p/core/inline.h:3:20");
  CHECK(inl->getObject("from")->getString("name") == "twice");
  REQUIRE(inl->getObject("through") != nullptr);
  CHECK(inl->getObject("through")->getString("site") == "/p/util/u.cpp:4:20");
  CHECK(inl->getObject("through")->getString("from") == "util_fmt");

  // The closure, by name: the two seeds and what they need.
  const auto *closure = out.getArray("closure");
  REQUIRE(closure != nullptr);
  std::vector<std::string> names;
  for (const auto &s : *closure)
    names.push_back(s.getAsObject()->getString("name")->str());
  CHECK(names == std::vector<std::string>{"clamp", "core_scale", "core_sum"});
  CHECK((*closure)[0].getAsObject()->getString("definedIn") ==
        "/p/core/a.cpp");
  CHECK(out.getInteger("closureTotal") == 3);
  CHECK(out.getBoolean("closureTruncated") == false);

  auto capped = fx.run("explain_dependency", {{"consumer", "util"},
                                              {"provider", "core"},
                                              {"closure_limit", 1}});
  CHECK(capped.getArray("closure")->size() == 1);
  CHECK(capped.getInteger("closureTotal") == 3);
  CHECK(capped.getBoolean("closureTruncated") == true);

  // A presumed seed says so.
  auto tool = fx.run("explain_dependency",
                     {{"consumer", "tool"}, {"provider", "core"}});
  const auto *last = (*tool.getArray("seeds"))[1].getAsObject();
  CHECK(last->getObject("symbol")->getString("name") == "core_leaf");
  CHECK(last->getString("owner") == "presumed");

  // No declared dependency and no reference: an empty edge, not an error.
  auto none = fx.run("explain_dependency",
                     {{"consumer", "extra"}, {"provider", "core"}});
  CHECK(none.getString("status") == "ok");
  CHECK(none.getObject("edge")->getString("declared") == "none");
  CHECK(none.getArray("seeds")->empty());
}

TEST_CASE("dependency audit: payloads do not depend on insertion order",
          "[dependency-audit]") {
  ToolFixture forward;
  ToolFixture backward(/*reversed=*/true);
  // Same map contents, different scratch files: compare all but the path.
  auto strip = [](llvm::json::Object o) {
    o.erase("targetMap");
    return render(llvm::json::Value(std::move(o)));
  };
  CHECK(strip(forward.run("audit_dependencies", {})) ==
        strip(backward.run("audit_dependencies", {})));
  CHECK(strip(forward.run("audit_dependencies", {{"sort", "gain"}})) ==
        strip(backward.run("audit_dependencies", {{"sort", "gain"}})));
  for (const char *consumer : {"app", "tool", "util"})
    CHECK(strip(forward.run("explain_dependency",
                            {{"consumer", consumer}, {"provider", "core"}})) ==
          strip(backward.run("explain_dependency",
                             {{"consumer", consumer}, {"provider", "core"}})));
}

TEST_CASE("dependency audit: a changed map file is read again",
          "[dependency-audit]") {
  ToolFixture fx;
  CHECK(fx.run("audit_dependencies", {}).getInteger("total") == 6);
  // Rewrite the map with a different size: the cached analysis is stale.
  {
    std::error_code ec;
    llvm::raw_fd_ostream os(fx.mapFile.path, ec);
    REQUIRE_FALSE(ec);
    os << R"({"schema": 1, "root": "/p", "targets": [
      {"name": "core", "kind": "static_library",
       "sources": ["core/a.cpp", "core/b.cpp"]},
      {"name": "util", "kind": "static_library", "sources": ["util/u.cpp"],
       "deps": ["core"]}]})";
  }
  auto out = fx.run("audit_dependencies", {});
  CHECK(out.getObject("targetMap")->getInteger("targets") == 2);
  CHECK(out.getInteger("total") == 1);
}

// ---------------------------------------------------------------------------
// Link facts from reference facts
// ---------------------------------------------------------------------------

TEST_CASE("dependency audit from references: a symbol belongs to the targets "
          "of its strong definition",
          "[dependency-audit]") {
  RefAnalysed a;
  const LinkFacts &lf = a.facts;
  CHECK(llvm::StringRef(lf.edgeSource) == "references");
  CHECK(classOf(lf, "c:@F@core_fn") == SymbolClass::Defined);
  CHECK(ownersOf(lf, "c:@F@core_fn") == std::vector<uint32_t>{rCore});
  CHECK(classOf(lf, "c:@F@base_fn") == SymbolClass::Defined);
  CHECK(ownersOf(lf, "c:@F@base_fn") == std::vector<uint32_t>{rBase});
  // A variable is a symbol like a function.
  CHECK(classOf(lf, "c:@g_table") == SymbolClass::Defined);
  CHECK(lf.symKind[symbolNamed(lf, "c:@g_table")] ==
        static_cast<uint8_t>(SymbolKind::Variable));
  // The declaration shown is the header's, not the definition.
  const uint32_t fn = symbolNamed(lf, "c:@F@core_fn");
  REQUIRE(lf.symDeclFile[fn] != kNoIndex);
  CHECK(lf.files[lf.symDeclFile[fn]] == "/q/core/core.h");
  CHECK(lf.symDeclLine[fn] == 3);
  CHECK(lf.files[lf.symFile[fn]] == "/q/core/a.cpp");
  // Vague linkage and template patterns travel with whoever includes them.
  CHECK(classOf(lf, "c:@F@core_inline") == SymbolClass::Inline);
  CHECK(ownersOf(lf, "c:@F@core_inline") == std::vector<uint32_t>{rCore});
  CHECK(classOf(lf, "c:@FT@>1#Tmake_plugin#v#") == SymbolClass::Inline);
  CHECK(classOf(lf, kLambdaUsr) == SymbolClass::Inline);
  // A class is defined where its vtable is.
  CHECK(classOf(lf, "c:@S@Shape") == SymbolClass::Defined);
  CHECK(ownersOf(lf, "c:@S@Shape") == std::vector<uint32_t>{rCore});
  CHECK(lf.files[lf.symFile[symbolNamed(lf, "c:@S@Shape")]] ==
        "/q/core/b.cpp");
  CHECK(classOf(lf, "c:plug.h@file") == SymbolClass::Unattributed);
  // plug.h is shared, and plug defines what it declares.
  CHECK(classOf(lf, "c:@F@plug_fn") == SymbolClass::Defined);
}

TEST_CASE("dependency audit from references: an undefined symbol is presumed "
          "only when its owner is not fully indexed",
          "[dependency-audit]") {
  RefAnalysed a;
  const LinkFacts &lf = a.facts;
  // half has a source the index never parsed: the definition may be there.
  CHECK(classOf(lf, "c:@F@half_fn") == SymbolClass::Presumed);
  CHECK(ownersOf(lf, "c:@F@half_fn") == std::vector<uint32_t>{rHalf});
  // core is fully indexed and defines no gone_fn: nobody provides it.
  CHECK(classOf(lf, "c:@F@gone_fn") == SymbolClass::Unattributed);
  CHECK(lf.counts.presumed == 1);
  CHECK(lf.counts.undefinedInCoveredOwner == 1);
  const AuditRow &half = rowOf(a.audit, rApp, rHalf);
  CHECK(half.symbols == 0);
  CHECK(half.uncertainSymbols == 1);
  CHECK(half.declared == DeclaredForm::None);
}

TEST_CASE("dependency audit from references: vtables, virtual calls, lambdas "
          "and file scopes",
          "[dependency-audit]") {
  RefAnalysed a;
  const LinkFacts &lf = a.facts;
  // The vtable holds the virtual function: constructing a Shape needs area.
  CHECK(lf.counts.vtableEntryRefs == 1);
  // Without a call graph every call of a virtual function is dispatched.
  CHECK(lf.counts.skippedVirtualCalls == 1);
  // A file-scope reference in a header two targets include, recorded once
  // for both, belongs to neither.
  CHECK(lf.counts.fileScopeDropped == 1);

  const AuditRow &core = rowOf(a.audit, rApp, rCore);
  // core_fn and the Shape vtable directly, core_cb through the lambda.
  CHECK(seedNames(lf, a.audit, core) ==
        std::vector<std::string>{"c:@F@core_fn", "c:@F@core_cb",
                                 "c:@S@Shape"});
  CHECK(core.symbols == 3);
  CHECK(core.viaInline == 0);
  // The seeds, core_helper, g_table and Shape::area.
  CHECK(core.closureSymbols == 6);
  CHECK(core.closureFiles == 2);
  // core_other also calls core_helper: extracting the closure keeps an edge
  // from what stays in core.
  CHECK(core.closureFanIn == 1);
  CHECK(core.closureFanInFiles == 1);
  std::vector<uint32_t> fanIn;
  closureFanInOf(lf, a.audit, core, fanIn);
  REQUIRE(fanIn.size() == 1);
  CHECK(lf.strings->resolve(lf.symUsr[fanIn[0]]) == "c:@F@core_other");
  // The dispatched call and the inline body are indirect uses of core.
  CHECK(core.indirectUses == 2);
  // plug's file scope reference never reaches app.
  const AuditRow &plug = rowOf(a.audit, rApp, rPlug);
  CHECK(seedNames(lf, a.audit, plug) ==
        std::vector<std::string>{"c:@F@plug_init"});
}

TEST_CASE("dependency audit from references: the call graph binds virtual "
          "calls and names bases and lambda callers",
          "[dependency-audit]") {
  RefAnalysed a(/*withGraph=*/true);
  // A statically bound call of area: a hard reference.
  a.graph.addNode({"main", "/q/app/main.cpp", 5, false, false, "",
                   "c:@F@main"});
  a.graph.addNode({"Shape::area", "/q/core/b.cpp", 5, true, false, "Shape",
                   "c:@S@Shape@F@area#"});
  a.graph.addEdge({"c:@F@main", "c:@S@Shape@F@area#", EdgeKind::VirtualDispatch,
                   Confidence::Proven, "/q/app/main.cpp:10:3", 0,
                   ExecutionContext::Synchronous},
                  "/q/app/main.cpp");
  // A derived class: its vtable needs the base's.
  SymbolFact circle;
  circle.usr = "c:@S@Circle";
  circle.name = "Circle";
  circle.file = "/q/core/b.cpp";
  circle.line = 20;
  circle.kind = SymbolKind::Class;
  circle.flags = kD | kSymDynamicClass | kSymVtableHome;
  a.refs.addSymbol(circle, "/q/core/b.cpp");
  a.graph.addDerivedClass("Shape", "Circle", "/q/core/b.cpp");
  // The lambda's caller, from the call graph.
  a.graph.addEdge({"c:@F@main", kLambdaUsr, EdgeKind::DirectCall,
                   Confidence::Proven, "/q/app/main.cpp:7:30", 0,
                   ExecutionContext::Synchronous},
                  "/q/app/main.cpp");
  a.rerun();
  const LinkFacts &lf = a.facts;
  CHECK(lf.counts.skippedVirtualCalls == 0);
  CHECK(lf.counts.baseClassRefs == 1);
  const AuditRow &core = rowOf(a.audit, rApp, rCore);
  CHECK(seedNames(lf, a.audit, core) ==
        std::vector<std::string>{"c:@F@core_fn", "c:@F@core_cb",
                                 "c:@S@Shape", "c:@S@Shape@F@area#"});
  // main carries its lambda: one more reference from main.
  const uint32_t mainSym = symbolNamed(lf, "c:@F@main");
  bool enclosed = false;
  for (uint32_t r = lf.refBegin[mainSym]; r < lf.refBegin[mainSym + 1]; ++r)
    enclosed |= lf.strings->resolve(lf.symUsr[lf.refTo[r]]) == kLambdaUsr &&
                std::string(lf.refKindName(lf.refKind[r])) == "enclosed_lambda";
  CHECK(enclosed);
  // Circle overrides nothing: its vtable holds Shape::area.
  CHECK(lf.counts.inheritedVirtualRefs == 1);
  const uint32_t circleSym = symbolNamed(lf, "c:@S@Circle");
  std::vector<std::string> circleRefs;
  for (uint32_t r = lf.refBegin[circleSym]; r < lf.refBegin[circleSym + 1]; ++r)
    circleRefs.push_back(std::string(lf.refKindName(lf.refKind[r])) + " " +
                         lf.strings->resolve(lf.symUsr[lf.refTo[r]]));
  // And constructing a Circle constructs a Shape: its vtable.
  CHECK(circleRefs ==
        std::vector<std::string>{"inherited_virtual c:@S@Shape@F@area#",
                                 "base_class c:@S@Shape"});
}

TEST_CASE("dependency audit from references: a dependency reached only "
          "through a declared target's inline body says whose",
          "[dependency-audit]") {
  RefAnalysed a;
  const LinkFacts &lf = a.facts;
  const AuditRow &base = rowOf(a.audit, rApp, rBase);
  CHECK(base.declared == DeclaredForm::Transitive);
  CHECK(base.symbols == 1);
  CHECK(base.viaInline == 1);
  REQUIRE(base.throughInlineRef != kNoIndex);
  CHECK(lf.strings->resolve(lf.symUsr[lf.refTo[base.throughInlineRef]]) ==
        "c:@F@core_inline");
  // A direct reference has no such excuse.
  CHECK(rowOf(a.audit, rApp, rCore).throughInlineRef == kNoIndex);
}

TEST_CASE("dependency audit from references: registration and static "
          "initializers are marked",
          "[dependency-audit]") {
  RefAnalysed a;
  const LinkFacts &lf = a.facts;
  CHECK((lf.symFlags[symbolNamed(lf, "c:@F@plug_init")] & kSymFlagRegisters) !=
        0);
  CHECK((lf.symFlags[symbolNamed(lf, "c:@F@plug_fn")] & kSymFlagRegisters) ==
        0);
  CHECK((lf.symFlags[symbolNamed(lf, "c:@extra_reg")] & kSymFlagStaticInit) !=
        0);
  CHECK((lf.symFlags[symbolNamed(lf, "c:@g_table")] & kSymFlagStaticInit) ==
        0);
  const AuditRow &plug = rowOf(a.audit, rApp, rPlug);
  CHECK(plug.symbols == 1);
  CHECK(plug.registrationSeeds == 1);
  CHECK(a.audit.providerStaticInits[rExtra] == 1);
  CHECK(a.audit.providerStaticInits[rCore] == 0);
}

namespace {

void addSym(ReferenceIndex &refs, const char *usr, const char *name,
            const char *file, uint32_t line, uint32_t flags,
            std::initializer_list<const char *> tus,
            SymbolKind kind = SymbolKind::Function,
            LinkageClass linkage = LinkageClass::External) {
  SymbolFact f;
  f.usr = usr;
  f.name = name;
  f.file = file;
  f.line = line;
  f.kind = kind;
  f.linkage = linkage;
  f.flags = flags;
  for (const char *tu : tus)
    refs.addSymbol(f, tu);
}

void addRef(ReferenceIndex &refs, const char *from, const char *to,
            RefRole role, const char *file, uint32_t line, uint32_t col,
            const char *tu) {
  ReferenceFact r;
  r.referrerUsr = from;
  r.targetUsr = to;
  r.role = role;
  r.file = file;
  r.line = line;
  r.col = col;
  refs.addReference(r, tu);
}

std::vector<std::string> needNames(const TargetMap &map,
                                   const DependencyAudit &audit,
                                   const AuditRow &row) {
  std::vector<std::string> out;
  for (uint32_t i = row.needBegin; i < row.needEnd; ++i)
    out.push_back(map.names[audit.needs[i]]);
  return out;
}

} // namespace

TEST_CASE("dependency audit from references: overload candidates are "
          "indirect uses the closure still takes",
          "[dependency-audit]") {
  RefAnalysed a;
  const char *main = "/q/app/main.cpp", *coreA = "/q/core/a.cpp";
  // Two overloads of pick in core and one in base.
  for (const char *usr : {"c:@F@pick#I#", "c:@F@pick#d#"}) {
    addSym(a.refs, usr, "pick", "/q/core/core.h", 20, kH, {main, coreA});
    addSym(a.refs, usr, "pick", coreA, 20, kD, {coreA});
  }
  addSym(a.refs, "c:@F@pick#f#", "pick", "/q/base/base.h", 3, kH, {coreA});
  addSym(a.refs, "c:@F@pick#f#", "pick", "/q/base/x.cpp", 3, kD,
         {"/q/base/x.cpp"});
  // A macro expansion: two differently named symbols at one site.
  for (const char *usr : {"c:@F@mac_a", "c:@F@mac_b"}) {
    addSym(a.refs, usr, usr + 4, "/q/core/core.h", 21, kH, {main});
    addSym(a.refs, usr, usr + 4, coreA, 21, kD, {coreA});
  }
  // main: an open overload set at 20:5, the macro's two calls at 21:1.
  addRef(a.refs, "c:@F@main", "c:@F@pick#I#", RefRole::Call, main, 20, 5,
         main);
  addRef(a.refs, "c:@F@main", "c:@F@pick#d#", RefRole::Call, main, 20, 5,
         main);
  addRef(a.refs, "c:@F@main", "c:@F@mac_a", RefRole::Call, main, 21, 1, main);
  addRef(a.refs, "c:@F@main", "c:@F@mac_b", RefRole::Call, main, 21, 1, main);
  // core_fn's own open set names candidates in core and in base.
  for (const char *usr : {"c:@F@pick#I#", "c:@F@pick#d#", "c:@F@pick#f#"})
    addRef(a.refs, "c:@F@core_fn", usr, RefRole::Call, coreA, 30, 3, coreA);
  a.rerun();
  const LinkFacts &lf = a.facts;
  CHECK(lf.counts.overloadCandidateRefs == 5);

  const AuditRow &core = rowOf(a.audit, rApp, rCore);
  // The macro's calls are seeds; the candidates are not.
  CHECK(seedNames(lf, a.audit, core) ==
        std::vector<std::string>{"c:@F@core_fn", "c:@F@core_cb",
                                 "c:@S@Shape", "c:@F@mac_a", "c:@F@mac_b"});
  // Two more indirect uses: main's candidates.
  CHECK(core.indirectUses == 4);
  // The closure takes the candidates core defines (6 + mac_a, mac_b, the
  // two picks), and base's candidate adds no need.
  CHECK(core.closureSymbols == 10);
  CHECK(needNames(a.map, a.audit, core).empty());
}

TEST_CASE("dependency audit from references: a symbol two targets define is "
          "charged to the target of the reference's site",
          "[dependency-audit]") {
  RefAnalysed a;
  // dup has a body in extra and in plug (one USR, as every tool's main).
  addSym(a.refs, "c:@F@dup", "dup", "/q/extra/e.cpp", 9, kD,
         {"/q/extra/e.cpp"});
  addSym(a.refs, "c:@F@dup", "dup", "/q/plug/p.cpp", 9, kD, {"/q/plug/p.cpp"});
  addSym(a.refs, "c:@F@base_fn", "base_fn", "/q/base/base.h", 2, kH,
         {"/q/extra/e.cpp"});
  addRef(a.refs, "c:@F@dup", "c:@F@base_fn", RefRole::Call, "/q/extra/e.cpp",
         10, 3, "/q/extra/e.cpp");
  // plug's copy calls into core instead; app calls dup.
  addRef(a.refs, "c:@F@dup", "c:@F@core_fn", RefRole::Call, "/q/plug/p.cpp",
         10, 3, "/q/plug/p.cpp");
  addSym(a.refs, "c:@F@dup", "dup", "/q/plug/plug.h", 4, kH,
         {"/q/app/main.cpp"});
  addRef(a.refs, "c:@F@main", "c:@F@dup", RefRole::Call, "/q/app/main.cpp",
         40, 3, "/q/app/main.cpp");
  a.rerun();
  CHECK(ownersOf(a.facts, "c:@F@dup") ==
        std::vector<uint32_t>{rExtra, rPlug});
  CHECK(a.facts.counts.siteAnchoredRefs == 2);
  CHECK(rowOf(a.audit, rExtra, rBase).symbols == 1);
  const uint32_t plugBase = a.audit.find(rPlug, rBase);
  CHECK((plugBase == kNoIndex || a.audit.rows[plugBase].symbols == 0));
  // Each copy's closure carries its own references only.
  CHECK(needNames(a.map, a.audit, rowOf(a.audit, rApp, rExtra)) ==
        std::vector<std::string>{"base"});
  CHECK(needNames(a.map, a.audit, rowOf(a.audit, rApp, rPlug)) ==
        std::vector<std::string>{"core"});
}

TEST_CASE("dependency audit from references: a weak definition in a header "
          "is held by every target that includes it",
          "[dependency-audit]") {
  RefAnalysed a;
  addSym(a.refs, "c:@F@weak_hook", "weak_hook", "/q/core/core.h", 30,
         kD | kH | kSymWeak, {"/q/app/main.cpp", "/q/core/a.cpp"},
         SymbolKind::Function, kVague);
  addRef(a.refs, "c:@F@weak_hook", "c:@F@extra_fn", RefRole::Call,
         "/q/core/core.h", 31, 3, "/q/core/a.cpp");
  a.rerun();
  CHECK(a.facts.counts.emittedDefinitions == 1);
  // core.h is opened by app, core and plug: each emits the definition.
  CHECK(rowOf(a.audit, rApp, rExtra).symbols == 1);
  CHECK(rowOf(a.audit, rCore, rExtra).symbols == 1);
  CHECK(rowOf(a.audit, rPlug, rExtra).symbols == 1);
}

TEST_CASE("dependency audit from references: an override keeps the base's "
          "virtual out of the derived vtable",
          "[dependency-audit]") {
  RefAnalysed a(/*withGraph=*/true);
  addSym(a.refs, "c:@S@Square", "Square", "/q/core/b.cpp", 30,
         kD | kSymDynamicClass | kSymVtableHome, {"/q/core/b.cpp"},
         SymbolKind::Class);
  addSym(a.refs, "c:@S@Square@F@area#", "Square::area", "/q/core/b.cpp", 31,
         kD | kSymVirtual | kSymMember, {"/q/core/b.cpp"});
  a.graph.addDerivedClass("Shape", "Square", "/q/core/b.cpp");
  a.rerun();
  const LinkFacts &lf = a.facts;
  CHECK(lf.counts.baseClassRefs == 1);
  CHECK(lf.counts.inheritedVirtualRefs == 0);
  const uint32_t square = symbolNamed(lf, "c:@S@Square");
  std::vector<std::string> refs;
  for (uint32_t r = lf.refBegin[square]; r < lf.refBegin[square + 1]; ++r)
    refs.push_back(std::string(lf.refKindName(lf.refKind[r])) + " " +
                   lf.strings->resolve(lf.symUsr[lf.refTo[r]]));
  CHECK(refs == std::vector<std::string>{"base_class c:@S@Shape",
                                         "vtable_entry c:@S@Square@F@area#"});
}

TEST_CASE("dependency audit from references: a split helps the executables "
          "that link the provider only through the edge",
          "[dependency-audit]") {
  TargetMap map;
  std::string error;
  REQUIRE(parseMap(R"({"schema": 1, "root": "/r", "targets": [
      {"name": "big", "kind": "static_library",
       "sources": ["big/b1.cpp", "big/b2.cpp"]},
      {"name": "exe1", "kind": "executable", "sources": ["exe1/m.cpp"],
       "deps": ["lib"]},
      {"name": "exe2", "kind": "executable", "sources": ["exe2/m.cpp"],
       "deps": ["big", "lib"]},
      {"name": "lib", "kind": "static_library", "sources": ["lib/l.cpp"],
       "deps": ["big"]}]})",
                   map, error, "/r"));
  enum : uint32_t { tBig = 0, tExe1 = 1, tExe2 = 2, tLib = 3 };
  SnapshotMeta meta = metaOf({{"/r/big/b1.cpp", {}},
                              {"/r/big/b2.cpp", {}},
                              {"/r/exe1/m.cpp", {}},
                              {"/r/exe2/m.cpp", {}},
                              {"/r/lib/l.cpp", {}}});
  ReferenceIndex refs;
  addSym(refs, "c:@F@big_a", "big_a", "/r/big/b1.cpp", 1, kD, {"/r/big/b1.cpp"});
  addSym(refs, "c:@F@big_b", "big_b", "/r/big/b1.cpp", 2, kD, {"/r/big/b1.cpp"});
  addSym(refs, "c:@F@big_c", "big_c", "/r/big/b2.cpp", 1, kD, {"/r/big/b2.cpp"});
  addSym(refs, "c:@F@lib_fn", "lib_fn", "/r/lib/l.cpp", 1, kD, {"/r/lib/l.cpp"});
  addSym(refs, "c:@F@m1", "m1", "/r/exe1/m.cpp", 1, kD, {"/r/exe1/m.cpp"});
  addSym(refs, "c:@F@m2", "m2", "/r/exe2/m.cpp", 1, kD, {"/r/exe2/m.cpp"});
  addRef(refs, "c:@F@lib_fn", "c:@F@big_a", RefRole::Call, "/r/lib/l.cpp", 2,
         3, "/r/lib/l.cpp");
  addRef(refs, "c:@F@m1", "c:@F@lib_fn", RefRole::Call, "/r/exe1/m.cpp", 2, 3,
         "/r/exe1/m.cpp");
  addRef(refs, "c:@F@m2", "c:@F@lib_fn", RefRole::Call, "/r/exe2/m.cpp", 2, 3,
         "/r/exe2/m.cpp");
  addRef(refs, "c:@F@m2", "c:@F@big_c", RefRole::Call, "/r/exe2/m.cpp", 3, 3,
         "/r/exe2/m.cpp");
  const LinkFacts lf = linkFactsFromReferences(refs, nullptr, map, &meta);
  const DependencyAudit audit = auditDependencies(lf, map);

  // lib uses one function of big: big less the closure is lib's gain.
  const AuditRow &row = rowOf(audit, tLib, tBig);
  CHECK(row.closureSymbols == 1);
  CHECK(row.droppableSymbols == 2);
  // exe1 links big only through lib; exe2 calls big itself.
  CHECK(row.linkTargetsHelped == 1);
  CHECK(row.linkTargetGain == 2);
  REQUIRE(row.helpedEnd - row.helpedBegin == 1);
  CHECK(audit.helpedTarget[row.helpedBegin] == tExe1);
  CHECK(audit.helpedSymbols[row.helpedBegin] == 2);

  // A library edge no executable gains from: exe2 → lib's closure needs
  // big, which exe2 links anyway; exe2 itself still drops lib.
  const AuditRow &exe2 = rowOf(audit, tExe2, tLib);
  CHECK(exe2.linkTargetsHelped == 1);
  CHECK(exe2.linkTargetGain == 0);
  // exe1 does not declare big: there is no edge to split.
  CHECK(audit.find(tExe1, tBig) == kNoIndex);
}

TEST_CASE("dependency audit from references: what one target's "
          "instantiations of a template reference stays with that target",
          "[dependency-audit]") {
  TargetMap map;
  std::string error;
  REQUIRE(parseMap(R"({"schema": 1, "root": "/r", "targets": [
      {"name": "a", "kind": "static_library",
       "sources": ["a/a1.cpp", "a/a2.cpp"], "deps": ["c", "e"]},
      {"name": "b", "kind": "static_library",
       "sources": ["b/b1.cpp", "b/b2.cpp"], "deps": ["d", "e"]},
      {"name": "c", "kind": "static_library", "sources": ["c/c.cpp"]},
      {"name": "d", "kind": "static_library", "sources": ["d/d.cpp"]},
      {"name": "e", "kind": "static_library", "sources": ["e/e.cpp"]}]})",
                   map, error, "/r"));
  enum : uint32_t { tA = 0, tB = 1, tC = 2, tD = 3, tE = 4 };
  ReferenceIndex refs;
  // make<T> in a shared header: its pattern holds what every
  // instantiation references. a's TUs instantiate it on c's type, b's on
  // d's; e's function is referenced by an a TU and a b TU alike.
  addSym(refs, "c:@FT@>1#Tmake#v#", "make", "/r/u/u.h", 1, kD | kH,
         {"/r/a/a1.cpp", "/r/a/a2.cpp", "/r/b/b1.cpp", "/r/b/b2.cpp"}, kFn,
         kVague);
  addSym(refs, "c:@F@a_fn", "a_fn", "/r/a/a1.cpp", 1, kD, {"/r/a/a1.cpp"});
  addSym(refs, "c:@F@b_fn", "b_fn", "/r/b/b1.cpp", 1, kD, {"/r/b/b1.cpp"});
  addSym(refs, "c:@F@c_fn", "c_fn", "/r/c/c.cpp", 1, kD, {"/r/c/c.cpp"});
  addSym(refs, "c:@F@d_fn", "d_fn", "/r/d/d.cpp", 1, kD, {"/r/d/d.cpp"});
  addSym(refs, "c:@F@e_fn", "e_fn", "/r/e/e.cpp", 1, kD, {"/r/e/e.cpp"});
  addRef(refs, "c:@F@a_fn", "c:@FT@>1#Tmake#v#", RefRole::Call,
         "/r/a/a1.cpp", 2, 3, "/r/a/a1.cpp");
  addRef(refs, "c:@F@b_fn", "c:@FT@>1#Tmake#v#", RefRole::Call,
         "/r/b/b1.cpp", 2, 3, "/r/b/b1.cpp");
  for (const char *tu : {"/r/a/a1.cpp", "/r/a/a2.cpp"})
    addRef(refs, "c:@FT@>1#Tmake#v#", "c:@F@c_fn", RefRole::Call, "/r/u/u.h",
           2, 3, tu);
  for (const char *tu : {"/r/b/b1.cpp", "/r/b/b2.cpp"})
    addRef(refs, "c:@FT@>1#Tmake#v#", "c:@F@d_fn", RefRole::Call, "/r/u/u.h",
           2, 3, tu);
  for (const char *tu : {"/r/a/a2.cpp", "/r/b/b1.cpp"})
    addRef(refs, "c:@FT@>1#Tmake#v#", "c:@F@e_fn", RefRole::Call, "/r/u/u.h",
           3, 3, tu);
  SnapshotMeta meta = metaOf({{"/r/a/a1.cpp", {"/r/u/u.h"}},
                              {"/r/a/a2.cpp", {"/r/u/u.h"}},
                              {"/r/b/b1.cpp", {"/r/u/u.h"}},
                              {"/r/b/b2.cpp", {"/r/u/u.h"}},
                              {"/r/c/c.cpp", {}},
                              {"/r/d/d.cpp", {}},
                              {"/r/e/e.cpp", {}}});
  auto symbolsOf = [](const DependencyAudit &audit, uint32_t consumer,
                      uint32_t provider) -> uint32_t {
    const uint32_t row = audit.find(consumer, provider);
    return row == kNoIndex ? 0 : audit.rows[row].symbols;
  };

  const LinkFacts lf = linkFactsFromReferences(refs, nullptr, map, &meta);
  const DependencyAudit audit = auditDependencies(lf, map);
  // c_fn's and d_fn's references are each scoped to one target's TUs;
  // e_fn's contributors span two targets and travel with the body.
  CHECK(lf.counts.tuScopedRefs == 2);
  CHECK(symbolsOf(audit, tA, tC) == 1);
  CHECK(symbolsOf(audit, tA, tD) == 0);
  CHECK(symbolsOf(audit, tB, tD) == 1);
  CHECK(symbolsOf(audit, tB, tC) == 0);
  CHECK(symbolsOf(audit, tA, tE) == 1);
  CHECK(symbolsOf(audit, tB, tE) == 1);

  // Without the index meta the TUs between two contributors are unknown:
  // only a one-TU record could scope, and every consumer of the body takes
  // all three.
  const LinkFacts bare = linkFactsFromReferences(refs, nullptr, map, nullptr);
  const DependencyAudit bareAudit = auditDependencies(bare, map);
  CHECK(bare.counts.tuScopedRefs == 0);
  CHECK(symbolsOf(bareAudit, tA, tD) == 1);
  CHECK(symbolsOf(bareAudit, tB, tC) == 1);
}

TEST_CASE("dependency audit from references: a library that supplies main "
          "is not unused",
          "[dependency-audit]") {
  TargetMap map;
  std::string error;
  REQUIRE(parseMap(R"({"schema": 1, "root": "/r", "targets": [
      {"name": "runner", "kind": "static_library",
       "sources": ["runner/main.cpp"]},
      {"name": "tests", "kind": "executable", "sources": ["tests/t.cpp"],
       "deps": ["runner", "tool"]},
      {"name": "tool", "kind": "executable", "sources": ["tool/main.cpp"],
       "deps": ["runner"]}]})",
                   map, error, "/r"));
  enum : uint32_t { tRunner = 0, tTests = 1, tTool = 2 };
  SnapshotMeta meta = metaOf({{"/r/runner/main.cpp", {}},
                              {"/r/tests/t.cpp", {}},
                              {"/r/tool/main.cpp", {}}});
  ReferenceIndex refs;
  // Clang's USR for main carries its signature.
  addSym(refs, "c:@F@main#I#**C#", "main", "/r/runner/main.cpp", 1, kD,
         {"/r/runner/main.cpp"});
  addSym(refs, "c:@F@main", "main", "/r/tool/main.cpp", 1, kD,
         {"/r/tool/main.cpp"});
  addSym(refs, "c:@F@a_test", "a_test", "/r/tests/t.cpp", 1, kD,
         {"/r/tests/t.cpp"});
  const LinkFacts lf = linkFactsFromReferences(refs, nullptr, map, &meta);
  const DependencyAudit audit = auditDependencies(lf, map);
  // No object of tests names the runner, but startup code calls its main.
  const AuditRow &tests = rowOf(audit, tTests, tRunner);
  CHECK(tests.symbols == 0);
  CHECK(tests.providesMain);
  CHECK(rowFindings(audit, lf, tests, {}, true) == 0);
  // tool has a main of its own: the runner is unused there.
  const AuditRow &tool = rowOf(audit, tTool, tRunner);
  CHECK_FALSE(tool.providesMain);
  CHECK(rowFindings(audit, lf, tool, {}, true) == kFindingUnused);
  // An executable's main is not handed to another link.
  CHECK_FALSE(rowOf(audit, tTests, tTool).providesMain);
}

TEST_CASE("audit_dependencies from references: payload members",
          "[dependency-audit]") {
  RefToolFixture fx;
  auto out = fx.run("audit_dependencies", {});
  CHECK(out.getString("status") == "ok");
  const auto *c = out.getObject("completeness");
  REQUIRE(c != nullptr);
  CHECK(c->getString("edgeSource") == "references");
  CHECK(c->getObject("references")->getInteger("fileScopeDropped") == 1);
  CHECK(c->getObject("references")->getInteger("vtableEntries") == 1);
  CHECK(c->getObject("symbols")->getInteger("undefinedInCoveredOwner") == 1);

  // extra is linked for nothing app references, but has a static
  // initializer: the finding says so.
  const auto *extra = edgeOf(out, "app", "extra");
  REQUIRE(extra != nullptr);
  CHECK(findingIds(*extra) ==
        std::vector<std::string>{"unused-link-dependency"});
  CHECK((*extra->getArray("findings"))[0].getAsObject()->getInteger(
            "providerStaticInits") == 1);
  CHECK(extra->getInteger("providerStaticInits") == 1);

  const auto *base = edgeOf(out, "app", "base");
  REQUIRE(base != nullptr);
  CHECK(findingIds(*base) ==
        std::vector<std::string>{"undeclared-link-dependency"});
  const auto *through =
      (*base->getArray("findings"))[0].getAsObject()->getObject("throughInline");
  REQUIRE(through != nullptr);
  CHECK(through->getString("symbol") == "core_inline");
  CHECK(through->getString("header") == "/q/core/core.h");
  CHECK(render(llvm::json::Value(llvm::json::Array(
            *through->getArray("owners")))) == R"(["core"])");
  CHECK(through->getString("site") == "/q/app/main.cpp:7:3");

  const auto *core = edgeOf(out, "app", "core");
  REQUIRE(core != nullptr);
  CHECK(core->getInteger("closureFanIn") == 1);
  // app is an executable: dropping extra helps app itself.
  CHECK(extra->getInteger("linkTargetsHelped") == 1);
  CHECK(extra->getInteger("linkTargetGain") == 2);
  auto byLinkGain = fx.run("audit_dependencies", {{"sort", "link_gain"}});
  CHECK(byLinkGain.getString("sort") == "link_gain");
  CHECK((*byLinkGain.getArray("edges"))[0].getAsObject()->getString(
            "provider") == "extra");
  auto bad = fx.run("audit_dependencies", {{"sort", "size"}});
  CHECK(bad.getString("status") == "usage_error");
  auto whyExtra = fx.run("explain_dependency",
                         {{"consumer", "app"}, {"provider", "extra"}});
  CHECK(render(llvm::json::Value(llvm::json::Array(
            *whyExtra.getArray("linkTargetsHelped")))) ==
        R"([{"droppableSymbols":2,"target":"app"}])");

  auto why = fx.run("explain_dependency",
                    {{"consumer", "app"}, {"provider", "core"}});
  const auto *fanIn = why.getArray("fanIn");
  REQUIRE(fanIn != nullptr);
  REQUIRE(fanIn->size() == 1);
  CHECK((*fanIn)[0].getAsObject()->getString("name") == "core_other");
  const auto *seed = (*why.getArray("seeds"))[0].getAsObject();
  CHECK(seed->getObject("symbol")->getString("kind") == "function");
  CHECK(seed->getObject("symbol")->getString("file") == "/q/core/core.h");

  auto plug = fx.run("explain_dependency",
                     {{"consumer", "app"}, {"provider", "plug"}});
  CHECK((*plug.getArray("seeds"))[0].getAsObject()->getBoolean("registers") ==
        true);
}

TEST_CASE("audit_dependencies from references: payloads do not depend on "
          "insertion order",
          "[dependency-audit]") {
  RefToolFixture forward;
  RefToolFixture backward(/*reversed=*/true);
  auto strip = [](llvm::json::Object o) {
    o.erase("targetMap");
    return render(llvm::json::Value(std::move(o)));
  };
  CHECK(strip(forward.run("audit_dependencies", {})) ==
        strip(backward.run("audit_dependencies", {})));
  for (const char *provider : {"base", "core", "plug"})
    CHECK(strip(forward.run("explain_dependency",
                            {{"consumer", "app"}, {"provider", provider}})) ==
          strip(backward.run("explain_dependency",
                             {{"consumer", "app"}, {"provider", provider}})));
}
