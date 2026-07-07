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

#include "vycor/mcp/McpTools.h"
#include "vycor/anneal/DeadCodeAnalyzer.h"

#include "llvm/ADT/StringRef.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vycor {

// ============================================================================
// JSON helper: build an MCP tool result with text content
// ============================================================================

static llvm::json::Value makeTextResult(llvm::json::Value payload) {
  std::string text;
  llvm::raw_string_ostream os(text);
  os << payload;
  os.flush();

  llvm::json::Object content;
  content["type"] = "text";
  content["text"] = std::move(text);

  llvm::json::Array contentArr;
  contentArr.push_back(llvm::json::Value(std::move(content)));

  llvm::json::Object result;
  result["content"] = std::move(contentArr);
  return llvm::json::Value(std::move(result));
}

static llvm::json::Value makeErrorResult(llvm::StringRef message) {
  llvm::json::Object content;
  content["type"] = "text";
  content["text"] = message.str();

  llvm::json::Array contentArr;
  contentArr.push_back(llvm::json::Value(std::move(content)));

  llvm::json::Object result;
  result["content"] = std::move(contentArr);
  result["isError"] = true;
  return llvm::json::Value(std::move(result));
}

// ============================================================================
// Enum serialization helpers
// ============================================================================

static const char *edgeKindToString(EdgeKind k) {
  switch (k) {
  case EdgeKind::DirectCall:
    return "DirectCall";
  case EdgeKind::VirtualDispatch:
    return "VirtualDispatch";
  case EdgeKind::FunctionPointer:
    return "FunctionPointer";
  case EdgeKind::ConstructorCall:
    return "ConstructorCall";
  case EdgeKind::DestructorCall:
    return "DestructorCall";
  case EdgeKind::OperatorCall:
    return "OperatorCall";
  case EdgeKind::TemplateInstantiation:
    return "TemplateInstantiation";
  case EdgeKind::LambdaCall:
    return "LambdaCall";
  case EdgeKind::ThreadEntry:
    return "ThreadEntry";
  }
  return "Unknown";
}

static const char *executionContextToString(ExecutionContext c) {
  switch (c) {
  case ExecutionContext::Synchronous:
    return "Synchronous";
  case ExecutionContext::ThreadSpawn:
    return "ThreadSpawn";
  case ExecutionContext::AsyncTask:
    return "AsyncTask";
  case ExecutionContext::PackagedTask:
    return "PackagedTask";
  case ExecutionContext::Invoke:
    return "Invoke";
  }
  return "Synchronous";
}

static std::optional<ExecutionContext>
parseExecutionContext(llvm::StringRef s) {
  if (s == "Synchronous") return ExecutionContext::Synchronous;
  if (s == "ThreadSpawn") return ExecutionContext::ThreadSpawn;
  if (s == "AsyncTask") return ExecutionContext::AsyncTask;
  if (s == "PackagedTask") return ExecutionContext::PackagedTask;
  if (s == "Invoke") return ExecutionContext::Invoke;
  return std::nullopt;
}

static const char *confidenceToString(Confidence c) {
  switch (c) {
  case Confidence::Proven:
    return "Proven";
  case Confidence::Plausible:
    return "Plausible";
  case Confidence::Unknown:
    return "Unknown";
  }
  return "Unknown";
}

static EdgeKind parseEdgeKind(llvm::StringRef s) {
  if (s == "DirectCall") return EdgeKind::DirectCall;
  if (s == "VirtualDispatch") return EdgeKind::VirtualDispatch;
  if (s == "FunctionPointer") return EdgeKind::FunctionPointer;
  if (s == "ConstructorCall") return EdgeKind::ConstructorCall;
  if (s == "DestructorCall") return EdgeKind::DestructorCall;
  if (s == "OperatorCall") return EdgeKind::OperatorCall;
  if (s == "TemplateInstantiation") return EdgeKind::TemplateInstantiation;
  if (s == "LambdaCall") return EdgeKind::LambdaCall;
  if (s == "ThreadEntry") return EdgeKind::ThreadEntry;
  return EdgeKind::DirectCall; // fallback
}

static Confidence parseConfidence(llvm::StringRef s) {
  if (s == "Proven") return Confidence::Proven;
  if (s == "Plausible") return Confidence::Plausible;
  return Confidence::Unknown;
}

static int confidenceRank(Confidence c) {
  switch (c) {
  case Confidence::Proven: return 2;
  case Confidence::Plausible: return 1;
  case Confidence::Unknown: return 0;
  }
  return 0;
}

// ============================================================================
// Serialize a CallGraphEdge to JSON
// ============================================================================

static llvm::json::Value edgeToJson(const CallGraphEdge &e) {
  llvm::json::Object obj;
  obj["callerName"] = e.callerName;
  obj["calleeName"] = e.calleeName;
  obj["kind"] = edgeKindToString(e.kind);
  obj["confidence"] = confidenceToString(e.confidence);
  obj["callSite"] = e.callSite;
  if (e.indirectionDepth > 0)
    obj["indirectionDepth"] = static_cast<int64_t>(e.indirectionDepth);
  if (e.execContext != ExecutionContext::Synchronous)
    obj["executionContext"] = executionContextToString(e.execContext);
  return llvm::json::Value(std::move(obj));
}

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
// System path heuristic for analyze_dead_code filtering
// ============================================================================

static bool isSystemPath(llvm::StringRef path) {
  if (path.empty())
    return false;
  static constexpr llvm::StringLiteral prefixes[] = {
      "/usr/include/",      "/usr/lib/",
      "/usr/local/include/", "/usr/local/lib/",
      "/Library/Developer/", "/Applications/Xcode.app/",
      "/opt/homebrew/",
  };
  for (auto p : prefixes) {
    if (path.starts_with(p))
      return true;
  }
  // Compiler-internal include directories, e.g. /opt/llvm-20/lib/clang/20/include.
  if (path.contains("/lib/clang/") || path.contains("/lib/gcc/"))
    return true;
  return false;
}

// ============================================================================
// Tool 1: lookup_function
// ============================================================================

static llvm::json::Value handleLookupFunction(const llvm::json::Object &args,
                                              const McpToolContext &ctx) {
  auto name = args.getString("name");
  if (!name)
    return makeErrorResult("Missing required parameter 'name'");

  auto *node = ctx.graph.findNode(name->str());
  if (!node)
    return makeErrorResult("Function not found: " + name->str());

  llvm::json::Object obj;
  obj["qualifiedName"] = node->qualifiedName;
  obj["file"] = node->file;
  obj["line"] = static_cast<int64_t>(node->line);
  obj["isEntryPoint"] = node->isEntryPoint;
  obj["isVirtual"] = node->isVirtual;
  if (!node->enclosingClass.empty())
    obj["enclosingClass"] = node->enclosingClass;
  return makeTextResult(llvm::json::Value(std::move(obj)));
}

