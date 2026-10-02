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


// test_utf8_strings.cpp — every index string is index text
// (callgraph/Utf8.h): valid UTF-8, converted from raw bytes by an exact
// escape. Found by fuzz_snapshot: a string that is not UTF-8 reached
// llvm::json::Value, which asserts in a build with assertions. Real input
// does this: a Latin-1 directory name, or a Latin-1 comment inside an if
// condition. The conversion must not change identity: two strings that
// differ in a non-UTF-8 byte stay two strings (review of PR #78).

#include "vycor/callgraph/ChannelIndex.h"
#include "vycor/callgraph/ControlFlowIndex.h"
#include "vycor/callgraph/ControlFlowOracle.h"
#include "vycor/callgraph/Snapshot.h"
#include "vycor/callgraph/Utf8.h"
#include "vycor/cli/MegascopeCli.h"
#include "vycor/query/Serialize.h"
#include "vycor/query/Tools.h"

#include "SnapshotBytes.h"

#include "clang/Tooling/CompilationDatabase.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace vycor;

namespace {

const std::string kLatin1E = "\xe9"; // 'e' acute in Latin-1: not UTF-8
// Its index text: U+10FFE9, the escape of byte 0xE9.
const std::string kLatin1EText = "\xf4\x8f\xbf\xa9";
const std::string kReplacement = "\xef\xbf\xbd"; // U+FFFD

struct Latin1Fixture {
  std::string dir;
  std::string tu;

