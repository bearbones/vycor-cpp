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

// fuzz_shard — the files anneal --isolate-workers passes between
// processes: phase-1 index shards (readAnnealIndexShard), phase-2
// diagnostics shards (readAnnealDiagShard), and the merged-index handoff
// (readGlobalIndexFile). (megascope's bake shards are index files: see
// fuzz_snapshot, whose Mutable leg is the shard merge.)
//
// Input: one flags byte, then the file image. The reader is chosen by the
// image's magic, or by flags bits 0-1 when the magic is none of the
// three. Flags bit 2 reseals every whole entry's checksum first, so
// mutations reach the payload decoders. What a reader accepts is used the
// way the parent uses it: index payloads applied to a GlobalIndex and the
// index-only checks run over it, diagnostics collected, and both written
// back out through the shard writers.

#include "FuzzCommon.h"

#include "vycor/anneal/Analyzer.h"
#include "vycor/anneal/Checkpoint.h"
#include "vycor/anneal/GlobalIndex.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace vycor;

namespace {

uint32_t ld32(const std::string &b, size_t at) {
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i)
    v |= static_cast<uint32_t>(static_cast<uint8_t>(b[at + i])) << (8 * i);
  return v;
}

void st32(std::string &b, size_t at, uint32_t v) {
  for (int i = 0; i < 4; ++i)
    b[at + i] = static_cast<char>((v >> (8 * i)) & 0xff);
}

/// Framing: magic(4) version u32 count u32, then per entry: tu (u32 len +
/// bytes), payloadLen u32, payload, fnv32(payload) u32.
void reseal(std::string &image) {
  size_t pos = 12;
  while (image.size() >= pos && image.size() - pos >= 4) {
    const uint32_t tuLen = ld32(image, pos);
    if (image.size() - pos - 4 < static_cast<size_t>(tuLen) + 4)
      return;
    pos += 4 + tuLen;
    const uint32_t len = ld32(image, pos);
    const size_t payload = pos + 4;
    if (image.size() - payload < static_cast<size_t>(len) + 4)
      return;
    st32(image, payload + len,
         annealRecordChecksum(image.data() + payload, len));
    pos = payload + len + 4;
  }
}

void runIndexChecks(const GlobalIndex &index, std::vector<Diagnostic> &diags) {
  analyzeCoverageProperties(index, diags);
  analyzeOdrViolations(index, diags);
  analyzeDefaultArgDivergence(index, diags);
  analyzeStaticInitOrder(index, diags);
  analyzeExceptionEscape(index, diags);
  analyzeExceptionSpecDivergence(index, diags);
  analyzeHeaderStaticDuplication(index, diags);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size < 1)
    return 0;
  std::string image(reinterpret_cast<const char *>(data + 1), size - 1);
  if (data[0] & 4)
    reseal(image);
  int which = data[0] & 3;
  if (image.size() >= 4) {
    if (std::memcmp(image.data(), "VYAI", 4) == 0)
      which = 0;
    else if (std::memcmp(image.data(), "VYAD", 4) == 0)
      which = 1;
    else if (std::memcmp(image.data(), "VYGI", 4) == 0)
      which = 2;
  }
  const std::string path = fuzz::scratchPath("shard.bin");
  const std::string rewritten = fuzz::scratchPath("shard.out");
  fuzz::writeScratch(path, image);

  GlobalIndex index;
  std::vector<Diagnostic> diags;
  switch (which) {
  case 0: {
    std::vector<std::pair<std::string, AnnealIndexPayload>> tus;
    if (readAnnealIndexShard(path, [&](const std::string &tu,
                                       const AnnealIndexPayload &payload) {
          payload.applyTo(index);
          tus.emplace_back(tu, payload);
        }))
      (void)writeAnnealIndexShard(rewritten, tus);
    runIndexChecks(index, diags);
    break;
  }
  case 1: {
    std::vector<std::pair<std::string, std::vector<Diagnostic>>> tus;
    if (readAnnealDiagShard(path, [&](const std::string &tu,
                                      std::vector<Diagnostic> got) {
          diags.insert(diags.end(), got.begin(), got.end());
          tus.emplace_back(tu, std::move(got));
        }))
      (void)writeAnnealDiagShard(rewritten, tus);
    break;
  }
  default:
    if (readGlobalIndexFile(path, index))
      (void)writeGlobalIndexFile(rewritten, index);
    runIndexChecks(index, diags);
    break;
  }
  // A shard's diagnostics are read like this run's own.
  size_t kinds = 0;
  for (const auto &d : diags)
    kinds += static_cast<size_t>(d.kind) + d.message.size();
  (void)kinds;
  return 0;
}
