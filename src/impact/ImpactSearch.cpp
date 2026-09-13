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

#include "vycor/impact/ImpactSearch.h"

#include <algorithm>
#include <deque>
#include <tuple>
#include <unordered_map>

namespace vycor {

namespace {

using SId = StringInterner::Id;

struct Reach {
  unsigned depth = 0;
  SId parent = 0;            // the node this one was reached from
  CallGraph::EdgeRef edge{}; // this node -> parent
  SId origin = 0;            // the changed node at depth 0
};

PathHop hopOf(const CallGraph &graph, const CallGraph::EdgeRef &e) {
  const StringInterner &in = graph.interner();
  PathHop h;
  h.callerUsr = in.resolve(e.caller);
  h.calleeUsr = in.resolve(e.callee);
  const CallGraphNode *cn = graph.findNode(h.callerUsr);
  const CallGraphNode *dn = graph.findNode(h.calleeUsr);
  h.caller = cn ? cn->qualifiedName : h.callerUsr;
  h.callee = dn ? dn->qualifiedName : h.calleeUsr;
  h.callSite = in.resolve(e.callSite);
  h.kind = e.kind;
  h.confidence = e.confidence;
  h.execContext = e.execContext;
  h.indirectionDepth = e.indirectionDepth;
  return h;
}

} // namespace

ImpactResult findImpact(const CallGraph &graph,
                        const std::vector<std::string> &changed,
                        const ImpactLimits &limits, EdgePredicate filter) {
  ImpactResult r;
  const StringInterner &in = graph.interner();

  // Seeds in usr order, so the FIFO order — and with it every tie between
  // two shallowest paths — is independent of the argument order.
  std::vector<SId> seeds;
  for (const auto &name : changed) {
    auto ids = resolveKnownIds(graph, name);
    if (ids.empty()) {
      r.unknown.push_back(name);
      continue;
    }
    seeds.insert(seeds.end(), ids.begin(), ids.end());
  }
  std::sort(seeds.begin(), seeds.end(),
            [&](SId a, SId b) { return in.resolve(a) < in.resolve(b); });
  seeds.erase(std::unique(seeds.begin(), seeds.end()), seeds.end());
  std::sort(r.unknown.begin(), r.unknown.end());
  r.unknown.erase(std::unique(r.unknown.begin(), r.unknown.end()),
                  r.unknown.end());
  for (SId s : seeds)
    r.changed.push_back(in.resolve(s));

  std::unordered_map<SId, Reach> reached;
  std::deque<SId> queue;
  for (SId s : seeds) {
    Reach seed;
    seed.origin = s;
    reached.emplace(s, seed);
    queue.push_back(s);
  }

  std::unordered_map<SId, size_t> hubs;
  while (!queue.empty()) {
    const SId node = queue.front();
    queue.pop_front();
    const unsigned depth = reached[node].depth;
    auto callers = graph.callerRefsOf(node);
    sortCallerRefsCanonically(graph, callers);
    auto admitted = [&](const CallGraph::EdgeRef &e) {
      return (!filter || filter(e)) && !reached.count(e.caller);
    };
    // Depth first: a node at the frontier has callers we deliberately do
    // not visit, whatever its fan-in or the remaining work budget.
    if (limits.maxDepth && depth >= limits.maxDepth) {
      if (std::any_of(callers.begin(), callers.end(), admitted))
        r.stops |= static_cast<unsigned>(StopReason::DepthLimit);
      continue;
    }
    // A changed function is always expanded, whatever its fan-in: its
    // callers are the question.
    if (depth > 0 && limits.maxFanIn &&
        graph.storedInDegree(node) > limits.maxFanIn) {
      hubs.emplace(node, graph.storedInDegree(node));
      r.stops |= static_cast<unsigned>(StopReason::HubPruned);
      continue;
    }
    if (limits.maxWork && r.expansions >= limits.maxWork) {
      r.stops |= static_cast<unsigned>(StopReason::WorkBudget);
      break;
    }
    ++r.expansions;
    for (const auto &e : callers) {
      if (!admitted(e))
        continue;
      Reach rc;
      rc.depth = depth + 1;
      rc.parent = node;
      rc.edge = e;
      rc.origin = reached[node].origin;
      reached.emplace(e.caller, rc);
      queue.push_back(e.caller);
    }
  }

  for (const auto &[id, rc] : reached) {
    if (rc.depth == 0)
      continue;
    AffectedFunction a;
    a.usr = in.resolve(id);
    const CallGraphNode *n = graph.findNode(a.usr);
    a.name = n ? n->qualifiedName : a.usr;
    a.depth = rc.depth;
    a.changedUsr = in.resolve(rc.origin);
    SId cur = id;
    while (true) {
      const Reach &step = reached.at(cur);
      if (step.depth == 0)
        break;
      a.path.push_back(hopOf(graph, step.edge));
      cur = step.parent;
    }
    r.affected.push_back(std::move(a));
  }
  std::sort(r.affected.begin(), r.affected.end(),
            [](const AffectedFunction &a, const AffectedFunction &b) {
              return std::tie(a.depth, a.usr) < std::tie(b.depth, b.usr);
            });
  for (const auto &[id, deg] : hubs) {
    SkippedHub h;
    h.usr = in.resolve(id);
    const CallGraphNode *n = graph.findNode(h.usr);
    h.name = n ? n->qualifiedName : h.usr;
    h.inDegree = deg;
    r.skippedHubs.push_back(std::move(h));
  }
  std::sort(r.skippedHubs.begin(), r.skippedHubs.end(),
            [](const SkippedHub &a, const SkippedHub &b) {
              return a.usr < b.usr;
            });
  return r;
}

} // namespace vycor
