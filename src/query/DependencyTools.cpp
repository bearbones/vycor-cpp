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

#include "vycor/callgraph/ReferenceIndex.h"
#include "vycor/deps/DependencyAudit.h"
#include "vycor/query/Tools.h"

#include "Paging.h"
#include "Registry.h"
#include "Schema.h"

#include "llvm/Support/Chrono.h"
#include "llvm/Support/FileSystem.h"

#include <algorithm>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

// The dependency audit tools (docs/dependency-audit.md): thin adapters
// over deps/DependencyAudit.h. The analysis of one (index, target map)
// pair is computed once and cached; every argument variation filters,
// sorts and pages the finished rows.

namespace vycor {

namespace {

constexpr size_t kDefaultClosureSample = 50;

struct FindingName {
  unsigned bit;
  const char *id;
};

// In the order a row lists them.
constexpr FindingName kFindingNames[] = {
    {kFindingUnused, "unused-link-dependency"},
    {kFindingUndeclared, "undeclared-link-dependency"},
    {kFindingThin, "thin-dependency"},
};

/// One analysis of the loaded index against one target-map file.
struct AuditAnalysis {
  TargetMap map;
  LinkFacts facts;
  DependencyAudit audit;
  uint64_t fileSize = 0;
  llvm::sys::TimePoint<> fileTime;
};

/// Loads the map and runs the audit, or answers from the cache when the
/// map file is unchanged. On failure returns null with `error` set to the
/// usage-error payload.
std::shared_ptr<const AuditAnalysis>
analysisFor(const llvm::json::Object &args, const ToolContext &ctx,
            llvm::json::Value &error) {
  auto path = args.getString("target_map");
  if (!path || path->empty()) {
    error = usageError(
        "Missing required argument: target_map (the target-map JSON a "
        "build adapter wrote; see docs/dependency-audit.md)");
    return nullptr;
  }
  llvm::sys::fs::file_status st;
  uint64_t size = 0;
  llvm::sys::TimePoint<> mtime;
  if (!llvm::sys::fs::status(*path, st)) {
    size = st.getSize();
    mtime = st.getLastModificationTime();
  }
  const std::string key = ("dependency_audit|" + *path).str();
  if (ctx.cache) {
    auto it = ctx.cache->objects.find(key);
    if (it != ctx.cache->objects.end()) {
      auto cached = std::static_pointer_cast<const AuditAnalysis>(it->second);
      if (cached->fileSize == size && cached->fileTime == mtime)
        return cached;
    }
  }
  auto analysis = std::make_shared<AuditAnalysis>();
  std::string detail;
  if (!loadTargetMap(*path, analysis->map, detail)) {
    error = usageError(detail);
    return nullptr;
  }
  // The reference facts when the index holds them; an index baked before
  // them (or a .mgsc one) falls back to the call edges.
  if (ctx.references && !ctx.references->empty())
    analysis->facts = linkFactsFromReferences(*ctx.references, &ctx.graph,
                                              analysis->map, ctx.meta);
  else
    analysis->facts =
        linkFactsFromCallEdges(ctx.graph, analysis->map, ctx.meta);
  analysis->audit = auditDependencies(analysis->facts, analysis->map);
  analysis->fileSize = size;
  analysis->fileTime = mtime;
  if (ctx.cache)
    ctx.cache->objects.insert_or_assign(
        key, std::static_pointer_cast<void>(analysis));
  return analysis;
}

/// The `completeness` member every payload carries: what the evidence
/// behind the rows can and cannot see, and how much of the map the index
/// covers. `unusedIsUniversal` is constant false at this stage.
llvm::json::Value completenessJson(const AuditAnalysis &a,
                                   const ToolContext &ctx) {
  const LinkFacts &lf = a.facts;
  llvm::json::Object obj;
  obj["edgeSource"] = lf.edgeSource;
  obj["unusedIsUniversal"] = false;
  llvm::json::Array invisible;
  for (const char *tag : lf.invisible)
    invisible.push_back(tag);
  obj["invisible"] = std::move(invisible);
  obj["depsForm"] = a.map.depsAreClosure ? "closure" : "direct";
  obj["coverageStated"] = ctx.facts.stated() && lf.hasIndexMeta;
  obj["edgesCollapsed"] = lf.edgesCollapsed;

  uint32_t withSources = 0, covered = 0;
  for (uint32_t t = 0; t < lf.targetCount; ++t) {
    withSources += lf.targetSources[t] != 0;
    covered += lf.targetCovered[t] != 0;
  }
  llvm::json::Object targets;
  targets["total"] = static_cast<int64_t>(lf.targetCount);
  targets["withSources"] = static_cast<int64_t>(withSources);
  targets["covered"] = static_cast<int64_t>(covered);
  obj["targets"] = std::move(targets);

  const auto &c = lf.counts;
  llvm::json::Object sources;
  sources["mapped"] = static_cast<int64_t>(c.mappedSources);
  sources["indexed"] = static_cast<int64_t>(c.indexedSources);
  sources["failed"] = static_cast<int64_t>(c.failedSources);
  sources["notIndexed"] = static_cast<int64_t>(
      c.mappedSources - c.indexedSources - c.failedSources);
  sources["unmappedTus"] = static_cast<int64_t>(c.unmappedTus);
  obj["sources"] = std::move(sources);

  llvm::json::Object symbols;
  symbols["defined"] = static_cast<int64_t>(c.defined);
  symbols["presumed"] = static_cast<int64_t>(c.presumed);
  symbols["inline"] = static_cast<int64_t>(c.inlined);
  symbols["unattributed"] = static_cast<int64_t>(c.unattributed);

  llvm::json::Object refs;
  refs["anchored"] = static_cast<int64_t>(c.anchoredRefs);
  refs["inlineBody"] = static_cast<int64_t>(c.inlineBodyRefs);
  refs["unmappedBody"] = static_cast<int64_t>(c.unmappedBodyRefs);
  refs["virtualDispatchSkipped"] = static_cast<int64_t>(c.skippedVirtualCalls);
  refs["deferredSkipped"] = static_cast<int64_t>(c.skippedDeferred);
  refs["unresolvedSite"] = static_cast<int64_t>(c.unresolvedSites);
  if (lf.ownedStrings) { // the reference-facts producer's own counts
    refs["fileScopeDropped"] = static_cast<int64_t>(c.fileScopeDropped);
    refs["vtableEntries"] = static_cast<int64_t>(c.vtableEntryRefs);
    refs["baseClasses"] = static_cast<int64_t>(c.baseClassRefs);
    refs["inheritedVirtuals"] = static_cast<int64_t>(c.inheritedVirtualRefs);
    refs["baseClassesUnseen"] = static_cast<int64_t>(c.baseClassUnseen);
    refs["overloadCandidates"] =
        static_cast<int64_t>(c.overloadCandidateRefs);
    refs["siteAnchored"] = static_cast<int64_t>(c.siteAnchoredRefs);
    symbols["undefinedInCoveredOwner"] =
        static_cast<int64_t>(c.undefinedInCoveredOwner);
    symbols["emittedEverywhere"] = static_cast<int64_t>(c.emittedDefinitions);
  }
  obj["symbols"] = std::move(symbols);
  obj["references"] = std::move(refs);
  return llvm::json::Value(std::move(obj));
}

llvm::json::Value targetMapJson(const AuditAnalysis &a,
                                llvm::StringRef path) {
  llvm::json::Object obj;
  obj["path"] = path.str();
  obj["generator"] = a.map.generator;
  obj["depsForm"] = a.map.depsAreClosure ? "closure" : "direct";
  obj["targets"] = static_cast<int64_t>(a.map.size());
  llvm::json::Array roots;
  for (const std::string &r : a.map.roots)
    roots.push_back(r);
  obj["roots"] = std::move(roots);
  return llvm::json::Value(std::move(obj));
}

const std::string &symbolName(const LinkFacts &lf, uint32_t sym) {
  if (const CallGraphNode *node = lf.symNode[sym])
    return node->qualifiedName;
  if (!lf.symName.empty())
    return lf.strings->resolve(lf.symName[sym]);
  return lf.strings->resolve(lf.symUsr[sym]); // an endpoint without a node
}

llvm::json::Object symbolJson(const LinkFacts &lf, uint32_t sym) {
  llvm::json::Object obj;
  obj["name"] = symbolName(lf, sym);
  obj["usr"] = lf.strings->resolve(lf.symUsr[sym]);
  if (!lf.symKind.empty())
    obj["kind"] = symbolKindName(static_cast<SymbolKind>(lf.symKind[sym]));
  if (const CallGraphNode *node = lf.symNode[sym]) {
    obj["file"] = node->file;
    obj["line"] = static_cast<int64_t>(node->line);
  } else if (!lf.symDeclLine.empty() && lf.symDeclFile[sym] != kNoIndex) {
    obj["file"] = lf.files[lf.symDeclFile[sym]];
    obj["line"] = static_cast<int64_t>(lf.symDeclLine[sym]);
  }
  return obj;
}

/// The symbol holding reference `ref` (references are CSR by holder).
uint32_t holderOf(const LinkFacts &lf, uint32_t ref) {
  auto it = std::upper_bound(lf.refBegin.begin(), lf.refBegin.end(), ref);
  return static_cast<uint32_t>(it - lf.refBegin.begin()) - 1;
}

llvm::json::Object witnessJson(const LinkFacts &lf, uint32_t ref) {
  llvm::json::Object obj;
  obj["site"] = lf.strings->resolve(lf.refSite[ref]);
  obj["from"] = symbolName(lf, holderOf(lf, ref));
  obj["to"] = symbolName(lf, lf.refTo[ref]);
  obj["kind"] = lf.refKindName(lf.refKind[ref]);
  return obj;
}

llvm::json::Array targetNames(const TargetMap &map,
                              const std::vector<uint32_t> &list,
                              uint32_t begin, uint32_t end) {
  llvm::json::Array out;
  for (uint32_t i = begin; i < end; ++i)
    out.push_back(map.names[list[i]]);
  return out;
}

/// Share of the provider the consumer does not need, in whole percent
/// (rounded down): 100 * (1 - closure / provider).
int64_t separabilityPercent(uint32_t closure, uint32_t provider) {
  if (provider == 0)
    return 0;
  return static_cast<int64_t>(
      (static_cast<uint64_t>(provider - std::min(closure, provider)) * 100) /
      provider);
}

llvm::json::Object rowJson(const AuditAnalysis &a, const AuditRow &row,
                           unsigned findings) {
  const LinkFacts &lf = a.facts;
  const DependencyAudit &audit = a.audit;
  const uint32_t providerSymbols = audit.providerSymbols[row.provider];
  llvm::json::Object obj;
  obj["consumer"] = a.map.names[row.consumer];
  obj["provider"] = a.map.names[row.provider];
  obj["providerKind"] = targetKindName(a.map.kinds[row.provider]);
  obj["declared"] = declaredFormName(row.declared);
  obj["symbols"] = static_cast<int64_t>(row.symbols);
  obj["uncertainSymbols"] = static_cast<int64_t>(row.uncertainSymbols);
  obj["viaInline"] = static_cast<int64_t>(row.viaInline);
  obj["indirectUses"] = static_cast<int64_t>(row.indirectUses);
  obj["consumerUnplacedUses"] =
      static_cast<int64_t>(audit.unplacedUses[row.consumer]);
  obj["closureSymbols"] = static_cast<int64_t>(row.closureSymbols);
  obj["providerSymbols"] = static_cast<int64_t>(providerSymbols);
  obj["providerPresumedSymbols"] =
      static_cast<int64_t>(audit.presumedSymbols[row.provider]);
  obj["closureFiles"] = static_cast<int64_t>(row.closureFiles);
  obj["providerFiles"] =
      static_cast<int64_t>(audit.providerFiles[row.provider]);
  obj["separabilityPct"] =
      separabilityPercent(row.closureSymbols, providerSymbols);
  obj["closureNeeds"] =
      targetNames(a.map, audit.needs, row.needBegin, row.needEnd);
  obj["onlyViaEdge"] = static_cast<int64_t>(row.onlyViaEdge);
  obj["droppableTargets"] =
      targetNames(a.map, audit.droppable, row.dropBegin, row.dropEnd);
  obj["droppableSymbols"] = static_cast<int64_t>(row.droppableSymbols);
  obj["providerDroppable"] = row.providerDroppable;
  obj["linkTargetsHelped"] = static_cast<int64_t>(row.linkTargetsHelped);
  obj["linkTargetGain"] = static_cast<int64_t>(row.linkTargetGain);
  obj["closureFanIn"] = static_cast<int64_t>(row.closureFanIn);
  obj["closureFanInFiles"] = static_cast<int64_t>(row.closureFanInFiles);
  obj["registrationSeeds"] = static_cast<int64_t>(row.registrationSeeds);
  obj["providerStaticInits"] =
      static_cast<int64_t>(audit.providerStaticInits[row.provider]);
  obj["providesMain"] = row.providesMain;
  obj["consumerCovered"] = lf.targetCovered[row.consumer] != 0;
  obj["providerCovered"] = lf.targetCovered[row.provider] != 0;
  if (row.seedBegin != row.seedEnd)
    obj["witness"] = witnessJson(lf, audit.seedRef[row.seedBegin]);
  llvm::json::Array list;
  for (const FindingName &f : kFindingNames) {
    if (!(findings & f.bit))
      continue;
    // No finding here is a verdict over every way one target can need
    // another: `completeness` lists what the evidence cannot see.
    llvm::json::Object finding;
    finding["id"] = f.id;
    finding["evidence"] = lf.edgeSource;
    finding["universal"] = false;
    if (f.bit == kFindingUndeclared && row.throughInlineRef != kNoIndex) {
      // The consumer names no provider symbol itself: every one is reached
      // through an inline body of a target it does declare.
      const uint32_t inlineSym = lf.refTo[row.throughInlineRef];
      llvm::json::Object through;
      through["symbol"] = symbolName(lf, inlineSym);
      if (lf.symDeclFile[inlineSym] != kNoIndex)
        through["header"] = lf.files[lf.symDeclFile[inlineSym]];
      llvm::json::Array owners;
      for (uint32_t t : lf.targetsOf(lf.symOwner[inlineSym]))
        owners.push_back(a.map.names[t]);
      through["owners"] = std::move(owners);
      through["site"] = lf.strings->resolve(lf.refSite[row.throughInlineRef]);
      finding["throughInline"] = std::move(through);
    }
    if (f.bit == kFindingThin && row.symbols != 0 &&
        row.registrationSeeds == row.symbols)
      // Every certain seed hands a factory to a registry: what the
      // consumer really uses is reached at run time, past the closure.
      finding["registration"] = true;
    if (f.bit == kFindingUnused && audit.providerStaticInits[row.provider])
      // Linking the provider runs these; a consumer may rely on that
      // without naming any symbol.
      finding["providerStaticInits"] =
          static_cast<int64_t>(audit.providerStaticInits[row.provider]);
    list.push_back(std::move(finding));
  }
  obj["findings"] = std::move(list);
  return obj;
}

bool readThresholds(const llvm::json::Object &args, ThinThresholds &out,
                    llvm::json::Value &error) {
  auto read = [&](llvm::StringRef key, int64_t max, uint32_t &slot) {
    if (!args.get(key))
      return true;
    auto v = args.getInteger(key);
    if (!v || *v < 0 || *v > max) {
      error = usageError(("Invalid " + key + ": must be an integer in [0, " +
                          std::to_string(max) + "]")
                             .str());
      return false;
    }
    slot = static_cast<uint32_t>(*v);
    return true;
  };
  return read("thin_max_symbols", 1000000, out.maxSymbols) &&
         read("thin_max_closure_pct", 100, out.maxClosurePercent);
}

/// Resolves an optional target-name argument. Returns false with `error`
/// set when the name is given but the map does not declare it.
bool readTarget(const llvm::json::Object &args, llvm::StringRef key,
                const TargetMap &map, std::optional<uint32_t> &out,
                llvm::json::Value &error) {
  const llvm::json::Value *v = args.get(key);
  if (!v)
    return true;
  auto name = v->getAsString();
  if (!name || name->empty()) {
    error = usageError(("Invalid " + key + ": must be a target name").str());
    return false;
  }
  out = map.find(*name);
  if (!out) {
    error = notFoundError(
        ("The target map declares no target named '" + *name + "'").str());
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// audit_dependencies
// ---------------------------------------------------------------------------

llvm::json::Value handleAuditDependencies(const llvm::json::Object &args,
                                          const ToolContext &ctx) {
  llvm::json::Value error(nullptr);
  auto analysis = analysisFor(args, ctx, error);
  if (!analysis)
    return error;
  const AuditAnalysis &a = *analysis;

  std::optional<uint32_t> consumer, provider;
  ThinThresholds thresholds;
  if (!readTarget(args, "consumer", a.map, consumer, error) ||
      !readTarget(args, "provider", a.map, provider, error) ||
      !readThresholds(args, thresholds, error))
    return error;

  unsigned wanted = 0; // 0: every row
  if (const llvm::json::Value *v = args.get("finding")) {
    auto name = v->getAsString();
    if (name && *name == "any") {
      wanted = kFindingUnused | kFindingUndeclared | kFindingThin;
    } else {
      for (const FindingName &f : kFindingNames)
        if (name && *name == f.id)
          wanted = f.bit;
      if (wanted == 0)
        return usageError("Invalid finding: must be unused-link-dependency, "
                          "undeclared-link-dependency, thin-dependency, or "
                          "any");
    }
  }
  enum class Sort { Name, Gain, LinkGain } sort = Sort::Name;
  if (const llvm::json::Value *v = args.get("sort")) {
    auto name = v->getAsString();
    if (name && *name == "gain")
      sort = Sort::Gain;
    else if (name && *name == "link_gain")
      sort = Sort::LinkGain;
    else if (!name || *name != "name")
      return usageError("Invalid sort: must be name, gain or link_gain");
  }
  Page page;
  if (auto bad = parsePage(args, kDefaultListLimit, page))
    return usageError(*bad);

  const bool coverageStated =
      ctx.facts.stated() && a.facts.absenceIsEvidence();
  const auto &rows = a.audit.rows;
  std::vector<uint32_t> selected;
  std::vector<unsigned> findingsOf(rows.size(), 0);
  int64_t counts[std::size(kFindingNames)] = {};
  int64_t unusedWithheld = 0;
  for (uint32_t i = 0; i < rows.size(); ++i) {
    const AuditRow &row = rows[i];
    if ((consumer && row.consumer != *consumer) ||
        (provider && row.provider != *provider))
      continue;
    const unsigned f =
        rowFindings(a.audit, a.facts, row, thresholds, coverageStated);
    findingsOf[i] = f;
    for (size_t k = 0; k < std::size(kFindingNames); ++k)
      counts[k] += (f & kFindingNames[k].bit) != 0;
    // A declared dependency with no observed use and no finding: coverage
    // of one side is incomplete or unstated, the consumer uses the
    // provider indirectly, or the index holds no symbol of the provider.
    if (!(f & kFindingUnused) && row.declared == DeclaredForm::Direct &&
        row.symbols == 0 && row.uncertainSymbols == 0)
      ++unusedWithheld;
    if (wanted == 0 || (f & wanted))
      selected.push_back(i);
  }
  // Rows are already in (consumer, provider) order; a stable sort keeps
  // that as the tie-break.
  if (sort == Sort::Gain)
    std::stable_sort(selected.begin(), selected.end(),
                     [&](uint32_t x, uint32_t y) {
                       const AuditRow &p = rows[x], &q = rows[y];
                       if (p.droppableSymbols != q.droppableSymbols)
                         return p.droppableSymbols > q.droppableSymbols;
                       return p.closureSymbols < q.closureSymbols;
                     });
  else if (sort == Sort::LinkGain)
    std::stable_sort(selected.begin(), selected.end(),
                     [&](uint32_t x, uint32_t y) {
                       const AuditRow &p = rows[x], &q = rows[y];
                       if (p.linkTargetGain != q.linkTargetGain)
                         return p.linkTargetGain > q.linkTargetGain;
                       if (p.linkTargetsHelped != q.linkTargetsHelped)
                         return p.linkTargetsHelped > q.linkTargetsHelped;
                       return p.closureSymbols < q.closureSymbols;
                     });

  llvm::json::Array edges;
  for (size_t i = page.begin(selected.size()), e = page.end(selected.size());
       i < e; ++i)
    edges.push_back(rowJson(a, rows[selected[i]], findingsOf[selected[i]]));

  llvm::json::Object obj;
  obj["targetMap"] = targetMapJson(a, *args.getString("target_map"));
  obj["completeness"] = completenessJson(a, ctx);
  llvm::json::Object thin;
  thin["maxSymbols"] = static_cast<int64_t>(thresholds.maxSymbols);
  thin["maxClosurePct"] = static_cast<int64_t>(thresholds.maxClosurePercent);
  obj["thinThresholds"] = std::move(thin);
  llvm::json::Object findingCounts;
  for (size_t k = 0; k < std::size(kFindingNames); ++k)
    findingCounts[kFindingNames[k].id] = counts[k];
  obj["findingCounts"] = std::move(findingCounts);
  obj["unusedWithheld"] = unusedWithheld;
  obj["sort"] = sort == Sort::Gain       ? "gain"
                : sort == Sort::LinkGain ? "link_gain"
                                         : "name";
  attachPage(obj, page, selected.size());
  obj["edges"] = std::move(edges);
  return llvm::json::Value(std::move(obj));
}

// ---------------------------------------------------------------------------
// explain_dependency
// ---------------------------------------------------------------------------

llvm::json::Value handleExplainDependency(const llvm::json::Object &args,
                                          const ToolContext &ctx) {
  llvm::json::Value error(nullptr);
  auto analysis = analysisFor(args, ctx, error);
  if (!analysis)
    return error;
  const AuditAnalysis &a = *analysis;
  const LinkFacts &lf = a.facts;

  std::optional<uint32_t> consumer, provider;
  ThinThresholds thresholds;
  if (!readTarget(args, "consumer", a.map, consumer, error) ||
      !readTarget(args, "provider", a.map, provider, error) ||
      !readThresholds(args, thresholds, error))
    return error;
  if (!consumer || !provider)
    return usageError("Missing required argument: consumer and provider "
                      "(target names from the target map)");
  if (*consumer == *provider)
    return usageError("consumer and provider must be different targets");
  size_t closureLimit = kDefaultClosureSample;
  if (args.get("closure_limit")) {
    auto v = args.getInteger("closure_limit");
    if (!v || *v < 0)
      return usageError("Invalid closure_limit: must be a non-negative "
                        "integer");
    closureLimit = static_cast<size_t>(*v);
  }
  Page page;
  if (auto bad = parsePage(args, kDefaultListLimit, page))
    return usageError(*bad);

  // A pair the build does not declare and no reference connects has no
  // row; it is answered as an empty edge, not as an error.
  AuditRow none;
  none.consumer = *consumer;
  none.provider = *provider;
  const uint32_t index = a.audit.find(*consumer, *provider);
  const AuditRow &row = index == kNoIndex ? none : a.audit.rows[index];
  const bool coverageStated = ctx.facts.stated() && lf.absenceIsEvidence();
  const unsigned findings =
      rowFindings(a.audit, lf, row, thresholds, coverageStated);

  const size_t seedCount = row.seedEnd - row.seedBegin;
  llvm::json::Array seeds;
  for (size_t i = page.begin(seedCount), e = page.end(seedCount); i < e; ++i) {
    const uint32_t s = row.seedBegin + static_cast<uint32_t>(i);
    const uint8_t flags = a.audit.seedFlags[s];
    llvm::json::Object seed;
    seed["symbol"] = symbolJson(lf, a.audit.seedSym[s]);
    seed["owner"] = (flags & kSeedUncertain) ? "presumed" : "defined";
    seed["via"] = (flags & kSeedViaInline) ? "inline" : "direct";
    if (flags & kSeedRegistration)
      seed["registers"] = true;
    const uint32_t ref = a.audit.seedRef[s];
    seed["kind"] = lf.refKindName(lf.refKind[ref]);
    seed["site"] = lf.strings->resolve(lf.refSite[ref]);
    seed["from"] = symbolJson(lf, holderOf(lf, ref));
    // The reference in the consumer's own sources the inline chain
    // started at.
    if (flags & kSeedViaInline)
      seed["through"] = witnessJson(lf, a.audit.seedRoot[s]);
    seeds.push_back(std::move(seed));
  }

  // The closure, by name: what would have to move for the consumer to
  // stop needing the rest of the provider.
  std::vector<uint32_t> closure;
  closureSymbolsOf(lf, a.audit, row, closure);
  std::sort(closure.begin(), closure.end(), [&](uint32_t x, uint32_t y) {
    const std::string &nx = symbolName(lf, x), &ny = symbolName(lf, y);
    if (nx != ny)
      return nx < ny;
    return x != y && lf.strings->resolve(lf.symUsr[x]) <
                         lf.strings->resolve(lf.symUsr[y]);
  });
  llvm::json::Array closureList;
  for (size_t i = 0; i < closure.size() && i < closureLimit; ++i) {
    llvm::json::Object sym = symbolJson(lf, closure[i]);
    if (lf.symFile[closure[i]] != kNoIndex)
      sym["definedIn"] = lf.files[lf.symFile[closure[i]]];
    closureList.push_back(std::move(sym));
  }

  // Who else inside the provider uses the closure (AuditRow::closureFanIn).
  std::vector<uint32_t> fanIn;
  closureFanInOf(lf, a.audit, row, fanIn);
  std::sort(fanIn.begin(), fanIn.end(), [&](uint32_t x, uint32_t y) {
    const std::string &nx = symbolName(lf, x), &ny = symbolName(lf, y);
    if (nx != ny)
      return nx < ny;
    return x != y && lf.strings->resolve(lf.symUsr[x]) <
                         lf.strings->resolve(lf.symUsr[y]);
  });
  llvm::json::Array fanInList;
  for (size_t i = 0; i < fanIn.size() && i < closureLimit; ++i) {
    llvm::json::Object sym = symbolJson(lf, fanIn[i]);
    if (lf.symFile[fanIn[i]] != kNoIndex)
      sym["definedIn"] = lf.files[lf.symFile[fanIn[i]]];
    fanInList.push_back(std::move(sym));
  }

  llvm::json::Object obj;
  obj["targetMap"] = targetMapJson(a, *args.getString("target_map"));
  obj["completeness"] = completenessJson(a, ctx);
  obj["edge"] = rowJson(a, row, findings);
  llvm::json::Array helped;
  for (uint32_t i = row.helpedBegin; i < row.helpedEnd; ++i) {
    llvm::json::Object h;
    h["target"] = a.map.names[a.audit.helpedTarget[i]];
    h["droppableSymbols"] = static_cast<int64_t>(a.audit.helpedSymbols[i]);
    helped.push_back(std::move(h));
  }
  obj["linkTargetsHelped"] = std::move(helped);
  obj["closure"] = std::move(closureList);
  obj["closureTotal"] = static_cast<int64_t>(closure.size());
  obj["closureTruncated"] = closure.size() > closureLimit;
  obj["fanIn"] = std::move(fanInList);
  obj["fanInTruncated"] = fanIn.size() > closureLimit;
  attachPage(obj, page, seedCount);
  obj["seeds"] = std::move(seeds);
  return llvm::json::Value(std::move(obj));
}

void addCommonProps(llvm::json::Object &props) {
  props["target_map"] = stringProp(
      "Path of the target-map JSON: the build's link targets, their "
      "sources and declared dependencies, written by a build adapter "
      "(scripts/cmake-target-map.py for CMake). Schema: "
      "docs/dependency-audit.md.");
  props["thin_max_symbols"] = intProp(
      "thin-dependency threshold: at most this many provider symbols "
      "referenced (default: 5).");
  props["thin_max_closure_pct"] = intProp(
      "thin-dependency threshold: the referenced symbols' closure is at "
      "most this percentage of the provider's defined symbols (default: "
      "10).");
}

} // namespace

void registerDependencyTools(std::vector<ToolEntry> &tools) {
  {
    llvm::json::Object props;
    addCommonProps(props);
    props["consumer"] =
        stringProp("Only edges of this consuming target (exact name).");
    props["provider"] =
        stringProp("Only edges onto this providing target (exact name).");
    props["finding"] = stringProp(
        "Only edges with this finding: unused-link-dependency, "
        "undeclared-link-dependency, thin-dependency, or any.");
    props["sort"] = stringProp(
        "name (default): by consumer, then provider. gain: by "
        "droppableSymbols, largest first — the symbols the consumer would "
        "stop linking if the closure were split out of the provider. "
        "link_gain: by linkTargetGain, largest first — the most symbols one "
        "executable or shared library linking the consumer would stop "
        "linking — then linkTargetsHelped.");
    addPagingProps(props, kDefaultListLimit, "edges");
    llvm::json::Object schema;
    schema["type"] = "object";
    schema["properties"] = std::move(props);
    schema["required"] = llvm::json::Array{"target_map"};
    tools.push_back(
        {"audit_dependencies",
         "Audit the build's link dependencies against the index: for every "
         "(consumer, provider) target pair, how many provider symbols the "
         "consumer's objects reference, how much of the provider those "
         "symbols drag along (their closure), and what the consumer would "
         "stop linking if the closure were split out. Findings: "
         "thin-dependency (a few symbols, a small closure), "
         "undeclared-link-dependency (used but only reachable "
         "transitively, or not declared), unused-link-dependency (declared, "
         "no reference seen). The evidence is the index's symbol reference "
         "facts (calls, address-taken functions, variables, vtables and "
         "typeinfo), or its call edges for an index without them; some "
         "references stay invisible (implicit destructor calls, template "
         "instantiation bodies), so no finding is a universal verdict — "
         "read `completeness`.",
         llvm::json::Value(std::move(schema)), handleAuditDependencies});
  }
  {
    llvm::json::Object props;
    addCommonProps(props);
    props["consumer"] = stringProp("The consuming target (exact name).");
    props["provider"] = stringProp("The providing target (exact name).");
    props["closure_limit"] = intProp(
        "Maximum closure and fan-in symbols to list (default: 50). "
        "`closureTotal` and the edge's `closureFanIn` are the full counts.");
    addPagingProps(props, kDefaultListLimit, "seeds");
    llvm::json::Object schema;
    schema["type"] = "object";
    schema["properties"] = std::move(props);
    schema["required"] =
        llvm::json::Array{"target_map", "consumer", "provider"};
    tools.push_back(
        {"explain_dependency",
         "Explain one link dependency edge: every provider symbol the "
         "consumer's objects reference, each with the call site that needs "
         "it (and, when the reference sits in an inline body, the call in "
         "the consumer's own sources that reaches it), plus the closure "
         "those symbols need inside the provider and the other targets "
         "that closure references, and the provider's own symbols that use "
         "the closure (`fanIn`). See `completeness` for what the evidence "
         "cannot see.",
         llvm::json::Value(std::move(schema)), handleExplainDependency});
  }
}

} // namespace vycor
