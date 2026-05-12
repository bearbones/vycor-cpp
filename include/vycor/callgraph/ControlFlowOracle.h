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

#include <string>
#include <vector>

namespace vycor {

class CallGraph;

// ============================================================================
// Query result types
// ============================================================================

enum class Protection {
  AlwaysCaught,     // All paths from entries catch this exception type
  SometimesCaught,  // Some paths catch, some don't
  NeverCaught,      // No paths catch this exception type
  NoexceptBarrier,  // A noexcept function on the path would terminate
  Unknown           // Cannot determine (e.g., no paths found)
};

struct PathInfo {
  std::vector<std::string> callChain; // entry -> ... -> target
  std::vector<TryCatchScope> tryCatchesOnPath;
  std::vector<ConditionalGuard> guardsOnPath;
  bool isCaught = false;
  std::string caughtAt;  // Location where caught (if caught)
  std::string caughtBy;  // Catch type that matches (if caught)
};

struct ExceptionPathResult {
  Protection protection = Protection::Unknown;
  std::vector<PathInfo> paths;
  std::string summary; // Natural-language summary for LLM consumption
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
  TryCatchScope scope;
  unsigned framesFromTarget = 0; // How many stack frames up
};

// ============================================================================
// ControlFlowOracle — query engine over CallGraph + ControlFlowIndex
// ============================================================================

class ControlFlowOracle {
public:
  ControlFlowOracle(const CallGraph &graph, const ControlFlowIndex &cfIndex);

  // Q1: Context at a specific call site.
  CallSiteExceptionInfo queryCallSite(const std::string &callSite) const;

  // Q2: Is function X always/sometimes/never called under try/catch for type T?
  ExceptionPathResult
  queryExceptionProtection(const std::string &functionName,
                           const std::string &exceptionType,
                           const std::vector<std::string> &entryPoints) const;

  // Q3: All paths from entries to X with their exception context.
  std::vector<PathInfo>
  queryAllPathContexts(const std::string &functionName,
                       const std::vector<std::string> &entryPoints,
                       unsigned maxPaths = 100) const;

  // Q4: If X throws T, is it caught before unwinding to an entry point?
  ExceptionPathResult
  queryThrowPropagation(const std::string &throwingFunction,
                        const std::string &thrownType,
                        const std::vector<std::string> &entryPoints) const;

  // Q5: Nearest try/catch on each call path to X.
  std::vector<NearestCatchInfo>
  queryNearestCatches(const std::string &functionName) const;

  // Serialize an ExceptionPathResult to JSON.
  static std::string toJson(const ExceptionPathResult &result,
                            const std::string &queryType,
                            const std::string &functionName,
                            const std::string &exceptionType = "");

  // Serialize the entire ControlFlowIndex as JSON (dump mode).
  static std::string dumpIndexToJson(const ControlFlowIndex &index);

private:
  const CallGraph &graph_;
  const ControlFlowIndex &cfIndex_;

  // Find paths from entry points to target using reverse BFS.
  void findPathsToTarget(const std::string &target,
                         const std::vector<std::string> &entryPoints,
                         unsigned maxPaths,
                         std::vector<std::vector<std::string>> &outPaths) const;

  // Check if an exception type is caught by a TryCatchScope.
  static bool isCaughtByScope(const std::string &exceptionType,
                              const TryCatchScope &scope);

  // Build a natural-language summary for an ExceptionPathResult.
  static std::string buildSummary(const ExceptionPathResult &result,
                                  const std::string &functionName,
                                  const std::string &exceptionType);
};

} // namespace vycor
