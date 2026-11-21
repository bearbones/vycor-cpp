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

#include "vycor/callgraph/ReferenceVisitor.h"
#include "vycor/callgraph/CompilerWritten.h"

#include "vycor/callgraph/EmissionGraph.h"
#include "vycor/callgraph/LockFacts.h"

#include "clang/AST/ASTLambda.h"
#include "clang/AST/Attr.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/GlobalDecl.h"
#include "clang/AST/Mangle.h"
#include "clang/AST/RecordLayout.h"
#include "clang/AST/StmtCXX.h"
#include "clang/Basic/Builtins.h"
#include "clang/Basic/FileManager.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/Path.h"

#include <algorithm>
#include <cassert>
#include <memory>
#include <optional>
#include <utility>

namespace vycor {

namespace {

// An unresolved overload set in a template body names every candidate that
// lookup found at the definition. Small sets are recorded as possible
// references; a set past this size (operator<< and friends) is noise that
// would cost a record per candidate per template.
constexpr unsigned kMaxOverloadCandidates = 16;

constexpr unsigned kReferrerShift = 35;
constexpr unsigned kTargetShift = 3;
constexpr uint64_t kRoleMask = (1u << kTargetShift) - 1;

// The variable a name stands for: itself, or for a structured binding the
// variable it decomposes (`auto [a, b] = g;` at namespace scope: a use of
// `a` is a use of the object named [a, b]).
[[nodiscard]] const clang::VarDecl *variableOf(const clang::ValueDecl *decl) {
  if (const auto *var = llvm::dyn_cast<clang::VarDecl>(decl))
    return var;
  if (const auto *binding = llvm::dyn_cast<clang::BindingDecl>(decl))
    return llvm::dyn_cast_or_null<clang::VarDecl>(
        binding->getDecomposedDecl());
  return nullptr;
}

[[nodiscard]] bool namesLinkable(const clang::ValueDecl *decl) {
  if (const clang::VarDecl *var = variableOf(decl))
    return var->hasGlobalStorage();
  return llvm::isa<clang::FunctionDecl>(decl);
}

// The subscripted operand of a[i]. ArraySubscriptExpr::getBase() tells the
// two operands apart by type, which an index that depends on a template
// parameter does not have yet.
[[nodiscard]] const clang::Expr *
subscriptBase(const clang::ArraySubscriptExpr *expr) {
  const clang::Expr *lhs = expr->getLHS();
  const clang::Expr *rhs = expr->getRHS();
  const auto integral = [](const clang::Expr *e) {
    return !e->getType().isNull() && e->getType()->isIntegerType();
  };
  return integral(lhs) && !integral(rhs) ? rhs : lhs;
}

// A non-const reference: binding to it hands out the object's identity.
[[nodiscard]] bool isMutableRef(clang::QualType type) {
  if (type.isNull() || !type->isReferenceType())
    return false;
  clang::QualType pointee = type->getPointeeType();
  return !pointee.isConstQualified() || pointee.isVolatileQualified();
}

[[nodiscard]] bool isConstLValueRef(clang::QualType type) {
  if (type.isNull())
    return false;
  const auto *ref = type->getAs<clang::LValueReferenceType>();
  if (!ref)
    return false;
  clang::QualType pointee = ref->getPointeeType();
  return pointee.isConstQualified() && !pointee.isVolatileQualified();
}

// The class a class-template specialization stands for in the code as
// written: its template pattern, or itself for an ordinary class and for an
// explicit specialization.
[[nodiscard]] const clang::CXXRecordDecl *
classPattern(const clang::CXXRecordDecl *cls) {
  if (const clang::CXXRecordDecl *pattern =
          cls->getTemplateInstantiationPattern())
    return pattern;
  if (const auto *spec =
          llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(cls)) {
    // Named but never instantiated (V<int> * in a catch clause).
    if (spec->getSpecializationKind() == clang::TSK_Undeclared)
      return spec->getSpecializedTemplate()->getTemplatedDecl();
  }
  return cls;
}

[[nodiscard]] bool isAtomic(clang::QualType type) {
  if (type->isAtomicType())
    return true;
  const clang::CXXRecordDecl *cls = type->getAsCXXRecordDecl();
  if (!cls || !cls->isInStdNamespace())
    return false;
  const clang::IdentifierInfo *id = cls->getIdentifier();
  return id && (id->isStr("atomic") || id->isStr("atomic_flag"));
}

// Attribute flags shared by functions, variables and classes, read from the
// most recent declaration (attributes are inherited forward).
[[nodiscard]] uint32_t
attributeFlags(const clang::NamedDecl *decl,
               clang::NamedDecl::ExplicitVisibilityKind visibilityKind) {
  uint32_t flags = 0;
  // Explicitly exported: a default or protected visibility written on the
  // declaration, on its class or namespace, or by a visibility pragma. A
  // command-line default (-fvisibility=) is not explicit.
  if (decl->getExplicitVisibility(visibilityKind)) {
    if (*decl->getExplicitVisibility(visibilityKind) !=
        clang::HiddenVisibility)
      flags |= kSymVisibility;
  } else if (decl->isExternallyVisible() && !decl->isInvalidDecl()) {
    const clang::LinkageInfo info = decl->getLinkageAndVisibility();
    if (info.isVisibilityExplicit() &&
        info.getVisibility() != clang::HiddenVisibility)
      flags |= kSymVisibility;
  }
  if (!decl->hasAttrs())
    return flags;
  if (decl->hasAttr<clang::DLLExportAttr>())
    flags |= kSymDllExport;
  if (decl->hasAttr<clang::DLLImportAttr>())
    flags |= kSymDllImport;
  if (decl->hasAttr<clang::UsedAttr>() || decl->hasAttr<clang::RetainAttr>() ||
      decl->hasAttr<clang::ConstructorAttr>() ||
      decl->hasAttr<clang::DestructorAttr>())
    flags |= kSymUsedAttr;
  if (decl->hasAttr<clang::WeakAttr>() ||
      decl->hasAttr<clang::WeakImportAttr>() ||
      decl->hasAttr<clang::WeakRefAttr>() ||
      decl->hasAttr<clang::AliasAttr>() || decl->hasAttr<clang::IFuncAttr>())
    flags |= kSymWeak;
  return flags;
}

// Layout queries assert on anything incomplete, dependent or invalid.
[[nodiscard]] bool hasLayout(const clang::CXXRecordDecl *def) {
  return def && def->isCompleteDefinition() && !def->isInvalidDecl() &&
         !def->isDependentContext();
}

[[nodiscard]] bool isSizable(const clang::ASTContext &ctx,
                             clang::QualType type) {
  if (type.isNull() || type->isDependentType() || type->isUndeducedType() ||
      type->isIncompleteType() || type->containsErrors())
    return false;
  clang::QualType element = ctx.getBaseElementType(type.getNonReferenceType());
  if (const clang::RecordDecl *record = element->getAsRecordDecl())
    return !record->isInvalidDecl() && !record->isDependentContext();
  return true;
}

// Whether a function's exception specification lets it throw. One Sema has
// not computed yet counts as throwing.
[[nodiscard]] bool canThrow(const clang::FunctionDecl *fn) {
  const auto *proto = fn->getType()->getAs<clang::FunctionProtoType>();
  if (!proto || clang::isUnresolvedExceptionSpec(proto->getExceptionSpecType()))
    return true;
  return proto->canThrow() != clang::CT_Cannot;
}

// Whether evaluating an initializer can throw, conservatively: a literal, a
// name, value-initialization and a trivial or non-throwing construction from
// such operands cannot; anything else (a call, an allocation) may.
[[nodiscard]] bool mayThrow(const clang::Expr *expr) {
  if (!expr)
    return false;
  expr = expr->IgnoreImplicit();
  if (const auto *init = llvm::dyn_cast<clang::CXXDefaultInitExpr>(expr))
    return mayThrow(init->getExpr());
  if (const auto *arg = llvm::dyn_cast<clang::CXXDefaultArgExpr>(expr))
    return mayThrow(arg->getExpr());
  if (llvm::isa<clang::IntegerLiteral, clang::FloatingLiteral,
                clang::CharacterLiteral, clang::StringLiteral,
                clang::CXXBoolLiteralExpr, clang::CXXNullPtrLiteralExpr,
                clang::ImplicitValueInitExpr, clang::CXXScalarValueInitExpr,
                clang::DeclRefExpr>(expr))
    return false;
  if (const auto *construct = llvm::dyn_cast<clang::CXXConstructExpr>(expr)) {
    const clang::CXXConstructorDecl *ctor = construct->getConstructor();
    if (!ctor || (!ctor->isTrivial() && canThrow(ctor)))
      return true;
    for (const clang::Expr *arg : construct->arguments())
      if (mayThrow(arg))
        return true;
    return false;
  }
  if (const auto *list = llvm::dyn_cast<clang::InitListExpr>(expr)) {
    for (const clang::Expr *init : list->inits())
      if (mayThrow(init))
        return true;
    return false;
  }
  if (const auto *cast = llvm::dyn_cast<clang::CastExpr>(expr))
    if (cast->getCastKind() != clang::CK_ConstructorConversion &&
        cast->getCastKind() != clang::CK_UserDefinedConversion)
      return mayThrow(cast->getSubExpr());
  return true;
}

} // namespace

ReferenceVisitor::ReferenceVisitor(ReferenceIndex &index,
                                   clang::SourceManager &sm,
                                   const std::string &tuPath,
                                   const LockTypeConfig *lockCfg)
    : index_(index), sm_(sm), tuPath_(tuPath), lockCfg_(lockCfg) {}

void ReferenceVisitor::run(clang::ASTContext &ctx) {
  ctx_ = &ctx;
  TraverseDecl(ctx.getTranslationUnitDecl());
  drainWork();
  collectEmissions();
  lower();
}

// ============================================================================
// Emission facts
// ============================================================================

void ReferenceVisitor::collectEmissions() {
  const EmissionGraph g = buildEmissionGraph(*ctx_);
  pendingEmissions_.reserve(g.size());
  for (uint32_t i = 1; i < g.size(); ++i) {
    const clang::FunctionDecl *def = g.function[i];
    // A trivial special member is lowered to a copy or to nothing, never
    // to a call, so CodeGen emits no body for it.
    if (!def || def->isConsteval() || def->isTrivial())
      continue;
    // What another TU emits too: a vague-linkage body (inline, an implicit
    // instantiation, an implicit member), a body kept for inlining only,
    // or an internal function a header defines. A strong definition is
    // the TU's own.
    uint8_t flags = 0;
    switch (ctx_->GetGVALinkageForFunction(def)) {
    case clang::GVA_DiscardableODR:
      break;
    case clang::GVA_AvailableExternally:
      flags = kEmitAvailableExternally;
      break;
    case clang::GVA_Internal:
      flags = kEmitInternal;
      break;
    default:
      continue;
    }
    // The standard library's and other system code is everyone's.
    clang::SourceLocation loc = sm_.getExpansionLoc(def->getLocation());
    if (loc.isInvalid() || sm_.isInSystemHeader(loc) ||
        !sm_.getFileEntryRefForID(sm_.getFileID(loc)))
      continue;
    if (flags == kEmitInternal && sm_.getFileID(loc) == sm_.getMainFileID())
      continue;
    uint32_t slot = kDropped;
    if (clang::isLambdaCallOperator(def)) {
      auto found = lambdaSlot_.find(def);
      slot = found != lambdaSlot_.end() ? found->second
                                        : lambdaPatternSlot(def);
    } else {
      slot = targetSlot(def);
    }
    if (slot != kDropped && entities_[slot].kind != EntityKind::Function &&
        entities_[slot].kind != EntityKind::Lambda)
      slot = kDropped;
    pendingEmissions_.push_back({def, slot, g.statements[i], flags});
  }
}

// ============================================================================
// Traversal
// ============================================================================

bool ReferenceVisitor::TraverseDecl(clang::Decl *decl) {
  if (!decl)
    return true;
  if (decl->isImplicit())
    return Base::TraverseDecl(decl);
  // A system header's own code references nothing of the project. Skipped
  // per top-level declaration, so a macro from a system header expanded in
  // a user function is still read.
  if (frames_.empty()) {
    clang::SourceLocation loc = decl->getLocation();
    if (loc.isValid() && sm_.isInSystemHeader(loc))
      return true;
  }
  if (auto *fn = llvm::dyn_cast<clang::FunctionDecl>(decl))
    return traverseFunction(fn);
  if (auto *var = llvm::dyn_cast<clang::VarDecl>(decl))
    return traverseVariable(var);
  if (const auto *cls = llvm::dyn_cast<clang::CXXRecordDecl>(decl))
    noteClass(cls);
  else if (auto *tmpl = llvm::dyn_cast<clang::FunctionTemplateDecl>(decl))
    noteExplicitInstantiations(tmpl);
  else if (auto *cls = llvm::dyn_cast<clang::ClassTemplateDecl>(decl))
    noteExplicitInstantiations(cls);
  return Base::TraverseDecl(decl);
}

void ReferenceVisitor::popFrame() {
  const FrameKind kind = frames_.back().kind;
  frames_.pop_back();
  // Marks are consumed within the declaration that set them. A lambda at
  // file scope can end while a sibling operand's mark is still pending, so
  // only a declaration's end clears them.
  if (frames_.empty() && kind != FrameKind::Lambda)
    marks_.clear();
}

bool ReferenceVisitor::traverseFunction(clang::FunctionDecl *fn) {
  // A member of a local class inside an instantiation's body runs only when
  // called: a call schedules it as an instantiation of its own pattern.
  if (mode_ != Mode::AsWritten)
    return true;
  frames_.push_back({fn, kUnresolved, FrameKind::Function, fn->isTemplated()});
  const bool definition =
      fn->isThisDeclarationADefinition() && !fn->isDeleted();
  if (definition)
    noteFunctionDefinition(fn);
  LockState saved;
  enterBody(saved);
  const bool ok = Base::TraverseDecl(fn);
  leaveBody(saved);
  if (ok && definition && !fn->isInvalidDecl()) {
    // What the compiler writes into this function: member and base
    // initializers not written, the body of a defaulted function (the
    // traversal skips both), the destruction of members and bases. None
    // of it runs under a lock the body takes.
    LockState outer;
    enterBody(outer);
    if (const auto *ctor = llvm::dyn_cast<clang::CXXConstructorDecl>(fn)) {
      traverseImplicitInits(ctor);
      if (!ctor->isDependentContext())
        noteConstructorCleanup(ctor);
    }
    if (fn->isDefaulted() && !fn->isDependentContext())
      TraverseStmt(fn->getBody());
    if (const auto *dtor = llvm::dyn_cast<clang::CXXDestructorDecl>(fn))
      noteImplicitDestruction(dtor);
    leaveBody(outer);
  }
  popFrame();
  return ok;
}

bool ReferenceVisitor::traverseVariable(clang::VarDecl *var) {
  if (llvm::isa<clang::ParmVarDecl>(var))
    return Base::TraverseDecl(var);
  // `const T &r = g;` reads g: nothing can be written through r. Any other
  // reference hands out g's identity, also in a template body.
  if (var->hasInit()) {
    if (isConstLValueRef(var->getType()))
      mark(var->getInit(), kMarkRead);
    else if (isMutableRef(var->getType()))
      mark(var->getInit(), kMarkAddress);
    else if (var->getInit()->isTypeDependent() &&
             (var->getType()->isScalarType() ||
              var->getType()->isUndeducedAutoType()))
      mark(var->getInit(), kMarkRead); // int x = table[i]; in a template
  }
  // __attribute__((cleanup(fn))): fn runs when the variable leaves scope.
  if (var->hasAttrs())
    if (const auto *cleanup = var->getAttr<clang::CleanupAttr>())
      if (const clang::FunctionDecl *fn = cleanup->getFunctionDecl())
        if (unevaluated_ == 0)
          addRef(functionTarget(fn), RefRole::Call, cleanup->getLocation());
  // A local's initializer (function-local statics included) belongs to the
  // enclosing function; a namespace-scope variable or static data member is
  // its own referrer. So does its destruction: at the end of the scope, or
  // at exit for a static.
  if (!var->isFileVarDecl() || mode_ != Mode::AsWritten) {
    noteVariableDestruction(var);
    // A function-local static is initialized once, under the guard the
    // compiler emits: its initializer holds that once lock.
    // A thread_local's initializer runs once per thread: no once lock.
    const bool onceInit = var->isStaticLocal() && var->hasInit() &&
                          var->getTLSKind() == clang::VarDecl::TLS_None &&
                          !frames_.empty() && unevaluated_ == 0;
    if (onceInit)
      acquire(onceLock(var));
    const bool ok = Base::TraverseDecl(var);
    if (onceInit)
      release(onceLock(var));
    if (var->isLocalVarDecl() && !var->isStaticLocal())
      noteGuard(var);
    return ok;
  }
  frames_.push_back({var, kUnresolved, FrameKind::Variable, var->isTemplated()});
  if (var->isThisDeclarationADefinition() != clang::VarDecl::DeclarationOnly)
    noteVariableDefinition(var);
  noteVariableDestruction(var);
  LockState saved;
  enterBody(saved);
  const bool ok = Base::TraverseDecl(var);
  leaveBody(saved);
  popFrame();
  return ok;
}

// A definition gets symbol records even when nothing references it when it
// is this TU's own: in the main file, or strong wherever it is written (an
// .inc file the TU includes).
void ReferenceVisitor::noteFunctionDefinition(const clang::FunctionDecl *fn) {
  bool emit = inMainFile(fn->getLocation());
  if (!emit && !fn->isInlined() && !fn->isTemplated())
    emit = linkageOfFunction(fn) == LinkageClass::External;
  if (emit) {
    const uint32_t slot = targetSlot(fn);
    if (slot != kDropped)
      entities_[slot].emit = true;
  }

  const auto *method = llvm::dyn_cast<clang::CXXMethodDecl>(fn);
  if (!method)
    return;
  const clang::CXXRecordDecl *cls = method->getParent();
  if (!cls->hasDefinition())
    return;
  // A user-provided constructor or destructor body stores the vtable
  // pointer: that function is what references the vtable.
  if ((llvm::isa<clang::CXXConstructorDecl>(method) ||
       llvm::isa<clang::CXXDestructorDecl>(method)) &&
      method->isUserProvided() && cls->isDynamicClass())
    noteVtableUse(cls, fn->getLocation());
  // The TU defining the key function emits the vtable and the typeinfo.
  if (method->isVirtual() && !fn->isInlined() && !cls->isInvalidDecl() &&
      !cls->isDependentContext()) {
    const clang::CXXMethodDecl *key = ctx_->getCurrentKeyFunction(cls);
    if (key && key->getCanonicalDecl() == method->getCanonicalDecl()) {
      const uint32_t slot = targetSlot(cls);
      if (slot != kDropped) {
        entities_[slot].flags |= kSymVtableHome;
        entities_[slot].emit = true;
      }
    }
  }
}

void ReferenceVisitor::noteVariableDefinition(const clang::VarDecl *var) {
  bool emit = inMainFile(var->getLocation());
  if (!emit && !var->isInline() && !var->isTemplated())
    emit = linkageOfVariable(var) == LinkageClass::External;
  if (!emit)
    return;
  const uint32_t slot = targetSlot(var);
  if (slot != kDropped)
    entities_[slot].emit = true;
}

void ReferenceVisitor::noteClass(const clang::CXXRecordDecl *cls) {
  if (const auto *spec =
          llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(cls)) {
    if (spec->getSpecializationKind() ==
        clang::TSK_ExplicitInstantiationDefinition) {
      const uint32_t slot = targetSlot(spec);
      if (slot != kDropped) {
        entities_[slot].flags |= kSymExplicitInstantiation;
        entities_[slot].emit = true;
      }
      // The TU emits every member: read their bodies.
      if (spec->hasDefinition())
        noteExplicitMembers(spec->getDefinition());
      return;
    }
  }
  if (!cls->isThisDeclarationADefinition() || !cls->getIdentifier() ||
      cls->isLambda() || !frames_.empty() || !inMainFile(cls->getLocation()))
    return;
  const uint32_t slot = targetSlot(cls);
  if (slot != kDropped)
    entities_[slot].emit = true;
}

// An explicit instantiation of a function template leaves no declaration in
// the enclosing context, only an entry in the template's specializations.
void ReferenceVisitor::noteExplicitInstantiations(
    clang::FunctionTemplateDecl *tmpl) {
  forEachExplicitInstantiation(tmpl, [&](const clang::FunctionDecl *spec) {
    const uint32_t slot = targetSlot(spec);
    if (slot != kDropped) {
      entities_[slot].flags |= kSymExplicitInstantiation;
      entities_[slot].emit = true;
    }
    functionTarget(spec); // the TU emits it: read its body
  });
}

// An explicit instantiation of a member (`template void Box<A>::fill();`)
// leaves no declaration in the enclosing context either; nor do members of
// a nested class of an explicitly instantiated class. Both show as the
// member's specialization kind in the template's specializations.
void ReferenceVisitor::noteExplicitInstantiations(
    clang::ClassTemplateDecl *tmpl) {
  forEachExplicitInstantiation(
      tmpl, [&](const clang::FunctionDecl *fn) { functionTarget(fn); });
}

// The members of `cls` and of its nested classes the TU emits as explicit
// instantiation definitions: their bodies are read.
void ReferenceVisitor::noteExplicitMembers(const clang::CXXRecordDecl *cls) {
  auto read = [&](const clang::FunctionDecl *fn) { functionTarget(fn); };
  detail::forEachExplicitMember(cls, read);
}

bool ReferenceVisitor::TraverseLambdaExpr(clang::LambdaExpr *expr) {
  const clang::CXXMethodDecl *op = expr->getCallOperator();
  if (!op)
    return Base::TraverseLambdaExpr(expr);
  if (mode_ != Mode::AsWritten) {
    // In an instantiation's body the closure is a copy of a lambda the code
    // as written has: its body is read under that lambda, when its call
    // operator runs (a generic one's, when an instantiation is used). A
    // lambda no traversal as written saw (a system template's) is part of
    // the function being read.
    // A lambda the code as written holds (in a default member initializer
    // an implicit constructor reads) was read under itself already.
    const clang::FunctionTemplateDecl *generic =
        op->getDescribedFunctionTemplate();
    const bool copy =
        op->getInstantiatedFromMemberFunction() ||
        (generic && generic->getInstantiatedFromMemberTemplate());
    const uint32_t slot = copy ? lambdaPatternSlot(op) : kDropped;
    if (copy && slot == kDropped)
      return Base::TraverseLambdaExpr(expr);
    if (!copy && sm_.isInSystemHeader(op->getLocation()))
      return Base::TraverseLambdaExpr(expr);
    if (slot != kDropped && !generic)
      scheduleLambda(op, slot);
    for (unsigned i = 0, n = expr->capture_size(); i < n; ++i)
      if (!TraverseLambdaCapture(expr, expr->capture_begin() + i,
                                 expr->capture_init_begin()[i]))
        return false;
    return true;
  }
  // Written under a lock: the enclosing function references the lambda
  // with that lock set, read as where it runs when it is called in place
  // (an algorithm, a callback invoked before the lock is released).
  // A once lock (a static local's initializer) covers the creation, not
  // later calls.
  const uint32_t creationLocks =
      unevaluated_ == 0 ? currentLocksetWithoutOnce() : 0;
  const bool inLoop = lockState_.loopDepth > 0;
  const uint32_t enclosing =
      creationLocks != 0 ? referrerSlot(expr->getBeginLoc()) : kDropped;
  frames_.push_back(
      {op, kUnresolved, FrameKind::Lambda, op->isTemplated(), inLoop});
  // A lambda an instantiation copies needs its identity when that copy is
  // read, whether or not the code as written references anything.
  if (op->isTemplated())
    resolveFrame(frames_.size() - 1);
  if (enclosing != kDropped) {
    const uint32_t self = resolveFrame(frames_.size() - 1);
    if (self != kDropped)
      addRefFrom(enclosing, self, RefRole::Call, expr->getBeginLoc(),
                 creationLocks, inLoop ? kRefInLoop : 0);
  }
  LockState saved;
  enterBody(saved);
  if (auto once = onceLambdas_.find(expr); once != onceLambdas_.end())
    acquire(once->second);
  const bool ok = Base::TraverseLambdaExpr(expr);
  leaveBody(saved);
  popFrame();
  return ok;
}

// ---- lexical lock scopes and loops ---------------------------------------------

namespace {
[[nodiscard]] const clang::LambdaExpr *lambdaArgument(const clang::Expr *arg);
[[nodiscard]] const clang::LambdaExpr *heldLambda(const clang::Expr *arg);
} // namespace

namespace {

// The statement has a break that leaves it: one not inside a nested loop
// or switch (or an expression: a lambda's body is its own).
[[nodiscard]] bool hasOwnBreak(const clang::Stmt *stmt) {
  if (!stmt || llvm::isa<clang::Expr>(stmt))
    return false;
  if (llvm::isa<clang::BreakStmt>(stmt))
    return true;
  if (llvm::isa<clang::ForStmt, clang::WhileStmt, clang::DoStmt,
                clang::CXXForRangeStmt, clang::SwitchStmt>(stmt))
    return false;
  for (const clang::Stmt *child : stmt->children())
    if (hasOwnBreak(child))
      return true;
  return false;
}

// A loop condition spelled as a constant true: absent (`for (;;)`),
// `true` or a nonzero integer.
[[nodiscard]] bool spelledTrue(const clang::Expr *cond) {
  if (!cond)
    return true;
  cond = cond->IgnoreParenImpCasts();
  if (const auto *b = llvm::dyn_cast<clang::CXXBoolLiteralExpr>(cond))
    return b->getValue();
  if (const auto *i = llvm::dyn_cast<clang::IntegerLiteral>(cond))
    return i->getValue().getBoolValue();
  return false;
}

// The statement cannot fall through its end: it is (or its last statement
// is) a return, break, continue, goto, co_return, throw, a call of a
// noreturn function, an if both of whose branches jump, a switch with a
// default and no break whose last statement jumps, or a loop spelled to
// run forever with no break.
[[nodiscard]] bool jumps(const clang::Stmt *stmt) {
  while (stmt) {
    if (const auto *label = llvm::dyn_cast<clang::LabelStmt>(stmt))
      stmt = label->getSubStmt();
    else if (const auto *attributed = llvm::dyn_cast<clang::AttributedStmt>(stmt))
      stmt = attributed->getSubStmt();
    else if (const auto *sc = llvm::dyn_cast<clang::SwitchCase>(stmt))
      stmt = sc->getSubStmt();
    else if (const auto *full = llvm::dyn_cast<clang::FullExpr>(stmt))
      stmt = full->getSubExpr();
    else if (const auto *block = llvm::dyn_cast<clang::CompoundStmt>(stmt))
      stmt = block->body_empty() ? nullptr : block->body_back();
    else
      break;
  }
  if (!stmt)
    return false;
  if (llvm::isa<clang::ReturnStmt, clang::BreakStmt, clang::ContinueStmt,
                clang::GotoStmt, clang::CoreturnStmt, clang::CXXThrowExpr>(
          stmt))
    return true;
  if (const auto *branch = llvm::dyn_cast<clang::IfStmt>(stmt))
    return branch->getElse() && jumps(branch->getThen()) &&
           jumps(branch->getElse());
  if (const auto *call = llvm::dyn_cast<clang::CallExpr>(stmt))
    if (const clang::FunctionDecl *fn = call->getDirectCallee())
      return fn->isNoReturn();
  if (const auto *sw = llvm::dyn_cast<clang::SwitchStmt>(stmt)) {
    bool hasDefault = false;
    for (const clang::SwitchCase *c = sw->getSwitchCaseList(); c;
         c = c->getNextSwitchCase())
      hasDefault |= llvm::isa<clang::DefaultStmt>(c);
    return hasDefault && !hasOwnBreak(sw->getBody()) && jumps(sw->getBody());
  }
  if (const auto *loop = llvm::dyn_cast<clang::ForStmt>(stmt))
    return spelledTrue(loop->getCond()) && !hasOwnBreak(loop->getBody());
  if (const auto *loop = llvm::dyn_cast<clang::WhileStmt>(stmt))
    return spelledTrue(loop->getCond()) && !hasOwnBreak(loop->getBody());
  if (const auto *loop = llvm::dyn_cast<clang::DoStmt>(stmt))
    return spelledTrue(loop->getCond()) && !hasOwnBreak(loop->getBody());
  return false;
}

// How long a held lock lasts: its guard's scope, the scope a met unknown
// lock was given, or the body (null).
template <typename Lock> const clang::VarDecl *lifetimeOf(const Lock &h) {
  return h.guard ? h.guard : h.scope;
}

// Must-hold meet: keep in `a` only the locks active in both `a` and `b`. A
// guard active only in `a` stays as an inactive guard (later toggles still
// find it); a lock taken by hand only in `a` is dropped. True when each
// side held a lock the other did not: some lock is held, but not which
// (ReferenceVisitor::meetLocks). `lifetimes` gets the lifetimes of the
// locks held on one side only.
template <typename Locks>
[[nodiscard]] bool
meetHeld(Locks &a, const Locks &b,
         llvm::SmallVectorImpl<const clang::VarDecl *> &lifetimes) {
  const auto activeIn = [](const Locks &locks, uint32_t lock) {
    for (const auto &h : locks)
      if (h.active && h.lock == lock)
        return true;
    return false;
  };
  lifetimes.clear();
  bool onlyB = false;
  for (const auto &h : b)
    if (h.active && !activeIn(a, h.lock)) {
      onlyB = true;
      lifetimes.push_back(lifetimeOf(h));
    }
  bool onlyA = false;
  for (size_t i = a.size(); i-- > 0;) {
    if (!a[i].active || activeIn(b, a[i].lock))
      continue;
    onlyA = true;
    lifetimes.push_back(lifetimeOf(a[i]));
    if (a[i].guard)
      a[i].active = false;
    else
      a.erase(a.begin() + static_cast<std::ptrdiff_t>(i));
  }
  return onlyA && onlyB;
}

// Drop the guards in `held` that `outer` does not have: they were declared
// in a scope that has ended (a block, an if or switch init statement). A
// met unknown lock scoped to such a guard goes with it.
template <typename Locks> void dropScopedGuards(Locks &held, const Locks &outer) {
  const auto declaredInside = [&](const auto &h) {
    const clang::VarDecl *lifetime = lifetimeOf(h);
    if (!lifetime)
      return false;
    for (const auto &o : outer)
      if (o.guard == lifetime)
        return false;
    return true;
  };
  held.erase(std::remove_if(held.begin(), held.end(), declaredInside),
             held.end());
}

template <typename Locks> bool sameHeld(const Locks &a, const Locks &b) {
  return a.size() == b.size() &&
         std::equal(a.begin(), a.end(), b.begin(),
                    [](const auto &x, const auto &y) {
                      return x.lock == y.lock && x.guard == y.guard &&
                             x.active == y.active && x.met == y.met &&
                             x.scope == y.scope;
                    });
}

// The condition is a constant that is true (`while (true)`, `for (;;)`) or
// false (`do ... while (0)`); std::nullopt when it is not constant.
std::optional<bool> constantCondition(const clang::Expr *cond,
                                      const clang::ASTContext *ctx) {
  if (!cond)
    return true;
  if (!ctx || cond->isValueDependent())
    return std::nullopt;
  bool value = false;
  if (!cond->EvaluateAsBooleanCondition(value, *ctx))
    return std::nullopt;
  return value;
}

} // namespace

void ReferenceVisitor::assignHeld(const HeldLocks &held) {
  if (sameHeld(lockState_.held, held))
    return;
  lockState_.held = held;
  heldSetValid_ = false;
}

// A different lock on each path (`if (x) a.lock(); else b.lock();`) meets
// to an unknown lock, not to no lock. It lasts as long as the longest-lived
// of those locks (the outermost guard, or the body when one was taken by
// hand), and the paired unlocks after the join (an unlock of a mutex not
// held) drop it.
void ReferenceVisitor::meetLocks(HeldLocks &a, const HeldLocks &b) {
  llvm::SmallVector<const clang::VarDecl *, 4> lifetimes;
  if (!meetHeld(a, b, lifetimes))
    return;
  const uint32_t unknown = unknownLock();
  for (const HeldLock &h : a)
    if (h.active && h.lock == unknown)
      return;
  // Guards in scope here are nested: the first declared is the outermost.
  const clang::VarDecl *scope = lifetimes.front();
  for (const clang::VarDecl *lifetime : lifetimes) {
    if (!lifetime || !scope) {
      scope = nullptr;
      break;
    }
    if (sm_.isBeforeInTranslationUnit(lifetime->getLocation(),
                                      scope->getLocation()))
      scope = lifetime;
  }
  a.push_back({unknown, nullptr, true, true, scope});
}

void ReferenceVisitor::meetLocksInto(bool &any, HeldLocks &into,
                                     const HeldLocks &state) {
  if (!any) {
    into = state;
    any = true;
  } else {
    meetLocks(into, state);
  }
}

// A label a forward goto reaches starts from the states at those gotos,
// met with the state above it when that falls through. A label reached
// only by a backward goto keeps the state above it.
void ReferenceVisitor::enterLabels(const clang::Stmt *stmt, bool falls) {
  while (const auto *label = llvm::dyn_cast_or_null<clang::LabelStmt>(stmt)) {
    if (auto it = gotoStates_.find(label->getDecl()); it != gotoStates_.end()) {
      HeldLocks in = std::move(it->second.held);
      gotoStates_.erase(it);
      dropScopedGuards(in, lockState_.held); // the guards a goto left behind
      if (falls)
        meetLocks(in, lockState_.held);
      assignHeld(in);
      falls = true;
    }
    stmt = label->getSubStmt();
  }
}

bool ReferenceVisitor::TraverseGotoStmt(clang::GotoStmt *stmt) {
  const clang::LabelDecl *label = stmt->getLabel();
  if (label && !sm_.isBeforeInTranslationUnit(label->getLocation(),
                                              stmt->getGotoLoc())) {
    GotoState &state = gotoStates_[label];
    meetLocksInto(state.any, state.held, lockState_.held);
  }
  return Base::TraverseGotoStmt(stmt);
}

bool ReferenceVisitor::TraverseCompoundStmt(clang::CompoundStmt *stmt) {
  // A guard declared in the block is released at its end. A lock taken or
  // dropped by hand inside it stays so after it (a `do { lock } while (0)`
  // macro). A block that ends in a jump leaves the state as it was before
  // it: the code after it is reached from elsewhere. Branches and loops
  // meet their paths' states (TraverseIfStmt, finishLoop).
  const HeldLocks saved = lockState_.held;
  if (!WalkUpFromCompoundStmt(stmt))
    return false;
  bool falls = true;
  for (clang::Stmt *child : stmt->body()) {
    enterLabels(child, falls);
    if (!TraverseStmt(child))
      return false;
    falls = !jumps(child);
  }
  if (jumps(stmt)) {
    assignHeld(saved);
  } else {
    HeldLocks after = lockState_.held;
    dropScopedGuards(after, saved);
    assignHeld(after);
  }
  return true;
}

bool ReferenceVisitor::TraverseIfStmt(clang::IfStmt *stmt) {
  // Each branch starts from the state after the condition; after the if a
  // lock is held when it is held at the end of every branch that falls
  // through.
  if (!WalkUpFromIfStmt(stmt))
    return false;
  const HeldLocks outer = lockState_.held;
  if (!TraverseStmt(stmt->getInit()) ||
      !TraverseStmt(stmt->getConditionVariableDeclStmt()) ||
      !TraverseStmt(stmt->getCond()))
    return false;
  const HeldLocks entry = lockState_.held;
  if (!TraverseStmt(stmt->getThen()))
    return false;
  const bool thenJumps = jumps(stmt->getThen());
  const HeldLocks thenExit = lockState_.held;
  assignHeld(entry);
  bool elseJumps = false;
  if (clang::Stmt *otherwise = stmt->getElse()) {
    if (!TraverseStmt(otherwise))
      return false;
    elseJumps = jumps(otherwise);
  }
  HeldLocks after = lockState_.held; // the else branch's end, or entry
  if (thenJumps && elseJumps)
    after = entry; // nothing falls through
  else if (elseJumps)
    after = thenExit;
  else if (!thenJumps)
    meetLocks(after, thenExit);
  dropScopedGuards(after, outer);
  assignHeld(after);
  return true;
}

bool ReferenceVisitor::TraverseSwitchStmt(clang::SwitchStmt *stmt) {
  // Each case starts from the state at the switch, met with the state of
  // the case above when that one falls through. After the switch a lock is
  // held when it is held at every break, at the end of the body when it
  // falls through, and at the switch when no default catches the rest.
  if (!WalkUpFromSwitchStmt(stmt))
    return false;
  const HeldLocks outer = lockState_.held;
  if (!TraverseStmt(stmt->getInit()) ||
      !TraverseStmt(stmt->getConditionVariableDeclStmt()) ||
      !TraverseStmt(stmt->getCond()))
    return false;
  const HeldLocks entry = lockState_.held;
  bool hasDefault = false;
  for (const clang::SwitchCase *c = stmt->getSwitchCaseList(); c;
       c = c->getNextSwitchCase())
    hasDefault |= llvm::isa<clang::DefaultStmt>(c);
  flow_.push_back(FlowFrame{});
  flow_.back().isSwitch = true;
  bool ok = true;
  bool falls = false;
  clang::Stmt *body = stmt->getBody();
  if (auto *block = llvm::dyn_cast_or_null<clang::CompoundStmt>(body)) {
    for (clang::Stmt *child : block->body()) {
      if (llvm::isa<clang::SwitchCase>(child)) {
        HeldLocks start = entry;
        if (falls)
          meetLocks(start, lockState_.held);
        assignHeld(start);
      }
      if (!(ok = TraverseStmt(child)))
        break;
      falls = !jumps(child);
    }
  } else if (body) {
    ok = TraverseStmt(body);
    falls = !jumps(body);
  }
  FlowFrame frame = std::move(flow_.back());
  flow_.pop_back();
  if (!ok)
    return false;
  bool any = frame.anyBreak;
  HeldLocks after = std::move(frame.breaks);
  if (falls)
    meetLocksInto(any, after, lockState_.held);
  if (!hasDefault)
    meetLocksInto(any, after, entry);
  if (!any)
    after = entry; // nothing leaves the switch: what follows is unreachable
  dropScopedGuards(after, outer);
  assignHeld(after);
  return true;
}

bool ReferenceVisitor::TraverseBreakStmt(clang::BreakStmt *stmt) {
  if (!flow_.empty())
    meetLocksInto(flow_.back().anyBreak, flow_.back().breaks, lockState_.held);
  return Base::TraverseBreakStmt(stmt);
}

bool ReferenceVisitor::TraverseContinueStmt(clang::ContinueStmt *stmt) {
  for (size_t i = flow_.size(); i-- > 0;)
    if (!flow_[i].isSwitch) {
      meetLocksInto(flow_[i].anyContinue, flow_[i].continues, lockState_.held);
      break;
    }
  return Base::TraverseContinueStmt(stmt);
}

// A loop leaves through its condition (from the state before it when it
// can run zero times, and from the end of its body and each continue) and
// through its breaks; a lock is held after it when it is held on all of
// those. A loop whose condition is constant true leaves by breaks only.
void ReferenceVisitor::finishLoop(const HeldLocks &outer, bool zeroTrips,
                                  bool condTrue, bool bodyFalls) {
  FlowFrame frame = std::move(flow_.back());
  flow_.pop_back();
  bool any = frame.anyBreak;
  HeldLocks after = std::move(frame.breaks);
  if (!condTrue) {
    if (zeroTrips)
      meetLocksInto(any, after, outer);
    if (bodyFalls)
      meetLocksInto(any, after, lockState_.held);
    if (frame.anyContinue)
      meetLocksInto(any, after, frame.continues);
  }
  if (!any)
    after = outer; // the loop never leaves: what follows is unreachable
  dropScopedGuards(after, outer);
  assignHeld(after);
}

bool ReferenceVisitor::TraverseForStmt(clang::ForStmt *stmt) {
  const HeldLocks outer = lockState_.held;
  flow_.push_back(FlowFrame{});
  ++lockState_.loopDepth;
  const bool ok = Base::TraverseForStmt(stmt);
  --lockState_.loopDepth;
  const auto cond = constantCondition(stmt->getCond(), ctx_);
  finishLoop(outer, true, cond.value_or(false), !jumps(stmt->getBody()));
  return ok;
}

bool ReferenceVisitor::TraverseWhileStmt(clang::WhileStmt *stmt) {
  const HeldLocks outer = lockState_.held;
  flow_.push_back(FlowFrame{});
  ++lockState_.loopDepth;
  const bool ok = Base::TraverseWhileStmt(stmt);
  --lockState_.loopDepth;
  const auto cond = constantCondition(stmt->getCond(), ctx_);
  finishLoop(outer, true, cond.value_or(false), !jumps(stmt->getBody()));
  return ok;
}

bool ReferenceVisitor::TraverseDoStmt(clang::DoStmt *stmt) {
  // `do { ... } while (0)` (a statement macro) runs its body once: not a
  // loop for in_loop, and its exits are the body's end and its breaks.
  const auto cond = constantCondition(stmt->getCond(), ctx_);
  const bool once = cond.has_value() && !*cond;
  const HeldLocks outer = lockState_.held;
  flow_.push_back(FlowFrame{});
  if (!once)
    ++lockState_.loopDepth;
  const bool ok = Base::TraverseDoStmt(stmt);
  if (!once)
    --lockState_.loopDepth;
  finishLoop(outer, false, cond.value_or(false), !jumps(stmt->getBody()));
  return ok;
}

bool ReferenceVisitor::TraverseCXXForRangeStmt(clang::CXXForRangeStmt *stmt) {
  const HeldLocks outer = lockState_.held;
  flow_.push_back(FlowFrame{});
  ++lockState_.loopDepth;
  const bool ok = Base::TraverseCXXForRangeStmt(stmt);
  --lockState_.loopDepth;
  finishLoop(outer, true, false, !jumps(stmt->getBody()));
  return ok;
}

// The syntactic form of a braced initializer has no implicit conversions,
// so `int t[] = {g}` would not show g being read. Read the semantic form
// when Sema built one.
bool ReferenceVisitor::TraverseInitListExpr(clang::InitListExpr *expr) {
  clang::InitListExpr *semantic =
      expr->isSemanticForm() ? expr : expr->getSemanticForm();
  return Base::TraverseSynOrSemInitListExpr(semantic ? semantic : expr);
}

bool ReferenceVisitor::TraverseUnaryExprOrTypeTraitExpr(
    clang::UnaryExprOrTypeTraitExpr *expr) {
  ++unevaluated_;
  const bool ok = Base::TraverseUnaryExprOrTypeTraitExpr(expr);
  --unevaluated_;
  return ok;
}

bool ReferenceVisitor::TraverseCXXNoexceptExpr(clang::CXXNoexceptExpr *expr) {
  ++unevaluated_;
  const bool ok = Base::TraverseCXXNoexceptExpr(expr);
  --unevaluated_;
  return ok;
}

// A requires-expression and a concept's constraint are never evaluated.
// Non-dependent names in them carry Sema's own "unevaluated" mark, but an
// overload set a template cannot resolve yet does not.
bool ReferenceVisitor::TraverseRequiresExpr(clang::RequiresExpr *expr) {
  ++unevaluated_;
  const bool ok = Base::TraverseRequiresExpr(expr);
  --unevaluated_;
  return ok;
}

bool ReferenceVisitor::TraverseConceptDecl(clang::ConceptDecl *decl) {
  ++unevaluated_;
  const bool ok = Base::TraverseConceptDecl(decl);
  --unevaluated_;
  return ok;
}

// decltype(e) and typeof(e) name what e would call without calling it. In a
// template an overload set there carries no "unevaluated" mark of its own.
// Clang 22 passes whether to walk the type's qualifier as well.
#if VYCOR_LLVM_AT_LEAST(22)
#define VYCOR_TYPELOC_PARAMS(T) T loc, bool qualifier
#define VYCOR_TYPELOC_ARGS loc, qualifier
#else
#define VYCOR_TYPELOC_PARAMS(T) T loc
#define VYCOR_TYPELOC_ARGS loc
#endif

bool ReferenceVisitor::TraverseDecltypeTypeLoc(
    VYCOR_TYPELOC_PARAMS(clang::DecltypeTypeLoc)) {
  ++unevaluated_;
  const bool ok = Base::TraverseDecltypeTypeLoc(VYCOR_TYPELOC_ARGS);
  --unevaluated_;
  return ok;
}

bool ReferenceVisitor::TraverseTypeOfExprTypeLoc(
    VYCOR_TYPELOC_PARAMS(clang::TypeOfExprTypeLoc)) {
  ++unevaluated_;
  const bool ok = Base::TraverseTypeOfExprTypeLoc(VYCOR_TYPELOC_ARGS);
  --unevaluated_;
  return ok;
}

#undef VYCOR_TYPELOC_PARAMS
#undef VYCOR_TYPELOC_ARGS

// A default member initializer used by a constructor or an aggregate
// initialization runs there; the traversal visits it only with implicit
// code (and not at all in older releases).
bool ReferenceVisitor::TraverseCXXDefaultInitExpr(
    clang::CXXDefaultInitExpr *expr) {
  if (!frames_.empty())
    return TraverseStmt(expr->getExpr());
  return true;
}

bool ReferenceVisitor::TraverseCXXTypeidExpr(clang::CXXTypeidExpr *expr) {
  // typeid evaluates its operand only for a glvalue of polymorphic class
  // type; the typeinfo itself is used either way.
  bool evaluated = false;
  clang::QualType operand;
  if (expr->isTypeOperand()) {
    operand = expr->getTypeOperandSourceInfo()->getType();
  } else {
    const clang::Expr *op = expr->getExprOperand();
    operand = op->getType();
    if (op->isGLValue()) {
      const clang::CXXRecordDecl *cls = operand->getAsCXXRecordDecl();
      evaluated = cls && cls->hasDefinition() && cls->isPolymorphic();
    }
  }
  if (unevaluated_ == 0)
    noteTypeinfoUse(operand, expr->getBeginLoc());
  if (!evaluated)
    ++unevaluated_;
  const bool ok = Base::TraverseCXXTypeidExpr(expr);
  if (!evaluated)
    --unevaluated_;
  return ok;
}

// ---- role marks -------------------------------------------------------------

// Walk from an operand down to the expression naming the global the access
// applies to: through parentheses, member access by value, indexing of an
// array (not of a pointer), no-op and base-class conversions, both arms of
// a conditional and the right side of a comma. Anything else (a call, a
// dereference, a temporary) ends the access path.
void ReferenceVisitor::mark(const clang::Expr *expr, uint8_t bits) {
  while (expr) {
    expr = expr->IgnoreParens();
    if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(expr)) {
      if (namesLinkable(ref->getDecl()))
        marks_.try_emplace(ref, bits);
      return;
    }
    if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(expr)) {
      if (namesLinkable(member->getMemberDecl())) {
        marks_.try_emplace(member, bits);
        return;
      }
      if (member->isArrow())
        return;
      expr = member->getBase();
      continue;
    }
    if (const auto *index = llvm::dyn_cast<clang::ArraySubscriptExpr>(expr)) {
      const clang::Expr *base = subscriptBase(index)->IgnoreParens();
      const auto *decay = llvm::dyn_cast<clang::ImplicitCastExpr>(base);
      if (decay && decay->getCastKind() == clang::CK_ArrayToPointerDecay) {
        expr = decay->getSubExpr();
        continue;
      }
      // Through a pointer the element is not part of the named object. A
      // base that is still an array, a vector or (in a template body, where
      // operator[] is not resolved yet) a class object is.
      if (decay || base->getType().isNull() ||
          base->getType()->isAnyPointerType())
        return;
      expr = base;
      continue;
    }
    if (const auto *cast = llvm::dyn_cast<clang::ImplicitCastExpr>(expr)) {
      switch (cast->getCastKind()) {
      case clang::CK_NoOp:
      case clang::CK_DerivedToBase:
      case clang::CK_UncheckedDerivedToBase:
        expr = cast->getSubExpr();
        continue;
      default:
        return;
      }
    }
    if (const auto *cond = llvm::dyn_cast<clang::ConditionalOperator>(expr)) {
      mark(cond->getTrueExpr(), bits);
      expr = cond->getFalseExpr();
      continue;
    }
    if (const auto *binary = llvm::dyn_cast<clang::BinaryOperator>(expr)) {
      if (binary->getOpcode() != clang::BO_Comma)
        return;
      expr = binary->getRHS();
      continue;
    }
    return;
  }
}

