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


#include "vycor/query/Tools.h"
#include "vycor/query/Identity.h"
#include "vycor/query/Serialize.h"
#include "Registry.h"
#include "Schema.h"

#include "llvm/ADT/StringRef.h"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace vycor {

// ============================================================================
// Edge filter shared across get_callees, get_callers, find_call_chain
// ============================================================================

struct EdgeFilter {
  std::set<EdgeKind> kinds; // empty = allow all
  std::set<Confidence> includeConfidences; // non-empty overrides minConf
  bool useIncludeSet = false;
  Confidence minConf = Confidence::Unknown;
  std::set<ExecutionContext> execContexts; // empty = allow all

  bool allows(const CallGraphEdge &e) const {
    return allowsFields(e.kind, e.confidence, e.execContext);
  }

  // Id-space twin for traversal loops that never materialize strings.
  bool allowsRef(const CallGraph::EdgeRef &e) const {
    return allowsFields(e.kind, e.confidence, e.execContext);
  }

private:
  bool allowsFields(EdgeKind kind, Confidence conf,
                    ExecutionContext execCtx) const {
    if (!kinds.empty() && !kinds.count(kind))
      return false;
    if (!execContexts.empty() && !execContexts.count(execCtx))
      return false;
    if (useIncludeSet)
      return includeConfidences.count(conf) > 0;
    return confidenceRank(conf) >= confidenceRank(minConf);
  }
};

// Parse an EdgeFilter from tool args. Returns an error message on invalid
// input (specifically, unrecognized include_confidences values).
static std::optional<std::string>
parseEdgeFilter(const llvm::json::Object &args, EdgeFilter &out) {
  if (auto *kindsArr = args.getArray("edge_kinds")) {
    for (auto &v : *kindsArr) {
      if (auto s = v.getAsString())
        out.kinds.insert(parseEdgeKind(*s));
    }
  }
  if (auto *confs = args.getArray("include_confidences")) {
    out.useIncludeSet = true;
    for (auto &v : *confs) {
      auto s = v.getAsString();
      if (!s)
        continue;
      if (*s != "Proven" && *s != "Plausible" && *s != "Unknown") {
        return "Invalid value in include_confidences: '" + s->str() +
               "' (expected Proven, Plausible, or Unknown)";
      }
      out.includeConfidences.insert(parseConfidence(*s));
    }
  } else if (auto mc = args.getString("min_confidence")) {
    out.minConf = parseConfidence(*mc);
  }
  if (auto *ctxArr = args.getArray("execution_contexts")) {
    for (auto &v : *ctxArr) {
      auto s = v.getAsString();
      if (!s)
        continue;
      auto parsed = parseExecutionContext(*s);
      if (!parsed) {
        return "Invalid value in execution_contexts: '" + s->str() +
               "' (expected Synchronous, ThreadSpawn, AsyncTask, "
               "PackagedTask, or Invoke)";
      }
      out.execContexts.insert(*parsed);
    }
  }
  return std::nullopt;
}


// ============================================================================
// Tool 1: lookup_function
// ============================================================================

static llvm::json::Value handleLookupFunction(const llvm::json::Object &args,
                                              const ToolContext &ctx) {
  std::optional<llvm::json::Value> ambiguous;
  auto ident = resolveIdentity(args, ctx, "name", "usr", ambiguous);
  if (ambiguous)
    return std::move(*ambiguous);
  if (!ident)
    return errorResult("Missing required parameter 'name' (or 'usr')");

  auto *node = ctx.graph.findNode(*ident);
  if (!node)
    return errorResult("Function not found: " + *ident);

  llvm::json::Object obj;
  obj["qualifiedName"] = node->qualifiedName;
  obj["usr"] = node->usr;
  obj["file"] = node->file;
  obj["line"] = static_cast<int64_t>(node->line);
  obj["isEntryPoint"] = node->isEntryPoint;
  obj["isVirtual"] = node->isVirtual;
  if (!node->enclosingClass.empty())
    obj["enclosingClass"] = node->enclosingClass;
  return llvm::json::Value(std::move(obj));
}

// ============================================================================
// Tool 1a: search_functions
// ============================================================================