// ============================================================================
// Tool 1a: search_functions
// ============================================================================

static llvm::json::Value
handleSearchFunctions(const llvm::json::Object &args,
                      const McpToolContext &ctx) {
  auto query = args.getString("query");
  if (!query || query->empty())
    return makeErrorResult("Missing required parameter 'query'");

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
  return makeTextResult(llvm::json::Value(std::move(obj)));
}

// ============================================================================
// Tool 2: get_callees
// ============================================================================

static llvm::json::Value handleGetCallees(const llvm::json::Object &args,
                                          const McpToolContext &ctx) {
  auto name = args.getString("name");
  if (!name)
    return makeErrorResult("Missing required parameter 'name'");

  EdgeFilter filter;
  if (auto err = parseEdgeFilter(args, filter))
    return makeErrorResult(*err);

  auto edges = ctx.graph.calleesOf(name->str());
  llvm::json::Array results;
  for (const auto &e : edges) {
    if (!filter.allows(e))
      continue;
    results.push_back(edgeToJson(e));
  }

  llvm::json::Object obj;
  obj["function"] = name->str();
  obj["calleeCount"] = static_cast<int64_t>(results.size());
  obj["callees"] = std::move(results);
  return makeTextResult(llvm::json::Value(std::move(obj)));
}

// ============================================================================
// Tool 3: get_callers
// ============================================================================

static llvm::json::Value handleGetCallers(const llvm::json::Object &args,
                                          const McpToolContext &ctx) {
  auto name = args.getString("name");
  if (!name)
    return makeErrorResult("Missing required parameter 'name'");

  EdgeFilter filter;
  if (auto err = parseEdgeFilter(args, filter))
    return makeErrorResult(*err);

  auto edges = ctx.graph.callersOf(name->str());
  llvm::json::Array results;
  for (const auto &e : edges) {
    if (!filter.allows(e))
      continue;
    results.push_back(edgeToJson(e));
  }

  llvm::json::Object obj;
  obj["function"] = name->str();
  obj["callerCount"] = static_cast<int64_t>(results.size());
  obj["callers"] = std::move(results);
  return makeTextResult(llvm::json::Value(std::move(obj)));
}

// ============================================================================
// Tool 4: find_call_chain
// ============================================================================

