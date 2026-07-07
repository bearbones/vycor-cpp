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

#include "vycor/callgraph/BuildStats.h"
#include "vycor/callgraph/CallGraphBuilder.h"
#include "vycor/callgraph/CollapseFilter.h"
#include "vycor/callgraph/ControlFlowIndex.h"
#include "vycor/callgraph/CallGraph.h"
#include "vycor/compat/ClangVersion.h"
#include "vycor/compat/PchCache.h"
#include "vycor/compat/ToolAdjusters.h"

#include "llvm/Support/ThreadPool.h"
#include "llvm/Support/raw_ostream.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <csetjmp>
#include <functional>

// Crash guard — same mechanism as CallGraphBuilder.cpp.
// These are defined there and shared via the signal handler table.
namespace {
thread_local sigjmp_buf tl_cfJumpBuf;
thread_local volatile sig_atomic_t tl_cfGuardActive = 0;
std::atomic<unsigned> g_cfCrashCount{0};

void cfCrashHandler(int sig) {
  if (tl_cfGuardActive) {
    tl_cfGuardActive = 0;
    g_cfCrashCount.fetch_add(1, std::memory_order_relaxed);
    siglongjmp(tl_cfJumpBuf, sig);
  }
  std::signal(sig, SIG_DFL);
  std::raise(sig);
}

int runCfToolGuarded(const clang::tooling::CompilationDatabase &compDb,
                     const std::string &file,
                     clang::tooling::FrontendActionFactory &factory,
                     const vycor::PchCache *pchCache,
                     const std::string &sysroot = "") {
  tl_cfGuardActive = 1;
  int sig = sigsetjmp(tl_cfJumpBuf, 1);
  if (sig != 0) {
    llvm::errs() << "CRASH (signal " << sig << ") in CF index for " << file
                 << " — skipping\n";
    return -1;
  }
  auto tool = vycor::makeClangTool(compDb, {file}, pchCache, sysroot);
  int status = tool.run(&factory);
  tl_cfGuardActive = 0;
  return status;
}

// TUs whose parse reported at least one error, per bake. Surfaced loudly at
// the end of bakeIndexes: a hollow index built from failed parses looks
// superficially plausible (headers index partially before the fatal error),
// so a silent stderr scroll-past is not enough.
std::atomic<unsigned> g_cfParseErrorCount{0};

// Run one guarded parse, timing it and recording the outcome. `stats` may
// be null (timing skipped); the parse-error counter always advances.
// `preTu`, when set, fires before the parse (worker-mode WORKER-TU marker).
void bakeRun(const clang::tooling::CompilationDatabase &compDb,
             const std::string &file,
             clang::tooling::FrontendActionFactory &factory,
             const vycor::PchCache *pchCache, const std::string &sysroot,
             int phase, vycor::BuildStats *stats,
             const std::function<void(const std::string &)> &preTu) {
  if (preTu)
    preTu(file);
  auto t0 = std::chrono::steady_clock::now();
  int status = runCfToolGuarded(compDb, file, factory, pchCache, sysroot);
  if (status == 1)
    g_cfParseErrorCount.fetch_add(1, std::memory_order_relaxed);
  if (stats) {
    double ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0)
                    .count();
    stats->addTuStat({file, phase, ms, status});
  }
}
} // anonymous namespace

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/StmtCXX.h"
#include "clang/Basic/ExceptionSpecificationType.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Lex/Lexer.h"
#include "clang/Tooling/CompilationDatabase.h"
#include "clang/Tooling/Tooling.h"

#include <optional>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

