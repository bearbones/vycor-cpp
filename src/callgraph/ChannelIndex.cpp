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

#include "vycor/callgraph/ChannelIndex.h"
#include "vycor/callgraph/Utf8.h"

#include <algorithm>
#include <unordered_set>

namespace vycor {

ChannelIndex::ChannelIndex(ChannelIndex &&other) noexcept
    : sites_(std::move(other.sites_)), index_(std::move(other.index_)),
      byChannel_(std::move(other.byChannel_)),
      byFunctionUsr_(std::move(other.byFunctionUsr_)),
      byFunctionDisplay_(std::move(other.byFunctionDisplay_)),
      byTu_(std::move(other.byTu_)), liveCount_(other.liveCount_) {}

ChannelIndex &ChannelIndex::operator=(ChannelIndex &&other) noexcept {
  sites_ = std::move(other.sites_);
  index_ = std::move(other.index_);
  byChannel_ = std::move(other.byChannel_);
  byFunctionUsr_ = std::move(other.byFunctionUsr_);
  byFunctionDisplay_ = std::move(other.byFunctionDisplay_);
  byTu_ = std::move(other.byTu_);
  liveCount_ = other.liveCount_;
  return *this;
}

void ChannelIndex::addSite(ChannelSite site) { addSiteRefs(std::move(site), 1); }

void ChannelIndex::addSiteRefs(ChannelSite site, uint32_t count) {
  if (count == 0)
    return;
  // Valid UTF-8 like every index string (Utf8.h); removeTUs converts its
  // TU paths the same way.
  for (std::string *s : {&site.channelId, &site.channelTypeName,
                         &site.category, &site.siteFunctionUsr,
                         &site.siteFunctionDisplay, &site.callSite,
                         &site.tuPath})
    makeValidUtf8(*s);
  for (auto &g : site.enclosingGuards) {
    makeValidUtf8(g.conditionText);
    makeValidUtf8(g.location);
  }
  std::lock_guard<std::mutex> lock(mutex_);
  SiteKey key{site.channelId, site.callSite, site.siteFunctionUsr, site.op};
  std::string tuPath = site.tuPath;

  auto it = index_.find(key);
  size_t idx;
  if (it != index_.end()) {
    // Identical site already stored (e.g. a header-inlined function indexed
    // by several TUs): register another contributor instead of duplicating.
    idx = it->second;
    sites_[idx].refs += count;
  } else {
    idx = sites_.size();
    std::string channelId = site.channelId;
    std::string funcUsr = site.siteFunctionUsr;
    std::string funcDisplay = site.siteFunctionDisplay;
    bool differentDisplay = funcDisplay != funcUsr;
    sites_.push_back(StoredSite{std::move(site), count, true});
    index_.emplace(key, idx);
    byChannel_[channelId].push_back(idx);
    byFunctionUsr_[funcUsr].push_back(idx);
    if (differentDisplay)
      byFunctionDisplay_[funcDisplay].push_back(idx);
    ++liveCount_;
  }

  // Recorded on every call (fresh insert or dedup hit), mirroring
  // CallGraph::addEdge's unconditional tuEdges_ push — the same physical
  // site can be (and, for header-inlined code, will be) contributed by
  // several TUs, and removeTU needs to know about each one.
  if (!tuPath.empty())
    byTu_[tuPath].insert(byTu_[tuPath].end(), count, idx);
}

std::vector<ChannelSite>
ChannelIndex::producersOf(const std::string &channelId) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<ChannelSite> result;
  auto it = byChannel_.find(channelId);
  if (it == byChannel_.end())
    return result;
  for (size_t i : it->second) {
    const auto &s = sites_[i];
    if (s.live && s.site.op == ChannelOperation::Produce)
      result.push_back(s.site);
  }
  return result;
}

