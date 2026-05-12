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

#include <string>
#include <vector>

namespace vycor {

/// Path-based edge collapse filter. When both the caller and callee of an edge
/// are in collapsed paths, the edge is skipped during graph construction.
/// Boundary edges (non-collapsed → collapsed) are preserved so investigators
/// can see entry points into collapsed regions.
class CollapseFilter {
public:
  CollapseFilter() = default;
  explicit CollapseFilter(std::vector<std::string> patterns);

  /// Returns true if the given absolute file path falls within any of the
  /// configured collapse patterns. Matching is path-component-aware:
  /// pattern "Client/Math" matches ".../Client/Math/..." but not
  /// ".../Client/MathExtras/...".
  bool isCollapsed(llvm::StringRef filePath) const;

  bool empty() const { return patterns_.empty(); }

private:
  // Stored with leading and trailing '/' for component-boundary matching.
  // e.g. "Client/Math" → "/Client/Math/"
  std::vector<std::string> patterns_;
};

} // namespace vycor
