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

#include "vycor/callgraph/StringInterner.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace vycor {

enum class EdgeKind {
  DirectCall,
  VirtualDispatch,
  FunctionPointer,
  ConstructorCall,
  DestructorCall,
  OperatorCall,
  TemplateInstantiation,
  // A lambda expression or closure object invoked directly or through
  // std::function. Distinct from FunctionPointer because closures carry
  // captured state.
  LambdaCall,
  // A callable handed to a concurrency primitive (std::thread, std::jthread,
  // std::async, std::packaged_task, std::invoke, std::bind). The edge points
  // from the spawner to the target callable; execContext records which
  // primitive triggered the edge.
  ThreadEntry,
  // Deferred function-pointer-through-return edge: `auto v = pick(); v();`
  // or `spawn(v)`. The stored edge's callee is the *returning* function
  // (pick), not the eventual target. At query time calleesOf/callersOf join
  // the edge through the functionReturns_ relation and synthesize
  // FunctionPointer (or ThreadEntry, per execContext) edges to each function
  // `pick` is known to return; the raw edge itself is never materialized.
  // Deferring the join keeps edge building free of cross-TU reads (the last
  // blocker to the single-parse bake pipeline) and keeps incremental
  // reindexing correct: returns recorded later become visible to existing
  // call sites without re-baking them.
  FunctionPointerReturn
};

enum class Confidence {
  Proven,
  Plausible,
  Unknown
};

// Where the callee runs relative to the caller. Recorded on CallGraphEdge so
// MCP clients can answer "does this callback run synchronously or on another
// thread/task?" without a second index join.
enum class ExecutionContext {
  Synchronous,
  ThreadSpawn,
  AsyncTask,
  PackagedTask,
  Invoke
};

struct CallGraphNode {
  std::string qualifiedName;
  std::string file;
  unsigned line = 0;
  bool isEntryPoint = false;
  bool isVirtual = false;
  std::string enclosingClass;
};

struct CallGraphEdge {
  std::string callerName;
  std::string calleeName;
  EdgeKind kind;
  Confidence confidence;
  std::string callSite;
  unsigned indirectionDepth = 0;
  ExecutionContext execContext = ExecutionContext::Synchronous;
};

// Thread-safety contract: mutating methods (addNode, addEdge, add*, removeTU,
// compact) lock the internal mutex and may be called concurrently. Read
// methods (calleesOf, callersOf, findNode, getOverrides, ...) do NOT lock;
// they are safe only when no mutation can run concurrently. The multi-TU
// builder relies on phase barriers for this: Phase 2 workers read only maps
// that Phase 1 finished writing (hierarchy, overrides, function returns)
// before the pool barrier, while Phase 2's own writes touch disjoint state
// (nodes_/edges_/outEdges_/inEdges_). The MCP serve loop is single-threaded,
// so queries never overlap reindexTU. Revisit before adding concurrent reads.
//
// calleesOf/callersOf return edges by value (the result can contain
// synthesized virtual-dispatch expansions that have no stored counterpart),
// so query results never dangle. edges_ remains a deque so that internal
// indices stay stable across growth; compact() rewrites them.
class CallGraph {
public:
  CallGraph() = default;
  CallGraph(CallGraph &&other) noexcept;
  CallGraph &operator=(CallGraph &&other) noexcept;
  CallGraph(const CallGraph &) = delete;
  CallGraph &operator=(const CallGraph &) = delete;

  void addNode(CallGraphNode node, const std::string &tuPath = "");
  void addEdge(CallGraphEdge edge, const std::string &tuPath = "");

  // Query the graph. Virtual dispatch is expanded at query time: for every
  // stored Plausible VirtualDispatch edge (caller -> static target), edges
  // to all transitive overrides of the static target are synthesized into
  // the result. The builder records only the static-target edge, so
  // overrides indexed after the call site (incremental reindex) are visible
  // to existing call sites without re-baking. Proven VirtualDispatch edges
  // (exact concrete type known at the call site) are never expanded.
  std::vector<CallGraphEdge> calleesOf(const std::string &name) const;
  std::vector<CallGraphEdge> callersOf(const std::string &name) const;

