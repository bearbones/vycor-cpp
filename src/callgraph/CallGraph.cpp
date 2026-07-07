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

#include "vycor/callgraph/CallGraph.h"

#include <algorithm>

namespace vycor {

CallGraph::CallGraph(CallGraph &&other) noexcept
    : interner_(std::move(other.interner_)),
      nodes_(std::move(other.nodes_)),
      edges_(std::move(other.edges_)),
      edgeIndex_(std::move(other.edgeIndex_)),
      outEdges_(std::move(other.outEdges_)),
      inEdges_(std::move(other.inEdges_)),
      derivedClasses_(std::move(other.derivedClasses_)),
      methodOverrides_(std::move(other.methodOverrides_)),
      overrideBases_(std::move(other.overrideBases_)),
      effectiveImplClasses_(std::move(other.effectiveImplClasses_)),
      functionReturns_(std::move(other.functionReturns_)),
      returnedBy_(std::move(other.returnedBy_)),
      tuEdges_(std::move(other.tuEdges_)),
      nodeContributors_(std::move(other.nodeContributors_)),
      liveEdgeCount_(other.liveEdgeCount_) {}

CallGraph &CallGraph::operator=(CallGraph &&other) noexcept {
  interner_ = std::move(other.interner_);
  nodes_ = std::move(other.nodes_);
  edges_ = std::move(other.edges_);
  edgeIndex_ = std::move(other.edgeIndex_);
  outEdges_ = std::move(other.outEdges_);
  inEdges_ = std::move(other.inEdges_);
  derivedClasses_ = std::move(other.derivedClasses_);
  methodOverrides_ = std::move(other.methodOverrides_);
  overrideBases_ = std::move(other.overrideBases_);
  effectiveImplClasses_ = std::move(other.effectiveImplClasses_);
  functionReturns_ = std::move(other.functionReturns_);
  returnedBy_ = std::move(other.returnedBy_);
  tuEdges_ = std::move(other.tuEdges_);
  nodeContributors_ = std::move(other.nodeContributors_);
  liveEdgeCount_ = other.liveEdgeCount_;
  return *this;
}

void CallGraph::addNode(CallGraphNode node, const std::string &tuPath) {
  std::lock_guard<std::mutex> lock(mutex_);
  SId nameId = interner_.intern(node.qualifiedName);
  auto it = nodes_.find(nameId);
  if (it == nodes_.end()) {
    nodes_.emplace(nameId, std::move(node));
  } else {
    if (!node.file.empty())
      it->second.file = std::move(node.file);
    if (node.line != 0)
      it->second.line = node.line;
    if (node.isEntryPoint)
      it->second.isEntryPoint = true;
    if (node.isVirtual)
      it->second.isVirtual = true;
    if (!node.enclosingClass.empty())
      it->second.enclosingClass = std::move(node.enclosingClass);
  }
  if (!tuPath.empty()) {
    SId tuId = interner_.intern(tuPath);
    nodeContributors_[nameId].insert(tuId);
  }
}

CallGraphEdge CallGraph::materialize(const StoredEdge &se) const {
  CallGraphEdge e;
  e.callerName = interner_.resolve(se.caller);
  e.calleeName = interner_.resolve(se.callee);
  e.kind = se.kind;
  e.confidence = se.confidence;
  e.callSite = interner_.resolve(se.callSite);
  e.indirectionDepth = se.indirectionDepth;
  e.execContext = se.execContext;
  return e;
}

void CallGraph::addEdge(CallGraphEdge edge, const std::string &tuPath) {
  std::lock_guard<std::mutex> lock(mutex_);
  StoredEdge se;
  se.caller = interner_.intern(edge.callerName);
  se.callee = interner_.intern(edge.calleeName);
  se.callSite = interner_.intern(edge.callSite);
  se.kind = edge.kind;
  se.confidence = edge.confidence;
  se.execContext = edge.execContext;
  se.indirectionDepth = edge.indirectionDepth;
  se.refs = 1;

  EdgeKey key = keyOf(se);
  size_t idx;
  auto it = edgeIndex_.find(key);
  if (it != edgeIndex_.end()) {
    // Identical edge already stored (e.g. header-inlined function indexed
    // by several TUs): register another contributor instead of duplicating.
    idx = it->second;
    ++edges_[idx].refs;
  } else {
    idx = edges_.size();
    outEdges_[se.caller].push_back(idx);
    inEdges_[se.callee].push_back(idx);
    edges_.push_back(se);
    edgeIndex_.emplace(key, idx);
    ++liveEdgeCount_;
  }
  if (!tuPath.empty()) {
    SId tuId = interner_.intern(tuPath);
    tuEdges_[tuId].push_back(idx);
  }
}

std::vector<CallGraph::SId> CallGraph::transitiveClosure(
    SId start,
    const std::unordered_map<SId, std::vector<SId>> &relation) const {
  std::vector<SId> result;
  std::vector<SId> stack{start};
  std::set<SId> visited{start};
  while (!stack.empty()) {
    SId cur = stack.back();
    stack.pop_back();
    auto it = relation.find(cur);
    if (it == relation.end())
      continue;
    for (SId next : it->second) {
      if (visited.insert(next).second) {
        result.push_back(next);
        stack.push_back(next);
      }
    }
  }
  return result;
}

std::vector<CallGraphEdge>
CallGraph::calleesOf(const std::string &name) const {
  std::vector<CallGraphEdge> result;
  auto id = interner_.find(name);
  if (!id)
    return result;
  auto it = outEdges_.find(*id);
  if (it == outEdges_.end())
    return result;

  // Keys of stored VirtualDispatch edges, so synthesized expansions never
  // duplicate an edge already on disk (e.g. a snapshot baked with the old
  // build-time fan-out).
  std::set<std::pair<SId, SId>> seen; // (callee, callSite)
  for (size_t idx : it->second) {
    const StoredEdge &se = edges_[idx];
    if (se.refs == 0)
      continue;
    // Deferred join rows are expanded below, never shown raw.
    if (se.kind == EdgeKind::FunctionPointerReturn)
      continue;
    if (se.kind == EdgeKind::VirtualDispatch)
      seen.insert({se.callee, se.callSite});
    result.push_back(materialize(se));
  }

  // Expand Plausible virtual dispatch through transitive overrides.
  for (size_t idx : it->second) {
    const StoredEdge &se = edges_[idx];
    if (se.refs == 0 || se.kind != EdgeKind::VirtualDispatch ||
        se.confidence != Confidence::Plausible)
      continue;
    for (SId ovId : transitiveClosure(se.callee, methodOverrides_)) {
      if (!seen.insert({ovId, se.callSite}).second)
        continue;
      CallGraphEdge synth = materialize(se);
      synth.calleeName = interner_.resolve(ovId);
      result.push_back(std::move(synth));
    }
  }

  // Expand deferred function-pointer-through-return edges: the stored
  // callee is the returning function; synthesize an edge to each function
  // it is known (now, at query time) to return. Empty joins yield nothing.
  for (size_t idx : it->second) {
    const StoredEdge &se = edges_[idx];
    if (se.refs == 0 || se.kind != EdgeKind::FunctionPointerReturn)
      continue;
    auto rit = functionReturns_.find(se.callee);
    if (rit == functionReturns_.end())
      continue;
    for (SId retId : rit->second) {
      if (!seen.insert({retId, se.callSite}).second)
        continue;
      CallGraphEdge synth = materialize(se);
      synth.calleeName = interner_.resolve(retId);
      synth.kind = se.execContext != ExecutionContext::Synchronous
                       ? EdgeKind::ThreadEntry
                       : EdgeKind::FunctionPointer;
      result.push_back(std::move(synth));
    }
  }
  return result;
}

std::vector<CallGraphEdge>
CallGraph::callersOf(const std::string &name) const {
  std::vector<CallGraphEdge> result;
  auto id = interner_.find(name);
  if (!id)
    return result;

  std::set<std::pair<SId, SId>> seen; // (caller, callSite)
  auto it = inEdges_.find(*id);
  if (it != inEdges_.end()) {
    for (size_t idx : it->second) {
      const StoredEdge &se = edges_[idx];
      if (se.refs == 0)
        continue;
      // Deferred join rows point at the *returning* function; they are
      // expanded onto the returned targets below, never shown raw.
      if (se.kind == EdgeKind::FunctionPointerReturn)
        continue;
      seen.insert({se.caller, se.callSite});
      result.push_back(materialize(se));
    }
  }

  // A dispatch recorded against any base declaration of this method can
  // reach it at runtime: synthesize caller -> name for those call sites.
  for (SId baseId : transitiveClosure(*id, overrideBases_)) {
    auto bit = inEdges_.find(baseId);
    if (bit == inEdges_.end())
      continue;
    for (size_t idx : bit->second) {
      const StoredEdge &se = edges_[idx];
      if (se.refs == 0 || se.kind != EdgeKind::VirtualDispatch ||
          se.confidence != Confidence::Plausible)
        continue;
      if (!seen.insert({se.caller, se.callSite}).second)
        continue;
      CallGraphEdge synth = materialize(se);
      synth.calleeName = name;
      result.push_back(std::move(synth));
    }
  }

  // Deferred function-pointer-through-return expansion: any call site that
  // consumed a pointer returned by F reaches `name` when F returns it.
  auto rbIt = returnedBy_.find(*id);
  if (rbIt != returnedBy_.end()) {
    for (SId returner : rbIt->second) {
      auto rin = inEdges_.find(returner);
      if (rin == inEdges_.end())
        continue;
      for (size_t idx : rin->second) {
        const StoredEdge &se = edges_[idx];
        if (se.refs == 0 || se.kind != EdgeKind::FunctionPointerReturn)
          continue;
        if (!seen.insert({se.caller, se.callSite}).second)
          continue;
        CallGraphEdge synth = materialize(se);
        synth.calleeName = name;
        synth.kind = se.execContext != ExecutionContext::Synchronous
                         ? EdgeKind::ThreadEntry
                         : EdgeKind::FunctionPointer;
        result.push_back(std::move(synth));
      }
    }
  }
  return result;
}

size_t CallGraph::storedInDegree(const std::string &name) const {
  auto id = interner_.find(name);
  if (!id)
    return 0;
  auto it = inEdges_.find(*id);
  if (it == inEdges_.end())
    return 0;
  size_t count = 0;
  for (size_t idx : it->second) {
    if (edges_[idx].refs > 0)
      ++count;
  }
  return count;
}

size_t CallGraph::nodeCount() const { return nodes_.size(); }

size_t CallGraph::edgeCount() const { return liveEdgeCount_; }

std::vector<const CallGraphNode *> CallGraph::allNodes() const {
  std::vector<const CallGraphNode *> result;
  result.reserve(nodes_.size());
  for (const auto &kv : nodes_)
    result.push_back(&kv.second);
  return result;
}

const CallGraphNode *
CallGraph::findNode(const std::string &qualifiedName) const {
  auto id = interner_.find(qualifiedName);
  if (!id)
    return nullptr;
  auto it = nodes_.find(*id);
  if (it != nodes_.end())
    return &it->second;
  return nullptr;
}

// --- Class hierarchy ---

void CallGraph::addDerivedClass(const std::string &baseClass,
                                const std::string &derivedClass,
                                const std::string & /*tuPath*/) {
  std::lock_guard<std::mutex> lock(mutex_);
  SId baseId = interner_.intern(baseClass);
  SId derivedId = interner_.intern(derivedClass);
  auto &vec = derivedClasses_[baseId];
  if (std::find(vec.begin(), vec.end(), derivedId) == vec.end())
    vec.push_back(derivedId);
}

std::vector<std::string>
CallGraph::getDerivedClasses(const std::string &baseClass) const {
  auto id = interner_.find(baseClass);
  if (!id)
    return {};
  auto it = derivedClasses_.find(*id);
  if (it != derivedClasses_.end()) {
    std::vector<std::string> result;
    result.reserve(it->second.size());
    for (SId sid : it->second)
      result.push_back(interner_.resolve(sid));
    return result;
  }
  return {};
}

std::vector<std::string>
CallGraph::getAllDerivedClasses(const std::string &baseClass) const {
  auto baseId = interner_.find(baseClass);
  if (!baseId)
    return {};
  std::vector<std::string> result;
  std::vector<SId> stack = {*baseId};
  std::set<SId> visited;
  while (!stack.empty()) {
    SId cls = stack.back();
    stack.pop_back();
    if (!visited.insert(cls).second)
      continue;
    auto it = derivedClasses_.find(cls);
    if (it != derivedClasses_.end()) {
      for (SId derived : it->second) {
        result.push_back(interner_.resolve(derived));
        stack.push_back(derived);
      }
    }
  }
  return result;
}

// --- Virtual method overrides ---

void CallGraph::addMethodOverride(const std::string &baseMethod,
                                  const std::string &overrideMethod,
                                  const std::string & /*tuPath*/) {
  std::lock_guard<std::mutex> lock(mutex_);
  SId baseId = interner_.intern(baseMethod);
  SId overrideId = interner_.intern(overrideMethod);
  auto &vec = methodOverrides_[baseId];
  if (std::find(vec.begin(), vec.end(), overrideId) == vec.end())
    vec.push_back(overrideId);
  auto &rev = overrideBases_[overrideId];
  if (std::find(rev.begin(), rev.end(), baseId) == rev.end())
    rev.push_back(baseId);
}

std::vector<std::string>
CallGraph::getOverrides(const std::string &baseMethod) const {
  auto id = interner_.find(baseMethod);
  if (!id)
    return {};
  auto it = methodOverrides_.find(*id);
  if (it != methodOverrides_.end()) {
    std::vector<std::string> result;
    result.reserve(it->second.size());
    for (SId sid : it->second)
      result.push_back(interner_.resolve(sid));
    return result;
  }
  return {};
}

std::vector<std::string>
CallGraph::getTransitiveOverrides(const std::string &baseMethod) const {
  auto id = interner_.find(baseMethod);
  if (!id)
    return {};
  std::vector<std::string> result;
  for (SId sid : transitiveClosure(*id, methodOverrides_))
    result.push_back(interner_.resolve(sid));
  return result;
}

std::vector<std::string>
CallGraph::getOverriddenBases(const std::string &method) const {
  auto id = interner_.find(method);
  if (!id)
    return {};
  std::vector<std::string> result;
  for (SId sid : transitiveClosure(*id, overrideBases_))
    result.push_back(interner_.resolve(sid));
  return result;
}

// --- Effective implementation mapping ---

void CallGraph::addEffectiveImpl(const std::string &concreteClass,
                                 const std::string &implMethod,
                                 const std::string & /*tuPath*/) {
  std::lock_guard<std::mutex> lock(mutex_);
  SId implId = interner_.intern(implMethod);
  SId classId = interner_.intern(concreteClass);
  effectiveImplClasses_[implId].insert(classId);
}

std::vector<std::string>
CallGraph::getClassesForImpl(const std::string &implMethod) const {
  auto id = interner_.find(implMethod);
  if (!id)
    return {};
  auto it = effectiveImplClasses_.find(*id);
  if (it != effectiveImplClasses_.end()) {
    std::vector<std::string> result;
    result.reserve(it->second.size());
    for (SId sid : it->second)
      result.push_back(interner_.resolve(sid));
    return result;
  }
  return {};
}

// --- Function returns ---

void CallGraph::addFunctionReturn(const std::string &funcName,
                                  const std::string &returnedFunc,
                                  const std::string & /*tuPath*/) {
  std::lock_guard<std::mutex> lock(mutex_);
  SId funcId = interner_.intern(funcName);
  SId retId = interner_.intern(returnedFunc);
  if (functionReturns_[funcId].insert(retId).second)
    returnedBy_[retId].push_back(funcId);
}

std::set<std::string>
CallGraph::getFunctionReturns(const std::string &funcName) const {
  auto id = interner_.find(funcName);
  if (!id)
    return {};
  auto it = functionReturns_.find(*id);
  if (it != functionReturns_.end()) {
    std::set<std::string> result;
    for (SId sid : it->second)
      result.insert(interner_.resolve(sid));
    return result;
  }
  return {};
}

// --- Per-TU removal ---

size_t CallGraph::removeTU(const std::string &tuPath) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto tuIdOpt = interner_.find(tuPath);
  if (!tuIdOpt)
    return 0;
  SId tuId = *tuIdOpt;
  size_t removed = 0;

