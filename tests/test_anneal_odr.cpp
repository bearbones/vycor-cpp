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

// test_anneal_odr.cpp — the --odr-diag analysis: vague-linkage definitions
// (inline functions, in-class method bodies, class definitions) whose
// content differs across sites or across TUs. Uses a real
// CompilationDatabase loaded from a generated compile_commands.json so the
// two TUs can carry DIFFERENT -D flags (the divergent-definition case).

#include "vycor/anneal/Analyzer.h"
#include "vycor/anneal/Checkpoint.h"
#include "vycor/anneal/GlobalIndex.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

#include <catch2/catch_test_macros.hpp>
#include <clang/Tooling/CompilationDatabase.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace vycor;

namespace {

// Scratch project exercising every ODR case at once:
//  - magic():   ONE definition site whose body depends on -DMODE, which
//               only tu1 passes            -> ODR_DivergentDefinition
//  - dup():     TWO sites with different bodies (dup1.hpp vs dup2.hpp,
//               one per TU)                -> ODR_DuplicateDefinition
//  - same():    TWO sites, token-identical -> NOT flagged (benign copy)
//  - Cfg:       class defined differently in cls1.hpp vs cls2.hpp
//               (with a differing method body) -> ODR_DuplicateDefinition
//               for the class; its method diag is suppressed as an echo
struct OdrFixture {
  std::string dir = "anneal_odr_fixture";
  std::string absDir;

  OdrFixture() {
    REQUIRE(!llvm::sys::fs::create_directory(dir));
    llvm::SmallString<256> abs;
    REQUIRE(!llvm::sys::fs::real_path(dir, abs));
    absDir = std::string(abs.str());

    write("magic.hpp", R"cpp(
#pragma once
inline int magic() {
#ifdef MODE
  return 1;
#else
  return 0;
#endif
}
)cpp");
    write("dup1.hpp", "#pragma once\ninline int dup() { return 1; }\n");
    write("dup2.hpp", "#pragma once\ninline int dup() { return 2; }\n");
    const char *sameBody = "#pragma once\ninline int same() { return 7; }\n";
    write("same1.hpp", sameBody);
    write("same2.hpp", sameBody);
    write("cls1.hpp", R"cpp(
#pragma once
struct Cfg {
  int a = 0;
  int get() const { return a; }
};
)cpp");
    write("cls2.hpp", R"cpp(
#pragma once
struct Cfg {
  long a = 0;
  long b = 0;
  int get() const { return static_cast<int>(a + b); }
};
)cpp");
    write("tu1.cpp", R"cpp(
#include "magic.hpp"
#include "dup1.hpp"
#include "same1.hpp"
#include "cls1.hpp"
int tu1() { Cfg c; return magic() + dup() + same() + c.get(); }
)cpp");
    write("tu2.cpp", R"cpp(
#include "magic.hpp"
#include "dup2.hpp"
#include "same2.hpp"
#include "cls2.hpp"
int tu2() { Cfg c; return magic() + dup() + same() + c.get(); }
)cpp");

    // Real compilation database: tu1 gets -DMODE, tu2 doesn't — the
    // divergence FixedCompilationDatabase (uniform args) can't express.
    std::ofstream db(dir + "/compile_commands.json",
                     std::ios::binary | std::ios::trunc);
    db << "[\n"
       << entry("tu1.cpp", " -DMODE") << ",\n"
       << entry("tu2.cpp", "") << "\n]\n";
  }

  ~OdrFixture() {
    for (const char *f :
         {"magic.hpp", "dup1.hpp", "dup2.hpp", "same1.hpp", "same2.hpp",
          "cls1.hpp", "cls2.hpp", "tu1.cpp", "tu2.cpp",
          "compile_commands.json"})
      std::remove((dir + "/" + f).c_str());
    llvm::sys::fs::remove(dir);
  }

  std::string entry(const std::string &file, const std::string &extra) const {
    return "  {\"directory\": \"" + absDir + "\", \"command\": \"clang++ "
           "-std=c++17" + extra + " -c " + file + "\", \"file\": \"" + file +
           "\"}";
  }

  void write(const std::string &name, const std::string &content) const {
    std::ofstream out(dir + "/" + name, std::ios::binary | std::ios::trunc);
    REQUIRE(out.good());
    out << content;
  }

  std::vector<std::string> files() const {
    return {absDir + "/tu1.cpp", absDir + "/tu2.cpp"};
  }

  std::unique_ptr<clang::tooling::CompilationDatabase> db() const {
    std::string error;
    auto compDb =
        clang::tooling::CompilationDatabase::loadFromDirectory(absDir, error);
    REQUIRE(compDb);
    return compDb;
  }
};

std::vector<Diagnostic> odrDiags(const std::vector<Diagnostic> &all) {
  std::vector<Diagnostic> out;
  for (const auto &d : all)
    if (d.kind == Diagnostic::ODR_DuplicateDefinition ||
        d.kind == Diagnostic::ODR_DivergentDefinition)
      out.push_back(d);
  return out;
}

bool hasDiag(const std::vector<Diagnostic> &diags, Diagnostic::Kind kind,
             const std::string &nameInMessage) {
  return std::any_of(diags.begin(), diags.end(), [&](const Diagnostic &d) {
    return d.kind == kind &&
           d.message.find("'" + nameInMessage + "'") != std::string::npos;
  });
}

} // namespace

