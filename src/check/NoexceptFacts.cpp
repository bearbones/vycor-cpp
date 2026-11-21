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

#include "vycor/check/NoexceptFacts.h"

#include "vycor/check/GlobalIndex.h"
#include "vycor/check/Indexer.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclFriend.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/ExprObjC.h"
#include "clang/AST/GlobalDecl.h"
#include "clang/AST/Mangle.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/StmtCXX.h"
#include "clang/AST/StmtObjC.h"
#include "clang/AST/StmtOpenMP.h"
#include "clang/Basic/ABI.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/ExceptionSpecificationType.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Index/USRGeneration.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vycor {
namespace {

enum NodeKind : uint8_t {
  // A definition written in project code: becomes a fact, and callers
  // refer to it by its identity key (keyFor) so the proof can cross TUs
  // (and close cycles).
  kExported,
  // A body visible here that is not a definition of its own: a template
  // instantiation, an implicit or defaulted member, a lambda, a function
  // local class's member, an inline function of a system header. Walked
  // in this TU and folded into whatever reaches it.
  kLocal,
  // No body in this TU. Callers carry it as an obligation, by key.
  kExternal,
  // Neither a body nor a key: nothing can ever be known about it.
  kOpaque,
};

constexpr uint8_t kMayThrow = 1;
constexpr uint8_t kExit = NoexceptFactEntry::Exit;
constexpr uint8_t kCall = NoexceptFactEntry::Call;
constexpr uint8_t kElsewhere = NoexceptFactEntry::Elsewhere;

// True when the function's own type promises no exception leaves it. An
// exception specification Sema never had to resolve (an unused defaulted
// member, an uninstantiated template's) answers "no": the body, if there
// is one, decides instead.
bool cannotThrowByDecl(const clang::FunctionDecl *fd) {
  if (fd->hasAttr<clang::NoThrowAttr>())
    return true;
  const auto *proto = fd->getType()->getAs<clang::FunctionProtoType>();
  if (!proto)
    return false;
  if (clang::isUnresolvedExceptionSpec(proto->getExceptionSpecType()))
    return false;
  return proto->isNothrow();
}

// What every override of a virtual function is bound by: only a written
// exception specification. The nothrow attribute (which Clang records in
// the type as EST_NoThrow) does not carry over to overriders, and in MSVC
// compatibility mode Clang accepts an override looser than the function
// it overrides.
bool overridersCannotThrow(const clang::CXXMethodDecl *md,
                           const clang::LangOptions &lang) {
  if (lang.MSVCCompat)
    return false;
  const auto *proto = md->getType()->getAs<clang::FunctionProtoType>();
  if (!proto)
    return false;
  const clang::ExceptionSpecificationType est = proto->getExceptionSpecType();
  return est != clang::EST_NoThrow && !clang::isUnresolvedExceptionSpec(est) &&
         proto->isNothrow();
}

class Collector {
public:
  explicit Collector(clang::ASTContext &ctx)
      : ctx_(ctx), sm_(ctx.getSourceManager()),
        mangle_(ctx.createMangleContext()) {}

  void run(GlobalIndex &index) {
    scan(ctx_.getTranslationUnitDecl());
    // Walking a body appends the nodes it calls, so this loop also drains
    // them. Bodies are walked in node order, which leaves the edge list
    // grouped by source: edgeBegin_ is its CSR offset table for free.
    for (uint32_t i = 0; i < decl_.size(); ++i) {
      edgeBegin_.push_back(static_cast<uint32_t>(edgeDst_.size()));
      // A local body already written off (a weak definition, say) has
      // nothing to add; an exported one still names its callees.
      if (kind_[i] == kExported ||
          (kind_[i] == kLocal && !(state_[i] & kMayThrow)))
        walk(i);
    }
    edgeBegin_.push_back(static_cast<uint32_t>(edgeDst_.size()));
    propagate();
    exportFacts(index);
  }

private:
  // ---- node table ------------------------------------------------------

  std::string_view usrOf(uint32_t n) const {
    return std::string_view(usrPool_).substr(usrOff_[n], usrLen_[n]);
  }

  bool inSystemHeader(const clang::FunctionDecl *def) const {
    clang::SourceLocation loc = sm_.getExpansionLoc(def->getLocation());
    return loc.isInvalid() || sm_.isInSystemHeader(loc) ||
           !sm_.getFileEntryRefForID(sm_.getFileID(loc));
  }

  // A definition that gets a fact of its own. Everything else with a body
  // is folded (kLocal).
  bool exportable(const clang::FunctionDecl *def) const {
    if (def->isImplicit() || def->isInvalidDecl() || def->isDependentContext())
      return false;
    auto tsk = def->getTemplateSpecializationKind();
    if (tsk == clang::TSK_ImplicitInstantiation ||
        tsk == clang::TSK_ExplicitInstantiationDeclaration)
      return false;
    // Defaulted on its first declaration: the compiler writes the body and
    // derives the exception specification from the members.
    if (def->getCanonicalDecl()->isDefaulted())
      return false;
    if (def->getParentFunctionOrMethod())
      return false;
    if (const auto *md = llvm::dyn_cast<clang::CXXMethodDecl>(def))
      if (md->getParent()->isLambda())
        return false;
    return !inSystemHeader(def);
  }

