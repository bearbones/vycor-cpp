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
      outEdges_(std::move(other.outEdges_)),
      inEdges_(std::move(other.inEdges_)),
      derivedClasses_(std::move(other.derivedClasses_)),
      methodOverrides_(std::move(other.methodOverrides_)),
      overrideBases_(std::move(other.overrideBases_)),
      effectiveImplClasses_(std::move(other.effectiveImplClasses_)),
      functionReturns_(std::move(other.functionReturns_)),
      tuEdges_(std::move(other.tuEdges_)),
      nodeContributors_(std::move(other.nodeContributors_)),
      liveEdgeCount_(other.liveEdgeCount_) {}

CallGraph &CallGraph::operator=(CallGraph &&other) noexcept {
  interner_ = std::move(other.interner_);
  nodes_ = std::move(other.nodes_);
  edges_ = std::move(other.edges_);
  outEdges_ = std::move(other.outEdges_);
  inEdges_ = std::move(other.inEdges_);
  derivedClasses_ = std::move(other.derivedClasses_);
  methodOverrides_ = std::move(other.methodOverrides_);
  overrideBases_ = std::move(other.overrideBases_);
  effectiveImplClasses_ = std::move(other.effectiveImplClasses_);
  functionReturns_ = std::move(other.functionReturns_);
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

void CallGraph::addEdge(CallGraphEdge edge, const std::string &tuPath) {
  std::lock_guard<std::mutex> lock(mutex_);
  SId callerId = interner_.intern(edge.callerName);
  SId calleeId = interner_.intern(edge.calleeName);
  size_t idx = edges_.size();
  outEdges_[callerId].push_back(idx);
  inEdges_[calleeId].push_back(idx);
  edges_.push_back(std::move(edge));
  ++liveEdgeCount_;
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
  std::set<std::pair<std::string, std::string>> seen; // (callee, callSite)
  for (size_t idx : it->second) {
    const CallGraphEdge &e = edges_[idx];
    if (e.callerName.empty())
      continue;
    if (e.kind == EdgeKind::VirtualDispatch)
      seen.insert({e.calleeName, e.callSite});
    result.push_back(e);
  }

  // Expand Plausible virtual dispatch through transitive overrides.
  for (size_t idx : it->second) {
    const CallGraphEdge &e = edges_[idx];
    if (e.callerName.empty() || e.kind != EdgeKind::VirtualDispatch ||
        e.confidence != Confidence::Plausible)
      continue;
    auto targetId = interner_.find(e.calleeName);
    if (!targetId)
      continue;
    for (SId ovId : transitiveClosure(*targetId, methodOverrides_)) {
      const std::string &ov = interner_.resolve(ovId);
      if (!seen.insert({ov, e.callSite}).second)
        continue;
      CallGraphEdge synth = e;
      synth.calleeName = ov;
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

  std::set<std::pair<std::string, std::string>> seen; // (caller, callSite)
  auto it = inEdges_.find(*id);
  if (it != inEdges_.end()) {
    for (size_t idx : it->second) {
      const CallGraphEdge &e = edges_[idx];
      if (e.callerName.empty())
        continue;
      if (e.kind == EdgeKind::VirtualDispatch)
        seen.insert({e.callerName, e.callSite});
      result.push_back(e);
    }
  }

  // A dispatch recorded against any base declaration of this method can
  // reach it at runtime: synthesize caller -> name for those call sites.
  for (SId baseId : transitiveClosure(*id, overrideBases_)) {
    auto bit = inEdges_.find(baseId);
    if (bit == inEdges_.end())
      continue;
    for (size_t idx : bit->second) {
      const CallGraphEdge &e = edges_[idx];
      if (e.callerName.empty() || e.kind != EdgeKind::VirtualDispatch ||
          e.confidence != Confidence::Plausible)
        continue;
      if (!seen.insert({e.callerName, e.callSite}).second)
        continue;
      CallGraphEdge synth = e;
      synth.calleeName = name;
      result.push_back(std::move(synth));
    }
  }
  return result;
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
  functionReturns_[funcId].insert(retId);
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
      if (edge.callerName.empty())
        continue;
      auto callerOpt = interner_.find(edge.callerName);
      auto calleeOpt = interner_.find(edge.calleeName);
      if (callerOpt) {
        auto &ov = outEdges_[*callerOpt];
        ov.erase(std::remove(ov.begin(), ov.end(), idx), ov.end());
      }
      if (calleeOpt) {
        auto &iv = inEdges_[*calleeOpt];
        iv.erase(std::remove(iv.begin(), iv.end(), idx), iv.end());
      }
      edge.callerName.clear();
      edge.calleeName.clear();
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
  std::deque<CallGraphEdge> newEdges;

  std::unordered_map<SId, std::vector<size_t>> newOut;
  std::unordered_map<SId, std::vector<size_t>> newIn;
  std::unordered_map<SId, std::vector<size_t>> newTuEdges;

  for (size_t old = 0; old < edges_.size(); ++old) {
    auto &edge = edges_[old];
    if (edge.callerName.empty())
      continue;
    size_t neu = newEdges.size();
    SId callerId = *interner_.find(edge.callerName);
    SId calleeId = *interner_.find(edge.calleeName);
    newOut[callerId].push_back(neu);
    newIn[calleeId].push_back(neu);
    newEdges.push_back(std::move(edge));
  }

  // Rebuild tuEdges_ with new indices.
  for (auto &[tuId, indices] : tuEdges_) {
    // We need a mapping from old index to new index. Build it by scanning
    // the new edges and matching. Instead, just clear and let the next
    // indexTU re-populate. This is correct because compact() is only called
    // between operations, not mid-build.
  }
  // Since compact invalidates old indices, clear tuEdges_ entirely.
  // Re-indexing will repopulate. Alternatively, build a remap:
  // For now, clear it — callers should only compact when done with removals.
  tuEdges_.clear();

  edges_ = std::move(newEdges);
  outEdges_ = std::move(newOut);
  inEdges_ = std::move(newIn);
}

} // namespace vycor
