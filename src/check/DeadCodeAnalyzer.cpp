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

#include "vycor/check/DeadCodeAnalyzer.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringSet.h"

#include <algorithm>
#include <deque>
#include <tuple>

namespace vycor {

DeadCodeAnalyzer::DeadCodeAnalyzer(
    const CallGraph &graph, const std::vector<std::string> &entryPoints,
    const std::vector<std::string> &publicApi)
    : graph_(graph), entryPoints_(entryPoints),
      publicApi_(publicApi.begin(), publicApi.end()) {}

void DeadCodeAnalyzer::analyzePessimistic() {
  pessimisticAlive_.clear();
  bfs(/*includeVirtualPlausible=*/false, pessimisticAlive_);
}

void DeadCodeAnalyzer::analyzeOptimistic() {
  optimisticAlive_.clear();
  bfs(/*includeVirtualPlausible=*/true, optimisticAlive_);
}

void DeadCodeAnalyzer::bfs(bool includeVirtualPlausible,
                           std::set<std::string> &alive) {
  // Walk in interned-id space over calleeRefsOf so two overloads sharing a
  // display name keep disjoint reachability AND we avoid copying 5 strings
  // per edge across the entire call graph.
  using SId = StringInterner::Id;
  const StringInterner &interner = graph_.interner();
  llvm::DenseSet<SId> aliveIds;
  std::deque<SId> queue;

  auto seed = [&](const std::string &name) {
    for (const auto &usr : graph_.usrsForName(name)) {
      if (auto id = interner.find(usr)) {
        if (aliveIds.insert(*id).second)
          queue.push_back(*id);
      }
    }
  };
  for (const auto &ep : entryPoints_)
    seed(ep);
  for (const auto &api : publicApi_)
    seed(api);

  // Track which classes have their constructors reachable.
  llvm::StringSet<> constructedClasses;

  // Track VirtualDispatch Plausible edges deferred because the target's class
  // wasn't yet constructed. Re-checked when new classes are constructed.
  std::vector<SId> deferredVirtualTargets;

  auto isTargetClassConstructed = [&](SId calleeId) -> bool {
    const std::string &calleeUsr = interner.resolve(calleeId);
    for (const auto &cls : graph_.getClassesForImpl(calleeUsr)) {
      if (constructedClasses.contains(cls))
        return true;
    }
    if (const auto *node = graph_.findNode(calleeUsr)) {
      if (!node->enclosingClass.empty() &&
          constructedClasses.contains(node->enclosingClass))
        return true;
    }
    return false;
  };

  auto shouldFollowEdge = [&](const CallGraph::EdgeRef &edge) -> bool {
    if (edge.confidence == Confidence::Proven)
      return true;
    if (!includeVirtualPlausible)
      return false;
    if (edge.kind == EdgeKind::FunctionPointer)
      return true;
    if (edge.kind == EdgeKind::VirtualDispatch)
      return isTargetClassConstructed(edge.callee);
    return true;
  };

  // BFS with iterative re-checking for deferred virtual dispatch edges.
  bool changed = true;
  while (changed) {
    changed = false;

    while (!queue.empty()) {
      const SId current = queue.front();
      queue.pop_front();

      for (const auto &edge : graph_.calleeRefsOf(current)) {
        if (edge.kind == EdgeKind::ConstructorCall) {
          const auto *calleeNode =
              graph_.findNode(interner.resolve(edge.callee));
          if (calleeNode && !calleeNode->enclosingClass.empty()) {
            if (constructedClasses.insert(calleeNode->enclosingClass).second)
              changed = true;
          }
        }

        if (shouldFollowEdge(edge)) {
          if (aliveIds.insert(edge.callee).second)
            queue.push_back(edge.callee);
        } else if (includeVirtualPlausible &&
                   edge.kind == EdgeKind::VirtualDispatch &&
                   edge.confidence == Confidence::Plausible) {
          deferredVirtualTargets.push_back(edge.callee);
        }
      }
    }

    if (changed && includeVirtualPlausible) {
      std::vector<SId> stillDeferred;
      stillDeferred.reserve(deferredVirtualTargets.size());
      for (SId calleeId : deferredVirtualTargets) {
        if (aliveIds.contains(calleeId))
          continue;
        if (isTargetClassConstructed(calleeId)) {
          aliveIds.insert(calleeId);
          queue.push_back(calleeId);
          changed = true;
        } else {
          stillDeferred.push_back(calleeId);
        }
      }
      deferredVirtualTargets = std::move(stillDeferred);
    }
  }

  for (SId id : aliveIds)
    alive.insert(interner.resolve(id));
}

std::unordered_map<std::string, Liveness>
DeadCodeAnalyzer::getResults() const {
  // Alive sets hold usrs; results are keyed by DISPLAY name (the shape
  // consumers query by). When overloads share a display name the merged
  // entry takes the most-alive verdict — the by-name union view; PR C's
  // usr-carrying tool responses expose the per-overload split.
  std::unordered_map<std::string, Liveness> results;

  auto rank = [](Liveness l) {
    return l == Liveness::Alive ? 2
           : l == Liveness::OptimisticallyAlive ? 1
                                                : 0;
  };

  for (const auto *node : graph_.allNodes()) {
    Liveness liveness = Liveness::Dead;
    if (pessimisticAlive_.count(node->usr))
      liveness = Liveness::Alive;
    else if (optimisticAlive_.count(node->usr))
      liveness = Liveness::OptimisticallyAlive;

    auto [it, inserted] = results.emplace(node->qualifiedName, liveness);
    if (!inserted && rank(liveness) > rank(it->second))
      it->second = liveness;
  }

  return results;
}

std::vector<Diagnostic> DeadCodeAnalyzer::getDiagnostics() const {
  std::vector<Diagnostic> diags;
  auto results = getResults();

  for (const auto &kv : results) {
    const auto *node = graph_.findNode(kv.first);
    if (!node)
      continue;

    // Skip system/library functions.
    if (node->file.find("<") == 0)
      continue;

    if (kv.second == Liveness::Dead) {
      Diagnostic diag;
      diag.kind = Diagnostic::DeadCode_Pessimistic;
      diag.callLocation =
          node->file + ":" + std::to_string(node->line);
      diag.resolvedDecl = kv.first;
      diag.message = "Dead code: " + kv.first +
                     " is not reachable from any entry point.";
      diags.push_back(std::move(diag));
    } else if (kv.second == Liveness::OptimisticallyAlive) {
      Diagnostic diag;
      diag.kind = Diagnostic::DeadCode_Optimistic;
      diag.callLocation =
          node->file + ":" + std::to_string(node->line);
      diag.resolvedDecl = kv.first;
      diag.message =
          "Possibly dead code: " + kv.first +
          " is only reachable through plausible (not proven) call paths.";
      diags.push_back(std::move(diag));
    }
  }

  std::sort(diags.begin(), diags.end(),
            [](const Diagnostic &a, const Diagnostic &b) {
              return std::tie(a.callLocation, a.resolvedDecl, a.kind) <
                     std::tie(b.callLocation, b.resolvedDecl, b.kind);
            });
  return diags;
}

} // namespace vycor