  // ------------------------------------------------------------------
  // Id-level query API for traversal-heavy tools (path search, lock DFS).
  // Works entirely in interned ids: no string materialization per hop,
  // no string-keyed visited sets. Same expansion semantics as
  // callersOf/calleesOf (virtual dispatch + deferred function-return
  // joins). Resolve ids through interner() only for the survivors.
  // ------------------------------------------------------------------

  // Lightweight edge view in interned-id space.
  struct EdgeRef {
    StringInterner::Id caller;
    StringInterner::Id callee;
    StringInterner::Id callSite;
    EdgeKind kind;
    Confidence confidence;
    ExecutionContext execContext;
    uint32_t indirectionDepth;
  };

  // All caller-side edges reaching `callee` (stored + synthesized), in id
  // space. Exactly the edge set callersOf materializes.
  std::vector<EdgeRef> callerRefsOf(StringInterner::Id callee) const;

  // All callee-side edges leaving `caller` (stored + synthesized), in id
  // space. Exactly the edge set calleesOf materializes.
  std::vector<EdgeRef> calleeRefsOf(StringInterner::Id caller) const;

  // Count of live stored in-edges (cheap: no materialization or virtual
  // expansion). Used as a hub heuristic by path-walking tools. The id
  // overload avoids the name lookup in inner loops.
  size_t storedInDegree(const std::string &name) const;
  size_t storedInDegree(StringInterner::Id id) const;

  size_t nodeCount() const;
  size_t edgeCount() const;

  std::vector<const CallGraphNode *> allNodes() const;
  const CallGraphNode *findNode(const std::string &qualifiedName) const;

  // Class hierarchy tracking.
  void addDerivedClass(const std::string &baseClass,
                       const std::string &derivedClass,
                       const std::string &tuPath = "");
  std::vector<std::string>
  getDerivedClasses(const std::string &baseClass) const;
  std::vector<std::string>
  getAllDerivedClasses(const std::string &baseClass) const;

  // Virtual method override tracking.
  void addMethodOverride(const std::string &baseMethod,
                         const std::string &overrideMethod,
                         const std::string &tuPath = "");
  std::vector<std::string>
  getOverrides(const std::string &baseMethod) const;

  // Transitive closure over the override relation: all methods that
  // (directly or through intermediate classes) override baseMethod.
  std::vector<std::string>
  getTransitiveOverrides(const std::string &baseMethod) const;

  // Inverse closure: all methods that the given method transitively
  // overrides (its base-class declarations, walking up the hierarchy).
  std::vector<std::string>
  getOverriddenBases(const std::string &method) const;

  // Effective implementation mapping: which concrete classes use a given
  // method implementation for virtual dispatch.
  void addEffectiveImpl(const std::string &concreteClass,
                        const std::string &implMethod,
                        const std::string &tuPath = "");
  std::vector<std::string>
  getClassesForImpl(const std::string &implMethod) const;

  // Function pointer return value tracking.
  void addFunctionReturn(const std::string &funcName,
                         const std::string &returnedFunc,
                         const std::string &tuPath = "");
  std::set<std::string>
  getFunctionReturns(const std::string &funcName) const;

  // Per-TU incremental re-indexing. removeTU drops the given TU's
  // contribution: each of its edge registrations is released, and an edge
  // is tombstoned only when its last contributor is removed (header-inlined
  // code registers the same edge from many TUs). Nodes contributed only by
  // this TU are removed. Returns the number of edges fully removed.
  size_t removeTU(const std::string &tuPath);

  // Compact the edge storage, eliminating tombstones.
  void compact();

  const StringInterner &interner() const { return interner_; }

  // Pre-size node/edge tables for a known bulk insert (snapshot load).
  void reserveNodes(size_t n);
  void reserveEdges(size_t n);

private:
  using SId = StringInterner::Id;

