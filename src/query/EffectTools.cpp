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
#include "Paging.h"
#include "Registry.h"
#include "Schema.h"

#include "vycor/callgraph/HotPath.h"
#include "vycor/ext/Extensions.h"
#include "vycor/query/Serialize.h"

// `query_path_effects` (docs/checks/hot-path-effects.md): allocation,
// locks, throws and I/O reachable from hot roots, with a witness path per
// effect call, weighted and classified by a profile when one is given.

namespace vycor {

namespace {

using Id = StringInterner::Id;

llvm::json::Value countJson(int64_t count) {
  return count == CoverageProfile::kNoCount ? llvm::json::Value(nullptr)
                                            : llvm::json::Value(count);
}

llvm::json::Array rootSourcesJson(uint8_t sources) {
  llvm::json::Array out;
  if (sources & kHotRootConfig)
    out.push_back("config");
  if (sources & kHotRootProfile)
    out.push_back("profile");
  return out;
}

llvm::json::Object findingJson(const HotPathReport &report,
                               const HotPathFinding &finding,
                               const CallGraph &graph) {
  const StringInterner &strings = graph.interner();
  llvm::ArrayRef<HotPathHop> path = report.path(finding);
  const HotPathHop &effect = path.back();
  const Id rootId = path.front().caller;

  llvm::json::Array hops;
  for (const HotPathHop &hop : path) {
    llvm::json::Object h;
    h["caller"] = graph.displayNameOf(hop.caller);
    h["callerUsr"] = strings.resolve(hop.caller);
    h["callee"] = graph.displayNameOf(hop.callee);
    h["calleeUsr"] = strings.resolve(hop.callee);
    h["callSite"] = strings.resolve(hop.callSite);
    h["kind"] = edgeKindToString(hop.kind);
    h["confidence"] = confidenceToString(hop.confidence);
    h["count"] = countJson(hop.count);
    h["calleeCount"] = countJson(hop.calleeCount);
    hops.push_back(std::move(h));
  }

  llvm::json::Object out;
  out["effect"] = effectKindName(finding.effect).str();
  out["evidence"] = hotEvidenceName(finding.evidence);
  out["weight"] = countJson(finding.weight);
  out["function"] = graph.displayNameOf(effect.caller);
  out["usr"] = strings.resolve(effect.caller);
  out["callee"] = graph.displayNameOf(effect.callee);
  out["calleeUsr"] = strings.resolve(effect.callee);
  out["callSite"] = strings.resolve(effect.callSite);
  out["count"] = countJson(effect.count);
  out["root"] = graph.displayNameOf(rootId);
  out["rootUsr"] = strings.resolve(rootId);
  out["rootSources"] = rootSourcesJson(finding.rootSources);
  out["rootCount"] = countJson(finding.rootCount);
  out["confidence"] = confidenceToString(finding.confidence);
  out["depth"] = static_cast<int64_t>(path.size());
  out["path"] = std::move(hops);
  return out;
}

// Appends the strings of array argument `key` to `out`; a usage error
// when it is not an array of strings.
std::optional<llvm::json::Value> appendStrings(const llvm::json::Object &args,
                                               llvm::StringRef key,
                                               std::vector<std::string> &out) {
  const llvm::json::Value *value = args.get(key);
  if (!value)
    return std::nullopt;
  const llvm::json::Array *array = value->getAsArray();
  if (!array)
    return usageError("Invalid '" + key.str() +
                      "': expected an array of strings");
  for (const llvm::json::Value &entry : *array) {
    auto s = entry.getAsString();
    if (!s)
      return usageError("Invalid '" + key.str() +
                        "': every entry must be a string");
    out.push_back(s->str());
  }
  return std::nullopt;
}

llvm::json::Value handlePathEffects(const llvm::json::Object &args,
                                    const ToolContext &ctx) {
  Page page;
  if (auto error = parsePage(args, kDefaultListLimit, page))
    return usageError(*error);

  // The org config's roots and allow list, plus the call's.
  HotPathConfig config = ExtensionRegistry::instance().hotPathConfig();
  if (auto invalid = appendStrings(args, "roots", config.roots))
    return std::move(*invalid);
  if (auto invalid = appendStrings(args, "root_patterns", config.rootPatterns))
    return std::move(*invalid);
  if (auto invalid = appendStrings(args, "allow", config.allow))
    return std::move(*invalid);
  const llvm::json::Value *minCountArg = args.get("min_count");
  if (minCountArg) {
    auto n = minCountArg->getAsInteger();
    if (!n || *n < 0)
      return usageError("Invalid 'min_count': expected a nonnegative integer");
    config.minCount = static_cast<uint64_t>(*n);
  }

  uint8_t effectMask = kAllEffects;
  std::vector<std::string> effectNames;
  if (auto invalid = appendStrings(args, "effects", effectNames))
    return std::move(*invalid);
  if (args.get("effects") && effectNames.empty())
    return usageError("Invalid 'effects': name at least one effect "
                      "(allocation, lock, io, throw), or omit it for all");
  if (!effectNames.empty()) {
    effectMask = 0;
    for (const std::string &name : effectNames) {
      auto kind = effectKindFromName(name);
      if (!kind)
        return usageError("Invalid 'effects': unknown effect '" + name +
                          "' (expected allocation, lock, io or throw)");
      effectMask |= effectBit(*kind);
    }
  }

  std::optional<CoverageProfile> profile;
  std::string profilePath;
  if (auto path = args.getString("profile")) {
    profilePath = path->str();
    profile.emplace();
    std::string error;
    if (!CoverageProfile::load(profilePath, *profile, error))
      return usageError("Invalid 'profile': " + error);
  }
  // The org config's minCount applies only with a profile; an explicit
  // min_count without one is a mistake.
  if (!profile) {
    if (minCountArg && config.minCount != 0)
      return usageError("Invalid 'min_count': takes roots from a profile, "
                        "and no 'profile' was given");
    config.minCount = 0;
  }
  if (config.roots.empty() && config.rootPatterns.empty() &&
      config.minCount == 0)
    return usageError(
        "No hot roots: pass 'roots' or 'root_patterns', or 'profile' with "
        "'min_count', or configure hotPaths in the org config "
        "(docs/checks/hot-path-effects.md)");

  HotPathReport report;
  std::string error;
  if (!analyzeHotPaths(ctx.graph, ExtensionRegistry::instance().effectTable(),
                       config, profile ? &*profile : nullptr, effectMask,
                       report, error))
    return usageError("Invalid root pattern: " + error);

  llvm::json::Object out;
  if (profile) {
    const HotPathJoin &j = report.join;
    llvm::json::Object join;
    join["callSites"] = static_cast<int64_t>(j.callSites);
    join["callSitesInProfile"] = static_cast<int64_t>(j.callSitesInProfile);
    join["callSitesCounted"] = static_cast<int64_t>(j.callSitesCounted);
    join["functions"] = static_cast<int64_t>(j.functions);
    join["functionsCounted"] = static_cast<int64_t>(j.functionsCounted);
    llvm::json::Object p;
    p["path"] = profilePath;
    p["files"] = static_cast<int64_t>(profile->fileCount());
    p["functions"] = static_cast<int64_t>(profile->functionCount());
    p["executedFunctions"] =
        static_cast<int64_t>(profile->executedFunctionCount());
    p["join"] = std::move(join);
    out["profile"] = std::move(p);
  } else {
    out["profile"] = nullptr;
  }

  llvm::json::Object roots;
  roots["total"] = static_cast<int64_t>(report.roots);
  roots["config"] = static_cast<int64_t>(report.configRoots);
  roots["profile"] = static_cast<int64_t>(report.profileRoots);
  roots["minCount"] = static_cast<int64_t>(config.minCount);
  llvm::json::Array unmatched;
  for (const std::string &name : report.unmatchedRoots)
    unmatched.push_back(name);
  roots["unmatched"] = std::move(unmatched);
  out["roots"] = std::move(roots);

  llvm::json::Array effects;
  for (EffectKind kind : kEffectKinds)
    if (effectMask & effectBit(kind))
      effects.push_back(effectKindName(kind).str());
  out["effects"] = std::move(effects);

  llvm::json::Object allow;
  allow["functions"] = static_cast<int64_t>(config.allow.size());
  allow["calls"] = static_cast<int64_t>(report.allowedCalls);
  out["allow"] = std::move(allow);
  out["reachedFunctions"] = static_cast<int64_t>(report.reachedFunctions);
  out["templateLeaves"] = static_cast<int64_t>(report.templateLeaves);

  llvm::json::Object summary;
  for (unsigned e = 0; e < kHotEvidenceCount; ++e) {
    llvm::json::Object byEffect;
    for (EffectKind kind : kEffectKinds)
      if (effectMask & effectBit(kind))
        byEffect[effectKindName(kind)] = static_cast<int64_t>(
            report.counts[e][static_cast<unsigned>(kind)]);
    summary[hotEvidenceName(static_cast<HotEvidence>(e))] =
        std::move(byEffect);
  }
  out["summary"] = std::move(summary);

  // The search visits everything reachable, so "no effect reachable" is
  // universal once there were roots, the index covers what was asked, and
  // no reached template instantiation hid its calls (#106). An index
  // baked with --collapse-paths dropped the edges inside those paths, so
  // what it reaches is not everything the program reaches.
  const bool collapsed = ctx.meta && !ctx.meta->collapsePaths.empty();
  const bool exhaustive = report.templateLeaves == 0 && !collapsed;
  out["collapsed"] = collapsed;
  out["exhaustive"] = exhaustive;
  const char *verdict = "unknown";
  if (!report.findings.empty())
    verdict = "effects";
  else if (report.roots != 0 && exhaustive && report.unmatchedRoots.empty() &&
           ctx.facts.coversRequested())
    verdict = "clean";
  out["verdict"] = verdict;

  llvm::json::Array findings;
  const size_t total = report.findings.size();
  for (size_t i = page.begin(total), e = page.end(total); i < e; ++i)
    findings.push_back(findingJson(report, report.findings[i], ctx.graph));
  out["findings"] = std::move(findings);
  attachPage(out, page, total);
  return out;
}

} // namespace

void registerEffectTools(std::vector<ToolEntry> &tools) {
  llvm::json::Object props;
  props["roots"] = stringArrayProp(
      "Hot roots: qualified names (every overload and instantiation of the "
      "name) or USRs. Added to the org config's hotPaths.roots.");
  props["root_patterns"] = stringArrayProp(
      "POSIX extended regexes searched in qualified names; every match is "
      "a root. Added to the org config's hotPaths.rootPatterns.");
  props["profile"] = stringProp(
      "Path of an `llvm-cov export` JSON document from a run of the "
      "program (source-based coverage). Weights every path by its rarest "
      "call and sorts findings into observed, unprofiled and latent.");
  props["min_count"] = intProp(
      "With profile: every function entered at least this often is a root. "
      "Overrides the org config's hotPaths.minCount; 0 takes no roots from "
      "the profile.");
  props["effects"] = stringArrayProp(
      "Effects to report: allocation, lock, io, throw (default: all).");
  props["allow"] = stringArrayProp(
      "Accepted functions (qualified names or USRs): a call of one is "
      "neither reported nor followed. Added to the org config's "
      "hotPaths.allow.");
  addPagingProps(props, kDefaultListLimit, "findings");

  llvm::json::Object schema;
  schema["type"] = "object";
  schema["properties"] = std::move(props);

  tools.push_back(
      {"query_path_effects",
       "Allocation, lock acquisition, I/O and throws reachable from hot "
       "roots (named, matched by pattern, or taken from a profile). One "
       "finding per effect call, with a witness path from a root (the "
       "nearest, or with a profile the root of the heaviest path); with a "
       "profile, `weight` is the smallest call-site or entry count on the "
       "path and `evidence` says whether the path ran (observed), "
       "never ran (latent) or is not covered by the profile (unprofiled). "
       "`verdict` is `clean` only with roots, every configured root "
       "matched, no finding, complete coverage, an index baked without "
       "collapse paths (`collapsed`) and no `templateLeaves` (reached template instantiations "
       "whose calls the graph does not hold). See "
       "docs/checks/hot-path-effects.md.",
       llvm::json::Value(std::move(schema)), handlePathEffects, "findings",
       kSectionGraph});
}

} // namespace vycor
