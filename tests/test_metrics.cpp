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

// test_metrics.cpp — the metrics surface (docs/metrics/README.md): metric
// selection, the record shape and roll-ups, the static-init-cost metric
// over examples/static_init, the deterministic-order and result contracts,
// the organization extension point, and the CLI verb.

#include "vycor/callgraph/CallGraph.h"
#include "vycor/callgraph/ControlFlowIndex.h"
#include "vycor/callgraph/ControlFlowOracle.h"
#include "vycor/callgraph/Snapshot.h"
#include "vycor/cli/MegascopeCli.h"
#include "vycor/ext/Extensions.h"
#include "vycor/ext/OrgConfig.h"
#include "vycor/metrics/Metric.h"
#include "vycor/metrics/MetricSet.h"
#include "vycor/metrics/TargetOwners.h"
#include "vycor/query/Tools.h"

#include <catch2/catch_test_macros.hpp>
#include <clang/Tooling/CompilationDatabase.h>

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

using namespace vycor;

namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::string fixture(llvm::StringRef rel) {
  return (llvm::StringRef(PROJECT_SOURCE_DIR) + "/examples/static_init/" + rel)
      .str();
}

std::vector<std::string> fixtureSources() {
  return {fixture("core/registry.cpp"), fixture("plugins/plugins.cpp"),
          fixture("plugins/codecs.cpp")};
}

BakedIndexes bakeFiles(const std::vector<std::string> &files,
                       unsigned threads = 1) {
  clang::tooling::FixedCompilationDatabase compDb(".", {"-std=c++17"});
  return bakeIndexes(compDb, files, {}, threads);
}

std::string serialized(const llvm::json::Value &v) {
  std::string out;
  llvm::raw_string_ostream os(out);
  os << v;
  return out;
}

llvm::json::Object objectOf(const llvm::json::Value &v) {
  const auto *obj = v.getAsObject();
  REQUIRE(obj != nullptr);
  return *obj;
}

const ToolEntry &metricsTool(const std::vector<ToolEntry> &tools) {
  for (const auto &t : tools)
    if (t.name == "metrics")
      return t;
  FAIL("no metrics tool registered");
  return tools.front();
}

/// The metrics tool over baked indexes, as an adapter would run it.
struct Surface {
  const BakedIndexes &ix;
  ControlFlowOracle oracle;
  std::vector<std::string> entryPoints{"main"};
  std::vector<ToolEntry> tools = getRegisteredTools();
  ToolContext ctx;

  explicit Surface(const BakedIndexes &baked, bool withStaticInits = true)
      : ix(baked), oracle(baked.graph, baked.cfIndex),
        ctx{baked.graph, oracle, baked.cfIndex, entryPoints} {
    if (withStaticInits)
      ctx.staticInits = &baked.staticInits;
  }

  llvm::json::Value
  runValue(std::initializer_list<llvm::json::Object::KV> kvs = {}) {
    return runTool(metricsTool(tools), llvm::json::Object(kvs), ctx);
  }
  llvm::json::Object
  run(std::initializer_list<llvm::json::Object::KV> kvs = {}) {
    return objectOf(runValue(kvs));
  }
};

const llvm::json::Array &recordsOf(const llvm::json::Object &payload) {
  const auto *records = payload.getArray("records");
  REQUIRE(records != nullptr);
  return *records;
}

/// The record whose entity name ends with `name` (group names are
/// absolute paths; symbol names are not).
const llvm::json::Object *recordNamed(const llvm::json::Object &payload,
                                      llvm::StringRef name) {
  for (const llvm::json::Value &value : recordsOf(payload)) {
    const llvm::json::Object *record = value.getAsObject();
    const llvm::json::Object *entity = record->getObject("entity");
    if (entity && entity->getString("name").value_or("").ends_with(name))
      return record;
  }
  return nullptr;
}

int64_t valueOf(const llvm::json::Object &payload, llvm::StringRef name) {
  const llvm::json::Object *record = recordNamed(payload, name);
  REQUIRE(record != nullptr);
  return record->getInteger("value").value_or(-1);
}

int64_t component(const llvm::json::Object &record, llvm::StringRef name) {
  const llvm::json::Object *components = record.getObject("components");
  REQUIRE(components != nullptr);
  return components->getInteger(name).value_or(-1);
}

std::vector<std::string> chain(const llvm::json::Object &record,
                               llvm::StringRef effect) {
  std::vector<std::string> out;
  const llvm::json::Object *witness = record.getObject("witness");
  const llvm::json::Array *names = witness ? witness->getArray(effect)
                                           : nullptr;
  if (names)
    for (const llvm::json::Value &name : *names)
      out.push_back(name.getAsString().value_or("").str());
  return out;
}

std::vector<std::string> names(const llvm::json::Object &payload) {
  std::vector<std::string> out;
  for (const llvm::json::Value &value : recordsOf(payload))
    out.push_back(value.getAsObject()
                      ->getObject("entity")
                      ->getString("name")
                      .value_or("")
                      .str());
  return out;
}

// A metric whose samples the test states outright.
class StatedMetric : public Metric {
public:
  StatedMetric(std::string name, std::vector<MetricSample> samples,
               RollupLevel native = RollupLevel::Symbol)
      : name_(std::move(name)), samples_(std::move(samples)), native_(native) {}

  std::string name() const override { return name_; }
  std::string summary() const override { return "Stated by the test"; }
  std::string unit() const override { return "bytes"; }
  RollupLevel nativeLevel() const override { return native_; }

  std::unique_ptr<MetricRun> measure(const MetricContext &) const override {
    struct Run : MetricRun {
      explicit Run(std::vector<MetricSample> samples) {
        samples_ = std::move(samples);
      }
    };
    return std::make_unique<Run>(samples_);
  }

private:
  std::string name_;
  std::vector<MetricSample> samples_;
  RollupLevel native_;
};

