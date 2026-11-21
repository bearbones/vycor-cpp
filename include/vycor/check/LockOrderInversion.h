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

#include <cstddef>
#include <string>
#include <vector>

namespace vycor {

// ============================================================================
// lock-order-inversion (docs/checks/lock-order-inversion.md) — locks
// acquired in opposite orders on different code paths, across TUs: two
// threads that take those paths at once can deadlock.
//
// The "held while acquiring" graph has an edge A -> B whenever lock B is
// acquired, directly or through a chain of calls, while lock A is held.
// Every elementary cycle of it is a finding with one witness per edge.
// A lock held while the same lock is acquired again is a cycle of one
// (a self-deadlock), reported only for a mutex that is not a data member.
//
//   LockOrder_Inversion  every edge proven, every acquisition exclusive,
//                        facts that cover the program (warning)
//   LockOrder_Unproven   facts that do not cover the program, a bound
//                        hit, or a self-deadlock on a mutex not known to
//                        be non-recursive (note)
//   LockOrder_Plausible  an edge's witness takes a plausible call edge
//                        (virtual dispatch, a function pointer) (note)
//   LockOrder_Shared     an edge's witness acquires its lock shared: it
//                        blocks only against an exclusive holder (note)
//
// Facts: the call graph and the reference facts' lock acquisitions
// (RefRole::Lock / LockShared, with the locks held before each) and lock
// texts, read through the join unsynchronized-shared-global shares
// (check/LockJoin.h), so the two checks agree on what the same lock is.
// ============================================================================

struct LockOrderOptions {
  // Why the facts may miss acquisitions or locks ("" when they cover the
  // program): describeCoverageGap (check/CouldBeInternal.h).
  std::string coverageGap;
  // Cycles longer than this many locks are not enumerated.
  unsigned maxCycleLength = 8;
  // Cycles reported per strongly connected group of locks, and in all.
  unsigned maxCyclesPerGroup = 64;
  unsigned maxCycles = 4096;
  // Depth-first steps of the cycle enumeration per group of locks.
  size_t maxEnumerationSteps = size_t(1) << 22;
  // 64-bit words of distinct acquisition closures kept (each function's
  // set of locks reachable through its calls, shared between functions
  // with the same set); past it, closures are not computed and every
  // finding is unproven.
  size_t maxClosureWords = size_t(1) << 23;
  // Functions visited per witness path search.
  size_t maxWitnessVisits = size_t(1) << 20;
};

// What the check saw, for --verbose.
struct LockOrderStats {
  size_t acquisitions = 0;        // acquisition records read
  size_t acquiringFunctions = 0;  // functions with one
  size_t locks = 0;               // named locks acquired (graph nodes)
  size_t unknownAcquisitions = 0; // acquisitions of a lock the bake could
                                  // not name (no node)
  size_t holdingCallers = 0;      // functions calling under a named lock
  size_t closureFunctions = 0;    // functions whose closure was computed
  size_t distinctClosures = 0;    // distinct non-empty closures (all edges)
  bool closureBounded = false;    // maxClosureWords was reached
  size_t orderEdges = 0;          // A -> B edges, A != B
  size_t lockGroups = 0;          // strongly connected groups of 2+ locks
  size_t cycles = 0;              // cycles found (gated ones included)
  size_t gatedCycles = 0;         // of those, every edge under a common
                                  // third lock: not reported
  size_t boundedGroups = 0;       // groups whose enumeration hit a bound
  size_t memberSelfEdges = 0;     // a member mutex acquired while one is
                                  // held: two objects, not reported
  size_t selfDeadlocks = 0;       // self-cycles reported
};

// The findings, one per cycle (and per self-deadlock), sorted by
// location. `stats`, when given, is filled.
[[nodiscard]] std::vector<Diagnostic>
findLockOrderInversions(const CallGraph &graph, const ReferenceIndex &refs,
                        const LockOrderOptions &opts,
                        LockOrderStats *stats = nullptr);

} // namespace vycor
