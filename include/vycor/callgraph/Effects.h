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

#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// The effect table: which functions are taken to allocate, acquire a lock,
// or perform I/O. A pure name table, with no index or AST dependency, so
// every analysis that asks "does this reach include an allocation?" (the
// static-init-cost metric, the hot-path checks) classifies the same way.
//
// A function carries an effect when its display name, normalized by
// effectLookupName, is in the table. The built-in entries name the
// primitives and the standard-library entry points whose effect is
// unconditional; an organization adds its own wrappers through
// ExtensionRegistry::addEffectFunctions or the org config's `effects` key
// (docs/EXTENDING.md).

namespace vycor {

enum class EffectKind : uint8_t { Allocation, Lock, Io };
inline constexpr unsigned kEffectKindCount = 3;
inline constexpr std::array<EffectKind, kEffectKindCount> kEffectKinds = {
    EffectKind::Allocation, EffectKind::Lock, EffectKind::Io};

/// "allocation", "lock", "io": the spelling used in payloads, the org
/// config and the docs.
llvm::StringRef effectKindName(EffectKind kind);
std::optional<EffectKind> effectKindFromName(llvm::StringRef name);

/// Bit of `kind` in an effect mask.
constexpr uint8_t effectBit(EffectKind kind) {
  return static_cast<uint8_t>(1u << static_cast<unsigned>(kind));
}

/// The table key of a display name: template argument lists removed and
/// implementation namespaces (components starting with two underscores,
/// such as libstdc++'s `__cxx11` and libc++'s `__1`) dropped, so
/// `std::__cxx11::basic_string<char>::basic_string` and
/// `std::lock_guard<std::mutex>::lock_guard` find `std::basic_string::
/// basic_string` and `std::lock_guard::lock_guard`. Operator names keep
/// their spelling (`operator<`, `operator new`).
std::string effectLookupName(llvm::StringRef displayName);

class EffectTable {
public:
  /// The built-in entries alone.
  static EffectTable builtin();

  /// Add `name` (a qualified function name; normalized on the way in) to
  /// `kind`. A name may carry several kinds.
  void add(EffectKind kind, llvm::StringRef name);

  /// Effect mask (effectBit) of a function by display name; 0 when it
  /// carries none.
  uint8_t effectsOf(llvm::StringRef displayName) const;

  /// The entries of `kind`, sorted.
  std::vector<std::string> names(EffectKind kind) const;

  bool empty() const { return byName_.empty(); }

private:
  llvm::StringMap<uint8_t> byName_;
};

} // namespace vycor
