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

#include "vycor/callgraph/ReferenceIndex.h"

#include "vycor/callgraph/LockFacts.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/DeclCXX.h"

namespace vycor {

bool inStdNamespace(const clang::DeclContext *dc) {
  for (; dc; dc = dc->getParent())
    if (dc->isStdNamespace())
      return true;
  return false;
}

namespace {

[[nodiscard]] const clang::CXXRecordDecl *recordOf(clang::QualType type) {
  if (type.isNull())
    return nullptr;
  return type.getNonReferenceType()->getAsCXXRecordDecl();
}

// The typedef a pthread object is spelled with (pthread_mutex_t is an
// anonymous union or struct behind it).
[[nodiscard]] bool isPthreadType(clang::QualType type,
                                 std::initializer_list<llvm::StringRef> names) {
  type = type.getNonReferenceType();
  while (const auto *typedefType = type->getAs<clang::TypedefType>()) {
    const llvm::StringRef name = typedefType->getDecl()->getName();
    for (llvm::StringRef n : names)
      if (name == n)
        return true;
    type = typedefType->desugar();
  }
  return false;
}

[[nodiscard]] bool isStdNamed(const clang::CXXRecordDecl *cls,
                              std::initializer_list<llvm::StringRef> names) {
  if (!cls || !inStdNamespace(cls->getDeclContext()))
    return false;
  const clang::IdentifierInfo *id = cls->getIdentifier();
  if (!id)
    return false;
  for (llvm::StringRef n : names)
    if (id->getName() == n)
      return true;
  return false;
}

// The class or one of its bases declares a member named `a` or `b`.
[[nodiscard]] bool hasMember(const clang::CXXRecordDecl *def,
                             llvm::StringRef a, llvm::StringRef b,
                             unsigned depth = 0) {
  for (const clang::CXXMethodDecl *method : def->methods()) {
    const clang::IdentifierInfo *id = method->getIdentifier();
    if (id && !method->isStatic() && (id->getName() == a || id->getName() == b))
      return true;
  }
  if (depth >= 8)
    return false;
  for (const clang::CXXBaseSpecifier &base : def->bases())
    if (const clang::CXXRecordDecl *cls = base.getType()->getAsCXXRecordDecl())
      if (const clang::CXXRecordDecl *baseDef = cls->getDefinition())
        if (hasMember(baseDef, a, b, depth + 1))
          return true;
  return false;
}

// A class shaped like a mutex (BasicLockable): it has lock() and unlock()
// members, or Lock() and Unlock(), its own or inherited, and is not
// constructed from another object it would be guarding (a guard with
// relock members).
[[nodiscard]] bool isBasicLockable(const clang::CXXRecordDecl *cls) {
  const clang::CXXRecordDecl *def = cls->getDefinition();
  if (!def || def->isInvalidDecl())
    return false;
  if (!hasMember(def, "lock", "Lock") || !hasMember(def, "unlock", "Unlock"))
    return false;
  for (const clang::CXXConstructorDecl *ctor : def->ctors()) {
    if (ctor->isCopyOrMoveConstructor())
      continue;
    for (const clang::ParmVarDecl *param : ctor->parameters()) {
      const clang::QualType type = param->getType();
      if ((type->isReferenceType() || type->isPointerType()) &&
          type->getPointeeType()->isRecordType())
        return false;
    }
  }
  return true;
}

[[nodiscard]] bool isMutexTypeImpl(clang::QualType type, unsigned depth);

// A guard by shape, whatever its name: a constructor other than copy and
// move takes a reference or pointer to a mutex (ScopedSpinLock(SpinLock&),
// MutexLock(Mutex*)).
[[nodiscard]] bool constructedFromMutex(const clang::CXXRecordDecl *cls,
                                        unsigned depth) {
  const clang::CXXRecordDecl *def = cls->getDefinition();
  if (!def || def->isInvalidDecl() || depth >= 2)
    return false;
  for (const clang::CXXConstructorDecl *ctor : def->ctors()) {
    if (ctor->isCopyOrMoveConstructor())
      continue;
    for (const clang::ParmVarDecl *param : ctor->parameters()) {
      const clang::QualType type = param->getType();
      if ((type->isReferenceType() || type->isPointerType()) &&
          type->getPointeeType()->isRecordType() &&
          isMutexTypeImpl(type->getPointeeType(), depth + 1))
        return true;
    }
  }
  return false;
}

bool isMutexTypeImpl(clang::QualType type, unsigned depth) {
  if (type.isNull())
    return false;
  if (isPthreadType(type, {"pthread_mutex_t", "pthread_rwlock_t",
                           "pthread_spinlock_t"}))
    return true;
  const clang::CXXRecordDecl *cls = recordOf(type);
  if (!cls)
    return false;
  if (isStdNamed(cls, {"mutex", "recursive_mutex", "timed_mutex",
                       "recursive_timed_mutex", "shared_mutex",
                       "shared_timed_mutex"}))
    return true;
  if (const clang::CXXRecordDecl *def = cls->getDefinition())
    if (def->hasAttr<clang::CapabilityAttr>())
      return true;
  if (inStdNamespace(cls->getDeclContext()))
    return false; // std guards and the rest of the library are not mutexes
  // Shape before name: a class constructed from a mutex guards it.
  if (constructedFromMutex(cls, depth))
    return false;
  const clang::IdentifierInfo *id = cls->getIdentifier();
  if (!id)
    return false;
  const llvm::StringRef name = id->getName();
  // A guard named after its mutex (MutexLock, MutexGuard, mutex_lock,
  // ScopedSpinLock, RWLockReadScope) is not one.
  const bool namesMutex = name.contains_insensitive("mutex");
  if (name.ends_with("Guard") || name.ends_with("_guard") ||
      name.ends_with("Locker") || name.ends_with("Holder") ||
      name.starts_with("Scoped") || name.ends_with("Scope") ||
      (namesMutex && (name.ends_with("Lock") || name.ends_with("_lock"))))
    return false;
  if (namesMutex || name.contains_insensitive("spinlock") ||
      name.contains_insensitive("spin_lock") ||
      name.contains_insensitive("rwlock") ||
      name.contains_insensitive("rw_lock") || name.contains("CriticalSection"))
    return true;
  return isBasicLockable(cls);
}

} // namespace

bool isMutexType(clang::QualType type) { return isMutexTypeImpl(type, 0); }

bool isMutexLike(clang::QualType type) {
  if (type.isNull())
    return false;
  type = type.getNonReferenceType();
  if (const auto *pointer = type->getAs<clang::PointerType>())
    type = pointer->getPointeeType();
  return isMutexType(type);
}

bool isHeuristicGuard(const clang::CXXRecordDecl *cls) {
  if (!cls)
    return false;
  const clang::CXXRecordDecl *def = cls->getDefinition();
  if (!def || def->isInvalidDecl() || def->isDependentContext())
    return false;
  const clang::CXXDestructorDecl *dtor = def->getDestructor();
  return dtor && !dtor->isTrivial();
}

bool isGuardType(clang::QualType type, const LockTypeConfig &cfg) {
  if (type.isNull() || type->isReferenceType())
    return false;
  const auto kind = classifyRaiiType(type, cfg);
  if (!kind || *kind != RaiiKind::Lock)
    return false;
  const clang::CXXRecordDecl *cls = recordOf(type);
  const clang::CXXRecordDecl *def = cls ? cls->getDefinition() : nullptr;
  return !(def && def->hasAttr<clang::CapabilityAttr>() &&
           !def->hasAttr<clang::ScopedLockableAttr>());
}

bool isSyncPrimitiveType(const clang::ASTContext &ctx, clang::QualType type) {
  if (type.isNull())
    return false;
  type = ctx.getBaseElementType(type.getNonReferenceType());
  if (isMutexType(type))
    return true;
  if (isPthreadType(type, {"pthread_cond_t", "pthread_once_t",
                           "pthread_barrier_t"}))
    return true;
  return isStdNamed(recordOf(type),
                    {"condition_variable", "condition_variable_any",
                     "once_flag", "counting_semaphore", "latch", "barrier",
                     "stop_source"});
}

uint8_t lockRecursionFlags(clang::QualType type) {
  if (type.isNull())
    return 0;
  type = type.getNonReferenceType();
  if (const auto *pointer = type->getAs<clang::PointerType>())
    type = pointer->getPointeeType();
  const clang::CXXRecordDecl *cls = recordOf(type);
  if (!cls || !cls->getIdentifier())
    return 0;
  if (isStdNamed(cls, {"recursive_mutex", "recursive_timed_mutex"}))
    return kRefRecursive;
  if (isStdNamed(cls, {"mutex", "timed_mutex", "shared_mutex",
                       "shared_timed_mutex"}))
    return kRefNonRecursive;
  if (inStdNamespace(cls->getDeclContext()))
    return 0;
  const llvm::StringRef name = cls->getName();
  if (name.contains_insensitive("recursive"))
    return kRefRecursive;
  if (name == "Mutex") {
    for (const clang::DeclContext *dc = cls->getDeclContext(); dc;
         dc = dc->getParent())
      if (const auto *ns = llvm::dyn_cast<clang::NamespaceDecl>(dc);
          ns && ns->getIdentifier() && ns->getName() == "absl")
        return kRefNonRecursive;
  }
  // A class built on a recursive standard mutex (llvm::sys::SmartMutex).
  if (const clang::CXXRecordDecl *def = cls->getDefinition();
      def && !def->isDependentContext())
    for (const clang::FieldDecl *field : def->fields())
      if (isStdNamed(recordOf(field->getType()),
                     {"recursive_mutex", "recursive_timed_mutex"}))
        return kRefRecursive;
  return 0;
}

bool isSharedGuardType(clang::QualType type) {
  const clang::CXXRecordDecl *cls = recordOf(type);
  if (!cls || !cls->getIdentifier())
    return false;
  if (inStdNamespace(cls->getDeclContext()))
    return cls->getName() == "shared_lock";
  const llvm::StringRef name = cls->getName();
  return name.contains("Reader") || name.contains("ReadLock") ||
         name.contains("ReadScope") || name.contains("SharedLock") ||
         name.contains("ReadGuard");
}

LockTag lockTagOf(clang::QualType argType) {
  const clang::CXXRecordDecl *cls = recordOf(argType);
  if (!cls || !inStdNamespace(cls->getDeclContext()) || !cls->getIdentifier())
    return LockTag::None;
  const llvm::StringRef name = cls->getName();
  if (name == "defer_lock_t")
    return LockTag::Defer;
  if (name == "try_to_lock_t")
    return LockTag::TryToLock;
  if (name == "adopt_lock_t")
    return LockTag::Adopt;
  return LockTag::None;
}

} // namespace vycor
