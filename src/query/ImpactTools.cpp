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

// impact_of_change (docs/change-impact.md) and the JSON shapes shared
// with the CLI `diff` verb.

#include "vycor/query/ChangeImpact.h"
#include "vycor/query/Identity.h"
#include "vycor/query/Serialize.h"
#include "vycor/query/Tools.h"
#include "EdgeFilter.h"
#include "Registry.h"
#include "Schema.h"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace vycor {

// ---------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------

namespace {

llvm::json::Value stringArray(const std::vector<std::string> &v) {
  llvm::json::Array a;
  for (const auto &s : v)
    a.push_back(s);
  return llvm::json::Value(std::move(a));
}

llvm::json::Value serializeSide(const RelationshipSide &s, bool contexts) {
  llvm::json::Object o;
  o["siteCount"] = static_cast<int64_t>(s.attributes.size());
  o["attributes"] = stringArray(s.attributes);
  if (contexts)
    o["signatures"] = stringArray(s.signatures);
  llvm::json::Array sites;
  for (const auto &w : s.sites) {
    llvm::json::Object site;
    site["callSite"] = w.callSite;
    site["confidence"] = confidenceToString(w.confidence);
    if (w.execContext != ExecutionContext::Synchronous)
      site["executionContext"] = executionContextToString(w.execContext);
    if (w.indirectionDepth > 0)
      site["indirectionDepth"] = static_cast<int64_t>(w.indirectionDepth);
    if (contexts)
      site["signature"] = w.signature;
    sites.push_back(llvm::json::Value(std::move(site)));
  }
  o["sites"] = std::move(sites);
  return llvm::json::Value(std::move(o));
}

llvm::json::Value serializeChange(const ChangeRecord &c, bool contexts) {
  llvm::json::Object o;
  o["change"] = changeKindName(c.change);
  if (c.change == ChangeKind::FunctionAdded ||
      c.change == ChangeKind::FunctionRemoved) {
    o["function"] = c.function.key;
    o["usr"] = c.function.usr;
    o["name"] = c.function.name;
    o["file"] = c.function.file;
    o["line"] = static_cast<int64_t>(c.function.line);
    o["isEntryPoint"] = c.function.isEntryPoint;
    o["isVirtual"] = c.function.isVirtual;
    if (!c.function.enclosingClass.empty())
      o["enclosingClass"] = c.function.enclosingClass;
    if (!c.explanation.empty())
      o["explanation"] = c.explanation;
    return llvm::json::Value(std::move(o));
  }
  o["caller"] = c.callerKey;
  o["callee"] = c.calleeKey;
  o["callerName"] = c.callerName;
  o["calleeName"] = c.calleeName;
  o["kind"] = edgeKindToString(c.kind);
  if (c.before)
    o["before"] = serializeSide(*c.before, contexts);
  if (c.after)
    o["after"] = serializeSide(*c.after, contexts);
  return llvm::json::Value(std::move(o));
}

llvm::json::Value serializeScope(const SideScope &s) {
  llvm::json::Object o;
  if (!s.bake.empty())
    o["bake"] = s.bake;
  o["requested"] = static_cast<int64_t>(s.coverage.requested);
  o["indexed"] = static_cast<int64_t>(s.coverage.indexed);
  o["partial"] = static_cast<int64_t>(s.coverage.partial);
  o["failed"] = static_cast<int64_t>(s.coverage.failed);
  o["complete"] = s.coverage.complete();
  if (!s.analyzer.empty())
    o["analyzer"] = s.analyzer;
  if (!s.toolchain.empty())
    o["toolchain"] = s.toolchain;
  return llvm::json::Value(std::move(o));
}

llvm::json::Value serializePath(const std::vector<PathHop> &hops) {
  llvm::json::Array a;
  for (const auto &h : hops)
    a.push_back(serializePathHop(h));
  return llvm::json::Value(std::move(a));
}

llvm::json::Value serializeSearchSide(const PathSearchResult &r) {
  llvm::json::Object o;
  o["pathCount"] = static_cast<int64_t>(r.paths.size());
  o["targetKnown"] = r.targetKnown;
  o["startKnown"] = r.startKnown;
  attachSearchFacts(o, r.stops, r.complete(), r.exhaustive(), r.skippedHubs);
  return llvm::json::Value(std::move(o));
}

} // namespace

