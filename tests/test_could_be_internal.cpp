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

// test_could_be_internal.cpp — the could-be-internal check
// (docs/checks/could-be-internal.md) over reference facts baked from
// small multi-TU projects.

#include "vycor/callgraph/ControlFlowIndex.h"
#include "vycor/check/CouldBeInternal.h"
#include "vycor/check/Report.h"

#include <catch2/catch_test_macros.hpp>
#include <clang/Tooling/CompilationDatabase.h>

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Regex.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace vycor;

namespace {

struct Project {
  std::string dir;
  std::vector<std::string> tus;

  Project() {
    llvm::SmallString<128> tmp;
    REQUIRE_FALSE(llvm::sys::fs::createUniqueDirectory("vycor_cbi", tmp));
    llvm::SmallString<128> real;
    REQUIRE_FALSE(llvm::sys::fs::real_path(tmp, real));
    dir = std::string(real.str());
  }
  Project(const Project &) = delete;
  Project &operator=(const Project &) = delete;
  ~Project() { llvm::sys::fs::remove_directories(dir); }

  std::string path(const std::string &name) const { return dir + "/" + name; }

  void file(const std::string &name, const std::string &code) const {
    llvm::SmallString<128> parent(path(name));
    llvm::sys::path::remove_filename(parent);
    REQUIRE_FALSE(llvm::sys::fs::create_directories(parent));
    std::ofstream out(path(name));
    out << code;
  }
  void tu(const std::string &name, const std::string &code) {
    file(name, code);
    tus.push_back(path(name));
  }

  BakedIndexes bake(std::vector<std::string> files) const {
    clang::tooling::FixedCompilationDatabase compDb(
        dir, {"-std=c++17", "-I" + dir});
    return bakeIndexes(compDb, files, {}, 1);
  }
  BakedIndexes bake() const { return bake(tus); }
};

// The fixture every case below reads: one library TU with a symbol for
// each tier and each exclusion, a user TU, and a test TU.
void writeFixture(Project &p) {
  p.file("api.h", R"(
int shared();
int headerOnly();
extern int sharedVar;
int forTests();
)");
  p.file("defined_in_header.h", R"(
int headerDefined() { return 7; }
)");
  p.tu("lib.cpp", R"(
#include "api.h"
#include "defined_in_header.h"
int lonely() { return 1; }
int lonelyVar = 3;
int shared() { return lonely() + lonelyVar; }
int headerOnly() { return 2; }
int sharedVar = 4;
int forTests() { return 5; }
static int already() { return 0; }
namespace { int anon() { return already(); } }
extern "C" int cfunc() { return anon(); }
__attribute__((used)) int kept() { return 0; }
__attribute__((visibility("default"))) int exported() { return 0; }
__attribute__((weak)) int weakly() { return 0; }
struct S { void method(); static int member; };
void S::method() {}
int S::member = 0;
template <typename T> T pick(T t) { return t; }
template <> int pick<int>(int t) { return t + 1; }
template long pick<long>(long);
inline int inl() { return 0; }
int entry() { return 0; }
void *operator new(decltype(sizeof 0) n) { return __builtin_malloc(n); }
int main() { return shared() + inl() + headerDefined(); }
)");
  p.tu("user.cpp", R"(
#include "api.h"
int useShared() { return shared() + sharedVar; }
)");
  p.tu("tests/lib_test.cpp", R"(
#include "api.h"
int testForTests() { return forTests(); }
)");
}

std::map<std::string, std::string> byName(const std::vector<Diagnostic> &diags) {
  std::map<std::string, std::string> out;
  for (const Diagnostic &d : diags)
    out[d.resolvedDecl] = diagnosticKindName(d.kind);
  return out;
}

CouldBeInternalOptions defaultOptions() {
  CouldBeInternalOptions opts;
  opts.entryPoints = {"entry"};
  opts.testPathPattern = kDefaultTestPathPattern;
  return opts;
}

} // namespace

