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

#include "clang/AST/ASTLambda.h"
#include "clang/AST/Attr.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/RecordLayout.h"
#include "clang/AST/StmtCXX.h"
#include "clang/Basic/Builtins.h"
#include "clang/Basic/FileManager.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/Path.h"

#include <algorithm>
#include <cassert>
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

} // namespace

ReferenceVisitor::ReferenceVisitor(ReferenceIndex &index,
                                   clang::SourceManager &sm,
                                   const std::string &tuPath)
    : index_(index), sm_(sm), tuPath_(tuPath) {}

void ReferenceVisitor::run(clang::ASTContext &ctx) {
  ctx_ = &ctx;
  TraverseDecl(ctx.getTranslationUnitDecl());
  lower();
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
  frames_.push_back({fn, kUnresolved, FrameKind::Function, fn->isTemplated()});
  if (fn->isThisDeclarationADefinition() && !fn->isDeleted())
    noteFunctionDefinition(fn);
  const bool ok = Base::TraverseDecl(fn);
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
          addRef(targetSlot(fn), RefRole::Call, cleanup->getLocation());
  // A local's initializer (function-local statics included) belongs to the
  // enclosing function; a namespace-scope variable or static data member is
  // its own referrer.
  if (!var->isFileVarDecl())
    return Base::TraverseDecl(var);
  frames_.push_back({var, kUnresolved, FrameKind::Variable, var->isTemplated()});
  if (var->isThisDeclarationADefinition() != clang::VarDecl::DeclarationOnly)
    noteVariableDefinition(var);
  const bool ok = Base::TraverseDecl(var);
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
    addRef(targetSlot(cls), RefRole::VtableUse, fn->getLocation());
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
  if (tmpl != tmpl->getCanonicalDecl())
    return;
  for (const clang::FunctionDecl *spec : tmpl->specializations()) {
    if (spec->getTemplateSpecializationKind() !=
        clang::TSK_ExplicitInstantiationDefinition)
      continue;
    const uint32_t slot = targetSlot(spec);
    if (slot != kDropped) {
      entities_[slot].flags |= kSymExplicitInstantiation;
      entities_[slot].emit = true;
    }
  }
}

