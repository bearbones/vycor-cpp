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
#include "vycor/metrics/MetricSet.h"

#include "vycor/ext/Extensions.h"

#include <algorithm>
#include <set>

namespace vycor {

namespace {

const std::vector<std::unique_ptr<Metric>> &builtinMetrics() {
  static const std::vector<std::unique_ptr<Metric>> metrics = [] {
    std::vector<std::unique_ptr<Metric>> out;
    out.push_back(makeStaticInitCostMetric());
    out.push_back(makeRedundantInstantiationMetric());
    return out;
  }();
  return metrics;
}

} // namespace

std::vector<const Metric *> allMetrics() {
  std::vector<const Metric *> out;
  std::set<std::string> names;
  for (const auto &metric : builtinMetrics())
    if (names.insert(metric->name()).second)
      out.push_back(metric.get());
  for (const auto &metric : ExtensionRegistry::instance().metrics())
    if (names.insert(metric->name()).second)
      out.push_back(metric.get());
  std::sort(out.begin(), out.end(), [](const Metric *a, const Metric *b) {
    return a->name() < b->name();
  });
  return out;
}

bool resolveMetricSpec(llvm::StringRef spec,
                       const std::vector<const Metric *> &known,
                       std::vector<const Metric *> &selected,
                       std::string &error) {
  std::set<std::string> knownNames;
  for (const Metric *metric : known)
    knownNames.insert(metric->name());

  std::set<std::string> enabled;
  spec = spec.trim();
  if (spec.empty() || spec.starts_with("-"))
    enabled = knownNames;

  // Split by hand: StringRef::split cannot tell "a" from "a,", and a
  // trailing comma is an empty entry like any other.
  llvm::StringRef rest = spec;
  for (bool more = !rest.empty(); more;) {
    const size_t comma = rest.find(',');
    more = comma != llvm::StringRef::npos;
    llvm::StringRef entry = rest.substr(0, comma);
    rest = more ? rest.substr(comma + 1) : llvm::StringRef();
    entry = entry.trim();
    bool disable = entry.consume_front("-");
    if (entry.empty()) {
      error = "empty entry in metrics specification";
      return false;
    }
    if (entry == "all") {
      if (disable)
        enabled.clear();
      else
        enabled = knownNames;
      continue;
    }
    if (!knownNames.count(entry.str())) {
      error = "unknown metric '" + entry.str() + "' (known:";
      for (const std::string &name : knownNames)
        error += " " + name;
      error += "; see docs/metrics/README.md)";
      return false;
    }
    if (disable)
      enabled.erase(entry.str());
    else
      enabled.insert(entry.str());
  }

  selected.clear();
  for (const Metric *metric : known)
    if (enabled.count(metric->name()))
      selected.push_back(metric);
  return true;
}

} // namespace vycor
