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

#ifndef VYCOR_COMPAT_CLANG_VERSION_H
#define VYCOR_COMPAT_CLANG_VERSION_H

#include "llvm/Config/llvm-config.h"

// Version-check macros for conditional compilation.
//
// Usage:
//   #if VYCOR_LLVM_AT_LEAST(20)
//     // Clang 20+ code path
//   #else
//     // Clang 18/19 code path
//   #endif
#define VYCOR_LLVM_AT_LEAST(major) (LLVM_VERSION_MAJOR >= (major))
#define VYCOR_LLVM_VERSION_IN_RANGE(lo, hi) \
  (LLVM_VERSION_MAJOR >= (lo) && LLVM_VERSION_MAJOR <= (hi))

#endif // VYCOR_COMPAT_CLANG_VERSION_H
