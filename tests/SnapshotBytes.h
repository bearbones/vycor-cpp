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

// SnapshotBytes.h — test helper for tests that patch a saved index's bytes
// to exercise a decoder's own validation: since v13 every section and the
// header carry a checksum, so a patched file is refused before any decoder
// sees it unless the checksums are recomputed first.

#pragma once

#include "vycor/callgraph/Snapshot.h"

#include "llvm/Support/xxhash.h"

#include <cstdint>
#include <string>

namespace vycor {
namespace testing {

inline uint64_t snapshotU64At(const std::string &bytes, size_t at) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i)
    v |= static_cast<uint64_t>(static_cast<uint8_t>(bytes[at + i])) << (8 * i);
  return v;
}

inline void snapshotPutU64At(std::string &bytes, size_t at, uint64_t v) {
  for (int i = 0; i < 8; ++i)
    bytes[at + i] = static_cast<char>((v >> (8 * i)) & 0xff);
}

inline uint64_t snapshotChecksum(const std::string &bytes, size_t at,
                                 size_t n) {
  return llvm::xxh3_64bits(llvm::ArrayRef<uint8_t>(
      reinterpret_cast<const uint8_t *>(bytes.data() + at), n));
}

/// Recompute the section checksums (for every section that still lies
/// inside `bytes`) and the header checksum of a v13 index image. Layout:
/// magic(4) version(4) summary(32) count(4), then per section {kind u8,
/// offset u64, length u64, checksum u64}, then the header checksum u64.
inline void resealSnapshot(std::string &bytes) {
  if (bytes.size() < SnapshotIO::kHeaderBytes)
    return;
  for (size_t i = 0; i < 4; ++i) {
    const size_t at = 44 + i * 25;
    const uint64_t offset = snapshotU64At(bytes, at + 1);
    const uint64_t length = snapshotU64At(bytes, at + 9);
    if (offset <= bytes.size() && length <= bytes.size() - offset)
      snapshotPutU64At(bytes, at + 17,
                       snapshotChecksum(bytes, offset, length));
  }
  const size_t covered = SnapshotIO::kHeaderBytes - 8;
  snapshotPutU64At(bytes, covered, snapshotChecksum(bytes, 0, covered));
}

} // namespace testing
} // namespace vycor
