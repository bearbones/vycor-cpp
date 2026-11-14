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

#include "vycor/callgraph/ControlFlowOracle.h"
#include "vycor/callgraph/CallGraph.h"
#include "vycor/ext/Extensions.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

#include <algorithm>
#include <deque>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace vycor {

const char *protectionName(Protection p) {
  switch (p) {
  case Protection::AlwaysCaught: return "always_caught";
  case Protection::SometimesCaught: return "sometimes_caught";
  case Protection::NeverCaught: return "never_caught";
  case Protection::NoexceptBarrier: return "noexcept_barrier";
  case Protection::Unknown: return "unknown";
  case Protection::ObservedCaught: return "observed_caught";
  case Protection::ObservedUncaught: return "observed_uncaught";
  }
  return "unknown";
}

const char *pathOutcomeName(PathOutcome o) {
  switch (o) {
  case PathOutcome::Caught: return "caught";
  case PathOutcome::Uncaught: return "uncaught";
  case PathOutcome::Terminates: return "terminates";
  case PathOutcome::Unknown: return "unknown";
  }
  return "unknown";
}

// ============================================================================
// Hardcoded std exception hierarchy for type matching
// ============================================================================

// Maps derived exception types to their known bases.
static const std::vector<std::pair<std::string, std::string>> &
stdExceptionHierarchy() {
  static const std::vector<std::pair<std::string, std::string>> hierarchy = {
      {"std::bad_alloc", "std::exception"},
      {"std::bad_cast", "std::exception"},
      {"std::bad_typeid", "std::exception"},
      {"std::bad_function_call", "std::exception"},
      {"std::bad_weak_ptr", "std::exception"},
      {"std::bad_array_new_length", "std::bad_alloc"},
      {"std::logic_error", "std::exception"},
      {"std::runtime_error", "std::exception"},
      {"std::domain_error", "std::logic_error"},
      {"std::invalid_argument", "std::logic_error"},
      {"std::length_error", "std::logic_error"},
      {"std::out_of_range", "std::logic_error"},
      {"std::overflow_error", "std::runtime_error"},
      {"std::underflow_error", "std::runtime_error"},
      {"std::range_error", "std::runtime_error"},
      {"std::system_error", "std::runtime_error"},
      {"std::ios_base::failure", "std::system_error"},
      {"std::filesystem::filesystem_error", "std::system_error"},
  };
  return hierarchy;
}

// Strip reference, const, and surrounding whitespace without front-shift erases.
static llvm::StringRef stripTypeSpellingView(llvm::StringRef s) noexcept {
  while (!s.empty() && (s.back() == '&' || s.back() == ' '))
    s = s.drop_back(1);
  if (s.starts_with("const "))
    s = s.drop_front(6);
  return s.trim(' ');
}

bool ControlFlowOracle::isSubtypeOf(const std::string &thrownType,
                                    const std::string &caughtType) const {
  if (thrownType == caughtType)
    return true;
  const llvm::StringRef thrownView = stripTypeSpellingView(thrownType);
  const llvm::StringRef caughtView = stripTypeSpellingView(caughtType);
  if (thrownView == caughtView)
    return true;

  // Walk the hardcoded std hierarchy on the stack (no std::set allocations).
  llvm::StringRef current = thrownView;
  llvm::SmallVector<llvm::StringRef, 8> visited;
  while (std::find(visited.begin(), visited.end(), current) == visited.end()) {
    visited.push_back(current);
    bool advanced = false;
    for (const auto &pair : stdExceptionHierarchy()) {
      if (pair.first == current) {
        if (pair.second == caughtView)
          return true;
        current = pair.second;
        advanced = true;
        break;
      }
    }
    if (!advanced)
      break;
  }

  // The graph's recorded class hierarchy (qualified names): a user type
  // derived, directly or transitively, from the caught type.
  const std::string caught(caughtView);
  for (const auto &derived : graph_.getAllDerivedClasses(caught)) {
    if (derived == thrownView)
      return true;
  }
  return false;
}

// ============================================================================
// Constructor
// ============================================================================

ControlFlowOracle::ControlFlowOracle(const CallGraph &graph,
                                     const ControlFlowIndex &cfIndex)
    : graph_(graph), cfIndex_(cfIndex) {}

// ============================================================================
// Q1: queryCallSite
// ============================================================================

