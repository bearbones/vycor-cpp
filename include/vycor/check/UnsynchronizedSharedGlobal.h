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

#include "vycor/callgraph/CallGraph.h"
#include "vycor/callgraph/ReferenceIndex.h"
#include "vycor/check/GlobalIndex.h"

#include <string>
#include <vector>

namespace vycor {

// ============================================================================
// unsynchronized-shared-global (docs/checks/unsynchronized-shared-global.md)
// — a variable with static storage duration that is written on at least
// one path, reached from two execution contexts that can run at the same
// time, with no lock common to the two accesses.
//
//   SharedGlobal_Race           two threads (std::thread, std::jthread,
//                               std::async), every edge on both entry paths
//                               proven, lock sets with no lock in common
//                               (an unknown lock may be any), facts that
//                               cover the program (warning)
//   SharedGlobal_Unproven       what would be SharedGlobal_Race, from facts
//                               that do not cover the program or a lock-set
//                               search that hit its bound (note)
//   SharedGlobal_Plausible      an entry path takes a plausible edge
//                               (virtual dispatch, a function pointer), or
//                               one side runs in a std::packaged_task whose
//                               runner is not known (note)
//   SharedGlobal_InitThenPublish one side runs on the main thread: the check
//                               cannot tell an access made before the
//                               thread starts from one made while it runs
//                               (note)
//
// Facts: the call graph (contexts are the ThreadEntry edges and the entry
// points) and the reference facts with their lock sets (ReferenceIndex
// lock text; ReferenceVisitor records it).
// ============================================================================

struct SharedGlobalOptions {
  // Display names or USRs of the main thread's entry points besides the
  // functions the reference facts mark `main` (--entry-point).
  std::vector<std::string> entryPoints;
  // Why the facts may miss accesses or locks ("" when they cover the
  // program): describeCoverageGap (check/CouldBeInternal.h).
  std::string coverageGap;
  // Distinct (lock set, plausibility) states kept per function and
  // context; a function reached under more is not searched exhaustively.
  unsigned maxStatesPerFunction = 64;
  // States kept per context in all.
  size_t maxStatesPerContext = 1u << 21;
  // States kept over every context (bounds memory on programs with many
  // thread entries; the contexts searched after it is reached are
  // incomplete).
  size_t maxStatesTotal = 1u << 23;
};

// What the check saw, for --verbose: a run with no thread context reports
// nothing because it saw no thread, not because the program is clean.
struct SharedGlobalStats {
  size_t threadContexts = 0; // thread entries, grouped by started function
  size_t selfConcurrent = 0; // of those, ones that can overlap themselves
  bool mainContext = false;  // `main` or an --entry-point was found
  size_t mainDefinitions = 0; // files defining `main`: >1 is several programs
  size_t candidates = 0;     // variables that could be shared data
  size_t statesSearched = 0; // (function, lock set) states, all contexts
  size_t boundedContexts = 0; // contexts whose search hit a bound
  std::vector<std::string> boundedNames; // those contexts, described
};

// The findings, one per variable (its strongest pair of accesses), sorted
// by the variable's location. `stats`, when given, is filled.
[[nodiscard]] std::vector<Diagnostic>
findUnsynchronizedSharedGlobals(const CallGraph &graph,
                                const ReferenceIndex &refs,
                                const SharedGlobalOptions &opts,
                                SharedGlobalStats *stats = nullptr);

} // namespace vycor
