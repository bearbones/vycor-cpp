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
#include "vycor/callgraph/Utf8.h"

#include <algorithm>
#include <cassert>
#include <tuple>

namespace vycor {

const char *refRoleName(RefRole role) noexcept {
  switch (role) {
  case RefRole::Call:
    return "call";
  case RefRole::AddressOf:
    return "address_of";
  case RefRole::Read:
    return "read";
  case RefRole::Write:
    return "write";
  case RefRole::VtableUse:
    return "vtable_use";
  case RefRole::TypeinfoUse:
    return "typeinfo_use";
  case RefRole::Lock:
    return "lock";
  case RefRole::LockShared:
    return "lock_shared";
  }
  return "call";
}

std::optional<RefRole> refRoleFromName(llvm::StringRef name) {
  for (unsigned i = 0; i < kRefRoleCount; ++i) {
    const auto role = static_cast<RefRole>(i);
    if (name == refRoleName(role))
      return role;
  }
  return std::nullopt;
}

const char *symbolKindName(SymbolKind kind) noexcept {
  switch (kind) {
  case SymbolKind::Function:
    return "function";
  case SymbolKind::Variable:
    return "variable";
  case SymbolKind::Class:
    return "class";
  case SymbolKind::File:
    return "file";
  }
  return "function";
}

std::vector<const char *> symbolAttributeNames(uint32_t flags) {
  struct FlagName {
    uint32_t bit;
    const char *name;
  };
  static constexpr FlagName kNames[] = {
      {kSymExternC, "extern_c"},
      {kSymVisibility, "visibility"},
      {kSymDllExport, "dllexport"},
      {kSymDllImport, "dllimport"},
      {kSymUsedAttr, "used"},
      {kSymWeak, "weak"},
      {kSymMember, "member"},
      {kSymStaticLocal, "static_local"},
      {kSymThreadLocal, "thread_local"},
      {kSymConst, "const"},
      {kSymAtomic, "atomic"},
      {kSymDynamicClass, "dynamic_class"},
      {kSymMain, "main"},
      {kSymExplicitInstantiation, "explicit_instantiation"},
      {kSymVirtual, "virtual"},
      {kSymLambda, "lambda"},
      {kSymSpecialization, "explicit_specialization"},
      {kSymSyncPrimitive, "sync_primitive"},
      {kSymInLoop, "in_loop"},
  };
  std::vector<const char *> out;
  for (const FlagName &f : kNames)
    if (flags & f.bit)
      out.push_back(f.name);
  return out;
}

const char *linkageClassName(LinkageClass linkage) noexcept {
  switch (linkage) {
  case LinkageClass::External:
    return "external";
  case LinkageClass::TemplateInstantiation:
    return "template_instantiation";
  case LinkageClass::Vague:
    return "vague";
  case LinkageClass::Internal:
    return "internal";
  case LinkageClass::None:
    return "none";
  }
  return "none";
}

// ---------------------------------------------------------------------------
// Moves (everything but the mutex)
// ---------------------------------------------------------------------------

ReferenceIndex::ReferenceIndex(ReferenceIndex &&other) noexcept {
  std::lock_guard<std::mutex> lock(other.mutex_);
  strings_ = std::move(other.strings_);
  symbols_ = std::move(other.symbols_);
  references_ = std::move(other.references_);
  symbolTable_ = std::move(other.symbolTable_);
  referenceTable_ = std::move(other.referenceTable_);
  byTu_ = std::move(other.byTu_);
  liveSymbols_ = other.liveSymbols_;
  liveReferences_ = other.liveReferences_;
  readOnly_ = other.readOnly_;
  minTuValid_ = other.minTuValid_;
  lookupValid_ = false;
  other.liveSymbols_ = 0;
  other.liveReferences_ = 0;
  other.lookupValid_ = false;
}

ReferenceIndex &ReferenceIndex::operator=(ReferenceIndex &&other) noexcept {
  if (this == &other)
    return *this;
  std::scoped_lock lock(mutex_, other.mutex_);
  strings_ = std::move(other.strings_);
  symbols_ = std::move(other.symbols_);
  references_ = std::move(other.references_);
  symbolTable_ = std::move(other.symbolTable_);
  referenceTable_ = std::move(other.referenceTable_);
  byTu_ = std::move(other.byTu_);
  liveSymbols_ = other.liveSymbols_;
  liveReferences_ = other.liveReferences_;
  readOnly_ = other.readOnly_;
  minTuValid_ = other.minTuValid_;
  lookupValid_ = false;
  other.liveSymbols_ = 0;
  other.liveReferences_ = 0;
  other.lookupValid_ = false;
  return *this;
}

// ---------------------------------------------------------------------------
// Content-keyed record tables
// ---------------------------------------------------------------------------

namespace {

inline uint64_t mix(uint64_t h, uint64_t v) noexcept {
  // splitmix64 finalizer over a running combination: cheap, and every input
  // bit reaches the low bits the table masks with.
  h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
  h ^= h >> 30;
  h *= 0xBF58476D1CE4E5B9ull;
  h ^= h >> 27;
  h *= 0x94D049BB133111EBull;
  h ^= h >> 31;
  return h;
}

// Sorted, duplicate-free union of `existing` (sorted, unique) and `added`
// (any order); calls onNew(idx) for each index that was not in `existing`.
template <typename OnNew>
void mergeContribution(std::vector<uint32_t> &existing,
                       std::vector<uint32_t> &added, OnNew onNew) {
  std::sort(added.begin(), added.end());
  added.erase(std::unique(added.begin(), added.end()), added.end());
  if (existing.empty()) {
    for (uint32_t idx : added)
      onNew(idx);
    existing = std::move(added);
    added.clear();
    return;
  }
  std::vector<uint32_t> merged;
  merged.reserve(existing.size() + added.size());
  size_t i = 0, j = 0;
  while (i < existing.size() || j < added.size()) {
    if (j == added.size() ||
        (i < existing.size() && existing[i] < added[j])) {
      merged.push_back(existing[i++]);
    } else if (i == existing.size() || added[j] < existing[i]) {
      onNew(added[j]);
      merged.push_back(added[j++]);
    } else {
      merged.push_back(existing[i]);
      ++i;
      ++j;
    }
  }
  existing = std::move(merged);
  added.clear();
}

} // namespace

uint64_t ReferenceIndex::hashOf(const SymbolRecord &r) noexcept {
  uint64_t h = mix(0x5359u, (static_cast<uint64_t>(r.usr) << 32) | r.file);
  h = mix(h, (static_cast<uint64_t>(r.name) << 32) | r.line);
  h = mix(h, (static_cast<uint64_t>(r.flags) << 32) | r.size);
  return mix(h, (static_cast<uint64_t>(r.kind) << 8) |
                    static_cast<uint64_t>(r.linkage));
}

uint64_t ReferenceIndex::hashOf(const ReferenceRecord &r) noexcept {
  uint64_t h =
      mix(0x5246u, (static_cast<uint64_t>(r.referrer) << 32) | r.target);
  h = mix(h, (static_cast<uint64_t>(r.file) << 32) | r.line);
  h = mix(h, (static_cast<uint64_t>(r.col) << 32) | r.count);
  h = mix(h, (static_cast<uint64_t>(r.locks) << 32) | r.flags);
  return mix(h, static_cast<uint64_t>(r.role));
}

bool ReferenceIndex::same(const SymbolRecord &a,
                          const SymbolRecord &b) noexcept {
  return a.usr == b.usr && a.name == b.name && a.file == b.file &&
         a.line == b.line && a.flags == b.flags && a.size == b.size &&
         a.kind == b.kind && a.linkage == b.linkage;
}

bool ReferenceIndex::same(const ReferenceRecord &a,
                          const ReferenceRecord &b) noexcept {
  return a.referrer == b.referrer && a.target == b.target &&
         a.file == b.file && a.line == b.line && a.col == b.col &&
         a.count == b.count && a.role == b.role && a.locks == b.locks &&
         a.flags == b.flags;
}

namespace {

// One implementation for both record kinds. `records` is the backing
// vector (StoredSymbol / StoredReference); the table holds index + 1.
template <typename Stored, typename Record, typename Table, typename Hash,
          typename Same>
uint32_t findOrInsertRecord(std::vector<Stored> &records, Table &table,
                            const Record &rec, Hash hash, Same same) {
  if ((table.used + 1) * 2 > table.slots.size()) {
    const size_t cap = std::max<size_t>(64, table.slots.size() * 2);
    std::vector<uint32_t> grown(cap, 0);
    const size_t mask = cap - 1;
    for (uint32_t slot : table.slots) {
      if (slot == 0)
        continue;
      size_t pos = hash(records[slot - 1].rec) & mask;
      while (grown[pos] != 0)
        pos = (pos + 1) & mask;
      grown[pos] = slot;
    }
    table.slots = std::move(grown);
  }
  const size_t mask = table.slots.size() - 1;
  size_t pos = hash(rec) & mask;
  while (true) {
    const uint32_t slot = table.slots[pos];
    if (slot == 0)
      break;
    if (same(records[slot - 1].rec, rec))
      return slot - 1;
    pos = (pos + 1) & mask;
  }
  const uint32_t idx = static_cast<uint32_t>(records.size());
  Stored stored;
  stored.rec = rec;
  records.push_back(stored);
  table.slots[pos] = idx + 1;
  ++table.used;
  return idx;
}

} // namespace

uint32_t ReferenceIndex::findOrInsertSymbol(const SymbolRecord &rec) {
  return findOrInsertRecord(
      symbols_, symbolTable_, rec,
      [](const SymbolRecord &r) { return hashOf(r); },
      [](const SymbolRecord &a, const SymbolRecord &b) { return same(a, b); });
}

uint32_t ReferenceIndex::findOrInsertReference(const ReferenceRecord &rec) {
  return findOrInsertRecord(
      references_, referenceTable_, rec,
      [](const ReferenceRecord &r) { return hashOf(r); },
      [](const ReferenceRecord &a, const ReferenceRecord &b) {
        return same(a, b);
      });
}

void ReferenceIndex::rebuildTables() {
  symbolTable_.clear();
  referenceTable_.clear();
  size_t cap = 64;
  while (cap < symbols_.size() * 2 + 2)
    cap *= 2;
  symbolTable_.slots.assign(cap, 0);
  for (size_t i = 0; i < symbols_.size(); ++i) {
    size_t pos = hashOf(symbols_[i].rec) & (cap - 1);
    while (symbolTable_.slots[pos] != 0)
      pos = (pos + 1) & (cap - 1);
    symbolTable_.slots[pos] = static_cast<uint32_t>(i) + 1;
  }
  symbolTable_.used = symbols_.size();

  cap = 64;
  while (cap < references_.size() * 2 + 2)
    cap *= 2;
  referenceTable_.slots.assign(cap, 0);
  for (size_t i = 0; i < references_.size(); ++i) {
    size_t pos = hashOf(references_[i].rec) & (cap - 1);
    while (referenceTable_.slots[pos] != 0)
      pos = (pos + 1) & (cap - 1);
    referenceTable_.slots[pos] = static_cast<uint32_t>(i) + 1;
  }
  referenceTable_.used = references_.size();
}

// ---------------------------------------------------------------------------
// Producers
// ---------------------------------------------------------------------------

ReferenceIndex::Id ReferenceIndex::internText(llvm::StringRef raw) {
  if (!needsIndexEscape(raw))
    return strings_.intern(std::string_view(raw.data(), raw.size()));
  return strings_.intern(toIndexText(raw));
}

void ReferenceIndex::addTuFacts(Id tu, llvm::ArrayRef<SymbolRecord> symbols,
                                llvm::ArrayRef<ReferenceRecord> references) {
  std::lock_guard<std::mutex> lock(mutex_);
  assert(!readOnly_ && "mutating a read-only ReferenceIndex");
  if (readOnly_)
    return;
  // What decode() refuses must not get in: a saved index has to load.
  std::vector<uint32_t> symIdx;
  symIdx.reserve(symbols.size());
  for (const SymbolRecord &rec : symbols) {
    assert((rec.flags & ~kSymbolFlagMask) == 0 && "unknown symbol flag");
    symIdx.push_back(findOrInsertSymbol(rec));
  }
  std::vector<uint32_t> refIdx;
  refIdx.reserve(references.size());
  for (const ReferenceRecord &rec : references) {
    assert(rec.count != 0 && "a reference occurs at least once");
    refIdx.push_back(findOrInsertReference(rec));
  }

  TuContribution &entry = byTu_[tu];
  mergeContribution(entry.symbols, symIdx, [&](uint32_t idx) {
    if (symbols_[idx].refs++ == 0)
      ++liveSymbols_;
  });
  mergeContribution(entry.references, refIdx, [&](uint32_t idx) {
    if (references_[idx].refs++ == 0)
      ++liveReferences_;
  });
  invalidateDerived();
}

void ReferenceIndex::addSymbol(const SymbolFact &fact,
                               const std::string &tuPath) {
  SymbolRecord rec;
  rec.usr = internText(fact.usr);
  rec.name = internText(fact.name);
  rec.file = internText(fact.file);
  rec.line = fact.line;
  rec.flags = fact.flags & kSymbolFlagMask;
  rec.size = fact.size;
  rec.kind = fact.kind;
  rec.linkage = fact.linkage;
  addTuFacts(internText(tuPath), {rec}, {});
}

void ReferenceIndex::addReference(const ReferenceFact &fact,
                                  const std::string &tuPath) {
  ReferenceRecord rec;
  rec.referrer = internText(fact.referrerUsr);
  rec.target = internText(fact.targetUsr);
  rec.file = internText(fact.file);
  rec.line = fact.line;
  rec.col = fact.col;
  rec.count = std::max<uint32_t>(fact.count, 1); // external input (bridge)
  rec.role = fact.role;
  rec.locks = fact.locks.empty() ? kNoId : internText(fact.locks);
  rec.flags = fact.flags & kRefFlagMask;
  addTuFacts(internText(tuPath), {}, {rec});
}

void ReferenceIndex::absorb(const ReferenceIndex &shard) {
  if (&shard == this)
    return;
  std::scoped_lock lock(mutex_, shard.mutex_);
  assert(!readOnly_ && !shard.readOnly_ &&
         "absorb needs both indexes' contributor lists");
  if (readOnly_ || shard.readOnly_)
    return;

  // Shard string id -> this index's id, and shard record index -> this
  // index's record index, both filled on first use: a shard's strings and
  // records are each translated once however many TUs list them.
  std::vector<Id> stringMap(shard.strings_.size(), kNoId);
  const auto mapString = [&](Id id) {
    Id &slot = stringMap[id];
    if (slot == kNoId)
      slot = strings_.intern(shard.strings_.resolve(id)); // index text as is
    return slot;
  };
  std::vector<uint32_t> symbolMap(shard.symbols_.size(), kNoId);
  std::vector<uint32_t> referenceMap(shard.references_.size(), kNoId);

  std::vector<uint32_t> symIdx;
  std::vector<uint32_t> refIdx;
  for (const auto &[shardTu, contribution] : shard.byTu_) {
    symIdx.clear();
    refIdx.clear();
    for (uint32_t i : contribution.symbols) {
      uint32_t &slot = symbolMap[i];
      if (slot == kNoId) {
        SymbolRecord rec = shard.symbols_[i].rec;
        rec.usr = mapString(rec.usr);
        rec.name = mapString(rec.name);
        rec.file = mapString(rec.file);
        slot = findOrInsertSymbol(rec);
      }
      symIdx.push_back(slot);
    }
    for (uint32_t i : contribution.references) {
      uint32_t &slot = referenceMap[i];
      if (slot == kNoId) {
        ReferenceRecord rec = shard.references_[i].rec;
        rec.referrer = mapString(rec.referrer);
        rec.target = mapString(rec.target);
        rec.file = mapString(rec.file);
        if (rec.locks != kNoId)
          rec.locks = mapString(rec.locks);
        slot = findOrInsertReference(rec);
      }
      refIdx.push_back(slot);
    }
    TuContribution &entry = byTu_[mapString(shardTu)];
    mergeContribution(entry.symbols, symIdx, [&](uint32_t idx) {
      if (symbols_[idx].refs++ == 0)
        ++liveSymbols_;
    });
    mergeContribution(entry.references, refIdx, [&](uint32_t idx) {
      if (references_[idx].refs++ == 0)
        ++liveReferences_;
    });
  }
  invalidateDerived();
}

size_t ReferenceIndex::removeTU(const std::string &tuPath) {
  std::lock_guard<std::mutex> lock(mutex_);
  return removeTUsLocked({tuPath});
}

size_t ReferenceIndex::removeTUs(const std::vector<std::string> &tuPaths) {
  std::lock_guard<std::mutex> lock(mutex_);
  return removeTUsLocked(tuPaths);
}

size_t
ReferenceIndex::removeTUsLocked(const std::vector<std::string> &tuPaths) {
  assert(!readOnly_ && "mutating a read-only ReferenceIndex");
  if (readOnly_)
    return 0;
  size_t dropped = 0;
  bool any = false;
  for (const std::string &raw : tuPaths) {
    // A TU path arrives raw (the snapshot meta keeps them so); the index
    // holds its index text (Utf8.h).
    const auto id = strings_.find(toIndexText(raw));
    if (!id)
      continue;
    auto it = byTu_.find(*id);
    if (it == byTu_.end())
      continue;
    for (uint32_t idx : it->second.symbols) {
      assert(symbols_[idx].refs > 0);
      if (--symbols_[idx].refs == 0) {
        --liveSymbols_;
        ++dropped;
      }
    }
    for (uint32_t idx : it->second.references) {
      assert(references_[idx].refs > 0);
      if (--references_[idx].refs == 0) {
        --liveReferences_;
        ++dropped;
      }
    }
    byTu_.erase(it);
    any = true;
  }
  if (any)
    invalidateDerived();
  return dropped;
}

void ReferenceIndex::compact() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (readOnly_)
    return;
  if (liveSymbols_ == symbols_.size() &&
      liveReferences_ == references_.size())
    return;
  std::vector<uint32_t> symbolMap(symbols_.size(), kNoId);
  std::vector<uint32_t> referenceMap(references_.size(), kNoId);
  size_t out = 0;
  for (size_t i = 0; i < symbols_.size(); ++i) {
    if (symbols_[i].refs == 0)
      continue;
    symbolMap[i] = static_cast<uint32_t>(out);
    symbols_[out++] = symbols_[i];
  }
  symbols_.resize(out);
  out = 0;
  for (size_t i = 0; i < references_.size(); ++i) {
    if (references_[i].refs == 0)
      continue;
    referenceMap[i] = static_cast<uint32_t>(out);
    references_[out++] = references_[i];
  }
  references_.resize(out);
  // Compaction keeps relative order, so the contributor lists stay sorted.
  for (auto &[tu, contribution] : byTu_) {
    for (uint32_t &idx : contribution.symbols)
      idx = symbolMap[idx];
    for (uint32_t &idx : contribution.references)
      idx = referenceMap[idx];
  }
  rebuildTables();
  lookupValid_ = false;
}

