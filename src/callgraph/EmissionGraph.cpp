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

#include "vycor/callgraph/EmissionGraph.h"

#include "vycor/compat/ClangVersion.h"

#include "clang/AST/APValue.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/CXXInheritance.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclFriend.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/StmtCXX.h"
#include "clang/Basic/Builtins.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"

#include <utility>

namespace vycor {

bool hasTargetAttribute(const clang::FunctionDecl *fd) {
  return fd->hasAttr<clang::TargetAttr>() ||
         fd->hasAttr<clang::TargetClonesAttr>() ||
         fd->hasAttr<clang::TargetVersionAttr>() ||
         fd->hasAttr<clang::CPUSpecificAttr>() ||
         fd->hasAttr<clang::CPUDispatchAttr>();
}

namespace {

// Builds the EmissionGraph (EmissionGraph.h). Walking a body appends the
// nodes it references, so bodies are walked in node order and the edge list
// comes out grouped by source: edgeBegin_ is its CSR offset table.
class Walker {
public:
  explicit Walker(clang::ASTContext &ctx) : ctx_(ctx) {}

  EmissionGraph run() {
    // Node 0: the TU. Its edges are the roots.
    decl_.push_back(nullptr);
    vars_.push_back(nullptr);
    bits_.push_back(0);
    statements_.push_back(0);
    edgeBegin_.push_back(0);
    cur_ = 0;
    scan(ctx_.getTranslationUnitDecl());
    drain();
    for (uint32_t i = 1; i < decl_.size(); ++i) {
      edgeBegin_.push_back(static_cast<uint32_t>(edgeDst_.size()));
      cur_ = i;
      if (vars_[i])
        walkVariable(vars_[i]);
      else
        walkFunction(decl_[i]);
      drain();
    }
    edgeBegin_.push_back(static_cast<uint32_t>(edgeDst_.size()));
    EmissionGraph g;
    g.function = std::move(decl_);
    g.variable = std::move(vars_);
    g.bits = std::move(bits_);
    g.statements = std::move(statements_);
    g.edgeBegin = std::move(edgeBegin_);
    g.edgeDst = std::move(edgeDst_);
    g.edgeFlags = std::move(edgeBits_);
    return g;
  }

private:
  // ---- node table ------------------------------------------------------

  uint32_t nodeFor(const clang::FunctionDecl *fd) {
    const clang::FunctionDecl *canon = fd->getCanonicalDecl();
    auto id = static_cast<uint32_t>(decl_.size());
    auto [it, inserted] = nodeOf_.try_emplace(canon, id);
    if (!inserted)
      return it->second;
    const clang::FunctionDecl *def = nullptr;
    if (!(fd->hasBody(def) && def && def->getBody() &&
          !def->hasSkippedBody() && !def->isLateTemplateParsed() &&
          !def->isInvalidDecl() && !def->isDependentContext()))
      def = nullptr;
    decl_.push_back(def);
    vars_.push_back(nullptr);
    bits_.push_back(def && hasTargetAttribute(def) ? uint32_t(kBodyTargetAttr) : 0u);
    statements_.push_back(0);
    return id;
  }

  // A global variable's dynamic initialization and destruction run in the
  // TU's global initializer, not in the body that references it: the
  // variable is a node of its own (no body, never folded) whose walk adds
  // the functions they reference.
  void reachVariable(const clang::VarDecl *vd) {
    if (!vd->hasGlobalStorage() || vd->isStaticLocal() || vd->isTemplated())
      return;
    const clang::VarDecl *def = vd->getDefinition();
    if (!def || def->isInvalidDecl())
      return;
    auto id = static_cast<uint32_t>(decl_.size());
    auto [it, inserted] = varNode_.try_emplace(def->getCanonicalDecl(), id);
    if (inserted) {
      decl_.push_back(nullptr);
      vars_.push_back(def);
      bits_.push_back(0);
      statements_.push_back(0);
    }
    if (edgeDst_.size() > edgeBegin_.back() && edgeDst_.back() == it->second)
      return;
    edgeDst_.push_back(it->second);
    edgeBits_.push_back(0);
  }

  void walkVariable(const clang::VarDecl *vd) {
    // A constant initializer is emitted as data: nothing runs, nothing it
    // names (a constexpr constructor, say) is emitted for it, but what the
    // data holds the address of is.
    const clang::Expr *init = vd->getInit();
    if (init && (init->isValueDependent() || !vd->evaluateValue()))
      push(init);
    else if (init)
      constantValue(*vd->getEvaluatedValue(), vd->getType());
    destroy(vd->getType());
  }