TEST_CASE("could-be-internal: each tier, and every exclusion stays silent",
          "[could-be-internal]") {
  Project p;
  writeFixture(p);
  BakedIndexes baked = p.bake();
  std::string error;
  auto diags = findCouldBeInternal(baked.references, defaultOptions(), error);
  REQUIRE(error.empty());

  const std::map<std::string, std::string> expected = {
      // Tier 1: no header declaration, no reference from another TU.
      {"lonely", "Internal_Unreferenced"},
      {"lonelyVar", "Internal_Unreferenced"},
      {"useShared", "Internal_Unreferenced"},
      {"testForTests", "Internal_Unreferenced"},
      // Tier 2: declared in a header, referenced by no other TU.
      {"headerOnly", "Internal_DeclaredInHeader"},
      // Referenced from another TU only by a test source.
      {"forTests", "Internal_TestOnly"},
  };
  CHECK(byName(diags) == expected);
  // Silent: referenced from another TU (shared, sharedVar), internal
  // already (already, anon), C linkage, used, visibility, weak, members,
  // the template, its explicit specialization and instantiation, inline,
  // a configured entry point, a replacement operator new, main, and a
  // definition in a header.
}

TEST_CASE("could-be-internal: findings carry the definition site and usr",
          "[could-be-internal]") {
  Project p;
  writeFixture(p);
  BakedIndexes baked = p.bake();
  std::string error;
  auto diags = findCouldBeInternal(baked.references, defaultOptions(), error);
  auto it = std::find_if(diags.begin(), diags.end(), [](const Diagnostic &d) {
    return d.resolvedDecl == "lonely";
  });
  REQUIRE(it != diags.end());
  CHECK(it->callLocation == p.path("lib.cpp") + ":4");
  CHECK(it->file == p.path("lib.cpp"));
  REQUIRE(it->entities.size() == 1);
  CHECK(it->entities[0].find("lonely") != std::string::npos);
  CHECK(it->message.find("function 'lonely' has external linkage") !=
        std::string::npos);

  auto header = std::find_if(diags.begin(), diags.end(),
                             [](const Diagnostic &d) {
                               return d.resolvedDecl == "headerOnly";
                             });
  REQUIRE(header != diags.end());
  CHECK(header->message.find(p.path("api.h") + ":3") != std::string::npos);

  auto test = std::find_if(diags.begin(), diags.end(), [](const Diagnostic &d) {
    return d.resolvedDecl == "forTests";
  });
  REQUIRE(test != diags.end());
  CHECK(test->message.find(p.path("tests/lib_test.cpp") + ":3") !=
        std::string::npos);

  // Ordered by definition site: file, then line.
  for (size_t i = 1; i < diags.size(); ++i) {
    INFO(diags[i - 1].callLocation << " then " << diags[i].callLocation);
    CHECK(diags[i - 1].file <= diags[i].file);
  }
  std::vector<std::string> libOrder;
  for (const Diagnostic &d : diags)
    if (d.file == p.path("lib.cpp"))
      libOrder.push_back(d.resolvedDecl);
  CHECK(libOrder == std::vector<std::string>{"lonely", "lonelyVar",
                                             "headerOnly", "forTests"});
}

TEST_CASE("could-be-internal: without test detection a test reference counts",
          "[could-be-internal]") {
  Project p;
  writeFixture(p);
  BakedIndexes baked = p.bake();
  CouldBeInternalOptions opts = defaultOptions();
  opts.testPathPattern.clear();
  std::string error;
  auto found = byName(findCouldBeInternal(baked.references, opts, error));
  CHECK(found.count("forTests") == 0);
  CHECK(found.at("lonely") == "Internal_Unreferenced");
}

