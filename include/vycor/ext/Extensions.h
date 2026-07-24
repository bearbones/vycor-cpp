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
#include <unordered_map>
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

// An organization check over the MERGED cross-TU GlobalIndex (phase 1.5,
// like the built-in coverage/ODR analyses): sees every TU's declarations
// at once, unlike AnnealCheck which runs per TU with AST access. The
// natural home for "must agree across TUs" invariants — e.g. "every IPC
// message struct is defined identically in client and server trees", or
// project-specific specialization/registration completeness rules.
// Diagnostics are recomputed each run from the (replayable) index, so
// IndexChecks never affect checkpoint record content.
class IndexCheck {
public:
  virtual ~IndexCheck() = default;

  // Stable identifier (short-kebab-case) — selectable/disable-able through
  // the same --checks configuration as built-in checks.
  virtual std::string name() const = 0;

  virtual void check(const GlobalIndex &index,
                     std::vector<Diagnostic> &out) = 0;
};

using IndexCheckFactory = std::function<std::unique_ptr<IndexCheck>()>;

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
  void addIndexCheck(IndexCheckFactory factory);
  void addLockTypes(std::vector<std::string> qualifiedNames);
  void addChannelTypes(std::vector<ChannelTypeSpec> specs);
  void addGuardClassifier(GuardClassifier classifier);

  // Register (or extend) a named check group usable in --checks
  // specifications, e.g. addCheckGroup("myorg-strict", {"no-legacy-alloc",
  // "ipc-struct-parity"}). The built-in groups ("all", "noisy",
  // "compute-heavy", ...) live in anneal/CheckSet.cpp; org groups may
  // reference built-in and org check names alike.
  void addCheckGroup(const std::string &group,
                     std::vector<std::string> checkNames);

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
  std::vector<std::unique_ptr<IndexCheck>>
  createIndexChecks(const std::vector<std::string> &disabled = {}) const;

  // Names of every registered organization check (both kinds), for the
  // --checks resolver.
  std::vector<std::string> allCheckNames() const;

  const std::unordered_map<std::string, std::vector<std::string>> &
  checkGroups() const {
    return checkGroups_;
  }

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
  std::vector<IndexCheckFactory> indexCheckFactories_;
  std::vector<std::string> lockTypes_;
  std::vector<ChannelTypeSpec> channelTypes_;
  std::vector<GuardClassifier> classifiers_;
  std::unordered_map<std::string, std::vector<std::string>> checkGroups_;
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

// Registers an IndexCheck subclass (default-constructible) — the merged
// cross-TU counterpart of VYCOR_REGISTER_ANNEAL_CHECK.
#define VYCOR_REGISTER_INDEX_CHECK(CheckClass)                                 \
  namespace {                                                                  \
  const bool vycorIndexCheckRegistrar_##CheckClass = [] {                      \
    ::vycor::ExtensionRegistry::instance().addIndexCheck(                      \
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
