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

#include "vycor/callgraph/CallGraph.h"
#include "vycor/callgraph/StaticInit.h"

#include "llvm/ADT/StringMap.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <limits>

// static-init-cost (docs/metrics/static-init-cost.md): the work a program
// does before `main`. One sample per dynamic initializer and constructor
// function; the value is the number of distinct functions reachable from
// it through the call graph, and the components say whether that reach
// allocates, takes a lock or does I/O (the shared effect table).

namespace vycor {

namespace {

constexpr uint32_t kNone = std::numeric_limits<uint32_t>::max();
// Parent link of a constructor function's own node: where the walk starts,
// which is the measured entity rather than something it reaches.
constexpr uint32_t kSelf = kNone - 1;
constexpr uint8_t kEffectKnown = 0x80;
// The effects this metric reports: allocation, lock and I/O, the first
// three kinds. A throw is not start-up work.
constexpr unsigned kCostedKinds = 3;

class StaticInitCostRun final : public MetricRun {
public:
  StaticInitCostRun(const CallGraph &graph, const StaticInitFacts &facts,
                    const EffectTable &effects)
      : graph_(graph), effects_(effects),
        graphIds_(static_cast<uint32_t>(graph.interner().size())) {
    bindRoots(facts);
    const size_t nodes = graphIds_ + extraNames_.size();
    adjBegin_.assign(graphIds_, kNone);
    adjCount_.assign(graphIds_, 0);
    effect_.assign(nodes, 0);
    stamp_.assign(nodes, 0);
    parent_.assign(nodes, kNone);
    queue_.reserve(1024);
    measureRoots(facts);
  }

  // A group's reach is the union of its members' reaches, not their sum:
  // two initializers that both reach `log()` run it twice but reach one
  // function.
  void rollUp(llvm::ArrayRef<uint32_t> members, MetricRecord &out) override {
    Reach reach = walk(members);
    out.value = reach.functions;
    out.components.clear();
    int64_t variables = 0, constructors = 0, direct = 0;
    std::array<int64_t, kCostedKinds> withEffect{};
    for (uint32_t member : members) {
      const Root &root = roots_[member];
      (root.constructorFunction ? constructors : variables) += 1;
      direct += root.directCalls;
      for (unsigned k = 0; k < kCostedKinds; ++k)
        withEffect[k] += (root.effects >> k) & 1u;
    }
    out.components.emplace_back("dynamicInitializers", variables);
    out.components.emplace_back("constructorFunctions", constructors);
    out.components.emplace_back("directCalls", direct);
    for (unsigned k = 0; k < kCostedKinds; ++k)
      out.components.emplace_back(effectKindName(kEffectKinds[k]).str(),
                                  withEffect[k]);
  }

private:
  struct Root {
    uint32_t seedBegin = 0;
    uint32_t seedEnd = 0;
    // Graph id of a constructor function (its own node is the root);
    // kNone for a variable, whose roots are its seeds.
    uint32_t self = kNone;
    int64_t directCalls = 0;
    uint8_t effects = 0; // mask over its own reach, set by measureRoots
    bool constructorFunction = false;
  };

  struct Reach {
    int64_t functions = 0;
    // The first function of each effect kind in walk order (breadth
    // first, so a shallowest one), or kNone.
    std::array<uint32_t, kCostedKinds> first{kNone, kNone, kNone};
  };

  // Node ids: [0, graphIds_) are the graph's interned ids; past that,
  // callees the graph has no id for (a function only an initializer
  // calls), which are leaves.
  uint32_t nodeFor(const std::string &usr, const std::string &display) {
    if (auto id = graph_.interner().find(usr))
      if (*id < graphIds_)
        return *id;
    auto [it, inserted] = extraByUsr_.try_emplace(
        usr, static_cast<uint32_t>(graphIds_ + extraNames_.size()));
    if (inserted) {
      extraNames_.push_back(display);
      extraUsrs_.push_back(usr);
    }
    return it->second;
  }