llvm::json::Value serializeComparability(const Comparability &c) {
  llvm::json::Object o;
  o["comparable"] = c.comparable;
  o["absenceReliable"] = c.absenceReliable;
  o["analyzerSame"] = c.analyzerSame;
  o["toolchainSame"] = c.toolchainSame;
  o["configSame"] = c.configSame;
  o["contextsCompared"] = c.contextsCompared;
  o["reasons"] = stringArray(c.reasons);
  o["before"] = serializeScope(c.before);
  o["after"] = serializeScope(c.after);
  o["tusOnlyBefore"] = stringArray(c.tusOnlyBefore);
  o["tusOnlyAfter"] = stringArray(c.tusOnlyAfter);
  o["failedBefore"] = stringArray(c.failedBefore);
  o["failedAfter"] = stringArray(c.failedAfter);
  o["partialBefore"] = stringArray(c.partialBefore);
  o["partialAfter"] = stringArray(c.partialAfter);
  return llvm::json::Value(std::move(o));
}

llvm::json::Value serializeSemanticDiff(const SemanticDiffResult &d) {
  const bool contexts = d.comparability.contextsCompared;
  llvm::json::Object o;
  llvm::json::Array changes;
  for (const auto &c : d.changes)
    changes.push_back(serializeChange(c, contexts));
  o["changes"] = std::move(changes);

  llvm::json::Object summary;
  summary["changeCount"] = static_cast<int64_t>(d.changes.size());
  summary["functionsBefore"] = static_cast<int64_t>(d.functionsBefore);
  summary["functionsAfter"] = static_cast<int64_t>(d.functionsAfter);
  summary["relationshipsBefore"] =
      static_cast<int64_t>(d.relationshipsBefore);
  summary["relationshipsAfter"] = static_cast<int64_t>(d.relationshipsAfter);
  summary["functionsRemoved"] =
      static_cast<int64_t>(d.count(ChangeKind::FunctionRemoved));
  summary["functionsAdded"] =
      static_cast<int64_t>(d.count(ChangeKind::FunctionAdded));
  summary["callsRemoved"] =
      static_cast<int64_t>(d.count(ChangeKind::CallRemoved));
  summary["callsAdded"] = static_cast<int64_t>(d.count(ChangeKind::CallAdded));
  summary["callsChanged"] =
      static_cast<int64_t>(d.count(ChangeKind::CallChanged));
  summary["contextsChanged"] =
      static_cast<int64_t>(d.count(ChangeKind::ContextChanged));
  summary["movedFunctions"] = static_cast<int64_t>(d.movedFunctions);
  summary["ambiguousIdentities"] = static_cast<int64_t>(d.ambiguous.size());
  summary["renameCandidates"] =
      static_cast<int64_t>(d.renameCandidates.size());
  o["summary"] = std::move(summary);

  llvm::json::Object identity;
  llvm::json::Array ambiguous;
  for (const auto &a : d.ambiguous) {
    llvm::json::Object ao;
    ao["group"] = a.group;
    ao["reason"] = llvm::StringRef(a.group).starts_with("closure:")
                       ? "anonymous_type_count_changed"
                       : "lambda_count_changed";
    ao["beforeUsrs"] = stringArray(a.beforeUsrs);
    ao["afterUsrs"] = stringArray(a.afterUsrs);
    ao["edgesWithheldBefore"] = static_cast<int64_t>(a.edgesWithheldBefore);
    ao["edgesWithheldAfter"] = static_cast<int64_t>(a.edgesWithheldAfter);
    ambiguous.push_back(llvm::json::Value(std::move(ao)));
  }
  identity["ambiguous"] = std::move(ambiguous);
  llvm::json::Array renames;
  for (const auto &rc : d.renameCandidates) {
    llvm::json::Object ro;
    ro["name"] = rc.name;
    ro["before"] = rc.beforeKey;
    ro["after"] = rc.afterKey;
    ro["beforeFile"] = rc.beforeFile;
    ro["afterFile"] = rc.afterFile;
    renames.push_back(llvm::json::Value(std::move(ro)));
  }
  identity["renameCandidates"] = std::move(renames);
  o["identity"] = std::move(identity);

  llvm::json::Array moves;
  for (const auto &m : d.moves) {
    llvm::json::Object mo;
    mo["function"] = m.key;
    mo["name"] = m.name;
    mo["beforeFile"] = m.beforeFile;
    mo["afterFile"] = m.afterFile;
    mo["beforeLine"] = static_cast<int64_t>(m.beforeLine);
    mo["afterLine"] = static_cast<int64_t>(m.afterLine);
    moves.push_back(llvm::json::Value(std::move(mo)));
  }
  o["moves"] = std::move(moves);
  o["comparability"] = serializeComparability(d.comparability);
  return llvm::json::Value(std::move(o));
}