std::vector<ChannelSite>
ChannelIndex::consumersOf(const std::string &channelId) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<ChannelSite> result;
  auto it = byChannel_.find(channelId);
  if (it == byChannel_.end())
    return result;
  for (size_t i : it->second) {
    const auto &s = sites_[i];
    if (s.live && s.site.op == ChannelOperation::Consume)
      result.push_back(s.site);
  }
  return result;
}

std::vector<ChannelSite>
ChannelIndex::sitesForFunction(const std::string &functionUsrOrDisplay) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<ChannelSite> result;
  auto collect =
      [&](const std::unordered_map<std::string, std::vector<size_t>> &m) {
        auto it = m.find(functionUsrOrDisplay);
        if (it == m.end())
          return;
        for (size_t i : it->second) {
          const auto &s = sites_[i];
          if (s.live)
            result.push_back(s.site);
        }
      };
  collect(byFunctionUsr_);
  collect(byFunctionDisplay_);
  return result;
}

std::vector<std::string> ChannelIndex::allChannelIds() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> ids;
  ids.reserve(byChannel_.size());
  for (const auto &[id, indices] : byChannel_) {
    for (size_t i : indices) {
      if (sites_[i].live) {
        ids.push_back(id);
        break;
      }
    }
  }
  return ids;
}

std::vector<ChannelSite> ChannelIndex::allSites() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<ChannelSite> result;
  result.reserve(liveCount_);
  for (const auto &s : sites_)
    if (s.live)
      result.push_back(s.site);
  return result;
}

void ChannelIndex::absorb(const ChannelIndex &shard) {
  if (&shard == this)
    return;

  // Snapshot the shard's live sites, and per site the TU of each of its
  // registrations (byTu_), under the shard's own lock first: addSiteRefs
  // below takes this->mutex_ itself, so we must not still be holding
  // shard.mutex_ (or this->mutex_) when we call it.
  std::vector<StoredSite> shardSites;
  std::vector<std::vector<std::string>> siteTus;
  {
    std::lock_guard<std::mutex> lockShard(shard.mutex_);
    shardSites.assign(shard.sites_.begin(), shard.sites_.end());
    siteTus.resize(shardSites.size());
    for (const auto &[tuPath, idxs] : shard.byTu_)
      for (size_t idx : idxs)
        if (idx < siteTus.size())
          siteTus[idx].push_back(tuPath);
  }

  // Replay each registration under the TU that made it, so this index's
  // dedup/refcount/byTu_ bookkeeping ends up identical to what it would be
  // had they happened directly against `this`. (Replaying all `refs` of a
  // site under its first TU, as this once did, lost the other
  // contributors: removeTU of a second TU released nothing.)
  for (size_t i = 0; i < shardSites.size(); ++i) {
    const StoredSite &stored = shardSites[i];
    if (!stored.live || stored.refs == 0)
      continue;
    std::vector<std::string> &tus = siteTus[i];
    std::sort(tus.begin(), tus.end()); // deterministic first contributor
    uint32_t replayed = 0;
    for (size_t t = 0; t < tus.size() && replayed < stored.refs;) {
      // Runs of the same TU (one TU registering a site several times)
      // in one call.
      size_t end = t;
      while (end < tus.size() && tus[end] == tus[t])
        ++end;
      const uint32_t n = static_cast<uint32_t>(
          std::min<size_t>(end - t, stored.refs - replayed));
      ChannelSite site = stored.site;
      site.tuPath = tus[t];
      addSiteRefs(std::move(site), n);
      replayed += n;
      t = end;
    }
    // Registrations no TU recorded (provenance-less producers).
    ChannelSite site = stored.site;
    site.tuPath.clear();
    addSiteRefs(std::move(site), stored.refs - replayed);
  }
}

size_t ChannelIndex::removeTU(const std::string &tuPath) {
  return removeTUs({tuPath});
}