  Latin1Fixture() {
    llvm::SmallString<128> base;
    llvm::sys::path::system_temp_directory(/*ErasedOnReboot=*/true, base);
    llvm::sys::path::append(base, "vycor-utf8-" + kLatin1E);
    llvm::SmallString<128> made;
    REQUIRE(!llvm::sys::fs::createUniqueDirectory(base, made));
    dir = std::string(made);
    tu = dir + "/t" + kLatin1E + ".cpp";
    std::ofstream(tu, std::ios::binary)
        << "int g(int);\n"
           "int f(const char *s) {\n"
           "  if (s[0] == 'A' /* caf" + kLatin1E + " */ && s[1] != 0)\n"
           "    return g(1);\n"
           "  return 0;\n"
           "}\n"
           "int main() { return f(\"x\"); }\n";
  }
  ~Latin1Fixture() { llvm::sys::fs::remove_directories(dir); }
};

/// Every tool, and every context and channel site through the dump
/// serializers: none may hand llvm::json a string that is not UTF-8.
size_t serializeEverything(const CallGraph &graph, const ControlFlowIndex &cf,
                           const ChannelIndex &channels,
                           const SnapshotMeta &meta) {
  ControlFlowOracle oracle(graph, cf);
  QueryCache cache;
  std::vector<std::string> entryPoints = {"main"};
  ToolContext ctx{graph, oracle, cf, entryPoints, &channels, &cache};
  ctx.facts = IndexFacts::of(meta, IndexFreshness::Unchecked);
  size_t n = 0;
  for (const auto &tool : getRegisteredTools()) {
    llvm::json::Object args;
    args["name"] = "f";
    args["function"] = "f";
    args["to"] = "g";
    args["from"] = "main";
    std::string out;
    llvm::raw_string_ostream os(out);
    os << runTool(tool, args, ctx);
    n += os.str().size();
  }
  cf.forEachContext([&](const CallSiteContext &c) {
    for (const std::string *s : {&c.callerName, &c.calleeName, &c.callSite,
                                 &c.callerUsr, &c.calleeUsr, &c.tuPath})
      CHECK(isValidUtf8(*s));
    for (const auto &g : c.enclosingGuards)
      n += llvm::json::Value(serializeGuard(g)).getAsObject()->size();
  });
  for (const auto &site : channels.allSites())
    n += serializeChannelSite(site).getAsObject()->size();
  return n;
}

std::string readAll(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

} // namespace

TEST_CASE("a bake from a Latin-1 path and source text holds UTF-8 only",
          "[utf8]") {
  Latin1Fixture fx;
  clang::tooling::FixedCompilationDatabase db(fx.dir, {"-std=c++17"});
  BakedIndexes baked = bakeIndexes(db, {fx.tu}, {}, /*threadCount=*/1);
  REQUIRE(baked.graph.findNode("f") != nullptr);
  CHECK(isValidUtf8(baked.graph.findNode("f")->file));

  bool sawGuard = false;
  baked.cfIndex.forEachContext([&](const CallSiteContext &c) {
    CHECK(isValidUtf8(c.callSite));
    CHECK(isValidUtf8(c.tuPath));
    for (const auto &g : c.enclosingGuards) {
      sawGuard = true;
      CHECK(isValidUtf8(g.conditionText));
      // The Latin-1 byte is escaped, not replaced: the raw text comes back.
      CHECK(g.conditionText.find("caf" + kLatin1EText) != std::string::npos);
      CHECK(fromIndexText(g.conditionText).find("caf" + kLatin1E) !=
            std::string::npos);
    }
  });
  CHECK(sawGuard);

  SnapshotMeta meta;
  meta.files = SnapshotIO::stampFiles({fx.tu});
  SnapshotIO::recordOutcomes(meta, baked.outcomes);
  CHECK(serializeEverything(baked.graph, baked.cfIndex, baked.channels,
                            meta) > 0);

  SECTION("saved, loaded both ways, and described by info and dump") {
    const std::string index = fx.dir + "/index.vycs";
    REQUIRE(SnapshotIO::save(index, baked.graph, baked.cfIndex, meta,
                             baked.channels));
    // The meta keeps the TU path as the file system spells it: warm
    // start stats it.
    auto metaOnly = SnapshotIO::load(index, nullptr, LoadMode::ReadOnly, 0);
    REQUIRE(metaOnly);
    CHECK(metaOnly->meta.files.at(0).path == fx.tu);
    for (LoadMode mode : {LoadMode::Mutable, LoadMode::ReadOnly}) {
      auto snap = SnapshotIO::load(index, nullptr, mode);
      REQUIRE(snap);
      CHECK(serializeEverything(snap->graph, snap->cfIndex, snap->channels,
                                snap->meta) > 0);
    }
    for (const char *verb : {"info", "dump"}) {
      std::string out, err;
      llvm::raw_string_ostream os(out), es(err);
      std::istringstream in;
      std::vector<std::string> args = {verb, "--index", index};
      if (std::string(verb) == "info")
        args.push_back("--files");
      CHECK(runMegascopeQueryVerb(args, os, es, in) == kExitResults);
      CHECK(os.str().find(kLatin1EText) != std::string::npos);
      CHECK(os.str().find(kReplacement) == std::string::npos);
    }

    // A query by the raw call site (as a shell passes a Latin-1 path) and
    // by its printed form find the same site, through the mapped
    // control-flow section and through the resident one.
    std::string site;
    baked.cfIndex.forEachContext([&](const CallSiteContext &c) {
      if (c.calleeName == "g")
        site = c.callSite;
    });
    REQUIRE(!site.empty());
    const std::string rawSite = fromIndexText(site);
    REQUIRE(rawSite != site);
    for (LoadMode mode : {LoadMode::Mutable, LoadMode::ReadOnly}) {
      auto snap = SnapshotIO::load(index, nullptr, mode);
      REQUIRE(snap);
      CHECK(snap->cfIndex.isMapped() == (mode == LoadMode::ReadOnly));
      CHECK(snap->cfIndex.contextsAtSite(rawSite).size() == 1);
      CHECK(snap->cfIndex.contextsAtSite(site).size() == 1);
    }
    for (const std::string &arg : {rawSite, site}) {
      std::string out, err;
      llvm::raw_string_ostream os(out), es(err);
      std::istringstream in;
      CHECK(runMegascopeQueryVerb({"query_call_site_context", "--index", index,
                                   "--call-site", arg},
                                  os, es, in) == kExitResults);
    }
  }

  SECTION("removing the TU by its raw path removes its facts") {
    CHECK(baked.graph.removeTU(fx.tu) > 0);
    CHECK(baked.cfIndex.removeTU(fx.tu) > 0);
    CHECK(baked.graph.findNode("f") == nullptr);
    CHECK(baked.cfIndex.size() == 0);
  }
}

TEST_CASE("a loaded index holds UTF-8 only whatever the file holds",
          "[utf8][snapshot]") {
  // Strings in every decoded place: an interned name, a node's inline
  // file, a guard's text in the control-flow set table, a channel site.
  CallGraph g;
  g.addNode({"ZZcaller", "ZZfile.cpp", 1, true, false, "ZZclass"},
            "ZZtu.cpp");
  g.addNode({"ZZcallee", "ZZfile.cpp", 2, false, false, ""}, "ZZtu.cpp");
  g.addEdge({"ZZcaller", "ZZcallee", EdgeKind::DirectCall,
             Confidence::Proven, "ZZfile.cpp:1:1", 0,
             ExecutionContext::Synchronous},
            "ZZtu.cpp");
  ControlFlowIndex cf;
  CallSiteContext c;
  c.callerName = "ZZcaller";
  c.calleeName = "ZZcallee";
  c.callSite = "ZZsite.cpp:1:1";
  c.tuPath = "ZZtu.cpp";
  ConditionalGuard guard;
  guard.conditionText = "ZZcond";
  guard.location = "ZZfile.cpp:1:1";
  c.enclosingGuards.push_back(guard);
  cf.addCallSiteContext(c);
  ChannelIndex ch;
  ChannelSite site;
  site.channelId = "ZZchan";
  site.siteFunctionUsr = "ZZcaller";
  site.siteFunctionDisplay = "ZZcaller";
  site.callSite = "ZZfile.cpp:1:1";
  site.tuPath = "ZZtu.cpp";
  ch.addSite(site);
  SnapshotMeta meta;
  meta.entryPoints = {"ZZentry"};

  llvm::SmallString<128> path;
  llvm::sys::fs::createUniquePath("vycor-utf8-%%%%%%.vycs", path, true);
  const std::string index(path.str());
  REQUIRE(SnapshotIO::save(index, g, cf, meta, ch));
  const std::string clean = readAll(index);
  // Damage the second byte of each occurrence of `marker` ("Z\xffsite"...).
  auto damaged = [&](const std::string &marker) {
    std::string bytes = clean;
    size_t n = 0;
    for (size_t at = bytes.find(marker); at != std::string::npos;
         at = bytes.find(marker, at + 1)) {
      bytes[at + 1] = '\xff';
      ++n;
    }
    REQUIRE(n > 0);
    testing::resealSnapshot(bytes);
    std::ofstream(index, std::ios::binary | std::ios::trunc) << bytes;
  };

  SECTION("an index string that is not UTF-8 is damage, refused at load") {
    // Index text is all a producer stores; repairing it could make it
    // equal another stored string.
    damaged("ZZ");
    for (LoadMode mode : {LoadMode::Mutable, LoadMode::ReadOnly}) {
      SnapshotLoadStats stats;
      CHECK(!SnapshotIO::load(index, &stats, mode));
      CHECK(stats.error.find("does not decode") != std::string::npos);
    }
  }

  SECTION("a mapped string is not scanned at load and still prints UTF-8") {
    damaged("ZZsite");
    CHECK(!SnapshotIO::load(index, nullptr, LoadMode::Mutable));
    auto snap = SnapshotIO::load(index, nullptr, LoadMode::ReadOnly);
    REQUIRE(snap);
    REQUIRE(snap->cfIndex.isMapped());
    size_t contexts = 0;
    snap->cfIndex.forEachContext([&](const CallSiteContext &ctx) {
      ++contexts;
      CHECK(ctx.callSite == toIndexText("Z\xffsite.cpp:1:1"));
    });
    CHECK(contexts == 1);
    CHECK(serializeEverything(snap->graph, snap->cfIndex, snap->channels,
                              snap->meta) > 0);
  }

  SECTION("the meta is left as written; what prints it converts") {
    damaged("ZZentry");
    const std::string kEntry = std::string("Z\xff") + "entry";
    for (LoadMode mode : {LoadMode::Mutable, LoadMode::ReadOnly}) {
      auto snap = SnapshotIO::load(index, nullptr, mode);
      REQUIRE(snap);
      CHECK(snap->meta.entryPoints == std::vector<std::string>{kEntry});
    }
    // info prints the meta; a tool echoes the default entry points.
    for (const char *verb : {"info", "list_entry_points"}) {
      std::string out, err;
      llvm::raw_string_ostream os(out), es(err);
      std::istringstream in;
      const int rc =
          runMegascopeQueryVerb({verb, "--index", index}, os, es, in);
      CHECK((rc == kExitResults || rc == kExitEmpty));
      CHECK(isValidUtf8(os.str()));
    }
    std::string out, err;
    llvm::raw_string_ostream os(out), es(err);
    std::istringstream in;
    REQUIRE(runMegascopeQueryVerb({"info", "--index", index}, os, es, in) ==
            kExitResults);
    CHECK(os.str().find(toIndexText(kEntry)) != std::string::npos);
  }
  std::remove(index.c_str());
}

// ----------------------------------------------------------------------------
// Identity: a string that is not UTF-8 keeps its identity in the index. Two
// paths (or USRs, which embed the file name of an internal-linkage entity)
// that differ only in a non-UTF-8 byte are two strings, not one.
// ----------------------------------------------------------------------------

namespace {

/// A temporary directory holding `files` (name -> text), removed at exit.
struct SourceDir {
  std::string dir;

