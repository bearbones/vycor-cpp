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
#include "llvm/ADT/StringSwitch.h"

#include <cctype>
#include <string>

namespace vycor {

// Normalize a type string produced by clang::QualType::getAsString() so that
// semantically-equivalent spellings compare equal as plain strings.
//
// Strips:
//   - const / volatile qualifiers (with word-boundary checks so e.g. an
//     identifier containing "const" is left alone)
//   - trailing reference markers (so "const Vector &" == "Vector")
//   - all whitespace (so "long long int" is consistent regardless of how
//     the upstream printer spaced the tokens)
//
// Single-pass O(n) implementation with zero inner-loop allocations or shifts.
// Shared between the Indexer and the Analyzer so both agree on how to key type
// names when consulting the TypeRelationIndex.
[[nodiscard]] inline std::string normalizeTypeForMatching(llvm::StringRef in) {
  auto isIdent = [](char c) noexcept {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
  };
  auto isWs = [](char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
  };

  std::string out;
  out.reserve(in.size());

  size_t i = 0;
  const size_t n = in.size();
  while (i < n) {
    const char c = in[i];
    if (isWs(c)) {
      ++i;
      continue;
    }
    if (isIdent(c)) {
      const size_t start = i;
      do {
        ++i;
      } while (i < n && isIdent(in[i]));
      const llvm::StringRef tok = in.slice(start, i);
      if (tok != "const" && tok != "volatile")
        out.append(tok.data(), tok.size());
      continue;
    }
    out.push_back(c);
    ++i;
  }

  // Strip trailing reference markers.
  while (!out.empty() && out.back() == '&')
    out.pop_back();

  return out;
}

// Fixed set of builtin C++ arithmetic type names after
// normalizeTypeForMatching(). Any two normalized type names in this set are
// mutually convertible under C++ standard arithmetic conversions.
[[nodiscard]] inline bool
isArithmeticNormalizedType(llvm::StringRef normalized) noexcept {
  return llvm::StringSwitch<bool>(normalized)
      .Case("bool", true)
      .Case("char", true)
      .Case("signedchar", true)
      .Case("unsignedchar", true)
      .Case("wchar_t", true)
      .Case("char8_t", true)
      .Case("char16_t", true)
      .Case("char32_t", true)
      .Case("short", true)
      .Case("shortint", true)
      .Case("unsignedshort", true)
      .Case("unsignedshortint", true)
      .Case("int", true)
      .Case("unsigned", true)
      .Case("unsignedint", true)
      .Case("long", true)
      .Case("longint", true)
      .Case("unsignedlong", true)
      .Case("unsignedlongint", true)
      .Case("longlong", true)
      .Case("longlongint", true)
      .Case("unsignedlonglong", true)
      .Case("unsignedlonglongint", true)
      .Case("float", true)
      .Case("double", true)
      .Case("longdouble", true)
      .Default(false);
}

} // namespace vycor