  auto eit = tuEdges_.find(tuId);
  if (eit != tuEdges_.end()) {
    for (size_t idx : eit->second) {
      auto &edge = edges_[idx];
      if (edge.refs == 0)
        continue;
      // Release this TU's registration; the edge survives while other
      // contributors (TUs or untagged additions) still reference it.
      if (--edge.refs > 0)
        continue;
      auto &ov = outEdges_[edge.caller];
      ov.erase(std::remove(ov.begin(), ov.end(), idx), ov.end());
      auto &iv = inEdges_[edge.callee];
      iv.erase(std::remove(iv.begin(), iv.end(), idx), iv.end());
      edgeIndex_.erase(keyOf(edge));
      --liveEdgeCount_;
      ++removed;
    }
    tuEdges_.erase(eit);
  }

  std::vector<SId> deadNodes;
  for (auto &[nodeId, tus] : nodeContributors_) {
    tus.erase(tuId);
    if (tus.empty())
      deadNodes.push_back(nodeId);
  }
  for (SId nid : deadNodes) {
    nodes_.erase(nid);
    nodeContributors_.erase(nid);
  }

  return removed;
}

void CallGraph::compact() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::deque<StoredEdge> newEdges;
  std::unordered_map<EdgeKey, size_t, EdgeKeyHash> newIndex;
  std::unordered_map<SId, std::vector<size_t>> newOut;
  std::unordered_map<SId, std::vector<size_t>> newIn;

  // Old index -> new index, for remapping TU provenance.
  std::unordered_map<size_t, size_t> remap;

  for (size_t old = 0; old < edges_.size(); ++old) {
    auto &edge = edges_[old];
    if (edge.refs == 0)
      continue;
    size_t neu = newEdges.size();
    remap.emplace(old, neu);
    newOut[edge.caller].push_back(neu);
    newIn[edge.callee].push_back(neu);
    newIndex.emplace(keyOf(edge), neu);
    newEdges.push_back(edge);
  }

  std::unordered_map<SId, std::vector<size_t>> newTuEdges;
  for (auto &[tuId, indices] : tuEdges_) {
    auto &vec = newTuEdges[tuId];
    for (size_t old : indices) {
      auto it = remap.find(old);
      if (it != remap.end())
        vec.push_back(it->second);
    }
    if (vec.empty())
      newTuEdges.erase(tuId);
  }

  edges_ = std::move(newEdges);
  edgeIndex_ = std::move(newIndex);
  outEdges_ = std::move(newOut);
  inEdges_ = std::move(newIn);
  tuEdges_ = std::move(newTuEdges);
}

} // namespace vycor
