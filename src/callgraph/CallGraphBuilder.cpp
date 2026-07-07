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

#include "vycor/callgraph/CallGraphBuilder.h"
#include "vycor/compat/ClangVersion.h"
#include "vycor/compat/ToolAdjusters.h"

#include "llvm/Support/ThreadPool.h"
#include "llvm/Support/raw_ostream.h"

#include <atomic>
#include <csignal>
#include <csetjmp>

namespace {

// Per-thread crash recovery using setjmp/longjmp + signal handler.
// When a TU triggers SIGSEGV/SIGBUS during AST traversal, the signal
// handler longjmps back to the setjmp point, skipping that TU.
thread_local sigjmp_buf tl_jumpBuf;
thread_local volatile sig_atomic_t tl_guardActive = 0;

std::atomic<unsigned> g_crashCount{0};

void crashSignalHandler(int sig) {
  if (tl_guardActive) {
    tl_guardActive = 0;
    g_crashCount.fetch_add(1, std::memory_order_relaxed);
    siglongjmp(tl_jumpBuf, sig);
  }
  // Not in a guarded region — re-raise to get default behavior.
  std::signal(sig, SIG_DFL);
  std::raise(sig);
}

/// Install crash signal handlers. Returns previous handlers for restoration.
struct SavedHandlers {
  void (*segv)(int);
  void (*bus)(int);
};

SavedHandlers installCrashGuard() {
  SavedHandlers saved;
  saved.segv = std::signal(SIGSEGV, crashSignalHandler);
  saved.bus = std::signal(SIGBUS, crashSignalHandler);
  return saved;
}

void restoreCrashGuard(const SavedHandlers &saved) {
  std::signal(SIGSEGV, saved.segv);
  std::signal(SIGBUS, saved.bus);
}

/// Run a ClangTool on a single file with crash recovery.
/// Returns the ClangTool::run() status (0 = success, 1 = errors occurred —
/// the AST may be partial, 2 = no compile command), or -1 if the TU crashed
/// and was skipped by the crash guard.
int runToolGuarded(const clang::tooling::CompilationDatabase &compDb,
                   const std::string &file,
                   clang::tooling::FrontendActionFactory &factory,
                   const vycor::PchCache *pchCache,
                   const std::string &sysroot = "") {
  tl_guardActive = 1;
  int sig = sigsetjmp(tl_jumpBuf, 1);
  if (sig != 0) {
    llvm::errs() << "CRASH (signal " << sig << ") processing " << file
                 << " — skipping\n";
    return -1;
  }

  auto tool = vycor::makeClangTool(compDb, {file}, pchCache, sysroot);
  int status = tool.run(&factory);
  tl_guardActive = 0;
  return status;
}

// TUs whose parse reported at least one error. A hollow index built from
// failed parses looks superficially plausible (headers index partially
// before the fatal error), so surfacing this loudly is load-bearing.
std::atomic<unsigned> g_parseErrorCount{0};

void notedRun(const clang::tooling::CompilationDatabase &compDb,
              const std::string &file,
              clang::tooling::FrontendActionFactory &factory,
              const vycor::PchCache *pchCache, const std::string &sysroot) {
  if (runToolGuarded(compDb, file, factory, pchCache, sysroot) == 1)
    g_parseErrorCount.fetch_add(1, std::memory_order_relaxed);
}

} // anonymous namespace

#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Tooling/CompilationDatabase.h"

