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

#include "vycor/callgraph/CallGraph.h"
#include "vycor/callgraph/ReferenceIndex.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace vycor {

// ============================================================================
// The lock-set join the lock checks share (unsynchronized-shared-global,
// lock-order-inversion): one reading of the reference facts' lock texts
// (ReferenceIndex "Lock facts") and one rule for which locks a call adds,
// so both checks agree on what "the same lock" is and on what a call
// holds.
//
//   LockSets       interned lock identities and sorted sets of them
//   CallLockFacts  the call graph's callees of a function in a canonical
//                  order, each with the lock sets its call was made under
// ============================================================================

// Interned sorted sets of interned lock identities. Set 0 is the empty set.
// Unions are memoized; nothing here allocates per search step once a set
// has been seen.
class LockSets {
public:
  LockSets();

  // The lock with this identity (a USR, kLockUnknownIdentity, or a
  // kLockOncePrefix identity). An empty display is replaced by the first
  // non-empty one given for the identity.
  uint32_t lock(llvm::StringRef identity, llvm::StringRef display);
  // The lock with this identity, if any code holds or acquires it.
  [[nodiscard]] std::optional<uint32_t> find(llvm::StringRef identity) const;

  uint32_t intern(llvm::SmallVectorImpl<uint32_t> &locks);
  uint32_t unite(uint32_t a, uint32_t b);

  [[nodiscard]] bool hasOnce(uint32_t set) const {
    return (flags_[set] & kOnce) != 0;
  }
  [[nodiscard]] bool hasUnknown(uint32_t set) const {
    return (flags_[set] & kUnknown) != 0;
  }

  // No lock can be common to the two sets. A lock the bake could not name
  // may be any mutex: two non-empty sets with one on either side are not
  // known to be disjoint.
  [[nodiscard]] bool disjoint(uint32_t a, uint32_t b) const;

  // "{a, b}" by display name, "no lock" for the empty set.
  [[nodiscard]] std::string describe(uint32_t set) const;

  [[nodiscard]] bool isLockIdentity(llvm::StringRef identity) const {
    return byIdentity_.count(identity) != 0;
  }

  // The set holding only a lock the bake could not name.
  uint32_t unknownSet();

  // A canonical order of sets by content (their lock identities), which
  // does not depend on the order sets were first met.
  [[nodiscard]] bool less(uint32_t a, uint32_t b);

  // ---- locks ----------------------------------------------------------------
  [[nodiscard]] size_t lockCount() const noexcept { return locks_.size(); }
  [[nodiscard]] const std::string &identity(uint32_t lock) const {
    return locks_[lock].identity;
  }
  // The display name, or the identity when no lock text gave one.
  [[nodiscard]] const std::string &display(uint32_t lock) const {
    return locks_[lock].display.empty() ? locks_[lock].identity
                                        : locks_[lock].display;
  }
  [[nodiscard]] bool isUnknown(uint32_t lock) const {
    return locks_[lock].unknown;
  }
  [[nodiscard]] bool isOnce(uint32_t lock) const { return locks_[lock].once; }
  [[nodiscard]] llvm::ArrayRef<uint32_t> locksOf(uint32_t set) const {
    return sets_[set];
  }

private:
  // A set's locks ordered by identity, computed once per set.
  void orderByIdentity(uint32_t set);

  struct Lock {
    std::string identity;
    std::string display;
    bool unknown;
    bool once;
  };
  static constexpr uint8_t kOnce = 1, kUnknown = 2;
  std::vector<Lock> locks_;
  llvm::StringMap<uint32_t> byIdentity_;
  std::vector<std::vector<uint32_t>> sets_;
  std::vector<uint8_t> flags_;
  std::map<std::vector<uint32_t>, uint32_t> byContent_;
  llvm::DenseMap<uint64_t, uint32_t> unions_;
  uint32_t unknownSet_ = UINT32_MAX;
  std::vector<llvm::SmallVector<uint32_t, 2>> ordered_;
  std::vector<uint8_t> haveOrder_;
};

// One line of a reference's lock text: where, and under which set.
struct LockEntry {
  uint32_t line;
  uint32_t col;
  uint32_t set;
};

// A callee of a function as the join sees it.
struct LockCallEdge {
  uint32_t callee;              // function index (CallLockFacts)
  StringInterner::Id calleeId;  // call-graph id
  StringInterner::Id lockTarget; // callee whose call records carry the locks
  StringInterner::Id callSite;  // call-graph id of "file:line:col"
  EdgeKind kind;
  bool plausible;
  bool lambda; // the callee is a lambda
  // A callable handed to a function of the program's own at this call
  // (withLock(f)): it runs where that function calls it, under locks the
  // call site does not show.
  bool handedOn;
};

// A lock set a call is made under, and the first occurrence under it (the
// call record's file; line 0 when no record gives one).
struct LockHop {
  uint32_t set;
  ReferenceIndex::Id file;
  uint32_t line;
  uint32_t col;
};