  // Constant data, as CodeGen's constant emitter lowers it: a function
  // whose address the value holds is emitted (reached through the data,
  // so never inlined), a global whose address it holds is reached, and an
  // object of a dynamic class without a key function emits that vtable.
  void constantValue(const clang::APValue &v, clang::QualType type) {
    switch (v.getKind()) {
    case clang::APValue::LValue: {
      const clang::APValue::LValueBase base = v.getLValueBase();
      if (const auto *vd = base.dyn_cast<const clang::ValueDecl *>()) {
        if (const auto *fd = llvm::dyn_cast<clang::FunctionDecl>(vd)) {
          if (!fd->getBuiltinID())
            addEdge(fd, 0);
        } else if (const auto *var = llvm::dyn_cast<clang::VarDecl>(vd)) {
          reachVariable(var);
        }
      }
      return;
    }
    case clang::APValue::MemberPointer:
      // A pointer to a virtual member holds a vtable offset, no function.
      if (const auto *md = llvm::dyn_cast_or_null<clang::CXXMethodDecl>(
              v.getMemberPointerDecl()))
        if (!md->isVirtual())
          addEdge(md, 0);
      return;
    case clang::APValue::Struct: {
      const clang::RecordDecl *rd = type->getAsRecordDecl();
      if (!rd)
        return;
      if (const auto *crd = llvm::dyn_cast<clang::CXXRecordDecl>(rd)) {
        if (crd->isDynamicClass() && !ctx_.getCurrentKeyFunction(crd))
          vtableMembers(crd);
        unsigned i = 0;
        for (const clang::CXXBaseSpecifier &b : crd->bases())
          if (i < v.getStructNumBases())
            constantValue(v.getStructBase(i++), b.getType());
      }
      for (const clang::FieldDecl *f : rd->fields()) {
        const unsigned at = f->getFieldIndex();
        if (at < v.getStructNumFields() && mayHoldAddress(f->getType()))
          constantValue(v.getStructField(at), f->getType());
      }
      return;
    }
    case clang::APValue::Union:
      if (const clang::FieldDecl *f = v.getUnionField())
        constantValue(v.getUnionValue(), f->getType());
      return;
    case clang::APValue::Array: {
      const clang::ArrayType *at = ctx_.getAsArrayType(type);
      if (!at || !mayHoldAddress(at->getElementType()))
        return;
      const clang::QualType elem = at->getElementType();
      for (unsigned i = 0, n = v.getArrayInitializedElts(); i < n; ++i)
        constantValue(v.getArrayInitializedElt(i), elem);
      if (v.hasArrayFiller())
        constantValue(v.getArrayFiller(), elem);
      return;
    }
    default:
      return; // integers, floats, vectors, complex: no address
    }
  }

  // Whether a constant of `type` can hold an address or need a vtable:
  // a large table of integers is skipped without looking at its elements.
  bool mayHoldAddress(clang::QualType type) {
    const clang::Type *t = ctx_.getBaseElementType(type).getCanonicalType()
                               .getTypePtr();
    if (t->isPointerType() || t->isReferenceType() ||
        t->isMemberPointerType() || t->isNullPtrType())
      return true;
    const clang::CXXRecordDecl *rd = t->getAsCXXRecordDecl();
    if (!rd)
      return t->isRecordType(); // a C struct: look
    if (!rd->hasDefinition())
      return false;
    auto [it, inserted] = holdsAddress_.try_emplace(rd, false);
    if (!inserted)
      return it->second;
    bool holds = rd->isDynamicClass();
    for (const clang::CXXBaseSpecifier &b : rd->bases())
      holds = holds || mayHoldAddress(b.getType());
    for (const clang::FieldDecl *f : rd->fields())
      holds = holds || mayHoldAddress(f->getType());
    holdsAddress_[rd] = holds;
    return holds;
  }

  void addEdge(const clang::FunctionDecl *fd, uint8_t bits = kEmissionEdgeInlinable) {
    if (!fd)
      return;
    uint32_t dst = nodeFor(fd);
    if (edgeDst_.size() > edgeBegin_.back() && edgeDst_.back() == dst &&
        edgeBits_.back() == bits)
      return;
    edgeDst_.push_back(dst);
    edgeBits_.push_back(bits);
  }

  // ---- roots -------------------------------------------------------------

  static bool emittedUnconditionally(clang::GVALinkage gva) {
    return gva == clang::GVA_StrongExternal || gva == clang::GVA_StrongODR;
  }

  void rootFunction(const clang::FunctionDecl *fd) {
    if (!fd->doesThisDeclarationHaveABody() || fd->isLateTemplateParsed() ||
        fd->isDependentContext() || fd->isInvalidDecl())
      return;
    if (emittedUnconditionally(ctx_.GetGVALinkageForFunction(fd)) ||
        fd->hasAttr<clang::UsedAttr>() ||
        fd->hasAttr<clang::ConstructorAttr>() ||
        fd->hasAttr<clang::DestructorAttr>())
      addEdge(fd, 0);
  }

  // ASTContext::DeclMustBeEmitted for variables: a variable another TU
  // can need, or one whose initialization or destruction has side
  // effects. Any other is emitted only where emitted code references it
  // (reachVariable from the body walk).
  void rootVariable(const clang::VarDecl *vd) {
    if (!vd->hasGlobalStorage() || vd->isStaticLocal() ||
        vd->isTemplated() || vd->isInvalidDecl() ||
        vd->isThisDeclarationADefinition() != clang::VarDecl::Definition)
      return;
    const clang::GVALinkage gva = ctx_.GetGVALinkageForVariable(vd);
    if (gva == clang::GVA_AvailableExternally)
      return;
    const clang::Expr *init = vd->getInit();
    if (!clang::isDiscardableGVALinkage(gva) || vd->needsDestruction(ctx_) ||
        (init && init->HasSideEffects(ctx_) &&
         (init->isValueDependent() || !vd->evaluateValue())))
      reachVariable(vd);
  }

  // A dynamic class whose key function this TU defines emits its vtable
  // here, and with it every virtual function.
  void rootVtable(const clang::CXXRecordDecl *rd) {
    if (!rd->isDynamicClass())
      return;
    const clang::CXXMethodDecl *key = ctx_.getCurrentKeyFunction(rd);
    const clang::FunctionDecl *keyDef = nullptr;
    if (key && key->hasBody(keyDef) && keyDef && !keyDef->isInlined())
      vtableMembers(rd);
  }

  // Every slot of the class's vtables, its bases' included: the final
  // overrider of each virtual function. Reached through the vtable, so
  // never inlined into the referencing body.
  void vtableMembers(const clang::CXXRecordDecl *rd) {
    if (!vtablesDone_.insert(rd->getCanonicalDecl()).second)
      return;
    clang::CXXFinalOverriderMap overriders;
    rd->getFinalOverriders(overriders);
    for (const auto &entry : overriders)
      for (const auto &subobject : entry.second)
        for (const clang::UniqueVirtualMethod &m : subobject.second)
          if (m.Method && !m.Method->isPureVirtual())
            addEdge(m.Method, 0);
  }

