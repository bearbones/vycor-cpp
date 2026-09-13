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

#include "vycor/callgraph/ControlFlowIndex.h"

#include "llvm/Support/Endian.h"
#include "llvm/Support/MemoryBuffer.h"

#include <algorithm>
#include <cassert>
#include <tuple>
#include <unordered_set>

namespace vycor {

// The mapped form: the v12 control-flow section's record region, read in
// place. Layout after the set tables, every integer little-endian u32:
//   count, then count records of kRecordBytes (the resident record's
//     fields in order: caller, callee, callerDisplay, calleeDisplay,
//     site, tuPath, scopeSet, guardSet, raiiSet, u8 noexcept, u8
//     insideCatch), stably sorted by site id
//   stringOffsets[stringCount]: each id's length-prefixed entry in the
//     interner table body
//   sortedIds[stringCount]: ids in string order (findId bisects it)
//   byCaller[count], byCallee[count]: record positions by (id, position)
//   n, byCallerDisplay[n]; n, byCalleeDisplay[n]: the positions whose
//     display id differs from the usr id, by (display id, position)
// Nothing is trusted: every offset, id, and position is bounds-checked
// where it is read, and a record that fails validation reads as dead.
struct ControlFlowIndex::MappedStore {
  std::shared_ptr<llvm::MemoryBuffer> buffer;
  const char *strings = nullptr;
  size_t stringBytes = 0;
  uint32_t stringCount = 0;
  const char *stringOffsets = nullptr;
  const char *sortedIds = nullptr;
  const char *records = nullptr;
  uint32_t recordCount = 0;
  const char *byCaller = nullptr;
  const char *byCallee = nullptr;
  const char *byCallerDisplay = nullptr;
  uint32_t callerDisplayCount = 0;
  const char *byCalleeDisplay = nullptr;
  uint32_t calleeDisplayCount = 0;
};

namespace {

// Record field offsets (bytes).
constexpr size_t kFieldCaller = 0;
constexpr size_t kFieldCallee = 4;
constexpr size_t kFieldCallerDisplay = 8;
constexpr size_t kFieldCalleeDisplay = 12;
constexpr size_t kFieldSite = 16;
constexpr size_t kFieldTuPath = 20;
constexpr size_t kFieldScopeSet = 24;
constexpr size_t kFieldGuardSet = 28;
constexpr size_t kFieldRaiiSet = 32;
constexpr size_t kFieldNoexcept = 36;
constexpr size_t kFieldInsideCatch = 37;

uint32_t ldU32(const char *p) { return llvm::support::endian::read32le(p); }

// ----------------------------------------------------------------------------
// Set-table canonical keys. Every variable-length field is length-prefixed so
// the dump is unambiguous (no separator can appear in a field and shift the
// parse); two sets produce the same key iff they are field-for-field equal.
// ----------------------------------------------------------------------------

void keyU32(std::string &key, uint32_t v) {
  key.append(reinterpret_cast<const char *>(&v), sizeof(v));
}

void keyStr(std::string &key, const std::string &s) {
  keyU32(key, static_cast<uint32_t>(s.size()));
  key.append(s);
}

} // anonymous namespace

std::string
ControlFlowIndex::scopeSetKey(const std::vector<TryCatchScope> &scopes) {
  std::string key;
  for (const auto &scope : scopes) {
    keyStr(key, scope.tryLocation);
    keyStr(key, scope.enclosingFunction);
    keyU32(key, scope.nestingDepth);
    keyU32(key, static_cast<uint32_t>(scope.handlers.size()));
    for (const auto &h : scope.handlers) {
      keyStr(key, h.caughtType);
      key.push_back(h.isCatchAll ? 1 : 0);
      key.push_back(h.rethrows ? 1 : 0);
      keyStr(key, h.location);
      keyStr(key, h.bodySummary);
    }
  }
  return key;
}

std::string
ControlFlowIndex::guardSetKey(const std::vector<ConditionalGuard> &guards) {
  std::string key;
  for (const auto &g : guards) {
    keyStr(key, g.conditionText);
    keyStr(key, g.location);
    key.push_back(g.inTrueBranch ? 1 : 0);
    key.push_back(g.isAssertion ? 1 : 0);
  }
  return key;
}

ControlFlowIndex::ControlFlowIndex() {
  // Seed index 0 of each set table with the empty set, so "no scopes/guards/
  // locals" is always set 0 (protectedCallsTo tests scopeSet != 0 directly).
  scopeSets_.emplace_back();
  guardSets_.emplace_back();
  raiiSets_.emplace_back();
}

ControlFlowIndex::~ControlFlowIndex() = default;

ControlFlowIndex::ControlFlowIndex(ControlFlowIndex &&other) noexcept
    : mapped_(std::move(other.mapped_)), interner_(std::move(other.interner_)),
      contexts_(std::move(other.contexts_)),
      scopeSets_(std::move(other.scopeSets_)),
      guardSets_(std::move(other.guardSets_)),
      raiiSets_(std::move(other.raiiSets_)),
      scopeSetIds_(std::move(other.scopeSetIds_)),
      guardSetIds_(std::move(other.guardSetIds_)),
      raiiSetIds_(std::move(other.raiiSetIds_)),
      byCallee_(std::move(other.byCallee_)),
      byCaller_(std::move(other.byCaller_)),
      byCalleeDisplay_(std::move(other.byCalleeDisplay_)),
      byCallerDisplay_(std::move(other.byCallerDisplay_)),
      bySite_(std::move(other.bySite_)), byTu_(std::move(other.byTu_)),
      noProvenance_(std::move(other.noProvenance_)),
      liveCount_(other.liveCount_) {}

ControlFlowIndex &
ControlFlowIndex::operator=(ControlFlowIndex &&other) noexcept {
  mapped_ = std::move(other.mapped_);
  interner_ = std::move(other.interner_);
  contexts_ = std::move(other.contexts_);
  scopeSets_ = std::move(other.scopeSets_);
  guardSets_ = std::move(other.guardSets_);
  raiiSets_ = std::move(other.raiiSets_);
  scopeSetIds_ = std::move(other.scopeSetIds_);
  guardSetIds_ = std::move(other.guardSetIds_);
  raiiSetIds_ = std::move(other.raiiSetIds_);
  byCallee_ = std::move(other.byCallee_);
  byCaller_ = std::move(other.byCaller_);
  byCalleeDisplay_ = std::move(other.byCalleeDisplay_);
  byCallerDisplay_ = std::move(other.byCallerDisplay_);
  bySite_ = std::move(other.bySite_);
  byTu_ = std::move(other.byTu_);
  noProvenance_ = std::move(other.noProvenance_);
  liveCount_ = other.liveCount_;
  return *this;
}

void ControlFlowIndex::reserveContexts(size_t n) {
  assert(!mapped_ && "mapped control-flow index is read-only");
  if (mapped_)
    return;
  std::lock_guard<std::mutex> lock(mutex_);
  byCallee_.reserve(n);
  byCaller_.reserve(n);
  bySite_.reserve(n);
}

uint32_t ControlFlowIndex::internScopeSet(std::string key,
                                          std::vector<TryCatchScope> scopes) {
  if (scopes.empty())
    return 0;
  auto it = scopeSetIds_.find(key);
  if (it != scopeSetIds_.end())
    return it->second;
  uint32_t id = static_cast<uint32_t>(scopeSets_.size());
  scopeSets_.push_back(std::move(scopes));
  scopeSetIds_.emplace(std::move(key), id);
  return id;
}

uint32_t
ControlFlowIndex::internGuardSet(std::string key,
                                 std::vector<ConditionalGuard> guards) {
  if (guards.empty())
    return 0;
  auto it = guardSetIds_.find(key);
  if (it != guardSetIds_.end())
    return it->second;
  uint32_t id = static_cast<uint32_t>(guardSets_.size());
  guardSets_.push_back(std::move(guards));
  guardSetIds_.emplace(std::move(key), id);
  return id;
}

std::string
ControlFlowIndex::raiiSetKey(const std::vector<StoredRaiiLocal> &locals) {
  // RaiiLocal strings are already interned, so the ids are the identity.
  std::string key;
  for (const auto &l : locals) {
    keyU32(key, l.typeName);
    keyU32(key, l.varName);
    keyU32(key, l.declLocation);
    key.push_back(static_cast<char>(l.kind));
  }
  return key;
}

uint32_t ControlFlowIndex::internRaiiSet(std::string key,
                                         std::vector<StoredRaiiLocal> locals) {
  if (locals.empty())
    return 0;
  auto it = raiiSetIds_.find(key);
  if (it != raiiSetIds_.end())
    return it->second;
  uint32_t id = static_cast<uint32_t>(raiiSets_.size());
  raiiSets_.push_back(std::move(locals));
  raiiSetIds_.emplace(std::move(key), id);
  return id;
}

void ControlFlowIndex::insertStored(SId caller, SId callee, SId callerDisplay,
                                    SId calleeDisplay, SId site, SId tuPath,
                                    uint32_t scopeSet, uint32_t guardSet,
                                    uint32_t raiiSet,
                                    NoexceptSpec callerNoexcept,
                                    bool insideCatchBlock, bool trackTu) {
  size_t idx = contexts_.size();
  byCallee_[callee].push_back(idx);
  byCaller_[caller].push_back(idx);
  // The display twins carry only contexts whose display differs from the
  // usr; the usr maps already cover the coinciding (name-only) case.
  if (calleeDisplay != callee)
    byCalleeDisplay_[calleeDisplay].push_back(idx);
  if (callerDisplay != caller)
    byCallerDisplay_[callerDisplay].push_back(idx);
  bySite_[site].push_back(idx);
  if (!trackTu) {
    // Read-only load: nothing will ever removeTU/compact this index.
  } else if (tuPath != kNoString) {
    byTu_[tuPath].push_back(idx);
  } else {
    noProvenance_.push_back(idx);
  }
  contexts_.push_back(StoredContext{caller, callee, callerDisplay,
                                    calleeDisplay, site, tuPath, scopeSet,
                                    guardSet, raiiSet, callerNoexcept,
                                    insideCatchBlock, /*live=*/true});
  ++liveCount_;
}

void ControlFlowIndex::addCallSiteContext(CallSiteContext ctx) {
  assert(!mapped_ && "mapped control-flow index is read-only");
  if (mapped_)
    return;
  // Intern strings and build set dedup keys BEFORE taking mutex_: the
  // interner has its own reader/writer lock, so only the set-table lookups
  // and the index insert need the exclusive index mutex. Keeps the critical
  // section O(map ops) instead of O(strings) as worker counts grow
  // (measured neutral at 12 threads; the whole insert path is ~3.4us per
  // context).
  SId calleeDisplayId = interner_.intern(ctx.calleeName);
  SId callerDisplayId = interner_.intern(ctx.callerName);
  // Name-only contexts (hand-built tests, legacy producers) key by display.
  SId calleeId = ctx.calleeUsr.empty() ? calleeDisplayId
                                       : interner_.intern(ctx.calleeUsr);
  SId callerId = ctx.callerUsr.empty() ? callerDisplayId
                                       : interner_.intern(ctx.callerUsr);
  SId siteId = interner_.intern(ctx.callSite);
  SId tuId = ctx.tuPath.empty() ? kNoString : interner_.intern(ctx.tuPath);

  std::vector<StoredRaiiLocal> locals;
  locals.reserve(ctx.liveRaiiLocals.size());
  for (const auto &l : ctx.liveRaiiLocals)
    locals.push_back(StoredRaiiLocal{interner_.intern(l.typeName),
                                     interner_.intern(l.varName),
                                     interner_.intern(l.declLocation), l.kind});

  std::string scopeKey = ctx.enclosingTryCatches.empty()
                             ? std::string()
                             : scopeSetKey(ctx.enclosingTryCatches);
  std::string guardKey = ctx.enclosingGuards.empty()
                             ? std::string()
                             : guardSetKey(ctx.enclosingGuards);
  std::string raiiKey = raiiSetKey(locals);

  std::lock_guard<std::mutex> lock(mutex_);
  uint32_t scopeSet = internScopeSet(std::move(scopeKey),
                                     std::move(ctx.enclosingTryCatches));
  uint32_t guardSet =
      internGuardSet(std::move(guardKey), std::move(ctx.enclosingGuards));
  uint32_t raiiSet = internRaiiSet(std::move(raiiKey), std::move(locals));

  insertStored(callerId, calleeId, callerDisplayId, calleeDisplayId, siteId,
               tuId, scopeSet, guardSet, raiiSet, ctx.callerNoexcept,
               ctx.insideCatchBlock);
}

CallSiteContext ControlFlowIndex::materialize(const StoredContext &se) const {
  CallSiteContext ctx;
  ctx.callerName = stringOf(se.callerDisplay);
  ctx.calleeName = stringOf(se.calleeDisplay);
  ctx.callerUsr = stringOf(se.caller);
  ctx.calleeUsr = stringOf(se.callee);
  ctx.callSite = stringOf(se.site);
  if (se.tuPath != kNoString)
    ctx.tuPath = stringOf(se.tuPath);
  ctx.enclosingTryCatches = scopeSets_[se.scopeSet];
  ctx.enclosingGuards = guardSets_[se.guardSet];
  ctx.callerNoexcept = se.callerNoexcept;
  ctx.insideCatchBlock = se.insideCatchBlock;
  const auto &locals = raiiSets_[se.raiiSet];
  ctx.liveRaiiLocals.reserve(locals.size());
  for (const auto &l : locals)
    ctx.liveRaiiLocals.push_back(RaiiLocal{stringOf(l.typeName),
                                           stringOf(l.varName),
                                           stringOf(l.declLocation), l.kind});
  return ctx;
}

// ----------------------------------------------------------------------------
// The mapped form
// ----------------------------------------------------------------------------

bool ControlFlowIndex::attachMapped(std::shared_ptr<llvm::MemoryBuffer> buffer,
                                    const char *strings, size_t stringBytes,
                                    uint32_t stringCount, const char *&p,
                                    const char *end) {
  auto m = std::make_unique<MappedStore>();
  m->buffer = std::move(buffer);
  m->strings = strings;
  m->stringBytes = stringBytes;
  m->stringCount = stringCount;
  auto remaining = [&]() { return static_cast<uint64_t>(end - p); };
  auto takeU32 = [&](uint32_t &v) {
    if (remaining() < 4)
      return false;
    v = ldU32(p);
    p += 4;
    return true;
  };
  // An array of `n` items of `bytes` each, or null when short.
  auto takeArray = [&](uint64_t n, uint64_t bytes) -> const char * {
    if (remaining() < n * bytes)
      return nullptr;
    const char *at = p;
    p += n * bytes;
    return at;
  };
  if (!takeU32(m->recordCount) || remaining() / kRecordBytes < m->recordCount)
    return false;
  m->records = takeArray(m->recordCount, kRecordBytes);
  m->stringOffsets = takeArray(stringCount, 4);
  m->sortedIds = takeArray(stringCount, 4);
  m->byCaller = takeArray(m->recordCount, 4);
  m->byCallee = takeArray(m->recordCount, 4);
  if (!m->records || !m->stringOffsets || !m->sortedIds || !m->byCaller ||
      !m->byCallee || !takeU32(m->callerDisplayCount) ||
      m->callerDisplayCount > m->recordCount)
    return false;
  m->byCallerDisplay = takeArray(m->callerDisplayCount, 4);
  if (!m->byCallerDisplay || !takeU32(m->calleeDisplayCount) ||
      m->calleeDisplayCount > m->recordCount)
    return false;
  m->byCalleeDisplay = takeArray(m->calleeDisplayCount, 4);
  if (!m->byCalleeDisplay)
    return false;
  liveCount_ = m->recordCount;
  mapped_ = std::move(m);
  return true;
}

std::string_view ControlFlowIndex::mappedString(SId id) const {
  const MappedStore &m = *mapped_;
  if (id >= m.stringCount)
    return {};
  const uint32_t off = ldU32(m.stringOffsets + size_t(id) * 4);
  if (off > m.stringBytes || m.stringBytes - off < 4)
    return {};
  const uint32_t len = ldU32(m.strings + off);
  if (m.stringBytes - off - 4 < len)
    return {};
  return std::string_view(m.strings + off + 4, len);
}

ControlFlowIndex::StoredContext
ControlFlowIndex::mappedRecord(uint32_t pos) const {
  const MappedStore &m = *mapped_;
  StoredContext se{};
  se.live = false;
  if (pos >= m.recordCount)
    return se;
  const char *r = m.records + size_t(pos) * kRecordBytes;
  se.caller = ldU32(r + kFieldCaller);
  se.callee = ldU32(r + kFieldCallee);
  se.callerDisplay = ldU32(r + kFieldCallerDisplay);
  se.calleeDisplay = ldU32(r + kFieldCalleeDisplay);
  se.site = ldU32(r + kFieldSite);
  se.tuPath = ldU32(r + kFieldTuPath);
  se.scopeSet = ldU32(r + kFieldScopeSet);
  se.guardSet = ldU32(r + kFieldGuardSet);
  se.raiiSet = ldU32(r + kFieldRaiiSet);
  se.callerNoexcept =
      static_cast<NoexceptSpec>(static_cast<uint8_t>(r[kFieldNoexcept]));
  se.insideCatchBlock = r[kFieldInsideCatch] != 0;
  auto validId = [&](SId id) { return id < m.stringCount; };
  se.live = validId(se.caller) && validId(se.callee) &&
            validId(se.callerDisplay) && validId(se.calleeDisplay) &&
            validId(se.site) &&
            (se.tuPath == kNoString || validId(se.tuPath)) &&
            se.scopeSet < scopeSets_.size() &&
            se.guardSet < guardSets_.size() && se.raiiSet < raiiSets_.size();
  return se;
}

std::pair<uint32_t, uint32_t> ControlFlowIndex::mappedRange(const char *order,
                                                            uint32_t count,
                                                            size_t field,
                                                            SId id) const {
  const MappedStore &m = *mapped_;
  // A position out of range sorts after every id, so a corrupt entry can
  // only shorten a range, never send a read past the records.
  auto fieldAt = [&](uint32_t i) -> uint64_t {
    const uint32_t pos = order ? ldU32(order + size_t(i) * 4) : i;
    if (pos >= m.recordCount)
      return UINT64_MAX;
    return ldU32(m.records + size_t(pos) * kRecordBytes + field);
  };
  uint32_t lo = 0, hi = count;
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    if (fieldAt(mid) < id)
      lo = mid + 1;
    else
      hi = mid;
  }
  const uint32_t first = lo;
  hi = count;
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    if (fieldAt(mid) <= id)
      lo = mid + 1;
    else
      hi = mid;
  }
  return {first, lo};
}