MetricSample sampleAt(const char *name, const char *file, unsigned line,
                      int64_t value, std::vector<std::string> tus,
                      int64_t parts) {
  MetricSample sample;
  sample.entity.kind = "variable";
  sample.entity.name = name;
  sample.entity.usr = std::string("c:@") + name;
  sample.entity.file = file;
  sample.entity.line = line;
  sample.value = value;
  sample.tus = std::move(tus);
  sample.components.emplace_back("parts", parts);
  return sample;
}

// alpha lives in a TU; shared lives in a header two TUs include; lone has
// no TU list and is grouped by its file.
std::vector<MetricSample> statedSamples() {
  return {sampleAt("alpha", "/p/a/x.cpp", 3, 5, {"/p/a/x.cpp"}, 2),
          sampleAt("shared", "/p/inc/h.hpp", 1, 5,
                   {"/p/a/x.cpp", "/p/b/y.cpp"}, 1),
          sampleAt("lone", "/p/b/y.cpp", 9, 1, {}, 4)};
}

struct EmptyIndexes {
  CallGraph graph;
  ControlFlowIndex cfIndex;
  ControlFlowOracle oracle{graph, cfIndex};
  std::vector<std::string> entryPoints;
  ToolContext ctx{graph, oracle, cfIndex, entryPoints};
  EffectTable effects = EffectTable::builtin();
  MetricContext metricCtx{ctx, effects};
};

std::vector<std::string> entityNames(const MetricReport &report) {
  std::vector<std::string> out;
  for (const MetricRecord &record : report.records)
    out.push_back(record.entity.name);
  return out;
}

int64_t componentOf(const MetricRecord &record, llvm::StringRef name) {
  for (const auto &entry : record.components)
    if (entry.first == name)
      return entry.second;
  return -1;
}

// The registry is process-wide; see the note in test_extensions.cpp.
struct RegistryReset {
  RegistryReset() { ExtensionRegistry::instance().clear(); }
  ~RegistryReset() { ExtensionRegistry::instance().clear(); }
};

struct Run {
  int code = -1;
  std::string out, err;
};

Run runVerb(std::vector<std::string> args) {
  Run r;
  llvm::raw_string_ostream out(r.out), err(r.err);
  std::istringstream in("");
  r.code = runMegascopeQueryVerb(args, out, err, in);
  out.flush();
  err.flush();
  return r;
}

llvm::json::Object parseObject(llvm::StringRef text) {
  auto v = llvm::json::parse(text);
  REQUIRE(bool(v));
  return objectOf(*v);
}

std::vector<std::string> withFixtureSources(std::vector<std::string> args) {
  for (const std::string &source : fixtureSources()) {
    args.push_back("--source");
    args.push_back(source);
  }
  args.insert(args.end(), {"--threads", "1", "--", "-std=c++17"});
  return args;
}

} // namespace

// ============================================================================
// Selection
// ============================================================================

TEST_CASE("metric specs select by name like check specs", "[metrics]") {
  StatedMetric a("alpha-size", {}), b("beta-size", {}), c("gamma-size", {});
  std::vector<const Metric *> known = {&a, &b, &c};
  std::vector<const Metric *> selected;
  std::string error;

  auto selectedNames = [&] {
    std::vector<std::string> out;
    for (const Metric *metric : selected)
      out.push_back(metric->name());
    return out;
  };
  using Names = std::vector<std::string>;

  REQUIRE(resolveMetricSpec("", known, selected, error));
  CHECK(selectedNames() == Names{"alpha-size", "beta-size", "gamma-size"});

  REQUIRE(resolveMetricSpec("all", known, selected, error));
  CHECK(selectedNames().size() == 3);

  REQUIRE(resolveMetricSpec("gamma-size, alpha-size", known, selected, error));
  CHECK(selectedNames() == Names{"alpha-size", "gamma-size"});

  // A spec that opens with a removal starts from everything.
  REQUIRE(resolveMetricSpec("-beta-size", known, selected, error));
  CHECK(selectedNames() == Names{"alpha-size", "gamma-size"});

  REQUIRE(resolveMetricSpec("all,-alpha-size,-gamma-size", known, selected,
                            error));
  CHECK(selectedNames() == Names{"beta-size"});

  REQUIRE(resolveMetricSpec("-all,beta-size", known, selected, error));
  CHECK(selectedNames() == Names{"beta-size"});

  SECTION("an unknown name is an error naming the known ones") {
    CHECK_FALSE(resolveMetricSpec("alpha-size,nope", known, selected, error));
    CHECK(error.find("unknown metric 'nope'") != std::string::npos);
    CHECK(error.find("alpha-size beta-size gamma-size") != std::string::npos);
  }

  SECTION("an empty entry is an error") {
    CHECK_FALSE(resolveMetricSpec("alpha-size,,beta-size", known, selected,
                                  error));
    CHECK(error.find("empty entry") != std::string::npos);
  }
}

TEST_CASE("static-init-cost is a built-in metric", "[metrics]") {
  bool found = false;
  std::vector<const Metric *> metrics = allMetrics();
  for (const Metric *metric : metrics)
    found = found || metric->name() == "static-init-cost";
  CHECK(found);
  CHECK(std::is_sorted(metrics.begin(), metrics.end(),
                       [](const Metric *a, const Metric *b) {
                         return a->name() < b->name();
                       }));
}

// ============================================================================
// Roll-ups
// ============================================================================