  void scan(const clang::DeclContext *dc) {
    for (const clang::Decl *d : dc->decls()) {
      if (const auto *fd = llvm::dyn_cast<clang::FunctionDecl>(d)) {
        rootFunction(fd);
        continue;
      }
      switch (d->getKind()) {
      case clang::Decl::Namespace:
        scan(llvm::cast<clang::NamespaceDecl>(d));
        break;
      case clang::Decl::LinkageSpec:
        scan(llvm::cast<clang::LinkageSpecDecl>(d));
        break;
      case clang::Decl::Export:
        scan(llvm::cast<clang::ExportDecl>(d));
        break;
      case clang::Decl::Var:
        rootVariable(llvm::cast<clang::VarDecl>(d));
        break;
      case clang::Decl::CXXRecord:
      case clang::Decl::ClassTemplateSpecialization:
        scanRecord(llvm::cast<clang::CXXRecordDecl>(d));
        break;
      case clang::Decl::ClassTemplate: {
        // Explicit instantiation definitions emit their members here.
        const auto *ctd = llvm::cast<clang::ClassTemplateDecl>(d);
        if (!ctd->isFirstDecl())
          break;
        for (const auto *spec :
             const_cast<clang::ClassTemplateDecl *>(ctd)->specializations())
          if (spec->getSpecializationKind() ==
              clang::TSK_ExplicitInstantiationDefinition)
            scanRecord(spec);
        break;
      }
      case clang::Decl::VarTemplate: {
        // Instantiations are not members of any DeclContext; Clang hands
        // them to CodeGen, which keeps them as for any variable.
        const auto *vtd = llvm::cast<clang::VarTemplateDecl>(d);
        if (!vtd->isFirstDecl())
          break;
        for (const auto *spec :
             const_cast<clang::VarTemplateDecl *>(vtd)->specializations())
          rootVariable(spec);
        break;
      }
      case clang::Decl::FunctionTemplate: {
        const auto *ftd = llvm::cast<clang::FunctionTemplateDecl>(d);
        if (!ftd->isFirstDecl())
          break;
        for (const auto *spec :
             const_cast<clang::FunctionTemplateDecl *>(ftd)->specializations())
          if (spec->getTemplateSpecializationKind() ==
              clang::TSK_ExplicitInstantiationDefinition)
            if (const clang::FunctionDecl *def = spec->getDefinition())
              rootFunction(def);
        break;
      }
      case clang::Decl::Friend:
        if (const auto *nd = llvm::cast<clang::FriendDecl>(d)->getFriendDecl())
          if (const auto *fd = llvm::dyn_cast<clang::FunctionDecl>(nd))
            rootFunction(fd);
        break;
      default:
        break;
      }
    }
  }

  void scanRecord(const clang::CXXRecordDecl *rd) {
    if (!rd->isThisDeclarationADefinition() || rd->isDependentContext() ||
        rd->isLambda() || rd->isInvalidDecl())
      return;
    rootVtable(rd);
    scan(rd);
  }

  // ---- body walk -------------------------------------------------------

  void push(const clang::Stmt *s) {
    if (s)
      stack_.push_back(s);
  }

  // Any floating-point value, even one only loaded, passed or returned:
  // with -mavx every scalar FP move is VEX-encoded. FP flags change only
  // arithmetic, comparisons and conversions (kBodyFloat).
  void markFloat(clang::QualType type) {
    if (!type.isNull() && type->isFloatingType())
      bits_[cur_] |= kBodyFloatMove;
  }

  void destroy(clang::QualType type) {
    if (type.isNull())
      return;
    const clang::CXXRecordDecl *rd =
        ctx_.getBaseElementType(type)->getAsCXXRecordDecl();
    if (!rd || !rd->hasDefinition() || rd->hasTrivialDestructor())
      return;
    if (const clang::CXXDestructorDecl *dtor = rd->getDestructor())
      if (!dtor->isDeleted()) {
        addEdge(dtor);
        bits_[cur_] |= kBodyCleanup;
      }
  }

  void construct(const clang::CXXConstructorDecl *ctor) {
    addEdge(ctor);
    if (!ctor->isTrivial() && !nothrow(ctor))
      bits_[cur_] |= kBodyMayThrowCall;
  }

  static bool nothrow(const clang::FunctionDecl *fd) {
    const auto *fpt = fd->getType()->getAs<clang::FunctionProtoType>();
    return fpt && fpt->isNothrow();
  }

  void callee(const clang::CallExpr *call) {
    const clang::FunctionDecl *fd = call->getDirectCallee();
    if (!fd) {
      bits_[cur_] |= kBodyMayThrowCall; // indirect: anything can happen
      return;
    }
    if (unsigned id = fd->getBuiltinID()) {
      builtin(id, call);
      return;
    }
    if (!nothrow(fd))
      bits_[cur_] |= kBodyMayThrowCall;
  }