std::string ControlFlowIndex::stringOf(SId id) const {
  if (mapped_)
    return std::string(mappedString(id));
  if (id >= interner_.size())
    return std::string();
  return interner_.resolve(id);
}

std::optional<ControlFlowIndex::SId>
ControlFlowIndex::findId(const std::string &s) const {
  if (!mapped_)
    return interner_.find(s);
  const MappedStore &m = *mapped_;
  const std::string_view key(s);
  uint32_t lo = 0, hi = m.stringCount;
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    const SId id = ldU32(m.sortedIds + size_t(mid) * 4);
    const int c = mappedString(id).compare(key);
    if (c == 0)
      return id;
    if (c < 0)
      lo = mid + 1;
    else
      hi = mid;
  }
  return std::nullopt;
}

ControlFlowIndex::StoredContext ControlFlowIndex::storedAt(size_t pos) const {
  if (mapped_)
    return mappedRecord(static_cast<uint32_t>(pos));
  return contexts_[pos];
}

std::vector<size_t>
ControlFlowIndex::sitePositions(const std::string &callSite) const {
  std::vector<size_t> out;
  auto id = findId(callSite);
  if (!id)
    return out;
  if (mapped_) {
    auto [first, last] =
        mappedRange(nullptr, mapped_->recordCount, kFieldSite, *id);
    for (uint32_t i = first; i < last; ++i)
      out.push_back(i);
    return out;
  }
  auto it = bySite_.find(*id);
  if (it != bySite_.end())
    out.assign(it->second.begin(), it->second.end());
  return out;
}

