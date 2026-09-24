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

#include <string>

namespace vycor {

// ============================================================================
// SIGINT/SIGTERM containment for runs that spawn worker processes or leave
// scratch files behind (docs/design-f12-subprocess-workers.md, "Failure
// modes").
//
// installInterruptHandler() blocks SIGINT and SIGTERM in the calling thread
// — and so in every thread created afterwards — and hands them to one
// watcher thread (sigwait). On delivery the watcher, in ordinary thread
// context: stops new worker spawns, SIGKILLs every tracked worker, removes
// every registered scratch path, runs LLVM's interrupt handlers (files
// registered with llvm::sys::RemoveFileOnSignal, which includes
// llvm::sys::fs::TempFile), and then re-raises the signal with the default
// disposition, so the process dies by it (shell status 130 / 143).
//
// Workers undo the inherited mask and bind their lifetime to the parent
// (bindWorkerToParent): SIGKILL on parent death on Linux
// (PR_SET_PDEATHSIG), a parent-pid poll elsewhere.
// ============================================================================

/// Install the watcher. Call once, from main(), before any other thread is
/// created (the blocked mask is inherited by threads created later).
/// Idempotent.
void installInterruptHandler();

/// True once SIGINT/SIGTERM was received by the watcher.
bool interruptRequested();

/// Worker side: unblock SIGINT/SIGTERM (a spawned worker inherits the
/// parent's blocked mask) and die when the spawning parent dies.
void bindWorkerToParent();

/// A file or directory removed (recursively) if the process is
/// interrupted while the object is alive. The caller still removes the
/// path itself on the normal path; the registration only covers the
/// interrupted one.
class InterruptCleanup {
public:
  explicit InterruptCleanup(std::string path);
  ~InterruptCleanup();
  InterruptCleanup(const InterruptCleanup &) = delete;
  InterruptCleanup &operator=(const InterruptCleanup &) = delete;

private:
  std::string path_;
};

namespace detail {
/// Child tracking for runWorkerProcess. `spawn` runs under the registry
/// lock so the watcher never misses a child spawned concurrently with the
/// signal; it returns the new pid (<= 0 on failure). Returns 0 without
/// calling `spawn` once an interrupt was requested.
long spawnTrackedChild(llvm::function_ref<long()> spawn);
void untrackChild(long pid);
} // namespace detail

} // namespace vycor
