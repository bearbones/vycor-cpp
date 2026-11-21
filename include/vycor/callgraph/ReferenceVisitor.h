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

#include "vycor/callgraph/ControlFlowIndex.h"
#include "vycor/callgraph/ReferenceIndex.h"
#include "vycor/callgraph/UsrIdent.h"
#include "vycor/compat/ClangVersion.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/SourceManager.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"

#include <map>
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
// The code as written is read first, then what the compiler writes on its
// own:
//
//   - The body of every template instantiation the TU uses whose pattern is
//     a recorded symbol. Its references are attributed to the pattern (a
//     reference to an instantiation is recorded against its pattern too),
//     and only those the code as written does not already hold are added,
//     with the count of the instantiation that holds the most.
//   - Code that is no symbol of its own: implicit special members, and
//     instantiations of system-header templates (std::make_unique<T>,
//     std::vector<T>::emplace_back). Such a function is *transparent*: its
//     body is read once, and a reference to it stands for everything it
//     references, transitively through other transparent functions. The
//     references land on the recorded referrer that uses it, at its site.
//   - Implicit destructor calls (end of scope, temporaries, members and
//     bases, statics), operator new and delete behind new and delete
//     expressions, and the virtual functions a vtable holds where a
//     reference elsewhere would not show them.
//
// Instantiation bodies are read from a worklist after the main traversal,
// never nested inside it, so attribution needs no stack of modes.
//
// Lock facts. While it reads a body the visitor keeps the set of locks
// held, lexically: a guard local (callgraph/LockFacts.h) holds its mutexes
// from its declaration to the end of its block, unique_lock::unlock() and
// lock() toggle it, a mutex's own lock()/unlock() (and the pthread calls)
// add and drop it, and the body of a std::call_once lambda or of a
// function-local static's initializer holds a "once" lock. A function or
// lambda body starts with no lock held (a lambda runs later, wherever it is
// called). Every variable access and call is recorded with the lock sets
// it was made under (ReferenceRecord::locks), and whether it was made
// inside a loop (kRefInLoop). The lock set is cached and recomputed only
// when the held set changes, so an occurrence still costs one hash bump.
// An instantiation's body is read under its own locks; a transparent
// function's references take the lock sets of the use they are spliced
// into.
// ============================================================================
class ReferenceVisitor : public clang::RecursiveASTVisitor<ReferenceVisitor> {
  using Base = clang::RecursiveASTVisitor<ReferenceVisitor>;

public:
  // `lockCfg` adds lock guard types (--lock-type); null: the built-ins.
  ReferenceVisitor(ReferenceIndex &index, clang::SourceManager &sm,
                   const std::string &tuPath,
                   const LockTypeConfig *lockCfg = nullptr);

  // Traverse the TU and register its facts with the index under tuPath:
  // the references and symbols, then the emission facts
  // (ReferenceIndex::EmissionRecord) from what its code generation emits
  // (EmissionGraph.h).
  void run(clang::ASTContext &ctx);

  bool TraverseDecl(clang::Decl *decl);
  bool TraverseLambdaExpr(clang::LambdaExpr *expr);
  bool TraverseInitListExpr(clang::InitListExpr *expr);
  bool TraverseUnaryExprOrTypeTraitExpr(clang::UnaryExprOrTypeTraitExpr *expr);
  bool TraverseCXXNoexceptExpr(clang::CXXNoexceptExpr *expr);
  bool TraverseCXXTypeidExpr(clang::CXXTypeidExpr *expr);
  bool TraverseRequiresExpr(clang::RequiresExpr *expr);
  bool TraverseConceptDecl(clang::ConceptDecl *decl);
  bool TraverseCompoundStmt(clang::CompoundStmt *stmt);
  bool TraverseForStmt(clang::ForStmt *stmt);
  bool TraverseWhileStmt(clang::WhileStmt *stmt);
  bool TraverseDoStmt(clang::DoStmt *stmt);
  bool TraverseCXXForRangeStmt(clang::CXXForRangeStmt *stmt);
  bool TraverseIfStmt(clang::IfStmt *stmt);
  bool TraverseSwitchStmt(clang::SwitchStmt *stmt);
  bool TraverseBreakStmt(clang::BreakStmt *stmt);
  bool TraverseContinueStmt(clang::ContinueStmt *stmt);
  bool TraverseGotoStmt(clang::GotoStmt *stmt);

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
  bool VisitCXXBindTemporaryExpr(clang::CXXBindTemporaryExpr *expr);
  bool VisitCXXNewExpr(clang::CXXNewExpr *expr);
  bool VisitCXXInheritedCtorInitExpr(clang::CXXInheritedCtorInitExpr *expr);
  bool TraverseCXXDefaultInitExpr(clang::CXXDefaultInitExpr *expr);
#if VYCOR_LLVM_AT_LEAST(22)
  bool TraverseDecltypeTypeLoc(clang::DecltypeTypeLoc loc, bool qualifier);
  bool TraverseTypeOfExprTypeLoc(clang::TypeOfExprTypeLoc loc, bool qualifier);
#else
  bool TraverseDecltypeTypeLoc(clang::DecltypeTypeLoc loc);
  bool TraverseTypeOfExprTypeLoc(clang::TypeOfExprTypeLoc loc);
#endif

private:
  using Id = ReferenceIndex::Id;
  static constexpr uint32_t kDropped = UINT32_MAX;
  static constexpr uint32_t kUnresolved = UINT32_MAX - 1;