uint8_t ReferenceVisitor::marksOf(const clang::Expr *expr) const {
  auto it = marks_.find(expr);
  return it == marks_.end() ? 0 : it->second;
}

// An argument bound to `const T &` is read; bound to any other reference
// its identity escapes. By value the lvalue-to-rvalue conversion (or the
// copy constructor's own parameter) says so.
namespace {

// True when `a` and `b` take the same parameters and ref-qualifier.
bool sameSignature(const clang::FunctionDecl *a, const clang::FunctionDecl *b) {
  if (a->getNumParams() != b->getNumParams())
    return false;
  const auto *ma = llvm::dyn_cast<clang::CXXMethodDecl>(a);
  const auto *mb = llvm::dyn_cast<clang::CXXMethodDecl>(b);
  if (!ma || !mb || ma->getRefQualifier() != mb->getRefQualifier())
    return false;
  const clang::ASTContext &ctx = a->getASTContext();
  for (unsigned i = 0, n = a->getNumParams(); i < n; ++i)
    if (!ctx.hasSameType(a->getParamDecl(i)->getType(),
                         b->getParamDecl(i)->getType()))
      return false;
  return true;
}

// A class returned by value that writes through to its source when
// assigned (std::vector<bool>::reference, std::bitset::reference): it has
// an assignment operator other than copy and move.
bool isWriteProxy(const clang::CXXRecordDecl *cls) {
  const clang::CXXRecordDecl *def = cls ? cls->getDefinition() : nullptr;
  if (!def)
    return false;
  for (const clang::CXXMethodDecl *method : def->methods())
    if (method->getOverloadedOperator() == clang::OO_Equal &&
        !method->isCopyAssignmentOperator() &&
        !method->isMoveAssignmentOperator())
      return true;
  return false;
}

// A non-const member whose result cannot change the object by itself
// being returned: not a reference or pointer to something non-const, nor
// a proxy that writes through when assigned.
bool returnsDetached(const clang::FunctionDecl *fn) {
  const clang::QualType ret = fn->getReturnType();
  if (ret->isReferenceType() || ret->isPointerType() ||
      ret->isMemberPointerType())
    return ret->getPointeeType().isConstQualified();
  if (ret->isDependentType())
    return false;
  return !isWriteProxy(ret->getAsCXXRecordDecl());
}

// The non-const `method` has a const overload taking the same parameters.
bool hasConstTwin(const clang::CXXMethodDecl *method) {
  const clang::CXXRecordDecl *cls = method->getParent();
  if (!cls)
    return false;
  const clang::FunctionTemplateDecl *tmpl = method->getPrimaryTemplate();
  const clang::FunctionDecl *self = method;
  if (tmpl)
    self = tmpl->getTemplatedDecl();
  for (const clang::NamedDecl *decl : cls->lookup(method->getDeclName())) {
    const clang::NamedDecl *under = decl->getUnderlyingDecl();
    const clang::FunctionDecl *other = nullptr;
    if (tmpl) {
      if (const auto *ft = llvm::dyn_cast<clang::FunctionTemplateDecl>(under))
        other = ft->getTemplatedDecl();
    } else {
      other = llvm::dyn_cast<clang::CXXMethodDecl>(under);
    }
    const auto *twin = llvm::dyn_cast_or_null<clang::CXXMethodDecl>(other);
    if (twin && twin != self && twin->isConst() && !twin->isStatic() &&
        sameSignature(self, twin))
      return true;
  }
  return false;
}

} // namespace