TEST_CASE("could-be-internal: test paths are matched under the project root",
          "[could-be-internal]") {
  // The whole fixture sits under a directory named tests/: without the
  // root every path, the definer's included, looks like a test source,
  // and a test-support symbol's test references are ordinary uses.
  Project outer;
  Project &p = outer;
  const std::string base = "tests/checkout/";
  p.file(base + "api.h", "int forTests();\n");
  p.tu(base + "lib.cpp", "#include \"tests/checkout/api.h\"\n"
                         "int forTests() { return 5; }\n");
  p.tu(base + "tests/x_test.cpp", "#include \"tests/checkout/api.h\"\n"
                                  "int user() { return forTests(); }\n");
  BakedIndexes baked = p.bake();
  CouldBeInternalOptions opts = defaultOptions();
  std::string error;
  CHECK(byName(findCouldBeInternal(baked.references, opts, error))
            .count("forTests") == 0);
  opts.projectRoot = p.path("tests/checkout") + "/";
  CHECK(byName(findCouldBeInternal(baked.references, opts, error))
            .at("forTests") == "Internal_TestOnly");
}

TEST_CASE("could-be-internal: a configured entry point is excluded by name",
          "[could-be-internal]") {
  Project p;
  writeFixture(p);
  BakedIndexes baked = p.bake();
  CouldBeInternalOptions opts = defaultOptions();
  opts.entryPoints.clear();
  std::string error;
  auto found = byName(findCouldBeInternal(baked.references, opts, error));
  CHECK(found.at("entry") == "Internal_Unreferenced");
}

TEST_CASE("could-be-internal: incomplete coverage turns tier 1 into a note "
          "that says why",
          "[could-be-internal]") {
  Project p;
  writeFixture(p);
  // user.cpp is not baked: its reference to shared and sharedVar is gone.
  BakedIndexes baked = p.bake({p.path("lib.cpp"), p.path("tests/lib_test.cpp")});
  CouldBeInternalOptions opts = defaultOptions();
  opts.coverageGap = describeCoverageGap(3, 2, baked.outcomes);
  REQUIRE(opts.coverageGap ==
          "the facts cover 2 of the 3 translation units in the compilation "
          "database");
  std::string error;
  auto diags = findCouldBeInternal(baked.references, opts, error);
  auto found = byName(diags);
  CHECK(found.at("lonely") == "Internal_Unproven");
  CHECK(found.at("sharedVar") == "Internal_DeclaredInHeader");
  CHECK(found.at("forTests") == "Internal_TestOnly");
  CHECK(found.count("useShared") == 0);
  for (const Diagnostic &d : diags) {
    INFO(d.message);
    CHECK(d.message.find("Not proven: the facts cover 2 of the 3") !=
          std::string::npos);
    CHECK(d.kind != Diagnostic::Internal_Unreferenced);
  }
}

TEST_CASE("could-be-internal: the coverage gap in words",
          "[could-be-internal]") {
  TuOutcomes outcomes;
  outcomes["/a.cpp"] = {TuStatus::Indexed, ""};
  CHECK(describeCoverageGap(1, 1, outcomes).empty());
  CHECK(describeCoverageGap(0, 1, outcomes) ==
        "the compilation database does not list the program's translation "
        "units");
  outcomes["/b.cpp"] = {TuStatus::Partial, ""};
  outcomes["/c.cpp"] = {TuStatus::Crashed, "signal 11"};
  outcomes["/d.cpp"] = {TuStatus::TimedOut, ""};
  CHECK(describeCoverageGap(4, 4, outcomes) ==
        "2 translation unit(s) did not parse; 1 translation unit(s) parsed "
        "with errors");
}

TEST_CASE("could-be-internal: an invalid test path pattern is an error",
          "[could-be-internal]") {
  ReferenceIndex empty;
  CouldBeInternalOptions opts;
  opts.testPathPattern = "(unclosed";
  std::string error;
  CHECK(findCouldBeInternal(empty, opts, error).empty());
  CHECK(error.find("invalid test path pattern") != std::string::npos);
}

