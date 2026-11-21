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

// test_shared_global.cpp — the lock facts the bake records on reference
// records (format v16) and the unsynchronized-shared-global check
// (docs/checks/unsynchronized-shared-global.md) over small threaded
// programs.

#include "vycor/callgraph/ControlFlowIndex.h"
#include "vycor/callgraph/ReferenceIndex.h"
#include "vycor/check/Report.h"
#include "vycor/check/UnsynchronizedSharedGlobal.h"

#include <catch2/catch_test_macros.hpp>
#include <clang/Tooling/CompilationDatabase.h>

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

#include <algorithm>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

using namespace vycor;

namespace {

struct Project {
  std::string dir;
  std::vector<std::string> tus;

  Project() {
    llvm::SmallString<128> tmp;
    REQUIRE_FALSE(llvm::sys::fs::createUniqueDirectory("vycor_usg", tmp));
    llvm::SmallString<128> real;
    REQUIRE_FALSE(llvm::sys::fs::real_path(tmp, real));
    dir = std::string(real.str());
  }
  Project(const Project &) = delete;
  Project &operator=(const Project &) = delete;
  ~Project() { llvm::sys::fs::remove_directories(dir); }

  std::string path(const std::string &name) const { return dir + "/" + name; }
  void tu(const std::string &name, const std::string &code) {
    std::ofstream out(path(name));
    out << code;
    tus.push_back(path(name));
  }
  BakedIndexes bake() const {
    clang::tooling::FixedCompilationDatabase compDb(
        dir, {"-std=c++17", "-I" + dir});
    return bakeIndexes(compDb, tus, {}, 1);
  }
};

std::optional<ReferenceInfo> find(const ReferenceIndex &ix,
                                  const std::string &referrer,
                                  const std::string &target, RefRole role) {
  for (ReferenceInfo &r : ix.allReferences())
    if (r.referrerName == referrer && r.targetName == target && r.role == role)
      return std::move(r);
  return std::nullopt;
}

// The lock sets a reference was made under, one "{a,b}" per distinct set
// (display names, sorted), in the order the lock text lists them; "" when
// no occurrence held a lock.
std::string lockSets(const ReferenceIndex &ix, const std::string &referrer,
                     const std::string &target,
                     RefRole role = RefRole::Write) {
  const auto r = find(ix, referrer, target, role);
  if (!r) {
    FAIL("no " << refRoleName(role) << " reference " << referrer << " -> "
               << target);
    return "";
  }
  std::string out;
  llvm::StringRef rest(r->locks);
  while (!rest.empty()) {
    auto [line, tail] = rest.split('\n');
    rest = tail;
    auto [where, fields] = line.split('\t');
    (void)where;
    std::vector<std::string> names;
    while (!fields.empty()) {
      auto [identity, after] = fields.split('\t');
      auto [display, next] = after.split('\t');
      (void)identity;
      names.push_back(display.str());
      fields = next;
    }
    std::sort(names.begin(), names.end());
    if (!out.empty())
      out += ' ';
    out += '{';
    for (size_t i = 0; i < names.size(); ++i)
      out += (i ? "," : "") + names[i];
    out += '}';
  }
  return out;
}

bool inLoop(const ReferenceIndex &ix, const std::string &referrer,
            const std::string &target, RefRole role) {
  const auto r = find(ix, referrer, target, role);
  REQUIRE(r.has_value());
  return (r->flags & kRefInLoop) != 0;
}

bool mutates(const ReferenceIndex &ix, const std::string &referrer,
             const std::string &target) {
  const auto r = find(ix, referrer, target, RefRole::Write);
  REQUIRE(r.has_value());
  return (r->flags & kRefMutates) != 0;
}

std::vector<Diagnostic> runCheck(const BakedIndexes &baked,
                                 SharedGlobalOptions opts = {}) {
  return findUnsynchronizedSharedGlobals(baked.graph, baked.references, opts);
}

std::string allMessages(const std::vector<Diagnostic> &diags) {
  std::string out;
  for (const Diagnostic &d : diags)
    out += d.message + "\n";
  return out;
}

} // namespace

// ============================================================================
// Lock facts (ReferenceVisitor)
// ============================================================================

TEST_CASE("shared-global facts: scoped guards hold their mutex to the end of "
          "the scope",
          "[shared-global][references]") {
  Project p;
  p.tu("tu.cpp", R"cpp(
#include <mutex>
std::mutex m1, m2;
int a, b, c, d, e;
void scoped() {
  { std::lock_guard<std::mutex> g(m1); a = 1; }
  b = 1;
  std::scoped_lock both(m1, m2);
  c = 1;
}
void unique() {
  std::unique_lock<std::mutex> u(m1, std::defer_lock);
  d = 1;
  u.lock();
  e = 1;
  u.unlock();
  d = 2;
}
)cpp");
  auto baked = p.bake();
  const ReferenceIndex &ix = baked.references;
  CHECK(lockSets(ix, "scoped", "a") == "{m1}");
  CHECK(lockSets(ix, "scoped", "b") == "");
  CHECK(lockSets(ix, "scoped", "c") == "{m1,m2}");
  CHECK(lockSets(ix, "unique", "d") == "");
  CHECK(lockSets(ix, "unique", "e") == "{m1}");
}

TEST_CASE("shared-global facts: manual lock calls, pthread and mixed sites",
          "[shared-global][references]") {
  Project p;
  p.tu("tu.cpp", R"cpp(
#include <mutex>
#include <pthread.h>
std::mutex m;
pthread_mutex_t pm = PTHREAD_MUTEX_INITIALIZER;
int x, y, z;
void manual() {
  m.lock();
  x = 1;
  m.unlock();
  x = 2;
  pthread_mutex_lock(&pm);
  y = 1;
  pthread_mutex_unlock(&pm);
  if (m.try_lock()) { z = 1; m.unlock(); }
}
)cpp");
  auto baked = p.bake();
  const ReferenceIndex &ix = baked.references;
  // Two occurrences under two sets: one line each, no lock as {}.
  CHECK(lockSets(ix, "manual", "x") == "{m} {}");
  CHECK(lockSets(ix, "manual", "y") == "{pm}");
  CHECK(lockSets(ix, "manual", "z") == "{m}");
}

