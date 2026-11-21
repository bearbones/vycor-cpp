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

#include "vycor/check/LockOrderInversion.h"

#include "vycor/check/LockJoin.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/bit.h"

#include <algorithm>
#include <cassert>
#include <tuple>

namespace vycor {

namespace {

using GId = StringInterner::Id; // call-graph string id
using RId = ReferenceIndex::Id; // reference-index string id
constexpr uint32_t kNone = UINT32_MAX;
constexpr const char *kCheck = "lock-order-inversion";

// A lock acquisition record of a function, its lock a graph node.
struct Acq {
  uint32_t fn;   // function index (CallLockFacts)
  uint32_t node; // the acquired lock
  bool shared;
  RId locks; // held before (lock text), kNoId: none
  RId file;
  uint32_t line;
  uint32_t col;
};

// How an order edge A -> B (or a self-edge A -> A) happens: in `holder`,
// B is acquired while A is held, at the site below (callee == kNone), or
// a call to `callee` is made while A is held, at the site below, and B is
// acquired somewhere beneath it.
struct Witness {
  uint32_t holder = kNone;
  uint32_t callee = kNone;
  RId file = ReferenceIndex::kNoId;
  uint32_t line = 0;
  uint32_t col = 0;
  bool plausible = false;
  bool shared = false;
};

struct OrderEdge {
  uint32_t from;
  uint32_t to;
  Witness witness;
  // The global (not member) named locks held on every witness of the
  // edge besides `from`: a lock common to every edge of a cycle orders
  // the whole cycle (a gate lock).
  uint32_t gate;
};

// The locks each function can acquire, itself or through its calls: one
// bitset per strongly connected component of the call graph, with two
// bits per lock (exclusive, shared). Identical sets are stored once
// (most functions reach the same few), and the empty set is id 0.
struct Closures {
  size_t words = 1; // per set
  std::vector<uint64_t> pool;
  std::vector<uint32_t> ofFn; // by function index; kNone: not computed
  llvm::DenseMap<uint64_t, uint32_t> head; // hash -> first set
  std::vector<uint32_t> next;              // set -> next with that hash
  bool bounded = false;

  [[nodiscard]] const uint64_t *set(uint32_t id) const {
    return pool.data() + size_t(id) * words;
  }
  [[nodiscard]] bool has(uint32_t id, uint32_t bit) const {
    return (set(id)[bit / 64] >> (bit % 64)) & 1u;
  }
  [[nodiscard]] uint32_t of(uint32_t fn) const {
    return fn < ofFn.size() && ofFn[fn] != kNone ? ofFn[fn] : 0;
  }
};

[[nodiscard]] bool isMemberIdentity(llvm::StringRef identity) {
  return identity.contains("@FI@");
}

class Analysis {
public:
  Analysis(const CallGraph &graph, const ReferenceIndex &refs,
           const LockOrderOptions &opts)
      : graph_(graph), refs_(refs), opts_(opts), facts_(graph, refs),
        sets_(facts_.sets()) {}

  std::vector<Diagnostic> run(LockOrderStats &stats);

private:
  // ---- facts ----------------------------------------------------------------
  void readFacts(LockOrderStats &stats);
  void grow();
  [[nodiscard]] uint32_t nodeOf(uint32_t lock) const {
    return lock < nodeOfLock_.size() ? nodeOfLock_[lock] : kNone;
  }
  [[nodiscard]] llvm::ArrayRef<Acq> acqsOf(uint32_t fn) const {
    if (fn >= acqRange_.size())
      return {};
    return llvm::ArrayRef<Acq>(acqs_).slice(
        acqRange_[fn].first, acqRange_[fn].second - acqRange_[fn].first);
  }
  [[nodiscard]] const std::string &usrOf(uint32_t fn) const {
    return graph_.interner().resolve(facts_.functionId(fn));
  }
  [[nodiscard]] llvm::StringRef fileText(RId id) const {
    return id == ReferenceIndex::kNoId ? llvm::StringRef()
                                       : llvm::StringRef(refs_.textOf(id));
  }

  // ---- closures -------------------------------------------------------------
  void computeClosures(Closures &cs, bool provenOnly,
                       llvm::ArrayRef<uint32_t> roots);
  uint32_t internClosure(Closures &cs, llvm::ArrayRef<uint64_t> bits);

  // ---- edges ----------------------------------------------------------------
  void directEdges();
  void callEdges();
  void addEdge(uint32_t from, uint32_t to, const Witness &w, uint32_t held);
  void addSelf(uint32_t node, const Witness &w);
  [[nodiscard]] bool better(const Witness &a, const Witness &b) const;
  [[nodiscard]] bool holds(uint32_t set, uint32_t lock) const;
  uint32_t gateOf(uint32_t held, uint32_t from);
  uint32_t intersect(uint32_t a, uint32_t b);