llvm::json::Value serializeRouteDiff(const RouteDiff &routes,
                                     const std::string &target) {
  llvm::json::Object o;
  o["target"] = target;
  o["before"] = serializeSearchSide(routes.before);
  o["after"] = serializeSearchSide(routes.after);
  llvm::json::Array added, removed;
  for (const auto &p : routes.added)
    added.push_back(serializePath(p.hops));
  for (const auto &p : routes.removed)
    removed.push_back(serializePath(p.hops));
  o["added"] = std::move(added);
  o["removed"] = std::move(removed);
  o["unchanged"] = static_cast<int64_t>(routes.unchanged);
  o["complete"] = routes.complete;
  o["exhaustive"] = routes.exhaustive;
  return llvm::json::Value(std::move(o));
}

llvm::json::Value serializeImpact(const ImpactResult &impact,
                                  const CallGraph &graph,
                                  const std::vector<std::string> &entryPoints,
                                  size_t maxResults, bool includePaths) {
  llvm::json::Object o;
  auto describe = [&](const std::string &usr, llvm::json::Object &into) {
    into["usr"] = usr;
    const CallGraphNode *n = graph.findNode(usr);
    into["name"] = n ? n->qualifiedName : usr;
    if (n) {
      into["file"] = n->file;
      into["line"] = static_cast<int64_t>(n->line);
      into["isEntryPoint"] = n->isEntryPoint;
    }
  };
  std::set<std::string> entrySet(entryPoints.begin(), entryPoints.end());
  auto isEntry = [&](const std::string &usr, const std::string &name) {
    const CallGraphNode *n = graph.findNode(usr);
    return (n && n->isEntryPoint) || entrySet.count(usr) ||
           entrySet.count(name);
  };

  llvm::json::Array changed;
  for (const auto &usr : impact.changed) {
    llvm::json::Object c;
    describe(usr, c);
    changed.push_back(llvm::json::Value(std::move(c)));
  }
  o["changed"] = std::move(changed);
  o["unknown"] = stringArray(impact.unknown);

  llvm::json::Array affected, entries;
  size_t emitted = 0;
  for (const auto &a : impact.affected) {
    const bool entry = isEntry(a.usr, a.name);
    if (entry) {
      llvm::json::Object e;
      e["usr"] = a.usr;
      e["name"] = a.name;
      e["depth"] = static_cast<int64_t>(a.depth);
      entries.push_back(llvm::json::Value(std::move(e)));
    }
    if (maxResults && emitted >= maxResults)
      continue;
    ++emitted;
    llvm::json::Object ao;
    describe(a.usr, ao);
    ao["isEntryPoint"] = entry;
    ao["depth"] = static_cast<int64_t>(a.depth);
    ao["changed"] = a.changedUsr;
    const CallGraphNode *cn = graph.findNode(a.changedUsr);
    ao["changedName"] = cn ? cn->qualifiedName : a.changedUsr;
    if (includePaths)
      ao["path"] = serializePath(a.path);
    affected.push_back(llvm::json::Value(std::move(ao)));
  }
  o["affected"] = std::move(affected);
  o["affectedCount"] = static_cast<int64_t>(impact.affected.size());
  o["truncated"] = maxResults != 0 && impact.affected.size() > maxResults;
  o["entryPointsAffected"] = std::move(entries);
  o["expansions"] = static_cast<int64_t>(impact.expansions);
  attachSearchFacts(o, impact.stops, impact.complete(), impact.exhaustive(),
                    impact.skippedHubs);
  o["note"] = "Affected functions are candidates: each has a call chain "
              "to a changed function within the declared bounds. This is "
              "not proof that their behaviour changes, nor that functions "
              "outside the set are unaffected.";
  return llvm::json::Value(std::move(o));
}

