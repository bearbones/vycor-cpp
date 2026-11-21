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
#include "vycor/callgraph/HotPath.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Regex.h"

#include <algorithm>
#include <cassert>
#include <tuple>

namespace vycor {

const char *hotEvidenceName(HotEvidence evidence) {
  switch (evidence) {
  case HotEvidence::Observed:
    return "observed";
  case HotEvidence::Unprofiled:
    return "unprofiled";
  case HotEvidence::Latent:
    return "latent";
  }
  return "unprofiled";
}

namespace {

using Id = StringInterner::Id;

constexpr uint32_t kNone = 0xffffffffu;
constexpr int64_t kNoCount = CoverageProfile::kNoCount;
constexpr int64_t kUnset = INT64_MIN;

// Per-id flags. The low bits are the effect mask of the function.
constexpr uint16_t kEffectBits = kAllEffects;
constexpr uint16_t kKnown = 1u << 4;       // effect mask and kAllowed are set
constexpr uint16_t kRootConfig = 1u << 5;
constexpr uint16_t kRootProfile = 1u << 6;
constexpr uint16_t kAllowed = 1u << 7;
constexpr uint16_t kHasCalls = 1u << 8;
constexpr uint16_t kSettled = 1u << 9;
constexpr uint16_t kRoot = kRootConfig | kRootProfile;
static_assert(kAllEffects < kKnown, "effect bits overlap the flags");

// The width of a path is the smallest capacity of a call site on it. A
// site that never ran is 0, one with no count is 1, and one that ran c
// times is c + 1: any path that is known to have run is wider than one
// that merely might have, which is wider than one known not to have.
constexpr uint64_t kRootCap = UINT64_MAX;
constexpr uint64_t kUnknownCap = 1;

uint64_t capOf(int64_t count) noexcept {
  return count == kNoCount ? kUnknownCap
                           : count == 0 ? 0 : static_cast<uint64_t>(count) + 1;
}

struct SiteLoc {
  llvm::StringRef file;
  uint32_t line = 0;
  uint32_t col = 0;
};

// "file:line:col". A site that is not one (empty, or a bare marker) has no
// location; the file keeps any colon of its own.
bool parseSite(llvm::StringRef site, SiteLoc &out) noexcept {
  auto [head, col] = site.rsplit(':');
  auto [file, line] = head.rsplit(':');
  if (file.empty() || line.getAsInteger(10, out.line) ||
      col.getAsInteger(10, out.col))
    return false;
  out.file = file;
  return true;
}

// Normalized names, sorted; `source` is the index of the configured entry
// each came from.
struct NameList {
  std::vector<std::pair<std::string, uint32_t>> names;

  void add(llvm::StringRef name, uint32_t source) {
    names.emplace_back(effectLookupName(name), source);
  }
  void finish() { std::sort(names.begin(), names.end()); }
  bool empty() const { return names.empty(); }

  // Calls fn(source) for every entry spelled `name`; true when any is.
  template <typename Fn> bool match(const std::string &name, Fn &&fn) const {
    auto it = std::lower_bound(
        names.begin(), names.end(), name,
        [](const std::pair<std::string, uint32_t> &entry,
           const std::string &key) { return entry.first < key; });
    bool any = false;
    for (; it != names.end() && it->first == name; ++it) {
      fn(it->second);
      any = true;
    }
    return any;
  }
};

// An effect call met by the search, before it is ordered.
struct RawFinding {
  Id caller;
  Id callee;
  Id site;
  EdgeKind kind;
  Confidence confidence;
  uint8_t effects;
  uint64_t cap;
};

class HotPathSearch {
public:
  HotPathSearch(const CallGraph &graph, const EffectTable &effects,
                const CoverageProfile *profile, uint8_t effectMask,
                HotPathReport &out)
      : graph_(graph), strings_(graph.interner()), effects_(effects),
        profile_(profile), effectMask_(effectMask & kAllEffects), out_(out) {}

