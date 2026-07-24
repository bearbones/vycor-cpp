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

// test_anneal_static_init.cpp — the two static-initialization checks:
// static-init-order (cross-TU SIOF) and static-init-hazards (initializers
// reaching the dynamic linker / thread create-join through the call graph).

#include "vycor/anneal/Analyzer.h"
#include "vycor/anneal/GlobalIndex.h"
#include "vycor/callgraph/CallGraph.h"
#include "vycor/callgraph/CallGraphBuilder.h"
#include "vycor/ext/Extensions.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

#include <catch2/catch_test_macros.hpp>
#include <clang/Tooling/CompilationDatabase.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace vycor;

namespace {

// Cross-TU fixture:
//  - a.cpp dynamically initializes ga; b.cpp initializes gb FROM ga (SIOF).
//  - c.cpp initializes gc from constant-initialized kSafe (silent).
//  - loader.cpp's global Loader object reaches dlopen through a helper
//    (hazard), and onLoad() is an __attribute__((constructor)) calling
//    pthread_create (hazard); ok.cpp's global calls a pure helper (silent).
struct StaticInitFixture {
  std::string dir = "anneal_sinit_fixture";
  std::string absDir;

  StaticInitFixture() {
    REQUIRE(!llvm::sys::fs::create_directory(dir));
    llvm::SmallString<256> abs;
    REQUIRE(!llvm::sys::fs::real_path(dir, abs));
    absDir = std::string(abs.str());

    write("globals.hpp", R"cpp(
#pragma once
extern int ga;
extern int gb;
extern const int kSafe;
int compute();
)cpp");
    write("a.cpp", R"cpp(
#include "globals.hpp"
int compute() { return 41; }
int ga = compute();
)cpp");
    write("b.cpp", R"cpp(
#include "globals.hpp"
int gb = ga + 1;
)cpp");
    write("c.cpp", R"cpp(
#include "globals.hpp"
const int kSafe = 7;
int gc = kSafe + compute(); // dynamic, but its global dep is constant-init
)cpp");
    write("loader.cpp", R"cpp(
extern "C" void *dlopen(const char *, int);
extern "C" int pthread_create(void *, const void *, void *(*)(void *),
                              void *);
namespace myorg {
void *loadPlugin() { return dlopen("plugin.so", 2); }
struct Loader {
  Loader() { handle = loadPlugin(); }
  void *handle;
};
Loader gLoader;
} // namespace myorg
__attribute__((constructor)) static void onLoad() {
  pthread_create(nullptr, nullptr, nullptr, nullptr);
}
)cpp");
    write("ok.cpp", R"cpp(
namespace myorg {
int pureHelper() { return 3; }
int gFine = pureHelper();
}
)cpp");
  }

  ~StaticInitFixture() {
    for (const char *f : {"globals.hpp", "a.cpp", "b.cpp", "c.cpp",
                          "loader.cpp", "ok.cpp"})
      std::remove((dir + "/" + f).c_str());
    llvm::sys::fs::remove(dir);
  }

  void write(const std::string &name, const std::string &content) const {
    std::ofstream out(dir + "/" + name, std::ios::binary | std::ios::trunc);
    REQUIRE(out.good());
    out << content;
  }

  std::vector<std::string> files() const {
    return {absDir + "/a.cpp", absDir + "/b.cpp", absDir + "/c.cpp",
            absDir + "/loader.cpp", absDir + "/ok.cpp"};
  }

  clang::tooling::FixedCompilationDatabase db() const {
    return clang::tooling::FixedCompilationDatabase(".", {"-std=c++17"});
  }
};

std::vector<Diagnostic> ofKind(const std::vector<Diagnostic> &all,
                               Diagnostic::Kind kind) {
  std::vector<Diagnostic> out;
  for (const auto &d : all)
    if (d.kind == kind)
      out.push_back(d);
  return out;
}

} // namespace

