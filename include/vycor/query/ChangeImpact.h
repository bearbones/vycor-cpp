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

#include "vycor/impact/ImpactSearch.h"
#include "vycor/impact/PatchMapping.h"
#include "vycor/impact/SemanticDiff.h"

#include "llvm/Support/JSON.h"

#include <cstddef>

// JSON shapes of the change-impact results (docs/change-impact.md),
// shared by the impact_of_change tool (ImpactTools.cpp) and the CLI
// `diff` verb, which composes a diff, a route diff, and an impact.

namespace vycor {

/// {changes, summary, identity, moves, comparability, contextsCompared}.
llvm::json::Value serializeSemanticDiff(const SemanticDiffResult &diff);

/// The `comparability` member alone (attached to a refused diff's error).
llvm::json::Value serializeComparability(const Comparability &c);

/// {target, before, after, added, removed, unchanged, complete,
/// exhaustive}.
llvm::json::Value serializeRouteDiff(const RouteDiff &routes,
                                     const std::string &target);

/// {changed, unknown, affected, affectedCount, truncated,
/// entryPointsAffected, complete, exhaustive, stopReasons, skippedHubs,
/// expansions}. `affected` is cut at maxResults (0 = all); paths are
/// emitted only with includePaths.
llvm::json::Value serializeImpact(const ImpactResult &impact,
                                  const CallGraph &graph,
                                  const std::vector<std::string> &entryPoints,
                                  size_t maxResults, bool includePaths);

/// {mapping: {call_site, definition, extent, file, unmapped}, unmapped:
/// [{file, firstLine, lastLine, deletionOnly, reason}]} merged into
/// `obj`; the mapped functions' `via` is reported per changed entry by
/// the caller.
void attachPatchMapping(llvm::json::Object &obj, const PatchMapping &mapping);

} // namespace vycor
