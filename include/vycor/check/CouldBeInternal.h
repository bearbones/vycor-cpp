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

#include "vycor/callgraph/ReferenceIndex.h"
#include "vycor/callgraph/TuOutcome.h"
#include "vycor/check/GlobalIndex.h"

#include <string>
#include <vector>

namespace vycor {

// ============================================================================
// could-be-internal (docs/checks/could-be-internal.md) — functions and
// variables with external linkage that no other translation unit
// references, read from the megascope reference facts (ReferenceIndex).
//
//   Internal_Unreferenced    no declaration in any header and no reference
//                            from another TU (warning)
//   Internal_DeclaredInHeader declared in a header, but no other TU
//                            references it (note)
//   Internal_TestOnly        the only references from other TUs are in
//                            test sources (note)
//   Internal_Unproven        what would be Internal_Unreferenced, from facts
//                            that do not cover the whole program (note)
//
// "No other TU references it" is a whole-program claim. It holds only when
// the facts cover every TU of the program; CouldBeInternalOptions::coverageGap
// says when they do not, and every finding then carries that reason.
// ============================================================================

struct CouldBeInternalOptions {
  // Display names that are entry points by configuration (--entry-point);
  // `main` is excluded by its own attribute.
  std::vector<std::string> entryPoints;
  // POSIX extended regex over a file path that marks test sources; empty
  // disables test detection. kDefaultTestPathPattern is the CLI default.
  std::string testPathPattern;
  // Paths under this directory are matched relative to it, so a checkout
  // that itself sits under a `tests/` directory is not all test code.
  std::string projectRoot;
  // Why the facts may miss references ("" when they cover the program):
  // see describeCoverageGap.
  std::string coverageGap;
};

extern const char *const kDefaultTestPathPattern;

// The coverage gap of a fact set, in words, or "" when there is none.
// `programTus` is the number of TUs the program has (the C/C++ entries of
// the compilation database), `outcomes` the parse outcome of every TU the
// facts were built from.
[[nodiscard]] std::string describeCoverageGap(size_t programTus,
                                              size_t coveredProgramTus,
                                              const TuOutcomes &outcomes);

// The findings, sorted by (location, symbol). Validates
// opts.testPathPattern; an invalid pattern sets `error` and returns none.
[[nodiscard]] std::vector<Diagnostic>
findCouldBeInternal(const ReferenceIndex &refs,
                    const CouldBeInternalOptions &opts, std::string &error);

} // namespace vycor