TEST_CASE("the target map names the targets that own a TU", "[metrics]") {
  TargetOwners owners;
  std::string error;
  REQUIRE(TargetOwners::parse(R"json({
    "schema": 1,
    "root": "src",
    "targets": [
      {"name": "core", "kind": "static_library",
       "sources": ["core/a.cpp", "/abs/b.cpp"]},
      {"name": "app", "kind": "executable",
       "sources": ["core/../core/a.cpp", "app/main.cpp"]},
      {"name": "headers", "kind": "interface_library"}
    ]})json",
                              "/repo", owners, error));
  CHECK(owners.targetCount() == 3);

  using Names = std::vector<std::string>;
  CHECK(Names(owners.ownersOf("/repo/src/core/a.cpp")) == Names{"app", "core"});
  CHECK(Names(owners.ownersOf("/repo/src/./core/a.cpp")) ==
        Names{"app", "core"});
  CHECK(Names(owners.ownersOf("/abs/b.cpp")) == Names{"core"});
  CHECK(Names(owners.ownersOf("/repo/src/app/main.cpp")) == Names{"app"});
  CHECK(owners.ownersOf("/repo/src/other.cpp").empty());

  SECTION("malformed maps are rejected with the reason") {
    TargetOwners out;
    CHECK_FALSE(TargetOwners::parse("{not json", "/repo", out, error));
    CHECK(error.find("parse error") != std::string::npos);
    CHECK_FALSE(TargetOwners::parse(R"({"target": []})", "/repo", out, error));
    CHECK(error.find("`targets` array") != std::string::npos);
    CHECK_FALSE(TargetOwners::parse(R"({"targets": [{"sources": []}]})",
                                    "/repo", out, error));
    CHECK(error.find("`name`") != std::string::npos);
    CHECK_FALSE(TargetOwners::parse(
        R"({"targets": [{"name": "t", "sources": "a.cpp"}]})", "/repo", out,
        error));
    CHECK(error.find("must be an array") != std::string::npos);
    CHECK_FALSE(TargetOwners::parse(
        R"({"targets": [{"name": "t", "sources": [1]}]})", "/repo", out,
        error));
    CHECK(error.find("must hold strings") != std::string::npos);
  }

  SECTION("a missing file is an error naming the path") {
    TargetOwners out;
    CHECK_FALSE(TargetOwners::load("/no/such/targets.json", out, error));
    CHECK(error.find("/no/such/targets.json") != std::string::npos);
  }
}

TEST_CASE("records roll up by file then TU then directory then target",
          "[metrics]") {
  EmptyIndexes none;
  StatedMetric metric("stated-size", statedSamples());
  using Names = std::vector<std::string>;

  SECTION("symbol: the samples, largest first, ties by location") {
    MetricReport report =
        runMetric(metric, none.metricCtx, RollupLevel::Symbol, nullptr);
    CHECK(report.metric == "stated-size");
    CHECK(report.unit == "bytes");
    CHECK(report.level == RollupLevel::Symbol);
    CHECK(entityNames(report) == Names{"alpha", "shared", "lone"});
    CHECK(report.records[0].entity.kind == "variable");
    CHECK(report.records[0].metric == "stated-size");
    CHECK(report.records[0].unit == "bytes");

    CHECK(report.overall.entity.kind == "total");
    CHECK(report.overall.value == 11);
    CHECK(componentOf(report.overall, "parts") == 7);
    CHECK(componentOf(report.overall, "entities") == 3);
  }

  SECTION("file: grouped by where the entity is written") {
    MetricReport report =
        runMetric(metric, none.metricCtx, RollupLevel::File, nullptr);
    CHECK(entityNames(report) ==
          Names{"/p/a/x.cpp", "/p/inc/h.hpp", "/p/b/y.cpp"});
    CHECK(report.records[0].entity.kind == "file");
    CHECK(report.records[0].entity.file == "/p/a/x.cpp");
    CHECK(report.records[0].value == 5);
    CHECK(componentOf(report.records[0], "entities") == 1);
  }

  SECTION("tu: a header's sample counts in every TU that includes it") {
    MetricReport report =
        runMetric(metric, none.metricCtx, RollupLevel::Tu, nullptr);
    CHECK(entityNames(report) == Names{"/p/a/x.cpp", "/p/b/y.cpp"});
    CHECK(report.records[0].entity.kind == "tu");
    CHECK(report.records[0].value == 10);
    CHECK(componentOf(report.records[0], "parts") == 3);
    CHECK(componentOf(report.records[0], "entities") == 2);
    CHECK(report.records[1].value == 6);
    // The total is over the samples, not over the groups.
    CHECK(report.overall.value == 11);
  }

  SECTION("directory: the TU's directory") {
    MetricReport report =
        runMetric(metric, none.metricCtx, RollupLevel::Directory, nullptr);
    CHECK(entityNames(report) == Names{"/p/a", "/p/b"});
    CHECK(report.records[0].entity.kind == "directory");
    CHECK(report.records[0].entity.file.empty());
    CHECK(report.records[0].value == 10);
  }

  SECTION("target: every target that lists the TU, and a bucket for none") {
    TargetOwners owners;
    std::string error;
    REQUIRE(TargetOwners::parse(R"json({"targets": [
        {"name": "liba", "sources": ["a/x.cpp"]},
        {"name": "tools", "sources": ["a/x.cpp"]}]})json",
                                "/p", owners, error));
    MetricReport report =
        runMetric(metric, none.metricCtx, RollupLevel::Target, &owners);
    // liba and tools tie at 10 and order by name; y.cpp has no target.
    CHECK(entityNames(report) == Names{"liba", "tools", kNoTargetName});
    CHECK(report.records[0].entity.kind == "target");
    CHECK(report.records[0].value == 10);
    CHECK(report.records[2].value == 6);
  }

  SECTION("a metric reports at its native level when that is coarser") {
    StatedMetric perTu("stated-tu-size", statedSamples(), RollupLevel::Tu);
    MetricReport report =
        runMetric(perTu, none.metricCtx, RollupLevel::Symbol, nullptr);
    CHECK(report.level == RollupLevel::Tu);
    CHECK(report.records.size() == 3);
    report = runMetric(perTu, none.metricCtx, RollupLevel::Directory, nullptr);
    CHECK(report.level == RollupLevel::Directory);
  }

  SECTION("sample order does not change the report") {
    std::vector<MetricSample> reversed = statedSamples();
    std::reverse(reversed.begin(), reversed.end());
    StatedMetric other("stated-size", reversed);
    for (RollupLevel level : {RollupLevel::Symbol, RollupLevel::File,
                              RollupLevel::Tu, RollupLevel::Directory}) {
      MetricReport a = runMetric(metric, none.metricCtx, level, nullptr);
      MetricReport b = runMetric(other, none.metricCtx, level, nullptr);
      CHECK(entityNames(a) == entityNames(b));
      CHECK(a.overall.value == b.overall.value);
    }
  }

  SECTION("level names round-trip") {
    for (RollupLevel level : {RollupLevel::Symbol, RollupLevel::File,
                              RollupLevel::Tu, RollupLevel::Directory,
                              RollupLevel::Target})
      CHECK(rollupLevelFromName(rollupLevelName(level)) == level);
    CHECK_FALSE(rollupLevelFromName("module").has_value());
  }
}

