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

#include <mutex>
#include <string>
#include <vector>

namespace vycor {

/// Per-TU, per-phase outcome of one frontend parse during an index build.
/// `toolStatus` is the ClangTool::run() result: 0 = success, 1 = at least
/// one error occurred (the AST may be partial — visitors still ran), 2 =
/// no compile command / skipped. -1 = the parse crashed and was skipped by
/// the crash guard.
struct TuBuildStat {
  std::string file;
  int phase = 0;   // 1 = node index, 2 = combined edge + control-flow parse
  double ms = 0.0; // wall-clock for this TU's parse + traversal
  int toolStatus = 0;
};

/// Aggregate timing/outcome record for one multi-TU index build.
/// Collected by bakeIndexes() when a non-null pointer is passed; the
/// per-TU vector is appended under a mutex from worker threads.
struct BuildStats {
  double phase1WallMs = 0.0; // node-index barrier wall time
  double phase2WallMs = 0.0; // edge+CF barrier wall time
  unsigned threads = 0;      // requested thread count (0 = hardware)
  std::vector<TuBuildStat> tuStats;

  void addTuStat(TuBuildStat s) {
    std::lock_guard<std::mutex> lock(mu_);
    tuStats.push_back(std::move(s));
  }

  size_t parseErrorCount() const {
    size_t n = 0;
    for (const auto &t : tuStats)
      if (t.toolStatus == 1)
        ++n;
    return n;
  }

  size_t crashCount() const {
    size_t n = 0;
    for (const auto &t : tuStats)
      if (t.toolStatus == -1)
        ++n;
    return n;
  }

private:
  std::mutex mu_;
};

} // namespace vycor
