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

#include <cstdint>
#include <string>

namespace vycor {

// ============================================================================
// Index text: every string the megascope indexes hold is valid UTF-8, and
// the conversion that makes it so is exact.
//
// Index strings end up in llvm::json values (every tool payload, dump,
// info), and llvm::json requires UTF-8: a build with assertions aborts on
// anything else. But file paths, USRs (which embed the file name of an
// internal-linkage entity) and source text are bytes, and a Latin-1
// directory name or comment is legal input. Replacing a bad byte with
// U+FFFD would be lossy: "a\xe9.cpp" and "a\xe8.cpp" would become one
// string, so one TU's provenance would merge with the other's and two
// `static helper()` would be one node. The conversion is therefore an
// escape that round-trips:
//
//   - A byte that is not part of a valid UTF-8 sequence, b (0x80..0xFF),
//     becomes the code point U+10FF00 + b (U+10FF80..U+10FFFF, the top of
//     plane 16, private use).
//   - A code point already in that range is escaped byte by byte the same
//     way, so a string that held one literally stays distinct from one that
//     held the bytes it escapes.
//   - Everything else is copied. A valid UTF-8 string with no code point in
//     the escape range — every ordinary string — is unchanged.
//
// fromIndexText(toIndexText(s)) == s for every byte string s, so the
// conversion is injective: distinct raw strings stay distinct strings.
//
// Where it applies (callgraph indexes; anneal's GlobalIndex never reaches
// llvm::json and keeps raw bytes):
//   - Producers: CallGraph::add*, ControlFlowIndex::addCallSiteContext and
//     ChannelIndex::addSite convert every string they are given (raw bytes
//     from the frontend). removeTUs converts its raw TU paths the same way.
//   - Copies: absorb, the snapshot loader and the semantic diff copy index
//     text as it is. The StringInterner stores bytes and never converts.
//   - Lookups (tool arguments, CLI flags, batch requests): lookupText. A
//     valid UTF-8 argument is taken as index text, so a string copied from
//     a tool's output finds what it names; an argument that is not UTF-8
//     (a raw path from the shell) cannot be index text and is converted.
//     The one string a lookup cannot name by its raw spelling is one that
//     literally contains U+10FF80..U+10FFFF; its index text (as printed)
//     still finds it.
//   - Output of strings that are not index text (the snapshot meta's TU
//     paths and config, which stay raw because warm start stats them; CLI
//     paths echoed in a summary): toIndexText at the JSON sink, so a TU path
//     prints the same in `info` as in a tool payload.
// ============================================================================

inline bool isValidUtf8(llvm::StringRef s) { return llvm::json::isUTF8(s); }

namespace utf8_detail {

/// First code point of the escape range: byte b maps to kEscapeBase + b.
constexpr uint32_t kEscapeBase = 0x10FF00;

inline bool isCont(unsigned char c) { return (c & 0xC0) == 0x80; }

/// Length of the valid UTF-8 sequence at s[i] (strict: no overlongs, no
/// surrogates, nothing above U+10FFFF), or 0 if there is none.
inline size_t seqLen(llvm::StringRef s, size_t i) {
  const auto at = [&](size_t k) {
    return static_cast<unsigned char>(s[i + k]);
  };
  const unsigned char c = at(0);
  const size_t left = s.size() - i;
  if (c < 0x80)
    return 1;
  if (c >= 0xC2 && c <= 0xDF)
    return left >= 2 && isCont(at(1)) ? 2 : 0;
  if (c >= 0xE0 && c <= 0xEF) {
    if (left < 3 || !isCont(at(1)) || !isCont(at(2)))
      return 0;
    if (c == 0xE0 && at(1) < 0xA0)
      return 0; // overlong
    if (c == 0xED && at(1) > 0x9F)
      return 0; // surrogate
    return 3;
  }
  if (c >= 0xF0 && c <= 0xF4) {
    if (left < 4 || !isCont(at(1)) || !isCont(at(2)) || !isCont(at(3)))
      return 0;
    if (c == 0xF0 && at(1) < 0x90)
      return 0; // overlong
    if (c == 0xF4 && at(1) > 0x8F)
      return 0; // above U+10FFFF
    return 4;
  }
  return 0;
}

/// Whether the 4-byte sequence at s[i] encodes U+10FF80..U+10FFFF.
inline bool isEscapeCodePoint(llvm::StringRef s, size_t i) {
  return static_cast<unsigned char>(s[i]) == 0xF4 &&
         static_cast<unsigned char>(s[i + 1]) == 0x8F &&
         static_cast<unsigned char>(s[i + 2]) >= 0xBE;
}

inline void appendEscape(std::string &out, unsigned char b) {
  const uint32_t cp = kEscapeBase + b;
  out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
  out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
  out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
  out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
}

} // namespace utf8_detail

/// Whether toIndexText would change `s` (a byte outside a valid sequence,
/// or a code point in the escape range). False for any ASCII string, which
/// is decided without decoding.
inline bool needsIndexEscape(llvm::StringRef s) {
  using namespace utf8_detail;
  for (size_t i = 0; i < s.size();) {
    if (static_cast<unsigned char>(s[i]) < 0x80) {
      ++i;
      continue;
    }
    const size_t n = seqLen(s, i);
    if (n == 0 || (n == 4 && isEscapeCodePoint(s, i)))
      return true;
    i += n;
  }
  return false;
}

/// The index text of raw bytes `s` (see above): valid UTF-8, and exactly
/// reversible by fromIndexText.
inline std::string toIndexText(llvm::StringRef s) {
  using namespace utf8_detail;
  if (!needsIndexEscape(s))
    return s.str();
  std::string out;
  out.reserve(s.size() + 8);
  for (size_t i = 0; i < s.size();) {
    const size_t n = seqLen(s, i);
    if (n == 0 || (n == 4 && isEscapeCodePoint(s, i))) {
      // One byte at a time: the rest of an escape-range code point is
      // escaped by the next iterations (its bytes are not sequence starts).
      appendEscape(out, static_cast<unsigned char>(s[i]));
      ++i;
      continue;
    }
    out.append(s.data() + i, n);
    i += n;
  }
  return out;
}

/// In place; no allocation when `s` needs no escape.
inline void makeIndexText(std::string &s) {
  if (needsIndexEscape(s))
    s = toIndexText(s);
}

/// The raw bytes index text `t` was made from: the inverse of toIndexText.
inline std::string fromIndexText(llvm::StringRef t) {
  using namespace utf8_detail;
  std::string out;
  out.reserve(t.size());
  for (size_t i = 0; i < t.size();) {
    const size_t n = seqLen(t, i);
    if (n == 4 && isEscapeCodePoint(t, i)) {
      const uint32_t cp =
          ((static_cast<unsigned char>(t[i]) & 0x07u) << 18) |
          ((static_cast<unsigned char>(t[i + 1]) & 0x3Fu) << 12) |
          ((static_cast<unsigned char>(t[i + 2]) & 0x3Fu) << 6) |
          (static_cast<unsigned char>(t[i + 3]) & 0x3Fu);
      out.push_back(static_cast<char>(cp - kEscapeBase));
      i += 4;
      continue;
    }
    // Not index text (a byte outside a valid sequence): copied, so the
    // result is still defined for any input.
    const size_t len = n == 0 ? 1 : n;
    out.append(t.data() + i, len);
    i += len;
  }
  return out;
}

/// A lookup key of unknown provenance (a tool argument, a CLI flag): valid
/// UTF-8 is taken as index text as it is; anything else is raw bytes and is
/// converted. Idempotent, and always valid UTF-8.
inline std::string lookupText(llvm::StringRef s) {
  return isValidUtf8(s) ? s.str() : toIndexText(s);
}

} // namespace vycor
