// Copyright (c) 2026 The vycor-cpp Authors
// SPDX-License-Identifier: Apache-2.0
#include "vycor/check/CallbackUnderLock.h"
#include "vycor/check/GlobalIndex.h"
#include "vycor/check/Indexer.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/StmtCXX.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Index/USRGeneration.h"
#include "llvm/ADT/SmallString.h"
#include <algorithm>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace vycor {
namespace {
using Function = const clang::FunctionDecl *;
using Variable = const clang::VarDecl *;

bool inStd(const clang::DeclContext *dc) {
  for (; dc; dc = dc->getParent()) {
    const auto *ns = llvm::dyn_cast<clang::NamespaceDecl>(dc);
    if (!ns)
      return false;
    if (ns->getName() == "std")
      return true;
    if (!ns->isInline())
      return false;
  }
  return false;
}
const clang::Expr *strip(const clang::Expr *e) {
  return e ? e->IgnoreParenImpCasts() : nullptr;
}
const clang::CXXConstructExpr *construction(const clang::Expr *e) {
  e = strip(e);
  if (const auto *cleanup = llvm::dyn_cast_or_null<clang::ExprWithCleanups>(e))
    return construction(cleanup->getSubExpr());
  return llvm::dyn_cast_or_null<clang::CXXConstructExpr>(e);
}
std::string nonrecursiveMutex(const clang::TemplateArgument &arg) {
  if (arg.getKind() == clang::TemplateArgument::Pack) {
    for (const auto &part : arg.pack_elements()) {
      auto result = nonrecursiveMutex(part);
      if (!result.empty())
        return result;
    }
  }
  if (arg.getKind() != clang::TemplateArgument::Type)
    return "";
  const auto *rd = arg.getAsType()->getAsCXXRecordDecl();
  if (!rd || !inStd(rd->getDeclContext()))
    return "";
  auto name = rd->getName();
  if (name == "mutex" || name == "timed_mutex" || name == "shared_mutex" ||
      name == "shared_timed_mutex")
    return "std::" + name.str();
  return "";
}
struct Guard {
  std::string name, mutex, location;
  bool held = false;
};
using Locks = std::map<Variable, Guard>;
struct Call {
  Function target = nullptr;
  std::string callback; // Nonempty only at an application callback boundary.
  std::string location;
  bool virtualDispatch = false;
  std::vector<Guard> locks;
};
struct Body {
  Function decl = nullptr;
  std::vector<Call> calls;
};
class Analysis : public clang::RecursiveASTVisitor<Analysis> {
public:
  explicit Analysis(clang::ASTContext &context)
      : context(context), sm(context.getSourceManager()) {}
  bool shouldVisitTemplateInstantiations() const { return true; }
  bool VisitFunctionDecl(clang::FunctionDecl *fn) {
    if (!fn->doesThisDeclarationHaveABody() || fn->isDependentContext() ||
        sm.isInSystemHeader(sm.getExpansionLoc(fn->getLocation())))
      return true;
    auto key = fn->getCanonicalDecl();
    if (bodies.count(key))
      return true;
    Body body;
    body.decl = fn;
    Locks locks;
    walk(fn->getBody(), locks, body.calls);
    bodies.emplace(key, std::move(body));
    return true;
  }
  bool VisitCXXMethodDecl(clang::CXXMethodDecl *method) {
    if (sm.isInSystemHeader(sm.getExpansionLoc(method->getLocation())))
      return true;
    for (auto *base : method->overridden_methods())
      overrides[base->getCanonicalDecl()].insert(method->getCanonicalDecl());
    return true;
  }
  void emit(std::vector<Diagnostic> &diagnostics);

private:
  clang::ASTContext &context;
  clang::SourceManager &sm;
  std::map<Function, Body> bodies;
  std::map<Function, std::set<Function>> overrides;

