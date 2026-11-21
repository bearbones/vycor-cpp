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

#include "vycor/callgraph/ReferenceIndex.h"

#include "vycor/check/LockJoin.h"
#include "vycor/check/UnsynchronizedSharedGlobal.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"

#include <algorithm>
#include <map>
#include <tuple>

namespace vycor {

namespace {

using GId = StringInterner::Id;  // call-graph string id
using RId = ReferenceIndex::Id;  // reference-index string id
constexpr uint32_t kNone = UINT32_MAX;
constexpr const char *kCheck = "unsynchronized-shared-global";

enum class CtxKind : uint8_t { Main, Thread, Task };

struct Context {
  std::vector<GId> roots;
  CtxKind kind = CtxKind::Main;
  ExecutionContext exec = ExecutionContext::Synchronous;
  bool selfConcurrent = false;
  bool overflow = false;
  uint32_t spawnCount = 0;
  GId spawnCaller = kNone; // witness spawn edge
  GId spawnSite = kNone;
};

struct Var {
  RId usr = 0;
  std::string name;
  std::string file; // definition, else first declaration
  uint32_t line = 0;
  uint32_t flags = 0;
  bool allSystem = true;
  bool haveDefinition = false;
};

struct Access {
  uint32_t var;
  RefRole role;
  RId locks;
  RId file;
  uint32_t line;
  uint32_t col;
};

// The accesses of a function (CallLockFacts function index).
struct AccessRange {
  uint32_t begin = 0, end = 0;
};

struct State {
  uint32_t fn;
  uint32_t set;
  uint32_t parent; // state index, kNone at a root
  uint32_t depth;
  bool plausible;
};

struct Instance {
  uint32_t ctx;
  uint32_t set;
  uint32_t state;
  uint32_t access;
  uint32_t line;
  uint32_t col;
  bool write;
  bool plausible;
};

enum Tier : uint8_t {
  kInitThenPublish = 0,
  kPlausible = 1,
  kUnproven = 2,
  kRace = 3,
};

bool isSpawn(ExecutionContext exec) {
  return exec == ExecutionContext::ThreadSpawn ||
         exec == ExecutionContext::AsyncTask ||
         exec == ExecutionContext::PackagedTask;
}

const char *spawnerName(ExecutionContext exec) {
  switch (exec) {
  case ExecutionContext::ThreadSpawn:
    return "std::thread";
  case ExecutionContext::AsyncTask:
    return "std::async";
  case ExecutionContext::PackagedTask:
    return "std::packaged_task";
  default:
    break;
  }
  return "a spawner";
}

class Analysis {
public:
  Analysis(const CallGraph &graph, const ReferenceIndex &refs,
           const SharedGlobalOptions &opts)
      : graph_(graph), refs_(refs), opts_(opts), facts_(graph, refs),
        sets_(facts_.sets()) {}

  std::vector<Diagnostic> run(SharedGlobalStats &stats);

private:
  // ---- facts ----------------------------------------------------------------
  void readSymbols();
  void readReferences();
  void findContexts();
  [[nodiscard]] AccessRange accessesOf(uint32_t fn) const {
    return fn < accessRanges_.size() ? accessRanges_[fn] : AccessRange{};
  }

  // ---- search ---------------------------------------------------------------
  void search(uint32_t ctx);
  void collect(uint32_t ctx, uint32_t begin, uint32_t end);
  void judge(std::vector<Diagnostic> &out);

  // ---- reporting ------------------------------------------------------------
  [[nodiscard]] std::string pathTo(uint32_t state) const;
  [[nodiscard]] std::string contextText(uint32_t ctx) const;
  [[nodiscard]] std::string siteText(const Instance &inst) const;
  [[nodiscard]] const std::string &fnName(uint32_t fn) const {
    return facts_.functionName(fn);
  }

