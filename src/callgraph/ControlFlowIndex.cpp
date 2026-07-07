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

ControlFlowIndex::ControlFlowIndex(ControlFlowIndex &&other) noexcept
    : interner_(std::move(other.interner_)),
      contexts_(std::move(other.contexts_)),
      byCallee_(std::move(other.byCallee_)),
      byCaller_(std::move(other.byCaller_)),
      bySite_(std::move(other.bySite_)),
      byTu_(std::move(other.byTu_)),
      noProvenance_(std::move(other.noProvenance_)),
      liveCount_(other.liveCount_) {}

ControlFlowIndex &ControlFlowIndex::operator=(ControlFlowIndex &&other) noexcept {
  interner_ = std::move(other.interner_);
  contexts_ = std::move(other.contexts_);
  byCallee_ = std::move(other.byCallee_);
  byCaller_ = std::move(other.byCaller_);
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

void ControlFlowIndex::addCallSiteContext(CallSiteContext ctx) {
  std::lock_guard<std::mutex> lock(mutex_);
  SId calleeId = interner_.intern(ctx.calleeName);
  SId callerId = interner_.intern(ctx.callerName);
  SId siteId = interner_.intern(ctx.callSite);
  size_t idx = contexts_.size();
  byCallee_[calleeId].push_back(idx);
  byCaller_[callerId].push_back(idx);
  bySite_[siteId] = idx;
  if (!ctx.tuPath.empty())
    byTu_[interner_.intern(ctx.tuPath)].push_back(idx);
  else
    noProvenance_.push_back(idx);
  contexts_.push_back(std::move(ctx));
  ++liveCount_;
}

const CallSiteContext *
ControlFlowIndex::contextAtSite(const std::string &callSite) const {
  auto id = interner_.find(callSite);
  if (!id)
    return nullptr;
  auto it = bySite_.find(*id);
  if (it == bySite_.end())
    return nullptr;
  return &contexts_[it->second];
}

std::vector<const CallSiteContext *>
ControlFlowIndex::contextsForCallee(const std::string &calleeName) const {
  std::vector<const CallSiteContext *> result;
  auto id = interner_.find(calleeName);
  if (!id)
    return result;
  auto it = byCallee_.find(*id);
  if (it == byCallee_.end())
    return result;
  for (size_t idx : it->second) {
    if (!contexts_[idx].callerName.empty())
      result.push_back(&contexts_[idx]);
  }
  return result;
}

std::vector<const CallSiteContext *>
ControlFlowIndex::contextsForCaller(const std::string &callerName) const {
  std::vector<const CallSiteContext *> result;
  auto id = interner_.find(callerName);
  if (!id)
    return result;
  auto it = byCaller_.find(*id);
  if (it == byCaller_.end())
    return result;
  for (size_t idx : it->second) {
    if (!contexts_[idx].callerName.empty())
      result.push_back(&contexts_[idx]);
  }
  return result;
}

std::vector<const CallSiteContext *>
ControlFlowIndex::protectedCallsTo(const std::string &calleeName) const {
  std::vector<const CallSiteContext *> result;
  auto id = interner_.find(calleeName);
  if (!id)
    return result;
  auto it = byCallee_.find(*id);
  if (it == byCallee_.end())
    return result;
  for (size_t idx : it->second) {
    if (contexts_[idx].callerName.empty())
      continue;
    if (!contexts_[idx].enclosingTryCatches.empty())
      result.push_back(&contexts_[idx]);
  }
  return result;
}

std::vector<const CallSiteContext *>
ControlFlowIndex::unprotectedCallsTo(const std::string &calleeName) const {
  std::vector<const CallSiteContext *> result;
  auto id = interner_.find(calleeName);
  if (!id)
    return result;
  auto it = byCallee_.find(*id);
  if (it == byCallee_.end())
    return result;
  for (size_t idx : it->second) {
    if (contexts_[idx].callerName.empty())
      continue;
    if (contexts_[idx].enclosingTryCatches.empty())
      result.push_back(&contexts_[idx]);
  }
  return result;
}

std::vector<const CallSiteContext *> ControlFlowIndex::allContexts() const {
  std::vector<const CallSiteContext *> result;
  result.reserve(liveCount_);
  for (const auto &ctx : contexts_) {
    if (!ctx.callerName.empty())
      result.push_back(&ctx);
  }
  return result;
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
  for (size_t n = 0; n < candidates.size(); ++n) {
    size_t i = candidates[n];
    auto &ctx = contexts_[i];
    if (ctx.callerName.empty())
      continue; // tombstoned earlier
    if (n >= provenanced &&
        ctx.callSite.compare(0, prefix.size(), prefix) != 0)
      continue; // no-provenance context from a different TU

    if (auto calleeId = interner_.find(ctx.calleeName))
      affectedCallees.insert(*calleeId);
    if (auto callerId = interner_.find(ctx.callerName))
      affectedCallers.insert(*callerId);
    if (auto siteId = interner_.find(ctx.callSite))
      bySite_.erase(*siteId);

    dead.insert(i);
    ctx.callerName.clear();
    ctx.calleeName.clear();
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
  std::deque<CallSiteContext> newCtx;

  std::unordered_map<SId, std::vector<size_t>> newByCallee;
  std::unordered_map<SId, std::vector<size_t>> newByCaller;
  std::unordered_map<SId, size_t> newBySite;
  std::unordered_map<SId, std::vector<size_t>> newByTu;
  std::vector<size_t> newNoProvenance;

  for (auto &ctx : contexts_) {
    if (ctx.callerName.empty())
      continue;
    size_t idx = newCtx.size();
    SId calleeId = interner_.intern(ctx.calleeName);
    SId callerId = interner_.intern(ctx.callerName);
    SId siteId = interner_.intern(ctx.callSite);
    newByCallee[calleeId].push_back(idx);
    newByCaller[callerId].push_back(idx);
    newBySite[siteId] = idx;
    if (!ctx.tuPath.empty())
      newByTu[interner_.intern(ctx.tuPath)].push_back(idx);
    else
      newNoProvenance.push_back(idx);
    newCtx.push_back(std::move(ctx));
  }

  contexts_ = std::move(newCtx);
  byCallee_ = std::move(newByCallee);
  byCaller_ = std::move(newByCaller);
  bySite_ = std::move(newBySite);
  byTu_ = std::move(newByTu);
  noProvenance_ = std::move(newNoProvenance);
}

} // namespace vycor