// The mark a member call puts on its object: a const member reads it, any
// other may write it. A non-const member with a const overload of the same
// parameters that returns by value (a non-const map's find, begin, end) is
// a weak write: the call does not change the object, what it returns
// might later.
uint8_t ReferenceVisitor::objectMarks(const clang::CXXMethodDecl *method) {
  if (method->isConst())
    return kMarkRead;
  if (returnsDetached(method) && hasConstTwin(method))
    return kMarkWrite | kMarkWeakWrite;
  return kMarkWrite;
}

void ReferenceVisitor::markArgument(const clang::FunctionDecl *callee,
                                    unsigned param, const clang::Expr *arg) {
  if (param >= callee->getNumParams())
    return; // variadic tail
  clang::QualType type = callee->getParamDecl(param)->getType();
  if (isConstLValueRef(type))
    mark(arg, kMarkRead);
  else if (isMutableRef(type))
    mark(arg, kMarkAddress);
}

// ---- template bodies ----------------------------------------------------------
//
// In a template body an expression that depends on a template parameter is
// kept unresolved: no overload is chosen and the conversions that mark a
// read elsewhere are not there. The forms below are read as the built-in
// operation they spell; any other unresolved use of a global stays unmarked
// and is recorded as its identity escaping (address_of), never as a read.

void ReferenceVisitor::markDependentOperands(const clang::Expr *lhs,
                                             const clang::Expr *rhs,
                                             clang::BinaryOperatorKind op) {
  if (clang::BinaryOperator::isCompoundAssignmentOp(op))
    mark(lhs, kMarkRead | kMarkWrite);
  else if (op == clang::BO_Assign)
    mark(lhs, kMarkWrite);
  else if (op == clang::BO_Shl || op == clang::BO_Shr ||
           op == clang::BO_PtrMemD || op == clang::BO_PtrMemI ||
           op == clang::BO_Comma)
    ; // a stream insertion writes its left side; leave it escaping
  else
    mark(lhs, kMarkRead);
  if (op != clang::BO_Comma)
    mark(rhs, kMarkRead);
}

bool ReferenceVisitor::VisitArraySubscriptExpr(
    clang::ArraySubscriptExpr *expr) {
  if (!expr->isTypeDependent() && !expr->isInstantiationDependent())
    return true;
  const clang::Expr *base = subscriptBase(expr);
  mark(base == expr->getLHS() ? expr->getRHS() : expr->getLHS(), kMarkRead);
  base = base->IgnoreParens();
  if (!base->getType().isNull() && base->getType()->isAnyPointerType())
    mark(base, kMarkRead); // the pointer is read; what it points at is not g
  return true;
}

bool ReferenceVisitor::VisitExplicitCastExpr(clang::ExplicitCastExpr *expr) {
  // static_cast<T>(g), (T)g: a conversion to a value.
  if (expr->isTypeDependent() && !expr->getTypeAsWritten().isNull() &&
      !expr->getTypeAsWritten()->isReferenceType())
    mark(expr->getSubExpr(), kMarkRead);
  return true;
}

bool ReferenceVisitor::VisitReturnStmt(clang::ReturnStmt *stmt) {
  // `return g;` from a function that returns by value copies g out. Where
  // the conversion is already known a cast says so; here it is not.
  if (frames_.empty() || !frames_.back().dependent || !stmt->getRetValue())
    return true;
  const auto *fn = llvm::dyn_cast<clang::FunctionDecl>(frames_.back().decl);
  if (!fn)
    return true;
  clang::QualType type = fn->getReturnType();
  if (type.isNull() || type->isReferenceType())
    return true;
  if (const clang::AutoType *deduced = type->getContainedAutoType())
    if (deduced->isDecltypeAuto())
      return true;
  mark(stmt->getRetValue(), kMarkRead);
  return true;
}

bool ReferenceVisitor::VisitImplicitCastExpr(clang::ImplicitCastExpr *expr) {
  if (expr->getCastKind() == clang::CK_LValueToRValue)
    mark(expr->getSubExpr(), kMarkRead);
  return true;
}

bool ReferenceVisitor::VisitBinaryOperator(clang::BinaryOperator *expr) {
  if (expr->isTypeDependent())
    markDependentOperands(expr->getLHS(), expr->getRHS(), expr->getOpcode());
  else if (expr->getOpcode() == clang::BO_Assign)
    mark(expr->getLHS(), kMarkWrite);
  else if (expr->isCompoundAssignmentOp())
    mark(expr->getLHS(), kMarkRead | kMarkWrite);
  return true;
}

