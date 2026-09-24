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

#include "vycor/callgraph/Interrupt.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/raw_ostream.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif

namespace vycor {

namespace {

// Name of the environment variable a parent sets to its own pid, so a
// worker can tell whether the parent died before it bound itself to it.
constexpr const char *kParentPidEnv = "VYCOR_WORKER_PARENT_PID";

// Leaked on purpose: the watcher thread may touch it while static
// destructors run at exit.
struct Registry {
  std::mutex mu;
  std::unordered_set<long> children;
  std::vector<std::string> paths; // multiset: the same path may nest
  bool interrupted = false;
};

Registry &registry() {
  static Registry *r = new Registry;
  return *r;
}

std::atomic<bool> g_interrupted{false};
std::once_flag g_installOnce;

void blockedSet(sigset_t &set) {
  sigemptyset(&set);
  sigaddset(&set, SIGINT);
  sigaddset(&set, SIGTERM);
}

[[noreturn]] void watcherMain(sigset_t set) {
  int sig = 0;
  while (sigwait(&set, &sig) != 0) {
  }
  std::vector<std::string> paths;
  size_t killed = 0;
  {
    Registry &r = registry();
    std::lock_guard<std::mutex> lock(r.mu);
    r.interrupted = true;
    g_interrupted.store(true);
    for (long pid : r.children)
      if (::kill(static_cast<pid_t>(pid), SIGKILL) == 0)
        ++killed;
    paths = r.paths;
  }
  llvm::errs() << "vycor-cpp: interrupted (" << (sig == SIGINT ? "SIGINT"
                                                               : "SIGTERM")
               << "): killed " << killed << " worker(s), removing "
               << paths.size() << " scratch path(s)\n";
  // Innermost registrations last in, first out (a file inside a registered
  // directory goes before the directory).
  for (auto it = paths.rbegin(); it != paths.rend(); ++it) {
    if (llvm::sys::fs::is_directory(*it))
      llvm::sys::fs::remove_directories(*it, /*IgnoreErrors=*/true);
    else
      llvm::sys::fs::remove(*it);
  }
  // Files registered with RemoveFileOnSignal (llvm::sys::fs::TempFile).
  llvm::sys::RunInterruptHandlers();

  struct sigaction dfl {};
  dfl.sa_handler = SIG_DFL;
  sigemptyset(&dfl.sa_mask);
  sigaction(sig, &dfl, nullptr);
  sigset_t one;
  sigemptyset(&one);
  sigaddset(&one, sig);
  pthread_sigmask(SIG_UNBLOCK, &one, nullptr);
  ::raise(sig);
  // Not reached unless the signal was somehow ignored.
  std::_Exit(128 + sig);
}

} // namespace

void installInterruptHandler() {
  std::call_once(g_installOnce, [] {
    sigset_t set;
    blockedSet(set);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);
    // Workers read this to detect a parent that died before they bound to
    // it (bindWorkerToParent).
    ::setenv(kParentPidEnv, std::to_string(::getpid()).c_str(), 1);
    std::thread(watcherMain, set).detach();
  });
}

bool interruptRequested() { return g_interrupted.load(); }

void bindWorkerToParent() {
  sigset_t set;
  blockedSet(set);
  pthread_sigmask(SIG_UNBLOCK, &set, nullptr);

  long expected = 0;
  if (const char *env = std::getenv(kParentPidEnv))
    expected = std::atol(env);
#if defined(__linux__)
  ::prctl(PR_SET_PDEATHSIG, SIGKILL);
#else
  // No parent-death signal: poll for reparenting.
  const pid_t parent = ::getppid();
  std::thread([parent] {
    for (;;) {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      if (::getppid() != parent)
        std::_Exit(1);
    }
  }).detach();
#endif
  // The parent may have died between the spawn and the line above.
  if (expected > 0 && ::getppid() != static_cast<pid_t>(expected))
    std::_Exit(1);
}

InterruptCleanup::InterruptCleanup(std::string path) : path_(std::move(path)) {
  Registry &r = registry();
  std::lock_guard<std::mutex> lock(r.mu);
  r.paths.push_back(path_);
}

InterruptCleanup::~InterruptCleanup() {
  Registry &r = registry();
  std::lock_guard<std::mutex> lock(r.mu);
  for (auto it = r.paths.rbegin(); it != r.paths.rend(); ++it)
    if (*it == path_) {
      r.paths.erase(std::next(it).base());
      break;
    }
}

namespace detail {

long spawnTrackedChild(llvm::function_ref<long()> spawn) {
  Registry &r = registry();
  std::lock_guard<std::mutex> lock(r.mu);
  if (r.interrupted)
    return 0;
  long pid = spawn();
  if (pid > 0)
    r.children.insert(pid);
  return pid;
}

void untrackChild(long pid) {
  Registry &r = registry();
  std::lock_guard<std::mutex> lock(r.mu);
  r.children.erase(pid);
}

} // namespace detail

} // namespace vycor
