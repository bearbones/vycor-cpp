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

#include "vycor/callgraph/PathSearch.h"

#include <algorithm>
#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace vycor {

const char *stopReasonName(StopReason r) {
  switch (r) {
  case StopReason::PathLimit: return "path_limit";
  case StopReason::DepthLimit: return "depth_limit";
  case StopReason::WorkBudget: return "work_budget";
  case StopReason::HubPruned: return "hub_pruned";
  }
  return "unknown";
}

std::vector<std::string> stopReasonNames(unsigned stops) {
  std::vector<std::string> out;
  for (StopReason r : {StopReason::PathLimit, StopReason::DepthLimit,
                       StopReason::WorkBudget, StopReason::HubPruned}) {
    if (stops & static_cast<unsigned>(r))
      out.push_back(stopReasonName(r));
  }
  return out;
}

namespace {

using SId = StringInterner::Id;

// Per-path exclusion key under CycleRule::SimpleEdges.
struct EdgeKey {
  SId caller, callee, site;
  bool operator==(const EdgeKey &o) const {
    return caller == o.caller && callee == o.callee && site == o.site;
  }
};
struct EdgeKeyHash {
  size_t operator()(const EdgeKey &k) const {
    uint64_t h = (static_cast<uint64_t>(k.caller) << 32) | k.callee;
    h ^= (h >> 33);
    h *= 0xff51afd7ed558ccdULL;
    h ^= k.site;
    h ^= (h >> 33);
    return static_cast<size_t>(h);
  }
};

// Resolve a usr-or-display name to node ids: every node carrying the
// display name, else the string itself when interned (an endpoint without
// a node; usr and display coincide there).
std::vector<SId> resolveIds(const CallGraph &graph, const std::string &name) {
  std::vector<SId> ids;
  const auto &interner = graph.interner();
  // Known to the search means the graph itself references the usr: a
  // node, or an edge end (a declared-only callee). Neither the interner
  // nor the name map can say on their own — the interner never forgets a
  // string, and an unregistered name resolves to itself — so after a warm
  // refresh removed the TU that knew a function, a search for it would
  // otherwise claim a complete, exhaustive answer (no callers) where a
  // clean bake of the same sources says the target is unknown.
  auto referenced = [&](SId id) {
    return graph.findNode(interner.resolve(id)) != nullptr ||
           !graph.callerRefsOf(id).empty() || !graph.calleeRefsOf(id).empty();
  };
  for (const auto &usr : graph.usrsForName(name)) {
    if (auto id = interner.find(usr))
      if (referenced(*id))
        ids.push_back(*id);
  }
  if (ids.empty()) {
    if (auto id = interner.find(name))
      if (referenced(*id))
        ids.push_back(*id);
  }
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  return ids;
}

struct Search {
  const CallGraph &graph;
  const StringInterner &interner;
  const SearchLimits &limits;
  CycleRule cycles;
  EdgePredicate filter;
  std::unordered_set<SId> startSet;
  std::unordered_map<SId, unsigned> minFromStart;

  // DFS state: currentPath[0] is the target; currentEdges[i] is the edge
  // currentPath[i+1] -> currentPath[i].
  std::vector<SId> currentPath;
  std::vector<CallGraph::EdgeRef> currentEdges;
  std::unordered_set<SId> onPath;
  std::unordered_set<EdgeKey, EdgeKeyHash> onPathEdges;
  std::unordered_map<SId, unsigned> deadAt;
  std::unordered_map<SId, std::vector<CallGraph::EdgeRef>> callersMemo;
  std::vector<std::vector<CallGraph::EdgeRef>> found; // target -> start
  std::unordered_map<SId, size_t> hubs;
  unsigned stops = 0;
  size_t expansions = 0;

  static constexpr int kFound = 1, kBlocked = 2;

  bool pathLimitHit() const {
    return limits.maxPaths != 0 && found.size() >= limits.maxPaths;
  }

  // Callers of `node` in canonical order: by caller usr, then call site,
  // then the remaining edge fields. Sorting by resolved strings (not ids,
  // which follow insertion order) is what makes the enumeration order —
  // and under a path limit, the enumerated subset — independent of TU and
  // insertion order. Computed once per node.
  const std::vector<CallGraph::EdgeRef> &callersOf(SId node) {
    auto it = callersMemo.find(node);
    if (it != callersMemo.end())
      return it->second;
    auto edges = graph.callerRefsOf(node);
    sortCallerRefsCanonically(graph, edges);
    return callersMemo.emplace(node, std::move(edges)).first->second;
  }