std::vector<size_t>
ControlFlowIndex::namePositions(Side side, const std::string &name) const {
  std::vector<size_t> out;
  auto id = findId(name);
  if (!id)
    return out;
  if (mapped_) {
    const MappedStore &m = *mapped_;
    const bool caller = side == Side::Caller;
    const char *order = caller ? m.byCaller : m.byCallee;
    auto [first, last] = mappedRange(order, m.recordCount,
                                     caller ? kFieldCaller : kFieldCallee, *id);
    if (first == last) {
      order = caller ? m.byCallerDisplay : m.byCalleeDisplay;
      std::tie(first, last) = mappedRange(
          order, caller ? m.callerDisplayCount : m.calleeDisplayCount,
          caller ? kFieldCallerDisplay : kFieldCalleeDisplay, *id);
    }
    for (uint32_t i = first; i < last; ++i)
      out.push_back(ldU32(order + size_t(i) * 4));
    return out;
  }
  const auto &usrMap = side == Side::Caller ? byCaller_ : byCallee_;
  const auto &displayMap =
      side == Side::Caller ? byCallerDisplay_ : byCalleeDisplay_;
  auto it = usrMap.find(*id);
  if (it != usrMap.end()) {
    out.assign(it->second.begin(), it->second.end());
    return out;
  }
  auto dit = displayMap.find(*id);
  if (dit != displayMap.end())
    out.assign(dit->second.begin(), dit->second.end());
  return out;
}

