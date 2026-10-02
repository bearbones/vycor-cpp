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


// Runtime defaults for the sanitizer builds (VYCOR_SANITIZE), compiled into
// every executable of such a build (not into vycor_lib: the runtime's own
// weak default would win over an archive member). ASAN_OPTIONS in the
// environment still overrides each flag.
//
// allow_user_poisoning=0: LLVM's BumpPtrAllocator (llvm/Support/Allocator.h)
// poisons a new slab and unpoisons each allocation, but only where the
// header was compiled with ASan. Our instrumented objects and the
// uninstrumented prebuilt clang/LLVM libraries both emit those inline
// functions, and the linker keeps one copy of each, so a slab can be
// poisoned by one build and allocated from by the other. Every resulting
// report is inside clang (35 of them in the LLVM 21 suite, the first in
// clang::IdentifierTable::get), and ASan has no suppression-file entry for
// use-after-poison. vycor never poisons memory itself, so ignoring manual
// poisoning loses only LLVM's own allocator checks, which the
// uninstrumented libraries do not run anyway.

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define VYCOR_ASAN_BUILD 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#define VYCOR_ASAN_BUILD 1
#endif

#ifdef VYCOR_ASAN_BUILD
extern "C" const char *__asan_default_options() {
  return "allow_user_poisoning=0";
}
#endif
