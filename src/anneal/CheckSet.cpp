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

#include "vycor/anneal/CheckSet.h"

#include "vycor/ext/Extensions.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Path.h"

#include <map>

namespace vycor {

const std::vector<CheckInfo> &builtinAnnealChecks() {
  // Group labels are advisory selection handles ("all,-noisy"); today's
  // members are the initial seeding, not a promise — checks may move as
  // experience accumulates.
  static const std::vector<CheckInfo> checks = {
      {"adl-visibility",
       "Fragile ADL resolutions: an invisible overload would win or tie",
       /*defaultOn=*/true,
       {}},
      {"ctad-visibility",
       "CTAD deducing differently because a deduction guide is not included",
       /*defaultOn=*/true,
       {}},
      {"specialization-visibility",
       "TU instantiates a primary template whose explicit specialization "
       "exists elsewhere (IFNDR)",
       /*defaultOn=*/true,
       {}},
      {"default-arg-divergence",
       "Declaration sites that disagree on a parameter's default argument",
       /*defaultOn=*/true,
       {}},
      {"static-init-order",
       "Dynamic initializers reading another TU's dynamically-initialized "
       "global (SIOF)",
       /*defaultOn=*/true,
       {}},
      {"static-init-hazards",
       "Static initializers reaching dlopen/dlsym or thread create/join "
       "(loader-lock deadlock risk)",
       /*defaultOn=*/false,
       {"compute-heavy"}},
      {"odr-violations",
       "Vague-linkage definitions that differ across sites or TUs",
       /*defaultOn=*/false,
       {"compute-heavy"}},
      {"coverage-properties",
       "GVA linkage / COMDAT properties that make coverage records vanish",
       /*defaultOn=*/false,
       {"noisy"}},
      {"dead-code",
       "Functions unreachable from the entry points via the call graph",
       /*defaultOn=*/false,
       {"compute-heavy"}},
  };
  return checks;
}

std::set<std::string> defaultCheckSet() {
  std::set<std::string> enabled;
  for (const auto &check : builtinAnnealChecks())
    if (check.defaultOn)
      enabled.insert(check.name);
  // Organization checks default on, preserving the pre-registry behaviour
  // (ext/ checks always ran unless disabled via org config).
  for (const auto &name : ExtensionRegistry::instance().allCheckNames())
    enabled.insert(name);
  return enabled;
}

bool resolveCheckSpec(const std::vector<std::string> &spec,
                      std::set<std::string> &enabled, std::string &error) {
  // Known names and group expansions, computed once per call (org
  // registrations are start-up-time only).
  std::set<std::string> known;
  std::map<std::string, std::vector<std::string>> groups;
  for (const auto &check : builtinAnnealChecks()) {
    known.insert(check.name);
    groups["all"].push_back(check.name);
    for (const auto &group : check.groups)
      groups[group].push_back(check.name);
  }
  for (const auto &name : ExtensionRegistry::instance().allCheckNames()) {
    known.insert(name);
    groups["all"].push_back(name);
  }
  for (const auto &[group, members] :
       ExtensionRegistry::instance().checkGroups()) {
    auto &into = groups[group];
    into.insert(into.end(), members.begin(), members.end());
  }

  for (const auto &raw : spec) {
    bool disable = !raw.empty() && raw[0] == '-';
    std::string name = disable ? raw.substr(1) : raw;
    if (name.empty()) {
      error = "empty entry in checks specification";
      return false;
    }
    auto groupIt = groups.find(name);
    if (groupIt != groups.end()) {
      for (const auto &member : groupIt->second) {
        if (disable)
          enabled.erase(member);
        else
          enabled.insert(member);
      }
      continue;
    }
    if (!known.count(name)) {
      error = "unknown check or group '" + name +
              "' (see docs/checks/README.md for the list)";
      return false;
    }
    if (disable)
      enabled.erase(name);
    else
      enabled.insert(name);
  }
  return true;
}

bool parseChecksConfigJson(const std::string &buffer,
                           std::vector<std::string> &outSpec,
                           std::string &error) {
  auto jsonOrErr = llvm::json::parse(buffer);
  if (!jsonOrErr) {
    error = "parse error: " + llvm::toString(jsonOrErr.takeError());
    return false;
  }
  const llvm::json::Object *root = jsonOrErr->getAsObject();
  if (!root) {
    error = "top level must be a JSON object";
    return false;
  }
  for (const auto &kv : *root) {
    if (kv.first.str() != "checks") {
      error = "unknown key '" + kv.first.str() + "' (expected: checks)";
      return false;
    }
  }
  const llvm::json::Value *checks = root->get("checks");
  if (!checks)
    return true; // an empty config is a valid no-op
  const llvm::json::Array *arr = checks->getAsArray();
  if (!arr) {
    error = "'checks' must be an array of strings";
    return false;
  }
  for (const auto &entry : *arr) {
    auto s = entry.getAsString();
    if (!s) {
      error = "'checks' must be an array of strings";
      return false;
    }
    outSpec.push_back(s->str());
  }
  return true;
}

std::string findChecksConfig(const std::string &startDir) {
  llvm::SmallString<256> abs(startDir);
  llvm::sys::fs::make_absolute(abs);
  std::string dir(abs.str());
  for (;;) {
    llvm::SmallString<256> candidate(dir);
    llvm::sys::path::append(candidate, ".vycor-anneal.json");
    if (llvm::sys::fs::exists(candidate))
      return std::string(candidate.str());
    // Copy before assigning: parent_path returns a view into `dir`.
    std::string parent = llvm::sys::path::parent_path(dir).str();
    if (parent.empty() || parent == dir)
      return "";
    dir = std::move(parent);
  }
}

} // namespace vycor