static llvm::json::Value
handleSearchFunctions(const llvm::json::Object &args,
                      const ToolContext &ctx) {
  auto query = args.getString("query");
  if (!query || query->empty())
    return errorResult("Missing required parameter 'query'");

  int64_t limit = 25;
  if (auto l = args.getInteger("limit"))
    limit = std::max<int64_t>(1, *l);

  std::string needle = query->lower();

  // Lowercased name index, built once per graph state and cached — the
  // per-query lowering of every node name was 14 ms on a 57k-node graph.
  // Node pointers are stable (nodes live in a node-based map) and the
  // cache is cleared whenever the graph mutates.
  struct SearchEntry {
    std::string lowerQualified;
    size_t unqualifiedOffset; // offset of the unqualified name within it
    const CallGraphNode *node;
  };
  using SearchIndex = std::vector<SearchEntry>;
  std::shared_ptr<const SearchIndex> index;
  if (ctx.cache) {
    auto it = ctx.cache->objects.find("search_index");
    if (it != ctx.cache->objects.end())
      index = std::static_pointer_cast<const SearchIndex>(it->second);
  }
  if (!index) {
    auto built = std::make_shared<SearchIndex>();
    auto nodes = ctx.graph.allNodes();
    built->reserve(nodes.size());
    for (auto *node : nodes) {
      llvm::StringRef qn(node->qualifiedName);
      size_t off = 0;
      auto sep = qn.rfind("::");
      if (sep != llvm::StringRef::npos)
        off = sep + 2;
      built->push_back({qn.lower(), off, node});
    }
    if (ctx.cache)
      ctx.cache->objects["search_index"] = built;
    index = std::move(built);
  }

  // Rank: exact name match, then prefix of the unqualified name, then any
  // substring. Within a tier, shorter qualified names first (closer match).
  struct Hit {
    const CallGraphNode *node;
    int tier;
  };
  std::vector<Hit> hits;
  for (const auto &entry : *index) {
    llvm::StringRef lower(entry.lowerQualified);
    if (lower.find(needle) == llvm::StringRef::npos)
      continue;
    int tier = 2;
    llvm::StringRef unqLower = lower.substr(entry.unqualifiedOffset);
    if (unqLower == needle)
      tier = 0;
    else if (unqLower.starts_with(needle))
      tier = 1;
    hits.push_back({entry.node, tier});
  }

  std::sort(hits.begin(), hits.end(), [](const Hit &a, const Hit &b) {
    if (a.tier != b.tier)
      return a.tier < b.tier;
    if (a.node->qualifiedName.size() != b.node->qualifiedName.size())
      return a.node->qualifiedName.size() < b.node->qualifiedName.size();
    return a.node->qualifiedName < b.node->qualifiedName;
  });

  llvm::json::Array results;
  for (const auto &hit : hits) {
    if (static_cast<int64_t>(results.size()) >= limit)
      break;
    llvm::json::Object entry;
    entry["qualifiedName"] = hit.node->qualifiedName;
    entry["usr"] = hit.node->usr;
    entry["file"] = hit.node->file;
    entry["line"] = static_cast<int64_t>(hit.node->line);
    if (!hit.node->enclosingClass.empty())
      entry["enclosingClass"] = hit.node->enclosingClass;
    if (hit.node->isVirtual)
      entry["isVirtual"] = true;
    results.push_back(llvm::json::Value(std::move(entry)));
  }

  llvm::json::Object obj;
  obj["query"] = query->str();
  obj["totalMatches"] = static_cast<int64_t>(hits.size());
  obj["returned"] = static_cast<int64_t>(results.size());
  obj["truncated"] = static_cast<int64_t>(hits.size()) > limit;
  obj["matches"] = std::move(results);
  return llvm::json::Value(std::move(obj));
}

// ============================================================================
// Tool 2: get_callees
// ============================================================================

static llvm::json::Value handleGetCallees(const llvm::json::Object &args,
                                          const ToolContext &ctx) {
  std::optional<llvm::json::Value> ambiguous;
  auto ident = resolveIdentity(args, ctx, "name", "usr", ambiguous);
  if (ambiguous)
    return std::move(*ambiguous);
  if (!ident)
    return errorResult("Missing required parameter 'name' (or 'usr')");

  EdgeFilter filter;
  if (auto err = parseEdgeFilter(args, filter))
    return errorResult(*err);

  // Query by the resolved USR: the by-name union path is never taken.
  auto edges = ctx.graph.calleesOf(*ident);
  llvm::json::Array results;
  for (const auto &e : edges) {
    if (!filter.allows(e))
      continue;
    results.push_back(edgeToJson(e));
  }

  llvm::json::Object obj;
  // Display the name the caller asked for; the precise identity rides in
  // "usr".
  auto name = args.getString("name");
  obj["function"] = name ? name->str() : *ident;
  attachUsr(obj, ctx, *ident);
  obj["calleeCount"] = static_cast<int64_t>(results.size());
  obj["callees"] = std::move(results);
  return llvm::json::Value(std::move(obj));
}

// ============================================================================
// Tool 3: get_callers
// ============================================================================

static llvm::json::Value handleGetCallers(const llvm::json::Object &args,
                                          const ToolContext &ctx) {
  std::optional<llvm::json::Value> ambiguous;
  auto ident = resolveIdentity(args, ctx, "name", "usr", ambiguous);
  if (ambiguous)
    return std::move(*ambiguous);
  if (!ident)
    return errorResult("Missing required parameter 'name' (or 'usr')");

  EdgeFilter filter;
  if (auto err = parseEdgeFilter(args, filter))
    return errorResult(*err);

  // Query by the resolved USR: the by-name union path is never taken.
  auto edges = ctx.graph.callersOf(*ident);
  llvm::json::Array results;
  for (const auto &e : edges) {
    if (!filter.allows(e))
      continue;
    results.push_back(edgeToJson(e));
  }

  llvm::json::Object obj;
  auto name = args.getString("name");
  obj["function"] = name ? name->str() : *ident;
  attachUsr(obj, ctx, *ident);
  obj["callerCount"] = static_cast<int64_t>(results.size());
  obj["callers"] = std::move(results);
  return llvm::json::Value(std::move(obj));
}