  // Transparent: a function that is no symbol of its own (an implicit
  // special member, an instantiation of a system-header template) whose
  // references are spliced into the referrers that use it. Never lowered.
  enum class EntityKind : uint8_t {
    Function,
    Variable,
    Class,
    Lambda,
    File,
    Transparent
  };

  // One referrer or target of this TU. Slots are dense and assigned in
  // traversal order.
  struct Entity {
    const clang::NamedDecl *decl = nullptr; // canonical; Lambda: the call
                                            // operator; File: null;
                                            // Transparent: the function
    clang::FileID file;                     // File only
    uint32_t aux = 0; // Lambda: index in lambdaIdents_; Transparent: ordinal
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
    bool inLoop = false; // Lambda: written inside a loop body
  };

  struct RefAcc {
    uint32_t count = 0;
    clang::SourceLocation site; // first occurrence in traversal order
    clang::SourceLocation unlockedSite; // first occurrence under no lock
    uint32_t unlockedSeq = 0;
    bool unlocked = false; // some occurrence was made under no lock
    bool locked = false;   // some occurrence was made under a lock
    uint8_t flags = 0;     // RefFlag bits
  };

  // ---- lock state -----------------------------------------------------------
  // One lock held at the current point of a body: a guard's mutex (guard
  // set) or a mutex locked by hand (guard null). An inactive entry is a
  // guard that does not own its mutex right now (defer_lock, unlock()).
  // An unknown lock a meet made (`met`: a different lock on each path)
  // lasts while `scope`, the guard the paths' locks belonged to, is in
  // scope (null: they were taken by hand), and an unlock of a mutex not
  // held drops it.
  struct HeldLock {
    uint32_t lock; // index in lockIdents_
    const clang::VarDecl *guard;
    bool active;
    bool met;
    const clang::VarDecl *scope;
  };
  using HeldLocks = llvm::SmallVector<HeldLock, 4>;
  struct LockState {
    HeldLocks held;
    unsigned loopDepth = 0;
  };
  // An enclosing loop or switch: the lock states at the breaks out of it
  // (and, for a loop, at the continues), met as they are reached.
  struct FlowFrame {
    bool isSwitch = false;
    bool anyBreak = false;
    bool anyContinue = false;
    HeldLocks breaks;
    HeldLocks continues;
  };
  // The lock states at the forward gotos to a label not reached yet, met.
  struct GotoState {
    bool any = false;
    HeldLocks held;
  };
  struct LockedOcc {
    uint32_t seq;               // occurrence order within the TU
    clang::SourceLocation site; // first occurrence under this lock set
  };

  // What the current traversal attributes to. AsWritten: the main pass.
  // Instantiation: the body of an instantiation whose pattern is a symbol;
  // references go to instAcc_ and are merged by maximum count. Transparent:
  // the body of a transparent function; references go to refs_ under its
  // slot and are spliced at lowering.
  enum class Mode : uint8_t { AsWritten, Instantiation, Transparent };
  struct Work {
    // A function's definition (a destructor: the decl), or the definition
    // of a variable template's or static data member's instantiation.
    const clang::Decl *decl;
    uint32_t slot;
    Mode mode;
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
    // With kMarkWrite: a non-const member call that a const overload could
    // have served, returning by value (the write is not kRefMutates).
    kMarkWeakWrite = 16,
  };