  bool run(const HotPathConfig &config, std::string &error) {
    out_ = HotPathReport();
    const size_t ids = strings_.size();
    flags_.assign(ids, 0);
    if (!markConfigured(config, error))
      return false;
    if (profile_)
      joinProfile(config.minCount);

    // Every table the search writes is sized here, once.
    cap_.assign(ids, 0);
    depth_.assign(ids, 0);
    parent_.assign(ids, kNone);
    parentSite_.assign(ids, kNone);
    parentKind_.assign(ids, EdgeKind::DirectCall);
    parentConfidence_.assign(ids, Confidence::Proven);
    heapPos_.assign(ids, kNone);
    heap_.reserve(graph_.nodeCount());

    for (Id id = 0; id < ids; ++id) {
      if (!(flags_[id] & kRoot))
        continue;
      ++out_.roots;
      out_.configRoots += (flags_[id] & kRootConfig) != 0;
      out_.profileRoots += (flags_[id] & kRootProfile) != 0;
      cap_[id] = kRootCap;
      push(id);
    }
    search();
    emit();
    return true;
  }

private:
  const std::string &str(Id id) const { return strings_.resolve(id); }

  // Effect mask and allow bit of a function, computed on first meeting.
  uint16_t flagsOf(Id id) {
    uint16_t &flags = flags_[id];
    if (flags & kKnown)
      return flags;
    const std::string &name = graph_.displayNameOf(id);
    flags |= kKnown | effects_.effectsOf(name, str(id));
    if (!allow_.empty() && allow_.match(effectLookupName(name), [](uint32_t) {}))
      flags |= kAllowed;
    return flags;
  }

  // ------------------------------------------------------------------
  // Roots and the allow list
  // ------------------------------------------------------------------

  bool markConfigured(const HotPathConfig &config, std::string &error) {
    // An entry that is the USR of a function in the graph names that one
    // function; any other entry is a qualified name.
    auto asUsr = [&](const std::string &entry) -> Id {
      std::optional<Id> id = strings_.find(entry);
      return id && graph_.isReferencedId(*id) ? *id : kNone;
    };
    for (const std::string &entry : config.allow) {
      if (Id id = asUsr(entry); id != kNone)
        flags_[id] |= kAllowed;
      else
        allow_.add(entry, 0);
    }
    allow_.finish();

    const uint32_t rootCount = static_cast<uint32_t>(config.roots.size());
    std::vector<bool> matched(rootCount + config.rootPatterns.size(), false);
    NameList rootNames;
    for (uint32_t i = 0; i < rootCount; ++i) {
      if (Id id = asUsr(config.roots[i]); id != kNone) {
        flags_[id] |= kRootConfig;
        matched[i] = true;
      } else {
        rootNames.add(config.roots[i], i);
      }
    }
    rootNames.finish();

    std::vector<llvm::Regex> patterns;
    patterns.reserve(config.rootPatterns.size());
    for (const std::string &pattern : config.rootPatterns) {
      patterns.emplace_back(pattern);
      std::string why;
      if (!patterns.back().isValid(why)) {
        error = "root pattern '" + pattern + "' is not a valid regex: " + why;
        return false;
      }
    }

    if (!rootNames.empty() || !patterns.empty()) {
      graph_.forEachNode([&](Id id, const CallGraphNode &node) {
        bool root = false;
        if (!rootNames.empty())
          root = rootNames.match(effectLookupName(node.qualifiedName),
                                 [&](uint32_t i) { matched[i] = true; });
        for (size_t p = 0; p < patterns.size(); ++p) {
          if (patterns[p].match(node.qualifiedName)) {
            matched[rootCount + p] = true;
            root = true;
          }
        }
        if (root)
          flags_[id] |= kRootConfig;
      });
    }
    for (size_t i = 0; i < matched.size(); ++i)
      if (!matched[i])
        out_.unmatchedRoots.push_back(
            i < rootCount ? config.roots[i]
                          : config.rootPatterns[i - rootCount]);
    std::sort(out_.unmatchedRoots.begin(), out_.unmatchedRoots.end());
    out_.unmatchedRoots.erase(
        std::unique(out_.unmatchedRoots.begin(), out_.unmatchedRoots.end()),
        out_.unmatchedRoots.end());
    return true;
  }

  // ------------------------------------------------------------------
  // The profile join
  // ------------------------------------------------------------------