namespace vycor {

// ============================================================================
// Helpers (same pattern as CallGraphBuilder.cpp)
// ============================================================================

static std::string formatLocationHelper(clang::SourceManager &sm,
                                        clang::SourceLocation loc) {
  auto sLoc = sm.getSpellingLoc(loc);
  auto file = sm.getFilename(sLoc);
  unsigned line = sm.getSpellingLineNumber(sLoc);
  unsigned col = sm.getSpellingColumnNumber(sLoc);
  return std::string(file) + ":" + std::to_string(line) + ":" +
         std::to_string(col);
}

// Set of qualified lock type names recognized out of the box. Matched by
// the stripped leading "::" of the record's qualified name.
static const std::unordered_set<std::string> &builtinLockTypes() {
  static const std::unordered_set<std::string> k = {
      "std::lock_guard",    "std::unique_lock",
      "std::shared_lock",   "std::scoped_lock",
      "absl::MutexLock",    "absl::ReaderMutexLock",
      "absl::WriterMutexLock",
  };
  return k;
}

// Classify a type as an RAII local we want to track. Returns nullopt if
// the type is trivially destructed or not a class. Returns Lock when the
// type is a recognized lock; SmartPtr for std::unique_ptr/shared_ptr;
// Other for any remaining non-trivial-destructor class.
static std::optional<RaiiKind> classifyRaiiType(clang::QualType qt,
                                                const LockTypeConfig &cfg) {
  if (qt.isNull())
    return std::nullopt;
  auto canonical = qt.getCanonicalType();
  auto *rd = canonical->getAsCXXRecordDecl();
  if (!rd || !rd->hasDefinition())
    return std::nullopt;

  std::string qname = rd->getQualifiedNameAsString();

  // (a) Built-in lock allowlist.
  if (cfg.useBuiltins) {
    const auto &builtins = builtinLockTypes();
    if (builtins.count(qname))
      return RaiiKind::Lock;
  }
  // (b) User-supplied allowlist.
  for (const auto &allowed : cfg.userAllowlist) {
    if (qname == allowed)
      return RaiiKind::Lock;
  }
  // (c) Clang thread-safety attributes on the record.
  if (rd->hasAttr<clang::CapabilityAttr>() ||
      rd->hasAttr<clang::ScopedLockableAttr>())
    return RaiiKind::Lock;

  // (d) Smart pointers.
  if (qname == "std::unique_ptr" || qname == "std::shared_ptr")
    return RaiiKind::SmartPtr;

  // (e) Any other non-trivial destructor → Other.
  auto *dtor = rd->getDestructor();
  if (dtor && !dtor->isTrivial())
    return RaiiKind::Other;

  return std::nullopt;
}

// ============================================================================
// ControlFlowContextVisitor (Phase 3)
// ============================================================================

class ControlFlowContextVisitor
    : public clang::RecursiveASTVisitor<ControlFlowContextVisitor> {
public:
  ControlFlowContextVisitor(ControlFlowIndex &index, const CallGraph &graph,
                            clang::SourceManager &sm,
                            const std::string &tuPath = "")
      : index_(index), graph_(graph), sm_(sm), tuPath_(tuPath) {}

  void setASTContext(clang::ASTContext *ctx) { ctx_ = ctx; }
  void setCollapseFilter(const CollapseFilter *filter) { collapse_ = filter; }
  void setLockConfig(const LockTypeConfig *cfg) { lockCfg_ = cfg; }

  // -- Function traversal (push/pop funcStack_) ----------------------------

  bool TraverseFunctionDecl(clang::FunctionDecl *decl) {
    funcStack_.push_back(decl);
    bool result = RecursiveASTVisitor::TraverseFunctionDecl(decl);
    funcStack_.pop_back();
    return result;
  }

  bool TraverseCXXMethodDecl(clang::CXXMethodDecl *decl) {
    funcStack_.push_back(decl);
    bool result = RecursiveASTVisitor::TraverseCXXMethodDecl(decl);
    funcStack_.pop_back();
    return result;
  }

  bool TraverseCXXConstructorDecl(clang::CXXConstructorDecl *decl) {
    funcStack_.push_back(decl);
    bool result = RecursiveASTVisitor::TraverseCXXConstructorDecl(decl);
    funcStack_.pop_back();
    return result;
  }

  bool TraverseCXXDestructorDecl(clang::CXXDestructorDecl *decl) {
    funcStack_.push_back(decl);
    bool result = RecursiveASTVisitor::TraverseCXXDestructorDecl(decl);
    funcStack_.pop_back();
    return result;
  }

  // -- Try/catch traversal (push/pop tryScopeStack_) -----------------------

  bool TraverseCXXTryStmt(clang::CXXTryStmt *stmt) {
    TryScopeEntry entry;
    entry.tryLocation = formatLocation(stmt->getTryLoc());
    entry.enclosingFunction = getCurrentFunction();
    entry.depth = tryScopeStack_.size();

    for (unsigned i = 0; i < stmt->getNumHandlers(); ++i) {
      entry.handlers.push_back(
          analyzeCatchClause(stmt->getHandler(i)));
    }

    tryScopeStack_.push_back(std::move(entry));

    // Traverse the try body — calls here see the enclosing try scope.
    TraverseStmt(stmt->getTryBlock());

    // Traverse each catch handler body.
    for (unsigned i = 0; i < stmt->getNumHandlers(); ++i) {
      insideCatchBlock_ = true;
      TraverseStmt(stmt->getHandler(i)->getHandlerBlock());
      insideCatchBlock_ = false;
    }

    tryScopeStack_.pop_back();
    return true; // Skip base traversal — we manually traversed children.
  }

  // -- Compound-statement scopes (push/pop scopeStack_) --------------------

  bool TraverseCompoundStmt(clang::CompoundStmt *stmt) {
    scopeStack_.push_back({});
    bool result = RecursiveASTVisitor::TraverseCompoundStmt(stmt);
    scopeStack_.pop_back();
    return result;
  }

  // Track local RAII VarDecls as they appear in lexical scope. Calls that
  // follow in the same CompoundStmt see the local as live.
  bool VisitVarDecl(clang::VarDecl *decl) {
    if (scopeStack_.empty())
      return true;
    if (!decl->isLocalVarDecl())
      return true;
    LockTypeConfig emptyCfg;
    const auto &cfg = lockCfg_ ? *lockCfg_ : emptyCfg;
    auto kind = classifyRaiiType(decl->getType(), cfg);
    if (!kind)
      return true;

    RaiiLocal local;
    local.typeName = decl->getType().getCanonicalType().getAsString();
    local.varName = decl->getNameAsString();
    local.declLocation = formatLocation(decl->getLocation());
    local.kind = *kind;
    scopeStack_.back().push_back(std::move(local));
    return true;
  }

  // -- If-statement traversal (push/pop guardStack_) -----------------------

  bool TraverseIfStmt(clang::IfStmt *stmt) {
    GuardEntry guard;
    guard.conditionText = getSourceText(stmt->getCond());
    guard.location = formatLocation(stmt->getIfLoc());

    // Traverse the then-branch with guard context.
    guard.inTrueBranch = true;
    guardStack_.push_back(guard);
    if (stmt->getThen())
      TraverseStmt(stmt->getThen());
    guardStack_.pop_back();

    // Traverse the else-branch with negated guard context.
    if (stmt->getElse()) {
      guard.inTrueBranch = false;
      guardStack_.push_back(guard);
      TraverseStmt(stmt->getElse());
      guardStack_.pop_back();
    }

    return true; // Skip base — manually traversed.
  }

  // -- Visit call sites and snapshot context -------------------------------

  bool VisitCallExpr(clang::CallExpr *expr) {
    std::string caller = getCurrentFunction();
    if (caller.empty())
      return true;

    if (!isInUserCode(expr->getBeginLoc()))
      return true;

    std::string calleeName;
    if (auto *callee = expr->getDirectCallee()) {
      calleeName = callee->getQualifiedNameAsString();
      // Skip internal edges within collapsed paths.
      if (!funcStack_.empty() &&
          isCollapsedEdge(funcStack_.back()->getLocation(),
                          callee->getLocation()))
        return true;
    } else {
      // Indirect call — record with placeholder name.
      calleeName = "<indirect>";
    }

    // Check if this is an assertion macro (assert, DCHECK, etc.).
    if (isAssertionCall(calleeName)) {
      // Record the assertion as a guard for subsequent calls in scope.
      // We handle this by checking the callee name — the assertion's
      // condition is the first argument.
      if (expr->getNumArgs() > 0) {
        GuardEntry guard;
        guard.conditionText = getSourceText(expr->getArg(0));
        guard.location = formatLocation(expr->getBeginLoc());
        guard.inTrueBranch = true;
        guard.isAssertion = true;
        // Assertions don't create a scope — they guard everything after
        // them in the current block. We record them as guards at this
        // call site for context, but don't push to the stack.
        // For now, just add the assertion context to subsequent calls
        // by storing it as a "point guard".
        assertionGuards_.push_back(guard);
      }
      return true;
    }

    CallSiteContext ctx = buildContext(caller, calleeName,
                                       expr->getBeginLoc());
    index_.addCallSiteContext(std::move(ctx));
    return true;
  }

  bool VisitCXXConstructExpr(clang::CXXConstructExpr *expr) {
    std::string caller = getCurrentFunction();
    if (caller.empty())
      return true;

    if (!isInUserCode(expr->getBeginLoc()))
      return true;

    auto *ctor = expr->getConstructor();
    if (!ctor || ctor->isImplicit())
      return true;

    // Skip internal edges within collapsed paths.
    if (!funcStack_.empty() &&
        isCollapsedEdge(funcStack_.back()->getLocation(), ctor->getLocation()))
      return true;

    std::string calleeName = ctor->getQualifiedNameAsString();
    CallSiteContext ctx = buildContext(caller, calleeName,
                                       expr->getBeginLoc());
    index_.addCallSiteContext(std::move(ctx));
    return true;
  }

private:
  ControlFlowIndex &index_;
  const CallGraph &graph_;
  clang::SourceManager &sm_;
  std::string tuPath_;
  clang::ASTContext *ctx_ = nullptr;
  const CollapseFilter *collapse_ = nullptr;
  const LockTypeConfig *lockCfg_ = nullptr;

  std::vector<clang::FunctionDecl *> funcStack_;
  // Per-scope stack of RAII locals declared inside the current lexical
  // CompoundStmt. Outer scope at the front, innermost scope at the back.
  std::vector<std::vector<RaiiLocal>> scopeStack_;

  struct TryScopeEntry {
    std::string tryLocation;
    std::string enclosingFunction;
    std::vector<CatchHandlerInfo> handlers;
    unsigned depth = 0;
  };
  std::vector<TryScopeEntry> tryScopeStack_;

  struct GuardEntry {
    std::string conditionText;
    std::string location;
    bool inTrueBranch = true;
    bool isAssertion = false;
  };
  std::vector<GuardEntry> guardStack_;
  std::vector<GuardEntry> assertionGuards_;

  bool insideCatchBlock_ = false;

  std::string getCurrentFunction() const {
    if (funcStack_.empty())
      return "";
    return funcStack_.back()->getQualifiedNameAsString();
  }

  std::string formatLocation(clang::SourceLocation loc) const {
    return formatLocationHelper(sm_, loc);
  }

  bool isInUserCode(clang::SourceLocation loc) const {
    if (loc.isInvalid())
      return false;
    return !sm_.isInSystemHeader(sm_.getSpellingLoc(loc));
  }

  bool isCollapsedEdge(clang::SourceLocation callerLoc,
                       clang::SourceLocation calleeLoc) const {
    if (!collapse_ || collapse_->empty())
      return false;
    auto callerFile = sm_.getFilename(sm_.getSpellingLoc(callerLoc));
    auto calleeFile = sm_.getFilename(sm_.getSpellingLoc(calleeLoc));
    return collapse_->isCollapsed(callerFile) &&
           collapse_->isCollapsed(calleeFile);
  }

  CatchHandlerInfo analyzeCatchClause(clang::CXXCatchStmt *catchStmt) {
    CatchHandlerInfo info;
    auto caughtType = catchStmt->getCaughtType();
    if (caughtType.isNull()) {
      info.isCatchAll = true;
    } else {
      info.caughtType = caughtType.getAsString();
    }
    info.location = formatLocation(catchStmt->getCatchLoc());
    info.bodySummary = extractHandlerBodySummary(catchStmt);
    return info;
  }

  // Best-effort source-text summary of a catch handler body. Returns an
  // empty string for macro-expanded or otherwise unprintable bodies.
  std::string
  extractHandlerBodySummary(clang::CXXCatchStmt *catchStmt) const {
    if (!ctx_)
      return "";
    auto *body = catchStmt->getHandlerBlock();
    if (!body)
      return "";
    auto range = clang::CharSourceRange::getTokenRange(body->getSourceRange());
    auto text = clang::Lexer::getSourceText(range, sm_, ctx_->getLangOpts());
    std::string s(text);
    // Truncate to min(160 chars, 3 newlines).
    constexpr size_t kMaxChars = 160;
    constexpr unsigned kMaxNewlines = 3;
    unsigned nl = 0;
    size_t cut = s.size();
    for (size_t i = 0; i < s.size(); ++i) {
      if (s[i] == '\n') {
        ++nl;
        if (nl >= kMaxNewlines) {
          cut = i;
          break;
        }
      }
      if (i + 1 >= kMaxChars) {
        cut = i + 1;
        break;
      }
    }
    if (cut < s.size())
      s.resize(cut);
    return s;
  }

  NoexceptSpec extractNoexceptSpec(const clang::FunctionDecl *decl) const {
    auto *proto = decl->getType()->getAs<clang::FunctionProtoType>();
    if (!proto)
      return NoexceptSpec::None;

    switch (proto->getExceptionSpecType()) {
    case clang::EST_BasicNoexcept:
    case clang::EST_NoexceptTrue:
      return NoexceptSpec::Noexcept;
    case clang::EST_NoexceptFalse:
      return NoexceptSpec::NoexceptFalse;
    case clang::EST_DynamicNone:
      return NoexceptSpec::ThrowNone;
    case clang::EST_None:
    case clang::EST_Dynamic:
      return NoexceptSpec::None;
    default:
      return NoexceptSpec::Unknown;
    }
  }

  std::string getSourceText(const clang::Stmt *stmt) const {
    if (!stmt || !ctx_)
      return "";
    auto range = clang::CharSourceRange::getTokenRange(stmt->getSourceRange());
    auto text = clang::Lexer::getSourceText(range, sm_, ctx_->getLangOpts());
    // Truncate very long conditions for readability.
    std::string result(text);
    if (result.size() > 200)
      result = result.substr(0, 197) + "...";
    return result;
  }

  bool isAssertionCall(const std::string &name) const {
    // Match common assertion functions/macros.
    return name == "__assert_fail" || name == "__assert_rtn" ||
           name == "__assert" || name.find("DCHECK") != std::string::npos ||
           name.find("CHECK") != std::string::npos ||
           name.find("ASSERT") != std::string::npos;
  }

  CallSiteContext buildContext(const std::string &caller,
                               const std::string &calleeName,
                               clang::SourceLocation callLoc) const {
    CallSiteContext ctx;
    ctx.callerName = caller;
    ctx.calleeName = calleeName;
    ctx.callSite = formatLocation(callLoc);
    ctx.tuPath = tuPath_;
    ctx.insideCatchBlock = insideCatchBlock_;

    // Snapshot try/catch scopes (innermost first = reverse of stack).
    for (auto it = tryScopeStack_.rbegin(); it != tryScopeStack_.rend(); ++it) {
      TryCatchScope scope;
      scope.tryLocation = it->tryLocation;
      scope.enclosingFunction = it->enclosingFunction;
      scope.handlers = it->handlers;
      scope.nestingDepth = it->depth;
      ctx.enclosingTryCatches.push_back(std::move(scope));
    }

    // Snapshot conditional guards (innermost first = reverse of stack).
    for (auto it = guardStack_.rbegin(); it != guardStack_.rend(); ++it) {
      ConditionalGuard guard;
      guard.conditionText = it->conditionText;
      guard.location = it->location;
      guard.inTrueBranch = it->inTrueBranch;
      guard.isAssertion = it->isAssertion;
      ctx.enclosingGuards.push_back(std::move(guard));
    }

    // Add any assertion guards seen in the current function scope.
    for (const auto &ag : assertionGuards_) {
      ConditionalGuard guard;
      guard.conditionText = ag.conditionText;
      guard.location = ag.location;
      guard.inTrueBranch = true;
      guard.isAssertion = true;
      ctx.enclosingGuards.push_back(std::move(guard));
    }

    // Extract noexcept spec of the caller.
    if (!funcStack_.empty()) {
      ctx.callerNoexcept = extractNoexceptSpec(funcStack_.back());
    }

    // Snapshot live RAII locals, outer scope first, innermost last.
    for (const auto &frame : scopeStack_) {
      for (const auto &local : frame)
        ctx.liveRaiiLocals.push_back(local);
    }

    return ctx;
  }
};

// ============================================================================
// Consumer / Action / Factory (standard chain)
// ============================================================================

namespace {

class ControlFlowContextConsumer : public clang::ASTConsumer {
public:
  ControlFlowContextConsumer(ControlFlowIndex &index, const CallGraph &graph,
                             clang::SourceManager &sm,
                             const CollapseFilter *collapse,
                             const LockTypeConfig *lockCfg,
                             const std::string &tuPath)
      : visitor_(index, graph, sm, tuPath) {
    visitor_.setCollapseFilter(collapse);
    visitor_.setLockConfig(lockCfg);
  }

