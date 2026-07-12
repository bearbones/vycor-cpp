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

#pragma once

#include "vycor/callgraph/ConditionalGuard.h"

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace vycor {

// ============================================================================
// Channel/data-flow tracing — parallel index alongside CallGraph and
// ControlFlowIndex (same "orthogonal per-call-site metadata" shape
// ControlFlowIndex already established). A CallGraphEdge is caller->callee
// between two functions; a channel (a queue, a concurrent map, an event bus)
// has N producer call sites and M consumer call sites with no single
// "the" edge to draw between them, so it gets its own record shape instead
// of a new EdgeKind.
// ============================================================================

enum class ChannelOperation { Produce, Consume };

// One registered container/channel type: which type to recognize, and which
// method names on it count as producing vs. consuming. Registered per
// project/per graph-build (mirrors LockTypeConfig's userAllowlist,
// ControlFlowIndex.h) — this project has no built-in notion of "a queue,"
// callers tell it what to look for.
struct ChannelTypeSpec {
  std::string qualifiedTypeName; // canonical type name to match, WITHOUT
                                 // the elaborated struct/class keyword
                                 // (e.g. "Queue", not "struct Queue"; a
                                 // template instantiation's canonical name
                                 // includes its arguments, e.g.
                                 // "moodycamel::ConcurrentQueue<Event>" —
                                 // registration is per instantiation)
  std::vector<std::string> produceMethods;
  std::vector<std::string> consumeMethods;
  std::string category; // free-form label surfaced in query results,
                        // e.g. "queue", "map", "channel"
};

struct ChannelTypeConfig {
  std::vector<ChannelTypeSpec> registeredTypes;
};

// One producer or consumer call site on a channel.
struct ChannelSite {
  std::string channelId;        // stable identity of the container (the
                                // FieldDecl/VarDecl backing the receiver;
                                // see ChannelVisitor in
                                // ControlFlowContextVisitor.cpp)
  std::string channelTypeName;  // matched ChannelTypeSpec::qualifiedTypeName
  std::string category;         // matched ChannelTypeSpec::category
  ChannelOperation op = ChannelOperation::Produce;
  std::string siteFunctionUsr;    // enclosing function (join key with
                                 // CallGraph)
  std::string siteFunctionDisplay;
  std::string callSite;          // file:line:col
  // Enclosing if/else and assertion guards, innermost first — reuses
  // ControlFlowIndex's ConditionalGuard so a caller can see e.g. "this send
  // is only reachable when streamingEnabled is true".
  std::vector<ConditionalGuard> enclosingGuards;
  std::string tuPath; // TU that produced this site; empty = no provenance
                      // (hand-built in tests)
};

// ChannelIndex: deliberately simpler storage than ControlFlowIndex — plain
// std::string fields and a content-keyed dedup map, no StringInterner or
// set-table interning. That machinery in ControlFlowIndex exists because it
// hit real memory costs at llvm-project scale; this is a new feature that
// hasn't earned that cost yet. Correctness under multi-TU registration and
// incremental reindex (dedup + removeTU + absorb) is still fully supported;
// only the memory-footprint optimization is deferred.
// Thread-safety: every public method locks mutex_ (unlike CallGraph's
// finer-grained "reads don't lock" contract — ChannelIndex has no phase
// barrier equivalent, so writer threads in the parallel bake and reader
// calls can genuinely overlap; simplicity wins over that optimization until
// profiling says otherwise).
class ChannelIndex {
public:
  ChannelIndex() = default;
  // Not = default: std::mutex is neither copyable nor movable, so the move
  // ctor/assignment move every field except mutex_ (mirrors
  // CallGraph::CallGraph(CallGraph&&)).
  ChannelIndex(ChannelIndex &&other) noexcept;
  ChannelIndex &operator=(ChannelIndex &&other) noexcept;
  ChannelIndex(const ChannelIndex &) = delete;
  ChannelIndex &operator=(const ChannelIndex &) = delete;

  // Register a site. Identical (channelId, op, callSite, siteFunctionUsr)
  // registrations (header-inlined code indexed by several TUs) accumulate a
  // refcount instead of duplicating, exactly like CallGraph::addEdge.
  void addSite(ChannelSite site);

  std::vector<ChannelSite> producersOf(const std::string &channelId) const;
  std::vector<ChannelSite> consumersOf(const std::string &channelId) const;

  // All sites (either role) whose enclosing function matches usr or display
  // name.
  std::vector<ChannelSite>
  sitesForFunction(const std::string &functionUsrOrDisplay) const;

  // All distinct channel ids with at least one live site.
  std::vector<std::string> allChannelIds() const;

  // All live sites (dump mode).
  std::vector<ChannelSite> allSites() const;

  size_t size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return liveCount_;
  }

  // Merge another index into this one (worker-shard merge, mirrors
  // CallGraph::absorb / ControlFlowIndex::absorb).
  void absorb(const ChannelIndex &shard);

  // Remove all sites contributed by the given TU. Matches on the recorded
  // tuPath; sites without one (hand-built tests) are never removed by this
  // (mirrors ControlFlowIndex's noProvenance_ fallback list, but since
  // there's no callSite-prefix reconstruction needed here they simply never
  // match a real tuPath).
  size_t removeTU(const std::string &tuPath);

  // Compact storage, eliminating tombstones.
  void compact();

private:
  struct StoredSite {
    ChannelSite site;
    uint32_t refs = 0;
    bool live = true;
  };

  struct SiteKey {
    std::string channelId;
    std::string callSite;
    std::string siteFunctionUsr;
    ChannelOperation op;
    bool operator==(const SiteKey &o) const {
      return op == o.op && channelId == o.channelId &&
             callSite == o.callSite && siteFunctionUsr == o.siteFunctionUsr;
    }
  };
  struct SiteKeyHash {
    size_t operator()(const SiteKey &k) const {
      size_t h = std::hash<std::string>{}(k.channelId);
      h ^= std::hash<std::string>{}(k.callSite) + 0x9e3779b97f4a7c15ull +
           (h << 6) + (h >> 2);
      h ^= std::hash<std::string>{}(k.siteFunctionUsr) +
           0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
      h ^= std::hash<uint8_t>{}(static_cast<uint8_t>(k.op)) +
           0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
      return h;
    }
  };

  mutable std::mutex mutex_;

  // deque: indices held by the maps below must stay stable across growth
  // (same reason CallGraph/ControlFlowIndex use deque, not vector).
  std::deque<StoredSite> sites_;
  std::unordered_map<SiteKey, size_t, SiteKeyHash> index_;

  std::unordered_map<std::string, std::vector<size_t>> byChannel_;
  std::unordered_map<std::string, std::vector<size_t>> byFunctionUsr_;
  // Sites whose siteFunctionDisplay differs from siteFunctionUsr (hand-built
  // name-only sites coincide and are skipped here, mirroring
  // ControlFlowIndex's byCallerDisplay_ split).
  std::unordered_map<std::string, std::vector<size_t>> byFunctionDisplay_;
  std::unordered_map<std::string, std::vector<size_t>> byTu_;

  size_t liveCount_ = 0;
};

} // namespace vycor