TEST_CASE("analyzeStaticInitOrder flags cross-file dynamic->dynamic only",
          "[AnnealStaticInit]") {
  GlobalIndex index;
  auto add = [&](const char *name, const char *file, bool dynamic,
                 std::vector<std::string> refs = {}) {
    StaticInitEntry e;
    e.qualifiedName = name;
    e.filePath = file;
    e.line = 3;
    e.dynamicInit = dynamic;
    e.referencedGlobals = std::move(refs);
    index.addStaticInit(e);
  };

  add("ga", "a.cpp", true);
  add("gb", "b.cpp", true, {"ga"});          // cross-file dynamic: flagged
  add("gc", "b.cpp", true, {"gb"});          // same-file: ordered, silent
  add("kSafe", "c.cpp", false);
  add("gd", "d.cpp", true, {"kSafe"});       // constant target: silent
  add("ge", "e.cpp", true, {"gUnknown"});    // unknown target: silent

  std::vector<Diagnostic> diags;
  analyzeStaticInitOrder(index, diags);
  REQUIRE(diags.size() == 1);
  CHECK(diags[0].kind == Diagnostic::StaticInit_OrderDependency);
  CHECK(diags[0].message.find("'gb'") != std::string::npos);
  CHECK(diags[0].message.find("'ga'") != std::string::npos);
}

TEST_CASE("End-to-end: SIOF flagged across TUs, constant deps silent",
          "[AnnealStaticInit]") {
  StaticInitFixture fx;
  auto compDb = fx.db();

  AnalysisOptions opts;
  opts.threadCount = 1;
  auto diags = ofKind(runAnalysis(compDb, fx.files(), opts),
                      Diagnostic::StaticInit_OrderDependency);

  REQUIRE(diags.size() == 1);
  CHECK(diags[0].message.find("'gb'") != std::string::npos);
  CHECK(diags[0].message.find("'ga'") != std::string::npos);
  CHECK(diags[0].message.find("gc") == std::string::npos);
}

TEST_CASE("End-to-end: initializers reaching dlopen/pthread_create are "
          "hazards; pure helpers are not",
          "[AnnealStaticInit]") {
  StaticInitFixture fx;
  auto compDb = fx.db();

  AnalysisOptions opts;
  opts.threadCount = 1;
  GlobalIndex index;
  auto all = runAnalysis(compDb, fx.files(), opts, &index);
  REQUIRE(index.staticInitCount() > 0);

  auto graph = buildCallGraph(compDb, fx.files());
  std::vector<Diagnostic> diags;
  analyzeStaticInitHazards(index, graph, diags);
  auto hazards = ofKind(diags, Diagnostic::StaticInit_Hazard);

  REQUIRE(hazards.size() == 2);
  bool sawLoader = false, sawCtorFn = false;
  for (const auto &d : hazards) {
    if (d.message.find("'myorg::gLoader'") != std::string::npos) {
      sawLoader = true;
      CHECK(d.message.find("dlopen") != std::string::npos);
      // The chain goes through the constructor and the helper.
      CHECK(d.message.find("loadPlugin") != std::string::npos);
    }
    if (d.message.find("'onLoad'") != std::string::npos) {
      sawCtorFn = true;
      CHECK(d.message.find("pthread_create") != std::string::npos);
    }
    CHECK(d.message.find("gFine") == std::string::npos);
  }
  CHECK(sawLoader);
  CHECK(sawCtorFn);
}

TEST_CASE("static-init-order can be disabled via options",
          "[AnnealStaticInit]") {
  StaticInitFixture fx;
  auto compDb = fx.db();

  AnalysisOptions opts;
  opts.threadCount = 1;
  opts.enableStaticInitOrderDiag = false;
  CHECK(ofKind(runAnalysis(compDb, fx.files(), opts),
               Diagnostic::StaticInit_OrderDependency)
            .empty());
}