  // True when the path limit cut the search while unexplored branches
  // remained (as opposed to exactly maxPaths paths existing).
  bool truncated = false;

  // Does any admitted caller of `node` lie in the corridor? Used to decide
  // whether cutting the walk at `node` hides a longer path.
  bool hasCorridorCaller(SId node) {
    for (const auto &edge : callersOf(node)) {
      if (filter && !filter(edge))
        continue;
      if (minFromStart.count(edge.caller))
        return true;
    }
    return false;
  }

  int dfs(SId node, unsigned depth) {
    if (pathLimitHit()) {
      truncated = true;
      return kBlocked;
    }
    if (limits.maxWork != 0 && expansions >= limits.maxWork) {
      stops |= static_cast<unsigned>(StopReason::WorkBudget);
      return kBlocked;
    }
    ++expansions;

    auto dit = deadAt.find(node);
    if (dit != deadAt.end() && depth >= dit->second)
      return 0;

    currentPath.push_back(node);
    onPath.insert(node);
    int flags = 0;

    // A start reached at depth > 0 is a path; its own callers are still
    // expanded, because another start may reach the target through it
    // (main -> api -> target with both main and api declared as starts)
    // and an enumeration that stopped here could not call itself
    // exhaustive.
    if (startSet.count(node) && depth > 0) {
      found.push_back(currentEdges);
      flags |= kFound;
    }

    if (limits.maxFanIn != 0 && depth > 0 &&
        graph.storedInDegree(node) > limits.maxFanIn) {
      // Hub: expanding its ancestry would dominate the search. Record and
      // prune. Deterministic per node, so it does not poison the memo.
      hubs.emplace(node, graph.storedInDegree(node));
      stops |= static_cast<unsigned>(StopReason::HubPruned);
    } else if (limits.maxDepth != 0 && depth >= limits.maxDepth) {
      // Budget exhausted: any caller would make the path too long. The
      // corridor prune below normally catches this one edge earlier; this
      // is the guard for a target that is itself at the bound.
      if (hasCorridorCaller(node))
        stops |= static_cast<unsigned>(StopReason::DepthLimit);
    } else if (pathLimitHit()) {
      // The path just recorded filled the limit; whatever this node's
      // callers would add stays unexplored.
      if (hasCorridorCaller(node))
        truncated = true;
      flags |= kBlocked;
    } else {
      const auto &callers = callersOf(node);
      for (size_t i = 0; i < callers.size(); ++i) {
        const auto &edge = callers[i];
        if (filter && !filter(edge))
          continue;
        auto mit = minFromStart.find(edge.caller);
        if (mit == minFromStart.end())
          continue; // no start reaches this caller at all
        if (limits.maxDepth != 0 &&
            mit->second + depth + 1 > limits.maxDepth) {
          // A start reaches the caller, but not within budget: a longer
          // walk to the target exists through it.
          stops |= static_cast<unsigned>(StopReason::DepthLimit);
          continue;
        }
        const EdgeKey key{edge.caller, edge.callee, edge.callSite};
        const bool excluded = cycles == CycleRule::SimpleNodes
                                  ? onPath.count(edge.caller) > 0
                                  : !onPathEdges.insert(key).second;
        if (excluded) {
          flags |= kBlocked; // path-dependent exclusion
          continue;
        }
        currentEdges.push_back(edge);
        flags |= dfs(edge.caller, depth + 1);
        currentEdges.pop_back();
        if (cycles == CycleRule::SimpleEdges)
          onPathEdges.erase(key);
        if (pathLimitHit()) {
          flags |= kBlocked; // exploration truncated, not exhausted
          if (i + 1 < callers.size())
            truncated = true;
          break;
        }
      }
    }

    currentPath.pop_back();
    onPath.erase(node);

    if (!(flags & (kFound | kBlocked))) {
      auto [it, inserted] = deadAt.emplace(node, depth);
      if (!inserted && depth < it->second)
        it->second = depth;
    }
    return flags;
  }
};

} // namespace

std::vector<StringInterner::Id> resolveKnownIds(const CallGraph &graph,
                                                const std::string &name) {
  return resolveIds(graph, name);
}

void sortCallerRefsCanonically(const CallGraph &graph,
                               std::vector<CallGraph::EdgeRef> &edges) {
  const StringInterner &interner = graph.interner();
  std::sort(edges.begin(), edges.end(),
            [&](const CallGraph::EdgeRef &a, const CallGraph::EdgeRef &b) {
              if (a.caller != b.caller)
                return interner.resolve(a.caller) <
                       interner.resolve(b.caller);
              if (a.callSite != b.callSite)
                return interner.resolve(a.callSite) <
                       interner.resolve(b.callSite);
              if (a.kind != b.kind)
                return a.kind < b.kind;
              if (a.confidence != b.confidence)
                return a.confidence < b.confidence;
              if (a.execContext != b.execContext)
                return a.execContext < b.execContext;
              return a.indirectionDepth < b.indirectionDepth;
            });
}

