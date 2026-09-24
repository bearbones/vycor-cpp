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

#include "vycor/callgraph/PathSearch.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/JSON.h"

#include <cstdint>
#include <optional>
#include <string>

namespace vycor {

// ============================================================================
// User-supplied search limits (max_depth, max_paths, max_fan_in, max_work,
// max_results). Every tool reads them through readLimit, which clamps a
// value above the documented maximum to that maximum BEFORE it is narrowed
// to the handler's integer type: a naive static_cast<unsigned> of
// 4294967296 is 0, which the searches read as "no limit". Maxima are
// documented in docs/path-analysis.md ("Limits").
// ============================================================================

/// Path length, in edges (find_call_chain, the exception path tools,
/// query_nearest_catches, the lock tools, impact_of_change).
constexpr int64_t kMaxSearchDepth = kPathSearchDepthCap;
/// Paths enumerated by one search.
constexpr int64_t kMaxSearchPaths = 100000;
/// Hub cutoff (stored in-degree).
constexpr int64_t kMaxFanInLimit = 100000000;
/// Node expansions / BFS work budget.
constexpr int64_t kMaxWorkLimit = 1000000000;
/// Records returned by one result list.
constexpr int64_t kMaxResultsLimit = 10000000;

/// What a value below `min` does.
enum class BelowMin {
  Reject, // usage error "Invalid <key>: must be <positive|non-negative>"
  Clamp,  // raised to min
};

/// Read integer argument `key` into `value` when present (absent or
/// non-integer leaves `value` — the default — untouched), clamped to
/// [min, max]. Returns the usage-error message when `below` is Reject and
/// the value is below `min`.
std::optional<std::string> readLimit(const llvm::json::Object &args,
                                     llvm::StringRef key, int64_t min,
                                     int64_t max, BelowMin below,
                                     int64_t &value);

/// readLimit into a narrower unsigned target (min >= 0, max fits).
template <typename T>
std::optional<std::string> readLimitAs(const llvm::json::Object &args,
                                       llvm::StringRef key, int64_t min,
                                       int64_t max, BelowMin below, T &out) {
  int64_t v = static_cast<int64_t>(out);
  auto err = readLimit(args, key, min, max, below, v);
  if (!err)
    out = static_cast<T>(v);
  return err;
}

} // namespace vycor
