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

#include "vycor/check/Analyzer.h"

#include <iterator>
#include <map>
#include <sstream>
#include <tuple>

namespace vycor {
namespace {
std::string site(const AbiLayoutEntry &entry) {
  return entry.filePath + ":" + std::to_string(entry.line) + " (TU " +
         entry.tuPath + ")";
}

std::string firstDifference(const std::string &a, const std::string &b) {
  std::istringstream left(a), right(b);
  std::string x, y;
  while (std::getline(left, x, ';') && std::getline(right, y, ';'))
    if (x != y)
      return x + " versus " + y;
  return "different physical layout facts";
}
} // namespace

void analyzeAbiLayoutDivergence(const GlobalIndex &index,
                                std::vector<Diagnostic> &diagnostics) {
  // Target ABI + USR -> layout -> TU -> canonical definition site. Compare
  // distinct variants, not all pairs of sightings of common header types.
  using ByTu = std::map<std::string, const AbiLayoutEntry *>;
  std::map<std::pair<std::string, std::string>, std::map<std::string, ByTu>>
      groups;
  index.forEachAbiLayout([&](const AbiLayoutEntry &entry) {
    if (entry.usr.empty() || entry.tuPath.empty() || entry.layout.empty())
      return;
    auto &selected =
        groups[{entry.targetAbi, entry.usr}][entry.layout][entry.tuPath];
    if (!selected || std::tie(entry.filePath, entry.line) <
                         std::tie(selected->filePath, selected->line))
      selected = &entry;
  });
  for (const auto &[identity, variants] : groups) {
    if (variants.size() < 2)
      continue;
    const auto &baseline = variants.begin()->second;
    for (auto variant = std::next(variants.begin()); variant != variants.end();
         ++variant) {
      const auto *a = baseline.begin()->second;
      const auto *b = variant->second.begin()->second;
      if (a->tuPath == b->tuPath) {
        if (baseline.size() > 1)
          a = std::next(baseline.begin())->second;
        else if (variant->second.size() > 1)
          b = std::next(variant->second.begin())->second;
        else
          continue; // no cross-TU witness
      }
      Diagnostic diag;
      diag.kind = Diagnostic::AbiLayout_Divergent;
      diag.file = a->filePath;
      diag.entities = {a->usr, a->targetAbi};
      diag.callLocation = a->filePath + ":" + std::to_string(a->line);
      diag.resolvedDecl = a->usr;
      diag.betterDecl = b->usr;
      diag.checkName = "abi-layout-divergence";
      diag.message =
          "ABI layout divergence: '" + a->qualifiedName +
          "' has incompatible computed record layouts for target '" +
          identity.first.substr(0, identity.first.find('|')) +
          "': " + site(*a) + " versus " + site(*b) + "; " +
          firstDifference(a->layout, b->layout) + ". " +
          "Linked TUs must agree on packing, alignment and record layout.";
      diagnostics.push_back(std::move(diag));
      break; // one bounded, deterministic witness per entity / target ABI
    }
  }
}
} // namespace vycor
