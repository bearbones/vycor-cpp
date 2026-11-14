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

#include "vycor/check/GlobalIndex.h"
#include "vycor/check/TypeNormalize.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <string_view>

namespace vycor {

namespace {

// Strip a single trailing '*' without heap allocation to compare
// pointer-to-class against base. Reference qualifiers are already stripped by
// normalizeTypeForMatching.
llvm::StringRef stripPointerView(llvm::StringRef s) noexcept {
  if (!s.empty() && s.back() == '*')
    return s.drop_back(1);
  return s;
}

bool isBaseOrSelfView(
    const StringInterner &interner,
    const std::unordered_map<StringInterner::Id,
                             std::vector<StringInterner::Id>> &bases,
    llvm::StringRef derived, llvm::StringRef maybeBase) {
  if (derived == maybeBase)
    return true;
  auto derivedOpt =
      interner.find(std::string_view(derived.data(), derived.size()));
  auto baseOpt =
      interner.find(std::string_view(maybeBase.data(), maybeBase.size()));
  if (!derivedOpt || !baseOpt)
    return false;
  const StringInterner::Id baseId = *baseOpt;
  llvm::SmallDenseSet<StringInterner::Id, 16> seen;
  llvm::SmallVector<StringInterner::Id, 16> stack{*derivedOpt};
  while (!stack.empty()) {
    StringInterner::Id cur = stack.pop_back_val();
    if (!seen.insert(cur).second)
      continue;
    auto it = bases.find(cur);
    if (it == bases.end())
      continue;
    for (StringInterner::Id b : it->second) {
      if (b == baseId)
        return true;
      stack.push_back(b);
    }
  }
  return false;
}

} // namespace

void TypeRelationIndex::addBase(const std::string &derived,
                                const std::string &base) {
  SId derivedId = interner_.intern(derived);
  SId baseId = interner_.intern(base);
  std::lock_guard<std::mutex> lock(writeMutex_);
  auto &v = bases_[derivedId];
  if (std::find(v.begin(), v.end(), baseId) == v.end())
    v.push_back(baseId);
}

void TypeRelationIndex::addCtorEdge(const std::string &toType,
                                    const std::string &fromType) {
  SId toId = interner_.intern(toType);
  SId fromId = interner_.intern(fromType);
  std::lock_guard<std::mutex> lock(writeMutex_);
  auto &v = ctorEdges_[toId];
  if (std::find(v.begin(), v.end(), fromId) == v.end())
    v.push_back(fromId);
}

void TypeRelationIndex::addConvOpEdge(const std::string &fromType,
                                      const std::string &toType) {
  SId fromId = interner_.intern(fromType);
  SId toId = interner_.intern(toType);
  std::lock_guard<std::mutex> lock(writeMutex_);
  auto &v = convOpEdges_[fromId];
  if (std::find(v.begin(), v.end(), toId) == v.end())
    v.push_back(toId);
}

void TypeRelationIndex::absorb(const TypeRelationIndex &other) {
  // `other` is a quiescent shard (its writers are done); route through the
  // add* methods so dedup and locking apply uniformly.
  other.forEachBase(
      [this](const std::string &d, const std::string &b) { addBase(d, b); });
  other.forEachCtorEdge([this](const std::string &to, const std::string &from) {
    addCtorEdge(to, from);
  });
  other.forEachConvOpEdge(
      [this](const std::string &from, const std::string &to) {
        addConvOpEdge(from, to);
      });
}

bool TypeRelationIndex::isBaseOrSelf(const std::string &derived,
                                     const std::string &maybeBase) const {
  return isBaseOrSelfView(interner_, bases_, derived, maybeBase);
}

bool TypeRelationIndex::isConvertible(const std::string &from,
                                      const std::string &to) const {
  if (from == to)
    return true;

  if (isArithmeticNormalizedType(from) && isArithmeticNormalizedType(to))
    return true;

  {
    const llvm::StringRef fromView(from);
    const llvm::StringRef toView(to);
    const llvm::StringRef fromClass = stripPointerView(fromView);
    const llvm::StringRef toClass = stripPointerView(toView);
    if (fromClass.size() != fromView.size() ||
        toClass.size() != toView.size()) {
      if (fromClass.size() != fromView.size() &&
          toClass.size() != toView.size() &&
          isBaseOrSelfView(interner_, bases_, fromClass, toClass))
        return true;
    } else if (fromClass != toClass &&
               isBaseOrSelfView(interner_, bases_, fromClass, toClass)) {
      return true;
    }
  }

  {
    auto toOpt = interner_.find(to);
    if (toOpt) {
      auto it = ctorEdges_.find(*toOpt);
      if (it != ctorEdges_.end()) {
        auto fromOpt = interner_.find(from);
        if (fromOpt) {
          for (SId src : it->second)
            if (src == *fromOpt)
              return true;
        }
      }
    }
  }

  {
    auto fromOpt = interner_.find(from);
    if (fromOpt) {
      auto it = convOpEdges_.find(*fromOpt);
      if (it != convOpEdges_.end()) {
        auto toOpt = interner_.find(to);
        if (toOpt) {
          for (SId tgt : it->second)
            if (tgt == *toOpt)
              return true;
        }
      }
    }
  }

  return false;
}

void GlobalIndex::addFunctionOverload(FunctionOverloadEntry entry) {
  SId key = interner_.intern(entry.qualifiedName);
  std::lock_guard<std::mutex> lock(writeMutex_);
  overloads_[key].push_back(std::move(entry));
}

void GlobalIndex::addDeductionGuide(DeductionGuideEntry entry) {
  SId key = interner_.intern(entry.templateName);
  std::lock_guard<std::mutex> lock(writeMutex_);
  guides_[key].push_back(std::move(entry));
}

void GlobalIndex::absorb(const GlobalIndex &shard) {
  shard.forEachOverload(
      [this](const FunctionOverloadEntry &e) { addFunctionOverload(e); });
  shard.forEachDeductionGuide(
      [this](const DeductionGuideEntry &e) { addDeductionGuide(e); });
  shard.forEachCoverageProperty(
      [this](const CoveragePropertyEntry &e) { addCoverageProperty(e); });
  shard.forEachOdrEntry([this](const OdrEntry &e) { addOdrEntry(e); });
  shard.forEachAbiLayout([this](const AbiLayoutEntry &e) { addAbiLayout(e); });
  shard.forEachSpecialization(
      [this](const SpecializationEntry &e) { addSpecialization(e); });
  shard.forEachDefaultArg(
      [this](const DefaultArgEntry &e) { addDefaultArg(e); });
  shard.forEachStaticInit(
      [this](const StaticInitEntry &e) { addStaticInit(e); });
  shard.forEachFunctionSummary(
      [this](const FunctionSummaryEntry &e) { addFunctionSummary(e); });
  shard.forEachHeaderStatic(
      [this](const HeaderStaticEntry &e) { addHeaderStatic(e); });
  shard.forEachExceptionSpec(
      [this](const ExceptionSpecEntry &e) { addExceptionSpec(e); });
  types_.absorb(shard.types_);
}

void GlobalIndex::addExceptionSpec(const ExceptionSpecEntry &entry) {
  std::string key = entry.qualifiedName + "|" + entry.signature + "|" +
                    (entry.isNoexcept ? "1|" : "0|") + entry.filePath + "|" +
                    std::to_string(entry.line);
  std::lock_guard<std::mutex> lock(writeMutex_);
  if (!exceptionSpecKeys_.insert(std::move(key)).second)
    return;
  exceptionSpecs_.push_back(entry);
}

size_t GlobalIndex::exceptionSpecCount() const {
  return exceptionSpecs_.size();
}

void GlobalIndex::addHeaderStatic(const HeaderStaticEntry &entry) {
  std::string key = entry.filePath + "|" + std::to_string(entry.line);
  std::lock_guard<std::mutex> lock(writeMutex_);
  auto it = headerStaticBySite_.find(key);
  if (it == headerStaticBySite_.end()) {
    headerStatics_.push_back(entry);
    headerStaticBySite_.emplace(std::move(key), headerStatics_.size() - 1);
    return;
  }
  auto &existing = headerStatics_[it->second];
  for (const auto &tu : entry.tuPaths)
    if (std::find(existing.tuPaths.begin(), existing.tuPaths.end(), tu) ==
        existing.tuPaths.end())
      existing.tuPaths.push_back(tu);
}

size_t GlobalIndex::headerStaticCount() const {
  return headerStatics_.size();
}

void GlobalIndex::addFunctionSummary(const FunctionSummaryEntry &entry) {
  std::lock_guard<std::mutex> lock(writeMutex_);
  auto it = functionSummaryByName_.find(entry.qualifiedName);
  if (it == functionSummaryByName_.end()) {
    functionSummaries_.push_back(entry);
    functionSummaryByName_.emplace(entry.qualifiedName,
                                   functionSummaries_.size() - 1);
    return;
  }
  auto &existing = functionSummaries_[it->second];
  existing.isNoexcept = existing.isNoexcept || entry.isNoexcept;
  existing.hasUncaughtThrow =
      existing.hasUncaughtThrow || entry.hasUncaughtThrow;
  if (!entry.uncaughtThrowSite.empty() &&
      (existing.uncaughtThrowSite.empty() ||
       entry.uncaughtThrowSite < existing.uncaughtThrowSite)) {
    existing.uncaughtThrowSite = entry.uncaughtThrowSite;
  }
  auto merge = [](std::vector<std::string> &into,
                  const std::vector<std::string> &from) {
    for (const auto &name : from)
      if (std::find(into.begin(), into.end(), name) == into.end())
        into.push_back(name);
  };
  merge(existing.calledFunctions, entry.calledFunctions);
  merge(existing.unguardedCalls, entry.unguardedCalls);
  merge(existing.referencedGlobals, entry.referencedGlobals);
  for (const auto &sitePair : entry.unguardedCallSites) {
    auto found = std::find_if(
        existing.unguardedCallSites.begin(), existing.unguardedCallSites.end(),
        [&](const auto &p) { return p.first == sitePair.first; });
    if (found == existing.unguardedCallSites.end()) {
      existing.unguardedCallSites.push_back(sitePair);
    } else if (!sitePair.second.empty() &&
               (found->second.empty() || sitePair.second < found->second)) {
      found->second = sitePair.second;
    }
  }
}

const FunctionSummaryEntry *
GlobalIndex::findFunctionSummary(const std::string &name) const {
  auto it = functionSummaryByName_.find(name);
  if (it == functionSummaryByName_.end())
    return nullptr;
  return &functionSummaries_[it->second];
}

size_t GlobalIndex::functionSummaryCount() const {
  return functionSummaries_.size();
}

void GlobalIndex::addStaticInit(const StaticInitEntry &entry) {
  std::string key = entry.qualifiedName + "|" + entry.filePath + "|" +
                    std::to_string(entry.line);
  std::lock_guard<std::mutex> lock(writeMutex_);
  if (!staticInitKeys_.insert(std::move(key)).second)
    return;
  staticInits_.push_back(entry);
  staticInitByName_.emplace(entry.qualifiedName, staticInits_.size() - 1);
}

const StaticInitEntry *
GlobalIndex::findStaticInit(const std::string &name) const {
  auto it = staticInitByName_.find(name);
  if (it == staticInitByName_.end())
    return nullptr;
  return &staticInits_[it->second];
}

size_t GlobalIndex::staticInitCount() const { return staticInits_.size(); }

void GlobalIndex::addDefaultArg(const DefaultArgEntry &entry) {
  std::string key = entry.qualifiedName + "|" + entry.signature + "|" +
                    std::to_string(entry.paramIndex) + "|" +
                    entry.defaultText + "|" + entry.filePath + "|" +
                    std::to_string(entry.line);
  std::lock_guard<std::mutex> lock(writeMutex_);
  if (!defaultArgKeys_.insert(std::move(key)).second)
    return;
  defaultArgs_.push_back(entry);
}

size_t GlobalIndex::defaultArgCount() const { return defaultArgs_.size(); }

void GlobalIndex::addSpecialization(const SpecializationEntry &entry) {
  SId key = interner_.intern(entry.templateName);
  std::lock_guard<std::mutex> lock(writeMutex_);
  auto &v = specializations_[key];
  for (auto &existing : v)
    if (existing.argsString == entry.argsString &&
        existing.headerPath == entry.headerPath && existing.line == entry.line) {
      if (!entry.resolutionSummary.empty() &&
          (existing.resolutionSummary.empty() ||
           (existing.resolutionSummary ==
                "explicit specialization declaration" &&
            entry.resolutionSummary !=
                "explicit specialization declaration"))) {
        existing.resolutionSummary = entry.resolutionSummary;
      }
      return;
    }
  v.push_back(entry);
}

std::vector<const SpecializationEntry *>
GlobalIndex::findSpecializations(const std::string &templateName) const {
  std::vector<const SpecializationEntry *> result;
  auto id = interner_.find(templateName);
  if (!id)
    return result;
  auto it = specializations_.find(*id);
  if (it != specializations_.end())
    for (const auto &entry : it->second)
      result.push_back(&entry);
  return result;
}

size_t GlobalIndex::specializationCount() const {
  size_t count = 0;
  for (const auto &kv : specializations_)
    count += kv.second.size();
  return count;
}

void GlobalIndex::addOdrEntry(const OdrEntry &entry) {
  std::string key = entry.qualifiedName + "|" + entry.signature +
                    (entry.isClass ? "|c|" : "|f|") + entry.filePath + "|" +
                    std::to_string(entry.line) + "|" +
                    std::to_string(entry.odrHash);
  std::lock_guard<std::mutex> lock(writeMutex_);
  auto it = odrByKey_.find(key);
  if (it != odrByKey_.end()) {
    auto &existing = odrEntries_[it->second];
    if (!entry.tuPath.empty() &&
        (existing.tuPath.empty() || entry.tuPath < existing.tuPath)) {
      existing.tuPath = entry.tuPath;
    }
    return;
  }
  odrEntries_.push_back(entry);
  odrByKey_.emplace(std::move(key), odrEntries_.size() - 1);
}

size_t GlobalIndex::odrEntryCount() const { return odrEntries_.size(); }

void GlobalIndex::addAbiLayout(const AbiLayoutEntry &entry) {
  std::string key;
  for (const auto *part : {&entry.usr, &entry.targetAbi, &entry.filePath,
                           &entry.tuPath, &entry.layout})
    key += std::to_string(part->size()) + ":" + *part;
  key += ":" + std::to_string(entry.line);
  std::lock_guard<std::mutex> lock(writeMutex_);
  if (abiLayoutKeys_.insert(std::move(key)).second)
    abiLayouts_.push_back(entry);
}

size_t GlobalIndex::abiLayoutCount() const { return abiLayouts_.size(); }

std::vector<const FunctionOverloadEntry *>
GlobalIndex::findOverloads(const std::string &qualifiedName) const {
  std::vector<const FunctionOverloadEntry *> result;
  auto id = interner_.find(qualifiedName);
  if (!id)
    return result;
  auto it = overloads_.find(*id);
  if (it != overloads_.end()) {
    for (const auto &entry : it->second)
      result.push_back(&entry);
  }
  return result;
}

std::vector<const DeductionGuideEntry *>
GlobalIndex::findDeductionGuides(const std::string &templateName) const {
  std::vector<const DeductionGuideEntry *> result;
  auto id = interner_.find(templateName);
  if (!id)
    return result;
  auto it = guides_.find(*id);
  if (it != guides_.end()) {
    for (const auto &entry : it->second)
      result.push_back(&entry);
  }
  return result;
}

size_t GlobalIndex::overloadCount() const {
  size_t count = 0;
  for (const auto &kv : overloads_)
    count += kv.second.size();
  return count;
}

size_t GlobalIndex::guideCount() const {
  size_t count = 0;
  for (const auto &kv : guides_)
    count += kv.second.size();
  return count;
}

void GlobalIndex::addCoverageProperty(CoveragePropertyEntry entry) {
  SId key = interner_.intern(entry.enclosingClass);
  std::lock_guard<std::mutex> lock(writeMutex_);
  coverageProps_[key].push_back(std::move(entry));
}

std::vector<const CoveragePropertyEntry *>
GlobalIndex::findClassMethods(const std::string &enclosingClass) const {
  std::vector<const CoveragePropertyEntry *> result;
  auto id = interner_.find(enclosingClass);
  if (!id)
    return result;
  auto it = coverageProps_.find(*id);
  if (it != coverageProps_.end()) {
    for (const auto &entry : it->second)
      result.push_back(&entry);
  }
  return result;
}

std::vector<std::string> GlobalIndex::allIndexedClasses() const {
  std::vector<std::string> result;
  result.reserve(coverageProps_.size());
  for (const auto &kv : coverageProps_)
    result.push_back(interner_.resolve(kv.first));
  return result;
}

size_t GlobalIndex::coverageEntryCount() const {
  size_t count = 0;
  for (const auto &kv : coverageProps_)
    count += kv.second.size();
  return count;
}

} // namespace vycor