TEST_CASE("shared-global facts: a mutex the bake cannot name is unknown, "
          "never no lock",
          "[shared-global][references]") {
  Project p;
  p.tu("tu.cpp", R"cpp(
#include <mutex>
struct Box { std::mutex m; int v; };
std::mutex *lookup();
std::mutex global;
Box box;
int viaPointer, viaMember;
void f(Box &other) {
  std::lock_guard<std::mutex> g(*lookup());
  viaPointer = 1;
}
void g(Box &other) {
  std::lock_guard<std::mutex> g(other.m);
  viaMember = 1;
}
)cpp");
  auto baked = p.bake();
  const ReferenceIndex &ix = baked.references;
  CHECK(lockSets(ix, "f", "viaPointer") == "{<unknown mutex>}");
  // A data member is named by the member: every object's mutex alike.
  CHECK(lockSets(ix, "g", "viaMember") == "{Box::m}");
  // A mutex variable is a synchronization primitive, not shared data.
  bool sync = false;
  for (const SymbolInfo &s : ix.allSymbols())
    if (s.name == "global")
      sync = (s.flags & kSymSyncPrimitive) != 0;
  CHECK(sync);
}

TEST_CASE("shared-global facts: call_once and function-local static "
          "initializers run under a once guard",
          "[shared-global][references]") {
  Project p;
  p.tu("tu.cpp", R"cpp(
#include <mutex>
std::once_flag flag;
int initOnce, initStatic;
int make() { initStatic = 1; return 1; }
void f() {
  std::call_once(flag, [] { initOnce = 1; });
  static int cached = make();
  (void)cached;
}
)cpp");
  auto baked = p.bake();
  const ReferenceIndex &ix = baked.references;
  bool sawOnce = false;
  for (const ReferenceInfo &r : ix.allReferences())
    if (r.targetName == "initOnce" && r.role == RefRole::Write)
      sawOnce = llvm::StringRef(r.locks).contains("once(flag)");
  CHECK(sawOnce);
  CHECK(lockSets(ix, "f", "make", RefRole::Call) == "{once(f::cached)}");
}

TEST_CASE("shared-global facts: loops mark references and lambdas",
          "[shared-global][references]") {
  Project p;
  p.tu("tu.cpp", R"cpp(
void spawn(void (*fn)());
void work();
int n;
void f() {
  for (int i = 0; i < 3; ++i)
    work();
  n = 1;
  while (n) { auto l = [] { n = 0; }; l(); }
}
)cpp");
  auto baked = p.bake();
  const ReferenceIndex &ix = baked.references;
  CHECK(inLoop(ix, "f", "work", RefRole::Call));
  CHECK_FALSE(inLoop(ix, "f", "n", RefRole::Write));
  bool lambdaInLoop = false;
  for (const SymbolInfo &s : ix.allSymbols())
    if ((s.flags & kSymLambda) != 0)
      lambdaInLoop = (s.flags & kSymInLoop) != 0;
  CHECK(lambdaInLoop);
}

TEST_CASE("shared-global facts: a lookup through a non-const member with a "
          "const twin does not mutate",
          "[shared-global][references]") {
  Project p;
  p.tu("tu.cpp", R"cpp(
#include <map>
#include <vector>
std::map<int, int> table;
std::vector<int> cells(4);
int lookup(int k) {
  auto it = table.find(k);
  return it == table.end() ? 0 : it->second;
}
void put(int k) { table[k] = 1; }
void fill() { cells[0] = 3; }
void grow() { cells.push_back(1); }
)cpp");
  auto baked = p.bake();
  const ReferenceIndex &ix = baked.references;
  // find / end: non-const, a const overload exists, an iterator by value.
  CHECK_FALSE(mutates(ix, "lookup", "table"));
  // map::operator[] has no const twin; vector::operator[] returns T&.
  CHECK(mutates(ix, "put", "table"));
  CHECK(mutates(ix, "fill", "cells"));
  CHECK(mutates(ix, "grow", "cells"));
}

TEST_CASE("shared-global facts: pointer guards, lockable types and mutexes "
          "behind a pointer",
          "[shared-global][references]") {
  Project p;
  p.tu("tu.cpp", R"cpp(
#include <mutex>
struct Mutex { void Lock(); void Unlock(); };
struct MutexLock {
  explicit MutexLock(Mutex *mu) : mu_(mu) { mu_->Lock(); }
  ~MutexLock() { mu_->Unlock(); }
  Mutex *mu_;
};
struct SpinLock { void lock(); void unlock(); };
Mutex mu;
SpinLock spin;
std::mutex sm;
std::mutex *gp = &sm;
int a, b, c, d, e;
void f() {
  { MutexLock l(&mu); a = 1; }
  { std::lock_guard<SpinLock> g(spin); b = 1; }
  spin.lock();
  c = 1;
  spin.unlock();
  { std::lock_guard<std::mutex> g(*gp); d = 1; }
  gp->lock();
  e = 1;
  gp->unlock();
}
)cpp");
  auto baked = p.bake();
  const ReferenceIndex &ix = baked.references;
  CHECK(lockSets(ix, "f", "a") == "{mu}");
  CHECK(lockSets(ix, "f", "b") == "{spin}");
  CHECK(lockSets(ix, "f", "c") == "{spin}");
  // A pointer names whatever it points at when the code runs.
  CHECK(lockSets(ix, "f", "d") == "{<unknown mutex>}");
  CHECK(lockSets(ix, "f", "e") == "{<unknown mutex>}");
}

TEST_CASE("shared-global facts: std::lock on deferred guards, and release "
          "hands the mutex over still locked",
          "[shared-global][references]") {
  Project p;
  p.tu("tu.cpp", R"cpp(
#include <mutex>
std::mutex m1, m2;
int x, y, z;
void f() {
  std::unique_lock<std::mutex> a(m1, std::defer_lock), b(m2, std::defer_lock);
  std::lock(a, b);
  x = 1;
}
void g() {
  { std::unique_lock<std::mutex> u(m1); u.release(); }
  y = 1;
  m1.unlock();
  z = 1;
}
)cpp");
  auto baked = p.bake();
  const ReferenceIndex &ix = baked.references;
  CHECK(lockSets(ix, "f", "x") == "{m1,m2}");
  CHECK(lockSets(ix, "g", "y") == "{m1}");
  CHECK(lockSets(ix, "g", "z") == "");
}

TEST_CASE("shared-global facts: a do-while(0) lock macro is no loop and a "
          "thread_local initializer has no once guard",
          "[shared-global][references]") {
  Project p;
  p.tu("tu.cpp", R"cpp(
#include <mutex>
#define LOCKED(stmt) do { std::lock_guard<std::mutex> g(m); stmt; } while (0)
std::mutex m;
int w;
int make() { return 1; }
void f() {
  LOCKED(w = 1);
  w = 2;
  thread_local int t = make();
  (void)t;
}
)cpp");
  auto baked = p.bake();
  const ReferenceIndex &ix = baked.references;
  CHECK(lockSets(ix, "f", "w") == "{m} {}");
  CHECK_FALSE(inLoop(ix, "f", "w", RefRole::Write));
  CHECK(lockSets(ix, "f", "make", RefRole::Call) == "");
}