  explicit SourceDir(
      const std::vector<std::pair<std::string, std::string>> &files) {
    llvm::SmallString<128> base;
    llvm::sys::path::system_temp_directory(/*ErasedOnReboot=*/true, base);
    llvm::sys::path::append(base, "vycor-utf8-id");
    llvm::SmallString<128> made;
    REQUIRE(!llvm::sys::fs::createUniqueDirectory(base, made));
    dir = std::string(made);
    for (const auto &[name, text] : files)
      std::ofstream(path(name), std::ios::binary) << text;
  }
  ~SourceDir() { llvm::sys::fs::remove_directories(dir); }

  std::string path(const std::string &name) const { return dir + "/" + name; }
};

std::vector<std::string> callerNames(const CallGraph &g,
                                     const std::string &callee) {
  std::vector<std::string> out;
  for (const auto &e : g.callersOf(callee))
    out.push_back(e.callerName);
  std::sort(out.begin(), out.end());
  return out;
}

/// The warm refresh `megascope index` runs for a dirty TU: drop its facts,
/// bake it again, absorb the result.
void refresh(SnapshotData &snap, const clang::tooling::CompilationDatabase &db,
             const std::string &tu) {
  snap.graph.removeTUs({tu});
  snap.cfIndex.removeTUs({tu});
  snap.channels.removeTUs({tu});
  BakedIndexes fresh = bakeIndexes(db, {tu}, {}, /*threadCount=*/1);
  snap.graph.absorb(fresh.graph);
  snap.cfIndex.absorb(fresh.cfIndex);
  snap.channels.absorb(fresh.channels);
}

} // namespace

TEST_CASE("two TU paths that differ in one non-UTF-8 byte stay two TUs "
          "through a warm refresh",
          "[utf8]") {
  const std::string a = "a\xe9.cpp", b = "a\xe8.cpp";
  SourceDir src({{a, "int fx();\nint ca() { return fx(); }\n"},
                 {b, "int fx();\nint cb() { return fx(); }\n"}});
  clang::tooling::FixedCompilationDatabase db(src.dir, {"-std=c++17"});
  BakedIndexes baked =
      bakeIndexes(db, {src.path(a), src.path(b)}, {}, /*threadCount=*/1);
  REQUIRE(callerNames(baked.graph, "fx") ==
          std::vector<std::string>{"ca", "cb"});

  // Through the saved index, as `megascope index` refreshes it.
  const std::string index = src.path("index.vycs");
  SnapshotMeta meta;
  meta.files = SnapshotIO::stampFiles({src.path(a), src.path(b)});
  REQUIRE(SnapshotIO::save(index, baked.graph, baked.cfIndex, meta,
                           baked.channels));
  auto snap = SnapshotIO::load(index, nullptr, LoadMode::Mutable);
  REQUIRE(snap);
  refresh(*snap, db, src.path(a));
  CHECK(snap->graph.edgeCount() == 2);
  CHECK(callerNames(snap->graph, "fx") == std::vector<std::string>{"ca", "cb"});
  size_t contexts = 0;
  snap->cfIndex.forEachContext([&](const CallSiteContext &) { ++contexts; });
  CHECK(contexts == 2);

  // And again for the other TU: neither refresh may drop the other's facts.
  refresh(*snap, db, src.path(b));
  CHECK(callerNames(snap->graph, "fx") == std::vector<std::string>{"ca", "cb"});

  // The answer a query gives from the refreshed index, saved and reloaded.
  REQUIRE(SnapshotIO::save(index, snap->graph, snap->cfIndex, meta,
                           snap->channels));
  auto ro = SnapshotIO::load(index, nullptr, LoadMode::ReadOnly);
  REQUIRE(ro);
  ControlFlowOracle oracle(ro->graph, ro->cfIndex);
  QueryCache cache;
  std::vector<std::string> entryPoints;
  ToolContext ctx{ro->graph,   oracle,        ro->cfIndex,
                  entryPoints, &ro->channels, &cache};
  const std::vector<ToolEntry> tools = getRegisteredTools();
  const ToolEntry *getCallers = nullptr;
  for (const auto &tool : tools)
    if (tool.name == "get_callers")
      getCallers = &tool;
  REQUIRE(getCallers);
  llvm::json::Object args;
  args["name"] = "fx";
  llvm::json::Value result = runTool(*getCallers, args, ctx);
  const llvm::json::Object *obj = result.getAsObject();
  REQUIRE(obj);
  CHECK(obj->getString("status") == "ok");
  const llvm::json::Array *callers = obj->getArray("callers");
  REQUIRE(callers);
  CHECK(callers->size() == 2);
}

TEST_CASE("internal-linkage functions in files whose names differ in one "
          "non-UTF-8 byte stay two nodes",
          "[utf8]") {
  const std::string a = "b\xe9.cpp", b = "b\xe8.cpp";
  SourceDir src({{a, "static int helper() { return 1; }\n"
                     "int ub() { return helper(); }\n"},
                 {b, "static int helper() { return 2; }\n"
                     "int vb() { return helper(); }\n"}});
  clang::tooling::FixedCompilationDatabase db(src.dir, {"-std=c++17"});
  BakedIndexes baked =
      bakeIndexes(db, {src.path(a), src.path(b)}, {}, /*threadCount=*/1);

  // Clang's USR for a static function carries its file name.
  std::vector<std::string> usrs = baked.graph.usrsForName("helper");
  REQUIRE(usrs.size() == 2);
  CHECK(usrs[0] != usrs[1]);
  for (const auto &usr : usrs) {
    CHECK(isValidUtf8(usr));
    // Each helper has exactly its own file's caller.
    CHECK(baked.graph.callersOf(usr).size() == 1);
  }
  // Their call sites are distinct strings as well.
  std::set<std::string> sites;
  baked.cfIndex.forEachContext(
      [&](const CallSiteContext &c) { sites.insert(c.callSite); });
  CHECK(sites.size() == 2);
}

TEST_CASE("index text round-trips every byte string", "[utf8]") {
  auto roundTrips = [](const std::string &raw) {
    const std::string text = toIndexText(raw);
    CHECK(llvm::json::isUTF8(text));
    CHECK(fromIndexText(text) == raw);
    // Index text is already a lookup key; a raw key converts to it.
    CHECK(lookupText(text) == text);
    if (!isValidUtf8(raw))
      CHECK(lookupText(raw) == text);
    CHECK(needsIndexEscape(raw) == (text != raw));
  };
  // Every one- and two-byte string.
  for (int a = 0; a < 256; ++a) {
    roundTrips(std::string(1, static_cast<char>(a)));
    for (int b = 0; b < 256; ++b)
      roundTrips(std::string{static_cast<char>(a), static_cast<char>(b)});
  }
  // Ordinary text is unchanged.
  for (const std::string s : {"", "plain", "caf\xc3\xa9", "\xe2\x82\xac",
                              "\xf0\x9f\x98\x80", "\xf4\x8f\xbd\xbf"})
    CHECK(toIndexText(s) == s);
  // The escape range itself: a literal U+10FF80..U+10FFFF is escaped byte
  // by byte, so it stays distinct from the byte it would stand for.
  const std::string literal = "\xf4\x8f\xbf\xa9"; // U+10FFE9
  roundTrips(literal);
  CHECK(toIndexText(literal) != literal);
  CHECK(toIndexText(literal) != toIndexText("\xe9"));
  CHECK(toIndexText("\xe9") == literal);
  // Overlongs, surrogates, above U+10FFFF, truncated sequences.
  for (const std::string s : {"\xc0\xaf", "\xe0\x80\xaf", "\xed\xa0\x80",
                              "\xf4\x90\x80\x80", "\xf0\x9f\x98",
                              "a\xe9"
                              "b\xe8"
                              "c",
                              "\xff\xfe"})
    roundTrips(s);
  // Pseudo-random strings drawn from the bytes that matter.
  const unsigned char alphabet[] = {'a',  '/',  0x80, 0x8f, 0xbe,
                                    0xbf, 0xc3, 0xa9, 0xe9, 0xed,
                                    0xf0, 0xf4, 0xff, 0x9f, 0xa0};
  uint32_t seed = 12345;
  for (int i = 0; i < 20000; ++i) {
    std::string s;
    const size_t len = (seed >> 3) % 12;
    for (size_t k = 0; k < len; ++k) {
      seed = seed * 1103515245u + 12345u;
      s.push_back(static_cast<char>(alphabet[(seed >> 16) % sizeof alphabet]));
    }
    seed = seed * 1103515245u + 12345u;
    roundTrips(s);
  }
}