// ---------------------------------------------------------------------------
// Derived planes
// ---------------------------------------------------------------------------

void ReferenceIndex::ensureMinTu() const {
  if (minTuValid_)
    return;
  // Visit the TUs in ascending path order: the first one to list a record
  // is its smallest contributor, the last its largest.
  std::vector<Id> tus;
  tus.reserve(byTu_.size());
  for (const auto &entry : byTu_)
    tus.push_back(entry.first);
  std::sort(tus.begin(), tus.end(),
            [&](Id a, Id b) { return text(a) < text(b); });
  const auto &symbols = symbols_;
  const auto &references = references_;
  for (const auto &s : symbols)
    s.minTu = kNoId;
  for (const auto &r : references)
    r.minTu = r.maxTu = kNoId;
  for (Id tu : tus) {
    const TuContribution &contribution = byTu_.find(tu)->second;
    for (uint32_t idx : contribution.symbols)
      if (symbols[idx].minTu == kNoId)
        symbols[idx].minTu = tu;
    for (uint32_t idx : contribution.references) {
      if (references[idx].minTu == kNoId)
        references[idx].minTu = tu;
      references[idx].maxTu = tu;
    }
  }
  minTuValid_ = true;
}

namespace {

// Build CSR adjacency from (key, item) pairs produced by `emit`, which is
// called twice: once to count, once to fill.
template <typename Emit>
void buildCsr(std::vector<uint32_t> &offsets, std::vector<uint32_t> &items,
              size_t keyCount, Emit emit) {
  offsets.assign(keyCount + 1, 0);
  emit([&](uint32_t key, uint32_t) { ++offsets[key + 1]; });
  for (size_t i = 1; i <= keyCount; ++i)
    offsets[i] += offsets[i - 1];
  items.assign(offsets[keyCount], 0);
  std::vector<uint32_t> cursor(offsets.begin(), offsets.end() - 1);
  emit([&](uint32_t key, uint32_t item) { items[cursor[key]++] = item; });
}

} // namespace

