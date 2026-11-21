// Copyright (c) 2026 The vycor-cpp Authors
// SPDX-License-Identifier: Apache-2.0
#include "vycor/compat/ToolingRuntime.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/StringRef.h"
#include <memory>
#include <mutex>

#if defined(__SANITIZE_THREAD__)
#define VYCOR_THREAD_SANITIZER 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define VYCOR_THREAD_SANITIZER 1
#endif
#endif

namespace vycor {
namespace {
// Sema keeps the standard-library names it attaches implicit attributes to
// (gsl::Owner and gsl::Pointer, lifetime capture, nullability) in
// function-local static string sets (clang's SemaAttr.cpp). Each is built by
// the first parse that reaches it: a class with a name, a class nested in
// another, a named method and an operator of a class in namespace std.
constexpr char kWarmup[] = "namespace std {\n"
                           "struct vycor_warmup {\n"
                           "  struct iterator {};\n"
                           "  void push_back(const int &);\n"
                           "  int &operator[](int);\n"
                           "};\n"
                           "}\n";
} // namespace

bool parseToolingWarmup() {
  return clang::tooling::runToolOnCodeWithArgs(
      std::make_unique<clang::SyntaxOnlyAction>(), kWarmup, {"-std=c++17"},
      "vycor-tooling-warmup.cpp");
}

void initializeToolingRuntime() {
  static std::once_flag initialized;
  std::call_once(initialized, [] {
    // GetStatistics materializes StatLock, StatInfo and their timer state.
    // The prebuilt LLVM library publishes these through uninstrumented atomics;
    // its first concurrent use otherwise produces TSan allocation/lock reports.
    // Our call_once gives every tooling caller a visible happens-before edge.
    // Unlike EnableStatistics, this neither enables collection nor prints it.
    (void)llvm::GetStatistics();
#ifdef VYCOR_THREAD_SANITIZER
    // Sema's name sets are guarded statics, so they are safe as they are. A
    // parse that finds one already built reads it through the guard's fast
    // path, though, and in a prebuilt uninstrumented libclang that path shows
    // ThreadSanitizer no ordering: it reports the set's construction on one
    // parsing thread racing with a lookup on another. Building the sets here
    // orders them for every caller. Other builds have nothing to gain from
    // the extra parse.
    (void)parseToolingWarmup();
#endif
  });
}
} // namespace vycor
