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

// test_hot_path_effects.cpp — Effects reachable from hot roots
// (docs/checks/hot-path-effects.md): throw edges, the llvm-cov profile
// reader, each root source, each effect, the profile's weights and
// evidence, the allow list, the witness choice and its order, the org
// config key, the tool and the check's diagnostics.

#include "vycor/callgraph/CallGraph.h"
#include "vycor/callgraph/CallGraphBuilder.h"
#include "vycor/callgraph/ControlFlowIndex.h"
#include "vycor/callgraph/ControlFlowOracle.h"
#include "vycor/callgraph/CoverageProfile.h"
#include "vycor/callgraph/Effects.h"
#include "vycor/callgraph/HotPath.h"
#include "vycor/check/HotPathEffects.h"
#include "vycor/check/Report.h"
#include "vycor/ext/Extensions.h"
#include "vycor/ext/OrgConfig.h"
#include "vycor/query/Tools.h"

#include "llvm/Support/JSON.h"

#include <catch2/catch_test_macros.hpp>
#include <clang/Tooling/Tooling.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

using namespace vycor;

namespace {

// Declarations the effect table knows, spelled out so the tests do not
// depend on the host's standard library.
const char *const kPrelude = R"(using size_t = decltype(sizeof 0);
namespace std { struct mutex { void lock(); void unlock(); }; }
extern "C" int printf(const char *, ...);
extern "C" void *malloc(size_t);
struct Error {};
)";

// One function per line, so a profile body can be a whole line.
const char *const kProgram = R"(std::mutex mu;
void lock_it() { mu.lock(); mu.unlock(); }
void log_it() { printf("x"); }
void raise_it() { throw Error(); }
int *alloc_it() { return new int(1); }
void hot() { lock_it(); log_it(); raise_it(); alloc_it(); }
void cold() { alloc_it(); }
int main() { hot(); cold(); }
)";

void build(CallGraph &graph, const std::string &code,
           const std::string &file = "test.cpp") {
  CallGraphBuilderFactory factory(graph);
  REQUIRE(clang::tooling::runToolOnCodeWithArgs(
      factory.create(), std::string(kPrelude) + code, {"-std=c++17"}, file));
}

// "file:line:col" of the one call from `caller` to `callee`.
std::pair<uint32_t, uint32_t> siteOf(const CallGraph &graph,
                                     const std::string &caller,
                                     const std::string &callee) {
  for (const CallGraphEdge &edge : graph.calleesOf(caller)) {
    if (edge.calleeName != callee)
      continue;
    llvm::StringRef site(edge.callSite);
    auto [head, col] = site.rsplit(':');
    auto [file, line] = head.rsplit(':');
    uint32_t l = 0, c = 0;
    REQUIRE_FALSE(line.getAsInteger(10, l));
    REQUIRE_FALSE(col.getAsInteger(10, c));
    return {l, c};
  }
  FAIL("no call from " << caller << " to " << callee);
  return {0, 0};
}

// An `llvm-cov export` document for test.cpp: one body per function line,
// one segment per call site.
struct ProfileSpec {
  std::vector<std::pair<uint32_t, int64_t>> functions; // line, entries
  std::vector<std::tuple<uint32_t, uint32_t, int64_t>> sites;

  void function(uint32_t line, int64_t entries) {
    functions.push_back({line, entries});
  }
  void site(std::pair<uint32_t, uint32_t> at, int64_t count) {
    sites.push_back({at.first, at.second, count});
  }

  std::string json(const std::string &file = "/elsewhere/src/test.cpp") const {
    llvm::json::Array segments;
    llvm::json::Array records;
    for (const auto &[line, entries] : functions) {
      segments.push_back(llvm::json::Array{line, 1, entries, true, true, false});
      segments.push_back(llvm::json::Array{line, 400, 0, false, false, false});
      llvm::json::Object record;
      record["name"] = "f" + std::to_string(line);
      record["count"] = entries;
      record["filenames"] = llvm::json::Array{file};
      record["regions"] = llvm::json::Array{
          llvm::json::Array{line, 1, line, 400, entries, 0, 0, 0}};
      records.push_back(std::move(record));
    }
    for (const auto &[line, col, count] : sites)
      segments.push_back(llvm::json::Array{line, col, count, true, true, false});
    llvm::json::Object fileObj;
    fileObj["filename"] = file;
    fileObj["segments"] = std::move(segments);
    llvm::json::Object data;
    data["files"] = llvm::json::Array{std::move(fileObj)};
    data["functions"] = std::move(records);
    llvm::json::Object root;
    root["type"] = "llvm.coverage.json.export";
    root["version"] = "2.0.1";
    root["data"] = llvm::json::Array{std::move(data)};
    std::string out;
    llvm::raw_string_ostream os(out);
    os << llvm::json::Value(std::move(root));
    return out;
  }
};

