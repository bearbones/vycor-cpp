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

#include "vycor/check/LockJoin.h"

#include "llvm/ADT/STLExtras.h"

#include <algorithm>
#include <tuple>

namespace vycor {

// ============================================================================
// LockSets
// ============================================================================

LockSets::LockSets() {
  sets_.emplace_back();
  flags_.push_back(0);
  byContent_.emplace(std::vector<uint32_t>{}, 0);
}

uint32_t LockSets::lock(llvm::StringRef identity, llvm::StringRef display) {
  auto [it, inserted] =
      byIdentity_.try_emplace(identity, static_cast<uint32_t>(locks_.size()));
  if (inserted)
    locks_.push_back({identity.str(), display.str(),
                      identity == kLockUnknownIdentity,
                      identity.starts_with(kLockOncePrefix)});
  else if (locks_[it->second].display.empty() && !display.empty())
    locks_[it->second].display = display.str();
  return it->second;
}

std::optional<uint32_t> LockSets::find(llvm::StringRef identity) const {
  auto it = byIdentity_.find(identity);
  if (it == byIdentity_.end())
    return std::nullopt;
  return it->second;
}

uint32_t LockSets::intern(llvm::SmallVectorImpl<uint32_t> &locks) {
  if (locks.empty())
    return 0;
  llvm::sort(locks);
  locks.erase(std::unique(locks.begin(), locks.end()), locks.end());
  std::vector<uint32_t> key(locks.begin(), locks.end());
  auto [it, inserted] = byContent_.try_emplace(
      std::move(key), static_cast<uint32_t>(sets_.size()));
  if (inserted) {
    uint8_t flags = 0;
    for (uint32_t l : it->first) {
      flags |= locks_[l].once ? kOnce : 0;
      flags |= locks_[l].unknown ? kUnknown : 0;
    }
    sets_.push_back(it->first);
    flags_.push_back(flags);
  }
  return it->second;
}

uint32_t LockSets::unite(uint32_t a, uint32_t b) {
  if (a == b || b == 0)
    return a;
  if (a == 0)
    return b;
  if (a > b)
    std::swap(a, b);
  const uint64_t key = lockPairKey(a, b);
  auto found = unions_.find(key);
  if (found != unions_.end())
    return found->second;
  llvm::SmallVector<uint32_t, 8> merged(sets_[a].begin(), sets_[a].end());
  merged.append(sets_[b].begin(), sets_[b].end());
  const uint32_t out = intern(merged);
  unions_[key] = out;
  return out;
}

bool LockSets::disjoint(uint32_t a, uint32_t b) const {
  if (a == 0 || b == 0)
    return true;
  if (((flags_[a] | flags_[b]) & kUnknown) != 0)
    return false;
  const std::vector<uint32_t> &x = sets_[a];
  const std::vector<uint32_t> &y = sets_[b];
  size_t i = 0, j = 0;
  while (i < x.size() && j < y.size()) {
    if (x[i] == y[j])
      return false;
    if (x[i] < y[j])
      ++i;
    else
      ++j;
  }
  return true;
}

std::string LockSets::describe(uint32_t set) const {
  if (set == 0)
    return "no lock";
  std::vector<llvm::StringRef> names;
  for (uint32_t l : sets_[set])
    names.push_back(display(l));
  llvm::sort(names);
  std::string out = "{";
  for (size_t i = 0; i < names.size(); ++i) {
    if (i)
      out += ", ";
    out += names[i].str();
  }
  return out + "}";
}

uint32_t LockSets::unknownSet() {
  if (unknownSet_ == UINT32_MAX) {
    llvm::SmallVector<uint32_t, 1> one{
        lock(kLockUnknownIdentity, "<unknown mutex>")};
    unknownSet_ = intern(one);
  }
  return unknownSet_;
}

bool LockSets::less(uint32_t a, uint32_t b) {
  if (a == b)
    return false;
  orderByIdentity(a);
  orderByIdentity(b); // may grow ordered_: take references after both
  const auto &x = ordered_[a];
  const auto &y = ordered_[b];
  return std::lexicographical_compare(
      x.begin(), x.end(), y.begin(), y.end(), [&](uint32_t l, uint32_t r) {
        return locks_[l].identity < locks_[r].identity;
      });
}

void LockSets::orderByIdentity(uint32_t set) {
  if (ordered_.size() < sets_.size()) {
    ordered_.resize(sets_.size());
    haveOrder_.resize(sets_.size(), 0);
  }
  if (!haveOrder_[set]) {
    auto &order = ordered_[set];
    order.assign(sets_[set].begin(), sets_[set].end());
    llvm::sort(order, [&](uint32_t l, uint32_t r) {
      return locks_[l].identity < locks_[r].identity;
    });
    haveOrder_[set] = 1;
  }
}

// ============================================================================
// CallLockFacts
// ============================================================================

namespace {

bool isSpawn(ExecutionContext exec) {
  return exec == ExecutionContext::ThreadSpawn ||
         exec == ExecutionContext::AsyncTask ||
         exec == ExecutionContext::PackagedTask;
}

} // namespace

CallLockFacts::CallLockFacts(const CallGraph &graph, const ReferenceIndex &refs)
    : graph_(graph), refs_(refs) {
  indirect_ = graph_.interner().find("<indirect>");
  indirectSynth_ = graph_.interner().find("vycor-synth:<indirect>");
}

CallLockFacts::GId CallLockFacts::graphId(RId id) {
  auto found = graphIdOf_.find(id);
  if (found != graphIdOf_.end())
    return found->second;
  const auto gid = graph_.interner().find(refs_.textOf(id));
  const GId out = gid ? *gid : kNone;
  graphIdOf_[id] = out;
  return out;
}

uint32_t CallLockFacts::functionFlags(GId id) const {
  const auto found = functionFlags_.find(id);
  return found == functionFlags_.end() ? 0 : found->second;
}

uint32_t CallLockFacts::functionIndex(GId id) {
  auto [it, inserted] =
      functionOf_.try_emplace(id, static_cast<uint32_t>(functions_.size()));
  if (inserted) {
    Function fn;
    fn.id = id;
    functions_.push_back(fn);
  }
  return it->second;
}

void CallLockFacts::readSymbols(
    llvm::function_ref<void(const ReferenceIndex::SymbolRecord &)> extra) {
  refs_.forEachSymbolRecord([&](const ReferenceIndex::SymbolRecord &rec,
                                uint32_t, ReferenceIndex::Id) {
    if (rec.kind == SymbolKind::Function) {
      const GId gid = graphId(rec.usr);
      if (gid != kNone)
        functionFlags_[gid] |= rec.flags;
    } else if (rec.kind == SymbolKind::Variable) {
      variables_.insert(rec.usr);
    }
    if (extra)
      extra(rec);
  });
}

void CallLockFacts::readReferences(
    llvm::function_ref<void(const ReferenceIndex::ReferenceRecord &, GId)>
        extra) {
  refs_.forEachReferenceRecord([&](const ReferenceIndex::ReferenceRecord &rec,
                                   uint32_t, ReferenceIndex::Id,
                                   ReferenceIndex::Id) {
    if (rec.role == RefRole::VtableUse || rec.role == RefRole::TypeinfoUse)
      return;
    // Every lock any code holds is known before a search, so a variable
    // used as a lock is recognized wherever it is locked.
    if (rec.locks != ReferenceIndex::kNoId)
      (void)lockEntries(rec.locks);
    const GId referrer = graphId(rec.referrer);
    if (referrer == kNone)
      return; // a variable initializer or file scope: static initialization
    if (!isLockRole(rec.role) && !variables_.count(rec.target)) {
      const GId target = graphId(rec.target);
      if (target != kNone) {
        const uint64_t key = lockPairKey(referrer, target);
        if (rec.role == RefRole::Call)
          callLocks_[key].push_back({rec.locks, rec.file});
        else if (rec.role == RefRole::AddressOf)
          addressLocks_[key].push_back({rec.locks, rec.file});
      }
    }
    if (extra)
      extra(rec, referrer);
  });
}

llvm::ArrayRef<LockEntry> CallLockFacts::lockEntries(RId text) {
  auto found = lockEntriesOf_.find(text);
  if (found == lockEntriesOf_.end()) {
    const auto begin = static_cast<uint32_t>(lockEntryPool_.size());
    llvm::StringRef rest(refs_.textOf(text));
    llvm::SmallVector<uint32_t, 4> locks;
    while (!rest.empty()) {
      auto [line, tail] = rest.split('\n');
      rest = tail;
      auto [where, fields] = line.split('\t');
      auto [lineText, colText] = where.split(':');
      LockEntry entry{0, 0, 0};
      if (lineText.getAsInteger(10, entry.line))
        entry.line = 0;
      if (colText.getAsInteger(10, entry.col))
        entry.col = 0;
      locks.clear();
      while (!fields.empty()) {
        auto [identity, after] = fields.split('\t');
        auto [display, next] = after.split('\t');
        fields = next;
        locks.push_back(sets_.lock(identity, display));
      }
      entry.set = sets_.intern(locks);
      lockEntryPool_.push_back(entry);
    }
    found = lockEntriesOf_
                .try_emplace(text, begin,
                             static_cast<uint32_t>(lockEntryPool_.size()) -
                                 begin)
                .first;
  }
  return llvm::ArrayRef<LockEntry>(lockEntryPool_)
      .slice(found->second.first, found->second.second);
}

// A call of a function of the program's own, not of a lambda or of the
// standard library: a callable handed to it runs inside it (withLock(f)).
bool CallLockFacts::isUserReceiver(const CallGraph::EdgeRef &call) const {
  switch (call.kind) {
  case EdgeKind::DirectCall:
  case EdgeKind::VirtualDispatch:
  case EdgeKind::OperatorCall:
  case EdgeKind::ConstructorCall:
  case EdgeKind::TemplateInstantiation:
    break;
  default:
    return false;
  }
  if ((functionFlags(call.callee) & (kSymLambda | kSymSystemHeader)) != 0)
    return false;
  return !llvm::StringRef(graph_.interner().resolve(call.callee))
              .starts_with("c:@N@std@");
}

llvm::ArrayRef<LockCallEdge> CallLockFacts::callees(uint32_t fn) {
  if (!functions_[fn].edgesBuilt)
    buildEdges(fn);
  const Function &f = functions_[fn];
  return llvm::ArrayRef<LockCallEdge>(edges_).slice(f.edgeBegin,
                                                    f.edgeEnd - f.edgeBegin);
}

void CallLockFacts::buildEdges(uint32_t fn) {
  const GId caller = functions_[fn].id;
  scratch_ = graph_.calleeRefsOf(caller);
  std::vector<CallGraph::EdgeRef> &out = scratch_;
  const auto &text = [&](GId id) -> const std::string & {
    return graph_.interner().resolve(id);
  };
  llvm::sort(out, [&](const CallGraph::EdgeRef &a, const CallGraph::EdgeRef &b) {
    return std::make_tuple(std::cref(text(a.callee)), std::cref(text(a.callSite)),
                           static_cast<int>(a.kind)) <
           std::make_tuple(std::cref(text(b.callee)), std::cref(text(b.callSite)),
                           static_cast<int>(b.kind));
  });
  const auto begin = static_cast<uint32_t>(edges_.size());
  for (const CallGraph::EdgeRef &e : out) {
    if ((indirect_ && e.callee == *indirect_) ||
        (indirectSynth_ && e.callee == *indirectSynth_))
      continue;
    if (e.kind == EdgeKind::ThreadEntry && isSpawn(e.execContext))
      continue;
    if (e.kind == EdgeKind::FunctionPointerReturn)
      continue; // a deferred join; calleeRefsOf already synthesized it
    LockCallEdge edge;
    edge.calleeId = e.callee;
    edge.lockTarget = e.callee;
    edge.callSite = e.callSite;
    edge.kind = e.kind;
    edge.plausible = e.confidence != Confidence::Proven;
    edge.lambda = (functionFlags(e.callee) & kSymLambda) != 0;
    edge.handedOn = false;
    if (e.kind == EdgeKind::LambdaCall || e.kind == EdgeKind::FunctionPointer)
      for (const CallGraph::EdgeRef &sib : out)
        if (sib.callSite == e.callSite && sib.callee != e.callee &&
            isUserReceiver(sib)) {
          edge.handedOn = true;
          break;
        }
    // A virtual call's records name the static target, not each override:
    // take the locks from the edge of the same call site that names it.
    if (e.kind == EdgeKind::VirtualDispatch &&
        !callLocks_.count(lockPairKey(caller, e.callee))) {
      for (const CallGraph::EdgeRef &sib : out)
        if (sib.callSite == e.callSite &&
            sib.kind == EdgeKind::VirtualDispatch &&
            callLocks_.count(lockPairKey(caller, sib.callee))) {
          edge.lockTarget = sib.callee;
          break;
        }
    }
    edge.callee = kNone; // resolved below (functionIndex may grow functions_)
    edges_.push_back(edge);
  }
  const auto end = static_cast<uint32_t>(edges_.size());
  for (uint32_t i = begin; i < end; ++i)
    edges_[i].callee = functionIndex(edges_[i].calleeId);
  Function &f = functions_[fn];
  f.edgeBegin = begin;
  f.edgeEnd = end;
  f.edgesBuilt = true;
}

const llvm::SmallVector<CallLockFacts::LockText, 1> *
CallLockFacts::lockTextsOf(uint32_t caller, const LockCallEdge &edge) const {
  const uint64_t key = lockPairKey(functions_[caller].id, edge.lockTarget);
  if (auto it = callLocks_.find(key); it != callLocks_.end())
    return &it->second;
  // A call through a pointer, std::invoke, std::call_once or an algorithm
  // names no call: the locks are those where the function's address was
  // taken.
  if (auto at = addressLocks_.find(key); at != addressLocks_.end())
    return &at->second;
  return nullptr;
}

void CallLockFacts::hops(uint32_t caller, const LockCallEdge &edge,
                         llvm::SmallVectorImpl<LockHop> &out) {
  out.clear();
  if (edge.handedOn) {
    out.push_back({sets_.unknownSet(), ReferenceIndex::kNoId, 0, 0});
    return;
  }
  const llvm::SmallVector<LockText, 1> *texts = lockTextsOf(caller, edge);
  if (!texts) {
    // A lambda's creation is recorded only under a lock, and a call of a
    // local holding it always: none recorded means none was held. Any
    // other edge with no record (an implicit destructor, a call the
    // reference facts do not see) may run under any lock: unknown, never
    // no lock.
    out.push_back({edge.lambda ? 0u : sets_.unknownSet(),
                   ReferenceIndex::kNoId, 0, 0});
    return;
  }
  for (const LockText &t : *texts) {
    if (t.text == ReferenceIndex::kNoId) {
      out.push_back({0, t.file, 0, 0});
      continue;
    }
    for (const LockEntry &entry : lockEntries(t.text))
      out.push_back({entry.set, t.file, entry.line, entry.col});
  }
  // One per set: the first by (set, file, line, col) after a canonical
  // order of the sets.
  const auto &fileText = [&](RId id) -> llvm::StringRef {
    return id == ReferenceIndex::kNoId ? llvm::StringRef()
                                       : llvm::StringRef(refs_.textOf(id));
  };
  llvm::sort(out, [&](const LockHop &a, const LockHop &b) {
    if (a.set != b.set)
      return sets_.less(a.set, b.set);
    return std::make_tuple(fileText(a.file), a.line, a.col) <
           std::make_tuple(fileText(b.file), b.line, b.col);
  });
  out.erase(std::unique(out.begin(), out.end(),
                        [](const LockHop &a, const LockHop &b) {
                          return a.set == b.set;
                        }),
            out.end());
}

void CallLockFacts::hopSets(uint32_t caller, const LockCallEdge &edge,
                            llvm::SmallVectorImpl<uint32_t> &out) {
  out.clear();
  if (edge.handedOn) {
    out.push_back(sets_.unknownSet());
    return;
  }
  const llvm::SmallVector<LockText, 1> *texts = lockTextsOf(caller, edge);
  if (!texts) {
    out.push_back(edge.lambda ? 0 : sets_.unknownSet());
    return;
  }
  for (const LockText &t : *texts) {
    if (t.text == ReferenceIndex::kNoId) {
      out.push_back(0);
      continue;
    }
    for (const LockEntry &entry : lockEntries(t.text))
      out.push_back(entry.set);
  }
  llvm::sort(out);
  out.erase(std::unique(out.begin(), out.end()), out.end());
  llvm::sort(out, [&](uint32_t a, uint32_t b) { return sets_.less(a, b); });
}

} // namespace vycor
