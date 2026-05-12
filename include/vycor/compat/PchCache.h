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

#include <string>
#include <unordered_map>
#include <vector>

namespace clang {
namespace tooling {
class CompilationDatabase;
} // namespace tooling
} // namespace clang

namespace vycor {

/// Scans a compilation database for PCH source headers, compiles each one
/// with the tool's Clang, and caches the compiled .pch binaries on disk.
/// When active, the argument adjuster replaces `-include <pch_src>` with
/// `-include-pch <compiled.pch>`, eliminating repeated header parsing.
class PchCache {
public:
  /// cacheDir: where to store compiled .pch files.
  /// clangBin: path to clang++ binary for PCH compilation.
  PchCache(std::string cacheDir, std::string clangBin);

  /// Scan compile commands for PCH source headers referenced by `-include`
  /// flags. For each unique PCH, compile it once with the flags extracted
  /// from the first matching TU. Compiled .pch files are stored in cacheDir.
  void buildFromCompileCommands(
      const clang::tooling::CompilationDatabase &compDb,
      const std::vector<std::string> &files);

  /// Returns the compiled .pch path for a given PCH source header, or
  /// empty string if not cached.
  std::string getCompiledPch(const std::string &pchSourceHeader) const;

  bool empty() const { return cache_.empty(); }

private:
  std::string cacheDir_;
  std::string clangBin_;
  // Map: absolute PCH source header path → compiled .pch binary path
  std::unordered_map<std::string, std::string> cache_;

  /// Extract compile flags suitable for PCH compilation from a TU's command.
  /// Strips the source file and adds -x c++-header.
  static std::vector<std::string> extractPchCompileFlags(
      const std::vector<std::string> &tuArgs);

  /// Compute a short hash key from the PCH path and flags for cache naming.
  static std::string computeCacheKey(const std::string &pchPath,
                                     const std::vector<std::string> &flags);
};

} // namespace vycor