  // ---- traversal ----------------------------------------------------------
  bool traverseFunction(clang::FunctionDecl *fn);
  bool traverseVariable(clang::VarDecl *var);
  void popFrame();
  void noteFunctionDefinition(const clang::FunctionDecl *fn);
  void noteVariableDefinition(const clang::VarDecl *var);
  void noteClass(const clang::CXXRecordDecl *cls);
  void noteExplicitInstantiations(clang::FunctionTemplateDecl *tmpl);
  void noteExplicitInstantiations(clang::ClassTemplateDecl *tmpl);
  void noteExplicitMembers(const clang::CXXRecordDecl *cls);

  void mark(const clang::Expr *expr, uint8_t bits);
  static uint8_t objectMarks(const clang::CXXMethodDecl *method);
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
  void addRef(uint32_t target, RefRole role, clang::SourceLocation site,
              uint8_t flags = 0);
  void addRefFrom(uint32_t referrer, uint32_t target, RefRole role,
                  clang::SourceLocation site, uint32_t lockset, uint8_t flags);

  // ---- locks --------------------------------------------------------------
  void noteGuard(const clang::VarDecl *var);
  void noteLockCall(const clang::CallExpr *expr);
  [[nodiscard]] uint32_t mutexIdentity(const clang::Expr *expr);
  [[nodiscard]] uint32_t lockIdentity(const clang::NamedDecl *decl,
                                      llvm::StringRef prefix);
  [[nodiscard]] uint32_t unknownLock();
  [[nodiscard]] uint32_t onceLock(const clang::NamedDecl *decl);
  void acquire(uint32_t lock);
  void release(uint32_t lock);
  // Record that `lock` is acquired here (a lock or lock_shared record)
  // while `heldBefore` (a locksets_ index) is held. Not in a transparent
  // body, an unevaluated operand or code in a system header.
  void noteAcquire(uint32_t lock, RefRole role, clang::SourceLocation site,
                   uint32_t heldBefore);
  // Replace the held locks, invalidating the cached set only on a change.
  void assignHeld(const HeldLocks &held);
  // Must-hold meet of `b` into `a`; a different lock on each side leaves
  // an unknown lock held.
  void meetLocks(HeldLocks &a, const HeldLocks &b);
  void meetLocksInto(bool &any, HeldLocks &into, const HeldLocks &state);
  // Start the labels heading `stmt` from the states of the gotos to them
  // (falls: the statement above can reach this one).
  void enterLabels(const clang::Stmt *stmt, bool falls);
  // A loop's lock state after it, from the states before it and at the end
  // of its body (bodyFalls: the body can reach its end).
  void finishLoop(const HeldLocks &outer, bool zeroTrips, bool condTrue,
                  bool bodyFalls);
  [[nodiscard]] uint32_t currentLockset();
  // The held locks without once locks: what a lambda created here holds
  // when it is called later.
  [[nodiscard]] uint32_t currentLocksetWithoutOnce();
  [[nodiscard]] uint32_t internLockset(llvm::SmallVectorImpl<uint32_t> &locks);
  void enterBody(LockState &saved);
  void leaveBody(LockState &saved);
  [[nodiscard]] std::string
  lockText(const RefAcc &acc,
           llvm::ArrayRef<std::pair<uint32_t, LockedOcc>> locked);