bool ReferenceVisitor::VisitUnaryOperator(clang::UnaryOperator *expr) {
  if (expr->isIncrementDecrementOp())
    mark(expr->getSubExpr(), kMarkRead | kMarkWrite);
  else if (expr->getOpcode() == clang::UO_AddrOf)
    mark(expr->getSubExpr(), kMarkAddress);
  else if (expr->isTypeDependent() && expr->isArithmeticOp())
    mark(expr->getSubExpr(), kMarkRead); // -g, !g in a template body
  return true;
}

bool ReferenceVisitor::VisitCXXForRangeStmt(clang::CXXForRangeStmt *stmt) {
  // `for (auto x : g)` and `for (const auto &x : g)` only read g.
  const clang::VarDecl *loopVar = stmt->getLoopVariable();
  const clang::Expr *range = stmt->getRangeInit();
  if (!loopVar || !range)
    return true;
  clang::QualType type = loopVar->getType();
  if (!type.isNull() && (!type->isReferenceType() || isConstLValueRef(type)))
    mark(range, kMarkRead);
  // The loop is written with begin(), end(), operator!=, operator++ and
  // operator* of the range, though none is spelled: the traversal skips
  // those statements, and a user's free begin() would look unreferenced.
  // (Null in a template body, where the range's type is not known.)
  const auto initOf = [](const clang::DeclStmt *decl) -> clang::Expr * {
    if (!decl || !decl->isSingleDecl())
      return nullptr;
    const auto *var = llvm::dyn_cast<clang::VarDecl>(decl->getSingleDecl());
    return var ? const_cast<clang::Expr *>(var->getInit()) : nullptr;
  };
  TraverseStmt(initOf(stmt->getBeginStmt()));
  TraverseStmt(initOf(stmt->getEndStmt()));
  TraverseStmt(stmt->getCond());
  TraverseStmt(stmt->getInc());
  TraverseStmt(const_cast<clang::Expr *>(loopVar->getInit()));
  return true;
}

// a != b rewritten to !(a == b), a < b to (a <=> b) < 0: the traversal
// visits the operands as written, so the operator actually called is read
// off the semantic form here.
void ReferenceVisitor::noteRewrittenCallees(const clang::Expr *expr,
                                            const clang::Expr *lhs,
                                            const clang::Expr *rhs,
                                            clang::SourceLocation loc) {
  if (!expr || expr == lhs || expr == rhs)
    return;
  if (const auto *call = llvm::dyn_cast<clang::CallExpr>(expr))
    if (const clang::FunctionDecl *fn = call->getDirectCallee()) {
      addRef(functionTarget(fn), RefRole::Call, loc);
      markCallArguments(call, fn);
    }
  for (const clang::Stmt *child : expr->children())
    noteRewrittenCallees(llvm::dyn_cast_or_null<clang::Expr>(child), lhs, rhs,
                         loc);
}

bool ReferenceVisitor::VisitCXXRewrittenBinaryOperator(
    clang::CXXRewrittenBinaryOperator *expr) {
  if (unevaluated_ > 0)
    return true;
  const auto form = expr->getDecomposedForm();
  noteRewrittenCallees(expr->getSemanticForm(), form.LHS, form.RHS,
                       expr->getOperatorLoc());
  return true;
}

// Tuple-like structured bindings call get<I>() once per name, in a hidden
// variable the traversal does not enter.
bool ReferenceVisitor::VisitBindingDecl(clang::BindingDecl *decl) {
  if (const clang::VarDecl *holding = decl->getHoldingVar())
    TraverseStmt(const_cast<clang::Expr *>(holding->getInit()));
  return true;
}

// `delete p` runs the destructor of what p points at, then the
// deallocation function.
// Through a virtual destructor both are one virtual call: the deleting
// destructor the vtable holds calls the deallocation function (`::delete p`
// still calls the global one itself). What that destructor does is the
// dynamic type's, not read here.
bool ReferenceVisitor::VisitCXXDeleteExpr(clang::CXXDeleteExpr *expr) {
  if (unevaluated_ > 0)
    return true;
  const clang::SourceLocation site = expr->getBeginLoc();
  const clang::CXXDestructorDecl *dtor = nullptr;
  bool dispatched = false;
  clang::QualType type = expr->getDestroyedType();
  const clang::CXXRecordDecl *cls =
      type.isNull() ? nullptr : type->getAsCXXRecordDecl();
  if (cls && cls->hasDefinition()) {
    cls = cls->getDefinition();
    if (!cls->isInvalidDecl() && !cls->isDependentContext() &&
        !cls->hasTrivialDestructor())
      dtor = cls->getDestructor();
    dispatched = dtor && dtor->isVirtual() && !expr->isArrayForm() &&
                 !cls->isEffectivelyFinal() && !dtor->hasAttr<clang::FinalAttr>();
  }
  if (const clang::FunctionDecl *dealloc = expr->getOperatorDelete())
    if (!dispatched || expr->isGlobalDelete())
      addRef(functionTarget(dealloc), RefRole::Call, site);
  if (!dtor)
    return true;
  addRef(dispatched ? targetSlot(dtor) : functionTarget(dtor), RefRole::Call,
         site);
  return true;
}

// `new T(...)` calls the allocation function; the constructor is the
// construct-expression below it. The matching deallocation function runs if
// the constructor throws.
bool ReferenceVisitor::VisitCXXNewExpr(clang::CXXNewExpr *expr) {
  if (unevaluated_ > 0)
    return true;
  const clang::SourceLocation site = expr->getBeginLoc();
  if (const clang::FunctionDecl *alloc = expr->getOperatorNew())
    addRef(functionTarget(alloc), RefRole::Call, site);
  // Only an initializer that can throw needs the cleanup: the deallocation
  // function, and for an array the destruction of the elements built.
  if (!ctx_->getLangOpts().CXXExceptions ||
      !mayThrow(expr->getInitializer()))
    return true;
  if (const clang::FunctionDecl *dealloc = expr->getOperatorDelete())
    addRef(functionTarget(dealloc), RefRole::Call, site);
  if (expr->isArray())
    noteDestructorOf(expr->getAllocatedType(), site);
  return true;
}

// The base-class constructor an inheriting constructor runs.
bool ReferenceVisitor::VisitCXXInheritedCtorInitExpr(
    clang::CXXInheritedCtorInitExpr *expr) {
  if (unevaluated_ == 0 && expr->getConstructor())
    addRef(functionTarget(expr->getConstructor()), RefRole::Call,
           expr->getLocation());
  return true;
}

// A temporary of a class with a non-trivial destructor is destroyed at the
// end of the full-expression (or of the reference it is bound to).
bool ReferenceVisitor::VisitCXXBindTemporaryExpr(
    clang::CXXBindTemporaryExpr *expr) {
  if (unevaluated_ > 0)
    return true;
  if (const clang::CXXTemporary *temp = expr->getTemporary())
    if (const clang::CXXDestructorDecl *dtor = temp->getDestructor())
      addRef(functionTarget(dtor), RefRole::Call, expr->getBeginLoc());
  return true;
}

bool ReferenceVisitor::VisitCallExpr(clang::CallExpr *expr) {
  noteLockCall(expr);
  // A local holding a lambda, called (l()) or handed to a call
  // (std::invoke(l), an algorithm taking it by reference): the call graph's
  // edge to the lambda starts at this call, so a call reference records
  // the locks held here. A copy, a capture or a return is no call.
  if (unevaluated_ == 0)
    for (const clang::Expr *arg : expr->arguments())
      if (const clang::LambdaExpr *lambda = heldLambda(arg))
        if (auto slot = lambdaSlot_.find(lambda->getCallOperator());
            slot != lambdaSlot_.end())
          addRef(slot->second, RefRole::Call, arg->getExprLoc());
  const clang::Expr *callee = expr->getCallee();
  while (callee) {
    callee = callee->IgnoreParenImpCasts();
    const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(callee);
    if (!unary || (unary->getOpcode() != clang::UO_AddrOf &&
                   unary->getOpcode() != clang::UO_Deref))
      break;
    callee = unary->getSubExpr(); // (&f)(), (*fp)()
  }
  if (!callee)
    return true;
  if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(callee)) {
    if (namesLinkable(ref->getDecl()))
      marks_.try_emplace(ref, kMarkCallee);
  } else if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(callee)) {
    marks_.try_emplace(member, kMarkCallee);
    // g.method(): a const method reads the object, any other may write it.
    const auto *method =
        llvm::dyn_cast<clang::CXXMethodDecl>(member->getMemberDecl());
    if (method && method->isInstance() && !member->isArrow())
      mark(member->getBase(), objectMarks(method));
  } else if (const auto *overloads =
                 llvm::dyn_cast<clang::OverloadExpr>(callee)) {
    marks_.try_emplace(callee, kMarkCallee);
    // g.put(v) with the overload still open: g is read only when every
    // candidate is a const member.
    const auto *member = llvm::dyn_cast<clang::UnresolvedMemberExpr>(callee);
    if (member && !member->isImplicitAccess() && !member->isArrow()) {
      bool allConst = overloads->getNumDecls() != 0;
      for (const clang::NamedDecl *decl : overloads->decls()) {
        const auto *method =
            llvm::dyn_cast<clang::CXXMethodDecl>(decl->getUnderlyingDecl());
        if (!method || !method->isConst() || method->isStatic()) {
          allConst = false;
          break;
        }
      }
      mark(member->getBase(), allConst ? kMarkRead : kMarkWrite);
    }
  }

  const clang::FunctionDecl *fn = expr->getDirectCallee();
  if (!fn) {
    // An operator with a dependent operand: read as the built-in it spells
    // (g += v writes g). Arguments of any other unresolved call escape.
    const auto *op = llvm::dyn_cast<clang::CXXOperatorCallExpr>(expr);
    if (!op)
      return true;
    const clang::OverloadedOperatorKind kind = op->getOperator();
    if (kind == clang::OO_PlusPlus || kind == clang::OO_MinusMinus) {
      if (op->getNumArgs() >= 1) // postfix carries a dummy second argument
        mark(op->getArg(0), kMarkRead | kMarkWrite);
    } else if (op->getNumArgs() == 2 && kind != clang::OO_Subscript &&
               kind != clang::OO_Call) {
      markDependentOperands(op->getArg(0), op->getArg(1),
                            clang::BinaryOperator::getOverloadedOpcode(kind));
    }
    return true;
  }
  markCallArguments(expr, fn);
  return true;
}

void ReferenceVisitor::markCallArguments(const clang::CallExpr *expr,
                                         const clang::FunctionDecl *fn) {
  unsigned first = 0;
  if (llvm::isa<clang::CXXOperatorCallExpr>(expr)) {
    // A member operator's object is argument 0 (g += 1, g[i], g()).
    const auto *method = llvm::dyn_cast<clang::CXXMethodDecl>(fn);
    if (method && method->isInstance() && expr->getNumArgs() > 0) {
      mark(expr->getArg(0), objectMarks(method));
      first = 1;
    }
  }
  for (unsigned i = first, n = expr->getNumArgs(); i < n; ++i)
    markArgument(fn, i - first, expr->getArg(i));
}

bool ReferenceVisitor::VisitCXXConstructExpr(clang::CXXConstructExpr *expr) {
  const clang::CXXConstructorDecl *ctor = expr->getConstructor();
  if (!ctor)
    return true;
  for (unsigned i = 0, n = expr->getNumArgs(); i < n; ++i)
    markArgument(ctor, i, expr->getArg(i));
  if (unevaluated_ > 0)
    return true;
  addRef(functionTarget(ctor), RefRole::Call, expr->getLocation());
  // A constructor the compiler writes stores the vtable pointer inline at
  // the construction site; a user-provided one is its own referrer
  // (noteFunctionDefinition).
  const clang::CXXRecordDecl *cls = ctor->getParent();
  if (!ctor->isUserProvided() && cls->hasDefinition() && cls->isDynamicClass())
    noteVtableUse(cls, expr->getLocation());
  return true;
}

// ---- references ---------------------------------------------------------------

bool ReferenceVisitor::VisitDeclRefExpr(clang::DeclRefExpr *expr) {
  const clang::ValueDecl *decl = expr->getDecl();
  if (const clang::VarDecl *var = variableOf(decl)) {
    noteVariableUse(var, expr, expr->isNonOdrUse(), expr->getLocation());
  }
  else if (const auto *fn = llvm::dyn_cast<clang::FunctionDecl>(decl))
    noteFunctionUse(fn, expr, expr->isNonOdrUse(), expr->getLocation());
  return true;
}

bool ReferenceVisitor::VisitMemberExpr(clang::MemberExpr *expr) {
  const clang::ValueDecl *decl = expr->getMemberDecl();
  const auto *var = llvm::dyn_cast<clang::VarDecl>(decl);
  const auto *fn = llvm::dyn_cast<clang::FunctionDecl>(decl);
  if (var)
    noteVariableUse(var, expr, expr->isNonOdrUse(), expr->getMemberLoc());
  else if (fn)
    noteFunctionUse(fn, expr, expr->isNonOdrUse(), expr->getMemberLoc());
  // obj.static_member / obj.static_fn(): obj is named, not accessed.
  const auto *method = llvm::dyn_cast_or_null<clang::CXXMethodDecl>(fn);
  if ((var || (method && method->isStatic())) && !expr->isArrow())
    mark(expr->getBase(), kMarkRead);
  return true;
}

bool ReferenceVisitor::VisitOverloadExpr(clang::OverloadExpr *expr) {
  // An instantiation adds what it resolves; a set still open in it (a
  // generic lambda's body) is the pattern's, already read.
  if (unevaluated_ > 0 || mode_ != Mode::AsWritten)
    return true;
  const unsigned candidates = expr->getNumDecls();
  if (candidates == 0 || candidates > kMaxOverloadCandidates)
    return true;
  const RefRole role = (marksOf(expr) & kMarkCallee) != 0 ? RefRole::Call
                                                          : RefRole::AddressOf;
  for (const clang::NamedDecl *decl : expr->decls()) {
    decl = decl->getUnderlyingDecl();
    if (const auto *tmpl = llvm::dyn_cast<clang::FunctionTemplateDecl>(decl))
      decl = tmpl->getTemplatedDecl();
    if (const auto *fn = llvm::dyn_cast<clang::FunctionDecl>(decl))
      addRef(targetSlot(fn), role, expr->getNameLoc());
  }
  return true;
}

void ReferenceVisitor::noteVariableUse(const clang::VarDecl *var,
                                       const clang::Expr *expr,
                                       clang::NonOdrUseReason nour,
                                       clang::SourceLocation loc) {
  if (!var->hasGlobalStorage())
    return;
  if (unevaluated_ > 0 || nour == clang::NOUR_Unevaluated ||
      nour == clang::NOUR_Discarded)
    return;
  // A function-local static of an instantiation has no identity of its own
  // in the code as written (the pattern's static is the one recorded).
  if (mode_ != Mode::AsWritten && var->isStaticLocal())
    return;
  const uint32_t target = targetSlot(var);
  if (target == kDropped)
    return;
  scheduleVariable(var, target);
  const uint8_t marks = marksOf(expr);
  bool accessed = false;
  // Calling through a function pointer or functor reads it.
  if ((marks & (kMarkRead | kMarkCallee)) != 0) {
    addRef(target, RefRole::Read, loc);
    accessed = true;
  }
  if ((marks & kMarkWrite) != 0) {
    addRef(target, RefRole::Write, loc,
           (marks & kMarkWeakWrite) != 0 ? 0 : kRefMutates);
    accessed = true;
  }
  if (accessed)
    return;
  // No parent claimed the access: the variable's identity escapes (address
  // taken, array decay, bound to a non-const reference, or an unresolved
  // use in a template body). A constant-expression use reads.
  const bool reads =
      nour == clang::NOUR_Constant && (marks & kMarkAddress) == 0;
  addRef(target, reads ? RefRole::Read : RefRole::AddressOf, loc);
}

void ReferenceVisitor::noteFunctionUse(const clang::FunctionDecl *fn,
                                       const clang::Expr *expr,
                                       clang::NonOdrUseReason nour,
                                       clang::SourceLocation loc) {
  if (unevaluated_ > 0 || nour == clang::NOUR_Unevaluated)
    return;
  const uint32_t target = functionTarget(fn);
  if (target == kDropped)
    return;
  const auto once = onceCallables_.find(expr);
  if (once != onceCallables_.end())
    acquire(once->second);
  addRef(target,
         (marksOf(expr) & kMarkCallee) != 0 ? RefRole::Call
                                            : RefRole::AddressOf,
         loc);
  if (once != onceCallables_.end())
    release(once->second);
}

// The class whose typeinfo an RTTI operation on `type` names: the type
// itself, or what a reference or one pointer level points at.
void ReferenceVisitor::noteTypeinfoUse(clang::QualType type,
                                       clang::SourceLocation loc) {
  if (type.isNull())
    return;
  type = type.getNonReferenceType();
  if (const auto *pointer = type->getAs<clang::PointerType>())
    type = pointer->getPointeeType();
  if (const clang::CXXRecordDecl *cls = type->getAsCXXRecordDecl())
    addRef(targetSlot(cls), RefRole::TypeinfoUse, loc);
}

bool ReferenceVisitor::VisitCXXDynamicCastExpr(
    clang::CXXDynamicCastExpr *expr) {
  if (unevaluated_ > 0)
    return true;
  // An upcast written as dynamic_cast compiles to a static conversion.
  if (expr->getCastKind() != clang::CK_Dynamic &&
      expr->getCastKind() != clang::CK_Dependent)
    return true;
  noteTypeinfoUse(expr->getTypeAsWritten(), expr->getOperatorLoc());
  noteTypeinfoUse(expr->getSubExpr()->getType(), expr->getOperatorLoc());
  return true;
}

bool ReferenceVisitor::VisitCXXThrowExpr(clang::CXXThrowExpr *expr) {
  if (unevaluated_ > 0)
    return true;
  if (const clang::Expr *thrown = expr->getSubExpr()) {
    noteTypeinfoUse(thrown->getType(), expr->getThrowLoc());
    // The runtime destroys the exception object.
    if (!thrown->isTypeDependent())
      noteDestructorOf(thrown->getType(), expr->getThrowLoc());
  }
  return true;
}

bool ReferenceVisitor::VisitCXXCatchStmt(clang::CXXCatchStmt *stmt) {
  noteTypeinfoUse(stmt->getCaughtType(), stmt->getCatchLoc());
  return true;
}

void ReferenceVisitor::addRef(uint32_t target, RefRole role,
                              clang::SourceLocation site, uint8_t flags) {
  if (target == kDropped)
    return;
  const uint32_t referrer = referrerSlot(site);
  // Lock facts are kept for accesses and calls; a vtable or typeinfo use
  // is not something a lock protects.
  const bool tracked = role != RefRole::VtableUse &&
                       role != RefRole::TypeinfoUse;
  if (lockState_.loopDepth > 0)
    flags |= kRefInLoop;
  addRefFrom(referrer, target, role, site, tracked ? currentLockset() : 0,
             flags);
}

