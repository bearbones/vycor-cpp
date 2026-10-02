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

// test_anneal_header_static.cpp — the header-static-duplication check:
// mutable internal-linkage statics defined in headers and materialized by
// two or more TUs (forked per-TU state).

#include "vycor/anneal/Analyzer.h"
#include "vycor/anneal/GlobalIndex.h"

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

// A fresh directory named after `dir` (dir-XXXXXX), stored back into `dir`:
// ctest runs test cases as parallel processes, and a fixed name let one
// case's cleanup delete another's sources mid-run.
[[maybe_unused]] bool makeUniqueFixtureDir(std::string &dir) {
  llvm::SmallString<128> made;
  if (llvm::sys::fs::createUniqueDirectory(dir, made))
    return false;
  dir = std::string(made);
  return true;
}

// state.hpp defines a MUTABLE static (gCounter) and a const one (kTag);
// both a.cpp and b.cpp include it — gCounter forks, kTag is benign.
// only.cpp includes solo.hpp's mutable static (one TU: no duplication),
// and has its own main-file static (normal usage, never recorded).
struct HeaderStaticFixture {
  std::string dir = "anneal_hstatic_fixture";
  std::string absDir;

  HeaderStaticFixture() {
    REQUIRE(makeUniqueFixtureDir(dir));
    llvm::SmallString<256> abs;
    REQUIRE(!llvm::sys::fs::real_path(dir, abs));
    absDir = std::string(abs.str());

    write("state.hpp", R"cpp(
#pragma once
static int gCounter = 0;
static const int kTag = 7;
static const char *kName = "svc"; // pointer-to-const idiom: benign
inline int bump() { return ++gCounter + kTag + (kName ? 1 : 0); }
)cpp");
    write("solo.hpp", R"cpp(
#pragma once
static int gLonely = 1;
inline int lonely() { return gLonely; }
)cpp");
    write("a.cpp", R"cpp(
#include "state.hpp"
int a() { return bump(); }
)cpp");
    write("b.cpp", R"cpp(
#include "state.hpp"
int b() { return bump(); }
)cpp");
    write("only.cpp", R"cpp(
#include "solo.hpp"
static int gMainFileLocal = 5; // main-file static: intended usage
int c() { return lonely() + gMainFileLocal; }
)cpp");
  }

  ~HeaderStaticFixture() {
    for (const char *f :
         {"state.hpp", "solo.hpp", "a.cpp", "b.cpp", "only.cpp"})
      std::remove((dir + "/" + f).c_str());
    llvm::sys::fs::remove(dir);
  }

  void write(const std::string &name, const std::string &content) const {
    std::ofstream out(dir + "/" + name, std::ios::binary | std::ios::trunc);
    REQUIRE(out.good());
    out << content;
  }

  std::vector<std::string> files() const {
    return {absDir + "/a.cpp", absDir + "/b.cpp", absDir + "/only.cpp"};
  }

  clang::tooling::FixedCompilationDatabase db() const {
    return clang::tooling::FixedCompilationDatabase(".", {"-std=c++17"});
  }
};

std::vector<Diagnostic> dupDiags(const std::vector<Diagnostic> &all) {
  std::vector<Diagnostic> out;
  for (const auto &d : all)
    if (d.kind == Diagnostic::HeaderStatic_Duplicated)
      out.push_back(d);
  return out;
}

} // namespace

TEST_CASE("analyzeHeaderStaticDuplication needs mutability and >=2 TUs",
          "[AnnealHeaderStatic]") {
  GlobalIndex index;
  auto add = [&](const char *name, const char *file, bool isConst,
                 std::vector<std::string> tus) {
    HeaderStaticEntry e;
    e.name = name;
    e.filePath = file;
    e.line = 2;
    e.isConst = isConst;
    e.tuPaths = std::move(tus);
    index.addHeaderStatic(e);
  };

  add("gCounter", "state.hpp", false, {"a.cpp"});
  add("gCounter", "state.hpp", false, {"b.cpp"}); // merges: 2 TUs -> flag
  add("kTag", "tag.hpp", true, {"a.cpp", "b.cpp"}); // const: silent
  add("gLonely", "solo.hpp", false, {"only.cpp"});  // 1 TU: silent

  std::vector<Diagnostic> diags;
  analyzeHeaderStaticDuplication(index, diags);
  REQUIRE(diags.size() == 1);
  CHECK(diags[0].kind == Diagnostic::HeaderStatic_Duplicated);
  CHECK(diags[0].message.find("'gCounter'") != std::string::npos);
  CHECK(diags[0].message.find("2 TUs") != std::string::npos);
  CHECK(diags[0].message.find("a.cpp") != std::string::npos);
  CHECK(diags[0].message.find("b.cpp") != std::string::npos);
}

TEST_CASE("End-to-end: mutable header static in two TUs flagged; const, "
          "single-TU, and main-file statics silent",
          "[AnnealHeaderStatic]") {
  HeaderStaticFixture fx;
  auto compDb = fx.db();

  AnalysisOptions opts;
  opts.threadCount = 1;
  auto diags = dupDiags(runAnalysis(compDb, fx.files(), opts));

  REQUIRE(diags.size() == 1);
  const auto &d = diags[0];
  CHECK(d.message.find("'gCounter'") != std::string::npos);
  CHECK(d.message.find("2 TUs") != std::string::npos);
  CHECK(d.message.find("kTag") == std::string::npos);
  CHECK(d.message.find("kName") == std::string::npos);
  CHECK(d.message.find("gLonely") == std::string::npos);
  CHECK(d.message.find("gMainFileLocal") == std::string::npos);
}

TEST_CASE("header-static-duplication can be disabled via options",
          "[AnnealHeaderStatic]") {
  HeaderStaticFixture fx;
  auto compDb = fx.db();

  AnalysisOptions opts;
  opts.threadCount = 1;
  opts.enableHeaderStaticDiag = false;
  CHECK(dupDiags(runAnalysis(compDb, fx.files(), opts)).empty());
}

TEST_CASE("Header-static entries merge TU sets and survive absorb",
          "[AnnealHeaderStatic]") {
  GlobalIndex shard;
  HeaderStaticEntry e;
  e.name = "g";
  e.filePath = "g.hpp";
  e.line = 2;
  e.tuPaths = {"a.cpp"};
  shard.addHeaderStatic(e);
  e.tuPaths = {"b.cpp"};
  shard.addHeaderStatic(e);
  e.tuPaths = {"a.cpp"}; // duplicate TU: no growth
  shard.addHeaderStatic(e);
  CHECK(shard.headerStaticCount() == 1);

  GlobalIndex master;
  master.absorb(shard);
  master.absorb(shard);
  CHECK(master.headerStaticCount() == 1);
  master.forEachHeaderStatic([](const HeaderStaticEntry &entry) {
    CHECK(entry.tuPaths.size() == 2);
  });
}