  // ---- cycles and reporting -------------------------------------------------
  void findCycles(LockOrderStats &stats, std::vector<Diagnostic> &out);
  void reportCycle(llvm::ArrayRef<uint32_t> cycle, bool groupBounded,
                   std::vector<Diagnostic> &out);
  void reportSelves(LockOrderStats &stats, std::vector<Diagnostic> &out);
  [[nodiscard]] std::string edgeText(uint32_t from, uint32_t to,
                                     const Witness &w);
  [[nodiscard]] std::string siteText(RId file, uint32_t line,
                                     uint32_t col) const;
  [[nodiscard]] std::string quoted(uint32_t node) const {
    return "'" + sets_.display(lockOfNode_[node]) + "'";
  }
  bool witnessPath(uint32_t start, uint32_t node, bool provenOnly,
                   std::vector<uint32_t> &chain, const Acq *&acq);
  void finish(Diagnostic &diag, std::string msg, const Witness &first,
              llvm::ArrayRef<std::string> reasons, bool plausible,
              bool shared, std::vector<std::string> entities);

  const CallGraph &graph_;
  const ReferenceIndex &refs_;
  const LockOrderOptions &opts_;
  CallLockFacts facts_;
  LockSets &sets_;

  std::vector<Acq> acqs_;
  std::vector<std::pair<uint32_t, uint32_t>> acqRange_; // by function
  std::vector<uint32_t> lockOfNode_;                    // node -> lock
  std::vector<uint32_t> nodeOfLock_;                    // lock -> node
  std::vector<uint8_t> recursion_; // node -> kRefRecursive|kRefNonRecursive
  std::vector<uint8_t> memberNode_;
  std::vector<uint8_t> gateLock_; // lock -> may gate (global and named)
  std::vector<uint32_t> holdingCallers_; // function indices, by usr

  Closures all_, proven_;

  std::vector<OrderEdge> edges_;
  llvm::DenseMap<uint64_t, uint32_t> edgeOf_; // (from, to) -> edge
  std::vector<uint32_t> selfOf_;              // node -> self edge or kNone
  std::vector<OrderEdge> selves_;
  llvm::DenseMap<uint64_t, uint32_t> gates_;      // (held, from) -> set
  llvm::DenseMap<uint64_t, uint32_t> intersects_; // (a, b) -> set
  // Member mutexes acquired while one is held: counted, never reported
  // (two objects of a class, or the same one: the identity cannot tell).
  size_t memberSelves_ = 0;

