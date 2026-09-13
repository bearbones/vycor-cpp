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
#include "vycor/callgraph/PathSearch.h"

#include <cstddef>
#include <string>
#include <vector>

// Bounded impact traversal (docs/change-impact.md, "Impact"): from a set
// of changed functions, the callers that reach them within a depth and
// work budget, each with the shallowest witness path. A candidate set,
// not proof.

namespace vycor {

struct ImpactLimits {
  // Maximum distance in EDGES from a changed function. 0 means no limit.
  unsigned maxDepth = 10;
  // Do not expand the callers of a function whose stored in-degree
  // exceeds this; record it in skippedHubs instead. 0 disables.
  size_t maxFanIn = 1000;
  // Maximum number of node expansions. 0 means no limit.
  size_t maxWork = 200000;
};

struct AffectedFunction {
  std::string usr;
  std::string name;
  unsigned depth = 0; // edges from the changed function it reaches
  std::string changedUsr; // the changed function its path ends at
  // affected -> ... -> changed, in call direction.
  std::vector<PathHop> path;
};

struct ImpactResult {
  // The changed identities the graph knows, sorted by usr.
  std::vector<std::string> changed;
  // Changed names/usrs the graph does not reference; nothing was
  // searched for them.
  std::vector<std::string> unknown;
  // Ordered by (depth, usr). Excludes the changed functions themselves.
  std::vector<AffectedFunction> affected;
  // Sorted by usr.
  std::vector<SkippedHub> skippedHubs;
  unsigned stops = 0; // StopReason bitmask (never PathLimit)
  size_t expansions = 0;

  bool stopped(StopReason r) const {
    return (stops & static_cast<unsigned>(r)) != 0;
  }
  // Every caller within maxDepth was reached: no work-budget cut and no
  // hub pruned. DepthLimit does not clear it.
  bool complete() const {
    return !stopped(StopReason::WorkBudget) && !stopped(StopReason::HubPruned);
  }
  // complete and no depth cut: the affected set is every transitive
  // caller admitted by the filter.
  bool exhaustive() const { return complete() && stops == 0; }
};

// Reverse breadth-first walk over caller edges (stored and query-time
// expansions, the callersOf edge set) from every changed function at
// once. Callers are taken in canonical edge order and every function is
// recorded at its first reach, so the affected set, its depths, and its
// witness paths do not depend on TU or insertion order. `changed` entries
// may be usrs or display names (a shared display name resolves to every
// node carrying it).
ImpactResult findImpact(const CallGraph &graph,
                        const std::vector<std::string> &changed,
                        const ImpactLimits &limits,
                        EdgePredicate filter = nullptr);

} // namespace vycor
