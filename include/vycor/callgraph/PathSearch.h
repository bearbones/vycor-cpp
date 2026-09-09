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

#include "llvm/ADT/STLFunctionalExtras.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Shared bounded reverse path search over the call graph: the one
// traversal find_call_chain, the exception oracle, and the lock tools all
// use. Every hop of every path carries the exact edge it came from (caller
// and callee USRs, call site, kind, confidence, execution context), and
// the result says how the search ended, so a consumer can tell "no path
// exists within the declared bounds" from "the search stopped early".
// Contract: docs/path-analysis.md.

namespace vycor {

// Why the enumeration is not the full set of simple paths within the
// declared bounds. A result with no stop reason enumerated every path from
// a start to the target that satisfies the edge filter, the cycle rule,
// and maxDepth.
enum class StopReason : uint8_t {
  // maxPaths paths were collected; unexplored branches remain.
  PathLimit = 1u << 0,
  // A branch was cut at maxDepth: a walk longer than maxDepth exists from a
  // start through the cut node to the target. Enumeration is complete
  // WITHIN the depth bound; universal claims must be stated as bounded.
  DepthLimit = 1u << 1,
  // maxWork node expansions were spent; unexplored branches remain.
  WorkBudget = 1u << 2,
  // At least one node's ancestry was skipped because its stored in-degree
  // exceeds maxFanIn (see PathSearchResult::skippedHubs).
  HubPruned = 1u << 3,
};

// Snake-case spelling used in tool payloads ("path_limit", ...).
const char *stopReasonName(StopReason r);
// The names of every reason set in `stops`, in enum order.
std::vector<std::string> stopReasonNames(unsigned stops);

struct SearchLimits {
  // Maximum number of paths to collect. 0 means no limit.
  unsigned maxPaths = 100;
  // Maximum path length in EDGES (a chain of N functions has N-1 edges;
  // "frames above the target" for a reverse walk). 0 means no limit.
  unsigned maxDepth = 20;
  // Do not expand the ancestry of a non-target node whose stored in-degree
  // exceeds this; record it in skippedHubs instead. 0 disables.
  size_t maxFanIn = 1000;
  // Maximum number of DFS node expansions. 0 means no limit.
  size_t maxWork = 2000000;
};

// What may repeat along one path.
enum class CycleRule : uint8_t {
  // No function appears twice on a path (the find_call_chain and exception
  // oracle rule). Parallel edges through the same pair at different call
  // sites still yield distinct paths.
  SimpleNodes,
  // No EDGE (caller, callee, call site) appears twice; a function may
  // recur through different edges (the lock tools' rule: a lock acquired
  // inside a recursive cycle is still observed).
  SimpleEdges,
};

// One edge of a found path, in the direction of the call.
struct PathHop {
  std::string callerUsr;
  std::string calleeUsr;
  std::string caller; // display name (node's qualifiedName, else the usr)
  std::string callee;
  std::string callSite;
  EdgeKind kind = EdgeKind::DirectCall;
  Confidence confidence = Confidence::Unknown;
  ExecutionContext execContext = ExecutionContext::Synchronous;
  unsigned indirectionDepth = 0;
};

// start -> ... -> target. hops.front().caller is the start,
// hops.back().callee the target. Never empty: a start that IS the target
// is not a path.
struct CallPath {
  std::vector<PathHop> hops;
};

struct SkippedHub {
  std::string usr;
  std::string name;
  size_t inDegree = 0;
};

struct PathSearchResult {
  // Paths in canonical order: the DFS expands a node's callers sorted by
  // (caller usr, call site, kind, confidence, execution context), so the
  // order — and, under a path limit, the SUBSET — does not depend on TU
  // input order or index insertion order.
  std::vector<CallPath> paths;
  // Sorted by usr.
  std::vector<SkippedHub> skippedHubs;
  // Bitmask of StopReason.
  unsigned stops = 0;
  // DFS node expansions spent.
  size_t expansions = 0;
  // Whether the target / any start resolved to a known graph identity. A
  // result with either false has no paths and no stop reasons: nothing
  // was searched, and complete()/exhaustive() are false.
  bool targetKnown = false;
  bool startKnown = false;

  bool stopped(StopReason r) const {
    return (stops & static_cast<unsigned>(r)) != 0;
  }
  // A search ran and every simple path within maxDepth was enumerated.
  // DepthLimit does not clear this: it says longer walks exist, not that
  // a path in bounds was missed.
  bool complete() const {
    return targetKnown && startKnown && !stopped(StopReason::PathLimit) &&
           !stopped(StopReason::WorkBudget) &&
           !stopped(StopReason::HubPruned);
  }
  // complete() and no DepthLimit: the paths are ALL the simple paths from
  // the starts to the target. The precondition for an unconditional
  // always/never verdict over paths.
  bool exhaustive() const { return complete() && stops == 0; }
};

// Per-edge admission predicate; a false return prunes the edge (it is
// neither followed nor counted as a stop reason).
using EdgePredicate = llvm::function_ref<bool(const CallGraph::EdgeRef &)>;

// Enumerate simple paths from any of `starts` to `target` by reverse DFS
// from the target over caller edges (stored + query-time expansions, the
// callersOf edge set). A start reached on the way is a path AND is
// expanded further, so a start that another start reaches contributes
// both paths. `target` and each start may be a USR or a display
// name; a display name shared by several nodes resolves to all of them.
// Preserved search optimizations: a corridor prune (forward BFS from the
// starts records the fewest edges from any start to each node, so the DFS
// expands only callers that can still complete a path within maxDepth)
// and a dead-end memo (a node whose ancestry was exhausted from depth d
// without reaching a start is not re-explored at depth >= d; failures
// caused by the per-path exclusion, the path limit, or the work budget are
// search-state-dependent and never memoized).
PathSearchResult findCallerPaths(const CallGraph &graph,
                                 const std::string &target,
                                 const std::vector<std::string> &starts,
                                 const SearchLimits &limits,
                                 CycleRule cycles = CycleRule::SimpleNodes,
                                 EdgePredicate filter = nullptr);

} // namespace vycor