size_t ChannelIndex::removeTUs(const std::vector<std::string> &tuPaths) {
  std::lock_guard<std::mutex> lock(mutex_);
  size_t removed = 0;

  // Release every TU's registrations first, then scrub each affected
  // index vector once for the whole set (mirrors CallGraph::removeTUs).
  std::unordered_set<size_t> dead;
  std::unordered_set<std::string> affectedChannels, affectedFuncUsr,
      affectedFuncDisplay;
  for (const auto &rawPath : tuPaths) {
    auto tit = byTu_.find(validUtf8(rawPath));
    if (tit == byTu_.end())
      continue;
    for (size_t idx : tit->second) {
      StoredSite &se = sites_[idx];
      if (se.refs == 0)
        continue; // already tombstoned
      if (--se.refs > 0)
        continue; // other contributors remain

      dead.insert(idx);
      affectedChannels.insert(se.site.channelId);
      affectedFuncUsr.insert(se.site.siteFunctionUsr);
      if (se.site.siteFunctionDisplay != se.site.siteFunctionUsr)
        affectedFuncDisplay.insert(se.site.siteFunctionDisplay);

      index_.erase(SiteKey{se.site.channelId, se.site.callSite,
                           se.site.siteFunctionUsr, se.site.op});
      se.live = false;
      --liveCount_;
      ++removed;
    }
    byTu_.erase(tit);
  }

  auto scrub = [&dead](std::vector<size_t> &v) {
    v.erase(std::remove_if(v.begin(), v.end(),
                           [&](size_t i) { return dead.count(i) > 0; }),
            v.end());
  };
  for (const auto &c : affectedChannels) {
    auto it = byChannel_.find(c);
    if (it == byChannel_.end())
      continue;
    scrub(it->second);
    if (it->second.empty())
      byChannel_.erase(it);
  }
  for (const auto &f : affectedFuncUsr) {
    auto it = byFunctionUsr_.find(f);
    if (it == byFunctionUsr_.end())
      continue;
    scrub(it->second);
    if (it->second.empty())
      byFunctionUsr_.erase(it);
  }
  for (const auto &f : affectedFuncDisplay) {
    auto it = byFunctionDisplay_.find(f);
    if (it == byFunctionDisplay_.end())
      continue;
    scrub(it->second);
    if (it->second.empty())
      byFunctionDisplay_.erase(it);
  }
  return removed;
}

void ChannelIndex::compact() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::deque<StoredSite> newSites;
  std::unordered_map<SiteKey, size_t, SiteKeyHash> newIndex;
  std::unordered_map<std::string, std::vector<size_t>> newByChannel,
      newByFuncUsr, newByFuncDisplay, newByTu;

  std::vector<size_t> oldToNew(sites_.size(),
                               static_cast<size_t>(-1));
  for (size_t i = 0; i < sites_.size(); ++i) {
    if (!sites_[i].live)
      continue;
    oldToNew[i] = newSites.size();
    newSites.push_back(sites_[i]);
  }
  for (const auto &[key, idx] : index_) {
    if (oldToNew[idx] != static_cast<size_t>(-1))
      newIndex.emplace(key, oldToNew[idx]);
  }
  auto remapInto =
      [&](const std::unordered_map<std::string, std::vector<size_t>> &src,
          std::unordered_map<std::string, std::vector<size_t>> &dst) {
        for (const auto &[k, v] : src) {
          std::vector<size_t> nv;
          nv.reserve(v.size());
          for (size_t i : v)
            if (oldToNew[i] != static_cast<size_t>(-1))
              nv.push_back(oldToNew[i]);
          if (!nv.empty())
            dst.emplace(k, std::move(nv));
        }
      };
  remapInto(byChannel_, newByChannel);
  remapInto(byFunctionUsr_, newByFuncUsr);
  remapInto(byFunctionDisplay_, newByFuncDisplay);
  remapInto(byTu_, newByTu);

  sites_ = std::move(newSites);
  index_ = std::move(newIndex);
  byChannel_ = std::move(newByChannel);
  byFunctionUsr_ = std::move(newByFuncUsr);
  byFunctionDisplay_ = std::move(newByFuncDisplay);
  byTu_ = std::move(newByTu);
}

} // namespace vycor
