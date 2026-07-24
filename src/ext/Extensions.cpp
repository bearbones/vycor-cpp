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

#include "vycor/ext/Extensions.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Regex.h"

#include <algorithm>

namespace vycor {

ExtensionRegistry &ExtensionRegistry::instance() {
  // Function-local static: safe across static-init-time registrations from
  // ext/ translation units regardless of their link order.
  static ExtensionRegistry registry;
  return registry;
}

void ExtensionRegistry::addAnnealCheck(AnnealCheckFactory factory) {
  checkFactories_.push_back(std::move(factory));
}

void ExtensionRegistry::addIndexCheck(IndexCheckFactory factory) {
  indexCheckFactories_.push_back(std::move(factory));
}

void ExtensionRegistry::addCheckGroup(const std::string &group,
                                      std::vector<std::string> checkNames) {
  auto &members = checkGroups_[group];
  for (auto &name : checkNames)
    if (std::find(members.begin(), members.end(), name) == members.end())
      members.push_back(std::move(name));
}

void ExtensionRegistry::addLockTypes(std::vector<std::string> qualifiedNames) {
  for (auto &name : qualifiedNames)
    if (std::find(lockTypes_.begin(), lockTypes_.end(), name) ==
        lockTypes_.end())
      lockTypes_.push_back(std::move(name));
}

void ExtensionRegistry::addChannelTypes(std::vector<ChannelTypeSpec> specs) {
  for (auto &spec : specs)
    if (std::find(channelTypes_.begin(), channelTypes_.end(), spec) ==
        channelTypes_.end())
      channelTypes_.push_back(std::move(spec));
}

void ExtensionRegistry::addGuardClassifier(GuardClassifier classifier) {
  classifiers_.push_back(std::move(classifier));
}

bool ExtensionRegistry::addFeatureFlagPattern(const std::string &pattern,
                                              unsigned nameGroup) {
  auto regex = std::make_shared<llvm::Regex>(pattern);
  std::string error;
  if (!regex->isValid(error))
    return false;
  classifiers_.push_back(
      [regex, nameGroup](
          const ConditionalGuard &g) -> std::optional<GuardAnnotation> {
        llvm::SmallVector<llvm::StringRef, 4> matches;
        if (!regex->match(g.conditionText, &matches))
          return std::nullopt;
        GuardAnnotation ann;
        ann.kind = "feature-flag";
        ann.name = nameGroup < matches.size() ? matches[nameGroup].str()
                                              : matches[0].str();
        return ann;
      });
  return true;
}

std::vector<std::unique_ptr<AnnealCheck>> ExtensionRegistry::createAnnealChecks(
    const std::vector<std::string> &disabled) const {
  std::vector<std::unique_ptr<AnnealCheck>> checks;
  for (const auto &factory : checkFactories_) {
    auto check = factory();
    if (!check)
      continue;
    if (std::find(disabled.begin(), disabled.end(), check->name()) !=
        disabled.end())
      continue;
    checks.push_back(std::move(check));
  }
  return checks;
}

std::vector<std::unique_ptr<IndexCheck>> ExtensionRegistry::createIndexChecks(
    const std::vector<std::string> &disabled) const {
  std::vector<std::unique_ptr<IndexCheck>> checks;
  for (const auto &factory : indexCheckFactories_) {
    auto check = factory();
    if (!check)
      continue;
    if (std::find(disabled.begin(), disabled.end(), check->name()) !=
        disabled.end())
      continue;
    checks.push_back(std::move(check));
  }
  return checks;
}

std::vector<std::string> ExtensionRegistry::allCheckNames() const {
  std::vector<std::string> names;
  for (const auto &factory : checkFactories_)
    if (auto check = factory())
      names.push_back(check->name());
  for (const auto &factory : indexCheckFactories_)
    if (auto check = factory())
      names.push_back(check->name());
  return names;
}

std::optional<GuardAnnotation>
ExtensionRegistry::classify(const ConditionalGuard &g) const {
  for (const auto &classifier : classifiers_)
    if (auto ann = classifier(g))
      return ann;
  return std::nullopt;
}

void ExtensionRegistry::clear() {
  checkFactories_.clear();
  indexCheckFactories_.clear();
  lockTypes_.clear();
  channelTypes_.clear();
  classifiers_.clear();
  checkGroups_.clear();
}

std::optional<GuardAnnotation> classifyGuard(const ConditionalGuard &g) {
  return ExtensionRegistry::instance().classify(g);
}

} // namespace vycor
