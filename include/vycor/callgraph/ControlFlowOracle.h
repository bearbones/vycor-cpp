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

#include "vycor/callgraph/ControlFlowIndex.h"
#include "vycor/callgraph/PathSearch.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace vycor {

class CallGraph;

// ============================================================================
// Query result types
// ============================================================================

// Verdict over the paths from the entry points to a function. The
// universal verdicts (Always/Never/NoexceptBarrier) are only issued when
// the path search was exhaustive within the declared bounds AND every
// path's outcome is known; otherwise the observed-* verdicts describe the
// paths actually examined and ExceptionPathResult::search says why the
// enumeration stopped. Contract: docs/path-analysis.md.
enum class Protection {
  AlwaysCaught,     // Every path catches this exception type (exhaustive).
  SometimesCaught,  // Witnesses of both a caught and a not-caught path.
  NeverCaught,      // No path catches it (exhaustive).
  NoexceptBarrier,  // Every path terminates at a noexcept boundary
                    // (exhaustive).
  Unknown,          // No paths found, or every path's outcome is unknown.
  ObservedCaught,   // Caught on every OBSERVED path; the search was not
                    // exhaustive or some path outcome is unknown.
  ObservedUncaught, // Not caught on any OBSERVED path; same qualification.
};

// How an exception thrown by the target ends on one path.
enum class PathOutcome {
  Caught,     // A handler on the path catches it (and does not rethrow).
  Uncaught,   // It escapes the entry point.
  Terminates, // It reaches a noexcept boundary or a spawned thread's
              // entry: std::terminate.
  Unknown,    // The model cannot say: no context indexed for a hop, an
              // unevaluated exception specification, or an asynchronous
              // boundary (future / packaged task) whose retrieval site is
              // not modeled.
};

const char *protectionName(Protection p);   // "always_caught", ...
const char *pathOutcomeName(PathOutcome o); // "caught", ...

struct PathInfo {
  std::vector<std::string> callChain; // display names, entry -> target
  // The exact edges: hops[i] is callChain[i] -> callChain[i+1], with the
  // call site the propagation walk joined the control-flow context on.
  std::vector<PathHop> hops;
  // Every try/catch scope and guard enclosing any hop of the path
  // (innermost first per hop, hops in call order), whether or not the
  // propagation walk reached them.
  std::vector<TryCatchScope> tryCatchesOnPath;
  std::vector<ConditionalGuard> guardsOnPath;
  PathOutcome outcome = PathOutcome::Unknown;
  bool isCaught = false; // outcome == Caught
  std::string caughtAt;  // try location of the catching scope
  std::string caughtBy;  // handler type, or "..." for catch-all
  // Handlers that matched but rethrew before the outcome was decided,
  // innermost first (their catch locations).
  std::vector<std::string> rethrownAt;
  // For Terminates / Unknown: the call site or function where the walk
  // stopped, and why.
  std::string stopAt;
  std::string note;
};

// Facts about the path enumeration behind a result (the producer side of
// package C's completeness contract).
struct PathSearchFacts {
  SearchLimits limits;
  unsigned stops = 0; // StopReason bitmask
  size_t expansions = 0;
  std::vector<SkippedHub> skippedHubs;
  bool targetKnown = false;
  bool startKnown = false;
  // Every simple path within limits.maxDepth was enumerated.
  bool complete = false;
  // complete and no depth cutoff: the paths are all the paths.
  bool exhaustive = false;
};

struct ExceptionPathResult {
  Protection protection = Protection::Unknown;
  std::vector<PathInfo> paths;
  std::string summary; // Natural-language summary for LLM consumption
  PathSearchFacts search;
  size_t caughtCount = 0;
  size_t uncaughtCount = 0;
  size_t terminatesCount = 0;
  size_t unknownCount = 0;
  // The caller's statement that the index covers every requested TU
  // (docs/result-contract.md); false demotes a universal verdict.
  bool indexComplete = true;
  // search.exhaustive && unknownCount == 0 && indexComplete: the
  // precondition for a universal verdict.
  bool verdictExhaustive = false;
};

struct PathContextsResult {
  std::vector<PathInfo> paths;
  PathSearchFacts search;
};

struct CallSiteExceptionInfo {
  std::string callSite;
  std::string caller;
  std::string callee;
  bool isUnderTryCatch = false;
  std::vector<TryCatchScope> enclosingScopes; // innermost first
  std::vector<ConditionalGuard> enclosingGuards;
  NoexceptSpec callerNoexcept = NoexceptSpec::None;
  bool wouldTerminateIfThrows = false; // true if noexcept caller
};