// ============================================================================
// Tool 4: find_call_chain
// ============================================================================

static llvm::json::Value handleFindCallChain(const llvm::json::Object &args,
                                             const ToolContext &ctx) {
  std::optional<llvm::json::Value> ambiguous;
  auto to = resolveIdentity(args, ctx, "to", "to_usr", ambiguous);
  if (ambiguous)
    return std::move(*ambiguous);
  if (!to)
    return errorResult("Missing required parameter 'to' (or 'to_usr')");

  int64_t maxPaths = 10;
  if (auto mp = args.getInteger("max_paths"))
    maxPaths = *mp;

  int64_t maxDepth = 20;
  if (auto md = args.getInteger("max_depth"))
    maxDepth = *md;

  // Hub cutoff: skip expanding nodes whose stored in-degree exceeds this,
  // reporting them instead. Bounds DFS work on graphs with high-fan-in
  // utility functions (loggers, allocators). 0 disables.
  int64_t maxFanIn = 1000;
  if (auto mf = args.getInteger("max_fan_in"))
    maxFanIn = std::max<int64_t>(0, *mf);

  EdgeFilter filter;
  if (auto err = parseEdgeFilter(args, filter))
    return errorResult(*err);

  // `from` is optional (absent -> entry points); the ambiguity check
  // applies only when an identity IS provided.
  std::vector<std::string> starts;
  auto from = resolveIdentity(args, ctx, "from", "from_usr", ambiguous);
  if (ambiguous)
    return std::move(*ambiguous);
  if (from) {
    starts.push_back(*from);
  } else {
    starts = ctx.entryPoints;
  }

  // Reverse DFS from target to start nodes, entirely in interned-id space:
  // no string materialization per hop and no string-keyed sets — measured
  // 5.4s -> ms-scale on a 938-TU / 345k-edge graph, where the ancestor
  // cone is large and names are long. Strings are resolved only for found
  // paths and skipped hubs. We track the edge used for every hop so the
  // response can carry kind/confidence/callSite per hop.
  using SId = StringInterner::Id;
  const auto &interner = ctx.graph.interner();

  std::set<SId> startSet;
  for (const auto &st : starts) {
    if (auto sid = interner.find(st))
      startSet.insert(*sid);
  }
  auto targetId = interner.find(*to);
  struct FoundPath {
    std::vector<SId> nodes;                     // start -> ... -> target
    std::vector<CallGraph::EdgeRef> edges;      // edges[i]: nodes[i] -> nodes[i+1]
  };
  std::vector<FoundPath> foundPaths;
  std::vector<SId> currentPath;                 // target -> ... -> start
  std::vector<CallGraph::EdgeRef> currentEdges; // parallel to currentPath
  std::unordered_set<SId> onPath;
  std::map<SId, size_t> skippedHubs; // id -> stored in-degree

  // Dead-end memo. Without it the simple-path enumeration re-explores the
  // whole ancestor cone once per permutation — exponential in the no-path
  // case (and in the ancestry of any near-miss). deadAt[n] = d records
  // that n's ancestry was exhaustively explored from depth d (budget
  // maxDepth - d) finding no start, with the failure independent of the
  // current path; a revisit at depth >= d has no more budget and cannot
  // succeed. Failures caused by an on-path exclusion or by the maxPaths
  // early-stop are path/search-state-dependent and are never memoized
  // (kBlocked poisons cleanness up the recursion). Budget-exhaustion
  // failures ARE memoizable: the depth comparison encodes the budget.
  // Corridor prune: one forward BFS from the start set over callee edges
  // (respecting the same edge filter) records the fewest edges from any
  // start to each node, bounded by maxDepth. The reverse DFS then only
  // expands nodes that can still complete a start->...->target path within
  // budget: minFromStart[node] + depth(node from target) <= maxDepth.
  // In the no-path case the DFS dies immediately after one O(E) BFS —
  // previously it enumerated the target's whole ancestor cone.
  std::unordered_map<SId, unsigned> minFromStart;
  {
    std::vector<SId> frontier(startSet.begin(), startSet.end());
    for (SId st : frontier)
      minFromStart.emplace(st, 0);
    unsigned dist = 0;
    while (!frontier.empty() && dist < static_cast<unsigned>(maxDepth)) {
      ++dist;
      std::vector<SId> next;
      for (SId node : frontier) {
        for (const auto &edge : ctx.graph.calleeRefsOf(node)) {
          if (!filter.allowsRef(edge))
            continue;
          if (minFromStart.emplace(edge.callee, dist).second)
            next.push_back(edge.callee);
        }
      }
      frontier = std::move(next);
    }
  }

  std::unordered_map<SId, unsigned> deadAt;
  // callerRefsOf synthesizes virtual-dispatch / deferred-return
  // expansions; a node can be visited several times at different depths,
  // so compute once per call.
  std::unordered_map<SId, std::vector<CallGraph::EdgeRef>> callersMemo;
  constexpr int kFound = 1, kBlocked = 2;

  std::function<int(SId, unsigned)> dfs = [&](SId node,
                                              unsigned depth) -> int {
    if (static_cast<int64_t>(foundPaths.size()) >= maxPaths)
      return kBlocked;
    if (depth > static_cast<unsigned>(maxDepth))
      return 0;
    auto dit = deadAt.find(node);
    if (dit != deadAt.end() && depth >= dit->second)
      return 0;

    currentPath.push_back(node);
    onPath.insert(node);
    int flags = 0;

    if (startSet.count(node)) {
      FoundPath fp;
      fp.nodes.assign(currentPath.rbegin(), currentPath.rend());
      fp.edges.assign(currentEdges.rbegin(), currentEdges.rend());
      foundPaths.push_back(std::move(fp));
      flags |= kFound;
    } else if (maxFanIn > 0 && depth > 0 &&
               ctx.graph.storedInDegree(node) >
                   static_cast<size_t>(maxFanIn)) {
      // Hub: expanding its ancestry would dominate the search. Record
      // and prune; the caller can re-run with a higher max_fan_in or
      // query the hub directly. Deterministic per node, so it does not
      // poison the dead-end memo.
      skippedHubs.emplace(node, ctx.graph.storedInDegree(node));
    } else {
      auto cit = callersMemo.find(node);
      if (cit == callersMemo.end())
        cit = callersMemo.emplace(node, ctx.graph.callerRefsOf(node)).first;
      const auto &callers = cit->second;
      for (const auto &edge : callers) {
        if (onPath.count(edge.caller)) {
          flags |= kBlocked; // path-dependent exclusion
          continue;
        }
        if (!filter.allowsRef(edge))
          continue;
        // Corridor prune: the caller must be reachable from a start with
        // enough budget left to descend back to the target.
        auto mit = minFromStart.find(edge.caller);
        if (mit == minFromStart.end() ||
            mit->second + depth + 1 > static_cast<unsigned>(maxDepth))
          continue;
        currentEdges.push_back(edge);
        flags |= dfs(edge.caller, depth + 1);
        currentEdges.pop_back();
        if (static_cast<int64_t>(foundPaths.size()) >= maxPaths) {
          flags |= kBlocked; // exploration truncated, not exhausted
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
  };

  if (targetId) {
    auto tmit = minFromStart.find(*targetId);
    if (tmit != minFromStart.end() &&
        tmit->second <= static_cast<unsigned>(maxDepth))
      dfs(*targetId, 0);
  }

  llvm::json::Array pathsJson;
  for (auto &fp : foundPaths) {
    llvm::json::Array chain;
    for (const auto &edge : fp.edges) {
      llvm::json::Object hop;
      hop["from"] = interner.resolve(edge.caller);
      hop["to"] = interner.resolve(edge.callee);
      hop["kind"] = edgeKindToString(edge.kind);
      hop["confidence"] = confidenceToString(edge.confidence);
      hop["callSite"] = interner.resolve(edge.callSite);
      if (edge.execContext != ExecutionContext::Synchronous)
        hop["executionContext"] = executionContextToString(edge.execContext);
      chain.push_back(llvm::json::Value(std::move(hop)));
    }
    pathsJson.push_back(llvm::json::Value(std::move(chain)));
  }

  llvm::json::Object obj;
  auto toName = args.getString("to");
  obj["target"] = toName ? toName->str() : *to;
  attachUsr(obj, ctx, *to, "targetUsr");
  obj["pathCount"] = static_cast<int64_t>(foundPaths.size());
  obj["paths"] = std::move(pathsJson);
  if (!skippedHubs.empty()) {
    llvm::json::Array hubs;
    for (const auto &[hubId, inDegree] : skippedHubs) {
      llvm::json::Object hub;
      hub["name"] = interner.resolve(hubId);
      hub["inDegree"] = static_cast<int64_t>(inDegree);
      hubs.push_back(llvm::json::Value(std::move(hub)));
    }
    obj["skippedHubs"] = std::move(hubs);
    obj["skippedHubsNote"] =
        "Ancestry of these high-fan-in functions was not expanded "
        "(stored in-degree exceeds max_fan_in). Re-run with a higher "
        "max_fan_in or query them directly with get_callers.";
  }
  return llvm::json::Value(std::move(obj));
}


// ============================================================================
// Tool 8: get_class_hierarchy
// ============================================================================

static llvm::json::Value
handleGetClassHierarchy(const llvm::json::Object &args,
                        const ToolContext &ctx) {
  auto className = args.getString("class_name");
  if (!className)
    return errorResult("Missing required parameter 'class_name'");

  bool transitive = false;
  if (auto t = args.getBoolean("include_transitive"))
    transitive = *t;

  bool includeOverrides = false;
  if (auto o = args.getBoolean("include_overrides"))
    includeOverrides = *o;

  auto derived = transitive ? ctx.graph.getAllDerivedClasses(className->str())
                            : ctx.graph.getDerivedClasses(className->str());

  llvm::json::Array derivedArr;
  for (auto &cls : derived)
    derivedArr.push_back(cls);

  llvm::json::Object obj;
  obj["className"] = className->str();
  obj["derivedClassCount"] = static_cast<int64_t>(derived.size());
  obj["derivedClasses"] = std::move(derivedArr);

  if (includeOverrides) {
    // Collect all virtual methods that belong to this class and show overrides.
    llvm::json::Array overridesArr;
    for (auto *node : ctx.graph.allNodes()) {
      if (node->enclosingClass != className->str())
        continue;
      if (!node->isVirtual)
        continue;
      auto overrides = ctx.graph.getOverrides(node->usr);
      if (overrides.empty())
        continue;
      llvm::json::Object methodObj;
      methodObj["baseMethod"] = node->qualifiedName;
      llvm::json::Array ovArr;
      for (auto &ov : overrides)
        ovArr.push_back(ov);
      methodObj["overrides"] = std::move(ovArr);
      overridesArr.push_back(llvm::json::Value(std::move(methodObj)));
    }
    obj["virtualMethodOverrides"] = std::move(overridesArr);
  }

  return llvm::json::Value(std::move(obj));
}

// ============================================================================
// Tool 9: list_entry_points
// ============================================================================

static llvm::json::Value
handleListEntryPoints(const llvm::json::Object & /*args*/,
                      const ToolContext &ctx) {
  llvm::json::Array entries;
  for (auto &ep : ctx.entryPoints) {
    llvm::json::Object entry;
    entry["name"] = ep;
    if (auto *node = ctx.graph.findNode(ep)) {
      entry["file"] = node->file;
      entry["line"] = static_cast<int64_t>(node->line);
      if (!node->enclosingClass.empty())
        entry["enclosingClass"] = node->enclosingClass;
    }
    entries.push_back(llvm::json::Value(std::move(entry)));
  }

  llvm::json::Object obj;
  obj["count"] = static_cast<int64_t>(entries.size());
  obj["entryPoints"] = std::move(entries);
  return llvm::json::Value(std::move(obj));
}

// ============================================================================
// Tool 10: graph_summary
// ============================================================================

static llvm::json::Value
handleGraphSummary(const llvm::json::Object & /*args*/,
                   const ToolContext &ctx) {
  // Whole-graph scan (calleesOf materialized for every node); the result
  // only changes when the graph does, so serve it from the cache.
  if (ctx.cache) {
    auto it = ctx.cache->byKey.find("graph_summary");
    if (it != ctx.cache->byKey.end())
      return it->second;
  }
  size_t totalEdges = 0;
  std::unordered_map<Confidence, size_t> confHist;
  std::unordered_map<EdgeKind, size_t> kindHist;
  std::vector<std::pair<std::string, size_t>> callerFanout;
  std::unordered_map<std::string, size_t> calleeInDegree;

  for (auto *node : ctx.graph.allNodes()) {
    // Per-node scan queries by usr: exact identity, so two nodes sharing a
    // display name are not double-counted through the by-name union.
    auto edges = ctx.graph.calleesOf(node->usr);
    if (!edges.empty())
      callerFanout.emplace_back(node->qualifiedName, edges.size());
    for (const auto &e : edges) {
      ++totalEdges;
      ++confHist[e.confidence];
      ++kindHist[e.kind];
      ++calleeInDegree[e.calleeName];
    }
  }

  auto topN = [](std::vector<std::pair<std::string, size_t>> v,
                 size_t n) -> llvm::json::Array {
    // Count descending, then name: ties must not depend on map order
    // (cold and warm-refreshed indexes iterate differently).
    std::sort(v.begin(), v.end(), [](const auto &a, const auto &b) {
      return a.second != b.second ? a.second > b.second : a.first < b.first;
    });
    if (v.size() > n)
      v.resize(n);
    llvm::json::Array out;
    for (auto &p : v) {
      llvm::json::Object e;
      e["qualifiedName"] = p.first;
      e["count"] = static_cast<int64_t>(p.second);
      out.push_back(llvm::json::Value(std::move(e)));
    }
    return out;
  };

  std::vector<std::pair<std::string, size_t>> calleeInVec(
      calleeInDegree.begin(), calleeInDegree.end());

  llvm::json::Object conf;
  conf["Proven"] = static_cast<int64_t>(confHist[Confidence::Proven]);
  conf["Plausible"] = static_cast<int64_t>(confHist[Confidence::Plausible]);
  conf["Unknown"] = static_cast<int64_t>(confHist[Confidence::Unknown]);

  llvm::json::Object kinds;
  for (auto kind :
       {EdgeKind::DirectCall, EdgeKind::VirtualDispatch,
        EdgeKind::FunctionPointer, EdgeKind::ConstructorCall,
        EdgeKind::DestructorCall, EdgeKind::OperatorCall,
        EdgeKind::TemplateInstantiation, EdgeKind::LambdaCall,
        EdgeKind::ThreadEntry}) {
    kinds[edgeKindToString(kind)] = static_cast<int64_t>(kindHist[kind]);
  }

  llvm::json::Object obj;
  obj["nodeCount"] = static_cast<int64_t>(ctx.graph.nodeCount());
  obj["edgeCount"] = static_cast<int64_t>(totalEdges);
  // One-shot verbs load the graph section only; the header count stands
  // in for the undecoded control-flow index.
  obj["callSiteCount"] = static_cast<int64_t>(
      ctx.summary ? ctx.summary->callSites : ctx.cfIndex.size());
  obj["entryPointCount"] = static_cast<int64_t>(ctx.entryPoints.size());
  obj["confidenceHistogram"] = llvm::json::Value(std::move(conf));
  obj["edgeKindHistogram"] = llvm::json::Value(std::move(kinds));
  obj["topFanoutCallers"] = topN(std::move(callerFanout), 5);
  obj["topFanoutCallees"] = topN(std::move(calleeInVec), 5);
  auto out = llvm::json::Value(std::move(obj));
  if (ctx.cache)
    ctx.cache->byKey.insert_or_assign("graph_summary", out);
  return out;
}

// ============================================================================
// Tool 11: list_callback_sites
// ============================================================================

static llvm::json::Value
handleListCallbackSites(const llvm::json::Object &args,
                        const ToolContext &ctx) {
  auto targetFilter = args.getString("target_prefix");

  // Group callback-like edges by calleeName (copies — calleesOf returns a
  // temporary vector per node).
  std::map<std::string, std::vector<CallGraphEdge>> byTarget;
  for (auto *node : ctx.graph.allNodes()) {
    for (const auto &e : ctx.graph.calleesOf(node->usr)) {
      if (e.kind != EdgeKind::FunctionPointer &&
          e.kind != EdgeKind::LambdaCall)
        continue;
      if (targetFilter && !llvm::StringRef(e.calleeName)
                               .starts_with(*targetFilter))
        continue;
      byTarget[e.calleeName].push_back(e);
    }
  }

  llvm::json::Array targets;
  for (auto &kv : byTarget) {
    llvm::json::Array sites;
    for (const auto &e : kv.second) {
      llvm::json::Object site;
      site["caller"] = e.callerName;
      site["callSite"] = e.callSite;
      site["kind"] = edgeKindToString(e.kind);
      site["confidence"] = confidenceToString(e.confidence);
      if (e.indirectionDepth > 0)
        site["indirectionDepth"] = static_cast<int64_t>(e.indirectionDepth);
      if (e.execContext != ExecutionContext::Synchronous)
        site["executionContext"] = executionContextToString(e.execContext);
      sites.push_back(llvm::json::Value(std::move(site)));
    }
    llvm::json::Object entry;
    entry["target"] = kv.first;
    entry["siteCount"] = static_cast<int64_t>(kv.second.size());
    entry["sites"] = std::move(sites);
    targets.push_back(llvm::json::Value(std::move(entry)));
  }

  llvm::json::Object obj;
  obj["targetCount"] = static_cast<int64_t>(targets.size());
  obj["targets"] = std::move(targets);
  return llvm::json::Value(std::move(obj));
}

// ============================================================================
// Tool 12: list_concurrency_entry_points
// ============================================================================

static llvm::json::Value
handleListConcurrencyEntryPoints(const llvm::json::Object &args,
                                 const ToolContext &ctx) {
  std::set<ExecutionContext> ctxFilter;
  if (auto *arr = args.getArray("execution_contexts")) {
    for (auto &v : *arr) {
      auto s = v.getAsString();
      if (!s)
        continue;
      auto parsed = parseExecutionContext(*s);
      if (!parsed) {
        return errorResult(
            "Invalid value in execution_contexts: '" + s->str() +
            "' (expected Synchronous, ThreadSpawn, AsyncTask, "
            "PackagedTask, or Invoke)");
      }
      ctxFilter.insert(*parsed);
    }
  }

  llvm::json::Array entries;
  size_t total = 0;
  for (auto *node : ctx.graph.allNodes()) {
    for (const auto &e : ctx.graph.calleesOf(node->usr)) {
      if (e.kind != EdgeKind::ThreadEntry)
        continue;
      if (!ctxFilter.empty() && !ctxFilter.count(e.execContext))
        continue;
      ++total;
      llvm::json::Object entry;
      entry["spawner"] = e.callerName;
      entry["target"] = e.calleeName;
      entry["executionContext"] =
          executionContextToString(e.execContext);
      entry["callSite"] = e.callSite;
      entry["confidence"] = confidenceToString(e.confidence);
      entries.push_back(llvm::json::Value(std::move(entry)));
    }
  }

  llvm::json::Object obj;
  obj["count"] = static_cast<int64_t>(total);
  obj["entries"] = std::move(entries);
  return llvm::json::Value(std::move(obj));
}

void registerGraphTools(std::vector<ToolEntry> &tools) {
  // 1. lookup_function
  {
    llvm::json::Object props;
    props["name"] = stringProp(
        "Qualified function name (e.g. 'MyClass::process'). Provide 'name' "
        "or 'usr' (usr wins when both are present).");
    props["usr"] = stringProp(
        "Exact function USR (from search_functions results or a prior "
        "disambiguation response). Bypasses name resolution entirely — use "
        "it to pick one overload/specialization when a name is ambiguous.");
    addIdentityRefinementProps(props, "");
    llvm::json::Object schema;
    schema["type"] = "object";
    schema["properties"] = std::move(props);

    tools.push_back({"lookup_function",
                     "Look up metadata for a function by qualified name. "
                     "Returns file, line, class membership, and virtual "
                     "status. If several functions share the name "
                     "(overloads, template specializations), returns "
                     "{ambiguous:true, candidates:[...]} — re-query with the "
                     "'usr' of the intended candidate.",
                     llvm::json::Value(std::move(schema)),
                     handleLookupFunction});
  }

  // 2. get_callees
  {
    llvm::json::Object props;
    props["name"] = stringProp(
        "Qualified name of the caller function. Provide 'name' or 'usr' "
        "(usr wins when both are present).");
    props["usr"] = stringProp(
        "Exact function USR of the caller. Bypasses name resolution — use "
        "it to pick one overload/specialization when the name is "
        "ambiguous.");
    addIdentityRefinementProps(props, "");
    props["edge_kinds"] = stringArrayProp(
        "Filter by edge kind: DirectCall, VirtualDispatch, FunctionPointer, "
        "ConstructorCall, DestructorCall, OperatorCall, TemplateInstantiation, "
        "LambdaCall, ThreadEntry");
    props["min_confidence"] = stringProp(
        "Inclusive minimum confidence tier: Proven, Plausible, or Unknown "
        "(default: Unknown). Plausible includes both Plausible and Proven "
        "edges. Use include_confidences to select exact tiers.");
    props["include_confidences"] = stringArrayProp(
        "Explicit set of confidence tiers to include (e.g. [\"Plausible\"] "
        "returns only Plausible edges). Overrides min_confidence.");
    props["execution_contexts"] = stringArrayProp(
        "Filter by execution context: Synchronous, ThreadSpawn, AsyncTask, "
        "PackagedTask, Invoke. Default: all contexts.");
    llvm::json::Object schema;
    schema["type"] = "object";
    schema["properties"] = std::move(props);

    tools.push_back({"get_callees",
                     "List all functions called by a given function. "
                     "Supports filtering by edge kind and confidence level. "
                     "An ambiguous name returns {ambiguous:true, "
                     "candidates:[...]} — re-query with 'usr'.",
                     llvm::json::Value(std::move(schema)),
                     handleGetCallees});
  }

  // 3. get_callers
  {
    llvm::json::Object props;
    props["name"] = stringProp(
        "Qualified name of the callee function. Provide 'name' or 'usr' "
        "(usr wins when both are present).");
    props["usr"] = stringProp(
        "Exact function USR of the callee. Bypasses name resolution — use "
        "it to pick one overload/specialization when the name is "
        "ambiguous.");
    addIdentityRefinementProps(props, "");
    props["edge_kinds"] = stringArrayProp(
        "Filter by edge kind: DirectCall, VirtualDispatch, FunctionPointer, "
        "ConstructorCall, DestructorCall, OperatorCall, TemplateInstantiation, "
        "LambdaCall, ThreadEntry");
    props["min_confidence"] = stringProp(
        "Inclusive minimum confidence tier: Proven, Plausible, or Unknown "
        "(default: Unknown). Plausible includes both Plausible and Proven "
        "edges. Use include_confidences to select exact tiers.");
    props["include_confidences"] = stringArrayProp(
        "Explicit set of confidence tiers to include. Overrides min_confidence.");
    props["execution_contexts"] = stringArrayProp(
        "Filter by execution context: Synchronous, ThreadSpawn, AsyncTask, "
        "PackagedTask, Invoke. Default: all contexts.");
    llvm::json::Object schema;
    schema["type"] = "object";
    schema["properties"] = std::move(props);

    tools.push_back({"get_callers",
                     "List all functions that call a given function. "
                     "Supports filtering by edge kind and confidence level. "
                     "An ambiguous name returns {ambiguous:true, "
                     "candidates:[...]} — re-query with 'usr'.",
                     llvm::json::Value(std::move(schema)),
                     handleGetCallers});
  }

  // 4. find_call_chain
  {
    llvm::json::Object props;
    props["from"] = stringProp(
        "Source function qualified name (omit to use entry points)");
    props["from_usr"] = stringProp(
        "Exact USR of the source function. Bypasses name resolution for "
        "'from' when the name is ambiguous.");
    addIdentityRefinementProps(props, "from_");
    props["to"] = stringProp(
        "Target function qualified name. Provide 'to' or 'to_usr' (to_usr "
        "wins when both are present).");
    props["to_usr"] = stringProp(
        "Exact USR of the target function. Bypasses name resolution for "
        "'to' when the name is ambiguous.");
    addIdentityRefinementProps(props, "to_");
    props["max_paths"] =
        intProp("Maximum number of paths to return (default: 10)");
    props["max_depth"] = intProp(
        "Maximum number of edges in a chain, i.e. node count minus one "
        "(default: 20)");
    props["edge_kinds"] = stringArrayProp(
        "Prune hops whose edge kind is not in this set.");
    props["min_confidence"] = stringProp(
        "Inclusive minimum confidence tier applied to every hop on the "
        "path (default: Unknown).");
    props["include_confidences"] = stringArrayProp(
        "Explicit set of confidence tiers allowed at every hop. Overrides "
        "min_confidence.");
    props["max_fan_in"] = intProp(
        "Skip expanding the ancestry of functions with more stored callers "
        "than this (high-fan-in hubs like loggers); skipped hubs are listed "
        "in the response. 0 disables the cutoff (default: 1000).");
    llvm::json::Object schema;
    schema["type"] = "object";
    schema["properties"] = std::move(props);

    tools.push_back({"find_call_chain",
                     "Find call chains from a source function (or entry points) "
                     "to a target function. Each path is an array of hop "
                     "objects with {from, to, kind, confidence, callSite, "
                     "executionContext?}. executionContext is only emitted on "
                     "ThreadEntry hops and other non-Synchronous edges. An "
                     "ambiguous 'from' or 'to' name returns {ambiguous:true, "
                     "candidates:[...]} — re-query with the *_usr parameter.",
                     llvm::json::Value(std::move(schema)),
                     handleFindCallChain});
  }

  // 4a. search_functions
  {
    llvm::json::Object props;
    props["query"] = stringProp(
        "Case-insensitive substring to match against qualified function "
        "names (e.g. 'execute' or 'TransformPipeline').");
    props["limit"] = intProp("Maximum matches to return (default: 25)");
    llvm::json::Array req;
    req.push_back("query");
    llvm::json::Object schema;
    schema["type"] = "object";
    schema["properties"] = std::move(props);
    schema["required"] = std::move(req);

    tools.push_back({"search_functions",
                     "Find functions by name substring when the exact "
                     "qualified name is unknown. Returns candidates ranked "
                     "by match quality (exact unqualified name, then prefix, "
                     "then substring). Use this before lookup_function / "
                     "get_callers when unsure of the precise name.",
                     llvm::json::Value(std::move(schema)),
                     handleSearchFunctions});
  }

  // 8. get_class_hierarchy
  {
    llvm::json::Object props;
    props["class_name"] = stringProp("Qualified class name");
    props["include_transitive"] = boolProp(
        "Include all descendants, not just direct (default: false)");
    props["include_overrides"] = boolProp(
        "Include virtual method override info (default: false)");
    llvm::json::Array req;
    req.push_back("class_name");
    llvm::json::Object schema;
    schema["type"] = "object";
    schema["properties"] = std::move(props);
    schema["required"] = std::move(req);

    tools.push_back({"get_class_hierarchy",
                     "Query class inheritance relationships and virtual method "
                     "overrides. Shows derived classes and optionally which "
                     "methods are overridden in each.",
                     llvm::json::Value(std::move(schema)),
                     handleGetClassHierarchy});
  }

  // 9. list_entry_points
  {
    llvm::json::Object schema;
    schema["type"] = "object";
    schema["properties"] = llvm::json::Object{};

    tools.push_back({"list_entry_points",
                     "List the configured entry-point functions with their "
                     "file/line when resolved in the call graph. Useful for "
                     "orientation before calling find_call_chain or "
                     "analyze_dead_code.",
                     llvm::json::Value(std::move(schema)),
                     handleListEntryPoints});
  }

  // 10. graph_summary
  {
    llvm::json::Object schema;
    schema["type"] = "object";
    schema["properties"] = llvm::json::Object{};

    tools.push_back({"graph_summary",
                     "Return aggregate statistics about the call graph: node "
                     "and edge counts, call-site count, entry-point count, "
                     "top-5 fan-out callers and callees, and histograms by "
                     "confidence and edge kind.",
                     llvm::json::Value(std::move(schema)),
                     handleGraphSummary});
  }

  // 11. list_callback_sites
  {
    llvm::json::Object props;
    props["target_prefix"] = stringProp(
        "Optional qualified-name prefix; only targets whose name starts "
        "with this prefix are returned.");
    llvm::json::Object schema;
    schema["type"] = "object";
    schema["properties"] = std::move(props);

    tools.push_back({"list_callback_sites",
                     "List every callback registration or invocation site "
                     "grouped by target. Covers FunctionPointer and "
                     "LambdaCall edges, including synthetic lambda "
                     "targets named 'lambda#file:line:col#enclosing'. "
                     "Returns {target, siteCount, sites:[{caller, callSite, "
                     "kind, confidence, indirectionDepth?, "
                     "executionContext?}]}.",
                     llvm::json::Value(std::move(schema)),
                     handleListCallbackSites});
  }

  // 12. list_concurrency_entry_points
  {
    llvm::json::Object props;
    props["execution_contexts"] = stringArrayProp(
        "Filter by execution context: ThreadSpawn, AsyncTask, "
        "PackagedTask, Invoke. Default: all ThreadEntry contexts.");
    llvm::json::Object schema;
    schema["type"] = "object";
    schema["properties"] = std::move(props);

    tools.push_back({"list_concurrency_entry_points",
                     "List every ThreadEntry edge: functions (including "
                     "synthetic lambda targets) that are handed to "
                     "std::thread, std::jthread, std::async, "
                     "std::packaged_task, std::invoke, or std::bind. "
                     "Returns {count, entries:[{spawner, target, "
                     "executionContext, callSite, confidence}]}.",
                     llvm::json::Value(std::move(schema)),
                     handleListConcurrencyEntryPoints});
  }
}

} // namespace vycor
