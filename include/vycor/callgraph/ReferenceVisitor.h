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

#include "vycor/callgraph/ReferenceIndex.h"
#include "vycor/callgraph/UsrIdent.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/SourceManager.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"

#include <string>
#include <vector>

namespace vycor {

// ============================================================================
// ReferenceVisitor — the bake's fourth traversal of a TU's AST (same parse
// as the call-graph and control-flow visitors). It records which function,
// variable initializer or file scope references which function, global
// variable or class, and how (RefRole), plus the linkage facts of every
// symbol involved (ReferenceIndex.h; docs/symbol-references.md).
//
// Cost model. During the traversal an occurrence is one hash-map bump keyed
// by (referrer slot, target slot, role): no string is built and nothing is
// allocated per occurrence. USRs, names, files and attributes are computed
// once per entity when the TU is lowered into the index (run()).
//
// Like the other bake visitors it reads the code as written: template
// patterns, not instantiations, and no implicit code. A reference to an
// instantiation is recorded against its template pattern.
// ============================================================================
class ReferenceVisitor : public clang::RecursiveASTVisitor<ReferenceVisitor> {
  using Base = clang::RecursiveASTVisitor<ReferenceVisitor>;

public:
  ReferenceVisitor(ReferenceIndex &index, clang::SourceManager &sm,
                   const std::string &tuPath);

  // Traverse the TU and register its facts with the index under tuPath.
  void run(clang::ASTContext &ctx);

  bool TraverseDecl(clang::Decl *decl);
  bool TraverseLambdaExpr(clang::LambdaExpr *expr);
  bool TraverseInitListExpr(clang::InitListExpr *expr);
  bool TraverseUnaryExprOrTypeTraitExpr(clang::UnaryExprOrTypeTraitExpr *expr);
  bool TraverseCXXNoexceptExpr(clang::CXXNoexceptExpr *expr);
  bool TraverseCXXTypeidExpr(clang::CXXTypeidExpr *expr);
  bool TraverseRequiresExpr(clang::RequiresExpr *expr);
  bool TraverseConceptDecl(clang::ConceptDecl *decl);

  bool VisitDeclRefExpr(clang::DeclRefExpr *expr);
  bool VisitMemberExpr(clang::MemberExpr *expr);
  bool VisitOverloadExpr(clang::OverloadExpr *expr);
  bool VisitCallExpr(clang::CallExpr *expr);
  bool VisitCXXConstructExpr(clang::CXXConstructExpr *expr);
  bool VisitImplicitCastExpr(clang::ImplicitCastExpr *expr);
  bool VisitBinaryOperator(clang::BinaryOperator *expr);
  bool VisitUnaryOperator(clang::UnaryOperator *expr);
  bool VisitCXXForRangeStmt(clang::CXXForRangeStmt *stmt);
  bool VisitCXXRewrittenBinaryOperator(clang::CXXRewrittenBinaryOperator *expr);
  bool VisitBindingDecl(clang::BindingDecl *decl);
  bool VisitCXXDeleteExpr(clang::CXXDeleteExpr *expr);
  bool VisitArraySubscriptExpr(clang::ArraySubscriptExpr *expr);
  bool VisitExplicitCastExpr(clang::ExplicitCastExpr *expr);
  bool VisitReturnStmt(clang::ReturnStmt *stmt);
  bool VisitCXXDynamicCastExpr(clang::CXXDynamicCastExpr *expr);
  bool VisitCXXThrowExpr(clang::CXXThrowExpr *expr);
  bool VisitCXXCatchStmt(clang::CXXCatchStmt *stmt);

private:
  using Id = ReferenceIndex::Id;
  static constexpr uint32_t kDropped = UINT32_MAX;
  static constexpr uint32_t kUnresolved = UINT32_MAX - 1;

  enum class EntityKind : uint8_t { Function, Variable, Class, Lambda, File };

  // One referrer or target of this TU. Slots are dense and assigned in
  // traversal order.
  struct Entity {
    const clang::NamedDecl *decl = nullptr; // canonical; Lambda: the call
                                            // operator; File: null
    clang::FileID file;                     // File only
    uint32_t lambdaIdent = 0;               // Lambda: index in lambdaIdents_
    uint32_t flags = 0;                     // symbol flags noted on the way
    EntityKind kind = EntityKind::Function;
    bool emit = false;                      // write symbol records
  };

  enum class FrameKind : uint8_t { Function, Variable, Lambda };
  // The entity references are attributed to while its declaration is being
  // traversed. The slot is resolved on the first reference.
  struct Frame {
    const clang::Decl *decl;
    uint32_t slot;
    FrameKind kind;
    bool dependent; // templated: a return type may not be known yet
  };

  struct RefAcc {
    uint32_t count = 0;
    clang::SourceLocation site; // first occurrence in traversal order
  };