  std::string location(clang::SourceLocation loc) const {
    loc = sm.getExpansionLoc(loc);
    if (!loc.isValid())
      return "";
    return absoluteFileName(sm, sm.getFilename(loc)) + ":" +
           std::to_string(sm.getExpansionLineNumber(loc)) + ":" +
           std::to_string(sm.getExpansionColumnNumber(loc));
  }
  std::string identity(Function fn) const {
    llvm::SmallString<128> usr;
    if (!clang::index::generateUSRForDecl(fn, usr))
      return std::string(usr);
    return orderKey(fn);
  }
  std::string functionName(Function fn) const {
    return fn->getQualifiedNameAsString();
  }
  std::string orderKey(Function fn) const {
    return functionName(fn) + "@" + location(fn->getLocation()) + "|" +
           fn->getType().getAsString();
  }
  void addGuard(const clang::VarDecl *var, Locks &locks) const {
    const auto *type =
        llvm::dyn_cast_or_null<clang::ClassTemplateSpecializationDecl>(
            var->getType()->getAsCXXRecordDecl());
    if (!type || !inStd(type->getDeclContext()))
      return;
    auto name = type->getName();
    if (name != "lock_guard" && name != "unique_lock" &&
        name != "scoped_lock" && name != "shared_lock")
      return;
    std::string mutex;
    for (const auto &arg : type->getTemplateArgs().asArray()) {
      mutex = nonrecursiveMutex(arg);
      if (!mutex.empty())
        break;
    }
    if (mutex.empty())
      return; // Recursive, null, dependent and custom mutexes are not guessed.
    const auto *init = construction(var->getInit());
    if (!init || init->getNumArgs() == 0 ||
        init->getConstructor()->isCopyOrMoveConstructor())
      return;
    // unique/shared timed constructors, like ownership tags, do not prove
    // acquisition. scoped_lock can acquire multiple mutexes unconditionally.
    bool held = name == "scoped_lock" || init->getNumArgs() == 1;
    for (const auto *arg : init->arguments()) {
      const auto *rd = arg->getType()->getAsCXXRecordDecl();
      if (rd && inStd(rd->getDeclContext()) &&
          (rd->getName() == "defer_lock_t" ||
           rd->getName() == "try_to_lock_t" || rd->getName() == "adopt_lock_t"))
        held = false;
    }
    locks[var->getCanonicalDecl()] = {var->getNameAsString(), mutex,
                                      location(var->getLocation()), held};
  }
  std::string callbackName(const clang::Expr *expr) const {
    expr = strip(expr);
    if (const auto *ref = llvm::dyn_cast_or_null<clang::DeclRefExpr>(expr))
      return ref->getDecl()->getNameAsString();
    if (const auto *member = llvm::dyn_cast_or_null<clang::MemberExpr>(expr))
      return member->getMemberDecl()->getQualifiedNameAsString();
    return "callback";
  }
  std::string callback(const clang::CallExpr *call) const {
    if (!call->getDirectCallee()) {
      auto type = call->getCallee()->getType();
      if (type->isFunctionPointerType() || type->isFunctionType())
        return "function pointer " + callbackName(call->getCallee());
    }
    const auto *method =
        llvm::dyn_cast_or_null<clang::CXXMethodDecl>(call->getDirectCallee());
    if (!method || !method->isOverloadedOperator() ||
        method->getOverloadedOperator() != clang::OO_Call)
      return "";
    const clang::Expr *object = nullptr;
    if (const auto *op = llvm::dyn_cast<clang::CXXOperatorCallExpr>(call)) {
      if (op->getNumArgs())
        object = strip(op->getArg(0));
    } else if (const auto *member =
                   llvm::dyn_cast<clang::CXXMemberCallExpr>(call)) {
      object = strip(member->getImplicitObjectArgument());
    }
    const auto *parent = method->getParent();
    if (inStd(parent->getDeclContext())) {
      if (parent->getName() == "function" ||
          parent->getName() == "move_only_function")
        return "std::" + parent->getNameAsString() + " " + callbackName(object);
      return "";
    }
    // Calling a supplied/stored functor is a callback boundary, including
    // instantiated lambda parameters. An immediately invoked local lambda is
    // not.
    if (const auto *ref = llvm::dyn_cast_or_null<clang::DeclRefExpr>(object))
      if (llvm::isa<clang::ParmVarDecl>(ref->getDecl()) && !method->hasBody())
        return ref->getDecl()->getNameAsString();
    if (const auto *member = llvm::dyn_cast_or_null<clang::MemberExpr>(object))
      if (llvm::isa<clang::FieldDecl>(member->getMemberDecl()))
        return member->getMemberDecl()->getQualifiedNameAsString();
    return "";
  }
  static void join(Locks &left, const Locks &right) {
    for (auto &[var, guard] : left) {
      auto found = right.find(var);
      if (found != right.end())
        guard.held |= found->second.held;
    }
  }
  void escapeGuardReferences(const clang::Stmt *stmt, Locks &locks) const {
    if (!stmt)
      return;
    if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(stmt)) {
      if (const auto *var = llvm::dyn_cast<clang::VarDecl>(ref->getDecl())) {
        auto found = locks.find(var->getCanonicalDecl());
        if (found != locks.end())
          found->second.held = false;
      }
    }
    // The expression is an argument, not an executed lambda body.
    if (llvm::isa<clang::LambdaExpr>(stmt))
      return;
    for (const auto *child : stmt->children())
      escapeGuardReferences(child, locks);
  }
  // false means no normal fallthrough. The state tracks possible ownership;
  // branch-local guards are removed by CompoundStmt before joining.
  bool walk(const clang::Stmt *stmt, Locks &locks, std::vector<Call> &calls) {
    if (!stmt)
      return true;
    if (const auto *lambda = llvm::dyn_cast<clang::LambdaExpr>(stmt)) {
      for (const auto *capture : lambda->capture_inits())
        walk(capture, locks, calls);
      return true; // Its body executes only through a call edge.
    }
    if (const auto *compound = llvm::dyn_cast<clang::CompoundStmt>(stmt)) {
      std::set<Variable> outer;
      for (const auto &entry : locks)
        outer.insert(entry.first);
      bool falls = true;
      for (const auto *child : compound->body())
        if (!walk(child, locks, calls)) {
          falls = false;
          break;
        }
      for (auto it = locks.begin(); it != locks.end();) {
        if (!outer.count(it->first))
          it = locks.erase(it);
        else
          ++it;
      }
      return falls;
    }
    if (const auto *decl = llvm::dyn_cast<clang::DeclStmt>(stmt)) {
      for (const auto *d : decl->decls())
        if (const auto *var = llvm::dyn_cast<clang::VarDecl>(d)) {
          walk(var->getInit(), locks,
               calls); // Acquisition follows initializer evaluation.
          if (var->getType()->isReferenceType() ||
              var->getType()->isPointerType())
            escapeGuardReferences(var->getInit(), locks);
          addGuard(var, locks);
        }
      return true;
    }
    if (const auto *branch = llvm::dyn_cast<clang::IfStmt>(stmt)) {
      Locks base = locks;
      walk(branch->getInit(), locks, calls);
      walk(branch->getConditionVariableDeclStmt(), locks, calls);
      walk(branch->getCond(), locks, calls);
      Locks other = locks;
      bool value = false;
      bool known =
          branch->getCond()->EvaluateAsBooleanCondition(value, context);
      bool thenFalls =
          (!known || value) && walk(branch->getThen(), locks, calls);
      bool elseFalls =
          (!known || !value) && walk(branch->getElse(), other, calls);
      if (!thenFalls)
        locks = other;
      else if (elseFalls)
        join(locks, other);
      // An if-init guard ends with the entire if statement.
      for (auto it = locks.begin(); it != locks.end();) {
        if (!base.count(it->first))
          it = locks.erase(it);
        else
          ++it;
      }
      return thenFalls || elseFalls;
    }
    if (const auto *loop = llvm::dyn_cast<clang::WhileStmt>(stmt)) {
      walk(loop->getCond(), locks, calls);
      bool value = false;
      if (loop->getCond()->EvaluateAsBooleanCondition(value, context) && !value)
        return true;
      Locks iteration = locks;
      walk(loop->getBody(), iteration, calls);
      join(locks, iteration);
      return true;
    }
    if (const auto *loop = llvm::dyn_cast<clang::ForStmt>(stmt)) {
      Locks base = locks;
      walk(loop->getInit(), locks, calls);
      walk(loop->getCond(), locks, calls);
      Locks iteration = locks;
      if (walk(loop->getBody(), iteration, calls))
        walk(loop->getInc(), iteration, calls);
      join(locks, iteration);
      for (auto it = locks.begin(); it != locks.end();) {
        if (!base.count(it->first))
          it = locks.erase(it);
        else
          ++it;
      }
      return true;
    }
    if (const auto *attempt = llvm::dyn_cast<clang::CXXTryStmt>(stmt)) {
      Locks base = locks;
      bool falls = walk(attempt->getTryBlock(), locks, calls);
      for (unsigned i = 0; i < attempt->getNumHandlers(); ++i) {
        Locks handler = base;
        if (walk(attempt->getHandler(i)->getHandlerBlock(), handler, calls)) {
          if (!falls)
            locks = handler;
          else
            join(locks, handler);
          falls = true;
        }
      }
      return falls;
    }
    if (const auto *binary = llvm::dyn_cast<clang::BinaryOperator>(stmt)) {
      if (binary->isLogicalOp()) {
        walk(binary->getLHS(), locks, calls);
        bool value = false;
        bool known =
            binary->getLHS()->EvaluateAsBooleanCondition(value, context);
        bool evaluate = binary->getOpcode() == clang::BO_LAnd ? value : !value;
        if (known) {
          if (evaluate)
            walk(binary->getRHS(), locks, calls);
        } else {
          Locks conditional = locks;
          walk(binary->getRHS(), conditional, calls);
          join(locks, conditional);
        }
        return true;
      }
    }
    if (const auto *choice = llvm::dyn_cast<clang::ConditionalOperator>(stmt)) {
      walk(choice->getCond(), locks, calls);
      bool value = false;
      if (choice->getCond()->EvaluateAsBooleanCondition(value, context))
        walk(value ? choice->getTrueExpr() : choice->getFalseExpr(), locks,
             calls);
      else {
        Locks other = locks;
        walk(choice->getTrueExpr(), locks, calls);
        walk(choice->getFalseExpr(), other, calls);
        join(locks, other);
      }
      return true;
    }
    // C++ arguments execute before entering the called function. Process
    // unlock/lock after their argument expressions and before the next
    // statement.
    for (const auto *child : stmt->children())
      walk(child, locks, calls);
    if (const auto *call = llvm::dyn_cast<clang::CallExpr>(stmt)) {
      if (const auto *member = llvm::dyn_cast<clang::CXXMemberCallExpr>(call)) {
        if (const auto *ref = llvm::dyn_cast_or_null<clang::DeclRefExpr>(
                strip(member->getImplicitObjectArgument()))) {
          if (const auto *var =
                  llvm::dyn_cast<clang::VarDecl>(ref->getDecl())) {
            auto guard = locks.find(var->getCanonicalDecl());
            if (guard != locks.end() && member->getMethodDecl()) {
              auto name = member->getMethodDecl()->getNameAsString();
              if (name == "unlock")
                guard->second.held = false;
              if (name == "lock")
                guard->second.held = true;
              if (name == "lock" || name == "unlock")
                return true;
            }
          }
        }
      }
      for (const auto *arg : call->arguments())
        escapeGuardReferences(arg, locks);
      Call edge;
      edge.target = call->getDirectCallee();
      if (edge.target)
        edge.target = edge.target->getCanonicalDecl();
      edge.callback = callback(call);
      edge.location = location(call->getExprLoc());
      if (const auto *member = llvm::dyn_cast<clang::CXXMemberCallExpr>(call)) {
        const auto *method = member->getMethodDecl();
        const auto *expr =
            llvm::dyn_cast<clang::MemberExpr>(strip(member->getCallee()));
        edge.virtualDispatch =
            method && method->isVirtual() && expr && !expr->hasQualifier();
      }
      if (const auto *op = llvm::dyn_cast<clang::CXXOperatorCallExpr>(call))
        if (const auto *method = llvm::dyn_cast_or_null<clang::CXXMethodDecl>(
                op->getDirectCallee()))
          edge.virtualDispatch = method->isVirtual();
      for (const auto &[var, guard] : locks) {
        (void)var;
        if (guard.held)
          edge.locks.push_back(guard);
      }
      std::sort(
          edge.locks.begin(), edge.locks.end(),
          [](const auto &a, const auto &b) { return a.location < b.location; });
      calls.push_back(std::move(edge));
    }
    return !llvm::isa<clang::ReturnStmt>(stmt) &&
           !llvm::isa<clang::CXXThrowExpr>(stmt) &&
           !llvm::isa<clang::BreakStmt>(stmt) &&
           !llvm::isa<clang::ContinueStmt>(stmt);
  }
  std::vector<Function> targets(const Call &edge) const {
    std::set<Function> visited;
    std::vector<Function> work;
    if (edge.target)
      work.push_back(edge.target);
    for (size_t i = 0; i < work.size(); ++i) {
      if (!visited.insert(work[i]).second || !edge.virtualDispatch)
        continue;
      auto found = overrides.find(work[i]);
      if (found != overrides.end())
        work.insert(work.end(), found->second.begin(), found->second.end());
    }
    work.assign(visited.begin(), visited.end());
    std::sort(work.begin(), work.end(),
              [&](auto a, auto b) { return orderKey(a) < orderKey(b); });
    return work;
  }
};

