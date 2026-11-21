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
#include "vycor/callgraph/CompilerWritten.h"
#include "vycor/callgraph/CrashGuard.h"
#include "vycor/callgraph/RetainUntilExit.h"
#include "vycor/compat/CallLoc.h"
#include "vycor/compat/ClangVersion.h"
#include "vycor/compat/ToolAdjusters.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/ThreadPool.h"
#include "llvm/Support/raw_ostream.h"

#include <atomic>
#include <memory>

namespace {

std::atomic<unsigned> g_crashCount{0};

/// Run a ClangTool on a single file under the in-process crash guard
/// (callgraph/CrashGuard.h). Returns the ClangTool::run() status (0 =
/// success, 1 = errors occurred — the AST may be partial, 2 = no compile
/// command), or -1 if the TU crashed and was skipped.
int runToolGuarded(const clang::tooling::CompilationDatabase &compDb,
                   const std::string &file,
                   clang::tooling::FrontendActionFactory &factory,
                   const vycor::PchCache *pchCache,
                   const std::string &sysroot = "") {
  int status = -1;
  int sig = 0;
  if (!vycor::runCrashGuarded(
          [&] {
            auto tool = vycor::makeClangTool(compDb, {file}, pchCache, sysroot);
            status = tool.run(&factory);
          },
          &sig)) {
    g_crashCount.fetch_add(1, std::memory_order_relaxed);
    llvm::errs() << "CRASH (signal " << sig << ") processing " << file
                 << " — skipping\n";
    return -1;
  }
  return status;
}

// TUs whose parse reported at least one error. A hollow index built from
// failed parses looks superficially plausible (headers index partially
// before the fatal error), so surfacing this loudly is load-bearing.
std::atomic<unsigned> g_parseErrorCount{0};

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
  llvm::SmallString<128> buf;
  llvm::raw_svector_ostream os(buf);
  os << sm.getFilename(sLoc) << ':' << sm.getSpellingLineNumber(sLoc) << ':'
     << sm.getSpellingColumnNumber(sLoc);
  return std::string(buf.str());
}

// Stable synthetic name for a lambda closure: "lambda#file:line:col#enclosing".
// Both phases must compute the identical name so Phase 2 edges land on the
// Phase 1 node. The canonical location is the lambda's closure class
// begin-loc, which equals the LambdaExpr begin-loc (`[`).
static std::string lambdaQualifiedName(clang::SourceManager &sm,
                                       clang::SourceLocation loc,
                                       const std::string &enclosing) {
  auto sLoc = sm.getSpellingLoc(loc);
  llvm::SmallString<160> buf;
  llvm::raw_svector_ostream os(buf);
  os << "lambda#" << sm.getFilename(sLoc) << ':'
     << sm.getSpellingLineNumber(sLoc) << ':'
     << sm.getSpellingColumnNumber(sLoc) << '#'
     << (enclosing.empty() ? llvm::StringRef("<tu>")
                           : llvm::StringRef(enclosing));
  return std::string(buf.str());
}

// Lambda identity: the synthetic name is already identity-stable across
// phases by construction, so it IS the usr (prefixed to keep the namespace
// disjoint from real USRs) as well as the display name.
static Ident lambdaIdent(std::string name) {
  Ident id;
  id.usr = "vycor-lambda:" + name;
  id.display = std::move(name);
  return id;
}

Ident lambdaClosureIdent(clang::SourceManager &sm, clang::SourceLocation loc,
                         const std::string &enclosing) {
  return lambdaIdent(lambdaQualifiedName(sm, loc, enclosing));
}

// The function a lambda's closure is declared in: the nearest enclosing
// function that is not itself a lambda's call operator, through any local
// class, or null at namespace or class scope. The function a traversal as
// written has on its stack when it meets the lambda, for a closure met
// without that stack (a call to a lambda, an instantiated body).
static const clang::FunctionDecl *
lambdaEnclosingFunction(const clang::CXXRecordDecl *closure) {
  for (const clang::DeclContext *dc = closure->getDeclContext(); dc;
       dc = dc->getParent()) {
    const auto *fn = llvm::dyn_cast<clang::FunctionDecl>(dc);
    if (!fn)
      continue;
    if (const auto *method = llvm::dyn_cast<clang::CXXMethodDecl>(fn))
      if (method->getParent()->isLambda())
        continue;
    return fn;
  }
  return nullptr;
}

