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

// Safety caps to bound the walk on hub functions and cyclic graphs.
constexpr unsigned kDefaultMaxDepth = 20;
constexpr unsigned kMaxPaths = 512;
constexpr size_t kDefaultMaxFanIn = 1000;

// Collect Lock-kind RAII locals live at the hop's edge, read from the
// control-flow context joined on the exact (call site, caller, callee) —
// not the first context stored at the site, which on a line with several
// calls could belong to another edge. Appends deduped-by-key occurrences.
static void collectLocksOnHop(const ControlFlowIndex &cfIndex,
                              const PathHop &hop,
                              std::vector<LockOccurrence> &out,
                              std::unordered_set<LockKey, LockKeyHash> &seen) {
  const auto cs =
      cfIndex.contextForEdge(hop.callSite, hop.callerUsr, hop.calleeUsr);
  if (!cs)
    return;
  for (const auto &l : cs->liveRaiiLocals) {
    if (l.kind != RaiiKind::Lock)
      continue;
    LockKey key{l.typeName, l.varName};
    if (seen.insert(key).second) {
      out.push_back({l.typeName, l.varName, hop.callSite});
    }
  }
}

struct LocksHeldResult {
  std::vector<PathResult> paths;
  PathSearchResult search;
};

// Reverse path search from `target` to the entry points through the
// shared engine (callgraph/PathSearch.h) under CycleRule::SimpleEdges:
// no EDGE repeats on a path, but a function may — parallel edges through
// the same node pair at different call sites legitimately yield distinct
// lock paths, and a lock acquired inside a recursive cycle is still
// observed. Edges from the synthetic `<indirect>` caller are skipped:
// they have no stable identity for transitive lock inheritance. maxDepth
// counts edges (frames above the target).
static LocksHeldResult
collectLocksHeld(const CallGraph &graph, const ControlFlowIndex &cfIndex,
                 const std::string &target,
                 const std::vector<std::string> &entryPoints,
                 unsigned maxDepth, size_t maxFanIn) {
  LocksHeldResult out;
  if (entryPoints.empty())
    return out;

  const auto &interner = graph.interner();
  auto indirectId = interner.find("<indirect>");
  SearchLimits limits;
  limits.maxPaths = kMaxPaths;
  limits.maxDepth = maxDepth;
  limits.maxFanIn = maxFanIn;
  out.search = findCallerPaths(
      graph, target, entryPoints, limits, CycleRule::SimpleEdges,
      [&](const CallGraph::EdgeRef &e) {
        return !indirectId || e.caller != *indirectId;
      });

  out.paths.reserve(out.search.paths.size());
  for (const auto &path : out.search.paths) {
    PathResult pr;
    pr.entryPoint = path.hops.front().callerUsr;
    pr.path.reserve(path.hops.size() + 1);
    pr.path.push_back(path.hops.front().callerUsr);
    for (const auto &hop : path.hops)
      pr.path.push_back(hop.calleeUsr);
    std::unordered_set<LockKey, LockKeyHash> seen;
    // Locks are accumulated from the entry point inward: the outermost
    // frame's guards are reported first.
    for (const auto &hop : path.hops)
      collectLocksOnHop(cfIndex, hop, pr.locksHeld, seen);
    out.paths.push_back(std::move(pr));
  }
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
                                              const ToolContext &ctx) {
  std::optional<llvm::json::Value> ambiguous;
  auto ident = resolveIdentity(args, ctx, "function", "usr", ambiguous);
  if (ambiguous)
    return std::move(*ambiguous);
  if (!ident)
    return errorResult("Missing required parameter 'function' (or 'usr')");

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

  // The lock walk resolves the target through the interner: a USR string
  // works verbatim (usrs ARE the interned identities).
  auto result = collectLocksHeld(ctx.graph, ctx.cfIndex, *ident,
                                 entryPoints, maxDepth, maxFanIn);

  llvm::json::Object out;
  auto fn = args.getString("function");
  out["function"] = fn ? fn->str() : *ident;
  attachUsr(out, ctx, *ident);
  llvm::json::Array arr;
  for (const auto &pr : result.paths)
    arr.push_back(pathResultToJson(pr));
  out["paths"] = std::move(arr);
  // truncated keeps its historical meaning: the path cap cut the walk.
  out["truncated"] = result.search.stopped(StopReason::PathLimit);
  out["pathCount"] = static_cast<int64_t>(result.paths.size());
  attachSearchFacts(out, result.search.stops, result.search.complete(),
                    result.search.exhaustive(), result.search.skippedHubs);
  return llvm::json::Value(std::move(out));
}

// ============================================================================
// Tool 6c: query_same_lock  —  intersection of locks_held(a) and
// locks_held(b). Lock identity = (typeName, varName).
// ============================================================================

