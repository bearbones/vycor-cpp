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

// test_anneal_exception_spec.cpp — the exception-spec-divergence check:
// declaration sites disagreeing on whether a function can throw.

#include "vycor/anneal/Analyzer.h"
#include "vycor/anneal/GlobalIndex.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

#include <catch2/catch_test_macros.hpp>
#include <clang/Tooling/CompilationDatabase.h>

#include <cstdio>
#include <fstream>
#include <memory>
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

// The shim-header scenario: sdk.h declares sdk_init() potentially-throwing,
// shim.hpp re-declares it noexcept; tu_a sees only the vendor header,
// tu_b only the shim — no TU sees both, so no compiler ever objects.
// ok() is declared noexcept consistently in both headers (silent), and
// g(int) noexcept vs g(double) throwing are different overloads (silent).
struct SpecDivergenceFixture {
  std::string dir = "anneal_xspec_fixture";
  std::string absDir;

  SpecDivergenceFixture() {
    REQUIRE(makeUniqueFixtureDir(dir));
    llvm::SmallString<256> abs;
    REQUIRE(!llvm::sys::fs::real_path(dir, abs));
    absDir = std::string(abs.str());

    write("sdk.h", R"cpp(
#pragma once
int sdk_init();
void ok() noexcept;
void g(int) noexcept;
void g(double);
)cpp");
    write("shim.hpp", R"cpp(
#pragma once
int sdk_init() noexcept;
void ok() noexcept;
)cpp");
    write("tu_a.cpp", R"cpp(
#include "sdk.h"
int a() { ok(); g(1); g(2.0); return sdk_init(); }
)cpp");
    write("tu_b.cpp", R"cpp(
#include "shim.hpp"
int b() { ok(); return sdk_init(); }
)cpp");
  }

  ~SpecDivergenceFixture() {
    for (const char *f : {"sdk.h", "shim.hpp", "tu_a.cpp", "tu_b.cpp"})
      std::remove((dir + "/" + f).c_str());
    llvm::sys::fs::remove(dir);
  }

  void write(const std::string &name, const std::string &content) const {
    std::ofstream out(dir + "/" + name, std::ios::binary | std::ios::trunc);
    REQUIRE(out.good());
    out << content;
  }

  std::vector<std::string> files() const {
    return {absDir + "/tu_a.cpp", absDir + "/tu_b.cpp"};
  }

  clang::tooling::FixedCompilationDatabase db() const {
    return clang::tooling::FixedCompilationDatabase(".", {"-std=c++17"});
  }
};

std::vector<Diagnostic> specDiags(const std::vector<Diagnostic> &all) {
  std::vector<Diagnostic> out;
  for (const auto &d : all)
    if (d.kind == Diagnostic::ExceptionSpec_Divergent)
      out.push_back(d);
  return out;
}

} // namespace

TEST_CASE("analyzeExceptionSpecDivergence flags cross-site and same-site "
          "disagreement, not overloads or agreement",
          "[AnnealExceptionSpec]") {
  GlobalIndex index;
  auto add = [&](const char *name, const char *sig, bool noexc,
                 const char *file, unsigned line) {
    ExceptionSpecEntry e;
    e.qualifiedName = name;
    e.signature = sig;
    e.isNoexcept = noexc;
    e.filePath = file;
    e.line = line;
    index.addExceptionSpec(e);
  };

  add("sdk_init", "()", false, "sdk.h", 2);
  add("sdk_init", "()", true, "shim.hpp", 2);  // cross-site conflict
  add("ok", "()", true, "sdk.h", 3);
  add("ok", "()", true, "shim.hpp", 3);        // agreement: silent
  add("g", "(int)", true, "sdk.h", 4);
  add("g", "(double)", false, "sdk.h", 5);     // different overloads: silent
  add("tune", "()", true, "tune.hpp", 2);
  add("tune", "()", false, "tune.hpp", 2);     // SAME site, both ways

  std::vector<Diagnostic> diags;
  analyzeExceptionSpecDivergence(index, diags);
  REQUIRE(diags.size() == 2);
  bool sawSdk = false, sawTune = false;
  for (const auto &d : diags) {
    if (d.message.find("'sdk_init'") != std::string::npos) {
      sawSdk = true;
      CHECK(d.message.find("shim.hpp:2") != std::string::npos);
      CHECK(d.message.find("sdk.h:2") != std::string::npos);
    }
    if (d.message.find("'tune'") != std::string::npos) {
      sawTune = true;
      CHECK(d.message.find("preprocessor state") != std::string::npos);
    }
    CHECK(d.message.find("'ok'") == std::string::npos);
    CHECK(d.message.find("'g'") == std::string::npos);
  }
  CHECK(sawSdk);
  CHECK(sawTune);
}

