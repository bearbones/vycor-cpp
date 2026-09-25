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
#include "vycor/query/Limits.h"
#include "vycor/query/Tools.h"
#include "vycor/query/Identity.h"
#include "vycor/query/Serialize.h"
#include "EdgeFilter.h"
#include "Paging.h"
#include "Registry.h"
#include "Schema.h"

#include "llvm/ADT/StringRef.h"

#include <algorithm>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace vycor {

/// allNodes() walks a hash map; every list a tool derives from it is
/// emitted in usr order (docs/deterministic-output.md).
static std::vector<const CallGraphNode *> sortedNodes(const CallGraph &g) {
  auto nodes = g.allNodes();
  std::sort(nodes.begin(), nodes.end(),
            [](const CallGraphNode *a, const CallGraphNode *b) {
              return a->usr < b->usr;
            });
  return nodes;
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
    return usageError("Missing required parameter 'name' (or 'usr')");

  auto *node = ctx.graph.findNode(*ident);
  if (!node)
    return notFoundError("Function not found: " + *ident);

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
    return usageError("Missing required parameter 'query'");

  Page page;
  if (auto err = parsePage(args, kDefaultSearchLimit, page))
    return usageError(*err);

  auto hits = rankFunctionMatches(ctx, *query);
  llvm::json::Array results;
  for (size_t i = page.begin(hits.size()); i < page.end(hits.size()); ++i) {
    const CallGraphNode *node = hits[i];
    llvm::json::Object entry;
    entry["qualifiedName"] = node->qualifiedName;
    entry["usr"] = node->usr;
    entry["file"] = node->file;
    entry["line"] = static_cast<int64_t>(node->line);
    if (!node->enclosingClass.empty())
      entry["enclosingClass"] = node->enclosingClass;
    if (node->isVirtual)
      entry["isVirtual"] = true;
    results.push_back(llvm::json::Value(std::move(entry)));
  }

  llvm::json::Object obj;
  obj["query"] = query->str();
  obj["totalMatches"] = static_cast<int64_t>(hits.size());
  attachPage(obj, page, hits.size());
  obj["matches"] = std::move(results);
  return llvm::json::Value(std::move(obj));
}

// ============================================================================
// Tools 2-3: get_callees / get_callers
// ============================================================================

// The shared body of get_callees and get_callers: resolve, filter, sort
// canonically, optionally collapse to one record per function at the
// other end (`distinct`, with `siteCount`), then page.
static llvm::json::Value handleEdgeList(const llvm::json::Object &args,
                                        const ToolContext &ctx,
                                        bool callees) {
  std::optional<llvm::json::Value> ambiguous;
  auto ident = resolveIdentity(args, ctx, "name", "usr", ambiguous);
  if (ambiguous)
    return std::move(*ambiguous);
  if (!ident)
    return usageError("Missing required parameter 'name' (or 'usr')");

  EdgeFilter filter;
  if (auto err = parseEdgeFilter(args, filter))
    return usageError(*err);
  Page page;
  if (auto err = parsePage(args, kDefaultEdgeListLimit, page))
    return usageError(*err);
  bool distinct = false;
  if (auto d = args.getBoolean("distinct"))
    distinct = *d;

  // Query by the resolved USR: the by-name union path is never taken.
  auto edges = callees ? ctx.graph.calleesOf(*ident)
                       : ctx.graph.callersOf(*ident);
  // Canonical order (other end's usr, call site, ...): storage order
  // follows the bake's TU order (docs/deterministic-output.md).
  sortEdgesCanonically(edges);
  std::vector<const CallGraphEdge *> kept;
  for (const auto &e : edges)
    if (filter.allows(e))
      kept.push_back(&e);

  // Distinct: the first site in canonical order stands for the function;
  // the rest of its sites are counted. The other end's usr is the sort's
  // leading key, so each function's sites are contiguous.
  std::vector<size_t> siteCounts;
  if (distinct) {
    std::vector<const CallGraphEdge *> firsts;
    auto otherEnd = [&](const CallGraphEdge *e) -> const std::string & {
      return callees ? e->calleeUsr : e->callerUsr;
    };
    for (const auto *e : kept) {
      if (!firsts.empty() && otherEnd(firsts.back()) == otherEnd(e)) {
        ++siteCounts.back();
        continue;
      }
      firsts.push_back(e);
      siteCounts.push_back(1);
    }
    kept = std::move(firsts);
  }

  llvm::json::Array results;
  for (size_t i = page.begin(kept.size()); i < page.end(kept.size()); ++i) {
    auto record = edgeToJson(*kept[i]);
    if (distinct)
      (*record.getAsObject())["siteCount"] =
          static_cast<int64_t>(siteCounts[i]);
    results.push_back(std::move(record));
  }

  llvm::json::Object obj;
  // Display the name the caller asked for; the precise identity rides in
  // "usr".
  auto name = identityName(args, "name");
  obj["function"] = name ? name->str() : *ident;
  attachUsr(obj, ctx, *ident);
  if (distinct)
    obj["distinct"] = true;
  // The count is the whole filtered list (records, or functions under
  // distinct), not the page.
  obj[callees ? "calleeCount" : "callerCount"] =
      static_cast<int64_t>(kept.size());
  attachPage(obj, page, kept.size());
  obj[callees ? "callees" : "callers"] = std::move(results);
  return llvm::json::Value(std::move(obj));
}

