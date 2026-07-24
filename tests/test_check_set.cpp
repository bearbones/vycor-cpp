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

// test_check_set.cpp — the named-check selection layer (--checks /
// .vycor-anneal.json): spec resolution, groups, config parsing, discovery.

#include "vycor/anneal/CheckSet.h"
#include "vycor/ext/Extensions.h"

#include "llvm/Support/FileSystem.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <fstream>
#include <string>

using namespace vycor;

namespace {

struct RegistryReset {
  RegistryReset() { ExtensionRegistry::instance().clear(); }
  ~RegistryReset() { ExtensionRegistry::instance().clear(); }
};

class FakeIndexCheck : public IndexCheck {
public:
  std::string name() const override { return "org-index-check"; }
  void check(const GlobalIndex &, std::vector<Diagnostic> &) override {}
};

} // namespace

TEST_CASE("Default check set is the defaultOn built-ins plus org checks",
          "[CheckSet]") {
  RegistryReset reset;
  auto defaults = defaultCheckSet();
  CHECK(defaults.count("adl-visibility"));
  CHECK(defaults.count("ctad-visibility"));
  CHECK(defaults.count("specialization-visibility"));
  CHECK_FALSE(defaults.count("odr-violations"));
  CHECK_FALSE(defaults.count("coverage-properties"));
  CHECK_FALSE(defaults.count("dead-code"));

  ExtensionRegistry::instance().addIndexCheck(
      [] { return std::make_unique<FakeIndexCheck>(); });
  CHECK(defaultCheckSet().count("org-index-check"));
}

TEST_CASE("resolveCheckSpec applies entries in order with group expansion",
          "[CheckSet]") {
  RegistryReset reset;
  std::string error;

  SECTION("enable and disable individual checks") {
    auto enabled = defaultCheckSet();
    REQUIRE(resolveCheckSpec({"odr-violations", "-adl-visibility"}, enabled,
                             error));
    CHECK(enabled.count("odr-violations"));
    CHECK_FALSE(enabled.count("adl-visibility"));
    CHECK(enabled.count("ctad-visibility")); // untouched default
  }

  SECTION("'all' enables everything; later entries win") {
    std::set<std::string> enabled;
    REQUIRE(resolveCheckSpec({"all", "-dead-code"}, enabled, error));
    CHECK(enabled.count("odr-violations"));
    CHECK(enabled.count("coverage-properties"));
    CHECK_FALSE(enabled.count("dead-code"));
  }

  SECTION("built-in group labels expand") {
    std::set<std::string> enabled;
    REQUIRE(resolveCheckSpec({"all", "-compute-heavy"}, enabled, error));
    CHECK_FALSE(enabled.count("odr-violations"));
    CHECK_FALSE(enabled.count("dead-code"));
    CHECK(enabled.count("adl-visibility"));
  }

  SECTION("'-all' clears, explicit list rebuilds (worker forwarding shape)") {
    auto enabled = defaultCheckSet();
    REQUIRE(resolveCheckSpec({"-all", "ctad-visibility"}, enabled, error));
    CHECK(enabled == std::set<std::string>{"ctad-visibility"});
  }

  SECTION("unknown names are a hard error") {
    auto enabled = defaultCheckSet();
    CHECK_FALSE(resolveCheckSpec({"adl-visibilty"}, enabled, error));
    CHECK(error.find("adl-visibilty") != std::string::npos);
  }

  SECTION("org checks and org groups resolve") {
    ExtensionRegistry::instance().addIndexCheck(
        [] { return std::make_unique<FakeIndexCheck>(); });
    ExtensionRegistry::instance().addCheckGroup(
        "org-strict", {"org-index-check", "odr-violations"});
    std::set<std::string> enabled;
    REQUIRE(resolveCheckSpec({"org-strict"}, enabled, error));
    CHECK(enabled ==
          std::set<std::string>{"org-index-check", "odr-violations"});
    REQUIRE(resolveCheckSpec({"-org-strict"}, enabled, error));
    CHECK(enabled.empty());
  }
}

TEST_CASE("parseChecksConfigJson accepts the schema and rejects typos",
          "[CheckSet]") {
  std::vector<std::string> spec;
  std::string error;

  REQUIRE(parseChecksConfigJson(R"({"checks": ["all", "-noisy"]})", spec,
                                error));
  CHECK(spec == std::vector<std::string>{"all", "-noisy"});

  spec.clear();
  REQUIRE(parseChecksConfigJson("{}", spec, error)); // empty config is a no-op
  CHECK(spec.empty());

  CHECK_FALSE(parseChecksConfigJson(R"({"cheks": []})", spec, error));
  CHECK(error.find("cheks") != std::string::npos);
  CHECK_FALSE(parseChecksConfigJson(R"({"checks": "all"})", spec, error));
  CHECK_FALSE(parseChecksConfigJson("not json", spec, error));
}

TEST_CASE("findChecksConfig walks up from the start directory", "[CheckSet]") {
  const std::string root = "check_cfg_root";
  const std::string nested = root + "/a/b";
  REQUIRE(!llvm::sys::fs::create_directories(nested));
  {
    std::ofstream out(root + "/.vycor-anneal.json");
    out << R"({"checks": ["all"]})";
  }

  auto found = findChecksConfig(nested);
  REQUIRE(!found.empty());
  CHECK(found.find(".vycor-anneal.json") != std::string::npos);

  // Nothing above an isolated tree without the file.
  const std::string bare = "check_cfg_bare/x";
  REQUIRE(!llvm::sys::fs::create_directories(bare));
  // (The walk continues above the test dir; we can only assert that IF a
  // config is found, it's not ours from check_cfg_root.)
  auto other = findChecksConfig(bare);
  CHECK(other.find("check_cfg_root") == std::string::npos);

  std::remove((root + "/.vycor-anneal.json").c_str());
  llvm::sys::fs::remove(nested);
  llvm::sys::fs::remove(root + "/a");
  llvm::sys::fs::remove(root);
  llvm::sys::fs::remove(bare);
  llvm::sys::fs::remove("check_cfg_bare");
}