CallSiteExceptionInfo
ControlFlowOracle::queryCallSite(const std::string &callSite) const {
  CallSiteExceptionInfo info;
  info.callSite = callSite;

  const auto ctx = cfIndex_.contextAtSite(callSite);
  if (!ctx)
    return info;

  info.caller = ctx->callerName;
  info.callee = ctx->calleeName;
  info.isUnderTryCatch = !ctx->enclosingTryCatches.empty();
  info.enclosingScopes = ctx->enclosingTryCatches;
  info.enclosingGuards = ctx->enclosingGuards;
  info.callerNoexcept = ctx->callerNoexcept;
  info.wouldTerminateIfThrows =
      (ctx->callerNoexcept == NoexceptSpec::Noexcept);

  return info;
}

// ============================================================================
// Exception type matching
// ============================================================================

const CatchHandlerInfo *
ControlFlowOracle::matchingHandler(const std::string &exceptionType,
                                   const TryCatchScope &scope) const {
  // Handlers are tried in source order; the first match wins (a catch-all
  // before a typed handler shadows it, as in the language).
  for (const auto &handler : scope.handlers) {
    if (handler.isCatchAll || exceptionType.empty() ||
        isSubtypeOf(exceptionType, handler.caughtType))
      return &handler;
  }
  return nullptr;
}

bool ControlFlowOracle::isCaughtByScope(const std::string &exceptionType,
                                        const TryCatchScope &scope) const {
  return matchingHandler(exceptionType, scope) != nullptr;
}

// ============================================================================
// Context join: exact (call site, caller) identity
// ============================================================================

std::optional<CallSiteContext>
ControlFlowOracle::contextForHop(const PathHop &hop) const {
  return cfIndex_.contextForEdge(hop.callSite, hop.callerUsr, hop.calleeUsr);
}

// ============================================================================
// Path annotation and propagation
// ============================================================================

PathInfo ControlFlowOracle::annotatePath(const CallPath &path) const {
  PathInfo info;
  info.hops = path.hops;
  info.callChain.reserve(path.hops.size() + 1);
  info.callChain.push_back(path.hops.front().caller);
  for (const auto &hop : path.hops)
    info.callChain.push_back(hop.callee);
  for (const auto &hop : path.hops) {
    auto ctx = contextForHop(hop);
    if (!ctx)
      continue;
    for (const auto &scope : ctx->enclosingTryCatches)
      info.tryCatchesOnPath.push_back(scope);
    for (const auto &guard : ctx->enclosingGuards)
      info.guardsOnPath.push_back(guard);
  }
  return info;
}

void ControlFlowOracle::propagate(PathInfo &info,
                                  const std::string &exceptionType) const {
  // Frame 0: a noexcept target terminates before unwinding anywhere. The
  // spec is known only through the target's own call sites; a leaf with
  // none is walked as if it may throw (docs/path-analysis.md, limits).
  const PathHop &into = info.hops.back();
  if (auto spec = cfIndex_.callerNoexceptOf(into.calleeUsr)) {
    if (*spec == NoexceptSpec::Noexcept || *spec == NoexceptSpec::ThrowNone) {
      info.outcome = PathOutcome::Terminates;
      info.stopAt = into.callee;
      info.note = into.callee + " is noexcept: a throw inside it calls "
                                "std::terminate";
      return;
    }
  }

  // Walk from the target outward: hops.back() is the call INTO the target.
  for (size_t i = info.hops.size(); i-- > 0;) {
    const PathHop &hop = info.hops[i];

    // The exception leaves hop.callee. On an asynchronous edge it never
    // unwinds into hop.caller's frame at all.
    switch (hop.execContext) {
    case ExecutionContext::ThreadSpawn:
      info.outcome = PathOutcome::Terminates;
      info.stopAt = hop.callSite;
      info.note = hop.callee + " runs on a thread spawned at " +
                  hop.callSite + "; an exception escaping a thread entry "
                  "calls std::terminate";
      return;
    case ExecutionContext::AsyncTask:
    case ExecutionContext::PackagedTask:
      info.outcome = PathOutcome::Unknown;
      info.stopAt = hop.callSite;
      info.note = hop.callee + " runs as an asynchronous task started at " +
                  hop.callSite + "; the exception is stored and rethrown "
                  "where the result is retrieved, which is not modeled";
      return;
    case ExecutionContext::Synchronous:
    case ExecutionContext::Invoke:
      break;
    }

    auto ctx = contextForHop(hop);
    if (!ctx) {
      info.outcome = PathOutcome::Unknown;
      info.stopAt = hop.callSite;
      info.note = "no call-site context indexed for " + hop.callSite +
                  " in " + hop.caller +
                  " (the TU may be missing, partial, or the call is "
                  "indirect)";
      return;
    }

    // Innermost scope first; the first matching handler in source order
    // decides. A rethrowing handler hands the exception to the next
    // enclosing scope.
    for (const auto &scope : ctx->enclosingTryCatches) {
      const CatchHandlerInfo *h = matchingHandler(exceptionType, scope);
      if (!h)
        continue;
      if (h->rethrows) {
        info.rethrownAt.push_back(h->location);
        continue;
      }
      info.outcome = PathOutcome::Caught;
      info.isCaught = true;
      info.caughtAt = scope.tryLocation;
      info.caughtBy = h->isCatchAll ? "..." : h->caughtType;
      return;
    }

    // Escapes hop.caller's body.
    switch (ctx->callerNoexcept) {
    case NoexceptSpec::Noexcept:
    case NoexceptSpec::ThrowNone:
      info.outcome = PathOutcome::Terminates;
      info.stopAt = hop.caller;
      info.note = "escapes " + hop.caller +
                  ", which is noexcept: std::terminate";
      return;
    case NoexceptSpec::Unknown:
      info.outcome = PathOutcome::Unknown;
      info.stopAt = hop.caller;
      info.note = "the exception specification of " + hop.caller +
                  " could not be evaluated";
      return;
    case NoexceptSpec::None:
    case NoexceptSpec::NoexceptFalse:
      break;
    }
  }
  info.outcome = PathOutcome::Uncaught;
  info.isCaught = false;
  info.stopAt = info.callChain.front();
  info.note = "escapes the entry point " + info.callChain.front();
}