  // Count of a call site; kNoCount without a profile, a location, a file
  // the profile has, or a counted region there.
  int64_t siteCount(Id site) {
    if (!profile_)
      return kNoCount;
    int64_t &count = siteCount_[site];
    if (count == kUnset) {
      SiteLoc loc;
      uint32_t file = CoverageProfile::kNone;
      if (parseSite(str(site), loc))
        file = profile_->fileFor(loc.file);
      count = file == CoverageProfile::kNone
                  ? kNoCount
                  : profile_->countAt(file, loc.line, loc.col);
    }
    return count;
  }

  int64_t calleeCount(Id id) const noexcept {
    return profile_ ? functionCount_[id] : kNoCount;
  }

  // One pass over the stored edges: the count of every call site, and the
  // entry count of every function that makes a call. The index has no
  // body range for a function, so a function is joined through its calls:
  // its count is that of the profile body most of its call sites lie in.
  // (A function that makes no call has no count and needs none: nothing
  // is reachable from it.)
  void joinProfile(uint64_t minCount) {
    const size_t ids = strings_.size();
    siteCount_.assign(ids, kUnset);
    functionCount_.assign(ids, kNoCount);
    // (caller, body) per call site inside a profiled body.
    std::vector<uint64_t> votes;
    votes.reserve(graph_.edgeCount());

    // The files the call sites name, resolved against the profile as a
    // set so a profile file is not matched by two index files.
    {
      llvm::StringSet<> seen;
      std::vector<llvm::StringRef> files;
      graph_.forEachStoredEdge([&](const CallGraph::EdgeRef &edge) {
        SiteLoc loc;
        if (parseSite(str(edge.callSite), loc) && seen.insert(loc.file).second)
          files.push_back(loc.file);
      });
      profile_->bindFiles(files);
    }

    // Counted once per call site (a site can carry several edges).
    HotPathJoin &join = out_.join;
    graph_.forEachStoredEdge([&](const CallGraph::EdgeRef &edge) {
      if (!(flags_[edge.caller] & kHasCalls)) {
        flags_[edge.caller] |= kHasCalls;
        ++join.functions;
      }
      int64_t &count = siteCount_[edge.callSite];
      const bool first = count == kUnset;
      join.callSites += first;
      SiteLoc loc;
      if (!parseSite(str(edge.callSite), loc)) {
        count = kNoCount;
        return;
      }
      const uint32_t file = profile_->fileFor(loc.file);
      if (file == CoverageProfile::kNone) {
        count = kNoCount;
        return;
      }
      if (first) {
        count = profile_->countAt(file, loc.line, loc.col);
        ++join.callSitesInProfile;
        join.callSitesCounted += count != kNoCount;
      }
      const uint32_t body = profile_->functionAt(file, loc.line, loc.col);
      if (body != CoverageProfile::kNone)
        votes.push_back(static_cast<uint64_t>(edge.caller) << 32 | body);
    });

    // The body with the most votes per caller; the first in profile order
    // on a tie.
    std::sort(votes.begin(), votes.end());
    for (size_t i = 0; i < votes.size();) {
      const Id caller = static_cast<Id>(votes[i] >> 32);
      uint32_t best = 0;
      size_t bestRun = 0;
      while (i < votes.size() && static_cast<Id>(votes[i] >> 32) == caller) {
        size_t run = i;
        while (run < votes.size() && votes[run] == votes[i])
          ++run;
        if (run - i > bestRun) {
          bestRun = run - i;
          best = static_cast<uint32_t>(votes[i]);
        }
        i = run;
      }
      const int64_t count = profile_->functionCount(best);
      functionCount_[caller] = count;
      ++join.functionsCounted;
      // A function the table calls an effect, or one that is accepted, is
      // not a root unless it is configured as one.
      if (minCount != 0 && static_cast<uint64_t>(count) >= minCount &&
          !(flagsOf(caller) & (kEffectBits | kAllowed)))
        flags_[caller] |= kRootProfile;
    }
  }

  // ------------------------------------------------------------------
  // The search: widest path from any root, fewest calls among equals
  // ------------------------------------------------------------------

  bool better(Id a, Id b) const noexcept {
    return cap_[a] != cap_[b] ? cap_[a] > cap_[b] : depth_[a] < depth_[b];
  }

