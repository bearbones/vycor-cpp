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

#include "vycor/callgraph/Effects.h"
#include "vycor/metrics/TargetOwners.h"
#include "vycor/query/Tools.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/JSON.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// The metrics surface (docs/metrics/README.md): measurements, not
// warnings. A metric is a named reduction over the facts a query tool
// sees. It produces samples at its native level (one per initializer, per
// file, per target, ...); the framework turns them into records, rolls
// them up by file, TU, directory or link target, and totals them. Nothing
// here passes or fails.
//
// Built-in metrics live in src/metrics/ and are listed in MetricSet.cpp;
// organization metrics register through ExtensionRegistry::addMetric
// (VYCOR_REGISTER_METRIC, docs/EXTENDING.md) and are selected, rolled up
// and emitted exactly like built-in ones.

namespace vycor {

/// What a record measures. `kind` says which of the other members mean
/// something: a symbol-like entity ("variable", "function", ...) has all
/// of them; "file" and "tu" have `name` == `file`; "directory", "target"
/// and "total" have a `name` only. Unused members are empty / 0.
struct MetricEntity {
  std::string kind;
  std::string name;
  std::string usr;
  std::string file;
  unsigned line = 0;
};

/// Named integer parts of a value ("allocation": 1, "entities": 12).
using MetricComponents = std::vector<std::pair<std::string, int64_t>>;

/// One emitted record: `{metric, entity, value, unit, components?,
/// witness?}`.
struct MetricRecord {
  std::string metric;
  MetricEntity entity;
  int64_t value = 0;
  std::string unit;
  MetricComponents components;
  /// Evidence for the value (a call chain, an include chain), or null.
  /// Free-form per metric; documented on the metric's page.
  llvm::json::Value witness = nullptr;
};

/// One native-level measurement, before roll-up.
struct MetricSample {
  MetricEntity entity;
  int64_t value = 0;
  MetricComponents components;
  llvm::json::Value witness = nullptr;
  /// The TUs this measurement belongs to: what the TU, directory and
  /// target roll-ups group by. A sample with none is grouped by
  /// `entity.file`.
  std::vector<std::string> tus;
  /// The same as indices into MetricRun::tuNames(), for a metric whose
  /// samples share a few TUs among many (read in addition to `tus`).
  std::vector<uint32_t> tuIds;
  /// The template the entity is an instance of (kind "template"): what
  /// the template roll-up groups by. A sample with no `pattern.usr` is a
  /// group of its own there, under its own entity.
  MetricEntity pattern;
};

/// Roll-up levels, finest first. A metric whose native level is coarser
/// than the requested one reports at its native level.
enum class RollupLevel : uint8_t {
  Symbol,
  Template,
  File,
  Tu,
  Directory,
  Target
};

llvm::StringRef rollupLevelName(RollupLevel level);
std::optional<RollupLevel> rollupLevelFromName(llvm::StringRef name);

/// What a metric reads.
struct MetricContext {
  /// The indexes and the facts about them, as every query tool sees them.
  const ToolContext &index;
  /// Built-in effect table plus the organization's additions.
  const EffectTable &effects;
};

/// One run of a metric over one context: its samples, and how a group of
/// them reduces to one value. Owns whatever working state the reduction
/// reuses between groups.
class MetricRun {
public:
  virtual ~MetricRun() = default;

  /// Native-level samples, in any order (the framework orders records).
  const std::vector<MetricSample> &samples() const { return samples_; }
  /// The TU paths MetricSample::tuIds index.
  const std::vector<std::string> &tuNames() const { return tuNames_; }

  /// Reduce the samples `members` (indices into samples(), ascending) to
  /// one record: fill `out.value`, `out.components` and, optionally,
  /// `out.witness`. The default is additive: the sum of the values and of
  /// each component. Override when the group's value is not the sum of its
  /// members' (a union, a maximum).
  virtual void rollUp(llvm::ArrayRef<uint32_t> members, MetricRecord &out);

protected:
  std::vector<MetricSample> samples_;
  std::vector<std::string> tuNames_;
};

class Metric {
public:
  virtual ~Metric() = default;

  /// Stable identifier, short-kebab-case: the `--metrics` name, the
  /// `metric` member of every record, and the docs page
  /// (docs/metrics/<name>.md for a built-in metric).
  virtual std::string name() const = 0;
  /// One line, shown by `--list-metrics`.
  virtual std::string summary() const = 0;
  /// What `value` counts ("functions", "bytes", "includes").
  virtual std::string unit() const = 0;
  /// The level of the samples.
  virtual RollupLevel nativeLevel() const { return RollupLevel::Symbol; }

  /// Why `ctx` cannot answer this metric (its facts were never collected),
  /// or empty. A non-empty answer makes the run `unavailable`
  /// (docs/result-contract.md): absent facts are not a zero.
  virtual std::string unavailable(const MetricContext &ctx) const {
    (void)ctx;
    return {};
  }

  /// Measure. Called only when unavailable(ctx) is empty.
  virtual std::unique_ptr<MetricRun>
  measure(const MetricContext &ctx) const = 0;
};

/// A metric's records at one level, plus its total over everything.
struct MetricReport {
  std::string metric;
  std::string unit;
  /// The level the records are at (the requested one, or the metric's
  /// native level when that is coarser).
  RollupLevel level = RollupLevel::Symbol;
  std::vector<MetricRecord> records; // canonical order
  MetricRecord overall;              // entity kind "total"
};

/// Run `metric` and roll its samples up to `level`. `owners` maps TUs to
/// link targets; required (non-null) for RollupLevel::Target.
///
/// Record order (docs/deterministic-output.md): value descending, then
/// entity file, line, name, usr and kind ascending.
MetricReport runMetric(const Metric &metric, const MetricContext &ctx,
                       RollupLevel level, const TargetOwners *owners);

/// Add `value` to the component `name` of `components`, appending it when
/// absent.
void addComponent(MetricComponents &components, llvm::StringRef name,
                  int64_t value);

} // namespace vycor