void attachPatchMapping(llvm::json::Object &obj,
                        const PatchMapping &mapping) {
  llvm::json::Object m;
  m["call_site"] = static_cast<int64_t>(mapping.viaCallSite);
  m["definition"] = static_cast<int64_t>(mapping.viaDefinition);
  m["extent"] = static_cast<int64_t>(mapping.viaExtent);
  m["file"] = static_cast<int64_t>(mapping.viaFile);
  m["unmapped"] = static_cast<int64_t>(mapping.unmapped.size());
  m["exact"] = mapping.viaExtent == 0 && mapping.viaFile == 0 &&
               mapping.unmapped.empty();
  obj["mapping"] = std::move(m);
  llvm::json::Array unmapped;
  for (const auto &u : mapping.unmapped) {
    llvm::json::Object uo;
    uo["file"] = u.range.file;
    uo["firstLine"] = static_cast<int64_t>(u.range.firstLine);
    uo["lastLine"] = static_cast<int64_t>(u.range.lastLine);
    uo["deletionOnly"] = u.range.deletionOnly;
    uo["reason"] = u.reason;
    unmapped.push_back(llvm::json::Value(std::move(uo)));
  }
  obj["unmapped"] = std::move(unmapped);
}

// ---------------------------------------------------------------------------
// impact_of_change
// ---------------------------------------------------------------------------

namespace {

llvm::json::Value handleImpactOfChange(const llvm::json::Object &args,
                                       const ToolContext &ctx) {
  std::vector<std::string> changed;
  std::map<std::string, std::string> via; // usr or name -> how it got here

  if (auto *names = args.getArray("changed")) {
    for (const auto &v : *names) {
      auto s = v.getAsString();
      if (!s)
        return usageError("Invalid 'changed': every entry must be a string");
      auto usrs = ctx.graph.usrsForName(s->str());
      if (usrs.size() > 1)
        return makeAmbiguousNameResult(ctx, "changed", *s, std::move(usrs));
      changed.push_back(s->str());
      via[s->str()] = mappingViaName(MappingVia::Argument);
      if (usrs.size() == 1)
        via[usrs.front()] = mappingViaName(MappingVia::Argument);
    }
  }
  if (auto *usrs = args.getArray("changed_usrs")) {
    for (const auto &v : *usrs) {
      auto s = v.getAsString();
      if (!s)
        return usageError(
            "Invalid 'changed_usrs': every entry must be a string");
      changed.push_back(s->str());
      via[s->str()] = mappingViaName(MappingVia::Argument);
    }
  }
  std::optional<PatchMapping> mapping;
  if (auto patch = args.getString("patch")) {
    auto ranges = parseUnifiedDiff(*patch);
    if (ranges.empty())
      return usageError("Invalid 'patch': no unified-diff hunks found "
                        "(expected `git diff` / `diff -u` text)");
    std::string root;
    if (auto pr = args.getString("patch_root"))
      root = pr->str();
    mapping = mapRangesToFunctions(ctx.graph, ranges, root);
    for (const auto &f : mapping->functions) {
      changed.push_back(f.usr);
      via[f.usr] = mappingViaName(f.via);
    }
  }
  if (changed.empty() && !mapping)
    return usageError("Missing required parameter: one of 'changed', "
                      "'changed_usrs', or 'patch'");

  ImpactLimits limits;
  if (auto md = args.getInteger("max_depth")) {
    if (*md < 0)
      return usageError("Invalid max_depth: must be non-negative");
    limits.maxDepth = static_cast<unsigned>(*md);
  }
  if (auto mf = args.getInteger("max_fan_in"))
    limits.maxFanIn = static_cast<size_t>(std::max<int64_t>(0, *mf));
  if (auto mw = args.getInteger("max_work"))
    limits.maxWork = static_cast<size_t>(std::max<int64_t>(0, *mw));
  size_t maxResults = 200;
  // `limit` is an alias (docs/result-contract.md); max_results wins.
  // Errors name the spelling the request used.
  llvm::StringRef resultsKey =
      args.get("max_results") || !args.get("limit") ? "max_results"
                                                     : "limit";
  if (args.get(resultsKey)) {
    auto mr = args.getInteger(resultsKey);
    if (!mr)
      return usageError("Invalid " + resultsKey.str() +
                        ": must be an integer");
    if (*mr < 0)
      return usageError("Invalid " + resultsKey.str() +
                        ": must be non-negative");
    maxResults = static_cast<size_t>(*mr);
  }
  bool includePaths = true;
  if (auto ip = args.getBoolean("include_paths"))
    includePaths = *ip;
  EdgeFilter filter;
  if (auto err = parseEdgeFilter(args, filter))
    return usageError(*err);

  ImpactResult impact = findImpact(
      ctx.graph, changed, limits,
      [&](const CallGraph::EdgeRef &e) { return filter.allowsRef(e); });
  llvm::json::Value payload = serializeImpact(impact, ctx.graph,
                                              ctx.entryPoints, maxResults,
                                              includePaths);
  auto *obj = payload.getAsObject();
  if (auto *changedArr = obj->getArray("changed")) {
    for (auto &entry : *changedArr) {
      auto *eo = entry.getAsObject();
      if (!eo)
        continue;
      auto usr = eo->getString("usr");
      auto name = eo->getString("name");
      auto it = usr ? via.find(usr->str()) : via.end();
      if (it == via.end() && name)
        it = via.find(name->str());
      (*eo)["via"] = it == via.end() ? "argument" : it->second;
    }
  }
  if (mapping)
    attachPatchMapping(*obj, *mapping);
  return payload;
}

} // namespace