  // The name a function is found by across TUs. For one the linker sees,
  // that is its linkage name: a USR merges overloads the linker keeps
  // apart (it does not spell a member pointer's class or a noexcept inside
  // a parameter type) and ignores asm labels, and merging two functions
  // would let one's proof stand for the other. Splitting can only leave a
  // callee unknown. An internal function keeps its USR, which carries the
  // file; its mangled name may be the same in every TU.
  bool keyFor(const clang::FunctionDecl *canon,
              llvm::SmallVectorImpl<char> &key) const {
    if (!canon->isExternallyVisible())
      return !clang::index::generateUSRForDecl(canon, key) && !key.empty();
    // The last declaration carries every attribute the earlier ones had,
    // and an asm label written on a redeclaration: the whole TU has been
    // parsed by now.
    const clang::FunctionDecl *last = canon->getMostRecentDecl();
    llvm::raw_svector_ostream os(key);
    os << "m:";
    if (!mangle_->shouldMangleDeclName(last)) {
      if (!last->getIdentifier())
        return false;
      os << last->getName(); // C, extern "C"
      return true;
    }
    if (const auto *ctor = llvm::dyn_cast<clang::CXXConstructorDecl>(last))
      mangle_->mangleName(clang::GlobalDecl(ctor, clang::Ctor_Complete), os);
    else if (const auto *dtor = llvm::dyn_cast<clang::CXXDestructorDecl>(last))
      mangle_->mangleName(clang::GlobalDecl(dtor, clang::Dtor_Complete), os);
    else
      mangle_->mangleName(clang::GlobalDecl(last), os);
    return key.size() > 2;
  }

  uint32_t nodeFor(const clang::FunctionDecl *fd) {
    const clang::FunctionDecl *canon = fd->getCanonicalDecl();
    auto id = static_cast<uint32_t>(decl_.size());
    auto [it, inserted] = nodeOf_.try_emplace(canon, id);
    if (!inserted)
      return it->second;

    const clang::FunctionDecl *def = nullptr;
    uint8_t kind = kExternal;
    uint8_t state = 0;
    if (fd->hasBody(def) && def && !def->hasSkippedBody() &&
        !def->isLateTemplateParsed() && def->getBody()) {
      kind = exportable(def) ? kExported : kLocal;
      // Bodies that do not say what runs: a weak definition the linker
      // may replace, a gnu_inline one or a C99 inline one whose external
      // definition is written elsewhere, the closure's static invoker
      // (Sema gives it an empty placeholder body), anything Sema rejected.
      const auto *md = llvm::dyn_cast<clang::CXXMethodDecl>(def);
      if (def->isWeak() || def->getMostRecentDecl()->isWeak() ||
          def->hasAttr<clang::GNUInlineAttr>() ||
          (!ctx_.getLangOpts().CPlusPlus && def->isInlined() &&
           !def->isInlineDefinitionExternallyVisible()) ||
          def->isInvalidDecl() || def->isDependentContext() ||
          (md && md->isLambdaStaticInvoker()))
        state |= kMayThrow;
    } else {
      def = canon;
    }

    uint32_t off = static_cast<uint32_t>(usrPool_.size());
    uint32_t len = 0;
    if (kind != kLocal) {
      llvm::SmallString<128> usr;
      if (!keyFor(canon, usr)) {
        // No stable name: an exported definition is folded instead, an
        // undefined callee can never be resolved.
        if (kind == kExported) {
          kind = kLocal;
        } else {
          kind = kOpaque;
          state |= kMayThrow;
        }
      } else {
        usrPool_.append(usr.data(), usr.size());
        len = static_cast<uint32_t>(usr.size());
      }
    }
    decl_.push_back(def);
    kind_.push_back(kind);
    state_.push_back(state);
    usrOff_.push_back(off);
    usrLen_.push_back(len);
    return id;
  }

  // ---- definitions written in this TU ----------------------------------

  void consider(const clang::FunctionDecl *fd) {
    if (fd->doesThisDeclarationHaveABody() && !fd->isLateTemplateParsed() &&
        exportable(fd))
      nodeFor(fd);
  }

