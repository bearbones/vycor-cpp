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


#include "vycor/query/Limits.h"

#include <algorithm>

namespace vycor {

std::optional<std::string> readLimit(const llvm::json::Object &args,
                                     llvm::StringRef key, int64_t min,
                                     int64_t max, BelowMin below,
                                     int64_t &value) {
  auto v = args.getInteger(key);
  if (!v)
    return std::nullopt;
  if (*v < min) {
    if (below == BelowMin::Reject)
      return "Invalid " + key.str() + ": must be " +
             (min > 0 ? "positive" : "non-negative");
    value = min;
    return std::nullopt;
  }
  value = std::min(*v, max);
  return std::nullopt;
}

} // namespace vycor