// ============================================================================
// static-init-cost over examples/static_init
// ============================================================================

TEST_CASE("static-init-cost measures each initializer of the fixture",
          "[metrics][static-init]") {
  BakedIndexes ix = bakeFiles(fixtureSources());
  Surface surface(ix);
  llvm::json::Object payload = surface.run();
  using Chain = std::vector<std::string>;

  CHECK(payload.getString("status") == "ok");
  CHECK(payload.getString("rollup") == "symbol");
  CHECK(payload.getInteger("total") == 8);

  SECTION("record shape") {
    const llvm::json::Object *record = recordNamed(payload, "reg::coreSlot");
    REQUIRE(record != nullptr);
    CHECK(record->getString("metric") == "static-init-cost");
    CHECK(record->getString("unit") == "functions");
    const llvm::json::Object *entity = record->getObject("entity");
    REQUIRE(entity != nullptr);
    CHECK(entity->getString("kind") == "variable");
    CHECK(entity->getString("file") == fixture("core/registry.cpp"));
    CHECK(entity->getInteger("line").value_or(0) > 0);
    CHECK_FALSE(entity->getString("usr").value_or("").empty());
  }

  SECTION("reach, effects and the chain that shows each effect") {
    // registerPlugin -> {nextId, allocateEntry -> malloc,
    // pthread_mutex_lock, pthread_mutex_unlock}.
    const llvm::json::Object *core = recordNamed(payload, "reg::coreSlot");
    REQUIRE(core != nullptr);
    CHECK(core->getInteger("value") == 6);
    CHECK(component(*core, "directCalls") == 1);
    CHECK(component(*core, "allocation") == 1);
    CHECK(component(*core, "lock") == 1);
    CHECK(component(*core, "io") == 0);
    CHECK(component(*core, "tus") == 1);
    CHECK(chain(*core, "allocation") ==
          Chain{"reg::registerPlugin",
                "reg::(anonymous namespace)::allocateEntry", "malloc"});
    CHECK(chain(*core, "lock") ==
          Chain{"reg::registerPlugin", "pthread_mutex_lock"});
    CHECK(chain(*core, "io").empty());
  }

  SECTION("a header initializer is one record that lists its TUs") {
    const llvm::json::Object *header = recordNamed(payload, "reg::headerSlot");
    REQUIRE(header != nullptr);
    CHECK(header->getInteger("value") == 6);
    CHECK(component(*header, "tus") == 3);
    CHECK(header->getObject("entity")->getString("file") ==
          fixture("core/registry.hpp"));
  }

  SECTION("a constructor's body and a lambda's body are reached") {
    const llvm::json::Object *banner = recordNamed(payload, "banner");
    REQUIRE(banner != nullptr);
    CHECK(banner->getInteger("value") == 3);
    CHECK(chain(*banner, "io") ==
          Chain{"Banner::Banner", "reg::announce", "puts"});

    const llvm::json::Object *greeted = recordNamed(payload, "greeted");
    REQUIRE(greeted != nullptr);
    CHECK(greeted->getInteger("value") == 3);
    Chain io = chain(*greeted, "io");
    REQUIRE(io.size() == 3);
    CHECK(llvm::StringRef(io[0]).starts_with("lambda#"));
    CHECK(io[1] == "reg::announce");
  }

  SECTION("a constructor function measures what it calls, not itself") {
    const llvm::json::Object *onLoad = recordNamed(payload, "onLoad");
    REQUIRE(onLoad != nullptr);
    CHECK(onLoad->getObject("entity")->getString("kind") ==
          "constructor-function");
    CHECK(onLoad->getInteger("value") == 2);
    CHECK(component(*onLoad, "directCalls") == 1);
    CHECK(chain(*onLoad, "io") == Chain{"reg::announce", "puts"});
  }

  SECTION("an instantiated callee continues through its template's "
          "definition") {
    const llvm::json::Object *codec = recordNamed(payload, "codecSlot");
    REQUIRE(codec != nullptr);
    // registerType<Codec> itself, plus everything registerPlugin reaches.
    CHECK(codec->getInteger("value") == 7);
    CHECK(chain(*codec, "lock") == Chain{"reg::registerType",
                                         "reg::registerPlugin",
                                         "pthread_mutex_lock"});
  }

  SECTION("a new-expression in the initializer is an allocation") {
    const llvm::json::Object *scratch = recordNamed(payload, "scratch");
    REQUIRE(scratch != nullptr);
    CHECK(scratch->getInteger("value") == 1);
    CHECK(component(*scratch, "allocation") == 1);
    CHECK(chain(*scratch, "allocation") == Chain{"operator new[]"});
  }

  SECTION("an initializer with no effect carries no witness") {
    const llvm::json::Object *ordinal = recordNamed(payload, "ordinal");
    REQUIRE(ordinal != nullptr);
    CHECK(ordinal->getInteger("value") == 1);
    CHECK(ordinal->get("witness") == nullptr);
  }

  SECTION("constant-initialized variables are not measured") {
    CHECK(recordNamed(payload, "kLimit") == nullptr);
    CHECK(recordNamed(payload, "limit") == nullptr);
  }

  SECTION("largest reach first, ties by location") {
    std::vector<std::string> order = names(payload);
    CHECK(order == std::vector<std::string>{
                       "codecSlot",       // 7
                       "reg::coreSlot",   // 6, core/registry.cpp
                       "reg::headerSlot", // 6, core/registry.hpp
                       "greeted",         // 3, plugins.cpp:21
                       "banner",          // 3, plugins.cpp:30
                       "onLoad",          // 2
                       "ordinal",         // 1, plugins.cpp:14
                       "scratch"});       // 1, plugins.cpp:17
  }

  SECTION("the total is the union of every reach, with the counts #95 "
          "asks for") {
    const llvm::json::Array *metrics = payload.getArray("metrics");
    REQUIRE(metrics != nullptr);
    REQUIRE(metrics->size() == 1);
    const llvm::json::Object &summary = *(*metrics)[0].getAsObject();
    CHECK(summary.getString("metric") == "static-init-cost");
    CHECK(summary.getString("unit") == "functions");
    CHECK(summary.getString("level") == "symbol");
    CHECK(summary.getInteger("records") == 8);
    const llvm::json::Object *overall = summary.getObject("overall");
    REQUIRE(overall != nullptr);
    // Twelve distinct functions, though the per-initializer reaches sum
    // to 29: most initializers reach the same registry code.
    CHECK(overall->getInteger("value") == 12);
    CHECK(component(*overall, "dynamicInitializers") == 7);
    CHECK(component(*overall, "constructorFunctions") == 1);
    CHECK(component(*overall, "entities") == 8);
    CHECK(component(*overall, "allocation") == 4);
    CHECK(component(*overall, "lock") == 3);
    CHECK(component(*overall, "io") == 3);
  }
}