  void siftUp(uint32_t pos) noexcept {
    const Id id = heap_[pos];
    while (pos > 0) {
      const uint32_t up = (pos - 1) / 2;
      if (!better(id, heap_[up]))
        break;
      heap_[pos] = heap_[up];
      heapPos_[heap_[pos]] = pos;
      pos = up;
    }
    heap_[pos] = id;
    heapPos_[id] = pos;
  }

  void siftDown(uint32_t pos) noexcept {
    const Id id = heap_[pos];
    const uint32_t size = static_cast<uint32_t>(heap_.size());
    for (;;) {
      uint32_t child = 2 * pos + 1;
      if (child >= size)
        break;
      if (child + 1 < size && better(heap_[child + 1], heap_[child]))
        ++child;
      if (!better(heap_[child], id))
        break;
      heap_[pos] = heap_[child];
      heapPos_[heap_[pos]] = pos;
      pos = child;
    }
    heap_[pos] = id;
    heapPos_[id] = pos;
  }

  void push(Id id) {
    heap_.push_back(id);
    siftUp(static_cast<uint32_t>(heap_.size() - 1));
  }

  Id pop() noexcept {
    const Id top = heap_.front();
    const Id last = heap_.back();
    heap_.pop_back();
    heapPos_[top] = kNone;
    if (!heap_.empty()) {
      heap_[0] = last;
      siftDown(0);
    }
    return top;
  }

  // Whether `edge` is the canonical parent among offers that tie: the
  // strongest confidence, then the smallest (caller USR, call site, kind).
  // Ids are positions in the interner and differ between bakes; the
  // strings do not.
  bool canonicalBefore(Id node, const CallGraph::EdgeRef &edge) const {
    if (parentConfidence_[node] != edge.confidence)
      return edge.confidence < parentConfidence_[node];
    if (parent_[node] != edge.caller)
      return str(edge.caller) < str(parent_[node]);
    if (parentSite_[node] != edge.callSite)
      return str(edge.callSite) < str(parentSite_[node]);
    return edge.kind < parentKind_[node];
  }

  void setParent(Id node, const CallGraph::EdgeRef &edge) noexcept {
    parent_[node] = edge.caller;
    parentSite_[node] = edge.callSite;
    parentKind_[node] = edge.kind;
    parentConfidence_[node] = edge.confidence;
  }

  // Label-setting search. Extending a path never widens or shortens it, so
  // a function's label is final when it leaves the heap, and every offer
  // that could tie with the final label has arrived by then: the labels
  // and, with canonicalBefore, the parents do not depend on the order
  // edges or equal labels are taken in.
  void search() {
    while (!heap_.empty()) {
      const Id node = pop();
      flags_[node] |= kSettled;
      ++out_.reachedFunctions;
      const uint64_t nodeCap = cap_[node];
      const uint32_t nodeDepth = depth_[node];
      graph_.calleeRefsOf(node, refs_);
      for (const CallGraph::EdgeRef &edge : refs_) {
        const Id callee = edge.callee;
        const uint16_t flags = flagsOf(callee);
        if (flags & kAllowed) {
          ++out_.allowedCalls;
          continue;
        }
        uint64_t cap = std::min(nodeCap, capOf(siteCount(edge.callSite)));
        if (const int64_t entries = calleeCount(callee); entries != kNoCount)
          cap = std::min(cap, capOf(entries));
        if (const uint8_t hit = flags & effectMask_)
          raw_.push_back({node, callee, edge.callSite, edge.kind,
                          edge.confidence, hit, cap});
        // A root is as wide as a path gets, and a function that is itself
        // an effect is where the path ends.
        if (flags & (kRoot | kSettled | kEffectBits))
          continue;
        const uint32_t depth = nodeDepth + 1;
        if (heapPos_[callee] == kNone) {
          cap_[callee] = cap;
          depth_[callee] = depth;
          setParent(callee, edge);
          push(callee);
        } else if (cap > cap_[callee] ||
                   (cap == cap_[callee] && depth < depth_[callee])) {
          cap_[callee] = cap;
          depth_[callee] = depth;
          setParent(callee, edge);
          siftUp(heapPos_[callee]);
        } else if (cap == cap_[callee] && depth == depth_[callee] &&
                   canonicalBefore(callee, edge)) {
          setParent(callee, edge);
        }
      }
      if (refs_.empty() && isInstantiation(node) && !bodyRead(node)) {
        ++out_.templateLeaves;
      }
    }
  }