  void bindRoots(const StaticInitFacts &facts) {
    roots_.reserve(facts.roots.size());
    for (const StaticInitRoot &fact : facts.roots) {
      Root root;
      root.seedBegin = static_cast<uint32_t>(seeds_.size());
      if (fact.kind == StaticInitKind::ConstructorFunction) {
        root.constructorFunction = true;
        if (auto id = graph_.interner().find(fact.usr))
          if (*id < graphIds_)
            root.self = *id;
      }
      for (const StaticInitCallee &callee : fact.callees)
        seeds_.push_back(nodeFor(callee.usr, callee.display));
      root.seedEnd = static_cast<uint32_t>(seeds_.size());
      roots_.push_back(root);
    }
  }

  const std::string &nameOf(uint32_t node) const {
    return node < graphIds_ ? graph_.displayNameOf(node)
                            : extraNames_[node - graphIds_];
  }

  const std::string &usrOf(uint32_t node) const {
    return node < graphIds_ ? graph_.interner().resolve(node)
                            : extraUsrs_[node - graphIds_];
  }

  uint8_t effectsOf(uint32_t node) {
    uint8_t &cached = effect_[node];
    if (!(cached & kEffectKnown))
      cached = static_cast<uint8_t>(
          kEffectKnown | effects_.effectsOf(nameOf(node), usrOf(node)));
    return cached & static_cast<uint8_t>(~kEffectKnown);
  }

  // Callees of a graph node, built once per node into a shared pool:
  // unique, ordered by display name then identity so the walk order (and
  // with it every witness) is the same for the same graph.
  void buildAdjacency(uint32_t node) {
    const size_t begin = adjPool_.size();
    for (const CallGraph::EdgeRef &edge : graph_.calleeRefsOf(node))
      if (edge.callee < graphIds_)
        adjPool_.push_back(edge.callee);
    auto first = adjPool_.begin() + static_cast<std::ptrdiff_t>(begin);
    std::sort(first, adjPool_.end(), [this](uint32_t a, uint32_t b) {
      const std::string &an = graph_.displayNameOf(a);
      const std::string &bn = graph_.displayNameOf(b);
      if (an != bn)
        return an < bn;
      return graph_.interner().resolve(a) < graph_.interner().resolve(b);
    });
    adjPool_.erase(std::unique(first, adjPool_.end()), adjPool_.end());
    adjBegin_[node] = static_cast<uint32_t>(begin);
    adjCount_[node] = static_cast<uint32_t>(adjPool_.size() - begin);
  }

  bool seen(uint32_t node) const { return stamp_[node] == generation_; }

  // Reach a function: count it, note its effects, queue it.
  void reachFunction(uint32_t node, uint32_t parent, Reach &reach) {
    if (seen(node))
      return;
    stamp_[node] = generation_;
    parent_[node] = parent;
    ++reach.functions;
    if (uint8_t mask = effectsOf(node))
      for (unsigned k = 0; k < kCostedKinds; ++k)
        if (((mask >> k) & 1u) && reach.first[k] == kNone)
          reach.first[k] = node;
    queue_.push_back(node);
  }

  // Queue a node whose callees are walked but which is not itself a
  // reached function.
  void walkThrough(uint32_t node, uint32_t parent) {
    if (seen(node))
      return;
    stamp_[node] = generation_;
    parent_[node] = parent;
    queue_.push_back(node);
  }

  // Breadth-first reach of the roots `members` together.
  Reach walk(llvm::ArrayRef<uint32_t> members) {
    ++generation_;
    queue_.clear();
    Reach reach;
    for (uint32_t member : members) {
      const Root &root = roots_[member];
      if (root.self != kNone)
        walkThrough(root.self, kSelf);
      for (uint32_t s = root.seedBegin; s < root.seedEnd; ++s)
        reachFunction(seeds_[s], kNone, reach);
    }
    for (size_t head = 0; head < queue_.size(); ++head) {
      const uint32_t node = queue_[head];
      if (node >= graphIds_)
        continue; // a leaf the graph has no edges for
      if (adjBegin_[node] == kNone)
        buildAdjacency(node);
      const uint32_t begin = adjBegin_[node], count = adjCount_[node];
      for (uint32_t i = 0; i < count; ++i)
        reachFunction(adjPool_[begin + i], node, reach);
    }
    return reach;
  }

  // The call chain from the initializer's first callee to `node`,
  // outermost first, as display names. Valid until the next walk.
  llvm::json::Array chainTo(uint32_t node) {
    chain_.clear();
    for (uint32_t at = node; at != kNone;) {
      const uint32_t link = parent_[at];
      if (link == kSelf)
        break;
      chain_.push_back(at);
      at = link;
    }
    llvm::json::Array out;
    for (auto it = chain_.rbegin(); it != chain_.rend(); ++it)
      out.push_back(nameOf(*it));
    return out;
  }

