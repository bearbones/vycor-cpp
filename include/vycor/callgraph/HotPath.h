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
#include "vycor/callgraph/CoverageProfile.h"
#include "vycor/callgraph/Effects.h"

#include "llvm/ADT/ArrayRef.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// Effects reachable from hot roots (docs/checks/hot-path-effects.md): the
// analysis behind the `query_path_effects` tool and the `hot-path-effects`
// check.
//
// A hot root is a function the caller names (a qualified name, a USR, or a
// regex over qualified names) or one a profile says ran often (entry count
// at or above a threshold). An effect is a call of a function the effect
// table lists: an allocation, a lock acquisition, I/O, or a throw. One
// finding per effect call reachable from a root, with one witness path
// to it from a root (the nearest without a profile; with one, the root of
// the heaviest path). A path never runs through another root.
//
// With a profile, every call site on a path has a count, and so has every
// function entered along it that the profile can be joined to (through
// the call sites in its body). The weight of a path is the smallest of
// those counts, and the witness is a path whose weight is as large as any
// path's to that call; the entry counts are what tell the targets of one
// virtual call apart. The weight sorts findings into three kinds of
// evidence:
//
//   observed    every call on the path ran; the weight says how often the
//               rarest one did
//   unprofiled  no call on the path is known not to have run, and at least
//               one has no count (also every finding of a run without a
//               profile)
//   latent      some call on the path never ran in the profiled run
//
// Counts are per call site, not per stack: an observed path is one whose
// every step ran, which is not proof the steps ran as that one chain.

namespace vycor {

struct HotPathConfig {
  // Qualified names (compared after effectLookupName, so `ns::f` names
  // every overload and instantiation of it) or USRs.
  std::vector<std::string> roots;
  // POSIX extended regexes searched in the qualified name.
  std::vector<std::string> rootPatterns;
  // Accepted functions, as qualified names or USRs: a call of one is not
  // reported and not followed.
  std::vector<std::string> allow;
  // With a profile: every function entered at least this often is a root.
  // 0 takes no roots from the profile.
  uint64_t minCount = 0;

  bool hasRoots() const {
    return !roots.empty() || !rootPatterns.empty() || minCount != 0;
  }
};

enum class HotEvidence : uint8_t { Observed, Unprofiled, Latent };
inline constexpr unsigned kHotEvidenceCount = 3;
/// "observed", "unprofiled", "latent".
const char *hotEvidenceName(HotEvidence evidence);

// Where a root came from; a root may be both.
inline constexpr uint8_t kHotRootConfig = 1;
inline constexpr uint8_t kHotRootProfile = 2;

struct HotPathHop {
  StringInterner::Id caller;
  StringInterner::Id callee;
  StringInterner::Id callSite;
  EdgeKind kind;
  Confidence confidence;
  // Count of the call site; CoverageProfile::kNoCount without one.
  int64_t count;
  // Entry count of the callee; kNoCount without one (a function that
  // makes no call cannot be joined to the profile).
  int64_t calleeCount;
};

struct HotPathFinding {
  EffectKind effect;
  HotEvidence evidence;
  // The weakest confidence of a hop on the path.
  Confidence confidence;
  uint8_t rootSources;
  // Smallest call-site or entry count along the path; kNoCount unless
  // every call site has one or one of the counts is zero.
  int64_t weight;
  // Entry count of the root; kNoCount without a profile or a join.
  int64_t rootCount;
  // The path, root first; the last hop is the effect call.
  uint32_t hopBegin;
  uint32_t hopEnd;
};

// How much of the index the profile's locations found.
struct HotPathJoin {
  size_t callSites = 0;          // distinct call sites in the graph
  size_t callSitesInProfile = 0; // ... in a file the profile has
  size_t callSitesCounted = 0;   // ... with an execution count
  size_t functions = 0;          // functions with at least one call
  size_t functionsCounted = 0;   // ... with an entry count
};

struct HotPathReport {
  // Canonical order: evidence (observed, unprofiled, latent), weight
  // descending, effect, then the effect call's function, site and callee.
  std::vector<HotPathFinding> findings;
  std::vector<HotPathHop> hops;

  size_t roots = 0;
  size_t configRoots = 0;
  size_t profileRoots = 0;
  // Configured roots (names, USRs, patterns) that named no function,
  // sorted.
  std::vector<std::string> unmatchedRoots;
  // Functions reached from the roots, roots included.
  size_t reachedFunctions = 0;
  // Calls of an allowed function met on the way.
  size_t allowedCalls = 0;
  // Reached template instantiations whose body no TU of the bake read (an
  // `extern template` member defined in prebuilt code, an index baked
  // before instantiations were read): what such a function does is not
  // seen, so the search was not exhaustive when this is nonzero.
  size_t templateLeaves = 0;
  // Findings by evidence, then effect kind.
  std::array<std::array<size_t, kEffectKindCount>, kHotEvidenceCount> counts{};
  HotPathJoin join;

  llvm::ArrayRef<HotPathHop> path(const HotPathFinding &finding) const {
    return llvm::ArrayRef<HotPathHop>(hops).slice(
        finding.hopBegin, finding.hopEnd - finding.hopBegin);
  }
};

/// Run the analysis. `effectMask` selects the kinds reported (effectBit);
/// `profile` may be null. False with `error` set when a root pattern is
/// not a valid regex. Every function reachable from a root through the
/// graph is visited; `templateLeaves` counts the instantiations among them
/// whose body the bake did not read.
bool analyzeHotPaths(const CallGraph &graph, const EffectTable &effects,
                     const HotPathConfig &config,
                     const CoverageProfile *profile, uint8_t effectMask,
                     HotPathReport &out, std::string &error);

} // namespace vycor
