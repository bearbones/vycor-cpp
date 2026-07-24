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

#include "vycor/anneal/Indexer.h"
#include "vycor/anneal/TypeNormalize.h"
#include "vycor/compat/ClangVersion.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/Attr.h"
#include "clang/AST/ODRHash.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Lex/Lexer.h"

#include <algorithm>

#include "llvm/Support/raw_ostream.h"

namespace vycor {

std::string formatTemplateArgs(const clang::TemplateArgumentList &args,
                               const clang::ASTContext &context) {
  clang::PrintingPolicy policy(context.getLangOpts());
  policy.SuppressTagKeyword = true;
  std::string out;
  llvm::raw_string_ostream os(out);
  for (unsigned i = 0; i < args.size(); ++i) {
    if (i)
      os << ", ";
    clang::TemplateArgument arg = args[i];
    // Canonicalize type arguments so both sides of the join print typedefs
    // and sugar identically.
    if (arg.getKind() == clang::TemplateArgument::Type) {
      os << arg.getAsType().getCanonicalType().getAsString(policy);
      continue;
    }
    arg.print(policy, os, /*IncludeType=*/true);
  }
  os.flush();
  return out;
}

// --- IndexerVisitor ---

// Record default arguments actually WRITTEN at this declaration site.
// hasInheritedDefaultArg filters redeclarations that merely inherit a
// default from an earlier declaration in the same TU, so each entry is a
// site that spelled the default out — the unit the divergence analysis
// compares. Always collected: scanning parameters costs nothing.

IndexerVisitor::IndexerVisitor(GlobalIndex &index, clang::SourceManager &sm,
                               bool collectOdr, bool collectSummaries)
    : index_(index), sm_(sm), collectOdr_(collectOdr),
      collectSummaries_(collectSummaries) {}

// ODR definition-site hashing (--odr-diag). Only vague-linkage entities are
// interesting: their definitions are legal in many TUs and the linker
// merges them WITHOUT comparing content, so a mismatch is silent — exactly
// what ordinary builds can't diagnose (duplicate strong symbols already
// fail the link). Internal-linkage entities are per-TU by design and
// exempt; templates and implicit instantiations are out of scope for now;
// system headers are skipped for cost and noise.
void IndexerVisitor::maybeRecordOdrFunction(clang::FunctionDecl *decl) {
  if (!collectOdr_)
    return;
  if (!decl->isThisDeclarationADefinition() || !decl->hasBody())
    return;
  if (!decl->isInlined()) // non-inline: one definition, linker enforces
    return;
  if (!decl->isExternallyVisible())
    return;
  if (decl->isDependentContext() || decl->getDescribedFunctionTemplate())
    return;
  if (decl->getTemplateSpecializationKind() ==
      clang::TSK_ImplicitInstantiation)
    return;
  auto loc = sm_.getSpellingLoc(decl->getLocation());
  if (sm_.isInSystemHeader(loc))
    return;

  clang::ODRHash hash;
  hash.AddFunctionDecl(decl);

  OdrEntry entry;
  entry.qualifiedName = decl->getQualifiedNameAsString();
  // Full function type spelling: distinguishes overloads and const/ref
  // qualified method pairs in one string.
  entry.signature = decl->getType().getAsString();
  entry.isClass = false;
  if (const auto *method = llvm::dyn_cast<clang::CXXMethodDecl>(decl))
    entry.enclosingClass = method->getParent()->getQualifiedNameAsString();
  entry.filePath = getFilePath(decl->getLocation());
  entry.line = sm_.getSpellingLineNumber(decl->getLocation());
  entry.odrHash = hash.CalculateHash();
  index_.addOdrEntry(entry);
}

void IndexerVisitor::maybeRecordOdrClass(clang::CXXRecordDecl *decl) {
  if (!collectOdr_)
    return;
  if (!decl->isCompleteDefinition() || decl->isLambda() ||
      !decl->getIdentifier())
    return;
  if (!decl->isExternallyVisible())
    return;
  if (decl->isDependentType() || decl->getDescribedClassTemplate())
    return;
  if (const auto *spec =
          llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(decl))
    if (spec->getSpecializationKind() == clang::TSK_ImplicitInstantiation)
      return;
  auto loc = sm_.getSpellingLoc(decl->getLocation());
  if (sm_.isInSystemHeader(loc))
    return;

  clang::ODRHash hash;
  hash.AddCXXRecordDecl(decl);

  OdrEntry entry;
  entry.qualifiedName = decl->getQualifiedNameAsString();
  entry.isClass = true;
  entry.filePath = getFilePath(decl->getLocation());
  entry.line = sm_.getSpellingLineNumber(decl->getLocation());
  entry.odrHash = hash.CalculateHash();
  index_.addOdrEntry(entry);
}

void IndexerVisitor::maybeRecordDefaultArgs(clang::FunctionDecl *decl) {
  if (!astContext_)
    return;
  if (!decl->isExternallyVisible())
    return;
  if (decl->isDependentContext() || decl->getDescribedFunctionTemplate())
    return;
  if (decl->getTemplateSpecializationKind() ==
      clang::TSK_ImplicitInstantiation)
    return;
  auto declLoc = sm_.getSpellingLoc(decl->getLocation());
  if (sm_.isInSystemHeader(declLoc))
    return;

  for (unsigned i = 0; i < decl->getNumParams(); ++i) {
    const clang::ParmVarDecl *param = decl->getParamDecl(i);
    if (!param->hasDefaultArg() || param->hasInheritedDefaultArg() ||
        param->hasUninstantiatedDefaultArg())
      continue;
    auto range = clang::CharSourceRange::getTokenRange(
        param->getDefaultArgRange());
    llvm::StringRef text = clang::Lexer::getSourceText(
        range, sm_, astContext_->getLangOpts());
    if (text.empty())
      continue; // macro-mangled or invalid range: nothing comparable

    DefaultArgEntry entry;
    entry.qualifiedName = decl->getQualifiedNameAsString();
    entry.signature = decl->getType().getAsString();
    entry.paramIndex = i;
    entry.paramName = param->getNameAsString();
    entry.defaultText = text.trim().str();
    entry.filePath = getFilePath(decl->getLocation());
    entry.line = sm_.getSpellingLineNumber(decl->getLocation());
    index_.addDefaultArg(entry);
  }
}

namespace {

// Collects what a body (or initializer expression) touches: directly
// referenced static-storage globals, directly called
// functions/constructors (split into all vs. not-inside-a-try), and
// whether a throw occurs outside every try. Serves the static-init
// checks' seeds and the exception-escape summaries in one walk.
class BodyCollector : public clang::RecursiveASTVisitor<BodyCollector> {
public:
  std::vector<std::string> referencedGlobals;
  std::vector<std::string> calledFunctions;
  std::vector<std::string> unguardedCalls;
  bool hasUncaughtThrow = false;

