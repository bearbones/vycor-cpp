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

// fuzz_snapshot — the megascope index loader (SnapshotIO::load), both modes.
//
// Input: one flags byte, then the index file image. Flags bit 0 reseals
// the image (recomputes the v13 section and header checksums) before it
// is written, so mutations reach the section decoders instead of stopping
// at the checksum; with the bit clear the checksum checks themselves are
// fuzzed. The image is then:
//   - loaded Mutable and put through what `megascope index` and the
//     isolated bake's shard merge do with a loaded index: absorbed into
//     fresh indexes, its TUs removed, saved, and reloaded;
//   - loaded ReadOnly (the query verbs' load, with the mapped
//     control-flow section) and every registered tool run against it,
//     plus the `dump` walk over every context and channel site.

#include "FuzzCommon.h"

#include "../tests/SnapshotBytes.h"

#include "vycor/callgraph/ChannelIndex.h"
#include "vycor/callgraph/ControlFlowOracle.h"
#include "vycor/query/Serialize.h"
#include "vycor/query/Tools.h"

#include <cstdint>
#include <string>

using namespace vycor;

namespace {

void exerciseMutable(const std::string &path) {
  auto snap = SnapshotIO::load(path, nullptr, LoadMode::Mutable);
  if (!snap)
    return;
  // The shard merge of an isolated bake (WorkerPool.cpp).
  CallGraph graph;
  ControlFlowIndex cf;
  ChannelIndex channels;
  graph.absorb(snap->graph);
  cf.absorb(snap->cfIndex);
  channels.absorb(snap->channels);
  // A warm refresh: drop the recorded TUs, then save what is left.
  std::vector<std::string> tus;
  for (const auto &f : snap->meta.files)
    tus.push_back(f.path);
  snap->graph.removeTUs(tus);
  snap->cfIndex.removeTUs(tus);
  snap->channels.removeTUs(tus);
  (void)SnapshotIO::dirtyTUs(snap->meta, snap->meta.files);
  (void)coverageOf(snap->meta);
  const std::string saved = fuzz::scratchPath("resaved.vycs");
  if (SnapshotIO::save(saved, graph, cf, snap->meta, channels, nullptr,
                       /*durable=*/false))
    (void)SnapshotIO::load(saved, nullptr, LoadMode::ReadOnly);
}

void exerciseReadOnly(const std::string &path) {
  auto snap = SnapshotIO::load(path, nullptr, LoadMode::ReadOnly);
  if (!snap)
    return;
  ControlFlowOracle oracle(snap->graph, snap->cfIndex);
  QueryCache cache;
  std::vector<std::string> entryPoints = snap->meta.entryPoints;
  if (entryPoints.empty())
    entryPoints.push_back("main");
  ToolContext ctx{snap->graph,  oracle,          snap->cfIndex,
                  entryPoints, &snap->channels, &cache,
                  &snap->summary};
  ctx.facts = IndexFacts::of(snap->meta, IndexFreshness::Unchecked);
  const auto pool =
      fuzz::ArgPool::from(snap->graph, snap->cfIndex, &snap->channels);
  static const std::vector<ToolEntry> tools = getRegisteredTools();
  for (size_t i = 0; i < tools.size(); ++i)
    (void)runTool(tools[i], pool.argsFor(tools[i], i), ctx);
  // `megascope dump` (MegascopeCli.cpp dumpRecord): every context and
  // channel site through the shared serializers.
  snap->cfIndex.forEachContext([&](const CallSiteContext &c) {
    llvm::json::Array rec;
    rec.push_back(c.callerName);
    rec.push_back(c.calleeUsr);
    rec.push_back(c.callSite);
    rec.push_back(c.tuPath);
    rec.push_back(noexceptSpecToString(c.callerNoexcept));
    for (const auto &scope : c.enclosingTryCatches)
      rec.push_back(serializeTryCatchScope(scope));
    for (const auto &g : c.enclosingGuards)
      rec.push_back(serializeGuard(g));
    for (const auto &l : c.liveRaiiLocals)
      rec.push_back(serializeRaiiLocal(l));
  });
  for (const auto &site : snap->channels.allSites())
    (void)serializeChannelSite(site);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size < 1)
    return 0;
  std::string image(reinterpret_cast<const char *>(data + 1), size - 1);
  if (data[0] & 1)
    testing::resealSnapshot(image);
  const std::string path = fuzz::scratchPath("input.vycs");
  fuzz::writeScratch(path, image);
  exerciseMutable(path);
  exerciseReadOnly(path);
  return 0;
}
