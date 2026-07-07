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

#include "vycor/callgraph/BuildStats.h"
#include "vycor/callgraph/CallGraph.h"
#include "vycor/callgraph/StringInterner.h"

#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace clang {
namespace tooling {
class CompilationDatabase;
} // namespace tooling
} // namespace clang

namespace vycor {

// ============================================================================
// RAII scope context
// ============================================================================

enum class RaiiKind : uint8_t {
  Lock,     // std::lock_guard, absl::MutexLock, or user-declared lock type
  SmartPtr, // std::unique_ptr, std::shared_ptr
  Other,    // Any other non-trivial-destructor local (file handles, guards)
};

// One RAII-capable local variable live at a call site. These are stored
// per CallSiteContext; innermost scopes come last in liveRaiiLocals.
struct RaiiLocal {
  std::string typeName;    // Canonical qualified type name
  std::string varName;     // Source variable name (may be "")
  std::string declLocation; // file:line:col of the VarDecl
  RaiiKind kind = RaiiKind::Other;
};

// User/CLI-configurable lock type recognition. useBuiltins enables the
// standard allowlist (std::lock_guard/unique_lock/shared_lock/scoped_lock,
// absl::MutexLock/ReaderMutexLock/WriterMutexLock). userAllowlist adds
// extra qualified type names matched exactly (e.g. "RBX::Arbiter").
struct LockTypeConfig {
  std::vector<std::string> userAllowlist;
  bool useBuiltins = true;
};

// ============================================================================
// Exception handling context
// ============================================================================

struct CatchHandlerInfo {
  std::string caughtType; // Qualified type name, or "" for catch(...)
  bool isCatchAll = false;
  std::string location; // file:line:col of the catch keyword
  // First min(160 chars, 3 lines) of the handler body source text. Empty
  // when the body is macro-expanded or the source range is invalid.
  std::string bodySummary;
};

struct TryCatchScope {
  std::string tryLocation;       // file:line:col of the try keyword
  std::string enclosingFunction; // Qualified name of containing function
  std::vector<CatchHandlerInfo> handlers;
  unsigned nestingDepth = 0; // 0 = outermost try in function
};

enum class NoexceptSpec {
  None,          // No noexcept specifier
  Noexcept,      // noexcept or noexcept(true)
  NoexceptFalse, // noexcept(false)
  ThrowNone,     // throw() (C++98 dynamic exception spec)
  Unknown        // Dependent or unresolved
};

// ============================================================================
// Conditional guard context
// ============================================================================

struct ConditionalGuard {
  std::string conditionText; // Source text of the condition
  std::string location;      // file:line:col of the if/assert
  bool inTrueBranch = true;  // true = if-branch, false = else-branch
  bool isAssertion = false;  // assert(), DCHECK(), etc.
};

// ============================================================================
// Per-call-site record
// ============================================================================

struct CallSiteContext {
  std::string callerName; // Matches CallGraphEdge::callerName
  std::string calleeName; // What's being called
  std::string callSite;   // file:line:col (join key with CallGraphEdge)

  // TU that produced this context (the path passed to indexTUControlFlow /
  // buildControlFlowIndex). Used by removeTU: callSite alone is unreliable
  // because its file component is the compile-command spelling (often
  // relative), not the TU path the caller knows.
  std::string tuPath;

  // Try/catch scopes enclosing this call, innermost first.
  std::vector<TryCatchScope> enclosingTryCatches;

  // Conditional guards enclosing this call, innermost first.
  std::vector<ConditionalGuard> enclosingGuards;

  // Noexcept spec of the caller function.
  NoexceptSpec callerNoexcept = NoexceptSpec::None;

  // Whether inside a catch block body (re-throw context).
  bool insideCatchBlock = false;

  // RAII-capable locals live at this call site. Outer scopes come first;
  // innermost-declared locals come last. A local is considered "live" when
  // its declaration is lexically before the call site inside a still-open
  // CompoundStmt scope.
  std::vector<RaiiLocal> liveRaiiLocals;
};

// ============================================================================
// ControlFlowIndex — parallel index alongside CallGraph
// ============================================================================

class ControlFlowIndex {
public:
  ControlFlowIndex();
  ControlFlowIndex(ControlFlowIndex &&other) noexcept;
  ControlFlowIndex &operator=(ControlFlowIndex &&other) noexcept;
  ControlFlowIndex(const ControlFlowIndex &) = delete;
  ControlFlowIndex &operator=(const ControlFlowIndex &) = delete;

  void addCallSiteContext(CallSiteContext ctx);