  void builtin(unsigned id, const clang::CallExpr *call) {
    if (ctx_.BuiltinInfo.isTSBuiltin(id)) {
      bits_[cur_] |= kBodyTargetBuiltin;
      return;
    }
    switch (id) {
    // Library functions Clang treats as builtins and CodeGen folds into
    // a cast: no code of their own, whatever their operand's type.
    case clang::Builtin::BImove:
    case clang::Builtin::BImove_if_noexcept:
    case clang::Builtin::BIforward:
    case clang::Builtin::BIforward_like:
    case clang::Builtin::BIas_const:
    case clang::Builtin::BIaddressof:
    case clang::Builtin::BI__addressof:
    case clang::Builtin::BI__builtin_addressof:
      return;
    case clang::Builtin::BI__builtin_popcount:
    case clang::Builtin::BI__builtin_popcountl:
    case clang::Builtin::BI__builtin_popcountll:
    case clang::Builtin::BI__builtin_clz:
    case clang::Builtin::BI__builtin_clzl:
    case clang::Builtin::BI__builtin_clzll:
    case clang::Builtin::BI__builtin_ctz:
    case clang::Builtin::BI__builtin_ctzl:
    case clang::Builtin::BI__builtin_ctzll:
    case clang::Builtin::BI__builtin_bswap16:
    case clang::Builtin::BI__builtin_bswap32:
    case clang::Builtin::BI__builtin_bswap64:
    case clang::Builtin::BI__builtin_rotateleft32:
    case clang::Builtin::BI__builtin_rotateleft64:
    case clang::Builtin::BI__builtin_rotateright32:
    case clang::Builtin::BI__builtin_rotateright64:
    case clang::Builtin::BI__builtin_rotateleft8:
    case clang::Builtin::BI__builtin_rotateleft16:
    case clang::Builtin::BI__builtin_rotateright8:
    case clang::Builtin::BI__builtin_rotateright16:
    case clang::Builtin::BI__builtin_ffs:
    case clang::Builtin::BI__builtin_ffsl:
    case clang::Builtin::BI__builtin_ffsll:
    case clang::Builtin::BI__builtin_parity:
    case clang::Builtin::BI__builtin_parityl:
    case clang::Builtin::BI__builtin_parityll:
    case clang::Builtin::BI__builtin_clrsb:
    case clang::Builtin::BI__builtin_clrsbl:
    case clang::Builtin::BI__builtin_clrsbll:
#if VYCOR_LLVM_AT_LEAST(19)
    case clang::Builtin::BI__builtin_popcountg:
    case clang::Builtin::BI__builtin_clzg:
    case clang::Builtin::BI__builtin_ctzg:
#endif
      bits_[cur_] |= kBodyBitBuiltin;
      return;
    case clang::Builtin::BImemcpy:
    case clang::Builtin::BImemmove:
    case clang::Builtin::BImemset:
    case clang::Builtin::BI__builtin_memcpy:
    case clang::Builtin::BI__builtin_memmove:
    case clang::Builtin::BI__builtin_memset:
      bits_[cur_] |= kBodyBulkMemory;
      return;
    default:
      break;
    }
    // A math builtin (sqrt, exp, floor, ...) on floating-point values,
    // vectors of floats included: -fno-math-errno and -fapprox-func change
    // how it is lowered. A classification builtin (isnan, fpclassify,
    // isgreater, ...) returns an int, but -ffinite-math-only folds it.
    // One CodeGen folds to a constant is a constant, and fabs/copysign
    // only move bits, whatever the FP flags.
    const bool floatResult = call->getType()->hasFloatingRepresentation();
    bool floatArgs = false;
    for (const clang::Expr *arg : call->arguments())
      floatArgs |= arg->getType()->hasFloatingRepresentation();
    if (!floatResult && !floatArgs)
      return;
    clang::Expr::EvalResult folded;
    if (!call->isValueDependent() && call->EvaluateAsRValue(folded, ctx_) &&
        !folded.HasSideEffects) {
      bits_[cur_] |= kBodyFloatMove;
      return;
    }
    if (signOrConstant(id)) {
      bits_[cur_] |= kBodyFloatMove;
      return;
    }
    bits_[cur_] |= kBodyFloat;
    if (floatResult)
      bits_[cur_] |= kBodyMathCall;
  }

  static bool signOrConstant(unsigned id) {
    switch (id) {
    case clang::Builtin::BIfabs:
    case clang::Builtin::BIfabsf:
    case clang::Builtin::BIfabsl:
    case clang::Builtin::BI__builtin_fabs:
    case clang::Builtin::BI__builtin_fabsf:
    case clang::Builtin::BI__builtin_fabsl:
    case clang::Builtin::BI__builtin_fabsf16:
    case clang::Builtin::BI__builtin_fabsf128:
    case clang::Builtin::BIcopysign:
    case clang::Builtin::BIcopysignf:
    case clang::Builtin::BIcopysignl:
    case clang::Builtin::BI__builtin_copysign:
    case clang::Builtin::BI__builtin_copysignf:
    case clang::Builtin::BI__builtin_copysignl:
    case clang::Builtin::BI__builtin_copysignf16:
    case clang::Builtin::BI__builtin_copysignf128:
    case clang::Builtin::BInan:
    case clang::Builtin::BInanf:
    case clang::Builtin::BInanl:
    case clang::Builtin::BI__builtin_nan:
    case clang::Builtin::BI__builtin_nanf:
    case clang::Builtin::BI__builtin_nanl:
    case clang::Builtin::BI__builtin_nanf128:
    case clang::Builtin::BI__builtin_inf:
    case clang::Builtin::BI__builtin_inff:
    case clang::Builtin::BI__builtin_infl:
    case clang::Builtin::BI__builtin_inff128:
    case clang::Builtin::BI__builtin_huge_val:
    case clang::Builtin::BI__builtin_huge_valf:
    case clang::Builtin::BI__builtin_huge_vall:
    case clang::Builtin::BI__builtin_huge_valf128:
      return true;
    default:
      return false;
    }
  }

