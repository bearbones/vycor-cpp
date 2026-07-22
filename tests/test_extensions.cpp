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

#include "vycor/anneal/Analyzer.h"
#include "vycor/ext/Extensions.h"
#include "vycor/ext/OrgConfig.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/Tooling/Tooling.h"

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <string>
#include <vector>

namespace {

// The registry is process-wide; these tests need a clean slate and must not
// leak their registrations into other test files' sections. NOTE: this also
// clears compiled ext/ registrars for the remainder of the test binary run —
// organization tests under ext/tests/ should register what they need
// explicitly rather than relying on static-init state surviving this file.
struct RegistryReset {
  RegistryReset() { vycor::ExtensionRegistry::instance().clear(); }
  ~RegistryReset() { vycor::ExtensionRegistry::instance().clear(); }
};

// Minimal org check: flags every function literally named "forbidden" among
// the TU's top-level declarations.
class ForbiddenFunctionCheck : public vycor::AnnealCheck {
public:
  std::string name() const override { return "forbidden-function"; }

  void checkTU(clang::ASTContext &context, const vycor::GlobalIndex &,
               std::vector<vycor::Diagnostic> &out) override {
    for (auto *decl : context.getTranslationUnitDecl()->decls()) {
      auto *fn = llvm::dyn_cast<clang::FunctionDecl>(decl);
      if (!fn || fn->getNameAsString() != "forbidden")
        continue;
      vycor::Diagnostic diag;
      diag.kind = vycor::Diagnostic::Custom;
      diag.checkName = name();
      diag.message = "function 'forbidden' is not allowed";
      out.push_back(std::move(diag));
    }
  }
};

std::vector<vycor::Diagnostic> analyzeCode(const std::string &code,
                                           vycor::AnalysisOptions opts = {}) {
  vycor::GlobalIndex index;
  std::vector<vycor::Diagnostic> diagnostics;
  REQUIRE(clang::tooling::runToolOnCodeWithArgs(
      std::make_unique<vycor::AnalyzerAction>(index, diagnostics,
                                              std::move(opts)),
      code, {"-std=c++17"}, "test_input.cpp"));
  return diagnostics;
}

} // namespace

TEST_CASE("Registered AnnealCheck runs per TU and emits Custom diagnostics",
          "[Extensions]") {
  RegistryReset reset;
  vycor::ExtensionRegistry::instance().addAnnealCheck(
      [] { return std::make_unique<ForbiddenFunctionCheck>(); });

  auto diagnostics = analyzeCode("void forbidden();\nvoid fine();\n");

  REQUIRE(diagnostics.size() == 1);
  CHECK(diagnostics[0].kind == vycor::Diagnostic::Custom);
  CHECK(diagnostics[0].checkName == "forbidden-function");
  CHECK(diagnostics[0].message == "function 'forbidden' is not allowed");
}

TEST_CASE("disabledChecks suppresses a registered check by name",
          "[Extensions]") {
  RegistryReset reset;
  vycor::ExtensionRegistry::instance().addAnnealCheck(
      [] { return std::make_unique<ForbiddenFunctionCheck>(); });

  vycor::AnalysisOptions opts;
  opts.disabledChecks = {"forbidden-function"};
  auto diagnostics = analyzeCode("void forbidden();\n", std::move(opts));

  CHECK(diagnostics.empty());
}

TEST_CASE("VYCOR_EXTENSION_SETUP-style registration merges lock and channel "
          "types with dedup",
          "[Extensions]") {
  RegistryReset reset;
  auto &registry = vycor::ExtensionRegistry::instance();

  registry.addLockTypes({"myorg::SpinLock", "myorg::SpinLock",
                         "myorg::RwLock"});
  registry.addLockTypes({"myorg::RwLock"});
  REQUIRE(registry.lockTypes() ==
          std::vector<std::string>{"myorg::SpinLock", "myorg::RwLock"});

  vycor::ChannelTypeSpec spec;
  spec.qualifiedTypeName = "myorg::EventBus";
  spec.produceMethods = {"post"};
  spec.consumeMethods = {"drain"};
  spec.category = "bus";
  registry.addChannelTypes({spec});
  registry.addChannelTypes({spec}); // identical — deduped
  REQUIRE(registry.channelTypes().size() == 1);
  CHECK(registry.channelTypes()[0].qualifiedTypeName == "myorg::EventBus");
}

TEST_CASE("Feature-flag pattern classifies guard condition text",
          "[Extensions]") {
  RegistryReset reset;
  auto &registry = vycor::ExtensionRegistry::instance();
  REQUIRE(registry.addFeatureFlagPattern("FFlag::([A-Za-z0-9_]+)", 1));

  vycor::ConditionalGuard guard;
  guard.conditionText = "FFlag::NewNav && user.isAdmin()";
  guard.inTrueBranch = true;

  auto ann = vycor::classifyGuard(guard);
  REQUIRE(ann.has_value());
  CHECK(ann->kind == "feature-flag");
  CHECK(ann->name == "NewNav");

  vycor::ConditionalGuard other;
  other.conditionText = "count > 0";
  CHECK_FALSE(vycor::classifyGuard(other).has_value());
}

TEST_CASE("Feature-flag pattern with nameGroup 0 uses the whole match; "
          "invalid regex is rejected",
          "[Extensions]") {
  RegistryReset reset;
  auto &registry = vycor::ExtensionRegistry::instance();

  CHECK_FALSE(registry.addFeatureFlagPattern("FFlag::(unclosed"));

  REQUIRE(registry.addFeatureFlagPattern("isFeatureEnabled"));
  vycor::ConditionalGuard guard;
  guard.conditionText = "isFeatureEnabled(\"streaming\")";
  auto ann = vycor::classifyGuard(guard);
  REQUIRE(ann.has_value());
  CHECK(ann->name == "isFeatureEnabled");
}