TEST_CASE("shared-global facts: call_once with a named function and a "
          "callable passed under a lock",
          "[shared-global][references]") {
  Project p;
  p.tu("tu.cpp", R"cpp(
#include <functional>
#include <mutex>
std::once_flag flag;
std::mutex m;
void init() {}
void other() {}
void work() {}
void f() {
  std::call_once(flag, init);
  std::call_once(flag, &other);
  std::lock_guard<std::mutex> g(m);
  std::invoke(work);
}
)cpp");
  auto baked = p.bake();
  const ReferenceIndex &ix = baked.references;
  CHECK(lockSets(ix, "f", "init", RefRole::AddressOf) == "{once(flag)}");
  CHECK(lockSets(ix, "f", "other", RefRole::AddressOf) == "{once(flag)}");
  CHECK(lockSets(ix, "f", "work", RefRole::AddressOf) == "{m}");
}

TEST_CASE("shared-global facts: a subscript through a write proxy mutates",
          "[shared-global][references]") {
  Project p;
  p.tu("tu.cpp", R"cpp(
#include <bitset>
#include <vector>
std::vector<bool> flags(8);
std::bitset<8> bits;
bool peek() { return flags[0] && bits[1]; }
void f() { flags[0] = true; bits[1] = true; }
)cpp");
  auto baked = p.bake();
  const ReferenceIndex &ix = baked.references;
  // vector<bool>::reference and bitset::reference assign through to the
  // container: the const twin does not make the non-const subscript a read.
  CHECK(mutates(ix, "f", "flags"));
  CHECK(mutates(ix, "f", "bits"));
}

TEST_CASE("shared-global facts: branches start from the state before them "
          "and meet after",
          "[shared-global][references]") {
  Project p;
  p.tu("tu.cpp", R"cpp(
#include <mutex>
std::mutex m;
int counter, h, g, k, g2;
void log();
void quick(bool q) {
  std::unique_lock<std::mutex> lk(m);
  if (q) { lk.unlock(); h = 1; } else { ++counter; }
}
void maybe(bool c, bool needLock) {
  if (c) { m.lock(); } else { ++g; }
  if (needLock) { m.lock(); }
  k = 1;
}
void early(bool bad) {
  m.lock();
  if (bad) { m.unlock(); return; }
  ++g2;
  m.unlock();
}
)cpp");
  auto baked = p.bake();
  const ReferenceIndex &ix = baked.references;
  // The then-branch's unlock does not reach the else branch.
  CHECK(lockSets(ix, "quick", "counter") == "{m}");
  CHECK(lockSets(ix, "quick", "h") == "");
  // A lock taken on one path only is not held after the if, nor in the
  // other branch.
  CHECK(lockSets(ix, "maybe", "g") == "");
  CHECK(lockSets(ix, "maybe", "k") == "");
  // A branch that returns does not count at the join.
  CHECK(lockSets(ix, "early", "g2") == "{m}");
}

TEST_CASE("shared-global facts: switch cases and loops meet their exits",
          "[shared-global][references]") {
  Project p;
  p.tu("tu.cpp", R"cpp(
#include <mutex>
std::mutex m;
int s2, s4, s5, r, w;
void log();
bool ready();
void sw(int x) {
  std::unique_lock<std::mutex> lk(m);
  switch (x) {
  case 1: { lk.unlock(); log(); } break;
  case 2: ++s2; break;
  case 3: lk.unlock(); [[fallthrough]];
  case 4: ++s4; break;
  }
  ++s5;
}
void retry() {
  for (;;) {
    m.lock();
    if (ready()) break;
    m.unlock();
  }
  r = 1;
  m.unlock();
}
void loop(int n) {
  while (n--) { m.lock(); }
  w = 1;
}
)cpp");
  auto baked = p.bake();
  const ReferenceIndex &ix = baked.references;
  // case 1's unlock does not reach case 2.
  CHECK(lockSets(ix, "sw", "s2") == "{m}");
  // case 3 falls into case 4 unlocked.
  CHECK(lockSets(ix, "sw", "s4") == "");
  // Some break leaves unlocked.
  CHECK(lockSets(ix, "sw", "s5") == "");
  // The only way out of for (;;) is the break, taken locked.
  CHECK(lockSets(ix, "retry", "r") == "{m}");
  // A while loop can run zero times.
  CHECK(lockSets(ix, "loop", "w") == "");
}

TEST_CASE("shared-global facts: breaks leave their own loop or switch, and "
          "do-while(0) exits meet",
          "[shared-global][references]") {
  Project p;
  p.tu("tu.cpp", R"cpp(
#include <mutex>
std::mutex m;
int k1, k2, k3, k4, k5;
bool ready();
void swloop(int n) {
  for (int i = 0; i < n; ++i) {
    m.lock();
    switch (i) { case 0: break; default: break; }
    ++k1;
    m.unlock();
  }
}
void nested(int n) {
  m.lock();
  while (n--) {
    for (;;) { if (ready()) break; }
    ++k2;
  }
  m.unlock();
}
void macro(bool bad) {
  do {
    m.lock();
    if (bad) break;
    ++k3;
  } while (0);
  ++k4;
  m.unlock();
  do { if (bad) continue; m.lock(); } while (0);
  ++k5;
}
)cpp");
  auto baked = p.bake();
  const ReferenceIndex &ix = baked.references;
  // The breaks leave the switch, not the loop.
  CHECK(lockSets(ix, "swloop", "k1") == "{m}");
  // The inner break leaves the inner loop only.
  CHECK(lockSets(ix, "nested", "k2") == "{m}");
  CHECK(lockSets(ix, "macro", "k3") == "{m}");
  // Both ways out of the first macro hold m; the continue leaves the
  // second one unlocked.
  CHECK(lockSets(ix, "macro", "k4") == "{m}");
  CHECK(lockSets(ix, "macro", "k5") == "");
}

