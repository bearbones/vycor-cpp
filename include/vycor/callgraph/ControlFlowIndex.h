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
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace clang {
namespace tooling {
class CompilationDatabase;
} // namespace tooling
} // namespace clang

namespace vycor {

class CallGraph;

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
  ControlFlowIndex() = default;
  ControlFlowIndex(ControlFlowIndex &&other) noexcept;
  ControlFlowIndex &operator=(ControlFlowIndex &&other) noexcept;
  ControlFlowIndex(const ControlFlowIndex &) = delete;
  ControlFlowIndex &operator=(const ControlFlowIndex &) = delete;

  void addCallSiteContext(CallSiteContext ctx);

  // Look up context at a specific call site (file:line:col).
  const CallSiteContext *contextAtSite(const std::string &callSite) const;

  // All contexts where calleeName is the target.
  std::vector<const CallSiteContext *>
  contextsForCallee(const std::string &calleeName) const;

  // All contexts where callerName is the source.
  std::vector<const CallSiteContext *>
  contextsForCaller(const std::string &callerName) const;

  // All call sites targeting calleeName that are inside a try/catch.
  std::vector<const CallSiteContext *>
  protectedCallsTo(const std::string &calleeName) const;

  // All call sites targeting calleeName that are NOT inside a try/catch.
  std::vector<const CallSiteContext *>
  unprotectedCallsTo(const std::string &calleeName) const;

  size_t size() const { return liveCount_; }

  // All stored contexts (for dump mode).
  std::vector<const CallSiteContext *> allContexts() const;

  // Remove all contexts contributed by the given TU. Matches on the
  // recorded tuPath; contexts without one (hand-built in tests, or from
  // pre-provenance snapshots) fall back to a callSite prefix match.
  // Returns the number of contexts removed.
  size_t removeTU(const std::string &tuPath);

  // Compact the contexts vector, eliminating tombstones. Invalidates all
  // previously returned const CallSiteContext * pointers.
  void compact();

  const StringInterner &interner() const { return interner_; }

private:
  using SId = StringInterner::Id;

  mutable std::mutex mutex_;
  StringInterner interner_;
  // deque, not vector: queries hand out pointers into this container, and
  // growth must not invalidate them. compact() is the only invalidator.
  std::deque<CallSiteContext> contexts_;
  std::unordered_map<SId, std::vector<size_t>> byCallee_;
  std::unordered_map<SId, std::vector<size_t>> byCaller_;
  std::unordered_map<SId, size_t> bySite_;
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

} // namespace vycor