  bool VisitDeclRefExpr(clang::DeclRefExpr *ref) {
    const auto *var = llvm::dyn_cast<clang::VarDecl>(ref->getDecl());
    if (!var || !var->hasGlobalStorage() || var->isStaticLocal())
      return true;
    if (!var->isExternallyVisible())
      return true; // internal linkage: same-TU by definition, ordered
    add(referencedGlobals, var->getQualifiedNameAsString());
    return true;
  }

  bool VisitCallExpr(clang::CallExpr *call) {
    if (const auto *callee = call->getDirectCallee())
      recordCall(callee->getQualifiedNameAsString());
    return true;
  }

  bool VisitCXXConstructExpr(clang::CXXConstructExpr *construct) {
    if (const auto *ctor = construct->getConstructor())
      recordCall(ctor->getQualifiedNameAsString());
    return true;
  }

  bool VisitCXXThrowExpr(clang::CXXThrowExpr *) {
    // A throw lexically inside a try is conservatively treated as locally
    // handled (matching handler types is out of scope) — precision over
    // recall.
    if (tryDepth_ == 0)
      hasUncaughtThrow = true;
    return true;
  }

  // Traverse the try block at increased guard depth; handlers at the
  // outer depth (a rethrow from a handler escapes unless an OUTER try
  // catches it).
  bool TraverseCXXTryStmt(clang::CXXTryStmt *stmt) {
    ++tryDepth_;
    TraverseStmt(stmt->getTryBlock());
    --tryDepth_;
    for (unsigned i = 0; i < stmt->getNumHandlers(); ++i)
      TraverseStmt(stmt->getHandler(i));
    return true;
  }

