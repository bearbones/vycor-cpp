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

#include "vycor/callgraph/CrashGuard.h"

#include "llvm/Support/CrashRecoveryContext.h"

#include <memory>
#include <mutex>

#include <signal.h>

namespace vycor {

namespace {

// The signals llvm::CrashRecoveryContext handles on Unix.
constexpr int kGuardedSignals[] = {SIGABRT, SIGBUS, SIGFPE,
                                   SIGILL,  SIGSEGV, SIGTRAP};

// Large enough for CrashRecoveryContext's handler (which only unblocks the
// signal and longjmps) with generous headroom.
constexpr size_t kAltStackSize = 256 * 1024;

std::mutex g_scopeMutex;
unsigned g_scopeCount = 0;
bool g_disabled = false;

/// CrashRecoveryContext installs its handlers without SA_ONSTACK, so a
/// stack overflow would fault again inside the handler. Re-install the
/// same handlers with the flag set.
void runHandlersOnAltStack() {
  for (int sig : kGuardedSignals) {
    struct sigaction cur;
    if (sigaction(sig, nullptr, &cur) != 0)
      continue;
    if (cur.sa_handler == SIG_DFL || cur.sa_handler == SIG_IGN ||
        (cur.sa_flags & SA_ONSTACK))
      continue;
    cur.sa_flags |= SA_ONSTACK;
    sigaction(sig, &cur, nullptr);
  }
}

/// One alternate signal stack per guarded thread, installed on first use
/// and disabled before its memory is freed at thread exit. A thread that
/// already has one (LLVM installs one on the main thread) keeps it.
struct AltStack {
  std::unique_ptr<char[]> mem;
  bool ours = false;

  void ensure() {
    if (ours)
      return;
    stack_t cur;
    if (sigaltstack(nullptr, &cur) == 0 && !(cur.ss_flags & SS_DISABLE))
      return;
    mem.reset(new char[kAltStackSize]);
    stack_t ss{};
    ss.ss_sp = mem.get();
    ss.ss_size = kAltStackSize;
    ss.ss_flags = 0;
    if (sigaltstack(&ss, nullptr) == 0)
      ours = true;
    else
      mem.reset();
  }

  ~AltStack() {
    if (!ours)
      return;
    stack_t ss{};
    ss.ss_flags = SS_DISABLE;
    sigaltstack(&ss, nullptr);
  }
};

thread_local AltStack tl_altStack;

} // namespace

void disableCrashGuard() {
  std::lock_guard<std::mutex> lock(g_scopeMutex);
  g_disabled = true;
  if (g_scopeCount > 0)
    llvm::CrashRecoveryContext::Disable();
}

CrashGuardScope::CrashGuardScope() {
  std::lock_guard<std::mutex> lock(g_scopeMutex);
  if (g_scopeCount++ == 0 && !g_disabled) {
    llvm::CrashRecoveryContext::Enable();
    runHandlersOnAltStack();
  }
}

CrashGuardScope::~CrashGuardScope() {
  std::lock_guard<std::mutex> lock(g_scopeMutex);
  if (--g_scopeCount == 0 && !g_disabled)
    llvm::CrashRecoveryContext::Disable();
}

bool runCrashGuarded(llvm::function_ref<void()> fn, int *signalOut) {
  tl_altStack.ensure();
  llvm::CrashRecoveryContext crc;
  if (crc.RunSafely(fn))
    return true;
  // On Unix a crash's RetCode is 128 + the signal number.
  if (signalOut)
    *signalOut = crc.RetCode > 128 ? crc.RetCode - 128 : crc.RetCode;
  return false;
}

} // namespace vycor
