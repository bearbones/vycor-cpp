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

#include "vycor/morph/MatcherEngine.h"
#include <string>
#include <vector>

namespace vycor {

// Orchestrates multiple passes of transform rules over a set of source files.
// Each pass runs a MatcherEngine with its own set of rules, collects
// replacements, and (optionally) applies them before the next pass.
class TransformPipeline {
public:
  // Add a pass consisting of one or more transform rules.
  void addPass(std::vector<TransformRule> rules);

  // Execute all passes sequentially against the given source files.
  // If dryRun is true, replacements are collected but not written to disk.
  // Multiple build paths can be provided; their compilation databases are
  // tried in order for each source file (first match wins).
  // Returns 0 on success, nonzero on failure.
  int execute(const std::vector<std::string> &buildPaths,
              const std::vector<std::string> &sourceFiles, bool dryRun = false);

  // Retrieve all accumulated replacements across all passes, keyed by file.
  const std::map<std::string, clang::tooling::Replacements> &
  getReplacements() const;

private:
  std::vector<std::vector<TransformRule>> passes_;
  std::map<std::string, clang::tooling::Replacements> allReplacements_;
};

} // namespace vycor
