// Copyright (c) 2026 The vycor-cpp Authors
// SPDX-License-Identifier: Apache-2.0
#include "vycor/compat/ToolingRuntime.h"
#include "llvm/ADT/Statistic.h"
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <thread>
#include <vector>

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
