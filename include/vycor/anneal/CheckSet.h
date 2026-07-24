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

#include <set>
#include <string>
#include <vector>

namespace vycor {

// ============================================================================
// Named-check selection (--checks / .vycor-anneal.json)
//
// Every anneal analysis is a named check with a documentation page under
// docs/checks/<name>.md (the clang-tidy model). A check specification is an
// ordered list of entries applied left to right:
//
//   "adl-visibility"      enable one check
//   "-dead-code"          disable one check
//   "all"                 enable every known check (group)
//   "-compute-heavy"      disable every member of a group
//
// Group entries expand to their members before applying. Unknown names are
// a hard error (typo protection, same policy as the org config). When no
// specification is given anywhere, the default set applies: every built-in
// check marked defaultOn plus every registered organization check.
//
// Sources are applied in order, later entries winning:
//   1. .vycor-anneal.json found by walking up from the working directory
//      ({"checks": ["all", "-coverage-properties"]}), or the file named by
//      --checks-config;
//   2. the --checks command-line value;
//   3. the legacy toggle flags (--odr-diag, --coverage-diag, --dead-code),
//      which append their check as an enable.
// ============================================================================

struct CheckInfo {
  std::string name;
  std::string summary; // one line, shown in docs index / future --list-checks
  bool defaultOn = false;
  // Built-in group memberships beyond the implicit "all" (e.g. "noisy",
  // "compute-heavy"). Selection-only labels: they carry no behavior.
  std::vector<std::string> groups;
};

// The built-in check table. Names are stable identifiers: each has a page
// at docs/checks/<name>.md, and renames are breaking changes.
const std::vector<CheckInfo> &builtinAnnealChecks();

// Resolve an ordered specification into the enabled-check set, starting
// from `enabled`'s current contents (pass the default set, or empty when a
// spec starts from scratch with a group). Known names are the built-in
// table plus ExtensionRegistry check names; known groups are "all", the
// built-in table's group labels, and ExtensionRegistry::checkGroups().
// Returns false and fills `error` on an unknown name.
bool resolveCheckSpec(const std::vector<std::string> &spec,
                      std::set<std::string> &enabled, std::string &error);

// The default enabled set (defaultOn built-ins + all organization checks).
std::set<std::string> defaultCheckSet();

// Parse a checks-config JSON buffer: {"checks": ["all", "-noisy", ...]}.
// Unknown keys are an error.
bool parseChecksConfigJson(const std::string &buffer,
                           std::vector<std::string> &outSpec,
                           std::string &error);

// Find a `.vycor-anneal.json` by walking up from `startDir` to the
// filesystem root. Returns the path, or "" when none exists.
std::string findChecksConfig(const std::string &startDir);

} // namespace vycor