TEST_CASE("static-init-cost rolls up by TU and directory and target",
          "[metrics][static-init]") {
  BakedIndexes ix = bakeFiles(fixtureSources());
  Surface surface(ix);

  SECTION("tu: the header's initializer counts in each TU") {
    llvm::json::Object payload = surface.run({{"rollup", "tu"}});
    CHECK(payload.getInteger("total") == 3);
    CHECK(names(payload) ==
          std::vector<std::string>{fixture("plugins/plugins.cpp"),
                                   fixture("plugins/codecs.cpp"),
                                   fixture("core/registry.cpp")});
    CHECK(valueOf(payload, "plugins/plugins.cpp") == 11);
    CHECK(valueOf(payload, "plugins/codecs.cpp") == 7);
    CHECK(valueOf(payload, "core/registry.cpp") == 6);

    const llvm::json::Object *plugins =
        recordNamed(payload, "plugins/plugins.cpp");
    CHECK(plugins->getObject("entity")->getString("kind") == "tu");
    // ordinal, scratch, greeted, banner and headerSlot; plus onLoad.
    CHECK(component(*plugins, "dynamicInitializers") == 5);
    CHECK(component(*plugins, "constructorFunctions") == 1);
    CHECK(component(*plugins, "entities") == 6);
    CHECK(component(*plugins, "io") == 3);
    // A group's value is a union and carries no single chain.
    CHECK(plugins->get("witness") == nullptr);
  }

  SECTION("file: the header is its own row") {
    llvm::json::Object payload = surface.run({{"rollup", "file"}});
    CHECK(payload.getInteger("total") == 4);
    CHECK(valueOf(payload, "core/registry.hpp") == 6);
    CHECK(valueOf(payload, "plugins/codecs.cpp") == 7);
  }

  SECTION("directory") {
    llvm::json::Object payload = surface.run({{"rollup", "directory"}});
    CHECK(names(payload) ==
          std::vector<std::string>{fixture("plugins"), fixture("core")});
    CHECK(valueOf(payload, "examples/static_init/plugins") == 12);
    CHECK(valueOf(payload, "examples/static_init/core") == 6);
  }

  SECTION("target, through the target map") {
    llvm::json::Object payload = surface.run(
        {{"rollup", "target"}, {"target_map", fixture("targets.json")}});
    CHECK(payload.getString("status") == "ok");
    CHECK(names(payload) == std::vector<std::string>{"plugins", "core"});
    CHECK(valueOf(payload, "plugins") == 12);
    CHECK(valueOf(payload, "core") == 6);
    const llvm::json::Object *core = recordNamed(payload, "core");
    CHECK(core->getObject("entity")->getString("kind") == "target");
    // coreSlot and the header's initializer.
    CHECK(component(*core, "dynamicInitializers") == 2);
  }
}

