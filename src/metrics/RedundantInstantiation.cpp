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
#include "vycor/metrics/MetricSet.h"

#include "vycor/callgraph/ReferenceIndex.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Demangle/Demangle.h"

#include <algorithm>
#include <tuple>

// redundant-instantiation (docs/metrics/redundant-instantiation.md): the
// same function body compiled by many TUs. One sample per vague-linkage
// function (an implicit instantiation, an inline function, an implicit
// member) or header-defined internal function that two or more TUs emit,
// read from the bake's emission facts; the value is the number of TUs
// that emit it times its size in statements.

namespace vycor {

namespace {

using Id = ReferenceIndex::Id;
constexpr Id kNoId = ReferenceIndex::kNoId;

class RedundantInstantiationRun final : public MetricRun {
public:
  explicit RedundantInstantiationRun(const ReferenceIndex &refs) {
    const auto &text = [&](Id id) -> const std::string & {
      return refs.textOf(id);
    };

    // One accumulator per linkage name: the copies the linker merges.
    // Records of one name differ only where TUs saw different definitions
    // (an ODR violation, or a macro); the largest is kept, then the
    // smallest location and symbol, so the result does not depend on
    // record order. An internal function's name is not unique (two
    // headers' `static helper()` mangle alike, and C names are not
    // mangled at all): its copies are one function only at one
    // definition, so its key adds the file and line.
    struct Acc {
      Id name;
      Id symbol;
      Id file;
      uint32_t line;
      uint32_t size;
      uint8_t flags;
    };
    std::vector<Acc> accs;
    static const std::string kNone;
    const auto &symbolText = [&](Id id) -> const std::string & {
      return id == kNoId ? kNone : text(id);
    };
    // (name, (file + 1, line)) for an internal function, (name, (0, 0))
    // for the rest.
    llvm::DenseMap<std::pair<Id, std::pair<Id, uint32_t>>, uint32_t> accOf;
    std::vector<std::pair<uint32_t, Id>> copies; // (acc, tu)
    std::vector<size_t> unnamed; // samples whose pattern has no record
    refs.forEachEmissionRecord([&](const ReferenceIndex::EmissionRecord &rec,
                                   llvm::ArrayRef<Id> tus) {
      // Kept for inlining only: no copy of its own.
      if (rec.flags & kEmitAvailableExternally)
        return;
      const bool internal = (rec.flags & kEmitInternal) != 0;
      const std::pair<Id, std::pair<Id, uint32_t>> key{
          rec.name, internal ? std::pair<Id, uint32_t>{rec.file + 1, rec.line}
                             : std::pair<Id, uint32_t>{0, 0}};
      auto [it, inserted] =
          accOf.try_emplace(key, static_cast<uint32_t>(accs.size()));
      if (inserted) {
        accs.push_back(
            {rec.name, rec.symbol, rec.file, rec.line, rec.size, rec.flags});
      } else {
        Acc &acc = accs[it->second];
        const std::string &recFile = text(rec.file);
        const std::string &accFile = text(acc.file);
        const std::string &recSymbol = symbolText(rec.symbol);
        const std::string &accSymbol = symbolText(acc.symbol);
        // A record that names its symbol wins over one that does not.
        const bool recNone = rec.symbol == kNoId;
        const bool accNone = acc.symbol == kNoId;
        if (std::tie(acc.size, recFile, rec.line, recNone, recSymbol) <
            std::tie(rec.size, accFile, acc.line, accNone, accSymbol)) {
          acc.size = rec.size;
          acc.file = rec.file;
          acc.line = rec.line;
          acc.symbol = rec.symbol;
        }
        acc.flags |= rec.flags;
      }
      for (Id tu : tus)
        copies.emplace_back(it->second, tu);
    });
    std::sort(copies.begin(), copies.end());
    copies.erase(std::unique(copies.begin(), copies.end()), copies.end());

    // Where each symbol is defined, and its name: the template roll-up's
    // group.
    struct Def {
      Id name;
      Id file;
      uint32_t line;
    };
    llvm::DenseMap<Id, Def> defOf;
    refs.forEachSymbolRecord(
        [&](const ReferenceIndex::SymbolRecord &rec, uint32_t, Id) {
          if (rec.kind != SymbolKind::Function ||
              !(rec.flags & kSymDefinition))
            return;
          auto [it, inserted] =
              defOf.try_emplace(rec.usr, Def{rec.name, rec.file, rec.line});
          if (inserted)
            return;
          const Def &had = it->second;
          if (std::tie(text(rec.file), rec.line, text(rec.name)) <
              std::tie(text(had.file), had.line, text(had.name)))
            it->second = Def{rec.name, rec.file, rec.line};
        });

    // TU ids: one table of paths, indexed by the samples.
    llvm::DenseMap<Id, uint32_t> tuIndex;
    for (size_t at = 0; at < copies.size();) {
      size_t end = at;
      while (end < copies.size() && copies[end].first == copies[at].first)
        ++end;
      const size_t count = end - at;
      if (count < 2) {
        at = end;
        continue; // one copy is no redundancy
      }
      const Acc &acc = accs[copies[at].first];
      MetricSample sample;
      sample.entity.kind = "function";
      sample.entity.usr = text(acc.name);
      sample.entity.name = llvm::demangle(sample.entity.usr);
      sample.entity.file = text(acc.file);
      sample.entity.line = acc.line;
      sample.value = static_cast<int64_t>(count) * acc.size;
      sample.components.emplace_back("copies", static_cast<int64_t>(count));
      sample.components.emplace_back("statements",
                                     static_cast<int64_t>(acc.size));
      sample.components.emplace_back(
          "internal", (acc.flags & kEmitInternal) ? 1 : 0);
      if (acc.symbol != kNoId) {
        sample.pattern.kind = "template";
        sample.pattern.usr = text(acc.symbol);
        auto def = defOf.find(acc.symbol);
        if (def != defOf.end()) {
          sample.pattern.name = text(def->second.name);
          sample.pattern.file = text(def->second.file);
          sample.pattern.line = def->second.line;
        } else {
          // No definition record for the pattern (only system code
          // instantiates it): named after an instantiation, the same one
          // for every sample of the pattern (below).
          sample.pattern.name = sample.entity.name;
          sample.pattern.file = sample.entity.file;
          sample.pattern.line = sample.entity.line;
          unnamed.push_back(samples_.size());
        }
      }
      sample.tuIds.reserve(count);
      for (size_t i = at; i < end; ++i) {
        auto [it, inserted] = tuIndex.try_emplace(
            copies[i].second, static_cast<uint32_t>(tuNames_.size()));
        if (inserted)
          tuNames_.push_back(text(copies[i].second));
        sample.tuIds.push_back(it->second);
      }
      samples_.push_back(std::move(sample));
      at = end;
    }

    // A pattern without a definition record takes the smallest
    // instantiation's location and name, whatever order the records came
    // in.
    llvm::StringMap<size_t> least;
    const auto key = [&](size_t i) {
      const MetricEntity &p = samples_[i].pattern;
      return std::tie(p.file, p.line, p.name);
    };
    for (size_t i : unnamed) {
      auto [it, inserted] = least.try_emplace(samples_[i].pattern.usr, i);
      if (!inserted && key(i) < key(it->second))
        it->second = i;
    }
    for (size_t i : unnamed) {
      const MetricEntity from = samples_[least[samples_[i].pattern.usr]].pattern;
      samples_[i].pattern = from;
    }
  }
};

class RedundantInstantiationMetric final : public Metric {
public:
  std::string name() const override { return "redundant-instantiation"; }
  std::string summary() const override {
    return "Function bodies compiled by many TUs: inline functions, "
           "template instantiations and header-defined internal functions "
           "emitted by two or more TUs, as copies times statements";
  }
  std::string unit() const override { return "statements"; }

  std::string unavailable(const MetricContext &ctx) const override {
    if (ctx.index.references && ctx.index.references->hasEmissionFacts())
      return {};
    return "the index holds no emission facts (it was built without "
           "reference facts): bake it with megascope index "
           "(docs/metrics/redundant-instantiation.md)";
  }

  std::unique_ptr<MetricRun>
  measure(const MetricContext &ctx) const override {
    return std::make_unique<RedundantInstantiationRun>(
        *ctx.index.references);
  }
};

} // namespace

std::unique_ptr<Metric> makeRedundantInstantiationMetric() {
  return std::make_unique<RedundantInstantiationMetric>();
}

} // namespace vycor
