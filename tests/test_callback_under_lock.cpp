// Copyright (c) 2026 The vycor-cpp Authors
// SPDX-License-Identifier: Apache-2.0
#include "vycor/check/Analyzer.h"
#include "vycor/check/CallbackUnderLock.h"
#include "vycor/check/CheckSet.h"
#include "vycor/check/Report.h"
#include "clang/Tooling/Tooling.h"
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>
using namespace vycor;
namespace {
// Minimal declaration models keep unit tests hermetic across standard
// libraries.
const char *preamble = R"cpp(
namespace std {
struct mutex {}; struct timed_mutex {}; struct shared_mutex {};
struct recursive_mutex {}; struct null_mutex {};
struct defer_lock_t {}; struct adopt_lock_t {}; struct try_to_lock_t {};
constexpr defer_lock_t defer_lock{}; constexpr adopt_lock_t adopt_lock{};
constexpr try_to_lock_t try_to_lock{};
template<class M> struct lock_guard { explicit lock_guard(M&); lock_guard(M&,adopt_lock_t); ~lock_guard(); };
template<class M> struct unique_lock {
  explicit unique_lock(M&); unique_lock(M&,defer_lock_t); unique_lock(M&,try_to_lock_t); ~unique_lock();
  void lock(); void unlock(); explicit operator bool() const;
};
template<class... M> struct scoped_lock { explicit scoped_lock(M&...); ~scoped_lock(); };
template<class S> struct function;
template<> struct function<void()> { void operator()() const; };
}
)cpp";
struct Consumer : clang::ASTConsumer {
  std::vector<Diagnostic> &out;
  explicit Consumer(std::vector<Diagnostic> &out) : out(out) {}
  void HandleTranslationUnit(clang::ASTContext &ctx) override {
    analyzeCallbacksUnderLock(ctx, out);
  }
};
struct Action : clang::ASTFrontendAction {
  std::vector<Diagnostic> &out;
  explicit Action(std::vector<Diagnostic> &out) : out(out) {}
  std::unique_ptr<clang::ASTConsumer>
  CreateASTConsumer(clang::CompilerInstance &, llvm::StringRef) override {
    return std::make_unique<Consumer>(out);
  }
};
std::vector<Diagnostic> check(const std::string &source) {
  std::vector<Diagnostic> out;
  REQUIRE(clang::tooling::runToolOnCodeWithArgs(
      std::make_unique<Action>(out), std::string(preamble) + source,
      {"-std=c++17"}, "callback.cpp"));
  return out;
}
} // namespace
TEST_CASE(
    "Callback guard tracks stored callbacks, pointers and functor parameters",
    "[callback-under-lock]") {
  auto result = check(R"cpp(
std::mutex m;
struct Sink { std::function<void()> callback; void log() { std::lock_guard<std::mutex> guard(m); callback(); } };
void notify(void (*cb)()) { std::lock_guard<std::mutex> guard(m); cb(); }
template<class F> void supply(F cb) { std::lock_guard<std::mutex> guard(m); cb(); }
struct Supplied { void operator()(); };
void use() { supply(Supplied{}); }
)cpp");
  REQUIRE(result.size() == 3);
  for (const auto &d : result) {
    CHECK(d.checkName == "callback-under-lock");
    CHECK(d.message.find("nonrecursive std::mutex") != std::string::npos);
    CHECK_FALSE(d.entities.empty());
  }
}
TEST_CASE(
    "Callback guard follows inherited virtual dispatch and recursive helpers",
    "[callback-under-lock]") {
  auto result = check(R"cpp(
std::mutex m;
struct Base { virtual void sink()=0; void log() { std::lock_guard<std::mutex> guard(m); sink(); } };
struct Derived:Base { std::function<void()> callback; void sink() override { callback(); } };
void loop(void (*cb)(),bool recurse) { if (recurse) loop(cb,false); cb(); }
void caller(void (*cb)()) { std::lock_guard<std::mutex> guard(m); loop(cb,true); }
)cpp");
  REQUIRE(result.size() == 2);
  CHECK(std::any_of(result.begin(), result.end(), [](const auto &d) {
    return d.message.find("Base::log") != std::string::npos &&
           d.message.find("Derived::sink") != std::string::npos;
  }));
}
TEST_CASE("Callback guard is silent after guard destruction and helper return",
          "[callback-under-lock]") {
  CHECK(check(R"cpp(
std::mutex m;
void handoff(std::unique_lock<std::mutex>&);
template<class F> void known(F cb) { std::lock_guard<std::mutex> guard(m); cb(); }
void known_empty() { known([] {}); }
void snapshot() { std::lock_guard<std::mutex> guard(m); }
void safe(std::function<void()> cb) { { std::lock_guard<std::mutex> guard(m); } cb(); }
void safe_helper(std::function<void()> cb) { snapshot(); cb(); }
void constructed_only(std::function<void()> cb) { std::lock_guard<std::mutex> guard(m); auto later=[&] { cb(); }; (void)later; }
void after_unlock(std::function<void()> cb) { std::unique_lock<std::mutex> guard(m); guard.unlock(); cb(); }
void deferred(std::function<void()> cb) { std::unique_lock<std::mutex> guard(m,std::defer_lock); cb(); }
void tried(std::function<void()> cb) { std::unique_lock<std::mutex> guard(m,std::try_to_lock); cb(); }
void adopted(std::function<void()> cb) { std::lock_guard<std::mutex> guard(m,std::adopt_lock); cb(); }
void not_called(std::function<void()> cb) { std::lock_guard<std::mutex> guard(m); if(false) cb(); }
void short_circuit(std::function<void()> cb) { std::lock_guard<std::mutex> guard(m); (void)(false && (cb(),true)); }
void transferred(std::function<void()> cb) { std::unique_lock<std::mutex> guard(m); handoff(guard); cb(); }
void recursive(std::recursive_mutex &m,std::function<void()> cb) { std::lock_guard<std::recursive_mutex> guard(m); cb(); }
void custom(std::null_mutex &m,std::function<void()> cb) { std::lock_guard<std::null_mutex> guard(m); cb(); }
)cpp")
            .empty());
}
TEST_CASE("Callback guard handles nested unlocks, explicit relock, branches "
          "and scope",
          "[callback-under-lock]") {
  auto result = check(R"cpp(
std::mutex m;
void safe(std::function<void()> cb) { std::unique_lock<std::mutex> guard(m); { guard.unlock(); } cb(); }
void relock(std::function<void()> cb) { std::unique_lock<std::mutex> guard(m,std::defer_lock); guard.lock(); cb(); }
void ownership_query(std::function<void()> cb) { std::unique_lock<std::mutex> guard(m); if(guard) cb(); }
void explicit_call(std::function<void()> cb) { std::lock_guard<std::mutex> guard(m); cb.operator()(); }
void maybe(std::function<void()> cb,bool b) { std::unique_lock<std::mutex> guard(m); if(b) guard.unlock(); cb(); }
void both(std::function<void()> cb,bool b) { std::unique_lock<std::mutex> guard(m); if(b) guard.unlock(); else guard.unlock(); cb(); }
void ifinit(std::function<void()> cb,bool b) { if(std::lock_guard<std::mutex> guard(m); b) {} cb(); }
void multi(std::function<void()> cb,std::mutex &n) { std::scoped_lock<std::mutex,std::mutex> guard(m,n); cb(); }
)cpp");
  REQUIRE(result.size() == 5);
}
TEST_CASE(
    "Callback guard paths are deterministic and have no call-depth cutoff",
    "[callback-under-lock]") {
  std::string code =
      "std::mutex m; void step40(std::function<void()> cb) { cb(); }\n";
  for (int i = 39; i >= 0; --i)
    code += "void step" + std::to_string(i) +
            "(std::function<void()> cb) { step" + std::to_string(i + 1) +
            "(cb); }\n";
  code += "void root(std::function<void()> cb) { std::lock_guard<std::mutex> "
          "guard(m); step0(cb); }\n";
  auto a = check(code);
  auto b = check(code);
  REQUIRE(a.size() == 1);
  REQUIRE(b.size() == 1);
  CHECK(a[0].message == b[0].message);
  CHECK(a[0].message.find("step20") != std::string::npos);
  CHECK(a[0].message.find("step40") != std::string::npos);
}
TEST_CASE("Callback guard is opt-in and part of the check catalog",
          "[callback-under-lock]") {
  CHECK_FALSE(defaultCheckSet().count("callback-under-lock"));
  CHECK_FALSE(AnalysisOptions{}.enableCallbackUnderLockDiag);
  auto checks = defaultCheckSet();
  std::string error;
  REQUIRE(resolveCheckSpec({"all"}, checks, error));
  CHECK(checks.count("callback-under-lock"));
  CHECK(checkSeverity("callback-under-lock") == Severity::Warning);
}