TEST_CASE("the effect table decides what counts as an effect",
          "[metrics][static-init][ext]") {
  RegistryReset reset;
  BakedIndexes ix = bakeFiles(fixtureSources());
  Surface surface(ix);

  const llvm::json::Object before = surface.run();
  const llvm::json::Object *ordinal = recordNamed(before, "ordinal");
  REQUIRE(ordinal != nullptr);
  CHECK(component(*ordinal, "lock") == 0);

  // The organization says reg::nextId takes a lock.
  OrgConfig cfg;
  std::string error;
  REQUIRE(parseOrgConfigJson(R"json({"effects": {"lock": ["reg::nextId"]}})json",
                             cfg, error));
  REQUIRE(applyOrgConfig(cfg, error));

  const llvm::json::Object after = surface.run();
  ordinal = recordNamed(after, "ordinal");
  REQUIRE(ordinal != nullptr);
  CHECK(component(*ordinal, "lock") == 1);
  CHECK(chain(*ordinal, "lock") == std::vector<std::string>{"reg::nextId"});
  // The reach itself does not depend on the table.
  CHECK(ordinal->getInteger("value") == 1);
}

// ============================================================================
// Deterministic order
// ============================================================================

TEST_CASE("metrics payloads are byte-identical across thread counts and TU "
          "order",
          "[metrics][determinism]") {
  std::vector<std::string> forward = fixtureSources();
  std::vector<std::string> reversed(forward.rbegin(), forward.rend());

  BakedIndexes serial = bakeFiles(forward, 1);
  BakedIndexes parallel = bakeFiles(forward, 4);
  BakedIndexes backwards = bakeFiles(reversed, 3);
  Surface a(serial), b(parallel), c(backwards);

  for (const char *rollup : {"symbol", "file", "tu", "directory"}) {
    INFO("rollup " << rollup);
    std::string first = serialized(a.runValue({{"rollup", rollup}}));
    CHECK(first == serialized(b.runValue({{"rollup", rollup}})));
    CHECK(first == serialized(c.runValue({{"rollup", rollup}})));
    // And the same context answers the same way twice.
    CHECK(first == serialized(a.runValue({{"rollup", rollup}})));
  }

  std::string map = fixture("targets.json");
  std::string first =
      serialized(a.runValue({{"rollup", "target"}, {"target_map", map}}));
  CHECK(first ==
        serialized(b.runValue({{"rollup", "target"}, {"target_map", map}})));
  CHECK(first ==
        serialized(c.runValue({{"rollup", "target"}, {"target_map", map}})));
}

TEST_CASE("paging walks the one canonical record order",
          "[metrics][determinism]") {
  BakedIndexes ix = bakeFiles(fixtureSources());
  Surface surface(ix);
  std::vector<std::string> whole = names(surface.run());
  REQUIRE(whole.size() == 8);

  std::vector<std::string> paged;
  for (int64_t offset = 0; offset < 8; offset += 3) {
    llvm::json::Object page = surface.run({{"limit", 3}, {"offset", offset}});
    CHECK(page.getInteger("total") == 8);
    CHECK(page.getBoolean("truncated") == (offset + 3 < 8));
    for (const std::string &name : names(page))
      paged.push_back(name);
  }
  CHECK(paged == whole);
}

// ============================================================================
// Result contract
// ============================================================================

TEST_CASE("every metrics payload carries status and indexScope",
          "[metrics][contract]") {
  BakedIndexes ix = bakeFiles(fixtureSources());
  Surface surface(ix);

  auto stamped = [](const llvm::json::Object &payload,
                    llvm::StringRef status) {
    INFO(serialized(llvm::json::Value(llvm::json::Object(payload))));
    CHECK(payload.getString("status") == status);
    CHECK(payload.getObject("indexScope") != nullptr);
  };

  SECTION("a report") { stamped(surface.run(), "ok"); }

  SECTION("each roll-up") {
    for (const char *rollup : {"file", "tu", "directory"})
      stamped(surface.run({{"rollup", rollup}}), "ok");
  }

  SECTION("the metric list") {
    llvm::json::Object payload = surface.run({{"list_metrics", true}});
    stamped(payload, "ok");
    bool found = false;
    for (const llvm::json::Value &value : recordsOf(payload)) {
      const llvm::json::Object &entry = *value.getAsObject();
      if (entry.getString("metric") != "static-init-cost")
        continue;
      found = true;
      CHECK(entry.getString("unit") == "functions");
      CHECK(entry.getString("level") == "symbol");
      CHECK_FALSE(entry.getString("summary").value_or("").empty());
    }
    CHECK(found);
  }

  SECTION("no facts is unavailable, never a zero") {
    Surface loaded(ix, /*withStaticInits=*/false);
    llvm::json::Object payload = loaded.run();
    stamped(payload, "unavailable");
    CHECK(payload.get("records") == nullptr);
    std::string message = payload.getString("error").value_or("").str();
    CHECK(message.find("static-init-cost") != std::string::npos);
    CHECK(message.find("--source") != std::string::npos);
    // The list does not read the facts and still answers.
    stamped(loaded.run({{"list_metrics", true}}), "ok");
  }

  SECTION("collected facts with no initializer are an empty report") {
    std::string path;
    {
      llvm::SmallString<128> tmp;
      int fd = -1;
      REQUIRE_FALSE(
          llvm::sys::fs::createTemporaryFile("vycor_metrics", "cpp", fd, tmp));
      ::close(fd);
      path = std::string(tmp.str());
      std::ofstream out(path);
      out << "int quiet() { return 1; }\nint main() { return quiet(); }\n";
    }
    BakedIndexes quiet = bakeFiles({path});
    llvm::sys::fs::remove(path);
    REQUIRE(quiet.staticInits.collected);
    Surface none(quiet);
    llvm::json::Object payload = none.run();
    CHECK(payload.getObject("indexScope") != nullptr);
    CHECK(payload.getString("status") != "unavailable");
    CHECK(payload.getInteger("total") == 0);
    CHECK(recordsOf(payload).empty());
  }

  SECTION("malformed requests are usage errors") {
    auto usage = [&](std::initializer_list<llvm::json::Object::KV> kvs,
                     llvm::StringRef needle) {
      llvm::json::Object payload = surface.run(kvs);
      stamped(payload, "usage_error");
      INFO(needle.str());
      CHECK(payload.getString("error").value_or("").contains(needle));
    };
    usage({{"metrics", "nope"}}, "unknown metric 'nope'");
    usage({{"rollup", "module"}}, "Invalid rollup");
    usage({{"rollup", "target"}}, "Requires target_map");
    usage({{"rollup", "target"}, {"target_map", "/no/such/map.json"}},
          "Invalid target_map");
    usage({{"target_map", fixture("targets.json")}}, "rollup=target");
    usage({{"limit", -1}}, "limit");
  }
}