  void measureRoots(const StaticInitFacts &facts) {
    samples_.reserve(roots_.size());
    for (uint32_t i = 0; i < roots_.size(); ++i) {
      const StaticInitRoot &fact = facts.roots[i];
      Root &root = roots_[i];
      const uint32_t self[1] = {i};
      Reach reach = walk(self);

      if (root.self != kNone) {
        // A constructor function was walked through, never built lazily
        // before its own walk: its adjacency exists now.
        assert(adjBegin_[root.self] != kNone);
        // Itself, when it recurses, is not something it calls: the
        // value does not count it either.
        const uint32_t begin = adjBegin_[root.self];
        for (uint32_t k = 0; k < adjCount_[root.self]; ++k)
          root.directCalls += adjPool_[begin + k] != root.self;
      } else {
        root.directCalls = root.seedEnd - root.seedBegin;
      }

      MetricSample sample;
      sample.entity.kind = fact.kind == StaticInitKind::ConstructorFunction
                               ? "constructor-function"
                               : "variable";
      sample.entity.name = fact.name;
      sample.entity.usr = fact.usr;
      sample.entity.file = fact.file;
      sample.entity.line = fact.line;
      sample.tus = fact.tus;
      sample.value = reach.functions;
      sample.components.emplace_back("directCalls", root.directCalls);
      llvm::json::Object witness;
      for (unsigned k = 0; k < kCostedKinds; ++k) {
        const bool has = reach.first[k] != kNone;
        const std::string kind = effectKindName(kEffectKinds[k]).str();
        sample.components.emplace_back(kind, has ? 1 : 0);
        if (has) {
          root.effects |= effectBit(kEffectKinds[k]);
          witness[kind] = chainTo(reach.first[k]);
        }
      }
      sample.components.emplace_back("tus",
                                     static_cast<int64_t>(fact.tus.size()));
      if (!witness.empty())
        sample.witness = std::move(witness);
      samples_.push_back(std::move(sample));
    }
  }

  const CallGraph &graph_;
  const EffectTable &effects_;
  const uint32_t graphIds_;

  std::vector<Root> roots_;
  std::vector<uint32_t> seeds_; // node of each callee an initializer calls
  llvm::StringMap<uint32_t> extraByUsr_;
  std::vector<std::string> extraNames_;
  std::vector<std::string> extraUsrs_;

  // Lazily built adjacency: adjPool_[adjBegin_[n] .. +adjCount_[n]).
  std::vector<uint32_t> adjBegin_;
  std::vector<uint32_t> adjCount_;
  std::vector<uint32_t> adjPool_;
  std::vector<uint8_t> effect_; // kEffectKnown | mask, per node

  // Walk workspace, reused by every walk: a node is visited when its
  // stamp equals the current generation.
  std::vector<uint32_t> stamp_;
  std::vector<uint32_t> parent_;
  std::vector<uint32_t> queue_;
  std::vector<uint32_t> chain_;
  uint32_t generation_ = 0;
};

class StaticInitCostMetric final : public Metric {
public:
  std::string name() const override { return "static-init-cost"; }
  std::string summary() const override {
    return "Work done before main: functions reachable from each dynamic "
           "initializer and constructor function, and whether that reach "
           "allocates, locks or does I/O";
  }
  std::string unit() const override { return "functions"; }

  std::string unavailable(const MetricContext &ctx) const override {
    if (ctx.index.staticInits && ctx.index.staticInits->collected)
      return {};
    return "this index holds no static-initialization facts (it was "
           "written without them); rebuild it with `megascope index`, or "
           "run with --source, --source-list or --source-re to bake in "
           "memory";
  }

  std::unique_ptr<MetricRun>
  measure(const MetricContext &ctx) const override {
    return std::make_unique<StaticInitCostRun>(
        ctx.index.graph, *ctx.index.staticInits, ctx.effects);
  }
};

} // namespace

std::unique_ptr<Metric> makeStaticInitCostMetric() {
  return std::make_unique<StaticInitCostMetric>();
}

} // namespace vycor