void Analysis::emit(std::vector<Diagnostic> &diagnostics) {
  std::vector<Function> functions;
  for (const auto &entry : bodies)
    functions.push_back(entry.first);
  std::sort(functions.begin(), functions.end(),
            [&](auto a, auto b) { return orderKey(a) < orderKey(b); });
  for (auto root : functions)
    for (const auto &seed : bodies.at(root).calls) {
      if (seed.locks.empty())
        continue;
      struct Step {
        Function function;
        const Call *call;
      };
      struct Pending {
        Function function;
        std::vector<Step> path;
      };
      std::deque<Pending> queue;
      std::set<Function> visited;
      std::vector<std::vector<Step>> witnesses;
      if (!seed.callback.empty())
        witnesses.push_back({{root, &seed}});
      else
        for (auto target : targets(seed))
          queue.push_back({target, {{root, &seed}}});
      while (!queue.empty()) {
        auto current = std::move(queue.front());
        queue.pop_front();
        if (!visited.insert(current.function).second)
          continue;
        auto found = bodies.find(current.function);
        if (found == bodies.end())
          continue;
        for (const auto &call : found->second.calls) {
          auto path = current.path;
          path.push_back({current.function, &call});
          if (!call.callback.empty())
            witnesses.push_back(std::move(path));
          else
            for (auto target : targets(call))
              queue.push_back({target, path});
        }
      }
      for (const auto &path : witnesses)
        for (const auto &guard : seed.locks) {
          Diagnostic diagnostic;
          diagnostic.kind = Diagnostic::Custom;
          diagnostic.checkName = "callback-under-lock";
          diagnostic.scope = identity(root);
          diagnostic.callLocation = seed.location;
          diagnostic.entities = {identity(root), guard.name, guard.mutex,
                                 functionName(path.back().function),
                                 path.back().call->callback};
          diagnostic.message = "Callback under nonrecursive " + guard.mutex +
                               " guard '" + guard.name + "' (acquired at " +
                               guard.location + "): ";
          for (const auto &step : path)
            diagnostic.message += functionName(step.function) + " at " +
                                  step.call->location + " -> ";
          diagnostic.message +=
              path.back().call->callback +
              ". Reentering code that acquires this mutex may deadlock; invoke "
              "callbacks after releasing the guard or document a non-reentrant "
              "callback contract. Virtual paths are possible dispatch targets, "
              "not proof of runtime reentry.";
          diagnostics.push_back(std::move(diagnostic));
        }
    }
}
} // namespace
void analyzeCallbacksUnderLock(clang::ASTContext &context,
                               std::vector<Diagnostic> &diagnostics) {
  Analysis analysis(context);
  analysis.TraverseDecl(context.getTranslationUnitDecl());
  analysis.emit(diagnostics);
}
} // namespace vycor
