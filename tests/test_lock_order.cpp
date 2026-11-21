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

// test_lock_order.cpp — the lock acquisition records the bake writes
// (RefRole::Lock / LockShared, format v18) and the lock-order-inversion
// check (docs/checks/lock-order-inversion.md) over small programs.

#include "vycor/callgraph/ControlFlowIndex.h"
#include "vycor/callgraph/ReferenceIndex.h"
#include "vycor/check/LockOrderInversion.h"
#include "vycor/check/Report.h"

#include <catch2/catch_test_macros.hpp>
#include <clang/Tooling/CompilationDatabase.h>

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FileSystem.h"

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

using namespace vycor;

namespace {

struct Project {
  std::string dir;
  std::vector<std::string> tus;

  Project() {
    llvm::SmallString<128> tmp;
    REQUIRE_FALSE(llvm::sys::fs::createUniqueDirectory("vycor_loi", tmp));
    llvm::SmallString<128> real;
    REQUIRE_FALSE(llvm::sys::fs::real_path(tmp, real));
    dir = std::string(real.str());
  }
  Project(const Project &) = delete;
  Project &operator=(const Project &) = delete;
  ~Project() { llvm::sys::fs::remove_directories(dir); }

  std::string path(const std::string &name) const { return dir + "/" + name; }
  void header(const std::string &name, const std::string &code) const {
    std::ofstream out(path(name));
    out << code;
  }
  void tu(const std::string &name, const std::string &code) {
    header(name, code);
    tus.push_back(path(name));
  }
  BakedIndexes bake() const {
    clang::tooling::FixedCompilationDatabase compDb(
        dir, {"-std=c++17", "-I" + dir});
    return bakeIndexes(compDb, tus, {}, 1);
  }
};

// The display names of a lock text's sets, "{a,b}" each, space separated.
std::string setsOf(llvm::StringRef text) {
  std::string out;
  while (!text.empty()) {
    auto [line, tail] = text.split('\n');
    text = tail;
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

// A function's lock acquisitions, sorted: "lock m2 held {m1}", with the
// target's name (a global mutex) or usr (a member, `?`), and "held {}" for
// none held.
std::vector<std::string> acquisitions(const ReferenceIndex &ix,
                                      const std::string &referrer) {
  std::vector<std::string> out;
  for (const ReferenceInfo &r : ix.allReferences()) {
    if (r.referrerName != referrer || !isLockRole(r.role))
      continue;
    std::string held = setsOf(r.locks);
    out.push_back(std::string(refRoleName(r.role)) + " " + r.targetName +
                  " held " + (held.empty() ? "{}" : held));
  }
  std::sort(out.begin(), out.end());
  return out;
}

uint8_t acquisitionFlags(const ReferenceIndex &ix, const std::string &referrer,
                         const std::string &target) {
  for (const ReferenceInfo &r : ix.allReferences())
    if (r.referrerName == referrer && r.targetName == target &&
        isLockRole(r.role))
      return r.flags;
  FAIL("no acquisition " << referrer << " -> " << target);
  return 0;
}

std::vector<Diagnostic> runCheck(const BakedIndexes &baked,
                                 LockOrderOptions opts = {},
                                 LockOrderStats *stats = nullptr) {
  return findLockOrderInversions(baked.graph, baked.references, opts, stats);
}

std::string allMessages(const std::vector<Diagnostic> &diags) {
  std::string out;
  for (const Diagnostic &d : diags)
    out += d.message + "\n";
  return out;
}

bool contains(const Diagnostic &d, llvm::StringRef text) {
  return llvm::StringRef(d.message).contains(text);
}

} // namespace

// ============================================================================
// Acquisition facts (ReferenceVisitor)
// ============================================================================

TEST_CASE("lock-order facts: guards, lock calls and std::lock record the "
          "locks held before",
          "[lock-order][references]") {
  Project p;
  p.tu("tu.cpp", R"cpp(
#include <mutex>
std::mutex m1, m2, m3;
void guards() {
  std::lock_guard<std::mutex> a(m1);
  std::lock_guard<std::mutex> b(m2);
}
void manual() {
  m1.lock();
  m3.lock();
  m3.unlock();
  m1.unlock();
}
void several() {
  std::scoped_lock both(m1, m2);
  std::lock_guard<std::mutex> c(m3);
}
void viaStdLock() {
  std::unique_lock<std::mutex> a(m1, std::defer_lock);
  std::unique_lock<std::mutex> b(m2, std::defer_lock);
  std::lock(a, b);
}
void deferred() {
  std::lock_guard<std::mutex> g(m3);
  std::unique_lock<std::mutex> u(m1, std::defer_lock);
  u.lock();
}
)cpp");
  auto baked = p.bake();
  const ReferenceIndex &ix = baked.references;
  CHECK(acquisitions(ix, "guards") ==
        std::vector<std::string>{"lock m1 held {}", "lock m2 held {m1}"});
  CHECK(acquisitions(ix, "manual") ==
        std::vector<std::string>{"lock m1 held {}", "lock m3 held {m1}"});
  // A multi-lock guard and std::lock acquire each mutex under the set
  // before them: none orders the others.
  CHECK(acquisitions(ix, "several") ==
        std::vector<std::string>{"lock m1 held {}", "lock m2 held {}",
                                 "lock m3 held {m1,m2}"});
  CHECK(acquisitions(ix, "viaStdLock") ==
        std::vector<std::string>{"lock m1 held {}", "lock m2 held {}"});
  CHECK(acquisitions(ix, "deferred") ==
        std::vector<std::string>{"lock m1 held {m3}", "lock m3 held {}"});
}

TEST_CASE("lock-order facts: a try or adopted lock is no acquisition, a "
          "shared one is lock_shared",
          "[lock-order][references]") {
  Project p;
  p.tu("tu.cpp", R"cpp(
#include <mutex>
#include <shared_mutex>
std::mutex m;
std::timed_mutex tm;
std::shared_mutex sm;
void tries() {
  if (m.try_lock()) m.unlock();
  std::unique_lock<std::mutex> t(m, std::try_to_lock);
}
void adopts() {
  m.lock();
  std::lock_guard<std::mutex> g(m, std::adopt_lock);
}
void timed() {
  std::unique_lock<std::timed_mutex> u(tm, std::defer_lock);
  (void)u.try_lock_for(std::chrono::milliseconds(1));
}
void readers() {
  std::shared_lock<std::shared_mutex> r(sm);
}
void manualShared() {
  sm.lock_shared();
  sm.unlock_shared();
  sm.lock();
  sm.unlock();
}
)cpp");
  auto baked = p.bake();
  const ReferenceIndex &ix = baked.references;
  CHECK(acquisitions(ix, "tries").empty());
  CHECK(acquisitions(ix, "adopts") ==
        std::vector<std::string>{"lock m held {}"});
  CHECK(acquisitions(ix, "timed").empty());
  CHECK(acquisitions(ix, "readers") ==
        std::vector<std::string>{"lock_shared sm held {}"});
  CHECK(acquisitions(ix, "manualShared") ==
        std::vector<std::string>{"lock sm held {}",
                                 "lock_shared sm held {}"});
}

TEST_CASE("lock-order facts: recursion flags and pthread read and write locks",
          "[lock-order][references]") {
  Project p;
  p.tu("tu.cpp", R"cpp(
#include <mutex>
#include <pthread.h>
std::mutex plain;
std::recursive_mutex rec;
pthread_mutex_t pm = PTHREAD_MUTEX_INITIALIZER;
pthread_rwlock_t rw = PTHREAD_RWLOCK_INITIALIZER;
void f() {
  std::lock_guard<std::mutex> a(plain);
  std::lock_guard<std::recursive_mutex> b(rec);
}
void g() {
  pthread_mutex_lock(&pm);
  pthread_mutex_unlock(&pm);
  pthread_rwlock_rdlock(&rw);
  pthread_rwlock_unlock(&rw);
  pthread_rwlock_wrlock(&rw);
  pthread_rwlock_unlock(&rw);
  if (pthread_mutex_trylock(&pm) == 0) pthread_mutex_unlock(&pm);
}
)cpp");
  auto baked = p.bake();
  const ReferenceIndex &ix = baked.references;
  CHECK((acquisitionFlags(ix, "f", "plain") & kRefNonRecursive) != 0);
  CHECK((acquisitionFlags(ix, "f", "rec") & kRefRecursive) != 0);
  CHECK((acquisitionFlags(ix, "g", "pm") &
         (kRefRecursive | kRefNonRecursive)) == 0);
  CHECK(acquisitions(ix, "g") ==
        std::vector<std::string>{"lock pm held {}", "lock rw held {}",
                                 "lock_shared rw held {}"});
}

TEST_CASE("lock-order facts: acquisition records survive encode, decode and "
          "absorb",
          "[lock-order][references][snapshot]") {
  ReferenceIndex ix;
  ReferenceFact r;
  r.referrerUsr = "c:@F@f#";
  r.targetUsr = "c:@m2";
  r.role = RefRole::Lock;
  r.file = "/src/f.cpp";
  r.line = 5;
  r.col = 3;
  r.locks = "5:3\tc:@m1\tm1";
  r.flags = kRefNonRecursive;
  ix.addReference(r, "/src/f.cpp");
  ReferenceFact shared = r;
  shared.role = RefRole::LockShared;
  shared.locks.clear();
  ix.addReference(shared, "/src/f.cpp");

  std::string bytes;
  ix.encode(bytes);
  ReferenceIndex back;
  REQUIRE(back.decode(bytes.data(), bytes.data() + bytes.size(), false));
  std::string again;
  back.encode(again);
  CHECK(again == bytes);
  auto refs = back.referencesTo("c:@m2");
  REQUIRE(refs.size() == 2);
  CHECK(refs[0].role == RefRole::Lock);
  CHECK(refs[0].locks == "5:3\tc:@m1\tm1");
  CHECK((refs[0].flags & kRefNonRecursive) != 0);
  CHECK(refs[1].role == RefRole::LockShared);
  CHECK(refRoleFromName("lock_shared") == RefRole::LockShared);

  ReferenceIndex absorbed;
  absorbed.absorb(back);
  std::string third;
  absorbed.encode(third);
  CHECK(third == bytes);
}

// ============================================================================
// The check
// ============================================================================

TEST_CASE("lock-order: two locks taken in opposite orders in two TUs",
          "[lock-order]") {
  Project p;
  p.header("locks.h", R"cpp(
#include <mutex>
extern std::mutex accounts, audit;
void credit();
void record();
)cpp");
  p.tu("a.cpp", R"cpp(
#include "locks.h"
std::mutex accounts;
void credit() {
  std::lock_guard<std::mutex> g(accounts);
  record();
}
)cpp");
  p.tu("b.cpp", R"cpp(
#include "locks.h"
std::mutex audit;
void record() { std::lock_guard<std::mutex> g(audit); }
void flush() {
  std::lock_guard<std::mutex> g(audit);
  std::lock_guard<std::mutex> h(accounts);
}
)cpp");
  auto baked = p.bake();
  LockOrderStats stats;
  auto diags = runCheck(baked, {}, &stats);
  INFO(allMessages(diags));
  REQUIRE(diags.size() == 1);
  const Diagnostic &d = diags[0];
  CHECK(d.kind == Diagnostic::LockOrder_Inversion);
  CHECK(d.checkName == "lock-order-inversion");
  CHECK(checkNameOf(d) == "lock-order-inversion");
  CHECK(diagnosticSeverity(d) == Severity::Warning);
  CHECK(contains(d, "'accounts' -> 'audit' -> 'accounts'"));
  CHECK(contains(d, "credit calls record at "));
  CHECK(contains(d, "while holding 'accounts', and record acquires 'audit'"));
  CHECK(contains(d, "flush acquires 'accounts' at "));
  CHECK(contains(d, "while holding 'audit'"));
  // Located at the first edge (from the lowest identity: c:@accounts).
  CHECK(llvm::StringRef(d.callLocation).contains("a.cpp:6:"));
  REQUIRE(d.entities.size() == 2);
  CHECK(d.entities[0] == "c:@accounts");
  CHECK(d.entities[1] == "c:@audit");
  CHECK(stats.locks == 2);
  CHECK(stats.orderEdges == 2);
  CHECK(stats.cycles == 1);
}

TEST_CASE("lock-order: three locks in a cycle across three TUs",
          "[lock-order]") {
  Project p;
  p.header("locks.h", R"cpp(
#include <mutex>
extern std::mutex la, lb, lc;
void takeB();
void takeC();
void takeA();
)cpp");
  p.tu("a.cpp", R"cpp(
#include "locks.h"
std::mutex la;
void takeA() { std::lock_guard<std::mutex> g(la); }
void ab() { std::lock_guard<std::mutex> g(la); takeB(); }
)cpp");
  p.tu("b.cpp", R"cpp(
#include "locks.h"
std::mutex lb;
void takeB() { std::lock_guard<std::mutex> g(lb); }
void bc() { std::lock_guard<std::mutex> g(lb); takeC(); }
)cpp");
  p.tu("c.cpp", R"cpp(
#include "locks.h"
std::mutex lc;
void takeC() { std::lock_guard<std::mutex> g(lc); }
void ca() { std::lock_guard<std::mutex> g(lc); takeA(); }
)cpp");
  auto baked = p.bake();
  auto diags = runCheck(baked);
  INFO(allMessages(diags));
  REQUIRE(diags.size() == 1);
  CHECK(diags[0].kind == Diagnostic::LockOrder_Inversion);
  CHECK(contains(diags[0], "'la' -> 'lb' -> 'lc' -> 'la'"));
  CHECK(contains(diags[0], "ab calls takeB"));
  CHECK(contains(diags[0], "bc calls takeC"));
  CHECK(contains(diags[0], "ca calls takeA"));
  CHECK(diags[0].entities ==
        std::vector<std::string>{"c:@la", "c:@lb", "c:@lc"});
}

TEST_CASE("lock-order: std::scoped_lock and std::lock create no order",
          "[lock-order]") {
  Project p;
  p.tu("tu.cpp", R"cpp(
#include <mutex>
std::mutex x, y;
void one() { std::scoped_lock both(x, y); }
void two() { std::scoped_lock both(y, x); }
void three() {
  std::unique_lock<std::mutex> a(y, std::defer_lock), b(x, std::defer_lock);
  std::lock(a, b);
}
void four() { std::lock(x, y); x.unlock(); y.unlock(); }
)cpp");
  auto baked = p.bake();
  LockOrderStats stats;
  auto diags = runCheck(baked, {}, &stats);
  INFO(allMessages(diags));
  CHECK(diags.empty());
  CHECK(stats.orderEdges == 0);
}

TEST_CASE("lock-order: a recursive mutex locked again is no self-deadlock, a "
          "plain one is",
          "[lock-order]") {
  Project p;
  p.tu("tu.cpp", R"cpp(
#include <mutex>
#include <pthread.h>
std::recursive_mutex rec;
std::mutex plain;
pthread_mutex_t pm = PTHREAD_MUTEX_INITIALIZER;
void innerRec() { std::lock_guard<std::recursive_mutex> g(rec); }
void outerRec() { std::lock_guard<std::recursive_mutex> g(rec); innerRec(); }
void innerPlain() { std::lock_guard<std::mutex> g(plain); }
void outerPlain() { std::lock_guard<std::mutex> g(plain); innerPlain(); }
void innerPm() { pthread_mutex_lock(&pm); pthread_mutex_unlock(&pm); }
void outerPm() { pthread_mutex_lock(&pm); innerPm(); pthread_mutex_unlock(&pm); }
)cpp");
  auto baked = p.bake();
  LockOrderStats stats;
  auto diags = runCheck(baked, {}, &stats);
  INFO(allMessages(diags));
  REQUIRE(diags.size() == 2);
  // Sorted by location: plain's (line 10) before pm's (line 12).
  CHECK(diags[0].kind == Diagnostic::LockOrder_Inversion);
  CHECK(contains(diags[0], "'plain' is acquired while already held: "
                           "outerPlain calls innerPlain"));
  CHECK(diags[0].entities == std::vector<std::string>{"c:@plain"});
  CHECK(diags[1].kind == Diagnostic::LockOrder_Unproven);
  CHECK(contains(diags[1], "whether 'pm' is recursive is not known"));
  CHECK(stats.selfDeadlocks == 2);
}

TEST_CASE("lock-order: member mutexes order classes, and one class's mutex "
          "under another object's is not reported",
          "[lock-order]") {
  Project p;
  p.header("types.h", R"cpp(
#include <mutex>
struct Logger {
  std::mutex mu;
  void write();
  void flushTo(struct Registry &r);
};
struct Registry {
  std::mutex mu;
  Logger *log;
  void add();
  int count();
};
struct Node {
  std::mutex mu;
  Node *child;
  void visit();
};
)cpp");
  p.tu("registry.cpp", R"cpp(
#include "types.h"
void Registry::add() {
  std::lock_guard<std::mutex> g(mu);
  log->write();
}
int Registry::count() { std::lock_guard<std::mutex> g(mu); return 0; }
void Node::visit() {
  std::lock_guard<std::mutex> g(mu);
  if (child) child->visit();
}
)cpp");
  p.tu("logger.cpp", R"cpp(
#include "types.h"
void Logger::write() { std::lock_guard<std::mutex> g(mu); }
void Logger::flushTo(Registry &r) {
  std::lock_guard<std::mutex> g(mu);
  r.count();
}
)cpp");
  auto baked = p.bake();
  LockOrderStats stats;
  auto diags = runCheck(baked, {}, &stats);
  INFO(allMessages(diags));
  REQUIRE(diags.size() == 1);
  CHECK(diags[0].kind == Diagnostic::LockOrder_Inversion);
  CHECK(contains(diags[0], "'Logger::mu' -> 'Registry::mu' -> 'Logger::mu'"));
  CHECK(stats.memberSelfEdges >= 1); // Node::visit
  CHECK(stats.selfDeadlocks == 0);
}

TEST_CASE("lock-order: a mutex the bake cannot name forms no cycle",
          "[lock-order]") {
  Project p;
  p.tu("tu.cpp", R"cpp(
#include <mutex>
std::mutex g;
void viaParam(std::mutex &m) {
  std::lock_guard<std::mutex> a(m);
  std::lock_guard<std::mutex> b(g);
}
void other(std::mutex &m) {
  std::lock_guard<std::mutex> b(g);
  std::lock_guard<std::mutex> a(m);
}
)cpp");
  auto baked = p.bake();
  LockOrderStats stats;
  auto diags = runCheck(baked, {}, &stats);
  INFO(allMessages(diags));
  CHECK(diags.empty());
  CHECK(stats.unknownAcquisitions == 2);
}

TEST_CASE("lock-order: a try_lock in the reverse order waits for nothing",
          "[lock-order]") {
  Project p;
  p.tu("tu.cpp", R"cpp(
#include <mutex>
std::mutex x, y;
void forward() { std::lock_guard<std::mutex> a(x); std::lock_guard<std::mutex> b(y); }
void backward() {
  std::lock_guard<std::mutex> b(y);
  std::unique_lock<std::mutex> a(x, std::try_to_lock);
}
)cpp");
  auto baked = p.bake();
  auto diags = runCheck(baked);
  INFO(allMessages(diags));
  CHECK(diags.empty());
}

TEST_CASE("lock-order: a lock held around both orders gates the cycle",
          "[lock-order]") {
  Project p;
  p.tu("tu.cpp", R"cpp(
#include <mutex>
std::mutex gate, x, y;
void forward() {
  std::lock_guard<std::mutex> g(gate);
  std::lock_guard<std::mutex> a(x);
  std::lock_guard<std::mutex> b(y);
}
void backward() {
  std::lock_guard<std::mutex> g(gate);
  std::lock_guard<std::mutex> b(y);
  std::lock_guard<std::mutex> a(x);
}
)cpp");
  auto baked = p.bake();
  LockOrderStats stats;
  auto diags = runCheck(baked, {}, &stats);
  INFO(allMessages(diags));
  CHECK(diags.empty());
  CHECK(stats.cycles == 1);
  CHECK(stats.gatedCycles == 1);
}

TEST_CASE("lock-order: a shared acquisition makes the cycle a note",
          "[lock-order]") {
  Project p;
  p.tu("tu.cpp", R"cpp(
#include <mutex>
#include <shared_mutex>
std::shared_mutex table;
std::mutex stats;
void reader() {
  std::shared_lock<std::shared_mutex> r(table);
  std::lock_guard<std::mutex> s(stats);
}
void writer() {
  std::lock_guard<std::mutex> s(stats);
  std::shared_lock<std::shared_mutex> r(table);
}
)cpp");
  auto baked = p.bake();
  auto diags = runCheck(baked);
  INFO(allMessages(diags));
  REQUIRE(diags.size() == 1);
  CHECK(diags[0].kind == Diagnostic::LockOrder_Shared);
  CHECK(diagnosticSeverity(diags[0]) == Severity::Note);
  CHECK(contains(diags[0], "acquires (shared) 'table'"));
  CHECK(contains(diags[0], "Not proven: a witness acquires its lock shared"));
}

TEST_CASE("lock-order: a path through virtual dispatch is a plausible note",
          "[lock-order]") {
  Project p;
  p.tu("tu.cpp", R"cpp(
#include <mutex>
std::mutex x, y;
struct Sink { virtual void put() = 0; virtual ~Sink() = default; };
struct Locked : Sink { void put() override { std::lock_guard<std::mutex> g(y); } };
Sink *sink();
void forward() { std::lock_guard<std::mutex> a(x); sink()->put(); }
void backward() { std::lock_guard<std::mutex> b(y); std::lock_guard<std::mutex> a(x); }
)cpp");
  auto baked = p.bake();
  auto diags = runCheck(baked);
  INFO(allMessages(diags));
  REQUIRE(diags.size() == 1);
  CHECK(diags[0].kind == Diagnostic::LockOrder_Plausible);
  CHECK(diagnosticSeverity(diags[0]) == Severity::Note);
  CHECK(contains(diags[0], "forward calls Locked::put"));
  CHECK(contains(diags[0], "plausible call edge"));
}

TEST_CASE("lock-order: facts that miss part of the program, or a bound, "
          "downgrade to unproven",
          "[lock-order]") {
  Project p;
  p.tu("tu.cpp", R"cpp(
#include <mutex>
std::mutex x, y;
void takeY() { std::lock_guard<std::mutex> b(y); }
void forward() { std::lock_guard<std::mutex> a(x); takeY(); }
void backward() { std::lock_guard<std::mutex> b(y); std::lock_guard<std::mutex> a(x); }
)cpp");
  auto baked = p.bake();
  LockOrderOptions gap;
  gap.coverageGap = "the facts cover 1 of the 2 translation units";
  auto diags = runCheck(baked, gap);
  INFO(allMessages(diags));
  REQUIRE(diags.size() == 1);
  CHECK(diags[0].kind == Diagnostic::LockOrder_Unproven);
  CHECK(diagnosticSeverity(diags[0]) == Severity::Note);
  CHECK(contains(diags[0], "Not proven: the facts cover 1 of the 2"));

  // A closure budget too small for one set: the call edge is not seen,
  // and what is seen is unproven.
  LockOrderOptions tight;
  tight.maxClosureWords = 1;
  LockOrderStats stats;
  auto bounded = runCheck(baked, tight, &stats);
  INFO(allMessages(bounded));
  CHECK(stats.closureBounded);
  for (const Diagnostic &d : bounded)
    CHECK(d.kind == Diagnostic::LockOrder_Unproven);
}

TEST_CASE("lock-order: the enumeration stops at its per-group bound",
          "[lock-order]") {
  Project p;
  // Four locks, every order between every pair: many cycles in one group.
  p.tu("tu.cpp", R"cpp(
#include <mutex>
std::mutex a, b, c, d;
#define PAIR(n, x, y) void n() { std::lock_guard<std::mutex> g1(x); std::lock_guard<std::mutex> g2(y); }
PAIR(ab, a, b) PAIR(ba, b, a) PAIR(ac, a, c) PAIR(ca, c, a) PAIR(ad, a, d)
PAIR(da, d, a) PAIR(bc, b, c) PAIR(cb, c, b) PAIR(bd, b, d) PAIR(db, d, b)
PAIR(cd, c, d) PAIR(dc, d, c)
)cpp");
  auto baked = p.bake();
  LockOrderStats all;
  auto every = runCheck(baked, {}, &all);
  CHECK(all.lockGroups == 1);
  CHECK(every.size() == 20); // 6 two-cycles, 8 three-cycles, 6 four-cycles
  LockOrderOptions capped;
  capped.maxCyclesPerGroup = 3;
  LockOrderStats stats;
  auto some = runCheck(baked, capped, &stats);
  INFO(allMessages(some));
  CHECK(some.size() == 3);
  CHECK(stats.boundedGroups == 1);
  for (const Diagnostic &d : some)
    CHECK(contains(d, "were not enumerated"));
}

TEST_CASE("lock-order: findings are the same in any bake order",
          "[lock-order]") {
  Project p;
  p.header("locks.h", R"cpp(
#include <mutex>
extern std::mutex m1, m2, m3;
void use1(); void use2(); void use3();
)cpp");
  p.tu("a.cpp", R"cpp(
#include "locks.h"
std::mutex m1;
void use1() { std::lock_guard<std::mutex> g(m1); }
void x() { std::lock_guard<std::mutex> g(m1); use2(); use3(); }
)cpp");
  p.tu("b.cpp", R"cpp(
#include "locks.h"
std::mutex m2;
void use2() { std::lock_guard<std::mutex> g(m2); }
void y() { std::lock_guard<std::mutex> g(m2); use1(); use3(); }
)cpp");
  p.tu("c.cpp", R"cpp(
#include "locks.h"
std::mutex m3;
void use3() { std::lock_guard<std::mutex> g(m3); }
void z() { std::lock_guard<std::mutex> g(m3); use1(); use2(); }
)cpp");
  auto forward = p.bake();
  std::reverse(p.tus.begin(), p.tus.end());
  auto backward = p.bake();
  auto one = runCheck(forward);
  auto two = runCheck(backward);
  REQUIRE(one.size() == 5); // three two-cycles and two three-cycles
  REQUIRE(two.size() == one.size());
  for (size_t i = 0; i < one.size(); ++i) {
    CHECK(one[i].message == two[i].message);
    CHECK(one[i].callLocation == two[i].callLocation);
    CHECK(one[i].entities == two[i].entities);
  }
}