PathSearchFacts ControlFlowOracle::factsOf(const PathSearchResult &r,
                                           const SearchLimits &limits) {
  PathSearchFacts f;
  f.limits = limits;
  f.stops = r.stops;
  f.expansions = r.expansions;
  f.skippedHubs = r.skippedHubs;
  f.targetKnown = r.targetKnown;
  f.startKnown = r.startKnown;
  f.complete = r.complete();
  f.exhaustive = r.exhaustive();
  return f;
}

// ============================================================================
// Q2: queryExceptionProtection
// ============================================================================

ExceptionPathResult ControlFlowOracle::queryExceptionProtection(
    const std::string &functionName, const std::string &exceptionType,
    const std::vector<std::string> &entryPoints,
    const SearchLimits &limits, bool indexComplete) const {

  ExceptionPathResult result;
  result.indexComplete = indexComplete;
  const auto found = findCallerPaths(graph_, functionName, entryPoints,
                                     limits, CycleRule::SimpleNodes);
  result.search = factsOf(found, limits);

  for (const auto &path : found.paths) {
    PathInfo pi = annotatePath(path);
    propagate(pi, exceptionType);
    switch (pi.outcome) {
    case PathOutcome::Caught: ++result.caughtCount; break;
    case PathOutcome::Uncaught: ++result.uncaughtCount; break;
    case PathOutcome::Terminates: ++result.terminatesCount; break;
    case PathOutcome::Unknown: ++result.unknownCount; break;
    }
    result.paths.push_back(std::move(pi));
  }

  result.verdictExhaustive = result.search.exhaustive &&
                             result.unknownCount == 0 && indexComplete;
  const bool exhaustive = result.verdictExhaustive;
  const size_t caught = result.caughtCount;
  const size_t uncaught = result.uncaughtCount;
  const size_t terminates = result.terminatesCount;

  if (result.paths.empty()) {
    result.protection = Protection::Unknown;
  } else if (caught > 0 && (uncaught > 0 || terminates > 0)) {
    result.protection = Protection::SometimesCaught;
  } else if (uncaught > 0) {
    result.protection =
        exhaustive ? Protection::NeverCaught : Protection::ObservedUncaught;
  } else if (caught > 0) {
    result.protection =
        exhaustive ? Protection::AlwaysCaught : Protection::ObservedCaught;
  } else if (terminates > 0) {
    result.protection =
        exhaustive ? Protection::NoexceptBarrier : Protection::Unknown;
  } else {
    result.protection = Protection::Unknown;
  }

  result.summary = buildSummary(result, functionName, exceptionType);
  return result;
}

// ============================================================================
// Q3: queryAllPathContexts
// ============================================================================

PathContextsResult ControlFlowOracle::queryAllPathContexts(
    const std::string &functionName,
    const std::vector<std::string> &entryPoints,
    const SearchLimits &limits) const {
  PathContextsResult result;
  const auto found = findCallerPaths(graph_, functionName, entryPoints,
                                     limits, CycleRule::SimpleNodes);
  result.search = factsOf(found, limits);
  for (const auto &path : found.paths)
    result.paths.push_back(annotatePath(path));
  return result;
}