  void HandleTranslationUnit(clang::ASTContext &ctx) override {
    visitor_.setASTContext(&ctx);
    visitor_.TraverseDecl(ctx.getTranslationUnitDecl());
  }

private:
  ControlFlowContextVisitor visitor_;
};

class ControlFlowContextAction : public clang::ASTFrontendAction {
public:
  ControlFlowContextAction(ControlFlowIndex &index, const CallGraph &graph,
                            const CollapseFilter *collapse,
                            const LockTypeConfig *lockCfg,
                            const std::string &tuPath)
      : index_(index), graph_(graph), collapse_(collapse),
        lockCfg_(lockCfg), tuPath_(tuPath) {}

  std::unique_ptr<clang::ASTConsumer>
  CreateASTConsumer(clang::CompilerInstance &ci, llvm::StringRef) override {
    return std::make_unique<ControlFlowContextConsumer>(
        index_, graph_, ci.getSourceManager(), collapse_, lockCfg_, tuPath_);
  }

private:
  ControlFlowIndex &index_;
  const CallGraph &graph_;
  const CollapseFilter *collapse_;
  const LockTypeConfig *lockCfg_;
  std::string tuPath_;
};

class ControlFlowContextFactory
    : public clang::tooling::FrontendActionFactory {
public:
  ControlFlowContextFactory(ControlFlowIndex &index, const CallGraph &graph,
                             const CollapseFilter *collapse,
                             const LockTypeConfig *lockCfg,
                             const std::string &tuPath = "")
      : index_(index), graph_(graph), collapse_(collapse),
        lockCfg_(lockCfg), tuPath_(tuPath) {}

  std::unique_ptr<clang::FrontendAction> create() override {
    return std::make_unique<ControlFlowContextAction>(index_, graph_,
                                                       collapse_, lockCfg_,
                                                       tuPath_);
  }

private:
  ControlFlowIndex &index_;
  const CallGraph &graph_;
  const CollapseFilter *collapse_;
  const LockTypeConfig *lockCfg_;
  std::string tuPath_;
};

} // anonymous namespace

// ============================================================================
// Public API
// ============================================================================

ControlFlowIndex
buildControlFlowIndex(const clang::tooling::CompilationDatabase &compDb,
                      const std::vector<std::string> &files,
                      const CallGraph &graph,
                      const std::vector<std::string> &collapsePaths,
                      unsigned threadCount,
                      const PchCache *pchCache,
                      const std::string &sysroot,
                      const LockTypeConfig &lockCfg) {
  ControlFlowIndex index;
  CollapseFilter collapseFilter(collapsePaths);
  const CollapseFilter *collapsePtr =
      collapseFilter.empty() ? nullptr : &collapseFilter;
  const LockTypeConfig *lockCfgPtr = &lockCfg;

  auto prevSegv = std::signal(SIGSEGV, cfCrashHandler);
  auto prevBus = std::signal(SIGBUS, cfCrashHandler);
  g_cfCrashCount.store(0, std::memory_order_relaxed);

  bool parallel = threadCount != 1 && files.size() > 1;

  if (parallel) {
#if VYCOR_LLVM_AT_LEAST(19)
    llvm::DefaultThreadPool pool(
        llvm::hardware_concurrency(threadCount));
#else
    llvm::ThreadPool pool(
        llvm::hardware_concurrency(threadCount));
#endif
    for (const auto &file : files) {
      pool.async([&compDb, &index, &graph, collapsePtr, lockCfgPtr, pchCache,
                  &sysroot, file]() {
        ControlFlowContextFactory factory(index, graph, collapsePtr,
                                          lockCfgPtr, file);
        runCfToolGuarded(compDb, file, factory, pchCache, sysroot);
      });
    }
    pool.wait();
  } else {
    for (const auto &file : files) {
      ControlFlowContextFactory factory(index, graph, collapsePtr, lockCfgPtr,
                                        file);
      runCfToolGuarded(compDb, file, factory, pchCache, sysroot);
    }
  }

  unsigned crashes = g_cfCrashCount.load(std::memory_order_relaxed);
  if (crashes > 0) {
    llvm::errs() << "cfindex: " << crashes << " TU(s) crashed and were skipped\n";
  }

  std::signal(SIGSEGV, prevSegv);
  std::signal(SIGBUS, prevBus);
  return index;
}

void indexTUControlFlow(ControlFlowIndex &index,
                        const clang::tooling::CompilationDatabase &compDb,
                        const std::string &file,
                        const CallGraph &graph,
                        const std::vector<std::string> &collapsePaths,
                        const PchCache *pchCache,
                        const std::string &sysroot,
                        const LockTypeConfig &lockCfg) {
  CollapseFilter collapseFilter(collapsePaths);
  const CollapseFilter *collapsePtr =
      collapseFilter.empty() ? nullptr : &collapseFilter;

  auto prevSegv = std::signal(SIGSEGV, cfCrashHandler);
  auto prevBus = std::signal(SIGBUS, cfCrashHandler);
  g_cfCrashCount.store(0, std::memory_order_relaxed);

  ControlFlowContextFactory factory(index, graph, collapsePtr, &lockCfg,
                                    file);
  runCfToolGuarded(compDb, file, factory, pchCache, sysroot);

  std::signal(SIGSEGV, prevSegv);
  std::signal(SIGBUS, prevBus);
}

// ============================================================================
// Combined bake: Phase 1 parse, then Phase 2+3 on one shared parse per TU
// ============================================================================

namespace {

// All three phases — node/hierarchy index, edge building, and control-flow
// context extraction — over the same ASTContext: ONE frontend parse per TU.
// Safe because edge building has no cross-TU reads (the virtual-dispatch
// fan-out and the function-pointer-through-return join are both deferred to
// query time) and the CF visitor never reads the graph during traversal.
// The indexer runs first so same-TU state precedes the edge walk.
class BakeEdgeAndContextConsumer : public clang::ASTConsumer {
public:
  BakeEdgeAndContextConsumer(CallGraph &graph, ControlFlowIndex &index,
                             clang::SourceManager &sm,
                             const CollapseFilter *collapse,
                             const LockTypeConfig *lockCfg,
                             const std::string &tuPath)
      : indexerVisitor_(graph, sm, tuPath),
        edgeVisitor_(graph, sm, tuPath),
        cfVisitor_(index, graph, sm, tuPath) {
    edgeVisitor_.setCollapseFilter(collapse);
    cfVisitor_.setCollapseFilter(collapse);
    cfVisitor_.setLockConfig(lockCfg);
  }

