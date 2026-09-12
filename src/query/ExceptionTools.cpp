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
#include <cctype>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include "vycor/ext/Extensions.h"

namespace vycor {

// ============================================================================
// Tool 5: query_exception_safety
// ============================================================================

// Bounded-search knobs shared by the path tools: max_paths, max_depth
// (edges), max_fan_in. Returns an error message for a non-positive
// max_paths/max_depth.
static std::optional<std::string>
parseSearchLimits(const llvm::json::Object &args, SearchLimits &out) {
  if (auto mp = args.getInteger("max_paths")) {
    if (*mp <= 0)
      return "Invalid max_paths: must be positive";
    out.maxPaths = static_cast<unsigned>(*mp);
  }
  if (auto md = args.getInteger("max_depth")) {
    if (*md <= 0)
      return "Invalid max_depth: must be positive";
    out.maxDepth = static_cast<unsigned>(*md);
  }
  if (auto mf = args.getInteger("max_fan_in"))
    out.maxFanIn = static_cast<size_t>(std::max<int64_t>(0, *mf));
  return std::nullopt;
}

static void attachFacts(llvm::json::Object &obj, const PathSearchFacts &f) {
  attachSearchFacts(obj, f.stops, f.complete, f.exhaustive, f.skippedHubs);
}

static void addSearchLimitProps(llvm::json::Object &props) {
  props["max_paths"] = intProp(
      "Maximum number of paths to enumerate (default 100)");
  props["max_depth"] = intProp(
      "Maximum number of edges in a path (default 20)");
  props["max_fan_in"] = intProp(
      "Skip expanding functions with more stored callers than this; "
      "skipped hubs are listed in the response. 0 disables "
      "(default: 1000).");
}

// `entry_points` from the arguments, else the context's configured list
// (the bake's --entry-point list, or main).
static std::vector<std::string> entryPointsArg(const llvm::json::Object &args,
                                               const ToolContext &ctx) {
  std::vector<std::string> entryPoints;
  if (auto *epsArr = args.getArray("entry_points")) {
    for (auto &v : *epsArr) {
      if (auto s = v.getAsString())
        entryPoints.push_back(s->str());
    }
  }
  if (entryPoints.empty())
    entryPoints = ctx.entryPoints;
  return entryPoints;
}

static llvm::json::Value
handleQueryExceptionSafety(const llvm::json::Object &args,
                           const ToolContext &ctx) {
  std::optional<llvm::json::Value> ambiguous;
  auto ident = resolveIdentity(args, ctx, "function", "usr", ambiguous);
  if (ambiguous)
    return std::move(*ambiguous);
  if (!ident)
    return usageError("Missing required parameter 'function' (or 'usr')");

  std::string exceptionType;
  if (auto et = args.getString("exception_type"))
    exceptionType = et->str();

  SearchLimits limits;
  if (auto err = parseSearchLimits(args, limits))
    return usageError(*err);

  auto result = ctx.oracle.queryExceptionProtection(
      *ident, exceptionType, entryPointsArg(args, ctx), limits,
      ctx.facts.coverage.complete());

  llvm::json::Object obj;
  auto function = args.getString("function");
  obj["function"] = function ? function->str() : *ident;
  attachUsr(obj, ctx, *ident);
  obj["protection"] = protectionName(result.protection);
  obj["totalPaths"] = static_cast<int64_t>(result.paths.size());
  obj["summary"] = result.summary;
  // Path counts by outcome (the counts, not the paths: see
  // query_throw_propagation for per-path detail). uncaughtPaths keeps its
  // historical meaning of "not caught" and so includes the paths that
  // terminate or are unknown; the finer split follows.
  obj["caughtPaths"] = static_cast<int64_t>(result.caughtCount);
  obj["uncaughtPaths"] = static_cast<int64_t>(
      result.paths.size() - result.caughtCount);
  obj["terminatingPaths"] = static_cast<int64_t>(result.terminatesCount);
  obj["unknownPaths"] = static_cast<int64_t>(result.unknownCount);
  attachFacts(obj, result.search);

  return llvm::json::Value(std::move(obj));
}

// ============================================================================
// Tool 6: query_call_site_context
// ============================================================================

// Non-error disambiguation response for a call-site spelling shared by
// several live contexts (macro expansion): the client picks a caller and
// re-queries with the `caller` parameter. Candidates sorted by callerUsr
// for determinism.
static llvm::json::Value
makeAmbiguousSiteResult(llvm::StringRef callSite,
                        std::vector<CallSiteContext> contexts) {
  std::sort(contexts.begin(), contexts.end(),
            [](const CallSiteContext &a, const CallSiteContext &b) {
              return a.callerUsr < b.callerUsr;
            });
  llvm::json::Array candidates;
  for (const auto &c : contexts) {
    llvm::json::Object cand;
    cand["callSite"] = c.callSite;
    cand["callerUsr"] = c.callerUsr;
    cand["callerName"] = c.callerName;
    candidates.push_back(llvm::json::Value(std::move(cand)));
  }
  llvm::json::Object obj;
  obj["ambiguous"] = true;
  obj["parameter"] = "caller";
  obj["callSite"] = callSite.str();
  obj["candidates"] = std::move(candidates);
  obj["note"] = "Multiple call sites share this spelling (macro expansion). "
                "Re-run with the 'caller' parameter (USR or qualified name) "
                "of the intended enclosing function.";
  return llvm::json::Value(std::move(obj));
}

// Resolves a call-site spelling to ONE live context per the disambiguation
// contract: `caller` provided -> the precise (spelling, caller) compound
// lookup; absent -> the unique live context, or `ambiguous` set to the
// candidates response when several contexts share the spelling. Returns
// nullopt with `ambiguous` unset when nothing is indexed (the handler emits
// its not-indexed error).
static std::optional<CallSiteContext>
resolveCallSiteContext(const llvm::json::Object &args,
                       const ToolContext &ctx, const std::string &callSite,
                       std::optional<llvm::json::Value> &ambiguous) {
  if (auto caller = args.getString("caller"))
    return ctx.cfIndex.contextAtSite(callSite, caller->str());
  auto contexts = ctx.cfIndex.contextsAtSite(callSite);
  if (contexts.empty())
    return std::nullopt;
  if (contexts.size() >= 2) {
    ambiguous = makeAmbiguousSiteResult(callSite, std::move(contexts));
    return std::nullopt;
  }
  return std::move(contexts.front());
}

static llvm::json::Value
handleQueryCallSiteContext(const llvm::json::Object &args,
                          const ToolContext &ctx) {
  auto callSite = args.getString("call_site");
  if (!callSite)
    return usageError("Missing required parameter 'call_site'");

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
    return usageError(
        "Invalid call_site format: expected 'file:line:col' (e.g. "
        "'src/foo.cpp:12:3'), got '" +
        raw + "'");
  }