void ReferenceVisitor::addRefFrom(uint32_t referrer, uint32_t target,
                                  RefRole role, clang::SourceLocation site,
                                  uint32_t lockset, uint8_t flags) {
  if (referrer == kDropped || target == kDropped)
    return;
  const uint64_t key = (static_cast<uint64_t>(referrer) << kReferrerShift) |
                       (static_cast<uint64_t>(target) << kTargetShift) |
                       static_cast<uint64_t>(role);
  RefAcc &acc = (mode_ == Mode::Instantiation ? instAcc_ : refs_)[key];
  if (acc.count++ == 0)
    acc.site = site;
  acc.flags |= flags;
  // A transparent function's references take the lock sets of the uses
  // they are spliced into; an instantiation's add nothing to a reference
  // the code as written holds (lower keeps that one).
  if (mode_ == Mode::Transparent ||
      (mode_ == Mode::Instantiation && refs_.count(key) != 0))
    return;
  const uint32_t seq = seq_++;
  if (lockset == 0) {
    if (!acc.unlocked) {
      acc.unlocked = true;
      acc.unlockedSite = site;
      acc.unlockedSeq = seq;
    }
    return;
  }
  acc.locked = true;
  lockedOcc_.try_emplace({key, lockset}, LockedOcc{seq, site});
}

// ============================================================================
// Locks
// ============================================================================

void ReferenceVisitor::enterBody(LockState &saved) {
  saved = std::move(lockState_);
  lockState_ = LockState();
  heldSetValid_ = false;
}

void ReferenceVisitor::leaveBody(LockState &saved) {
  lockState_ = std::move(saved);
  heldSetValid_ = false;
}

void ReferenceVisitor::acquire(uint32_t lock) {
  lockState_.held.push_back({lock, nullptr, true, false, nullptr});
  heldSetValid_ = false;
}

// Drop the innermost lock taken by hand on this mutex. A guard keeps its
// mutex: unlocking a guarded mutex by hand is a bug this does not model.
void ReferenceVisitor::release(uint32_t lock) {
  auto &held = lockState_.held;
  for (size_t i = held.size(); i-- > 0;) {
    if (held[i].lock == lock && !held[i].guard && !held[i].met) {
      held.erase(held.begin() + static_cast<std::ptrdiff_t>(i));
      heldSetValid_ = false;
      return;
    }
  }
  // Not held: the unlock pairs with a lock a meet made unknown.
  for (size_t i = held.size(); i-- > 0;) {
    if (held[i].met) {
      held.erase(held.begin() + static_cast<std::ptrdiff_t>(i));
      heldSetValid_ = false;
      return;
    }
  }
}

void ReferenceVisitor::noteAcquire(uint32_t lock, RefRole role,
                                   clang::SourceLocation site,
                                   uint32_t heldBefore) {
  // A transparent body is library code or code the compiler writes: its
  // locks are the library's own (std::lock_guard's constructor locks the
  // mutex it was given, which the guard's declaration already recorded).
  if (mode_ == Mode::Transparent || unevaluated_ > 0 || frames_.empty())
    return;
  if (llvm::StringRef(lockIdents_[lock].usr).starts_with(kLockOncePrefix))
    return;
  const clang::SourceLocation at = sm_.getExpansionLoc(site);
  if (at.isInvalid() || sm_.isInSystemHeader(at))
    return;
  const uint32_t referrer = referrerSlot(site);
  if (referrer == kDropped)
    return;
  const uint64_t key = (static_cast<uint64_t>(referrer) << kReferrerShift) |
                       (static_cast<uint64_t>(lock) << kTargetShift) |
                       static_cast<uint64_t>(role);
  // Instantiation bodies are read after the code as written: an
  // acquisition the pattern holds keeps the pattern's facts.
  if (mode_ == Mode::Instantiation && acqs_.count(key) != 0)
    return;
  RefAcc &acc = (mode_ == Mode::Instantiation ? instAcqs_ : acqs_)[key];
  if (acc.count++ == 0)
    acc.site = site;
  acc.flags |= lockFlags_[lock];
  if (lockState_.loopDepth > 0)
    acc.flags |= kRefInLoop;
  const uint32_t seq = seq_++;
  if (heldBefore == 0) {
    if (!acc.unlocked) {
      acc.unlocked = true;
      acc.unlockedSite = site;
      acc.unlockedSeq = seq;
    }
    return;
  }
  acc.locked = true;
  acqOcc_.try_emplace({key, heldBefore}, LockedOcc{seq, site});
}

uint32_t ReferenceVisitor::internLockset(
    llvm::SmallVectorImpl<uint32_t> &locks) {
  if (locks.empty())
    return 0;
  llvm::sort(locks);
  locks.erase(std::unique(locks.begin(), locks.end()), locks.end());
  std::vector<uint32_t> key(locks.begin(), locks.end());
  auto [it, inserted] = locksetOf_.try_emplace(
      std::move(key), static_cast<uint32_t>(locksets_.size()));
  if (inserted)
    locksets_.push_back(it->first);
  return it->second;
}

uint32_t ReferenceVisitor::currentLockset() {
  if (heldSetValid_)
    return heldSet_;
  heldSetValid_ = true;
  llvm::SmallVector<uint32_t, 4> locks;
  for (const HeldLock &h : lockState_.held)
    if (h.active)
      locks.push_back(h.lock);
  heldSet_ = internLockset(locks);
  return heldSet_;
}

uint32_t ReferenceVisitor::currentLocksetWithoutOnce() {
  llvm::SmallVector<uint32_t, 4> locks;
  for (const HeldLock &h : lockState_.held)
    if (h.active && !llvm::StringRef(lockIdents_[h.lock].usr)
                         .starts_with(kLockOncePrefix))
      locks.push_back(h.lock);
  return internLockset(locks);
}

uint32_t ReferenceVisitor::lockIdentity(const clang::NamedDecl *decl,
                                        llvm::StringRef prefix) {
  const Ident &ident = usrs_.identFor(decl);
  std::string usr = (prefix + ident.usr).str();
  auto [it, inserted] = lockIdentOf_.try_emplace(
      usr, static_cast<uint32_t>(lockIdents_.size()));
  if (inserted) {
    std::string display = ident.display;
    if (const auto *var = llvm::dyn_cast<clang::VarDecl>(decl);
        var && var->isStaticLocal()) {
      if (const auto *fn =
              llvm::dyn_cast<clang::FunctionDecl>(var->getDeclContext()))
        display = fn->getQualifiedNameAsString() + "::" + display;
    }
    if (!prefix.empty())
      display = (llvm::Twine("once(") + display + ")").str();
    lockIdents_.push_back({std::move(usr), std::move(display)});
    uint8_t flags = 0;
    if (prefix.empty())
      if (const auto *value = llvm::dyn_cast<clang::ValueDecl>(decl))
        flags = lockRecursionFlags(value->getType());
    lockFlags_.push_back(flags);
  }
  return it->second;
}

uint32_t ReferenceVisitor::unknownLock() {
  if (unknownLock_ == UINT32_MAX) {
    unknownLock_ = static_cast<uint32_t>(lockIdents_.size());
    lockIdents_.push_back({kLockUnknownIdentity, "<unknown mutex>"});
    lockFlags_.push_back(0);
    lockIdentOf_[kLockUnknownIdentity] = unknownLock_;
  }
  return unknownLock_;
}

uint32_t ReferenceVisitor::onceLock(const clang::NamedDecl *decl) {
  return decl ? lockIdentity(decl, kLockOncePrefix) : unknownLock();
}

// The mutex an expression names: a global, static member or function-local
// static by its USR, a data member by the member's USR (every object's
// mutex alike), anything else (through a pointer, a reference, a call, an
// array element) unknown.
uint32_t ReferenceVisitor::mutexIdentity(const clang::Expr *expr) {
  if (!expr)
    return unknownLock();
  expr = expr->IgnoreParenImpCasts();
  if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(expr))
    if (unary->getOpcode() == clang::UO_AddrOf)
      expr = unary->getSubExpr()->IgnoreParenImpCasts();
  // A variable or member that is itself a pointer or reference names
  // whatever it points at: unknown.
  const auto names = [](const clang::ValueDecl *decl) {
    const clang::QualType type = decl->getType();
    return !type->isPointerType() && !type->isReferenceType();
  };
  if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(expr)) {
    if (const auto *var = llvm::dyn_cast<clang::VarDecl>(ref->getDecl()))
      if (var->hasGlobalStorage() && names(var))
        return lockIdentity(var->getCanonicalDecl(), "");
    return unknownLock();
  }
  if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(expr)) {
    const clang::ValueDecl *decl = member->getMemberDecl();
    if (!names(decl))
      return unknownLock();
    if (const auto *field = llvm::dyn_cast<clang::FieldDecl>(decl))
      return lockIdentity(field, "");
    if (const auto *var = llvm::dyn_cast<clang::VarDecl>(decl))
      if (var->hasGlobalStorage())
        return lockIdentity(var->getCanonicalDecl(), "");
  }
  return unknownLock();
}

namespace {

// The constructor call that initializes a local, through the wrappers a
// copy-initialization or a functional cast leaves (`auto g =
// std::lock_guard(m);`); null for a call or anything else.
[[nodiscard]] const clang::CXXConstructExpr *
constructionOf(const clang::Expr *init) {
  while (init) {
    if (const auto *construct = llvm::dyn_cast<clang::CXXConstructExpr>(init)) {
      if (construct->isElidable() && construct->getNumArgs() == 1) {
        init = construct->getArg(0);
        continue;
      }
      return construct;
    }
    if (const auto *full = llvm::dyn_cast<clang::FullExpr>(init))
      init = full->getSubExpr();
    else if (const auto *temp =
                 llvm::dyn_cast<clang::MaterializeTemporaryExpr>(init))
      init = temp->getSubExpr();
    else if (const auto *bind = llvm::dyn_cast<clang::CXXBindTemporaryExpr>(init))
      init = bind->getSubExpr();
    else if (const auto *cast = llvm::dyn_cast<clang::CastExpr>(init))
      init = cast->getSubExpr();
    else if (const auto *paren = llvm::dyn_cast<clang::ParenExpr>(init))
      init = paren->getSubExpr();
    else
      return nullptr;
  }
  return nullptr;
}

enum class LockOp : uint8_t { None, Acquire, Release, Disown };

// What a member function does to the lock its object holds or is: by the
// thread-safety attributes when the class carries them, else by the
// standard and Abseil spellings.
[[nodiscard]] LockOp lockOpOf(const clang::FunctionDecl *fn) {
  if (fn->hasAttrs()) {
    if (fn->hasAttr<clang::AcquireCapabilityAttr>() ||
        fn->hasAttr<clang::TryAcquireCapabilityAttr>())
      return LockOp::Acquire;
    if (fn->hasAttr<clang::ReleaseCapabilityAttr>())
      return LockOp::Release;
  }
  const clang::IdentifierInfo *id = fn->getIdentifier();
  if (!id)
    return LockOp::None;
  const llvm::StringRef name = id->getName();
  if (name == "lock" || name == "lock_shared" || name == "try_lock" ||
      name == "try_lock_shared" || name == "try_lock_for" ||
      name == "try_lock_until" || name == "try_lock_shared_for" ||
      name == "try_lock_shared_until" || name == "Lock" ||
      name == "ReaderLock" || name == "WriterLock" || name == "TryLock" ||
      name == "ReaderTryLock" || name == "WriterTryLock")
    return LockOp::Acquire;
  if (name == "unlock" || name == "unlock_shared" || name == "Unlock" ||
      name == "ReaderUnlock" || name == "WriterUnlock")
    return LockOp::Release;
  if (name == "release")
    return LockOp::Disown;
  return LockOp::None;
}

// How a member function that acquires (lockOpOf) does it: a try or timed
// lock gives up instead of waiting forever, and a shared acquisition
// admits other shared holders. By the thread-safety attributes when
// present, else by the standard and Abseil spellings.
struct AcquireKind {
  bool tryLock = false;
  bool shared = false;
};
[[nodiscard]] AcquireKind acquireKindOf(const clang::FunctionDecl *fn) {
  AcquireKind out;
  if (fn->hasAttrs()) {
    if (const auto *attr = fn->getAttr<clang::TryAcquireCapabilityAttr>()) {
      out.tryLock = true;
      out.shared = attr->isShared();
      return out;
    }
    if (const auto *attr = fn->getAttr<clang::AcquireCapabilityAttr>()) {
      out.shared = attr->isShared();
      return out;
    }
  }
  const clang::IdentifierInfo *id = fn->getIdentifier();
  if (!id)
    return out;
  const llvm::StringRef name = id->getName();
  out.tryLock = name.starts_with("try_") || name.contains("TryLock");
  out.shared = name.contains("shared") || name.starts_with("Reader");
  return out;
}

[[nodiscard]] const clang::LambdaExpr *lambdaArgument(const clang::Expr *arg) {
  return arg ? llvm::dyn_cast<clang::LambdaExpr>(arg->IgnoreImplicit())
             : nullptr;
}

// The lambda a local was initialized from, through the wrappers the call
// graph looks through (std::function, a cast, a temporary): the call
// graph's varLambdaSources_.
[[nodiscard]] const clang::LambdaExpr *initLambda(const clang::Expr *init) {
  const clang::Expr *cur = init ? init->IgnoreParenImpCasts() : nullptr;
  while (cur) {
    if (const auto *lambda = llvm::dyn_cast<clang::LambdaExpr>(cur))
      return lambda;
    if (const auto *temp = llvm::dyn_cast<clang::MaterializeTemporaryExpr>(cur))
      cur = temp->getSubExpr();
    else if (const auto *bind = llvm::dyn_cast<clang::CXXBindTemporaryExpr>(cur))
      cur = bind->getSubExpr();
    else if (const auto *cast = llvm::dyn_cast<clang::CastExpr>(cur))
      cur = cast->getSubExpr();
    else if (const auto *construct = llvm::dyn_cast<clang::CXXConstructExpr>(cur);
             construct && construct->getNumArgs() >= 1)
      cur = construct->getArg(0);
    else
      return nullptr;
  }
  return nullptr;
}

// A call argument naming a local that holds a lambda (`l`, `&l`), as the
// call graph's processCallableArgs matches it for an edge to the lambda.
[[nodiscard]] const clang::LambdaExpr *heldLambda(const clang::Expr *arg) {
  if (!arg)
    return nullptr;
  const clang::Expr *e = arg->IgnoreParenImpCasts();
  if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(e);
      unary && unary->getOpcode() == clang::UO_AddrOf)
    e = unary->getSubExpr()->IgnoreParenImpCasts();
  const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(e);
  const auto *var = ref ? llvm::dyn_cast<clang::VarDecl>(ref->getDecl()) : nullptr;
  if (!var || !var->isLocalVarDecl())
    return nullptr;
  return initLambda(var->getInit());
}

} // namespace

// A local that guards a mutex from here to the end of its block.
void ReferenceVisitor::noteGuard(const clang::VarDecl *var) {
  if (unevaluated_ > 0 || frames_.empty())
    return;
  const clang::QualType type = var->getType();
  if (type.isNull() || type->isReferenceType() || type->isDependentType())
    return;
  const clang::CXXRecordDecl *cls = type->getAsCXXRecordDecl();
  if (!cls)
    return;
  static const LockTypeConfig kBuiltins;
  const bool known = isGuardType(type, lockCfg_ ? *lockCfg_ : kBuiltins);
  if (!known && (inStdNamespace(cls->getDeclContext()) ||
                 !isHeuristicGuard(cls) || isMutexLike(type)))
    return;
  const clang::CXXConstructExpr *construct = constructionOf(var->getInit());
  if (!construct || construct->getConstructor()->isCopyOrMoveConstructor()) {
    // A guard handed over from elsewhere (a factory, a moved lock): it may
    // hold any mutex.
    if (known && var->getInit()) {
      lockState_.held.push_back({unknownLock(), var, true, false, nullptr});
      heldSetValid_ = false;
    }
    return;
  }
  bool active = true;
  bool waits = true; // the construction blocks until it owns the mutexes
  bool sawArgument = false;
  llvm::SmallVector<uint32_t, 2> mutexes;
  for (const clang::Expr *arg : construct->arguments()) {
    if (llvm::isa<clang::CXXDefaultArgExpr>(arg))
      continue;
    switch (lockTagOf(arg->getType())) {
    case LockTag::Defer:
      active = false;
      continue;
    case LockTag::TryToLock: // read as taken: a race needs it not to be
    case LockTag::Adopt:
      waits = false;
      continue;
    case LockTag::None:
      break;
    }
    sawArgument = true;
    if (isMutexLike(arg->getType()))
      mutexes.push_back(mutexIdentity(arg));
  }
  // A lock type constructed from something the bake does not read as a
  // mutex still holds one: unknown, never no lock. (Constructed from
  // nothing, it owns nothing.)
  if (known && mutexes.empty() && sawArgument)
    mutexes.push_back(unknownLock());
  if (active && waits && !mutexes.empty()) {
    // Every mutex of a multi-lock guard (std::scoped_lock) is acquired
    // under the locks held before it: none orders the others.
    const uint32_t before = currentLockset();
    const clang::CXXConstructorDecl *ctor = construct->getConstructor();
    const auto *attr = ctor->getAttr<clang::AcquireCapabilityAttr>();
    const bool shared = attr ? attr->isShared() : isSharedGuardType(type);
    for (uint32_t lock : mutexes)
      noteAcquire(lock, shared ? RefRole::LockShared : RefRole::Lock,
                  var->getLocation(), before);
  }
  for (uint32_t lock : mutexes)
    lockState_.held.push_back({lock, var, active, false, nullptr});
  if (!mutexes.empty())
    heldSetValid_ = false;
}

