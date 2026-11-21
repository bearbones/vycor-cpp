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

// The graph's identity for a lambda an initializer evaluates. Must stay
// the spelling CallGraphBuilder gives it
// ("vycor-lambda:lambda#file:line:col#enclosing"), or the lambda finds no
// node; tests/test_static_init_facts.cpp pins it. `enclosing` is the
// function whose declaration the lambda is written in (a default
// argument's), empty at namespace or class scope.
Ident initializerLambdaIdent(const clang::SourceManager &sm,
                             clang::SourceLocation loc,
                             llvm::StringRef enclosing) {
  auto sLoc = sm.getSpellingLoc(loc);
  llvm::SmallString<160> buf;
  llvm::raw_svector_ostream os(buf);
  os << "lambda#" << sm.getFilename(sLoc) << ':'
     << sm.getSpellingLineNumber(sLoc) << ':'
     << sm.getSpellingColumnNumber(sLoc) << '#'
     << (enclosing.empty() ? llvm::StringRef("<tu>") : enclosing);
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

  // The base constructor an inheriting constructor (`using Base::Base;`)
  // forwards to.
  bool VisitCXXInheritedCtorInitExpr(clang::CXXInheritedCtorInitExpr *expr) {
    addCallee(expr->getConstructor());
    return true;
  }

  // RecursiveASTVisitor leaves these to shouldVisitImplicitCode; the
  // calls in a default argument or a default member initializer run as
  // part of this initializer. A lambda in a default argument is the
  // graph's under the function that declares the parameter.
  bool VisitCXXDefaultArgExpr(clang::CXXDefaultArgExpr *expr) {
    std::string outer = std::move(enclosing_);
    enclosing_.clear();
    if (const auto *fn = llvm::dyn_cast_or_null<clang::FunctionDecl>(
            expr->getParam()->getDeclContext())) {
      const auto *method = llvm::dyn_cast<clang::CXXMethodDecl>(fn);
      enclosing_ = method && clang::isLambdaCallOperator(method)
                       ? outer
                       : fn->getQualifiedNameAsString();
    }
    bool ok = TraverseStmt(expr->getExpr());
    enclosing_ = std::move(outer);
    return ok;
  }
  bool VisitCXXDefaultInitExpr(clang::CXXDefaultInitExpr *expr) {
    return TraverseStmt(expr->getExpr());
  }

  // A braced list is walked in its semantic form: the converting
  // constructors, default member initializers and array filler of
  // `Entry table[] = {{"a", 1}, {"b", 2}};` exist only there, and the
  // default traversal walks the list as written.
  bool TraverseInitListExpr(clang::InitListExpr *list) {
    clang::InitListExpr *semantic =
        list->isSemanticForm() ? list : list->getSemanticForm();
    if (!semantic)
      semantic = list;
    for (clang::Stmt *child : semantic->children())
      if (child && !TraverseStmt(child))
        return false;
    if (clang::Expr *filler = semantic->getArrayFiller())
      if (!TraverseStmt(filler))
        return false;
    return true;
  }

  // A lambda written in an initializer is taken as called by it: the
  // graph holds the body's calls under the lambda's own node. The body is
  // not walked here; the captures' initializers are.
  bool TraverseLambdaExpr(clang::LambdaExpr *expr) {
    Ident id = initializerLambdaIdent(sm_, expr->getBeginLoc(), enclosing_);
    out_.push_back({std::move(id.usr), std::move(id.display)});
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
    // A compiler-generated constructor (defaulted, or inherited with
    // `using Base::Base;`) has no graph node and no written body: look
    // through it to what its member and base initializers run.
    if (auto *ctor = llvm::dyn_cast<clang::CXXConstructorDecl>(callee)) {
      if (ctor->isDefaulted() || ctor->isInheritingConstructor()) {
        if (seenGenerated_.insert(ctor->getCanonicalDecl()).second) {
          const clang::FunctionDecl *def = nullptr;
          if (ctor->hasBody(def)) {
            for (const clang::CXXCtorInitializer *init :
                 llvm::cast<clang::CXXConstructorDecl>(def)->inits())
              if (init->getInit())
                TraverseStmt(init->getInit());
          } else if (ctor->isInheritingConstructor()) {
            // Never defined in this TU: the base constructor at least.
            addCallee(ctor->getInheritedConstructor().getConstructor());
          }
        }
        return;
      }
    }
    if (const clang::IdentifierInfo *ii = callee->getIdentifier())
      if (ii->getName().starts_with("__builtin_"))
        return;
    const Ident &id = usrs_.identFor(callee);
    out_.push_back(StaticInitCallee{id.usr, id.display});
  }

  const clang::SourceManager &sm_;
  UsrCache &usrs_;
  std::vector<StaticInitCallee> &out_;
  llvm::SmallPtrSet<const clang::Decl *, 8> seenGenerated_;
  // The function a lambda met now is written in; empty outside one.
  std::string enclosing_;
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
    // `seeded<int>` and `seeded<long>`, not two records named `seeded`.
    if (const auto *spec =
            llvm::dyn_cast<clang::VarTemplateSpecializationDecl>(decl)) {
      root.name.clear();
      llvm::raw_string_ostream os(root.name);
      spec->getNameForDiagnostic(os, ctx_.getPrintingPolicy(),
                                 /*Qualified=*/true);
    }
    root.usr = id.usr;
    const clang::SourceLocation at = fileLoc(decl);
    root.file = rootFilePathOf(sm_, at);
    root.line = sm_.getSpellingLineNumber(at);
    root.tus.push_back(tuPath_);
    out_.roots.push_back(std::move(root));
    return out_.roots.back();
  }

  // The shape of check's static-init index (check/Indexer.cpp), minus
  // thread-locals, which initialize on first use. Constant initialization
  // is Sema's own verdict on the variable, evaluated in place: asking
  // whether the initializer alone is a constant expression says no for
  // every type with a non-trivial destructor (`std::unique_ptr<T> p;`),
  // however constexpr its constructor, and yes for `T table[3] = {};`
  // whatever T's constructor does (it does not look at the array
  // filler). Sema evaluates every global from C++11 on; the syntactic
  // rule is for C++98, where it does not. One case Sema calls
  // non-constant runs nothing all the same: a trivial default
  // constructor (`static stack_t old;`, `static struct sigaction
  // prev[N];`) leaves the members indeterminate, so the evaluation
  // fails, and the object is the zero-initialized one it already was.
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
        var->hasConstantInitialization())
      return;
    if (!ctx_.getLangOpts().CPlusPlus11 &&
        init->isConstantInitializer(ctx_, var->getType()->isReferenceType()))
      return;
    if (const auto *construct =
            llvm::dyn_cast<clang::CXXConstructExpr>(init->IgnoreImplicit())) {
      const clang::CXXConstructorDecl *ctor = construct->getConstructor();
      if (ctor->isDefaultConstructor() && ctor->isTrivial())
        return;
    }
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