  // Lexical walk of the declaration contexts that can hold a definition
  // exportable() accepts. Function bodies are not entered: nothing local
  // to a function is exported, and local code is reached through calls.
  void scan(const clang::DeclContext *dc) {
    for (const clang::Decl *d : dc->decls()) {
      if (const auto *fd = llvm::dyn_cast<clang::FunctionDecl>(d)) {
        consider(fd);
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
      case clang::Decl::CXXRecord:
      case clang::Decl::ClassTemplateSpecialization:
        scanRecord(llvm::cast<clang::CXXRecordDecl>(d));
        break;
      case clang::Decl::ClassTemplate: {
        // An explicit instantiation definition emits every member here:
        // the one TU a caller that only saw `extern template` can get
        // those members' facts from.
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
      case clang::Decl::FunctionTemplate: {
        const auto *ftd = llvm::cast<clang::FunctionTemplateDecl>(d);
        if (!ftd->isFirstDecl())
          break;
        for (const auto *spec :
             const_cast<clang::FunctionTemplateDecl *>(ftd)->specializations())
          if (spec->getTemplateSpecializationKind() ==
              clang::TSK_ExplicitInstantiationDefinition)
            if (const clang::FunctionDecl *def = spec->getDefinition())
              consider(def);
        break;
      }
      case clang::Decl::Friend:
        if (const auto *nd = llvm::cast<clang::FriendDecl>(d)->getFriendDecl())
          if (const auto *fd = llvm::dyn_cast<clang::FunctionDecl>(nd))
            consider(fd);
        break;
      default:
        break;
      }
    }
  }

  void scanRecord(const clang::CXXRecordDecl *rd) {
    if (rd->isThisDeclarationADefinition() && !rd->isDependentContext() &&
        !rd->isLambda() && !rd->isInvalidDecl())
      scan(rd);
  }

  // ---- body walk -------------------------------------------------------

  void push(const clang::Stmt *s, bool guarded) {
    if (s)
      stack_.push_back({s, guarded});
  }

  void pushChildren(const clang::Stmt *s, bool guarded) {
    for (const clang::Stmt *child : s->children())
      push(child, guarded);
  }

  // An expression shared between several parents (a default argument, a
  // default member initializer, an opaque value's source). Walked once
  // per guard level, which also ends a default member initializer that
  // names its own class.
  void pushShared(const clang::Stmt *s, bool guarded) {
    if (s && shared_[guarded].insert(s).second)
      stack_.push_back({s, guarded});
  }

  // Something here can throw and nothing statically bounds it.
  void opaque(bool guarded) {
    if (!guarded)
      state_[cur_] |= kMayThrow;
  }

  void addEdge(uint32_t dst, bool guarded) {
    // Consecutive duplicates (a loop of calls to one function) are the
    // common repetition; the rest is merged when the fact is built.
    uint8_t bits = guarded ? kCall : (kCall | kExit);
    if (edgeDst_.size() > edgeBegin_.back() && edgeDst_.back() == dst &&
        edgeBits_.back() == bits)
      return;
    edgeDst_.push_back(dst);
    edgeBits_.push_back(bits);
  }

  void callee(const clang::FunctionDecl *fd, bool guarded) {
    if (!fd) {
      opaque(guarded);
      return;
    }
    // A trivial special member runs no code at all.
    if (fd->isTrivial() || cannotThrowByDecl(fd))
      return;
    addEdge(nodeFor(fd), guarded);
  }

  // The destructor that runs for an object of type `type` (an array's
  // elements included). Trivial and deleted destructors run nothing.
  void destroy(clang::QualType type, bool guarded) {
    if (type.isNull())
      return;
    const clang::CXXRecordDecl *rd =
        ctx_.getBaseElementType(type)->getAsCXXRecordDecl();
    if (!rd)
      return;
    if (!rd->hasDefinition()) {
      opaque(guarded);
      return;
    }
    if (rd->hasTrivialDestructor())
      return;
    const clang::CXXDestructorDecl *dtor = rd->getDestructor();
    if (!dtor) {
      opaque(guarded);
      return;
    }
    if (!dtor->isDeleted())
      callee(dtor, guarded);
  }

  void localDecl(const clang::Decl *d, bool guarded) {
    if (const auto *v = llvm::dyn_cast<clang::VarDecl>(d)) {
      if (v->getType()->isVariablyModifiedType())
        opaque(guarded); // the bound is an expression the AST hides here
      // A constexpr variable's initializer was evaluated at compile time;
      // an evaluation that reaches a throw is not a constant expression.
      if (!v->isConstexpr())
        push(v->getInit(), guarded);
      // Static and thread locals are destroyed at exit, not here.
      if (v->hasLocalStorage())
        destroy(v->getType(), guarded);
      if (const auto *cleanup = v->getAttr<clang::CleanupAttr>())
        callee(cleanup->getFunctionDecl(), guarded); // runs at scope exit
      if (const auto *dd = llvm::dyn_cast<clang::DecompositionDecl>(v))
        for (const clang::BindingDecl *b : dd->bindings())
          if (const clang::VarDecl *holding = b->getHoldingVar()) {
            push(holding->getInit(), guarded);
            destroy(holding->getType(), guarded);
          }
      return;
    }
    if (const auto *t = llvm::dyn_cast<clang::TypedefNameDecl>(d))
      if (t->getUnderlyingType()->isVariablyModifiedType())
        opaque(guarded);
  }

  // A call with no statically known callee cannot throw only if the
  // callee's type says so: a noexcept function pointer or member pointer
  // can hold nothing else.
  bool indirectCallCannotThrow(const clang::CallExpr *ce) const {
    // Before C++17 noexcept is not part of a function type: a pointer
    // declared noexcept can hold a function that throws.
    if (!ctx_.getLangOpts().CPlusPlus17)
      return false;
    const clang::Expr *fn = ce->getCallee();
    clang::QualType t = fn->getType();
    if (t.isNull())
      return false;
    if (t->isSpecificBuiltinType(clang::BuiltinType::BoundMember))
      t = clang::Expr::findBoundMemberType(fn);
    else if (const auto *pt = t->getAs<clang::PointerType>())
      t = pt->getPointeeType();
    else if (const auto *rt = t->getAs<clang::ReferenceType>())
      t = rt->getPointeeType();
    else
      return false; // a block, or something this does not model
    if (t.isNull())
      return false;
    const auto *proto = t->getAs<clang::FunctionProtoType>();
    return proto &&
           !clang::isUnresolvedExceptionSpec(proto->getExceptionSpecType()) &&
           proto->isNothrow();
  }

  void call(const clang::CallExpr *ce, bool guarded) {
    const clang::FunctionDecl *fd = ce->getDirectCallee();
    if (!fd) {
      // `p->~T()` on a scalar runs nothing.
      if (!llvm::isa<clang::CXXPseudoDestructorExpr>(
              ce->getCallee()->IgnoreParens()) &&
          !indirectCallCannotThrow(ce))
        opaque(guarded);
      return;
    }
    const auto *md = llvm::dyn_cast<clang::CXXMethodDecl>(fd);
    if (!md || !md->isVirtual() ||
        overridersCannotThrow(md, ctx_.getLangOpts())) {
      // Not dispatched, or dispatched to overriders that must all be at
      // least as restrictive as this non-throwing declaration.
      callee(fd, guarded);
      return;
    }
    const clang::Expr *object = nullptr;
    if (const auto *me = llvm::dyn_cast<clang::MemberExpr>(
            ce->getCallee()->IgnoreParens())) {
      if (me->hasQualifier()) { // `Base::f()` names its target
        callee(fd, guarded);
        return;
      }
      object = me->getBase();
    } else if (llvm::isa<clang::CXXOperatorCallExpr>(ce) &&
               ce->getNumArgs() > 0) {
      object = ce->getArg(0);
    }
    const clang::CXXMethodDecl *target =
        object ? md->getDevirtualizedMethod(object, /*IsAppleKext=*/false)
               : nullptr;
    if (target)
      callee(target, guarded);
    else
      opaque(guarded); // any overrider, including ones not written yet
  }

  // The first use of a thread_local variable on a thread goes through its
  // wrapper, which runs the dynamic initializers of every thread_local
  // its TU defines. Only a definition seen here that needs neither a
  // dynamic initializer nor a destructor (registered on first use) has
  // no wrapper.
  void threadLocalUse(const clang::ValueDecl *d, bool guarded) {
    const auto *v = llvm::dyn_cast_or_null<clang::VarDecl>(d);
    if (!v || v->getTLSKind() != clang::VarDecl::TLS_Dynamic ||
        v->isLocalVarDeclOrParm())
      return;
    const clang::VarDecl *def = v->getDefinition();
    if (!def || def->needsDestruction(ctx_) != clang::QualType::DK_none) {
      opaque(guarded);
      return;
    }
    for (const clang::VarDecl *r : v->redecls())
      if (r->hasAttr<clang::ConstInitAttr>())
        return;
    if (!def->hasInit() || def->hasConstantInitialization())
      return;
    opaque(guarded);
  }

  void visit(const clang::Stmt *s, bool guarded) {
    if (const auto *e = llvm::dyn_cast<clang::Expr>(s)) {
      if (e->containsErrors() || e->isInstantiationDependent()) {
        opaque(guarded);
        return;
      }
      // A variably modified type (a cast to a pointer to a VLA, say)
      // evaluates its bound where the expression runs.
      if (e->getType()->isVariablyModifiedType())
        opaque(guarded);
    }

    switch (s->getStmtClass()) {
    case clang::Stmt::CXXThrowExprClass:
      opaque(guarded);
      break;

    case clang::Stmt::CXXTryStmtClass: {
      const auto *t = llvm::cast<clang::CXXTryStmt>(s);
      bool catchAll = false;
      for (unsigned i = 0; i < t->getNumHandlers(); ++i)
        if (!t->getHandler(i)->getExceptionDecl())
          catchAll = true;
      // Only catch (...) stops everything; a typed handler would need the
      // thrown types. Handlers run outside the try they belong to.
      push(t->getTryBlock(), guarded || (catchAll && t != functionTry_));
      for (unsigned i = 0; i < t->getNumHandlers(); ++i)
        push(t->getHandler(i), guarded);
      return;
    }

    case clang::Stmt::CXXCatchStmtClass: {
      const auto *c = llvm::cast<clang::CXXCatchStmt>(s);
      if (const clang::VarDecl *v = c->getExceptionDecl()) {
        push(v->getInit(), guarded);
        destroy(v->getType(), guarded);
      }
      push(c->getHandlerBlock(), guarded);
      return;
    }

    case clang::Stmt::DeclRefExprClass:
      threadLocalUse(llvm::cast<clang::DeclRefExpr>(s)->getDecl(), guarded);
      break;

    case clang::Stmt::MemberExprClass:
      threadLocalUse(llvm::cast<clang::MemberExpr>(s)->getMemberDecl(),
                     guarded);
      break;

    case clang::Stmt::DeclStmtClass:
      for (const clang::Decl *d : llvm::cast<clang::DeclStmt>(s)->decls())
        localDecl(d, guarded);
      return;

    case clang::Stmt::IfStmtClass: {
      const auto *i = llvm::cast<clang::IfStmt>(s);
      if (i->isConsteval()) {
        // The compile-time branch never runs.
        push(i->isNonNegatedConsteval() ? i->getElse() : i->getThen(),
             guarded);
        return;
      }
      if (i->isConstexpr() && i->getCond() &&
          !i->getCond()->containsErrors() &&
          !i->getCond()->isValueDependent()) {
        if (auto taken = i->getNondiscardedCase(ctx_)) {
          push(i->getInit(), guarded);
          push(*taken, guarded);
          return;
        }
      }
      break;
    }

    case clang::Stmt::CallExprClass:
    case clang::Stmt::CXXMemberCallExprClass:
    case clang::Stmt::CXXOperatorCallExprClass:
    case clang::Stmt::UserDefinedLiteralClass:
    case clang::Stmt::CUDAKernelCallExprClass: {
      const auto *ce = llvm::cast<clang::CallExpr>(s);
      if (ce->isUnevaluatedBuiltinCall(ctx_))
        return;
      call(ce, guarded);
      break;
    }

    case clang::Stmt::CXXConstructExprClass:
    case clang::Stmt::CXXTemporaryObjectExprClass:
      callee(llvm::cast<clang::CXXConstructExpr>(s)->getConstructor(), guarded);
      break;

    case clang::Stmt::CXXInheritedCtorInitExprClass:
      callee(llvm::cast<clang::CXXInheritedCtorInitExpr>(s)->getConstructor(),
             guarded);
      return;

    case clang::Stmt::CXXBindTemporaryExprClass:
      callee(llvm::cast<clang::CXXBindTemporaryExpr>(s)
                 ->getTemporary()
                 ->getDestructor(),
             guarded);
      break;

    case clang::Stmt::CXXNewExprClass: {
      const auto *n = llvm::cast<clang::CXXNewExpr>(s);
      const clang::FunctionDecl *op = n->getOperatorNew();
      if (!op)
        opaque(guarded);
      else if (!cannotThrowByDecl(op)) {
        // The replaceable global forms report failure by throwing,
        // whatever definition the program ends up linking.
        if (op->isReplaceableGlobalAllocationFunction())
          opaque(guarded);
        else
          callee(op, guarded);
        // With an allocation function that may throw, an array size that
        // is negative or too large throws std::bad_array_new_length before
        // the function is called ([expr.new]/9).
        if (n->isArray()) {
          auto size = n->getArraySize();
          if (!size || !*size || (*size)->isValueDependent() ||
              !(*size)->isIntegerConstantExpr(ctx_))
            opaque(guarded);
        }
      }
      break; // placement arguments, array size, initializer
    }

    case clang::Stmt::CXXDeleteExprClass: {
      const auto *d = llvm::cast<clang::CXXDeleteExpr>(s);
      if (const clang::FunctionDecl *op = d->getOperatorDelete())
        callee(op, guarded);
      clang::QualType t = d->getDestroyedType();
      if (t.isNull()) {
        opaque(guarded);
        break;
      }
      const clang::CXXRecordDecl *rd =
          ctx_.getBaseElementType(t)->getAsCXXRecordDecl();
      if (!rd)
        break;
      if (!rd->hasDefinition()) {
        opaque(guarded);
        break;
      }
      if (rd->hasTrivialDestructor())
        break;
      const clang::CXXDestructorDecl *dtor = rd->getDestructor();
      if (!dtor)
        opaque(guarded);
      else if (dtor->isVirtual() &&
               !overridersCannotThrow(dtor, ctx_.getLangOpts()) &&
               !rd->isEffectivelyFinal())
        opaque(guarded); // the dynamic type's destructor runs
      else if (!dtor->isDeleted())
        callee(dtor, guarded);
      break;
    }

    case clang::Stmt::LambdaExprClass:
      // Creating a closure runs the capture initializers; the body runs
      // when its call operator is called, which is a call like any other.
      for (const clang::Expr *init :
           llvm::cast<clang::LambdaExpr>(s)->capture_inits())
        push(init, guarded);
      return;

    case clang::Stmt::CXXDefaultArgExprClass:
      pushShared(llvm::cast<clang::CXXDefaultArgExpr>(s)->getExpr(), guarded);
      return;

    case clang::Stmt::CXXDefaultInitExprClass:
      pushShared(llvm::cast<clang::CXXDefaultInitExpr>(s)->getExpr(), guarded);
      return;

    case clang::Stmt::OpaqueValueExprClass:
      pushShared(llvm::cast<clang::OpaqueValueExpr>(s)->getSourceExpr(),
                 guarded);
      return;

    case clang::Stmt::InitListExprClass:
      push(llvm::cast<clang::InitListExpr>(s)->getArrayFiller(), guarded);
      break;

    case clang::Stmt::CXXParenListInitExprClass:
      push(llvm::cast<clang::CXXParenListInitExpr>(s)->getArrayFiller(),
           guarded);
      break;

    case clang::Stmt::CXXTypeidExprClass: {
      const auto *t = llvm::cast<clang::CXXTypeidExpr>(s);
      if (!t->isTypeOperand() && t->isPotentiallyEvaluated()) {
        opaque(guarded); // std::bad_typeid on a null pointer
        push(t->getExprOperand(), guarded);
      }
      return;
    }

    case clang::Stmt::CXXDynamicCastExprClass: {
      const auto *c = llvm::cast<clang::CXXDynamicCastExpr>(s);
      if (c->getCastKind() == clang::CK_Dynamic && c->isGLValue())
        opaque(guarded); // std::bad_cast
      break;
    }

    case clang::Stmt::ConstantExprClass: {
      const auto *c = llvm::cast<clang::ConstantExpr>(s);
      if (!c->hasAPValueResult())
        break;
      // Evaluated at compile time, but an object it produces is still
      // destroyed at run time.
      if (c->isPRValue())
        destroy(c->getType(), guarded);
      return;
    }

    case clang::Stmt::UnaryExprOrTypeTraitExprClass: {
      // sizeof and friends evaluate nothing, except a VLA's bound.
      const auto *u = llvm::cast<clang::UnaryExprOrTypeTraitExpr>(s);
      if (u->isArgumentType()) {
        if (u->getArgumentType()->isVariablyModifiedType())
          opaque(guarded);
      } else if (u->getArgumentExpr()->getType()->isVariablyModifiedType()) {
        push(u->getArgumentExpr(), guarded);
      }
      return;
    }

    case clang::Stmt::OffsetOfExprClass: {
      // A non-constant array index is evaluated.
      const auto *o = llvm::cast<clang::OffsetOfExpr>(s);
      for (unsigned i = 0; i < o->getNumComponents(); ++i)
        if (o->getComponent(i).getKind() == clang::OffsetOfNode::Array)
          push(o->getIndexExpr(o->getComponent(i).getArrayExprIndex()),
               guarded);
      return;
    }

    case clang::Stmt::CXXNoexceptExprClass:
    case clang::Stmt::RequiresExprClass:
    case clang::Stmt::ConceptSpecializationExprClass:
    case clang::Stmt::ExpressionTraitExprClass:
    case clang::Stmt::CXXUuidofExprClass:
      return; // unevaluated operands

    case clang::Stmt::GenericSelectionExprClass: {
      const auto *g = llvm::cast<clang::GenericSelectionExpr>(s);
      if (!g->isResultDependent())
        push(g->getResultExpr(), guarded);
      return;
    }

    case clang::Stmt::ChooseExprClass: {
      const auto *c = llvm::cast<clang::ChooseExpr>(s);
      if (!c->getCond()->isValueDependent())
        push(c->getChosenSubExpr(), guarded);
      return;
    }

    // Constructs whose control flow or callees this walk does not model.
    case clang::Stmt::CoroutineBodyStmtClass:
    case clang::Stmt::CoreturnStmtClass:
    case clang::Stmt::CoawaitExprClass:
    case clang::Stmt::CoyieldExprClass:
    case clang::Stmt::DependentCoawaitExprClass:
    case clang::Stmt::CapturedStmtClass:
    case clang::Stmt::BlockExprClass:
      opaque(guarded);
      return;

    // Objective-C constructs that send messages or unwind in ways this
    // walk does not model.
    case clang::Stmt::ObjCMessageExprClass:
    case clang::Stmt::ObjCAtThrowStmtClass:
    case clang::Stmt::ObjCBoxedExprClass:
    case clang::Stmt::ObjCArrayLiteralClass:
    case clang::Stmt::ObjCDictionaryLiteralClass:
    case clang::Stmt::ObjCSubscriptRefExprClass:
    case clang::Stmt::ObjCPropertyRefExprClass:
    case clang::Stmt::ObjCForCollectionStmtClass:
    case clang::Stmt::ObjCAtTryStmtClass:
    case clang::Stmt::ObjCAtSynchronizedStmtClass:
    case clang::Stmt::ObjCAutoreleasePoolStmtClass:
      opaque(guarded);
      break;

    default:
      if (llvm::isa<clang::OMPExecutableDirective>(s)) {
        opaque(guarded);
        return;
      }
      break;
    }
    pushChildren(s, guarded);
  }

  void walk(uint32_t n) {
    const clang::FunctionDecl *def = decl_[n];
    cur_ = n;
    stack_.clear();
    shared_[0].clear();
    shared_[1].clear();
    const clang::Stmt *body = def->getBody();

    // A constructor's or destructor's function-try-block rethrows when a
    // handler ends, so it guards nothing.
    functionTry_ = nullptr;
    if (llvm::isa<clang::CXXConstructorDecl>(def) ||
        llvm::isa<clang::CXXDestructorDecl>(def))
      functionTry_ = llvm::dyn_cast_or_null<clang::CXXTryStmt>(body);

    if (const auto *ctor = llvm::dyn_cast<clang::CXXConstructorDecl>(def))
      for (const clang::CXXCtorInitializer *init : ctor->inits())
        push(init->getInit(), false);

    if (const auto *dtor = llvm::dyn_cast<clang::CXXDestructorDecl>(def)) {
      // After the body: non-variant members, then bases. These calls are
      // in no statement.
      const clang::CXXRecordDecl *cls = dtor->getParent();
      if (!cls->isUnion())
        for (const clang::FieldDecl *field : cls->fields())
          destroy(field->getType(), false);
      for (const clang::CXXBaseSpecifier &base : cls->bases())
        if (!base.isVirtual())
          destroy(base.getType(), false);
      for (const clang::CXXBaseSpecifier &base : cls->vbases())
        destroy(base.getType(), false);
    }

    // By-value class parameters are destroyed by the caller on the
    // Itanium ABI, where the argument's temporary carries the call; where
    // the callee destroys them, the call is this function's.
    for (const clang::ParmVarDecl *p : def->parameters()) {
      if (const clang::CXXRecordDecl *rd = p->getType()->getAsCXXRecordDecl())
        if (rd->hasDefinition() && rd->isParamDestroyedInCallee())
          destroy(p->getType(), false);
      // A VLA bound in a parameter's type is evaluated on entry.
      if (p->getType()->isVariablyModifiedType())
        opaque(false);
    }

    push(body, false);
    while (!stack_.empty()) {
      auto [s, guarded] = stack_.pop_back_val();
      visit(s, guarded);
    }
  }

  // ---- TU-local resolution ---------------------------------------------

  // "May throw" flows backwards over unguarded edges. What it reaches is
  // settled for good; everything else is an obligation the cross-TU
  // analysis discharges.
  void propagate() {
    const auto n = static_cast<uint32_t>(decl_.size());
    std::vector<uint32_t> rbegin(n + 1, 0);
    for (uint32_t e = 0; e < edgeDst_.size(); ++e)
      if (edgeBits_[e] & kExit)
        ++rbegin[edgeDst_[e] + 1];
    for (uint32_t i = 0; i < n; ++i)
      rbegin[i + 1] += rbegin[i];
    std::vector<uint32_t> rsrc(rbegin[n]);
    std::vector<uint32_t> cursor(rbegin.begin(), rbegin.end() - 1);
    for (uint32_t src = 0; src < n; ++src)
      for (uint32_t e = edgeBegin_[src]; e < edgeBegin_[src + 1]; ++e)
        if (edgeBits_[e] & kExit)
          rsrc[cursor[edgeDst_[e]]++] = src;

    std::vector<uint32_t> work;
    work.reserve(n);
    for (uint32_t i = 0; i < n; ++i)
      if (state_[i] & kMayThrow)
        work.push_back(i);
    while (!work.empty()) {
      uint32_t v = work.back();
      work.pop_back();
      for (uint32_t r = rbegin[v]; r < rbegin[v + 1]; ++r) {
        uint32_t caller = rsrc[r];
        if (state_[caller] & kMayThrow)
          continue;
        state_[caller] |= kMayThrow;
        work.push_back(caller); // each node enters once: capacity holds
      }
    }
  }

  uint32_t flagsOf(const clang::FunctionDecl *def, uint8_t state) const {
    uint32_t flags = cannotThrowByDecl(def)
                         ? NoexceptFactEntry::DeclaredNothrow
                         : NoexceptFactEntry::DeclaredThrowing;
    if (state & kMayThrow)
      flags |= NoexceptFactEntry::MayThrow;
    if (const auto *md = llvm::dyn_cast<clang::CXXMethodDecl>(def)) {
      if (md->isVirtual())
        flags |= NoexceptFactEntry::Virtual;
      if (md->isMoveAssignmentOperator())
        flags |= NoexceptFactEntry::Move;
    }
    if (const auto *ctor = llvm::dyn_cast<clang::CXXConstructorDecl>(def))
      if (ctor->isMoveConstructor())
        flags |= NoexceptFactEntry::Move;
    if (def->getIdentifier() && def->getName() == "swap")
      flags |= NoexceptFactEntry::Swap;

    const clang::LangOptions &lang = ctx_.getLangOpts();
    const auto *proto = def->getType()->getAs<clang::FunctionProtoType>();
    bool writtenThrowing =
        proto && proto->hasExceptionSpec() &&
        !(flags & NoexceptFactEntry::DeclaredNothrow);
    // An allocation function declared noexcept reports failure by null
    // instead, and callers then test for it: a change of contract.
    const clang::OverloadedOperatorKind op = def->getOverloadedOperator();
    const bool allocation = op == clang::OO_New || op == clang::OO_Array_New;
    if (!lang.CPlusPlus11 || def->isMain() || def->isExternC() ||
        def->getLocation().isMacroID() || writtenThrowing ||
        def->isConsteval() || allocation ||
        def->getTemplateSpecializationKind() ==
            clang::TSK_ExplicitInstantiationDefinition)
      flags |= NoexceptFactEntry::NotReportable;
    return flags;
  }

  const std::string &fileOf(clang::FileID fid) {
    auto [it, inserted] = fileIndex_.try_emplace(fid, files_.size());
    if (inserted) {
      auto entry = sm_.getFileEntryRefForID(fid);
      files_.push_back(entry ? absoluteFileName(sm_, entry->getName())
                             : std::string("<unknown>"));
    }
    return files_[it->second];
  }

  // One fact per exported definition. Its callees are what it reaches
  // outside this TU's folded code: the search runs through kLocal nodes
  // and stops at everything that has a USR.
  void exportFacts(GlobalIndex &index) {
    const auto n = static_cast<uint32_t>(decl_.size());
    size_t exported = 0;
    for (uint32_t i = 0; i < n; ++i)
      exported += kind_[i] == kExported;
    if (exported == 0)
      return;

    // An AST built from a TU with errors can be missing statements.
    const clang::DiagnosticsEngine &diags = ctx_.getDiagnostics();
    const bool unreliable = diags.hasUncompilableErrorOccurred() ||
                            diags.hasFatalErrorOccurred();

    std::vector<NoexceptFactEntry> facts;
    facts.reserve(exported);
    // Round stamps instead of clearing per root. A local node reached
    // first through a guarded call and later through an unguarded path is
    // expanded a second time, so that its callees gain the Exit bit.
    std::vector<uint32_t> seenCall(n, 0), seenExit(n, 0);
    std::vector<uint32_t> dfs, out;
    dfs.reserve(2 * static_cast<size_t>(n));
    uint32_t round = 0;

    for (uint32_t root = 0; root < n; ++root) {
      if (kind_[root] != kExported)
        continue;
      ++round;
      out.clear();
      dfs.clear();
      auto expand = [&](uint32_t from, bool exitPath) {
        for (uint32_t e = edgeBegin_[from]; e < edgeBegin_[from + 1]; ++e) {
          uint32_t to = edgeDst_[e];
          bool exit = exitPath && (edgeBits_[e] & kExit);
          bool newCall = seenCall[to] != round;
          bool newExit = exit && seenExit[to] != round;
          if (!newCall && !newExit)
            continue;
          seenCall[to] = round;
          if (exit)
            seenExit[to] = round;
          if (kind_[to] == kLocal)
            dfs.push_back((to << 1) | (exit ? 1u : 0u));
          else if (newCall && kind_[to] != kOpaque)
            out.push_back(to);
        }
      };
      expand(root, true);
      while (!dfs.empty()) {
        uint32_t item = dfs.back();
        dfs.pop_back();
        expand(item >> 1, item & 1u);
      }

      const clang::FunctionDecl *def = decl_[root];
      NoexceptFactEntry fact;
      fact.usr = std::string(usrOf(root));
      {
        llvm::raw_string_ostream os(fact.qualifiedName);
        def->getNameForDiagnostic(os, ctx_.getPrintingPolicy(),
                                  /*Qualified=*/true);
      }
      clang::SourceLocation loc = sm_.getExpansionLoc(def->getLocation());
      fact.filePath = fileOf(sm_.getFileID(loc));
      fact.line = sm_.getExpansionLineNumber(loc);
      fact.column = sm_.getExpansionColumnNumber(loc);
      fact.flags = flagsOf(def, state_[root]);
      if (unreliable)
        fact.flags |= NoexceptFactEntry::Unreliable;

      std::sort(out.begin(), out.end(), [&](uint32_t a, uint32_t b) {
        return usrOf(a) < usrOf(b);
      });
      // A refuted function's obligations no longer matter; its calls
      // still count towards its callees' fan-in.
      const bool refuted = fact.flags & NoexceptFactEntry::MayThrow;
      fact.callees.reserve(out.size());
      fact.calleeBits.reserve(out.size());
      for (uint32_t to : out) {
        uint8_t bits = kCall;
        if (!refuted && seenExit[to] == round)
          bits |= kExit;
        if (kind_[to] == kExternal)
          bits |= kElsewhere;
        std::string_view usr = usrOf(to);
        if (!fact.callees.empty() && fact.callees.back() == usr) {
          fact.calleeBits.back() |= bits; // two entities, one USR
          continue;
        }
        fact.callees.emplace_back(usr);
        fact.calleeBits.push_back(bits);
      }
      facts.push_back(std::move(fact));
    }
    index.addNoexceptFacts(std::move(facts));
  }

  clang::ASTContext &ctx_;
  const clang::SourceManager &sm_;
  std::unique_ptr<clang::MangleContext> mangle_;

  // Node table, one slot per function this TU mentions (struct of arrays).
  llvm::DenseMap<const clang::FunctionDecl *, uint32_t> nodeOf_;
  std::vector<const clang::FunctionDecl *> decl_; // the definition, if any
  std::vector<uint8_t> kind_;
  std::vector<uint8_t> state_;
  std::vector<uint32_t> usrOff_, usrLen_; // identity keys, into usrPool_
  std::string usrPool_;

  // Edges in CSR form: node i's are [edgeBegin_[i], edgeBegin_[i + 1]).
  std::vector<uint32_t> edgeBegin_;
  std::vector<uint32_t> edgeDst_;
  std::vector<uint8_t> edgeBits_;

  // Body walk state, reused across bodies.
  llvm::SmallVector<std::pair<const clang::Stmt *, bool>, 128> stack_;
  llvm::SmallPtrSet<const clang::Stmt *, 32> shared_[2];
  const clang::CXXTryStmt *functionTry_ = nullptr;
  uint32_t cur_ = 0;

  llvm::DenseMap<clang::FileID, size_t> fileIndex_;
  std::vector<std::string> files_;
};

} // namespace

void collectNoexceptFacts(clang::ASTContext &context, GlobalIndex &index) {
  Collector(context).run(index);
}

} // namespace vycor
