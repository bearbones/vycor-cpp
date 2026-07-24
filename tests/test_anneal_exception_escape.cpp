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

// test_anneal_exception_escape.cpp — the exception-escape check: noexcept
// functions that can transitively reach an uncaught throw across TUs.

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

// Cross-TU fixture: safeApi() noexcept (root.cpp) calls mid() (mid.cpp)
// which calls deepThrow() (thrower.cpp) — the throw is two TUs away, past
// where bugprone-exception-escape can see. guarded() wraps its call in
// try/catch and localSafe() throws only inside its own try: both silent.
struct EscapeFixture {
  std::string dir = "anneal_escape_fixture";
  std::string absDir;

  EscapeFixture() {
    REQUIRE(!llvm::sys::fs::create_directory(dir));
    llvm::SmallString<256> abs;
    REQUIRE(!llvm::sys::fs::real_path(dir, abs));
    absDir = std::string(abs.str());

    write("api.hpp", R"cpp(
#pragma once
namespace app {
void deepThrow();
void mid();
}
)cpp");
    write("thrower.cpp", R"cpp(
#include "api.hpp"
namespace app {
void deepThrow() { throw 42; }
}
)cpp");
    write("mid.cpp", R"cpp(
#include "api.hpp"
namespace app {
void mid() { deepThrow(); }
}
)cpp");
    write("root.cpp", R"cpp(
#include "api.hpp"
namespace app {
void safeApi() noexcept { mid(); }
void guarded() noexcept {
  try {
    mid();
  } catch (...) {
  }
}
void localSafe() noexcept {
  try {
    throw 1;
  } catch (...) {
  }
}
}
)cpp");
  }

  ~EscapeFixture() {
    for (const char *f : {"api.hpp", "thrower.cpp", "mid.cpp", "root.cpp"})
      std::remove((dir + "/" + f).c_str());
    llvm::sys::fs::remove(dir);
  }

  void write(const std::string &name, const std::string &content) const {
    std::ofstream out(dir + "/" + name, std::ios::binary | std::ios::trunc);
    REQUIRE(out.good());
    out << content;
  }

  std::vector<std::string> files() const {
    return {absDir + "/thrower.cpp", absDir + "/mid.cpp",
            absDir + "/root.cpp"};
  }

  clang::tooling::FixedCompilationDatabase db() const {
    return clang::tooling::FixedCompilationDatabase(".", {"-std=c++17"});
  }
};

std::vector<Diagnostic> escapes(const std::vector<Diagnostic> &all) {
  std::vector<Diagnostic> out;
  for (const auto &d : all)
    if (d.kind == Diagnostic::Exception_Escape)
      out.push_back(d);
  return out;
}

} // namespace

TEST_CASE("analyzeExceptionEscape follows unguarded calls only",
          "[AnnealExceptionEscape]") {
  GlobalIndex index;
  auto add = [&](const char *name, bool noexc, bool throws,
                 std::vector<std::string> unguarded = {},
                 std::vector<std::string> all = {}) {
    FunctionSummaryEntry e;
    e.qualifiedName = name;
    e.filePath = "f.cpp";
    e.line = 1;
    e.isNoexcept = noexc;
    e.hasUncaughtThrow = throws;
    e.unguardedCalls = std::move(unguarded);
    e.calledFunctions = all.empty() ? e.unguardedCalls : std::move(all);
    index.addFunctionSummary(e);
  };

  add("thrower", false, true);
  add("mid", false, false, {"thrower"});
  add("root", true, false, {"mid"});              // flagged: root->mid->thrower
  // guarded's call to mid is inside a try: present in calledFunctions but
  // NOT in unguardedCalls — silent.
  add("guarded", true, false, {}, {"mid"});
  add("selfThrow", true, true);                   // flagged: own body

  std::vector<Diagnostic> diags;
  analyzeExceptionEscape(index, diags);
  REQUIRE(diags.size() == 2);
  bool sawRoot = false, sawSelf = false;
  for (const auto &d : diags) {
    if (d.message.find("'root'") != std::string::npos) {
      sawRoot = true;
      CHECK(d.message.find("mid -> thrower") != std::string::npos);
    }
    if (d.message.find("'selfThrow'") != std::string::npos)
      sawSelf = true;
    CHECK(d.message.find("'guarded'") == std::string::npos);
  }
  CHECK(sawRoot);
  CHECK(sawSelf);
}

TEST_CASE("End-to-end: noexcept reaching a throw two TUs away is flagged; "
          "guarded and locally-handled stay silent",
          "[AnnealExceptionEscape]") {
  EscapeFixture fx;
  auto compDb = fx.db();

  AnalysisOptions opts;
  opts.threadCount = 1;
  opts.enableExceptionEscapeDiag = true;
  auto diags = escapes(runAnalysis(compDb, fx.files(), opts));

  REQUIRE(diags.size() == 1);
  const auto &d = diags[0];
  CHECK(d.message.find("'app::safeApi'") != std::string::npos);
  CHECK(d.message.find("app::mid -> app::deepThrow") != std::string::npos);
  CHECK(d.message.find("guarded") == std::string::npos);
  CHECK(d.message.find("localSafe") == std::string::npos);
}

TEST_CASE("exception-escape is off by default", "[AnnealExceptionEscape]") {
  EscapeFixture fx;
  auto compDb = fx.db();

  AnalysisOptions opts;
  opts.threadCount = 1;
  CHECK(escapes(runAnalysis(compDb, fx.files(), opts)).empty());
}

TEST_CASE("Function summaries merge across TUs and survive absorb",
          "[AnnealExceptionEscape]") {
  GlobalIndex shard;
  FunctionSummaryEntry e;
  e.qualifiedName = "f";
  e.filePath = "f.hpp";
  e.line = 2;
  e.unguardedCalls = {"a"};
  e.calledFunctions = {"a"};
  shard.addFunctionSummary(e);
  e.unguardedCalls = {"b"};
  e.calledFunctions = {"b"};
  e.hasUncaughtThrow = true;
  shard.addFunctionSummary(e); // merges into the first
  CHECK(shard.functionSummaryCount() == 1);

  GlobalIndex master;
  master.absorb(shard);
  master.absorb(shard);
  CHECK(master.functionSummaryCount() == 1);
  const auto *found = master.findFunctionSummary("f");
  REQUIRE(found);
  CHECK(found->hasUncaughtThrow);
  CHECK(found->unguardedCalls ==
        std::vector<std::string>{"a", "b"});
}