  // noexcept(expr) is unevaluated: nothing in it runs.
  bool TraverseCXXNoexceptExpr(clang::CXXNoexceptExpr *) { return true; }

private:
  void recordCall(std::string name) {
    add(calledFunctions, name);
    if (tryDepth_ == 0)
      add(unguardedCalls, std::move(name));
  }
  static void add(std::vector<std::string> &into, std::string name) {
    if (std::find(into.begin(), into.end(), name) == into.end())
      into.push_back(std::move(name));
  }
  unsigned tryDepth_ = 0;
};

} // namespace

// __attribute__((constructor)) functions ARE static initializers (ELF
// .init_array entries): record them as hazard-walk roots. The call graph
// already has their outgoing edges, so no seed collection is needed.
void IndexerVisitor::maybeRecordConstructorFn(clang::FunctionDecl *decl) {
  if (!decl->hasAttr<clang::ConstructorAttr>() ||
      !decl->isThisDeclarationADefinition())
    return;
  auto loc = sm_.getSpellingLoc(decl->getLocation());
  if (sm_.isInSystemHeader(loc))
    return;
  StaticInitEntry entry;
  entry.qualifiedName = decl->getQualifiedNameAsString();
  entry.filePath = getFilePath(decl->getLocation());
  entry.line = sm_.getSpellingLineNumber(decl->getLocation());
  entry.dynamicInit = true;
  entry.isConstructorFn = true;
  index_.addStaticInit(entry);
}

// Per-function call summary (transitive static-init-order and
// exception-escape). Rides the same parse; gated because the payload cost
// is real on large trees.
void IndexerVisitor::maybeRecordFunctionSummary(clang::FunctionDecl *decl) {
  if (!collectSummaries_)
    return;
  if (!decl->isThisDeclarationADefinition() || !decl->hasBody())
    return;
  if (decl->isDependentContext() || decl->getDescribedFunctionTemplate())
    return;
  if (decl->getTemplateSpecializationKind() ==
      clang::TSK_ImplicitInstantiation)
    return;
  auto loc = sm_.getSpellingLoc(decl->getLocation());
  if (sm_.isInSystemHeader(loc))
    return;

  FunctionSummaryEntry entry;
  entry.qualifiedName = decl->getQualifiedNameAsString();
  entry.filePath = getFilePath(decl->getLocation());
  entry.line = sm_.getSpellingLineNumber(decl->getLocation());
  if (const auto *proto = decl->getType()->getAs<clang::FunctionProtoType>())
    entry.isNoexcept = proto->canThrow() == clang::CT_Cannot;

  BodyCollector collector;
  collector.TraverseStmt(decl->getBody());
  entry.hasUncaughtThrow = collector.hasUncaughtThrow;
  entry.calledFunctions = std::move(collector.calledFunctions);
  entry.unguardedCalls = std::move(collector.unguardedCalls);
  entry.referencedGlobals = std::move(collector.referencedGlobals);

  // Nothing to say -> no entry (a noexcept leaf with no calls and no
  // throws can never produce a diagnostic).
  if (!entry.hasUncaughtThrow && entry.calledFunctions.empty() &&
      entry.referencedGlobals.empty())
    return;
  index_.addFunctionSummary(entry);
}

bool IndexerVisitor::VisitVarDecl(clang::VarDecl *decl) {
  // Static-storage-duration variable DEFINITIONS at namespace/class scope.
  // Function-local statics initialize lazily on first pass — safe, and
  // deliberately excluded. Parameters and locals have no global storage.
  if (llvm::isa<clang::ParmVarDecl>(decl) || !decl->hasGlobalStorage() ||
      decl->isStaticLocal())
    return true;
  if (decl->isThisDeclarationADefinition() != clang::VarDecl::Definition ||
      !decl->hasInit())
    return true;
  if (decl->getType()->isDependentType() || decl->getDescribedVarTemplate())
    return true;
  if (decl->getTemplateSpecializationKind() ==
      clang::TSK_ImplicitInstantiation)
    return true;
  auto loc = sm_.getSpellingLoc(decl->getLocation());
  if (sm_.isInSystemHeader(loc) || !astContext_)
    return true;

  StaticInitEntry entry;
  entry.qualifiedName = decl->getQualifiedNameAsString();
  entry.filePath = getFilePath(decl->getLocation());
  entry.line = sm_.getSpellingLineNumber(decl->getLocation());
  // constexpr/constinit and constant initializers run in the static phase
  // (compile-time image), before ALL dynamic initialization: safe as
  // dependency targets, never hazard roots.
  entry.dynamicInit =
      !decl->isConstexpr() && !decl->hasAttr<clang::ConstInitAttr>() &&
      !decl->getInit()->isConstantInitializer(
          *astContext_, decl->getType()->isReferenceType());
  if (entry.dynamicInit) {
    BodyCollector collector;
    collector.TraverseStmt(decl->getInit());
    entry.referencedGlobals = std::move(collector.referencedGlobals);
    entry.calledFunctions = std::move(collector.calledFunctions);
  }
  index_.addStaticInit(entry);
  return true;
}

bool IndexerVisitor::VisitFunctionDecl(clang::FunctionDecl *decl) {
  // Skip implicit declarations (compiler-generated).
  if (decl->isImplicit())
    return true;

  // Skip deduction guides — they're handled separately.
  if (llvm::isa<clang::CXXDeductionGuideDecl>(decl))
    return true;

  // Skip function templates (we want concrete overloads).
  // Template specializations are fine.
  if (decl->getDescribedFunctionTemplate())
    return true;

  // ODR hashing wants the DEFINITION, which is frequently a redeclaration
  // — record before the first-declaration-only skip below.
  maybeRecordOdrFunction(decl);
  maybeRecordDefaultArgs(decl);
  maybeRecordConstructorFn(decl);
  maybeRecordFunctionSummary(decl);

  // Only index definitions or the first declaration to avoid duplicates
  // from multiple includes. We prefer to index every unique declaration
  // since different headers may declare different overloads.
  if (decl->getPreviousDecl())
    return true;

  FunctionOverloadEntry entry;
  entry.qualifiedName = decl->getQualifiedNameAsString();
  entry.headerPath = getFilePath(decl->getLocation());
  entry.returnType = decl->getReturnType().getAsString();
  entry.sourceLine = sm_.getSpellingLineNumber(decl->getLocation());

  for (const auto *param : decl->parameters()) {
    entry.paramTypes.push_back(param->getType().getAsString());
  }

  index_.addFunctionOverload(std::move(entry));
  return true;
}

bool IndexerVisitor::VisitCXXDeductionGuideDecl(
    clang::CXXDeductionGuideDecl *decl) {
  if (decl->isImplicit())
    return true;

  // Skip redeclarations.
  if (decl->getPreviousDecl())
    return true;

  DeductionGuideEntry entry;
  entry.templateName = decl->getDeducedTemplate()->getQualifiedNameAsString();
  entry.headerPath = getFilePath(decl->getLocation());
  entry.deducedType = decl->getReturnType().getAsString();
  entry.sourceLine = sm_.getSpellingLineNumber(decl->getLocation());

  for (const auto *param : decl->parameters()) {
    entry.paramTypes.push_back(param->getType().getAsString());
  }

  index_.addDeductionGuide(std::move(entry));
  return true;
}

bool IndexerVisitor::VisitCXXRecordDecl(clang::CXXRecordDecl *decl) {
  // Only record complete, non-implicit class definitions. Forward
  // declarations and implicit decls have no base list to walk.
  if (decl->isImplicit())
    return true;
  maybeRecordOdrClass(decl);

  // Explicit (full) class template specializations — recorded even when
  // declaration-only, and BEFORE the definition guards below: the
  // specialization-visibility check cares that the declaration exists at
  // all. Partial specializations are patterns, not concrete arguments, and
  // are out of scope.
  if (const auto *spec =
          llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(decl)) {
    if (spec->getSpecializationKind() == clang::TSK_ExplicitSpecialization &&
        !llvm::isa<clang::ClassTemplatePartialSpecializationDecl>(spec) &&
        astContext_ &&
        !sm_.isInSystemHeader(sm_.getSpellingLoc(spec->getLocation()))) {
      SpecializationEntry entry;
      entry.templateName =
          spec->getSpecializedTemplate()->getQualifiedNameAsString();
      entry.argsString =
          formatTemplateArgs(spec->getTemplateArgs(), *astContext_);
      entry.headerPath = getFilePath(spec->getLocation());
      entry.line = sm_.getSpellingLineNumber(spec->getLocation());
      index_.addSpecialization(entry);
    }
  }
  if (!decl->hasDefinition())
    return true;
  if (decl->getDefinition() != decl)
    return true;

  std::string derived =
      normalizeTypeForMatching(decl->getQualifiedNameAsString());

  auto &rels = index_.mutableTypeRelations();
  for (const auto &base : decl->bases()) {
    const clang::CXXRecordDecl *baseDecl =
        base.getType()->getAsCXXRecordDecl();
    if (!baseDecl)
      continue;
    std::string baseName =
        normalizeTypeForMatching(baseDecl->getQualifiedNameAsString());
    rels.addBase(derived, baseName);
  }
  return true;
}

bool IndexerVisitor::VisitCXXConstructorDecl(clang::CXXConstructorDecl *decl) {
  if (decl->isImplicit())
    return true;
  if (decl->isExplicit())
    return true;
  // Templates are not modelled — the scorer doesn't do deduction.
  if (decl->getDescribedFunctionTemplate())
    return true;

  // A non-explicit single-parameter ctor (or one where every extra parameter
  // has a default) creates an implicit converting edge: `ToType` is
  // constructible from `FromType` without user intervention.
  unsigned required = decl->getMinRequiredArguments();
  if (required != 1)
    return true;
  if (decl->getNumParams() < 1)
    return true;

  const auto *parent = decl->getParent();
  if (!parent)
    return true;

  std::string toType =
      normalizeTypeForMatching(parent->getQualifiedNameAsString());
  std::string fromType = normalizeTypeForMatching(
      decl->getParamDecl(0)->getType().getCanonicalType().getAsString());

  index_.mutableTypeRelations().addCtorEdge(std::move(toType),
                                            std::move(fromType));
  return true;
}

bool IndexerVisitor::VisitCXXConversionDecl(clang::CXXConversionDecl *decl) {
  if (decl->isImplicit())
    return true;
  if (decl->isExplicit())
    return true;
  if (decl->getDescribedFunctionTemplate())
    return true;

  const auto *parent = decl->getParent();
  if (!parent)
    return true;

  std::string fromType =
      normalizeTypeForMatching(parent->getQualifiedNameAsString());
  std::string toType = normalizeTypeForMatching(
      decl->getConversionType().getCanonicalType().getAsString());

  index_.mutableTypeRelations().addConvOpEdge(std::move(fromType),
                                              std::move(toType));
  return true;
}

void IndexerVisitor::setASTContext(clang::ASTContext *ctx) {
  astContext_ = ctx;
}

bool IndexerVisitor::VisitCXXMethodDecl(clang::CXXMethodDecl *decl) {
  if (decl->isImplicit())
    return true;

  if (!decl->isThisDeclarationADefinition())
    return true;

  if (decl->getPreviousDecl())
    return true;

  auto *parent = llvm::dyn_cast<clang::CXXRecordDecl>(decl->getParent());
  if (!parent)
    return true;

  CoveragePropertyEntry entry;
  entry.qualifiedName = decl->getQualifiedNameAsString();
  entry.headerPath = getFilePath(decl->getLocation());
  entry.sourceLine = sm_.getSpellingLineNumber(decl->getLocation());
  entry.enclosingClass = parent->getQualifiedNameAsString();

  if (astContext_)
    entry.gvaLinkage =
        static_cast<int>(astContext_->GetGVALinkageForFunction(decl));

  entry.isInlined = decl->isInlined();
  entry.isConstexpr = decl->isConstexpr();
  entry.isDefaulted = decl->isDefaulted();
  entry.isTrivial = decl->isTrivial();
  entry.isVirtual = decl->isVirtual();
  entry.isStaticMethod = decl->isStatic();
  entry.isImplicitlyInstantiable = decl->isImplicitlyInstantiable();
  entry.templatedKind = static_cast<int>(decl->getTemplatedKind());
  entry.storageClass = static_cast<int>(decl->getStorageClass());
  entry.formalLinkage =
      static_cast<int>(decl->getLinkageAndVisibility().getLinkage());

  if (auto *body = decl->getBody())
    entry.bodyStmtCount = countStmts(body);

  // Build human-readable signature.
  entry.signature = decl->getReturnType().getAsString() + " " +
                    decl->getQualifiedNameAsString() + "(";
  for (unsigned i = 0; i < decl->getNumParams(); ++i) {
    if (i > 0)
      entry.signature += ", ";
    entry.signature += decl->getParamDecl(i)->getType().getAsString();
  }
  entry.signature += ")";
  if (decl->isConst())
    entry.signature += " const";

  index_.addCoverageProperty(std::move(entry));
  return true;
}

unsigned IndexerVisitor::countStmts(const clang::Stmt *s,
                                    unsigned limit) const {
  if (!s || limit == 0)
    return 0;
  unsigned count = 1;
  for (auto *child : s->children()) {
    if (child) {
      count += countStmts(child, limit - count);
      if (count >= limit)
        return limit;
    }
  }
  return count;
}

std::string IndexerVisitor::getFilePath(clang::SourceLocation loc) const {
  auto fileEntry = sm_.getFileEntryRefForID(sm_.getFileID(
      sm_.getSpellingLoc(loc)));
  if (fileEntry)
    return std::string(fileEntry->getName());
  return "<unknown>";
}

// --- IndexerConsumer ---

IndexerConsumer::IndexerConsumer(GlobalIndex &index, clang::SourceManager &sm,
                                 bool collectOdr, bool collectSummaries)
    : visitor_(index, sm, collectOdr, collectSummaries) {}

void IndexerConsumer::HandleTranslationUnit(clang::ASTContext &context) {
  visitor_.setASTContext(&context);
  visitor_.TraverseDecl(context.getTranslationUnitDecl());
}

// --- IndexerAction ---

IndexerAction::IndexerAction(GlobalIndex &index, bool collectOdr,
                             bool collectSummaries)
    : index_(index), collectOdr_(collectOdr),
      collectSummaries_(collectSummaries) {}

std::unique_ptr<clang::ASTConsumer>
IndexerAction::CreateASTConsumer(clang::CompilerInstance &ci,
                                 llvm::StringRef /*file*/) {
  return std::make_unique<IndexerConsumer>(index_, ci.getSourceManager(),
                                           collectOdr_, collectSummaries_);
}

// --- IndexerActionFactory ---

IndexerActionFactory::IndexerActionFactory(GlobalIndex &index,
                                           bool collectOdr,
                                           bool collectSummaries)
    : index_(index), collectOdr_(collectOdr),
      collectSummaries_(collectSummaries) {}

std::unique_ptr<clang::FrontendAction> IndexerActionFactory::create() {
  return std::make_unique<IndexerAction>(index_, collectOdr_,
                                         collectSummaries_);
}

} // namespace vycor