// ============================================================================
// Q4: queryThrowPropagation (delegates to queryExceptionProtection)
// ============================================================================

ExceptionPathResult ControlFlowOracle::queryThrowPropagation(
    const std::string &throwingFunction, const std::string &thrownType,
    const std::vector<std::string> &entryPoints,
    const SearchLimits &limits, bool indexComplete) const {
  return queryExceptionProtection(throwingFunction, thrownType, entryPoints,
                                  limits, indexComplete);
}

// ============================================================================
// Q5: queryNearestCatches
// ============================================================================

NearestCatchResult
ControlFlowOracle::queryNearestCatches(const std::string &functionName,
                                       unsigned maxDepth) const {
  NearestCatchResult result;
  result.maxDepth = maxDepth;

  using SId = StringInterner::Id;
  const StringInterner &interner = graph_.interner();
  // Resolve via resolveKnownIds so stale strings left in StringInterner after
  // removeTU warm refresh do not falsely report targetKnown = true.
  std::vector<SId> targets = resolveKnownIds(graph_, functionName);
  result.targetKnown = !targets.empty();
  if (!result.targetKnown)
    return result;

  std::sort(targets.begin(), targets.end(), [&](SId a, SId b) {
    return interner.resolve(a) < interner.resolve(b);
  });

  // BFS tree in SId space: callerId -> (child calleeId, hop into child).
  // Callers are visited in canonical (callerUsr, callSite, ...) order so the
  // tree — and reported chains — never depend on edge insertion order.
  struct Parent {
    SId child;
    PathHop hop;
  };
  std::unordered_map<SId, Parent> parent;
  std::unordered_set<SId> visited;
  std::deque<std::pair<SId, unsigned>> queue;
  for (SId t : targets) {
    if (visited.insert(t).second)
      queue.emplace_back(t, 0);
  }

  // Rebuild the chain from a caller down to the target through the tree.
  auto chainFrom = [&](SId callerId, SId firstCalleeId,
                       const PathHop &firstHop) {
    NearestCatchInfo nci;
    nci.hops.push_back(firstHop);
    SId cur = firstCalleeId;
    while (true) {
      auto it = parent.find(cur);
      if (it == parent.end())
        break;
      nci.hops.push_back(it->second.hop);
      cur = it->second.child;
    }
    nci.pathSegment.push_back(graph_.displayNameOf(callerId));
    for (const auto &h : nci.hops)
      nci.pathSegment.push_back(h.callee);
    return nci;
  };

  while (!queue.empty()) {
    auto [node, depth] = queue.front();
    queue.pop_front();
    if (depth >= maxDepth) {
      if (!graph_.callerRefsOf(node).empty())
        result.stops |= static_cast<unsigned>(StopReason::DepthLimit);
      continue;
    }

    auto callers = graph_.callerRefsOf(node);
    sortCallerRefsCanonically(graph_, callers);
    for (const auto &edge : callers) {
      PathHop hop;
      hop.callerUsr = interner.resolve(edge.caller);
      hop.calleeUsr = interner.resolve(edge.callee);
      hop.caller = graph_.displayNameOf(edge.caller);
      hop.callee = graph_.displayNameOf(edge.callee);
      hop.callSite = interner.resolve(edge.callSite);
      hop.kind = edge.kind;
      hop.confidence = edge.confidence;
      hop.execContext = edge.execContext;
      hop.indirectionDepth = edge.indirectionDepth;

      // The scope check is per EDGE: a caller already reached through
      // another edge still contributes its try/catch at this one.
      auto ctx = contextForHop(hop);
      if (ctx && !ctx->enclosingTryCatches.empty()) {
        NearestCatchInfo nci = chainFrom(edge.caller, edge.callee, hop);
        nci.scope = ctx->enclosingTryCatches.front(); // Innermost.
        nci.callSite = hop.callSite;
        nci.framesFromTarget = depth + 1;
        result.catches.push_back(std::move(nci));
        continue;
      }
      if (visited.insert(edge.caller).second) {
        parent.emplace(edge.caller, Parent{node, std::move(hop)});
        queue.emplace_back(edge.caller, depth + 1);
      }
    }
  }

  std::sort(result.catches.begin(), result.catches.end(),
            [](const NearestCatchInfo &a, const NearestCatchInfo &b) {
              if (a.framesFromTarget != b.framesFromTarget)
                return a.framesFromTarget < b.framesFromTarget;
              if (a.pathSegment != b.pathSegment)
                return a.pathSegment < b.pathSegment;
              return a.callSite < b.callSite;
            });
  result.complete = result.stops == 0;
  return result;
}