void sortUniqueStrings(std::vector<std::string> &v) {
  std::sort(v.begin(), v.end());
  v.erase(std::unique(v.begin(), v.end()), v.end());
}

bool calleeLess(const StaticInitCallee &a, const StaticInitCallee &b) {
  return std::tie(a.usr, a.display) < std::tie(b.usr, b.display);
}

// One record's lists in canonical form, its TU list taken as the
// record's own: TUs sorted and unique, callees by usr with duplicates
// folded (an empty TU list is every TU of the record, so it absorbs any
// other), and a callee list naming every TU of the record emptied.
void normalizeRecord(StaticInitRoot &root) {
  sortUniqueStrings(root.tus);
  std::sort(root.callees.begin(), root.callees.end(), calleeLess);
  size_t kept = 0;
  for (size_t i = 0; i < root.callees.size(); ++i) {
    StaticInitCallee &c = root.callees[i];
    if (kept > 0 && root.callees[kept - 1].usr == c.usr) {
      StaticInitCallee &into = root.callees[kept - 1];
      if (into.tus.empty() || c.tus.empty()) {
        into.tus.clear();
      } else {
        into.tus.insert(into.tus.end(), c.tus.begin(), c.tus.end());
        sortUniqueStrings(into.tus);
      }
      continue;
    }
    if (kept != i)
      root.callees[kept] = std::move(c);
    ++kept;
  }
  root.callees.resize(kept);
  for (StaticInitCallee &c : root.callees) {
    if (c.tus.empty())
      continue;
    sortUniqueStrings(c.tus);
    if (c.tus.size() >= root.tus.size())
      c.tus.clear();
  }
}

// Whether every record of [first, last) calls the same functions from
// all of its TUs: then the merged record is the union of the TU lists
// over the first record's callees, with nothing to attribute.
bool sameCallsEverywhere(const std::vector<StaticInitRoot> &roots,
                         size_t first, size_t last) {
  const auto &calls = roots[first].callees;
  for (size_t i = first; i < last; ++i) {
    const auto &other = roots[i].callees;
    if (other.size() != calls.size())
      return false;
    for (size_t k = 0; k < calls.size(); ++k)
      if (!other[k].tus.empty() || other[k].usr != calls[k].usr ||
          other[k].display != calls[k].display)
        return false;
  }
  return true;
}

