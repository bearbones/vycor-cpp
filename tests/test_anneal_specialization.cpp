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

// test_anneal_specialization.cpp — the specialization-visibility check:
// a TU implicitly instantiates a primary template while an explicit
// specialization exists in a header that TU does not include (IFNDR).

#include "vycor/anneal/Analyzer.h"
#include "vycor/anneal/GlobalIndex.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

#include <catch2/catch_test_macros.hpp>
#include <clang/Tooling/CompilationDatabase.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace vycor;

namespace {

// traits.hpp declares the primary Traits<T>; traits_int.hpp explicitly
// specializes Traits<int>. tu_bad instantiates Traits<int> WITHOUT the
// specialization header (IFNDR once tu_good exists); tu_good includes it;
// both also instantiate Traits<double>, which has no specialization
// anywhere and must stay silent.
struct SpecFixture {
  std::string dir = "anneal_spec_fixture";
  std::string absDir;

  SpecFixture() {
    REQUIRE(!llvm::sys::fs::create_directory(dir));
    llvm::SmallString<256> abs;
    REQUIRE(!llvm::sys::fs::real_path(dir, abs));
    absDir = std::string(abs.str());

    write("traits.hpp", R"cpp(
#pragma once
template <typename T> struct Traits {
  static int id() { return 0; }
};
)cpp");
    write("traits_int.hpp", R"cpp(
#pragma once
#include "traits.hpp"
template <> struct Traits<int> {
  static int id() { return 1; }
};
)cpp");
    write("tu_bad.cpp", R"cpp(
#include "traits.hpp"
int bad() { return Traits<int>::id() + Traits<double>::id(); }
)cpp");
    write("tu_good.cpp", R"cpp(
#include "traits.hpp"
#include "traits_int.hpp"
int good() { return Traits<int>::id() + Traits<double>::id(); }
)cpp");
  }

  ~SpecFixture() {
    for (const char *f :
         {"traits.hpp", "traits_int.hpp", "tu_bad.cpp", "tu_good.cpp"})
      std::remove((dir + "/" + f).c_str());
    llvm::sys::fs::remove(dir);
  }

  void write(const std::string &name, const std::string &content) const {
    std::ofstream out(dir + "/" + name, std::ios::binary | std::ios::trunc);
    REQUIRE(out.good());
    out << content;
  }

  std::vector<std::string> files() const {
    return {absDir + "/tu_bad.cpp", absDir + "/tu_good.cpp"};
  }

  clang::tooling::FixedCompilationDatabase db() const {
    return clang::tooling::FixedCompilationDatabase(".", {"-std=c++17"});
  }
};

std::vector<Diagnostic> specDiags(const std::vector<Diagnostic> &all) {
  std::vector<Diagnostic> out;
  for (const auto &d : all)
    if (d.kind == Diagnostic::Specialization_Invisible)
      out.push_back(d);
  return out;
}

} // namespace

TEST_CASE("GlobalIndex stores and finds specializations with dedup",
          "[AnnealSpecialization]") {
  GlobalIndex index;
  SpecializationEntry e;
  e.templateName = "Traits";
  e.argsString = "int";
  e.headerPath = "traits_int.hpp";
  e.line = 4;
  index.addSpecialization(e);
  index.addSpecialization(e); // N TUs including one header
  CHECK(index.specializationCount() == 1);

  e.argsString = "long";
  index.addSpecialization(e);
  CHECK(index.specializationCount() == 2);

  auto found = index.findSpecializations("Traits");
  CHECK(found.size() == 2);
  CHECK(index.findSpecializations("Other").empty());

  GlobalIndex master;
  master.absorb(index);
  master.absorb(index);
  CHECK(master.specializationCount() == 2);
}

TEST_CASE("Invisible explicit specialization is flagged in exactly the "
          "TU that cannot see it",
          "[AnnealSpecialization]") {
  SpecFixture fx;
  auto compDb = fx.db();

  AnalysisOptions opts;
  opts.threadCount = 1;
  auto diags = specDiags(runAnalysis(compDb, fx.files(), opts));

  REQUIRE(diags.size() == 1);
  const auto &d = diags[0];
  // Reported at tu_bad's instantiation, naming Traits<int> and the header
  // to include.
  CHECK(d.callLocation.find("tu_bad.cpp") != std::string::npos);
  CHECK(d.message.find("'Traits<int>'") != std::string::npos);
  CHECK(d.message.find("traits_int.hpp") != std::string::npos);
  CHECK(d.missingHeader.find("traits_int.hpp") != std::string::npos);
  // Traits<double> (no specialization anywhere) stays silent.
  CHECK(d.message.find("double") == std::string::npos);
}

TEST_CASE("specialization-visibility can be disabled via options",
          "[AnnealSpecialization]") {
  SpecFixture fx;
  auto compDb = fx.db();

  AnalysisOptions opts;
  opts.threadCount = 1;
  opts.enableSpecializationDiag = false;
  CHECK(specDiags(runAnalysis(compDb, fx.files(), opts)).empty());
}

TEST_CASE("Specialization entries survive parallel runs and match serial",
          "[AnnealSpecialization]") {
  SpecFixture fx;
  auto compDb = fx.db();

  AnalysisOptions serial;
  serial.threadCount = 1;
  auto serialDiags = specDiags(runAnalysis(compDb, fx.files(), serial));

  AnalysisOptions parallel;
  parallel.threadCount = 0;
  auto parallelDiags = specDiags(runAnalysis(compDb, fx.files(), parallel));

  REQUIRE(serialDiags.size() == parallelDiags.size());
  CHECK(serialDiags[0].message == parallelDiags[0].message);
}