uint32_t lineOf(const std::string &needle) {
  const std::string text = std::string(kPrelude) + kProgram;
  const size_t at = text.find(needle);
  REQUIRE(at != std::string::npos);
  return 1 + static_cast<uint32_t>(
                 std::count(text.begin(), text.begin() + at, '\n'));
}

HotPathReport analyze(const CallGraph &graph, const HotPathConfig &config,
                      const CoverageProfile *profile = nullptr,
                      uint8_t mask = kAllEffects) {
  HotPathReport report;
  std::string error;
  REQUIRE(analyzeHotPaths(graph, EffectTable::builtin(), config, profile,
                          mask, report, error));
  return report;
}

// "<evidence> <weight> <effect> <root> > ... > <callee>" per finding, in
// report order.
std::vector<std::string> describe(const CallGraph &graph,
                                  const HotPathReport &report) {
  std::vector<std::string> out;
  for (const HotPathFinding &finding : report.findings) {
    llvm::ArrayRef<HotPathHop> path = report.path(finding);
    std::string line = std::string(hotEvidenceName(finding.evidence)) + " " +
                       (finding.weight == CoverageProfile::kNoCount
                            ? std::string("-")
                            : std::to_string(finding.weight)) +
                       " " + effectKindName(finding.effect).str() + " " +
                       graph.displayNameOf(path.front().caller);
    for (const HotPathHop &hop : path)
      line += " > " + graph.displayNameOf(hop.callee);
    out.push_back(line);
  }
  return out;
}

CoverageProfile parseProfile(const std::string &json) {
  CoverageProfile profile;
  std::string error;
  INFO(error);
  REQUIRE(CoverageProfile::parse(json, profile, error));
  return profile;
}

// The profile of kProgram: hot runs 100 times; log_it and the throw never
// run; cold once.
ProfileSpec programProfile(const CallGraph &graph) {
  ProfileSpec spec;
  spec.function(lineOf("void lock_it"), 100);
  spec.function(lineOf("void log_it"), 0);
  spec.function(lineOf("void raise_it"), 100);
  spec.function(lineOf("int *alloc_it"), 101);
  spec.function(lineOf("void hot"), 100);
  spec.function(lineOf("void cold"), 1);
  spec.function(lineOf("int main"), 1);
  spec.site(siteOf(graph, "lock_it", "std::mutex::lock"), 100);
  spec.site(siteOf(graph, "lock_it", "std::mutex::unlock"), 100);
  spec.site(siteOf(graph, "log_it", "printf"), 0);
  spec.site(siteOf(graph, "raise_it", "__cxa_throw"), 0);
  spec.site(siteOf(graph, "alloc_it", "operator new"), 101);
  spec.site(siteOf(graph, "hot", "lock_it"), 100);
  spec.site(siteOf(graph, "hot", "log_it"), 0);
  spec.site(siteOf(graph, "hot", "raise_it"), 100);
  spec.site(siteOf(graph, "hot", "alloc_it"), 100);
  spec.site(siteOf(graph, "cold", "alloc_it"), 1);
  spec.site(siteOf(graph, "main", "hot"), 1);
  spec.site(siteOf(graph, "main", "cold"), 1);
  return spec;
}

} // namespace

// ---------------------------------------------------------------------------
// Throw edges
// ---------------------------------------------------------------------------