TEST_CASE("End-to-end: the shim-header contradiction is flagged; agreement "
          "and overloads are silent",
          "[AnnealExceptionSpec]") {
  SpecDivergenceFixture fx;
  auto compDb = fx.db();

  AnalysisOptions opts;
  opts.threadCount = 1;
  auto diags = specDiags(runAnalysis(compDb, fx.files(), opts));

  REQUIRE(diags.size() == 1);
  const auto &d = diags[0];
  CHECK(d.message.find("'sdk_init'") != std::string::npos);
  CHECK(d.message.find("shim.hpp") != std::string::npos);
  CHECK(d.message.find("sdk.h") != std::string::npos);
  CHECK(d.message.find("'ok'") == std::string::npos);
}

TEST_CASE("End-to-end: noexcept(MACRO) resolving differently per compile "
          "command is flagged at its single site",
          "[AnnealExceptionSpec]") {
  // Real compilation database with per-TU -D flags, like the ODR fixture.
  std::string dir = "anneal_xspec_macro_fixture";
  REQUIRE(makeUniqueFixtureDir(dir));
  llvm::SmallString<256> abs;
  REQUIRE(!llvm::sys::fs::real_path(dir, abs));
  const std::string absDir(abs.str());

  auto write = [&](const std::string &name, const std::string &content) {
    std::ofstream out(dir + "/" + name, std::ios::binary | std::ios::trunc);
    REQUIRE(out.good());
    out << content;
  };
  write("tune.hpp", "#pragma once\nvoid tune() noexcept(FAST_PATH);\n");
  write("fast.cpp", "#include \"tune.hpp\"\nvoid f() { tune(); }\n");
  write("slow.cpp", "#include \"tune.hpp\"\nvoid s() { tune(); }\n");
  {
    std::ofstream db(dir + "/compile_commands.json",
                     std::ios::binary | std::ios::trunc);
    db << "[\n  {\"directory\": \"" << absDir
       << "\", \"command\": \"clang++ -std=c++17 -DFAST_PATH=true -c "
          "fast.cpp\", \"file\": \"fast.cpp\"},\n  {\"directory\": \""
       << absDir
       << "\", \"command\": \"clang++ -std=c++17 -DFAST_PATH=false -c "
          "slow.cpp\", \"file\": \"slow.cpp\"}\n]\n";
  }

  std::string error;
  auto compDb =
      clang::tooling::CompilationDatabase::loadFromDirectory(absDir, error);
  REQUIRE(compDb);

  AnalysisOptions opts;
  opts.threadCount = 1;
  auto diags = specDiags(runAnalysis(
      *compDb, {absDir + "/fast.cpp", absDir + "/slow.cpp"}, opts));

  REQUIRE(diags.size() == 1);
  CHECK(diags[0].message.find("'tune'") != std::string::npos);
  CHECK(diags[0].message.find("preprocessor state") != std::string::npos);

  for (const char *f :
       {"tune.hpp", "fast.cpp", "slow.cpp", "compile_commands.json"})
    std::remove((dir + "/" + f).c_str());
  llvm::sys::fs::remove(dir);
}

TEST_CASE("exception-spec-divergence can be disabled via options",
          "[AnnealExceptionSpec]") {
  SpecDivergenceFixture fx;
  auto compDb = fx.db();

  AnalysisOptions opts;
  opts.threadCount = 1;
  opts.enableExceptionSpecDiag = false;
  CHECK(specDiags(runAnalysis(compDb, fx.files(), opts)).empty());
}

TEST_CASE("Exception-spec entries dedup and survive absorb",
          "[AnnealExceptionSpec]") {
  GlobalIndex shard;
  ExceptionSpecEntry e;
  e.qualifiedName = "f";
  e.signature = "()";
  e.isNoexcept = true;
  e.filePath = "f.hpp";
  e.line = 2;
  shard.addExceptionSpec(e);
  shard.addExceptionSpec(e); // identical: dedup
  CHECK(shard.exceptionSpecCount() == 1);
  e.isNoexcept = false; // same site, other resolution: kept
  shard.addExceptionSpec(e);
  CHECK(shard.exceptionSpecCount() == 2);

  GlobalIndex master;
  master.absorb(shard);
  master.absorb(shard);
  CHECK(master.exceptionSpecCount() == 2);
}