TEST_CASE("a request that cannot be answered does not ask for an index",
          "[metrics][contract]") {
  using O = llvm::json::Object;
  CHECK(metricsRequestReadsIndex(O{}));
  CHECK(metricsRequestReadsIndex(O{{"rollup", "tu"}}));
  CHECK(metricsRequestReadsIndex(O{{"metrics", "static-init-cost"}}));
  CHECK_FALSE(metricsRequestReadsIndex(O{{"list_metrics", true}}));
  CHECK_FALSE(metricsRequestReadsIndex(O{{"metrics", "nope"}}));
  CHECK_FALSE(metricsRequestReadsIndex(O{{"rollup", "module"}}));
  CHECK_FALSE(metricsRequestReadsIndex(O{{"rollup", "target"}}));
}

// ============================================================================
// Organization metrics
// ============================================================================

TEST_CASE("organization metrics register like organization checks",
          "[metrics][ext]") {
  RegistryReset reset;
  BakedIndexes ix = bakeFiles(fixtureSources());
  Surface surface(ix);

  ExtensionRegistry::instance().addMetric(
      std::make_unique<StatedMetric>("org-size", statedSamples()));

  SECTION("listed beside the built-in ones, by name") {
    llvm::json::Object payload = surface.run({{"list_metrics", true}});
    std::vector<std::string> listed;
    for (const llvm::json::Value &value : recordsOf(payload))
      listed.push_back(
          value.getAsObject()->getString("metric").value_or("").str());
    CHECK(listed ==
          std::vector<std::string>{"org-size", "static-init-cost"});
  }

  SECTION("selected by name and reported in the one record shape") {
    llvm::json::Object payload = surface.run({{"metrics", "org-size"}});
    CHECK(payload.getString("status") == "ok");
    CHECK(names(payload) ==
          std::vector<std::string>{"alpha", "shared", "lone"});
    const llvm::json::Object *alpha = recordNamed(payload, "alpha");
    CHECK(alpha->getString("metric") == "org-size");
    CHECK(alpha->getString("unit") == "bytes");
    CHECK(component(*alpha, "parts") == 2);
  }

  SECTION("several metrics: grouped by metric name, each in its own "
          "order") {
    llvm::json::Object payload = surface.run();
    CHECK(payload.getInteger("total") == 11);
    const llvm::json::Array *metrics = payload.getArray("metrics");
    REQUIRE(metrics != nullptr);
    REQUIRE(metrics->size() == 2);
    CHECK((*metrics)[0].getAsObject()->getString("metric") == "org-size");
    CHECK((*metrics)[1].getAsObject()->getString("metric") ==
          "static-init-cost");
    const llvm::json::Array &records = recordsOf(payload);
    CHECK(records[0].getAsObject()->getString("metric") == "org-size");
    CHECK(records[3].getAsObject()->getString("metric") ==
          "static-init-cost");
  }

  SECTION("a built-in name is not replaced") {
    ExtensionRegistry::instance().addMetric(
        std::make_unique<StatedMetric>("static-init-cost", statedSamples()));
    llvm::json::Object payload =
        surface.run({{"metrics", "static-init-cost"}});
    CHECK(payload.getInteger("total") == 8);
  }

  SECTION("clear drops them") {
    ExtensionRegistry::instance().clear();
    CHECK(ExtensionRegistry::instance().metrics().empty());
    llvm::json::Object payload = surface.run({{"metrics", "org-size"}});
    CHECK(payload.getString("status") == "usage_error");
  }
}

TEST_CASE("the org config's effects key extends the effect table",
          "[metrics][ext]") {
  RegistryReset reset;
  OrgConfig cfg;
  std::string error;

  REQUIRE(parseOrgConfigJson(R"json({
    "effects": {
      "allocation": ["myorg::Arena::grow"],
      "lock": ["myorg::SpinLock::acquire", "myorg::Guard<T>::Guard"],
      "io": ["myorg::log"]
    }})json",
                             cfg, error));
  CHECK(cfg.effects[static_cast<unsigned>(EffectKind::Allocation)] ==
        std::vector<std::string>{"myorg::Arena::grow"});
  CHECK(cfg.effects[static_cast<unsigned>(EffectKind::Lock)].size() == 2);
  CHECK(cfg.effects[static_cast<unsigned>(EffectKind::Io)] ==
        std::vector<std::string>{"myorg::log"});

  REQUIRE(applyOrgConfig(cfg, error));
  const EffectTable &table = ExtensionRegistry::instance().effectTable();
  CHECK(table.effectsOf("myorg::Arena::grow") ==
        effectBit(EffectKind::Allocation));
  CHECK(table.effectsOf("myorg::Guard<int>::Guard") ==
        effectBit(EffectKind::Lock));
  CHECK(table.effectsOf("myorg::log") == effectBit(EffectKind::Io));
  // On top of the built-in entries, not in place of them.
  CHECK(table.effectsOf("malloc") == effectBit(EffectKind::Allocation));

  SECTION("clear returns to the built-in table") {
    ExtensionRegistry::instance().clear();
    const EffectTable &fresh = ExtensionRegistry::instance().effectTable();
    CHECK(fresh.effectsOf("myorg::log") == 0);
    CHECK(fresh.effectsOf("malloc") == effectBit(EffectKind::Allocation));
  }

  SECTION("an unknown effect or a malformed list is rejected") {
    OrgConfig bad;
    CHECK_FALSE(parseOrgConfigJson(R"({"effects": {"network": ["send"]}})",
                                   bad, error));
    CHECK(error.find("unknown effect 'network'") != std::string::npos);
    CHECK_FALSE(parseOrgConfigJson(R"({"effects": ["malloc"]})", bad, error));
    CHECK_FALSE(parseOrgConfigJson(R"({"effects": {"io": "puts"}})", bad,
                                   error));
    CHECK_FALSE(parseOrgConfigJson(R"({"effects": {"io": [1]}})", bad, error));
  }
}

