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

// test_reference_tools.cpp — the symbol reference facts as a query sees
// them (docs/symbol-references.md): the lookup_symbol and get_references
// payloads and their result contract, and the snapshot's references
// section (round trip, section-selective load, damage, merge, refresh).

#include "SnapshotBytes.h"

#include "vycor/callgraph/ControlFlowIndex.h"
#include "vycor/callgraph/ControlFlowOracle.h"
#include "vycor/callgraph/ReferenceIndex.h"
#include "vycor/callgraph/Snapshot.h"
#include "vycor/query/Tools.h"

#include <catch2/catch_test_macros.hpp>
#include <clang/Tooling/CompilationDatabase.h>

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace vycor;

namespace {

// Three TUs over one header: `total` is written by a.cpp and b.cpp and
// read by c.cpp; `helper` is defined in a.cpp and called only there;
// `shared` is defined in a.cpp and called from b.cpp; each of a.cpp and
// b.cpp has its own file-local `local`.
struct Fixture {
  std::string dir, a, b, c;

  Fixture() {
    llvm::SmallString<128> tmp;
    REQUIRE_FALSE(llvm::sys::fs::createUniqueDirectory("vycor_reftools", tmp));
    llvm::SmallString<128> real;
    REQUIRE_FALSE(llvm::sys::fs::real_path(tmp, real));
    dir = std::string(real.str());
    write("api.h", R"cpp(
extern int total;
void helper();
void shared();
using Hook = void (*)();
void install(Hook);
)cpp");
    a = write("a.cpp", R"cpp(
#include "api.h"
int total = 0;
static int local() { return 1; }
void helper() { total += local(); }
void shared() { helper(); total = 2; }
)cpp");
    b = write("b.cpp", R"cpp(
#include "api.h"
static int local() { return 2; }
void install(Hook) {}
void useB() {
  shared();
  total = local();
  install(&shared);
}
)cpp");
    c = write("c.cpp", R"cpp(
#include "api.h"
int readC() { return total; }
)cpp");
  }
  Fixture(const Fixture &) = delete;
  Fixture &operator=(const Fixture &) = delete;
  ~Fixture() { llvm::sys::fs::remove_directories(dir); }

  std::string write(const std::string &name, const std::string &code) const {
    std::ofstream out(dir + "/" + name);
    out << code;
    return dir + "/" + name;
  }

  BakedIndexes bake(std::vector<std::string> files, unsigned threads = 1) const {
    clang::tooling::FixedCompilationDatabase compDb(
        dir, {"-std=c++17", "-I" + dir});
    return bakeIndexes(compDb, files, {}, threads);
  }
  BakedIndexes bake() const { return bake({a, b, c}); }

  std::string scratch(const std::string &name) const {
    return dir + "/" + name;
  }
};

using O = llvm::json::Object;

std::string serialized(const llvm::json::Value &v) {
  std::string s;
  llvm::raw_string_ostream os(s);
  os << v;
  os.flush();
  return s;
}

/// Runs one tool over `refs` (null: a context without reference facts).
struct Runner {
  CallGraph graph;
  ControlFlowIndex cfIndex;
  ControlFlowOracle oracle{graph, cfIndex};
  std::vector<std::string> entryPoints;
  std::vector<ToolEntry> tools = getRegisteredTools();
  const ReferenceIndex *refs;

  explicit Runner(const ReferenceIndex *r) : refs(r) {}