// ----------------------------------------------------------------------------
// Queries (both forms)
// ----------------------------------------------------------------------------

std::optional<CallSiteContext>
ControlFlowIndex::contextAtSite(const std::string &callSite) const {
  // Several contexts can share a spelling (macro expansion): return the
  // first live one; the caller-qualified overload picks a specific one.
  for (size_t pos : sitePositions(callSite)) {
    StoredContext se = storedAt(pos);
    if (se.live)
      return materialize(se);
  }
  return std::nullopt;
}

std::optional<CallSiteContext>
ControlFlowIndex::contextAtSite(const std::string &callSite,
                                const std::string &callerUsrOrName) const {
  auto callerId = findId(callerUsrOrName);
  if (!callerId)
    return std::nullopt;
  for (size_t pos : sitePositions(callSite)) {
    StoredContext se = storedAt(pos);
    if (!se.live)
      continue;
    if (se.caller == *callerId || se.callerDisplay == *callerId)
      return materialize(se);
  }
  return std::nullopt;
}

std::optional<CallSiteContext>
ControlFlowIndex::contextForEdge(const std::string &callSite,
                                 const std::string &callerUsr,
                                 const std::string &calleeUsr) const {
  auto contexts = contextsAtSite(callSite);
  std::vector<CallSiteContext> mine;
  for (auto &ctx : contexts) {
    if (ctx.callerUsr == callerUsr || ctx.callerName == callerUsr)
      mine.push_back(std::move(ctx));
  }
  if (mine.empty())
    return std::nullopt;
  // Deterministic choice: prefer the hop's callee, then the smallest
  // callee usr, then the TU that recorded the context (two TUs can see
  // one header call site under different macro state), then the caller
  // spelling. Never insertion order.
  std::stable_sort(mine.begin(), mine.end(),
                   [&](const CallSiteContext &a, const CallSiteContext &b) {
                     const bool am = a.calleeUsr == calleeUsr;
                     const bool bm = b.calleeUsr == calleeUsr;
                     if (am != bm)
                       return am;
                     if (a.calleeUsr != b.calleeUsr)
                       return a.calleeUsr < b.calleeUsr;
                     if (a.tuPath != b.tuPath)
                       return a.tuPath < b.tuPath;
                     return a.callerName < b.callerName;
                   });
  return std::move(mine.front());
}