static llvm::json::Value handleQuerySameLock(const llvm::json::Object &args,
                                             const ToolContext &ctx) {
  std::optional<llvm::json::Value> ambiguous;
  auto a = resolveIdentity(args, ctx, "fn_a", "fn_a_usr", ambiguous);
  if (ambiguous)
    return std::move(*ambiguous);
  auto b = resolveIdentity(args, ctx, "fn_b", "fn_b_usr", ambiguous);
  if (ambiguous)
    return std::move(*ambiguous);
  if (!a || !b)
    return errorResult(
        "Missing required parameters 'fn_a' and 'fn_b' (or their *_usr "
        "twins)");

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

  auto resA = collectLocksHeld(ctx.graph, ctx.cfIndex, *a, entryPoints,
                               maxDepth, maxFanIn);
  auto resB = collectLocksHeld(ctx.graph, ctx.cfIndex, *b, entryPoints,
                               maxDepth, maxFanIn);
  const auto &pathsA = resA.paths;
  const auto &pathsB = resB.paths;

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
  auto aName = args.getString("fn_a");
  auto bName = args.getString("fn_b");
  out["fn_a"] = aName ? aName->str() : *a;
  out["fn_b"] = bName ? bName->str() : *b;
  attachUsr(out, ctx, *a, "fn_a_usr");
  attachUsr(out, ctx, *b, "fn_b_usr");
  out["sharedLocks"] = std::move(sharedArr);
  out["shared"] = sharedCount;
  out["aOnly"] =
      static_cast<int64_t>(byKeyA.size()) - static_cast<int64_t>(sharedCount);
  out["bOnly"] =
      static_cast<int64_t>(byKeyB.size()) - static_cast<int64_t>(sharedCount);
  out["truncated"] = resA.search.stopped(StopReason::PathLimit) ||
                     resB.search.stopped(StopReason::PathLimit);
  // The facts of the two searches, combined: the answer is only as
  // complete as the less complete side, and a hub skipped on either side
  // is reported once.
  std::vector<SkippedHub> hubs = resA.search.skippedHubs;
  for (const auto &h : resB.search.skippedHubs) {
    bool dup = false;
    for (const auto &e : hubs)
      dup = dup || e.usr == h.usr;
    if (!dup)
      hubs.push_back(h);
  }
  std::sort(hubs.begin(), hubs.end(),
            [](const SkippedHub &x, const SkippedHub &y) {
              return x.usr < y.usr;
            });
  unsigned stops = resA.search.stops | resB.search.stops;
  attachSearchFacts(out, stops,
                    resA.search.complete() && resB.search.complete(),
                    resA.search.exhaustive() && resB.search.exhaustive(),
                    hubs);
  return llvm::json::Value(std::move(out));
}

void registerLockTools(std::vector<ToolEntry> &tools) {
  // 6b. query_locks_held
  {
    llvm::json::Object props;
    props["function"] = stringProp(
        "Qualified name of the target function. Provide 'function' or "
        "'usr' (usr wins when both are present).");
    props["usr"] = stringProp(
        "Exact USR of the target function. Bypasses name resolution — use "
        "it to pick one overload/specialization when the name is "
        "ambiguous.");
    addIdentityRefinementProps(props, "");
    props["max_depth"] = intProp(
        "Maximum number of frames above the target to walk (default: 20)");
    props["max_fan_in"] = intProp(
        "Skip expanding functions with more stored callers than this; "
        "skipped hubs are listed in the response. 0 disables "
        "(default: 1000).");
    props["entry_points"] = stringArrayProp(
        "Entry points to root the reverse walk (default: configured "
        "entry points)");
    llvm::json::Object schema;
    schema["type"] = "object";
    schema["properties"] = std::move(props);

    tools.push_back({"query_locks_held",
                     "For each entry point, enumerate call paths reaching "
                     "`function` via reverse-walking the call graph, and "
                     "report Lock-kind RAII locals live on any edge of each "
                     "path. Result: {paths:[{entryPoint, path:[fn...], "
                     "locksHeld:[{typeName, varName, heldAt}]}], truncated, "
                     "pathCount}. Truncated at 512 paths total. Walks only "
                     "through edges with stable callee identity "
                     "(indirect/function-pointer targets are skipped). An "
                     "ambiguous name returns {ambiguous:true, "
                     "candidates:[...]} — re-query with 'usr'.",
                     llvm::json::Value(std::move(schema)),
                     handleQueryLocksHeld});
  }

  // 6c. query_same_lock
  {
    llvm::json::Object props;
    props["fn_a"] = stringProp(
        "First function qualified name. Provide 'fn_a' or 'fn_a_usr' "
        "(the usr wins when both are present).");
    props["fn_a_usr"] = stringProp(
        "Exact USR of the first function. Bypasses name resolution for "
        "'fn_a' when the name is ambiguous.");
    addIdentityRefinementProps(props, "fn_a_");
    props["fn_b"] = stringProp(
        "Second function qualified name. Provide 'fn_b' or 'fn_b_usr' "
        "(the usr wins when both are present).");
    props["fn_b_usr"] = stringProp(
        "Exact USR of the second function. Bypasses name resolution for "
        "'fn_b' when the name is ambiguous.");
    addIdentityRefinementProps(props, "fn_b_");
    props["max_depth"] = intProp(
        "Maximum number of frames above each target to walk (default: 20)");
    props["max_fan_in"] = intProp(
        "Skip expanding functions with more stored callers than this; "
        "skipped hubs are listed in the response. 0 disables "
        "(default: 1000).");
    props["entry_points"] = stringArrayProp(
        "Entry points to root the reverse walk (default: configured "
        "entry points)");
    llvm::json::Object schema;
    schema["type"] = "object";
    schema["properties"] = std::move(props);

    tools.push_back({"query_same_lock",
                     "Compute the intersection of locks held across paths "
                     "reaching fn_a and fn_b. Lock identity is the tuple "
                     "(typeName, varName); the same physical mutex under "
                     "different variable names will not match. Result: "
                     "{sharedLocks:[{typeName, varName, pathsA, pathsB}], "
                     "shared, aOnly, bOnly, truncated}. An ambiguous name "
                     "returns {ambiguous:true, candidates:[...]} — re-query "
                     "with the *_usr parameter.",
                     llvm::json::Value(std::move(schema)),
                     handleQuerySameLock});
  }
}

} // namespace vycor
