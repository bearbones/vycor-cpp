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

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/JSON.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace vycor {

/// What a link target is. Only "does it have objects of its own" matters to
/// the audit (a target without sources owns no symbols); the rest is echoed.
enum class TargetKind : uint8_t {
  Executable,
  StaticLibrary,
  SharedLibrary,
  ModuleLibrary,
  ObjectLibrary,
  InterfaceLibrary,
  External,
};

const char *targetKindName(TargetKind kind);
std::optional<TargetKind> parseTargetKind(llvm::StringRef name);

/// The build's link targets: which sources each compiles and which targets
/// it declares as dependencies (docs/dependency-audit.md, "Target map").
///
/// A neutral input, produced outside vycor by a build-system adapter
/// (scripts/cmake-target-map.py) and supplied at query time. Validated once
/// by parseTargetMap; everything downstream reads these flat tables.
///
/// Targets are sorted by name, so a target's index is its rank in every
/// name-ordered output. Per-target lists are CSR ranges: target t's sources
/// are sources[sourceBegin[t] .. sourceBegin[t + 1]).
struct TargetMap {
  std::string generator;
  /// `depsForm: "closure"`: each `deps` list may already contain what the
  /// target's dependencies pull in (CMake File API before codemodel 2.9),
  /// so a listed dependency is not known to be named by the build file.
  bool depsAreClosure = false;
  /// The directories the build owns, absolute and lexically normal. A
  /// header outside all of them (a system or third-party header) is never
  /// attributed to a target because of who includes it; a target can still
  /// list it. Never empty after a successful parse.
  std::vector<std::string> roots;

  [[nodiscard]] bool underRoots(llvm::StringRef path) const noexcept;

  std::vector<std::string> names;
  std::vector<TargetKind> kinds;
  std::vector<uint32_t> sourceBegin;
  std::vector<std::string> sources; // absolute, lexically normal
  std::vector<uint32_t> headerBegin;
  std::vector<std::string> headers; // absolute, lexically normal
  std::vector<uint32_t> depBegin;
  std::vector<uint32_t> deps; // target indices, ascending, no self, unique
  /// `interfaceDeps`: what a target tells its consumers to link without
  /// linking it itself (a header-only library forwarding the library its
  /// headers call into). They extend what is reached through the target;
  /// they are never the target's own declared dependencies, so they make
  /// no row and no unused finding.
  std::vector<uint32_t> interfaceDepBegin;
  std::vector<uint32_t> interfaceDeps; // as deps, and disjoint from them

  [[nodiscard]] uint32_t size() const noexcept {
    return static_cast<uint32_t>(names.size());
  }
  [[nodiscard]] std::optional<uint32_t> find(llvm::StringRef name) const;
  [[nodiscard]] llvm::ArrayRef<std::string> sourcesOf(uint32_t t) const {
    return llvm::ArrayRef<std::string>(sources).slice(
        sourceBegin[t], sourceBegin[t + 1] - sourceBegin[t]);
  }
  [[nodiscard]] llvm::ArrayRef<std::string> headersOf(uint32_t t) const {
    return llvm::ArrayRef<std::string>(headers).slice(
        headerBegin[t], headerBegin[t + 1] - headerBegin[t]);
  }
  [[nodiscard]] llvm::ArrayRef<uint32_t> depsOf(uint32_t t) const {
    return llvm::ArrayRef<uint32_t>(deps).slice(depBegin[t],
                                                depBegin[t + 1] - depBegin[t]);
  }
  [[nodiscard]] llvm::ArrayRef<uint32_t> interfaceDepsOf(uint32_t t) const {
    return llvm::ArrayRef<uint32_t>(interfaceDeps)
        .slice(interfaceDepBegin[t],
               interfaceDepBegin[t + 1] - interfaceDepBegin[t]);
  }
};

/// `path` made absolute against `base` (when relative) with `.` and `..`
/// components removed lexically. No filesystem access: symlinks are not
/// resolved, so two spellings of one file through a symlink stay distinct.
/// A relative path with an empty base stays relative.
std::string normalizeAuditPath(llvm::StringRef path, llvm::StringRef base);

/// Parse and validate a target-map document. `baseDir` resolves a relative
/// `root` and, without a `root`, stands in for it (the loader passes the
/// directory of the map file). On failure returns false with
/// `error` naming the first problem.
bool parseTargetMap(const llvm::json::Value &doc, llvm::StringRef baseDir,
                    TargetMap &out, std::string &error);

/// Read and parse a target-map file.
bool loadTargetMap(llvm::StringRef path, TargetMap &out, std::string &error);

} // namespace vycor