std::vector<CallSiteContext>
ControlFlowIndex::contextsAtSite(const std::string &callSite) const {
  std::vector<CallSiteContext> result;
  for (size_t pos : sitePositions(callSite)) {
    StoredContext se = storedAt(pos);
    if (se.live)
      result.push_back(materialize(se));
  }
  return result;
}

std::vector<CallSiteContext>
ControlFlowIndex::contextsForCallee(const std::string &calleeName) const {
  std::vector<CallSiteContext> result;
  for (size_t pos : namePositions(Side::Callee, calleeName)) {
    StoredContext se = storedAt(pos);
    if (se.live)
      result.push_back(materialize(se));
  }
  return result;
}

std::optional<NoexceptSpec>
ControlFlowIndex::callerNoexceptOf(const std::string &caller) const {
  for (size_t pos : namePositions(Side::Caller, caller)) {
    StoredContext se = storedAt(pos);
    if (se.live)
      return se.callerNoexcept;
  }
  return std::nullopt;
}

std::vector<CallSiteContext>
ControlFlowIndex::contextsForCaller(const std::string &callerName) const {
  std::vector<CallSiteContext> result;
  for (size_t pos : namePositions(Side::Caller, callerName)) {
    StoredContext se = storedAt(pos);
    if (se.live)
      result.push_back(materialize(se));
  }
  return result;
}