  // Compact interned edge record. Public queries materialize CallGraphEdge
  // (with resolved strings) on demand; nothing outside CallGraph sees this.
  struct StoredEdge {
    SId caller;
    SId callee;
    SId callSite;
    EdgeKind kind;
    Confidence confidence;
    ExecutionContext execContext;
    uint32_t indirectionDepth;
    // Number of registrations (one per addEdge call, TU-tagged or not).
    // 0 = tombstone.
    uint32_t refs;
  };

  // Content identity for dedup: every semantic field participates, so
  // edges that differ only in confidence or execution context stay
  // distinct.
  struct EdgeKey {
    SId caller, callee, callSite;
    uint8_t kind, confidence, execContext;
    uint32_t indirectionDepth;
    bool operator==(const EdgeKey &o) const {
      return caller == o.caller && callee == o.callee &&
             callSite == o.callSite && kind == o.kind &&
             confidence == o.confidence && execContext == o.execContext &&
             indirectionDepth == o.indirectionDepth;
    }
  };
  struct EdgeKeyHash {
    size_t operator()(const EdgeKey &k) const {
      size_t h = std::hash<uint64_t>{}(
          (static_cast<uint64_t>(k.caller) << 32) | k.callee);
      h ^= std::hash<uint64_t>{}(
               (static_cast<uint64_t>(k.callSite) << 32) |
               (static_cast<uint64_t>(k.kind) << 16) |
               (static_cast<uint64_t>(k.confidence) << 8) | k.execContext) +
           0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
      return h ^ k.indirectionDepth;
    }
  };

  static EdgeKey keyOf(const StoredEdge &se) {
    return {se.caller,
            se.callee,
            se.callSite,
            static_cast<uint8_t>(se.kind),
            static_cast<uint8_t>(se.confidence),
            static_cast<uint8_t>(se.execContext),
            se.indirectionDepth};
  }

  CallGraphEdge materialize(const StoredEdge &se) const;

  // Snapshot serialization reads provenance maps (tuEdges_,
  // nodeContributors_) and edge storage that have no public accessors;
  // deserialization goes through the public API.
  friend class SnapshotIO;

  mutable std::mutex mutex_;
  StringInterner interner_;
  std::unordered_map<SId, CallGraphNode> nodes_;
  // deque, not vector: indices into this container are held by the maps
  // below and must stay stable across growth (see class comment).
  std::deque<StoredEdge> edges_;
  std::unordered_map<EdgeKey, size_t, EdgeKeyHash> edgeIndex_;
  std::unordered_map<SId, std::vector<size_t>> outEdges_;
  std::unordered_map<SId, std::vector<size_t>> inEdges_;

  std::unordered_map<SId, std::vector<SId>> derivedClasses_;
  std::unordered_map<SId, std::vector<SId>> methodOverrides_;
  // Reverse of methodOverrides_: override -> base methods it overrides.
  // Maintained by addMethodOverride; used by callersOf expansion.
  std::unordered_map<SId, std::vector<SId>> overrideBases_;
  std::unordered_map<SId, std::set<SId>> effectiveImplClasses_;
  std::unordered_map<SId, std::set<SId>> functionReturns_;
  // Reverse of functionReturns_: returned function -> functions returning it.
  // Drives callersOf expansion of FunctionPointerReturn edges.
  std::unordered_map<SId, std::vector<SId>> returnedBy_;

  // Per-TU provenance tracking for incremental re-indexing.
  std::unordered_map<SId, std::vector<size_t>> tuEdges_;
  std::unordered_map<SId, std::set<SId>> nodeContributors_;
  size_t liveEdgeCount_ = 0;

  // Transitive closure helpers over the override relation (callers must
  // not hold mutex_; these only read maps written during Phase 1).
  std::vector<SId> transitiveClosure(
      SId start,
      const std::unordered_map<SId, std::vector<SId>> &relation) const;
};

} // namespace vycor
