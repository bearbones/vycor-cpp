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
#include <unordered_set>

namespace vycor {

CallGraph::CallGraph(CallGraph &&other) noexcept
    : interner_(std::move(other.interner_)),
      nodes_(std::move(other.nodes_)),
      byName_(std::move(other.byName_)),
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
      tuNodes_(std::move(other.tuNodes_)),
      liveEdgeCount_(other.liveEdgeCount_) {}

CallGraph &CallGraph::operator=(CallGraph &&other) noexcept {
  interner_ = std::move(other.interner_);
  nodes_ = std::move(other.nodes_);
  byName_ = std::move(other.byName_);
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
  tuNodes_ = std::move(other.tuNodes_);
  liveEdgeCount_ = other.liveEdgeCount_;
  return *this;
}

void CallGraph::reserveNodes(size_t n) {
  std::lock_guard<std::mutex> lock(mutex_);
  nodes_.reserve(n);
  nodeContributors_.reserve(n);
  outEdges_.reserve(n);
  inEdges_.reserve(n);
}

void CallGraph::reserveEdges(size_t n) {
  std::lock_guard<std::mutex> lock(mutex_);
  edgeIndex_.reserve(n);
}

void CallGraph::addNode(CallGraphNode node, const std::string &tuPath) {
  std::lock_guard<std::mutex> lock(mutex_);
  // Name-only callers (hand-built graphs, tests) get usr == display, which
  // keeps their edges — also name-keyed — consistent with the node key.
  if (node.usr.empty())
    node.usr = node.qualifiedName;
  SId nameId = interner_.intern(node.usr);
  SId displayId = interner_.intern(node.qualifiedName);
  auto it = nodes_.find(nameId);
  if (it == nodes_.end()) {
    auto &candidates = byName_[displayId];
    if (std::find(candidates.begin(), candidates.end(), nameId) ==
        candidates.end())
      candidates.push_back(nameId);
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
    if (nodeContributors_[nameId].insert(tuId).second)
      tuNodes_[tuId].push_back(nameId);
  }
}

const std::string &CallGraph::displayFor(SId usrId) const {
  auto it = nodes_.find(usrId);
  if (it != nodes_.end())
    return it->second.qualifiedName;
  return interner_.resolve(usrId);
}

std::vector<CallGraph::SId>
CallGraph::resolveUsrIds(const std::string &name) const {
  auto id = interner_.find(name);
  if (!id)
    return {};
  // Exact-usr match first: the string keys a registered node directly.
  if (nodes_.count(*id))
    return {*id};
  auto it = byName_.find(*id);
  if (it != byName_.end() && !it->second.empty())
    return it->second;
  // Unregistered endpoint (edge without a node): usr and display coincide.
  return {*id};
}

std::vector<std::string> CallGraph::usrsForName(const std::string &name) const {
  std::vector<std::string> out;
  for (SId id : resolveUsrIds(name))
    out.push_back(interner_.resolve(id));
  return out;
}

CallGraphEdge CallGraph::materialize(const EdgeRef &r) const {
  CallGraphEdge e;
  e.callerName = displayFor(r.caller);
  e.calleeName = displayFor(r.callee);
  e.kind = r.kind;
  e.confidence = r.confidence;
  e.callSite = interner_.resolve(r.callSite);
  e.indirectionDepth = r.indirectionDepth;
  e.execContext = r.execContext;
  e.callerUsr = interner_.resolve(r.caller);
  e.calleeUsr = interner_.resolve(r.callee);
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
  if (relation.find(start) == relation.end())
    return result; // common case: no entry — skip all allocations
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

std::vector<CallGraph::EdgeRef>
CallGraph::calleeRefsOf(StringInterner::Id caller) const {
  std::vector<EdgeRef> result;
  auto it = outEdges_.find(caller);
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
    result.push_back({se.caller, se.callee, se.callSite, se.kind,
                      se.confidence, se.execContext, se.indirectionDepth});
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
      result.push_back({se.caller, ovId, se.callSite, se.kind,
                        se.confidence, se.execContext, se.indirectionDepth});
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
      EdgeKind kind = se.execContext != ExecutionContext::Synchronous
                          ? EdgeKind::ThreadEntry
                          : EdgeKind::FunctionPointer;
      result.push_back({se.caller, retId, se.callSite, kind, se.confidence,
                        se.execContext, se.indirectionDepth});
    }
  }
  return result;
}