// ============================================================================
// Summary generation
// ============================================================================

std::string
ControlFlowOracle::buildSummary(const ExceptionPathResult &result,
                                const std::string &functionName,
                                const std::string &exceptionType) {
  std::ostringstream ss;
  const std::string what =
      exceptionType.empty() ? std::string("an exception")
                            : exceptionType;
  const size_t total = result.paths.size();

  auto firstUncaught = [&]() -> const PathInfo * {
    for (const auto &p : result.paths)
      if (p.outcome == PathOutcome::Uncaught)
        return &p;
    return nullptr;
  };
  auto chainOf = [](const PathInfo &p) {
    std::string s;
    for (size_t i = 0; i < p.callChain.size(); ++i) {
      if (i > 0)
        s += " -> ";
      s += p.callChain[i];
    }
    return s;
  };

  // Which precondition of a universal verdict failed: the search bound,
  // the index coverage, or an unknown path outcome (any combination).
  auto whyObserved = [&]() {
    std::vector<std::string> parts;
    if (!result.search.exhaustive)
      parts.push_back("the search is not exhaustive");
    if (!result.indexComplete)
      parts.push_back("the index does not cover every requested TU");
    if (result.unknownCount > 0)
      parts.push_back(std::to_string(result.unknownCount) +
                      " path(s) have an unknown outcome");
    std::string why;
    for (size_t i = 0; i < parts.size(); ++i) {
      if (i > 0)
        why += i + 1 == parts.size() ? " and " : ", ";
      why += parts[i];
    }
    return why;
  };

  switch (result.protection) {
  case Protection::AlwaysCaught:
    ss << functionName << " throwing " << what << " is caught on all "
       << total << " path(s) from entry points.";
    if (!result.paths.empty() && !result.paths[0].caughtBy.empty()) {
      ss << " Caught by " << result.paths[0].caughtBy << " at "
         << result.paths[0].caughtAt << ".";
    }
    break;

  case Protection::NeverCaught:
    ss << functionName << " throwing " << what
       << " is NOT caught on any of the " << total
       << " path(s) from entry points.";
    break;

  case Protection::SometimesCaught:
    ss << functionName << " throwing " << what << " is caught on "
       << result.caughtCount << " of " << total << " path(s)";
    if (result.terminatesCount > 0)
      ss << " (" << result.terminatesCount << " terminate at a noexcept or "
         << "thread boundary)";
    ss << ".";
    if (const PathInfo *p = firstUncaught())
      ss << " Uncaught path: " << chainOf(*p) << ".";
    break;

  case Protection::NoexceptBarrier:
    ss << functionName << " throwing " << what
       << " reaches a noexcept function or a thread entry on all " << total
       << " path(s): std::terminate rather than propagation.";
    break;

  case Protection::ObservedCaught:
    ss << functionName << " throwing " << what << " is caught on every one "
       << "of the " << total << " observed path(s), but " << whyObserved();
    ss << "; unexamined paths may be unprotected.";
    break;

  case Protection::ObservedUncaught:
    ss << functionName << " throwing " << what << " is NOT caught on any "
       << "of the " << total << " observed path(s); " << whyObserved();
    ss << ".";
    if (const PathInfo *p = firstUncaught())
      ss << " Uncaught path: " << chainOf(*p) << ".";
    break;

  case Protection::Unknown:
    if (result.paths.empty()) {
      if (!result.search.targetKnown)
        ss << functionName << " is not in the call graph.";
      else if (!result.search.startKnown)
        ss << "None of the entry points is in the call graph.";
      else
        ss << "No call paths found from entry points to " << functionName
           << ".";
    } else {
      ss << functionName << " throwing " << what << ": the outcome of the "
         << total << " path(s) cannot be determined";
      if (result.terminatesCount > 0)
        ss << " (" << result.terminatesCount << " terminate, "
           << result.unknownCount << " unknown)";
      // A would-be noexcept_barrier demoted by the search bound or the
      // coverage: say which, as the observed verdicts do.
      if (result.terminatesCount > 0 && result.unknownCount == 0 &&
          !result.verdictExhaustive)
        ss << "; every observed path terminates, but " << whyObserved();
      ss << ".";
    }
    break;
  }

  if (result.search.stops != 0) {
    ss << " Search stopped:";
    for (const auto &name : stopReasonNames(result.search.stops))
      ss << " " << name;
    ss << ".";
  }
  return ss.str();
}

} // namespace vycor