  // Distinguish "not indexed" from "indexed with no enclosing try". When
  // several contexts share the spelling (macro expansion) and no `caller`
  // narrows them, a non-error candidates response is returned instead.
  std::optional<llvm::json::Value> ambiguous;
  const auto rawCtx = resolveCallSiteContext(args, ctx, raw, ambiguous);
  if (ambiguous)
    return std::move(*ambiguous);
  if (!rawCtx) {
    return notFoundError(
        "Call site not indexed: '" + raw +
        "'. Ensure the path matches the compilation database "
        "canonicalization (typically an absolute path).");
  }

  // Built from the resolved context directly (the same fields
  // ControlFlowOracle::queryCallSite derives, but honoring the specific
  // caller-qualified context rather than the first spelling match).
  llvm::json::Object obj;
  obj["callSite"] = rawCtx->callSite;
  obj["caller"] = rawCtx->callerName;
  obj["callerUsr"] = rawCtx->callerUsr;
  obj["callee"] = rawCtx->calleeName;
  obj["isUnderTryCatch"] = !rawCtx->enclosingTryCatches.empty();
  obj["wouldTerminateIfThrows"] =
      (rawCtx->callerNoexcept == NoexceptSpec::Noexcept);
  obj["enclosingScopeCount"] =
      static_cast<int64_t>(rawCtx->enclosingTryCatches.size());
  obj["enclosingGuardCount"] =
      static_cast<int64_t>(rawCtx->enclosingGuards.size());
  obj["liveRaiiLocalsCount"] =
      static_cast<int64_t>(rawCtx->liveRaiiLocals.size());