// ============================================================================
// CLI
// ============================================================================

TEST_CASE("metrics verb: list and usage errors answer without an index",
          "[metrics][cli]") {
  SECTION("--list-metrics") {
    Run r = runVerb({"metrics", "--list-metrics"});
    INFO(r.out << r.err);
    CHECK(r.code == 0);
    llvm::json::Object payload = parseObject(r.out);
    CHECK(payload.getString("status") == "ok");
    CHECK(payload.getObject("indexScope") != nullptr);
    CHECK(r.out.find("static-init-cost") != std::string::npos);
  }

  SECTION("an unknown metric is a usage error before any bake") {
    // The source does not exist: reaching the bake would fail differently.
    Run r = runVerb({"metrics", "--metrics", "nope", "--source",
                     "/no/such/file.cpp", "--", "-std=c++17"});
    INFO(r.out << r.err);
    CHECK(r.code == 2);
    llvm::json::Object payload = parseObject(r.out);
    CHECK(payload.getString("status") == "usage_error");
    CHECK(payload.getObject("indexScope") != nullptr);
  }

  SECTION("an unknown rollup is a usage error") {
    Run r = runVerb({"metrics", "--rollup", "module"});
    CHECK(r.code == 2);
  }
}

TEST_CASE("metrics verb: an in-memory bake answers and a saved index is "
          "unavailable",
          "[metrics][cli]") {
  SECTION("json") {
    Run r = runVerb(withFixtureSources({"metrics"}));
    INFO(r.out << r.err);
    REQUIRE(r.code == 0);
    llvm::json::Object payload = parseObject(r.out);
    CHECK(payload.getString("status") == "ok");
    CHECK(payload.getInteger("total") == 8);
    const llvm::json::Object *scope = payload.getObject("indexScope");
    REQUIRE(scope != nullptr);
    CHECK(scope->getString("freshness") == "baked");
    CHECK(scope->getBoolean("complete") == true);
    CHECK(valueOf(payload, "codecSlot") == 7);
  }

  SECTION("ndjson: a summary line then one record per line") {
    Run r = runVerb(withFixtureSources(
        {"metrics", "--rollup", "tu", "--format", "ndjson"}));
    INFO(r.out << r.err);
    REQUIRE(r.code == 0);
    std::vector<std::string> lines;
    std::stringstream ss(r.out);
    for (std::string line; std::getline(ss, line);)
      lines.push_back(line);
    REQUIRE(lines.size() == 4);
    llvm::json::Object summary = parseObject(lines[0]);
    const llvm::json::Object *head = summary.getObject("_summary");
    REQUIRE(head != nullptr);
    CHECK(head->getString("status") == "ok");
    CHECK(head->getObject("indexScope") != nullptr);
    CHECK(head->getString("rollup") == "tu");
    llvm::json::Object first = parseObject(lines[1]);
    CHECK(first.getString("metric") == "static-init-cost");
    CHECK(first.getInteger("value") == 11);
  }

  SECTION("tsv") {
    Run r = runVerb(withFixtureSources({"metrics", "--format", "tsv"}));
    INFO(r.out << r.err);
    CHECK(r.code == 0);
    CHECK(r.out.find("static-init-cost") != std::string::npos);
  }

  SECTION("target roll-up through --target-map") {
    Run r = runVerb(withFixtureSources({"metrics", "--rollup", "target",
                                        "--target-map",
                                        fixture("targets.json")}));
    INFO(r.out << r.err);
    REQUIRE(r.code == 0);
    llvm::json::Object payload = parseObject(r.out);
    CHECK(valueOf(payload, "plugins") == 12);
    CHECK(valueOf(payload, "core") == 6);
  }

  SECTION("a saved index does not carry the facts") {
    llvm::SmallString<128> index;
    llvm::sys::fs::createUniquePath("vycor-metrics-%%%%%%.vycs", index,
                                    /*MakeAbsolute=*/true);
    CallGraph graph;
    graph.addNode({"main", "/src/a.cpp", 1, true, false, ""}, "/src/a.cpp");
    ControlFlowIndex cf;
    SnapshotMeta meta;
    meta.files = {{"/src/a.cpp", 1, 2}};
    REQUIRE(SnapshotIO::save(std::string(index.str()), graph, cf, meta));
    Run r = runVerb({"metrics", "--index", std::string(index.str())});
    std::remove(index.c_str());
    INFO(r.out << r.err);
    CHECK(r.code == 3);
    llvm::json::Object payload = parseObject(r.out);
    CHECK(payload.getString("status") == "unavailable");
    CHECK(payload.getObject("indexScope") != nullptr);
  }
}
