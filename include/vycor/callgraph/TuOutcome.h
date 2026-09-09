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

#include <cstdint>
#include <string>
#include <unordered_map>

namespace vycor {

/// How a requested TU's parse ended. Only an Indexed TU is a trustworthy
/// cache entry: every other status is retried on the next warm start
/// (SnapshotIO::dirtyTUs) and counted against the index's coverage
/// (IndexCoverage in Snapshot.h).
enum class TuStatus : uint8_t {
  Indexed = 0,  // clean parse: this TU's facts are complete
  Partial = 1,  // the parse reported errors: facts from a partial AST
  Crashed = 2,  // the in-process crash guard fired: no facts
  Poisoned = 3, // its worker died (--isolate-workers): no facts
  Skipped = 4,  // never parsed (no compile command, no outcome reported)
};

/// The lowercase spelling used in JSON payloads ("indexed", "partial",
/// "crashed", "poisoned", "skipped").
const char *tuStatusName(TuStatus status);

struct TuOutcome {
  TuStatus status = TuStatus::Skipped;
  std::string detail; // short reason: "signal 11", "worker crashed", ...

  bool operator==(const TuOutcome &o) const {
    return status == o.status && detail == o.detail;
  }
  bool operator!=(const TuOutcome &o) const { return !(*this == o); }
};

/// Per TU (absolute, dot-free path) the outcome of its last parse.
/// Produced by the bake (BakedIndexes::outcomes), recorded in the
/// snapshot meta (SnapshotMeta::outcomes).
using TuOutcomes = std::unordered_map<std::string, TuOutcome>;

} // namespace vycor