  void HandleTranslationUnit(clang::ASTContext &ctx) override {
    indexerVisitor_.setASTContext(&ctx);
    indexerVisitor_.TraverseDecl(ctx.getTranslationUnitDecl());
    edgeVisitor_.setASTContext(&ctx);
    edgeVisitor_.TraverseDecl(ctx.getTranslationUnitDecl());
    cfVisitor_.setASTContext(&ctx);
    cfVisitor_.TraverseDecl(ctx.getTranslationUnitDecl());
  }

private:
  CallGraphIndexerVisitor indexerVisitor_;
  CallGraphEdgeVisitor edgeVisitor_;
  ControlFlowContextVisitor cfVisitor_;
};

class BakeEdgeAndContextAction : public clang::ASTFrontendAction {
public:
  BakeEdgeAndContextAction(CallGraph &graph, ControlFlowIndex &index,
                           const CollapseFilter *collapse,
                           const LockTypeConfig *lockCfg,
                           const std::string &tuPath)
      : graph_(graph), index_(index), collapse_(collapse), lockCfg_(lockCfg),
        tuPath_(tuPath) {}

  std::unique_ptr<clang::ASTConsumer>
  CreateASTConsumer(clang::CompilerInstance &ci, llvm::StringRef) override {
    return std::make_unique<BakeEdgeAndContextConsumer>(
        graph_, index_, ci.getSourceManager(), collapse_, lockCfg_, tuPath_);
  }

private:
  CallGraph &graph_;
  ControlFlowIndex &index_;
  const CollapseFilter *collapse_;
  const LockTypeConfig *lockCfg_;
  std::string tuPath_;
};

class BakeEdgeAndContextFactory : public clang::tooling::FrontendActionFactory {
public:
  BakeEdgeAndContextFactory(CallGraph &graph, ControlFlowIndex &index,
                            const CollapseFilter *collapse,
                            const LockTypeConfig *lockCfg,
                            const std::string &tuPath)
      : graph_(graph), index_(index), collapse_(collapse), lockCfg_(lockCfg),
        tuPath_(tuPath) {}

