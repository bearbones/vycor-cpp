// Copyright (c) 2026 The vycor-cpp Authors
// SPDX-License-Identifier: Apache-2.0
#include "vycor/compat/ToolingRuntime.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/StringRef.h"
#include <mutex>

namespace vycor {
void initializeToolingRuntime() {
  static std::once_flag initialized;
  std::call_once(initialized, [] {
    // GetStatistics materializes StatLock, StatInfo and their timer state.
    // The prebuilt LLVM library publishes these through uninstrumented atomics;
    // its first concurrent use otherwise produces TSan allocation/lock reports.
    // Our call_once gives every tooling caller a visible happens-before edge.
    // Unlike EnableStatistics, this neither enables collection nor prints it.
    (void)llvm::GetStatistics();
  });
}
} // namespace vycor