// Fold the normalized records [first, last) of one initializer into
// roots[first]. The usual case (every TU calls the same functions) is a
// union of TU lists; otherwise each callee's TUs are spelled out, merged,
// and emptied again where they are all of the root's.
void foldRecords(std::vector<StaticInitRoot> &roots, size_t first,
                 size_t last) {
  StaticInitRoot &into = roots[first];
  if (sameCallsEverywhere(roots, first, last)) {
    for (size_t i = first + 1; i < last; ++i)
      into.tus.insert(into.tus.end(),
                      std::make_move_iterator(roots[i].tus.begin()),
                      std::make_move_iterator(roots[i].tus.end()));
    sortUniqueStrings(into.tus);
    return;
  }
  std::vector<StaticInitCallee> callees;
  for (size_t i = first; i < last; ++i) {
    StaticInitRoot &record = roots[i];
    for (StaticInitCallee &c : record.callees) {
      if (c.tus.empty())
        c.tus = record.tus;
      callees.push_back(std::move(c));
    }
    if (i != first)
      into.tus.insert(into.tus.end(),
                      std::make_move_iterator(record.tus.begin()),
                      std::make_move_iterator(record.tus.end()));
  }
  into.callees = std::move(callees);
  normalizeRecord(into);
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
  for (StaticInitRoot &root : roots)
    normalizeRecord(root);
  std::stable_sort(roots.begin(), roots.end(),
                   [&](const StaticInitRoot &a, const StaticInitRoot &b) {
                     return key(a) < key(b);
                   });
  size_t kept = 0;
  for (size_t first = 0; first < roots.size();) {
    size_t last = first + 1;
    while (last < roots.size() && key(roots[last]) == key(roots[first]))
      ++last;
    if (last - first > 1)
      foldRecords(roots, first, last);
    if (kept != first)
      roots[kept] = std::move(roots[first]);
    ++kept;
    first = last;
  }
  roots.resize(kept);
  std::sort(roots.begin(), roots.end(),
            [](const StaticInitRoot &a, const StaticInitRoot &b) {
              return std::tie(a.file, a.line, a.name, a.usr) <
                     std::tie(b.file, b.line, b.name, b.usr);
            });
}

void StaticInitFacts::append(StaticInitFacts &&other) {
  collected = collected && other.collected;
  roots.insert(roots.end(), std::make_move_iterator(other.roots.begin()),
               std::make_move_iterator(other.roots.end()));
  other.roots.clear();
}

void StaticInitFacts::absorb(StaticInitFacts &&other) {
  append(std::move(other));
  canonicalize();
}

size_t StaticInitFacts::removeTUs(const std::vector<std::string> &tuPaths) {
  if (tuPaths.empty() || roots.empty())
    return 0;
  std::vector<std::string> gone;
  gone.reserve(tuPaths.size());
  for (const std::string &raw : tuPaths)
    gone.push_back(toIndexText(raw));
  sortUniqueStrings(gone);
  auto isGone = [&](const std::string &tu) {
    return std::binary_search(gone.begin(), gone.end(), tu);
  };
  size_t kept = 0;
  for (size_t i = 0; i < roots.size(); ++i) {
    StaticInitRoot &root = roots[i];
    root.tus.erase(std::remove_if(root.tus.begin(), root.tus.end(), isGone),
                   root.tus.end());
    if (root.tus.empty())
      continue;
    // A callee every TU of the root calls still is; one some of them
    // call loses the removed ones, and is gone with the last.
    size_t keptCallees = 0;
    for (size_t k = 0; k < root.callees.size(); ++k) {
      StaticInitCallee &c = root.callees[k];
      if (!c.tus.empty()) {
        c.tus.erase(std::remove_if(c.tus.begin(), c.tus.end(), isGone),
                    c.tus.end());
        if (c.tus.empty())
          continue;
        if (c.tus.size() == root.tus.size())
          c.tus.clear();
      }
      if (keptCallees != k)
        root.callees[keptCallees] = std::move(c);
      ++keptCallees;
    }
    root.callees.resize(keptCallees);
    if (kept != i)
      roots[kept] = std::move(root);
    ++kept;
  }
  const size_t dropped = roots.size() - kept;
  roots.resize(kept);
  return dropped;
}

} // namespace vycor
