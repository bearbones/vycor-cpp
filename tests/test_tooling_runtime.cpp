// Copyright (c) 2026 The vycor-cpp Authors
// SPDX-License-Identifier: Apache-2.0
#include "vycor/compat/ToolingRuntime.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/ADT/Statistic.h"
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#if defined(__SANITIZE_THREAD__)
#define VYCOR_TEST_THREAD_SANITIZER 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define VYCOR_TEST_THREAD_SANITIZER 1
#endif
#endif

namespace {
llvm::TrackingStatistic runtimeCount("vycor-test", "RuntimeCount",
                                     "Concurrent tooling initialization");
}

TEST_CASE("Concurrent tooling startup initializes LLVM statistics without "
          "enabling them",
          "[worker_pool][tooling-runtime]") {
  const bool enabled = llvm::AreStatisticsEnabled();
  const auto count = runtimeCount.getValue();
  std::atomic<unsigned> arrived{0};
  std::atomic<bool> start{false};
  std::vector<std::thread> threads;
  for (unsigned i = 0; i < 8; ++i) {
    threads.emplace_back([&] {
      ++arrived;
      while (!start.load())
        std::this_thread::yield();
      vycor::initializeToolingRuntime();
      ++runtimeCount;
    });
  }
  while (arrived.load() != 8)
    std::this_thread::yield();
  start = true;
  for (auto &thread : threads)
    thread.join();
  CHECK(runtimeCount.getValue() == count + 8);
  CHECK(llvm::AreStatisticsEnabled() == enabled);
  vycor::initializeToolingRuntime();
  CHECK(llvm::AreStatisticsEnabled() == enabled);
}

TEST_CASE("The tooling warm-up declarations parse cleanly",
          "[worker_pool][tooling-runtime]") {
  CHECK(vycor::parseToolingWarmup());
}

TEST_CASE("Parses ordered only by the clock share the frontend's name sets",
          "[worker_pool][tooling-runtime]") {
  // Declarations that make Sema look names up in each of its lazily built
  // sets: owners and pointers, containers and their iterators, map-like
  // subscripts, capturing methods, nullable classes.
  static const char code[] =
      "namespace std {\n"
      "template <class T> struct basic_string_view { struct iterator {}; };\n"
      "template <class K, class V> struct map {\n"
      "  V &operator[](const K &);\n"
      "  void insert(const K &);\n"
      "};\n"
      "template <class T> struct unique_ptr {};\n"
      "}\n"
      "std::basic_string_view<char> view;\n"
      "std::map<int, int> table;\n"
      "std::unique_ptr<int> owner;\n";
  const auto parse = [] {
    vycor::initializeToolingRuntime();
    return clang::tooling::runToolOnCodeWithArgs(
        std::make_unique<clang::SyntaxOnlyAction>(), code, {"-std=c++17"},
        "name-sets.cpp");
  };
  // Under ThreadSanitizer the second thread starts its parse after the first
  // has finished, and nothing but the clock says so: no join, no atomic, no
  // lock between them. The barrier is then the only ordering the sanitizer
  // can see between the first parse building the sets and the second reading
  // them; without the warm-up it reports that pair as a race. Other builds
  // have no such report to provoke and skip the wait.
#ifdef VYCOR_TEST_THREAD_SANITIZER
  const std::chrono::seconds lag(3);
#else
  const std::chrono::seconds lag(0);
#endif
  bool first = false;
  bool second = false;
  std::thread early([&] { first = parse(); });
  std::thread late([&] {
    std::this_thread::sleep_for(lag);
    second = parse();
  });
  early.join();
  late.join();
  CHECK(first);
  CHECK(second);
}
