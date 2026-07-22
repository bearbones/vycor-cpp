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

#pragma once

#include "vycor/anneal/GlobalIndex.h"
#include "vycor/callgraph/ChannelIndex.h"
#include "vycor/callgraph/ConditionalGuard.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace clang {
class ASTContext;
} // namespace clang

namespace vycor {

// ============================================================================
// Organization extension points (see docs/EXTENDING.md)
//
// Forks/mirrors customize vycor-cpp through two channels that never require
// editing upstream files:
//   - declaratively, via an org config JSON file (OrgConfig.h, --org-config)
//   - in code, by dropping .cpp files into the top-level ext/ directory,
//     which the build compiles directly into the executable. Those files
//     register hooks with ExtensionRegistry at static-initialization time
//     (VYCOR_REGISTER_ANNEAL_CHECK, or an ExtensionRegistrar block).
// ============================================================================

// An organization-specific anneal check. One instance is created per TU
// (via the registered factory), so per-TU state in members needs no reset.
// Runs after the built-in AnalyzerVisitor traversal of the same TU; push
// findings into `out` with kind = Diagnostic::Custom and checkName = name().
class AnnealCheck {
public:
  virtual ~AnnealCheck() = default;

  // Stable identifier, used by OrgConfig::disabledAnnealChecks and surfaced
  // in Diagnostic::checkName. Convention: short-kebab-case.
  virtual std::string name() const = 0;

  virtual void checkTU(clang::ASTContext &context, const GlobalIndex &index,
                       std::vector<Diagnostic> &out) = 0;
};

using AnnealCheckFactory = std::function<std::unique_ptr<AnnealCheck>()>;

// Result of classifying a ConditionalGuard: e.g. {"feature-flag", "NewNav"}
// for a guard whose condition matched a registered feature-flag pattern.
// Surfaced as an "annotation" object on guard records in prism dump JSON
// and MCP tool responses; combined with ConditionalGuard::inTrueBranch it
// answers "this call path runs only with flag X on/off".
struct GuardAnnotation {
  std::string kind; // e.g. "feature-flag"
  std::string name; // e.g. the flag name extracted from the condition
};

// Classifies a guard, or nullopt when it doesn't recognize it. Classifiers
// run in registration order; first non-nullopt wins.
using GuardClassifier =
    std::function<std::optional<GuardAnnotation>(const ConditionalGuard &)>;

// Process-wide registry of organization hooks. Registration happens at
// static-initialization time (ext/ registrars) or during CLI startup
// (--org-config), strictly before index building / analysis begins; reads
// after that point are unsynchronized by design.
class ExtensionRegistry {
public:
  static ExtensionRegistry &instance();

  // ---- registration --------------------------------------------------------
  void addAnnealCheck(AnnealCheckFactory factory);
  void addLockTypes(std::vector<std::string> qualifiedNames);
  void addChannelTypes(std::vector<ChannelTypeSpec> specs);
  void addGuardClassifier(GuardClassifier classifier);

  // Convenience wrapper over addGuardClassifier: an ERE applied (searching,
  // not anchored) to ConditionalGuard::conditionText. On match the guard is
  // annotated {kind: "feature-flag", name: capture group `nameGroup`}
  // (0 = the whole match). Returns false without registering when the
  // pattern is not a valid llvm::Regex.
  bool addFeatureFlagPattern(const std::string &pattern,
                             unsigned nameGroup = 0);

  // ---- consumption ---------------------------------------------------------
  // Fresh check instances, skipping any whose name() is in `disabled`.
  std::vector<std::unique_ptr<AnnealCheck>>
  createAnnealChecks(const std::vector<std::string> &disabled = {}) const;

  const std::vector<std::string> &lockTypes() const { return lockTypes_; }
  const std::vector<ChannelTypeSpec> &channelTypes() const {
    return channelTypes_;
  }

  // First classifier that recognizes the guard, or nullopt.
  std::optional<GuardAnnotation> classify(const ConditionalGuard &g) const;

  // Drop all registrations. Test support only — upstream tests reset the
  // process-wide registry between sections.
  void clear();

private:
  ExtensionRegistry() = default;

  std::vector<AnnealCheckFactory> checkFactories_;
  std::vector<std::string> lockTypes_;
  std::vector<ChannelTypeSpec> channelTypes_;
  std::vector<GuardClassifier> classifiers_;
};

// Shorthand for ExtensionRegistry::instance().classify(g), used at the
// guard-serialization points (prism dump, MCP tools).
std::optional<GuardAnnotation> classifyGuard(const ConditionalGuard &g);

// Registers an AnnealCheck subclass (default-constructible) at static-init
// time. Use at namespace scope in an ext/*.cpp file:
//   class NoLegacyApiCheck : public vycor::AnnealCheck { ... };
//   VYCOR_REGISTER_ANNEAL_CHECK(NoLegacyApiCheck)
#define VYCOR_REGISTER_ANNEAL_CHECK(CheckClass)                                \
  namespace {                                                                  \
  const bool vycorAnnealCheckRegistrar_##CheckClass = [] {                     \
    ::vycor::ExtensionRegistry::instance().addAnnealCheck(                     \
        [] { return std::make_unique<CheckClass>(); });                        \
    return true;                                                               \
  }();                                                                         \
  }

// For arbitrary registration code (lock types, guard classifiers, channel
// types) at static-init time:
//   VYCOR_EXTENSION_SETUP(MyOrgHooks) {
//     registry.addLockTypes({"myorg::SpinLock"});
//     registry.addFeatureFlagPattern("FFlag::([A-Za-z0-9_]+)", 1);
//   }
#define VYCOR_EXTENSION_SETUP(Tag)                                             \
  namespace {                                                                  \
  struct VycorExtensionSetup_##Tag {                                           \
    static void run(::vycor::ExtensionRegistry &registry);                     \
    VycorExtensionSetup_##Tag() {                                              \
      run(::vycor::ExtensionRegistry::instance());                             \
    }                                                                          \
  };                                                                           \
  const VycorExtensionSetup_##Tag vycorExtensionSetupInstance_##Tag;           \
  }                                                                            \
  void VycorExtensionSetup_##Tag::run(::vycor::ExtensionRegistry &registry)

} // namespace vycor
