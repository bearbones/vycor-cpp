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

#include <algorithm>
#include <unordered_set>

namespace vycor {

namespace {

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

ControlFlowIndex::ControlFlowIndex(ControlFlowIndex &&other) noexcept
    : interner_(std::move(other.interner_)),
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
      bySite_(std::move(other.bySite_)),
      byTu_(std::move(other.byTu_)),
      noProvenance_(std::move(other.noProvenance_)),
      liveCount_(other.liveCount_) {}

ControlFlowIndex &ControlFlowIndex::operator=(ControlFlowIndex &&other) noexcept {
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
                                    bool insideCatchBlock) {
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
  if (tuPath != kNoString)
    byTu_[tuPath].push_back(idx);
  else
    noProvenance_.push_back(idx);
  contexts_.push_back(StoredContext{caller, callee, callerDisplay,
                                    calleeDisplay, site, tuPath, scopeSet,
                                    guardSet, raiiSet, callerNoexcept,
                                    insideCatchBlock, /*live=*/true});
  ++liveCount_;
}

void ControlFlowIndex::addCallSiteContext(CallSiteContext ctx) {
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
  ctx.callerName = interner_.resolve(se.callerDisplay);
  ctx.calleeName = interner_.resolve(se.calleeDisplay);
  ctx.callerUsr = interner_.resolve(se.caller);
  ctx.calleeUsr = interner_.resolve(se.callee);
  ctx.callSite = interner_.resolve(se.site);
  if (se.tuPath != kNoString)
    ctx.tuPath = interner_.resolve(se.tuPath);
  ctx.enclosingTryCatches = scopeSets_[se.scopeSet];
  ctx.enclosingGuards = guardSets_[se.guardSet];
  ctx.callerNoexcept = se.callerNoexcept;
  ctx.insideCatchBlock = se.insideCatchBlock;
  const auto &locals = raiiSets_[se.raiiSet];
  ctx.liveRaiiLocals.reserve(locals.size());
  for (const auto &l : locals)
    ctx.liveRaiiLocals.push_back(RaiiLocal{interner_.resolve(l.typeName),
                                           interner_.resolve(l.varName),
                                           interner_.resolve(l.declLocation),
                                           l.kind});
  return ctx;
}

const std::vector<size_t> *ControlFlowIndex::indicesFor(
    const std::unordered_map<SId, std::vector<size_t>> &usrMap,
    const std::unordered_map<SId, std::vector<size_t>> &displayMap,
    const std::string &name) const {
  auto id = interner_.find(name);
  if (!id)
    return nullptr;
  auto it = usrMap.find(*id);
  if (it != usrMap.end())
    return &it->second;
  auto dit = displayMap.find(*id);
  if (dit != displayMap.end())
    return &dit->second;
  return nullptr;
}

std::optional<CallSiteContext>
ControlFlowIndex::contextAtSite(const std::string &callSite) const {
  auto id = interner_.find(callSite);
  if (!id)
    return std::nullopt;
  auto it = bySite_.find(*id);
  if (it == bySite_.end())
    return std::nullopt;
  // Several contexts can share a spelling (macro expansion): return the
  // first live one; the caller-qualified overload picks a specific one.
  for (size_t idx : it->second) {
    if (contexts_[idx].live)
      return materialize(contexts_[idx]);
  }
  return std::nullopt;
}

std::optional<CallSiteContext>
ControlFlowIndex::contextAtSite(const std::string &callSite,
                                const std::string &callerUsrOrName) const {
  auto id = interner_.find(callSite);
  if (!id)
    return std::nullopt;
  auto callerId = interner_.find(callerUsrOrName);
  if (!callerId)
    return std::nullopt;
  auto it = bySite_.find(*id);
  if (it == bySite_.end())
    return std::nullopt;
  for (size_t idx : it->second) {
    const StoredContext &se = contexts_[idx];
    if (!se.live)
      continue;
    if (se.caller == *callerId || se.callerDisplay == *callerId)
      return materialize(se);
  }
  return std::nullopt;
}

std::vector<CallSiteContext>
ControlFlowIndex::contextsForCallee(const std::string &calleeName) const {
  std::vector<CallSiteContext> result;
  const auto *indices = indicesFor(byCallee_, byCalleeDisplay_, calleeName);
  if (!indices)
    return result;
  for (size_t idx : *indices) {
    if (contexts_[idx].live)
      result.push_back(materialize(contexts_[idx]));
  }
  return result;
}

std::vector<CallSiteContext>
ControlFlowIndex::contextsForCaller(const std::string &callerName) const {
  std::vector<CallSiteContext> result;
  const auto *indices = indicesFor(byCaller_, byCallerDisplay_, callerName);
  if (!indices)
    return result;
  for (size_t idx : *indices) {
    if (contexts_[idx].live)
      result.push_back(materialize(contexts_[idx]));
  }
  return result;
}

std::vector<CallSiteContext>
ControlFlowIndex::protectedCallsTo(const std::string &calleeName) const {
  std::vector<CallSiteContext> result;
  const auto *indices = indicesFor(byCallee_, byCalleeDisplay_, calleeName);
  if (!indices)
    return result;
  for (size_t idx : *indices) {
    const StoredContext &se = contexts_[idx];
    if (!se.live)
      continue;
    // scopeSet 0 is the empty set: != 0 <=> enclosingTryCatches non-empty.
    if (se.scopeSet != 0)
      result.push_back(materialize(se));
  }
  return result;
}

std::vector<CallSiteContext>
ControlFlowIndex::unprotectedCallsTo(const std::string &calleeName) const {
  std::vector<CallSiteContext> result;
  const auto *indices = indicesFor(byCallee_, byCalleeDisplay_, calleeName);
  if (!indices)
    return result;
  for (size_t idx : *indices) {
    const StoredContext &se = contexts_[idx];
    if (!se.live)
      continue;
    if (se.scopeSet == 0)
      result.push_back(materialize(se));
  }
  return result;
}

std::vector<CallSiteContext> ControlFlowIndex::allContexts() const {
  std::vector<CallSiteContext> result;
  result.reserve(liveCount_);
  for (const auto &se : contexts_) {
    if (se.live)
      result.push_back(materialize(se));
  }
  return result;
}

void ControlFlowIndex::absorb(const ControlFlowIndex &shard) {
  if (&shard == this)
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
  std::lock_guard<std::mutex> lock(mutex_);
  std::string prefix = tuPath + ":";
  size_t removed = 0;

  // Candidates: exactly this TU's contexts via the reverse index, plus the
  // no-provenance list (legacy fallback matches on a callSite prefix).
  // The old implementation scanned every stored context per removeTU and
  // scrubbed byCallee_/byCaller_ once per removed context (O(degree)
  // each); candidates are now O(TU size) and each affected adjacency
  // vector is scrubbed once.
  std::vector<size_t> candidates;
  if (auto tuId = interner_.find(tuPath)) {
    auto it = byTu_.find(*tuId);
    if (it != byTu_.end())
      candidates = it->second;
  }
  size_t provenanced = candidates.size();
  candidates.insert(candidates.end(), noProvenance_.begin(),
                    noProvenance_.end());

  std::unordered_set<size_t> dead;
  std::unordered_set<SId> affectedCallees, affectedCallers;
  std::unordered_set<SId> affectedCalleeDisplays, affectedCallerDisplays;
  std::unordered_set<SId> affectedSites;
  for (size_t n = 0; n < candidates.size(); ++n) {
    size_t i = candidates[n];
    StoredContext &se = contexts_[i];
    if (!se.live)
      continue; // tombstoned earlier
    if (n >= provenanced &&
        interner_.resolve(se.site).compare(0, prefix.size(), prefix) != 0)
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
    if (auto tuId = interner_.find(tuPath)) {
      auto it = byTu_.find(*tuId);
      if (it != byTu_.end())
        byTu_.erase(it);
    }
    scrub(noProvenance_);
  }
  return removed;
}

void ControlFlowIndex::compact() {
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