  std::unique_ptr<clang::FrontendAction> create() override {
    return std::make_unique<BakeEdgeAndContextAction>(graph_, index_,
                                                      collapse_, lockCfg_,
                                                      tuPath_);
  }

private:
  CallGraph &graph_;
  ControlFlowIndex &index_;
  const CollapseFilter *collapse_;
  const LockTypeConfig *lockCfg_;
  std::string tuPath_;
};

} // anonymous namespace

BakedIndexes bakeIndexes(const clang::tooling::CompilationDatabase &compDb,
                         const std::vector<std::string> &files,
                         const std::vector<std::string> &collapsePaths,
                         unsigned threadCount,
                         const PchCache *pchCache,
                         const std::string &sysroot,
                         const LockTypeConfig &lockCfg,
                         BuildStats *stats,
                         std::function<void(const std::string &)> preTu) {
  BakedIndexes out;
  CollapseFilter collapseFilter(collapsePaths);
  const CollapseFilter *collapsePtr =
      collapseFilter.empty() ? nullptr : &collapseFilter;

  auto prevSegv = std::signal(SIGSEGV, cfCrashHandler);
  auto prevBus = std::signal(SIGBUS, cfCrashHandler);
  g_cfCrashCount.store(0, std::memory_order_relaxed);
  g_cfParseErrorCount.store(0, std::memory_order_relaxed);
  if (stats)
    stats->threads = threadCount;

  bool parallel = threadCount != 1 && files.size() > 1;
  auto bakeStart = std::chrono::steady_clock::now();

  // Single pass: all three visitor phases share one frontend parse per TU
  // (no phase barrier — edge building has no cross-TU reads).
  if (parallel) {
#if VYCOR_LLVM_AT_LEAST(19)
    llvm::DefaultThreadPool pool(llvm::hardware_concurrency(threadCount));
#else
    llvm::ThreadPool pool(llvm::hardware_concurrency(threadCount));
#endif

    for (const auto &file : files) {
      pool.async([&compDb, &out, collapsePtr, &lockCfg, pchCache, &sysroot,
                  stats, &preTu, file]() {
        BakeEdgeAndContextFactory factory(out.graph, out.cfIndex, collapsePtr,
                                          &lockCfg, file);
        bakeRun(compDb, file, factory, pchCache, sysroot, 0, stats, preTu);
      });
    }
    pool.wait();
  } else {
    for (const auto &file : files) {
      BakeEdgeAndContextFactory factory(out.graph, out.cfIndex, collapsePtr,
                                        &lockCfg, file);
      bakeRun(compDb, file, factory, pchCache, sysroot, 0, stats, preTu);
    }
  }
  if (stats)
    stats->phase2WallMs = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - bakeStart)
                              .count();

