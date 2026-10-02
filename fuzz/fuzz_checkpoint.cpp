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

// fuzz_checkpoint — anneal's --checkpoint journal: open (header check,
// record load, damaged-tail truncation) and replay.
//
// Input: one flags byte, then the journal file image. The journal header's
// options fingerprint is taken from the image itself, so a mutated header
// is not simply discarded as "different options". Flags bit 0 reseals
// every whole record's checksum before the file is written, so mutations
// reach the record decoders instead of stopping at the checksum. Every
// TU a record names is then replayed the way runAnalysis does on resume
// (attempt counts, phase-1 contribution into a GlobalIndex, phase-2
// diagnostics), the index-only checks run over the replayed index, and the
// journal is appended to once (open() truncated any damaged tail).

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

constexpr size_t kHeaderSize = 16; // magic(4) version(4) fingerprint(8)

uint32_t ld32(const std::string &b, size_t at) {
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i)
    v |= static_cast<uint32_t>(static_cast<uint8_t>(b[at + i])) << (8 * i);
  return v;
}

uint64_t ld64(const std::string &b, size_t at) {
  return static_cast<uint64_t>(ld32(b, at)) |
         (static_cast<uint64_t>(ld32(b, at + 4)) << 32);
}

void st32(std::string &b, size_t at, uint32_t v) {
  for (int i = 0; i < 4; ++i)
    b[at + i] = static_cast<char>((v >> (8 * i)) & 0xff);
}

/// What a record names: enough to ask the checkpoint about it.
struct Named {
  uint8_t kind = 0;
  uint8_t phase = 0;
  FileStamp stamp;
  uint64_t indexSetHash = 0;
};

/// Walk the record framing (kind u8, len u32, payload, fnv32 u32),
/// resealing each whole record when asked, and collect the TU and stamp
/// each payload starts with.
std::vector<Named> walkRecords(std::string &image, bool reseal) {
  std::vector<Named> out;
  size_t pos = kHeaderSize;
  while (image.size() - pos >= 5) {
    const uint8_t kind = static_cast<uint8_t>(image[pos]);
    const uint32_t len = ld32(image, pos + 1);
    const size_t payload = pos + 5;
    if (image.size() - payload < static_cast<size_t>(len) + 4)
      break;
    if (reseal)
      st32(image, payload + len,
           annealRecordChecksum(image.data() + payload, len));
    // attempt: phase u8, tu str, mtime u64, size u64
    // phase 1: tu str, mtime u64, size u64, ...
    // phase 2: tu str, mtime u64, size u64, indexSetHash u64, ...
    Named n;
    n.kind = kind;
    size_t at = payload, end = payload + len;
    if (kind == 1 && at < end)
      n.phase = static_cast<uint8_t>(image[at++]);
    if (end - at >= 4) {
      const uint32_t tuLen = ld32(image, at);
      at += 4;
      if (end - at >= tuLen) {
        n.stamp.path = image.substr(at, tuLen);
        at += tuLen;
        if (end - at >= 16) {
          n.stamp.mtimeNs = ld64(image, at);
          n.stamp.size = ld64(image, at + 8);
          at += 16;
          if (end - at >= 8)
            n.indexSetHash = ld64(image, at);
        }
        out.push_back(std::move(n));
      }
    }
    pos = payload + len + 4;
  }
  return out;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size < 1)
    return 0;
  std::string image(reinterpret_cast<const char *>(data + 1), size - 1);
  uint64_t fingerprint = 0;
  std::vector<Named> named;
  if (image.size() >= kHeaderSize) {
    fingerprint = ld64(image, 8);
    named = walkRecords(image, (data[0] & 1) != 0);
  }
  const std::string path = fuzz::scratchPath("journal.ckpt");
  fuzz::writeScratch(path, image);

  auto ckpt = AnnealCheckpoint::open(path, fingerprint);
  if (!ckpt)
    return 0;
  GlobalIndex index;
  std::vector<Diagnostic> diags;
  for (const auto &n : named) {
    (void)ckpt->attempts(AnnealCheckpoint::kPhaseIndex, n.stamp.path, n.stamp);
    (void)ckpt->attempts(AnnealCheckpoint::kPhaseAnalyze, n.stamp.path,
                         n.stamp);
    (void)ckpt->replayPhase1(n.stamp.path, n.stamp, index);
    (void)ckpt->replayPhase2(n.stamp.path, n.stamp, n.indexSetHash, diags);
  }
  // Phase 1.5: the index-only checks over what was replayed.
  analyzeCoverageProperties(index, diags);
  analyzeOdrViolations(index, diags);
  analyzeDefaultArgDivergence(index, diags);
  analyzeStaticInitOrder(index, diags);
  analyzeExceptionEscape(index, diags);
  analyzeExceptionSpecDivergence(index, diags);
  analyzeHeaderStaticDuplication(index, diags);
  // A replayed diagnostic is read like one produced by this run.
  size_t kinds = 0;
  for (const auto &d : diags)
    kinds += static_cast<size_t>(d.kind) + d.message.size();
  (void)kinds;
  // Appending after a load: lands behind the last valid record.
  FileStamp stamp;
  stamp.path = "fuzz.cpp";
  ckpt->recordAttempt(AnnealCheckpoint::kPhaseIndex, stamp.path, stamp);
  ckpt.reset();
  (void)AnnealCheckpoint::open(path, fingerprint);
  return 0;
}