std::vector<CallSiteContext>
ControlFlowIndex::protectedCallsTo(const std::string &calleeName) const {
  std::vector<CallSiteContext> result;
  for (size_t pos : namePositions(Side::Callee, calleeName)) {
    StoredContext se = storedAt(pos);
    // scopeSet 0 is the empty set: != 0 <=> enclosingTryCatches non-empty.
    if (se.live && se.scopeSet != 0)
      result.push_back(materialize(se));
  }
  return result;
}

std::vector<CallSiteContext>
ControlFlowIndex::unprotectedCallsTo(const std::string &calleeName) const {
  std::vector<CallSiteContext> result;
  for (size_t pos : namePositions(Side::Callee, calleeName)) {
    StoredContext se = storedAt(pos);
    if (se.live && se.scopeSet == 0)
      result.push_back(materialize(se));
  }
  return result;
}

void ControlFlowIndex::forEachContext(
    llvm::function_ref<void(const CallSiteContext &)> fn) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (mapped_) {
    for (uint32_t pos = 0; pos < mapped_->recordCount; ++pos) {
      StoredContext se = mappedRecord(pos);
      if (se.live)
        fn(materialize(se));
    }
    return;
  }
  for (const auto &se : contexts_) {
    if (se.live)
      fn(materialize(se));
  }
}

bool ControlFlowIndex::ContextShape::operator<(const ContextShape &o) const {
  return std::tie(scopeSet, guardSet, raiiSet, callerNoexcept,
                  insideCatchBlock) <
         std::tie(o.scopeSet, o.guardSet, o.raiiSet, o.callerNoexcept,
                  o.insideCatchBlock);
}

void ControlFlowIndex::forEachContextRecord(
    llvm::function_ref<void(const ContextRecord &)> fn) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto emit = [&](const StoredContext &se) {
    ContextRecord r;
    r.callSite = se.site;
    r.callerUsr = se.caller;
    r.callerName = se.callerDisplay;
    r.calleeUsr = se.callee;
    r.tuPath = se.tuPath;
    r.shape = {se.scopeSet, se.guardSet, se.raiiSet, se.callerNoexcept,
               se.insideCatchBlock};
    fn(r);
  };
  if (mapped_) {
    for (uint32_t pos = 0; pos < mapped_->recordCount; ++pos) {
      StoredContext se = mappedRecord(pos);
      if (se.live)
        emit(se);
    }
    return;
  }
  for (const auto &se : contexts_) {
    if (se.live)
      emit(se);
  }
}

