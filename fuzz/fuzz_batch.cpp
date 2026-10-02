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

// fuzz_batch — the `megascope batch` request loop (runBatch): the input is
// the NDJSON request stream on stdin, answered against a small index of
// examples/deep_chains loaded the way the query verbs load one
// (ReadOnly, mapped control-flow section).
//
// The index is baked once at startup from the fixture (a few seconds), or
// read from $VYCOR_FUZZ_INDEX when set. Beyond "no crash", every input is
// held to the batch contract: one response line per non-blank request
// line, each a JSON object carrying `status` and `exit`.

#include "FuzzCommon.h"

#include "vycor/callgraph/ControlFlowOracle.h"
#include "vycor/cli/MegascopeCli.h"
#include "vycor/query/Tools.h"

#include "clang/Tooling/CompilationDatabase.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

using namespace vycor;

namespace {

SnapshotData *gSnap = nullptr;

/// Bake a few deep_chains TUs (callers, callbacks, virtual dispatch, a
/// try/catch) and save them where the loader can map them.
std::string bakeFixtureIndex() {
  const std::string fixture =
      std::string(PROJECT_SOURCE_DIR) + "/examples/deep_chains/";
  std::vector<std::string> files;
  for (const char *tu : {"main.cpp", "pipeline.cpp", "stage5_sink.cpp",
                         "plugins.cpp", "callbacks.cpp"})
    files.push_back(fixture + tu);
  clang::tooling::FixedCompilationDatabase db(
      fixture, {"-std=c++17", "-I" + fixture});
  BakedIndexes baked = bakeIndexes(db, files, {}, /*threadCount=*/1);
  SnapshotMeta meta;
  meta.files = SnapshotIO::stampFiles(files);
  SnapshotIO::recordOutcomes(meta, baked.outcomes);
  const std::string path = fuzz::scratchPath("fixture.vycs");
  std::string error;
  if (!SnapshotIO::save(path, baked.graph, baked.cfIndex, meta,
                        baked.channels, &error, /*durable=*/false)) {
    std::fprintf(stderr, "fuzz_batch: cannot save the fixture index: %s\n",
                 error.c_str());
    std::abort();
  }
  return path;
}

size_t requestLines(const std::string &input) {
  std::istringstream in(input);
  std::string line;
  size_t n = 0;
  while (std::getline(in, line))
    if (!llvm::StringRef(line).trim().empty())
      ++n;
  return n;
}

} // namespace

extern "C" int LLVMFuzzerInitialize(int *, char ***) {
  const char *env = std::getenv("VYCOR_FUZZ_INDEX");
  const std::string path = env && *env ? std::string(env) : bakeFixtureIndex();
  SnapshotLoadStats stats;
  auto loaded = SnapshotIO::load(path, &stats, LoadMode::ReadOnly);
  if (!loaded) {
    std::fprintf(stderr, "fuzz_batch: cannot load %s: %s\n", path.c_str(),
                 stats.error.c_str());
    std::abort();
  }
  // Process lifetime, like the query verbs' index.
  gSnap = new SnapshotData(std::move(*loaded));
  return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  static const std::vector<ToolEntry> tools = getRegisteredTools();
  static const ControlFlowOracle oracle(gSnap->graph, gSnap->cfIndex);
  static const std::vector<std::string> entryPoints = {"main"};
  QueryCache cache;
  ToolContext ctx{gSnap->graph, oracle,          gSnap->cfIndex,
                  entryPoints,  &gSnap->channels, &cache,
                  &gSnap->summary};
  ctx.facts = IndexFacts::of(gSnap->meta, IndexFreshness::Unchecked);

  const std::string input(reinterpret_cast<const char *>(data), size);
  std::istringstream in(input);
  std::string answers;
  llvm::raw_string_ostream out(answers);
  runBatch(tools, ctx, in, out);
  out.flush();

  // The contract: one well-formed response per request line.
  size_t responses = 0;
  llvm::StringRef rest(answers);
  while (!rest.empty()) {
    auto [line, tail] = rest.split('\n');
    rest = tail;
    ++responses;
    auto parsed = llvm::json::parse(line);
    const llvm::json::Object *obj = parsed ? parsed->getAsObject() : nullptr;
    if (!obj || !obj->getString("status") || !obj->getInteger("exit")) {
      std::fprintf(stderr, "fuzz_batch: malformed response line: %.*s\n",
                   static_cast<int>(line.size()), line.data());
      if (!parsed)
        llvm::consumeError(parsed.takeError());
      std::abort();
    }
  }
  if (responses != requestLines(input)) {
    std::fprintf(stderr, "fuzz_batch: %zu responses for %zu requests\n",
                 responses, requestLines(input));
    std::abort();
  }
  return 0;
}