void registerImpactTools(std::vector<ToolEntry> &tools) {
  llvm::json::Object props;
  props["changed"] = stringArrayProp(
      "Changed functions by qualified name or USR. A name shared by "
      "several functions returns {ambiguous:true, candidates:[...]}; "
      "re-query with changed_usrs.");
  props["changed_usrs"] = stringArrayProp(
      "Changed functions by exact USR (bypasses name resolution).");
  props["patch"] = stringProp(
      "Unified diff text (`git diff -U0 base head`, `diff -u`). Its "
      "after-side hunks are mapped to functions through the indexed call "
      "sites and function locations; `mapping` reports how precisely "
      "(call_site / definition exact, extent / file estimated) and "
      "`unmapped` lists the hunks the index could not place. The CLI "
      "reads it from --patch-file F (or -), or runs git for you with "
      "--git-base A --git-head B [--repo DIR].");
  props["patch_root"] = stringProp(
      "Directory prepended to the patch's paths before matching them to "
      "the index's file spellings (default: match by path suffix).");
  props["max_depth"] = intProp(
      "Maximum distance in edges from a changed function (default: 10; "
      "0 = unlimited). Callers beyond it set stopReasons depth_limit.");
  props["max_results"] = intProp(
      "Maximum affected functions to list, after ordering by (depth, usr) "
      "(default: 200; 0 = all). affectedCount is the full count and "
      "truncated says whether the list was cut. Alias: 'limit'.");
  props["max_fan_in"] = intProp(
      "Do not expand the callers of an affected function with more "
      "stored callers than this (high-fan-in hubs; the changed functions "
      "themselves are always expanded); skipped hubs are listed and "
      "clear `complete`. 0 disables (default: 1000).");
  props["max_work"] = intProp(
      "Maximum node expansions (default: 200000; 0 = unlimited). "
      "Exceeding it sets work_budget and clears `complete`.");
  props["include_paths"] = boolProp(
      "Emit the witness path (affected -> ... -> changed) per affected "
      "function (default: true).");
  props["edge_kinds"] = stringArrayProp(
      "Follow only caller edges whose kind is in this set.");
  props["min_confidence"] = stringProp(
      "Inclusive minimum confidence tier of every edge followed "
      "(default: Unknown).");
  props["include_confidences"] = stringArrayProp(
      "Explicit set of confidence tiers followed. Overrides "
      "min_confidence.");
  props["execution_contexts"] = stringArrayProp(
      "Follow only edges whose execution context is in this set "
      "(Synchronous, ThreadSpawn, AsyncTask, PackagedTask, Invoke).");
  llvm::json::Object schema;
  schema["type"] = "object";
  schema["properties"] = std::move(props);

  tools.push_back(
      {"impact_of_change",
       "Callers affected by a change: the functions with a call chain to "
       "any changed function within max_depth, each at its shallowest "
       "depth with a witness path, plus the entry points among them. The "
       "changed set comes from names/USRs, or from a unified diff mapped "
       "onto the index (see `patch`). A candidate set, not proof: "
       "complete/exhaustive/stopReasons say how much of the caller graph "
       "was walked (docs/change-impact.md).",
       llvm::json::Value(std::move(schema)), handleImpactOfChange});
}

} // namespace vycor