TEST_CASE("shared-global facts: a forward goto carries its locks to its "
          "label",
          "[shared-global][references]") {
  Project p;
  p.tu("tu.cpp", R"cpp(
#include <mutex>
std::mutex m;
int a3, a4, x1, x2, x3, x4;
void t5(bool b) {
  m.lock();
  if (b) goto out;
  ++a3;
  m.unlock();
  return;
out:
  ++a4;
  m.unlock();
}
void scoped(bool b) {
  { std::lock_guard<std::mutex> lk(m); if (b) goto done; ++x1; }
  ++x2;
done:
  ++x3;
}
void falls(bool b) {
  m.lock();
  if (b) goto out;
  m.unlock();
out:
  ++x4;
}
)cpp");
  auto baked = p.bake();
  const ReferenceIndex &ix = baked.references;
  CHECK(lockSets(ix, "t5", "a3") == "{m}");
  // Only the goto reaches out, and it holds m.
  CHECK(lockSets(ix, "t5", "a4") == "{m}");
  CHECK(lockSets(ix, "scoped", "x1") == "{m}");
  CHECK(lockSets(ix, "scoped", "x2") == "");
  // The guard's block ended on the way to the label.
  CHECK(lockSets(ix, "scoped", "x3") == "");
  // The goto (locked) meets the fall-through (unlocked).
  CHECK(lockSets(ix, "falls", "x4") == "");
}

TEST_CASE("shared-global facts: a different lock on each path is an unknown "
          "lock; switches without breaks and endless loops jump",
          "[shared-global][references]") {
  Project p;
  p.tu("tu.cpp", R"cpp(
#include <mutex>
std::mutex m, a, b;
int d1, d2, x1, x2, j1, j2;
void log();
void two(bool x) {
  if (x) a.lock(); else b.lock();
  ++d1;
  if (x) a.unlock(); else b.unlock();
  ++d2;
}
void guards(bool c) {
  { std::unique_lock<std::mutex> la(a, std::defer_lock), lb(b, std::defer_lock);
    if (c) la.lock(); else lb.lock();
    ++x1; }
  ++x2;
}
void sw(int x) {
  m.lock();
  if (x > 0) {
    switch (x) { case 1: m.unlock(); return; default: m.unlock(); return; }
  }
  ++j1;
  m.unlock();
}
void spin(bool bad) {
  m.lock();
  if (bad) { m.unlock(); for (;;) { log(); } }
  ++j2;
  m.unlock();
}
)cpp");
  auto baked = p.bake();
  const ReferenceIndex &ix = baked.references;
  CHECK(lockSets(ix, "two", "d1") == "{<unknown mutex>}");
  // The paired unlocks drop it.
  CHECK(lockSets(ix, "two", "d2") == "");
  // It ends with the guards it came from.
  CHECK(lockSets(ix, "guards", "x1") == "{<unknown mutex>}");
  CHECK(lockSets(ix, "guards", "x2") == "");
  // Neither the switch nor the endless loop falls through to the access.
  CHECK(lockSets(ix, "sw", "j1") == "{m}");
  CHECK(lockSets(ix, "spin", "j2") == "{m}");
}

TEST_CASE("shared-global facts: the loop back edge is not met into the body "
          "(a documented limit)",
          "[shared-global][references]") {
  Project p;
  p.tu("tu.cpp", R"cpp(
#include <mutex>
std::mutex m;
int b1;
bool skip();
void backEdge(int n) {
  std::unique_lock<std::mutex> lk(m);
  for (int i = 0; i < n; ++i) {
    ++b1;
    if (skip()) { lk.unlock(); continue; }
  }
}
)cpp");
  auto baked = p.bake();
  // A later iteration runs after the unlock, but the body is read once,
  // from the state before the loop.
  CHECK(lockSets(baked.references, "backEdge", "b1") == "{m}");
}

TEST_CASE("shared-global facts: a local holding a lambda is called where it "
          "is called, not where it is copied, captured or measured",
          "[shared-global][references]") {
  Project p;
  p.tu("tu.cpp", R"cpp(
#include <mutex>
std::mutex m;
int lg;
void f() {
  auto l = [] { ++lg; };
  auto l2 = l;
  std::lock_guard<std::mutex> lk(m);
  l();
  auto inner = [l] { l(); };
  inner();
  (void)sizeof(l);
  (void)l2;
}
)cpp");
  auto baked = p.bake();
  const ReferenceIndex &ix = baked.references;
  // l is the lambda that writes lg.
  std::string lUsr;
  for (const ReferenceInfo &r : ix.allReferences())
    if (r.targetName == "lg" && r.role == RefRole::Write)
      lUsr = r.referrerUsr;
  REQUIRE_FALSE(lUsr.empty());
  uint32_t calls = 0;
  std::string locks;
  for (const ReferenceInfo &r : ix.allReferences())
    if (r.referrerName == "f" && r.targetUsr == lUsr && r.role == RefRole::Call) {
      calls += r.count;
      locks = r.locks;
    }
  CHECK(calls == 1);
  CHECK(llvm::StringRef(locks).contains("\tm"));
}

TEST_CASE("shared-global facts: guards named after spin and rw locks, and "
          "inherited lock members",
          "[shared-global][references]") {
  Project p;
  p.tu("tu.cpp", R"cpp(
#include <mutex>
struct SpinLock { void lock(); void unlock(); };
struct ScopedSpinLock {
  explicit ScopedSpinLock(SpinLock &l);
  ~ScopedSpinLock();
};
struct RWLock { void lock(); void unlock(); };
struct RWLockReadScope {
  explicit RWLockReadScope(RWLock *l);
  ~RWLockReadScope();
};
struct Lockable : std::mutex {};
SpinLock sl;
RWLock rw;
Lockable lockable;
int a, b, c;
void f() { ScopedSpinLock g(sl); a = 1; }
void h() { RWLockReadScope g(&rw); b = 1; }
void i() { lockable.lock(); c = 1; lockable.unlock(); }
)cpp");
  auto baked = p.bake();
  const ReferenceIndex &ix = baked.references;
  CHECK(lockSets(ix, "f", "a") == "{sl}");
  CHECK(lockSets(ix, "h", "b") == "{rw}");
  CHECK(lockSets(ix, "i", "c") == "{lockable}");
}

