// Copyright (c) 2026 The vycor-cpp Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace vycor {
// Establish an instrumented initialization barrier for LLVM's lazy statistics
// state before entering the prebuilt, potentially uninstrumented frontend.
// Does not enable statistics or serialize subsequent parses.
void initializeToolingRuntime();
} // namespace vycor
