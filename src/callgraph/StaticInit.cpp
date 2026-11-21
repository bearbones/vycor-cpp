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
#include "vycor/callgraph/StaticInit.h"

#include "vycor/callgraph/UsrIdent.h"
#include "vycor/callgraph/Utf8.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/ASTLambda.h"
#include "clang/AST/Attr.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/SourceManager.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <tuple>

namespace vycor {

namespace {

// Where a root is written, given its file location: absolute with dots
// removed, the spelling TU paths have. A header reached as
// "plugins/../core/registry.hpp" from one TU and "core/registry.hpp" from
// another is one file, and one root.
std::string rootFilePathOf(const clang::SourceManager &sm,
                           clang::SourceLocation fileLoc) {
  auto entry = sm.getFileEntryRefForID(sm.getFileID(fileLoc));
  if (!entry)
    return "<unknown>";
  llvm::SmallString<256> path(entry->getName());
  sm.getFileManager().makeAbsolutePath(path);
  llvm::sys::path::remove_dots(path, /*remove_dot_dot=*/true);
  return std::string(path);
}

// The graph's identity for a lambda at namespace scope. Must stay the
// spelling CallGraphBuilder gives a lambda with no enclosing function
// ("vycor-lambda:lambda#file:line:col#<tu>"), or a lambda in an
// initializer finds no node; tests/test_static_init_facts.cpp pins it.
Ident namespaceScopeLambdaIdent(const clang::SourceManager &sm,
                                clang::SourceLocation loc) {
  auto sLoc = sm.getSpellingLoc(loc);
  llvm::SmallString<160> buf;
  llvm::raw_svector_ostream os(buf);
  os << "lambda#" << sm.getFilename(sLoc) << ':'
     << sm.getSpellingLineNumber(sLoc) << ':'
     << sm.getSpellingColumnNumber(sLoc) << "#<tu>";
  Ident id;
  id.display = std::string(buf.str());
  id.usr = "vycor-lambda:" + id.display;
  return id;
}

// Direct callees of one initializer expression.
class InitCallCollector
    : public clang::RecursiveASTVisitor<InitCallCollector> {
public:
  InitCallCollector(const clang::SourceManager &sm, UsrCache &usrs,
                    std::vector<StaticInitCallee> &out)
      : sm_(sm), usrs_(usrs), out_(out) {}

  bool VisitCallExpr(clang::CallExpr *expr) {
    if (const clang::FunctionDecl *callee = expr->getDirectCallee())
      addCallee(callee);
    return true;
  }

  bool VisitCXXConstructExpr(clang::CXXConstructExpr *expr) {
    addCallee(expr->getConstructor());
    return true;
  }

  bool VisitCXXNewExpr(clang::CXXNewExpr *expr) {
    if (const clang::FunctionDecl *alloc = expr->getOperatorNew())
      addCallee(alloc);
    return true;
  }

  // RecursiveASTVisitor leaves these to shouldVisitImplicitCode; the
  // calls in a default argument or a default member initializer run as
  // part of this initializer.
  bool VisitCXXDefaultArgExpr(clang::CXXDefaultArgExpr *expr) {
    return TraverseStmt(expr->getExpr());
  }
  bool VisitCXXDefaultInitExpr(clang::CXXDefaultInitExpr *expr) {
    return TraverseStmt(expr->getExpr());
  }

  // A lambda written in an initializer is taken as called by it: the
  // graph holds the body's calls under the lambda's own node. The body is
  // not walked here; the captures' initializers are.
  bool TraverseLambdaExpr(clang::LambdaExpr *expr) {
    Ident id = namespaceScopeLambdaIdent(sm_, expr->getBeginLoc());
    out_.push_back({std::move(id.usr), std::move(id.display), {}});
    for (clang::Expr *init : expr->capture_inits())
      if (init && !TraverseStmt(init))
        return false;
    return true;
  }

private:
  void addCallee(const clang::FunctionDecl *callee) {
    if (!callee)
      return;
    // The call of a lambda's operator() is covered by the lambda itself.
    if (auto *method = llvm::dyn_cast<clang::CXXMethodDecl>(callee))
      if (clang::isLambdaCallOperator(method))
        return;
    // A compiler-generated constructor has no graph node and no written
    // body: look through it to what its member and base initializers run.
    if (auto *ctor = llvm::dyn_cast<clang::CXXConstructorDecl>(callee)) {
      if (ctor->isDefaulted()) {
        if (seenDefaulted_.insert(ctor->getCanonicalDecl()).second) {
          const clang::FunctionDecl *def = nullptr;
          const auto *defined = ctor->hasBody(def)
                                    ? llvm::cast<clang::CXXConstructorDecl>(def)
                                    : ctor;
          for (const clang::CXXCtorInitializer *init : defined->inits())
            if (init->getInit())
              TraverseStmt(init->getInit());
        }
        return;
      }
    }
    if (const clang::IdentifierInfo *ii = callee->getIdentifier())
      if (ii->getName().starts_with("__builtin_"))
        return;
    const Ident &id = usrs_.identFor(callee);
    StaticInitCallee rec{id.usr, id.display, {}};
    if (const clang::FunctionDecl *pattern =
            callee->getTemplateInstantiationPattern())
      if (pattern != callee)
        rec.pattern = usrs_.identFor(pattern).usr;
    out_.push_back(std::move(rec));
  }

  const clang::SourceManager &sm_;
  UsrCache &usrs_;
  std::vector<StaticInitCallee> &out_;
  llvm::SmallPtrSet<const clang::Decl *, 8> seenDefaulted_;
};

class RootCollector {
public:
  RootCollector(clang::ASTContext &ctx, const std::string &tuPath,
                StaticInitFacts &out)
      : ctx_(ctx), sm_(ctx.getSourceManager()), tuPath_(tuPath), out_(out) {}

