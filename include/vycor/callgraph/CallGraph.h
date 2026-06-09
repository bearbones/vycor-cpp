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

#include <deque>
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
  ThreadEntry
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
// Pointer stability: const CallGraphEdge* returned by calleesOf/callersOf
// remains valid across addEdge/removeTU (edges_ is a deque and removal only
// tombstones). compact() is the ONLY operation that invalidates them.
class CallGraph {
public:
  CallGraph() = default;
  CallGraph(CallGraph &&other) noexcept;
  CallGraph &operator=(CallGraph &&other) noexcept;
  CallGraph(const CallGraph &) = delete;
  CallGraph &operator=(const CallGraph &) = delete;

  void addNode(CallGraphNode node, const std::string &tuPath = "");
  void addEdge(CallGraphEdge edge, const std::string &tuPath = "");

  std::vector<const CallGraphEdge *> calleesOf(const std::string &name) const;
  std::vector<const CallGraphEdge *> callersOf(const std::string &name) const;

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

  // Per-TU incremental re-indexing. removeTU tombstones all edges from the
  // given TU and removes nodes that were only contributed by that TU.
  // Returns the number of edges removed.
  size_t removeTU(const std::string &tuPath);

  // Compact the edge vector, eliminating tombstones. Invalidates all
  // previously returned const CallGraphEdge * pointers.
  void compact();

  const StringInterner &interner() const { return interner_; }

private:
  using SId = StringInterner::Id;

  // Snapshot serialization reads provenance maps (tuEdges_,
  // nodeContributors_) that have no public accessors; deserialization goes
  // through the public API.
  friend class SnapshotIO;

  mutable std::mutex mutex_;
  StringInterner interner_;
  std::unordered_map<SId, CallGraphNode> nodes_;
  // deque, not vector: queries hand out pointers into this container, and
  // growth must not invalidate them (see class comment).
  std::deque<CallGraphEdge> edges_;
  std::unordered_map<SId, std::vector<size_t>> outEdges_;
  std::unordered_map<SId, std::vector<size_t>> inEdges_;

  std::unordered_map<SId, std::vector<SId>> derivedClasses_;
  std::unordered_map<SId, std::vector<SId>> methodOverrides_;
  std::unordered_map<SId, std::set<SId>> effectiveImplClasses_;
  std::unordered_map<SId, std::set<SId>> functionReturns_;

  // Per-TU provenance tracking for incremental re-indexing.
  std::unordered_map<SId, std::vector<size_t>> tuEdges_;
  std::unordered_map<SId, std::set<SId>> nodeContributors_;
  size_t liveEdgeCount_ = 0;
};

} // namespace vycor
