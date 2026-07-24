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

#include "vycor/anneal/GlobalIndex.h"
#include "vycor/anneal/TypeNormalize.h"

#include "llvm/ADT/StringSwitch.h"

#include <algorithm>
#include <unordered_set>

namespace vycor {

namespace {

// The fixed set of builtin arithmetic type names (names as produced by
// clang::QualType::getAsString() after normalization). Any two normalized
// type names in this set are treated as mutually convertible, matching the
// behaviour of C++ standard conversions.
bool isArithmeticTypeName(const std::string &normalized) {
  return llvm::StringSwitch<bool>(normalized)
      .Case("bool", true)
      .Case("char", true)
      .Case("signedchar", true)
      .Case("unsignedchar", true)
      .Case("wchar_t", true)
      .Case("char8_t", true)
      .Case("char16_t", true)
      .Case("char32_t", true)
      .Case("short", true)
      .Case("shortint", true)
      .Case("unsignedshort", true)
      .Case("unsignedshortint", true)
      .Case("int", true)
      .Case("unsigned", true)
      .Case("unsignedint", true)
      .Case("long", true)
      .Case("longint", true)
      .Case("unsignedlong", true)
      .Case("unsignedlongint", true)
      .Case("longlong", true)
      .Case("longlongint", true)
      .Case("unsignedlonglong", true)
      .Case("unsignedlonglongint", true)
      .Case("float", true)
      .Case("double", true)
      .Case("longdouble", true)
      .Default(false);
}

// Strip a single trailing '*' to compare pointer-to-class against base.
// Reference qualifiers are already stripped by normalizeTypeForMatching.
std::string stripPointer(const std::string &s) {
  if (!s.empty() && s.back() == '*') {
    std::string out = s;
    out.pop_back();
    return out;
  }
  return s;
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
  if (derived == maybeBase)
    return true;
  auto derivedOpt = interner_.find(derived);
  auto baseOpt = interner_.find(maybeBase);
  if (!derivedOpt || !baseOpt)
    return false;
  SId baseId = *baseOpt;
  std::unordered_set<SId> seen;
  std::vector<SId> stack{*derivedOpt};
  while (!stack.empty()) {
    SId cur = stack.back();
    stack.pop_back();
    if (!seen.insert(cur).second)
      continue;
    auto it = bases_.find(cur);
    if (it == bases_.end())
      continue;
    for (SId b : it->second) {
      if (b == baseId)
        return true;
      stack.push_back(b);
    }
  }
  return false;
}

bool TypeRelationIndex::isConvertible(const std::string &from,
                                      const std::string &to) const {
  if (from == to)
    return true;

  if (isArithmeticTypeName(from) && isArithmeticTypeName(to))
    return true;

  {
    std::string fromClass = stripPointer(from);
    std::string toClass = stripPointer(to);
    if (fromClass != from || toClass != to) {
      if (fromClass != from && toClass != to &&
          isBaseOrSelf(fromClass, toClass))
        return true;
    } else if (isBaseOrSelf(fromClass, toClass) && fromClass != toClass) {
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
  shard.forEachSpecialization(
      [this](const SpecializationEntry &e) { addSpecialization(e); });
  types_.absorb(shard.types_);
}

void GlobalIndex::addSpecialization(const SpecializationEntry &entry) {
  SId key = interner_.intern(entry.templateName);
  std::lock_guard<std::mutex> lock(writeMutex_);
  auto &v = specializations_[key];
  for (const auto &existing : v)
    if (existing.argsString == entry.argsString &&
        existing.headerPath == entry.headerPath && existing.line == entry.line)
      return;
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
  if (!odrKeys_.insert(std::move(key)).second)
    return;
  odrEntries_.push_back(entry);
}

size_t GlobalIndex::odrEntryCount() const { return odrEntries_.size(); }

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