void ReferenceIndex::ensureLookup() const {
  ensureMinTu();
  if (lookupValid_)
    return;
  const size_t keys = strings_.size();
  buildCsr(symbolsByUsr_.offsets, symbolsByUsr_.items, keys, [&](auto put) {
    for (size_t i = 0; i < symbols_.size(); ++i)
      if (symbols_[i].refs != 0)
        put(symbols_[i].rec.usr, static_cast<uint32_t>(i));
  });
  buildCsr(refsByTarget_.offsets, refsByTarget_.items, keys, [&](auto put) {
    for (size_t i = 0; i < references_.size(); ++i)
      if (references_[i].refs != 0)
        put(references_[i].rec.target, static_cast<uint32_t>(i));
  });
  buildCsr(refsByReferrer_.offsets, refsByReferrer_.items, keys,
           [&](auto put) {
             for (size_t i = 0; i < references_.size(); ++i)
               if (references_[i].refs != 0)
                 put(references_[i].rec.referrer, static_cast<uint32_t>(i));
           });
  // name -> distinct USRs.
  std::vector<std::pair<Id, Id>> named;
  named.reserve(liveSymbols_);
  for (const StoredSymbol &s : symbols_)
    if (s.refs != 0)
      named.emplace_back(s.rec.name, s.rec.usr);
  std::sort(named.begin(), named.end());
  named.erase(std::unique(named.begin(), named.end()), named.end());
  buildCsr(usrsByName_.offsets, usrsByName_.items, keys, [&](auto put) {
    for (const auto &[name, usr] : named)
      put(name, usr);
  });
  lookupValid_ = true;
}