PathSearchResult findCallerPaths(const CallGraph &graph,
                                 const std::string &target,
                                 const std::vector<std::string> &starts,
                                 const SearchLimits &limitsIn,
                                 CycleRule cycles,
                                 EdgePredicate filter) {
  PathSearchResult result;
  const auto &interner = graph.interner();
  // The walk below recurses once per edge: cap the depth so no request
  // (a 0 "no limit", a huge value) can overflow the stack.
  SearchLimits capped = limitsIn;
  if (capped.maxDepth == 0 || capped.maxDepth > kPathSearchDepthCap)
    capped.maxDepth = kPathSearchDepthCap;
  const SearchLimits &limits = capped;

  const auto targetIds = resolveIds(graph, target);
  result.targetKnown = !targetIds.empty();

  Search st{graph, interner, limits, cycles, filter};
  for (const auto &s : starts) {
    for (SId id : resolveIds(graph, s))
      st.startSet.insert(id);
  }
  result.startKnown = !st.startSet.empty();
  if (!result.targetKnown || !result.startKnown)
    return result;

  // Corridor: fewest edges from any start to each reachable node, over the
  // same filtered callee edges the reverse walk follows. Unbounded, so a
  // node absent from the map is truly unreachable from the starts and a
  // present node beyond budget proves a longer walk exists.
  {
    std::vector<SId> frontier(st.startSet.begin(), st.startSet.end());
    for (SId s : frontier)
      st.minFromStart.emplace(s, 0);
    unsigned dist = 0;
    while (!frontier.empty()) {
      ++dist;
      std::vector<SId> next;
      for (SId node : frontier) {
        for (const auto &edge : graph.calleeRefsOf(node)) {
          if (filter && !filter(edge))
            continue;
          if (st.minFromStart.emplace(edge.callee, dist).second)
            next.push_back(edge.callee);
        }
      }
      frontier = std::move(next);
    }
  }

  // Several targets (an ambiguous display name) are searched in usr order
  // so the combined enumeration stays canonical.
  std::vector<SId> orderedTargets = targetIds;
  std::sort(orderedTargets.begin(), orderedTargets.end(), [&](SId a, SId b) {
    return interner.resolve(a) < interner.resolve(b);
  });
  for (size_t i = 0; i < orderedTargets.size(); ++i) {
    SId t = orderedTargets[i];
    if (st.pathLimitHit()) {
      st.truncated = true;
      break;
    }
    auto mit = st.minFromStart.find(t);
    if (mit == st.minFromStart.end())
      continue;
    if (limits.maxDepth != 0 && mit->second > limits.maxDepth) {
      st.stops |= static_cast<unsigned>(StopReason::DepthLimit);
      continue;
    }
    st.dfs(t, 0);
  }
  if (st.truncated)
    st.stops |= static_cast<unsigned>(StopReason::PathLimit);

  auto displayOf = [&](SId id) -> const std::string & {
    const std::string &usr = interner.resolve(id);
    if (const auto *node = graph.findNode(usr))
      return node->qualifiedName;
    return usr;
  };

  result.paths.reserve(st.found.size());
  for (const auto &edges : st.found) {
    CallPath path;
    path.hops.reserve(edges.size());
    for (auto it = edges.rbegin(); it != edges.rend(); ++it) {
      PathHop hop;
      hop.callerUsr = interner.resolve(it->caller);
      hop.calleeUsr = interner.resolve(it->callee);
      hop.caller = displayOf(it->caller);
      hop.callee = displayOf(it->callee);
      hop.callSite = interner.resolve(it->callSite);
      hop.kind = it->kind;
      hop.confidence = it->confidence;
      hop.execContext = it->execContext;
      hop.indirectionDepth = it->indirectionDepth;
      path.hops.push_back(std::move(hop));
    }
    result.paths.push_back(std::move(path));
  }
  for (const auto &[id, deg] : st.hubs)
    result.skippedHubs.push_back({interner.resolve(id), displayOf(id), deg});
  std::sort(result.skippedHubs.begin(), result.skippedHubs.end(),
            [](const SkippedHub &a, const SkippedHub &b) {
              return a.usr < b.usr;
            });
  result.stops = st.stops;
  result.expansions = st.expansions;
  return result;
}

} // namespace vycor