std::vector<CallGraphEdge>
CallGraph::calleesOf(const std::string &name) const {
  std::vector<CallGraphEdge> result;
  // Union over the display-name candidates. Concatenation cannot duplicate:
  // every edge in a candidate's result carries that candidate as its caller,
  // so results from distinct candidates are disjoint.
  for (SId usrId : resolveUsrIds(name)) {
    auto refs = calleeRefsOf(usrId);
    result.reserve(result.size() + refs.size());
    for (const auto &r : refs)
      result.push_back(materialize(r));
  }
  return result;
}

std::vector<CallGraph::EdgeRef>
CallGraph::callerRefsOf(StringInterner::Id callee) const {
  std::vector<EdgeRef> result;

  std::set<std::pair<SId, SId>> seen; // (caller, callSite)
  auto it = inEdges_.find(callee);
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
      result.push_back({se.caller, se.callee, se.callSite, se.kind,
                        se.confidence, se.execContext, se.indirectionDepth});
    }
  }

  // A dispatch recorded against any base declaration of this method can
  // reach it at runtime: synthesize caller -> callee for those call sites.
  for (SId baseId : transitiveClosure(callee, overrideBases_)) {
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
      result.push_back({se.caller, callee, se.callSite, se.kind,
                        se.confidence, se.execContext, se.indirectionDepth});
    }
  }

  // Deferred function-pointer-through-return expansion: any call site that
  // consumed a pointer returned by F reaches `callee` when F returns it.
  auto rbIt = returnedBy_.find(callee);
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
        EdgeKind kind = se.execContext != ExecutionContext::Synchronous
                            ? EdgeKind::ThreadEntry
                            : EdgeKind::FunctionPointer;
        result.push_back({se.caller, callee, se.callSite, kind,
                          se.confidence, se.execContext,
                          se.indirectionDepth});
      }
    }
  }
  return result;
}

std::vector<CallGraphEdge>
CallGraph::callersOf(const std::string &name) const {
  std::vector<CallGraphEdge> result;
  // Union over the display-name candidates; disjoint by construction (every
  // edge in a candidate's result targets that candidate as its callee).
  for (SId usrId : resolveUsrIds(name)) {
    auto refs = callerRefsOf(usrId);
    result.reserve(result.size() + refs.size());
    for (const auto &r : refs)
      result.push_back(materialize(r));
  }
  return result;
}

size_t CallGraph::storedInDegree(const std::string &name) const {
  size_t count = 0;
  for (SId usrId : resolveUsrIds(name))
    count += storedInDegree(usrId);
  return count;
}

size_t CallGraph::storedInDegree(StringInterner::Id id) const {
  auto it = inEdges_.find(id);
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
  const CallGraphNode *best = nullptr;
  for (SId usrId : resolveUsrIds(qualifiedName)) {
    auto it = nodes_.find(usrId);
    if (it == nodes_.end())
      continue;
    // Ambiguous display name: deterministic pick by usr-string order (PR C
    // surfaces the ambiguity itself; findNode serves metadata display).
    if (!best || it->second.usr < best->usr)
      best = &it->second;
  }
  return best;
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
  // Methods overload, so the override maps are usr-keyed: resolve the name
  // to its candidates, union, and emit display names.
  std::vector<std::string> result;
  std::set<SId> seen;
  for (SId usrId : resolveUsrIds(baseMethod)) {
    auto it = methodOverrides_.find(usrId);
    if (it == methodOverrides_.end())
      continue;
    for (SId sid : it->second)
      if (seen.insert(sid).second)
        result.push_back(displayFor(sid));
  }
  return result;
}

std::vector<std::string>
CallGraph::getTransitiveOverrides(const std::string &baseMethod) const {
  std::vector<std::string> result;
  std::set<SId> seen;
  for (SId usrId : resolveUsrIds(baseMethod))
    for (SId sid : transitiveClosure(usrId, methodOverrides_))
      if (seen.insert(sid).second)
        result.push_back(displayFor(sid));
  return result;
}