TEST_CASE("A throw expression is a call of __cxa_throw and a rethrow of "
          "__cxa_rethrow",
          "[hot-path][throw]") {
  CallGraph graph;
  build(graph, "void f() { try { throw 1; } catch (...) { throw; } }\n");
  std::map<std::string, CallGraphEdge> byCallee;
  for (const CallGraphEdge &edge : graph.calleesOf("f"))
    byCallee[edge.calleeName] = edge;
  REQUIRE(byCallee.count("__cxa_throw") == 1);
  REQUIRE(byCallee.count("__cxa_rethrow") == 1);
  CHECK(byCallee["__cxa_throw"].kind == EdgeKind::DirectCall);
  CHECK(byCallee["__cxa_throw"].confidence == Confidence::Proven);
  CHECK(byCallee["__cxa_throw"].calleeUsr == "c:@F@__cxa_throw");
  CHECK(byCallee["__cxa_rethrow"].calleeUsr == "c:@F@__cxa_rethrow");

  const EffectTable table = EffectTable::builtin();
  CHECK(table.effectsOf("__cxa_throw") == effectBit(EffectKind::Throw));
  CHECK(table.effectsOf("__cxa_rethrow") == effectBit(EffectKind::Throw));
}

// ---------------------------------------------------------------------------
// The profile reader
// ---------------------------------------------------------------------------

TEST_CASE("CoverageProfile reads counts at locations and the enclosing "
          "function",
          "[hot-path][profile]") {
  // f spans lines 1-10 and holds a lambda on lines 3-5; two records share
  // the body on line 20 (two instantiations of one template).
  const std::string json = R"({"type":"llvm.coverage.json.export",
    "version":"2.0.1","data":[{"files":[{"filename":"/a/b/src/x.cpp",
    "segments":[[1,10,7,true,true,false],[3,5,2,true,true,false],
                [5,2,7,true,false,false],[6,1,0,true,true,false],
                [10,2,0,false,false,false],[20,1,3,true,true,false],
                [20,40,0,false,false,false]]}],
    "functions":[
      {"name":"f","count":7,"filenames":["/a/b/src/x.cpp"],
       "regions":[[1,10,10,2,7,0,0,0]]},
      {"name":"f::lambda","count":2,"filenames":["/a/b/src/x.cpp"],
       "regions":[[3,5,5,2,2,0,0,0]]},
      {"name":"g<int>","count":1,"filenames":["/a/b/src/x.cpp"],
       "regions":[[20,1,20,40,1,0,0,0]]},
      {"name":"g<long>","count":2,"filenames":["/a/b/src/x.cpp"],
       "regions":[[20,1,20,40,2,0,0,0]]}]}]})";
  CoverageProfile profile = parseProfile(json);
  CHECK(profile.fileCount() == 1);
  CHECK(profile.functionCount() == 3); // the two g records merge
  CHECK(profile.executedFunctionCount() == 3);

  const uint32_t file = profile.fileFor("src/x.cpp");
  REQUIRE(file != CoverageProfile::kNone);
  CHECK(profile.fileFor("./src/x.cpp") == file);
  CHECK(profile.fileFor("/elsewhere/x.cpp") == file); // base name, unique
  CHECK(profile.fileFor("y.cpp") == CoverageProfile::kNone);

  CHECK(profile.countAt(file, 2, 1) == 7);
  CHECK(profile.countAt(file, 4, 9) == 2);
  CHECK(profile.countAt(file, 7, 3) == 0);
  CHECK(profile.countAt(file, 15, 1) == CoverageProfile::kNoCount);
  CHECK(profile.countAt(file, 1, 1) == CoverageProfile::kNoCount);

  const uint32_t inLambda = profile.functionAt(file, 4, 1);
  const uint32_t inF = profile.functionAt(file, 8, 1);
  REQUIRE(inLambda != CoverageProfile::kNone);
  REQUIRE(inF != CoverageProfile::kNone);
  CHECK(profile.functionCount(inLambda) == 2);
  CHECK(profile.functionCount(inF) == 7);
  CHECK(profile.functionAt(file, 6, 1) == inF); // after the lambda
  CHECK(profile.functionCount(profile.functionAt(file, 20, 5)) == 3);
  CHECK(profile.functionAt(file, 15, 1) == CoverageProfile::kNone);
}

