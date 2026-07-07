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

#include "vycor/anneal/DeadCodeAnalyzer.h"

#include <queue>

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
  // The walk runs in usr space (edges carry callerUsr/calleeUsr) so two
  // overloads sharing a display name keep disjoint reachability; entry
  // points and public API arrive as display names and are resolved through
  // the graph's by-name union.
  std::queue<std::string> queue;

  auto seed = [&](const std::string &name) {
    for (const auto &usr : graph_.usrsForName(name)) {
      if (alive.insert(usr).second)
        queue.push(usr);
    }
  };
  for (const auto &ep : entryPoints_)
    seed(ep);
  for (const auto &api : publicApi_)
    seed(api);

  // Track which classes have their constructors reachable.
  std::set<std::string> constructedClasses;

  // Track VirtualDispatch Plausible edges that we deferred because the
  // target's class wasn't yet constructed. We'll re-check them when new
  // classes are constructed.
  struct DeferredEdge {
    std::string callee;
  };
  std::vector<DeferredEdge> deferredVirtualEdges;

  auto shouldFollowEdge = [&](const CallGraphEdge &edge) -> bool {
    // Always follow Proven edges.
    if (edge.confidence == Confidence::Proven)
      return true;

    if (!includeVirtualPlausible)
      return false;

    // In optimistic mode, follow Plausible edges based on kind.
    if (edge.kind == EdgeKind::FunctionPointer)
      return true; // Address-taken functions are optimistically alive.

    if (edge.kind == EdgeKind::VirtualDispatch) {
      // Only follow if the target's class is constructible.
      auto classes = graph_.getClassesForImpl(edge.calleeUsr);
      for (const auto &cls : classes) {
        if (constructedClasses.count(cls))
          return true;
      }
      // Also check: is the target node's enclosingClass directly constructed?
      auto *node = graph_.findNode(edge.calleeUsr);
      if (node && !node->enclosingClass.empty()) {
        if (constructedClasses.count(node->enclosingClass))
          return true;
      }
      return false; // Defer this edge.
    }

    // Other Plausible kinds: follow in optimistic mode.
    return true;
  };

  // BFS with iterative re-checking for deferred virtual dispatch edges.
  bool changed = true;
  while (changed) {
    changed = false;

    while (!queue.empty()) {
      std::string current = std::move(queue.front());
      queue.pop();

      auto edges = graph_.calleesOf(current);
      for (const auto &edge : edges) {
        // Track constructed classes from ConstructorCall edges.
        if (edge.kind == EdgeKind::ConstructorCall) {
          auto *calleeNode = graph_.findNode(edge.calleeUsr);
          if (calleeNode && !calleeNode->enclosingClass.empty()) {
            if (constructedClasses.insert(calleeNode->enclosingClass).second) {
              changed = true; // New class constructed, may unlock deferred edges.
            }
          }
        }

        if (shouldFollowEdge(edge)) {
          if (alive.insert(edge.calleeUsr).second)
            queue.push(edge.calleeUsr);
        } else if (includeVirtualPlausible &&
                   edge.kind == EdgeKind::VirtualDispatch &&
                   edge.confidence == Confidence::Plausible) {
          deferredVirtualEdges.push_back({edge.calleeUsr});
        }
      }
    }

    // Re-check deferred virtual dispatch edges with updated constructed classes.
    if (changed && includeVirtualPlausible) {
      std::vector<DeferredEdge> stillDeferred;
      for (const auto &deferred : deferredVirtualEdges) {
        bool shouldFollow = false;
        auto classes = graph_.getClassesForImpl(deferred.callee);
        for (const auto &cls : classes) {
          if (constructedClasses.count(cls)) {
            shouldFollow = true;
            break;
          }
        }
        if (!shouldFollow) {
          auto *node = graph_.findNode(deferred.callee);
          if (node && !node->enclosingClass.empty() &&
              constructedClasses.count(node->enclosingClass))
            shouldFollow = true;
        }

        if (shouldFollow) {
          if (alive.insert(deferred.callee).second) {
            queue.push(deferred.callee);
            changed = true;
          }
        } else {
          stillDeferred.push_back(deferred);
        }
      }
      deferredVirtualEdges = std::move(stillDeferred);
    }
  }
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

  return diags;
}

} // namespace vycor