  static bool floatCast(clang::CastKind kind) {
    switch (kind) {
    case clang::CK_IntegralToFloating:
    case clang::CK_FloatingToIntegral:
    case clang::CK_FloatingCast:
    case clang::CK_FloatingToBoolean:
    case clang::CK_FloatingRealToComplex:
    case clang::CK_FloatingComplexCast:
    case clang::CK_FloatingComplexToReal:
    case clang::CK_IntegralComplexToFloatingComplex:
    case clang::CK_FloatingComplexToIntegralComplex:
      return true;
    default:
      return false;
    }
  }

  static bool signedArithmetic(clang::BinaryOperatorKind op) {
    switch (op) {
    case clang::BO_Add:
    case clang::BO_Sub:
    case clang::BO_Mul:
    case clang::BO_Shl:
    case clang::BO_AddAssign:
    case clang::BO_SubAssign:
    case clang::BO_MulAssign:
    case clang::BO_ShlAssign:
      return true;
    default:
      return false;
    }
  }

  void walkFunction(const clang::FunctionDecl *def) {
    if (!def)
      return;
    // A constructor or destructor body stores the vtable pointer: a vtable
    // with no key function is emitted, with every function in its slots,
    // by each TU that emits such a body (not by each TU that constructs
    // an object: the constructor may be defined elsewhere).
    if (const auto *md = llvm::dyn_cast<clang::CXXMethodDecl>(def)) {
      const clang::CXXRecordDecl *rd = md->getParent();
      if ((llvm::isa<clang::CXXConstructorDecl>(md) ||
           (llvm::isa<clang::CXXDestructorDecl>(md) &&
            destructorStoresVptr(llvm::cast<clang::CXXDestructorDecl>(md)))) &&
          rd->isDynamicClass() && !ctx_.getCurrentKeyFunction(rd))
        vtableMembers(rd);
    }
    if (const auto *ctor = llvm::dyn_cast<clang::CXXConstructorDecl>(def))
      for (const clang::CXXCtorInitializer *init : ctor->inits())
        push(init->getInit());
    if (const auto *dtor = llvm::dyn_cast<clang::CXXDestructorDecl>(def)) {
      // Members and bases are destroyed after the body.
      const clang::CXXRecordDecl *rd = dtor->getParent();
      for (const clang::FieldDecl *field : rd->fields())
        destroy(field->getType());
      for (const clang::CXXBaseSpecifier &base : rd->bases())
        destroy(base.getType());
    }
    // A by-value parameter is destroyed by the callee only where Clang
    // says so (the Microsoft ABI, or a trivial_abi class); otherwise the
    // caller destroys it, which the caller's walk sees as a bound
    // temporary.
    for (const clang::ParmVarDecl *param : def->parameters()) {
      if (const clang::RecordDecl *rd = param->getType()->getAsRecordDecl())
        if (rd->isParamDestroyedInCallee())
          destroy(param->getType());
      if (param->getType()->isFloatingType())
        bits_[cur_] |= kBodyFloatMove;
    }
    if (def->getReturnType()->isFloatingType())
      bits_[cur_] |= kBodyFloatMove;
    push(def->getBody());
    drain();
  }

  // CodeGen's CanSkipVTablePointerInitialization, inverted: a destructor
  // resets the vtable pointer unless the class is effectively final or
  // the body and every field's destruction have trivial bodies.
  static bool destructorStoresVptr(const clang::CXXDestructorDecl *dtor) {
    const clang::CXXRecordDecl *rd = dtor->getParent();
    if (rd->isEffectivelyFinal())
      return false;
    if (!dtor->hasTrivialBody())
      return true;
    for (const clang::FieldDecl *field : rd->fields())
      if (!fieldHasTrivialDestructorBody(field))
        return true;
    return false;
  }

  // CGClass.cpp's FieldHasTrivialDestructorBody / HasTrivialDestructorBody.
  static bool fieldHasTrivialDestructorBody(const clang::FieldDecl *field) {
    const clang::CXXRecordDecl *frd =
        field->getType()->getBaseElementTypeUnsafe()->getAsCXXRecordDecl();
    if (!frd || !frd->hasDefinition())
      return true;
    // An anonymous union member's destructor is never invoked.
    if (frd->isUnion() && frd->isAnonymousStructOrUnion())
      return true;
    return hasTrivialDestructorBody(frd, frd);
  }

  static bool hasTrivialDestructorBody(const clang::CXXRecordDecl *rd,
                                       const clang::CXXRecordDecl *mostDerived) {
    if (!rd || !rd->hasDefinition() || rd->hasTrivialDestructor())
      return true;
    const clang::CXXDestructorDecl *dtor = rd->getDestructor();
    if (!dtor || !dtor->hasTrivialBody())
      return false;
    for (const clang::FieldDecl *field : rd->fields())
      if (!fieldHasTrivialDestructorBody(field))
        return false;
    for (const clang::CXXBaseSpecifier &base : rd->bases())
      if (!base.isVirtual() &&
          !hasTrivialDestructorBody(base.getType()->getAsCXXRecordDecl(),
                                    mostDerived))
        return false;
    if (rd == mostDerived)
      for (const clang::CXXBaseSpecifier &base : rd->vbases())
        if (!hasTrivialDestructorBody(base.getType()->getAsCXXRecordDecl(),
                                      mostDerived))
          return false;
    return true;
  }

  void drain() {
    while (!stack_.empty()) {
      const clang::Stmt *s = stack_.pop_back_val();
      visit(s);
    }
  }