TEST_CASE("CoverageProfile refuses an ambiguous file and other documents",
          "[hot-path][profile]") {
  const std::string two = R"({"type":"llvm.coverage.json.export",
    "data":[{"files":[{"filename":"/r/a/util.cpp","segments":[]},
                      {"filename":"/r/b/util.cpp","segments":[]}],
             "functions":[]}]})";
  CoverageProfile profile = parseProfile(two);
  CHECK(profile.fileFor("util.cpp") == CoverageProfile::kNone);
  CHECK(profile.fileFor("b/util.cpp") != CoverageProfile::kNone);
  CHECK(profile.fileFor("/src/a/util.cpp") != CoverageProfile::kNone);

  CoverageProfile out;
  std::string error;
  CHECK_FALSE(CoverageProfile::parse("[1,2]", out, error));
  CHECK(error.find("llvm-cov export") != std::string::npos);
  CHECK_FALSE(CoverageProfile::parse("{", out, error));
  const std::string summaryOnly = R"({"type":"llvm.coverage.json.export",
    "data":[{"files":[{"filename":"/r/a.cpp","summary":{}}]}]})";
  CHECK_FALSE(CoverageProfile::parse(summaryOnly, out, error));
  CHECK(error.find("per-function") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Roots and effects
// ---------------------------------------------------------------------------

TEST_CASE("Every effect kind is reported from a configured root",
          "[hot-path][effects]") {
  CallGraph graph;
  build(graph, kProgram);
  HotPathConfig config;
  config.roots = {"hot"};
  HotPathReport report = analyze(graph, config);
  CHECK(report.roots == 1);
  CHECK(report.configRoots == 1);
  CHECK(report.unmatchedRoots.empty());
  CHECK(describe(graph, report) ==
        std::vector<std::string>{
            "unprofiled - allocation hot > alloc_it > operator new",
            "unprofiled - lock hot > lock_it > std::mutex::lock",
            "unprofiled - io hot > log_it > printf",
            "unprofiled - throw hot > raise_it > __cxa_throw",
        });
  for (const HotPathFinding &finding : report.findings) {
    CHECK(finding.rootSources == kHotRootConfig);
    CHECK(finding.rootCount == CoverageProfile::kNoCount);
    CHECK(finding.confidence == Confidence::Proven);
  }
  CHECK(report.counts[static_cast<unsigned>(HotEvidence::Unprofiled)]
                     [static_cast<unsigned>(EffectKind::Lock)] == 1);
  // hot, the four functions it calls and std::mutex::unlock; not cold or
  // main, and not the effect functions, where a path ends.
  CHECK(report.reachedFunctions == 6);

  // The effect mask narrows the kinds.
  HotPathReport throws =
      analyze(graph, config, nullptr, effectBit(EffectKind::Throw));
  CHECK(describe(graph, throws) ==
        std::vector<std::string>{
            "unprofiled - throw hot > raise_it > __cxa_throw"});
}

TEST_CASE("Roots by USR and by pattern attach findings to the nearest root",
          "[hot-path][roots]") {
  CallGraph graph;
  build(graph, kProgram);
  const CallGraphNode *hot = graph.findNode("hot");
  REQUIRE(hot);
  HotPathConfig config;
  config.roots = {hot->usr, "no_such_function"};
  config.rootPatterns = {"^lock_", "^nothing_matches$"};
  HotPathReport report = analyze(graph, config);
  CHECK(report.roots == 2);
  CHECK(report.unmatchedRoots ==
        std::vector<std::string>{"^nothing_matches$", "no_such_function"});
  CHECK(describe(graph, report) ==
        std::vector<std::string>{
            "unprofiled - allocation hot > alloc_it > operator new",
            "unprofiled - lock lock_it > std::mutex::lock",
            "unprofiled - io hot > log_it > printf",
            "unprofiled - throw hot > raise_it > __cxa_throw",
        });

  HotPathConfig bad;
  bad.rootPatterns = {"("};
  HotPathReport out;
  std::string error;
  CHECK_FALSE(analyzeHotPaths(graph, EffectTable::builtin(), bad, nullptr,
                              kAllEffects, out, error));
  CHECK(error.find("not a valid regex") != std::string::npos);
}

TEST_CASE("An allowed function is neither reported nor followed",
          "[hot-path][allow]") {
  CallGraph graph;
  build(graph, kProgram);
  HotPathConfig config;
  config.roots = {"hot"};
  config.allow = {"lock_it", "log_it"};
  HotPathReport report = analyze(graph, config);
  CHECK(report.allowedCalls == 2);
  CHECK(describe(graph, report) ==
        std::vector<std::string>{
            "unprofiled - allocation hot > alloc_it > operator new",
            "unprofiled - throw hot > raise_it > __cxa_throw",
        });

  // By USR: the effect function itself.
  const CallGraphNode *lock = graph.findNode("std::mutex::lock");
  REQUIRE(lock);
  config.allow = {lock->usr};
  report = analyze(graph, config, nullptr, effectBit(EffectKind::Lock));
  CHECK(report.findings.empty());
  CHECK(report.allowedCalls == 1);
}

TEST_CASE("A function the effect table lists ends the path",
          "[hot-path][effects]") {
  // An organization allocator that itself calls new: one finding, at the
  // call of the allocator.
  CallGraph graph;
  build(graph, "namespace org { int *grab() { return new int(0); } }\n"
               "void hot() { org::grab(); }\n");
  EffectTable table = EffectTable::builtin();
  table.add(EffectKind::Allocation, "org::grab");
  HotPathConfig config;
  config.roots = {"hot"};
  HotPathReport report;
  std::string error;
  REQUIRE(analyzeHotPaths(graph, table, config, nullptr, kAllEffects, report,
                          error));
  CHECK(describe(graph, report) ==
        std::vector<std::string>{"unprofiled - allocation hot > org::grab"});
}

// ---------------------------------------------------------------------------
// The profile
// ---------------------------------------------------------------------------

TEST_CASE("A profile weights each path and keeps latent apart from observed",
          "[hot-path][profile]") {
  CallGraph graph;
  build(graph, kProgram);
  const std::string json = programProfile(graph).json();
  CoverageProfile profile = parseProfile(json);
  HotPathConfig config;
  config.roots = {"hot"};
  HotPathReport report = analyze(graph, config, &profile);
  CHECK(describe(graph, report) ==
        std::vector<std::string>{
            "observed 100 allocation hot > alloc_it > operator new",
            "observed 100 lock hot > lock_it > std::mutex::lock",
            "latent 0 io hot > log_it > printf",
            "latent 0 throw hot > raise_it > __cxa_throw",
        });
  REQUIRE_FALSE(report.findings.empty());
  CHECK(report.findings.front().rootCount == 100);
  llvm::ArrayRef<HotPathHop> path = report.path(report.findings.front());
  REQUIRE(path.size() == 2);
  CHECK(path[0].count == 100);
  CHECK(path[0].calleeCount == 101);
  CHECK(path[1].count == 101);

  // Every stored call site sits in the profiled file and has a count.
  CHECK(report.join.callSites == report.join.callSitesInProfile);
  CHECK(report.join.callSitesCounted == report.join.callSites);
  CHECK(report.join.functions == 7);
  CHECK(report.join.functionsCounted == 7);
}

TEST_CASE("Profile roots are the functions entered at least min_count times",
          "[hot-path][profile][roots]") {
  CallGraph graph;
  build(graph, kProgram);
  CoverageProfile profile = parseProfile(programProfile(graph).json());
  HotPathConfig config;
  config.minCount = 50;
  HotPathReport report = analyze(graph, config, &profile);
  // hot, lock_it, raise_it, alloc_it; not main, cold or log_it.
  CHECK(report.roots == 4);
  CHECK(report.profileRoots == 4);
  CHECK(describe(graph, report) ==
        std::vector<std::string>{
            "observed 101 allocation alloc_it > operator new",
            "observed 100 lock lock_it > std::mutex::lock",
            "latent 0 io hot > log_it > printf",
            "latent 0 throw raise_it > __cxa_throw",
        });
  CHECK(report.findings.front().rootSources == kHotRootProfile);
  CHECK(report.findings.front().rootCount == 101);

  // A configured root that is also hot carries both sources.
  config.roots = {"hot"};
  report = analyze(graph, config, &profile, effectBit(EffectKind::Io));
  REQUIRE(report.findings.size() == 1);
  CHECK(report.findings.front().rootSources ==
        (kHotRootConfig | kHotRootProfile));
}

TEST_CASE("The witness is the widest path, then the shortest, then by name",
          "[hot-path][witness]") {
  // r reaches e through a (ran once) and through b -> c (ran 50 times).
  const char *code = "int *e() { return new int(0); }\n"
                     "void a() { e(); }\n"
                     "void c() { e(); }\n"
                     "void b() { c(); }\n"
                     "void r() { a(); b(); }\n";
  CallGraph graph;
  build(graph, code);
  const uint32_t base = 1 + static_cast<uint32_t>(std::count(
                                kPrelude, kPrelude + strlen(kPrelude), '\n'));
  ProfileSpec spec;
  spec.function(base + 0, 51);
  spec.function(base + 1, 1);
  spec.function(base + 2, 50);
  spec.function(base + 3, 50);
  spec.function(base + 4, 50);
  spec.site(siteOf(graph, "e", "operator new"), 51);
  spec.site(siteOf(graph, "a", "e"), 1);
  spec.site(siteOf(graph, "c", "e"), 50);
  spec.site(siteOf(graph, "b", "c"), 50);
  spec.site(siteOf(graph, "r", "a"), 1);
  spec.site(siteOf(graph, "r", "b"), 50);
  CoverageProfile profile = parseProfile(spec.json());
  HotPathConfig config;
  config.roots = {"r"};
  CHECK(describe(graph, analyze(graph, config, &profile)) ==
        std::vector<std::string>{
            "observed 50 allocation r > b > c > e > operator new"});
  // Without counts the shortest path wins.
  CHECK(describe(graph, analyze(graph, config)) ==
        std::vector<std::string>{
            "unprofiled - allocation r > a > e > operator new"});

  // Two equal paths: the parent with the smaller USR, whatever the order
  // the calls are written or the TUs are read in.
  const char *tieY = "int *e() { return new int(0); }\n"
                     "void y() { e(); }\n"
                     "void x() { e(); }\n"
                     "void r() { y(); x(); }\n";
  const char *tieX = "int *e() { return new int(0); }\n"
                     "void x() { e(); }\n"
                     "void y() { e(); }\n"
                     "void r() { x(); y(); }\n";
  for (const char *tie : {tieY, tieX}) {
    CallGraph g;
    build(g, tie);
    CHECK(describe(g, analyze(g, config)) ==
          std::vector<std::string>{
              "unprofiled - allocation r > x > e > operator new"});
  }
}

TEST_CASE("A virtual target the profile never entered is latent",
          "[hot-path][profile][virtual]") {
  const char *code =
      "struct Base { virtual void run(); };\n"
      "struct Fast : Base { void run() override; };\n"
      "struct Slow : Base { void run() override; };\n"
      "void Fast::run() { printf(\"\"); }\n"
      "void Slow::run() { new int(0); }\n"
      "void hot(Base &b) { b.run(); }\n";
  CallGraph graph;
  build(graph, code);
  const uint32_t base = 1 + static_cast<uint32_t>(std::count(
                                kPrelude, kPrelude + strlen(kPrelude), '\n'));
  ProfileSpec spec;
  spec.function(base + 3, 100);
  spec.function(base + 4, 0);
  spec.function(base + 5, 100);
  spec.site(siteOf(graph, "Fast::run", "printf"), 100);
  spec.site(siteOf(graph, "Slow::run", "operator new"), 0);
  spec.site(siteOf(graph, "hot", "Base::run"), 100);
  CoverageProfile profile = parseProfile(spec.json());
  HotPathConfig config;
  config.roots = {"hot"};
  HotPathReport report = analyze(graph, config, &profile);
  CHECK(describe(graph, report) ==
        std::vector<std::string>{
            "observed 100 io hot > Fast::run > printf",
            "latent 0 allocation hot > Slow::run > operator new",
        });
  REQUIRE(report.findings.size() == 2);
  llvm::ArrayRef<HotPathHop> slow = report.path(report.findings[1]);
  CHECK(slow[0].kind == EdgeKind::VirtualDispatch);
  CHECK(slow[0].count == 100);
  CHECK(slow[0].calleeCount == 0);
  CHECK(report.findings[1].confidence == Confidence::Plausible);
}

TEST_CASE("Reached template instantiations without calls are counted",
          "[hot-path][templates]") {
  // The graph holds Box::put's calls under the template, not under
  // Box<int>::put (#106): the lock is not seen, and the report says that
  // two reached functions hide their calls. A function whose parameter is
  // a specialization, and operator<, are not instantiations.
  CallGraph graph;
  build(graph, R"(std::mutex mu;
template <class T> struct Box { void put(T) { mu.lock(); } };
template <class T> T twice(T x) { return x + x; }
struct A {};
bool operator<(A, A);
void takes(Box<int> &) {}
void hot() { Box<int> b; b.put(1); twice(2); takes(b); A x, y; (void)(x < y); }
)");
  HotPathConfig config;
  config.roots = {"hot"};
  HotPathReport report = analyze(graph, config);
  CHECK(report.findings.empty());
  CHECK(report.templateLeaves == 2);

  ControlFlowIndex cfIndex;
  ControlFlowOracle oracle(graph, cfIndex);
  std::vector<std::string> eps = {"hot"};
  ToolContext ctx{graph, oracle, cfIndex, eps};
  ToolHandler handler;
  for (const ToolEntry &tool : getRegisteredTools())
    if (tool.name == "query_path_effects")
      handler = tool.handler;
  REQUIRE(handler);
  ExtensionRegistry::instance().clear();
  auto parsed = llvm::json::parse(R"({"roots": ["hot"]})");
  REQUIRE(bool(parsed));
  llvm::json::Value result = handler(*parsed->getAsObject(), ctx);
  const llvm::json::Object *out = result.getAsObject();
  REQUIRE(out);
  CHECK(out->getInteger("templateLeaves").value_or(-1) == 2);
  CHECK(out->getBoolean("exhaustive") == std::optional<bool>(false));
  CHECK(out->getString("verdict").value_or("") == "unknown");
}

TEST_CASE("Findings do not depend on the order TUs are read in",
          "[hot-path][determinism]") {
  const std::string tuA = "int *alloc_it();\nvoid lock_it();\n"
                          "void hot() { lock_it(); alloc_it(); }\n";
  const std::string tuB = "std::mutex mu;\n"
                          "void lock_it() { mu.lock(); }\n"
                          "int *alloc_it() { return new int(1); }\n";
  auto run = [](const std::string &first, const std::string &firstFile,
                const std::string &second, const std::string &secondFile) {
    CallGraph graph;
    build(graph, first, firstFile);
    build(graph, second, secondFile);
    HotPathConfig config;
    config.roots = {"hot"};
    return describe(graph, analyze(graph, config));
  };
  const auto ab = run(tuA, "a.cpp", tuB, "b.cpp");
  CHECK(ab == run(tuB, "b.cpp", tuA, "a.cpp"));
  CHECK(ab.size() == 2);
}

// ---------------------------------------------------------------------------
// Org config, tool, check
// ---------------------------------------------------------------------------

TEST_CASE("The org config's hotPaths key", "[hot-path][org-config]") {
  OrgConfig cfg;
  std::string error;
  REQUIRE(parseOrgConfigJson(
      R"({"hotPaths": {"roots": ["hot"], "rootPatterns": ["^kernel_"],
                       "allow": ["Pool::get"], "minCount": 10},
          "effects": {"throw": ["org::raise"]}})",
      cfg, error));
  CHECK(cfg.hotPaths.roots == std::vector<std::string>{"hot"});
  CHECK(cfg.hotPaths.rootPatterns == std::vector<std::string>{"^kernel_"});
  CHECK(cfg.hotPaths.allow == std::vector<std::string>{"Pool::get"});
  CHECK(cfg.hotPaths.minCount == 10);

  auto &registry = ExtensionRegistry::instance();
  registry.clear();
  REQUIRE(applyOrgConfig(cfg, error));
  CHECK(registry.hotPathConfig().roots == std::vector<std::string>{"hot"});
  CHECK(registry.hotPathConfig().minCount == 10);
  CHECK(registry.effectTable().effectsOf("org::raise") ==
        effectBit(EffectKind::Throw));
  registry.clear();

  OrgConfig bad;
  CHECK_FALSE(parseOrgConfigJson(R"({"hotPaths": {"root": ["x"]}})", bad,
                                 error));
  CHECK(error.find("unknown key 'root'") != std::string::npos);
  CHECK_FALSE(parseOrgConfigJson(R"({"hotPaths": {"minCount": -1}})", bad,
                                 error));
  CHECK_FALSE(parseOrgConfigJson(R"({"hotPaths": []})", bad, error));
  OrgConfig badRegex;
  REQUIRE(parseOrgConfigJson(R"({"hotPaths": {"rootPatterns": ["("]}})",
                             badRegex, error));
  CHECK_FALSE(applyOrgConfig(badRegex, error));
  registry.clear();
}