CallSiteContext
ControlFlowIndex::contextOfShape(const ContextShape &shape) const {
  std::lock_guard<std::mutex> lock(mutex_);
  CallSiteContext ctx;
  if (shape.scopeSet >= scopeSets_.size() ||
      shape.guardSet >= guardSets_.size() ||
      shape.raiiSet >= raiiSets_.size())
    return ctx;
  ctx.enclosingTryCatches = scopeSets_[shape.scopeSet];
  ctx.enclosingGuards = guardSets_[shape.guardSet];
  ctx.callerNoexcept = shape.callerNoexcept;
  ctx.insideCatchBlock = shape.insideCatchBlock;
  for (const auto &l : raiiSets_[shape.raiiSet])
    ctx.liveRaiiLocals.push_back(RaiiLocal{stringOf(l.typeName),
                                           stringOf(l.varName),
                                           stringOf(l.declLocation), l.kind});
  return ctx;
}

std::vector<CallSiteContext> ControlFlowIndex::allContexts() const {
  std::vector<CallSiteContext> result;
  result.reserve(liveCount_);
  forEachContext([&](const CallSiteContext &ctx) { result.push_back(ctx); });
  return result;
}

void ControlFlowIndex::absorb(const ControlFlowIndex &shard) {
  assert(!mapped_ && !shard.mapped_ &&
         "mapped control-flow index is read-only");
  if (&shard == this || mapped_ || shard.mapped_)
    return;
  std::lock_guard<std::mutex> lockThis(mutex_);
  std::lock_guard<std::mutex> lockShard(shard.mutex_);

  // Shard string id -> master string id, by position.
  std::vector<SId> remap;
  remap.reserve(shard.interner_.size());
  shard.interner_.forEachString(
      [&](const std::string &s) { remap.push_back(interner_.intern(s)); });

  // Set tables: shard index -> master index through the dedup key maps.
  // Index 0 is the seeded empty set on both sides. Scope/guard tables hold
  // plain strings, so their keys need no remap; the RAII table is in id
  // space and must be remapped BEFORE its key is built.
  std::vector<uint32_t> scopeRemap(shard.scopeSets_.size(), 0);
  for (uint32_t i = 1; i < shard.scopeSets_.size(); ++i)
    scopeRemap[i] =
        internScopeSet(scopeSetKey(shard.scopeSets_[i]), shard.scopeSets_[i]);
  std::vector<uint32_t> guardRemap(shard.guardSets_.size(), 0);
  for (uint32_t i = 1; i < shard.guardSets_.size(); ++i)
    guardRemap[i] =
        internGuardSet(guardSetKey(shard.guardSets_[i]), shard.guardSets_[i]);
  std::vector<uint32_t> raiiRemap(shard.raiiSets_.size(), 0);
  for (uint32_t i = 1; i < shard.raiiSets_.size(); ++i) {
    std::vector<StoredRaiiLocal> locals = shard.raiiSets_[i];
    for (auto &l : locals) {
      l.typeName = remap[l.typeName];
      l.varName = remap[l.varName];
      l.declLocation = remap[l.declLocation];
    }
    std::string key = raiiSetKey(locals);
    raiiRemap[i] = internRaiiSet(std::move(key), std::move(locals));
  }

  for (const auto &se : shard.contexts_) {
    if (!se.live)
      continue;
    SId tuId = se.tuPath == kNoString ? kNoString : remap[se.tuPath];
    insertStored(remap[se.caller], remap[se.callee], remap[se.callerDisplay],
                 remap[se.calleeDisplay], remap[se.site], tuId,
                 scopeRemap[se.scopeSet], guardRemap[se.guardSet],
                 raiiRemap[se.raiiSet], se.callerNoexcept,
                 se.insideCatchBlock);
  }
}

size_t ControlFlowIndex::removeTU(const std::string &tuPath) {
  return removeTUs({tuPath});
}