struct NearestCatchInfo {
  std::vector<std::string> pathSegment; // From catch site to target
  std::vector<PathHop> hops;            // pathSegment's edges, in order
  TryCatchScope scope;                  // innermost scope at the call
  std::string callSite;                 // the call the scope encloses
  unsigned framesFromTarget = 0;        // How many stack frames up
};

struct NearestCatchResult {
  std::vector<NearestCatchInfo> catches;
  unsigned maxDepth = 0;
  unsigned stops = 0; // DepthLimit when callers remained beyond maxDepth
  bool complete = true;
  bool targetKnown = false;
};

// ============================================================================
// ControlFlowOracle — query engine over CallGraph + ControlFlowIndex
// ============================================================================

class ControlFlowOracle {
public:
  ControlFlowOracle(const CallGraph &graph, const ControlFlowIndex &cfIndex);

  // Q1: Context at a specific call site.
  CallSiteExceptionInfo queryCallSite(const std::string &callSite) const;

  // Q2: Is function X always/sometimes/never called under try/catch for
  // type T? An empty exceptionType matches every handler. Paths come from
  // findCallerPaths (CycleRule::SimpleNodes) with `limits`; each is
  // walked from the target outward in propagation order: a spawned-thread
  // edge terminates, an async edge is unknown, then the caller's scopes
  // innermost first (first matching handler in source order; a rethrowing
  // handler passes the exception on), then the caller's noexcept
  // boundary, then the next hop. Contexts are joined on the exact (call
  // site, caller USR); a hop with no indexed context is unknown, not
  // unprotected. `indexComplete` is the adapter's coverage fact: with
  // it false the universal verdicts (Always/Never/NoexceptBarrier) are
  // demoted to the observed ones, since a handler may live in a TU the
  // index does not hold.
  ExceptionPathResult
  queryExceptionProtection(const std::string &functionName,
                           const std::string &exceptionType,
                           const std::vector<std::string> &entryPoints,
                           const SearchLimits &limits = {},
                           bool indexComplete = true) const;

  // Q3: All paths from entries to X with their exception context (no
  // propagation walk: scopes and guards of every hop, outcome Unknown).
  PathContextsResult
  queryAllPathContexts(const std::string &functionName,
                       const std::vector<std::string> &entryPoints,
                       const SearchLimits &limits = {}) const;

  // Q4: If X throws T, is it caught before unwinding to an entry point?
  ExceptionPathResult
  queryThrowPropagation(const std::string &throwingFunction,
                        const std::string &thrownType,
                        const std::vector<std::string> &entryPoints,
                        const SearchLimits &limits = {},
                        bool indexComplete = true) const;

  // Q5: Nearest try/catch on each caller chain into X: a breadth-first
  // walk over caller edges (each edge examined once, callers in canonical
  // order) up to maxDepth frames; a chain stops at the first edge whose
  // context has a try/catch. Deterministic: the reported segment is the
  // canonical shortest chain.
  NearestCatchResult queryNearestCatches(const std::string &functionName,
                                         unsigned maxDepth = 20) const;

  // Whether `exceptionType` is caught by `scope` (any handler). Empty type
  // matches every handler. Uses the hardcoded std hierarchy and the
  // graph's recorded class hierarchy.
  bool isCaughtByScope(const std::string &exceptionType,
                       const TryCatchScope &scope) const;

private:
  const CallGraph &graph_;
  const ControlFlowIndex &cfIndex_;

  // The control-flow context of a hop: contexts at the hop's call site
  // whose caller is the hop's caller (exact identity), preferring the one
  // recorded for the hop's callee. nullopt when nothing is indexed there.
  std::optional<CallSiteContext> contextForHop(const PathHop &hop) const;

  // First handler of `scope` (source order) that catches exceptionType.
  const CatchHandlerInfo *
  matchingHandler(const std::string &exceptionType,
                  const TryCatchScope &scope) const;

  bool isSubtypeOf(const std::string &thrownType,
                   const std::string &caughtType) const;

  // Fill callChain/hops/scopes/guards from a found path.
  PathInfo annotatePath(const CallPath &path) const;
  // Decide the outcome of a throw of exceptionType from the path's target.
  void propagate(PathInfo &info, const std::string &exceptionType) const;

  static PathSearchFacts factsOf(const PathSearchResult &r,
                                 const SearchLimits &limits);

  // Build a natural-language summary for an ExceptionPathResult.
  static std::string buildSummary(const ExceptionPathResult &result,
                                  const std::string &functionName,
                                  const std::string &exceptionType);
};

} // namespace vycor
