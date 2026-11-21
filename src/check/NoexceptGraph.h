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

// Internal to src/check: the merged noexcept facts as one graph on flat
// arrays. noexcept-candidate's proof and exception-boundary-escape's
// witness search both read it.

#ifndef VYCOR_SRC_CHECK_NOEXCEPT_GRAPH_H
#define VYCOR_SRC_CHECK_NOEXCEPT_GRAPH_H

#include "vycor/check/GlobalIndex.h"

#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace vycor {
namespace noexcept_graph {

// Edges are packed (callee, caller, bits) so that one sort groups them by
// callee, which is the direction the proof reads.
constexpr unsigned kBitsWidth = 4;
constexpr unsigned kIdWidth = 30;
static_assert(NoexceptFactEntry::AllCalleeBits < (1u << kBitsWidth),
              "a callee bit does not fit the packed edge");
static_assert(2 * kIdWidth + kBitsWidth <= 64, "a packed edge is 64 bits");
constexpr uint64_t kIdMask = (uint64_t{1} << kIdWidth) - 1;
constexpr uint64_t packEdge(uint32_t callee, uint32_t caller, uint8_t bits) {
  return (uint64_t{callee} << (kIdWidth + kBitsWidth)) |
         (uint64_t{caller} << kBitsWidth) | bits;
}
constexpr uint32_t edgeCaller(uint64_t e) {
  return static_cast<uint32_t>((e >> kBitsWidth) & kIdMask);
}
constexpr uint32_t edgeCallee(uint64_t e) {
  return static_cast<uint32_t>(e >> (kIdWidth + kBitsWidth));
}
constexpr uint8_t edgeBits(uint64_t e) {
  return static_cast<uint8_t>(e & ((1u << kBitsWidth) - 1));
}

} // namespace noexcept_graph

struct NoexceptGraph {
  // Facts arrive in whatever order the TUs finished. Everything derived
  // here (node ids, edge order) comes from this sorted view, so it is a
  // function of the facts alone.
  std::vector<const NoexceptFactEntry *> facts;
  // One node per key. Definition sites sharing a key (a function defined
  // differently in two binaries of one build, two overloads the key does
  // not tell apart) are one node. Nodes [0, definedCount) have facts, in
  // key order; the rest are callees nothing defines, in first-seen order.
  std::vector<uint32_t> nodeOfFact;
  std::vector<uint32_t> firstFactOfNode;
  std::vector<std::string_view> keyOfNode;
  std::unordered_map<std::string_view, uint32_t> ids;
  uint32_t definedCount = 0;
  size_t nodeCount = 0;
  // Packed edges sorted by (callee, caller), the sightings of one pair
  // folded; callersBegin is their CSR offset table by callee.
  std::vector<uint64_t> edges;
  std::vector<uint32_t> callersBegin;
  // OR of every sighting's flags, per node.
  std::vector<uint32_t> flags;

  // False when there is nothing to build, or more nodes than an edge can
  // address (then nothing is reported rather than a truncated graph).
  bool build(const GlobalIndex &index);
};

} // namespace vycor

#endif // VYCOR_SRC_CHECK_NOEXCEPT_GRAPH_H