  unsigned crashes = g_cfCrashCount.load(std::memory_order_relaxed);
  if (crashes > 0) {
    llvm::errs() << "bake: " << crashes << " TU parse(s) crashed and were "
                 << "skipped (" << files.size() << " TUs total)\n";
  }
  unsigned parseErrors = g_cfParseErrorCount.load(std::memory_order_relaxed);
  if (parseErrors > 0) {
    llvm::errs() << "bake: WARNING: " << parseErrors << " of "
                 << files.size() << " TU parse(s) reported errors — the "
                 << "index may be missing nodes/edges/contexts. Check include "
                 << "paths (--extra-arg can inject e.g. "
                 << "--gcc-install-dir=/usr/lib/gcc/<triple>/<ver>)\n";
  }

  std::signal(SIGSEGV, prevSegv);
  std::signal(SIGBUS, prevBus);
  return out;
}

void bakeTU(CallGraph &graph, ControlFlowIndex &cfIndex,
            const clang::tooling::CompilationDatabase &compDb,
            const std::string &file,
            const std::vector<std::string> &collapsePaths,
            const PchCache *pchCache,
            const std::string &sysroot,
            const LockTypeConfig &lockCfg) {
  CollapseFilter collapseFilter(collapsePaths);
  const CollapseFilter *collapsePtr =
      collapseFilter.empty() ? nullptr : &collapseFilter;

  auto prevSegv = std::signal(SIGSEGV, cfCrashHandler);
  auto prevBus = std::signal(SIGBUS, cfCrashHandler);

  BakeEdgeAndContextFactory combinedFactory(graph, cfIndex, collapsePtr,
                                            &lockCfg, file);
  runCfToolGuarded(compDb, file, combinedFactory, pchCache, sysroot);

  std::signal(SIGSEGV, prevSegv);
  std::signal(SIGBUS, prevBus);
}

} // namespace vycor