void ReferenceVisitor::noteLockCall(const clang::CallExpr *expr) {
  if (unevaluated_ > 0 || frames_.empty())
    return;
  const clang::FunctionDecl *fn = expr->getDirectCallee();
  if (!fn)
    return;
  if (const auto *call = llvm::dyn_cast<clang::CXXMemberCallExpr>(expr)) {
    const LockOp op = lockOpOf(fn);
    if (op == LockOp::None)
      return;
    const clang::Expr *object = call->getImplicitObjectArgument();
    if (!object)
      return;
    object = object->IgnoreParenImpCasts();
    // A guard of this block: lock()/unlock() toggle what it owns, and
    // release() hands its mutex over still locked (a later unlock() by
    // hand drops it).
    const AcquireKind kind = acquireKindOf(fn);
    if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(object)) {
      bool guard = false;
      auto &held = lockState_.held;
      const uint32_t before = currentLockset();
      for (size_t i = held.size(); i-- > 0;) {
        if (held[i].guard != ref->getDecl())
          continue;
        guard = true;
        if (op == LockOp::Acquire && !held[i].active && !kind.tryLock)
          noteAcquire(held[i].lock,
                      kind.shared || isSharedGuardType(held[i].guard->getType())
                          ? RefRole::LockShared
                          : RefRole::Lock,
                      expr->getExprLoc(), before);
        if (op != LockOp::Disown)
          held[i].active = op == LockOp::Acquire;
        else if (held[i].active)
          held[i].guard = nullptr;
        else
          held.erase(held.begin() + static_cast<std::ptrdiff_t>(i));
      }
      if (guard) {
        heldSetValid_ = false;
        return;
      }
    }
    if (!isMutexLike(object->getType()))
      return;
    if (op == LockOp::Acquire) {
      const uint32_t lock = mutexIdentity(object);
      if (!kind.tryLock)
        noteAcquire(lock, kind.shared ? RefRole::LockShared : RefRole::Lock,
                    expr->getExprLoc(), currentLockset());
      acquire(lock);
    } else if (op == LockOp::Release)
      release(mutexIdentity(object));
    return;
  }
  const clang::IdentifierInfo *id = fn->getIdentifier();
  if (!id)
    return;
  const llvm::StringRef name = id->getName();
  if (fn->isInStdNamespace()) {
    if (name == "call_once" && expr->getNumArgs() >= 2) {
      // The callable runs once, and every other call_once on the flag
      // waits for it: its body holds the flag's once lock.
      const clang::Expr *flag = expr->getArg(0)->IgnoreParenImpCasts();
      const clang::NamedDecl *flagDecl = nullptr;
      if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(flag))
        flagDecl = ref->getDecl();
      else if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(flag))
        flagDecl = member->getMemberDecl();
      const clang::Expr *callable = expr->getArg(1)->IgnoreParenImpCasts();
      if (const clang::LambdaExpr *lambda = lambdaArgument(expr->getArg(1))) {
        onceLambdas_[lambda] = onceLock(flagDecl);
      } else {
        // A function named there: its address is taken under the once
        // lock, which the call graph's edge to it reads.
        if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(callable))
          if (unary->getOpcode() == clang::UO_AddrOf)
            callable = unary->getSubExpr()->IgnoreParenImpCasts();
        if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(callable))
          if (llvm::isa<clang::FunctionDecl>(ref->getDecl()))
            onceCallables_[ref] = onceLock(flagDecl);
      }
    } else if (name == "lock" || name == "try_lock") {
      // std::lock(a, b): mutexes, or guards made with defer_lock. Every
      // one is acquired under the locks held before the call (std::lock
      // avoids deadlock among them); std::try_lock waits for none.
      const bool waits = name == "lock";
      const uint32_t before = currentLockset();
      for (const clang::Expr *arg : expr->arguments()) {
        const clang::Expr *bare = arg->IgnoreParenImpCasts();
        bool guard = false;
        if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(bare))
          for (HeldLock &h : lockState_.held)
            if (h.guard == ref->getDecl()) {
              if (waits && !h.active)
                noteAcquire(h.lock,
                            isSharedGuardType(h.guard->getType())
                                ? RefRole::LockShared
                                : RefRole::Lock,
                            arg->getExprLoc(), before);
              h.active = true;
              guard = true;
            }
        if (guard) {
          heldSetValid_ = false;
        } else if (isMutexLike(arg->getType())) {
          const uint32_t lock = mutexIdentity(arg);
          if (waits)
            noteAcquire(lock, RefRole::Lock, arg->getExprLoc(), before);
          acquire(lock);
        }
      }
    }
    return;
  }
  if (expr->getNumArgs() == 0 || !name.starts_with("pthread_"))
    return;
  if (name == "pthread_mutex_lock" || name == "pthread_mutex_trylock" ||
      name == "pthread_mutex_timedlock" || name == "pthread_rwlock_rdlock" ||
      name == "pthread_rwlock_wrlock" || name == "pthread_rwlock_tryrdlock" ||
      name == "pthread_rwlock_trywrlock" || name == "pthread_spin_lock" ||
      name == "pthread_spin_trylock") {
    const uint32_t lock = mutexIdentity(expr->getArg(0));
    // A try or timed lock gives up instead of waiting forever.
    if (name == "pthread_mutex_lock" || name == "pthread_spin_lock" ||
        name == "pthread_rwlock_wrlock")
      noteAcquire(lock, RefRole::Lock, expr->getExprLoc(), currentLockset());
    else if (name == "pthread_rwlock_rdlock")
      noteAcquire(lock, RefRole::LockShared, expr->getExprLoc(),
                  currentLockset());
    acquire(lock);
  }
  else if (name == "pthread_mutex_unlock" || name == "pthread_rwlock_unlock" ||
           name == "pthread_spin_unlock")
    release(mutexIdentity(expr->getArg(0)));
}

// The lock text of one reference (ReferenceIndex.h): its lock sets in
// first-occurrence order. "" when every occurrence was made under none.
std::string ReferenceVisitor::lockText(
    const RefAcc &acc, llvm::ArrayRef<std::pair<uint32_t, LockedOcc>> locked) {
  if (!acc.locked)
    return {};
  struct Entry {
    uint32_t seq;
    uint32_t lockset;
    clang::SourceLocation site;
  };
  llvm::SmallVector<Entry, 4> entries;
  if (acc.unlocked)
    entries.push_back({acc.unlockedSeq, 0, acc.unlockedSite});
  for (const auto &[lockset, occ] : locked)
    entries.push_back({occ.seq, lockset, occ.site});
  llvm::sort(entries, [](const Entry &a, const Entry &b) {
    return a.seq < b.seq;
  });
  std::string text;
  llvm::SmallVector<const Ident *, 4> locks;
  for (const Entry &e : entries) {
    if (!text.empty())
      text += '\n';
    const Site site = siteOf(e.site);
    text += std::to_string(site.line);
    text += ':';
    text += std::to_string(site.col);
    locks.clear();
    for (uint32_t lock : locksets_[e.lockset])
      locks.push_back(&lockIdents_[lock]);
    llvm::sort(locks, [](const Ident *a, const Ident *b) {
      return a->usr < b->usr;
    });
    for (const Ident *lock : locks) {
      text += '\t';
      text += lock->usr;
      text += '\t';
      text += lock->display;
    }
  }
  return text;
}

// ============================================================================
// Implicit code
// ============================================================================

uint32_t ReferenceVisitor::functionTarget(const clang::FunctionDecl *fn) {
  auto found = funcSlot_.find(fn);
  if (found != funcSlot_.end())
    return found->second;
  uint32_t slot = kDropped;
  const auto *ctor = llvm::dyn_cast<clang::CXXConstructorDecl>(fn);
  const auto *method = llvm::dyn_cast<clang::CXXMethodDecl>(fn);
  if (ctor && ctor->isImplicit() && ctor->isInheritingConstructor()) {
    // `using Base::Base;`: the derived constructor builds the other bases
    // and the members, then runs the base constructor. Not defined (only
    // named): the base constructor.
    slot = transparentSlot(fn);
    if (slot == kDropped)
      slot = functionTarget(ctor->getInheritedConstructor().getConstructor());
  } else if (method && method->getParent()->isLambda()) {
    // A copy of a lambda the code as written has (an instantiation of a
    // generic lambda, a lambda in an instantiation's body): read under that
    // lambda. A call to a lambda is no reference of its own.
    const uint32_t lambda = lambdaPatternSlot(fn);
    if (lambda != kDropped)
      scheduleLambda(fn, lambda);
    else
      slot = transparentSlot(fn);
  } else {
    slot = targetSlot(fn);
    if (slot != kDropped)
      scheduleInstantiation(fn, slot);
    else
      slot = transparentSlot(fn);
  }
  funcSlot_[fn] = slot;
  return slot;
}

// The slot of the lambda as written that `op` (a closure's call operator) is
// a copy of, or kDropped. Follows the instantiation chain: a specialization
// of a generic call operator, the call operator of a closure in an
// instantiated body, a member template instantiated from one.
uint32_t
ReferenceVisitor::lambdaPatternSlot(const clang::FunctionDecl *op) const {
  const clang::FunctionDecl *step = op;
  for (unsigned depth = 0; step && depth < 8; ++depth) {
    auto found = lambdaSlot_.find(step);
    if (found != lambdaSlot_.end())
      return step == op ? kDropped : found->second; // as written: read already
    const clang::FunctionDecl *next = nullptr;
    if (const clang::FunctionTemplateDecl *primary = step->getPrimaryTemplate())
      next = primary->getTemplatedDecl();
    else if (const clang::FunctionDecl *member =
                 step->getInstantiatedFromMemberFunction())
      next = member;
    else if (const clang::FunctionTemplateDecl *described =
                 step->getDescribedFunctionTemplate())
      if (const clang::FunctionTemplateDecl *from =
              described->getInstantiatedFromMemberTemplate())
        next = from->getTemplatedDecl();
    step = next;
  }
  return kDropped;
}

void ReferenceVisitor::scheduleLambda(const clang::FunctionDecl *op,
                                      uint32_t slot) {
  const clang::FunctionDecl *def = readableDefinition(op);
  if (!def || !scheduled_.insert(def).second)
    return;
  work_.push_back({def, slot, Mode::Instantiation});
}

// The initializer of a variable template's or a class template's static
// data member's instantiation, read under the pattern's slot.
void ReferenceVisitor::scheduleVariable(const clang::VarDecl *var,
                                        uint32_t slot) {
  if (!var->getTemplateInstantiationPattern())
    return;
  const clang::VarDecl *def = var->getDefinition();
  if (!def || !def->hasInit() || def->isInvalidDecl() ||
      def->getDeclContext()->isDependentContext() || !scheduled_.insert(def).second)
    return;
  work_.push_back({def, slot, Mode::Instantiation});
}

// An instantiation of a recorded template: its body is read once, under the
// pattern's slot.
void ReferenceVisitor::scheduleInstantiation(const clang::FunctionDecl *fn,
                                             uint32_t slot) {
  if (!fn->getTemplateInstantiationPattern())
    return;
  const clang::FunctionDecl *def = readableDefinition(fn);
  if (!def || !scheduled_.insert(def).second)
    return;
  work_.push_back({def, slot, Mode::Instantiation});
}

// A function that is no symbol of its own but runs code that may reference
// symbols: an implicit special member, an instantiation of a template whose
// pattern is not recorded (a system header's, a generic lambda's call
// operator). Anything else dropped stays dropped: a system header's own
// non-template code references nothing of the project.
uint32_t ReferenceVisitor::transparentSlot(const clang::FunctionDecl *fn) {
  if (llvm::isa<clang::CXXDeductionGuideDecl>(fn))
    return kDropped;
  const bool special = isCompilerWrittenSpecialMember(fn);
  if (!special && !fn->getTemplateInstantiationPattern())
    return kDropped;
  const clang::FunctionDecl *def = readableDefinition(fn);
  // A destructor destroys members and bases whether or not Sema defined it.
  const auto *dtor = llvm::dyn_cast<clang::CXXDestructorDecl>(fn);
  if (!def && !dtor)
    return kDropped;
  const clang::FunctionDecl *key = def ? def : fn->getCanonicalDecl();
  auto found = transparentSlot_.find(key);
  if (found != transparentSlot_.end())
    return found->second;
  const auto slot = static_cast<uint32_t>(entities_.size());
  Entity entity;
  entity.decl = key;
  entity.kind = EntityKind::Transparent;
  entity.aux = transparentCount_++;
  entities_.push_back(entity);
  transparentSlot_[key] = slot;
  work_.push_back({key, slot, Mode::Transparent});
  return slot;
}

void ReferenceVisitor::drainWork() {
  // Each body is read once; reading one may schedule others. LIFO keeps the
  // order a function of the TU alone.
  while (!work_.empty()) {
    const Work work = work_.back();
    work_.pop_back();
    traverseWork(work);
  }
}

void ReferenceVisitor::traverseWork(const Work &work) {
  assert(frames_.empty() && unevaluated_ == 0);
  LockState outer;
  enterBody(outer);
  mode_ = work.mode;
  if (mode_ == Mode::Instantiation)
    instAcc_.clear();
  if (const auto *var = llvm::dyn_cast<clang::VarDecl>(work.decl)) {
    frames_.push_back({var, work.slot, FrameKind::Variable, false});
    TraverseStmt(const_cast<clang::Expr *>(var->getInit()));
    noteDestructorOf(var->getType(), var->getLocation());
  } else {
    traverseWorkFunction(llvm::cast<clang::FunctionDecl>(work.decl),
                         work.slot);
  }
  frames_.pop_back();
  marks_.clear();
  if (mode_ == Mode::Instantiation) {
    // The largest count any one instantiation has, at its first site.
    // Its lock facts are those of every instantiation: the occurrences
    // under a lock are already in lockedOcc_; the first one under none
    // and the flags are carried over.
    for (const auto &entry : instAcc_) {
      auto [it, inserted] = instRefs_.try_emplace(entry.first, entry.second);
      if (!inserted) {
        const RefAcc before = it->second;
        if (entry.second.count > before.count)
          it->second = entry.second;
        mergeLockFacts(it->second, before);
        mergeLockFacts(it->second, entry.second);
      }
    }
  }
  mode_ = Mode::AsWritten;
  leaveBody(outer);
}

// Fold another accumulation's lock facts (not its count or site) into acc.
void ReferenceVisitor::mergeLockFacts(RefAcc &acc, const RefAcc &other) {
  acc.flags |= other.flags;
  acc.locked |= other.locked;
  if (other.unlocked &&
      (!acc.unlocked || other.unlockedSeq < acc.unlockedSeq)) {
    acc.unlocked = true;
    acc.unlockedSite = other.unlockedSite;
    acc.unlockedSeq = other.unlockedSeq;
  }
}

void ReferenceVisitor::traverseWorkFunction(const clang::FunctionDecl *fn,
                                            uint32_t slot) {
  frames_.push_back({fn, slot, FrameKind::Function, false});
  const bool defined = fn->doesThisDeclarationHaveABody() ||
                       fn->isThisDeclarationADefinition();
  if (defined && !fn->isInvalidDecl() && !fn->isDependentContext()) {
    if (const auto *ctor = llvm::dyn_cast<clang::CXXConstructorDecl>(fn)) {
      for (const clang::CXXCtorInitializer *init : ctor->inits())
        if (init->getInit())
          TraverseStmt(init->getInit());
      noteConstructorCleanup(ctor);
    }
    TraverseStmt(fn->getBody());
    // Default arguments callers used, instantiated on this declaration:
    // an instantiation's are attributed to the template (where they are
    // written), a transparent function's to its callers.
    for (const clang::ParmVarDecl *param : fn->parameters())
      if (param->hasDefaultArg() && !param->hasUninstantiatedDefaultArg() &&
          !param->hasUnparsedDefaultArg())
        TraverseStmt(const_cast<clang::Expr *>(param->getDefaultArg()));
  }
  if (const auto *dtor = llvm::dyn_cast<clang::CXXDestructorDecl>(fn))
    noteImplicitDestruction(dtor);
  // A constructor or destructor stores the vtable pointer.
  if (const auto *method = llvm::dyn_cast<clang::CXXMethodDecl>(fn))
    if (llvm::isa<clang::CXXConstructorDecl>(method) ||
        llvm::isa<clang::CXXDestructorDecl>(method)) {
      const clang::CXXRecordDecl *cls = method->getParent();
      if (cls->hasDefinition() && !cls->isDependentContext() &&
          cls->isDynamicClass())
        noteVtableUse(cls, fn->getLocation());
    }
}

// Member and base initializers a constructor does not write (default
// construction, a default member initializer).
void ReferenceVisitor::traverseImplicitInits(
    const clang::CXXConstructorDecl *ctor) {
  for (const clang::CXXCtorInitializer *init : ctor->inits())
    if (!init->isWritten() && init->getInit())
      TraverseStmt(init->getInit());
}

// If a constructor throws, the bases and members already built are
// destroyed: each one a later initializer or the body can throw after. A
// delegating constructor's object is complete once the target returns, so
// its body throwing runs the class's destructor.
void ReferenceVisitor::noteConstructorCleanup(
    const clang::CXXConstructorDecl *ctor) {
  if (!ctx_->getLangOpts().CXXExceptions || ctor->isInvalidDecl() ||
      !canThrow(ctor))
    return;
  const clang::CXXRecordDecl *cls = ctor->getParent();
  if (!cls->hasDefinition())
    return;
  cls = cls->getDefinition();
  if (cls->isInvalidDecl() || cls->isDependentContext() || cls->isUnion())
    return;
  // Any body but an empty block may throw (a function-try-block too).
  const clang::Stmt *body = ctor->getBody();
  const auto *block = llvm::dyn_cast_or_null<clang::CompoundStmt>(body);
  bool later = body && !(block && block->body_empty());
  const clang::SourceLocation site = ctor->getLocation();
  if (ctor->isDelegatingConstructor()) {
    if (later)
#if VYCOR_LLVM_AT_LEAST(22)
      noteDestructorOf(ctx_->getCanonicalTagType(cls), site);
#else
      noteDestructorOf(ctx_->getRecordType(cls), site);
#endif
    return;
  }
  // Initializers are in construction order: walk them last to first.
  clang::CXXConstructorDecl::init_const_iterator inits = ctor->init_begin();
  for (unsigned i = ctor->getNumCtorInitializers(); i-- > 0;) {
    const clang::CXXCtorInitializer *init = inits[i];
    if (later) {
      if (init->isBaseInitializer())
        noteDestructorOf(clang::QualType(init->getBaseClass(), 0), site);
      else if (const clang::FieldDecl *field = init->getAnyMember())
        noteDestructorOf(field->getType(), site);
    }
    if (!later && mayThrow(init->getInit()))
      later = true;
  }
}

// After its body a destructor destroys the members, then the bases (the
// complete-object destructor also the virtual bases). A virtual destructor's
// deleting form calls the class's deallocation function.
void ReferenceVisitor::noteImplicitDestruction(
    const clang::CXXDestructorDecl *dtor) {
  const clang::SourceLocation site = dtor->getLocation();
  if (!forEachDestroyedSubobject(dtor, [&](clang::QualType type) {
        noteDestructorOf(type, site);
      }))
    return;
  if (dtor->isVirtual())
    if (const clang::FunctionDecl *dealloc = dtor->getOperatorDelete())
      addRef(functionTarget(dealloc), RefRole::Call, site);
}

void ReferenceVisitor::noteDestructorOf(clang::QualType type,
                                        clang::SourceLocation site) {
  if (const clang::CXXDestructorDecl *dtor = destructorRunFor(*ctx_, type))
    addRef(functionTarget(dtor), RefRole::Call, site);
}

// A variable of a class with a non-trivial destructor is destroyed when it
// leaves scope; a static is destroyed at exit through a handler registered
// by its initialization. A parameter is destroyed by the caller, whose
// argument is a temporary there.
void ReferenceVisitor::noteVariableDestruction(const clang::VarDecl *var) {
  if (unevaluated_ > 0 || llvm::isa<clang::ParmVarDecl>(var) ||
      var->isInvalidDecl() || var->getType().isNull() ||
      var->isThisDeclarationADefinition() == clang::VarDecl::DeclarationOnly)
    return;
  if (var->hasGlobalStorage() && var->isFileVarDecl() &&
      mode_ != Mode::AsWritten)
    return;
  noteDestructorOf(var->getType(), var->getLocation());
}