  // ---- implicit code --------------------------------------------------------
  // The slot a reference to `fn` records: its symbol (scheduling the body of
  // an instantiation), a transparent slot, or kDropped. One lookup per
  // reference after the first.
  uint32_t functionTarget(const clang::FunctionDecl *fn);
  uint32_t transparentSlot(const clang::FunctionDecl *fn);
  void scheduleInstantiation(const clang::FunctionDecl *fn, uint32_t slot);
  void scheduleVariable(const clang::VarDecl *var, uint32_t slot);
  uint32_t lambdaPatternSlot(const clang::FunctionDecl *op) const;
  void scheduleLambda(const clang::FunctionDecl *op, uint32_t slot);
  void drainWork();
  void traverseWork(const Work &work);
  void traverseWorkFunction(const clang::FunctionDecl *fn, uint32_t slot);
  void traverseImplicitInits(const clang::CXXConstructorDecl *ctor);
  void noteImplicitDestruction(const clang::CXXDestructorDecl *dtor);
  void noteConstructorCleanup(const clang::CXXConstructorDecl *ctor);
  void noteDestructorOf(clang::QualType type, clang::SourceLocation site);
  void noteVariableDestruction(const clang::VarDecl *var);
  void noteVtableUse(const clang::CXXRecordDecl *cls,
                     clang::SourceLocation site);

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
  // The emission walk: fills pendingEmissions_ (before lower(), which may
  // need entity slots it adds).
  void collectEmissions();
  void spliceTransparent(std::vector<std::pair<uint64_t, RefAcc>> &refs,
                         std::vector<std::pair<uint64_t, uint64_t>> &lockVia);
  static void mergeLockFacts(RefAcc &acc, const RefAcc &other);
  [[nodiscard]] Id lowerEntity(const Entity &entity,
                               std::vector<ReferenceIndex::SymbolRecord> &out);

  // One emitted body, lowered with the symbols.
  struct PendingEmission {
    const clang::FunctionDecl *def;
    uint32_t slot;     // its entity, or kDropped
    uint32_t size;     // EmissionGraph::statements
    uint8_t flags;     // EmissionFlag bits
  };

  ReferenceIndex &index_;
  clang::SourceManager &sm_;
  std::vector<PendingEmission> pendingEmissions_;
  clang::ASTContext *ctx_ = nullptr;
  std::string tuPath_;
  UsrCache usrs_;
  Mode mode_ = Mode::AsWritten;

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

  // Implicit code. Referenced decl -> functionTarget().
  llvm::DenseMap<const clang::Decl *, uint32_t> funcSlot_;
  llvm::DenseMap<const clang::Decl *, uint32_t> transparentSlot_;
  llvm::DenseSet<const clang::Decl *> scheduled_;
  llvm::DenseSet<const clang::Decl *> vtableClasses_;
  std::vector<Work> work_;
  uint32_t transparentCount_ = 0;
  // The instantiation being read, and the maximum over those read so far.
  llvm::DenseMap<uint64_t, RefAcc> instAcc_;
  llvm::DenseMap<uint64_t, RefAcc> instRefs_;
  const LockTypeConfig *lockCfg_;
  LockState lockState_;
  std::vector<FlowFrame> flow_; // innermost last
  llvm::DenseMap<const clang::LabelDecl *, GotoState> gotoStates_;
  uint32_t heldSet_ = 0;       // index in locksets_ of the active held locks
  bool heldSetValid_ = true;
  uint32_t seq_ = 0;
  std::vector<Ident> lockIdents_;           // usr = identity, display
  llvm::StringMap<uint32_t> lockIdentOf_;   // identity -> index
  uint32_t unknownLock_ = UINT32_MAX;
  // Lock sets: sorted lockIdents_ indices; set 0 is the empty set.
  std::vector<std::vector<uint32_t>> locksets_{{}};
  std::map<std::vector<uint32_t>, uint32_t> locksetOf_;
  // Lambdas handed to std::call_once -> the once lock their body holds.
  llvm::DenseMap<const clang::LambdaExpr *, uint32_t> onceLambdas_;
  // Functions named as the callable of std::call_once -> the once lock
  // their address is taken under (the call graph's edge to them reads it).
  llvm::DenseMap<const clang::Expr *, uint32_t> onceCallables_;
  // (refs_ key, lock set) -> first occurrence, for occurrences under a lock.
  llvm::DenseMap<std::pair<uint64_t, uint32_t>, LockedOcc> lockedOcc_;
  // Lock acquisitions, keyed like refs_ with a lockIdents_ index as the
  // target: as written, and what only instantiations hold. Occurrences
  // under a lock (the set held before) are in acqOcc_.
  llvm::DenseMap<uint64_t, RefAcc> acqs_;
  llvm::DenseMap<uint64_t, RefAcc> instAcqs_;
  llvm::DenseMap<std::pair<uint64_t, uint32_t>, LockedOcc> acqOcc_;
  // RefFlag recursion bits per lockIdents_ index, from the declared type.
  std::vector<uint8_t> lockFlags_;
};

} // namespace vycor