TEST_CASE("shared-global facts: locks and flags survive encode, decode and "
          "absorb",
          "[shared-global][references][snapshot]") {
  ReferenceIndex ix;
  ReferenceFact r;
  r.referrerUsr = "c:@F@f#";
  r.targetUsr = "c:@g";
  r.role = RefRole::Write;
  r.file = "/src/f.cpp";
  r.line = 5;
  r.col = 3;
  r.locks = "5:3\tc:@m\tm";
  r.flags = kRefInLoop;
  ix.addReference(r, "/src/f.cpp");
  ReferenceFact plain = r;
  plain.locks.clear();
  plain.flags = 0;
  ix.addReference(plain, "/src/other.cpp"); // differs only in lock facts

  std::string bytes;
  ix.encode(bytes);
  ReferenceIndex back;
  REQUIRE(back.decode(bytes.data(), bytes.data() + bytes.size(), false));
  std::string again;
  back.encode(again);
  CHECK(again == bytes);
  CHECK(back.referenceCount() == 2);
  auto merged = back.referencesTo("c:@g");
  REQUIRE(merged.size() == 1);
  CHECK((merged[0].flags & kRefInLoop) != 0);
  CHECK(merged[0].tuCount == 2);

  ReferenceIndex absorbed;
  absorbed.absorb(back);
  std::string third;
  absorbed.encode(third);
  CHECK(third == bytes);

  // A flags byte with an unknown bit is refused. The flags byte is the one
  // byte where the encodings of a record with and without kRefInLoop differ.
  ReferenceIndex withFlag, withoutFlag;
  withFlag.addReference(r, "/src/f.cpp");
  ReferenceFact unflagged = r;
  unflagged.flags = 0;
  withoutFlag.addReference(unflagged, "/src/f.cpp");
  std::string one, zero;
  withFlag.encode(one);
  withoutFlag.encode(zero);
  REQUIRE(one.size() == zero.size());
  std::vector<size_t> differ;
  for (size_t i = 0; i < one.size(); ++i)
    if (one[i] != zero[i])
      differ.push_back(i);
  REQUIRE(differ.size() == 1);
  CHECK(static_cast<uint8_t>(one[differ[0]]) == kRefInLoop);
  std::string damaged = one;
  damaged[differ[0]] = static_cast<char>(0x80);
  ReferenceIndex bad;
  CHECK_FALSE(bad.decode(damaged.data(), damaged.data() + damaged.size(),
                         false));
  ReferenceIndex good;
  CHECK(good.decode(one.data(), one.data() + one.size(), false));
}

// ============================================================================
// The check
// ============================================================================

TEST_CASE("shared-global: a global two threads write with no lock is a race",
          "[shared-global]") {
  Project p;
  p.tu("tu.cpp", R"cpp(
#include <atomic>
#include <mutex>
#include <thread>
int counter = 0;
int guarded = 0;
std::mutex guardMutex;
std::atomic<int> hits{0};
thread_local int scratch = 0;
const int kLimit = 10;
int onceValue = 0;
std::once_flag onceFlag;
void bump() { ++counter; }
void worker() {
  bump();
  { std::lock_guard<std::mutex> g(guardMutex); ++guarded; }
  hits.fetch_add(1);
  scratch = kLimit;
  std::call_once(onceFlag, [] { onceValue = 1; });
}
void reader() {
  int c = counter;
  (void)c;
  std::unique_lock<std::mutex> u(guardMutex);
  int g = guarded + onceValue + scratch;
  (void)g;
}
int main() {
  std::thread a(worker);
  std::thread b(reader);
  a.join();
  b.join();
}
)cpp");
  auto baked = p.bake();
  auto diags = runCheck(baked);
  INFO(allMessages(diags));
  REQUIRE(diags.size() == 1);
  const Diagnostic &d = diags[0];
  CHECK(d.kind == Diagnostic::SharedGlobal_Race);
  CHECK(checkNameOf(d) == "unsynchronized-shared-global");
  CHECK(diagnosticSeverity(d) == Severity::Warning);
  CHECK(d.message.rfind("'counter' is written in bump", 0) == 0);
  CHECK(llvm::StringRef(d.message).contains("read in reader"));
  CHECK(llvm::StringRef(d.message).contains("holding no lock"));
  CHECK(llvm::StringRef(d.message).contains("Paths: worker -> bump; reader."));
  REQUIRE(d.entities.size() == 1);
  CHECK(d.entities[0] == "c:@counter");
  CHECK(llvm::StringRef(d.callLocation).ends_with("tu.cpp:5"));
}

TEST_CASE("shared-global: a lock held by the caller protects the access",
          "[shared-global]") {
  Project p;
  p.tu("tu.cpp", R"cpp(
#include <mutex>
#include <thread>
std::mutex m;
int total = 0;
void add() { ++total; }       // no lock here: the callers hold it
void producer() { std::lock_guard<std::mutex> g(m); add(); }
void consumer() {
  m.lock();
  int t = total;
  (void)t;
  m.unlock();
}
int main() {
  std::thread a(producer), b(consumer);
  a.join();
  b.join();
}
)cpp");
  auto baked = p.bake();
  auto diags = runCheck(baked);
  INFO(allMessages(diags));
  CHECK(diags.empty());
}

TEST_CASE("shared-global: different locks on the two sides are reported with "
          "both lock sets",
          "[shared-global]") {
  Project p;
  p.tu("tu.cpp", R"cpp(
#include <mutex>
#include <thread>
std::mutex ma, mb;
int total = 0;
void a() { std::lock_guard<std::mutex> g(ma); ++total; }
void b() { std::lock_guard<std::mutex> g(mb); ++total; }
int main() {
  std::thread x(a), y(b);
  x.join();
  y.join();
}
)cpp");
  auto baked = p.bake();
  auto diags = runCheck(baked);
  INFO(allMessages(diags));
  REQUIRE(diags.size() == 1);
  CHECK(diags[0].kind == Diagnostic::SharedGlobal_Race);
  CHECK(llvm::StringRef(diags[0].message).contains("holding {ma}"));
  CHECK(llvm::StringRef(diags[0].message).contains("holding {mb}"));
}

TEST_CASE("shared-global: the main thread's write before the start is an "
          "init-then-publish note",
          "[shared-global]") {
  Project p;
  p.tu("tu.cpp", R"cpp(
#include <thread>
int config = 0;
void worker() { int c = config; (void)c; }
int main() {
  config = 3;
  std::thread t(worker);
  t.join();
}
)cpp");
  auto baked = p.bake();
  auto diags = runCheck(baked);
  INFO(allMessages(diags));
  REQUIRE(diags.size() == 1);
  CHECK(diags[0].kind == Diagnostic::SharedGlobal_InitThenPublish);
  CHECK(diagnosticSeverity(diags[0]) == Severity::Note);
  CHECK(llvm::StringRef(diags[0].message).contains("initialize, then publish"));
}

TEST_CASE("shared-global: one thread alone, or only reads, is not a race",
          "[shared-global]") {
  Project p;
  p.tu("tu.cpp", R"cpp(
#include <thread>
int mine = 0;
int table = 7;
void worker() { ++mine; }
void a() { int t = table; (void)t; }
void b() { int t = table; (void)t; }
int main() {
  std::thread w(worker), x(a), y(b);
  w.join(); x.join(); y.join();
}
)cpp");
  auto baked = p.bake();
  auto diags = runCheck(baked);
  INFO(allMessages(diags));
  CHECK(diags.empty());
}