TEST_CASE("could-be-internal: the default test path pattern",
          "[could-be-internal]") {
  const llvm::Regex re(kDefaultTestPathPattern);
  REQUIRE(re.isValid());
  for (const char *path :
       {"/src/tests/a.cpp", "/src/unittests/Support/Foo.cpp",
        "/src/test/x.cc", "/src/lib/test_util.cpp", "/src/lib/foo_test.cpp",
        "/src/lib/foo-unittest.cc", "/src/lib/FooTest.cpp",
        "/src/lib/FooTests.cpp"}) {
    INFO(path);
    CHECK(re.match(path));
  }
  for (const char *path : {"/src/lib/attest.cpp", "/src/lib/Testing.cpp",
                           "/src/lib/contest/a.cpp", "/src/lib/latest.cpp"}) {
    INFO(path);
    CHECK_FALSE(re.match(path));
  }
}

TEST_CASE("could-be-internal: findings do not depend on TU order",
          "[could-be-internal]") {
  Project p;
  writeFixture(p);
  std::vector<std::string> reversed(p.tus.rbegin(), p.tus.rend());
  BakedIndexes forward = p.bake();
  BakedIndexes backward = p.bake(reversed);
  std::string error;
  auto a = findCouldBeInternal(forward.references, defaultOptions(), error);
  auto b = findCouldBeInternal(backward.references, defaultOptions(), error);
  REQUIRE(a.size() == b.size());
  for (size_t i = 0; i < a.size(); ++i) {
    CHECK(a[i].callLocation == b[i].callLocation);
    CHECK(a[i].message == b[i].message);
    CHECK(a[i].entities == b[i].entities);
  }
}

TEST_CASE("could-be-internal: which references from other TUs count",
          "[could-be-internal]") {
  Project p;
  // Test-support code: a helper defined in a test source and used by a
  // test is shared by the tests, not test-only.
  p.file("tests/support.h", "int testHelper();\n");
  p.tu("tests/support.cpp", "#include \"tests/support.h\"\n"
                            "int testHelper() { return 1; }\n");
  // A production header that only the test TU includes: the witness is
  // in the header, the one TU holding it is a test.
  p.file("wrap.h", "int viaTestHeader();\n"
                   "inline int wrapT() { return viaTestHeader(); }\n");
  // An inline function in a header two TUs include: its reference is
  // held by both, the definer and another TU.
  p.file("inline_user.h", "int fromInline();\n"
                          "inline int wrapInline() { return fromInline(); }\n");
  p.tu("lib.cpp", "#include \"inline_user.h\"\n"
                  "int fromInline() { return 2; }\n"
                  "int viaTestHeader() { return 3; }\n");
  p.tu("user.cpp", "#include \"inline_user.h\"\n"
                   "int useInline() { return wrapInline(); }\n");
  p.tu("tests/a_test.cpp", "#include \"tests/support.h\"\n"
                           "#include \"wrap.h\"\n"
                           "int runA() { return testHelper() + wrapT(); }\n");
  BakedIndexes baked = p.bake();
  std::string error;
  auto diags = findCouldBeInternal(baked.references, defaultOptions(), error);
  REQUIRE(error.empty());
  const auto found = byName(diags);
  CHECK(found.count("testHelper") == 0);
  CHECK(found.count("fromInline") == 0);
  REQUIRE(found.count("viaTestHeader") == 1);
  CHECK(found.at("viaTestHeader") == "Internal_TestOnly");
  auto via = std::find_if(diags.begin(), diags.end(), [](const Diagnostic &d) {
    return d.resolvedDecl == "viaTestHeader";
  });
  CHECK(via->message.find(p.path("wrap.h") + ":2") != std::string::npos);
  CHECK(found.at("runA") == "Internal_Unreferenced");
  CHECK(found.at("useInline") == "Internal_Unreferenced");
}