  // Include scope details.
  llvm::json::Array scopes;
  for (auto &scope : rawCtx->enclosingTryCatches)
    scopes.push_back(serializeTryCatchScope(scope));
  obj["enclosingScopes"] = std::move(scopes);

  // Guard details (innermost first), including any organization annotation
  // (feature flags etc.) — previously only the count was reported.
  llvm::json::Array guards;
  for (auto &g : rawCtx->enclosingGuards)
    guards.push_back(serializeGuard(g));
  obj["enclosingGuards"] = std::move(guards);

  return llvm::json::Value(std::move(obj));
}

// ============================================================================
// Tool 6a: query_raii_scopes_at_callsite
// ============================================================================

static std::optional<RaiiKind> parseRaiiKind(llvm::StringRef s) {
  if (s == "lock") return RaiiKind::Lock;
  if (s == "smart_ptr") return RaiiKind::SmartPtr;
  if (s == "other") return RaiiKind::Other;
  return std::nullopt;
}

static llvm::json::Value
handleQueryRaiiScopesAtCallsite(const llvm::json::Object &args,
                                const ToolContext &ctx) {
  auto callSite = args.getString("call_site");
  if (!callSite)
    return usageError("Missing required parameter 'call_site'");

  // Optional kinds filter. If absent or empty, all kinds are included.
  std::set<RaiiKind> allowed;
  if (auto *kindsArr = args.getArray("kinds")) {
    for (auto &v : *kindsArr) {
      if (auto s = v.getAsString()) {
        auto k = parseRaiiKind(*s);
        if (!k) {
          return usageError(
              "Invalid value in kinds: '" + s->str() +
              "' (expected lock, smart_ptr, or other)");
        }
        allowed.insert(*k);
      }
    }
  }
  bool filterByKind = !allowed.empty();

  // Same disambiguation contract as query_call_site_context: an optional
  // `caller` routes to the precise compound-key lookup; a bare spelling
  // matching several live contexts returns a candidates response.
  std::optional<llvm::json::Value> ambiguous;
  const auto csCtx =
      resolveCallSiteContext(args, ctx, callSite->str(), ambiguous);
  if (ambiguous)
    return std::move(*ambiguous);
  if (!csCtx) {
    return notFoundError(
        "Call site not indexed: '" + callSite->str() +
        "'. Ensure the path matches the compilation database "
        "canonicalization (typically an absolute path).");
  }

  llvm::json::Array locals;
  for (const auto &l : csCtx->liveRaiiLocals) {
    if (filterByKind && !allowed.count(l.kind))
      continue;
    locals.push_back(serializeRaiiLocal(l));
  }

  llvm::json::Object out;
  out["callSite"] = callSite->str();
  out["caller"] = csCtx->callerName;
  out["callerUsr"] = csCtx->callerUsr;
  out["callee"] = csCtx->calleeName;
  out["locals"] = std::move(locals);
  return llvm::json::Value(std::move(out));
}

// ============================================================================
// Tools 6b-6d: the path-level oracle queries (Q3-Q5), formerly `prism
// --mode query`. Each answers with full per-path detail where
// query_exception_safety keeps to the counts.
// ============================================================================

