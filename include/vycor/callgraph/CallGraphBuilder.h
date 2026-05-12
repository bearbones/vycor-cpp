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

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Tooling/Tooling.h"

#include <memory>
#include <set>
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
  bool TraverseFunctionDecl(clang::FunctionDecl *decl);
  bool TraverseCXXMethodDecl(clang::CXXMethodDecl *decl);
  bool TraverseCXXConstructorDecl(clang::CXXConstructorDecl *decl);
  bool TraverseCXXDestructorDecl(clang::CXXDestructorDecl *decl);
  bool TraverseLambdaExpr(clang::LambdaExpr *expr);
  bool VisitLambdaExpr(clang::LambdaExpr *expr);
  bool VisitReturnStmt(clang::ReturnStmt *stmt);

private:
  CallGraph &graph_;
  clang::SourceManager &sm_;
  clang::ASTContext *ctx_ = nullptr;
  std::string tuPath_;
  std::vector<clang::FunctionDecl *> funcStack_;

  std::string getFilePath(clang::SourceLocation loc) const;
  std::string formatLocation(clang::SourceLocation loc) const;
  std::string getCurrentFunction() const;
  void computeEffectiveImpls(const clang::CXXRecordDecl *cls);
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
  bool TraverseLambdaExpr(clang::LambdaExpr *expr);

  bool VisitCallExpr(clang::CallExpr *expr);
  bool VisitCXXConstructExpr(clang::CXXConstructExpr *expr);
  bool VisitVarDecl(clang::VarDecl *decl);
  bool VisitDeclRefExpr(clang::DeclRefExpr *expr);

private:
  CallGraph &graph_;
  clang::SourceManager &sm_;
  clang::ASTContext *ctx_ = nullptr;
  std::string tuPath_;
  const CollapseFilter *collapse_ = nullptr;
  std::vector<clang::FunctionDecl *> funcStack_;

  // Track DeclRefExprs that are direct callees or function-pointer arguments,
  // so VisitDeclRefExpr can add Plausible edges only for uncovered refs.
  std::set<const clang::DeclRefExpr *> handledRefs_;

  // Track vars assigned from functions that return function pointers.
  std::map<const clang::VarDecl *, std::set<std::string>> varFuncSources_;

  // Track local vars initialized from a LambdaExpr so concurrency spawners
  // and downstream callers can resolve the synthetic lambda name.
  std::map<const clang::VarDecl *, std::string> varLambdaSources_;

  std::string getFilePath(clang::SourceLocation loc) const;
  std::string formatLocation(clang::SourceLocation loc) const;
  std::string getCurrentFunction() const;
  bool isInUserCode(clang::SourceLocation loc) const;
  bool isCollapsedEdge(clang::SourceLocation callerLoc,
                       clang::SourceLocation calleeLoc) const;

  void handleVirtualDispatch(const std::string &caller,
                             clang::CXXMethodDecl *method,
                             clang::SourceLocation loc);
  void addConcreteTypeEdges(const std::string &caller,
                            const clang::CXXRecordDecl *cls,
                            clang::SourceLocation loc);

  // Shared argument scan for both CallExpr and CXXConstructExpr spawners.
  // Emits edges from `caller` for every argument that resolves to a callable
  // (FunctionDecl, LambdaExpr, or a local var tracked in varFuncSources_ /
  // varLambdaSources_). If `spawnerCtx` is Synchronous, emits FunctionPointer
  // edges; otherwise emits ThreadEntry edges with the given context.
  void processCallableArgs(llvm::ArrayRef<clang::Expr *> args,
                           const std::string &caller,
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
