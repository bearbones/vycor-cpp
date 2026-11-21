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
constexpr uint8_t kBare = NoexceptFactEntry::Bare;

// How a statement is guarded: by no try, by a try whose handlers are all
// typed, or by a try with a catch (...). Only the last discharges
// anything; the first two are told apart for exception-boundary-escape.
constexpr uint8_t kGuardNone = 0;
constexpr uint8_t kGuardTyped = 1;
constexpr uint8_t kGuardAll = 2;

// What made a statement able to throw (the leaf reasons).
enum Why : uint8_t { kWhyThrow, kWhyLibraryThrow, kWhyDynamic, kWhyOther,
                     kWhyAlloc };

uint8_t reasonBit(Why why, uint8_t guard) {
  using F = NoexceptFactEntry;
  if (guard == kGuardTyped)
    return why == kWhyThrow || why == kWhyLibraryThrow ? F::ThrowTyped
                                                       : F::OpaqueTyped;
  switch (why) {
  case kWhyThrow:
    return F::ThrowBare;
  case kWhyLibraryThrow:
    return F::LibraryThrowBare;
  case kWhyDynamic:
    return F::DynamicBare;
  case kWhyOther:
    return F::OtherBare;
  case kWhyAlloc:
    return F::AllocBare;
  }
  return F::OtherBare;
}

// Leaf reasons of a body reached through a path guarded at `guard`.
uint8_t throughGuard(uint8_t reasons, uint8_t guard) {
  using F = NoexceptFactEntry;
  if (guard == kGuardNone)
    return reasons;
  if (guard == kGuardAll)
    return 0;
  uint8_t out = 0;
  if (reasons & (F::ThrowBare | F::LibraryThrowBare | F::ThrowTyped))
    out |= F::ThrowTyped;
  if (reasons & (F::DynamicBare | F::OtherBare | F::AllocBare | F::OpaqueTyped))
    out |= F::OpaqueTyped;
  return out;
}