TEST_CASE("Static-init entries dedup and survive absorb",
          "[AnnealStaticInit]") {
  GlobalIndex shard;
  StaticInitEntry e;
  e.qualifiedName = "g";
  e.filePath = "g.hpp";
  e.line = 4;
  e.dynamicInit = true;
  e.referencedGlobals = {"other"};
  e.calledFunctions = {"make"};
  shard.addStaticInit(e);
  shard.addStaticInit(e);
  CHECK(shard.staticInitCount() == 1);

  GlobalIndex master;
  master.absorb(shard);
  master.absorb(shard);
  CHECK(master.staticInitCount() == 1);
  const auto *found = master.findStaticInit("g");
  REQUIRE(found);
  CHECK(found->referencedGlobals == std::vector<std::string>{"other"});
  CHECK(found->calledFunctions == std::vector<std::string>{"make"});
}

TEST_CASE("Transitive SIOF: initializer reaching a cross-TU global through "
          "function calls is flagged with the chain",
          "[AnnealStaticInit]") {
  // gBase (base.cpp) is dynamic; reader.cpp's readBase() returns it;
  // derived.cpp initializes gDerived = readBase() — no direct reference,
  // but the summary walk finds base.cpp's global two hops away.
  struct Fixture {
    std::string dir = "anneal_sinit_trans_fixture";
    std::string absDir;
    Fixture() {
      REQUIRE(!llvm::sys::fs::create_directory(dir));
      llvm::SmallString<256> abs;
      REQUIRE(!llvm::sys::fs::real_path(dir, abs));
      absDir = std::string(abs.str());
      write("api.hpp", "#pragma once\nextern int gBase;\nint compute();\n"
                       "int readBase();\n");
      write("base.cpp", "#include \"api.hpp\"\nint compute() { return 9; }\n"
                        "int gBase = compute();\n");
      write("reader.cpp",
            "#include \"api.hpp\"\nint readBase() { return gBase * 2; }\n");
      write("derived.cpp",
            "#include \"api.hpp\"\nint gDerived = readBase();\n");
    }
    ~Fixture() {
      for (const char *f : {"api.hpp", "base.cpp", "reader.cpp",
                            "derived.cpp"})
        std::remove((dir + "/" + f).c_str());
      llvm::sys::fs::remove(dir);
    }
    void write(const std::string &name, const std::string &content) const {
      std::ofstream out(dir + "/" + name,
                        std::ios::binary | std::ios::trunc);
      REQUIRE(out.good());
      out << content;
    }
  } fx;

  clang::tooling::FixedCompilationDatabase compDb(".", {"-std=c++17"});
  std::vector<std::string> files = {fx.absDir + "/base.cpp",
                                    fx.absDir + "/reader.cpp",
                                    fx.absDir + "/derived.cpp"};
  AnalysisOptions opts;
  opts.threadCount = 1;
  auto diags = ofKind(runAnalysis(compDb, files, opts),
                      Diagnostic::StaticInit_OrderDependency);

  REQUIRE(diags.size() == 1);
  CHECK(diags[0].message.find("'gDerived'") != std::string::npos);
  CHECK(diags[0].message.find("'gBase'") != std::string::npos);
  CHECK(diags[0].message.find("via readBase") != std::string::npos);
}

TEST_CASE("Org-registered hazard functions extend static-init-hazards",
          "[AnnealStaticInit]") {
  struct RegistryReset {
    RegistryReset() { ExtensionRegistry::instance().clear(); }
    ~RegistryReset() { ExtensionRegistry::instance().clear(); }
  } reset;
  ExtensionRegistry::instance().addStaticInitHazards(
      {"myorg::JniEnv::attach"});

  GlobalIndex index;
  StaticInitEntry root;
  root.qualifiedName = "gBridge";
  root.filePath = "bridge.cpp";
  root.line = 4;
  root.dynamicInit = true;
  root.calledFunctions = {"myorg::JniEnv::attach"};
  index.addStaticInit(root);

  CallGraph graph; // hazard is a direct seed: no edges needed
  std::vector<Diagnostic> diags;
  analyzeStaticInitHazards(index, graph, diags);
  REQUIRE(diags.size() == 1);
  CHECK(diags[0].kind == Diagnostic::StaticInit_Hazard);
  CHECK(diags[0].message.find("myorg::JniEnv::attach") != std::string::npos);
}