// withOutcome=false for query_all_path_contexts, which does no
// propagation walk (no outcome to report).
static llvm::json::Value serializePathInfo(const PathInfo &p,
                                           bool withOutcome) {
  llvm::json::Object obj;
  llvm::json::Array chain;
  for (const auto &fn : p.callChain)
    chain.push_back(fn);
  obj["callChain"] = std::move(chain);
  llvm::json::Array hops;
  for (const auto &hop : p.hops)
    hops.push_back(serializePathHop(hop));
  obj["hops"] = std::move(hops);
  obj["isCaught"] = p.isCaught;
  if (p.isCaught) {
    obj["caughtAt"] = p.caughtAt;
    obj["caughtBy"] = p.caughtBy;
  }
  if (withOutcome) {
    obj["outcome"] = pathOutcomeName(p.outcome);
    if (!p.rethrownAt.empty()) {
      llvm::json::Array rethrown;
      for (const auto &loc : p.rethrownAt)
        rethrown.push_back(loc);
      obj["rethrownAt"] = std::move(rethrown);
    }
    if (p.outcome != PathOutcome::Caught) {
      obj["stopAt"] = p.stopAt;
      obj["note"] = p.note;
    }
  }
  llvm::json::Array scopes;
  for (const auto &scope : p.tryCatchesOnPath)
    scopes.push_back(serializeTryCatchScope(scope));
  obj["tryCatchesOnPath"] = std::move(scopes);
  llvm::json::Array guards;
  for (const auto &g : p.guardsOnPath)
    guards.push_back(serializeGuard(g));
  obj["guardsOnPath"] = std::move(guards);
  return llvm::json::Value(std::move(obj));
}

static llvm::json::Value serializePaths(const std::vector<PathInfo> &paths,
                                        bool withOutcome) {
  llvm::json::Array arr;
  for (const auto &p : paths)
    arr.push_back(serializePathInfo(p, withOutcome));
  return llvm::json::Value(std::move(arr));
}

static llvm::json::Value
handleQueryThrowPropagation(const llvm::json::Object &args,
                            const ToolContext &ctx) {
  std::optional<llvm::json::Value> ambiguous;
  auto ident = resolveIdentity(args, ctx, "function", "usr", ambiguous);
  if (ambiguous)
    return std::move(*ambiguous);
  if (!ident)
    return usageError("Missing required parameter 'function' (or 'usr')");
  std::string exceptionType;
  if (auto et = args.getString("exception_type"))
    exceptionType = et->str();

  SearchLimits limits;
  if (auto err = parseSearchLimits(args, limits))
    return usageError(*err);

  auto result = ctx.oracle.queryThrowPropagation(
      *ident, exceptionType, entryPointsArg(args, ctx), limits,
      ctx.facts.coverage.complete());

  llvm::json::Object obj;
  auto function = args.getString("function");
  obj["function"] = function ? function->str() : *ident;
  attachUsr(obj, ctx, *ident);
  obj["exceptionType"] = exceptionType;
  obj["protection"] = protectionName(result.protection);
  obj["totalPaths"] = static_cast<int64_t>(result.paths.size());
  obj["caughtPaths"] = static_cast<int64_t>(result.caughtCount);
  obj["uncaughtPaths"] = static_cast<int64_t>(
      result.paths.size() - result.caughtCount);
  obj["terminatingPaths"] = static_cast<int64_t>(result.terminatesCount);
  obj["unknownPaths"] = static_cast<int64_t>(result.unknownCount);
  obj["summary"] = result.summary;
  attachFacts(obj, result.search);
  obj["paths"] = serializePaths(result.paths, /*withOutcome=*/true);
  return llvm::json::Value(std::move(obj));
}

static llvm::json::Value
handleQueryAllPathContexts(const llvm::json::Object &args,
                           const ToolContext &ctx) {
  std::optional<llvm::json::Value> ambiguous;
  auto ident = resolveIdentity(args, ctx, "function", "usr", ambiguous);
  if (ambiguous)
    return std::move(*ambiguous);
  if (!ident)
    return usageError("Missing required parameter 'function' (or 'usr')");
  SearchLimits limits;
  if (auto err = parseSearchLimits(args, limits))
    return usageError(*err);

  auto result = ctx.oracle.queryAllPathContexts(
      *ident, entryPointsArg(args, ctx), limits);

  llvm::json::Object obj;
  auto function = args.getString("function");
  obj["function"] = function ? function->str() : *ident;
  attachUsr(obj, ctx, *ident);
  obj["totalPaths"] = static_cast<int64_t>(result.paths.size());
  obj["maxPaths"] = static_cast<int64_t>(limits.maxPaths);
  obj["maxDepth"] = static_cast<int64_t>(limits.maxDepth);
  attachFacts(obj, result.search);
  obj["paths"] = serializePaths(result.paths, /*withOutcome=*/false);
  return llvm::json::Value(std::move(obj));
}

