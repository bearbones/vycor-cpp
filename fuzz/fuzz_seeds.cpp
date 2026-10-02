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

// fuzz_seeds <out-dir> — writes the seed corpora of the fuzz targets from
// the test fixtures, at the current on-disk formats (so a format bump
// never leaves the fuzzers starting from inputs the loader rejects at the
// version check):
//
//   <out>/snapshot/   index files (flag byte 0x01 = reseal), baked from a
//                     small all-features TU
//   <out>/checkpoint/ an anneal journal (flag byte 0x01 = reseal) from a
//                     cold run over an ADL-fragile fixture
//   <out>/shard/      an index shard, a diagnostics shard, and a merged
//                     index handoff file (flag byte 0x04 = reseal)
//   <out>/batch/      NDJSON request streams: the cli-golden batch, and
//                     one request per registered tool
//   <out>/batch.dict  libFuzzer dictionary: tool and argument names
//   <out>/batch.vycs  an examples/deep_chains index for fuzz_batch
//                     (VYCOR_FUZZ_INDEX), so it need not bake at startup

#include "FuzzCommon.h"

#include "vycor/anneal/Analyzer.h"
#include "vycor/anneal/Checkpoint.h"
#include "vycor/anneal/GlobalIndex.h"
#include "vycor/callgraph/ChannelIndex.h"
#include "vycor/query/Tools.h"

#include "clang/Tooling/CompilationDatabase.h"
#include "clang/Tooling/JSONCompilationDatabase.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdio>
#include <set>
#include <string>
#include <vector>

using namespace vycor;

namespace {

std::string gOut;

void writeSeed(const std::string &rel, const std::string &bytes) {
  const std::string path = gOut + "/" + rel;
  llvm::sys::fs::create_directories(llvm::sys::path::parent_path(path));
  fuzz::writeScratch(path, bytes);
  std::printf("%s (%zu bytes)\n", path.c_str(), bytes.size());
}

std::string readFile(const std::string &path) {
  auto buf = llvm::MemoryBuffer::getFile(path, /*IsText=*/false,
                                         /*RequiresNullTerminator=*/false);
  if (!buf) {
    std::fprintf(stderr, "fuzz_seeds: cannot read %s\n", path.c_str());
    std::exit(1);
  }
  return std::string((*buf)->getBuffer());
}

std::string writeSource(const std::string &name, const std::string &text) {
  const std::string path = fuzz::scratchPath(name.c_str());
  fuzz::writeScratch(path, text);
  return path;
}

// Every section populated: virtual dispatch, a function pointer, a lambda,
// nested try/catch with a rethrow, guards, an RAII local, noexcept, and
// two channels.
const char *kAllFeatures = R"cpp(
struct Queue {
  void push(int) {}
  int pop() { return 0; }
};
struct Guard {
  Guard() {}
  ~Guard() {}
};
struct Base {
  virtual void run() = 0;
  virtual ~Base() = default;
};
struct Impl : Base {
  void run() override;
};
void sink(int) noexcept {}
void Impl::run() { sink(1); }
struct Replicator {
  Queue fast_, slow_;
  bool streaming = false;
  void onCreate(int evt) {
    if (streaming)
      fast_.push(evt);
    else
      slow_.push(evt);
  }
};
int risky(int x) {
  if (x > 0)
    throw x;
  return x;
}
void driver(Base &b, Replicator &r) {
  Guard g;
  try {
    b.run();
    try {
      risky(2);
    } catch (...) {
      throw;
    }
  } catch (int) {
    sink(0);
  }
  if (r.streaming)
    r.onCreate(3);
  auto fn = [&] { risky(4); };
  fn();
  void (*fp)(int) noexcept = sink;
  fp(5);
}
int main() {
  Impl i;
  Replicator r;
  driver(i, r);
  return r.fast_.pop() + r.slow_.pop();
}
)cpp";

