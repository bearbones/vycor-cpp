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
#include "Paging.h"
#include "Registry.h"
#include "Schema.h"

#include "vycor/ext/Extensions.h"
#include "vycor/metrics/MetricSet.h"

#include <algorithm>

// The `metrics` tool (docs/metrics/README.md): run the selected metrics
// over the loaded indexes and answer with one record list. A measurement,
// never a verdict: nothing here passes or fails.

namespace vycor {

namespace {

llvm::json::Object componentsJson(const MetricComponents &components) {
  llvm::json::Object out;
  for (const auto &component : components)
    out[component.first] = component.second;
  return out;
}

// `{metric, entity: {kind, name, usr, file, line}, value, unit,
// components?, witness?}`. Every entity member is always present so the
// shape does not depend on the level; the ones a kind does not use are
// empty / 0.
llvm::json::Object recordJson(const MetricRecord &record) {
  llvm::json::Object entity;
  entity["kind"] = record.entity.kind;
  entity["name"] = record.entity.name;
  entity["usr"] = record.entity.usr;
  entity["file"] = record.entity.file;
  entity["line"] = static_cast<int64_t>(record.entity.line);

  llvm::json::Object out;
  out["metric"] = record.metric;
  out["entity"] = std::move(entity);
  out["value"] = record.value;
  out["unit"] = record.unit;
  if (!record.components.empty())
    out["components"] = componentsJson(record.components);
  if (record.witness.kind() != llvm::json::Value::Null)
    out["witness"] = record.witness;
  return out;
}

llvm::json::Value listMetrics(const std::vector<const Metric *> &metrics,
                              const Page &page) {
  llvm::json::Array records;
  for (size_t i = page.begin(metrics.size()), e = page.end(metrics.size());
       i < e; ++i) {
    const Metric &metric = *metrics[i];
    llvm::json::Object record;
    record["metric"] = metric.name();
    record["summary"] = metric.summary();
    record["unit"] = metric.unit();
    record["level"] = rollupLevelName(metric.nativeLevel());
    records.push_back(std::move(record));
  }
  llvm::json::Object out;
  out["records"] = std::move(records);
  attachPage(out, page, metrics.size());
  return out;
}

// What a call asks for, once its arguments are known to be well-formed.
struct Request {
  Page page;
  bool list = false;
  std::vector<const Metric *> known;
  std::vector<const Metric *> selected;
  RollupLevel level = RollupLevel::Symbol;
  TargetOwners owners;
};

// Everything about a call that does not depend on the indexes. A usage
// error when the arguments are malformed.
std::optional<llvm::json::Value> parseRequest(const llvm::json::Object &args,
                                              Request &request) {
  if (auto error = parsePage(args, kDefaultListLimit, request.page))
    return usageError(*error);

  request.known = allMetrics();
  request.list = args.getBoolean("list_metrics").value_or(false);
  if (request.list)
    return std::nullopt;

  std::string error;
  if (!resolveMetricSpec(args.getString("metrics").value_or(""),
                         request.known, request.selected, error))
    return usageError("Invalid metrics: " + error);

  if (auto name = args.getString("rollup")) {
    auto parsed = rollupLevelFromName(*name);
    if (!parsed)
      return usageError("Invalid rollup: '" + name->str() +
                        "' (expected symbol, file, tu, directory or target)");
    request.level = *parsed;
  }

  // A target roll-up needs the map; a map without one would be ignored,
  // which is worth saying rather than doing.
  auto targetMap = args.getString("target_map");
  if (request.level == RollupLevel::Target) {
    if (!targetMap)
      return usageError("Requires target_map: the target roll-up needs a "
                        "target map (docs/metrics/README.md)");
    if (!TargetOwners::load(*targetMap, request.owners, error))
      return usageError("Invalid target_map: " + error);
  } else if (targetMap) {
    return usageError("Invalid target_map: only read by rollup=target");
  }
  return std::nullopt;
}

llvm::json::Value runMetrics(const llvm::json::Object &args,
                             const ToolContext &ctx) {
  Request request;
  if (auto invalid = parseRequest(args, request))
    return std::move(*invalid);
  if (request.list)
    return listMetrics(request.known, request.page);
  const Page &page = request.page;
  const std::vector<const Metric *> &selected = request.selected;
  const RollupLevel level = request.level;

  const MetricContext metricCtx{ctx,
                                ExtensionRegistry::instance().effectTable()};
  // Absent facts are not a zero: one selected metric that cannot answer
  // makes the whole run unavailable, rather than a report missing a metric.
  for (const Metric *metric : selected) {
    std::string why = metric->unavailable(metricCtx);
    if (!why.empty())
      return unavailableError("metric '" + metric->name() +
                              "' is unavailable: " + why);
  }

  // `selected` is sorted by name and each report is in its canonical
  // order, so concatenation is the record order: metric, then value
  // descending, then entity (docs/deterministic-output.md).
  llvm::json::Array summaries;
  std::vector<MetricRecord> records;
  for (const Metric *metric : selected) {
    MetricReport report =
        runMetric(*metric, metricCtx, level, &request.owners);
    llvm::json::Object overall;
    overall["value"] = report.overall.value;
    overall["components"] = componentsJson(report.overall.components);
    llvm::json::Object summary;
    summary["metric"] = report.metric;
    summary["unit"] = report.unit;
    summary["level"] = rollupLevelName(report.level);
    summary["records"] = static_cast<int64_t>(report.records.size());
    summary["overall"] = std::move(overall);
    summaries.push_back(std::move(summary));
    std::move(report.records.begin(), report.records.end(),
              std::back_inserter(records));
  }

  llvm::json::Array window;
  for (size_t i = page.begin(records.size()), e = page.end(records.size());
       i < e; ++i)
    window.push_back(recordJson(records[i]));

  llvm::json::Object out;
  out["rollup"] = rollupLevelName(level);
  out["metrics"] = std::move(summaries);
  out["records"] = std::move(window);
  attachPage(out, page, records.size());
  return out;
}

} // namespace

bool metricsRequestReadsIndex(const llvm::json::Object &args) {
  Request request;
  return !parseRequest(args, request) && !request.list;
}

void registerMetricsTools(std::vector<ToolEntry> &tools) {
  llvm::json::Object props;
  props["metrics"] = stringProp(
      "Which metrics to run, by name: comma-separated, applied left to "
      "right; `all` selects every metric and `-name` removes one (default: "
      "every metric). An unknown name is a usage error naming the known "
      "ones.");
  props["list_metrics"] = boolProp(
      "List the available metrics (name, summary, unit, native level) "
      "instead of running any.");
  props["rollup"] = stringProp(
      "The level of the records: symbol (default; one record per measured "
      "entity), file, tu, directory, or target (needs target_map). A "
      "metric whose native level is coarser reports at its native level; "
      "each entry of `metrics` says which level its records are at.");
  props["target_map"] = stringProp(
      "Path of a target map (`{\"targets\": [{\"name\", \"sources\"}]}`) "
      "naming the link target that owns each TU; required by, and only "
      "read for, rollup=target. TUs no target lists are grouped as "
      "'(no target)'.");
  addPagingProps(props, kDefaultListLimit, "records");

  llvm::json::Object schema;
  schema["type"] = "object";
  schema["properties"] = std::move(props);

  tools.push_back(
      {"metrics",
       "Measurements, not warnings: run named metrics over the index and "
       "report `{metric, entity, value, unit, components?, witness?}` "
       "records, largest value first, rolled up by symbol, file, TU, "
       "directory or link target. `metrics` carries each metric's total "
       "over everything measured. Nothing passes or fails. See "
       "docs/metrics/README.md.",
       llvm::json::Value(std::move(schema)), runMetrics, "", kSectionGraph});
}

} // namespace vycor