TEST_CASE("shared-global: lookups in a static table are reads, a subscript "
          "write is not",
          "[shared-global]") {
  Project p;
  p.tu("tu.cpp", R"cpp(
#include <thread>
#include <unordered_map>
#include <vector>
int lookup(int k) {
  static std::unordered_map<int, int> table = {{1, 2}};
  auto it = table.find(k);
  return it == table.end() ? 0 : it->second;
}
std::vector<int> slots(8);
void a() { lookup(1); slots[1] = 1; }
void b() { lookup(2); slots[2] = 2; }
int main() {
  std::thread x(a), y(b);
  x.join(); y.join();
}
)cpp");
  auto baked = p.bake();
  auto diags = runCheck(baked);
  INFO(allMessages(diags));
  REQUIRE(diags.size() == 1);
  CHECK(diags[0].kind == Diagnostic::SharedGlobal_Race);
  CHECK(llvm::StringRef(diags[0].message).contains("'slots'"));
}

TEST_CASE("shared-global: a thread started in a loop races with itself",
          "[shared-global]") {
  Project p;
  p.tu("tu.cpp", R"cpp(
#include <thread>
#include <vector>
int hits = 0;
void work() { ++hits; }
int main() {
  for (int i = 0; i < 4; ++i) {
    std::thread t(work);
    t.detach();
  }
}
)cpp");
  auto baked = p.bake();
  SharedGlobalStats stats;
  auto diags = findUnsynchronizedSharedGlobals(baked.graph, baked.references,
                                               {}, &stats);
  INFO(allMessages(diags));
  REQUIRE(diags.size() == 1);
  CHECK(diags[0].kind == Diagnostic::SharedGlobal_Race);
  CHECK(llvm::StringRef(diags[0].message)
            .contains("which can run more than once at the same time"));
  CHECK(stats.threadContexts == 1);
  CHECK(stats.selfConcurrent == 1);
  CHECK(stats.boundedContexts == 0);
  CHECK(stats.statesSearched >= 2);
}

TEST_CASE("shared-global: an unknown lock on both sides is not a finding",
          "[shared-global]") {
  Project p;
  p.tu("tu.cpp", R"cpp(
#include <mutex>
#include <thread>
std::mutex &lockFor(int);
int total = 0;
void a() { std::lock_guard<std::mutex> g(lockFor(0)); ++total; }
void b() { std::lock_guard<std::mutex> g(lockFor(0)); ++total; }
int main() {
  std::thread x(a), y(b);
  x.join();
  y.join();
}
)cpp");
  auto baked = p.bake();
  auto diags = runCheck(baked);
  INFO(allMessages(diags));
  CHECK(diags.empty());
}

TEST_CASE("shared-global: a path through virtual dispatch is a plausible note",
          "[shared-global]") {
  Project p;
  p.tu("tu.cpp", R"cpp(
#include <thread>
struct Job { virtual void run() = 0; virtual ~Job() = default; };
int viaVirtual = 0;
struct Bump : Job { void run() override { ++viaVirtual; } };
Job *current();
void a() { current()->run(); }
void b() { ++viaVirtual; }
int main() {
  std::thread x(a), y(b);
  x.join();
  y.join();
}
)cpp");
  auto baked = p.bake();
  auto diags = runCheck(baked);
  INFO(allMessages(diags));
  REQUIRE(diags.size() == 1);
  CHECK(diags[0].kind == Diagnostic::SharedGlobal_Plausible);
  CHECK(diagnosticSeverity(diags[0]) == Severity::Note);
  CHECK(llvm::StringRef(diags[0].message).contains("plausible edge"));
  CHECK(llvm::StringRef(diags[0].message).contains("Paths: a -> Bump::run; b."));
}

TEST_CASE("shared-global: facts that miss part of the program downgrade a "
          "race to unproven",
          "[shared-global]") {
  Project p;
  p.tu("tu.cpp", R"cpp(
#include <thread>
int counter = 0;
void helper() {}
void a() { ++counter; helper(); }
int main() {
  std::thread x(a), y(a);
  x.join();
  y.join();
}
)cpp");
  auto baked = p.bake();
  SharedGlobalOptions opts;
  opts.coverageGap = "the facts cover 1 of the 2 translation units";
  auto diags = runCheck(baked, opts);
  INFO(allMessages(diags));
  REQUIRE(diags.size() == 1);
  CHECK(diags[0].kind == Diagnostic::SharedGlobal_Unproven);
  CHECK(diagnosticSeverity(diags[0]) == Severity::Note);
  CHECK(llvm::StringRef(diags[0].message)
            .contains("Not proven: the facts cover 1 of the 2"));

  // The same program with a search bound it overruns: what was reached is
  // reported, as not proven.
  SharedGlobalOptions tight;
  tight.maxStatesPerContext = 1;
  auto bounded = runCheck(baked, tight);
  INFO(allMessages(bounded));
  REQUIRE(bounded.size() == 1);
  CHECK(bounded[0].kind == Diagnostic::SharedGlobal_Unproven);
  CHECK(llvm::StringRef(bounded[0].message).contains("stopped at its bound"));
}

TEST_CASE("shared-global: no thread, no finding", "[shared-global]") {
  Project p;
  p.tu("tu.cpp", R"cpp(
int counter = 0;
void bump() { ++counter; }
int main() { bump(); return counter; }
)cpp");
  auto baked = p.bake();
  SharedGlobalStats stats;
  CHECK(findUnsynchronizedSharedGlobals(baked.graph, baked.references, {},
                                        &stats)
            .empty());
  CHECK(stats.threadContexts == 0);
  CHECK(stats.mainContext);
  CHECK(stats.mainDefinitions == 1);
  CHECK(stats.candidates == 1);
  CHECK(stats.statesSearched == 0);
}

TEST_CASE("shared-global: several programs' mains are counted",
          "[shared-global]") {
  Project p;
  p.tu("one.cpp", "int main() { return 0; }\n");
  p.tu("two.cpp", "int main() { return 1; }\n");
  auto baked = p.bake();
  SharedGlobalStats stats;
  CHECK(findUnsynchronizedSharedGlobals(baked.graph, baked.references, {},
                                        &stats)
            .empty());
  CHECK(stats.mainDefinitions == 2);
}