  // The function a virtual call on `base` calls directly, or null when the
  // call goes through the vtable: CodeGen's block in
  // EmitCXXMemberOrOperatorMemberCallExpr (CGExprCXX.cpp). It asks
  // getDevirtualizedMethod, then calls the method of the best dynamic
  // class, falling back to the vtable for a covariant return or when that
  // class is neither the base's nor the inner expression's.
  static const clang::CXXMethodDecl *
  directTarget(const clang::CXXMethodDecl *md, const clang::Expr *base) {
    if (!md->getDevirtualizedMethod(base, /*IsAppleKext=*/false))
      return nullptr;
    const clang::CXXRecordDecl *best = base->getBestDynamicClassType();
    const clang::CXXMethodDecl *direct =
        best ? md->getCorrespondingMethodInClass(best) : nullptr;
    if (!direct || direct->getReturnType().getCanonicalType() !=
                       md->getReturnType().getCanonicalType())
      return nullptr;
    const clang::CXXRecordDecl *cls = direct->getParent();
    if (clang::declaresSameEntity(recordOf(base->IgnoreParenBaseCasts()), cls) ||
        clang::declaresSameEntity(recordOf(base), cls))
      return direct;
    return nullptr;
  }

  // CGExprCXX.cpp's getCXXRecord: the class of an object or pointer.
  static const clang::CXXRecordDecl *recordOf(const clang::Expr *e) {
    clang::QualType t = e->getType();
    if (const auto *ptr = t->getAs<clang::PointerType>())
      t = ptr->getPointeeType();
    return t->getAsCXXRecordDecl();
  }

  // An if/?:/&&/|| condition Clang folds without side effects: CodeGen
  // emits only the branch that runs (unless the other holds a label).
  bool foldedCondition(const clang::Expr *cond, bool &value) const {
    clang::Expr::EvalResult result;
    if (!cond || cond->isValueDependent() ||
        !cond->EvaluateAsInt(result, ctx_))
      return false;
    value = result.Val.getInt().getBoolValue();
    return true;
  }

  // CodeGenFunction::ContainsLabel: a label, or a case/default label of a
  // switch outside `s`, makes the dead branch reachable, so CodeGen
  // emits it.
  bool containsLabel(const clang::Stmt *s) {
    labelWork_.clear();
    if (s)
      labelWork_.push_back({s, false});
    while (!labelWork_.empty()) {
      auto [cur, ignoreCases] = labelWork_.pop_back_val();
      if (llvm::isa<clang::LabelStmt>(cur) ||
          (!ignoreCases && llvm::isa<clang::SwitchCase>(cur)))
        return true;
      if (llvm::isa<clang::SwitchStmt>(cur))
        ignoreCases = true;
      for (const clang::Stmt *child : cur->children())
        if (child)
          labelWork_.push_back({child, ignoreCases});
    }
    return false;
  }

  bool constantFalse(const clang::Expr *cond) const {
    bool value = true;
    return foldedCondition(cond, value) && !value;
  }

