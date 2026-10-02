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


// test_utf8_strings.cpp — every index string is valid UTF-8
// (callgraph/Utf8.h). Found by fuzz_snapshot: a string that is not
// reached llvm::json::Value, which asserts in a build with assertions
// (and silently replaces the bytes without them). Real input does this:
// a Latin-1 directory name, or a Latin-1 comment inside an if condition.

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

#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

using namespace vycor;

namespace {

const std::string kLatin1E = "\xe9"; // 'e' acute in Latin-1: not UTF-8

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
      // The Latin-1 byte reads as U+FFFD, as llvm::json would print it.
      CHECK(g.conditionText.find("caf\xef\xbf\xbd") != std::string::npos);
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
      CHECK(os.str().find("\xef\xbf\xbd") != std::string::npos);
    }
  }

  SECTION("removing the TU by its raw path removes its facts") {
    CHECK(baked.graph.removeTU(fx.tu) > 0);
    CHECK(baked.cfIndex.removeTU(fx.tu) > 0);
    CHECK(baked.graph.findNode("f") == nullptr);
    CHECK(baked.cfIndex.size() == 0);
  }
}

TEST_CASE("a loaded index's strings are made UTF-8 whatever the file holds",
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
  meta.entryPoints = {"ZZcaller"};

  llvm::SmallString<128> path;
  llvm::sys::fs::createUniquePath("vycor-utf8-%%%%%%.vycs", path, true);
  const std::string index(path.str());
  REQUIRE(SnapshotIO::save(index, g, cf, meta, ch));
  std::string bytes = readAll(index);
  // Damage the second byte of every marker: "Z\xffcaller" and so on.
  size_t damaged = 0;
  for (size_t at = bytes.find("ZZ"); at != std::string::npos;
       at = bytes.find("ZZ", at + 2)) {
    bytes[at + 1] = '\xff';
    ++damaged;
  }
  REQUIRE(damaged >= 8);
  testing::resealSnapshot(bytes);
  std::ofstream(index, std::ios::binary) << bytes;

  for (LoadMode mode : {LoadMode::Mutable, LoadMode::ReadOnly}) {
    auto snap = SnapshotIO::load(index, nullptr, mode);
    REQUIRE(snap);
    for (const CallGraphNode *n : snap->graph.allNodes()) {
      CHECK(isValidUtf8(n->usr));
      CHECK(isValidUtf8(n->qualifiedName));
      CHECK(isValidUtf8(n->file));
      CHECK(isValidUtf8(n->enclosingClass));
    }
    size_t contexts = 0;
    snap->cfIndex.forEachContext([&](const CallSiteContext &ctx) {
      ++contexts;
      CHECK(ctx.callerName == "Z\xef\xbf\xbd" "caller");
      CHECK(ctx.callSite == "Z\xef\xbf\xbdsite.cpp:1:1");
      REQUIRE(ctx.enclosingGuards.size() == 1);
      CHECK(ctx.enclosingGuards[0].conditionText == "Z\xef\xbf\xbd" "cond");
    });
    CHECK(contexts == 1);
    for (const auto &s : snap->channels.allSites()) {
      CHECK(isValidUtf8(s.channelId));
      CHECK(isValidUtf8(s.tuPath));
    }
    CHECK(serializeEverything(snap->graph, snap->cfIndex, snap->channels,
                              snap->meta) > 0);
  }
  // The meta is left as written; `info` converts what it prints.
  std::string out, err;
  llvm::raw_string_ostream os(out), es(err);
  std::istringstream in;
  CHECK(runMegascopeQueryVerb({"info", "--index", index}, os, es, in) ==
        kExitResults);
  CHECK(os.str().find("Z\xef\xbf\xbd" "caller") != std::string::npos);
  std::remove(index.c_str());
}
