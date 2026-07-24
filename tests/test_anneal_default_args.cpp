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

// test_anneal_default_args.cpp — the default-arg-divergence check:
// declaration sites that disagree on a parameter's default argument.

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

// log_a.hpp and log_b.hpp both declare myorg::log with CONFLICTING default
// levels; each TU includes one of them. okfn is declared with a default in
// one header and redeclared WITHOUT one elsewhere (the common
// header-declares/cpp-adds pattern) — must stay silent.
struct DefaultArgFixture {
  std::string dir = "anneal_defarg_fixture";
  std::string absDir;

  DefaultArgFixture() {
    REQUIRE(!llvm::sys::fs::create_directory(dir));
    llvm::SmallString<256> abs;
    REQUIRE(!llvm::sys::fs::real_path(dir, abs));
    absDir = std::string(abs.str());

    write("log_a.hpp", R"cpp(
#pragma once
namespace myorg { void log(const char *msg, int level = 1); }
)cpp");
    write("log_b.hpp", R"cpp(
#pragma once
namespace myorg { void log(const char *msg, int level = 2); }
)cpp");
    write("ok.hpp", R"cpp(
#pragma once
namespace myorg { int okfn(int x = 7); }
)cpp");
    write("tu_a.cpp", R"cpp(
#include "log_a.hpp"
#include "ok.hpp"
namespace myorg { int okfn(int x) { return x; } } // no default: legal, silent
void a() { myorg::log("a"); }
)cpp");
    write("tu_b.cpp", R"cpp(
#include "log_b.hpp"
void b() { myorg::log("b"); }
)cpp");
  }

  ~DefaultArgFixture() {
    for (const char *f :
         {"log_a.hpp", "log_b.hpp", "ok.hpp", "tu_a.cpp", "tu_b.cpp"})
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

std::vector<Diagnostic> defArgDiags(const std::vector<Diagnostic> &all) {
  std::vector<Diagnostic> out;
  for (const auto &d : all)
    if (d.kind == Diagnostic::DefaultArg_Divergent)
      out.push_back(d);
  return out;
}

} // namespace

TEST_CASE("analyzeDefaultArgDivergence flags conflicts, ignores single-site "
          "and absence-vs-presence",
          "[AnnealDefaultArgs]") {
  GlobalIndex index;
  auto add = [&](const char *name, unsigned param, const char *text,
                 const char *file, unsigned line) {
    DefaultArgEntry e;
    e.qualifiedName = name;
    e.signature = "void (const char *, int)";
    e.paramIndex = param;
    e.paramName = "level";
    e.defaultText = text;
    e.filePath = file;
    e.line = line;
    index.addDefaultArg(e);
  };

  add("myorg::log", 1, "1", "log_a.hpp", 3);
  add("myorg::log", 1, "2", "log_b.hpp", 3);  // conflict
  add("myorg::quiet", 0, "true", "q.hpp", 1); // single site — silent
  add("myorg::quiet", 0, "true", "q2.hpp", 9); // same text elsewhere — silent

  std::vector<Diagnostic> diags;
  analyzeDefaultArgDivergence(index, diags);
  REQUIRE(diags.size() == 1);
  CHECK(diags[0].kind == Diagnostic::DefaultArg_Divergent);
  CHECK(diags[0].message.find("'myorg::log'") != std::string::npos);
  CHECK(diags[0].message.find("'level'") != std::string::npos);
  CHECK(diags[0].message.find("log_a.hpp:3") != std::string::npos);
  CHECK(diags[0].message.find("log_b.hpp:3") != std::string::npos);
}

TEST_CASE("End-to-end: conflicting defaults across headers are flagged; the "
          "add-no-default redeclaration pattern is not",
          "[AnnealDefaultArgs]") {
  DefaultArgFixture fx;
  auto compDb = fx.db();

  AnalysisOptions opts;
  opts.threadCount = 1;
  auto diags = defArgDiags(runAnalysis(compDb, fx.files(), opts));

  REQUIRE(diags.size() == 1);
  CHECK(diags[0].message.find("'myorg::log'") != std::string::npos);
  CHECK(diags[0].message.find("log_a.hpp") != std::string::npos);
  CHECK(diags[0].message.find("log_b.hpp") != std::string::npos);
  // okfn (default in header, none on the defining redeclaration) is silent.
  CHECK(diags[0].message.find("okfn") == std::string::npos);
}

TEST_CASE("default-arg-divergence can be disabled via options",
          "[AnnealDefaultArgs]") {
  DefaultArgFixture fx;
  auto compDb = fx.db();

  AnalysisOptions opts;
  opts.threadCount = 1;
  opts.enableDefaultArgDiag = false;
  CHECK(defArgDiags(runAnalysis(compDb, fx.files(), opts)).empty());
}

TEST_CASE("Default-arg entries dedup and survive absorb",
          "[AnnealDefaultArgs]") {
  GlobalIndex shard;
  DefaultArgEntry e;
  e.qualifiedName = "f";
  e.signature = "void (int)";
  e.paramIndex = 0;
  e.defaultText = "42";
  e.filePath = "f.hpp";
  e.line = 2;
  shard.addDefaultArg(e);
  shard.addDefaultArg(e);
  CHECK(shard.defaultArgCount() == 1);

  GlobalIndex master;
  master.absorb(shard);
  master.absorb(shard);
  CHECK(master.defaultArgCount() == 1);
}