  // Role marks a parent expression sets on the entity-naming expression
  // (DeclRefExpr / MemberExpr / OverloadExpr) at the root of an operand.
  // RecursiveASTVisitor visits parents first, so the mark is there when the
  // naming expression is visited; the outermost mark wins.
  enum : uint8_t {
    kMarkRead = 1,
    kMarkWrite = 2,
    kMarkCallee = 4,
    kMarkAddress = 8,
  };

  // ---- traversal ----------------------------------------------------------
  bool traverseFunction(clang::FunctionDecl *fn);
  bool traverseVariable(clang::VarDecl *var);
  void popFrame();
  void noteFunctionDefinition(const clang::FunctionDecl *fn);
  void noteVariableDefinition(const clang::VarDecl *var);
  void noteClass(const clang::CXXRecordDecl *cls);
  void noteExplicitInstantiations(clang::FunctionTemplateDecl *tmpl);

  void mark(const clang::Expr *expr, uint8_t bits);
  [[nodiscard]] uint8_t marksOf(const clang::Expr *expr) const;
  void markArgument(const clang::FunctionDecl *callee, unsigned param,
                    const clang::Expr *arg);
  void markCallArguments(const clang::CallExpr *expr,
                         const clang::FunctionDecl *fn);
  void markDependentOperands(const clang::Expr *lhs, const clang::Expr *rhs,
                             clang::BinaryOperatorKind op);
  void noteRewrittenCallees(const clang::Expr *expr, const clang::Expr *lhs,
                            const clang::Expr *rhs, clang::SourceLocation loc);
  void noteVariableUse(const clang::VarDecl *var, const clang::Expr *expr,
                       clang::NonOdrUseReason nour, clang::SourceLocation loc);
  void noteFunctionUse(const clang::FunctionDecl *fn, const clang::Expr *expr,
                       clang::NonOdrUseReason nour, clang::SourceLocation loc);
  void noteTypeinfoUse(clang::QualType type, clang::SourceLocation loc);
  void addRef(uint32_t target, RefRole role, clang::SourceLocation site);

  // ---- entities -----------------------------------------------------------
  uint32_t targetSlot(const clang::NamedDecl *decl);
  uint32_t resolveTarget(const clang::NamedDecl *decl);
  uint32_t referrerSlot(clang::SourceLocation site);
  uint32_t resolveFrame(size_t frameIndex);
  uint32_t fileSlot(clang::SourceLocation site);
  [[nodiscard]] bool onlyInSystemHeaders(const clang::Decl *decl) const;
  [[nodiscard]] bool inMainFile(clang::SourceLocation loc) const;

  // ---- classification -----------------------------------------------------
  [[nodiscard]] LinkageClass linkageOf(EntityKind kind,
                                       const clang::NamedDecl *decl) const;
  [[nodiscard]] LinkageClass
  linkageOfFunction(const clang::FunctionDecl *fn) const;
  [[nodiscard]] LinkageClass
  linkageOfVariable(const clang::VarDecl *var) const;
  [[nodiscard]] LinkageClass
  linkageOfClass(const clang::CXXRecordDecl *cls) const;
  [[nodiscard]] uint32_t symbolFlags(const Entity &entity) const;
  [[nodiscard]] uint32_t definitionSize(EntityKind kind,
                                        const clang::Decl *definition) const;
  [[nodiscard]] uint32_t lineSpan(clang::SourceRange range) const;

  // ---- lowering -----------------------------------------------------------
  struct Site {
    Id file = 0;
    uint32_t line = 0;
    uint32_t col = 0;
    bool mainFile = false;
  };
  [[nodiscard]] Site siteOf(clang::SourceLocation loc);
  [[nodiscard]] Id fileId(clang::FileID fid);
  [[nodiscard]] bool filePath(clang::FileID fid,
                              llvm::SmallVectorImpl<char> &path) const;
  void lower();
  [[nodiscard]] Id lowerEntity(const Entity &entity,
                               std::vector<ReferenceIndex::SymbolRecord> &out);

  ReferenceIndex &index_;
  clang::SourceManager &sm_;
  clang::ASTContext *ctx_ = nullptr;
  std::string tuPath_;
  UsrCache usrs_;

  std::vector<Frame> frames_;
  unsigned unevaluated_ = 0;
  llvm::DenseMap<const clang::Expr *, uint8_t> marks_;

  std::vector<Entity> entities_;
  std::vector<Ident> lambdaIdents_;
  // Decl (as referenced, and its canonical pattern) -> slot or kDropped.
  llvm::DenseMap<const clang::Decl *, uint32_t> slotOf_;
  llvm::DenseMap<const clang::Decl *, uint32_t> lambdaSlot_;
  llvm::DenseMap<clang::FileID, uint32_t> fileSlot_;
  llvm::DenseMap<clang::FileID, Id> fileIds_;
  Id unknownFile_ = ReferenceIndex::kNoId;
  // (referrer slot << 35) | (target slot << 3) | role.
  llvm::DenseMap<uint64_t, RefAcc> refs_;
};

} // namespace vycor
