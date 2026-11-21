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

#pragma once

#include "vycor/callgraph/ControlFlowIndex.h"

#include "clang/AST/Type.h"

#include <optional>

namespace clang {
class CXXConstructExpr;
class CXXRecordDecl;
class DeclContext;
} // namespace clang

namespace vycor {

// ============================================================================
// Lock recognition shared by the bake visitors: which local is a lock
// guard, which object is a mutex, and which global is a synchronization
// primitive rather than data. The control-flow visitor reads guards as RAII
// locals (query_locks_held); the reference visitor reads them to record the
// locks held at each reference (ReferenceIndex lock facts).
// ============================================================================

// Classify a type as an RAII local: Lock for a recognized lock type (the
// built-in std/absl guards when cfg.useBuiltins, cfg.userAllowlist, or a
// class carrying a thread-safety capability or scoped_lockable attribute),
// SmartPtr for std::unique_ptr/shared_ptr, Other for any other class with a
// non-trivial destructor; nullopt for anything else.
[[nodiscard]] std::optional<RaiiKind>
classifyRaiiType(clang::QualType type, const LockTypeConfig &cfg);

[[nodiscard]] bool inStdNamespace(const clang::DeclContext *dc);

// A mutex: the std mutex family, a class with a thread-safety capability
// attribute, a class whose name says mutex, spin lock, rw lock or critical
// section (not a guard: a class constructed from a reference or pointer to
// a mutex, whatever its name, or one named like a guard: MutexLock,
// ScopedSpinLock, ...Guard), a class shaped like one (lock() and unlock()
// members, own or inherited, not constructed from an object it guards), or
// a pthread mutex, rwlock or spinlock. References are looked through.
[[nodiscard]] bool isMutexType(clang::QualType type);
// isMutexType, or a pointer to such a type.
[[nodiscard]] bool isMutexLike(clang::QualType type);

// A guard type: what classifyRaiiType calls a Lock, minus a capability
// class (that is the mutex itself). A local of another class type is also
// read as a guard when it is constructed from a mutex and has a
// non-trivial destructor (llvm::sys::ScopedLock and every hand-rolled
// guard): isHeuristicGuard.
[[nodiscard]] bool isGuardType(clang::QualType type, const LockTypeConfig &cfg);
[[nodiscard]] bool isHeuristicGuard(const clang::CXXRecordDecl *cls);

// A synchronization primitive rather than data: a mutex, a condition
// variable, a once flag, a semaphore, latch or barrier (std or pthread).
// Element types of arrays are looked through.
[[nodiscard]] bool isSyncPrimitiveType(const clang::ASTContext &ctx,
                                       clang::QualType type);

// Whether a mutex type is recursive, as lock records carry it (RefFlag):
// kRefRecursive for std::recursive_mutex / std::recursive_timed_mutex, a
// class whose name says recursive, or one holding a recursive standard
// mutex (llvm::sys::SmartMutex); kRefNonRecursive for std::mutex,
// std::timed_mutex, std::shared_mutex, std::shared_timed_mutex and
// absl::Mutex; 0 when not known (a pthread mutex takes its kind at run
// time). Pointers and references are looked through.
[[nodiscard]] uint8_t lockRecursionFlags(clang::QualType type);

// A guard that takes its mutex shared: std::shared_lock, or a guard named
// as a reader (absl::ReaderMutexLock, llvm::sys::ScopedReader,
// ...ReadLock, ...ReadScope, ...SharedLock, ...ReadGuard).
[[nodiscard]] bool isSharedGuardType(clang::QualType type);

// The ownership tag a guard constructor names, if any.
enum class LockTag : uint8_t { None, Defer, TryToLock, Adopt };
[[nodiscard]] LockTag lockTagOf(clang::QualType argType);

} // namespace vycor
