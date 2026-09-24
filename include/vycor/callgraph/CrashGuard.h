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

#include "llvm/ADT/STLFunctionalExtras.h"

namespace vycor {

// ============================================================================
// In-process crash guard for TU parses (docs/design-f12-subprocess-workers.md,
// "Failure modes").
//
// Built on llvm::CrashRecoveryContext: SIGSEGV, SIGBUS, SIGILL, SIGFPE,
// SIGABRT and SIGTRAP raised inside the guarded function unwind (by
// longjmp) to the guard, which reports the signal. The handlers run on a
// per-thread alternate signal stack, so a stack overflow from deep template
// recursion is recovered instead of faulting again inside the handler.
//
// Recovery skips every destructor between the fault and the guard, so
// callers must not let the guarded function touch shared state: the bake
// parses each TU into TU-local indexes and absorbs them into the shared ones
// only after a clean return. A crashed TU's local indexes are leaked, never
// merged, so no partial facts survive and no shared lock is ever held by a
// crashed thread. What the guard cannot contain is a fault inside a lock
// the frontend or the C runtime shares across threads (the allocator); that
// is why whole-project bakes default to subprocess workers.
// ============================================================================

/// Enables the crash guard process-wide for its lifetime (reference
/// counted; nested and concurrent scopes are fine). runCrashGuarded
/// outside any scope runs its function unguarded.
class CrashGuardScope {
public:
  CrashGuardScope();
  ~CrashGuardScope();
  CrashGuardScope(const CrashGuardScope &) = delete;
  CrashGuardScope &operator=(const CrashGuardScope &) = delete;
};

/// Run `fn` on the calling thread under the crash guard. Returns true when
/// `fn` returned normally; false when it crashed, with the signal number
/// in `*signalOut` (when non-null).
bool runCrashGuarded(llvm::function_ref<void()> fn, int *signalOut = nullptr);

} // namespace vycor