std::string bakeAndSave(const clang::tooling::CompilationDatabase &db,
                        const std::vector<std::string> &files,
                        const ChannelTypeConfig &channels,
                        const char *name) {
  BakedIndexes baked =
      bakeIndexes(db, files, {}, /*threadCount=*/1, nullptr, "", {}, nullptr,
                  nullptr, channels);
  SnapshotMeta meta;
  meta.files = SnapshotIO::stampFiles(files);
  meta.channelTypes = channels.registeredTypes;
  meta.entryPoints = {"main"};
  SnapshotIO::recordDependencies(meta, baked.deps);
  SnapshotIO::recordOutcomes(meta, baked.outcomes);
  const std::string path = fuzz::scratchPath(name);
  std::string error;
  if (!SnapshotIO::save(path, baked.graph, baked.cfIndex, meta,
                        baked.channels, &error, /*durable=*/false)) {
    std::fprintf(stderr, "fuzz_seeds: %s\n", error.c_str());
    std::exit(1);
  }
  return path;
}

void snapshotSeeds(std::string &deepChainsIndex) {
  ChannelTypeConfig channels;
  ChannelTypeSpec queue;
  queue.qualifiedTypeName = "Queue";
  queue.produceMethods = {"push"};
  queue.consumeMethods = {"pop"};
  queue.category = "queue";
  channels.registeredTypes.push_back(queue);

  clang::tooling::FixedCompilationDatabase flat(".", {"-std=c++17"});
  const std::string allFeatures =
      bakeAndSave(flat, {writeSource("all_features.cpp", kAllFeatures)},
                  channels, "all_features.vycs");
  writeSeed("snapshot/all_features", "\x01" + readFile(allFeatures));
  // The same image with checksums left alone: the checksum checks
  // themselves are fuzzed from here.
  writeSeed("snapshot/all_features_raw",
            std::string(1, '\0') + readFile(allFeatures));
  const std::string empty = bakeAndSave(flat, {}, {}, "empty.vycs");
  writeSeed("snapshot/empty", "\x01" + readFile(empty));

  const std::string fixture =
      std::string(PROJECT_SOURCE_DIR) + "/examples/deep_chains/";
  clang::tooling::FixedCompilationDatabase deep(
      fixture, {"-std=c++17", "-I" + fixture});
  deepChainsIndex = bakeAndSave(
      deep, {fixture + "main.cpp", fixture + "callbacks.cpp"}, {},
      "deep_chains.vycs");
  // Not a snapshot seed: its standard-library instantiations make it
  // 0.8 MB, which slows every execution for little extra coverage. The
  // batch seeds draw their arguments from it.
}

// tests/test_anneal_checkpoint.cpp's fixture shape: user_a.cpp sees only
// core.hpp's int overload while user_b.cpp's ext.hpp adds a double one (an
// ADL fallback), plus an inline function and a default argument the two
// TUs define differently (ODR, default-arg divergence), a dynamic
// initializer, and a noexcept function reaching a throw.
void annealSeeds() {
  writeSource("core.hpp", R"cpp(
#pragma once
namespace MathLib {
struct Vector { int x = 0; };
inline void scale(Vector, int) {}
}
int tunable(int level = LEVEL);
#ifdef WIDE
inline int width() { return 2; }
#else
inline int width() { return 1; }
#endif
)cpp");
  writeSource("ext.hpp", R"cpp(
#pragma once
#include "core.hpp"
namespace MathLib {
inline void scale(Vector, double) {}
}
)cpp");
  const std::string a = writeSource("user_a.cpp", R"cpp(
#include "core.hpp"
int seed();
int counter = seed();
void thrower() { throw 1; }
void safe() noexcept { thrower(); }
void use_a() {
  MathLib::Vector v;
  scale(v, 3.14);
  tunable();
  width();
}
)cpp");
  const std::string b = writeSource("user_b.cpp", R"cpp(
#include "core.hpp"
#include "ext.hpp"
extern int counter;
int seed() { return 4; }
int copy = counter;
void use_b() {
  MathLib::Vector v;
  scale(v, 3.14);
  width();
}
)cpp");
  // Each TU gets its own LEVEL and WIDE: a FixedCompilationDatabase
  // applies one command line to all, so use a JSON database.
  const std::string dir = fuzz::scratchDir();
  std::string jsonDb = "[";
  for (const auto &[file, flags] :
       std::vector<std::pair<std::string, std::string>>{
           {a, "\"-DLEVEL=1\""}, {b, "\"-DLEVEL=2\", \"-DWIDE\""}}) {
    if (jsonDb.size() > 1)
      jsonDb += ",";
    jsonDb += "{\"directory\": \"" + dir + "\", \"file\": \"" + file +
              "\", \"arguments\": [\"clang++\", \"-std=c++17\", " + flags +
              ", \"-c\", \"" + file + "\"]}";
  }
  jsonDb += "]";
  std::string error;
  auto db = clang::tooling::JSONCompilationDatabase::loadFromBuffer(
      jsonDb, error, clang::tooling::JSONCommandLineSyntax::AutoDetect);
  if (!db) {
    std::fprintf(stderr, "fuzz_seeds: %s\n", error.c_str());
    std::exit(1);
  }

  AnalysisOptions opts;
  opts.enableOdrDiag = true;
  opts.enableCoverageDiag = true;
  opts.enableExceptionEscapeDiag = true;
  opts.checkpointPath = fuzz::scratchPath("seed.ckpt");
  GlobalIndex merged;
  auto diags = runAnalysis(*db, {a, b}, opts, &merged);
  writeSeed("checkpoint/journal", "\x01" + readFile(opts.checkpointPath));

  const std::vector<TuOutcome> clean{TuOutcome{TuStatus::Indexed, ""}};
  if (!writeAnnealIndexShard(fuzz::scratchPath("index.shard"),
                             {{a, AnnealIndexPayload::capture(merged)}},
                             clean) ||
      !writeAnnealDiagShard(fuzz::scratchPath("diag.shard"), {{a, diags}},
                            clean) ||
      !writeGlobalIndexFile(fuzz::scratchPath("global.index"), merged)) {
    std::fprintf(stderr, "fuzz_seeds: cannot write the shard seeds\n");
    std::exit(1);
  }
  writeSeed("shard/index", "\x04" + readFile(fuzz::scratchPath("index.shard")));
  writeSeed("shard/diag", "\x04" + readFile(fuzz::scratchPath("diag.shard")));
  writeSeed("shard/global",
            "\x04" + readFile(fuzz::scratchPath("global.index")));
}

