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

#include "vycor/anneal/GlobalIndex.h"

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Tooling/Tooling.h"

#include <memory>

namespace vycor {

// AST visitor that collects function overloads and deduction guides
// into a GlobalIndex.
class IndexerVisitor : public clang::RecursiveASTVisitor<IndexerVisitor> {
public:
  IndexerVisitor(GlobalIndex &index, clang::SourceManager &sm);

  bool VisitFunctionDecl(clang::FunctionDecl *decl);
  bool VisitCXXDeductionGuideDecl(clang::CXXDeductionGuideDecl *decl);
  bool VisitCXXMethodDecl(clang::CXXMethodDecl *decl);
  bool VisitCXXRecordDecl(clang::CXXRecordDecl *decl);
  bool VisitCXXConstructorDecl(clang::CXXConstructorDecl *decl);
  bool VisitCXXConversionDecl(clang::CXXConversionDecl *decl);

  void setASTContext(clang::ASTContext *ctx);

private:
  GlobalIndex &index_;
  clang::SourceManager &sm_;
  clang::ASTContext *astContext_ = nullptr;

  std::string getFilePath(clang::SourceLocation loc) const;
  unsigned countStmts(const clang::Stmt *s, unsigned limit = 100) const;
};

// ASTConsumer that drives the IndexerVisitor.
class IndexerConsumer : public clang::ASTConsumer {
public:
  IndexerConsumer(GlobalIndex &index, clang::SourceManager &sm);
  void HandleTranslationUnit(clang::ASTContext &context) override;

private:
  IndexerVisitor visitor_;
};

// FrontendAction that creates an IndexerConsumer.
class IndexerAction : public clang::ASTFrontendAction {
public:
  explicit IndexerAction(GlobalIndex &index);
  std::unique_ptr<clang::ASTConsumer>
  CreateASTConsumer(clang::CompilerInstance &ci, llvm::StringRef file) override;

private:
  GlobalIndex &index_;
};

// Factory for creating IndexerActions, for use with ClangTool.
class IndexerActionFactory : public clang::tooling::FrontendActionFactory {
public:
  explicit IndexerActionFactory(GlobalIndex &index);
  std::unique_ptr<clang::FrontendAction> create() override;

private:
  GlobalIndex &index_;
};

} // namespace vycor