  // Namespace- and class-scope declarations only: function bodies hold no
  // roots, so the walk is over declaration contexts, not statements.
  void walk(const clang::DeclContext *dc) {
    for (const clang::Decl *decl : dc->decls()) {
      if (auto *var = llvm::dyn_cast<clang::VarDecl>(decl)) {
        variable(var);
      } else if (auto *fn = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
        function(fn);
      } else if (auto *record = llvm::dyn_cast<clang::CXXRecordDecl>(decl)) {
        if (record->isThisDeclarationADefinition() &&
            !record->isDependentContext())
          walk(record);
      } else if (llvm::isa<clang::NamespaceDecl>(decl) ||
                 llvm::isa<clang::LinkageSpecDecl>(decl) ||
                 llvm::isa<clang::ExportDecl>(decl)) {
        walk(llvm::cast<clang::DeclContext>(decl));
      }
    }
  }

private:
  // A declaration a macro produced is where the macro is used: the name
  // of `STATISTIC(NumHits, ...)` is written at the use, and a name pasted
  // together in a macro body is written nowhere in a file.
  clang::SourceLocation fileLoc(const clang::Decl *decl) const {
    return sm_.getFileLoc(decl->getLocation());
  }

  bool inSystemHeader(const clang::Decl *decl) const {
    clang::SourceLocation loc = fileLoc(decl);
    return loc.isInvalid() || sm_.isInSystemHeader(loc);
  }

  StaticInitRoot &addRoot(StaticInitKind kind, const clang::NamedDecl *decl) {
    const Ident &id = usrs_.identFor(decl);
    StaticInitRoot root;
    root.kind = kind;
    root.name = id.display;
    root.usr = id.usr;
    const clang::SourceLocation at = fileLoc(decl);
    root.file = rootFilePathOf(sm_, at);
    root.line = sm_.getSpellingLineNumber(at);
    root.tus.push_back(tuPath_);
    out_.roots.push_back(std::move(root));
    return out_.roots.back();
  }

  // The rule of check's static-init index (check/Indexer.cpp), minus
  // thread-locals, which initialize on first use.
  void variable(const clang::VarDecl *var) {
    if (llvm::isa<clang::ParmVarDecl>(var) || !var->hasGlobalStorage() ||
        var->isStaticLocal() || var->getTLSKind() != clang::VarDecl::TLS_None)
      return;
    if (var->isThisDeclarationADefinition() != clang::VarDecl::Definition ||
        !var->hasInit())
      return;
    if (var->getType()->isDependentType() || var->getDescribedVarTemplate() ||
        var->getDeclContext()->isDependentContext() ||
        llvm::isa<clang::VarTemplatePartialSpecializationDecl>(var))
      return;
    if (var->getTemplateSpecializationKind() ==
        clang::TSK_ImplicitInstantiation)
      return;
    const clang::Expr *init = var->getInit();
    if (init->isInstantiationDependent() || inSystemHeader(var))
      return;
    if (var->isConstexpr() || var->hasAttr<clang::ConstInitAttr>() ||
        init->isConstantInitializer(ctx_, var->getType()->isReferenceType()))
      return;
    StaticInitRoot &root = addRoot(StaticInitKind::Variable, var);
    InitCallCollector calls(sm_, usrs_, root.callees);
    calls.TraverseStmt(const_cast<clang::Expr *>(init));
  }