// The vtable of `cls` is used. Recorded as the class's vtable_use; the
// virtual functions the vtable holds are code the program keeps:
//   - of a recorded class template's specialization, their bodies are read
//     as instantiations (the class's own symbol holds the vtable entries);
//   - an implicit virtual destructor is no symbol: the class references
//     what it does;
//   - of a class that is not recorded (a system template's specialization,
//     such as a shared_ptr control block), the referrer constructing it
//     references what they do.
void ReferenceVisitor::noteVtableUse(const clang::CXXRecordDecl *cls,
                                     clang::SourceLocation site) {
  const uint32_t slot = targetSlot(cls);
  addRef(slot, RefRole::VtableUse, site);
  const clang::CXXRecordDecl *def = cls->getDefinition();
  if (!def || def->isInvalidDecl() || def->isDependentContext())
    return;
  const bool known = !vtableClasses_.insert(def).second;
  if (known && slot != kDropped)
    return;
  for (const clang::CXXMethodDecl *method : def->methods()) {
    if (!method->isVirtual() || method->isPureVirtual())
      continue;
    if (slot == kDropped) {
      addRef(functionTarget(method), RefRole::Call, site);
      continue;
    }
    const uint32_t target = functionTarget(method);
    if (target != kDropped && entities_[target].kind == EntityKind::Transparent)
      addRefFrom(slot, target, RefRole::Call, site, 0, 0);
  }
}

// ============================================================================
// Entities
// ============================================================================

uint32_t ReferenceVisitor::targetSlot(const clang::NamedDecl *decl) {
  auto it = slotOf_.find(decl);
  if (it != slotOf_.end())
    return it->second;
  const uint32_t slot = resolveTarget(decl);
  slotOf_[decl] = slot;
  return slot;
}

uint32_t ReferenceVisitor::resolveTarget(const clang::NamedDecl *decl) {
  const clang::NamedDecl *canonical = nullptr;
  EntityKind kind = EntityKind::Function;
  if (const auto *fn = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
    // Compiler-written functions are not in the code as written. A lambda's
    // call operator is reached through the closure, never by name.
    if (llvm::isa<clang::CXXDeductionGuideDecl>(fn))
      return kDropped;
    if (fn->isImplicit()) {
      // `using Base::Base;`: constructing the derived class runs the base
      // constructor the user wrote.
      const auto *ctor = llvm::dyn_cast<clang::CXXConstructorDecl>(fn);
      while (ctor && ctor->isImplicit() && ctor->isInheritingConstructor())
        ctor = ctor->getInheritedConstructor().getConstructor();
      if (ctor)
        fn = ctor;
      if (!fn)
        return kDropped;
      if (fn->isImplicit()) {
        // What is left: implicit special members and compiler builtins are
        // dropped. A C function called without a declaration, and a library
        // function the compiler declares itself, are real symbols.
        if (llvm::isa<clang::CXXMethodDecl>(fn) || fn->isDefaulted())
          return kDropped;
        const unsigned builtin = fn->getBuiltinID();
        if (builtin != 0 &&
            !ctx_->BuiltinInfo.isPredefinedLibFunction(builtin))
          return kDropped;
      }
    }
    if (const auto *method = llvm::dyn_cast<clang::CXXMethodDecl>(fn))
      if (method->getParent()->isLambda())
        return kDropped;
    if (const clang::FunctionDecl *pattern =
            fn->getTemplateInstantiationPattern())
      fn = pattern;
    canonical = fn->getCanonicalDecl();
  } else if (const auto *var = llvm::dyn_cast<clang::VarDecl>(decl)) {
    if (!var->hasGlobalStorage())
      return kDropped;
    if (const clang::VarDecl *pattern = var->getTemplateInstantiationPattern())
      var = pattern;
    canonical = var->getCanonicalDecl();
    kind = EntityKind::Variable;
  } else if (const auto *cls = llvm::dyn_cast<clang::CXXRecordDecl>(decl)) {
    if (cls->isLambda())
      return kDropped;
    canonical = classPattern(cls)->getCanonicalDecl();
    kind = EntityKind::Class;
  } else {
    return kDropped;
  }

  if (canonical != decl) {
    auto it = slotOf_.find(canonical);
    if (it != slotOf_.end())
      return it->second;
  }
  uint32_t slot = kDropped;
  // Of the standard library and other system code only strong symbols are
  // kept (malloc, std::exception's typeinfo): those are link dependencies.
  // Its inline and template code is everywhere and says nothing.
  if (!onlyInSystemHeaders(canonical) ||
      linkageOf(kind, canonical) == LinkageClass::External) {
    slot = static_cast<uint32_t>(entities_.size());
    Entity entity;
    entity.decl = canonical;
    entity.kind = kind;
    entities_.push_back(entity);
  }
  slotOf_[canonical] = slot;
  return slot;
}

bool ReferenceVisitor::onlyInSystemHeaders(const clang::Decl *decl) const {
  for (const clang::Decl *redecl : decl->redecls()) {
    clang::SourceLocation loc = redecl->getLocation();
    if (loc.isValid() && !sm_.isInSystemHeader(loc))
      return false;
  }
  return true;
}

bool ReferenceVisitor::inMainFile(clang::SourceLocation loc) const {
  return loc.isValid() &&
         sm_.getFileID(sm_.getFileLoc(loc)) == sm_.getMainFileID();
}

uint32_t ReferenceVisitor::referrerSlot(clang::SourceLocation site) {
  if (!frames_.empty()) {
    const uint32_t slot = resolveFrame(frames_.size() - 1);
    if (slot != kDropped)
      return slot;
  }
  return fileSlot(site);
}

uint32_t ReferenceVisitor::resolveFrame(size_t frameIndex) {
  Frame &frame = frames_[frameIndex];
  if (frame.slot != kUnresolved)
    return frame.slot;
  if (frame.kind != FrameKind::Lambda) {
    frame.slot = targetSlot(llvm::cast<clang::NamedDecl>(frame.decl));
    return frame.slot;
  }
  const auto *op = llvm::cast<clang::CXXMethodDecl>(frame.decl);
  auto found = lambdaSlot_.find(op);
  if (found != lambdaSlot_.end()) {
    frame.slot = found->second;
    return frame.slot;
  }
  // The call graph's lambda identity: closure location plus the nearest
  // enclosing function that is not itself a lambda.
  std::string enclosing;
  for (size_t i = frameIndex; i-- > 0;) {
    if (frames_[i].kind != FrameKind::Function)
      continue;
    enclosing = llvm::cast<clang::FunctionDecl>(frames_[i].decl)
                    ->getQualifiedNameAsString();
    break;
  }
  Entity entity;
  entity.decl = op;
  entity.kind = EntityKind::Lambda;
  if (frame.inLoop)
    entity.flags |= kSymInLoop;
  entity.aux = static_cast<uint32_t>(lambdaIdents_.size());
  lambdaIdents_.push_back(
      lambdaClosureIdent(sm_, op->getParent()->getBeginLoc(), enclosing));
  const auto slot = static_cast<uint32_t>(entities_.size());
  entities_.push_back(entity);
  lambdaSlot_[op] = slot;
  frames_[frameIndex].slot = slot;
  return slot;
}

// File scope as a referrer: a reference outside every function and variable
// initializer (a default member initializer, a template argument of a
// declaration) belongs to the file it is written in.
uint32_t ReferenceVisitor::fileSlot(clang::SourceLocation site) {
  clang::FileID fid;
  if (site.isValid())
    fid = sm_.getFileID(sm_.getFileLoc(site));
  if (fid.isInvalid())
    fid = sm_.getMainFileID();
  if (fid.isInvalid())
    return kDropped;
  auto it = fileSlot_.find(fid);
  if (it != fileSlot_.end())
    return it->second;
  const auto slot = static_cast<uint32_t>(entities_.size());
  Entity entity;
  entity.file = fid;
  entity.kind = EntityKind::File;
  entities_.push_back(entity);
  fileSlot_[fid] = slot;
  return slot;
}

// ============================================================================
// Classification
// ============================================================================

LinkageClass ReferenceVisitor::linkageOf(EntityKind kind,
                                         const clang::NamedDecl *decl) const {
  switch (kind) {
  case EntityKind::Function:
    return linkageOfFunction(llvm::cast<clang::FunctionDecl>(decl));
  case EntityKind::Variable:
    return linkageOfVariable(llvm::cast<clang::VarDecl>(decl));
  case EntityKind::Class:
    return linkageOfClass(llvm::cast<clang::CXXRecordDecl>(decl));
  case EntityKind::Lambda:
    return LinkageClass::Internal;
  case EntityKind::File:
  case EntityKind::Transparent:
    break;
  }
  return LinkageClass::None;
}

LinkageClass
ReferenceVisitor::linkageOfFunction(const clang::FunctionDecl *fn) const {
  if (!fn->isExternallyVisible())
    return LinkageClass::Internal;
  if (fn->isTemplated())
    return LinkageClass::TemplateInstantiation;
  const clang::FunctionDecl *definition = nullptr;
  const clang::FunctionDecl *probe =
      fn->isDefined(definition) ? definition : fn->getMostRecentDecl();
  if (probe->isInlined())
    return LinkageClass::Vague;
  // Defaulted on its first declaration: emitted wherever it is used.
  if (const auto *method = llvm::dyn_cast<clang::CXXMethodDecl>(probe))
    if (!method->isUserProvided())
      return LinkageClass::Vague;
  return LinkageClass::External;
}

LinkageClass
ReferenceVisitor::linkageOfVariable(const clang::VarDecl *var) const {
  if (!var->isExternallyVisible())
    return LinkageClass::Internal;
  if (var->isTemplated())
    return LinkageClass::TemplateInstantiation;
  if (var->isStaticLocal())
    return LinkageClass::Vague; // of an inline function
  for (const clang::VarDecl *redecl : var->redecls())
    if (redecl->isInline())
      return LinkageClass::Vague;
  return LinkageClass::External;
}

// A class is a link-level entity through its vtable and typeinfo: strong
// when a key function pins them to one TU, vague otherwise.
LinkageClass
ReferenceVisitor::linkageOfClass(const clang::CXXRecordDecl *cls) const {
  if (!cls->isExternallyVisible())
    return LinkageClass::Internal;
  if (cls->isTemplated())
    return LinkageClass::TemplateInstantiation;
  const clang::CXXRecordDecl *definition = cls->getDefinition();
  if (definition && !definition->isInvalidDecl() &&
      ctx_->getCurrentKeyFunction(definition))
    return LinkageClass::External;
  return LinkageClass::Vague;
}

uint32_t ReferenceVisitor::symbolFlags(const Entity &entity) const {
  uint32_t flags = entity.flags;
  switch (entity.kind) {
  case EntityKind::Function: {
    const auto *fn =
        llvm::cast<clang::FunctionDecl>(entity.decl)->getMostRecentDecl();
    flags |= attributeFlags(fn, clang::NamedDecl::VisibilityForValue);
    if (fn->isExternC())
      flags |= kSymExternC;
    if (fn->isMain() || fn->isMSVCRTEntryPoint())
      flags |= kSymMain;
    if (fn->getTemplateSpecializationKind() ==
        clang::TSK_ExplicitSpecialization)
      flags |= kSymSpecialization;
    if (const auto *method = llvm::dyn_cast<clang::CXXMethodDecl>(fn)) {
      flags |= kSymMember;
      if (method->isVirtual())
        flags |= kSymVirtual;
    }
    break;
  }
  case EntityKind::Variable: {
    const auto *var =
        llvm::cast<clang::VarDecl>(entity.decl)->getMostRecentDecl();
    flags |= attributeFlags(var, clang::NamedDecl::VisibilityForValue);
    if (var->isExternC())
      flags |= kSymExternC;
    if (var->isStaticDataMember())
      flags |= kSymMember;
    if (var->isStaticLocal())
      flags |= kSymStaticLocal;
    if (var->getTLSKind() != clang::VarDecl::TLS_None)
      flags |= kSymThreadLocal;
    if (var->getTemplateSpecializationKind() ==
        clang::TSK_ExplicitSpecialization)
      flags |= kSymSpecialization;
    clang::QualType type = var->getType();
    if (!type.isNull()) {
      clang::QualType element =
          ctx_->getBaseElementType(type.getNonReferenceType());
      if (element.isConstQualified())
        flags |= kSymConst;
      if (isAtomic(element))
        flags |= kSymAtomic;
      if (isSyncPrimitiveType(*ctx_, element))
        flags |= kSymSyncPrimitive;
    }
    break;
  }
  case EntityKind::Class: {
    const auto *cls = llvm::cast<clang::CXXRecordDecl>(entity.decl);
    flags |= attributeFlags(cls->getMostRecentDecl(),
                            clang::NamedDecl::VisibilityForType);
    if (cls->hasDefinition() && cls->isDynamicClass())
      flags |= kSymDynamicClass;
    if (cls->getTemplateSpecializationKind() ==
        clang::TSK_ExplicitSpecialization)
      flags |= kSymSpecialization;
    break;
  }
  case EntityKind::Lambda:
  case EntityKind::File:
  case EntityKind::Transparent:
    break;
  }
  return flags;
}

uint32_t ReferenceVisitor::lineSpan(clang::SourceRange range) const {
  clang::SourceLocation begin = sm_.getFileLoc(range.getBegin());
  clang::SourceLocation end = sm_.getFileLoc(range.getEnd());
  if (begin.isInvalid() || end.isInvalid())
    return 0;
  const auto [beginFile, beginOffset] = sm_.getDecomposedLoc(begin);
  const auto [endFile, endOffset] = sm_.getDecomposedLoc(end);
  if (beginFile != endFile || endOffset < beginOffset)
    return 1;
  return sm_.getLineNumber(endFile, endOffset) -
         sm_.getLineNumber(beginFile, beginOffset) + 1;
}

// Size proxy of a definition: source lines for a function, bytes for a
// variable or class. 0 when it cannot be computed.
uint32_t ReferenceVisitor::definitionSize(EntityKind kind,
                                          const clang::Decl *definition) const {
  switch (kind) {
  case EntityKind::Function:
  case EntityKind::Lambda:
    return lineSpan(definition->getSourceRange());
  case EntityKind::Variable: {
    const auto *var = llvm::cast<clang::VarDecl>(definition);
    if (var->isInvalidDecl() || !isSizable(*ctx_, var->getType()))
      return 0;
    return static_cast<uint32_t>(std::min<int64_t>(
        ctx_->getTypeSizeInChars(var->getType()).getQuantity(), UINT32_MAX));
  }
  case EntityKind::Class: {
    const auto *cls = llvm::cast<clang::CXXRecordDecl>(definition);
    if (!hasLayout(cls))
      return 0;
    return static_cast<uint32_t>(std::min<int64_t>(
        ctx_->getASTRecordLayout(cls).getSize().getQuantity(), UINT32_MAX));
  }
  case EntityKind::File:
  case EntityKind::Transparent:
    break;
  }
  return 0;
}

// ============================================================================
// Lowering: entity slots -> interned records
// ============================================================================

// One name per file however an #include spelled it: absolute, dots removed
// (the form TU paths have).
bool ReferenceVisitor::filePath(clang::FileID fid,
                                llvm::SmallVectorImpl<char> &path) const {
  if (!fid.isValid())
    return false;
  auto entry = sm_.getFileEntryRefForID(fid);
  if (!entry)
    return false;
  const llvm::StringRef name = entry->getName();
  path.assign(name.begin(), name.end());
  sm_.getFileManager().makeAbsolutePath(path);
  llvm::sys::path::remove_dots(path, /*remove_dot_dot=*/true);
  return true;
}

ReferenceVisitor::Id ReferenceVisitor::fileId(clang::FileID fid) {
  if (fid.isValid()) {
    auto it = fileIds_.find(fid);
    if (it != fileIds_.end())
      return it->second;
    llvm::SmallString<256> path;
    if (filePath(fid, path)) {
      const Id id = index_.internText(path);
      fileIds_[fid] = id;
      return id;
    }
  }
  if (unknownFile_ == ReferenceIndex::kNoId)
    unknownFile_ = index_.internText("<unknown>");
  return unknownFile_;
}

ReferenceVisitor::Site ReferenceVisitor::siteOf(clang::SourceLocation loc) {
  Site site;
  if (loc.isValid())
    loc = sm_.getFileLoc(loc);
  if (loc.isInvalid()) {
    site.file = fileId(clang::FileID());
    return site;
  }
  const auto [fid, offset] = sm_.getDecomposedLoc(loc);
  site.file = fileId(fid);
  site.line = sm_.getLineNumber(fid, offset);
  site.col = sm_.getColumnNumber(fid, offset);
  site.mainFile = fid == sm_.getMainFileID();
  return site;
}

