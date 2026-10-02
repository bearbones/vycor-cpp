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

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/JSON.h"

#include <string>

namespace vycor {

// ============================================================================
// Every string the indexes hold is valid UTF-8.
//
// Index strings end up in llvm::json values (every tool payload, dump,
// info), and llvm::json requires UTF-8: a build with assertions aborts on
// anything else, one without replaces each bad byte with U+FFFD. But file
// paths and source text are bytes, and a Latin-1 directory name or a
// Latin-1 comment inside an if condition is legal input. So strings are
// made valid where they enter an index — interned (StringInterner),
// stored inline (CallGraph::addNode, ControlFlowIndex::addCallSiteContext,
// ChannelIndex::addSite), or decoded from a saved index (Snapshot.cpp,
// and the mapped control-flow strings in ControlFlowIndex::stringOf) — the
// same replacement a build without assertions makes at output, so the
// text a query prints is unchanged. Lookups by a raw string (removeTU with
// a TU path as the compilation database spells it) apply the same
// conversion, so they still find what was stored. The snapshot meta keeps
// TU paths raw: warm start stats them. Its JSON output (`info`, the diff
// comparability report) converts.
// ============================================================================

inline bool isValidUtf8(llvm::StringRef s) { return llvm::json::isUTF8(s); }

/// `s` itself when it is valid UTF-8, else a copy with each invalid byte
/// replaced by U+FFFD.
inline std::string validUtf8(llvm::StringRef s) {
  return isValidUtf8(s) ? s.str() : llvm::json::fixUTF8(s);
}

/// In place; no allocation when `s` is already valid.
inline void makeValidUtf8(std::string &s) {
  if (!isValidUtf8(s))
    s = llvm::json::fixUTF8(s);
}

} // namespace vycor