TEST_CASE("shared-global: a pointer guard and a callable invoked under "
          "the lock protect the access",
          "[shared-global]") {
  Project p;
  p.tu("tu.cpp", R"cpp(
#include <functional>
#include <mutex>
#include <thread>
struct Mutex { void Lock(); void Unlock(); };
struct MutexLock {
  explicit MutexLock(Mutex *mu) : mu_(mu) { mu_->Lock(); }
  ~MutexLock() { mu_->Unlock(); }
  Mutex *mu_;
};
Mutex mu;
std::mutex m;
int viaGuard = 0, viaInvoke = 0, bare = 0;
void bump() { ++viaInvoke; }
void a() {
  ++bare;
  { MutexLock l(&mu); ++viaGuard; }
  std::lock_guard<std::mutex> g(m);
  std::invoke(bump);
}
void b() {
  bare = 0;
  { MutexLock l(&mu); viaGuard = 0; }
  std::lock_guard<std::mutex> g(m);
  bump();
}
int main() {
  std::thread x(a), y(b);
  x.join();
  y.join();
}
)cpp");
  auto baked = p.bake();
  auto diags = runCheck(baked);
  INFO(allMessages(diags));
  // Only the control without a lock is reported.
  REQUIRE(diags.size() == 1);
  CHECK(diags[0].entities == std::vector<std::string>{"c:@bare"});
}

TEST_CASE("shared-global: a lambda held in a variable runs under the lock "
          "where it is used; an unlocked lambda is no lock",
          "[shared-global]") {
  Project p;
  p.tu("tu.cpp", R"cpp(
#include <functional>
#include <mutex>
#include <thread>
std::mutex m;
int viaVar = 0, viaInvoke = 0;
void a() {
  auto l = [] { ++viaVar; };
  std::lock_guard<std::mutex> g(m);
  std::invoke(l);
}
void b() { std::lock_guard<std::mutex> g(m); ++viaVar; }
void c() { std::invoke([] { ++viaInvoke; }); }
void d() { std::lock_guard<std::mutex> g(m); ++viaInvoke; }
int main() {
  std::thread x(a), y(b), z(c), w(d);
  x.join();
  y.join();
  z.join();
  w.join();
}
)cpp");
  auto baked = p.bake();
  auto diags = runCheck(baked);
  INFO(allMessages(diags));
  REQUIRE(diags.size() == 1);
  CHECK(diags[0].entities == std::vector<std::string>{"c:@viaInvoke"});
  CHECK(diags[0].kind == Diagnostic::SharedGlobal_Race);
}

TEST_CASE("shared-global: a lambda copied before the lock, or captured by "
          "a runner that locks, is not called unlocked",
          "[shared-global]") {
  Project p;
  p.tu("tu.cpp", R"cpp(
#include <mutex>
#include <thread>
std::mutex m;
int g = 0, h = 0;
void t1() {
  auto l = [] { ++g; };
  auto keep = l;
  std::lock_guard<std::mutex> lk(m);
  l();
  (void)keep;
}
void t3() {
  auto k = [] { ++h; };
  auto runner = [&k] { std::lock_guard<std::mutex> lk(m); k(); };
  runner();
}
void t2() { std::lock_guard<std::mutex> lk(m); ++g; ++h; }
int main() {
  std::thread x(t1), y(t2), z(t3);
  x.join();
  y.join();
  z.join();
}
)cpp");
  auto baked = p.bake();
  auto diags = runCheck(baked);
  INFO(allMessages(diags));
  CHECK(diags.empty());
}

TEST_CASE("shared-global: a callable handed to a function of the program's "
          "own runs under locks the call does not show",
          "[shared-global]") {
  Project p;
  p.tu("tu.cpp", R"cpp(
#include <mutex>
#include <thread>
std::mutex m;
int counter = 0, cnt2 = 0;
template <class F> void withLock(F &&f) { std::lock_guard<std::mutex> g(m); f(); }
void withLock2(void (*f)()) { std::lock_guard<std::mutex> g(m); f(); }
void body() { ++cnt2; }
void t1() { withLock([] { ++counter; }); withLock2(body); }
void t2() { std::lock_guard<std::mutex> g(m); ++counter; ++cnt2; }
int main() {
  std::thread x(t1), y(t2);
  x.join();
  y.join();
}
)cpp");
  auto baked = p.bake();
  auto diags = runCheck(baked);
  INFO(allMessages(diags));
  // The lock withLock takes is unknown at the call, against m: silent.
  CHECK(diags.empty());
}

TEST_CASE("shared-global: a callable handed to a helper that takes no lock, "
          "or to a standard algorithm, still races",
          "[shared-global]") {
  Project p;
  p.tu("tu.cpp", R"cpp(
#include <algorithm>
#include <mutex>
#include <thread>
std::mutex m;
int viaHelper = 0, viaAlgo = 0;
template <class F> void each(F &&f) { f(); }
void t1() {
  each([] { ++viaHelper; });
  int xs[2] = {1, 2};
  std::for_each(xs, xs + 2, [](int) { ++viaAlgo; });
}
void t2() {
  ++viaHelper;
  std::lock_guard<std::mutex> g(m);
  ++viaAlgo;
}
int main() {
  std::thread x(t1), y(t2);
  x.join();
  y.join();
}
)cpp");
  auto baked = p.bake();
  auto diags = runCheck(baked);
  INFO(allMessages(diags));
  std::vector<std::string> raced;
  for (const Diagnostic &d : diags)
    if (d.kind == Diagnostic::SharedGlobal_Race)
      raced.push_back(d.entities.at(0));
  std::sort(raced.begin(), raced.end());
  // The helper's lock is unknown, against no lock at all: disjoint. The
  // algorithm is no receiver of the program's own: the lambda runs
  // unlocked, against m.
  CHECK(raced == std::vector<std::string>{"c:@viaAlgo", "c:@viaHelper"});
}

TEST_CASE("shared-global: an instantiation's accesses carry its locks",
          "[shared-global]") {
  Project p;
  p.tu("tu.cpp", R"cpp(
#include <mutex>
std::mutex m;
struct Glob { static int count; };
int Glob::count = 0;
struct S { using G = Glob; static constexpr bool locked = true; };
struct S2 { using G = Glob; static constexpr bool locked = false; };
// T::G::count is named only in an instantiation.
template <class T> void both() {
  if constexpr (T::locked) {
    std::lock_guard<std::mutex> g(m);
    ++T::G::count;
  } else {
    ++T::G::count;
  }
}
template <class T> void guarded() { std::lock_guard<std::mutex> g(m); ++T::G::count; }
void use() { both<S>(); both<S2>(); guarded<S2>(); }
)cpp");
  auto baked = p.bake();
  const ReferenceIndex &ix = baked.references;
  // Each instantiation's lock set, in the order the bake read them.
  CHECK(lockSets(ix, "both", "Glob::count") == "{} {m}");
  CHECK(lockSets(ix, "guarded", "Glob::count") == "{m}");
}