  void function(const clang::FunctionDecl *fn) {
    if (!fn->hasAttr<clang::ConstructorAttr>() ||
        !fn->isThisDeclarationADefinition() || fn->isTemplated() ||
        inSystemHeader(fn))
      return;
    addRoot(StaticInitKind::ConstructorFunction, fn);
  }

  clang::ASTContext &ctx_;
  const clang::SourceManager &sm_;
  const std::string &tuPath_;
  StaticInitFacts &out_;
  UsrCache usrs_;
};

template <typename T, typename Less, typename Same>
void sortUnique(std::vector<T> &v, Less less, Same same) {
  std::sort(v.begin(), v.end(), less);
  v.erase(std::unique(v.begin(), v.end(), same), v.end());
}

void normalizeLists(StaticInitRoot &root) {
  std::sort(root.tus.begin(), root.tus.end());
  root.tus.erase(std::unique(root.tus.begin(), root.tus.end()),
                 root.tus.end());
  sortUnique(
      root.callees,
      [](const StaticInitCallee &a, const StaticInitCallee &b) {
        return std::tie(a.usr, a.pattern) < std::tie(b.usr, b.pattern);
      },
      [](const StaticInitCallee &a, const StaticInitCallee &b) {
        return a.usr == b.usr;
      });
}

} // namespace

void collectStaticInits(clang::ASTContext &ctx, const std::string &tuPath,
                        StaticInitFacts &out) {
  size_t first = out.roots.size();
  RootCollector collector(ctx, tuPath, out);
  collector.walk(ctx.getTranslationUnitDecl());
  // Stored as index text (Utf8.h), like every string the indexes hold.
  for (size_t i = first; i < out.roots.size(); ++i) {
    StaticInitRoot &root = out.roots[i];
    makeIndexText(root.name);
    makeIndexText(root.usr);
    makeIndexText(root.file);
    for (auto &tu : root.tus)
      makeIndexText(tu);
    for (auto &callee : root.callees) {
      makeIndexText(callee.usr);
      makeIndexText(callee.display);
      makeIndexText(callee.pattern);
    }
  }
  out.collected = true;
}

void StaticInitFacts::canonicalize() {
  // Group the records of one initializer and fold each group into its
  // first record. An initializer is its usr at its location: a usr alone
  // does not identify an internal-linkage variable, which shares it with
  // a same-named variable in any other file of the same basename.
  auto key = [](const StaticInitRoot &root) {
    return std::tie(root.usr, root.file, root.line);
  };
  std::stable_sort(roots.begin(), roots.end(),
                   [&](const StaticInitRoot &a, const StaticInitRoot &b) {
                     return key(a) < key(b);
                   });
  size_t kept = 0;
  for (size_t i = 0; i < roots.size(); ++i) {
    if (kept > 0 && key(roots[kept - 1]) == key(roots[i])) {
      StaticInitRoot &into = roots[kept - 1];
      into.tus.insert(into.tus.end(),
                      std::make_move_iterator(roots[i].tus.begin()),
                      std::make_move_iterator(roots[i].tus.end()));
      into.callees.insert(into.callees.end(),
                          std::make_move_iterator(roots[i].callees.begin()),
                          std::make_move_iterator(roots[i].callees.end()));
      continue;
    }
    if (kept != i)
      roots[kept] = std::move(roots[i]);
    ++kept;
  }
  roots.resize(kept);
  for (StaticInitRoot &root : roots)
    normalizeLists(root);
  std::sort(roots.begin(), roots.end(),
            [](const StaticInitRoot &a, const StaticInitRoot &b) {
              return std::tie(a.file, a.line, a.name, a.usr) <
                     std::tie(b.file, b.line, b.name, b.usr);
            });
}

} // namespace vycor