ReferenceVisitor::Id ReferenceVisitor::lowerEntity(
    const Entity &entity, std::vector<ReferenceIndex::SymbolRecord> &out) {
  ReferenceIndex::SymbolRecord rec;

  if (entity.kind == EntityKind::File) {
    rec.name = fileId(entity.file);
    rec.file = rec.name;
    llvm::SmallString<256> usr("vycor-file:");
    llvm::SmallString<256> path;
    if (filePath(entity.file, path))
      usr += path;
    else
      usr += "<unknown>";
    rec.usr = index_.internText(usr);
    rec.kind = SymbolKind::File;
    rec.linkage = LinkageClass::None;
    rec.flags = kSymDefinition;
    if (entity.file != sm_.getMainFileID())
      rec.flags |= kSymInHeader;
    if (sm_.isInSystemHeader(sm_.getLocForStartOfFile(entity.file)))
      rec.flags |= kSymSystemHeader;
    out.push_back(rec);
    return rec.usr;
  }

  if (entity.kind == EntityKind::Lambda) {
    const Ident &ident = lambdaIdents_[entity.aux];
    const auto *op = llvm::cast<clang::CXXMethodDecl>(entity.decl);
    const Site site = siteOf(op->getParent()->getBeginLoc());
    rec.usr = index_.internText(ident.usr);
    rec.name = index_.internText(ident.display);
    rec.file = site.file;
    rec.line = site.line;
    rec.kind = SymbolKind::Function;
    rec.linkage = LinkageClass::Internal;
    rec.flags = kSymLambda | kSymDefinition | entity.flags;
    if (!site.mainFile)
      rec.flags |= kSymInHeader;
    rec.size = definitionSize(EntityKind::Lambda, op);
    out.push_back(rec);
    return rec.usr;
  }

  const Ident &ident = usrs_.identFor(entity.decl);
  rec.usr = index_.internText(ident.usr);
  rec.name = index_.internText(ident.display);
  if (const auto *var = llvm::dyn_cast<clang::VarDecl>(entity.decl);
      var && var->isStaticLocal()) {
    // Clang prints a block-scope name bare; qualify it with its function
    // ("next::n") so two functions' statics are told apart.
    for (const clang::DeclContext *dc = var->getDeclContext(); dc;
         dc = dc->getParent()) {
      const auto *fn = llvm::dyn_cast<clang::FunctionDecl>(dc);
      if (!fn || clang::isLambdaCallOperator(fn))
        continue;
      rec.name = index_.internText(fn->getQualifiedNameAsString() +
                                   "::" + ident.display);
      break;
    }
  }
  rec.linkage = linkageOf(entity.kind, entity.decl);
  switch (entity.kind) {
  case EntityKind::Variable:
    rec.kind = SymbolKind::Variable;
    break;
  case EntityKind::Class:
    rec.kind = SymbolKind::Class;
    break;
  default:
    rec.kind = SymbolKind::Function;
    break;
  }
  const uint32_t symbolBits = symbolFlags(entity) & ~kSymbolPerDeclFlags;

  // One record per declaration: where it is, and whether it defines.
  auto emit = [&](const clang::Decl *decl, bool definition, bool system) {
    const Site site = siteOf(decl->getLocation());
    rec.file = site.file;
    rec.line = site.line;
    rec.flags = symbolBits;
    rec.size = 0;
    if (definition) {
      rec.flags |= kSymDefinition;
      rec.size = definitionSize(entity.kind, decl);
    }
    if (!site.mainFile)
      rec.flags |= kSymInHeader;
    if (system)
      rec.flags |= kSymSystemHeader;
    out.push_back(rec);
  };
  auto isSystem = [&](const clang::Decl *decl) {
    clang::SourceLocation loc = decl->getLocation();
    return loc.isInvalid() || sm_.isInSystemHeader(loc);
  };

  if (entity.kind == EntityKind::Class) {
    // A class is declared by its definition; forward declarations are not
    // facts about where it lives.
    const auto *cls = llvm::cast<clang::CXXRecordDecl>(entity.decl);
    const clang::CXXRecordDecl *definition = cls->getDefinition();
    const clang::Decl *decl = definition ? definition : cls;
    emit(decl, definition != nullptr, isSystem(decl));
    return rec.usr;
  }

  bool any = false;
  for (const clang::Decl *redecl : entity.decl->redecls()) {
    // An implicit declaration with a place in the user's source is a C
    // function called without a prototype: the call site declares it.
    if (isSystem(redecl))
      continue;
    bool definition = false;
    if (const auto *fn = llvm::dyn_cast<clang::FunctionDecl>(redecl))
      definition = fn->isThisDeclarationADefinition();
    else if (const auto *var = llvm::dyn_cast<clang::VarDecl>(redecl))
      definition = var->isThisDeclarationADefinition() !=
                   clang::VarDecl::DeclarationOnly;
    emit(redecl, definition, false);
    any = true;
  }
  if (!any)
    emit(entity.decl, false, true); // a strong symbol of a system library
  return rec.usr;
}

// Replace every reference to a transparent function by what that function
// references, transitively through other transparent functions. Summaries
// are computed once per strongly connected component of the transparent
// functions (Tarjan, iterative), sinks first. A spliced reference keeps the
// count and site of the reference to the transparent function; one the code
// as written already holds is not added again. Either way the reference
// takes the lock facts and loop flag of the use and the `mutates` flag of
// the transparent code. `refs` is sorted by key on entry and on return.
void ReferenceVisitor::spliceTransparent(
    std::vector<std::pair<uint64_t, RefAcc>> &refs,
    std::vector<std::pair<uint64_t, uint64_t>> &lockVia) {
  const uint32_t n = transparentCount_;
  const auto referrerOf = [](uint64_t key) {
    return static_cast<uint32_t>(key >> kReferrerShift);
  };
  const auto targetOf = [](uint64_t key) {
    return static_cast<uint32_t>((key >> kTargetShift) &
                                 ((uint64_t{1} << (kReferrerShift -
                                                    kTargetShift)) -
                                  1));
  };
  const auto transparent = [&](uint32_t slot) {
    return entities_[slot].kind == EntityKind::Transparent;
  };

  // Each transparent function's own references: one run of the sorted list.
  std::vector<uint32_t> begin(n, 0), end(n, 0);
  for (uint32_t i = 0, count = static_cast<uint32_t>(refs.size()); i < count;) {
    const uint32_t referrer = referrerOf(refs[i].first);
    uint32_t j = i + 1;
    while (j < count && referrerOf(refs[j].first) == referrer)
      ++j;
    if (transparent(referrer)) {
      begin[entities_[referrer].aux] = i;
      end[entities_[referrer].aux] = j;
    }
    i = j;
  }

  constexpr uint32_t kNone = UINT32_MAX;
  std::vector<uint32_t> order(n, kNone), low(n, 0), sccOf(n, kNone);
  std::vector<uint8_t> onStack(n, 0);
  std::vector<uint32_t> stack;
  struct Step {
    uint32_t node;
    uint32_t edge;
  };
  std::vector<Step> calls;
  // Component summaries, flat: component c's keys (referrer bits clear) and
  // the `mutates` flag of each are sumData[sumBegin[c], sumBegin[c + 1]).
  std::vector<std::pair<uint64_t, uint8_t>> sumData;
  std::vector<uint32_t> sumBegin{0};
  std::vector<std::pair<uint64_t, uint8_t>> acc;
  uint32_t counter = 0;
  const auto open = [&](uint32_t v) {
    order[v] = low[v] = counter++;
    stack.push_back(v);
    onStack[v] = 1;
    calls.push_back({v, begin[v]});
  };
  for (uint32_t root = 0; root < n; ++root) {
    if (order[root] != kNone)
      continue;
    open(root);
    while (!calls.empty()) {
      const uint32_t v = calls.back().node;
      if (calls.back().edge < end[v]) {
        const uint32_t target = targetOf(refs[calls.back().edge++].first);
        if (!transparent(target))
          continue;
        const uint32_t u = entities_[target].aux;
        if (order[u] == kNone)
          open(u);
        else if (onStack[u])
          low[v] = std::min(low[v], order[u]);
        continue;
      }
      calls.pop_back();
      if (!calls.empty()) {
        const uint32_t parent = calls.back().node;
        low[parent] = std::min(low[parent], low[v]);
      }
      if (low[v] != order[v])
        continue;
      // v roots a component: everything it reaches is summarized already.
      const auto scc = static_cast<uint32_t>(sumBegin.size() - 1);
      size_t first = stack.size();
      do {
        --first;
        sccOf[stack[first]] = scc;
        onStack[stack[first]] = 0;
      } while (stack[first] != v);
      acc.clear();
      for (size_t m = first; m < stack.size(); ++m) {
        const uint32_t node = stack[m];
        for (uint32_t e = begin[node]; e < end[node]; ++e) {
          const uint64_t key = refs[e].first;
          const uint32_t target = targetOf(key);
          if (!transparent(target)) {
            acc.emplace_back(key & ((uint64_t{1} << kReferrerShift) - 1),
                             static_cast<uint8_t>(refs[e].second.flags &
                                                  kRefMutates));
            continue;
          }
          const uint32_t other = sccOf[entities_[target].aux];
          if (other != scc)
            acc.insert(acc.end(), sumData.begin() + sumBegin[other],
                       sumData.begin() + sumBegin[other + 1]);
        }
      }
      stack.resize(first);
      llvm::sort(acc);
      // One entry per key, its flags the union.
      size_t out = 0;
      for (size_t m = 0; m < acc.size(); ++m) {
        if (out != 0 && acc[out - 1].first == acc[m].first)
          acc[out - 1].second |= acc[m].second;
        else
          acc[out++] = acc[m];
      }
      acc.resize(out);
      sumData.insert(sumData.end(), acc.begin(), acc.end());
      sumBegin.push_back(static_cast<uint32_t>(sumData.size()));
    }
  }

  llvm::DenseMap<uint64_t, RefAcc> spliced;
  // Uses folded into a reference the code as written holds: (key, facts).
  std::vector<std::pair<uint64_t, RefAcc>> intoWritten;
  size_t kept = 0;
  for (size_t i = 0; i < refs.size(); ++i) {
    const uint64_t key = refs[i].first;
    const uint32_t referrer = referrerOf(key);
    const uint32_t target = targetOf(key);
    if (transparent(referrer))
      continue;
    if (!transparent(target)) {
      refs[kept++] = refs[i];
      continue;
    }
    const RefAcc &via = refs[i].second;
    const uint64_t from = static_cast<uint64_t>(referrer) << kReferrerShift;
    const uint32_t scc = sccOf[entities_[target].aux];
    for (uint32_t k = sumBegin[scc]; k < sumBegin[scc + 1]; ++k) {
      const uint64_t reached = sumData[k].first;
      const uint64_t joined = from | reached;
      // A class whose implicit destructor stores its own vtable pointer
      // says nothing new.
      if (static_cast<uint32_t>(reached >> kTargetShift) == referrer &&
          static_cast<RefRole>(reached & kRoleMask) == RefRole::VtableUse)
        continue;
      RefAcc use = via;
      use.flags = static_cast<uint8_t>((via.flags & kRefInLoop) |
                                       sumData[k].second);
      if (via.locked)
        lockVia.emplace_back(joined, key);
      // One the code as written holds keeps its count and site.
      if (refs_.count(joined) != 0) {
        intoWritten.emplace_back(joined, use);
        continue;
      }
      auto [it, inserted] = spliced.try_emplace(joined, use);
      if (!inserted) {
        it->second.count = std::max(it->second.count, use.count);
        mergeLockFacts(it->second, use);
      }
    }
  }
  refs.resize(kept);
  // Every such key is kept above: its referrer and target are not
  // transparent.
  for (const auto &[joined, use] : intoWritten) {
    auto at = std::lower_bound(
        refs.begin(), refs.end(), joined,
        [](const auto &entry, uint64_t key) { return entry.first < key; });
    assert(at != refs.end() && at->first == joined);
    if (at != refs.end() && at->first == joined)
      mergeLockFacts(at->second, use);
  }
  for (const auto &entry : spliced)
    refs.emplace_back(entry.first, entry.second);
  llvm::sort(refs, [](const auto &a, const auto &b) {
    return a.first < b.first;
  });
}

void ReferenceVisitor::lower() {
  // What only instantiations hold, at the largest count one of them has.
  for (const auto &entry : instRefs_)
    refs_.try_emplace(entry.first, entry.second);
  // Hash-map order is an implementation detail; slot order is the order the
  // code was read in.
  std::vector<std::pair<uint64_t, RefAcc>> refs;
  refs.reserve(refs_.size());
  for (const auto &entry : refs_)
    refs.emplace_back(entry.first, entry.second);
  llvm::sort(refs, [](const auto &a, const auto &b) {
    return a.first < b.first;
  });
  // A spliced reference's occurrences under a lock are its uses' (spliced
  // key, use key).
  std::vector<std::pair<uint64_t, uint64_t>> lockVia;
  if (transparentCount_ != 0)
    spliceTransparent(refs, lockVia);
  for (const auto &ref : refs) {
    entities_[ref.first >> kReferrerShift].emit = true;
    entities_[static_cast<uint32_t>(ref.first >> kTargetShift)].emit = true;
  }
  // Lock acquisitions: what only instantiations hold joins the code as
  // written. The target is a lock identity, not an entity.
  for (const auto &entry : instAcqs_)
    acqs_.try_emplace(entry.first, entry.second);
  std::vector<std::pair<uint64_t, RefAcc>> acqs(acqs_.begin(), acqs_.end());
  llvm::sort(acqs, [](const auto &a, const auto &b) {
    return a.first < b.first;
  });
  for (const auto &acq : acqs)
    entities_[acq.first >> kReferrerShift].emit = true;

  std::vector<Id> usrOf(entities_.size(), ReferenceIndex::kNoId);
  std::vector<ReferenceIndex::SymbolRecord> symbols;
  symbols.reserve(entities_.size() + entities_.size() / 2);
  for (size_t slot = 0; slot < entities_.size(); ++slot)
    if (entities_[slot].emit)
      usrOf[slot] = lowerEntity(entities_[slot], symbols);

  // Occurrences under a lock, grouped by reference.
  std::vector<std::pair<uint64_t, std::pair<uint32_t, LockedOcc>>> locked;
  locked.reserve(lockedOcc_.size());
  for (const auto &entry : lockedOcc_)
    locked.push_back({entry.first.first, {entry.first.second, entry.second}});
  if (!lockVia.empty()) {
    llvm::sort(locked, [](const auto &a, const auto &b) {
      return a.first < b.first;
    });
    // Indices, not iterators: the appends below may reallocate.
    const size_t own = locked.size();
    for (const auto &[joined, use] : lockVia) {
      size_t at = static_cast<size_t>(
          std::lower_bound(locked.begin(),
                           locked.begin() + static_cast<std::ptrdiff_t>(own),
                           use,
                           [](const auto &entry, uint64_t key) {
                             return entry.first < key;
                           }) -
          locked.begin());
      for (; at < own && locked[at].first == use; ++at) {
        const std::pair<uint32_t, LockedOcc> occ = locked[at].second;
        locked.push_back({joined, occ});
      }
    }
    // One occurrence per (reference, lock set): the first.
    llvm::sort(locked, [](const auto &a, const auto &b) {
      return std::tie(a.first, a.second.first, a.second.second.seq) <
             std::tie(b.first, b.second.first, b.second.second.seq);
    });
    locked.erase(std::unique(locked.begin(), locked.end(),
                             [](const auto &a, const auto &b) {
                               return a.first == b.first &&
                                      a.second.first == b.second.first;
                             }),
                 locked.end());
  }
  llvm::sort(locked, [](const auto &a, const auto &b) {
    return std::tie(a.first, a.second.second.seq) <
           std::tie(b.first, b.second.second.seq);
  });
  std::vector<std::pair<uint32_t, LockedOcc>> lockedOfRef;
  size_t nextLocked = 0;

  std::vector<ReferenceIndex::ReferenceRecord> records;
  records.reserve(refs.size());
  for (const auto &ref : refs) {
    lockedOfRef.clear();
    while (nextLocked < locked.size() && locked[nextLocked].first < ref.first)
      ++nextLocked;
    for (; nextLocked < locked.size() && locked[nextLocked].first == ref.first;
         ++nextLocked)
      lockedOfRef.push_back(locked[nextLocked].second);
    const std::string locks = lockText(ref.second, lockedOfRef);
    const Site site = siteOf(ref.second.site);
    ReferenceIndex::ReferenceRecord rec;
    rec.referrer = usrOf[ref.first >> kReferrerShift];
    rec.target = usrOf[static_cast<uint32_t>(ref.first >> kTargetShift)];
    rec.file = site.file;
    rec.line = site.line;
    rec.col = site.col;
    rec.count = ref.second.count;
    rec.role = static_cast<RefRole>(ref.first & kRoleMask);
    rec.locks = locks.empty() ? ReferenceIndex::kNoId : index_.internText(locks);
    rec.flags = ref.second.flags;
    assert(rec.referrer != ReferenceIndex::kNoId &&
           rec.target != ReferenceIndex::kNoId);
    records.push_back(rec);
  }

  // Lock acquisitions, with the sets held before each.
  std::vector<std::pair<uint64_t, std::pair<uint32_t, LockedOcc>>> acqLocked;
  acqLocked.reserve(acqOcc_.size());
  for (const auto &entry : acqOcc_)
    acqLocked.push_back({entry.first.first, {entry.first.second, entry.second}});
  llvm::sort(acqLocked, [](const auto &a, const auto &b) {
    return std::tie(a.first, a.second.second.seq) <
           std::tie(b.first, b.second.second.seq);
  });
  nextLocked = 0;
  for (const auto &acq : acqs) {
    lockedOfRef.clear();
    while (nextLocked < acqLocked.size() &&
           acqLocked[nextLocked].first < acq.first)
      ++nextLocked;
    for (; nextLocked < acqLocked.size() &&
           acqLocked[nextLocked].first == acq.first;
         ++nextLocked)
      lockedOfRef.push_back(acqLocked[nextLocked].second);
    const std::string locks = lockText(acq.second, lockedOfRef);
    const Site site = siteOf(acq.second.site);
    ReferenceIndex::ReferenceRecord rec;
    rec.referrer = usrOf[acq.first >> kReferrerShift];
    rec.target = index_.internText(
        lockIdents_[static_cast<uint32_t>(acq.first >> kTargetShift)].usr);
    rec.file = site.file;
    rec.line = site.line;
    rec.col = site.col;
    rec.count = acq.second.count;
    rec.role = static_cast<RefRole>(acq.first & kRoleMask);
    rec.locks = locks.empty() ? ReferenceIndex::kNoId : index_.internText(locks);
    rec.flags = acq.second.flags;
    assert(rec.referrer != ReferenceIndex::kNoId);
    records.push_back(rec);
  }

  const Id tu = index_.internText(tuPath_);
  index_.addTuFacts(tu, symbols, records);

  // Emission facts: the linkage name is the identity the linker merges
  // copies by (one per instantiation); the symbol is the entity the
  // references above attribute the body's references to.
  std::unique_ptr<clang::MangleContext> mangle(ctx_->createMangleContext());
  std::vector<ReferenceIndex::EmissionRecord> emissions;
  emissions.reserve(pendingEmissions_.size());
  std::string name;
  for (const PendingEmission &pending : pendingEmissions_) {
    const clang::FunctionDecl *def = pending.def;
    name.clear();
    {
      llvm::raw_string_ostream os(name);
      if (!mangle->shouldMangleDeclName(def)) {
        if (def->getIdentifier())
          os << def->getName();
      } else if (const auto *ctor =
                     llvm::dyn_cast<clang::CXXConstructorDecl>(def)) {
        // The base-object variant (C2 / D2): CodeGen always emits it, and
        // where the complete-object variant is equivalent (no virtual
        // bases) a discardable definition is emitted only as the base one.
        mangle->mangleName(clang::GlobalDecl(ctor, clang::Ctor_Base), os);
      } else if (const auto *dtor =
                     llvm::dyn_cast<clang::CXXDestructorDecl>(def)) {
        mangle->mangleName(clang::GlobalDecl(dtor, clang::Dtor_Base), os);
      } else {
        mangle->mangleName(clang::GlobalDecl(def), os);
      }
    }
    if (name.empty())
      continue;
    ReferenceIndex::EmissionRecord rec;
    if (pending.slot != kDropped) {
      rec.symbol = usrOf[pending.slot];
      if (rec.symbol == ReferenceIndex::kNoId) {
        const Entity &entity = entities_[pending.slot];
        rec.symbol = index_.internText(
            entity.kind == EntityKind::Lambda
                ? lambdaIdents_[entity.aux].usr
                : usrs_.identFor(entity.decl).usr);
      }
    }
    rec.name = index_.internText(name);
    const Site site = siteOf(def->getLocation());
    rec.file = site.file;
    rec.line = site.line;
    rec.size = pending.size;
    rec.flags = pending.flags;
    emissions.push_back(rec);
  }
  index_.addTuEmissions(tu, emissions);
}

} // namespace vycor