  void visit(const clang::Stmt *s) {
    ++statements_[cur_];
    switch (s->getStmtClass()) {
    case clang::Stmt::ForStmtClass:
      if (!constantFalse(llvm::cast<clang::ForStmt>(s)->getCond()))
        bits_[cur_] |= kBodyLoop;
      break;
    case clang::Stmt::WhileStmtClass:
      if (!constantFalse(llvm::cast<clang::WhileStmt>(s)->getCond()))
        bits_[cur_] |= kBodyLoop;
      break;
    case clang::Stmt::DoStmtClass:
      // do { ... } while (0): the statement-macro idiom, not a loop.
      if (!constantFalse(llvm::cast<clang::DoStmt>(s)->getCond()))
        bits_[cur_] |= kBodyLoop;
      break;
    case clang::Stmt::CXXForRangeStmtClass:
      bits_[cur_] |= kBodyLoop;
      break;
    case clang::Stmt::CXXTryStmtClass:
      bits_[cur_] |= kBodyTry;
      break;
    case clang::Stmt::DeclStmtClass:
      for (const clang::Decl *d : llvm::cast<clang::DeclStmt>(s)->decls())
        if (const auto *v = llvm::dyn_cast<clang::VarDecl>(d)) {
          if (v->hasLocalStorage())
            destroy(v->getType());
          else if (v->isStaticLocal() && v->needsDestruction(ctx_))
            if (const clang::CXXRecordDecl *rd =
                    ctx_.getBaseElementType(v->getType())->getAsCXXRecordDecl())
              addEdge(rd->getDestructor(), 0); // registered with atexit
          // A constexpr local's initializer, and a static local's constant
          // one, are evaluated by the compiler: data, not code.
          const clang::Expr *init = v->getInit();
          if (!init)
            continue;
          if ((v->isConstexpr() || v->isStaticLocal()) &&
              !init->isValueDependent())
            if (const clang::APValue *value = v->evaluateValue()) {
              constantValue(*value, v->getType());
              continue;
            }
          if (!v->isConstexpr())
            push(init);
        }
      return;
    case clang::Stmt::IfStmtClass: {
      const auto *ifs = llvm::cast<clang::IfStmt>(s);
      if (ifs->isConsteval()) {
        // if consteval: only the runtime branch is code.
        push(ifs->isNegatedConsteval() ? ifs->getThen() : ifs->getElse());
        return;
      }
      if (auto taken = ifs->getNondiscardedCase(ctx_)) {
        // if constexpr: the discarded branch is never emitted.
        push(ifs->getInit());
        push(*taken);
        return;
      }
      bool value = false;
      if (!ifs->getConditionVariable() &&
          foldedCondition(ifs->getCond(), value) &&
          !containsLabel(value ? ifs->getElse() : ifs->getThen())) {
        push(ifs->getInit());
        push(value ? ifs->getThen() : ifs->getElse());
        return;
      }
      break;
    }
    case clang::Stmt::ConditionalOperatorClass: {
      // CodeGen folds a constant ?: on its scalar and lvalue paths only;
      // a class, array or _Complex prvalue emits both arms.
      const auto *co = llvm::cast<clang::ConditionalOperator>(s);
      clang::QualType type = co->getType();
      const bool foldable =
          co->isGLValue() ||
          (!type.isNull() &&
           ((type->isScalarType() && !type->isAnyComplexType()) ||
            type->isVectorType() || type->isVoidType()));
      bool value = false;
      if (foldable && foldedCondition(co->getCond(), value) &&
          !containsLabel(value ? co->getFalseExpr() : co->getTrueExpr())) {
        markFloat(type);
        push(value ? co->getTrueExpr() : co->getFalseExpr());
        return;
      }
      break;
    }
    case clang::Stmt::BinaryOperatorClass: {
      const auto *bo = llvm::cast<clang::BinaryOperator>(s);
      bool value = false;
      if ((bo->getOpcode() == clang::BO_LAnd ||
           bo->getOpcode() == clang::BO_LOr) &&
          foldedCondition(bo->getLHS(), value)) {
        // false && x and true || x never evaluate x (unless x holds a
        // label, which only a statement expression can).
        const bool rhsRuns = value == (bo->getOpcode() == clang::BO_LAnd);
        if (rhsRuns || !containsLabel(bo->getRHS())) {
          if (rhsRuns)
            push(bo->getRHS());
          return;
        }
        break;
      }
      break;
    }
    // Unevaluated operands and values the compiler computed: no code.
    case clang::Stmt::UnaryExprOrTypeTraitExprClass:
      if (llvm::cast<clang::UnaryExprOrTypeTraitExpr>(s)
              ->getTypeOfArgument()
              ->isVariablyModifiedType())
        break; // sizeof a VLA is evaluated
      return;
    case clang::Stmt::CXXNoexceptExprClass:
    case clang::Stmt::RequiresExprClass:
      return;
    case clang::Stmt::CXXTypeidExprClass:
      if (!llvm::cast<clang::CXXTypeidExpr>(s)->isPotentiallyEvaluated())
        return;
      break;
    case clang::Stmt::ConstantExprClass: {
      const auto *ce = llvm::cast<clang::ConstantExpr>(s);
      if (ce->hasAPValueResult()) {
        constantValue(ce->getAPValueResult(), ce->getType());
        return;
      }
      break;
    }
    case clang::Stmt::GenericSelectionExprClass: {
      const auto *gse = llvm::cast<clang::GenericSelectionExpr>(s);
      if (!gse->isResultDependent())
        push(gse->getResultExpr());
      return;
    }
    case clang::Stmt::LambdaExprClass: {
      // The closure's call operator is its own function, inlined where it
      // is called; only the captures are evaluated here.
      const auto *lambda = llvm::cast<clang::LambdaExpr>(s);
      addEdge(lambda->getCallOperator());
      for (const clang::Expr *init : lambda->capture_inits())
        push(init);
      return;
    }
    case clang::Stmt::CXXDefaultArgExprClass:
      push(llvm::cast<clang::CXXDefaultArgExpr>(s)->getExpr());
      return;
    case clang::Stmt::CXXDefaultInitExprClass:
      push(llvm::cast<clang::CXXDefaultInitExpr>(s)->getExpr());
      return;
    default:
      break;
    }

    if (const auto *e = llvm::dyn_cast<clang::Expr>(s)) {
      clang::QualType type = e->getType();
      if (!type.isNull() &&
          (type->isVectorType() || type->isSizelessBuiltinType()))
        bits_[cur_] |= kBodyVector;
      markFloat(type);
      if (const auto *bo = llvm::dyn_cast<clang::BinaryOperator>(e)) {
        clang::QualType operand = bo->getLHS()->getType();
        if (operand->hasFloatingRepresentation() ||
            bo->getRHS()->getType()->hasFloatingRepresentation()) {
          if (bo->getOpcode() != clang::BO_Assign &&
              bo->getOpcode() != clang::BO_Comma)
            bits_[cur_] |= kBodyFloat;
        }
        else if (signedArithmetic(bo->getOpcode()) &&
                 type->isSignedIntegerType())
          bits_[cur_] |= kBodySignedArith;
      } else if (const auto *uo = llvm::dyn_cast<clang::UnaryOperator>(e)) {
        clang::QualType operand = uo->getSubExpr()->getType();
        if (uo->isArithmeticOp() || uo->isIncrementDecrementOp()) {
          if (operand->hasFloatingRepresentation())
            bits_[cur_] |= kBodyFloat;
          else if (operand->isSignedIntegerType() &&
                   uo->getOpcode() != clang::UO_Plus &&
                   uo->getOpcode() != clang::UO_Not &&
                   uo->getOpcode() != clang::UO_LNot)
            bits_[cur_] |= kBodySignedArith;
        }
      } else if (const auto *cast = llvm::dyn_cast<clang::CastExpr>(e)) {
        if (floatCast(cast->getCastKind()) ||
            (cast->getCastKind() == clang::CK_VectorSplat &&
             type->hasFloatingRepresentation()))
          bits_[cur_] |= kBodyFloat;
      } else if (llvm::isa<clang::ConvertVectorExpr>(e)) {
        // __builtin_convertvector: a conversion, not a CastExpr.
        const auto *cv = llvm::cast<clang::ConvertVectorExpr>(e);
        if (type->hasFloatingRepresentation() ||
            cv->getSrcExpr()->getType()->hasFloatingRepresentation())
          bits_[cur_] |= kBodyFloat;
      } else if (const auto *call = llvm::dyn_cast<clang::CallExpr>(e)) {
        callee(call);
        // A virtual call goes through the vtable unless Clang can call the
        // function directly: through the vtable it neither emits the callee
        // here nor lets this body inline it. Walk the object and the
        // arguments, and add the direct target, if any, instead of the
        // named member.
        if (const auto *mc = llvm::dyn_cast<clang::CXXMemberCallExpr>(call)) {
          if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(
                  mc->getCallee()->IgnoreParens()))
            if (const auto *md = llvm::dyn_cast<clang::CXXMethodDecl>(
                    member->getMemberDecl()))
              if (md->isVirtual() && !member->hasQualifier()) {
                const clang::CXXMethodDecl *direct =
                    directTarget(md, member->getBase());
                if (direct != md) {
                  addEdge(direct);
                  push(member->getBase());
                  for (const clang::Expr *arg : mc->arguments())
                    push(arg);
                  return;
                }
              }
        } else if (const auto *oc =
                       llvm::dyn_cast<clang::CXXOperatorCallExpr>(call)) {
          if (const auto *md = llvm::dyn_cast_or_null<clang::CXXMethodDecl>(
                  oc->getDirectCallee()))
            if (md->isVirtual() && oc->getNumArgs() > 0) {
              const clang::CXXMethodDecl *direct =
                  directTarget(md, oc->getArg(0));
              if (direct != md) {
                addEdge(direct);
                for (const clang::Expr *arg : oc->arguments())
                  push(arg);
                return;
              }
            }
        }
      } else if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(e)) {
        if (const auto *fd = llvm::dyn_cast<clang::FunctionDecl>(ref->getDecl())) {
          if (!fd->getBuiltinID())
            addEdge(fd);
        } else if (const auto *vd = llvm::dyn_cast<clang::VarDecl>(ref->getDecl())) {
          reachVariable(vd);
        }
      } else if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(e)) {
        if (const auto *md =
                llvm::dyn_cast<clang::CXXMethodDecl>(member->getMemberDecl()))
          addEdge(md);
        else if (const auto *vd =
                     llvm::dyn_cast<clang::VarDecl>(member->getMemberDecl()))
          reachVariable(vd); // a static data member named through an object
      } else if (const auto *ctor = llvm::dyn_cast<clang::CXXConstructExpr>(e)) {
        construct(ctor->getConstructor());
      } else if (const auto *bind =
                     llvm::dyn_cast<clang::CXXBindTemporaryExpr>(e)) {
        if (const clang::CXXDestructorDecl *dtor =
                bind->getTemporary()->getDestructor()) {
          addEdge(dtor);
          bits_[cur_] |= kBodyCleanup;
        }
      } else if (const auto *newExpr = llvm::dyn_cast<clang::CXXNewExpr>(e)) {
        addEdge(newExpr->getOperatorNew());
        if (!newExpr->shouldNullCheckAllocation())
          bits_[cur_] |= kBodyMayThrowCall;
      } else if (const auto *del = llvm::dyn_cast<clang::CXXDeleteExpr>(e)) {
        // A virtual destructor runs through the vtable (the deleting
        // destructor, which calls the class's operator delete itself)
        // unless Clang calls it directly: EmitObjectDelete devirtualizes
        // only to the class of the deleted expression.
        const clang::CXXRecordDecl *rd =
            del->getDestroyedType().isNull()
                ? nullptr
                : ctx_.getBaseElementType(del->getDestroyedType())
                      ->getAsCXXRecordDecl();
        const clang::CXXDestructorDecl *dtor =
            rd && rd->hasDefinition() ? rd->getDestructor() : nullptr;
        if (dtor && dtor->isVirtual() && !del->isArrayForm()) {
          const clang::CXXMethodDecl *direct = dtor->getDevirtualizedMethod(
              del->getArgument(), /*IsAppleKext=*/false);
          if (direct && clang::declaresSameEntity(
                            recordOf(del->getArgument()), direct->getParent())) {
            addEdge(direct);
            addEdge(del->getOperatorDelete());
            bits_[cur_] |= kBodyCleanup;
          } else if (del->isGlobalDelete()) {
            addEdge(del->getOperatorDelete()); // ::delete calls it here
          }
        } else {
          addEdge(del->getOperatorDelete());
          destroy(del->getDestroyedType());
        }
      } else if (llvm::isa<clang::CXXThrowExpr>(e)) {
        bits_[cur_] |= kBodyMayThrowCall;
      }
    }
    for (const clang::Stmt *child : s->children())
      push(child);
  }

  clang::ASTContext &ctx_;

  // Node table (struct of arrays), one slot per function this TU reaches.
  llvm::DenseMap<const clang::FunctionDecl *, uint32_t> nodeOf_;
  std::vector<const clang::FunctionDecl *> decl_; // the definition, if any
  std::vector<const clang::VarDecl *> vars_; // set for a variable's node
  llvm::DenseMap<const clang::VarDecl *, uint32_t> varNode_;
  llvm::DenseSet<const clang::CXXRecordDecl *> vtablesDone_;
  llvm::DenseMap<const clang::CXXRecordDecl *, bool> holdsAddress_;
  std::vector<uint32_t> bits_;
  std::vector<uint32_t> statements_;
  // containsLabel's scratch: (statement, inside a nested switch).
  llvm::SmallVector<std::pair<const clang::Stmt *, bool>, 32> labelWork_;
  std::vector<uint32_t> edgeBegin_;
  std::vector<uint32_t> edgeDst_;
  std::vector<uint8_t> edgeBits_;

  llvm::SmallVector<const clang::Stmt *, 128> stack_;
  uint32_t cur_ = 0;
};

} // namespace

EmissionGraph buildEmissionGraph(clang::ASTContext &ctx) {
  return Walker(ctx).run();
}

} // namespace vycor