// ---------------------------------------------------------------------------
// Sizes
// ---------------------------------------------------------------------------

size_t ReferenceIndex::symbolRecordCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return liveSymbols_;
}

size_t ReferenceIndex::symbolCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<uint8_t> seen(strings_.size(), 0);
  size_t n = 0;
  for (const StoredSymbol &s : symbols_)
    if (s.refs != 0 && !seen[s.rec.usr]) {
      seen[s.rec.usr] = 1;
      ++n;
    }
  return n;
}

size_t ReferenceIndex::referenceCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return liveReferences_;
}

bool ReferenceIndex::empty() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return liveSymbols_ == 0 && liveReferences_ == 0;
}

bool ReferenceIndex::readOnly() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return readOnly_;
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

SymbolInfo ReferenceIndex::symbolInfoLocked(Id usr) const {
  SymbolInfo info;
  info.usr = text(usr);
  const llvm::ArrayRef<uint32_t> records = symbolsByUsr_.at(usr);
  if (records.empty()) {
    info.name = info.usr;
    info.linkage = LinkageClass::None;
    return info;
  }
  struct Decl {
    Id file;
    uint32_t line;
    bool definition;
    bool inHeader;
    uint32_t tuCount;
    Id firstTu;
  };
  // A symbol has a handful of declarations: a small flat list, merged by
  // linear search.
  std::vector<Decl> decls;
  const std::string *name = nullptr;
  uint8_t kind = UINT8_MAX;
  uint8_t linkage = UINT8_MAX;
  // A TU that sees the definition knows the linkage; one that sees only an
  // earlier declaration can miss a later `inline`.
  uint8_t definedLinkage = UINT8_MAX;
  Id firstDefiningTu = kNoId;
  Id vtableHome = kNoId;
  const auto minTu = [&](Id a, Id b) {
    if (a == kNoId)
      return b;
    if (b == kNoId)
      return a;
    return text(b) < text(a) ? b : a;
  };
  for (uint32_t idx : records) {
    const StoredSymbol &s = symbols_[idx];
    const SymbolRecord &r = s.rec;
    const std::string &recName = text(r.name);
    if (!name || recName < *name)
      name = &recName;
    kind = std::min(kind, static_cast<uint8_t>(r.kind));
    linkage = std::min(linkage, static_cast<uint8_t>(r.linkage));
    info.flags |= r.flags & ~kSymbolPerDeclFlags;
    info.size = std::max(info.size, r.size);
    const bool definition = (r.flags & kSymDefinition) != 0;
    const bool inHeader = (r.flags & kSymInHeader) != 0;
    if (definition) {
      definedLinkage =
          std::min(definedLinkage, static_cast<uint8_t>(r.linkage));
      info.definingTuCount += s.refs;
      firstDefiningTu = minTu(firstDefiningTu, s.minTu);
    }
    if (inHeader)
      info.declaredInHeader = true;
    if (r.flags & kSymVtableHome)
      vtableHome = minTu(vtableHome, s.minTu);
    auto it = std::find_if(decls.begin(), decls.end(), [&](const Decl &d) {
      return d.file == r.file && d.line == r.line &&
             d.definition == definition && d.inHeader == inHeader;
    });
    if (it == decls.end()) {
      decls.push_back({r.file, r.line, definition, inHeader, s.refs, s.minTu});
    } else {
      it->tuCount += s.refs;
      it->firstTu = minTu(it->firstTu, s.minTu);
    }
  }
  info.name = *name;
  info.kind = static_cast<SymbolKind>(kind);
  info.linkage = static_cast<LinkageClass>(
      definedLinkage != UINT8_MAX ? definedLinkage : linkage);
  if (firstDefiningTu != kNoId)
    info.firstDefiningTu = text(firstDefiningTu);
  if (vtableHome != kNoId)
    info.vtableHomeTu = text(vtableHome);
  info.declarations.reserve(decls.size());
  for (const Decl &d : decls) {
    SymbolDeclInfo out;
    out.file = text(d.file);
    out.line = d.line;
    out.definition = d.definition;
    out.inHeader = d.inHeader;
    out.tuCount = d.tuCount;
    if (d.firstTu != kNoId)
      out.firstTu = text(d.firstTu);
    info.declarations.push_back(std::move(out));
  }
  std::sort(info.declarations.begin(), info.declarations.end(),
            [](const SymbolDeclInfo &a, const SymbolDeclInfo &b) {
              return std::tie(a.file, a.line, a.definition, a.inHeader) <
                     std::tie(b.file, b.line, b.definition, b.inHeader);
            });
  return info;
}