bool ReferenceVisitor::TraverseLambdaExpr(clang::LambdaExpr *expr) {
  const clang::CXXMethodDecl *op = expr->getCallOperator();
  if (!op)
    return Base::TraverseLambdaExpr(expr);
  frames_.push_back({op, kUnresolved, FrameKind::Lambda, op->isTemplated()});
  const bool ok = Base::TraverseLambdaExpr(expr);
  popFrame();
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
      addRef(targetSlot(fn), RefRole::Call, loc);
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

// `delete p` runs the destructor of what p points at.
bool ReferenceVisitor::VisitCXXDeleteExpr(clang::CXXDeleteExpr *expr) {
  if (unevaluated_ > 0)
    return true;
  clang::QualType type = expr->getDestroyedType();
  if (type.isNull())
    return true;
  const clang::CXXRecordDecl *cls = type->getAsCXXRecordDecl();
  if (!cls || !cls->hasDefinition())
    return true;
  cls = cls->getDefinition();
  if (cls->isInvalidDecl() || cls->isDependentContext())
    return true;
  if (const clang::CXXDestructorDecl *dtor = cls->getDestructor())
    addRef(targetSlot(dtor), RefRole::Call, expr->getBeginLoc());
  return true;
}

bool ReferenceVisitor::VisitCallExpr(clang::CallExpr *expr) {
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
      mark(member->getBase(), method->isConst() ? kMarkRead : kMarkWrite);
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
      mark(expr->getArg(0), method->isConst() ? kMarkRead : kMarkWrite);
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
  addRef(targetSlot(ctor), RefRole::Call, expr->getLocation());
  // A constructor the compiler writes stores the vtable pointer inline at
  // the construction site; a user-provided one is its own referrer
  // (noteFunctionDefinition).
  const clang::CXXRecordDecl *cls = ctor->getParent();
  if (!ctor->isUserProvided() && cls->hasDefinition() && cls->isDynamicClass())
    addRef(targetSlot(cls), RefRole::VtableUse, expr->getLocation());
  return true;
}

// ---- references ---------------------------------------------------------------

bool ReferenceVisitor::VisitDeclRefExpr(clang::DeclRefExpr *expr) {
  const clang::ValueDecl *decl = expr->getDecl();
  if (const clang::VarDecl *var = variableOf(decl))
    noteVariableUse(var, expr, expr->isNonOdrUse(), expr->getLocation());
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
  if (unevaluated_ > 0)
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
  const uint32_t target = targetSlot(var);
  if (target == kDropped)
    return;
  const uint8_t marks = marksOf(expr);
  bool accessed = false;
  // Calling through a function pointer or functor reads it.
  if ((marks & (kMarkRead | kMarkCallee)) != 0) {
    addRef(target, RefRole::Read, loc);
    accessed = true;
  }
  if ((marks & kMarkWrite) != 0) {
    addRef(target, RefRole::Write, loc);
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
  const uint32_t target = targetSlot(fn);
  if (target == kDropped)
    return;
  addRef(target,
         (marksOf(expr) & kMarkCallee) != 0 ? RefRole::Call
                                            : RefRole::AddressOf,
         loc);
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
  if (const clang::Expr *thrown = expr->getSubExpr())
    noteTypeinfoUse(thrown->getType(), expr->getThrowLoc());
  return true;
}

bool ReferenceVisitor::VisitCXXCatchStmt(clang::CXXCatchStmt *stmt) {
  noteTypeinfoUse(stmt->getCaughtType(), stmt->getCatchLoc());
  return true;
}

void ReferenceVisitor::addRef(uint32_t target, RefRole role,
                              clang::SourceLocation site) {
  if (target == kDropped)
    return;
  const uint32_t referrer = referrerSlot(site);
  const uint64_t key = (static_cast<uint64_t>(referrer) << kReferrerShift) |
                       (static_cast<uint64_t>(target) << kTargetShift) |
                       static_cast<uint64_t>(role);
  RefAcc &acc = refs_[key];
  if (acc.count++ == 0)
    acc.site = site;
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
        if (llvm::isa<clang::CXXMethodDecl>(fn))
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
  entity.lambdaIdent = static_cast<uint32_t>(lambdaIdents_.size());
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
    const Ident &ident = lambdaIdents_[entity.lambdaIdent];
    const auto *op = llvm::cast<clang::CXXMethodDecl>(entity.decl);
    const Site site = siteOf(op->getParent()->getBeginLoc());
    rec.usr = index_.internText(ident.usr);
    rec.name = index_.internText(ident.display);
    rec.file = site.file;
    rec.line = site.line;
    rec.kind = SymbolKind::Function;
    rec.linkage = LinkageClass::Internal;
    rec.flags = kSymLambda | kSymDefinition;
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

void ReferenceVisitor::lower() {
  // Hash-map order is an implementation detail; slot order is the order the
  // code was read in.
  std::vector<std::pair<uint64_t, RefAcc>> refs;
  refs.reserve(refs_.size());
  for (const auto &entry : refs_)
    refs.emplace_back(entry.first, entry.second);
  llvm::sort(refs, [](const auto &a, const auto &b) {
    return a.first < b.first;
  });
  for (const auto &ref : refs) {
    entities_[ref.first >> kReferrerShift].emit = true;
    entities_[static_cast<uint32_t>(ref.first >> kTargetShift)].emit = true;
  }

  std::vector<Id> usrOf(entities_.size(), ReferenceIndex::kNoId);
  std::vector<ReferenceIndex::SymbolRecord> symbols;
  symbols.reserve(entities_.size() + entities_.size() / 2);
  for (size_t slot = 0; slot < entities_.size(); ++slot)
    if (entities_[slot].emit)
      usrOf[slot] = lowerEntity(entities_[slot], symbols);

  std::vector<ReferenceIndex::ReferenceRecord> records;
  records.reserve(refs.size());
  for (const auto &ref : refs) {
    const Site site = siteOf(ref.second.site);
    ReferenceIndex::ReferenceRecord rec;
    rec.referrer = usrOf[ref.first >> kReferrerShift];
    rec.target = usrOf[static_cast<uint32_t>(ref.first >> kTargetShift)];
    rec.file = site.file;
    rec.line = site.line;
    rec.col = site.col;
    rec.count = ref.second.count;
    rec.role = static_cast<RefRole>(ref.first & kRoleMask);
    assert(rec.referrer != ReferenceIndex::kNoId &&
           rec.target != ReferenceIndex::kNoId);
    records.push_back(rec);
  }
  index_.addTuFacts(index_.internText(tuPath_), symbols, records);
}

} // namespace vycor
