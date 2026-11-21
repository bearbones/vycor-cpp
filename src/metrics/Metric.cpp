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
#include "vycor/metrics/Metric.h"

#include "llvm/Support/Path.h"

#include <algorithm>
#include <map>
#include <numeric>
#include <tuple>

namespace vycor {

namespace {

struct LevelName {
  RollupLevel level;
  const char *name;
};
constexpr LevelName kLevelNames[] = {
    {RollupLevel::Symbol, "symbol"},       {RollupLevel::Template, "template"},
    {RollupLevel::File, "file"},
    {RollupLevel::Tu, "tu"},               {RollupLevel::Directory, "directory"},
    {RollupLevel::Target, "target"},
};

// The groups a sample belongs to at `level`, unique. A sample can be in
// several: a header's initializer runs in every TU that includes it.
void groupKeysOf(const MetricSample &sample, RollupLevel level,
                 const TargetOwners *owners,
                 const std::vector<std::string> &tuNames,
                 std::vector<std::string> &keys) {
  keys.clear();
  if (level == RollupLevel::File) {
    keys.push_back(sample.entity.file);
    return;
  }
  if (level == RollupLevel::Template) {
    // Kind first, so a template and an entity standing for itself never
    // share a key.
    const MetricEntity &e =
        sample.pattern.usr.empty() ? sample.entity : sample.pattern;
    keys.push_back(e.kind + '\0' + e.usr);
    return;
  }
  auto perTu = [&](llvm::StringRef tu) {
    switch (level) {
    case RollupLevel::Directory:
      keys.push_back(llvm::sys::path::parent_path(tu).str());
      break;
    case RollupLevel::Target: {
      auto owning = owners ? owners->ownersOf(tu)
                           : llvm::ArrayRef<std::string>();
      if (owning.empty())
        keys.push_back(kNoTargetName);
      for (const std::string &name : owning)
        keys.push_back(name);
      break;
    }
    default:
      keys.push_back(tu.str());
      break;
    }
  };
  if (sample.tus.empty() && sample.tuIds.empty())
    perTu(sample.entity.file);
  for (const std::string &tu : sample.tus)
    perTu(tu);
  for (uint32_t tu : sample.tuIds)
    perTu(tuNames[tu]);
  std::sort(keys.begin(), keys.end());
  keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
}

MetricEntity groupEntity(RollupLevel level, const std::string &key,
                         const MetricSample &first) {
  if (level == RollupLevel::Template)
    return first.pattern.usr.empty() ? first.entity : first.pattern;
  MetricEntity entity;
  entity.kind = rollupLevelName(level).str();
  entity.name = key;
  if (level == RollupLevel::File || level == RollupLevel::Tu)
    entity.file = key;
  return entity;
}

void setComponent(MetricComponents &components, llvm::StringRef name,
                  int64_t value) {
  for (auto &component : components)
    if (component.first == name) {
      component.second = value;
      return;
    }
  components.emplace_back(name.str(), value);
}

bool recordBefore(const MetricRecord &a, const MetricRecord &b) {
  if (a.value != b.value)
    return a.value > b.value;
  return std::tie(a.entity.file, a.entity.line, a.entity.name, a.entity.usr,
                  a.entity.kind) < std::tie(b.entity.file, b.entity.line,
                                            b.entity.name, b.entity.usr,
                                            b.entity.kind);
}

} // namespace

llvm::StringRef rollupLevelName(RollupLevel level) {
  for (const LevelName &entry : kLevelNames)
    if (entry.level == level)
      return entry.name;
  return "symbol";
}

std::optional<RollupLevel> rollupLevelFromName(llvm::StringRef name) {
  for (const LevelName &entry : kLevelNames)
    if (name == entry.name)
      return entry.level;
  return std::nullopt;
}

void addComponent(MetricComponents &components, llvm::StringRef name,
                  int64_t value) {
  for (auto &component : components)
    if (component.first == name) {
      component.second += value;
      return;
    }
  components.emplace_back(name.str(), value);
}

void MetricRun::rollUp(llvm::ArrayRef<uint32_t> members, MetricRecord &out) {
  out.value = 0;
  out.components.clear();
  for (uint32_t member : members) {
    const MetricSample &sample = samples_[member];
    out.value += sample.value;
    for (const auto &component : sample.components)
      addComponent(out.components, component.first, component.second);
  }
}

MetricReport runMetric(const Metric &metric, const MetricContext &ctx,
                       RollupLevel level, const TargetOwners *owners) {
  MetricReport report;
  report.metric = metric.name();
  report.unit = metric.unit();
  report.level = std::max(level, metric.nativeLevel());

  std::unique_ptr<MetricRun> run = metric.measure(ctx);
  const std::vector<MetricSample> &samples = run->samples();

  if (report.level == metric.nativeLevel()) {
    report.records.reserve(samples.size());
    for (const MetricSample &sample : samples) {
      MetricRecord record;
      record.metric = report.metric;
      record.unit = report.unit;
      record.entity = sample.entity;
      record.value = sample.value;
      record.components = sample.components;
      record.witness = sample.witness;
      report.records.push_back(std::move(record));
    }
  } else {
    // Ordered so that members are handed to rollUp ascending and groups
    // are formed the same way whatever order the samples came in.
    std::map<std::string, std::vector<uint32_t>> groups;
    std::vector<std::string> keys;
    for (uint32_t i = 0; i < samples.size(); ++i) {
      groupKeysOf(samples[i], report.level, owners, run->tuNames(), keys);
      for (const std::string &key : keys)
        groups[key].push_back(i);
    }
    report.records.reserve(groups.size());
    for (const auto &[key, members] : groups) {
      MetricRecord record;
      record.metric = report.metric;
      record.unit = report.unit;
      record.entity = groupEntity(report.level, key, samples[members.front()]);
      run->rollUp(members, record);
      setComponent(record.components, "entities",
                   static_cast<int64_t>(members.size()));
      report.records.push_back(std::move(record));
    }
  }
  std::sort(report.records.begin(), report.records.end(), recordBefore);

  std::vector<uint32_t> everything(samples.size());
  std::iota(everything.begin(), everything.end(), 0u);
  report.overall.metric = report.metric;
  report.overall.unit = report.unit;
  report.overall.entity.kind = "total";
  run->rollUp(everything, report.overall);
  setComponent(report.overall.components, "entities",
               static_cast<int64_t>(samples.size()));
  return report;
}

} // namespace vycor
