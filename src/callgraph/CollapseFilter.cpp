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

#include "vycor/callgraph/CollapseFilter.h"

namespace vycor {

CollapseFilter::CollapseFilter(std::vector<std::string> patterns) {
  patterns_.reserve(patterns.size());
  for (auto &p : patterns) {
    // Normalize to "/pattern/" for component-boundary matching.
    std::string normalized;
    if (!p.empty() && p.front() != '/')
      normalized += '/';
    normalized += p;
    if (!normalized.empty() && normalized.back() != '/')
      normalized += '/';
    patterns_.push_back(std::move(normalized));
  }
}

bool CollapseFilter::isCollapsed(llvm::StringRef filePath) const {
  for (const auto &pattern : patterns_) {
    if (filePath.find(pattern) != llvm::StringRef::npos)
      return true;
  }
  return false;
}

} // namespace vycor