static llvm::json::Value handleGetCallees(const llvm::json::Object &args,
                                          const ToolContext &ctx) {
  return handleEdgeList(args, ctx, /*callees=*/true);
}

static llvm::json::Value handleGetCallers(const llvm::json::Object &args,
                                          const ToolContext &ctx) {
  return handleEdgeList(args, ctx, /*callees=*/false);
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
    return usageError("Missing required parameter 'to' (or 'to_usr')");

  int64_t maxPaths = 10;
  if (auto err = readLimit(args, "max_paths", 1, kMaxSearchPaths,
                           BelowMin::Reject, maxPaths))
    return usageError(*err);

  int64_t maxDepth = 20;
  if (auto err = readLimit(args, "max_depth", 1, kMaxSearchDepth,
                           BelowMin::Reject, maxDepth))
    return usageError(*err);

  // Hub cutoff: skip expanding nodes whose stored in-degree exceeds this,
  // reporting them instead. Bounds DFS work on graphs with high-fan-in
  // utility functions (loggers, allocators). 0 disables.
  int64_t maxFanIn = 1000;
  readLimit(args, "max_fan_in", 0, kMaxFanInLimit, BelowMin::Clamp, maxFanIn);

  EdgeFilter filter;
  if (auto err = parseEdgeFilter(args, filter))
    return usageError(*err);

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

  // The shared bounded reverse search (callgraph/PathSearch.h): every
  // hop carries its exact edge, the paths come in canonical order, and
  // the result says why the enumeration stopped.
  SearchLimits limits;
  limits.maxPaths = static_cast<unsigned>(maxPaths);
  limits.maxDepth = static_cast<unsigned>(maxDepth);
  limits.maxFanIn = static_cast<size_t>(maxFanIn);
  auto search = findCallerPaths(
      ctx.graph, *to, starts, limits, CycleRule::SimpleNodes,
      [&](const CallGraph::EdgeRef &e) { return filter.allowsRef(e); });

  llvm::json::Array pathsJson;
  for (const auto &path : search.paths) {
    llvm::json::Array chain;
    for (const auto &hop : path.hops) {
      // from/to stay USR strings (the historical shape); the display
      // names ride in fromName/toName.
      llvm::json::Object h;
      h["from"] = hop.callerUsr;
      h["to"] = hop.calleeUsr;
      h["fromName"] = hop.caller;
      h["toName"] = hop.callee;
      h["kind"] = edgeKindToString(hop.kind);
      h["confidence"] = confidenceToString(hop.confidence);
      h["callSite"] = hop.callSite;
      if (hop.execContext != ExecutionContext::Synchronous)
        h["executionContext"] = executionContextToString(hop.execContext);
      chain.push_back(llvm::json::Value(std::move(h)));
    }
    pathsJson.push_back(llvm::json::Value(std::move(chain)));
  }

  llvm::json::Object obj;
  auto toName = args.getString("to");
  obj["target"] = toName ? toName->str() : *to;
  attachUsr(obj, ctx, *to, "targetUsr");
  obj["pathCount"] = static_cast<int64_t>(search.paths.size());
  obj["paths"] = std::move(pathsJson);
  attachSearchFacts(obj, search.stops, search.complete(),
                    search.exhaustive(), search.skippedHubs);
  if (!search.skippedHubs.empty()) {
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
    return usageError("Missing required parameter 'class_name'");

  bool transitive = false;
  if (auto t = args.getBoolean("include_transitive"))
    transitive = *t;

  bool includeOverrides = false;
  if (auto o = args.getBoolean("include_overrides"))
    includeOverrides = *o;

  Page page;
  if (auto err = parsePage(args, kDefaultListLimit, page))
    return usageError(*err);

  auto derived = transitive ? ctx.graph.getAllDerivedClasses(className->str())
                            : ctx.graph.getDerivedClasses(className->str());
  // Name order: the hierarchy maps follow insertion order.
  std::sort(derived.begin(), derived.end());

  llvm::json::Array derivedArr;
  for (size_t i = page.begin(derived.size()); i < page.end(derived.size());
       ++i)
    derivedArr.push_back(derived[i]);

  llvm::json::Object obj;
  obj["className"] = className->str();
  obj["derivedClassCount"] = static_cast<int64_t>(derived.size());
  attachPage(obj, page, derived.size());
  obj["derivedClasses"] = std::move(derivedArr);

  if (includeOverrides) {
    // Collect all virtual methods that belong to this class and show
    // overrides, in usr order (allNodes iterates a hash map).
    llvm::json::Array overridesArr;
    for (auto *node : sortedNodes(ctx.graph)) {
      if (node->enclosingClass != className->str())
        continue;
      if (!node->isVirtual)
        continue;
      auto overrides = ctx.graph.getOverrides(node->usr);
      if (overrides.empty())
        continue;
      std::sort(overrides.begin(), overrides.end());
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
handleListEntryPoints(const llvm::json::Object &args,
                      const ToolContext &ctx) {
  Page page;
  if (auto err = parsePage(args, kDefaultListLimit, page))
    return usageError(*err);
  const size_t total = ctx.entryPoints.size();
  llvm::json::Array entries;
  for (size_t i = page.begin(total); i < page.end(total); ++i) {
    const std::string &ep = ctx.entryPoints[i];
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
  obj["count"] = static_cast<int64_t>(total);
  attachPage(obj, page, total);
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
  Page page;
  if (auto err = parsePage(args, kDefaultCallbackTargetLimit, page))
    return usageError(*err);
  size_t siteLimit = kDefaultSitesPerTarget;
  if (auto sl = args.getInteger("site_limit")) {
    if (*sl < 1)
      return usageError("Invalid site_limit: must be at least 1");
    siteLimit = static_cast<size_t>(*sl);
  }

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

  // Targets are in name order (std::map); the page is a window of them.
  const size_t total = byTarget.size();
  auto first = byTarget.begin();
  std::advance(first, page.begin(total));
  auto last = first;
  std::advance(last, page.end(total) - page.begin(total));
  llvm::json::Array targets;
  for (auto it = first; it != last; ++it) {
    auto &kv = *it;
    // Sites within a target in canonical edge order rather than the
    // hash-map walk's, cut at site_limit.
    sortEdgesCanonically(kv.second);
    llvm::json::Array sites;
    for (const auto &e : kv.second) {
      if (sites.size() >= siteLimit)
        break;
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
    entry["sitesTruncated"] = kv.second.size() > siteLimit;
    entry["sites"] = std::move(sites);
    targets.push_back(llvm::json::Value(std::move(entry)));
  }

  llvm::json::Object obj;
  obj["targetCount"] = static_cast<int64_t>(total);
  obj["siteLimit"] = static_cast<int64_t>(siteLimit);
  attachPage(obj, page, total);
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
        return usageError(
            "Invalid value in execution_contexts: '" + s->str() +
            "' (expected Synchronous, ThreadSpawn, AsyncTask, "
            "PackagedTask, or Invoke)");
      }
      ctxFilter.insert(*parsed);
    }
  }

  Page page;
  if (auto err = parsePage(args, kDefaultListLimit, page))
    return usageError(*err);

  std::vector<CallGraphEdge> spawns;
  for (auto *node : ctx.graph.allNodes()) {
    for (const auto &e : ctx.graph.calleesOf(node->usr)) {
      if (e.kind != EdgeKind::ThreadEntry)
        continue;
      if (!ctxFilter.empty() && !ctxFilter.count(e.execContext))
        continue;
      spawns.push_back(e);
    }
  }
  // Canonical edge order (spawner usr, target usr, call site, ...): the
  // node walk above is a hash-map walk.
  sortEdgesCanonically(spawns);
  llvm::json::Array entries;
  for (size_t i = page.begin(spawns.size()); i < page.end(spawns.size());
       ++i) {
    const CallGraphEdge &e = spawns[i];
    llvm::json::Object entry;
    entry["spawner"] = e.callerName;
    entry["target"] = e.calleeName;
    entry["executionContext"] = executionContextToString(e.execContext);
    entry["callSite"] = e.callSite;
    entry["confidence"] = confidenceToString(e.confidence);
    entries.push_back(llvm::json::Value(std::move(entry)));
  }

  llvm::json::Object obj;
  obj["count"] = static_cast<int64_t>(spawns.size());
  attachPage(obj, page, spawns.size());
  obj["entries"] = std::move(entries);
  return llvm::json::Value(std::move(obj));
}

void registerGraphTools(std::vector<ToolEntry> &tools) {
  // 1. lookup_function
  {
    llvm::json::Object props;
    props["name"] = stringProp(
        "Qualified function name (e.g. 'MyClass::process'). Provide 'name' "
        "or 'usr' (usr wins when both are present). Alias: 'function'.");
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
        "(usr wins when both are present). Alias: 'function'.");
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
    props["distinct"] = boolProp(
        "One record per callee function instead of one per call site: the "
        "first site in canonical order, with siteCount (default: false).");
    addPagingProps(props, kDefaultEdgeListLimit, "callees");
    llvm::json::Object schema;
    schema["type"] = "object";
    schema["properties"] = std::move(props);

    tools.push_back({"get_callees",
                     "List the functions called by a given function, one "
                     "record per call site (or per callee with distinct), "
                     "paged by limit/offset. Supports filtering by edge "
                     "kind and confidence level. An ambiguous name returns "
                     "{ambiguous:true, candidates:[...]} — re-query with "
                     "'usr'; an unknown name is not_found with didYouMean "
                     "suggestions.",
                     llvm::json::Value(std::move(schema)),
                     handleGetCallees});
  }

  // 3. get_callers
  {
    llvm::json::Object props;
    props["name"] = stringProp(
        "Qualified name of the callee function. Provide 'name' or 'usr' "
        "(usr wins when both are present). Alias: 'function'.");
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
    props["distinct"] = boolProp(
        "One record per caller function instead of one per call site: the "
        "first site in canonical order, with siteCount (default: false).");
    addPagingProps(props, kDefaultEdgeListLimit, "callers");
    llvm::json::Object schema;
    schema["type"] = "object";
    schema["properties"] = std::move(props);

    tools.push_back({"get_callers",
                     "List the functions that call a given function, one "
                     "record per call site (or per caller with distinct), "
                     "paged by limit/offset. Supports filtering by edge "
                     "kind and confidence level. An ambiguous name returns "
                     "{ambiguous:true, candidates:[...]} — re-query with "
                     "'usr'; an unknown name is not_found with didYouMean "
                     "suggestions.",
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
    addPagingProps(props, kDefaultSearchLimit, "matches");
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
    addPagingProps(props, kDefaultListLimit, "derived classes");
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
    llvm::json::Object props;
    addPagingProps(props, kDefaultListLimit, "entry points");
    llvm::json::Object schema;
    schema["type"] = "object";
    schema["properties"] = std::move(props);

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
    props["site_limit"] = intProp(
        "Maximum sites listed per target, at least 1 (default: 50). "
        "siteCount stays the full count; sitesTruncated says whether the "
        "list was cut.");
    addPagingProps(props, kDefaultCallbackTargetLimit, "targets");
    llvm::json::Object schema;
    schema["type"] = "object";
    schema["properties"] = std::move(props);

    tools.push_back({"list_callback_sites",
                     "List every callback registration or invocation site "
                     "grouped by target. Covers FunctionPointer and "
                     "LambdaCall edges, including synthetic lambda "
                     "targets named 'lambda#file:line:col#enclosing'. "
                     "Returns {target, siteCount, sitesTruncated, "
                     "sites:[{caller, callSite, kind, confidence, "
                     "indirectionDepth?, executionContext?}]}, targets "
                     "paged by limit/offset.",
                     llvm::json::Value(std::move(schema)),
                     handleListCallbackSites});
  }

  // 12. list_concurrency_entry_points
  {
    llvm::json::Object props;
    props["execution_contexts"] = stringArrayProp(
        "Filter by execution context: ThreadSpawn, AsyncTask, "
        "PackagedTask, Invoke. Default: all ThreadEntry contexts.");
    addPagingProps(props, kDefaultListLimit, "entries");
    llvm::json::Object schema;
    schema["type"] = "object";
    schema["properties"] = std::move(props);

    tools.push_back({"list_concurrency_entry_points",
                     "List every ThreadEntry edge: functions (including "
                     "synthetic lambda targets) that are handed to "
                     "std::thread, std::jthread, std::async, "
                     "std::packaged_task, std::invoke, or std::bind. "
                     "Returns {count, entries:[{spawner, target, "
                     "executionContext, callSite, confidence}]}, paged by "
                     "limit/offset.",
                     llvm::json::Value(std::move(schema)),
                     handleListConcurrencyEntryPoints});
  }
}

} // namespace vycor