  // Whether some TU of the bake read the function's body: an
  // instantiation with no call there calls nothing.
  bool bodyRead(Id id) const {
    const CallGraphNode *node = graph_.findNodeById(id);
    return node && node->hasBody;
  }

  // Whether a function is a template instantiation: a member of a class
  // template specialization, read from its display name with parameter
  // lists dropped (`std::vector<int>::push_back`, but not a local class
  // in `f(std::vector<int> &)`), or a function template specialization,
  // read from the name in its USR (`@F@name<#<args>>#<params>`).
  bool isInstantiation(Id id) const {
    return scopeHasTemplateArgs(graph_.displayNameOf(id)) ||
           usrNamesSpecialization(str(id));
  }

  static bool scopeHasTemplateArgs(llvm::StringRef name) noexcept {
    unsigned parens = 0;
    for (size_t i = 0; i < name.size(); ++i) {
      const char c = name[i];
      if (c == '(') {
        ++parens;
      } else if (c == ')') {
        parens -= parens != 0;
      } else if (c == '<' && parens == 0) {
        // `operator<`, `operator<<`, `operator<=` and `operator<=>`.
        const llvm::StringRef before = name.take_front(i);
        if (!before.ends_with("operator") && !before.ends_with("operator<"))
          return true;
      }
    }
    return false;
  }

  static bool usrNamesSpecialization(llvm::StringRef usr) noexcept {
    const size_t f = usr.rfind("@F@");
    if (f == llvm::StringRef::npos)
      return false;
    // The name runs to the first '#'; a specialization's ends in '<'.
    llvm::StringRef name = usr.drop_front(f + 3);
    name = name.take_front(name.find('#'));
    if (!name.consume_back("<"))
      return false;
    // `operator<` and `operator<<` end in '<' without being one.
    if (name.consume_front("operator"))
      return !name.empty() && name != "<";
    return !name.empty();
  }

  // ------------------------------------------------------------------
  // Findings
  // ------------------------------------------------------------------

  struct SortKey {
    HotEvidence evidence;
    EffectKind effect;
    int64_t weight;
    const std::string *callerName, *callerUsr;
    const std::string *calleeName, *calleeUsr;
    llvm::StringRef file; // the whole site when it has no location
    uint32_t line, col;
    uint32_t raw;
  };

  static bool keyBefore(const SortKey &a, const SortKey &b) {
    if (a.evidence != b.evidence)
      return a.evidence < b.evidence;
    if (a.weight != b.weight)
      return a.weight > b.weight;
    if (a.effect != b.effect)
      return a.effect < b.effect;
    if (int c = a.callerName->compare(*b.callerName))
      return c < 0;
    if (int c = a.callerUsr->compare(*b.callerUsr))
      return c < 0;
    if (int c = a.file.compare(b.file))
      return c < 0;
    if (a.line != b.line)
      return a.line < b.line;
    if (a.col != b.col)
      return a.col < b.col;
    if (int c = a.calleeName->compare(*b.calleeName))
      return c < 0;
    return *a.calleeUsr < *b.calleeUsr;
  }