TEST_CASE("query_path_effects answers through the tool contract",
          "[hot-path][tools]") {
  CallGraph graph;
  build(graph, kProgram);
  ControlFlowIndex cfIndex;
  ControlFlowOracle oracle(graph, cfIndex);
  std::vector<std::string> eps = {"main"};
  ToolContext ctx{graph, oracle, cfIndex, eps};
  ToolHandler handler;
  for (const ToolEntry &tool : getRegisteredTools())
    if (tool.name == "query_path_effects")
      handler = tool.handler;
  REQUIRE(handler);
  ExtensionRegistry::instance().clear();

  auto call = [&](const char *args) {
    auto parsed = llvm::json::parse(args);
    REQUIRE(bool(parsed));
    llvm::json::Value result = handler(*parsed->getAsObject(), ctx);
    REQUIRE(result.getAsObject());
    return *result.getAsObject();
  };

  llvm::json::Object out = call(R"({"roots": ["hot"], "limit": 2})");
  CHECK(out.getString("verdict").value_or("") == "effects");
  CHECK(out.get("profile")->kind() == llvm::json::Value::Null);
  const llvm::json::Array *findings = out.getArray("findings");
  REQUIRE(findings);
  CHECK(findings->size() == 2);
  CHECK(out.getInteger("total").value_or(0) == 4);
  const llvm::json::Object *first = (*findings)[0].getAsObject();
  REQUIRE(first);
  CHECK(first->getString("effect").value_or("") == "allocation");
  CHECK(first->getString("evidence").value_or("") == "unprofiled");
  CHECK(first->get("weight")->kind() == llvm::json::Value::Null);
  CHECK(first->getArray("path")->size() == 2);

  // No finding and unstated coverage: not a universal "clean".
  out = call(R"({"roots": ["cold"], "effects": ["lock"]})");
  CHECK(out.getString("verdict").value_or("") == "unknown");

  CHECK(call(R"({})").getString("status").value_or("") == "usage_error");
  CHECK(call(R"({"roots": ["hot"], "min_count": 5})")
            .getString("status")
            .value_or("") == "usage_error");
  CHECK(call(R"({"roots": ["hot"], "effects": ["sleep"]})")
            .getString("status")
            .value_or("") == "usage_error");
  CHECK(call(R"({"roots": ["hot"], "profile": "/no/such/profile.json"})")
            .getString("status")
            .value_or("") == "usage_error");
}

TEST_CASE("The check reports latent findings as notes",
          "[hot-path][check]") {
  CallGraph graph;
  build(graph, kProgram);
  CoverageProfile profile = parseProfile(programProfile(graph).json());
  HotPathConfig config;
  config.roots = {"hot"};
  HotPathReport report = analyze(graph, config, &profile);
  std::vector<Diagnostic> diagnostics;
  appendHotPathDiagnostics(graph, report, "run.json", diagnostics);
  REQUIRE(diagnostics.size() == 4);
  const Diagnostic &observed = diagnostics[0];
  CHECK(observed.checkName == "hot-path-effects");
  CHECK(observed.file == "test.cpp");
  CHECK(observed.message.find("observed, the rarest call on the path ran "
                              "100 times in run.json") != std::string::npos);
  CHECK(diagnosticSeverity(observed) == Severity::Warning);
  const Diagnostic &latent = diagnostics[2];
  CHECK(latent.note);
  CHECK(diagnosticSeverity(latent) == Severity::Note);
  CHECK(latent.message.find("hot -> log_it -> printf") != std::string::npos);
  // The fingerprint identity names no line.
  for (const std::string &entity : latent.entities)
    CHECK(entity.find("test.cpp") == std::string::npos);
}