TEST_CASE("Custom GuardClassifier wins in registration order",
          "[Extensions]") {
  RegistryReset reset;
  auto &registry = vycor::ExtensionRegistry::instance();

  registry.addGuardClassifier([](const vycor::ConditionalGuard &g)
                                  -> std::optional<vycor::GuardAnnotation> {
    if (g.isAssertion)
      return vycor::GuardAnnotation{"invariant", "assert"};
    return std::nullopt;
  });
  REQUIRE(registry.addFeatureFlagPattern("assert_flag_[a-z]+"));

  vycor::ConditionalGuard guard;
  guard.conditionText = "assert_flag_streaming";
  guard.isAssertion = true;
  // The classifier registered first sees it first.
  auto ann = vycor::classifyGuard(guard);
  REQUIRE(ann.has_value());
  CHECK(ann->kind == "invariant");
}

TEST_CASE("OrgConfig parses all sections", "[Extensions][OrgConfig]") {
  const char *json = R"json({
    "lockTypes": ["myorg::SpinLock"],
    "channelTypes": [{"type": "myorg::EventBus", "produce": ["post"],
                      "consume": ["drain"], "category": "bus"}],
    "featureFlags": [{"pattern": "FFlag::([A-Za-z0-9_]+)", "nameGroup": 1}],
    "collapsePaths": ["ThirdParty/Math"],
    "disabledAnnealChecks": ["forbidden-function"]
  })json";

  vycor::OrgConfig cfg;
  std::string error;
  REQUIRE(vycor::parseOrgConfigJson(json, cfg, error));
  CHECK(cfg.lockTypes == std::vector<std::string>{"myorg::SpinLock"});
  REQUIRE(cfg.channelTypes.size() == 1);
  CHECK(cfg.channelTypes[0].qualifiedTypeName == "myorg::EventBus");
  CHECK(cfg.channelTypes[0].produceMethods ==
        std::vector<std::string>{"post"});
  CHECK(cfg.channelTypes[0].consumeMethods ==
        std::vector<std::string>{"drain"});
  CHECK(cfg.channelTypes[0].category == "bus");
  REQUIRE(cfg.featureFlags.size() == 1);
  CHECK(cfg.featureFlags[0].pattern == "FFlag::([A-Za-z0-9_]+)");
  CHECK(cfg.featureFlags[0].nameGroup == 1);
  CHECK(cfg.collapsePaths == std::vector<std::string>{"ThirdParty/Math"});
  CHECK(cfg.disabledAnnealChecks ==
        std::vector<std::string>{"forbidden-function"});
}

TEST_CASE("OrgConfig rejects malformed input", "[Extensions][OrgConfig]") {
  vycor::OrgConfig cfg;
  std::string error;

  SECTION("invalid JSON") {
    CHECK_FALSE(vycor::parseOrgConfigJson("{not json", cfg, error));
    CHECK_FALSE(error.empty());
  }
  SECTION("top level not an object") {
    CHECK_FALSE(vycor::parseOrgConfigJson("[1, 2]", cfg, error));
  }
  SECTION("unknown key (typo protection)") {
    CHECK_FALSE(vycor::parseOrgConfigJson(R"({"locktypes": []})", cfg, error));
    CHECK(error.find("locktypes") != std::string::npos);
  }
  SECTION("wrong value shape") {
    CHECK_FALSE(
        vycor::parseOrgConfigJson(R"({"lockTypes": "notAnArray"})", cfg,
                                  error));
  }
  SECTION("channelTypes entry missing type") {
    CHECK_FALSE(vycor::parseOrgConfigJson(
        R"({"channelTypes": [{"produce": ["x"]}]})", cfg, error));
  }
  SECTION("featureFlags entry missing pattern") {
    CHECK_FALSE(vycor::parseOrgConfigJson(
        R"({"featureFlags": [{"nameGroup": 1}]})", cfg, error));
  }
}

TEST_CASE("applyOrgConfig installs hooks into the registry",
          "[Extensions][OrgConfig]") {
  RegistryReset reset;

  vycor::OrgConfig cfg;
  std::string error;
  REQUIRE(vycor::parseOrgConfigJson(
      R"json({
        "lockTypes": ["myorg::SpinLock"],
        "featureFlags": [{"pattern": "FFlag::([A-Za-z0-9_]+)", "nameGroup": 1}]
      })json",
      cfg, error));
  REQUIRE(vycor::applyOrgConfig(cfg, error));

  auto &registry = vycor::ExtensionRegistry::instance();
  CHECK(registry.lockTypes() == std::vector<std::string>{"myorg::SpinLock"});

  vycor::ConditionalGuard guard;
  guard.conditionText = "FFlag::Rollout";
  auto ann = vycor::classifyGuard(guard);
  REQUIRE(ann.has_value());
  CHECK(ann->name == "Rollout");
}

TEST_CASE("applyOrgConfig reports invalid feature-flag regex",
          "[Extensions][OrgConfig]") {
  RegistryReset reset;

  vycor::OrgConfig cfg;
  std::string error;
  REQUIRE(vycor::parseOrgConfigJson(
      R"({"featureFlags": [{"pattern": "(unclosed"}]})", cfg, error));
  CHECK_FALSE(vycor::applyOrgConfig(cfg, error));
  CHECK(error.find("regex") != std::string::npos);
}