  llvm::json::Object run(llvm::StringRef tool, llvm::json::Object args) {
    ToolContext ctx{graph, oracle, cfIndex, entryPoints};
    ctx.facts.freshness = IndexFreshness::Baked;
    ctx.references = refs;
    for (const ToolEntry &t : tools) {
      if (t.name != tool)
        continue;
      llvm::json::Value v = runTool(t, args, ctx);
      REQUIRE(v.getAsObject() != nullptr);
      return *v.getAsObject();
    }
    FAIL("no tool named " << tool.str());
    return {};
  }
};

std::string str(const llvm::json::Object &o, llvm::StringRef key) {
  auto v = o.getString(key);
  REQUIRE(v.has_value());
  return v->str();
}

/// "referrer:role:count" for each record, in the order answered.
std::vector<std::string> referrers(const llvm::json::Object &payload) {
  std::vector<std::string> out;
  const auto *records = payload.getArray("references");
  REQUIRE(records != nullptr);
  for (const llvm::json::Value &v : *records) {
    const auto *r = v.getAsObject();
    REQUIRE(r != nullptr);
    out.push_back(str(*r, "referrer") + ":" + str(*r, "role") + ":" +
                  std::to_string(*r->getInteger("count")));
  }
  return out;
}

std::vector<std::string> targets(const llvm::json::Object &payload) {
  std::vector<std::string> out;
  const auto *records = payload.getArray("references");
  REQUIRE(records != nullptr);
  for (const llvm::json::Value &v : *records) {
    const auto *r = v.getAsObject();
    REQUIRE(r != nullptr);
    out.push_back(str(*r, "target") + ":" + str(*r, "role"));
  }
  return out;
}

std::string fileBytes(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

} // namespace

// ============================================================================
// lookup_symbol
// ============================================================================

TEST_CASE("lookup_symbol reports linkage and declarations and use",
          "[references][tools]") {
  Fixture f;
  auto ix = f.bake();
  Runner r(&ix.references);

  SECTION("a global defined in one TU and used from others") {
    auto p = r.run("lookup_symbol", O{{"name", "total"}});
    CHECK(str(p, "status") == "ok");
    CHECK(str(p, "qualifiedName") == "total");
    CHECK(str(p, "kind") == "variable");
    CHECK(str(p, "linkage") == "external");
    CHECK(p.getInteger("sizeBytes") == 4);
    CHECK(p.get("bodyLines") == nullptr);
    CHECK(p.getBoolean("declaredInHeader") == true);
    CHECK(p.getInteger("definingTuCount") == 1);
    CHECK(str(p, "definingTu") == f.a);
    CHECK(p.getBoolean("referencedFromOtherTu") == true);
    const auto *decls = p.getArray("declarations");
    REQUIRE(decls != nullptr);
    REQUIRE(decls->size() == 2);
    // Declarations in (file, line) order: a.cpp's definition, then the
    // header every TU sees.
    const auto *def = (*decls)[0].getAsObject();
    const auto *hdr = (*decls)[1].getAsObject();
    CHECK(str(*def, "file") == f.a);
    CHECK(def->getBoolean("definition") == true);
    CHECK(def->getBoolean("inHeader") == false);
    CHECK(str(*hdr, "file") == f.dir + "/api.h");
    CHECK(hdr->getBoolean("definition") == false);
    CHECK(hdr->getBoolean("inHeader") == true);
    CHECK(hdr->getInteger("tuCount") == 3);
    const auto *use = p.getObject("referrers");
    REQUIRE(use != nullptr);
    CHECK(use->getInteger("write") == 3); // helper, shared, useB
    CHECK(use->getInteger("read") == 2);  // helper's +=, readC
    CHECK(use->getInteger("call") == 0);
    CHECK(use->getInteger("address_of") == 0);
    CHECK(use->getInteger("vtable_use") == 0);
    CHECK(use->getInteger("typeinfo_use") == 0);
  }

  SECTION("a function only its own TU uses") {
    auto p = r.run("lookup_symbol", O{{"name", "helper"}});
    CHECK(str(p, "kind") == "function");
    CHECK(str(p, "linkage") == "external");
    CHECK(p.getInteger("bodyLines") == 1);
    CHECK(p.get("sizeBytes") == nullptr);
    CHECK(p.getBoolean("declaredInHeader") == true);
    CHECK(p.getBoolean("referencedFromOtherTu") == false);
    CHECK(p.getObject("referrers")->getInteger("call") == 1);
  }

  SECTION("a function another TU calls and takes the address of") {
    auto p = r.run("lookup_symbol", O{{"name", "shared"}});
    CHECK(p.getBoolean("referencedFromOtherTu") == true);
    CHECK(p.getObject("referrers")->getInteger("call") == 1);
    CHECK(p.getObject("referrers")->getInteger("address_of") == 1);
  }

  SECTION("a name two TUs each define privately is ambiguous") {
    auto p = r.run("lookup_symbol", O{{"name", "local"}});
    CHECK(str(p, "status") == "ambiguous");
    CHECK(p.getBoolean("ambiguous") == true);
    CHECK(p.getInteger("total_candidates") == 2);
    const auto *cands = p.getArray("candidates");
    REQUIRE(cands != nullptr);
    REQUIRE(cands->size() == 2);
    // Candidates in usr order, each with where it is defined.
    const auto *c0 = (*cands)[0].getAsObject();
    const auto *c1 = (*cands)[1].getAsObject();
    CHECK(str(*c0, "usr") < str(*c1, "usr"));
    CHECK(str(*c0, "linkage") == "internal");
    CHECK(str(*c0, "file") == f.a);
    CHECK(str(*c1, "file") == f.b);

    // The usr of a candidate, or a filter that leaves one, resolves it.
    auto byUsr = r.run("lookup_symbol", O{{"usr", str(*c1, "usr")}});
    CHECK(str(byUsr, "status") == "ok");
    CHECK(str(byUsr, "definingTu") == f.b);
    auto byFilter =
        r.run("lookup_symbol", O{{"name", "local"}, {"filter", "a.cpp"}});
    CHECK(str(byFilter, "status") == "ok");
    CHECK(str(byFilter, "linkage") == "internal");
    CHECK(str(byFilter, "definingTu") == f.a);
    // A filter that matches nothing changes nothing.
    auto noMatch =
        r.run("lookup_symbol", O{{"name", "local"}, {"filter", "zzz"}});
    CHECK(str(noMatch, "status") == "ambiguous");
    CHECK(noMatch.getInteger("total_candidates") == 2);
  }

  SECTION("errors carry a typed status") {
    CHECK(str(r.run("lookup_symbol", O{{"name", "nope"}}), "status") ==
          "not_found");
    CHECK(str(r.run("lookup_symbol", O{{"usr", "c:@F@nope#"}}), "status") ==
          "not_found");
    CHECK(str(r.run("lookup_symbol", O{}), "status") == "usage_error");
    Runner none(nullptr);
    CHECK(str(none.run("lookup_symbol", O{{"name", "total"}}), "status") ==
          "unavailable");
    CHECK(str(none.run("get_references", O{{"name", "total"}}), "status") ==
          "unavailable");
  }
}

// ============================================================================
// get_references
// ============================================================================

TEST_CASE("get_references lists referrers and targets in a fixed order",
          "[references][tools]") {
  Fixture f;
  auto ix = f.bake();
  Runner r(&ix.references);

  SECTION("to: one record per referrer and role with where it came from") {
    auto p = r.run("get_references", O{{"name", "total"}});
    CHECK(str(p, "status") == "ok");
    CHECK(str(p, "direction") == "to");
    CHECK(str(p, "kind") == "variable");
    CHECK(p.getBoolean("referencedFromOtherTu") == true);
    CHECK(p.getInteger("total") == 5);
    CHECK(p.getBoolean("truncated") == false);
    // Referrer usr order, then role order (read before write).
    CHECK(referrers(p) ==
          std::vector<std::string>{"helper:read:1", "helper:write:1",
                                   "readC:read:1", "shared:write:1",
                                   "useB:write:1"});
    const auto *records = p.getArray("references");
    const auto *helperRead = (*records)[0].getAsObject();
    CHECK(str(*helperRead, "referrerKind") == "function");
    CHECK(str(*helperRead, "file") == f.a);
    CHECK(helperRead->getInteger("line") == 5);
    CHECK(helperRead->getInteger("tuCount") == 1);
    CHECK(str(*helperRead, "firstTu") == f.a);
    CHECK(helperRead->getBoolean("fromOtherTu") == false);
    const auto *readC = (*records)[2].getAsObject();
    CHECK(str(*readC, "firstTu") == f.c);
    CHECK(readC->getBoolean("fromOtherTu") == true);
  }

  SECTION("roles filter before the page is cut") {
    auto writes = r.run("get_references",
                        O{{"name", "total"},
                         {"roles", llvm::json::Array{"write"}}});
    CHECK(writes.getInteger("total") == 3);
    CHECK(referrers(writes) ==
          std::vector<std::string>{"helper:write:1", "shared:write:1",
                                   "useB:write:1"});
    auto page = r.run("get_references",
                      O{{"name", "total"},
                       {"roles", llvm::json::Array{"write"}},
                       {"limit", 1},
                       {"offset", 1}});
    CHECK(page.getInteger("total") == 3);
    CHECK(page.getInteger("returned") == 1);
    CHECK(page.getBoolean("truncated") == true);
    CHECK(page.getInteger("nextOffset") == 2);
    CHECK(referrers(page) == std::vector<std::string>{"shared:write:1"});
    // The verdict covers the whole filtered set, not the page.
    CHECK(page.getBoolean("referencedFromOtherTu") == true);
    auto local = r.run("get_references",
                       O{{"name", "helper"},
                        {"roles", llvm::json::Array{"call"}}});
    CHECK(local.getBoolean("referencedFromOtherTu") == false);
  }

  SECTION("from: what a function body references") {
    auto p = r.run("get_references",
                   O{{"name", "useB"}, {"direction", "from"}});
    CHECK(str(p, "direction") == "from");
    CHECK(p.get("referencedFromOtherTu") == nullptr);
    auto got = targets(p);
    CHECK(got.size() == 5);
    for (const char *want :
         {"shared:call", "shared:address_of", "total:write", "local:call",
          "install:call"})
      CHECK(std::count(got.begin(), got.end(), want) == 1);
    const auto *first = (*p.getArray("references"))[0].getAsObject();
    CHECK(first->get("fromOtherTu") == nullptr);
    CHECK(first->get("referrer") == nullptr);
  }

  SECTION("an empty answer is ok and empty") {
    auto p = r.run("get_references",
                   O{{"name", "readC"},
                    {"roles", llvm::json::Array{"vtable_use"}}});
    CHECK(str(p, "status") == "ok");
    CHECK(p.getInteger("total") == 0);
    CHECK(p.getArray("references")->empty());
  }

  SECTION("bad arguments are usage errors") {
    auto status = [&](O args) {
      return str(r.run("get_references", std::move(args)), "status");
    };
    CHECK(status(O{{"name", "total"}, {"direction", "sideways"}}) ==
          "usage_error");
    CHECK(status(O{{"name", "total"}, {"direction", 3}}) == "usage_error");
    CHECK(status(O{{"name", "total"},
                  {"roles", llvm::json::Array{"observe"}}}) == "usage_error");
    CHECK(status(O{{"name", "total"}, {"roles", "write"}}) == "usage_error");
    CHECK(status(O{{"name", "total"}, {"roles", llvm::json::Array{}}}) ==
          "usage_error");
    CHECK(status(O{{"name", "total"}, {"limit", 0}}) == "usage_error");
    CHECK(status(O{}) == "usage_error");
    CHECK(status(O{{"name", "nope"}}) == "not_found");
    CHECK(status(O{{"name", "local"}}) == "ambiguous");
  }

  SECTION("the tools read the references section alone") {
    for (const ToolEntry &t : r.tools)
      if (t.name == "lookup_symbol" || t.name == "get_references")
        CHECK(t.needs == kSectionReferences);
  }
}

// ============================================================================
// The snapshot's references section
// ============================================================================

TEST_CASE("a saved index answers the reference tools as the bake did",
          "[references][snapshot]") {
  Fixture f;
  auto ix = f.bake();
  const std::string path = f.scratch("refs.vycs");
  SnapshotMeta meta;
  meta.files = {{f.a, 1, 1}, {f.b, 1, 1}, {f.c, 1, 1}};
  REQUIRE(SnapshotIO::save(path, ix.graph, ix.cfIndex, meta, ix.channels,
                           ix.references));

  Runner baked(&ix.references);
  const llvm::json::Object lookupArgs{{"name", "total"}};
  const llvm::json::Object refsArgs{{"name", "total"}};
  const std::string wantLookup =
      serialized(baked.run("lookup_symbol", lookupArgs));
  const std::string wantRefs =
      serialized(baked.run("get_references", refsArgs));

  SECTION("read-only load of the references section alone") {
    SnapshotLoadStats stats;
    auto snap =
        SnapshotIO::load(path, &stats, LoadMode::ReadOnly, kSectionReferences);
    REQUIRE(snap.has_value());
    CHECK(snap->loaded == kSectionReferences);
    CHECK(snap->graph.nodeCount() == 0);
    CHECK(snap->cfIndex.size() == 0);
    CHECK(snap->summary.symbols == ix.references.symbolCount());
    CHECK(snap->summary.references == ix.references.referenceCount());
    CHECK(snap->summary.references > 0);
    // Every other index section was skipped, not decoded.
    unsigned others = 0;
    for (const auto &sec : stats.sections) {
      const llvm::StringRef name(sec.name);
      if (name == "graph" || name == "control_flow" || name == "channels") {
        ++others;
        CHECK(sec.skipped);
      }
    }
    CHECK(others == 3);
    Runner loaded(&snap->references);
    CHECK(serialized(loaded.run("lookup_symbol", lookupArgs)) == wantLookup);
    CHECK(serialized(loaded.run("get_references", refsArgs)) == wantRefs);
    // It holds no contributor lists, so it cannot be written back.
    std::string error;
    CHECK_FALSE(SnapshotIO::save(f.scratch("again.vycs"), snap->graph,
                                 snap->cfIndex, meta, snap->channels,
                                 snap->references, &error));
    CHECK(error.find("read-only") != std::string::npos);
    CHECK_FALSE(llvm::sys::fs::exists(f.scratch("again.vycs")));
  }

  SECTION("a load that does not ask for the section leaves it empty") {
    auto snap =
        SnapshotIO::load(path, nullptr, LoadMode::ReadOnly, kSectionGraph);
    REQUIRE(snap.has_value());
    CHECK(snap->references.empty());
    // The header still says what the file holds.
    CHECK(snap->summary.symbols == ix.references.symbolCount());
    CHECK(snap->summary.references == ix.references.referenceCount());
  }

  SECTION("mutable load, refresh of one TU, and save again") {
    auto snap = SnapshotIO::load(path, nullptr, LoadMode::Mutable);
    REQUIRE(snap.has_value());
    CHECK(snap->references.removeTU(f.b) > 0);
    {
      // Without b.cpp nothing outside a.cpp calls `shared`.
      Runner partial(&snap->references);
      auto p = partial.run("lookup_symbol", O{{"name", "shared"}});
      CHECK(p.getBoolean("referencedFromOtherTu") == false);
    }
    auto again = f.bake({f.b});
    snap->references.absorb(again.references);
    Runner refreshed(&snap->references);
    CHECK(serialized(refreshed.run("lookup_symbol", lookupArgs)) ==
          wantLookup);
    CHECK(serialized(refreshed.run("get_references", refsArgs)) == wantRefs);

    // The refreshed index saves to the same bytes as the cold one.
    const std::string path2 = f.scratch("refs2.vycs");
    REQUIRE(SnapshotIO::save(path2, snap->graph, snap->cfIndex, meta,
                             snap->channels, snap->references));
    auto cold = SnapshotIO::load(path, nullptr, LoadMode::Mutable);
    auto warm = SnapshotIO::load(path2, nullptr, LoadMode::Mutable);
    REQUIRE(cold.has_value());
    REQUIRE(warm.has_value());
    std::string coldBytes, warmBytes;
    cold->references.encode(coldBytes);
    warm->references.encode(warmBytes);
    CHECK(coldBytes == warmBytes);
  }

  SECTION("damage inside the section is named") {
    std::string bytes = fileBytes(path);
    bool found = false;
    for (size_t i = 0; i < testing::kSnapshotSections; ++i) {
      const size_t at = testing::snapshotTableEntryAt(i);
      if (static_cast<uint8_t>(bytes[at]) != 4)
        continue;
      found = true;
      const uint64_t offset = testing::snapshotU64At(bytes, at + 1);
      const uint64_t length = testing::snapshotU64At(bytes, at + 9);
      REQUIRE(length > 0);
      bytes[offset + length / 2] ^= 0x01;
    }
    REQUIRE(found);
    std::ofstream(path, std::ios::binary | std::ios::trunc) << bytes;
    SnapshotLoadStats stats;
    CHECK_FALSE(SnapshotIO::load(path, &stats, LoadMode::ReadOnly,
                                 kSectionReferences));
    CHECK(stats.error.find("section 'references' checksum mismatch") !=
          std::string::npos);
    // A load that never reads the section does not trip over it.
    CHECK(SnapshotIO::load(path, nullptr, LoadMode::ReadOnly, kSectionGraph));
    // `info`'s whole-file verification does.
    std::string verifyError;
    CHECK_FALSE(SnapshotIO::verify(path, &verifyError));
    CHECK(verifyError.find("references") != std::string::npos);
  }
}

TEST_CASE("merging per-TU shards gives the references of one bake",
          "[references][snapshot]") {
  Fixture f;
  auto whole = f.bake();
  Runner want(&whole.references);

  std::vector<std::string> shards;
  for (const std::string &tu : {f.c, f.a, f.b}) {
    auto one = f.bake({tu});
    SnapshotMeta meta;
    meta.files = {{tu, 1, 1}};
    meta.outcomes = {{TuStatus::Indexed, ""}};
    meta.fingerprints = {"fp"};
    shards.push_back(tu + ".vycs");
    REQUIRE(SnapshotIO::save(shards.back(), one.graph, one.cfIndex, meta,
                             one.channels, one.references));
  }
  // A diamond: the b.cpp shard is listed twice.
  shards.push_back(shards.back());

  std::string error;
  auto merged = SnapshotIO::merge(shards, {}, &error);
  INFO(error);
  REQUIRE(merged.has_value());
  CHECK(merged->summary.symbols == whole.references.symbolCount());
  CHECK(merged->summary.references == whole.references.referenceCount());

  Runner got(&merged->references);
  for (const char *name : {"total", "helper", "shared", "install", "useB"}) {
    const llvm::json::Object args{{"name", name}};
    INFO(name);
    CHECK(serialized(got.run("lookup_symbol", args)) ==
          serialized(want.run("lookup_symbol", args)));
    CHECK(serialized(got.run("get_references", args)) ==
          serialized(want.run("get_references", args)));
    const llvm::json::Object from{{"name", name}, {"direction", "from"}};
    CHECK(serialized(got.run("get_references", from)) ==
          serialized(want.run("get_references", from)));
  }
}

// Two shards that both hold b.cpp, baked from different contents of it:
// the first shard listed supplies b.cpp's facts, and nothing of the other
// shard's view of b.cpp survives.
TEST_CASE("merging overlapping shards keeps one shard's facts per TU",
          "[references][snapshot]") {
  Fixture f;
  const auto shard = [&](const std::string &name,
                         const std::vector<std::string> &tus) {
    auto baked = f.bake(tus);
    SnapshotMeta meta;
    for (const std::string &tu : tus) {
      meta.files.push_back({tu, 1, 1});
      meta.outcomes.push_back({TuStatus::Indexed, ""});
      meta.fingerprints.push_back("fp");
    }
    const std::string path = f.scratch(name);
    REQUIRE(SnapshotIO::save(path, baked.graph, baked.cfIndex, meta,
                             baked.channels, baked.references));
    return path;
  };
  f.write("b.cpp", "#include \"api.h\"\nvoid useB() { shared(); }\n");
  const std::string ab = shard("ab.vycs", {f.a, f.b});
  f.write("b.cpp", "#include \"api.h\"\nvoid useB() { total = 7; }\n");
  const std::string bc = shard("bc.vycs", {f.b, f.c});

  const auto has = [](const std::vector<std::string> &list,
                      const std::string &item) {
    return std::find(list.begin(), list.end(), item) != list.end();
  };
  const O toShared{{"name", "shared"}};
  const O toTotal{{"name", "total"}};

  SECTION("ab then bc: b.cpp as ab saw it") {
    std::string error;
    auto merged = SnapshotIO::merge({ab, bc}, {}, &error);
    INFO(error);
    REQUIRE(merged.has_value());
    Runner got(&merged->references);
    CHECK(has(referrers(got.run("get_references", toShared)), "useB:call:1"));
    const auto total = referrers(got.run("get_references", toTotal));
    CHECK_FALSE(has(total, "useB:write:1"));
    CHECK(has(total, "readC:read:1")); // c.cpp came from the second shard
    CHECK(has(total, "shared:write:1"));
  }
  SECTION("bc then ab: b.cpp as bc saw it") {
    std::string error;
    auto merged = SnapshotIO::merge({bc, ab}, {}, &error);
    INFO(error);
    REQUIRE(merged.has_value());
    Runner got(&merged->references);
    const auto total = referrers(got.run("get_references", toTotal));
    CHECK(has(total, "useB:write:1"));
    CHECK(has(total, "readC:read:1"));
    CHECK(has(total, "shared:write:1")); // a.cpp came from the second shard
    CHECK_FALSE(
        has(referrers(got.run("get_references", toShared)), "useB:call:1"));
  }
}
