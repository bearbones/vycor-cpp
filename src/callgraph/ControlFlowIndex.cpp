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

namespace vycor {

ControlFlowIndex::ControlFlowIndex(ControlFlowIndex &&other) noexcept
    : interner_(std::move(other.interner_)),
      contexts_(std::move(other.contexts_)),
      byCallee_(std::move(other.byCallee_)),
      byCaller_(std::move(other.byCaller_)),
      bySite_(std::move(other.bySite_)),
      liveCount_(other.liveCount_) {}

ControlFlowIndex &ControlFlowIndex::operator=(ControlFlowIndex &&other) noexcept {
  interner_ = std::move(other.interner_);
  contexts_ = std::move(other.contexts_);
  byCallee_ = std::move(other.byCallee_);
  byCaller_ = std::move(other.byCaller_);
  bySite_ = std::move(other.bySite_);
  liveCount_ = other.liveCount_;
  return *this;
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

  for (size_t i = 0; i < contexts_.size(); ++i) {
    auto &ctx = contexts_[i];
    if (ctx.callerName.empty())
      continue;
    if (ctx.callSite.compare(0, prefix.size(), prefix) != 0)
      continue;

    auto calleeId = interner_.find(ctx.calleeName);
    if (calleeId) {
      auto &cv = byCallee_[*calleeId];
      cv.erase(std::remove(cv.begin(), cv.end(), i), cv.end());
    }
    auto callerId = interner_.find(ctx.callerName);
    if (callerId) {
      auto &cv = byCaller_[*callerId];
      cv.erase(std::remove(cv.begin(), cv.end(), i), cv.end());
    }
    auto siteId = interner_.find(ctx.callSite);
    if (siteId)
      bySite_.erase(*siteId);

    ctx.callerName.clear();
    ctx.calleeName.clear();
    --liveCount_;
    ++removed;
  }
  return removed;
}

void ControlFlowIndex::compact() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::deque<CallSiteContext> newCtx;

  std::unordered_map<SId, std::vector<size_t>> newByCallee;
  std::unordered_map<SId, std::vector<size_t>> newByCaller;
  std::unordered_map<SId, size_t> newBySite;

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
    newCtx.push_back(std::move(ctx));
  }

  contexts_ = std::move(newCtx);
  byCallee_ = std::move(newByCallee);
  byCaller_ = std::move(newByCaller);
  bySite_ = std::move(newBySite);
}

} // namespace vycor