class CallLockFacts {
public:
  using GId = StringInterner::Id;
  using RId = ReferenceIndex::Id;
  static constexpr uint32_t kNone = UINT32_MAX;

  CallLockFacts(const CallGraph &graph, const ReferenceIndex &refs);

  // ---- reading the facts (once, in this order) ------------------------------
  // Symbol records: the flags of every function by graph id, and which
  // USRs are variables. `extra` sees every record after that.
  void readSymbols(
      llvm::function_ref<void(const ReferenceIndex::SymbolRecord &)> extra);
  // Reference records except vtable and typeinfo uses: every lock text is
  // interned (so a lock any code holds is known before a search), and the
  // lock texts of calls and address-takings of functions are kept for
  // hops(). `extra` then sees the record with its referrer's graph id
  // (kNone for a variable initializer or file scope).
  void readReferences(
      llvm::function_ref<void(const ReferenceIndex::ReferenceRecord &, GId)>
          extra);

  // ---- lookups --------------------------------------------------------------
  [[nodiscard]] LockSets &sets() noexcept { return sets_; }
  [[nodiscard]] const LockSets &sets() const noexcept { return sets_; }
  [[nodiscard]] GId graphId(RId id);
  // Symbol flags of a function (OR over its records); 0 when none.
  [[nodiscard]] uint32_t functionFlags(GId id) const;
  [[nodiscard]] bool isVariable(RId usr) const {
    return variables_.count(usr) != 0;
  }
  [[nodiscard]] llvm::ArrayRef<LockEntry> lockEntries(RId text);

  // ---- functions ------------------------------------------------------------
  // Dense function indices in first-use order.
  [[nodiscard]] uint32_t functionIndex(GId id);
  [[nodiscard]] GId functionId(uint32_t fn) const { return functions_[fn].id; }
  [[nodiscard]] size_t functionCount() const noexcept {
    return functions_.size();
  }
  [[nodiscard]] const std::string &functionName(uint32_t fn) const {
    return graph_.displayNameOf(functions_[fn].id);
  }

  // The callees of a function, in a canonical order (callee usr, call
  // site, kind): thread spawns (std::thread, std::async, a packaged task)
  // start a context of their own and are not callees; std::invoke and
  // std::bind are. Built on first use. The returned view is invalidated by
  // the next callees() of a function not built yet.
  [[nodiscard]] llvm::ArrayRef<LockCallEdge> callees(uint32_t fn);

  // The lock sets a call adds: one per distinct set its occurrences in the
  // caller were made under, in a canonical order. A call with no call
  // record takes the sets where the callee's address was taken (a call
  // through a pointer, std::invoke, an algorithm); a lambda with neither
  // was called holding no lock; any other edge with no record, and a
  // callable handed on, adds the unknown set.
  void hopSets(uint32_t caller, const LockCallEdge &edge,
               llvm::SmallVectorImpl<uint32_t> &out);
  // The same with the first occurrence under each set.
  void hops(uint32_t caller, const LockCallEdge &edge,
            llvm::SmallVectorImpl<LockHop> &out);

private:
  struct Function {
    GId id;
    uint32_t edgeBegin = 0, edgeEnd = 0;
    bool edgesBuilt = false;
  };
  struct LockText {
    RId text; // kNoId: no lock held
    RId file;
  };

  void buildEdges(uint32_t fn);
  [[nodiscard]] bool isUserReceiver(const CallGraph::EdgeRef &call) const;
  [[nodiscard]] const llvm::SmallVector<LockText, 1> *
  lockTextsOf(uint32_t caller, const LockCallEdge &edge) const;

  const CallGraph &graph_;
  const ReferenceIndex &refs_;
  LockSets sets_;
  std::optional<GId> indirect_, indirectSynth_;

  llvm::DenseMap<GId, uint32_t> functionFlags_;
  llvm::DenseSet<RId> variables_;
  llvm::DenseMap<RId, GId> graphIdOf_;
  llvm::DenseMap<RId, std::pair<uint32_t, uint32_t>> lockEntriesOf_;
  std::vector<LockEntry> lockEntryPool_;
  // (caller, callee) graph ids -> lock texts of the call (kNoId: none held).
  llvm::DenseMap<uint64_t, llvm::SmallVector<LockText, 1>> callLocks_;
  llvm::DenseMap<uint64_t, llvm::SmallVector<LockText, 1>> addressLocks_;

  std::vector<Function> functions_;
  llvm::DenseMap<GId, uint32_t> functionOf_;
  std::vector<LockCallEdge> edges_;
  std::vector<CallGraph::EdgeRef> scratch_;
};

[[nodiscard]] inline uint64_t lockPairKey(uint32_t a, uint32_t b) noexcept {
  return (static_cast<uint64_t>(a) << 32) | b;
}

} // namespace vycor