  void emit() {
    // One finding per (function, call site, callee): the same call may be
    // stored under more than one kind.
    std::sort(raw_.begin(), raw_.end(),
              [](const RawFinding &a, const RawFinding &b) {
                return std::tie(a.caller, a.site, a.callee, a.kind,
                                a.confidence) <
                       std::tie(b.caller, b.site, b.callee, b.kind,
                                b.confidence);
              });
    raw_.erase(std::unique(raw_.begin(), raw_.end(),
                           [](const RawFinding &a, const RawFinding &b) {
                             return a.caller == b.caller && a.site == b.site &&
                                    a.callee == b.callee;
                           }),
               raw_.end());

    std::vector<SortKey> keys;
    keys.reserve(raw_.size());
    for (uint32_t i = 0; i < raw_.size(); ++i) {
      const RawFinding &raw = raw_[i];
      SortKey key{};
      key.evidence = raw.cap == 0             ? HotEvidence::Latent
                     : raw.cap == kUnknownCap ? HotEvidence::Unprofiled
                                              : HotEvidence::Observed;
      key.weight = raw.cap == 0             ? 0
                   : raw.cap == kUnknownCap ? kNoCount
                                            : static_cast<int64_t>(raw.cap - 1);
      key.callerName = &graph_.displayNameOf(raw.caller);
      key.callerUsr = &str(raw.caller);
      key.calleeName = &graph_.displayNameOf(raw.callee);
      key.calleeUsr = &str(raw.callee);
      SiteLoc loc;
      if (parseSite(str(raw.site), loc)) {
        key.file = loc.file;
        key.line = loc.line;
        key.col = loc.col;
      } else {
        key.file = str(raw.site);
      }
      key.raw = i;
      for (EffectKind kind : kEffectKinds) {
        if (!(raw.effects & effectBit(kind)))
          continue;
        key.effect = kind;
        keys.push_back(key);
      }
    }
    std::sort(keys.begin(), keys.end(), keyBefore);

    out_.findings.reserve(keys.size());
    for (const SortKey &key : keys) {
      const RawFinding &raw = raw_[key.raw];
      HotPathFinding finding{};
      finding.effect = key.effect;
      finding.evidence = key.evidence;
      finding.weight = key.weight;
      finding.hopBegin = static_cast<uint32_t>(out_.hops.size());

      // The witness: the parents of the calling function back to its root,
      // then the effect call itself.
      const size_t first = out_.hops.size();
      Confidence weakest = raw.confidence;
      Id node = raw.caller;
      while (parent_[node] != kNone) {
        out_.hops.push_back({parent_[node], node, parentSite_[node],
                             parentKind_[node], parentConfidence_[node],
                             siteCount(parentSite_[node]), calleeCount(node)});
        weakest = std::max(weakest, parentConfidence_[node]);
        node = parent_[node];
      }
      std::reverse(out_.hops.begin() + static_cast<std::ptrdiff_t>(first),
                   out_.hops.end());
      out_.hops.push_back({raw.caller, raw.callee, raw.site, raw.kind,
                           raw.confidence, siteCount(raw.site),
                           calleeCount(raw.callee)});
      assert(flags_[node] & kRoot);

      finding.hopEnd = static_cast<uint32_t>(out_.hops.size());
      finding.confidence = weakest;
      finding.rootSources =
          ((flags_[node] & kRootConfig) ? kHotRootConfig : 0) |
          ((flags_[node] & kRootProfile) ? kHotRootProfile : 0);
      finding.rootCount = profile_ ? functionCount_[node] : kNoCount;
      ++out_.counts[static_cast<unsigned>(key.evidence)]
                   [static_cast<unsigned>(key.effect)];
      out_.findings.push_back(finding);
    }
  }

  const CallGraph &graph_;
  const StringInterner &strings_;
  const EffectTable &effects_;
  const CoverageProfile *profile_;
  const uint8_t effectMask_;
  HotPathReport &out_;

  NameList allow_;

  // Indexed by interner id.
  std::vector<uint16_t> flags_;
  std::vector<uint64_t> cap_;
  std::vector<uint32_t> depth_;
  std::vector<Id> parent_;
  std::vector<Id> parentSite_;
  std::vector<EdgeKind> parentKind_;
  std::vector<Confidence> parentConfidence_;
  std::vector<uint32_t> heapPos_;
  std::vector<int64_t> siteCount_;     // with a profile
  std::vector<int64_t> functionCount_; // with a profile

  std::vector<Id> heap_;
  std::vector<CallGraph::EdgeRef> refs_; // callee buffer reused per node
  std::vector<RawFinding> raw_;
};

} // namespace

bool analyzeHotPaths(const CallGraph &graph, const EffectTable &effects,
                     const HotPathConfig &config,
                     const CoverageProfile *profile, uint8_t effectMask,
                     HotPathReport &out, std::string &error) {
  return HotPathSearch(graph, effects, profile, effectMask, out)
      .run(config, error);
}

} // namespace vycor