std::vector<ReferenceInfo> ReferenceIndex::referenceInfosLocked(
    llvm::ArrayRef<uint32_t> indices) const {
  // Group the records of one (referrer, target, role) by sorting on the
  // ids, then merge each run.
  std::vector<uint32_t> order(indices.begin(), indices.end());
  const auto key = [&](uint32_t idx) {
    const ReferenceRecord &r = references_[idx].rec;
    return std::make_tuple(r.referrer, r.target, static_cast<uint8_t>(r.role));
  };
  std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
    const auto ka = key(a), kb = key(b);
    return ka != kb ? ka < kb : a < b;
  });

  // Display name and kind of a USR: from its symbol records, the USR
  // itself when there are none.
  struct Named {
    const std::string *name;
    SymbolKind kind;
    bool symbol; // false: no symbol record, the USR stands in
  };
  const auto named = [&](Id usr) {
    Named out{&text(usr), SymbolKind::Function, false};
    const std::string *best = nullptr;
    uint8_t kind = UINT8_MAX;
    for (uint32_t idx : symbolsByUsr_.at(usr)) {
      const SymbolRecord &r = symbols_[idx].rec;
      const std::string &n = text(r.name);
      if (!best || n < *best)
        best = &n;
      kind = std::min(kind, static_cast<uint8_t>(r.kind));
    }
    if (best) {
      out.name = best;
      out.kind = static_cast<SymbolKind>(kind);
      out.symbol = true;
    }
    return out;
  };

  std::vector<ReferenceInfo> result;
  for (size_t i = 0; i < order.size();) {
    const auto k = key(order[i]);
    const ReferenceRecord &first = references_[order[i]].rec;
    ReferenceInfo info;
    info.referrerUsr = text(first.referrer);
    info.targetUsr = text(first.target);
    info.role = first.role;
    const Named referrer = named(first.referrer);
    const Named target = named(first.target);
    info.referrerName = *referrer.name;
    info.referrerKind = referrer.kind;
    info.targetName = *target.name;
    info.targetKind = target.kind;
    // A lock target with no symbol (a data member mutex, an unnamed lock,
    // a once guard) is a variable, not a function.
    if (isLockRole(first.role) && !target.symbol)
      info.targetKind = SymbolKind::Variable;
    const std::string *file = nullptr;
    const std::string *firstTu = nullptr;
    Id locks = kNoId;
    for (; i < order.size() && key(order[i]) == k; ++i) {
      const StoredReference &s = references_[order[i]];
      const ReferenceRecord &r = s.rec;
      info.count = std::max(info.count, r.count);
      info.tuCount += s.refs;
      info.flags |= r.flags;
      const std::string &f = text(r.file);
      if (!file || std::tie(f, r.line, r.col) <
                       std::tie(*file, info.line, info.col)) {
        file = &f;
        info.line = r.line;
        info.col = r.col;
        locks = r.locks;
      }
      if (s.minTu != kNoId) {
        const std::string &tu = text(s.minTu);
        if (!firstTu || tu < *firstTu)
          firstTu = &tu;
      }
    }
    if (file)
      info.file = *file;
    if (firstTu)
      info.firstTu = *firstTu;
    if (locks != kNoId)
      info.locks = text(locks);
    result.push_back(std::move(info));
  }
  std::sort(result.begin(), result.end(),
            [](const ReferenceInfo &a, const ReferenceInfo &b) {
              return std::tie(a.referrerName, a.referrerUsr, a.targetName,
                              a.targetUsr, a.role) <
                     std::tie(b.referrerName, b.referrerUsr, b.targetName,
                              b.targetUsr, b.role);
            });
  return result;
}

std::optional<SymbolInfo> ReferenceIndex::symbol(const std::string &usr) const {
  std::lock_guard<std::mutex> lock(mutex_);
  ensureLookup();
  const auto id = strings_.find(lookupText(usr));
  if (!id || symbolsByUsr_.at(*id).empty())
    return std::nullopt;
  return symbolInfoLocked(*id);
}

std::vector<std::string>
ReferenceIndex::usrsNamed(const std::string &name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  ensureLookup();
  std::vector<std::string> result;
  const auto id = strings_.find(lookupText(name));
  if (!id)
    return result;
  for (Id usr : usrsByName_.at(*id))
    result.push_back(text(usr));
  std::sort(result.begin(), result.end());
  return result;
}

std::vector<SymbolInfo> ReferenceIndex::allSymbols() const {
  std::lock_guard<std::mutex> lock(mutex_);
  ensureLookup();
  std::vector<SymbolInfo> result;
  const size_t keys = symbolsByUsr_.offsets.empty()
                          ? 0
                          : symbolsByUsr_.offsets.size() - 1;
  for (size_t usr = 0; usr < keys; ++usr)
    if (!symbolsByUsr_.at(static_cast<Id>(usr)).empty())
      result.push_back(symbolInfoLocked(static_cast<Id>(usr)));
  std::sort(result.begin(), result.end(),
            [](const SymbolInfo &a, const SymbolInfo &b) {
              return std::tie(a.name, a.usr) < std::tie(b.name, b.usr);
            });
  return result;
}

std::vector<ReferenceInfo>
ReferenceIndex::referencesTo(const std::string &targetUsr) const {
  std::lock_guard<std::mutex> lock(mutex_);
  ensureLookup();
  const auto id = strings_.find(lookupText(targetUsr));
  if (!id)
    return {};
  return referenceInfosLocked(refsByTarget_.at(*id));
}

std::vector<ReferenceInfo>
ReferenceIndex::referencesFrom(const std::string &referrerUsr) const {
  std::lock_guard<std::mutex> lock(mutex_);
  ensureLookup();
  const auto id = strings_.find(lookupText(referrerUsr));
  if (!id)
    return {};
  return referenceInfosLocked(refsByReferrer_.at(*id));
}

