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

#include "NoexceptGraph.h"

#include <algorithm>
#include <tuple>

namespace vycor {

using namespace noexcept_graph;

bool NoexceptGraph::build(const GlobalIndex &index) {
  const size_t factCount = index.noexceptFactCount();
  if (factCount == 0)
    return false;

  facts.reserve(factCount);
  size_t edgeTotal = 0;
  index.forEachNoexceptFact([&](const NoexceptFactEntry &f) {
    facts.push_back(&f);
    edgeTotal += f.callees.size();
  });
  std::sort(facts.begin(), facts.end(),
            [](const NoexceptFactEntry *a, const NoexceptFactEntry *b) {
              return std::tie(a->usr, a->filePath, a->line, a->column) <
                     std::tie(b->usr, b->filePath, b->line, b->column);
            });

  ids.reserve(factCount * 2);
  nodeOfFact.assign(factCount, 0);
  firstFactOfNode.reserve(factCount);
  keyOfNode.reserve(factCount * 2);
  for (size_t i = 0; i < factCount; ++i) {
    if (i > 0 && facts[i]->usr == facts[i - 1]->usr) {
      nodeOfFact[i] = nodeOfFact[i - 1];
      continue;
    }
    nodeOfFact[i] = static_cast<uint32_t>(firstFactOfNode.size());
    ids.emplace(facts[i]->usr, nodeOfFact[i]);
    keyOfNode.push_back(facts[i]->usr);
    firstFactOfNode.push_back(static_cast<uint32_t>(i));
  }
  definedCount = static_cast<uint32_t>(firstFactOfNode.size());

  edges.reserve(edgeTotal);
  for (size_t i = 0; i < factCount; ++i) {
    const NoexceptFactEntry &f = *facts[i];
    for (size_t c = 0; c < f.callees.size(); ++c) {
      auto next = static_cast<uint32_t>(ids.size());
      auto [it, added] = ids.try_emplace(f.callees[c], next);
      if (added)
        keyOfNode.push_back(it->first);
      uint8_t bits = c < f.calleeBits.size()
                         ? f.calleeBits[c] & NoexceptFactEntry::AllCalleeBits
                         : NoexceptFactEntry::AllCalleeBits;
      edges.push_back(packEdge(it->second, nodeOfFact[i], bits));
    }
  }
  nodeCount = ids.size();
  if (nodeCount > kIdMask)
    return false;

  // Group by callee; fold the sightings of one (callee, caller) pair.
  std::sort(edges.begin(), edges.end());
  size_t out = 0;
  for (size_t i = 0; i < edges.size(); ++i) {
    if (out > 0 && (edges[out - 1] >> kBitsWidth) == (edges[i] >> kBitsWidth))
      edges[out - 1] |= edges[i];
    else
      edges[out++] = edges[i];
  }
  edges.resize(out);
  callersBegin.assign(nodeCount + 1, 0);
  for (uint64_t e : edges)
    ++callersBegin[edgeCallee(e) + 1];
  for (size_t i = 0; i < nodeCount; ++i)
    callersBegin[i + 1] += callersBegin[i];

  flags.assign(nodeCount, 0);
  for (size_t i = 0; i < factCount; ++i)
    flags[nodeOfFact[i]] |= facts[i]->flags;
  return true;
}

} // namespace vycor
