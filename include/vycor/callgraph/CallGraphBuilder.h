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
#include "vycor/callgraph/CollapseFilter.h"
#include "vycor/callgraph/UsrIdent.h"
#include "vycor/compat/ClangVersion.h"

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"

#include <memory>
#include <string>
#include <vector>

namespace vycor {

// Phase 1 visitor: indexes declarations, class hierarchy, function returns.
class CallGraphIndexerVisitor
    : public clang::RecursiveASTVisitor<CallGraphIndexerVisitor> {
public:
  CallGraphIndexerVisitor(CallGraph &graph, clang::SourceManager &sm,
                          const std::string &tuPath = "");

  void setASTContext(clang::ASTContext *ctx) { ctx_ = ctx; }

  bool VisitFunctionDecl(clang::FunctionDecl *decl);
  bool VisitCXXRecordDecl(clang::CXXRecordDecl *decl);
  bool VisitClassTemplateDecl(clang::ClassTemplateDecl *decl);
  bool TraverseFunctionDecl(clang::FunctionDecl *decl);
  bool TraverseCXXMethodDecl(clang::CXXMethodDecl *decl);
  bool TraverseCXXConstructorDecl(clang::CXXConstructorDecl *decl);
  bool TraverseCXXDestructorDecl(clang::CXXDestructorDecl *decl);
  bool TraverseCXXConversionDecl(clang::CXXConversionDecl *decl);
  bool TraverseLambdaExpr(clang::LambdaExpr *expr);
  bool VisitLambdaExpr(clang::LambdaExpr *expr);
  bool VisitReturnStmt(clang::ReturnStmt *stmt);

private:
  CallGraph &graph_;
  clang::SourceManager &sm_;
  clang::ASTContext *ctx_ = nullptr;
  std::string tuPath_;
  std::vector<clang::FunctionDecl *> funcStack_;
  // Per-TU (usr, display) memo; getCurrentFunction is const but memoizes.
  mutable UsrCache usrs_;

  std::string getFilePath(clang::SourceLocation loc) const;
  std::string formatLocation(clang::SourceLocation loc) const;
  Ident getCurrentFunction() const;
  void computeEffectiveImpls(const clang::CXXRecordDecl *cls);
  void recordClass(const clang::CXXRecordDecl *decl);
};

// Phase 2 visitor: builds call edges.
class CallGraphEdgeVisitor
    : public clang::RecursiveASTVisitor<CallGraphEdgeVisitor> {
public:
  CallGraphEdgeVisitor(CallGraph &graph, clang::SourceManager &sm,
                       const std::string &tuPath = "");

  void setASTContext(clang::ASTContext *ctx) { ctx_ = ctx; }
  void setCollapseFilter(const CollapseFilter *filter) { collapse_ = filter; }

  bool TraverseFunctionDecl(clang::FunctionDecl *decl);
  bool TraverseCXXMethodDecl(clang::CXXMethodDecl *decl);
  bool TraverseCXXConstructorDecl(clang::CXXConstructorDecl *decl);
  bool TraverseCXXDestructorDecl(clang::CXXDestructorDecl *decl);
  bool TraverseCXXConversionDecl(clang::CXXConversionDecl *decl);
  bool TraverseLambdaExpr(clang::LambdaExpr *expr);
  // The TU as written, then the bodies it reaches that no written
  // definition holds (drainBodies).
  bool TraverseTranslationUnitDecl(clang::TranslationUnitDecl *decl);
  bool TraverseCXXDefaultInitExpr(clang::CXXDefaultInitExpr *expr);
  bool TraverseInitListExpr(clang::InitListExpr *expr);
  bool TraverseLambdaCapture(clang::LambdaExpr *expr,
                             const clang::LambdaCapture *capture,
                             clang::Expr *init);
  // Operands that are never evaluated call nothing: not traversed.
  bool TraverseUnaryExprOrTypeTraitExpr(clang::UnaryExprOrTypeTraitExpr *expr);
  bool TraverseCXXNoexceptExpr(clang::CXXNoexceptExpr *expr);
  bool TraverseRequiresExpr(clang::RequiresExpr *expr);
  bool TraverseConceptDecl(clang::ConceptDecl *decl);
#if VYCOR_LLVM_AT_LEAST(22)
  bool TraverseDecltypeTypeLoc(clang::DecltypeTypeLoc loc, bool qualifier);
  bool TraverseTypeOfExprTypeLoc(clang::TypeOfExprTypeLoc loc, bool qualifier);
#else
  bool TraverseDecltypeTypeLoc(clang::DecltypeTypeLoc loc);
  bool TraverseTypeOfExprTypeLoc(clang::TypeOfExprTypeLoc loc);
#endif

  bool VisitCallExpr(clang::CallExpr *expr);
  bool VisitCXXConstructExpr(clang::CXXConstructExpr *expr);
  bool VisitCXXInheritedCtorInitExpr(clang::CXXInheritedCtorInitExpr *expr);
  bool VisitCXXBindTemporaryExpr(clang::CXXBindTemporaryExpr *expr);
  bool VisitCXXNewExpr(clang::CXXNewExpr *expr);
  bool VisitCXXDeleteExpr(clang::CXXDeleteExpr *expr);
  bool VisitCXXThrowExpr(clang::CXXThrowExpr *expr);
  bool VisitVarDecl(clang::VarDecl *decl);
  bool VisitDeclRefExpr(clang::DeclRefExpr *expr);
  // Explicit instantiation definitions: the TU emits their bodies.
  bool VisitClassTemplateDecl(clang::ClassTemplateDecl *decl);
  bool VisitFunctionTemplateDecl(clang::FunctionTemplateDecl *decl);

private:
  CallGraph &graph_;
  clang::SourceManager &sm_;
  clang::ASTContext *ctx_ = nullptr;
  std::string tuPath_;
  const CollapseFilter *collapse_ = nullptr;
  std::vector<clang::FunctionDecl *> funcStack_;

  // Track DeclRefExprs that are direct callees or function-pointer arguments,
  // so VisitDeclRefExpr can add Plausible edges only for uncovered refs.
  llvm::DenseSet<const clang::DeclRefExpr *> handledRefs_;

  // Track callable-typed vars initialized from a direct call, keyed to the
  // *returning* function's USR. Consumed by processCallableArgs as deferred
  // FunctionPointerReturn edges; the function-returns join happens at query
  // time (CallGraph::calleesOf/callersOf), so edge building never reads
  // cross-TU state.
  llvm::DenseMap<const clang::VarDecl *, std::string> varCallSources_;

  // Track local vars initialized from a LambdaExpr so concurrency spawners
  // and downstream callers can resolve the synthetic lambda USR.
  llvm::DenseMap<const clang::VarDecl *, std::string> varLambdaSources_;

  // Per-TU (usr, display) memo; getCurrentFunction is const but memoizes.
  mutable UsrCache usrs_;

  // Callee decls whose node this visitor registered (see identForCallee).
  llvm::DenseSet<const clang::FunctionDecl *> auxNodes_;

  // Bodies to read after the TU as written: template instantiations and
  // compiler-written special members the TU's code reaches (scheduleBody).
  // Each is read once per TU, under its own identity; reading one may
  // schedule others. Keyed by the definition read.
  std::vector<const clang::FunctionDecl *> work_;
  llvm::DenseSet<const clang::FunctionDecl *> scheduled_;
  // The bodies already read, a subset of scheduled_.
  llvm::DenseSet<const clang::FunctionDecl *> read_;
  // True while drainBodies reads a scheduled body: its calls are recorded
  // wherever they are spelled, a system header included.
  bool inCompilerCode_ = false;
  // Depth of default member initializers being read where they run (a
  // constructor, an aggregate initialization): a lambda in one was read
  // where it is written.
  unsigned inDefaultInit_ = 0;
  // Whether this TU has registered the node of the throw / rethrow entry
  // point (throwCalleeIdent), indexed by `rethrow`.
  bool throwNodes_[2] = {false, false};

  std::string getFilePath(clang::SourceLocation loc) const;
  std::string formatLocation(clang::SourceLocation loc) const;
  Ident getCurrentFunction() const;

  // identFor + node registration for callees the indexer never visits
  // (implicit members, template instantiations — RecursiveASTVisitor skips
  // instantiations), so display-name resolution can find their edges. A
  // lambda's call operator answers the lambda's identity (lambdaIdentOf).
  // Schedules the callee's body when no written definition holds it
  // (scheduleBody).
  Ident identForCallee(const clang::FunctionDecl *decl);
  // The node of a callee: its identity, declaration site, class.
  CallGraphNode calleeNode(const clang::FunctionDecl *decl, const Ident &id);
  // The identity the graph gives a lambda (lambdaQualifiedName), named by
  // the function its closure is declared in.
  Ident lambdaIdentOf(const clang::CXXRecordDecl *closure) const;
  // The node of a lambda whose body this visitor reads.
  void addLambdaNode(const clang::CXXRecordDecl *closure, const Ident &id,
                     llvm::StringRef enclosing);

  bool isInUserCode(clang::SourceLocation loc) const;
  // Whether a call spelled at `loc` is recorded: in user code, or anywhere
  // inside a scheduled body.
  bool recordsCallAt(clang::SourceLocation loc) const {
    return inCompilerCode_ || isInUserCode(loc);
  }

  // Worklist of bodies (see work_).
  void scheduleBody(const clang::FunctionDecl *fn);
  void scheduleOutsideFunctions(const clang::FunctionDecl *fn,
                                clang::SourceLocation loc);
  void drainBodies();
  void readBody(const clang::FunctionDecl *fn);
  // The code the compiler writes into a function definition: member and
  // base initializers not written, a defaulted body, and the destruction
  // of members and bases after a destructor's body.
  void readCompilerWritten(clang::FunctionDecl *fn);
  void addMemberDestruction(const clang::CXXDestructorDecl *dtor);
  // A DestructorCall edge to the destructor of `type` (an array's element
  // type) when it is not trivial.
  void addDestructorEdge(clang::QualType type, clang::SourceLocation site);
  // An edge for a call no CallExpr spells (a constructor, destructor,
  // allocation or deallocation function), from the current function.
  void addImplicitCall(const clang::FunctionDecl *fn, EdgeKind kind,
                       Confidence confidence, clang::SourceLocation loc);
  bool isCollapsedEdge(clang::SourceLocation callerLoc,
                       clang::SourceLocation calleeLoc) const;


  void handleVirtualDispatch(const std::string &callerUsr,
                             clang::CXXMethodDecl *method,
                             clang::SourceLocation loc);
  void addConcreteTypeEdges(const std::string &callerUsr,
                            const clang::CXXRecordDecl *cls,
                            clang::SourceLocation loc);

  // Shared argument scan for both CallExpr and CXXConstructExpr spawners.
  // Emits edges from `callerUsr` for every argument that resolves to a
  // callable (FunctionDecl, LambdaExpr, or a local var tracked in
  // varCallSources_ / varLambdaSources_). If `spawnerCtx` is Synchronous,
  // emits FunctionPointer edges; otherwise emits ThreadEntry edges with the
  // given context.
  void processCallableArgs(llvm::ArrayRef<clang::Expr *> args,
                           const std::string &callerUsr,
                           clang::SourceLocation callSite,
                           ExecutionContext spawnerCtx);

  // Resolve the enclosing non-lambda function's qualified name (for synthetic
  // lambda naming).
  std::string enclosingNonLambdaName() const;

};

// Consumer that runs both phases per TU (for single-TU tests).
class CallGraphBuilderConsumer : public clang::ASTConsumer {
public:
  CallGraphBuilderConsumer(CallGraph &graph, clang::SourceManager &sm);
  void HandleTranslationUnit(clang::ASTContext &context) override;

private:
  CallGraphIndexerVisitor indexer_;
  CallGraphEdgeVisitor edgeBuilder_;
};

// FrontendAction for combined builder.
class CallGraphBuilderAction : public clang::ASTFrontendAction {
public:
  explicit CallGraphBuilderAction(CallGraph &graph);
  std::unique_ptr<clang::ASTConsumer>
  CreateASTConsumer(clang::CompilerInstance &ci, llvm::StringRef file) override;

private:
  CallGraph &graph_;
};

// Factory for single-TU usage and runToolOnCodeWithArgs.
class CallGraphBuilderFactory : public clang::tooling::FrontendActionFactory {
public:
  explicit CallGraphBuilderFactory(CallGraph &graph);
  std::unique_ptr<clang::FrontendAction> create() override;

private:
  CallGraph &graph_;
};

class PchCache;

// Build a call graph from a compilation database (multi-TU, two-pass).
// If collapsePaths is non-empty, internal edges within collapsed paths are
// skipped (boundary edges from non-collapsed callers are preserved).
// threadCount=0 uses hardware_concurrency; threadCount=1 forces serial.
// pchCache, if non-null, provides compiled PCH binaries for faster parsing.
CallGraph buildCallGraph(const clang::tooling::CompilationDatabase &compDb,
                         const std::vector<std::string> &files,
                         const std::vector<std::string> &collapsePaths = {},
                         unsigned threadCount = 0,
                         const PchCache *pchCache = nullptr,
                         const std::string &sysroot = "");

// Index a single TU into an existing graph (Phase 1 + Phase 2).
// Call graph.removeTU(file) first when re-indexing a changed file.
void indexTU(CallGraph &graph,
             const clang::tooling::CompilationDatabase &compDb,
             const std::string &file,
             const std::vector<std::string> &collapsePaths = {},
             const PchCache *pchCache = nullptr,
             const std::string &sysroot = "");

} // namespace vycor
