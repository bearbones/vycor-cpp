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


#pragma once

namespace vycor {

/// Keep `p` allocated, never freed, until the process exits — for the
/// objects this codebase leaks on purpose:
///   - a loaded or baked index in a one-shot query process (destroying
///     maps with millions of entries costs seconds at exit and buys
///     nothing; MegascopeCli.cpp, DiffVerb.cpp);
///   - a crashed parse's TU-local indexes, which may be torn and hold a
///     lock, so they are neither merged nor destroyed (CrashGuard.h).
/// Recording the pointer keeps the object reachable from a global, which
/// is all that distinguishes these from real leaks to LeakSanitizer: the
/// sanitizer jobs then report only the leaks nobody meant. Thread-safe.
void retainPointerUntilExit(const void *p);

template <typename T> T *retainUntilExit(T *p) {
  retainPointerUntilExit(p);
  return p;
}

} // namespace vycor
