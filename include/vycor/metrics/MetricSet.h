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

#include "vycor/metrics/Metric.h"

#include <string>
#include <vector>

// Named-metric selection (docs/metrics/README.md), the model of named
// checks: every metric has a stable name and a page, and a specification
// selects among them.
//
//   (no specification)        every metric
//   "static-init-cost"        that metric alone
//   "all,-static-init-cost"   every metric but one
//   "-static-init-cost"       the same: a specification that starts with
//                             a removal starts from every metric
//
// Entries are comma-separated and applied left to right. An unknown name
// is an error naming the known ones.

namespace vycor {

/// Every metric: the built-in ones, then the organization's
/// (ExtensionRegistry::metrics), sorted by name. An organization metric
/// that reuses a built-in name is ignored. The pointers stay valid until
/// the registry is cleared.
std::vector<const Metric *> allMetrics();

/// Resolve `spec` against `known` into the selected metrics, in `known`'s
/// order. Returns false and fills `error` on an unknown or empty entry.
bool resolveMetricSpec(llvm::StringRef spec,
                       const std::vector<const Metric *> &known,
                       std::vector<const Metric *> &selected,
                       std::string &error);

/// Whether a `metrics` tool call with these arguments reads the indexes:
/// false for `list_metrics` and for malformed arguments, which the tool
/// answers (with the list, or a usage error) over any context. Lets an
/// adapter skip loading or baking an index nobody will read.
bool metricsRequestReadsIndex(const llvm::json::Object &args);

/// The built-in metric constructors (src/metrics/).
std::unique_ptr<Metric> makeStaticInitCostMetric();
std::unique_ptr<Metric> makeRedundantInstantiationMetric();

} // namespace vycor