  // Pre-size the site/caller/callee maps for a known bulk insert (snapshot
  // load): avoids repeated rehashing of multi-million-entry tables.
  void reserveContexts(size_t n);

  // Look up context at a specific call site (file:line:col). Contexts are
  // stored in a deduplicated internal form; queries materialize
  // CallSiteContext values on the fly.
  std::optional<CallSiteContext> contextAtSite(const std::string &callSite) const;

  // All contexts where calleeName is the target.
  std::vector<CallSiteContext>
  contextsForCallee(const std::string &calleeName) const;

  // All contexts where callerName is the source.
  std::vector<CallSiteContext>
  contextsForCaller(const std::string &callerName) const;

  // All call sites targeting calleeName that are inside a try/catch.
  std::vector<CallSiteContext>
  protectedCallsTo(const std::string &calleeName) const;

  // All call sites targeting calleeName that are NOT inside a try/catch.
  std::vector<CallSiteContext>
  unprotectedCallsTo(const std::string &calleeName) const;

  size_t size() const { return liveCount_; }

  // All stored contexts (for dump mode).
  std::vector<CallSiteContext> allContexts() const;

  // Remove all contexts contributed by the given TU. Matches on the
  // recorded tuPath; contexts without one (hand-built in tests, or from
  // pre-provenance snapshots) fall back to a callSite prefix match.
  // Returns the number of contexts removed.
  size_t removeTU(const std::string &tuPath);

  // Compact the contexts vector, eliminating tombstones. The shared set
  // tables are index-stable and left untouched.
  void compact();

  const StringInterner &interner() const { return interner_; }

private:
  friend class SnapshotIO;

  using SId = StringInterner::Id;

  // "no tuPath recorded" sentinel (the interner never assigns UINT32_MAX).
  static constexpr SId kNoString = UINT32_MAX;

  // Interned form of RaiiLocal: three ids into interner_ plus the kind.
  struct StoredRaiiLocal {
    SId typeName;
    SId varName;
    SId declLocation;
    RaiiKind kind;
  };

  // Deduplicated per-context record. The public CallSiteContext costs
  // ~660 B/context on large testbeds (string/vector headers + heap blocks);
  // this form is a few dozen bytes, with the string payload interned and
  // the scope/guard/RAII vectors shared through the set tables below.
  struct StoredContext {
    SId caller;
    SId callee;
    SId site;
    SId tuPath;          // kNoString when the context has no provenance
    uint32_t scopeSet;   // index into scopeSets_
    uint32_t guardSet;   // index into guardSets_
    uint32_t raiiSet;    // index into raiiSets_
    NoexceptSpec callerNoexcept;
    bool insideCatchBlock;
    bool live;           // false = tombstoned by removeTU
  };

  // Materialize the public value form of a stored context (resolve ids,
  // copy the shared set-table vectors).
  CallSiteContext materialize(const StoredContext &se) const;

  // Set-table dedup: convert an incoming vector to its table index, reusing
  // an existing entry when an identical set was seen before. Callers must
  // hold mutex_. An empty set always maps to index 0 (seeded at
  // construction), so "no try/catch" is exactly `scopeSet == 0`.
  // Set-table dedup. Callers hold mutex_ and pass the precomputed
  // canonical key (built OUTSIDE the lock — key construction is the
  // expensive part of an insert). Empty sets return index 0 (the seeded
  // empty entry) without touching the maps.
  static std::string scopeSetKey(const std::vector<TryCatchScope> &scopes);
  static std::string guardSetKey(const std::vector<ConditionalGuard> &guards);
  static std::string raiiSetKey(const std::vector<StoredRaiiLocal> &locals);
  uint32_t internScopeSet(std::string key, std::vector<TryCatchScope> scopes);
  uint32_t internGuardSet(std::string key,
                          std::vector<ConditionalGuard> guards);
  uint32_t internRaiiSet(std::string key,
                         std::vector<StoredRaiiLocal> locals);

  // Core insert shared by addCallSiteContext and snapshot load; callers
  // must hold mutex_ and pass already-interned ids / set indices.
  void insertStored(SId caller, SId callee, SId site, SId tuPath,
                    uint32_t scopeSet, uint32_t guardSet, uint32_t raiiSet,
                    NoexceptSpec callerNoexcept, bool insideCatchBlock);

  mutable std::mutex mutex_;
  StringInterner interner_;
  // deque, not vector: bulk growth without doubling-copy spikes; indices in
  // the maps below stay valid until compact() rewrites them.
  std::deque<StoredContext> contexts_;

