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
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"

#include <string>
#include <vector>

// Which link target owns each TU, for the metrics surface's `target`
// roll-up. Read from the neutral target map the dependency audit defines
// (`{"root"?, "targets": [{"name", "sources", ...}]}`); only the target
// names and source lists are used here.

namespace vycor {

class TargetOwners {
public:
  /// Read a target-map file. Relative sources resolve against the map's
  /// `root` (itself relative to the map file's directory), or against the
  /// map file's directory when it has none. On failure returns false with
  /// `error` naming the problem.
  static bool load(llvm::StringRef path, TargetOwners &out,
                   std::string &error);
  /// Parse a target-map document; `baseDir` is the map file's directory.
  static bool parse(llvm::StringRef json, llvm::StringRef baseDir,
                    TargetOwners &out, std::string &error);

  /// The targets that list `tuPath` as a source, sorted by name; empty
  /// when no target does. Paths are compared absolute and lexically
  /// normal (no symlink resolution).
  llvm::ArrayRef<std::string> ownersOf(llvm::StringRef tuPath) const;

  size_t targetCount() const { return targetCount_; }

private:
  llvm::StringMap<std::vector<std::string>> bySource_;
  size_t targetCount_ = 0;
};

/// The name of the group holding TUs that no target lists.
inline constexpr const char *kNoTargetName = "(no target)";

} // namespace vycor