// The standard library's throw helpers (libstdc++'s out-of-line
// std::__throw_out_of_range and kin, libc++'s inline ones,
// std::rethrow_exception): a call is a throw, whether or not a body says
// so. The out-of-memory and size-limit ones (bad_alloc,
// bad_array_new_length, length_error past max_size()) count as
// allocation, which every container growth reaches. False when `fd` is
// not one.
bool libraryThrowHelper(const clang::FunctionDecl *fd, Why &why) {
  if (!fd->isInStdNamespace() || !fd->getIdentifier())
    return false;
  llvm::StringRef name = fd->getName();
  if (name == "rethrow_exception") {
    why = kWhyLibraryThrow;
    return true;
  }
  if (!name.consume_front("__throw_"))
    return false;
  why = name == "bad_alloc" || name == "bad_array_new_length" ||
                name == "length_error"
            ? kWhyAlloc
            : kWhyLibraryThrow;
  return true;
}

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
        mangle_(ctx.createMangleContext()) {
    // An exception can only start in C++ code compiled with exceptions;
    // anywhere else the boundaries that matter are the frames themselves.
    const clang::LangOptions &lang = ctx.getLangOpts();
    collectBoundaries_ = lang.CPlusPlus && lang.CXXExceptions;
  }

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
    // Written off before any walk: nothing says what it runs.
    leaf_.push_back(state & kMayThrow ? NoexceptFactEntry::OtherBare : 0);
    leafLoc_.push_back(def->getLocation());
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

  void push(const clang::Stmt *s, uint8_t guard) {
    if (s)
      stack_.push_back({s, guard});
  }

  void pushChildren(const clang::Stmt *s, uint8_t guard) {
    for (const clang::Stmt *child : s->children())
      push(child, guard);
  }

  // An expression shared between several parents (a default argument, a
  // default member initializer, an opaque value's source). Walked once
  // per guard level, which also ends a default member initializer that
  // names its own class.
  void pushShared(const clang::Stmt *s, uint8_t guard) {
    if (s && shared_[guard].insert(s).second)
      stack_.push_back({s, guard});
  }

  // Records why the current body can throw, at the statement being
  // walked. The site kept is that of the most telling reason.
  void leaf(Why why, uint8_t guard) {
    if (guard == kGuardAll)
      return;
    const uint8_t bit = reasonBit(why, guard);
    if (NoexceptFactEntry::reasonRank(bit) <
        NoexceptFactEntry::reasonRank(leaf_[cur_]))
      leafLoc_[cur_] = curLoc_;
    leaf_[cur_] |= bit;
  }

  // Something here can throw and nothing statically bounds it.
  void opaque(uint8_t guard, Why why = kWhyOther) {
    if (guard == kGuardAll)
      return;
    state_[cur_] |= kMayThrow;
    leaf(why, guard);
  }

  void addEdge(uint32_t dst, uint8_t guard) {
    // Consecutive duplicates (a loop of calls to one function) are the
    // common repetition; the rest is merged when the fact is built.
    uint8_t bits = guard == kGuardAll    ? kCall
                   : guard == kGuardNone ? (kCall | kExit | kBare)
                                         : (kCall | kExit);
    if (edgeDst_.size() > edgeBegin_.back() && edgeDst_.back() == dst &&
        edgeBits_.back() == bits)
      return;
    edgeDst_.push_back(dst);
    edgeBits_.push_back(bits);
  }

  void callee(const clang::FunctionDecl *fd, uint8_t guard) {
    if (!fd) {
      opaque(guard);
      return;
    }
    // A trivial special member runs no code at all.
    if (fd->isTrivial() || cannotThrowByDecl(fd))
      return;
    // A throw of the library's own, not an unknown callee.
    if (Why why; libraryThrowHelper(fd, why)) {
      opaque(guard, why);
      return;
    }
    addEdge(nodeFor(fd), guard);
  }

  // The destructor that runs for an object of type `type` (an array's
  // elements included). Trivial and deleted destructors run nothing.
  void destroy(clang::QualType type, uint8_t guard) {
    if (type.isNull())
      return;
    const clang::CXXRecordDecl *rd =
        ctx_.getBaseElementType(type)->getAsCXXRecordDecl();
    if (!rd)
      return;
    if (!rd->hasDefinition()) {
      opaque(guard);
      return;
    }
    if (rd->hasTrivialDestructor())
      return;
    const clang::CXXDestructorDecl *dtor = rd->getDestructor();
    if (!dtor) {
      opaque(guard);
      return;
    }
    if (!dtor->isDeleted())
      callee(dtor, guard);
  }

  void localDecl(const clang::Decl *d, uint8_t guard) {
    if (const auto *v = llvm::dyn_cast<clang::VarDecl>(d)) {
      if (v->getType()->isVariablyModifiedType())
        opaque(guard); // the bound is an expression the AST hides here
      // A constexpr variable's initializer was evaluated at compile time;
      // an evaluation that reaches a throw is not a constant expression.
      if (!v->isConstexpr())
        push(v->getInit(), guard);
      // Static and thread locals are destroyed at exit, not here.
      if (v->hasLocalStorage())
        destroy(v->getType(), guard);
      if (const auto *cleanup = v->getAttr<clang::CleanupAttr>())
        callee(cleanup->getFunctionDecl(), guard); // runs at scope exit
      if (const auto *dd = llvm::dyn_cast<clang::DecompositionDecl>(v))
        for (const clang::BindingDecl *b : dd->bindings())
          if (const clang::VarDecl *holding = b->getHoldingVar()) {
            push(holding->getInit(), guard);
            destroy(holding->getType(), guard);
          }
      return;
    }
    if (const auto *t = llvm::dyn_cast<clang::TypedefNameDecl>(d))
      if (t->getUnderlyingType()->isVariablyModifiedType())
        opaque(guard);
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

  void call(const clang::CallExpr *ce, uint8_t guard) {
    const clang::FunctionDecl *fd = ce->getDirectCallee();
    if (!fd) {
      // `p->~T()` on a scalar runs nothing.
      if (!llvm::isa<clang::CXXPseudoDestructorExpr>(
              ce->getCallee()->IgnoreParens()) &&
          !indirectCallCannotThrow(ce))
        opaque(guard, kWhyDynamic);
      return;
    }
    const auto *md = llvm::dyn_cast<clang::CXXMethodDecl>(fd);
    if (!md || !md->isVirtual() ||
        overridersCannotThrow(md, ctx_.getLangOpts())) {
      // Not dispatched, or dispatched to overriders that must all be at
      // least as restrictive as this non-throwing declaration.
      callee(fd, guard);
      return;
    }
    const clang::Expr *object = nullptr;
    if (const auto *me = llvm::dyn_cast<clang::MemberExpr>(
            ce->getCallee()->IgnoreParens())) {
      if (me->hasQualifier()) { // `Base::f()` names its target
        callee(fd, guard);
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
      callee(target, guard);
    else
      opaque(guard, kWhyDynamic); // any overrider, including future ones
  }

  // The first use of a thread_local variable on a thread goes through its
  // wrapper, which runs the dynamic initializers of every thread_local
  // its TU defines. Only a definition seen here that needs neither a
  // dynamic initializer nor a destructor (registered on first use) has
  // no wrapper.
  void threadLocalUse(const clang::ValueDecl *d, uint8_t guard) {
    const auto *v = llvm::dyn_cast_or_null<clang::VarDecl>(d);
    if (!v || v->getTLSKind() != clang::VarDecl::TLS_Dynamic ||
        v->isLocalVarDeclOrParm())
      return;
    const clang::VarDecl *def = v->getDefinition();
    if (!def || def->needsDestruction(ctx_) != clang::QualType::DK_none) {
      opaque(guard);
      return;
    }
    for (const clang::VarDecl *r : v->redecls())
      if (r->hasAttr<clang::ConstInitAttr>())
        return;
    if (!def->hasInit() || def->hasConstantInitialization())
      return;
    opaque(guard);
  }

  // Records the `throw;` statements of a handler body, outside nested
  // try statements and lambdas, and says whether there was one.
  bool noteRethrows(const clang::Stmt *body) {
    bool found = false;
    llvm::SmallVector<const clang::Stmt *, 32> todo;
    todo.push_back(body);
    while (!todo.empty()) {
      const clang::Stmt *s = todo.pop_back_val();
      if (!s || llvm::isa<clang::LambdaExpr>(s) ||
          llvm::isa<clang::CXXTryStmt>(s))
        continue;
      if (const auto *te = llvm::dyn_cast<clang::CXXThrowExpr>(s);
          te && !te->getSubExpr()) {
        rethrows_.insert(te);
        found = true;
        continue;
      }
      for (const clang::Stmt *child : s->children())
        todo.push_back(child);
    }
    return found;
  }

  void visit(const clang::Stmt *s, uint8_t guard) {
    curLoc_ = s->getBeginLoc();
    if (collectBoundaries_)
      noteBoundary(s);
    if (const auto *e = llvm::dyn_cast<clang::Expr>(s)) {
      if (e->containsErrors() || e->isInstantiationDependent()) {
        opaque(guard);
        return;
      }
      // A variably modified type (a cast to a pointer to a VLA, say)
      // evaluates its bound where the expression runs.
      if (e->getType()->isVariablyModifiedType())
        opaque(guard);
    }

    switch (s->getStmtClass()) {
    case clang::Stmt::CXXThrowExprClass: {
      if (rethrows_.count(s))
        break; // raises only what its try block did, recorded there
      clang::SourceLocation loc = sm_.getExpansionLoc(curLoc_);
      opaque(guard, loc.isValid() && sm_.isInSystemHeader(loc)
                        ? kWhyLibraryThrow
                        : kWhyThrow);
      break;
    }

    case clang::Stmt::CXXTryStmtClass: {
      const auto *t = llvm::cast<clang::CXXTryStmt>(s);
      const clang::CXXCatchStmt *catchAll = nullptr;
      bool typedHandler = false;
      for (unsigned i = 0; i < t->getNumHandlers(); ++i) {
        if (!t->getHandler(i)->getExceptionDecl())
          catchAll = t->getHandler(i);
        else
          typedHandler = true;
      }
      // Only catch (...) stops everything; a typed handler would need the
      // thrown types. Handlers run outside the try they belong to. A
      // constructor's or destructor's function-try-block rethrows, and so
      // does `catch (...) { cleanup(); throw; }` (the standard library's
      // containers are full of it): every exception of the try block
      // leaves, but for what a typed handler before it catches.
      uint8_t inner = guard;
      if (t != functionTry_) {
        if (!catchAll)
          inner = std::max(guard, kGuardTyped);
        else if (!noteRethrows(catchAll->getHandlerBlock()))
          inner = kGuardAll;
        else if (typedHandler)
          inner = std::max(guard, kGuardTyped);
      }
      push(t->getTryBlock(), inner);
      for (unsigned i = 0; i < t->getNumHandlers(); ++i)
        push(t->getHandler(i), guard);
      return;
    }

    case clang::Stmt::CXXCatchStmtClass: {
      const auto *c = llvm::cast<clang::CXXCatchStmt>(s);
      if (const clang::VarDecl *v = c->getExceptionDecl()) {
        push(v->getInit(), guard);
        destroy(v->getType(), guard);
      }
      push(c->getHandlerBlock(), guard);
      return;
    }

    case clang::Stmt::DeclRefExprClass:
      threadLocalUse(llvm::cast<clang::DeclRefExpr>(s)->getDecl(), guard);
      break;

    case clang::Stmt::MemberExprClass:
      threadLocalUse(llvm::cast<clang::MemberExpr>(s)->getMemberDecl(),
                     guard);
      break;

    case clang::Stmt::DeclStmtClass:
      for (const clang::Decl *d : llvm::cast<clang::DeclStmt>(s)->decls())
        localDecl(d, guard);
      return;

    case clang::Stmt::IfStmtClass: {
      const auto *i = llvm::cast<clang::IfStmt>(s);
      if (i->isConsteval()) {
        // The compile-time branch never runs.
        push(i->isNonNegatedConsteval() ? i->getElse() : i->getThen(),
             guard);
        return;
      }
      if (i->isConstexpr() && i->getCond() &&
          !i->getCond()->containsErrors() &&
          !i->getCond()->isValueDependent()) {
        if (auto taken = i->getNondiscardedCase(ctx_)) {
          push(i->getInit(), guard);
          push(*taken, guard);
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
      call(ce, guard);
      break;
    }

    case clang::Stmt::CXXConstructExprClass:
    case clang::Stmt::CXXTemporaryObjectExprClass:
      callee(llvm::cast<clang::CXXConstructExpr>(s)->getConstructor(), guard);
      break;

    case clang::Stmt::CXXInheritedCtorInitExprClass:
      callee(llvm::cast<clang::CXXInheritedCtorInitExpr>(s)->getConstructor(),
             guard);
      return;

    case clang::Stmt::CXXBindTemporaryExprClass:
      callee(llvm::cast<clang::CXXBindTemporaryExpr>(s)
                 ->getTemporary()
                 ->getDestructor(),
             guard);
      break;

    case clang::Stmt::CXXNewExprClass: {
      const auto *n = llvm::cast<clang::CXXNewExpr>(s);
      const clang::FunctionDecl *op = n->getOperatorNew();
      if (!op)
        opaque(guard);
      else if (!cannotThrowByDecl(op)) {
        // The replaceable global forms report failure by throwing,
        // whatever definition the program ends up linking.
        if (op->isReplaceableGlobalAllocationFunction())
          opaque(guard, kWhyAlloc);
        else
          callee(op, guard);
        // With an allocation function that may throw, an array size that
        // is negative or too large throws std::bad_array_new_length before
        // the function is called ([expr.new]/9).
        if (n->isArray()) {
          auto size = n->getArraySize();
          if (!size || !*size || (*size)->isValueDependent() ||
              !(*size)->isIntegerConstantExpr(ctx_))
            opaque(guard, kWhyAlloc);
        }
      }
      break; // placement arguments, array size, initializer
    }

    case clang::Stmt::CXXDeleteExprClass: {
      const auto *d = llvm::cast<clang::CXXDeleteExpr>(s);
      if (const clang::FunctionDecl *op = d->getOperatorDelete())
        callee(op, guard);
      clang::QualType t = d->getDestroyedType();
      if (t.isNull()) {
        opaque(guard);
        break;
      }
      const clang::CXXRecordDecl *rd =
          ctx_.getBaseElementType(t)->getAsCXXRecordDecl();
      if (!rd)
        break;
      if (!rd->hasDefinition()) {
        opaque(guard);
        break;
      }
      if (rd->hasTrivialDestructor())
        break;
      const clang::CXXDestructorDecl *dtor = rd->getDestructor();
      if (!dtor)
        opaque(guard);
      else if (dtor->isVirtual() &&
               !overridersCannotThrow(dtor, ctx_.getLangOpts()) &&
               !rd->isEffectivelyFinal())
        opaque(guard, kWhyDynamic); // the dynamic type's destructor runs
      else if (!dtor->isDeleted())
        callee(dtor, guard);
      break;
    }

    case clang::Stmt::LambdaExprClass:
      // Creating a closure runs the capture initializers; the body runs
      // when its call operator is called, which is a call like any other.
      for (const clang::Expr *init :
           llvm::cast<clang::LambdaExpr>(s)->capture_inits())
        push(init, guard);
      return;

    case clang::Stmt::CXXDefaultArgExprClass:
      pushShared(llvm::cast<clang::CXXDefaultArgExpr>(s)->getExpr(), guard);
      return;

    case clang::Stmt::CXXDefaultInitExprClass:
      pushShared(llvm::cast<clang::CXXDefaultInitExpr>(s)->getExpr(), guard);
      return;

    case clang::Stmt::OpaqueValueExprClass:
      pushShared(llvm::cast<clang::OpaqueValueExpr>(s)->getSourceExpr(),
                 guard);
      return;

    case clang::Stmt::InitListExprClass:
      push(llvm::cast<clang::InitListExpr>(s)->getArrayFiller(), guard);
      break;

    case clang::Stmt::CXXParenListInitExprClass:
      push(llvm::cast<clang::CXXParenListInitExpr>(s)->getArrayFiller(),
           guard);
      break;

    case clang::Stmt::CXXTypeidExprClass: {
      const auto *t = llvm::cast<clang::CXXTypeidExpr>(s);
      if (!t->isTypeOperand() && t->isPotentiallyEvaluated()) {
        opaque(guard); // std::bad_typeid on a null pointer
        push(t->getExprOperand(), guard);
      }
      return;
    }

    case clang::Stmt::CXXDynamicCastExprClass: {
      const auto *c = llvm::cast<clang::CXXDynamicCastExpr>(s);
      if (c->getCastKind() == clang::CK_Dynamic && c->isGLValue())
        opaque(guard); // std::bad_cast
      break;
    }

    case clang::Stmt::ConstantExprClass: {
      const auto *c = llvm::cast<clang::ConstantExpr>(s);
      if (!c->hasAPValueResult())
        break;
      // Evaluated at compile time, but an object it produces is still
      // destroyed at run time.
      if (c->isPRValue())
        destroy(c->getType(), guard);
      return;
    }

    case clang::Stmt::UnaryExprOrTypeTraitExprClass: {
      // sizeof and friends evaluate nothing, except a VLA's bound.
      const auto *u = llvm::cast<clang::UnaryExprOrTypeTraitExpr>(s);
      if (u->isArgumentType()) {
        if (u->getArgumentType()->isVariablyModifiedType())
          opaque(guard);
      } else if (u->getArgumentExpr()->getType()->isVariablyModifiedType()) {
        push(u->getArgumentExpr(), guard);
      }
      return;
    }

    case clang::Stmt::OffsetOfExprClass: {
      // A non-constant array index is evaluated.
      const auto *o = llvm::cast<clang::OffsetOfExpr>(s);
      for (unsigned i = 0; i < o->getNumComponents(); ++i)
        if (o->getComponent(i).getKind() == clang::OffsetOfNode::Array)
          push(o->getIndexExpr(o->getComponent(i).getArrayExprIndex()),
               guard);
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
        push(g->getResultExpr(), guard);
      return;
    }

    case clang::Stmt::ChooseExprClass: {
      const auto *c = llvm::cast<clang::ChooseExpr>(s);
      if (!c->getCond()->isValueDependent())
        push(c->getChosenSubExpr(), guard);
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
      opaque(guard);
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
      opaque(guard);
      break;

    default:
      if (llvm::isa<clang::OMPExecutableDirective>(s)) {
        opaque(guard);
        return;
      }
      break;
    }
    pushChildren(s, guard);
  }

  void walk(uint32_t n) {
    const clang::FunctionDecl *def = decl_[n];
    cur_ = n;
    curLoc_ = def->getLocation();
    stack_.clear();
    for (auto &shared : shared_)
      shared.clear();
    const clang::Stmt *body = def->getBody();

    // A constructor's or destructor's function-try-block rethrows when a
    // handler ends, so it guards nothing.
    functionTry_ = nullptr;
    if (llvm::isa<clang::CXXConstructorDecl>(def) ||
        llvm::isa<clang::CXXDestructorDecl>(def))
      functionTry_ = llvm::dyn_cast_or_null<clang::CXXTryStmt>(body);

    if (const auto *ctor = llvm::dyn_cast<clang::CXXConstructorDecl>(def))
      for (const clang::CXXCtorInitializer *init : ctor->inits())
        push(init->getInit(), kGuardNone);

    if (const auto *dtor = llvm::dyn_cast<clang::CXXDestructorDecl>(def)) {
      // After the body: non-variant members, then bases. These calls are
      // in no statement.
      const clang::CXXRecordDecl *cls = dtor->getParent();
      if (!cls->isUnion())
        for (const clang::FieldDecl *field : cls->fields())
          destroy(field->getType(), kGuardNone);
      for (const clang::CXXBaseSpecifier &base : cls->bases())
        if (!base.isVirtual())
          destroy(base.getType(), kGuardNone);
      for (const clang::CXXBaseSpecifier &base : cls->vbases())
        destroy(base.getType(), kGuardNone);
    }

    // By-value class parameters are destroyed by the caller on the
    // Itanium ABI, where the argument's temporary carries the call; where
    // the callee destroys them, the call is this function's.
    for (const clang::ParmVarDecl *p : def->parameters()) {
      if (const clang::CXXRecordDecl *rd = p->getType()->getAsCXXRecordDecl())
        if (rd->hasDefinition() && rd->isParamDestroyedInCallee())
          destroy(p->getType(), kGuardNone);
      // A VLA bound in a parameter's type is evaluated on entry.
      if (p->getType()->isVariablyModifiedType())
        opaque(kGuardNone);
    }

    push(body, kGuardNone);
    while (!stack_.empty()) {
      auto [s, guard] = stack_.pop_back_val();
      visit(s, guard);
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
    // Whether the frames this TU compiles can be unwound through: Clang
    // marks every function nounwind when LangOptions::Exceptions is off,
    // which is the TU's effective -f[no-]exceptions after the driver (the
    // setting isa-leak's codegen profile reads, as CXXExceptions, from the
    // same LangOptions). A C TU built with -fexceptions unwinds too.
    flags |= lang.Exceptions ? NoexceptFactEntry::CompiledWithExceptions
                             : NoexceptFactEntry::CompiledWithoutExceptions;
    if (lang.CPlusPlus && lang.Exceptions && def->isExternC() &&
        !def->isMain())
      flags |= NoexceptFactEntry::ExternC;
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

  // What a body and everything folded into it reach: the reason bits of
  // the folded bodies seen through the path's guard, the most telling
  // site, and the calls that leave this TU's folded code (or reach an
  // exported definition here). The search runs through kLocal nodes and
  // stops at everything that has a key. Round stamps instead of clearing
  // per root; a node reached first under a stronger guard is expanded
  // again when a weaker one reaches it, so its callees get the bits of
  // the weaker path.
  struct Summary {
    uint8_t reasons = 0;
    uint32_t siteNode = UINT32_MAX;
    uint8_t siteRank = 8;
  };

  Summary summarize(uint32_t root) {
    ++round_;
    out_.clear();
    dfs_.clear();
    Summary sum;
    auto fold = [&](uint32_t node, uint8_t guard) {
      const uint8_t bits = throughGuard(leaf_[node], guard);
      if (!bits)
        return;
      sum.reasons |= bits;
      const unsigned rank = NoexceptFactEntry::reasonRank(bits);
      if (rank < sum.siteRank) {
        sum.siteRank = static_cast<uint8_t>(rank);
        sum.siteNode = node;
      }
    };
    auto expand = [&](uint32_t from, uint8_t pathGuard) {
      for (uint32_t e = edgeBegin_[from]; e < edgeBegin_[from + 1]; ++e) {
        const uint32_t to = edgeDst_[e];
        const uint8_t bits = edgeBits_[e];
        const uint8_t edgeGuard = !(bits & kExit)  ? kGuardAll
                                  : (bits & kBare) ? kGuardNone
                                                   : kGuardTyped;
        const uint8_t guard = std::max(pathGuard, edgeGuard);
        const bool newCall = seenCall_[to] != round_;
        const bool newExit = guard <= kGuardTyped && seenExit_[to] != round_;
        const bool newBare = guard == kGuardNone && seenBare_[to] != round_;
        if (!newCall && !newExit && !newBare)
          continue;
        seenCall_[to] = round_;
        if (guard <= kGuardTyped)
          seenExit_[to] = round_;
        if (guard == kGuardNone)
          seenBare_[to] = round_;
        if (kind_[to] == kLocal) {
          fold(to, guard);
          dfs_.push_back((to << 2) | guard);
        } else if (kind_[to] == kOpaque) {
          fold(to, guard);
        } else if (newCall) {
          out_.push_back(to);
        }
      }
    };
    fold(root, kGuardNone);
    expand(root, kGuardNone);
    while (!dfs_.empty()) {
      const uint32_t item = dfs_.back();
      dfs_.pop_back();
      expand(item >> 2, static_cast<uint8_t>(item & 3u));
    }
    std::sort(out_.begin(), out_.end(), [&](uint32_t a, uint32_t b) {
      return usrOf(a) < usrOf(b);
    });
    return sum;
  }

  // The callees summarize() found, with the bits of the paths that reach
  // them, into a sorted (key, bits) list.
  void emitCallees(std::vector<std::string> &callees,
                   std::vector<uint8_t> &calleeBits) const {
    callees.reserve(out_.size());
    calleeBits.reserve(out_.size());
    for (uint32_t to : out_) {
      uint8_t bits = kCall;
      if (seenExit_[to] == round_)
        bits |= kExit;
      if (seenBare_[to] == round_)
        bits |= kBare;
      if (kind_[to] == kExternal)
        bits |= kElsewhere;
      std::string_view usr = usrOf(to);
      if (!callees.empty() && callees.back() == usr) {
        calleeBits.back() |= bits; // two entities, one USR
        continue;
      }
      callees.emplace_back(usr);
      calleeBits.push_back(bits);
    }
  }

  std::string siteOf(clang::SourceLocation raw) {
    clang::SourceLocation loc = sm_.getExpansionLoc(raw);
    if (loc.isInvalid())
      return std::string();
    return fileOf(sm_.getFileID(loc)) + ":" +
           std::to_string(sm_.getExpansionLineNumber(loc));
  }

  // A name a reader recognizes: a lambda is named by where it is written
  // and the function it is written in, an instantiation by its arguments.
  std::string displayName(const clang::FunctionDecl *fd) {
    std::string name;
    llvm::raw_string_ostream os(name);
    const auto *md = llvm::dyn_cast<clang::CXXMethodDecl>(fd);
    if (md && md->getParent()->isLambda()) {
      os << "lambda";
      const clang::DeclContext *dc = md->getParent()->getDeclContext();
      while (dc && !llvm::isa<clang::FunctionDecl>(dc) &&
             !dc->isFileContext())
        dc = dc->getParent();
      if (const auto *outer = llvm::dyn_cast_or_null<clang::FunctionDecl>(dc))
        os << " in '" << displayName(outer) << "'";
      return name;
    }
    fd->getNameForDiagnostic(os, ctx_.getPrintingPolicy(), /*Qualified=*/true);
    return name;
  }

  void setReasons(const Summary &sum, uint32_t root, uint8_t &reasons,
                  std::string &site, std::string &function) {
    reasons = sum.reasons;
    if (sum.siteNode == UINT32_MAX)
      return;
    site = siteOf(leafLoc_[sum.siteNode]);
    if (sum.siteNode != root)
      function = displayName(decl_[sum.siteNode]);
  }

  // One fact per exported definition.
  void exportFacts(GlobalIndex &index) {
    const auto n = static_cast<uint32_t>(decl_.size());
    size_t exported = 0;
    for (uint32_t i = 0; i < n; ++i)
      exported += kind_[i] == kExported;
    if (exported == 0 && boundaries_.empty())
      return;

    // An AST built from a TU with errors can be missing statements.
    const clang::DiagnosticsEngine &diags = ctx_.getDiagnostics();
    const bool unreliable = diags.hasUncompilableErrorOccurred() ||
                            diags.hasFatalErrorOccurred();

    seenCall_.assign(n, 0);
    seenExit_.assign(n, 0);
    seenBare_.assign(n, 0);
    dfs_.reserve(3 * static_cast<size_t>(n));
    round_ = 0;

    std::vector<NoexceptFactEntry> facts;
    facts.reserve(exported);
    for (uint32_t root = 0; root < n; ++root) {
      if (kind_[root] != kExported)
        continue;
      const Summary sum = summarize(root);
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
      setReasons(sum, root, fact.reasons, fact.reasonSite,
                 fact.reasonFunction);
      // A refuted function keeps its Exit callees: the proof no longer
      // needs them, but exception-boundary-escape's witness paths do.
      emitCallees(fact.callees, fact.calleeBits);
      facts.push_back(std::move(fact));
    }
    if (!facts.empty())
      index.addNoexceptFacts(std::move(facts));
    exportBoundaries(index);
  }

  // ---- exception boundaries --------------------------------------------

  struct PendingBoundary {
    uint8_t kind;
    uint32_t target;
    uint32_t receiver; // UINT32_MAX: none
    std::string api;
    clang::SourceLocation site;
  };

  // The function an expression hands over as a callable: a function or
  // its address, a lambda (or its conversion to a function pointer), a
  // local variable initialized with one of those, an object whose class
  // has one call operator. Null when it cannot be told.
  const clang::FunctionDecl *callableTarget(const clang::Expr *e,
                                            unsigned depth = 0) const {
    if (!e || depth > 4)
      return nullptr;
    e = e->IgnoreParenCasts();
    if (const auto *m = llvm::dyn_cast<clang::MaterializeTemporaryExpr>(e))
      return callableTarget(m->getSubExpr(), depth + 1);
    if (const auto *b = llvm::dyn_cast<clang::CXXBindTemporaryExpr>(e))
      return callableTarget(b->getSubExpr(), depth + 1);
    if (const auto *u = llvm::dyn_cast<clang::UnaryOperator>(e))
      if (u->getOpcode() == clang::UO_AddrOf ||
          u->getOpcode() == clang::UO_Plus)
        return callableTarget(u->getSubExpr(), depth + 1);
    if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(e)) {
      if (const auto *fd = llvm::dyn_cast<clang::FunctionDecl>(ref->getDecl()))
        return fd;
      if (const auto *v = llvm::dyn_cast<clang::VarDecl>(ref->getDecl())) {
        if (v->isLocalVarDecl() && v->getInit() &&
            !v->getType()->isReferenceType() &&
            (v->getType().isConstQualified() ||
             v->getType()->getAsCXXRecordDecl()))
          return callableTarget(v->getInit(), depth + 1);
        return classCallOperator(v->getType());
      }
      return nullptr;
    }
    if (const auto *le = llvm::dyn_cast<clang::LambdaExpr>(e))
      return lambdaCallOperator(le->getLambdaClass());
    // A lambda converted to a function pointer: the conversion function
    // returns the static invoker, which calls the call operator.
    if (const auto *mc = llvm::dyn_cast<clang::CXXMemberCallExpr>(e)) {
      if (llvm::isa_and_nonnull<clang::CXXConversionDecl>(
              mc->getMethodDecl()) &&
          mc->getRecordDecl() && mc->getRecordDecl()->isLambda())
        return lambdaCallOperator(mc->getRecordDecl());
      return nullptr;
    }
    if (const auto *ce = llvm::dyn_cast<clang::CXXConstructExpr>(e)) {
      if (ce->getNumArgs() == 1 &&
          ce->getConstructor()->isCopyOrMoveConstructor())
        return callableTarget(ce->getArg(0), depth + 1);
      return classCallOperator(ce->getType());
    }
    return nullptr;
  }

  static const clang::FunctionDecl *
  lambdaCallOperator(const clang::CXXRecordDecl *rd) {
    if (!rd)
      return nullptr;
    const clang::CXXMethodDecl *op = rd->getLambdaCallOperator();
    if (!op)
      return nullptr;
    // A generic lambda: the one specialization that was instantiated.
    if (const clang::FunctionTemplateDecl *tmpl =
            op->getDescribedFunctionTemplate()) {
      const clang::FunctionDecl *only = nullptr;
      for (const clang::FunctionDecl *spec : tmpl->specializations()) {
        if (only)
          return nullptr;
        only = spec;
      }
      return only;
    }
    return op;
  }

  static const clang::FunctionDecl *classCallOperator(clang::QualType type) {
    const clang::CXXRecordDecl *rd =
        type.isNull() ? nullptr : type->getAsCXXRecordDecl();
    if (!rd || !rd->hasDefinition())
      return nullptr;
    if (rd->isLambda())
      return lambdaCallOperator(rd);
    const clang::FunctionDecl *only = nullptr;
    for (const clang::CXXMethodDecl *md : rd->methods()) {
      if (md->getOverloadedOperator() != clang::OO_Call)
        continue;
      if (only)
        return nullptr;
      only = md;
    }
    return only;
  }

  static bool isStdThreadClass(const clang::CXXRecordDecl *rd) {
    if (!rd || !rd->isInStdNamespace() || !rd->getIdentifier())
      return false;
    llvm::StringRef name = rd->getName();
    return name == "thread" || name == "jthread";
  }

  // C thread-creation functions: their start routine is a thread entry.
  // Index of the start-routine parameter, or -1.
  static int threadStartParam(const clang::FunctionDecl *fd) {
    if (!fd->getIdentifier())
      return -1;
    llvm::StringRef name = fd->getName();
    if (name == "pthread_create")
      return 2;
    if (name == "thrd_create")
      return 1;
    return -1;
  }

  static bool isFunctionPointer(clang::QualType t) {
    return !t.isNull() && t->isFunctionPointerType();
  }

  void addBoundary(uint8_t kind, const clang::FunctionDecl *target,
                   const clang::FunctionDecl *receiver, std::string api,
                   clang::SourceLocation site) {
    if (!target)
      return;
    PendingBoundary b;
    b.kind = kind;
    b.target = nodeFor(target);
    b.receiver = receiver ? nodeFor(receiver) : UINT32_MAX;
    b.api = std::move(api);
    b.site = site;
    boundaries_.push_back(std::move(b));
  }

  void noteCallbackField(const clang::FieldDecl *field,
                         const clang::Expr *value,
                         clang::SourceLocation site) {
    if (!field || !isFunctionPointer(field->getType()))
      return;
    const clang::RecordDecl *rd = field->getParent();
    if (!rd || !rd->isExternCContext())
      return;
    std::string api;
    llvm::raw_string_ostream os(api);
    rd->printQualifiedName(os);
    os << "::" << field->getName();
    addBoundary(ExceptionBoundaryEntry::CCallback, callableTarget(value),
                nullptr, std::move(api), site);
  }

  // Hand-over sites in C++ code compiled with exceptions: a callable given
  // to std::thread, a function given to a C function's function-pointer
  // parameter or stored in a C struct's function-pointer field.
  void noteBoundary(const clang::Stmt *s) {
    switch (s->getStmtClass()) {
    case clang::Stmt::CXXConstructExprClass:
    case clang::Stmt::CXXTemporaryObjectExprClass: {
      const auto *ce = llvm::cast<clang::CXXConstructExpr>(s);
      const clang::CXXConstructorDecl *ctor = ce->getConstructor();
      if (ce->getNumArgs() == 0 || !isStdThreadClass(ctor->getParent()) ||
          ctor->isCopyOrMoveConstructor())
        return;
      addBoundary(ExceptionBoundaryEntry::ThreadEntry,
                  callableTarget(ce->getArg(0)), nullptr,
                  "std::" + ctor->getParent()->getName().str(),
                  ce->getBeginLoc());
      return;
    }
    case clang::Stmt::CallExprClass: {
      const auto *ce = llvm::cast<clang::CallExpr>(s);
      const clang::FunctionDecl *fd = ce->getDirectCallee();
      if (!fd || !fd->isExternC())
        return;
      const int start = threadStartParam(fd);
      const unsigned n = std::min(ce->getNumArgs(), fd->getNumParams());
      for (unsigned i = 0; i < n; ++i) {
        if (!isFunctionPointer(fd->getParamDecl(i)->getType()))
          continue;
        const bool thread = static_cast<int>(i) == start;
        addBoundary(thread ? ExceptionBoundaryEntry::ThreadEntry
                           : ExceptionBoundaryEntry::CCallback,
                    callableTarget(ce->getArg(i)), thread ? nullptr : fd,
                    fd->getName().str(), ce->getBeginLoc());
      }
      return;
    }
    case clang::Stmt::CXXMemberCallExprClass: {
      // A thread built in place in a standard container
      // (`workers.emplace_back([this] { run(); })`): the constructor call
      // is inside the library, on a forwarded parameter, so the hand-over
      // is taken here. The first argument that names a function is it
      // (emplace's first is a position).
      const auto *ce = llvm::cast<clang::CXXMemberCallExpr>(s);
      const clang::CXXMethodDecl *md = ce->getMethodDecl();
      if (!md || !md->getIdentifier() ||
          !md->getName().starts_with("emplace"))
        return;
      const auto *spec =
          llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(
              md->getParent());
      if (!spec || !spec->isInStdNamespace() ||
          spec->getTemplateArgs().size() == 0 ||
          spec->getTemplateArgs()[0].getKind() !=
              clang::TemplateArgument::Type)
        return;
      const clang::CXXRecordDecl *element =
          spec->getTemplateArgs()[0].getAsType()->getAsCXXRecordDecl();
      if (!isStdThreadClass(element))
        return;
      for (const clang::Expr *arg : ce->arguments()) {
        if (const clang::FunctionDecl *target = callableTarget(arg)) {
          addBoundary(ExceptionBoundaryEntry::ThreadEntry, target, nullptr,
                      "std::" + element->getName().str(), ce->getBeginLoc());
          break;
        }
      }
      return;
    }
    case clang::Stmt::BinaryOperatorClass: {
      const auto *bo = llvm::cast<clang::BinaryOperator>(s);
      if (bo->getOpcode() != clang::BO_Assign)
        return;
      if (const auto *me = llvm::dyn_cast<clang::MemberExpr>(
              bo->getLHS()->IgnoreParenImpCasts()))
        noteCallbackField(
            llvm::dyn_cast<clang::FieldDecl>(me->getMemberDecl()),
            bo->getRHS(), bo->getBeginLoc());
      return;
    }
    case clang::Stmt::InitListExprClass: {
      const auto *il = llvm::cast<clang::InitListExpr>(s);
      const clang::RecordDecl *rd =
          il->getType().isNull() ? nullptr : il->getType()->getAsRecordDecl();
      if (!rd || rd->isUnion() || !rd->isExternCContext() ||
          !rd->getDefinition())
        return;
      unsigned i = 0;
      for (const clang::FieldDecl *field : rd->getDefinition()->fields()) {
        // An unnamed bit-field takes no initializer.
        if (field->isBitField() && !field->getIdentifier())
          continue;
        if (i >= il->getNumInits())
          break;
        noteCallbackField(field, il->getInit(i++), il->getBeginLoc());
      }
      return;
    }
    default:
      return;
    }
  }

  std::string fullSiteOf(clang::SourceLocation raw) {
    clang::SourceLocation loc = sm_.getExpansionLoc(raw);
    if (loc.isInvalid())
      return std::string();
    return fileOf(sm_.getFileID(loc)) + ":" +
           std::to_string(sm_.getExpansionLineNumber(loc)) + ":" +
           std::to_string(sm_.getExpansionColumnNumber(loc));
  }

  void exportBoundaries(GlobalIndex &index) {
    std::vector<ExceptionBoundaryEntry> out;
    out.reserve(boundaries_.size());
    for (const PendingBoundary &b : boundaries_) {
      const clang::FunctionDecl *def = decl_[b.target];
      clang::SourceLocation site = sm_.getExpansionLoc(b.site);
      // Hand-overs inside the standard library (std::async starting its
      // own thread, say), targets it defines (what std::bind or
      // std::packaged_task wrap is not visible), and targets declared
      // noexcept (exception-escape's) are not this check's.
      if (site.isInvalid() || sm_.isInSystemHeader(site) ||
          inSystemHeader(def) || cannotThrowByDecl(def))
        continue;
      ExceptionBoundaryEntry e;
      e.kind = b.kind;
      e.qualifiedName = displayName(def);
      clang::SourceLocation loc = sm_.getExpansionLoc(def->getLocation());
      e.filePath = fileOf(sm_.getFileID(loc));
      e.line = sm_.getExpansionLineNumber(loc);
      e.column = sm_.getExpansionColumnNumber(loc);
      e.api = b.api;
      e.apiSite = fullSiteOf(b.site);
      if (b.receiver != UINT32_MAX && usrLen_[b.receiver] != 0)
        e.receiver = std::string(usrOf(b.receiver));
      if (kind_[b.target] == kLocal) {
        llvm::SmallString<128> usr;
        if (clang::index::generateUSRForDecl(def->getCanonicalDecl(), usr) ||
            usr.empty())
          e.target = e.filePath + ":" + std::to_string(e.line) + ":" +
                     std::to_string(e.column);
        else
          e.target = std::string(usr);
        const Summary sum = summarize(b.target);
        setReasons(sum, b.target, e.reasons, e.reasonSite, e.reasonFunction);
        emitCallees(e.callees, e.calleeBits);
      } else if (kind_[b.target] == kOpaque) {
        e.target = e.qualifiedName;
        e.reasons = NoexceptFactEntry::OtherBare;
      } else {
        // Its own fact (here or in another TU) says what it can raise.
        e.target = std::string(usrOf(b.target));
        e.callees.push_back(e.target);
        e.calleeBits.push_back(static_cast<uint8_t>(
            kCall | kExit | kBare |
            (kind_[b.target] == kExternal ? kElsewhere : 0)));
      }
      out.push_back(std::move(e));
    }
    if (!out.empty())
      index.addExceptionBoundaries(std::move(out));
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

  // Leaf reasons per node (NoexceptFactEntry::Reason bits) and the site of
  // the most telling one.
  std::vector<uint8_t> leaf_;
  std::vector<clang::SourceLocation> leafLoc_;

  // Hand-overs found while walking; walked bodies only, so code no
  // exported definition reaches is not looked at.
  std::vector<PendingBoundary> boundaries_;
  bool collectBoundaries_ = false;

  // Rethrows of a catch (...) that lets its try block's exceptions out.
  llvm::SmallPtrSet<const clang::Stmt *, 16> rethrows_;

  // summarize() state, sized once per TU.
  std::vector<uint32_t> seenCall_, seenExit_, seenBare_;
  std::vector<uint32_t> dfs_, out_;
  uint32_t round_ = 0;

  // Body walk state, reused across bodies.
  llvm::SmallVector<std::pair<const clang::Stmt *, uint8_t>, 128> stack_;
  llvm::SmallPtrSet<const clang::Stmt *, 32> shared_[3]; // per guard level
  const clang::CXXTryStmt *functionTry_ = nullptr;
  uint32_t cur_ = 0;
  clang::SourceLocation curLoc_;

  llvm::DenseMap<clang::FileID, size_t> fileIndex_;
  std::vector<std::string> files_;
};

} // namespace

void collectNoexceptFacts(clang::ASTContext &context, GlobalIndex &index) {
  Collector(context).run(index);
}

} // namespace vycor
