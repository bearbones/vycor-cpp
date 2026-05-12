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
// This is shared between the Indexer and the Analyzer so both agree on how
// to key type names when consulting the TypeRelationIndex.
inline std::string normalizeTypeForMatching(llvm::StringRef in) {
  std::string s = in.str();

  auto isIdent = [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
  };

  auto stripKeyword = [&](llvm::StringRef kw) {
    size_t pos = 0;
    while ((pos = s.find(kw.str(), pos)) != std::string::npos) {
      bool boundaryLeft = (pos == 0) || !isIdent(s[pos - 1]);
      bool boundaryRight =
          (pos + kw.size() == s.size()) || !isIdent(s[pos + kw.size()]);
      if (boundaryLeft && boundaryRight) {
        s.erase(pos, kw.size());
      } else {
        pos += kw.size();
      }
    }
  };

  stripKeyword("const");
  stripKeyword("volatile");

  // Strip trailing reference markers.
  while (!s.empty() && s.back() == '&')
    s.pop_back();

  // Strip all whitespace so spacing variations don't matter.
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
      out.push_back(c);
  }
  return out;
}

} // namespace vycor