size_t ControlFlowIndex::removeTUs(const std::vector<std::string> &tuPaths) {
  assert(!mapped_ && "mapped control-flow index is read-only");
  if (mapped_)
    return 0;
  std::lock_guard<std::mutex> lock(mutex_);
  size_t removed = 0;

  // Candidates: exactly these TUs' contexts via the reverse index, plus the
  // no-provenance list (legacy fallback matches on a callSite prefix).
  // The old implementation scanned every stored context per removeTU and
  // scrubbed byCallee_/byCaller_ once per removed context (O(degree)
  // each); candidates are now O(TU size) and each affected adjacency
  // vector is scrubbed once for the whole set of TUs.
  std::vector<std::string> prefixes;
  std::vector<size_t> candidates;
  for (const auto &tuPath : tuPaths) {
    prefixes.push_back(tuPath + ":");
    if (auto tuId = interner_.find(tuPath)) {
      auto it = byTu_.find(*tuId);
      if (it != byTu_.end())
        candidates.insert(candidates.end(), it->second.begin(),
                          it->second.end());
    }
  }
  size_t provenanced = candidates.size();
  candidates.insert(candidates.end(), noProvenance_.begin(),
                    noProvenance_.end());
  auto fromRemovedTu = [&](const StoredContext &se) {
    const std::string &site = interner_.resolve(se.site);
    for (const auto &prefix : prefixes)
      if (site.compare(0, prefix.size(), prefix) == 0)
        return true;
    return false;
  };

  std::unordered_set<size_t> dead;
  std::unordered_set<SId> affectedCallees, affectedCallers;
  std::unordered_set<SId> affectedCalleeDisplays, affectedCallerDisplays;
  std::unordered_set<SId> affectedSites;
  for (size_t n = 0; n < candidates.size(); ++n) {
    size_t i = candidates[n];
    StoredContext &se = contexts_[i];
    if (!se.live)
      continue; // tombstoned earlier
    if (n >= provenanced && !fromRemovedTu(se))
      continue; // no-provenance context from a different TU

    affectedCallees.insert(se.callee);
    affectedCallers.insert(se.caller);
    if (se.calleeDisplay != se.callee)
      affectedCalleeDisplays.insert(se.calleeDisplay);
    if (se.callerDisplay != se.caller)
      affectedCallerDisplays.insert(se.callerDisplay);
    affectedSites.insert(se.site);

    dead.insert(i);
    se.live = false;
    --liveCount_;
    ++removed;
  }

  auto scrub = [&dead](std::vector<size_t> &v) {
    v.erase(std::remove_if(v.begin(), v.end(),
                           [&](size_t i) { return dead.count(i) > 0; }),
            v.end());
  };
  for (SId id : affectedCallees)
    scrub(byCallee_[id]);
  for (SId id : affectedCallers)
    scrub(byCaller_[id]);
  for (SId id : affectedCalleeDisplays)
    scrub(byCalleeDisplay_[id]);
  for (SId id : affectedCallerDisplays)
    scrub(byCallerDisplay_[id]);
  for (SId id : affectedSites) {
    auto it = bySite_.find(id);
    if (it == bySite_.end())
      continue;
    scrub(it->second);
    if (it->second.empty())
      bySite_.erase(it);
  }
  if (!dead.empty()) {
    for (const auto &tuPath : tuPaths) {
      if (auto tuId = interner_.find(tuPath)) {
        auto it = byTu_.find(*tuId);
        if (it != byTu_.end())
          byTu_.erase(it);
      }
    }
    scrub(noProvenance_);
  }
  return removed;
}

void ControlFlowIndex::compact() {
  assert(!mapped_ && "mapped control-flow index is read-only");
  if (mapped_)
    return;
  std::lock_guard<std::mutex> lock(mutex_);
  std::deque<StoredContext> newCtx;

  std::unordered_map<SId, std::vector<size_t>> newByCallee;
  std::unordered_map<SId, std::vector<size_t>> newByCaller;
  std::unordered_map<SId, std::vector<size_t>> newByCalleeDisplay;
  std::unordered_map<SId, std::vector<size_t>> newByCallerDisplay;
  std::unordered_map<SId, std::vector<size_t>> newBySite;
  std::unordered_map<SId, std::vector<size_t>> newByTu;
  std::vector<size_t> newNoProvenance;

  // Set tables are shared and index-stable: only the contexts and the
  // index maps are rewritten.
  for (const auto &se : contexts_) {
    if (!se.live)
      continue;
    size_t idx = newCtx.size();
    newByCallee[se.callee].push_back(idx);
    newByCaller[se.caller].push_back(idx);
    if (se.calleeDisplay != se.callee)
      newByCalleeDisplay[se.calleeDisplay].push_back(idx);
    if (se.callerDisplay != se.caller)
      newByCallerDisplay[se.callerDisplay].push_back(idx);
    newBySite[se.site].push_back(idx);
    if (se.tuPath != kNoString)
      newByTu[se.tuPath].push_back(idx);
    else
      newNoProvenance.push_back(idx);
    newCtx.push_back(se);
  }

  contexts_ = std::move(newCtx);
  byCallee_ = std::move(newByCallee);
  byCaller_ = std::move(newByCaller);
  byCalleeDisplay_ = std::move(newByCalleeDisplay);
  byCallerDisplay_ = std::move(newByCallerDisplay);
  bySite_ = std::move(newBySite);
  byTu_ = std::move(newByTu);
  noProvenance_ = std::move(newNoProvenance);
}

} // namespace vycor