std::vector<ReferenceInfo> ReferenceIndex::allReferences() const {
  std::lock_guard<std::mutex> lock(mutex_);
  ensureLookup();
  std::vector<uint32_t> live;
  live.reserve(liveReferences_);
  for (size_t i = 0; i < references_.size(); ++i)
    if (references_[i].refs != 0)
      live.push_back(static_cast<uint32_t>(i));
  return referenceInfosLocked(live);
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------
//
// Layout (varint = unsigned LEB128, at most 32 bits):
//
//   varint stringCount
//     per string, in byte order: varint sharedPrefix, varint suffixLength,
//     suffix bytes (front coding: USRs and paths share long prefixes).
//     After every string the text decoded so far is at most
//     kStringExpansion times the bytes read so far plus kStringSlack; the
//     writer stores a string whole (sharedPrefix 0) rather than break that.
//   varint symbolRecordCount
//     per record, sorted by (usr, file, line, flags, size, kind, linkage,
//     name): varint usr - previous usr, varint name, varint file,
//     varint line, varint flags, varint size, u8 kind, u8 linkage,
//     varint contributing TUs, varint smallest contributing TU
//   varint referenceRecordCount
//     per record, sorted by (referrer, target, role, file, line, col,
//     count): varint referrer - previous referrer, varint target, u8 role,
//     varint file, varint line, varint col, varint count,
//     varint contributing TUs, varint smallest contributing TU
//   u64 byte length of the rest (a read-only load skips it)
//   varint tuCount
//     per TU, ascending: varint tu, then its symbol records and its
//     reference records, each as varint runCount and per run
//     (varint gap from the previous run's end, varint length - 1), a run
//     holding at most kMaxRun records (a longer one is written as several
//     with gap 0)
//
// The two limits bound what a load allocates by a constant times the bytes
// it was given, whatever those bytes say.
//
// A string id is the string's rank, a record index its position.

namespace {

void putVarint(std::string &out, uint64_t v) {
  while (v >= 0x80) {
    out.push_back(static_cast<char>(v | 0x80));
    v >>= 7;
  }
  out.push_back(static_cast<char>(v));
}

constexpr uint64_t kStringExpansion = 64;
constexpr uint64_t kStringSlack = 64 * 1024;
constexpr size_t kMaxRun = 64;

void putRuns(std::string &out, const std::vector<uint32_t> &sorted) {
  const auto runEnd = [&](size_t i) {
    size_t j = i + 1;
    while (j < sorted.size() && sorted[j] == sorted[j - 1] + 1)
      ++j;
    return j;
  };
  size_t runs = 0;
  for (size_t i = 0; i < sorted.size(); i = runEnd(i))
    runs += (runEnd(i) - i + kMaxRun - 1) / kMaxRun;
  putVarint(out, runs);
  uint32_t end = 0;
  for (size_t i = 0; i < sorted.size();) {
    const size_t j = runEnd(i);
    for (; i < j; i += std::min(kMaxRun, j - i)) {
      const size_t length = std::min(kMaxRun, j - i);
      putVarint(out, sorted[i] - end);
      putVarint(out, length - 1);
      end = sorted[i] + static_cast<uint32_t>(length);
    }
  }
}

// Bounded reader over untrusted bytes: a read past the end or a varint
// wider than 32 bits clears `ok` and yields 0.
struct ByteReader {
  const unsigned char *p;
  const unsigned char *end;
  bool ok = true;

  [[nodiscard]] size_t remaining() const noexcept {
    return static_cast<size_t>(end - p);
  }
  uint32_t varint() noexcept {
    uint32_t v = 0;
    for (unsigned shift = 0; shift < 35 && p != end; shift += 7) {
      const unsigned char b = *p++;
      if (shift == 28 && (b & 0xF0))
        break;
      v |= static_cast<uint32_t>(b & 0x7F) << shift;
      if (!(b & 0x80))
        return v;
    }
    ok = false;
    return 0;
  }
  uint8_t byte() noexcept {
    if (p == end) {
      ok = false;
      return 0;
    }
    return *p++;
  }
  uint64_t fixed64() noexcept {
    if (remaining() < 8) {
      ok = false;
      return 0;
    }
    uint64_t v = 0;
    for (unsigned i = 0; i < 8; ++i)
      v |= static_cast<uint64_t>(p[i]) << (8 * i);
    p += 8;
    return v;
  }
  // Runs of record indices below `limit`, appended to `out` in ascending
  // order.
  bool runs(uint32_t limit, std::vector<uint32_t> &out) {
    const uint32_t count = varint();
    if (!ok || count > remaining() / 2)
      return ok = false;
    uint64_t pos = 0;
    for (uint32_t i = 0; i < count; ++i) {
      const uint64_t start = pos + varint();
      const uint64_t length = uint64_t(varint()) + 1;
      const uint64_t stop = start + length;
      if (!ok || length > kMaxRun || stop > limit)
        return ok = false;
      for (uint64_t idx = start; idx < stop; ++idx)
        out.push_back(static_cast<uint32_t>(idx));
      pos = stop;
    }
    return true;
  }
};

} // namespace

void ReferenceIndex::encode(std::string &out) const {
  std::lock_guard<std::mutex> lock(mutex_);
  // SnapshotIO::save refuses a read-only index before it gets here.
  assert(!readOnly_ && "a read-only index holds no contributor lists");
  ensureMinTu();

  // The strings the live facts use, in byte order.
  const size_t stringCount = strings_.size();
  std::vector<uint8_t> used(stringCount, 0);
  for (const StoredSymbol &s : symbols_) {
    if (s.refs == 0)
      continue;
    used[s.rec.usr] = used[s.rec.name] = used[s.rec.file] = 1;
    if (s.minTu != kNoId)
      used[s.minTu] = 1;
  }
  for (const StoredReference &r : references_) {
    if (r.refs == 0)
      continue;
    used[r.rec.referrer] = used[r.rec.target] = used[r.rec.file] = 1;
    if (r.rec.locks != kNoId)
      used[r.rec.locks] = 1;
    if (r.minTu != kNoId)
      used[r.minTu] = 1;
  }
  std::vector<Id> tus;
  for (const auto &[tu, contribution] : byTu_)
    if (!contribution.symbols.empty() || !contribution.references.empty()) {
      used[tu] = 1;
      tus.push_back(tu);
    }
  std::vector<std::pair<std::string_view, Id>> order;
  for (size_t id = 0; id < stringCount; ++id)
    if (used[id])
      order.emplace_back(text(static_cast<Id>(id)), static_cast<Id>(id));
  std::sort(order.begin(), order.end());
  std::vector<Id> rank(stringCount, kNoId);
  for (size_t i = 0; i < order.size(); ++i)
    rank[order[i].second] = static_cast<Id>(i);

  putVarint(out, order.size());
  const size_t tableStart = out.size();
  uint64_t textBytes = 0;
  std::string_view prev;
  for (const auto &entry : order) {
    const std::string_view cur = entry.first;
    size_t shared = 0;
    const size_t limit = std::min(prev.size(), cur.size());
    while (shared < limit && prev[shared] == cur[shared])
      ++shared;
    textBytes += cur.size();
    const size_t entryStart = out.size();
    for (;;) {
      putVarint(out, shared);
      putVarint(out, cur.size() - shared);
      out.append(cur.data() + shared, cur.size() - shared);
      if (shared == 0 ||
          textBytes <= kStringExpansion * (out.size() - tableStart) +
                           kStringSlack)
        break;
      out.resize(entryStart); // over the expansion limit: store it whole
      shared = 0;
    }
    prev = cur;
  }

  // Records with on-disk ids, sorted; `from` is the in-memory index.
  struct Sym {
    SymbolRecord rec;
    uint32_t refs;
    Id minTu;
    uint32_t from;
  };
  struct Ref {
    ReferenceRecord rec;
    uint32_t refs;
    Id minTu, maxTu;
    uint32_t from;
  };
  const auto symKey = [](const SymbolRecord &r) {
    return std::make_tuple(r.usr, r.file, r.line, r.flags, r.size,
                           static_cast<uint8_t>(r.kind),
                           static_cast<uint8_t>(r.linkage), r.name);
  };
  // A record without locks sorts before one with (kNoId + 1 wraps to 0).
  const auto refKey = [](const ReferenceRecord &r) {
    return std::make_tuple(r.referrer, r.target, static_cast<uint8_t>(r.role),
                           r.file, r.line, r.col, r.count, r.locks + 1,
                           r.flags);
  };
  const auto rankOfTu = [&](Id tu) { return tu == kNoId ? 0 : rank[tu]; };

  std::vector<Sym> syms;
  syms.reserve(liveSymbols_);
  for (size_t i = 0; i < symbols_.size(); ++i) {
    const StoredSymbol &s = symbols_[i];
    if (s.refs == 0)
      continue;
    Sym e{s.rec, s.refs, rankOfTu(s.minTu), static_cast<uint32_t>(i)};
    e.rec.usr = rank[s.rec.usr];
    e.rec.name = rank[s.rec.name];
    e.rec.file = rank[s.rec.file];
    syms.push_back(e);
  }
  std::sort(syms.begin(), syms.end(), [&](const Sym &a, const Sym &b) {
    return symKey(a.rec) < symKey(b.rec);
  });
  std::vector<uint32_t> symbolPos(symbols_.size(), kNoId);
  putVarint(out, syms.size());
  Id prevId = 0;
  for (size_t i = 0; i < syms.size(); ++i) {
    const Sym &e = syms[i];
    symbolPos[e.from] = static_cast<uint32_t>(i);
    putVarint(out, e.rec.usr - prevId);
    prevId = e.rec.usr;
    putVarint(out, e.rec.name);
    putVarint(out, e.rec.file);
    putVarint(out, e.rec.line);
    putVarint(out, e.rec.flags);
    putVarint(out, e.rec.size);
    out.push_back(static_cast<char>(e.rec.kind));
    out.push_back(static_cast<char>(e.rec.linkage));
    putVarint(out, e.refs);
    putVarint(out, e.minTu);
  }
  syms = {};

  std::vector<Ref> refs;
  refs.reserve(liveReferences_);
  for (size_t i = 0; i < references_.size(); ++i) {
    const StoredReference &r = references_[i];
    if (r.refs == 0)
      continue;
    Ref e{r.rec, r.refs, rankOfTu(r.minTu), rankOfTu(r.maxTu),
          static_cast<uint32_t>(i)};
    e.rec.referrer = rank[r.rec.referrer];
    e.rec.target = rank[r.rec.target];
    e.rec.file = rank[r.rec.file];
    e.rec.locks = r.rec.locks == kNoId ? kNoId : rank[r.rec.locks];
    refs.push_back(e);
  }
  std::sort(refs.begin(), refs.end(), [&](const Ref &a, const Ref &b) {
    return refKey(a.rec) < refKey(b.rec);
  });
  std::vector<uint32_t> referencePos(references_.size(), kNoId);
  putVarint(out, refs.size());
  prevId = 0;
  for (size_t i = 0; i < refs.size(); ++i) {
    const Ref &e = refs[i];
    referencePos[e.from] = static_cast<uint32_t>(i);
    putVarint(out, e.rec.referrer - prevId);
    prevId = e.rec.referrer;
    putVarint(out, e.rec.target);
    out.push_back(static_cast<char>(e.rec.role));
    putVarint(out, e.rec.file);
    putVarint(out, e.rec.line);
    putVarint(out, e.rec.col);
    putVarint(out, e.rec.count);
    putVarint(out, e.rec.locks + 1); // 0 = no lock facts
    out.push_back(static_cast<char>(e.rec.flags));
    putVarint(out, e.refs);
    putVarint(out, e.minTu);
    putVarint(out, e.maxTu);
  }
  refs = {};

  // Contributor lists, TU by TU.
  std::sort(tus.begin(), tus.end(),
            [&](Id a, Id b) { return rank[a] < rank[b]; });
  std::string lists;
  putVarint(lists, tus.size());
  std::vector<uint32_t> positions;
  for (Id tu : tus) {
    const TuContribution &contribution = byTu_.find(tu)->second;
    putVarint(lists, rank[tu]);
    positions.clear();
    for (uint32_t idx : contribution.symbols)
      positions.push_back(symbolPos[idx]);
    std::sort(positions.begin(), positions.end());
    putRuns(lists, positions);
    positions.clear();
    for (uint32_t idx : contribution.references)
      positions.push_back(referencePos[idx]);
    std::sort(positions.begin(), positions.end());
    putRuns(lists, positions);
  }
  const uint64_t listBytes = lists.size();
  for (unsigned i = 0; i < 8; ++i)
    out.push_back(static_cast<char>(listBytes >> (8 * i)));
  out += lists;
}

bool ReferenceIndex::decode(const char *begin, const char *end,
                            bool readOnly) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (strings_.size() != 0 || !symbols_.empty() || !references_.empty() ||
      !byTu_.empty())
    return false;
  ByteReader r{reinterpret_cast<const unsigned char *>(begin),
               reinterpret_cast<const unsigned char *>(end)};

  // Strings: strictly ascending (so unique) index text.
  const uint32_t stringCount = r.varint();
  if (!r.ok || stringCount > r.remaining() / 2)
    return false;
  std::vector<std::string> table;
  table.reserve(stringCount);
  const unsigned char *const tableStart = r.p;
  uint64_t textBytes = 0;
  for (uint32_t i = 0; i < stringCount; ++i) {
    const uint32_t shared = r.varint();
    const uint32_t length = r.varint();
    static const std::string kNone;
    const std::string &prev = table.empty() ? kNone : table.back();
    if (!r.ok || shared > prev.size() || length > r.remaining())
      return false;
    // Front coding repeats the shared prefix for free; the limit keeps the
    // decoded table within a constant factor of its bytes.
    textBytes += uint64_t(shared) + length;
    const uint64_t read = static_cast<uint64_t>(r.p - tableStart) + length;
    if (textBytes > kStringExpansion * read + kStringSlack)
      return false;
    std::string cur;
    cur.reserve(static_cast<size_t>(shared) + length);
    cur.assign(prev, 0, shared);
    cur.append(reinterpret_cast<const char *>(r.p), length);
    r.p += length;
    if ((i != 0 && !(prev < cur)) || !isValidUtf8(cur))
      return false;
    table.push_back(std::move(cur));
  }

  // Records: every id in range, every enum known, strictly ascending (so
  // no two alike, which the dedup tables rely on).
  const uint32_t symbolCount = r.varint();
  if (!r.ok || symbolCount > r.remaining() / 10)
    return false;
  std::vector<StoredSymbol> symbols;
  symbols.reserve(symbolCount);
  const auto symKey = [](const SymbolRecord &rec) {
    return std::make_tuple(rec.usr, rec.file, rec.line, rec.flags, rec.size,
                           static_cast<uint8_t>(rec.kind),
                           static_cast<uint8_t>(rec.linkage), rec.name);
  };
  uint64_t prevId = 0;
  for (uint32_t i = 0; i < symbolCount; ++i) {
    StoredSymbol s;
    const uint64_t usr = prevId + r.varint();
    prevId = usr;
    s.rec.name = r.varint();
    s.rec.file = r.varint();
    s.rec.line = r.varint();
    s.rec.flags = r.varint();
    s.rec.size = r.varint();
    const uint8_t kind = r.byte();
    const uint8_t linkage = r.byte();
    s.refs = r.varint();
    s.minTu = r.varint();
    if (!r.ok || usr >= stringCount || s.rec.name >= stringCount ||
        s.rec.file >= stringCount || s.minTu >= stringCount || s.refs == 0 ||
        (s.rec.flags & ~kSymbolFlagMask) != 0 ||
        kind > static_cast<uint8_t>(SymbolKind::File) ||
        linkage > static_cast<uint8_t>(LinkageClass::None))
      return false;
    s.rec.usr = static_cast<Id>(usr);
    s.rec.kind = static_cast<SymbolKind>(kind);
    s.rec.linkage = static_cast<LinkageClass>(linkage);
    if (i != 0 && !(symKey(symbols.back().rec) < symKey(s.rec)))
      return false;
    symbols.push_back(s);
  }

  const uint32_t referenceCount = r.varint();
  if (!r.ok || referenceCount > r.remaining() / 12)
    return false;
  std::vector<StoredReference> references;
  references.reserve(referenceCount);
  const auto refKey = [](const ReferenceRecord &rec) {
    return std::make_tuple(rec.referrer, rec.target,
                           static_cast<uint8_t>(rec.role), rec.file, rec.line,
                           rec.col, rec.count, rec.locks + 1, rec.flags);
  };
  prevId = 0;
  for (uint32_t i = 0; i < referenceCount; ++i) {
    StoredReference ref;
    const uint64_t referrer = prevId + r.varint();
    prevId = referrer;
    ref.rec.target = r.varint();
    const uint8_t role = r.byte();
    ref.rec.file = r.varint();
    ref.rec.line = r.varint();
    ref.rec.col = r.varint();
    ref.rec.count = r.varint();
    const uint64_t locks = r.varint();
    const uint8_t flags = r.byte();
    ref.refs = r.varint();
    ref.minTu = r.varint();
    ref.maxTu = r.varint();
    // Ids are ranks in path order: the largest contributor sorts last.
    if (!r.ok || referrer >= stringCount || ref.rec.target >= stringCount ||
        ref.rec.file >= stringCount || ref.minTu >= stringCount ||
        ref.maxTu >= stringCount || ref.maxTu < ref.minTu ||
        (ref.refs == 1) != (ref.maxTu == ref.minTu) || ref.refs == 0 ||
        ref.rec.count == 0 || role >= kRefRoleCount ||
        locks > stringCount || (flags & ~kRefFlagMask) != 0)
      return false;
    ref.rec.locks = locks == 0 ? kNoId : static_cast<Id>(locks - 1);
    ref.rec.flags = flags;
    ref.rec.referrer = static_cast<Id>(referrer);
    ref.rec.role = static_cast<RefRole>(role);
    if (i != 0 && !(refKey(references.back().rec) < refKey(ref.rec)))
      return false;
    references.push_back(ref);
  }

  // Contributor lists: they must account for every record's TU count and
  // name its smallest TU, or removeTUs would corrupt the counts later.
  const uint64_t listBytes = r.fixed64();
  if (!r.ok || listBytes != r.remaining())
    return false;
  std::unordered_map<Id, TuContribution> byTu;
  if (!readOnly) {
    const uint32_t tuCount = r.varint();
    if (!r.ok || tuCount > r.remaining() / 3)
      return false;
    std::vector<uint32_t> symbolSeen(symbolCount, 0);
    std::vector<uint32_t> referenceSeen(referenceCount, 0);
    byTu.reserve(tuCount);
    uint64_t prevTu = 0;
    for (uint32_t i = 0; i < tuCount; ++i) {
      const uint32_t tu = r.varint();
      if (!r.ok || tu >= stringCount || (i != 0 && tu <= prevTu))
        return false;
      prevTu = tu;
      TuContribution &contribution = byTu[tu];
      if (!r.runs(symbolCount, contribution.symbols) ||
          !r.runs(referenceCount, contribution.references))
        return false;
      // TUs come in ascending order: the first to list a record is its
      // smallest contributor, the one that completes its count its largest.
      for (uint32_t idx : contribution.symbols)
        if (symbolSeen[idx]++ == 0 ? symbols[idx].minTu != tu
                                   : symbolSeen[idx] > symbols[idx].refs)
          return false;
      for (uint32_t idx : contribution.references) {
        if (referenceSeen[idx]++ == 0
                ? references[idx].minTu != tu
                : referenceSeen[idx] > references[idx].refs)
          return false;
        if ((referenceSeen[idx] == references[idx].refs) !=
            (references[idx].maxTu == tu))
          return false;
      }
    }
    if (r.p != r.end)
      return false;
    for (uint32_t i = 0; i < symbolCount; ++i)
      if (symbolSeen[i] != symbols[i].refs)
        return false;
    for (uint32_t i = 0; i < referenceCount; ++i)
      if (referenceSeen[i] != references[i].refs)
        return false;
  }

  if (!strings_.installStrings(std::move(table)))
    return false;
  symbols_ = std::move(symbols);
  references_ = std::move(references);
  byTu_ = std::move(byTu);
  liveSymbols_ = symbols_.size();
  liveReferences_ = references_.size();
  readOnly_ = readOnly;
  minTuValid_ = true;
  lookupValid_ = false;
  if (!readOnly)
    rebuildTables();
  return true;
}

} // namespace vycor