std::vector<std::string>
CallGraph::getOverriddenBases(const std::string &method) const {
  std::vector<std::string> result;
  std::set<SId> seen;
  for (SId usrId : resolveUsrIds(method))
    for (SId sid : transitiveClosure(usrId, overrideBases_))
      if (seen.insert(sid).second)
        result.push_back(displayFor(sid));
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
  // implMethod is usr-keyed; the class values are display-keyed and resolve
  // directly (classes do not overload).
  std::vector<std::string> result;
  std::set<SId> seen;
  for (SId usrId : resolveUsrIds(implMethod)) {
    auto it = effectiveImplClasses_.find(usrId);
    if (it == effectiveImplClasses_.end())
      continue;
    for (SId sid : it->second)
      if (seen.insert(sid).second)
        result.push_back(interner_.resolve(sid));
  }
  return result;
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
  std::set<std::string> result;
  for (SId usrId : resolveUsrIds(funcName)) {
    auto it = functionReturns_.find(usrId);
    if (it == functionReturns_.end())
      continue;
    for (SId sid : it->second)
      result.insert(displayFor(sid));
  }
  return result;
}

// --- Shard merge ---

void CallGraph::absorb(const CallGraph &shard) {
  if (&shard == this)
    return;
  std::lock_guard<std::mutex> lockThis(mutex_);
  std::lock_guard<std::mutex> lockShard(shard.mutex_);

  // Shard string id -> master string id, by position (the shard interner is
  // id-dense, so a flat vector is the whole remap).
  std::vector<SId> remap;
  remap.reserve(shard.interner_.size());
  shard.interner_.forEachString(
      [&](const std::string &s) { remap.push_back(interner_.intern(s)); });

  // Nodes: addNode's union/backfill semantics (including byName_
  // maintenance), plus contributor provenance.
  for (const auto &[shardNameId, node] : shard.nodes_) {
    SId nameId = remap[shardNameId];
    auto it = nodes_.find(nameId);
    if (it == nodes_.end()) {
      SId displayId = interner_.intern(node.qualifiedName);
      auto &candidates = byName_[displayId];
      if (std::find(candidates.begin(), candidates.end(), nameId) ==
          candidates.end())
        candidates.push_back(nameId);
      nodes_.emplace(nameId, node);
    } else {
      if (!node.file.empty())
        it->second.file = node.file;
      if (node.line != 0)
        it->second.line = node.line;
      if (node.isEntryPoint)
        it->second.isEntryPoint = true;
      if (node.isVirtual)
        it->second.isVirtual = true;
      if (!node.enclosingClass.empty())
        it->second.enclosingClass = node.enclosingClass;
    }
    auto cit = shard.nodeContributors_.find(shardNameId);
    if (cit != shard.nodeContributors_.end()) {
      for (SId shardTu : cit->second) {
        SId tuId = remap[shardTu];
        if (nodeContributors_[nameId].insert(tuId).second)
          tuNodes_[tuId].push_back(nameId);
      }
    }
  }

  // Edges: dedup probe per live shard edge; a hit adds the shard's
  // registrations to refs, a miss appends. shardEdgeIdx -> masterEdgeIdx is
  // kept so the shard's TU provenance can be re-pointed below.
  constexpr size_t kDeadEdge = static_cast<size_t>(-1);
  std::vector<size_t> edgeRemap(shard.edges_.size(), kDeadEdge);
  for (size_t i = 0; i < shard.edges_.size(); ++i) {
    const StoredEdge &src = shard.edges_[i];
    if (src.refs == 0)
      continue;
    StoredEdge se = src;
    se.caller = remap[src.caller];
    se.callee = remap[src.callee];
    se.callSite = remap[src.callSite];
    EdgeKey key = keyOf(se);
    size_t idx;
    auto it = edgeIndex_.find(key);
    if (it != edgeIndex_.end()) {
      idx = it->second;
      edges_[idx].refs += se.refs;
    } else {
      idx = edges_.size();
      outEdges_[se.caller].push_back(idx);
      inEdges_[se.callee].push_back(idx);
      edgeIndex_.emplace(key, idx);
      edges_.push_back(se);
      ++liveEdgeCount_;
    }
    edgeRemap[i] = idx;
  }

  for (const auto &[shardTu, idxs] : shard.tuEdges_) {
    SId tuId = remap[shardTu];
    auto &vec = tuEdges_[tuId];
    for (size_t i : idxs)
      if (edgeRemap[i] != kDeadEdge) // tombstoned shard edges carry no refs
        vec.push_back(edgeRemap[i]);
  }

  // Relations: remapped pairs with the public mutators' dedup + reverse-map
  // maintenance.
  for (const auto &[base, vec] : shard.derivedClasses_) {
    auto &dst = derivedClasses_[remap[base]];
    for (SId d : vec) {
      SId derivedId = remap[d];
      if (std::find(dst.begin(), dst.end(), derivedId) == dst.end())
        dst.push_back(derivedId);
    }
  }
  for (const auto &[base, vec] : shard.methodOverrides_) {
    SId baseId = remap[base];
    for (SId o : vec) {
      SId overrideId = remap[o];
      auto &fwd = methodOverrides_[baseId];
      if (std::find(fwd.begin(), fwd.end(), overrideId) == fwd.end())
        fwd.push_back(overrideId);
      auto &rev = overrideBases_[overrideId];
      if (std::find(rev.begin(), rev.end(), baseId) == rev.end())
        rev.push_back(baseId);
    }
  }
  for (const auto &[impl, classes] : shard.effectiveImplClasses_) {
    auto &dst = effectiveImplClasses_[remap[impl]];
    for (SId cls : classes)
      dst.insert(remap[cls]);
  }
  for (const auto &[fn, rets] : shard.functionReturns_) {
    SId fnId = remap[fn];
    for (SId ret : rets) {
      SId retId = remap[ret];
      if (functionReturns_[fnId].insert(retId).second)
        returnedBy_[retId].push_back(fnId);
    }
  }
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
    // Two passes: mark dead edges first, then scrub each affected
    // adjacency vector ONCE. The old per-edge std::remove was
    // O(degree) per removed edge — quadratic when a removed TU touches a
    // high-fan-in hub.
    std::unordered_set<size_t> dead;
    std::unordered_set<SId> affectedCallers, affectedCallees;
    for (size_t idx : eit->second) {
      auto &edge = edges_[idx];
      if (edge.refs == 0)
        continue;
      // Release this TU's registration; the edge survives while other
      // contributors (TUs or untagged additions) still reference it.
      if (--edge.refs > 0)
        continue;
      dead.insert(idx);
      affectedCallers.insert(edge.caller);
      affectedCallees.insert(edge.callee);
      edgeIndex_.erase(keyOf(edge));
      --liveEdgeCount_;
      ++removed;
    }
    for (SId c : affectedCallers) {
      auto &ov = outEdges_[c];
      ov.erase(std::remove_if(ov.begin(), ov.end(),
                              [&](size_t i) { return dead.count(i) > 0; }),
               ov.end());
    }
    for (SId c : affectedCallees) {
      auto &iv = inEdges_[c];
      iv.erase(std::remove_if(iv.begin(), iv.end(),
                              [&](size_t i) { return dead.count(i) > 0; }),
               iv.end());
    }
    tuEdges_.erase(eit);
  }

  // Visit exactly the nodes this TU contributed (reverse list) instead of
  // scanning every node in the graph.
  auto nit = tuNodes_.find(tuId);
  if (nit != tuNodes_.end()) {
    for (SId nodeId : nit->second) {
      auto cit = nodeContributors_.find(nodeId);
      if (cit == nodeContributors_.end())
        continue;
      cit->second.erase(tuId);
      if (cit->second.empty()) {
        auto nit2 = nodes_.find(nodeId);
        if (nit2 != nodes_.end()) {
          // Drop the disambiguation entry with the node.
          if (auto displayId = interner_.find(nit2->second.qualifiedName)) {
            auto bit = byName_.find(*displayId);
            if (bit != byName_.end()) {
              auto &vec = bit->second;
              vec.erase(std::remove(vec.begin(), vec.end(), nodeId),
                        vec.end());
              if (vec.empty())
                byName_.erase(bit);
            }
          }
          nodes_.erase(nit2);
        }
        nodeContributors_.erase(cit);
      }
    }
    tuNodes_.erase(nit);
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
