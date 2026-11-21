// Copyright (c) 2026 The vycor-cpp Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace vycor {
// Establish an instrumented initialization barrier for the lazy state of the
// prebuilt, potentially uninstrumented LLVM and Clang libraries before
// entering the frontend: LLVM's statistics state and, under ThreadSanitizer,
// the name sets Sema builds on first use (parseToolingWarmup).
// Does not enable statistics or serialize subsequent parses.
void initializeToolingRuntime();

// Parse the warm-up declarations, the ones that make Sema build each of its
// function-local name sets. True when they parse without an error. The
// barrier runs this under ThreadSanitizer only; it is declared here so a test
// keeps the declarations valid on every supported LLVM.
bool parseToolingWarmup();
} // namespace vycor