  // Witness path search scratch, by function index.
  std::vector<uint32_t> seenEpoch_;
  std::vector<uint32_t> parent_;
  uint32_t epoch_ = 0;
};

void Analysis::grow() {
  const size_t n = facts_.functionCount();
  if (all_.ofFn.size() >= n)
    return;
  all_.ofFn.resize(n, kNone);
  proven_.ofFn.resize(n, kNone);
  seenEpoch_.resize(n, 0);
  parent_.resize(n, kNone);
}

void Analysis::readFacts(LockOrderStats &stats) {
  facts_.readSymbols(nullptr);
  struct Raw {
    GId fn;
    uint32_t lock;
    bool shared;
    RId locks, file;
    uint32_t line, col;
    uint8_t flags;
  };
  std::vector<Raw> raw;
  std::vector<GId> holding;
  facts_.readReferences([&](const ReferenceIndex::ReferenceRecord &rec,
                            GId referrer) {
    if (isLockRole(rec.role)) {
      ++stats.acquisitions;
      const std::string &identity = refs_.textOf(rec.target);
      if (identity == kLockUnknownIdentity) {
        ++stats.unknownAcquisitions;
        return;
      }
      if (llvm::StringRef(identity).starts_with(kLockOncePrefix))
        return;
      raw.push_back({referrer, sets_.lock(identity, ""),
                     rec.role == RefRole::LockShared, rec.locks, rec.file,
                     rec.line, rec.col, rec.flags});
      return;
    }
    if ((rec.role == RefRole::Call || rec.role == RefRole::AddressOf) &&
        rec.locks != ReferenceIndex::kNoId)
      holding.push_back(referrer);
  });

  // Nodes: the named locks some code waits to acquire, ranked by identity
  // (a lock never acquired that way closes no cycle).
  for (const Raw &r : raw)
    lockOfNode_.push_back(r.lock);
  llvm::sort(lockOfNode_);
  lockOfNode_.erase(std::unique(lockOfNode_.begin(), lockOfNode_.end()),
                    lockOfNode_.end());
  llvm::sort(lockOfNode_, [&](uint32_t a, uint32_t b) {
    return sets_.identity(a) < sets_.identity(b);
  });
  nodeOfLock_.assign(sets_.lockCount(), kNone);
  recursion_.assign(lockOfNode_.size(), 0);
  memberNode_.assign(lockOfNode_.size(), 0);
  for (uint32_t n = 0; n < lockOfNode_.size(); ++n) {
    nodeOfLock_[lockOfNode_[n]] = n;
    memberNode_[n] = isMemberIdentity(sets_.identity(lockOfNode_[n]));
  }
  gateLock_.assign(sets_.lockCount(), 0);
  for (uint32_t l = 0; l < sets_.lockCount(); ++l)
    gateLock_[l] = !sets_.isUnknown(l) && !sets_.isOnce(l) &&
                   !isMemberIdentity(sets_.identity(l));
  stats.locks = lockOfNode_.size();

  acqs_.reserve(raw.size());
  for (const Raw &r : raw) {
    const uint32_t node = nodeOfLock_[r.lock];
    recursion_[node] |= r.flags & (kRefRecursive | kRefNonRecursive);
    acqs_.push_back({facts_.functionIndex(r.fn), node, r.shared, r.locks,
                     r.file, r.line, r.col});
  }
  raw = {};
  llvm::sort(acqs_, [&](const Acq &a, const Acq &b) {
    if (a.fn != b.fn)
      return a.fn < b.fn;
    return std::make_tuple(a.node, a.shared, fileText(a.file), a.line,
                           a.col) <
           std::make_tuple(b.node, b.shared, fileText(b.file), b.line, b.col);
  });
  acqRange_.assign(facts_.functionCount(), {0, 0});
  for (size_t i = 0; i < acqs_.size();) {
    const uint32_t fn = acqs_[i].fn;
    const auto begin = static_cast<uint32_t>(i);
    while (i < acqs_.size() && acqs_[i].fn == fn)
      ++i;
    acqRange_[fn] = {begin, static_cast<uint32_t>(i)};
    ++stats.acquiringFunctions;
  }

  llvm::sort(holding);
  holding.erase(std::unique(holding.begin(), holding.end()), holding.end());
  for (GId gid : holding)
    holdingCallers_.push_back(facts_.functionIndex(gid));
  llvm::sort(holdingCallers_, [&](uint32_t a, uint32_t b) {
    return usrOf(a) < usrOf(b);
  });
  stats.holdingCallers = holdingCallers_.size();
  grow();
}

uint32_t Analysis::internClosure(Closures &cs, llvm::ArrayRef<uint64_t> bits) {
  if (llvm::all_of(bits, [](uint64_t w) { return w == 0; }))
    return 0;
  // Masked: DenseMap reserves the two largest keys.
  const uint64_t hash =
      static_cast<uint64_t>(static_cast<size_t>(
          llvm::hash_combine_range(bits.begin(), bits.end()))) >>
      1;
  auto [it, inserted] = cs.head.try_emplace(hash, kNone);
  for (uint32_t id = it->second; id != kNone; id = cs.next[id])
    if (std::equal(bits.begin(), bits.end(), cs.set(id)))
      return id;
  if (cs.pool.size() + cs.words > opts_.maxClosureWords) {
    cs.bounded = true;
    return 0;
  }
  const auto id = static_cast<uint32_t>(cs.pool.size() / cs.words);
  cs.pool.insert(cs.pool.end(), bits.begin(), bits.end());
  cs.next.push_back(it->second);
  it->second = id;
  return id;
}

// Tarjan's strongly connected components over the call graph reachable
// from the roots (iterative), each component's closure computed when it
// completes: every component it calls has completed before it.
void Analysis::computeClosures(Closures &cs, bool provenOnly,
                               llvm::ArrayRef<uint32_t> roots) {
  cs.words = std::max<size_t>(1, (2 * lockOfNode_.size() + 63) / 64);
  cs.pool.assign(cs.words, 0); // set 0: empty
  cs.next.assign(1, kNone);
  cs.head.clear();
  std::vector<uint32_t> index, low;
  std::vector<uint8_t> onStack;
  std::vector<uint32_t> stack, members;
  struct Frame {
    uint32_t fn;
    uint32_t pos;
  };
  std::vector<Frame> frames;
  std::vector<uint64_t> scratch(cs.words);
  std::vector<uint32_t> seenSet; // closure id -> stamp of the last union
  uint32_t stamp = 0;
  uint32_t counter = 0;
  const auto sized = [&] {
    grow();
    const size_t n = facts_.functionCount();
    if (index.size() < n) {
      index.resize(n, kNone);
      low.resize(n, 0);
      onStack.resize(n, 0);
    }
  };
  const auto visit = [&](uint32_t fn) {
    index[fn] = low[fn] = counter++;
    stack.push_back(fn);
    onStack[fn] = 1;
    frames.push_back({fn, 0});
  };
  sized();
  for (uint32_t root : roots) {
    if (index[root] != kNone)
      continue;
    visit(root);
    while (!frames.empty()) {
      const uint32_t fn = frames.back().fn;
      const llvm::ArrayRef<LockCallEdge> edges = facts_.callees(fn);
      sized();
      if (frames.back().pos < edges.size()) {
        const LockCallEdge &e = edges[frames.back().pos++];
        if (provenOnly && e.plausible)
          continue;
        const uint32_t w = e.callee;
        if (index[w] == kNone)
          visit(w);
        else if (onStack[w])
          low[fn] = std::min(low[fn], index[w]);
        continue;
      }
      frames.pop_back();
      if (!frames.empty())
        low[frames.back().fn] = std::min(low[frames.back().fn], low[fn]);
      if (low[fn] != index[fn])
        continue;
      members.clear();
      uint32_t m;
      do {
        m = stack.back();
        stack.pop_back();
        onStack[m] = 0;
        members.push_back(m);
      } while (m != fn);
      // The component's closure: its own acquisitions and its callees'.
      std::fill(scratch.begin(), scratch.end(), 0);
      ++stamp;
      for (uint32_t member : members) {
        for (const Acq &a : acqsOf(member)) {
          const uint32_t bit = a.node * 2 + (a.shared ? 1 : 0);
          scratch[bit / 64] |= uint64_t(1) << (bit % 64);
        }
        for (const LockCallEdge &e : facts_.callees(member)) {
          if (provenOnly && e.plausible)
            continue;
          const uint32_t c = cs.ofFn[e.callee];
          if (c == kNone || c == 0)
            continue; // the component itself, or nothing to add
          if (seenSet.size() <= c)
            seenSet.resize(c + 1, 0);
          if (seenSet[c] == stamp)
            continue;
          seenSet[c] = stamp;
          const uint64_t *bits = cs.set(c);
          for (size_t i = 0; i < cs.words; ++i)
            scratch[i] |= bits[i];
        }
      }
      const uint32_t id = internClosure(cs, scratch);
      for (uint32_t member : members)
        cs.ofFn[member] = id;
    }
  }
}

bool Analysis::holds(uint32_t set, uint32_t lock) const {
  const llvm::ArrayRef<uint32_t> locks = sets_.locksOf(set);
  return std::binary_search(locks.begin(), locks.end(), lock);
}

uint32_t Analysis::gateOf(uint32_t held, uint32_t from) {
  auto [it, inserted] = gates_.try_emplace(lockPairKey(held, from), 0);
  if (!inserted)
    return it->second;
  llvm::SmallVector<uint32_t, 4> keep;
  const uint32_t fromLock = lockOfNode_[from];
  for (uint32_t l : sets_.locksOf(held))
    if (l != fromLock && l < gateLock_.size() && gateLock_[l])
      keep.push_back(l);
  const uint32_t out = sets_.intern(keep);
  gates_[lockPairKey(held, from)] = out;
  return out;
}

uint32_t Analysis::intersect(uint32_t a, uint32_t b) {
  if (a == b)
    return a;
  if (a == 0 || b == 0)
    return 0;
  if (a > b)
    std::swap(a, b);
  auto found = intersects_.find(lockPairKey(a, b));
  if (found != intersects_.end())
    return found->second;
  llvm::SmallVector<uint32_t, 4> common;
  const llvm::ArrayRef<uint32_t> x = sets_.locksOf(a), y = sets_.locksOf(b);
  std::set_intersection(x.begin(), x.end(), y.begin(), y.end(),
                        std::back_inserter(common));
  const uint32_t out = sets_.intern(common);
  intersects_[lockPairKey(a, b)] = out;
  return out;
}

// The witness shown for an edge: proven before plausible, exclusive before
// shared, an acquisition in the holder itself before one through a call,
// then by holder usr and site.
bool Analysis::better(const Witness &a, const Witness &b) const {
  const auto key = [&](const Witness &w) {
    return std::make_tuple(w.plausible, w.shared, w.callee != kNone);
  };
  if (key(a) != key(b))
    return key(a) < key(b);
  if (a.holder != b.holder)
    return usrOf(a.holder) < usrOf(b.holder);
  const auto site = [&](const Witness &w) {
    return std::make_tuple(fileText(w.file), w.line, w.col);
  };
  if (site(a) != site(b))
    return site(a) < site(b);
  if (a.callee == b.callee)
    return false;
  if (a.callee == kNone || b.callee == kNone)
    return a.callee == kNone;
  return usrOf(a.callee) < usrOf(b.callee);
}

void Analysis::addEdge(uint32_t from, uint32_t to, const Witness &w,
                       uint32_t held) {
  const uint32_t gate = gateOf(held, from);
  auto [it, inserted] = edgeOf_.try_emplace(
      lockPairKey(from, to), static_cast<uint32_t>(edges_.size()));
  if (inserted) {
    edges_.push_back({from, to, w, gate});
    return;
  }
  OrderEdge &edge = edges_[it->second];
  edge.gate = intersect(edge.gate, gate);
  if (better(w, edge.witness))
    edge.witness = w;
}

void Analysis::addSelf(uint32_t node, const Witness &w) {
  if (selfOf_[node] == kNone) {
    selfOf_[node] = static_cast<uint32_t>(selves_.size());
    selves_.push_back({node, node, w, 0});
    return;
  }
  OrderEdge &self = selves_[selfOf_[node]];
  if (better(w, self.witness))
    self.witness = w;
}

// Every acquisition made while named locks are held, in the function
// itself.
void Analysis::directEdges() {
  for (const Acq &a : acqs_) {
    if (a.locks == ReferenceIndex::kNoId)
      continue;
    const uint32_t lock = lockOfNode_[a.node];
    for (const LockEntry &entry : facts_.lockEntries(a.locks)) {
      if (entry.set == 0)
        continue;
      Witness w;
      w.holder = a.fn;
      w.file = a.file;
      w.line = entry.line;
      w.col = entry.col;
      w.shared = a.shared;
      if (holds(entry.set, lock)) {
        // Acquired again while held: a self-deadlock unless recursive.
        // Every object's member mutex has one identity: two objects'.
        if (!memberNode_[a.node])
          addSelf(a.node, w);
        else
          ++memberSelves_;
        continue;
      }
      for (uint32_t l : sets_.locksOf(entry.set)) {
        const uint32_t from = nodeOf(l);
        if (from != kNone)
          addEdge(from, a.node, w, entry.set);
      }
    }
  }
}

// Every call made while named locks are held, with each lock its callee
// can acquire.
void Analysis::callEdges() {
  llvm::SmallVector<LockHop, 4> hops;
  std::vector<LockCallEdge> calls;
  for (uint32_t f : holdingCallers_) {
    const llvm::ArrayRef<LockCallEdge> view = facts_.callees(f);
    calls.assign(view.begin(), view.end());
    grow();
    for (const LockCallEdge &e : calls) {
      const uint32_t c = all_.of(e.callee);
      if (c == 0)
        continue;
      const uint32_t p = e.plausible ? 0 : proven_.of(e.callee);
      facts_.hops(f, e, hops);
      for (const LockHop &hop : hops) {
        bool named = false;
        for (uint32_t l : sets_.locksOf(hop.set))
          named |= nodeOf(l) != kNone;
        if (!named)
          continue;
        const uint64_t *bits = all_.set(c);
        for (size_t word = 0; word < all_.words; ++word) {
          for (uint64_t rest = bits[word]; rest != 0; rest &= rest - 1) {
            const auto bit =
                static_cast<uint32_t>(word * 64 + llvm::countr_zero(rest));
            const uint32_t to = bit / 2;
            // An exclusive and a shared acquisition of one lock: one pass.
            if ((bit & 1u) != 0 && all_.has(c, bit - 1))
              continue;
            Witness w;
            w.holder = f;
            w.callee = e.callee;
            w.file = hop.file;
            w.line = hop.line;
            w.col = hop.col;
            const bool isProven =
                p != 0 && (proven_.has(p, to * 2) || proven_.has(p, to * 2 + 1));
            w.plausible = !isProven;
            w.shared = isProven ? !proven_.has(p, to * 2)
                                : !all_.has(c, to * 2);
            if (holds(hop.set, lockOfNode_[to])) {
              if (!memberNode_[to])
                addSelf(to, w);
              else
                ++memberSelves_;
              continue;
            }
            for (uint32_t l : sets_.locksOf(hop.set)) {
              const uint32_t from = nodeOf(l);
              if (from != kNone)
                addEdge(from, to, w, hop.set);
            }
          }
        }
      }
    }
  }
}

// A path from `start` down to a function that acquires `node`, through
// functions whose closure holds it: breadth first, so the shortest, in
// the canonical callee order.
bool Analysis::witnessPath(uint32_t start, uint32_t node, bool provenOnly,
                           std::vector<uint32_t> &chain, const Acq *&acq) {
  const Closures &cs = provenOnly ? proven_ : all_;
  const auto reaches = [&](uint32_t fn) {
    const uint32_t c = cs.of(fn);
    return c != 0 && (cs.has(c, node * 2) || cs.has(c, node * 2 + 1));
  };
  chain.clear();
  acq = nullptr;
  if (++epoch_ == 0) {
    std::fill(seenEpoch_.begin(), seenEpoch_.end(), 0);
    epoch_ = 1;
  }
  grow();
  std::vector<uint32_t> queue{start};
  seenEpoch_[start] = epoch_;
  parent_[start] = kNone;
  size_t visits = 0;
  for (size_t qi = 0; qi < queue.size(); ++qi) {
    const uint32_t fn = queue[qi];
    for (const Acq &a : acqsOf(fn))
      if (a.node == node && (acq == nullptr || (acq->shared && !a.shared)))
        acq = &a;
    if (acq) {
      for (uint32_t at = fn; at != kNone; at = parent_[at])
        chain.push_back(at);
      std::reverse(chain.begin(), chain.end());
      return true;
    }
    if (++visits > opts_.maxWitnessVisits)
      return false;
    const llvm::ArrayRef<LockCallEdge> edges = facts_.callees(fn);
    grow();
    for (const LockCallEdge &e : edges) {
      if (provenOnly && e.plausible)
        continue;
      const uint32_t w = e.callee;
      if (seenEpoch_[w] == epoch_ || !reaches(w))
        continue;
      seenEpoch_[w] = epoch_;
      parent_[w] = fn;
      queue.push_back(w);
    }
  }
  return false;
}

std::string Analysis::siteText(RId file, uint32_t line, uint32_t col) const {
  std::string out = fileText(file).str();
  if (line != 0)
    out += ":" + std::to_string(line) + ":" + std::to_string(col);
  return out;
}

std::string Analysis::edgeText(uint32_t from, uint32_t to, const Witness &w) {
  const std::string &holder = facts_.functionName(w.holder);
  const std::string verb = w.shared ? "acquires (shared) " : "acquires ";
  if (w.callee == kNone)
    return holder + " " + verb + quoted(to) + " at " +
           siteText(w.file, w.line, w.col) + " while holding " +
           quoted(from);
  std::vector<uint32_t> chain;
  const Acq *acq = nullptr;
  std::string out = holder + " calls ";
  if (witnessPath(w.callee, to, !w.plausible, chain, acq)) {
    constexpr size_t kHead = 2, kTail = 2;
    for (size_t i = 0; i < chain.size(); ++i) {
      if (chain.size() > kHead + kTail + 1 && i == kHead) {
        out += " -> ...";
        i = chain.size() - kTail - 1;
        continue;
      }
      if (i)
        out += " -> ";
      out += facts_.functionName(chain[i]);
    }
    out += " at " + siteText(w.file, w.line, w.col) + " while holding " +
           quoted(from) + ", and " + facts_.functionName(chain.back()) + " " +
           (acq->shared ? "acquires (shared) " : "acquires ") + quoted(to) +
           " at " + siteText(acq->file, acq->line, acq->col);
    return out;
  }
  return out + facts_.functionName(w.callee) + " at " +
         siteText(w.file, w.line, w.col) + " while holding " + quoted(from) +
         ", and " + quoted(to) +
         " is acquired beneath it (the path search stopped at its bound)";
}

void Analysis::finish(Diagnostic &diag, std::string msg, const Witness &first,
                      llvm::ArrayRef<std::string> reasons, bool plausible,
                      bool shared, std::vector<std::string> entities) {
  diag.checkName = kCheck;
  if (plausible)
    diag.kind = Diagnostic::LockOrder_Plausible;
  else if (shared)
    diag.kind = Diagnostic::LockOrder_Shared;
  else if (!reasons.empty())
    diag.kind = Diagnostic::LockOrder_Unproven;
  else
    diag.kind = Diagnostic::LockOrder_Inversion;
  std::vector<std::string> why(reasons.begin(), reasons.end());
  if (plausible)
    why.push_back("a witness takes a plausible call edge (virtual dispatch "
                  "or a function pointer)");
  if (shared)
    why.push_back("a witness acquires its lock shared, which blocks only "
                  "against an exclusive holder");
  if (!why.empty()) {
    msg += " Not proven: ";
    for (size_t i = 0; i < why.size(); ++i) {
      if (i)
        msg += "; ";
      msg += why[i];
    }
    msg += ".";
  }
  diag.file = fileText(first.file).str();
  diag.callLocation = siteText(first.file, first.line, first.col);
  diag.message = std::move(msg);
  diag.entities = std::move(entities);
}

void Analysis::reportCycle(llvm::ArrayRef<uint32_t> cycle, bool groupBounded,
                           std::vector<Diagnostic> &out) {
  std::string chain;
  for (uint32_t n : cycle)
    chain += quoted(n) + " -> ";
  chain += quoted(cycle.front());
  std::string msg = "Locks acquired in opposite orders: " + chain + ".";
  bool plausible = false, shared = false;
  std::vector<std::string> entities;
  const Witness *first = nullptr;
  for (size_t i = 0; i < cycle.size(); ++i) {
    const uint32_t from = cycle[i], to = cycle[(i + 1) % cycle.size()];
    const OrderEdge &edge = edges_[edgeOf_.find(lockPairKey(from, to))->second];
    if (!first)
      first = &edge.witness;
    plausible |= edge.witness.plausible;
    shared |= edge.witness.shared;
    msg += " " + quoted(from) + " then " + quoted(to) + ": " +
           edgeText(from, to, edge.witness) + ".";
    entities.push_back(sets_.identity(lockOfNode_[from]));
  }
  msg += " Threads taking these paths at the same time can deadlock.";
  if (groupBounded)
    msg += " (Other cycles among these locks were not enumerated: bound.)";
  std::vector<std::string> reasons;
  if (!opts_.coverageGap.empty())
    reasons.push_back(opts_.coverageGap);
  if (all_.bounded || proven_.bounded)
    reasons.push_back("the acquisition closure stopped at its bound");
  Diagnostic diag;
  finish(diag, std::move(msg), *first, reasons, plausible, shared,
         std::move(entities));
  out.push_back(std::move(diag));
}

void Analysis::reportSelves(LockOrderStats &stats,
                            std::vector<Diagnostic> &out) {
  for (uint32_t node = 0; node < selfOf_.size(); ++node) {
    if (selfOf_[node] == kNone)
      continue;
    if ((recursion_[node] & kRefRecursive) != 0)
      continue; // a recursive mutex may be locked again by its holder
    const OrderEdge &self = selves_[selfOf_[node]];
    std::string msg = quoted(node) + " is acquired while already held: " +
                      edgeText(node, node, self.witness) +
                      ". A mutex that is not recursive deadlocks on itself "
                      "here.";
    std::vector<std::string> reasons;
    if (!opts_.coverageGap.empty())
      reasons.push_back(opts_.coverageGap);
    if (all_.bounded || proven_.bounded)
      reasons.push_back("the acquisition closure stopped at its bound");
    if ((recursion_[node] & kRefNonRecursive) == 0)
      reasons.push_back("whether " + quoted(node) +
                        " is recursive is not known (a pthread mutex takes "
                        "its kind at run time)");
    Diagnostic diag;
    finish(diag, std::move(msg), self.witness, reasons,
           self.witness.plausible, self.witness.shared,
           {sets_.identity(lockOfNode_[node])});
    out.push_back(std::move(diag));
    ++stats.selfDeadlocks;
  }
}

// Tarjan's components of the lock graph; in each of two or more locks,
// every elementary cycle up to the length bound, each enumerated once from
// its lowest-ranked lock through higher-ranked ones only (a bounded
// depth-first enumeration).
void Analysis::findCycles(LockOrderStats &stats,
                          std::vector<Diagnostic> &out) {
  const auto n = static_cast<uint32_t>(lockOfNode_.size());
  // Adjacency (CSR) by rank.
  std::vector<uint32_t> order(edges_.size());
  for (uint32_t i = 0; i < order.size(); ++i)
    order[i] = i;
  llvm::sort(order, [&](uint32_t a, uint32_t b) {
    return std::tie(edges_[a].from, edges_[a].to) <
           std::tie(edges_[b].from, edges_[b].to);
  });
  std::vector<uint32_t> begin(n + 1, 0), adj;
  adj.reserve(order.size());
  for (uint32_t e : order) {
    ++begin[edges_[e].from + 1];
    adj.push_back(edges_[e].to);
  }
  for (uint32_t i = 0; i < n; ++i)
    begin[i + 1] += begin[i];

  // Components.
  std::vector<uint32_t> index(n, kNone), low(n, 0), comp(n, kNone);
  std::vector<uint8_t> onStack(n, 0);
  std::vector<uint32_t> stack;
  std::vector<std::pair<uint32_t, uint32_t>> frames;
  uint32_t counter = 0, comps = 0;
  for (uint32_t root = 0; root < n; ++root) {
    if (index[root] != kNone)
      continue;
    index[root] = low[root] = counter++;
    stack.push_back(root);
    onStack[root] = 1;
    frames.push_back({root, begin[root]});
    while (!frames.empty()) {
      auto &[v, pos] = frames.back();
      if (pos < begin[v + 1]) {
        const uint32_t w = adj[pos++];
        if (index[w] == kNone) {
          index[w] = low[w] = counter++;
          stack.push_back(w);
          onStack[w] = 1;
          frames.push_back({w, begin[w]});
        } else if (onStack[w]) {
          low[v] = std::min(low[v], index[w]);
        }
        continue;
      }
      const uint32_t done = v;
      frames.pop_back();
      if (!frames.empty())
        low[frames.back().first] =
            std::min(low[frames.back().first], low[done]);
      if (low[done] != index[done])
        continue;
      uint32_t m;
      do {
        m = stack.back();
        stack.pop_back();
        onStack[m] = 0;
        comp[m] = comps;
      } while (m != done);
      ++comps;
    }
  }
  std::vector<uint32_t> compSize(comps, 0);
  for (uint32_t v = 0; v < n; ++v)
    ++compSize[comp[v]];

  std::vector<uint8_t> onPath(n, 0);
  std::vector<std::pair<uint32_t, uint32_t>> path; // (node, next adj pos)
  std::vector<uint32_t> cycle;
  std::vector<std::vector<uint32_t>> found;
  std::vector<uint8_t> groupBounded(comps, 0), groupSeen(comps, 0);
  std::vector<size_t> groupSteps(comps, 0);
  std::vector<uint32_t> groupCycles(comps, 0);
  size_t total = 0;
  for (uint32_t s = 0; s < n; ++s) {
    const uint32_t g = comp[s];
    if (compSize[g] < 2)
      continue;
    if (!groupSeen[g]) {
      groupSeen[g] = 1;
      ++stats.lockGroups;
    }
    if (groupBounded[g])
      continue;
    path.clear();
    path.push_back({s, begin[s]});
    onPath[s] = 1;
    while (!path.empty()) {
      auto &[v, pos] = path.back();
      if (pos == begin[v + 1] || groupBounded[g]) {
        onPath[v] = 0;
        path.pop_back();
        continue;
      }
      const uint32_t w = adj[pos++];
      if (++groupSteps[g] > opts_.maxEnumerationSteps) {
        groupBounded[g] = 1;
        continue;
      }
      if (comp[w] != g || w < s)
        continue;
      if (w == s) {
        if (path.size() < 2)
          continue;
        cycle.clear();
        for (const auto &step : path)
          cycle.push_back(step.first);
        ++stats.cycles;
        // A lock held on every edge orders the cycle.
        uint32_t gate = kNone;
        for (size_t i = 0; i < cycle.size(); ++i) {
          const OrderEdge &edge = edges_[edgeOf_
                                             .find(lockPairKey(
                                                 cycle[i],
                                                 cycle[(i + 1) % cycle.size()]))
                                             ->second];
          gate = gate == kNone ? edge.gate : intersect(gate, edge.gate);
        }
        if (gate != 0) {
          ++stats.gatedCycles;
          continue;
        }
        found.push_back(cycle);
        ++total;
        if (++groupCycles[g] >= opts_.maxCyclesPerGroup ||
            total >= opts_.maxCycles)
          groupBounded[g] = 1;
        continue;
      }
      if (onPath[w] || path.size() >= opts_.maxCycleLength)
        continue;
      onPath[w] = 1;
      path.push_back({w, begin[w]});
    }
    if (total >= opts_.maxCycles)
      break;
  }
  for (uint32_t g = 0; g < comps; ++g)
    stats.boundedGroups += groupBounded[g];
  for (const auto &c : found)
    reportCycle(c, groupBounded[comp[c.front()]] != 0, out);
}

std::vector<Diagnostic> Analysis::run(LockOrderStats &stats) {
  readFacts(stats);
  std::vector<Diagnostic> out;
  if (lockOfNode_.empty())
    return out;
  computeClosures(all_, false, holdingCallers_);
  computeClosures(proven_, true, holdingCallers_);
  stats.closureBounded = all_.bounded || proven_.bounded;
  for (uint32_t c : all_.ofFn)
    stats.closureFunctions += c != kNone ? 1 : 0;
  stats.distinctClosures = all_.pool.size() / all_.words - 1;

  selfOf_.assign(lockOfNode_.size(), kNone);
  directEdges();
  callEdges();
  stats.orderEdges = edges_.size();
  stats.memberSelfEdges = memberSelves_;

  findCycles(stats, out);
  reportSelves(stats, out);
  llvm::sort(out, [](const Diagnostic &a, const Diagnostic &b) {
    return std::tie(a.file, a.callLocation, a.message) <
           std::tie(b.file, b.callLocation, b.message);
  });
  return out;
}

} // namespace

std::vector<Diagnostic> findLockOrderInversions(const CallGraph &graph,
                                                const ReferenceIndex &refs,
                                                const LockOrderOptions &opts,
                                                LockOrderStats *stats) {
  LockOrderStats local;
  return Analysis(graph, refs, opts).run(stats ? *stats : local);
}

} // namespace vycor