  const CallGraph &graph_;
  const ReferenceIndex &refs_;
  const SharedGlobalOptions &opts_;
  // The lock join shared with lock-order-inversion (check/LockJoin.h).
  CallLockFacts facts_;
  LockSets &sets_;

  std::vector<Var> vars_;
  llvm::DenseMap<RId, uint32_t> varOf_;
  std::vector<GId> mainRoots_;          // functions marked `main`
  llvm::DenseSet<RId> mainDefinitions_; // files defining a `main`

  std::vector<AccessRange> accessRanges_; // by function index
  std::vector<std::pair<GId, Access>> pendingAccesses_;
  std::vector<Access> accesses_;
  // (referrer, target) graph ids with an occurrence inside a loop, and
  // functions called inside a loop.
  llvm::DenseSet<uint64_t> loopRefs_;
  llvm::DenseSet<GId> calledInLoop_;

  std::vector<Context> contexts_;
  std::vector<State> states_;
  std::vector<Instance> instances_;
  // (var, ctx, set, write) -> best instance
  llvm::DenseMap<std::tuple<uint32_t, uint32_t, uint32_t, uint8_t>, uint32_t>
      groupOf_;
};

// Symbol facts: the candidate variables and the `main` functions (the
// function flags are the join's).
void Analysis::readSymbols() {
  facts_.readSymbols([&](const ReferenceIndex::SymbolRecord &rec) {
    if (rec.kind == SymbolKind::Function) {
      if ((rec.flags & kSymMain) != 0)
        if (const GId gid = facts_.graphId(rec.usr); gid != kNone)
          mainRoots_.push_back(gid);
      // Every program's `main` is one symbol; its definitions tell them
      // apart (a declaration record per defining TU).
      if ((rec.flags & (kSymMain | kSymDefinition)) ==
          (kSymMain | kSymDefinition))
        mainDefinitions_.insert(rec.file);
      return;
    }
    if (rec.kind != SymbolKind::Variable)
      return;
    auto [it, inserted] =
        varOf_.try_emplace(rec.usr, static_cast<uint32_t>(vars_.size()));
    if (inserted) {
      Var var;
      var.usr = rec.usr;
      vars_.push_back(std::move(var));
    }
    Var &var = vars_[it->second];
    var.flags |= rec.flags & ~kSymbolPerDeclFlags;
    if ((rec.flags & kSymSystemHeader) == 0)
      var.allSystem = false;
    const std::string &name = refs_.textOf(rec.name);
    if (var.name.empty() || name < var.name)
      var.name = name;
    // The location: the definition outside system headers, else the first
    // declaration, by (file, line).
    const bool definition = (rec.flags & kSymDefinition) != 0;
    const std::string &file = refs_.textOf(rec.file);
    const bool better =
        var.file.empty() || (definition && !var.haveDefinition) ||
        (definition == var.haveDefinition &&
         std::tie(file, rec.line) < std::tie(var.file, var.line));
    if (better) {
      var.file = file;
      var.line = rec.line;
      var.haveDefinition = definition;
    }
  });
}

// Reference facts: accesses of variables by functions and the loop marks
// the self-concurrency test reads (the lock texts of calls are the join's).
void Analysis::readReferences() {
  facts_.readReferences([&](const ReferenceIndex::ReferenceRecord &rec,
                            GId referrer) {
    if (isLockRole(rec.role))
      return; // an acquisition: lock-order-inversion's
    auto var = varOf_.find(rec.target);
    if (var != varOf_.end()) {
      if (rec.role == RefRole::Call)
        return;
      // A write that only calls a non-const member with a const twin (a
      // map's find) does not change the variable: count it as a read.
      const RefRole role =
          rec.role == RefRole::Write && (rec.flags & kRefMutates) == 0
              ? RefRole::Read
              : rec.role;
      pendingAccesses_.push_back(
          {referrer,
           Access{var->second, role, rec.locks, rec.file, rec.line,
                  rec.col}});
      return;
    }
    const GId target = facts_.graphId(rec.target);
    if (target == kNone)
      return;
    if ((rec.flags & kRefInLoop) != 0) {
      loopRefs_.insert(lockPairKey(referrer, target));
      if (rec.role == RefRole::Call)
        calledInLoop_.insert(target);
    }
  });
}

// Every thread entry is a context; so is the main thread, rooted at `main`
// and the configured entry points.
void Analysis::findContexts() {
  struct Spawn {
    GId caller, callee, site;
    ExecutionContext exec;
  };
  std::vector<Spawn> spawns;
  llvm::DenseSet<GId> joinCallers;
  graph_.forEachStoredEdge([&](const CallGraph::EdgeRef &e) {
    if (!isSpawn(e.execContext))
      return;
    if (e.kind == EdgeKind::ThreadEntry)
      spawns.push_back({e.caller, e.callee, e.callSite, e.execContext});
    else if (e.kind == EdgeKind::FunctionPointerReturn)
      joinCallers.insert(e.caller); // synthesized at query time
  });
  for (GId caller : joinCallers)
    for (const CallGraph::EdgeRef &e : graph_.calleeRefsOf(caller))
      if (e.kind == EdgeKind::ThreadEntry && isSpawn(e.execContext))
        spawns.push_back({e.caller, e.callee, e.callSite, e.execContext});
  const auto &text = [&](GId id) -> const std::string & {
    return graph_.interner().resolve(id);
  };
  llvm::sort(spawns, [&](const Spawn &a, const Spawn &b) {
    return std::make_tuple(std::cref(text(a.callee)), std::cref(text(a.caller)),
                           std::cref(text(a.site))) <
           std::make_tuple(std::cref(text(b.callee)), std::cref(text(b.caller)),
                           std::cref(text(b.site)));
  });
  spawns.erase(std::unique(spawns.begin(), spawns.end(),
                           [](const Spawn &a, const Spawn &b) {
                             return a.caller == b.caller &&
                                    a.callee == b.callee && a.site == b.site;
                           }),
               spawns.end());

  for (size_t i = 0; i < spawns.size();) {
    Context ctx;
    ctx.roots.push_back(spawns[i].callee);
    ctx.kind = CtxKind::Task;
    ctx.spawnCaller = spawns[i].caller;
    ctx.spawnSite = spawns[i].site;
    ctx.exec = spawns[i].exec;
    const GId root = spawns[i].callee;
    if ((facts_.functionFlags(root) & kSymInLoop) != 0)
      ctx.selfConcurrent = true; // a lambda written in a loop
    for (; i < spawns.size() && spawns[i].callee == root; ++i) {
      const Spawn &s = spawns[i];
      ++ctx.spawnCount;
      if (s.exec != ExecutionContext::PackagedTask) {
        if (ctx.kind == CtxKind::Task) {
          ctx.spawnCaller = s.caller;
          ctx.spawnSite = s.site;
          ctx.exec = s.exec;
        }
        ctx.kind = CtxKind::Thread;
      }
      if (loopRefs_.count(lockPairKey(s.caller, s.callee)) ||
          calledInLoop_.count(s.caller))
        ctx.selfConcurrent = true;
    }
    if (ctx.spawnCount > 1)
      ctx.selfConcurrent = true;
    contexts_.push_back(std::move(ctx));
  }

  Context main;
  main.kind = CtxKind::Main;
  main.roots = mainRoots_;
  for (const std::string &name : opts_.entryPoints) {
    if (auto gid = graph_.interner().find(name); gid && graph_.isReferencedId(*gid)) {
      main.roots.push_back(*gid);
      continue;
    }
    for (const std::string &usr : graph_.usrsForName(name))
      if (auto gid = graph_.interner().find(usr))
        main.roots.push_back(*gid);
  }
  llvm::sort(main.roots, [&](GId a, GId b) { return text(a) < text(b); });
  main.roots.erase(std::unique(main.roots.begin(), main.roots.end()),
                   main.roots.end());
  if (!main.roots.empty()) {
    contexts_.insert(contexts_.begin(), std::move(main));
  }
}

// Breadth-first search of one context over (function, lock set,
// plausibility) states, from its roots with no lock held.
void Analysis::search(uint32_t ctxIndex) {
  Context &ctx = contexts_[ctxIndex];
  const auto begin = static_cast<uint32_t>(states_.size());
  llvm::DenseMap<uint64_t, uint8_t> seen;
  llvm::DenseMap<uint32_t, uint32_t> perFunction;
  const auto keyOf = [](uint32_t fn, uint32_t set, bool plausible) {
    return (static_cast<uint64_t>(fn) << 32) | (static_cast<uint64_t>(set) << 1) |
           (plausible ? 1u : 0u);
  };
  const auto push = [&](uint32_t fn, uint32_t set, uint32_t parent,
                        uint32_t depth, bool plausible) {
    if (!seen.try_emplace(keyOf(fn, set, plausible), 1).second)
      return;
    uint32_t &count = perFunction[fn];
    if (count >= opts_.maxStatesPerFunction ||
        states_.size() - begin >= opts_.maxStatesPerContext ||
        states_.size() >= opts_.maxStatesTotal) {
      ctx.overflow = true;
      return;
    }
    ++count;
    states_.push_back({fn, set, parent, depth, plausible});
  };
  for (GId root : ctx.roots)
    push(facts_.functionIndex(root), 0, kNone, 0, false);
  llvm::SmallVector<uint32_t, 4> hops;
  for (size_t qi = begin; qi < states_.size(); ++qi) {
    const State s = states_[qi];
    // Nothing below builds another function's callees: the view holds.
    for (const LockCallEdge &edge : facts_.callees(s.fn)) {
      facts_.hopSets(s.fn, edge, hops);
      for (uint32_t hop : hops)
        push(edge.callee, sets_.unite(s.set, hop), static_cast<uint32_t>(qi),
             s.depth + 1, s.plausible || edge.plausible);
    }
  }
  collect(ctxIndex, begin, static_cast<uint32_t>(states_.size()));
}

// Every access made in a reached state, folded to the best instance per
// (variable, context, lock set, write).
void Analysis::collect(uint32_t ctx, uint32_t begin, uint32_t end) {
  for (uint32_t si = begin; si < end; ++si) {
    const State &s = states_[si];
    const AccessRange range = accessesOf(s.fn);
    for (uint32_t ai = range.begin; ai < range.end; ++ai) {
      const Access &a = accesses_[ai];
      const auto consider = [&](uint32_t set, uint32_t line, uint32_t col) {
        const uint32_t effective = sets_.unite(s.set, set);
        // Code that runs once under a guard (call_once, a function-local
        // static's initializer) happens before every access that waits on
        // that guard: not a race.
        if (sets_.hasOnce(effective))
          return;
        const bool write = a.role == RefRole::Write;
        Instance inst{ctx, effective, si, ai, line, col, write, s.plausible};
        auto [it, inserted] = groupOf_.try_emplace(
            std::make_tuple(a.var, ctx, effective, uint8_t(write)),
            static_cast<uint32_t>(instances_.size()));
        if (inserted) {
          instances_.push_back(inst);
          return;
        }
        Instance &best = instances_[it->second];
        if (std::make_tuple(inst.plausible, states_[inst.state].depth) <
            std::make_tuple(best.plausible, states_[best.state].depth))
          best = inst;
      };
      if (a.locks == ReferenceIndex::kNoId) {
        consider(0, a.line, a.col);
        continue;
      }
      for (const LockEntry &entry : facts_.lockEntries(a.locks))
        consider(entry.set, entry.line, entry.col);
    }
  }
}

std::string Analysis::pathTo(uint32_t state) const {
  std::vector<uint32_t> chain;
  for (uint32_t s = state; s != kNone; s = states_[s].parent)
    chain.push_back(states_[s].fn);
  std::reverse(chain.begin(), chain.end());
  std::string out;
  constexpr size_t kHead = 2, kTail = 3;
  for (size_t i = 0; i < chain.size(); ++i) {
    if (chain.size() > kHead + kTail + 1 && i == kHead) {
      out += " -> ...";
      i = chain.size() - kTail - 1;
      continue;
    }
    if (!out.empty())
      out += " -> ";
    out += fnName(chain[i]);
  }
  return out;
}

std::string Analysis::contextText(uint32_t ctxIndex) const {
  const Context &ctx = contexts_[ctxIndex];
  if (ctx.kind == CtxKind::Main)
    return "the main thread";
  std::string out = std::string("a ") + spawnerName(ctx.exec) + " started in " +
                    graph_.displayNameOf(ctx.spawnCaller) + " (" +
                    graph_.interner().resolve(ctx.spawnSite) + ")";
  if (ctx.spawnCount > 1)
    out += " and " + std::to_string(ctx.spawnCount - 1) + " other place(s)";
  return out;
}

std::string Analysis::siteText(const Instance &inst) const {
  const Access &a = accesses_[inst.access];
  return refs_.textOf(a.file) + ":" + std::to_string(inst.line) + ":" +
         std::to_string(inst.col);
}

void Analysis::judge(std::vector<Diagnostic> &out) {
  // Instances grouped by variable, in a canonical order within each.
  std::vector<std::pair<uint32_t, uint32_t>> byVar; // (var, instance)
  byVar.reserve(groupOf_.size());
  for (const auto &entry : groupOf_)
    byVar.push_back({std::get<0>(entry.first), entry.second});
  llvm::sort(byVar, [&](const auto &x, const auto &y) {
    if (x.first != y.first)
      return x.first < y.first;
    const Instance &a = instances_[x.second];
    const Instance &b = instances_[y.second];
    if (std::make_tuple(a.ctx, a.state, a.access, a.line, a.col, a.write) !=
        std::make_tuple(b.ctx, b.state, b.access, b.line, b.col, b.write))
      return std::make_tuple(a.ctx, a.state, a.access, a.line, a.col,
                             a.write) < std::make_tuple(b.ctx, b.state,
                                                        b.access, b.line,
                                                        b.col, b.write);
    return sets_.less(a.set, b.set);
  });

  for (size_t i = 0; i < byVar.size();) {
    const uint32_t varIndex = byVar[i].first;
    size_t end = i;
    while (end < byVar.size() && byVar[end].first == varIndex)
      ++end;
    int bestTier = -1, bestWrites = 0;
    uint32_t bestA = kNone, bestB = kNone;
    for (size_t x = i; x < end; ++x) {
      const Instance &a = instances_[byVar[x].second];
      for (size_t y = x; y < end; ++y) {
        const Instance &b = instances_[byVar[y].second];
        if (!a.write && !b.write)
          continue;
        const Context &ca = contexts_[a.ctx];
        const Context &cb = contexts_[b.ctx];
        if (a.ctx == b.ctx && (ca.kind == CtxKind::Main || !ca.selfConcurrent))
          continue;
        if (!sets_.disjoint(a.set, b.set))
          continue;
        int tier;
        if (ca.kind == CtxKind::Main || cb.kind == CtxKind::Main)
          tier = kInitThenPublish;
        else if (a.plausible || b.plausible || ca.kind == CtxKind::Task ||
                 cb.kind == CtxKind::Task)
          tier = kPlausible;
        else if (ca.overflow || cb.overflow || !opts_.coverageGap.empty())
          tier = kUnproven;
        else
          tier = kRace;
        // Equal tiers: the pair with more writes (a write-write race reads
        // plainer than a write and the read in the same `++x`).
        const int writes = int(a.write) + int(b.write);
        if (tier > bestTier || (tier == bestTier && writes > bestWrites)) {
          bestTier = tier;
          bestWrites = writes;
          bestA = byVar[x].second;
          bestB = byVar[y].second;
        }
      }
    }
    i = end;
    if (bestTier < 0)
      continue;

    const Var &var = vars_[varIndex];
    // The write first.
    if (!instances_[bestA].write)
      std::swap(bestA, bestB);
    const Instance &a = instances_[bestA];
    const Instance &b = instances_[bestB];
    const auto verb = [&](const Instance &inst) {
      switch (accesses_[inst.access].role) {
      case RefRole::Write:
        return "written";
      case RefRole::AddressOf:
        return "has its address taken";
      default:
        return "read";
      }
    };
    const uint32_t fnA = states_[a.state].fn, fnB = states_[b.state].fn;
    std::string msg = "'" + var.name + "' is " + verb(a) + " in " +
                      fnName(fnA) + " (" + siteText(a) + ") holding " +
                      sets_.describe(a.set) + ", and " + verb(b) + " in " +
                      fnName(fnB) + " (" + siteText(b) + ") holding " +
                      sets_.describe(b.set) + "; ";
    if (a.ctx == b.ctx)
      msg += "both run in " + contextText(a.ctx) +
             ", which can run more than once at the same time";
    else
      msg += "they run in " + contextText(a.ctx) + " and in " +
             contextText(b.ctx);
    msg += ", with no lock in common. Paths: " + pathTo(a.state) + "; " +
           pathTo(b.state) + ".";

    Diagnostic diag;
    diag.checkName = kCheck;
    switch (bestTier) {
    case kRace:
      diag.kind = Diagnostic::SharedGlobal_Race;
      break;
    case kUnproven: {
      diag.kind = Diagnostic::SharedGlobal_Unproven;
      std::string why = opts_.coverageGap;
      if (contexts_[a.ctx].overflow || contexts_[b.ctx].overflow) {
        if (!why.empty())
          why += "; ";
        why += "the lock-set search of a thread stopped at its bound";
      }
      msg += " Not proven: " + why + ".";
      break;
    }
    case kPlausible: {
      diag.kind = Diagnostic::SharedGlobal_Plausible;
      std::string why;
      for (const Instance *inst : {&a, &b}) {
        if (!why.empty())
          why += "; ";
        const Context &c = contexts_[inst->ctx];
        if (c.kind == CtxKind::Task)
          why += fnName(states_[inst->state].fn) +
                 " runs in a std::packaged_task, whose runner is not known";
        else if (inst->plausible)
          why += "the path to " + fnName(states_[inst->state].fn) +
                 " takes a plausible edge (virtual dispatch or a function "
                 "pointer)";
      }
      while (!why.empty() && (why.back() == ' ' || why.back() == ';'))
        why.pop_back();
      if (why.rfind("; ", 0) == 0)
        why.erase(0, 2);
      msg += " Not proven: " + why + ".";
      break;
    }
    default:
      diag.kind = Diagnostic::SharedGlobal_InitThenPublish;
      msg += " One side runs on the main thread: whether its access happens "
             "before the thread starts (initialize, then publish) is not "
             "proven.";
      break;
    }
    diag.file = var.file;
    diag.callLocation = var.file + ":" + std::to_string(var.line);
    diag.message = std::move(msg);
    diag.entities = {refs_.textOf(var.usr)};
    out.push_back(std::move(diag));
  }
}

std::vector<Diagnostic> Analysis::run(SharedGlobalStats &stats) {
  readSymbols();
  readReferences();

  // What is shared data: not const, atomic, thread-local or a lock, not
  // the library's own (declared only in system headers), and never used as
  // a lock itself.
  std::vector<uint8_t> candidate(vars_.size(), 0);
  for (size_t v = 0; v < vars_.size(); ++v) {
    const Var &var = vars_[v];
    candidate[v] = (var.flags & (kSymConst | kSymAtomic | kSymThreadLocal |
                                 kSymSyncPrimitive)) == 0 &&
                   !var.allSystem;
  }

  static const std::string kNoLocks;
  const auto locksText = [&](RId id) -> const std::string & {
    return id == ReferenceIndex::kNoId ? kNoLocks : refs_.textOf(id);
  };
  // Accesses by function, canonical within each.
  llvm::sort(pendingAccesses_, [&](const auto &x, const auto &y) {
    if (x.first != y.first)
      return x.first < y.first;
    const Access &a = x.second, &b = y.second;
    return std::make_tuple(std::cref(refs_.textOf(vars_[a.var].usr)),
                           static_cast<int>(a.role),
                           std::cref(refs_.textOf(a.file)), a.line, a.col,
                           std::cref(locksText(a.locks))) <
           std::make_tuple(std::cref(refs_.textOf(vars_[b.var].usr)),
                           static_cast<int>(b.role),
                           std::cref(refs_.textOf(b.file)), b.line, b.col,
                           std::cref(locksText(b.locks)));
  });
  for (size_t i = 0; i < pendingAccesses_.size();) {
    const GId gid = pendingAccesses_[i].first;
    const uint32_t fn = facts_.functionIndex(gid);
    if (accessRanges_.size() <= fn)
      accessRanges_.resize(fn + 1);
    accessRanges_[fn].begin = static_cast<uint32_t>(accesses_.size());
    for (; i < pendingAccesses_.size() && pendingAccesses_[i].first == gid; ++i)
      if (candidate[pendingAccesses_[i].second.var])
        accesses_.push_back(pendingAccesses_[i].second);
    accessRanges_[fn].end = static_cast<uint32_t>(accesses_.size());
  }
  pendingAccesses_ = {};

  findContexts();
  for (uint8_t c : candidate)
    stats.candidates += c;
  stats.mainDefinitions = mainDefinitions_.size();
  for (const Context &ctx : contexts_) {
    if (ctx.kind == CtxKind::Main) {
      stats.mainContext = true;
    } else {
      ++stats.threadContexts;
      stats.selfConcurrent += ctx.selfConcurrent ? 1 : 0;
    }
  }
  // Nothing can race without a second thread.
  std::vector<Diagnostic> out;
  if (stats.threadContexts == 0)
    return out;
  // Threads first: the main thread usually reaches the most code, and only
  // thread-against-thread pairs can be races, so the total state budget
  // goes to the threads before main.
  for (int pass = 0; pass < 2; ++pass)
    for (uint32_t c = 0; c < contexts_.size(); ++c) {
      if ((contexts_[c].kind == CtxKind::Main) != (pass == 1))
        continue;
      const size_t before = states_.size();
      search(c);
      if (!contexts_[c].overflow)
        continue;
      ++stats.boundedContexts;
      std::string name = contextText(c);
      if (states_.size() == before)
        name += " (no state: the total bound was already spent)";
      stats.boundedNames.push_back(std::move(name));
    }
  stats.statesSearched = states_.size();

  // A variable some code locks is a mutex of a type the bake does not know.
  for (auto it = groupOf_.begin(); it != groupOf_.end();) {
    auto cur = it++;
    if (sets_.isLockIdentity(refs_.textOf(vars_[std::get<0>(cur->first)].usr)))
      groupOf_.erase(cur);
  }
  judge(out);
  llvm::sort(out, [](const Diagnostic &a, const Diagnostic &b) {
    return std::tie(a.file, a.callLocation, a.message) <
           std::tie(b.file, b.callLocation, b.message);
  });
  return out;
}

} // namespace

std::vector<Diagnostic>
findUnsynchronizedSharedGlobals(const CallGraph &graph,
                                const ReferenceIndex &refs,
                                const SharedGlobalOptions &opts,
                                SharedGlobalStats *stats) {
  SharedGlobalStats local;
  return Analysis(graph, refs, opts).run(stats ? *stats : local);
}

} // namespace vycor