static llvm::json::Value
handleQueryNearestCatches(const llvm::json::Object &args,
                          const ToolContext &ctx) {
  std::optional<llvm::json::Value> ambiguous;
  auto ident = resolveIdentity(args, ctx, "function", "usr", ambiguous);
  if (ambiguous)
    return std::move(*ambiguous);
  if (!ident)
    return usageError("Missing required parameter 'function' (or 'usr')");

  unsigned maxDepth = 20;
  if (auto md = args.getInteger("max_depth")) {
    if (*md <= 0)
      return usageError("Invalid max_depth: must be positive");
    maxDepth = static_cast<unsigned>(*md);
  }

  auto result = ctx.oracle.queryNearestCatches(*ident, maxDepth);

  llvm::json::Object obj;
  auto function = args.getString("function");
  obj["function"] = function ? function->str() : *ident;
  attachUsr(obj, ctx, *ident);
  obj["maxDepth"] = static_cast<int64_t>(result.maxDepth);
  attachSearchFacts(obj, result.stops, result.complete,
                    result.stops == 0, {});
  llvm::json::Array arr;
  for (const auto &c : result.catches) {
    llvm::json::Object o;
    o["framesFromTarget"] = static_cast<int64_t>(c.framesFromTarget);
    llvm::json::Array segment;
    for (const auto &fn : c.pathSegment)
      segment.push_back(fn);
    o["pathSegment"] = std::move(segment);
    llvm::json::Array hops;
    for (const auto &hop : c.hops)
      hops.push_back(serializePathHop(hop));
    o["hops"] = std::move(hops);
    o["callSite"] = c.callSite;
    o["scope"] = serializeTryCatchScope(c.scope);
    arr.push_back(llvm::json::Value(std::move(o)));
  }
  obj["catches"] = std::move(arr);
  return llvm::json::Value(std::move(obj));
}