  // Deduplicated set tables. Entry 0 of each is the empty set; entries are
  // append-only and index-stable (contexts reference them by index, and
  // compact() leaves them in place).
  std::deque<std::vector<TryCatchScope>> scopeSets_;
  std::deque<std::vector<ConditionalGuard>> guardSets_;
  std::deque<std::vector<StoredRaiiLocal>> raiiSets_;
  // Canonical-key lookup for the tables (key = unambiguous field dump of
  // the set's contents; see setKey helpers in the .cpp).
  std::unordered_map<std::string, uint32_t> scopeSetIds_;
  std::unordered_map<std::string, uint32_t> guardSetIds_;
  std::unordered_map<std::string, uint32_t> raiiSetIds_;

  std::unordered_map<SId, std::vector<size_t>> byCallee_;
  std::unordered_map<SId, std::vector<size_t>> byCaller_;
  std::unordered_map<SId, size_t> bySite_;
  // TU provenance reverse index: removeTU visits exactly the TU's own
  // contexts instead of scanning all of them (6.37M on the llvm testbed).
  // Contexts recorded without a tuPath (hand-built tests, pre-provenance
  // snapshots) land in noProvenance_ and keep the legacy callSite-prefix
  // fallback.
  std::unordered_map<SId, std::vector<size_t>> byTu_;
  std::vector<size_t> noProvenance_;
  size_t liveCount_ = 0;
};

class PchCache;

// Build a ControlFlowIndex from a compilation database (Phase 3, after call
// graph construction). The CallGraph is used to resolve callee noexcept specs.
// If collapsePaths is non-empty, call site contexts within collapsed paths are
// skipped (same filtering as buildCallGraph).
// threadCount=0 uses hardware_concurrency; threadCount=1 forces serial.
// pchCache, if non-null, provides compiled PCH binaries for faster parsing.
ControlFlowIndex
buildControlFlowIndex(const clang::tooling::CompilationDatabase &compDb,
                      const std::vector<std::string> &files,
                      const CallGraph &graph,
                      const std::vector<std::string> &collapsePaths = {},
                      unsigned threadCount = 0,
                      const PchCache *pchCache = nullptr,
                      const std::string &sysroot = "",
                      const LockTypeConfig &lockCfg = {});

// Index a single TU into an existing ControlFlowIndex (Phase 3).
// Call cfIndex.removeTU(file) first when re-indexing a changed file.
void indexTUControlFlow(ControlFlowIndex &index,
                        const clang::tooling::CompilationDatabase &compDb,
                        const std::string &file,
                        const CallGraph &graph,
                        const std::vector<std::string> &collapsePaths = {},
                        const PchCache *pchCache = nullptr,
                        const std::string &sysroot = "",
                        const LockTypeConfig &lockCfg = {});

// ============================================================================
// Combined bake — ONE frontend parse per TU
// ============================================================================

struct BakedIndexes {
  CallGraph graph;
  ControlFlowIndex cfIndex;
};

// Build both indexes in a single frontend parse per TU: the node/hierarchy
// index, edge building, and control-flow context extraction all run over
// the same ASTContext. Equivalent query results to the historical
// three-parse build at one-third the frontend cost: the two builds that
// used to need cross-TU Phase-1 data (virtual-dispatch fan-out and the
// function-pointer-through-return join) are deferred to query time
// (CallGraph::calleesOf/callersOf), which also keeps incremental reindexing
// consistent with a full rebuild.
// When `stats` is non-null, per-phase wall times and per-TU parse timings
// and outcomes are recorded into it (see BuildStats.h).
BakedIndexes bakeIndexes(const clang::tooling::CompilationDatabase &compDb,
                         const std::vector<std::string> &files,
                         const std::vector<std::string> &collapsePaths = {},
                         unsigned threadCount = 0,
                         const PchCache *pchCache = nullptr,
                         const std::string &sysroot = "",
                         const LockTypeConfig &lockCfg = {},
                         BuildStats *stats = nullptr);

// Single-TU variant for incremental reindex (one parse). Call
// graph.removeTU(file) and cfIndex.removeTU(file) first when re-indexing a
// changed file.
void bakeTU(CallGraph &graph, ControlFlowIndex &cfIndex,
            const clang::tooling::CompilationDatabase &compDb,
            const std::string &file,
            const std::vector<std::string> &collapsePaths = {},
            const PchCache *pchCache = nullptr,
            const std::string &sysroot = "",
            const LockTypeConfig &lockCfg = {});

} // namespace vycor