static bool isLambdaCallOperator(const clang::FunctionDecl *fn) {
  const auto *method = llvm::dyn_cast<clang::CXXMethodDecl>(fn);
  return method && method->getParent()->isLambda() &&
         method->getOverloadedOperator() == clang::OO_Call;
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

// Does this type plausibly hold a callable produced by another function?
// Gate for deferred function-pointer-through-return tracking: function
// pointers, member-function pointers, and std::function-like wrappers.
static bool isCallableLikeType(clang::QualType ty) {
  if (ty.isNull())
    return false;
  ty = ty.getNonReferenceType();
  if (ty->isFunctionPointerType() || ty->isMemberFunctionPointerType())
    return true;
  if (auto *rd = ty->getAsCXXRecordDecl()) {
    std::string qn = rd->getQualifiedNameAsString();
    if (qn == "std::function" || qn == "std::move_only_function")
      return true;
  }
  return false;
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

Ident CallGraphIndexerVisitor::getCurrentFunction() const {
  if (funcStack_.empty())
    return {};
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
      return lambdaIdent(lambdaQualifiedName(
          sm_, md->getParent()->getBeginLoc(), enclosing));
    }
  }
  return usrs_.identFor(top);
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

bool CallGraphIndexerVisitor::TraverseCXXConversionDecl(
    clang::CXXConversionDecl *decl) {
  funcStack_.push_back(decl);
  bool result = RecursiveASTVisitor::TraverseCXXConversionDecl(decl);
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

  const Ident &id = usrs_.identFor(decl);
  CallGraphNode node;
  node.usr = id.usr;
  node.qualifiedName = id.display;
  node.file = getFilePath(decl->getLocation());
  node.line = sm_.getSpellingLineNumber(decl->getLocation());
  // A body the edge visitor reads: written outside a system header (whose
  // own code it skips). An instantiation's or implicit member's body is
  // marked where the edge visitor reads it.
  const clang::FunctionDecl *def = nullptr;
  node.hasBody = decl->isDefined(def) && !def->isDeleted() &&
                 !sm_.isInSystemHeader(def->getLocation());

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
  recordClass(decl);
  return true;
}

// A class template's instantiated specializations are no code as written,
// so the traversal does not visit them; their bases and overrides are the
// dispatch targets of a virtual call all the same (`Impl<int>::run`
// overriding `Base::run`).
bool CallGraphIndexerVisitor::VisitClassTemplateDecl(
    clang::ClassTemplateDecl *decl) {
  if (decl != decl->getCanonicalDecl())
    return true;
  for (const clang::ClassTemplateSpecializationDecl *spec :
       decl->specializations()) {
    if (spec->getSpecializationKind() == clang::TSK_ExplicitSpecialization)
      continue; // written, visited as such
    const clang::CXXRecordDecl *def = spec->getDefinition();
    if (def && def == spec && !def->isInvalidDecl() &&
        !def->isDependentContext())
      recordClass(def);
  }
  return true;
}

void CallGraphIndexerVisitor::recordClass(const clang::CXXRecordDecl *decl) {
  std::string className = decl->getQualifiedNameAsString();

  // Record base class relationships.
  for (const auto &base : decl->bases()) {
    auto *baseType = base.getType()->getAsCXXRecordDecl();
    if (baseType)
      graph_.addDerivedClass(baseType->getQualifiedNameAsString(), className, tuPath_);
  }

  // Record virtual method overrides (methods overload: usr-keyed).
  for (auto *method : decl->methods()) {
    if (!method->isVirtual() || method->isImplicit())
      continue;

    std::string methodUsr = usrs_.identFor(method).usr;
    for (auto *overridden : method->overridden_methods()) {
      graph_.addMethodOverride(usrs_.identFor(overridden).usr,
                               methodUsr, tuPath_);
    }
  }

  // Compute effective implementations for concrete classes.
  if (!decl->isAbstract())
    computeEffectiveImpls(decl);
}

void CallGraphIndexerVisitor::computeEffectiveImpls(
    const clang::CXXRecordDecl *cls) {
  std::string className = cls->getQualifiedNameAsString();
  llvm::SmallDenseSet<const clang::IdentifierInfo *, 16> handledMethodNames;

  // Walk from most-derived (cls) upward through bases.
  // The first implementation found for a method name is the effective one.
  llvm::SmallVector<const clang::CXXRecordDecl *, 8> hierarchy;
  hierarchy.push_back(cls);

  for (size_t i = 0; i < hierarchy.size(); ++i) {
    const auto *current = hierarchy[i];

    for (auto *method : current->methods()) {
      if (!method->isVirtual() || method->isImplicit())
        continue;

      const clang::IdentifierInfo *methodId = method->getIdentifier();
      if (methodId && handledMethodNames.contains(methodId))
        continue;

      if (!method->isPureVirtual()) {
        if (methodId)
          handledMethodNames.insert(methodId);
        // Class stays display-keyed; the impl method is usr-keyed.
        graph_.addEffectiveImpl(className, usrs_.identFor(method).usr,
                                tuPath_);
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

  Ident id = lambdaIdent(
      lambdaQualifiedName(sm_, expr->getBeginLoc(), enclosing));
  CallGraphNode node;
  node.usr = std::move(id.usr);
  node.qualifiedName = std::move(id.display);
  node.file = getFilePath(expr->getBeginLoc());
  node.line = sm_.getSpellingLineNumber(expr->getBeginLoc());
  node.enclosingClass = enclosing;
  node.hasBody = !sm_.isInSystemHeader(expr->getBeginLoc());
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
      Ident enclosing = getCurrentFunction();
      if (!enclosing.usr.empty()) {
        graph_.addFunctionReturn(enclosing.usr,
                                 usrs_.identFor(funcDecl).usr, tuPath_);
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

Ident CallGraphEdgeVisitor::getCurrentFunction() const {
  if (funcStack_.empty())
    return {};
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
      return lambdaIdent(lambdaQualifiedName(
          sm_, md->getParent()->getBeginLoc(), enclosing));
    }
  }
  return usrs_.identFor(top);
}

Ident CallGraphEdgeVisitor::identForCallee(const clang::FunctionDecl *decl) {
  // A call to a lambda reaches the body read under the lambda's identity:
  // as written, or (a generic lambda's specialization, a closure in an
  // instantiated body) where drainBodies reads it.
  if (isLambdaCallOperator(decl)) {
    scheduleBody(decl);
    return lambdaIdentOf(llvm::cast<clang::CXXMethodDecl>(decl)->getParent());
  }
  Ident id = usrs_.identFor(decl);
  // The indexer never visits implicit members, and RecursiveASTVisitor
  // does not walk template specializations (implicit instantiations or
  // explicit specializations reached only through the template's
  // specialization set), so such a callee has no node yet; register one
  // here so the display-name index can resolve its edges. Redundant
  // registration is a no-op (addNode unions).
  //
  // The global `operator new` and `operator delete` the compiler declares
  // before any header does are implicit only in that first declaration;
  // once <new> redeclares one, lookup finds the redeclaration, which the
  // indexer skipped as one. The node is then the implicit declaration's
  // (`<unknown>`, line 0), so it reads the same in a TU that includes
  // <new> and one that does not. Only these operators: a C library builtin
  // is implicit in its first declaration too, and stays without a node.
  if (auxNodes_.insert(decl).second) {
    CallGraphNode node = calleeNode(decl, id);
    if (!node.usr.empty())
      graph_.addNode(std::move(node), tuPath_);
  }
  scheduleBody(decl);
  return id;
}

CallGraphNode CallGraphEdgeVisitor::calleeNode(const clang::FunctionDecl *decl,
                                               const Ident &id) {
  CallGraphNode node;
  // Located at the first declaration, as the indexer locates a function:
  // which redeclaration a call resolved to depends on where in the TU it
  // was resolved, and every TU must give the node one location.
  const clang::FunctionDecl *where = decl->getFirstDecl();
  if (!where->isImplicit() &&
      decl->getTemplateSpecializationKind() == clang::TSK_Undeclared)
    return node; // the indexer's: usr left empty
  node.usr = id.usr;
  node.qualifiedName = id.display;
  if (where->isImplicit() && decl->getBuiltinID() != 0) {
    // Declared by the compiler where the TU first names it.
    node.file = "<unknown>";
  } else {
    node.file = getFilePath(where->getLocation());
    node.line = sm_.getSpellingLineNumber(where->getLocation());
  }
  // A builtin the compiler expands itself (`std::move`, `std::forward`,
  // `__builtin_operator_new`) has no body to read, and calls nothing the
  // graph would hold: what it does is known.
  node.hasBody = decl->getBuiltinID() != 0;
  if (auto *method = llvm::dyn_cast<clang::CXXMethodDecl>(decl)) {
    node.isVirtual = method->isVirtual();
    if (auto *parent =
            llvm::dyn_cast<clang::CXXRecordDecl>(method->getParent()))
      node.enclosingClass = parent->getQualifiedNameAsString();
  }
  return node;
}

Ident lambdaClosureIdentOf(clang::SourceManager &sm,
                           const clang::CXXRecordDecl *closure) {
  const clang::FunctionDecl *fn = lambdaEnclosingFunction(closure);
  return lambdaIdent(lambdaQualifiedName(
      sm, closure->getBeginLoc(),
      fn ? fn->getQualifiedNameAsString() : std::string()));
}

Ident CallGraphEdgeVisitor::lambdaIdentOf(
    const clang::CXXRecordDecl *closure) const {
  return lambdaClosureIdentOf(sm_, closure);
}

void CallGraphEdgeVisitor::addLambdaNode(const clang::CXXRecordDecl *closure,
                                         const Ident &id,
                                         llvm::StringRef enclosing) {
  CallGraphNode node;
  node.usr = id.usr;
  node.qualifiedName = id.display;
  node.file = getFilePath(closure->getBeginLoc());
  node.line = sm_.getSpellingLineNumber(closure->getBeginLoc());
  node.enclosingClass = std::string(enclosing);
  node.hasBody = true;
  graph_.addNode(std::move(node), tuPath_);
}

// ----------------------------------------------------------------------------
// Bodies no written definition holds
// ----------------------------------------------------------------------------
//
// The traversal as written reads each function where its definition is
// written. A template instantiation's body (implicit, or an explicit
// instantiation definition), a generic lambda's specialization, and the
// code the compiler writes for an implicit or defaulted special member
// have no written definition of their own: the TU's code reaches them by
// calling them, and they are read after the TU, once each, under their own
// identity, so a forward walk continues through them. Inside one, calls
// are recorded wherever they are spelled, a system header included: a
// standard container's instantiated member reaches `operator new` there.
// A system header's own non-template code stays unread.

void CallGraphEdgeVisitor::scheduleBody(const clang::FunctionDecl *fn) {
  if (llvm::isa<clang::CXXDeductionGuideDecl>(fn))
    return;
  // A closure's special members are read where they run (addImplicitCall,
  // VisitCXXConstructExpr).
  if (const auto *method = llvm::dyn_cast<clang::CXXMethodDecl>(fn))
    if (method->getParent()->isLambda() && !isLambdaCallOperator(fn) &&
        (llvm::isa<clang::CXXConstructorDecl>(fn) ||
         llvm::isa<clang::CXXDestructorDecl>(fn)))
      return;
  if (!fn->getTemplateInstantiationPattern() &&
      !isCompilerWrittenSpecialMember(fn))
    return;
  const clang::FunctionDecl *def = readableDefinition(fn);
  // A destructor destroys members and bases whether or not Sema defined it.
  if (!def && !llvm::isa<clang::CXXDestructorDecl>(fn))
    return;
  // A member defaulted where it is declared, outside a system header, is
  // read with its class (readCompilerWritten).
  if (def && !def->isImplicit() && !fn->getTemplateInstantiationPattern() &&
      isInUserCode(def->getLocation()))
    return;
  const clang::FunctionDecl *key = def ? def : fn->getCanonicalDecl();
  if (scheduled_.insert(key).second)
    work_.push_back(key);
}

// Code outside any function (a namespace-scope initializer) records no
// edge, but what it calls runs: the static-initialization metric walks
// from it.
void CallGraphEdgeVisitor::scheduleOutsideFunctions(
    const clang::FunctionDecl *fn, clang::SourceLocation loc) {
  if (fn && isInUserCode(loc))
    scheduleBody(fn);
}

bool CallGraphEdgeVisitor::TraverseTranslationUnitDecl(
    clang::TranslationUnitDecl *decl) {
  const bool ok = RecursiveASTVisitor::TraverseTranslationUnitDecl(decl);
  drainBodies();
  return ok;
}

void CallGraphEdgeVisitor::drainBodies() {
  // Each body is read once; reading one may schedule others. LIFO keeps the
  // order a function of the TU alone.
  assert(funcStack_.empty());
  inCompilerCode_ = true;
  while (!work_.empty()) {
    const clang::FunctionDecl *fn = work_.back();
    work_.pop_back();
    // A closure read where its expression is (TraverseLambdaExpr) may
    // also have been queued by a call seen first (`[&] { ... }()`).
    if (read_.insert(fn).second)
      readBody(fn);
  }
  inCompilerCode_ = false;
}

void CallGraphEdgeVisitor::readBody(const clang::FunctionDecl *decl) {
  auto *fn = const_cast<clang::FunctionDecl *>(decl);
  const size_t frames = funcStack_.size();
  if (isLambdaCallOperator(fn)) {
    // Read under the lambda: getCurrentFunction names it by the function
    // below it on the stack.
    const auto *closure = llvm::cast<clang::CXXMethodDecl>(fn)->getParent();
    const clang::FunctionDecl *outer = lambdaEnclosingFunction(closure);
    if (outer)
      funcStack_.push_back(const_cast<clang::FunctionDecl *>(outer));
    funcStack_.push_back(fn);
    addLambdaNode(closure, getCurrentFunction(),
                  outer ? outer->getQualifiedNameAsString() : std::string());
  } else {
    funcStack_.push_back(fn);
    const Ident &id = usrs_.identFor(fn);
    CallGraphNode node = calleeNode(fn, id);
    if (node.usr.empty()) {
      node.usr = id.usr;
      node.qualifiedName = id.display;
      const clang::SourceLocation loc = fn->getFirstDecl()->getLocation();
      node.file = getFilePath(loc);
      node.line = sm_.getSpellingLineNumber(loc);
    }
    // A destructor with no definition here is still read for its member
    // destruction, but its own body is unseen unless the compiler writes it.
    node.hasBody = fn->doesThisDeclarationHaveABody() ||
                   isCompilerWrittenSpecialMember(fn);
    graph_.addNode(std::move(node), tuPath_);
  }
  if (fn->doesThisDeclarationHaveABody() && !fn->isInvalidDecl() &&
      !fn->isDependentContext()) {
    if (const auto *ctor = llvm::dyn_cast<clang::CXXConstructorDecl>(fn))
      for (const clang::CXXCtorInitializer *init : ctor->inits())
        traverseInitializer(init);
    TraverseStmt(fn->getBody());
  }
  if (const auto *dtor = llvm::dyn_cast<clang::CXXDestructorDecl>(fn))
    addMemberDestruction(dtor);
  funcStack_.resize(frames);
}

// What the compiler writes into a definition the traversal as written
// reads (it skips all three): member and base initializers not written
// (default construction, a default member initializer), the body of a
// defaulted function, and the destruction of members and bases after a
// destructor's body.
void CallGraphEdgeVisitor::readCompilerWritten(clang::FunctionDecl *fn) {
  if (!fn->isThisDeclarationADefinition() || fn->isDeleted() ||
      fn->isInvalidDecl() || fn->isDependentContext())
    return;
  if (const auto *ctor = llvm::dyn_cast<clang::CXXConstructorDecl>(fn))
    for (const clang::CXXCtorInitializer *init : ctor->inits())
      if (!init->isWritten())
        traverseInitializer(init);
  if (fn->isDefaulted() && fn->getBody())
    TraverseStmt(fn->getBody());
  if (const auto *dtor = llvm::dyn_cast<clang::CXXDestructorDecl>(fn))
    addMemberDestruction(dtor);
}

// A constructor's initializer. The compiler sites an initializer it writes
// at the constructor, a base's and a member's alike: a member default
// constructed there is sited at the member instead, so the construction of
// a base and of a member of the same class stay two edges.
void CallGraphEdgeVisitor::traverseInitializer(
    const clang::CXXCtorInitializer *init) {
  clang::Expr *expr = init->getInit();
  if (!expr)
    return;
  const clang::Expr *saved = implicitInit_;
  const clang::SourceLocation savedSite = implicitInitSite_;
  if (!init->isWritten() && init->isAnyMemberInitializer() &&
      llvm::isa<clang::CXXConstructExpr>(expr)) {
    implicitInit_ = expr;
    implicitInitSite_ = init->getAnyMember()->getLocation();
  }
  TraverseStmt(expr);
  implicitInit_ = saved;
  implicitInitSite_ = savedSite;
}

// After its body a destructor destroys the members, then the bases: each a
// DestructorCall at the destructor's declaration.
void CallGraphEdgeVisitor::addMemberDestruction(
    const clang::CXXDestructorDecl *dtor) {
  const clang::SourceLocation site = dtor->getLocation();
  forEachDestroyedSubobject(
      dtor, [&](clang::QualType type) { addDestructorEdge(type, site); });
}

void CallGraphEdgeVisitor::addDestructorEdge(clang::QualType type,
                                             clang::SourceLocation site) {
  if (const clang::CXXDestructorDecl *dtor = destructorRunFor(*ctx_, type))
    addImplicitCall(dtor, EdgeKind::DestructorCall, Confidence::Proven, site);
}

void CallGraphEdgeVisitor::addImplicitCall(const clang::FunctionDecl *fn,
                                           EdgeKind kind,
                                           Confidence confidence,
                                           clang::SourceLocation loc) {
  if (!fn)
    return; // unresolved: the type or the operand is dependent
  Ident caller = getCurrentFunction();
  if (caller.display.empty())
    return;
  if (!recordsCallAt(loc))
    return;
  if (!funcStack_.empty() &&
      isCollapsedEdge(funcStack_.back()->getLocation(), fn->getLocation()))
    return;
  if (const auto *dtor = llvm::dyn_cast<clang::CXXDestructorDecl>(fn))
    if (dtor->getParent()->isLambda()) {
      // A closure has no identity of its own beyond its call operator:
      // destroying one destroys its captures, here.
      forEachDestroyedSubobject(
          dtor, [&](clang::QualType type) { addDestructorEdge(type, loc); });
      return;
    }
  graph_.addEdge({caller.usr, identForCallee(fn).usr, kind, confidence,
                  formatLocation(loc), 0},
                 tuPath_);
}

// An explicit instantiation definition makes the TU emit the functions it
// names, called or not.
bool CallGraphEdgeVisitor::VisitFunctionTemplateDecl(
    clang::FunctionTemplateDecl *decl) {
  forEachExplicitInstantiation(
      decl, [&](const clang::FunctionDecl *fn) { scheduleBody(fn); });
  return true;
}

bool CallGraphEdgeVisitor::VisitClassTemplateDecl(
    clang::ClassTemplateDecl *decl) {
  forEachExplicitInstantiation(
      decl, [&](const clang::FunctionDecl *fn) { scheduleBody(fn); });
  // A virtual member an instantiated class defines is reached through its
  // vtable, not a call the TU spells: read the ones Sema instantiated (the
  // class's vtable is used), so a dispatch to the override walks on.
  if (decl != decl->getCanonicalDecl())
    return true;
  for (const clang::ClassTemplateSpecializationDecl *spec :
       decl->specializations()) {
    const clang::CXXRecordDecl *def = spec->getDefinition();
    if (!def || def != spec || def->isInvalidDecl() ||
        def->isDependentContext() || !def->isDynamicClass())
      continue;
    for (const clang::CXXMethodDecl *method : def->methods())
      if (method->isVirtual() && readableDefinition(method))
        scheduleBody(method);
  }
  return true;
}

// ----------------------------------------------------------------------------
// Operands never evaluated
// ----------------------------------------------------------------------------
//
// `sizeof(f())`, `noexcept(f())`, `decltype(f())`, a requires-expression
// and a concept's constraint name what would be called without calling
// it (the standard library's bodies are full of them). The traversal
// skips them, so no edge comes from one.

bool CallGraphEdgeVisitor::TraverseUnaryExprOrTypeTraitExpr(
    clang::UnaryExprOrTypeTraitExpr *expr) {
  // `sizeof` of a variable-length array evaluates its operand.
  if (expr->getKind() == clang::UETT_SizeOf &&
      expr->getTypeOfArgument()->isVariablyModifiedType())
    return RecursiveASTVisitor::TraverseUnaryExprOrTypeTraitExpr(expr);
  return true;
}

bool CallGraphEdgeVisitor::TraverseCXXNoexceptExpr(clang::CXXNoexceptExpr *) {
  return true;
}

bool CallGraphEdgeVisitor::TraverseRequiresExpr(clang::RequiresExpr *) {
  return true;
}

bool CallGraphEdgeVisitor::TraverseConceptDecl(clang::ConceptDecl *) {
  return true;
}

#if VYCOR_LLVM_AT_LEAST(22)
bool CallGraphEdgeVisitor::TraverseDecltypeTypeLoc(clang::DecltypeTypeLoc,
                                                   bool) {
  return true;
}
bool CallGraphEdgeVisitor::TraverseTypeOfExprTypeLoc(clang::TypeOfExprTypeLoc,
                                                     bool) {
  return true;
}
#else
bool CallGraphEdgeVisitor::TraverseDecltypeTypeLoc(clang::DecltypeTypeLoc) {
  return true;
}
bool CallGraphEdgeVisitor::TraverseTypeOfExprTypeLoc(
    clang::TypeOfExprTypeLoc) {
  return true;
}
#endif

// A braced initializer's semantic form holds what aggregate initialization
// runs that the syntactic form leaves out: default member initializers and
// the constructors of members and array elements the list omits. One form
// is read, so a call in both is one edge.
bool CallGraphEdgeVisitor::TraverseInitListExpr(clang::InitListExpr *expr) {
  clang::InitListExpr *semantic =
      expr->isSemanticForm() ? expr : expr->getSemanticForm();
  if (!semantic)
    semantic = expr;
  if (!TraverseSynOrSemInitListExpr(semantic))
    return false;
  // The elements past the written ones (`Logger a[3] = {}`) are
  // initialized by the array filler, which is no child.
  if (semantic->hasArrayFiller())
    return TraverseStmt(semantic->getArrayFiller());
  return true;
}

// A default member initializer runs in the constructor or aggregate
// initialization that uses it; the traversal visits it only with implicit
// code. A lambda in one is read where it is written.
bool CallGraphEdgeVisitor::TraverseCXXDefaultInitExpr(
    clang::CXXDefaultInitExpr *expr) {
  if (funcStack_.empty())
    return true;
  ++inDefaultInit_;
  const bool ok = TraverseStmt(expr->getExpr());
  --inDefaultInit_;
  return ok;
}

// A throw expression raises through the runtime: `throw x` allocates the
// exception object and calls `__cxa_throw`, `throw;` calls
// `__cxa_rethrow`. The call is as certain as a written one, so the edge is
// a Proven DirectCall at the `throw` keyword; the node is compiler-declared
// (`<unknown>`, line 0) like the implicit `operator new`. Recorded for a
// throw in a template definition too, whatever its operand's type.
bool CallGraphEdgeVisitor::VisitCXXThrowExpr(clang::CXXThrowExpr *expr) {
  Ident caller = getCurrentFunction();
  if (caller.display.empty())
    return true;
  if (!recordsCallAt(expr->getThrowLoc()))
    return true;
  const bool rethrow = expr->getSubExpr() == nullptr;
  Ident callee = throwCalleeIdent(rethrow);
  if (!throwNodes_[rethrow]) {
    throwNodes_[rethrow] = true;
    CallGraphNode node;
    node.usr = callee.usr;
    node.qualifiedName = callee.display;
    node.file = "<unknown>";
    graph_.addNode(std::move(node), tuPath_);
  }
  graph_.addEdge({caller.usr, callee.usr, EdgeKind::DirectCall,
                  Confidence::Proven, formatLocation(expr->getThrowLoc()), 0},
                 tuPath_);
  return true;
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

// A function declared inside a body drainBodies reads (a local class's
// member) runs only when called: the call schedules it as an
// instantiation of its own.
bool CallGraphEdgeVisitor::TraverseFunctionDecl(clang::FunctionDecl *decl) {
  if (inCompilerCode_)
    return true;
  funcStack_.push_back(decl);
  bool result = RecursiveASTVisitor::TraverseFunctionDecl(decl);
  if (result)
    readCompilerWritten(decl);
  funcStack_.pop_back();
  return result;
}

bool CallGraphEdgeVisitor::TraverseCXXMethodDecl(
    clang::CXXMethodDecl *decl) {
  if (inCompilerCode_)
    return true;
  funcStack_.push_back(decl);
  bool result = RecursiveASTVisitor::TraverseCXXMethodDecl(decl);
  if (result)
    readCompilerWritten(decl);
  funcStack_.pop_back();
  return result;
}

bool CallGraphEdgeVisitor::TraverseCXXConstructorDecl(
    clang::CXXConstructorDecl *decl) {
  if (inCompilerCode_)
    return true;
  funcStack_.push_back(decl);
  bool result = RecursiveASTVisitor::TraverseCXXConstructorDecl(decl);
  if (result)
    readCompilerWritten(decl);
  funcStack_.pop_back();
  return result;
}

bool CallGraphEdgeVisitor::TraverseLambdaExpr(clang::LambdaExpr *expr) {
  // A lambda in a default member initializer was read where it is written.
  if (inDefaultInit_ > 0)
    return true;
  auto *op = expr->getCallOperator();
  if (op && inCompilerCode_) {
    if (const clang::FunctionDecl *def = readableDefinition(op)) {
      scheduled_.insert(def);
      if (!read_.insert(def).second) {
        // Its body was read already (drainBodies reached it first): what
        // runs here is the captures' initialization.
        for (unsigned i = 0, n = expr->capture_size(); i != n; ++i) {
          const clang::LambdaCapture *capture = expr->capture_begin() + i;
          if (capture->isExplicit() || shouldVisitImplicitCode())
            if (!TraverseLambdaCapture(expr, capture,
                                       expr->capture_init_begin()[i]))
              return false;
        }
        return true;
      }
    }
  }
  if (op) {
    funcStack_.push_back(op);
    if (inCompilerCode_) {
      // A closure in a body drainBodies reads: read here, once, and given
      // a node (an instantiated class template member's lambda is named
      // by that member, not by the pattern the indexer saw).
      addLambdaNode(op->getParent(), getCurrentFunction(),
                    enclosingNonLambdaName());
    }
  }
  bool result = RecursiveASTVisitor::TraverseLambdaExpr(expr);
  if (op)
    funcStack_.pop_back();
  return result;
}

// A capture's initializer runs where the closure is created, in the
// enclosing function, not in the lambda's body.
bool CallGraphEdgeVisitor::TraverseLambdaCapture(
    clang::LambdaExpr *expr, const clang::LambdaCapture *capture,
    clang::Expr *init) {
  clang::CXXMethodDecl *op = expr->getCallOperator();
  const bool lifted = op && !funcStack_.empty() && funcStack_.back() == op;
  if (lifted)
    funcStack_.pop_back();
  const bool ok =
      RecursiveASTVisitor::TraverseLambdaCapture(expr, capture, init);
  if (lifted)
    funcStack_.push_back(op);
  return ok;
}

bool CallGraphEdgeVisitor::TraverseCXXDestructorDecl(
    clang::CXXDestructorDecl *decl) {
  if (inCompilerCode_)
    return true;
  funcStack_.push_back(decl);
  bool result = RecursiveASTVisitor::TraverseCXXDestructorDecl(decl);
  if (result)
    readCompilerWritten(decl);
  funcStack_.pop_back();
  return result;
}

bool CallGraphEdgeVisitor::TraverseCXXConversionDecl(
    clang::CXXConversionDecl *decl) {
  if (inCompilerCode_)
    return true;
  funcStack_.push_back(decl);
  bool result = RecursiveASTVisitor::TraverseCXXConversionDecl(decl);
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
    llvm::ArrayRef<clang::Expr *> args, const std::string &callerUsr,
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
      Ident lambda = lambdaIdent(lambdaQualifiedName(
          sm_, le->getBeginLoc(), enclosingNonLambdaName()));
      graph_.addEdge({callerUsr, lambda.usr, lambdaEdgeKind,
                      Confidence::Proven, siteStr, 1, spawnerCtx}, tuPath_);
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
        graph_.addEdge({callerUsr, identForCallee(funcDecl).usr,
                        ptrEdgeKind, Confidence::Proven, siteStr, 1,
                        spawnerCtx}, tuPath_);
        handledRefs_.insert(dre);
      } else if (auto *varDecl =
                     llvm::dyn_cast<clang::VarDecl>(dre->getDecl())) {
        auto fnIt = varCallSources_.find(varDecl);
        if (fnIt != varCallSources_.end()) {
          // Deferred: the stored callee is the function that produced the
          // pointer; CallGraph::calleesOf joins it through functionReturns_
          // at query time and synthesizes the ptrEdgeKind edges (empty
          // joins synthesize nothing). This keeps edge building free of
          // cross-TU reads so all phases share one parse.
          graph_.addEdge({callerUsr, fnIt->second,
                          EdgeKind::FunctionPointerReturn, Confidence::Proven,
                          siteStr, 2, spawnerCtx}, tuPath_);
          handledRefs_.insert(dre);
        }
        auto lamIt = varLambdaSources_.find(varDecl);
        if (lamIt != varLambdaSources_.end()) {
          graph_.addEdge({callerUsr, lamIt->second, lambdaEdgeKind,
                          Confidence::Proven, siteStr, 1, spawnerCtx}, tuPath_);
          handledRefs_.insert(dre);
        }
      }
    }
  }
}

bool CallGraphEdgeVisitor::VisitCallExpr(clang::CallExpr *expr) {
  Ident caller = getCurrentFunction();
  if (caller.display.empty()) {
    scheduleOutsideFunctions(expr->getDirectCallee(), expr->getBeginLoc());
    return true;
  }

  // Skip calls from within system headers, outside a body drainBodies
  // reads.
  if (!recordsCallAt(callBeginLoc(expr)))
    return true;

  // Skip internal edges within collapsed paths (keep boundary edges).
  auto *directCallee = expr->getDirectCallee();

  // `p->~T()`: a trivial destructor runs no code, a closure's destroys its
  // captures (addImplicitCall).
  if (const auto *dtor =
          llvm::dyn_cast_or_null<clang::CXXDestructorDecl>(directCallee)) {
    if (dtor->isTrivial())
      return true;
    if (dtor->getParent()->isLambda()) {
      addImplicitCall(dtor, EdgeKind::DestructorCall, Confidence::Proven,
                      callBeginLoc(expr));
      return true;
    }
  }

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

  // Process arguments directly via ArrayRef (zero heap allocation per call).
  processCallableArgs(
      llvm::ArrayRef<clang::Expr *>(expr->getArgs(), expr->getNumArgs()),
      caller.usr, callBeginLoc(expr), spawnerCtx);

  auto *callee = expr->getDirectCallee();
  if (!callee)
    return true;

  // Handle CXXMemberCallExpr for virtual dispatch. A qualified call
  // (`Base::run()`, `obj.Base::run()`) is bound statically to the method
  // it names: a direct call.
  if (auto *memberCall = llvm::dyn_cast<clang::CXXMemberCallExpr>(expr)) {
    auto *methodDecl = memberCall->getMethodDecl();
    const auto *member = llvm::dyn_cast<clang::MemberExpr>(
        memberCall->getCallee()->IgnoreParens());
    if (methodDecl && methodDecl->isVirtual() &&
        !(member && member->hasQualifier())) {
      handleVirtualDispatch(caller.usr, methodDecl,
                            callBeginLoc(expr));
      return true;
    }
  }

  // Handle CXXOperatorCallExpr.
  if (llvm::isa<clang::CXXOperatorCallExpr>(expr)) {
    graph_.addEdge({caller.usr, identForCallee(callee).usr,
                    EdgeKind::OperatorCall, Confidence::Proven,
                    formatLocation(callBeginLoc(expr)), 0}, tuPath_);
    return true;
  }

  // `std::make_unique<T>(...)` and the like construct T in their
  // instantiated bodies (drainBodies), with T's own constructor.

  // Regular direct call.
  graph_.addEdge({caller.usr, identForCallee(callee).usr,
                  EdgeKind::DirectCall, Confidence::Proven,
                  formatLocation(callBeginLoc(expr)), 0}, tuPath_);

  return true;
}

void CallGraphEdgeVisitor::handleVirtualDispatch(
    const std::string &callerUsr, clang::CXXMethodDecl *method,
    clang::SourceLocation loc) {
  // Record a single Plausible edge to the static target — even when it is
  // pure virtual, since it identifies the dispatch point. The feasible
  // runtime targets (transitive overrides of the static target) are
  // synthesized at query time by CallGraph::calleesOf/callersOf, so
  // overrides indexed later (other TUs, incremental reindex) are visible
  // to this call site without re-baking, and edge storage stays one row
  // per call site instead of one per override.
  graph_.addEdge({callerUsr, identForCallee(method).usr,
                  EdgeKind::VirtualDispatch, Confidence::Plausible,
                  formatLocation(loc), 0}, tuPath_);
}

bool CallGraphEdgeVisitor::VisitCXXConstructExpr(
    clang::CXXConstructExpr *expr) {
  Ident caller = getCurrentFunction();
  if (caller.display.empty()) {
    scheduleOutsideFunctions(expr->getConstructor(), expr->getBeginLoc());
    return true;
  }

  const clang::SourceLocation site =
      expr == implicitInit_ ? implicitInitSite_ : expr->getBeginLoc();
  if (!recordsCallAt(site))
    return true;

  // An implicit constructor runs the code the compiler writes for it
  // (member and base construction, default member initializers) unless it
  // is trivial: then it runs none.
  auto *ctor = expr->getConstructor();
  if (!ctor || (ctor->isImplicit() && ctor->isTrivial()))
    return true;

  // Copying or moving a closure copies or moves its captures, here: the
  // closure has no identity of its own beyond its call operator.
  if (ctor->getParent()->isLambda()) {
    if (const clang::FunctionDecl *def = readableDefinition(ctor))
      for (const clang::CXXCtorInitializer *init :
           llvm::cast<clang::CXXConstructorDecl>(def)->inits())
        if (init->getInit())
          TraverseStmt(init->getInit());
    return true;
  }

  // Skip internal edges within collapsed paths.
  if (!funcStack_.empty() &&
      isCollapsedEdge(funcStack_.back()->getLocation(), ctor->getLocation()))
    return true;

  // Concurrency spawner via constructor (e.g. `std::thread t(&fn, arg)`).
  // Emit ThreadEntry edges for each callable argument in addition to the
  // normal ConstructorCall edge. Spawner matching stays display-name-based.
  Ident ctorId = identForCallee(ctor);
  ExecutionContext spawnerCtx = spawnerContextFor(ctorId.display);
  if (spawnerCtx != ExecutionContext::Synchronous) {
    processCallableArgs(
        llvm::ArrayRef<clang::Expr *>(expr->getArgs(), expr->getNumArgs()),
        caller.usr, site, spawnerCtx);
  }

  // Add constructor edge.
  CallGraphNode ctorNode;
  ctorNode.usr = ctorId.usr;
  ctorNode.qualifiedName = ctorId.display;
  const clang::SourceLocation ctorLoc = ctor->getFirstDecl()->getLocation();
  ctorNode.file = getFilePath(ctorLoc);
  ctorNode.line = sm_.getSpellingLineNumber(ctorLoc);
  ctorNode.enclosingClass = ctor->getParent()->getQualifiedNameAsString();
  graph_.addNode(std::move(ctorNode), tuPath_);
  graph_.addEdge({caller.usr, ctorId.usr, EdgeKind::ConstructorCall,
                  Confidence::Proven, formatLocation(site), 0}, tuPath_);

  return true;
}

// The base-class constructor an inheriting constructor (`using
// Base::Base;`) runs, in the body the compiler writes for it.
bool CallGraphEdgeVisitor::VisitCXXInheritedCtorInitExpr(
    clang::CXXInheritedCtorInitExpr *expr) {
  addImplicitCall(expr->getConstructor(), EdgeKind::ConstructorCall,
                  Confidence::Proven, expr->getLocation());
  return true;
}

// A temporary of a class with a non-trivial destructor is destroyed at the
// end of the full-expression (or of the reference it is bound to): a
// DestructorCall at the expression that creates it.
bool CallGraphEdgeVisitor::VisitCXXBindTemporaryExpr(
    clang::CXXBindTemporaryExpr *expr) {
  if (const clang::CXXTemporary *temp = expr->getTemporary())
    addImplicitCall(temp->getDestructor(), EdgeKind::DestructorCall,
                    Confidence::Proven, expr->getBeginLoc());
  return true;
}

// `new T(args)` calls two functions: the allocation function, then the
// constructor (VisitCXXConstructExpr sees that one). The allocation call is
// as direct as a written one: overload resolution picked the function at
// compile time, so the edge is a Proven DirectCall at the expression. The
// deallocation function a new-expression also names runs only when the
// constructor throws, and is not recorded.
bool CallGraphEdgeVisitor::VisitCXXNewExpr(clang::CXXNewExpr *expr) {
  addImplicitCall(expr->getOperatorNew(), EdgeKind::DirectCall,
                  Confidence::Proven, expr->getBeginLoc());
  return true;
}

// `delete p` calls the destructor and then the deallocation function.
// When the destructor is virtual, a plain `delete p` runs the *dynamic*
// type's destructor, and uses the deallocation function looked up there:
// the destructor is a Plausible VirtualDispatch to the static one
// (expanded through the overrides at query time, unless the class is
// final), and the deallocation function the likely callee rather than the
// certain one. The other forms are settled at compile time: `delete[] p`
// (the static and dynamic types must agree), `::delete p` (the global
// function, whatever the class declares; the destructor is still virtual)
// and a destroying `operator delete` (called in place of the destructor,
// never through it).
bool CallGraphEdgeVisitor::VisitCXXDeleteExpr(clang::CXXDeleteExpr *expr) {
  const clang::FunctionDecl *fn = expr->getOperatorDelete();
  const bool destroying = fn && fn->isDestroyingOperatorDelete();
  const clang::QualType destroyed = expr->getDestroyedType();
  const clang::CXXDestructorDecl *dtor =
      destroyed.isNull() || destroying ? nullptr
                                       : destructorRunFor(*ctx_, destroyed);
  const bool dispatched = dtor && dtor->isVirtual() && !expr->isArrayForm() &&
                          !dtor->getParent()->isEffectivelyFinal();
  if (dtor)
    addImplicitCall(dtor,
                    dispatched ? EdgeKind::VirtualDispatch
                               : EdgeKind::DestructorCall,
                    dispatched ? Confidence::Plausible : Confidence::Proven,
                    expr->getBeginLoc());
  Confidence confidence = Confidence::Proven;
  if (fn && !expr->isArrayForm() && !expr->isGlobalDelete() && !destroying &&
      !destroyed.isNull())
    if (const clang::CXXRecordDecl *record = destroyed->getAsCXXRecordDecl())
      if (record->hasDefinition())
        if (const clang::CXXDestructorDecl *virt = record->getDestructor())
          if (virt->isVirtual())
            confidence = Confidence::Plausible;
  addImplicitCall(fn, EdgeKind::DirectCall, confidence, expr->getBeginLoc());
  return true;
}

bool CallGraphEdgeVisitor::VisitVarDecl(clang::VarDecl *decl) {
  if (!decl->isLocalVarDecl())
    return true;

  Ident caller = getCurrentFunction();
  if (caller.display.empty())
    return true;

  // An automatic variable is destroyed when it leaves scope: a
  // DestructorCall at its declaration (the stable site; the scope's end has
  // none of its own). A static local is destroyed at exit, not here.
  // An init-capture is a member of the closure, destroyed with it.
  if (decl->hasLocalStorage() && !decl->isInvalidDecl() &&
      !decl->isInitCapture())
    addDestructorEdge(decl->getType(), decl->getLocation());

  // Track callable-typed variables initialized from a direct call. The
  // callee may return function pointers (recorded by the indexer visitor as
  // functionReturns); resolving WHICH functions it returns is deferred to
  // query time, so no cross-TU graph read happens here. The type gate keeps
  // the tracking to plausibly-callable vars (function pointers,
  // member-function pointers, std::function).
  if (auto *init = decl->getInit()) {
    auto *initExpr = init->IgnoreParenImpCasts();
    if (auto *callExpr = llvm::dyn_cast<clang::CallExpr>(initExpr)) {
      auto *callee = callExpr->getDirectCallee();
      if (callee && isCallableLikeType(decl->getType())) {
        varCallSources_[decl] = identForCallee(callee).usr;
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
          lambdaIdent(lambdaQualifiedName(sm_, le->getBeginLoc(), enclosing))
              .usr;
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

  addConcreteTypeEdges(caller.usr, recordDecl, decl->getLocation());
  return true;
}

void CallGraphEdgeVisitor::addConcreteTypeEdges(
    const std::string &callerUsr, const clang::CXXRecordDecl *cls,
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
        graph_.addEdge({callerUsr, identForCallee(method).usr,
                        EdgeKind::VirtualDispatch, Confidence::Proven, site,
                        0}, tuPath_);
      }
    }

    // Also add destructor edges.
    if (auto *dtor = current->getDestructor()) {
      Ident dtorId = identForCallee(dtor);
      if (!handledMethodNames.count("~")) {
        CallGraphNode dtorNode;
        dtorNode.usr = dtorId.usr;
        dtorNode.qualifiedName = dtorId.display;
        const clang::SourceLocation dtorLoc =
            dtor->getFirstDecl()->getLocation();
        dtorNode.file = getFilePath(dtorLoc);
        dtorNode.line = sm_.getSpellingLineNumber(dtorLoc);
        dtorNode.isVirtual = dtor->isVirtual();
        dtorNode.enclosingClass = current->getQualifiedNameAsString();
        graph_.addNode(std::move(dtorNode), tuPath_);
        graph_.addEdge({callerUsr, dtorId.usr, EdgeKind::DestructorCall,
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

  Ident caller = getCurrentFunction();
  if (caller.display.empty()) {
    scheduleOutsideFunctions(funcDecl, expr->getBeginLoc());
    return true;
  }

  if (!recordsCallAt(expr->getBeginLoc()))
    return true;

  // Skip internal edges within collapsed paths.
  if (!funcStack_.empty() &&
      isCollapsedEdge(funcStack_.back()->getLocation(), funcDecl->getLocation()))
    return true;

  // This is a function reference in a non-call, non-argument context.
  // Treat as address-taken: Plausible edge.
  graph_.addEdge({caller.usr, identForCallee(funcDecl).usr,
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

// Index + edges on ONE frontend parse per TU. Safe because edge building
// has no cross-TU reads: the virtual-dispatch fan-out (task: F3) and the
// function-pointer-through-return join (FunctionPointerReturn) are both
// deferred to query time. The indexer runs first so same-TU state (nodes,
// hierarchy, function returns) precedes the edge walk, matching the
// single-TU CallGraphBuilderConsumer.
class IndexEdgeConsumer : public clang::ASTConsumer {
public:
  IndexEdgeConsumer(CallGraph &graph, clang::SourceManager &sm,
                    const CollapseFilter *collapse, const std::string &tuPath)
      : indexer_(graph, sm, tuPath), edges_(graph, sm, tuPath) {
    edges_.setCollapseFilter(collapse);
  }
  void HandleTranslationUnit(clang::ASTContext &ctx) override {
    indexer_.setASTContext(&ctx);
    indexer_.TraverseDecl(ctx.getTranslationUnitDecl());
    edges_.setASTContext(&ctx);
    edges_.TraverseDecl(ctx.getTranslationUnitDecl());
  }

private:
  CallGraphIndexerVisitor indexer_;
  CallGraphEdgeVisitor edges_;
};

class IndexEdgeAction : public clang::ASTFrontendAction {
public:
  IndexEdgeAction(CallGraph &g, const CollapseFilter *collapse,
                  const std::string &tuPath)
      : graph_(g), collapse_(collapse), tuPath_(tuPath) {}
  std::unique_ptr<clang::ASTConsumer>
  CreateASTConsumer(clang::CompilerInstance &ci, llvm::StringRef) override {
    return std::make_unique<IndexEdgeConsumer>(graph_, ci.getSourceManager(),
                                               collapse_, tuPath_);
  }

private:
  CallGraph &graph_;
  const CollapseFilter *collapse_;
  std::string tuPath_;
};

class IndexEdgeFactory : public clang::tooling::FrontendActionFactory {
public:
  IndexEdgeFactory(CallGraph &g, const CollapseFilter *collapse,
                   const std::string &tuPath)
      : graph_(g), collapse_(collapse), tuPath_(tuPath) {}
  std::unique_ptr<clang::FrontendAction> create() override {
    return std::make_unique<IndexEdgeAction>(graph_, collapse_, tuPath_);
  }

private:
  CallGraph &graph_;
  const CollapseFilter *collapse_;
  std::string tuPath_;
};

// Index one TU into a TU-local graph under the crash guard and absorb it
// into `graph` only after the parse returned. A crashed parse's local
// graph may be torn (its lock held), so it is leaked, never merged: the TU
// adds no partial facts and no shared lock is held by a crashed thread.
int indexTuLocally(CallGraph &graph,
                   const clang::tooling::CompilationDatabase &compDb,
                   const std::string &file, const CollapseFilter *collapse,
                   const PchCache *pchCache, const std::string &sysroot) {
  auto local = std::make_unique<CallGraph>();
  IndexEdgeFactory factory(*local, collapse, file);
  int status = runToolGuarded(compDb, file, factory, pchCache, sysroot);
  if (status == -1) {
    retainUntilExit(local.release());
    return status;
  }
  if (status == 1)
    g_parseErrorCount.fetch_add(1, std::memory_order_relaxed);
  graph.absorb(*local);
  return status;
}

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

  CrashGuardScope crashGuard;
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

    // Single pass: index + edges share one frontend parse per TU. Edge
    // building has no cross-TU reads (virtual dispatch and function-return
    // joins are query-time), so no phase barrier is needed.
    for (const auto &file : files) {
      pool.async([&compDb, &graph, collapsePtr, pchCache, &sysroot, file]() {
        indexTuLocally(graph, compDb, file, collapsePtr, pchCache, sysroot);
      });
    }
    pool.wait();
  } else {
    // Serial path — process per-file for crash isolation.
    for (const auto &file : files) {
      indexTuLocally(graph, compDb, file, collapsePtr, pchCache, sysroot);
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
                 << files.size() << " TU parse(s) reported errors — the "
                 << "index may be missing nodes/edges. Check include paths "
                 << "(--extra-arg can inject e.g. --gcc-install-dir=...)\n";
  }

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

  CrashGuardScope crashGuard;
  g_crashCount.store(0, std::memory_order_relaxed);

  indexTuLocally(graph, compDb, file, collapsePtr, pchCache, sysroot);
}

} // namespace vycor