static llvm::json::Value handleFindCallChain(const llvm::json::Object &args,
                                             const McpToolContext &ctx) {
  auto to = args.getString("to");
  if (!to)
    return makeErrorResult("Missing required parameter 'to'");

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
    return makeErrorResult(*err);

  std::vector<std::string> starts;
  if (auto from = args.getString("from")) {
    starts.push_back(from->str());
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
  auto targetId = interner.find(to->str());
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
  obj["target"] = to->str();
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
  return makeTextResult(llvm::json::Value(std::move(obj)));
}

// ============================================================================
// Tool 5: query_exception_safety
// ============================================================================

static const char *protectionToStr(Protection p) {
  switch (p) {
  case Protection::AlwaysCaught: return "always_caught";
  case Protection::SometimesCaught: return "sometimes_caught";
  case Protection::NeverCaught: return "never_caught";
  case Protection::NoexceptBarrier: return "noexcept_barrier";
  case Protection::Unknown: return "unknown";
  }
  return "unknown";
}

static llvm::json::Value
handleQueryExceptionSafety(const llvm::json::Object &args,
                           const McpToolContext &ctx) {
  auto function = args.getString("function");
  if (!function)
    return makeErrorResult("Missing required parameter 'function'");

  std::string exceptionType;
  if (auto et = args.getString("exception_type"))
    exceptionType = et->str();

  std::vector<std::string> entryPoints;
  if (auto *epsArr = args.getArray("entry_points")) {
    for (auto &v : *epsArr) {
      if (auto s = v.getAsString())
        entryPoints.push_back(s->str());
    }
  }
  if (entryPoints.empty())
    entryPoints = ctx.entryPoints;

  auto result = ctx.oracle.queryExceptionProtection(function->str(),
                                                    exceptionType, entryPoints);

  llvm::json::Object obj;
  obj["function"] = function->str();
  obj["protection"] = protectionToStr(result.protection);
  obj["totalPaths"] = static_cast<int64_t>(result.paths.size());
  obj["summary"] = result.summary;

  // Include path summaries (without full detail to keep response manageable).
  int64_t caught = 0, uncaught = 0;
  for (auto &p : result.paths) {
    if (p.isCaught)
      ++caught;
    else
      ++uncaught;
  }
  obj["caughtPaths"] = caught;
  obj["uncaughtPaths"] = uncaught;

  return makeTextResult(llvm::json::Value(std::move(obj)));
}

// ============================================================================
// Tool 6: query_call_site_context
// ============================================================================

static llvm::json::Value
handleQueryCallSiteContext(const llvm::json::Object &args,
                          const McpToolContext &ctx) {
  auto callSite = args.getString("call_site");
  if (!callSite)
    return makeErrorResult("Missing required parameter 'call_site'");

  // Validate file:line:col format. Split on the rightmost two colons so that
  // Unix absolute paths are preserved.
  auto raw = callSite->str();
  auto lastColon = raw.rfind(':');
  auto secondLast =
      lastColon == std::string::npos ? std::string::npos
                                      : raw.rfind(':', lastColon - 1);
  auto isDigits = [](llvm::StringRef s) {
    if (s.empty())
      return false;
    for (char c : s)
      if (!std::isdigit(static_cast<unsigned char>(c)))
        return false;
    return true;
  };
  if (lastColon == std::string::npos || secondLast == std::string::npos ||
      secondLast == 0 ||
      !isDigits(llvm::StringRef(raw).substr(secondLast + 1,
                                            lastColon - secondLast - 1)) ||
      !isDigits(llvm::StringRef(raw).substr(lastColon + 1))) {
    return makeErrorResult(
        "Invalid call_site format: expected 'file:line:col' (e.g. "
        "'src/foo.cpp:12:3'), got '" +
        raw + "'");
  }

  // Distinguish "not indexed" from "indexed with no enclosing try".
  const auto rawCtx = ctx.cfIndex.contextAtSite(raw);
  if (!rawCtx) {
    return makeErrorResult(
        "Call site not indexed: '" + raw +
        "'. Ensure the path matches the compilation database "
        "canonicalization (typically an absolute path).");
  }

  auto info = ctx.oracle.queryCallSite(raw);

  llvm::json::Object obj;
  obj["callSite"] = info.callSite;
  obj["caller"] = info.caller;
  obj["callee"] = info.callee;
  obj["isUnderTryCatch"] = info.isUnderTryCatch;
  obj["wouldTerminateIfThrows"] = info.wouldTerminateIfThrows;
  obj["enclosingScopeCount"] =
      static_cast<int64_t>(info.enclosingScopes.size());
  obj["enclosingGuardCount"] =
      static_cast<int64_t>(info.enclosingGuards.size());
  obj["liveRaiiLocalsCount"] =
      static_cast<int64_t>(rawCtx->liveRaiiLocals.size());

  // Include scope details.
  llvm::json::Array scopes;
  for (auto &scope : info.enclosingScopes) {
    llvm::json::Object s;
    s["tryLocation"] = scope.tryLocation;
    s["enclosingFunction"] = scope.enclosingFunction;
    s["nestingDepth"] = static_cast<int64_t>(scope.nestingDepth);
    llvm::json::Array handlers;
    for (auto &h : scope.handlers) {
      llvm::json::Object ho;
      ho["caughtType"] = h.caughtType;
      ho["isCatchAll"] = h.isCatchAll;
      ho["body"] = h.bodySummary;
      handlers.push_back(llvm::json::Value(std::move(ho)));
    }
    s["handlers"] = std::move(handlers);
    scopes.push_back(llvm::json::Value(std::move(s)));
  }
  obj["enclosingScopes"] = std::move(scopes);

  return makeTextResult(llvm::json::Value(std::move(obj)));
}

// ============================================================================
// Tool 6a: query_raii_scopes_at_callsite
// ============================================================================

static const char *raiiKindToString(RaiiKind k) {
  switch (k) {
  case RaiiKind::Lock: return "lock";
  case RaiiKind::SmartPtr: return "smart_ptr";
  case RaiiKind::Other: return "other";
  }
  return "other";
}

static std::optional<RaiiKind> parseRaiiKind(llvm::StringRef s) {
  if (s == "lock") return RaiiKind::Lock;
  if (s == "smart_ptr") return RaiiKind::SmartPtr;
  if (s == "other") return RaiiKind::Other;
  return std::nullopt;
}

static llvm::json::Value
handleQueryRaiiScopesAtCallsite(const llvm::json::Object &args,
                                const McpToolContext &ctx) {
  auto callSite = args.getString("call_site");
  if (!callSite)
    return makeErrorResult("Missing required parameter 'call_site'");

  // Optional kinds filter. If absent or empty, all kinds are included.
  std::set<RaiiKind> allowed;
  if (auto *kindsArr = args.getArray("kinds")) {
    for (auto &v : *kindsArr) {
      if (auto s = v.getAsString()) {
        auto k = parseRaiiKind(*s);
        if (!k) {
          return makeErrorResult(
              "Invalid value in kinds: '" + s->str() +
              "' (expected lock, smart_ptr, or other)");
        }
        allowed.insert(*k);
      }
    }
  }
  bool filterByKind = !allowed.empty();

  const auto csCtx = ctx.cfIndex.contextAtSite(callSite->str());
  if (!csCtx) {
    return makeErrorResult(
        "Call site not indexed: '" + callSite->str() +
        "'. Ensure the path matches the compilation database "
        "canonicalization (typically an absolute path).");
  }

  llvm::json::Array locals;
  for (const auto &l : csCtx->liveRaiiLocals) {
    if (filterByKind && !allowed.count(l.kind))
      continue;
    llvm::json::Object obj;
    obj["typeName"] = l.typeName;
    obj["varName"] = l.varName;
    obj["declLocation"] = l.declLocation;
    obj["kind"] = raiiKindToString(l.kind);
    locals.push_back(llvm::json::Value(std::move(obj)));
  }

  llvm::json::Object out;
  out["callSite"] = callSite->str();
  out["caller"] = csCtx->callerName;
  out["callee"] = csCtx->calleeName;
  out["locals"] = std::move(locals);
  return makeTextResult(llvm::json::Value(std::move(out)));
}

// ============================================================================
// Tool 6b: query_locks_held  —  reverse DFS from target to entry points,
// accumulating Lock-kind RAII locals along each discovered path.
// ============================================================================

namespace {

// Hashable key identifying a lock across call sites.
struct LockKey {
  std::string typeName;
  std::string varName;
  bool operator==(const LockKey &o) const {
    return typeName == o.typeName && varName == o.varName;
  }
};

struct LockKeyHash {
  size_t operator()(const LockKey &k) const {
    return std::hash<std::string>{}(k.typeName) ^
           (std::hash<std::string>{}(k.varName) << 1);
  }
};

struct LockOccurrence {
  std::string typeName;
  std::string varName;
  std::string heldAt; // file:line:col of the call site where it was in scope
};

struct PathResult {
  std::string entryPoint;
  std::vector<std::string> path; // entry → ... → target
  std::vector<LockOccurrence> locksHeld;
};

// Safety caps to bound DFS cost on hub functions and cyclic graphs.
constexpr unsigned kDefaultMaxDepth = 20;
constexpr size_t kMaxPaths = 512;
constexpr size_t kDefaultMaxFanIn = 1000;

// Collect Lock-kind RAII locals live at the edge (caller → callee @ callSite),
// reading from the ControlFlowIndex. Appends deduped-by-key occurrences.
static void collectLocksOnEdge(const ControlFlowIndex &cfIndex,
                               const std::string &callSite,
                               std::vector<LockOccurrence> &out,
                               std::unordered_set<LockKey, LockKeyHash> &seen) {
  const auto cs = cfIndex.contextAtSite(callSite);
  if (!cs)
    return;
  for (const auto &l : cs->liveRaiiLocals) {
    if (l.kind != RaiiKind::Lock)
      continue;
    LockKey key{l.typeName, l.varName};
    if (seen.insert(key).second) {
      out.push_back({l.typeName, l.varName, callSite});
    }
  }
}

// Reverse path search from `target` walking caller edges, entirely in
// interned-id space (the string-space version was measured >10 min on a
// 938-TU graph — same exponential class find_call_chain had). Strings are
// resolved only for emitted paths and skipped hubs. Two prunes bound the
// walk (both proven on find_call_chain):
//  - corridor: a forward BFS from the entry set over callee edges records
//    min-edges-from-entry per node; the DFS only expands callers that can
//    still complete an entry->...->target path within maxDepth.
//  - dead-end memo: a node whose ancestry was exhaustively explored from
//    depth d without reaching an entry cannot succeed on a revisit with
//    depth >= d. Failures caused by the per-path edge exclusion or the
//    kMaxPaths early-stop are search-state-dependent and never memoized;
//    hub skips are deterministic per node and stay memoizable.
// The per-path VISITED-EDGE exclusion (not a node-cycle guard) is
// preserved from the original: parallel edges through the same node pair
// at different call sites legitimately yield distinct lock paths.

using SId = StringInterner::Id;

struct EdgeVisitKey {
  SId caller, callee, site;
  bool operator==(const EdgeVisitKey &o) const {
    return caller == o.caller && callee == o.callee && site == o.site;
  }
};
struct EdgeVisitKeyHash {
  size_t operator()(const EdgeVisitKey &k) const {
    uint64_t h = (static_cast<uint64_t>(k.caller) << 32) | k.callee;
    h ^= (h >> 33);
    h *= 0xff51afd7ed558ccdULL;
    h ^= k.site;
    h ^= (h >> 33);
    return static_cast<size_t>(h);
  }
};

namespace {
struct RawLockPath {
  SId entryPoint;
  std::vector<SId> nodes;     // target -> ... -> entry (reversed at emit)
  std::vector<SId> edgeSites; // parallel to edges along nodes
};

struct LockDfsState {
  const CallGraph &graph;
  const std::unordered_set<SId> &entrySet;
  const std::unordered_map<SId, unsigned> &minFromEntry;
  unsigned maxDepth;
  size_t maxFanIn;
  std::optional<SId> indirectId;
  std::vector<SId> path;
  std::vector<SId> edgeSites;
  std::unordered_set<EdgeVisitKey, EdgeVisitKeyHash> visitedEdges;
  std::unordered_map<SId, unsigned> deadAt;
  std::unordered_map<SId, std::vector<CallGraph::EdgeRef>> callersMemo;
  std::map<SId, size_t> skippedHubs;
  std::vector<RawLockPath> out;
  bool truncated = false;
};
} // namespace

static int lockReverseDfs(LockDfsState &st) {
  constexpr int kFound = 1, kBlocked = 2;
  if (st.out.size() >= kMaxPaths) {
    st.truncated = true;
    return kBlocked;
  }

  SId cur = st.path.back();
  if (st.entrySet.count(cur)) {
    RawLockPath rp;
    rp.entryPoint = cur;
    rp.nodes = st.path;
    rp.edgeSites = st.edgeSites;
    st.out.push_back(std::move(rp));
    return kFound;
  }

  if (st.path.size() >= st.maxDepth)
    return 0; // budget failure — memoizable via the depth comparison

  auto dit = st.deadAt.find(cur);
  if (dit != st.deadAt.end() &&
      static_cast<unsigned>(st.path.size()) >= dit->second)
    return 0;

  // Hub cutoff: never refuse to expand the query target itself.
  // Deterministic per node, so it does not poison the dead-end memo.
  if (st.maxFanIn > 0 && st.path.size() > 1) {
    size_t inDeg = st.graph.storedInDegree(cur);
    if (inDeg > st.maxFanIn) {
      st.skippedHubs.emplace(cur, inDeg);
      return 0;
    }
  }

  int flags = 0;
  auto cit = st.callersMemo.find(cur);
  if (cit == st.callersMemo.end())
    cit = st.callersMemo.emplace(cur, st.graph.callerRefsOf(cur)).first;
  const auto &callers = cit->second;
  for (const auto &edge : callers) {
    // Skip indirect edges — they have no stable callee identity for
    // transitive lock inheritance.
    if (st.indirectId && edge.caller == *st.indirectId)
      continue;
    // Corridor prune: the caller must be reachable from an entry with
    // enough budget left to descend back to the target (path holds
    // st.path.size() nodes; pushing the caller and completing via the
    // entry needs minFromEntry more edges).
    auto mit = st.minFromEntry.find(edge.caller);
    if (mit == st.minFromEntry.end() ||
        st.path.size() + 1 + mit->second > st.maxDepth)
      continue;
    EdgeVisitKey key{edge.caller, edge.callee, edge.callSite};
    if (!st.visitedEdges.insert(key).second) {
      flags |= kBlocked; // path-dependent exclusion
      continue;
    }

    st.path.push_back(edge.caller);
    st.edgeSites.push_back(edge.callSite);
    flags |= lockReverseDfs(st);
    st.edgeSites.pop_back();
    st.path.pop_back();
    st.visitedEdges.erase(key);

    if (st.out.size() >= kMaxPaths) {
      st.truncated = true;
      flags |= kBlocked; // exploration truncated, not exhausted
      break;
    }
  }

  if (!(flags & (kFound | kBlocked))) {
    auto [it, inserted] =
        st.deadAt.emplace(cur, static_cast<unsigned>(st.path.size()));
    if (!inserted && static_cast<unsigned>(st.path.size()) < it->second)
      it->second = static_cast<unsigned>(st.path.size());
  }
  return flags;
}

static std::vector<PathResult>
collectLocksHeld(const CallGraph &graph, const ControlFlowIndex &cfIndex,
                 const std::string &target,
                 const std::vector<std::string> &entryPoints,
                 unsigned maxDepth, size_t maxFanIn,
                 std::map<std::string, size_t> &skippedHubs,
                 bool &truncated) {
  std::vector<PathResult> out;
  truncated = false;
  if (entryPoints.empty())
    return out;

  const auto &interner = graph.interner();
  auto targetId = interner.find(target);
  if (!targetId)
    return out;
  std::unordered_set<SId> entrySet;
  for (const auto &ep : entryPoints) {
    if (auto id = interner.find(ep))
      entrySet.insert(*id);
  }
  if (entrySet.empty())
    return out;

  // Corridor BFS: min edges from any entry, over callee edges, bounded by
  // maxDepth (node budget; a path of N nodes has N-1 edges).
  std::unordered_map<SId, unsigned> minFromEntry;
  {
    std::vector<SId> frontier(entrySet.begin(), entrySet.end());
    for (SId e : frontier)
      minFromEntry.emplace(e, 0);
    unsigned dist = 0;
    while (!frontier.empty() && dist + 1 < maxDepth) {
      ++dist;
      std::vector<SId> next;
      for (SId node : frontier) {
        for (const auto &edge : graph.calleeRefsOf(node)) {
          if (minFromEntry.emplace(edge.callee, dist).second)
            next.push_back(edge.callee);
        }
      }
      frontier = std::move(next);
    }
  }
  {
    auto tmit = minFromEntry.find(*targetId);
    if (tmit == minFromEntry.end() || tmit->second + 1 > maxDepth)
      return out; // no entry->target corridor within budget
  }

  LockDfsState st{graph,    entrySet, minFromEntry,
                  maxDepth, maxFanIn, interner.find("<indirect>")};
  st.path.push_back(*targetId);
  lockReverseDfs(st);
  truncated = st.truncated;

  // Resolve ids only for the emitted paths and skipped hubs.
  out.reserve(st.out.size());
  for (const auto &rp : st.out) {
    PathResult pr;
    pr.entryPoint = interner.resolve(rp.entryPoint);
    pr.path.reserve(rp.nodes.size());
    for (auto it = rp.nodes.rbegin(); it != rp.nodes.rend(); ++it)
      pr.path.push_back(interner.resolve(*it));
    std::unordered_set<LockKey, LockKeyHash> seen;
    for (SId site : rp.edgeSites)
      collectLocksOnEdge(cfIndex, interner.resolve(site), pr.locksHeld,
                         seen);
    out.push_back(std::move(pr));
  }
  for (const auto &[hubId, deg] : st.skippedHubs)
    skippedHubs.emplace(interner.resolve(hubId), deg);
  return out;
}

static llvm::json::Value pathResultToJson(const PathResult &pr) {
  llvm::json::Object obj;
  obj["entryPoint"] = pr.entryPoint;
  llvm::json::Array p;
  for (const auto &f : pr.path)
    p.push_back(f);
  obj["path"] = std::move(p);
  llvm::json::Array locks;
  for (const auto &l : pr.locksHeld) {
    llvm::json::Object lo;
    lo["typeName"] = l.typeName;
    lo["varName"] = l.varName;
    lo["heldAt"] = l.heldAt;
    locks.push_back(llvm::json::Value(std::move(lo)));
  }
  obj["locksHeld"] = std::move(locks);
  return llvm::json::Value(std::move(obj));
}

} // namespace

static llvm::json::Value handleQueryLocksHeld(const llvm::json::Object &args,
                                              const McpToolContext &ctx) {
  auto fn = args.getString("function");
  if (!fn)
    return makeErrorResult("Missing required parameter 'function'");

  unsigned maxDepth = kDefaultMaxDepth;
  if (auto md = args.getInteger("max_depth"))
    maxDepth = static_cast<unsigned>(std::max<int64_t>(1, *md));

  std::vector<std::string> entryPoints;
  if (auto *epsArr = args.getArray("entry_points")) {
    for (auto &v : *epsArr) {
      if (auto s = v.getAsString())
        entryPoints.push_back(s->str());
    }
  }
  if (entryPoints.empty())
    entryPoints = ctx.entryPoints;

  size_t maxFanIn = kDefaultMaxFanIn;
  if (auto mf = args.getInteger("max_fan_in"))
    maxFanIn = static_cast<size_t>(std::max<int64_t>(0, *mf));

  bool truncated = false;
  std::map<std::string, size_t> skippedHubs;
  auto paths = collectLocksHeld(ctx.graph, ctx.cfIndex, fn->str(),
                                 entryPoints, maxDepth, maxFanIn,
                                 skippedHubs, truncated);

  llvm::json::Object out;
  out["function"] = fn->str();
  llvm::json::Array arr;
  for (const auto &pr : paths)
    arr.push_back(pathResultToJson(pr));
  out["paths"] = std::move(arr);
  out["truncated"] = truncated;
  out["pathCount"] = static_cast<int64_t>(paths.size());
  if (!skippedHubs.empty()) {
    llvm::json::Array hubs;
    for (const auto &[name, inDegree] : skippedHubs) {
      llvm::json::Object hub;
      hub["name"] = name;
      hub["inDegree"] = static_cast<int64_t>(inDegree);
      hubs.push_back(llvm::json::Value(std::move(hub)));
    }
    out["skippedHubs"] = std::move(hubs);
  }
  return makeTextResult(llvm::json::Value(std::move(out)));
}

// ============================================================================
// Tool 6c: query_same_lock  —  intersection of locks_held(a) and
// locks_held(b). Lock identity = (typeName, varName).
// ============================================================================

static llvm::json::Value handleQuerySameLock(const llvm::json::Object &args,
                                             const McpToolContext &ctx) {
  auto a = args.getString("fn_a");
  auto b = args.getString("fn_b");
  if (!a || !b)
    return makeErrorResult("Missing required parameters 'fn_a' and 'fn_b'");

  unsigned maxDepth = kDefaultMaxDepth;
  if (auto md = args.getInteger("max_depth"))
    maxDepth = static_cast<unsigned>(std::max<int64_t>(1, *md));

  std::vector<std::string> entryPoints;
  if (auto *epsArr = args.getArray("entry_points")) {
    for (auto &v : *epsArr) {
      if (auto s = v.getAsString())
        entryPoints.push_back(s->str());
    }
  }
  if (entryPoints.empty())
    entryPoints = ctx.entryPoints;

  size_t maxFanIn = kDefaultMaxFanIn;
  if (auto mf = args.getInteger("max_fan_in"))
    maxFanIn = static_cast<size_t>(std::max<int64_t>(0, *mf));

  bool truncA = false, truncB = false;
  std::map<std::string, size_t> skippedHubs;
  auto pathsA = collectLocksHeld(ctx.graph, ctx.cfIndex, a->str(),
                                  entryPoints, maxDepth, maxFanIn,
                                  skippedHubs, truncA);
  auto pathsB = collectLocksHeld(ctx.graph, ctx.cfIndex, b->str(),
                                  entryPoints, maxDepth, maxFanIn,
                                  skippedHubs, truncB);

  // Collect lock identity sets per side, remembering which paths use each.
  std::unordered_map<LockKey, std::vector<size_t>, LockKeyHash> byKeyA,
      byKeyB;
  for (size_t i = 0; i < pathsA.size(); ++i)
    for (const auto &l : pathsA[i].locksHeld)
      byKeyA[{l.typeName, l.varName}].push_back(i);
  for (size_t i = 0; i < pathsB.size(); ++i)
    for (const auto &l : pathsB[i].locksHeld)
      byKeyB[{l.typeName, l.varName}].push_back(i);

  // Intersect.
  llvm::json::Array sharedArr;
  int sharedCount = 0;
  for (const auto &[key, idxsA] : byKeyA) {
    auto it = byKeyB.find(key);
    if (it == byKeyB.end())
      continue;
    ++sharedCount;
    llvm::json::Object obj;
    obj["typeName"] = key.typeName;
    obj["varName"] = key.varName;
    llvm::json::Array pa, pb;
    for (auto idx : idxsA)
      pa.push_back(pathResultToJson(pathsA[idx]));
    for (auto idx : it->second)
      pb.push_back(pathResultToJson(pathsB[idx]));
    obj["pathsA"] = std::move(pa);
    obj["pathsB"] = std::move(pb);
    sharedArr.push_back(llvm::json::Value(std::move(obj)));
  }

  llvm::json::Object out;
  out["fn_a"] = a->str();
  out["fn_b"] = b->str();
  out["sharedLocks"] = std::move(sharedArr);
  out["shared"] = sharedCount;
  out["aOnly"] =
      static_cast<int64_t>(byKeyA.size()) - static_cast<int64_t>(sharedCount);
  out["bOnly"] =
      static_cast<int64_t>(byKeyB.size()) - static_cast<int64_t>(sharedCount);
  out["truncated"] = truncA || truncB;
  if (!skippedHubs.empty()) {
    llvm::json::Array hubs;
    for (const auto &[name, inDegree] : skippedHubs) {
      llvm::json::Object hub;
      hub["name"] = name;
      hub["inDegree"] = static_cast<int64_t>(inDegree);
      hubs.push_back(llvm::json::Value(std::move(hub)));
    }
    out["skippedHubs"] = std::move(hubs);
  }
  return makeTextResult(llvm::json::Value(std::move(out)));
}

// ============================================================================
// Tool 7: analyze_dead_code
// ============================================================================

static const char *livenessToStr(Liveness l) {
  switch (l) {
  case Liveness::Alive: return "alive";
  case Liveness::OptimisticallyAlive: return "optimistically_alive";
  case Liveness::Dead: return "dead";
  }
  return "unknown";
}

static llvm::json::Value handleAnalyzeDeadCode(const llvm::json::Object &args,
                                               const McpToolContext &ctx) {
  std::vector<std::string> entryPoints;
  if (auto *epsArr = args.getArray("entry_points")) {
    for (auto &v : *epsArr) {
      if (auto s = v.getAsString())
        entryPoints.push_back(s->str());
    }
  }
  if (entryPoints.empty())
    entryPoints = ctx.entryPoints;

  bool includeOptimistic = true;
  if (auto io = args.getBoolean("include_optimistic"))
    includeOptimistic = *io;

  bool includeSystem = false;
  if (auto is = args.getBoolean("include_system"))
    includeSystem = *is;

  std::string namePrefix;
  if (auto np = args.getString("name_prefix"))
    namePrefix = np->str();
  std::string filePrefix;
  if (auto fp = args.getString("file_prefix"))
    filePrefix = fp->str();

  int64_t limit = 500;
  if (auto l = args.getInteger("limit"))
    limit = *l;
  int64_t offset = 0;
  if (auto o = args.getInteger("offset"))
    offset = *o;
  if (limit < 0)
    limit = 0;
  if (offset < 0)
    offset = 0;

  // Two cache layers: the final JSON keyed on ALL args (identical repeat
  // calls are free), and the liveness map keyed on the analysis inputs
  // only (entry points + optimistic flag) so pagination/filter variations
  // share the whole-graph BFS.
  std::string fullKey;
  if (ctx.cache) {
    fullKey = "dead_code_json|";
    fullKey += includeOptimistic ? '1' : '0';
    fullKey += includeSystem ? '1' : '0';
    fullKey += '|' + namePrefix + '|' + filePrefix + '|' +
               std::to_string(limit) + '|' + std::to_string(offset);
    for (const auto &ep : entryPoints) {
      fullKey += '|';
      fullKey += ep;
    }
    auto it = ctx.cache->byKey.find(fullKey);
    if (it != ctx.cache->byKey.end())
      return it->second;
  }

  using LivenessMap = std::unordered_map<std::string, Liveness>;
  std::shared_ptr<const LivenessMap> resultsPtr;
  std::string cacheKey;
  if (ctx.cache) {
    cacheKey = "dead_code|";
    cacheKey += includeOptimistic ? '1' : '0';
    for (const auto &ep : entryPoints) {
      cacheKey += '|';
      cacheKey += ep;
    }
    auto it = ctx.cache->objects.find(cacheKey);
    if (it != ctx.cache->objects.end())
      resultsPtr = std::static_pointer_cast<const LivenessMap>(it->second);
  }
  if (!resultsPtr) {
    DeadCodeAnalyzer analyzer(ctx.graph, entryPoints);
    analyzer.analyzePessimistic();
    if (includeOptimistic)
      analyzer.analyzeOptimistic();
    auto computed = std::make_shared<LivenessMap>(analyzer.getResults());
    if (ctx.cache)
      ctx.cache->objects[cacheKey] = computed;
    resultsPtr = std::move(computed);
  }
  const LivenessMap &results = *resultsPtr;

  // Counts of all categories, computed before filtering — aliveCount and
  // optimisticallyAliveCount are meta-stats, not affected by the dead-list
  // filters below.
  int64_t aliveCount = 0, optimisticCount = 0;

  auto passesFilter = [&](const CallGraphNode *node,
                          const std::string &name) {
    if (!includeSystem && node && isSystemPath(node->file))
      return false;
    if (!namePrefix.empty() && !llvm::StringRef(name).starts_with(namePrefix))
      return false;
    if (!filePrefix.empty() &&
        (!node || !llvm::StringRef(node->file).starts_with(filePrefix)))
      return false;
    return true;
  };

  llvm::json::Array optimistic;
  // Collect filtered dead entries first so we can paginate.
  std::vector<llvm::json::Value> deadAll;
  for (auto &kv : results) {
    auto *node = ctx.graph.findNode(kv.first);
    llvm::json::Object entry;
    entry["name"] = kv.first;
    if (node) {
      entry["file"] = node->file;
      entry["line"] = static_cast<int64_t>(node->line);
    }

    switch (kv.second) {
    case Liveness::Alive:
      ++aliveCount;
      break;
    case Liveness::OptimisticallyAlive:
      ++optimisticCount;
      if (passesFilter(node, kv.first))
        optimistic.push_back(llvm::json::Value(std::move(entry)));
      break;
    case Liveness::Dead:
      if (passesFilter(node, kv.first))
        deadAll.push_back(llvm::json::Value(std::move(entry)));
      break;
    }
  }

  const int64_t totalDead = static_cast<int64_t>(deadAll.size());
  const int64_t start = std::min(offset, totalDead);
  const int64_t end = std::min(start + limit, totalDead);
  llvm::json::Array dead;
  for (int64_t i = start; i < end; ++i)
    dead.push_back(std::move(deadAll[i]));

  llvm::json::Object obj;
  obj["totalFunctions"] = static_cast<int64_t>(results.size());
  obj["aliveCount"] = aliveCount;
  obj["optimisticallyAliveCount"] = optimisticCount;
  obj["totalDead"] = totalDead;
  obj["deadCount"] = static_cast<int64_t>(dead.size());
  obj["offset"] = offset;
  obj["limit"] = limit;
  obj["truncated"] = end < totalDead;
  obj["dead"] = std::move(dead);
  obj["optimisticallyAlive"] = std::move(optimistic);
  // Omit alive list to keep response size down — caller usually wants dead.
  auto out = makeTextResult(llvm::json::Value(std::move(obj)));
  if (ctx.cache)
    ctx.cache->byKey.insert_or_assign(fullKey, out);
  return out;
}

// ============================================================================
// Tool 8: get_class_hierarchy
// ============================================================================

static llvm::json::Value
handleGetClassHierarchy(const llvm::json::Object &args,
                        const McpToolContext &ctx) {
  auto className = args.getString("class_name");
  if (!className)
    return makeErrorResult("Missing required parameter 'class_name'");

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
      auto overrides = ctx.graph.getOverrides(node->qualifiedName);
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

  return makeTextResult(llvm::json::Value(std::move(obj)));
}

// ============================================================================
// Tool 9: list_entry_points
// ============================================================================

static llvm::json::Value
handleListEntryPoints(const llvm::json::Object & /*args*/,
                      const McpToolContext &ctx) {
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
  return makeTextResult(llvm::json::Value(std::move(obj)));
}

// ============================================================================
// Tool 10: graph_summary
// ============================================================================

static llvm::json::Value
handleGraphSummary(const llvm::json::Object & /*args*/,
                   const McpToolContext &ctx) {
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
    auto edges = ctx.graph.calleesOf(node->qualifiedName);
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
    std::sort(v.begin(), v.end(),
              [](const auto &a, const auto &b) { return a.second > b.second; });
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
  obj["callSiteCount"] = static_cast<int64_t>(ctx.cfIndex.size());
  obj["entryPointCount"] = static_cast<int64_t>(ctx.entryPoints.size());
  obj["confidenceHistogram"] = llvm::json::Value(std::move(conf));
  obj["edgeKindHistogram"] = llvm::json::Value(std::move(kinds));
  obj["topFanoutCallers"] = topN(std::move(callerFanout), 5);
  obj["topFanoutCallees"] = topN(std::move(calleeInVec), 5);
  auto out = makeTextResult(llvm::json::Value(std::move(obj)));
  if (ctx.cache)
    ctx.cache->byKey.insert_or_assign("graph_summary", out);
  return out;
}

// ============================================================================
// Tool 11: list_callback_sites
// ============================================================================

static llvm::json::Value
handleListCallbackSites(const llvm::json::Object &args,
                        const McpToolContext &ctx) {
  auto targetFilter = args.getString("target_prefix");

  // Group callback-like edges by calleeName (copies — calleesOf returns a
  // temporary vector per node).
  std::map<std::string, std::vector<CallGraphEdge>> byTarget;
  for (auto *node : ctx.graph.allNodes()) {
    for (const auto &e : ctx.graph.calleesOf(node->qualifiedName)) {
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
  return makeTextResult(llvm::json::Value(std::move(obj)));
}

// ============================================================================
// Tool 12: list_concurrency_entry_points
// ============================================================================

static llvm::json::Value
handleListConcurrencyEntryPoints(const llvm::json::Object &args,
                                 const McpToolContext &ctx) {
  std::set<ExecutionContext> ctxFilter;
  if (auto *arr = args.getArray("execution_contexts")) {
    for (auto &v : *arr) {
      auto s = v.getAsString();
      if (!s)
        continue;
      auto parsed = parseExecutionContext(*s);
      if (!parsed) {
        return makeErrorResult(
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
    for (const auto &e : ctx.graph.calleesOf(node->qualifiedName)) {
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
  return makeTextResult(llvm::json::Value(std::move(obj)));
}

// ============================================================================
// JSON Schema builders for tool input schemas
// ============================================================================

static llvm::json::Value stringProp(llvm::StringRef desc) {
  llvm::json::Object p;
  p["type"] = "string";
  p["description"] = desc.str();
  return llvm::json::Value(std::move(p));
}

static llvm::json::Value intProp(llvm::StringRef desc) {
  llvm::json::Object p;
  p["type"] = "integer";
  p["description"] = desc.str();
  return llvm::json::Value(std::move(p));
}

static llvm::json::Value boolProp(llvm::StringRef desc) {
  llvm::json::Object p;
  p["type"] = "boolean";
  p["description"] = desc.str();
  return llvm::json::Value(std::move(p));
}

static llvm::json::Value stringArrayProp(llvm::StringRef desc) {
  llvm::json::Object items;
  items["type"] = "string";
  llvm::json::Object p;
  p["type"] = "array";
  p["items"] = std::move(items);
  p["description"] = desc.str();
  return llvm::json::Value(std::move(p));
}

// ============================================================================
// Tool registration
// ============================================================================

std::vector<McpToolEntry> getRegisteredTools() {
  std::vector<McpToolEntry> tools;

  // 1. lookup_function
  {
    llvm::json::Object props;
    props["name"] = stringProp("Qualified function name (e.g. 'MyClass::process')");
    llvm::json::Array req;
    req.push_back("name");
    llvm::json::Object schema;
    schema["type"] = "object";
    schema["properties"] = std::move(props);
    schema["required"] = std::move(req);

    tools.push_back({"lookup_function",
                     "Look up metadata for a function by qualified name. "
                     "Returns file, line, class membership, and virtual status.",
                     llvm::json::Value(std::move(schema)),
                     handleLookupFunction});
  }

  // 2. get_callees
  {
    llvm::json::Object props;
    props["name"] = stringProp("Qualified name of the caller function");
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
    llvm::json::Array req;
    req.push_back("name");
    llvm::json::Object schema;
    schema["type"] = "object";
    schema["properties"] = std::move(props);
    schema["required"] = std::move(req);

    tools.push_back({"get_callees",
                     "List all functions called by a given function. "
                     "Supports filtering by edge kind and confidence level.",
                     llvm::json::Value(std::move(schema)),
                     handleGetCallees});
  }

  // 3. get_callers
  {
    llvm::json::Object props;
    props["name"] = stringProp("Qualified name of the callee function");
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
    llvm::json::Array req;
    req.push_back("name");
    llvm::json::Object schema;
    schema["type"] = "object";
    schema["properties"] = std::move(props);
    schema["required"] = std::move(req);

    tools.push_back({"get_callers",
                     "List all functions that call a given function. "
                     "Supports filtering by edge kind and confidence level.",
                     llvm::json::Value(std::move(schema)),
                     handleGetCallers});
  }

  // 4. find_call_chain
  {
    llvm::json::Object props;
    props["from"] = stringProp(
        "Source function qualified name (omit to use entry points)");
    props["to"] = stringProp("Target function qualified name");
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
    llvm::json::Array req;
    req.push_back("to");
    llvm::json::Object schema;
    schema["type"] = "object";
    schema["properties"] = std::move(props);
    schema["required"] = std::move(req);

    tools.push_back({"find_call_chain",
                     "Find call chains from a source function (or entry points) "
                     "to a target function. Each path is an array of hop "
                     "objects with {from, to, kind, confidence, callSite, "
                     "executionContext?}. executionContext is only emitted on "
                     "ThreadEntry hops and other non-Synchronous edges.",
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

  // 5. query_exception_safety
  {
    llvm::json::Object props;
    props["function"] = stringProp("Target function qualified name");
    props["exception_type"] = stringProp(
        "Exception type to check (e.g. 'std::runtime_error')");
    props["entry_points"] = stringArrayProp(
        "Entry point function names (default: configured entry points)");
    llvm::json::Array req;
    req.push_back("function");
    llvm::json::Object schema;
    schema["type"] = "object";
    schema["properties"] = std::move(props);
    schema["required"] = std::move(req);

    tools.push_back({"query_exception_safety",
                     "Determine whether a function is protected by try/catch "
                     "on its call paths from entry points. Reports always, "
                     "sometimes, or never caught.",
                     llvm::json::Value(std::move(schema)),
                     handleQueryExceptionSafety});
  }

  // 6. query_call_site_context
  {
    llvm::json::Object props;
    props["call_site"] = stringProp(
        "Call site location formatted as 'file:line:col'. The file path "
        "must match the compilation database canonicalization (typically "
        "an absolute path). Returns isError if the site is not indexed.");
    llvm::json::Array req;
    req.push_back("call_site");
    llvm::json::Object schema;
    schema["type"] = "object";
    schema["properties"] = std::move(props);
    schema["required"] = std::move(req);

    tools.push_back({"query_call_site_context",
                     "Get exception handling and guard context at a specific "
                     "call site location (file:line:col). Shows enclosing "
                     "try/catch scopes and conditional guards.",
                     llvm::json::Value(std::move(schema)),
                     handleQueryCallSiteContext});
  }

  // 6a. query_raii_scopes_at_callsite
  {
    llvm::json::Object props;
    props["call_site"] = stringProp(
        "Call site location formatted as 'file:line:col'. Must match the "
        "compilation database canonicalization.");
    props["kinds"] = stringArrayProp(
        "Filter by kind: lock, smart_ptr, other. Default: all kinds.");
    llvm::json::Array req;
    req.push_back("call_site");
    llvm::json::Object schema;
    schema["type"] = "object";
    schema["properties"] = std::move(props);
    schema["required"] = std::move(req);

    tools.push_back({"query_raii_scopes_at_callsite",
                     "List RAII-capable locals (non-trivial-destructor) live "
                     "at a call site. Each entry is {typeName, varName, "
                     "kind, declLocation}, kind in {lock, smart_ptr, other}. "
                     "Use `kinds` to narrow the response (e.g. [\"lock\"] "
                     "for concurrency audits).",
                     llvm::json::Value(std::move(schema)),
                     handleQueryRaiiScopesAtCallsite});
  }

  // 6b. query_locks_held
  {
    llvm::json::Object props;
    props["function"] =
        stringProp("Qualified name of the target function");
    props["max_depth"] = intProp(
        "Maximum number of frames above the target to walk (default: 20)");
    props["max_fan_in"] = intProp(
        "Skip expanding functions with more stored callers than this; "
        "skipped hubs are listed in the response. 0 disables "
        "(default: 1000).");
    props["entry_points"] = stringArrayProp(
        "Entry points to root the reverse walk (default: configured "
        "entry points)");
    llvm::json::Array req;
    req.push_back("function");
    llvm::json::Object schema;
    schema["type"] = "object";
    schema["properties"] = std::move(props);
    schema["required"] = std::move(req);

    tools.push_back({"query_locks_held",
                     "For each entry point, enumerate call paths reaching "
                     "`function` via reverse-walking the call graph, and "
                     "report Lock-kind RAII locals live on any edge of each "
                     "path. Result: {paths:[{entryPoint, path:[fn...], "
                     "locksHeld:[{typeName, varName, heldAt}]}], truncated, "
                     "pathCount}. Truncated at 512 paths total. Walks only "
                     "through edges with stable callee identity "
                     "(indirect/function-pointer targets are skipped).",
                     llvm::json::Value(std::move(schema)),
                     handleQueryLocksHeld});
  }

  // 6c. query_same_lock
  {
    llvm::json::Object props;
    props["fn_a"] = stringProp("First function qualified name");
    props["fn_b"] = stringProp("Second function qualified name");
    props["max_depth"] = intProp(
        "Maximum number of frames above each target to walk (default: 20)");
    props["max_fan_in"] = intProp(
        "Skip expanding functions with more stored callers than this; "
        "skipped hubs are listed in the response. 0 disables "
        "(default: 1000).");
    props["entry_points"] = stringArrayProp(
        "Entry points to root the reverse walk (default: configured "
        "entry points)");
    llvm::json::Array req;
    req.push_back("fn_a");
    req.push_back("fn_b");
    llvm::json::Object schema;
    schema["type"] = "object";
    schema["properties"] = std::move(props);
    schema["required"] = std::move(req);

    tools.push_back({"query_same_lock",
                     "Compute the intersection of locks held across paths "
                     "reaching fn_a and fn_b. Lock identity is the tuple "
                     "(typeName, varName); the same physical mutex under "
                     "different variable names will not match. Result: "
                     "{sharedLocks:[{typeName, varName, pathsA, pathsB}], "
                     "shared, aOnly, bOnly, truncated}.",
                     llvm::json::Value(std::move(schema)),
                     handleQuerySameLock});
  }

  // 7. analyze_dead_code
  {
    llvm::json::Object props;
    props["entry_points"] = stringArrayProp(
        "Entry point function names (default: configured entry points)");
    props["include_optimistic"] = boolProp(
        "Include optimistically-alive functions (default: true)");
    props["include_system"] = boolProp(
        "Include functions whose source file lives in a system include "
        "directory such as /usr/include or compiler-internal clang/gcc "
        "include dirs (default: false). Stdlib template instantiations "
        "otherwise dominate the response.");
    props["name_prefix"] = stringProp(
        "Only include dead functions whose qualified name starts with "
        "this prefix.");
    props["file_prefix"] = stringProp(
        "Only include dead functions whose source file path starts with "
        "this prefix.");
    props["limit"] = intProp(
        "Maximum number of dead entries to return after filtering "
        "(default: 500).");
    props["offset"] = intProp(
        "Number of filtered dead entries to skip before returning "
        "(default: 0). Use with limit to paginate.");
    llvm::json::Object schema;
    schema["type"] = "object";
    schema["properties"] = std::move(props);

    tools.push_back({"analyze_dead_code",
                     "Run dead code analysis via call graph reachability. "
                     "Reports dead, optimistically-alive, and alive functions "
                     "from the configured entry points. System-header "
                     "functions are excluded by default; use include_system "
                     "to include them.",
                     llvm::json::Value(std::move(schema)),
                     handleAnalyzeDeadCode});
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

  // reindex_tu — handled specially by McpServer (needs mutable access).
  {
    llvm::json::Object schema;
    schema["type"] = "object";
    schema["required"] = llvm::json::Array{"file"};
    llvm::json::Object props;
    props["file"] = llvm::json::Object{
        {"type", "string"},
        {"description", "Absolute path of the TU to re-index"}};
    schema["properties"] = std::move(props);

    tools.push_back({"reindex_tu",
                     "Re-index a single translation unit after source changes. "
                     "Removes stale edges/contexts and re-runs all three "
                     "analysis phases for the given file. Returns counts of "
                     "edges and contexts removed and current totals.",
                     llvm::json::Value(std::move(schema)),
                     nullptr});
  }

  return tools;
}

} // namespace vycor