void batchSeeds(const std::string &deepChainsIndex) {
  // scripts/cli-golden.py's batch query.
  writeSeed("batch/golden",
            "{\"id\":1,\"tool\":\"get_callers\",\"args\":{\"name\":"
            "\"stage2_parse\"}}\n"
            "{\"id\":2,\"tool\":\"lookup_function\",\"args\":{\"name\":"
            "\"nope\"}}\n"
            "{\"id\":3,\"tool\":\"no_such_tool\",\"args\":{}}\n");
  auto snap = SnapshotIO::load(deepChainsIndex, nullptr, LoadMode::ReadOnly);
  if (!snap) {
    std::fprintf(stderr, "fuzz_seeds: cannot reload the fixture index\n");
    std::exit(1);
  }
  const auto pool =
      fuzz::ArgPool::from(snap->graph, snap->cfIndex, &snap->channels);
  std::set<std::string> words = {"tool", "args", "arguments", "id"};
  const auto tools = getRegisteredTools();
  for (size_t i = 0; i < tools.size(); ++i) {
    llvm::json::Object req;
    req["id"] = static_cast<int64_t>(i);
    req["tool"] = tools[i].name;
    req["args"] = pool.argsFor(tools[i], i);
    std::string line;
    llvm::raw_string_ostream os(line);
    os << llvm::json::Value(std::move(req)) << "\n";
    os.flush();
    writeSeed("batch/" + tools[i].name, line);
    words.insert(tools[i].name);
    if (const auto *schema = tools[i].inputSchema.getAsObject())
      if (const auto *props = schema->getObject("properties"))
        for (const auto &prop : *props)
          words.insert(prop.first.str());
  }
  // The index fuzz_batch answers from, so it can skip its own bake:
  // VYCOR_FUZZ_INDEX=<out>/batch.vycs.
  writeSeed("batch.vycs", readFile(deepChainsIndex));
  std::string dict;
  for (const auto &w : words)
    dict += "\"\\\"" + w + "\\\"\"\n";
  writeSeed("batch.dict", dict);
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: fuzz_seeds <out-dir>\n");
    return 2;
  }
  gOut = argv[1];
  std::string deepChainsIndex;
  snapshotSeeds(deepChainsIndex);
  annealSeeds();
  batchSeeds(deepChainsIndex);
  return 0;
}