namespace vycor {

// ============================================================================
// Helpers shared by both visitors
// ============================================================================

static std::string getFilePathHelper(clang::SourceManager &sm,
                                     clang::SourceLocation loc) {
  auto fileEntry =
      sm.getFileEntryRefForID(sm.getFileID(sm.getSpellingLoc(loc)));
  if (fileEntry)
    return std::string(fileEntry->getName());
  return "<unknown>";
}

static std::string formatLocationHelper(clang::SourceManager &sm,
                                        clang::SourceLocation loc) {
  auto sLoc = sm.getSpellingLoc(loc);
  auto file = sm.getFilename(sLoc);
  unsigned line = sm.getSpellingLineNumber(sLoc);
  unsigned col = sm.getSpellingColumnNumber(sLoc);
  return std::string(file) + ":" + std::to_string(line) + ":" +
         std::to_string(col);
}

// Stable synthetic name for a lambda closure: "lambda#file:line:col#enclosing".
// Both phases must compute the identical name so Phase 2 edges land on the
// Phase 1 node. The canonical location is the lambda's closure class
// begin-loc, which equals the LambdaExpr begin-loc (`[`).
static std::string lambdaQualifiedName(clang::SourceManager &sm,
                                       clang::SourceLocation loc,
                                       const std::string &enclosing) {
  std::string site = formatLocationHelper(sm, loc);
  std::string parent = enclosing.empty() ? std::string("<tu>") : enclosing;
  return "lambda#" + site + "#" + parent;
}

// Unwrap implicit/temporary/functional-cast wrappers and ask: does this
// expression denote a LambdaExpr? Handles the common
// std::function<…>(lambda) and std::thread(lambda, args…) argument shapes.
static const clang::LambdaExpr *asLambdaExpr(const clang::Expr *expr) {
  if (!expr)
    return nullptr;
  const clang::Expr *cur = expr->IgnoreParenImpCasts();
  while (cur) {
    if (auto *le = llvm::dyn_cast<clang::LambdaExpr>(cur))
      return le;
    if (auto *mt = llvm::dyn_cast<clang::MaterializeTemporaryExpr>(cur)) {
      cur = mt->getSubExpr();
      continue;
    }
    if (auto *bt = llvm::dyn_cast<clang::CXXBindTemporaryExpr>(cur)) {
      cur = bt->getSubExpr();
      continue;
    }
    if (auto *fc = llvm::dyn_cast<clang::CXXFunctionalCastExpr>(cur)) {
      cur = fc->getSubExpr();
      continue;
    }
    if (auto *cst = llvm::dyn_cast<clang::CastExpr>(cur)) {
      cur = cst->getSubExpr();
      continue;
    }
    if (auto *ce = llvm::dyn_cast<clang::CXXConstructExpr>(cur)) {
      if (ce->getNumArgs() >= 1) {
        cur = ce->getArg(0);
        continue;
      }
    }
    break;
  }
  return nullptr;
}

// Map a well-known concurrency-spawner qualified name to its ExecutionContext.
// Returns Synchronous for non-spawners; callers check for != Synchronous.
static ExecutionContext spawnerContextFor(llvm::StringRef qualifiedName) {
  if (qualifiedName == "std::thread::thread" ||
      qualifiedName == "std::jthread::jthread")
    return ExecutionContext::ThreadSpawn;
  if (qualifiedName == "std::async")
    return ExecutionContext::AsyncTask;
  if (qualifiedName == "std::packaged_task::packaged_task")
    return ExecutionContext::PackagedTask;
  if (qualifiedName == "std::invoke" || qualifiedName == "std::bind")
    return ExecutionContext::Invoke;
  return ExecutionContext::Synchronous;
}

// ============================================================================
// CallGraphIndexerVisitor (Phase 1)
// ============================================================================

CallGraphIndexerVisitor::CallGraphIndexerVisitor(CallGraph &graph,
                                                 clang::SourceManager &sm,
                                                 const std::string &tuPath)
    : graph_(graph), sm_(sm), tuPath_(tuPath) {}

std::string CallGraphIndexerVisitor::getFilePath(
    clang::SourceLocation loc) const {
  return getFilePathHelper(sm_, loc);
}

std::string CallGraphIndexerVisitor::formatLocation(
    clang::SourceLocation loc) const {
  return formatLocationHelper(sm_, loc);
}

std::string CallGraphIndexerVisitor::getCurrentFunction() const {
  if (funcStack_.empty())
    return "";
  auto *top = funcStack_.back();
  if (auto *md = llvm::dyn_cast<clang::CXXMethodDecl>(top)) {
    if (md->getParent() && md->getParent()->isLambda()) {
      // Find the nearest non-lambda enclosing function.
      std::string enclosing;
      for (auto it = funcStack_.rbegin() + 1; it != funcStack_.rend(); ++it) {
        auto *fd = *it;
        if (auto *mm = llvm::dyn_cast<clang::CXXMethodDecl>(fd)) {
          if (mm->getParent() && mm->getParent()->isLambda())
            continue;
        }
        enclosing = fd->getQualifiedNameAsString();
        break;
      }
      return lambdaQualifiedName(sm_, md->getParent()->getBeginLoc(),
                                 enclosing);
    }
  }
  return top->getQualifiedNameAsString();
}

bool CallGraphIndexerVisitor::TraverseFunctionDecl(
    clang::FunctionDecl *decl) {
  funcStack_.push_back(decl);
  bool result = RecursiveASTVisitor::TraverseFunctionDecl(decl);
  funcStack_.pop_back();
  return result;
}

bool CallGraphIndexerVisitor::TraverseCXXMethodDecl(
    clang::CXXMethodDecl *decl) {
  funcStack_.push_back(decl);
  bool result = RecursiveASTVisitor::TraverseCXXMethodDecl(decl);
  funcStack_.pop_back();
  return result;
}

bool CallGraphIndexerVisitor::TraverseCXXConstructorDecl(
    clang::CXXConstructorDecl *decl) {
  funcStack_.push_back(decl);
  bool result = RecursiveASTVisitor::TraverseCXXConstructorDecl(decl);
  funcStack_.pop_back();
  return result;
}

bool CallGraphIndexerVisitor::TraverseCXXDestructorDecl(
    clang::CXXDestructorDecl *decl) {
  funcStack_.push_back(decl);
  bool result = RecursiveASTVisitor::TraverseCXXDestructorDecl(decl);
  funcStack_.pop_back();
  return result;
}

bool CallGraphIndexerVisitor::VisitFunctionDecl(clang::FunctionDecl *decl) {
  if (decl->isImplicit())
    return true;

  // Skip function templates (we want concrete instantiations).
  if (decl->getDescribedFunctionTemplate())
    return true;

  // Avoid duplicates from redeclarations.
  if (decl->getPreviousDecl())
    return true;

  CallGraphNode node;
  node.qualifiedName = decl->getQualifiedNameAsString();
  node.file = getFilePath(decl->getLocation());
  node.line = sm_.getSpellingLineNumber(decl->getLocation());

  if (auto *method = llvm::dyn_cast<clang::CXXMethodDecl>(decl)) {
    node.isVirtual = method->isVirtual();
    if (auto *parent =
            llvm::dyn_cast<clang::CXXRecordDecl>(method->getParent()))
      node.enclosingClass = parent->getQualifiedNameAsString();
  }

  graph_.addNode(std::move(node), tuPath_);
  return true;
}

bool CallGraphIndexerVisitor::VisitCXXRecordDecl(
    clang::CXXRecordDecl *decl) {
  if (decl->isImplicit() || !decl->isThisDeclarationADefinition())
    return true;

  std::string className = decl->getQualifiedNameAsString();

  // Record base class relationships.
  for (const auto &base : decl->bases()) {
    auto *baseType = base.getType()->getAsCXXRecordDecl();
    if (baseType)
      graph_.addDerivedClass(baseType->getQualifiedNameAsString(), className, tuPath_);
  }

  // Record virtual method overrides.
  for (auto *method : decl->methods()) {
    if (!method->isVirtual() || method->isImplicit())
      continue;

    for (auto *overridden : method->overridden_methods()) {
      graph_.addMethodOverride(overridden->getQualifiedNameAsString(),
                               method->getQualifiedNameAsString(), tuPath_);
    }
  }

  // Compute effective implementations for concrete classes.
  if (!decl->isAbstract())
    computeEffectiveImpls(decl);

  return true;
}

void CallGraphIndexerVisitor::computeEffectiveImpls(
    const clang::CXXRecordDecl *cls) {
  std::string className = cls->getQualifiedNameAsString();
  std::set<std::string> handledMethodNames;

  // Walk from most-derived (cls) upward through bases.
  // The first implementation found for a method name is the effective one.
  std::vector<const clang::CXXRecordDecl *> hierarchy;
  hierarchy.push_back(cls);

  for (size_t i = 0; i < hierarchy.size(); ++i) {
    const auto *current = hierarchy[i];

    for (auto *method : current->methods()) {
      if (!method->isVirtual() || method->isImplicit())
        continue;

      std::string methodName = method->getNameAsString();
      if (handledMethodNames.count(methodName))
        continue;

      if (!method->isPureVirtual()) {
        handledMethodNames.insert(methodName);
        graph_.addEffectiveImpl(className,
                                method->getQualifiedNameAsString(), tuPath_);
      }
    }

    // Add base classes to walk.
    for (const auto &base : current->bases()) {
      auto *baseDecl = base.getType()->getAsCXXRecordDecl();
      if (baseDecl && baseDecl->isThisDeclarationADefinition())
        hierarchy.push_back(baseDecl);
    }
  }
}

bool CallGraphIndexerVisitor::TraverseLambdaExpr(clang::LambdaExpr *expr) {
  // While inside the lambda body, bodyFunc_ should be the call operator so
  // nested VisitReturnStmt / visitor methods attribute to the lambda node.
  auto *op = expr->getCallOperator();
  if (op)
    funcStack_.push_back(op);
  bool result = RecursiveASTVisitor::TraverseLambdaExpr(expr);
  if (op)
    funcStack_.pop_back();
  return result;
}

bool CallGraphIndexerVisitor::VisitLambdaExpr(clang::LambdaExpr *expr) {
  // At this point TraverseLambdaExpr has already pushed the call operator, so
  // skip the top frame when searching for the real enclosing function.
  std::string enclosing;
  auto begin = funcStack_.rbegin();
  if (begin != funcStack_.rend()) {
    if (auto *md = llvm::dyn_cast<clang::CXXMethodDecl>(*begin)) {
      if (md->getParent() && md->getParent()->isLambda())
        ++begin;
    }
  }
  for (auto it = begin; it != funcStack_.rend(); ++it) {
    auto *fd = *it;
    if (auto *md = llvm::dyn_cast<clang::CXXMethodDecl>(fd)) {
      if (md->getParent() && md->getParent()->isLambda())
        continue;
    }
    enclosing = fd->getQualifiedNameAsString();
    break;
  }

  CallGraphNode node;
  node.qualifiedName =
      lambdaQualifiedName(sm_, expr->getBeginLoc(), enclosing);
  node.file = getFilePath(expr->getBeginLoc());
  node.line = sm_.getSpellingLineNumber(expr->getBeginLoc());
  node.enclosingClass = enclosing;
  graph_.addNode(std::move(node), tuPath_);
  return true;
}

bool CallGraphIndexerVisitor::VisitReturnStmt(clang::ReturnStmt *stmt) {
  auto *retVal = stmt->getRetValue();
  if (!retVal)
    return true;

  // Check if returning a function reference.
  auto *expr = retVal->IgnoreParenImpCasts();
  if (auto *dre = llvm::dyn_cast<clang::DeclRefExpr>(expr)) {
    if (auto *funcDecl = llvm::dyn_cast<clang::FunctionDecl>(dre->getDecl())) {
      std::string enclosing = getCurrentFunction();
      if (!enclosing.empty()) {
        graph_.addFunctionReturn(enclosing,
                                 funcDecl->getQualifiedNameAsString(), tuPath_);
      }
    }
  }

  return true;
}

// ============================================================================
// CallGraphEdgeVisitor (Phase 2)
// ============================================================================

CallGraphEdgeVisitor::CallGraphEdgeVisitor(CallGraph &graph,
                                           clang::SourceManager &sm,
                                           const std::string &tuPath)
    : graph_(graph), sm_(sm), tuPath_(tuPath) {}

std::string CallGraphEdgeVisitor::getFilePath(
    clang::SourceLocation loc) const {
  return getFilePathHelper(sm_, loc);
}

std::string CallGraphEdgeVisitor::formatLocation(
    clang::SourceLocation loc) const {
  return formatLocationHelper(sm_, loc);
}

std::string CallGraphEdgeVisitor::getCurrentFunction() const {
  if (funcStack_.empty())
    return "";
  auto *top = funcStack_.back();
  if (auto *md = llvm::dyn_cast<clang::CXXMethodDecl>(top)) {
    if (md->getParent() && md->getParent()->isLambda()) {
      std::string enclosing;
      for (auto it = funcStack_.rbegin() + 1; it != funcStack_.rend(); ++it) {
        auto *fd = *it;
        if (auto *mm = llvm::dyn_cast<clang::CXXMethodDecl>(fd)) {
          if (mm->getParent() && mm->getParent()->isLambda())
            continue;
        }
        enclosing = fd->getQualifiedNameAsString();
        break;
      }
      return lambdaQualifiedName(sm_, md->getParent()->getBeginLoc(),
                                 enclosing);
    }
  }
  return top->getQualifiedNameAsString();
}

bool CallGraphEdgeVisitor::isInUserCode(clang::SourceLocation loc) const {
  if (loc.isInvalid())
    return false;
  return !sm_.isInSystemHeader(sm_.getSpellingLoc(loc));
}

bool CallGraphEdgeVisitor::isCollapsedEdge(
    clang::SourceLocation callerLoc,
    clang::SourceLocation calleeLoc) const {
  if (!collapse_ || collapse_->empty())
    return false;
  std::string callerFile = getFilePath(callerLoc);
  std::string calleeFile = getFilePath(calleeLoc);
  return collapse_->isCollapsed(callerFile) &&
         collapse_->isCollapsed(calleeFile);
}

bool CallGraphEdgeVisitor::TraverseFunctionDecl(clang::FunctionDecl *decl) {
  funcStack_.push_back(decl);
  bool result = RecursiveASTVisitor::TraverseFunctionDecl(decl);
  funcStack_.pop_back();
  return result;
}

bool CallGraphEdgeVisitor::TraverseCXXMethodDecl(
    clang::CXXMethodDecl *decl) {
  funcStack_.push_back(decl);
  bool result = RecursiveASTVisitor::TraverseCXXMethodDecl(decl);
  funcStack_.pop_back();
  return result;
}

bool CallGraphEdgeVisitor::TraverseCXXConstructorDecl(
    clang::CXXConstructorDecl *decl) {
  funcStack_.push_back(decl);
  bool result = RecursiveASTVisitor::TraverseCXXConstructorDecl(decl);
  funcStack_.pop_back();
  return result;
}

bool CallGraphEdgeVisitor::TraverseLambdaExpr(clang::LambdaExpr *expr) {
  auto *op = expr->getCallOperator();
  if (op)
    funcStack_.push_back(op);
  bool result = RecursiveASTVisitor::TraverseLambdaExpr(expr);
  if (op)
    funcStack_.pop_back();
  return result;
}

bool CallGraphEdgeVisitor::TraverseCXXDestructorDecl(
    clang::CXXDestructorDecl *decl) {
  funcStack_.push_back(decl);
  bool result = RecursiveASTVisitor::TraverseCXXDestructorDecl(decl);
  funcStack_.pop_back();
  return result;
}

std::string CallGraphEdgeVisitor::enclosingNonLambdaName() const {
  for (auto it = funcStack_.rbegin(); it != funcStack_.rend(); ++it) {
    auto *fd = *it;
    if (auto *md = llvm::dyn_cast<clang::CXXMethodDecl>(fd)) {
      if (md->getParent() && md->getParent()->isLambda())
        continue;
    }
    return fd->getQualifiedNameAsString();
  }
  return "";
}

void CallGraphEdgeVisitor::processCallableArgs(
    llvm::ArrayRef<clang::Expr *> args, const std::string &caller,
    clang::SourceLocation callSite, ExecutionContext spawnerCtx) {
  const bool isSpawner = spawnerCtx != ExecutionContext::Synchronous;
  const EdgeKind ptrEdgeKind =
      isSpawner ? EdgeKind::ThreadEntry : EdgeKind::FunctionPointer;
  const EdgeKind lambdaEdgeKind =
      isSpawner ? EdgeKind::ThreadEntry : EdgeKind::LambdaCall;
  const std::string siteStr = formatLocation(callSite);

  for (auto *argExpr : args) {
    if (!argExpr)
      continue;
    auto *arg = argExpr->IgnoreParenImpCasts();

    // Lambda passed as argument (direct, or wrapped in std::function /
    // packaged_task / bind trampolines).
    if (auto *le = asLambdaExpr(argExpr)) {
      std::string lambdaName = lambdaQualifiedName(
          sm_, le->getBeginLoc(), enclosingNonLambdaName());
      graph_.addEdge({caller, lambdaName, lambdaEdgeKind, Confidence::Proven,
                      siteStr, 1, spawnerCtx}, tuPath_);
      continue;
    }

    // Unwrap `&f` (explicit address-take of a function or member function).
    if (auto *uo = llvm::dyn_cast<clang::UnaryOperator>(arg)) {
      if (uo->getOpcode() == clang::UO_AddrOf)
        arg = uo->getSubExpr()->IgnoreParenImpCasts();
    }

    if (auto *dre = llvm::dyn_cast<clang::DeclRefExpr>(arg)) {
      if (auto *funcDecl =
              llvm::dyn_cast<clang::FunctionDecl>(dre->getDecl())) {
        graph_.addEdge({caller, funcDecl->getQualifiedNameAsString(),
                        ptrEdgeKind, Confidence::Proven, siteStr, 1,
                        spawnerCtx}, tuPath_);
        handledRefs_.insert(dre);
      } else if (auto *varDecl =
                     llvm::dyn_cast<clang::VarDecl>(dre->getDecl())) {
        auto fnIt = varFuncSources_.find(varDecl);
        if (fnIt != varFuncSources_.end()) {
          for (const auto &funcName : fnIt->second) {
            graph_.addEdge({caller, funcName, ptrEdgeKind,
                            Confidence::Proven, siteStr, 2, spawnerCtx}, tuPath_);
          }
          handledRefs_.insert(dre);
        }
        auto lamIt = varLambdaSources_.find(varDecl);
        if (lamIt != varLambdaSources_.end()) {
          graph_.addEdge({caller, lamIt->second, lambdaEdgeKind,
                          Confidence::Proven, siteStr, 1, spawnerCtx}, tuPath_);
          handledRefs_.insert(dre);
        }
      }
    }
  }
}

bool CallGraphEdgeVisitor::VisitCallExpr(clang::CallExpr *expr) {
  std::string caller = getCurrentFunction();
  if (caller.empty())
    return true;

  // Skip calls from within system headers.
  if (!isInUserCode(expr->getBeginLoc()))
    return true;

  // Skip internal edges within collapsed paths (keep boundary edges).
  auto *directCallee = expr->getDirectCallee();
  if (directCallee && !funcStack_.empty()) {
    if (isCollapsedEdge(funcStack_.back()->getLocation(),
                        directCallee->getLocation()))
      return true;
  }

  // Record the callee DeclRefExpr so VisitDeclRefExpr won't double-count it.
  if (auto *calleeExpr = expr->getCallee()) {
    if (auto *dre = llvm::dyn_cast<clang::DeclRefExpr>(
            calleeExpr->IgnoreParenImpCasts())) {
      handledRefs_.insert(dre);
    }
  }

  // Spawner? (std::async, std::invoke, std::bind, etc. — CallExpr forms.)
  ExecutionContext spawnerCtx = ExecutionContext::Synchronous;
  if (auto *direct = expr->getDirectCallee()) {
    spawnerCtx = spawnerContextFor(direct->getQualifiedNameAsString());
  }

  // Process arguments: detect function pointers and lambdas as callables.
  std::vector<clang::Expr *> args;
  args.reserve(expr->getNumArgs());
  for (unsigned i = 0; i < expr->getNumArgs(); ++i)
    args.push_back(expr->getArg(i));
  processCallableArgs(args, caller, expr->getBeginLoc(), spawnerCtx);

  auto *callee = expr->getDirectCallee();
  if (!callee)
    return true;

  // Handle CXXMemberCallExpr for virtual dispatch.
  if (auto *memberCall = llvm::dyn_cast<clang::CXXMemberCallExpr>(expr)) {
    auto *methodDecl = memberCall->getMethodDecl();
    if (methodDecl && methodDecl->isVirtual()) {
      handleVirtualDispatch(caller, methodDecl,
                            expr->getBeginLoc());
      return true;
    }
  }

  // Handle CXXOperatorCallExpr.
  if (llvm::isa<clang::CXXOperatorCallExpr>(expr)) {
    graph_.addEdge({caller, callee->getQualifiedNameAsString(),
                    EdgeKind::OperatorCall, Confidence::Proven,
                    formatLocation(expr->getBeginLoc()), 0}, tuPath_);
    return true;
  }

  // Detect std::make_unique<T> / std::make_shared<T> as constructing T.
  if (auto *specArgs = callee->getTemplateSpecializationArgs()) {
    std::string calleeName = callee->getQualifiedNameAsString();
    if (calleeName.find("make_unique") != std::string::npos ||
        calleeName.find("make_shared") != std::string::npos) {
      if (specArgs->size() > 0 &&
          specArgs->get(0).getKind() == clang::TemplateArgument::Type) {
        auto constructedType = specArgs->get(0).getAsType();
        if (auto *recordDecl = constructedType->getAsCXXRecordDecl()) {
          if (recordDecl->isThisDeclarationADefinition()) {
            std::string typeName = recordDecl->getQualifiedNameAsString();
            // Add a constructor edge for the constructed type.
            for (auto *ctor : recordDecl->ctors()) {
              if (!ctor->isImplicit() || ctor->isCopyOrMoveConstructor())
                continue;
              graph_.addNode(
                  {ctor->getQualifiedNameAsString(),
                   getFilePath(ctor->getLocation()),
                   sm_.getSpellingLineNumber(ctor->getLocation()), false, false,
                   typeName});
              graph_.addEdge({caller, ctor->getQualifiedNameAsString(),
                              EdgeKind::ConstructorCall, Confidence::Proven,
                              formatLocation(expr->getBeginLoc()), 0}, tuPath_);
            }
            // Also just add a generic constructor node for the type.
            std::string ctorName = typeName + "::" +
                                   recordDecl->getNameAsString();
            graph_.addNode({ctorName, getFilePath(recordDecl->getLocation()),
                            sm_.getSpellingLineNumber(recordDecl->getLocation()),
                            false, false, typeName}, tuPath_);
            graph_.addEdge({caller, ctorName, EdgeKind::ConstructorCall,
                            Confidence::Proven,
                            formatLocation(expr->getBeginLoc()), 0}, tuPath_);
          }
        }
      }
    }
  }

  // Regular direct call.
  graph_.addEdge({caller, callee->getQualifiedNameAsString(),
                  EdgeKind::DirectCall, Confidence::Proven,
                  formatLocation(expr->getBeginLoc()), 0}, tuPath_);

  return true;
}

void CallGraphEdgeVisitor::handleVirtualDispatch(
    const std::string &caller, clang::CXXMethodDecl *method,
    clang::SourceLocation loc) {
  // Record a single Plausible edge to the static target — even when it is
  // pure virtual, since it identifies the dispatch point. The feasible
  // runtime targets (transitive overrides of the static target) are
  // synthesized at query time by CallGraph::calleesOf/callersOf, so
  // overrides indexed later (other TUs, incremental reindex) are visible
  // to this call site without re-baking, and edge storage stays one row
  // per call site instead of one per override.
  graph_.addEdge({caller, method->getQualifiedNameAsString(),
                  EdgeKind::VirtualDispatch, Confidence::Plausible,
                  formatLocation(loc), 0}, tuPath_);
}

bool CallGraphEdgeVisitor::VisitCXXConstructExpr(
    clang::CXXConstructExpr *expr) {
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

  // Concurrency spawner via constructor (e.g. `std::thread t(&fn, arg)`).
  // Emit ThreadEntry edges for each callable argument in addition to the
  // normal ConstructorCall edge.
  std::string ctorName = ctor->getQualifiedNameAsString();
  ExecutionContext spawnerCtx = spawnerContextFor(ctorName);
  if (spawnerCtx != ExecutionContext::Synchronous) {
    std::vector<clang::Expr *> args;
    args.reserve(expr->getNumArgs());
    for (unsigned i = 0; i < expr->getNumArgs(); ++i)
      args.push_back(expr->getArg(i));
    processCallableArgs(args, caller, expr->getBeginLoc(), spawnerCtx);
  }

  // Add constructor edge.
  graph_.addNode({ctorName, getFilePath(ctor->getLocation()),
                  sm_.getSpellingLineNumber(ctor->getLocation()), false, false,
                  ctor->getParent()->getQualifiedNameAsString()}, tuPath_);
  graph_.addEdge({caller, ctorName, EdgeKind::ConstructorCall,
                  Confidence::Proven, formatLocation(expr->getBeginLoc()), 0}, tuPath_);

  return true;
}

bool CallGraphEdgeVisitor::VisitVarDecl(clang::VarDecl *decl) {
  if (!decl->isLocalVarDecl())
    return true;

  std::string caller = getCurrentFunction();
  if (caller.empty())
    return true;

  // Track variables assigned from functions returning function pointers.
  if (auto *init = decl->getInit()) {
    auto *initExpr = init->IgnoreParenImpCasts();
    if (auto *callExpr = llvm::dyn_cast<clang::CallExpr>(initExpr)) {
      auto *callee = callExpr->getDirectCallee();
      if (callee) {
        auto returns =
            graph_.getFunctionReturns(callee->getQualifiedNameAsString());
        if (!returns.empty())
          varFuncSources_[decl] = std::move(returns);
      }
    }

    // Track locals initialized from a lambda expression (e.g.
    // `auto cb = [=](int x){ ... };` then later `std::thread(cb)`).
    if (auto *le = asLambdaExpr(init)) {
      std::string enclosing;
      for (auto it = funcStack_.rbegin(); it != funcStack_.rend(); ++it) {
        auto *fd = *it;
        if (auto *md = llvm::dyn_cast<clang::CXXMethodDecl>(fd)) {
          if (md->getParent() && md->getParent()->isLambda())
            continue;
        }
        enclosing = fd->getQualifiedNameAsString();
        break;
      }
      varLambdaSources_[decl] =
          lambdaQualifiedName(sm_, le->getBeginLoc(), enclosing);
    }
  }

  // Add "concrete type knowledge" edges for polymorphic local variables.
  auto *recordDecl = decl->getType()->getAsCXXRecordDecl();
  if (!recordDecl || !recordDecl->isPolymorphic())
    return true;
  if (recordDecl->isAbstract())
    return true;
  if (!recordDecl->isThisDeclarationADefinition())
    return true;

  addConcreteTypeEdges(caller, recordDecl, decl->getLocation());
  return true;
}

void CallGraphEdgeVisitor::addConcreteTypeEdges(
    const std::string &caller, const clang::CXXRecordDecl *cls,
    clang::SourceLocation loc) {
  std::string site = formatLocation(loc);
  std::set<std::string> handledMethodNames;

  // Walk from most-derived class upward to find effective implementations.
  std::vector<const clang::CXXRecordDecl *> hierarchy;
  hierarchy.push_back(cls);

  for (size_t i = 0; i < hierarchy.size(); ++i) {
    const auto *current = hierarchy[i];

    for (auto *method : current->methods()) {
      if (!method->isVirtual() || method->isImplicit())
        continue;

      std::string methodName = method->getNameAsString();
      if (handledMethodNames.count(methodName))
        continue;

      if (!method->isPureVirtual()) {
        handledMethodNames.insert(methodName);
        graph_.addEdge({caller, method->getQualifiedNameAsString(),
                        EdgeKind::VirtualDispatch, Confidence::Proven, site,
                        0}, tuPath_);
      }
    }

    // Also add destructor edges.
    if (auto *dtor = current->getDestructor()) {
      std::string dtorName = dtor->getQualifiedNameAsString();
      if (!handledMethodNames.count("~")) {
        graph_.addNode({dtorName, getFilePath(dtor->getLocation()),
                        sm_.getSpellingLineNumber(dtor->getLocation()), false,
                        dtor->isVirtual(),
                        current->getQualifiedNameAsString()}, tuPath_);
        graph_.addEdge({caller, dtorName, EdgeKind::DestructorCall,
                        Confidence::Proven, site, 0}, tuPath_);
      }
    }

    for (const auto &base : current->bases()) {
      auto *baseDecl = base.getType()->getAsCXXRecordDecl();
      if (baseDecl && baseDecl->isThisDeclarationADefinition())
        hierarchy.push_back(baseDecl);
    }
  }
}

bool CallGraphEdgeVisitor::VisitDeclRefExpr(clang::DeclRefExpr *expr) {
  // Skip if already handled as callee or function-pointer argument.
  if (handledRefs_.count(expr))
    return true;

  auto *funcDecl = llvm::dyn_cast<clang::FunctionDecl>(expr->getDecl());
  if (!funcDecl)
    return true;

  std::string caller = getCurrentFunction();
  if (caller.empty())
    return true;

  if (!isInUserCode(expr->getBeginLoc()))
    return true;

  // Skip internal edges within collapsed paths.
  if (!funcStack_.empty() &&
      isCollapsedEdge(funcStack_.back()->getLocation(), funcDecl->getLocation()))
    return true;

  // This is a function reference in a non-call, non-argument context.
  // Treat as address-taken: Plausible edge.
  graph_.addEdge({caller, funcDecl->getQualifiedNameAsString(),
                  EdgeKind::FunctionPointer, Confidence::Plausible,
                  formatLocation(expr->getBeginLoc()), 0}, tuPath_);

  return true;
}

// ============================================================================
// Consumer / Action / Factory
// ============================================================================

CallGraphBuilderConsumer::CallGraphBuilderConsumer(CallGraph &graph,
                                                   clang::SourceManager &sm)
    : indexer_(graph, sm), edgeBuilder_(graph, sm) {}

void CallGraphBuilderConsumer::HandleTranslationUnit(
    clang::ASTContext &context) {
  // Phase 1: Index declarations and hierarchy.
  indexer_.setASTContext(&context);
  indexer_.TraverseDecl(context.getTranslationUnitDecl());

  // Phase 2: Build edges.
  edgeBuilder_.setASTContext(&context);
  edgeBuilder_.TraverseDecl(context.getTranslationUnitDecl());
}

CallGraphBuilderAction::CallGraphBuilderAction(CallGraph &graph)
    : graph_(graph) {}

std::unique_ptr<clang::ASTConsumer>
CallGraphBuilderAction::CreateASTConsumer(clang::CompilerInstance &ci,
                                          llvm::StringRef /*file*/) {
  return std::make_unique<CallGraphBuilderConsumer>(graph_,
                                                    ci.getSourceManager());
}

CallGraphBuilderFactory::CallGraphBuilderFactory(CallGraph &graph)
    : graph_(graph) {}

std::unique_ptr<clang::FrontendAction> CallGraphBuilderFactory::create() {
  return std::make_unique<CallGraphBuilderAction>(graph_);
}

// ============================================================================
// Multi-TU builder
// ============================================================================

namespace {

class IndexerOnlyConsumer : public clang::ASTConsumer {
public:
  IndexerOnlyConsumer(CallGraph &graph, clang::SourceManager &sm,
                      const std::string &tuPath)
      : visitor_(graph, sm, tuPath) {}
  void HandleTranslationUnit(clang::ASTContext &ctx) override {
    visitor_.setASTContext(&ctx);
    visitor_.TraverseDecl(ctx.getTranslationUnitDecl());
  }

private:
  CallGraphIndexerVisitor visitor_;
};

class IndexerOnlyAction : public clang::ASTFrontendAction {
public:
  IndexerOnlyAction(CallGraph &g, const std::string &tuPath)
      : graph_(g), tuPath_(tuPath) {}
  std::unique_ptr<clang::ASTConsumer>
  CreateASTConsumer(clang::CompilerInstance &ci, llvm::StringRef) override {
    return std::make_unique<IndexerOnlyConsumer>(graph_,
                                                 ci.getSourceManager(),
                                                 tuPath_);
  }

private:
  CallGraph &graph_;
  std::string tuPath_;
};

class IndexerOnlyFactory : public clang::tooling::FrontendActionFactory {
public:
  IndexerOnlyFactory(CallGraph &g, const std::string &tuPath)
      : graph_(g), tuPath_(tuPath) {}
  std::unique_ptr<clang::FrontendAction> create() override {
    return std::make_unique<IndexerOnlyAction>(graph_, tuPath_);
  }

private:
  CallGraph &graph_;
  std::string tuPath_;
};

class EdgeOnlyConsumer : public clang::ASTConsumer {
public:
  EdgeOnlyConsumer(CallGraph &graph, clang::SourceManager &sm,
                   const CollapseFilter *collapse, const std::string &tuPath)
      : visitor_(graph, sm, tuPath) {
    visitor_.setCollapseFilter(collapse);
  }
  void HandleTranslationUnit(clang::ASTContext &ctx) override {
    visitor_.setASTContext(&ctx);
    visitor_.TraverseDecl(ctx.getTranslationUnitDecl());
  }

private:
  CallGraphEdgeVisitor visitor_;
};

class EdgeOnlyAction : public clang::ASTFrontendAction {
public:
  EdgeOnlyAction(CallGraph &g, const CollapseFilter *collapse,
                 const std::string &tuPath)
      : graph_(g), collapse_(collapse), tuPath_(tuPath) {}
  std::unique_ptr<clang::ASTConsumer>
  CreateASTConsumer(clang::CompilerInstance &ci, llvm::StringRef) override {
    return std::make_unique<EdgeOnlyConsumer>(graph_, ci.getSourceManager(),
                                              collapse_, tuPath_);
  }

private:
  CallGraph &graph_;
  const CollapseFilter *collapse_;
  std::string tuPath_;
};

class EdgeOnlyFactory : public clang::tooling::FrontendActionFactory {
public:
  EdgeOnlyFactory(CallGraph &g, const CollapseFilter *collapse,
                  const std::string &tuPath)
      : graph_(g), collapse_(collapse), tuPath_(tuPath) {}
  std::unique_ptr<clang::FrontendAction> create() override {
    return std::make_unique<EdgeOnlyAction>(graph_, collapse_, tuPath_);
  }

private:
  CallGraph &graph_;
  const CollapseFilter *collapse_;
  std::string tuPath_;
};

} // anonymous namespace

CallGraph buildCallGraph(const clang::tooling::CompilationDatabase &compDb,
                         const std::vector<std::string> &files,
                         const std::vector<std::string> &collapsePaths,
                         unsigned threadCount,
                         const PchCache *pchCache,
                         const std::string &sysroot) {
  CallGraph graph;
  CollapseFilter collapseFilter(collapsePaths);
  const CollapseFilter *collapsePtr =
      collapseFilter.empty() ? nullptr : &collapseFilter;

  auto saved = installCrashGuard();
  g_crashCount.store(0, std::memory_order_relaxed);
  g_parseErrorCount.store(0, std::memory_order_relaxed);

  bool parallel = threadCount != 1 && files.size() > 1;

  if (parallel) {
#if VYCOR_LLVM_AT_LEAST(19)
    llvm::DefaultThreadPool pool(
        llvm::hardware_concurrency(threadCount));
#else
    llvm::ThreadPool pool(
        llvm::hardware_concurrency(threadCount));
#endif

    // Pass 1: Parallel index of all declarations and class hierarchy.
    for (const auto &file : files) {
      pool.async([&compDb, &graph, pchCache, &sysroot, file]() {
        IndexerOnlyFactory factory(graph, file);
        notedRun(compDb, file, factory, pchCache, sysroot);
      });
    }
    pool.wait();

    // Pass 2: Parallel edge building with full hierarchy knowledge.
    for (const auto &file : files) {
      pool.async([&compDb, &graph, collapsePtr, pchCache, &sysroot, file]() {
        EdgeOnlyFactory factory(graph, collapsePtr, file);
        notedRun(compDb, file, factory, pchCache, sysroot);
      });
    }
    pool.wait();
  } else {
    // Serial path — process per-file for crash isolation.
    for (const auto &file : files) {
      IndexerOnlyFactory factory(graph, file);
      notedRun(compDb, file, factory, pchCache, sysroot);
    }
    for (const auto &file : files) {
      EdgeOnlyFactory factory(graph, collapsePtr, file);
      notedRun(compDb, file, factory, pchCache, sysroot);
    }
  }

  unsigned crashes = g_crashCount.load(std::memory_order_relaxed);
  if (crashes > 0) {
    llvm::errs() << "callgraph: " << crashes << " TU(s) crashed and were skipped"
                 << " (" << files.size() << " total)\n";
  }
  unsigned parseErrors = g_parseErrorCount.load(std::memory_order_relaxed);
  if (parseErrors > 0) {
    llvm::errs() << "callgraph: WARNING: " << parseErrors << " of "
                 << 2 * files.size() << " TU parse(s) reported errors — the "
                 << "index may be missing nodes/edges. Check include paths "
                 << "(--extra-arg can inject e.g. --gcc-install-dir=...)\n";
  }

  restoreCrashGuard(saved);
  return graph;
}

void indexTU(CallGraph &graph,
             const clang::tooling::CompilationDatabase &compDb,
             const std::string &file,
             const std::vector<std::string> &collapsePaths,
             const PchCache *pchCache,
             const std::string &sysroot) {
  CollapseFilter collapseFilter(collapsePaths);
  const CollapseFilter *collapsePtr =
      collapseFilter.empty() ? nullptr : &collapseFilter;

  auto saved = installCrashGuard();
  g_crashCount.store(0, std::memory_order_relaxed);

  IndexerOnlyFactory indexerFactory(graph, file);
  runToolGuarded(compDb, file, indexerFactory, pchCache, sysroot);

  EdgeOnlyFactory edgeFactory(graph, collapsePtr, file);
  runToolGuarded(compDb, file, edgeFactory, pchCache, sysroot);

  restoreCrashGuard(saved);
}

} // namespace vycor