void registerExceptionTools(std::vector<ToolEntry> &tools) {
  // 5. query_exception_safety
  {
    llvm::json::Object props;
    props["function"] = stringProp(
        "Target function qualified name. Provide 'function' or 'usr' (usr "
        "wins when both are present).");
    props["usr"] = stringProp(
        "Exact USR of the target function. Bypasses name resolution — use "
        "it to pick one overload/specialization when the name is "
        "ambiguous.");
    addIdentityRefinementProps(props, "");
    props["exception_type"] = stringProp(
        "Exception type to check (e.g. 'std::runtime_error')");
    props["entry_points"] = stringArrayProp(
        "Entry point function names (default: configured entry points)");
    addSearchLimitProps(props);
    llvm::json::Object schema;
    schema["type"] = "object";
    schema["properties"] = std::move(props);

    tools.push_back({"query_exception_safety",
                     "Determine whether a function is protected by try/catch "
                     "on its call paths from entry points. protection is "
                     "always_caught / never_caught / noexcept_barrier only "
                     "when every path was enumerated (exhaustive:true); "
                     "sometimes_caught needs one witness of each; "
                     "observed_caught / observed_uncaught describe the "
                     "paths examined when the search stopped early "
                     "(stopReasons) or a path's outcome is unknown. An "
                     "ambiguous name returns {ambiguous:true, "
                     "candidates:[...]} — re-query with 'usr'.",
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
    props["caller"] = stringProp(
        "Optional enclosing function (qualified name or USR). Needed when "
        "several call sites share one spelling (a macro expanded in "
        "different functions); without it such a spelling returns "
        "{ambiguous:true, candidates:[...]} listing the callers.");
    llvm::json::Array req;
    req.push_back("call_site");
    llvm::json::Object schema;
    schema["type"] = "object";
    schema["properties"] = std::move(props);
    schema["required"] = std::move(req);

    tools.push_back({"query_call_site_context",
                     "Get exception handling and guard context at a specific "
                     "call site location (file:line:col). Shows enclosing "
                     "try/catch scopes and conditional guards. A spelling "
                     "shared by several call sites (macro expansion) returns "
                     "{ambiguous:true, candidates:[...]} — re-query with "
                     "'caller'.",
                     llvm::json::Value(std::move(schema)),
                     handleQueryCallSiteContext});
  }

  // 6a. query_raii_scopes_at_callsite
  {
    llvm::json::Object props;
    props["call_site"] = stringProp(
        "Call site location formatted as 'file:line:col'. Must match the "
        "compilation database canonicalization.");
    props["caller"] = stringProp(
        "Optional enclosing function (qualified name or USR). Needed when "
        "several call sites share one spelling (a macro expanded in "
        "different functions); without it such a spelling returns "
        "{ambiguous:true, candidates:[...]} listing the callers.");
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
                     "for concurrency audits). A spelling shared by several "
                     "call sites (macro expansion) returns {ambiguous:true, "
                     "candidates:[...]} — re-query with 'caller'.",
                     llvm::json::Value(std::move(schema)),
                     handleQueryRaiiScopesAtCallsite});
  }

  // Shared identity props for the path tools (function/usr + refinements).
  auto functionProps = [] {
    llvm::json::Object props;
    props["function"] = stringProp(
        "Target function qualified name. Provide 'function' or 'usr' (usr "
        "wins when both are present).");
    props["usr"] = stringProp(
        "Exact USR of the target function. Bypasses name resolution — use "
        "it to pick one overload/specialization when the name is "
        "ambiguous.");
    addIdentityRefinementProps(props, "");
    return props;
  };

  // 6b. query_throw_propagation
  {
    llvm::json::Object props = functionProps();
    props["exception_type"] = stringProp(
        "Exception type the function throws (e.g. 'std::runtime_error'); "
        "empty matches any handler");
    props["entry_points"] = stringArrayProp(
        "Entry point function names (default: configured entry points)");
    addSearchLimitProps(props);
    llvm::json::Object schema;
    schema["type"] = "object";
    schema["properties"] = std::move(props);
    tools.push_back({"query_throw_propagation",
                     "If a function throws the given exception type, is it "
                     "caught before unwinding to an entry point? Reports "
                     "the protection verdict (see query_exception_safety) "
                     "plus every call path with its exact hops (call "
                     "sites), try/catch scopes, guards, and outcome: "
                     "caught (where and by which handler), uncaught "
                     "(escapes the entry point), terminates (noexcept or "
                     "thread boundary), or unknown (no context indexed, "
                     "async boundary). An ambiguous name returns "
                     "{ambiguous:true, candidates:[...]} — re-query with "
                     "'usr'.",
                     llvm::json::Value(std::move(schema)),
                     handleQueryThrowPropagation});
  }

  // 6c. query_all_path_contexts
  {
    llvm::json::Object props = functionProps();
    props["entry_points"] = stringArrayProp(
        "Entry point function names (default: configured entry points)");
    addSearchLimitProps(props);
    llvm::json::Object schema;
    schema["type"] = "object";
    schema["properties"] = std::move(props);
    tools.push_back({"query_all_path_contexts",
                     "Enumerate the call paths from the entry points to a "
                     "function, each with the try/catch scopes and "
                     "conditional guards along it. The exception-context "
                     "counterpart of find_call_chain. An ambiguous name "
                     "returns {ambiguous:true, candidates:[...]} — re-query "
                     "with 'usr'.",
                     llvm::json::Value(std::move(schema)),
                     handleQueryAllPathContexts});
  }

  // 6d. query_nearest_catches
  {
    llvm::json::Object props = functionProps();
    props["max_depth"] = intProp(
        "Maximum number of frames above the function to walk (default 20)");
    llvm::json::Object schema;
    schema["type"] = "object";
    schema["properties"] = std::move(props);
    tools.push_back({"query_nearest_catches",
                     "For each call path into a function, the nearest "
                     "enclosing try/catch walking up the callers: the "
                     "handler scope, how many frames up it sits, and the "
                     "path segment between. Empty when nothing on any path "
                     "catches. An ambiguous name returns {ambiguous:true, "
                     "candidates:[...]} — re-query with 'usr'.",
                     llvm::json::Value(std::move(schema)),
                     handleQueryNearestCatches});
  }
}

} // namespace vycor