TEST_CASE("addOdrEntry dedups identical entries and survives absorb",
          "[AnnealOdr]") {
  OdrEntry e;
  e.qualifiedName = "magic";
  e.signature = "int ()";
  e.filePath = "magic.hpp";
  e.line = 3;
  e.odrHash = 42;

  GlobalIndex shard;
  shard.addOdrEntry(e);
  shard.addOdrEntry(e); // identical — the N-TUs-one-header case
  CHECK(shard.odrEntryCount() == 1);

  e.odrHash = 43; // same site, different body across TUs
  shard.addOdrEntry(e);
  CHECK(shard.odrEntryCount() == 2);

  GlobalIndex master;
  master.absorb(shard);
  master.absorb(shard); // absorbing twice must not duplicate either
  CHECK(master.odrEntryCount() == 2);
}

TEST_CASE("analyzeOdrViolations classifies divergent, duplicate, and benign "
          "groups",
          "[AnnealOdr]") {
  GlobalIndex index;
  auto add = [&](const char *name, bool isClass, const char *file,
                 unsigned line, uint64_t hash, const char *encClass = "") {
    OdrEntry e;
    e.qualifiedName = name;
    e.signature = isClass ? "" : "int ()";
    e.isClass = isClass;
    e.enclosingClass = encClass;
    e.filePath = file;
    e.line = line;
    e.odrHash = hash;
    index.addOdrEntry(e);
  };

  add("magic", false, "magic.hpp", 3, 1);
  add("magic", false, "magic.hpp", 3, 2);  // same site, two hashes
  add("dup", false, "dup1.hpp", 2, 10);
  add("dup", false, "dup2.hpp", 2, 20);    // two sites, two hashes
  add("same", false, "same1.hpp", 2, 7);
  add("same", false, "same2.hpp", 2, 7);   // two sites, one hash — benign
  add("Cfg", true, "cls1.hpp", 3, 100);
  add("Cfg", true, "cls2.hpp", 3, 200);    // duplicated class...
  add("Cfg::get", false, "cls1.hpp", 5, 101, "Cfg");
  add("Cfg::get", false, "cls2.hpp", 6, 201, "Cfg"); // ...suppresses this

  std::vector<Diagnostic> diags;
  analyzeOdrViolations(index, diags);

  CHECK(hasDiag(diags, Diagnostic::ODR_DivergentDefinition, "magic"));
  CHECK(hasDiag(diags, Diagnostic::ODR_DuplicateDefinition, "dup"));
  CHECK(hasDiag(diags, Diagnostic::ODR_DuplicateDefinition, "Cfg"));
  CHECK_FALSE(hasDiag(diags, Diagnostic::ODR_DuplicateDefinition, "same"));
  CHECK_FALSE(hasDiag(diags, Diagnostic::ODR_DuplicateDefinition, "Cfg::get"));
  CHECK(diags.size() == 3);
}

TEST_CASE("End-to-end: --odr-diag flags divergent and duplicate definitions "
          "over a real compilation database",
          "[AnnealOdr]") {
  OdrFixture fx;
  auto compDb = fx.db();

  AnalysisOptions opts;
  opts.threadCount = 1;
  opts.enableOdrDiag = true;
  auto diags = odrDiags(runAnalysis(*compDb, fx.files(), opts));

  // magic(): one site, body differs under -DMODE vs not.
  CHECK(hasDiag(diags, Diagnostic::ODR_DivergentDefinition, "magic"));
  // dup(): two headers, two bodies.
  CHECK(hasDiag(diags, Diagnostic::ODR_DuplicateDefinition, "dup"));
  // Cfg: two class definitions; method echo suppressed.
  CHECK(hasDiag(diags, Diagnostic::ODR_DuplicateDefinition, "Cfg"));
  CHECK_FALSE(hasDiag(diags, Diagnostic::ODR_DuplicateDefinition, "Cfg::get"));
  // same(): token-identical copies stay silent.
  for (const auto &d : diags)
    CHECK(d.message.find("'same'") == std::string::npos);
}

TEST_CASE("ODR analysis is off by default and collects nothing",
          "[AnnealOdr]") {
  OdrFixture fx;
  auto compDb = fx.db();

  AnalysisOptions opts;
  opts.threadCount = 1;
  auto diags = runAnalysis(*compDb, fx.files(), opts);
  CHECK(odrDiags(diags).empty());
}

TEST_CASE("ODR entries ride the checkpoint: warm resume reproduces the "
          "diagnostics without re-parsing",
          "[AnnealOdr]") {
  OdrFixture fx;
  auto compDb = fx.db();
  std::string ckptPath = "anneal_odr_ckpt.vycj";
  std::remove(ckptPath.c_str());

  AnalysisOptions opts;
  opts.threadCount = 1;
  opts.enableOdrDiag = true;
  opts.checkpointPath = ckptPath;

  auto cold = odrDiags(runAnalysis(*compDb, fx.files(), opts));
  REQUIRE(!cold.empty());

  auto warm = odrDiags(runAnalysis(*compDb, fx.files(), opts));
  auto key = [](const Diagnostic &d) {
    return std::to_string(static_cast<int>(d.kind)) + "|" + d.callLocation +
           "|" + d.message;
  };
  std::vector<std::string> coldKeys, warmKeys;
  for (const auto &d : cold)
    coldKeys.push_back(key(d));
  for (const auto &d : warm)
    warmKeys.push_back(key(d));
  std::sort(coldKeys.begin(), coldKeys.end());
  std::sort(warmKeys.begin(), warmKeys.end());
  CHECK(coldKeys == warmKeys);

  std::remove(ckptPath.c_str());
}