TEST_CASE("shared-global: the search walks through an instantiation's "
          "calls",
          "[shared-global]") {
  Project p;
  p.tu("tu.cpp", R"cpp(
#include <mutex>
#include <thread>
int inBody = 0, beyond = 0;
void touch() { ++beyond; }
template <class T> void bump() { ++inBody; touch(); }
void t1() { bump<int>(); }
void t2() { ++inBody; ++beyond; }
int main() {
  std::thread x(t1), y(t2);
  x.join();
  y.join();
}
)cpp");
  auto baked = p.bake();
  auto diags = runCheck(baked);
  INFO(allMessages(diags));
  std::vector<std::string> raced;
  for (const Diagnostic &d : diags)
    if (d.kind == Diagnostic::SharedGlobal_Race)
      raced.push_back(d.entities.at(0));
  // touch is reached through bump<int>, under an unknown lock against
  // none. The race on inBody is real but missed: the access in the
  // instantiation's body is the pattern's fact, not found at the
  // instantiation (a documented limit, #156).
  CHECK(raced == std::vector<std::string>{"c:@beyond"});
}

TEST_CASE("shared-global: an implicit constructor's accesses take its use's "
          "locks",
          "[shared-global]") {
  Project p;
  p.tu("tu.cpp", R"cpp(
#include <mutex>
#include <thread>
std::mutex m;
int viaCtor = 0, viaCtorBare = 0, solo = 0, both = 0;
struct B { int x = ++viaCtor; };   // written by the implicit constructor
struct D { int y = ++viaCtorBare; };
struct T { int v = (solo = 1); };
struct U { int w = ++both; };
void t1() {
  { std::lock_guard<std::mutex> g(m); B b; }
  D d;
  T t;
  // Written under m here and, through U's constructor, under none.
  { std::lock_guard<std::mutex> g(m); ++both; }
  U u;
}
void t2() {
  std::lock_guard<std::mutex> g(m);
  ++viaCtor; ++viaCtorBare; ++both;
}
int t3() { return solo; }
int main() {
  std::thread x(t1), y(t2), z(t3);
  x.join();
  y.join();
  z.join();
}
)cpp");
  auto baked = p.bake();
  const ReferenceIndex &ix = baked.references;
  CHECK(lockSets(ix, "t1", "viaCtor") == "{m}");
  CHECK(lockSets(ix, "t1", "viaCtorBare") == "");
  CHECK(lockSets(ix, "t1", "both") == "{m} {}");
  // The constructor's write is a write.
  CHECK(mutates(ix, "t1", "solo"));
  CHECK(mutates(ix, "t1", "viaCtorBare"));
  auto diags = runCheck(baked);
  INFO(allMessages(diags));
  std::vector<std::string> raced;
  for (const Diagnostic &d : diags)
    if (d.kind == Diagnostic::SharedGlobal_Race)
      raced.push_back(d.entities.at(0));
  std::sort(raced.begin(), raced.end());
  CHECK(raced ==
        std::vector<std::string>{"c:@both", "c:@solo", "c:@viaCtorBare"});
}

TEST_CASE("shared-global: the total state bound goes to threads first and "
          "names what it starved",
          "[shared-global]") {
  Project p;
  p.tu("tu.cpp", R"cpp(
#include <thread>
int counter = 0;
void helper() {}
void a() { ++counter; helper(); }
int main() {
  std::thread x(a), y(a);
  x.join();
  y.join();
}
)cpp");
  auto baked = p.bake();
  SharedGlobalOptions tight;
  tight.maxStatesTotal = 1;
  SharedGlobalStats stats;
  auto diags = findUnsynchronizedSharedGlobals(baked.graph, baked.references,
                                               tight, &stats);
  INFO(allMessages(diags));
  // The thread got the one state (its root, where the access is); main got
  // none and says so.
  REQUIRE(diags.size() == 1);
  CHECK(diags[0].kind == Diagnostic::SharedGlobal_Unproven);
  REQUIRE(stats.boundedNames.size() == 2);
  CHECK(stats.boundedContexts == 2);
  CHECK(llvm::StringRef(stats.boundedNames[1]).starts_with("the main thread"));
  CHECK(llvm::StringRef(stats.boundedNames[1]).contains("no state"));
}

TEST_CASE("shared-global: lock facts are the same in any bake order",
          "[shared-global]") {
  Project p;
  p.tu("shared.h", R"cpp(
#pragma once
#include <mutex>
extern std::mutex hm;
extern int fromHeader;
inline void touch() { std::lock_guard<std::mutex> g(hm); ++fromHeader; }
)cpp");
  p.tus.pop_back(); // a header, not a TU
  p.tu("a.cpp", R"cpp(
#include "shared.h"
#include <thread>
std::mutex hm, am;
int fromHeader = 0, plain = 0;
void wa() { touch(); std::lock_guard<std::mutex> g(am); ++plain; }
void wb();
int main() {
  std::thread x(wa), y(wb);
  x.join();
  y.join();
}
)cpp");
  p.tu("b.cpp", R"cpp(
#include "shared.h"
extern std::mutex am;
extern int plain;
std::mutex bm;
void wb() { touch(); std::lock_guard<std::mutex> g(bm); plain = 2; }
)cpp");
  auto forward = p.bake();
  std::reverse(p.tus.begin(), p.tus.end());
  auto backward = p.bake();
  std::string one, two;
  forward.references.encode(one);
  backward.references.encode(two);
  CHECK(one == two);
  auto d1 = runCheck(forward);
  auto d2 = runCheck(backward);
  INFO(allMessages(d1));
  REQUIRE(d1.size() == 1); // plain: am against bm
  REQUIRE(d2.size() == 1);
  CHECK(d1[0].message == d2[0].message);
  CHECK(llvm::StringRef(d1[0].message).contains("'plain'"));
}

TEST_CASE("shared-global: findings are the same in any bake order",
          "[shared-global]") {
  Project p;
  p.tu("a.cpp", R"cpp(
#include <thread>
extern int shared1, shared2;
void w1() { ++shared1; ++shared2; }
void w2();
int main() {
  std::thread x(w1), y(w2);
  x.join();
  y.join();
}
)cpp");
  p.tu("b.cpp", R"cpp(
int shared1 = 0, shared2 = 0;
void w2() { shared1 = shared2; }
)cpp");
  auto forward = p.bake();
  std::reverse(p.tus.begin(), p.tus.end());
  auto backward = p.bake();
  auto one = runCheck(forward);
  auto two = runCheck(backward);
  REQUIRE(one.size() == 2);
  REQUIRE(two.size() == 2);
  for (size_t i = 0; i < one.size(); ++i) {
    CHECK(one[i].message == two[i].message);
    CHECK(one[i].callLocation == two[i].callLocation);
  }
}
