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

#include "vycor/deps/DependencyAudit.h"

#include "vycor/callgraph/CallGraph.h"
#include "vycor/callgraph/ReferenceIndex.h"
#include "vycor/callgraph/Snapshot.h"
#include "vycor/callgraph/Utf8.h"
#include "vycor/query/Serialize.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/bit.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/xxhash.h"

#include <algorithm>
#include <cassert>
#include <map>
#include <numeric>

namespace vycor {

namespace {

using SId = StringInterner::Id;

// ---------------------------------------------------------------------------
// Shared small tables
// ---------------------------------------------------------------------------

/// Order key of a site: files rank by path, then line, then column.
constexpr unsigned kKeyLineBits = 24;
constexpr unsigned kKeyColBits = 20;

uint64_t siteKey(uint32_t fileRank, uint32_t line, uint32_t col) noexcept {
  line = std::min<uint32_t>(line, (1u << kKeyLineBits) - 1);
  col = std::min<uint32_t>(col, (1u << kKeyColBits) - 1);
  return (static_cast<uint64_t>(fileRank) << (kKeyLineBits + kKeyColBits)) |
         (static_cast<uint64_t>(line) << kKeyColBits) | col;
}

/// Interned target sets (LinkFacts, "Target sets").
struct SetTable {
  std::vector<uint32_t> begin;
  std::vector<uint32_t> targets;
  std::map<std::vector<uint32_t>, uint32_t> multi;

  explicit SetTable(uint32_t targetCount) {
    begin.reserve(targetCount + 2);
    targets.reserve(targetCount);
    begin.push_back(0);
    begin.push_back(0); // the empty set, id 0
    for (uint32_t t = 0; t < targetCount; ++t) {
      targets.push_back(t);
      begin.push_back(static_cast<uint32_t>(targets.size()));
    }
  }

  /// `sorted`: ascending, unique.
  uint32_t intern(const std::vector<uint32_t> &sorted) {
    if (sorted.empty())
      return 0;
    if (sorted.size() == 1)
      return sorted[0] + 1;
    auto it = multi.find(sorted);
    if (it != multi.end())
      return it->second;
    const auto id = static_cast<uint32_t>(begin.size() - 1);
    targets.insert(targets.end(), sorted.begin(), sorted.end());
    begin.push_back(static_cast<uint32_t>(targets.size()));
    multi.emplace(sorted, id);
    return id;
  }

  [[nodiscard]] llvm::ArrayRef<uint32_t> of(uint32_t set) const {
    return llvm::ArrayRef<uint32_t>(targets).slice(begin[set],
                                                   begin[set + 1] - begin[set]);
  }
};

/// Every file the audit meets, by normalized path.
struct FileTable {
  llvm::StringMap<uint32_t> byPath;
  std::vector<std::string> paths;

  uint32_t add(llvm::StringRef normalized) {
    auto [it, inserted] =
        byPath.try_emplace(normalized, static_cast<uint32_t>(paths.size()));
    if (inserted)
      paths.push_back(normalized.str());
    return it->second;
  }
};

/// Maps a path as an index spells it (a call site's file, a node's file) to
/// a file id. Absolute paths normalize lexically. A relative spelling —
/// build systems that compile from an execution root — resolves to the one
/// known file it is a path suffix of, else stays its own unknown file.
class FileResolver {
public:
  explicit FileResolver(FileTable &files) : files_(files) {}

  /// Call once, after every file the map and the index meta name is in the
  /// table: only those are candidates for a relative spelling.
  void freezeKnown() {
    known_.reserve(files_.paths.size());
    for (uint32_t f = 0; f < files_.paths.size(); ++f)
      known_.emplace_back(llvm::sys::path::filename(files_.paths[f]).str(), f);
    std::sort(known_.begin(), known_.end());
  }

  /// kNoIndex for an empty spelling.
  uint32_t resolve(llvm::StringRef raw) {
    if (raw.empty())
      return kNoIndex;
    auto it = memo_.find(raw);
    if (it != memo_.end())
      return it->second;
    uint32_t id;
    if (llvm::sys::path::is_absolute(raw)) {
      id = files_.add(normalizeAuditPath(raw, ""));
    } else {
      std::string rel = normalizeAuditPath(raw, "");
      llvm::StringRef tail = rel;
      while (tail.consume_front("../") || tail.consume_front("./")) {
      }
      id = suffixMatch(tail);
      if (id == kNoIndex) {
        id = files_.add(rel);
        if (unresolved_.size() <= id)
          unresolved_.resize(id + 1, 0);
        unresolved_[id] = 1;
      }
    }
    memo_.try_emplace(raw, id);
    return id;
  }

  [[nodiscard]] bool unresolved(uint32_t file) const noexcept {
    return file < unresolved_.size() && unresolved_[file] != 0;
  }

private:
  uint32_t suffixMatch(llvm::StringRef tail) const {
    const std::string base = llvm::sys::path::filename(tail).str();
    auto lo = std::lower_bound(
        known_.begin(), known_.end(), base,
        [](const std::pair<std::string, uint32_t> &a, const std::string &b) {
          return a.first < b;
        });
    uint32_t found = kNoIndex;
    for (; lo != known_.end() && lo->first == base; ++lo) {
      llvm::StringRef path = files_.paths[lo->second];
      if (!path.ends_with(tail))
        continue;
      const size_t at = path.size() - tail.size();
      if (at != 0 && path[at - 1] != '/')
        continue;
      if (found != kNoIndex)
        return kNoIndex; // ambiguous: several known files end this way
      found = lo->second;
    }
    return found;
  }

  FileTable &files_;
  llvm::StringMap<uint32_t> memo_;
  std::vector<std::pair<std::string, uint32_t>> known_;
  std::vector<uint8_t> unresolved_;
};

bool splitSite(llvm::StringRef site, llvm::StringRef &file, uint32_t &line,
               uint32_t &col) {
  const size_t c2 = site.rfind(':');
  if (c2 == llvm::StringRef::npos || c2 == 0)
    return false;
  const size_t c1 = site.rfind(':', c2);
  if (c1 == llvm::StringRef::npos)
    return false;
  if (site.substr(c1 + 1, c2 - c1 - 1).getAsInteger(10, line) ||
      site.substr(c2 + 1).getAsInteger(10, col))
    return false;
  file = site.substr(0, c1);
  return true;
}

/// Whether a USR names a template instantiation or a member of a class
/// template: code emitted into whichever object instantiates it. Looks at
/// the declaration context and the function's own name only — template
/// arguments also appear in parameter types, which say nothing.
///
/// A USR spells a function as `<context>@F@<name>[<#arg...>]#param...#`.
/// The function's own `@F@` is the first one: a later one sits inside a
/// template argument or a parameter type (a lambda or local class names
/// its enclosing function).
bool templateLikeUsr(llvm::StringRef usr) {
  if (usr.contains("@FT@")) // a function template's pattern
    return true;
  const size_t fn = usr.find("@F@");
  if (fn == llvm::StringRef::npos)
    return false;
  llvm::StringRef context = usr.substr(0, fn);
  if (context.contains(">#") || context.contains("@ST>") ||
      context.contains("@SP>"))
    return true;
  // The name runs to the first '#'. Template arguments open with "<#",
  // so the name is an instantiation's when that '#' follows a '<'.
  llvm::StringRef name = usr.substr(fn + 3);
  const size_t hash = name.find('#');
  if (hash == llvm::StringRef::npos || hash == 0 || name[hash - 1] != '<')
    return false;
  if (!name.consume_front("operator"))
    return true;
  // An operator's own spelling may end in '<'. "operator<#" is operator<
  // itself; "operator<<#" is operator<< or an instantiation of operator<,
  // and is read as the former (every stream inserter is one).
  llvm::StringRef op = name.substr(0, hash - 8 - 1);
  return !op.empty() && op != "<";
}

const char *callEdgeKindName(uint8_t kind) {
  return edgeKindToString(static_cast<EdgeKind>(kind));
}

// ---------------------------------------------------------------------------
// File attribution, shared by both producers
// ---------------------------------------------------------------------------

/// What the map and the index meta say about each file.
struct FileAttrs {
  /// The target set whose objects hold code written in the file, or 0 when
  /// it is compiled into whoever includes it.
  std::vector<uint32_t> fileSet;
  /// The target set a shared header belongs to, for declarations without a
  /// visible definition: listed, else voted, else by stem (0 none).
  std::vector<uint32_t> headerOwner;
  /// The one target set with a source named like the header that includes
  /// it (0 none seen, kNoIndex several).
  std::vector<uint32_t> stemOwner;
  std::vector<uint8_t> isSource;   // a source the map lists
  /// 0 not in the index, 1 parsed cleanly, 2 parse not clean.
  std::vector<uint8_t> fileStatus;
  std::vector<uint8_t> unmappedTu; // an indexed TU no target lists

  void grow(size_t n) {
    fileSet.resize(n, 0);
    headerOwner.resize(n, 0);
    stemOwner.resize(n, 0);
    isSource.resize(n, 0);
    fileStatus.resize(n, 0);
    unmappedTu.resize(n, 0);
  }
  /// The target set a translation unit's objects belong to, or 0.
  [[nodiscard]] uint32_t tuSet(uint32_t f) const noexcept {
    return f < isSource.size() && isSource[f] ? fileSet[f] : 0;
  }
};

/// Files the map names (sources anchor, headers carry ownership), then the
/// index meta: per-TU outcomes, headers private to one target set, stem
/// affinity. Fills the coverage planes and source counts of `lf`.
void attributeFiles(const TargetMap &map, const SnapshotMeta *meta,
                    SetTable &sets, FileTable &files, FileAttrs &fa,
                    LinkFacts &lf) {
  const uint32_t targetCount = map.size();
  std::vector<uint32_t> scratchSet;
  auto assignListed = [&](bool sourcesNotHeaders, std::vector<uint32_t> &attr) {
    std::vector<std::pair<uint32_t, uint32_t>> listed; // (file, target)
    for (uint32_t t = 0; t < targetCount; ++t)
      for (const std::string &p :
           sourcesNotHeaders ? map.sourcesOf(t) : map.headersOf(t))
        listed.emplace_back(files.add(p), t);
    fa.grow(files.paths.size());
    std::sort(listed.begin(), listed.end());
    for (size_t i = 0; i < listed.size();) {
      size_t j = i;
      scratchSet.clear();
      for (; j < listed.size() && listed[j].first == listed[i].first; ++j)
        scratchSet.push_back(listed[j].second);
      attr[listed[i].first] = sets.intern(scratchSet);
      i = j;
    }
  };
  assignListed(true, fa.fileSet);
  for (uint32_t f = 0; f < fa.fileSet.size(); ++f)
    fa.isSource[f] = fa.fileSet[f] != 0;
  assignListed(false, fa.headerOwner);

  if (meta) {
    const size_t tuCount = meta->files.size();
    std::vector<uint32_t> tuFile(tuCount), depFile(meta->deps.size());
    for (size_t i = 0; i < tuCount; ++i)
      tuFile[i] = files.add(
          normalizeAuditPath(toIndexText(meta->files[i].path), ""));
    for (size_t j = 0; j < depFile.size(); ++j)
      depFile[j] = files.add(
          normalizeAuditPath(toIndexText(meta->deps[j].path), ""));
    fa.grow(files.paths.size());

    constexpr uint32_t kUnset = kNoIndex;
    std::vector<uint32_t> depSet(depFile.size(), kUnset);
    std::vector<uint8_t> depClean(depFile.size(), 1);
    for (size_t i = 0; i < tuCount; ++i) {
      const bool clean = i < meta->outcomes.size() &&
                         meta->outcomes[i].status == TuStatus::Indexed;
      const uint32_t tuSet = fa.tuSet(tuFile[i]);
      if (tuSet == 0) {
        ++lf.counts.unmappedTus;
        fa.unmappedTu[tuFile[i]] = 1;
      }
      fa.fileStatus[tuFile[i]] = clean ? 1 : 2;
      if (i >= meta->tuDeps.size())
        continue;
      for (uint32_t j : meta->tuDeps[i]) {
        if (j >= depSet.size())
          continue;
        if (depSet[j] == kUnset)
          depSet[j] = tuSet;
        else if (depSet[j] != tuSet)
          depSet[j] = 0;
        if (!clean)
          depClean[j] = 0;
      }
    }
    for (size_t j = 0; j < depFile.size(); ++j) {
      const uint32_t f = depFile[j];
      if (depSet[j] == kUnset)
        continue;
      if (fa.isSource[f]) {
        // A mapped source that is not a TU of its own but is included by
        // one (a unity build): as covered as its includers.
        if (fa.fileStatus[f] == 0)
          fa.fileStatus[f] = depClean[j] ? 1 : 2;
      } else if (depSet[j] != 0 && map.underRoots(files.paths[f])) {
        // Opened only by TUs of one target set, and the build's own: its
        // code is compiled into that set's objects and nowhere else.
        fa.fileSet[f] = depSet[j];
      }
    }

    // Stem affinity: a shared header named like a source that includes it
    // (tokenizer.hpp and tokenizer.cpp) most likely declares what that
    // source defines. Used only when nothing better names an owner.
    std::vector<uint64_t> stem(files.paths.size());
    for (size_t f = 0; f < stem.size(); ++f)
      stem[f] = llvm::xxh3_64bits(llvm::sys::path::stem(files.paths[f]));
    for (size_t i = 0; i < tuCount && i < meta->tuDeps.size(); ++i) {
      const uint32_t tuSet = fa.tuSet(tuFile[i]);
      if (tuSet == 0)
        continue;
      for (uint32_t j : meta->tuDeps[i]) {
        if (j >= depFile.size())
          continue;
        const uint32_t f = depFile[j];
        if (fa.isSource[f] || stem[f] != stem[tuFile[i]])
          continue;
        if (fa.stemOwner[f] == 0)
          fa.stemOwner[f] = tuSet;
        else if (fa.stemOwner[f] != tuSet)
          fa.stemOwner[f] = kNoIndex;
      }
    }
  }

  lf.targetSources.assign(targetCount, 0);
  lf.targetIndexed.assign(targetCount, 0);
  lf.targetCovered.assign(targetCount, 0);
  for (uint32_t t = 0; t < targetCount; ++t) {
    for (const std::string &p : map.sourcesOf(t)) {
      ++lf.targetSources[t];
      if (fa.fileStatus[files.byPath.lookup(p)] == 1)
        ++lf.targetIndexed[t];
    }
    lf.targetCovered[t] = meta && lf.targetSources[t] != 0 &&
                          lf.targetIndexed[t] == lf.targetSources[t];
  }
  for (uint32_t f = 0; f < fa.isSource.size(); ++f) {
    if (!fa.isSource[f])
      continue;
    ++lf.counts.mappedSources;
    lf.counts.indexedSources += fa.fileStatus[f] == 1;
    lf.counts.failedSources += fa.fileStatus[f] == 2;
  }
}

/// Header affinity: a shared header nobody lists belongs, for the symbols
/// it declares without a visible definition, to the target set that
/// defines the most of the symbols it declares (`votes`: (header, owner
/// set) per defined symbol); failing that, to the one whose source shares
/// its name.
void settleHeaderOwners(std::vector<std::pair<uint32_t, uint32_t>> &votes,
                        SetTable &sets, const FileTable &files,
                        const TargetMap &map, FileAttrs &fa) {
  std::vector<uint32_t> scratchSet;
  std::sort(votes.begin(), votes.end());
  for (size_t i = 0; i < votes.size();) {
    const uint32_t header = votes[i].first;
    const size_t first = i;
    size_t bestCount = 0;
    while (i < votes.size() && votes[i].first == header) {
      size_t j = i;
      while (j < votes.size() && votes[j] == votes[i])
        ++j;
      bestCount = std::max(bestCount, j - i);
      i = j;
    }
    // Every set with the most votes: one winner owns the header; on a tie
    // nothing tells the tied sets apart, so the header is presumed to
    // belong to all of them.
    scratchSet.clear();
    for (size_t k = first; k < i;) {
      size_t j = k;
      while (j < i && votes[j] == votes[k])
        ++j;
      if (j - k == bestCount)
        for (uint32_t t : sets.of(votes[k].second))
          scratchSet.push_back(t);
      k = j;
    }
    std::sort(scratchSet.begin(), scratchSet.end());
    scratchSet.erase(std::unique(scratchSet.begin(), scratchSet.end()),
                     scratchSet.end());
    fa.headerOwner[header] = sets.intern(scratchSet);
  }
  for (uint32_t f = 0; f < fa.headerOwner.size(); ++f)
    if (fa.headerOwner[f] == 0 && fa.fileSet[f] == 0 &&
        fa.stemOwner[f] != 0 && fa.stemOwner[f] != kNoIndex &&
        map.underRoots(files.paths[f]))
      fa.headerOwner[f] = fa.stemOwner[f];
}

} // namespace

// ---------------------------------------------------------------------------
// Call edges -> link facts (an index without reference facts)
// ---------------------------------------------------------------------------

LinkFacts linkFactsFromCallEdges(const CallGraph &graph, const TargetMap &map,
                                 const SnapshotMeta *meta) {
  LinkFacts lf;
  lf.edgeSource = "call-edges";
  lf.invisible = {"variable-references",
                  "vtable-and-typeinfo-references",
                  "static-initializer-calls",
                  "implicit-destructor-calls",
                  "calls-in-template-bodies",
                  "definitions-without-calls",
                  "name-based-lookup",
                  "other-build-configurations"};
  lf.refKindName = callEdgeKindName;
  const StringInterner &strings = graph.interner();
  lf.strings = &strings;
  lf.hasIndexMeta = meta != nullptr;
  lf.edgesCollapsed = meta != nullptr && !meta->collapsePaths.empty();
  if (lf.edgesCollapsed)
    lf.invisible.push_back("collapsed-call-edges");

  const uint32_t targetCount = map.size();
  lf.targetCount = targetCount;
  SetTable sets(targetCount);
  FileTable files;
  FileAttrs fa;
  attributeFiles(map, meta, sets, files, fa, lf);
  std::vector<uint32_t> &fileSet = fa.fileSet, &headerOwner = fa.headerOwner;
  std::vector<uint8_t> &unmappedTu = fa.unmappedTu;
  std::vector<uint32_t> scratchSet;

  FileResolver resolver(files);
  resolver.freezeKnown();

  // --- Symbols: every node, then every edge endpoint without one ----------
  std::vector<uint32_t> symOfId(strings.size(), kNoIndex);
  graph.forEachNode([&](SId usr, const CallGraphNode &node) {
    symOfId[usr] = static_cast<uint32_t>(lf.symUsr.size());
    lf.symUsr.push_back(usr);
    lf.symNode.push_back(&node);
  });
  auto symbolOf = [&](SId usr) {
    uint32_t &s = symOfId[usr];
    if (s == kNoIndex) {
      s = static_cast<uint32_t>(lf.symUsr.size());
      lf.symUsr.push_back(usr);
      lf.symNode.push_back(nullptr);
    }
    return s;
  };

  // --- Raw references, in storage order -----------------------------------
  // Every stored edge is kept: whatever it is, it shows that its caller
  // has a body in the site's file. rawSoft marks the ones that are no
  // link-time reference (kRawSoft) or no reference at all (kRawSelf).
  enum : uint8_t { kRawHard = 0, kRawSoft = 1, kRawSelf = 2 };
  std::vector<uint32_t> rawFrom, rawTo, rawFile;
  std::vector<uint64_t> rawLineCol;
  std::vector<SId> rawSite;
  std::vector<uint8_t> rawKind, rawSoft;
  {
    const size_t expected = graph.edgeCount();
    rawFrom.reserve(expected);
    rawTo.reserve(expected);
    rawFile.reserve(expected);
    rawLineCol.reserve(expected);
    rawSite.reserve(expected);
    rawKind.reserve(expected);
    rawSoft.reserve(expected);
  }
  graph.forEachStoredEdge([&](const CallGraph::EdgeRef &e) {
    uint8_t soft = kRawHard;
    if (e.caller == e.callee) {
      soft = kRawSelf;
    } else if (e.kind == EdgeKind::VirtualDispatch &&
               e.confidence != Confidence::Proven) {
      // A call through a vtable slot needs no definition of its static
      // target at link time; the vtable does, and that reference is
      // recorded where the object is constructed (the Proven edges). The
      // index gives a qualified call (`Base::f()`), which does need the
      // definition, the same kind: soft, so it still counts as a use.
      soft = kRawSoft;
      ++lf.counts.skippedVirtualCalls;
    } else if (e.kind == EdgeKind::FunctionPointerReturn) {
      // The callee is the function that returns the pointer, joined at
      // query time; the pointer's own address-of is its own edge.
      soft = kRawSoft;
      ++lf.counts.skippedDeferred;
    }
    llvm::StringRef file;
    uint32_t line = 0, col = 0;
    uint32_t fileId = kNoIndex;
    if (splitSite(strings.resolve(e.callSite), file, line, col))
      fileId = resolver.resolve(file);
    const uint32_t from = symbolOf(e.caller);
    const uint32_t to = symbolOf(e.callee);
    rawFrom.push_back(from);
    rawTo.push_back(to);
    rawFile.push_back(fileId);
    rawLineCol.push_back((static_cast<uint64_t>(line) << 32) | col);
    rawSite.push_back(e.callSite);
    rawKind.push_back(static_cast<uint8_t>(e.kind));
    rawSoft.push_back(soft);
  });
  const uint32_t symbolCount = lf.symbolCount();
  const size_t rawCount = rawFrom.size();

  // Declaration file per symbol (kNoIndex without a node).
  std::vector<uint32_t> declFile(symbolCount, kNoIndex);
  for (uint32_t s = 0; s < symbolCount; ++s)
    if (lf.symNode[s])
      declFile[s] = resolver.resolve(lf.symNode[s]->file);

  fa.grow(files.paths.size());
  const uint32_t fileCount = static_cast<uint32_t>(files.paths.size());
  std::vector<uint32_t> fileRank(fileCount);
  {
    std::vector<uint32_t> order(fileCount);
    std::iota(order.begin(), order.end(), 0u);
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
      return files.paths[a] < files.paths[b];
    });
    for (uint32_t r = 0; r < fileCount; ++r)
      fileRank[order[r]] = r;
  }

  // --- Group the raw references by referencing symbol (counting sort) -----
  std::vector<uint32_t> rawBegin(symbolCount + 1, 0);
  for (size_t r = 0; r < rawCount; ++r)
    ++rawBegin[rawFrom[r] + 1];
  for (uint32_t s = 0; s < symbolCount; ++s)
    rawBegin[s + 1] += rawBegin[s];
  std::vector<uint32_t> byFrom(rawCount);
  {
    std::vector<uint32_t> cursor(rawBegin.begin(), rawBegin.end() - 1);
    for (size_t r = 0; r < rawCount; ++r)
      byFrom[cursor[rawFrom[r]]++] = static_cast<uint32_t>(r);
  }
  std::vector<uint64_t> rawKey(rawCount);
  for (size_t r = 0; r < rawCount; ++r)
    rawKey[r] = siteKey(rawFile[r] == kNoIndex ? 0 : fileRank[rawFile[r]],
                        static_cast<uint32_t>(rawLineCol[r] >> 32),
                        static_cast<uint32_t>(rawLineCol[r]));
  auto siteSetOf = [&](size_t r) {
    return rawFile[r] == kNoIndex ? 0u : fileSet[rawFile[r]];
  };

  // --- Symbol classes ------------------------------------------------------
  lf.symClass.assign(symbolCount, SymbolClass::Unattributed);
  lf.symOwner.assign(symbolCount, 0);
  lf.symFile.assign(symbolCount, kNoIndex);
  lf.symUnplaced.assign(symbolCount, 0);

  // 1. A reference held by a target's own file: the body is there.
  // 2. Else declared in such a file (a function with no calls in it).
  std::vector<std::pair<uint32_t, uint32_t>> votes; // (header, owner set)
  // unmappedBody[s]: s makes a call written in an indexed translation
  // unit no target lists, and none in a target's file.
  std::vector<uint8_t> unmappedBody(symbolCount, 0);
  for (uint32_t s = 0; s < symbolCount; ++s) {
    uint32_t owner = 0, bodyFile = kNoIndex;
    uint64_t bestKey = ~0ull;
    bool mixed = false, inUnmappedTu = false;
    for (uint32_t i = rawBegin[s]; i < rawBegin[s + 1]; ++i) {
      const uint32_t r = byFrom[i];
      const uint32_t set = siteSetOf(r);
      if (set == 0) {
        inUnmappedTu |= rawFile[r] != kNoIndex && rawFile[r] < unmappedTu.size() &&
                        unmappedTu[rawFile[r]];
        continue;
      }
      if (owner == 0)
        owner = set;
      else if (owner != set)
        mixed = true;
      if (rawKey[r] < bestKey) {
        bestKey = rawKey[r];
        bodyFile = rawFile[r];
      }
    }
    if (mixed) {
      // One identity with bodies in several targets' files (same-named
      // functions in unnamed namespaces share a USR): owned by all.
      scratchSet.clear();
      for (uint32_t i = rawBegin[s]; i < rawBegin[s + 1]; ++i)
        for (uint32_t t : sets.of(siteSetOf(byFrom[i])))
          scratchSet.push_back(t);
      std::sort(scratchSet.begin(), scratchSet.end());
      scratchSet.erase(std::unique(scratchSet.begin(), scratchSet.end()),
                       scratchSet.end());
      owner = sets.intern(scratchSet);
    }
    const uint32_t decl = declFile[s];
    if (owner != 0) {
      lf.symClass[s] = SymbolClass::Defined;
      lf.symOwner[s] = owner;
      lf.symFile[s] = bodyFile;
      if (decl != kNoIndex && fileSet[decl] == 0 && headerOwner[decl] == 0 &&
          map.underRoots(files.paths[decl]))
        votes.emplace_back(decl, owner);
    } else if (inUnmappedTu) {
      unmappedBody[s] = 1;
    } else if (decl != kNoIndex && fileSet[decl] != 0) {
      lf.symClass[s] = SymbolClass::Defined;
      lf.symOwner[s] = fileSet[decl];
      lf.symFile[s] = decl;
    }
  }

  settleHeaderOwners(votes, sets, files, map, fa);

  // 3. Calls only at shared sites: an inline body. 4. No calls at all:
  //    template code is inline; otherwise the declaring header's owner is
  //    presumed.
  for (uint32_t s = 0; s < symbolCount; ++s) {
    if (lf.symClass[s] == SymbolClass::Defined) {
      ++lf.counts.defined;
      continue;
    }
    const uint32_t decl = declFile[s];
    if (unmappedBody[s]) {
      // Code of a source no target compiles: neither a provider's symbol
      // nor a carrier of references.
      ++lf.counts.unattributed;
    } else if (rawBegin[s + 1] != rawBegin[s] ||
               templateLikeUsr(strings.resolve(lf.symUsr[s]))) {
      lf.symClass[s] = SymbolClass::Inline;
      // Whose header it is, for indirect uses; no link owner.
      if (decl != kNoIndex)
        lf.symOwner[s] = headerOwner[decl];
      ++lf.counts.inlined;
    } else if (decl != kNoIndex && headerOwner[decl] != 0) {
      lf.symClass[s] = SymbolClass::Presumed;
      lf.symOwner[s] = headerOwner[decl];
      ++lf.counts.presumed;
    } else {
      ++lf.counts.unattributed;
      lf.symUnplaced[s] = decl != kNoIndex && !resolver.unresolved(decl) &&
                          !(decl < unmappedTu.size() && unmappedTu[decl]) &&
                          map.underRoots(files.paths[decl]);
    }
  }

  // --- Final references: anchored, deduplicated, canonically ordered ------
  // A reference at a shared site inside a Defined function (a macro body
  // from a header) is held by that function's objects.
  auto anchorOf = [&](uint32_t r) {
    const uint32_t set = siteSetOf(r);
    if (set != 0)
      return set;
    const uint32_t from = rawFrom[r];
    return lf.symClass[from] == SymbolClass::Defined ? lf.symOwner[from] : 0u;
  };
  lf.refBegin.assign(symbolCount + 1, 0);
  lf.refTo.reserve(rawCount);
  lf.refAnchor.reserve(rawCount);
  lf.refKey.reserve(rawCount);
  lf.refSite.reserve(rawCount);
  lf.refKind.reserve(rawCount);
  lf.softBegin.assign(symbolCount + 1, 0);
  std::vector<uint32_t> range, rangeAnchor;
  std::vector<std::pair<uint32_t, uint32_t>> softPairs; // (to, anchor)
  for (uint32_t s = 0; s < symbolCount; ++s) {
    range.clear();
    softPairs.clear();
    if (unmappedBody[s]) {
      lf.counts.unmappedBodyRefs += rawBegin[s + 1] - rawBegin[s];
    } else {
      for (uint32_t i = rawBegin[s]; i < rawBegin[s + 1]; ++i) {
        const uint32_t r = byFrom[i];
        if (rawSoft[r] == kRawHard)
          range.push_back(r);
        else if (rawSoft[r] == kRawSoft)
          softPairs.emplace_back(rawTo[r], anchorOf(r));
      }
    }
    std::sort(softPairs.begin(), softPairs.end());
    softPairs.erase(std::unique(softPairs.begin(), softPairs.end()),
                    softPairs.end());
    for (const auto &p : softPairs) {
      lf.softTo.push_back(p.first);
      lf.softAnchor.push_back(p.second);
    }
    lf.softBegin[s + 1] = static_cast<uint32_t>(lf.softTo.size());
    // Anchors index by raw reference; a per-range side table keeps the
    // comparator free of the class lookup.
    rangeAnchor.resize(range.size());
    for (size_t i = 0; i < range.size(); ++i)
      rangeAnchor[i] = anchorOf(range[i]);
    std::vector<uint32_t> &order = scratchSet; // positions into range
    order.resize(range.size());
    std::iota(order.begin(), order.end(), 0u);
    // One reference per (to, anchor): the first site, in site order.
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
      const uint32_t ra = range[a], rb = range[b];
      if (rawTo[ra] != rawTo[rb])
        return rawTo[ra] < rawTo[rb];
      if (rangeAnchor[a] != rangeAnchor[b])
        return rangeAnchor[a] < rangeAnchor[b];
      if (rawKey[ra] != rawKey[rb])
        return rawKey[ra] < rawKey[rb];
      if (rawKind[ra] != rawKind[rb])
        return rawKind[ra] < rawKind[rb];
      // Sites whose files share a rank (unresolved spellings): the text.
      return rawSite[ra] != rawSite[rb] &&
             strings.resolve(rawSite[ra]) < strings.resolve(rawSite[rb]);
    });
    order.erase(std::unique(order.begin(), order.end(),
                            [&](uint32_t a, uint32_t b) {
                              return rawTo[range[a]] == rawTo[range[b]] &&
                                     rangeAnchor[a] == rangeAnchor[b];
                            }),
                order.end());
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
      const uint32_t ra = range[a], rb = range[b];
      if (rawKey[ra] != rawKey[rb])
        return rawKey[ra] < rawKey[rb];
      if (rawKind[ra] != rawKind[rb])
        return rawKind[ra] < rawKind[rb];
      return strings.resolve(lf.symUsr[rawTo[ra]]) <
             strings.resolve(lf.symUsr[rawTo[rb]]);
    });
    for (uint32_t pos : order) {
      const uint32_t r = range[pos];
      lf.refTo.push_back(rawTo[r]);
      lf.refAnchor.push_back(rangeAnchor[pos]);
      lf.refKey.push_back(rawKey[r]);
      lf.refSite.push_back(rawSite[r]);
      lf.refKind.push_back(rawKind[r]);
      if (rangeAnchor[pos] != 0)
        ++lf.counts.anchoredRefs;
      else
        ++lf.counts.inlineBodyRefs;
      if (rawFile[r] == kNoIndex || resolver.unresolved(rawFile[r]))
        ++lf.counts.unresolvedSites;
    }
    lf.refBegin[s + 1] = static_cast<uint32_t>(lf.refTo.size());
  }

  lf.symDeclFile = std::move(declFile);
  lf.symFlags.assign(symbolCount, 0);
  lf.files = std::move(files.paths);
  lf.setBegin = std::move(sets.begin);
  lf.setTargets = std::move(sets.targets);
  return lf;
}

// ---------------------------------------------------------------------------
// Reference facts -> link facts
// ---------------------------------------------------------------------------

namespace {

// Reference kinds the producer adds, numbered after the RefRole values.
constexpr uint8_t kRefVtableEntry = kRefRoleCount; // vtable -> virtual function
constexpr uint8_t kRefBaseClass = kRefRoleCount + 1; // vtable -> base's vtable
constexpr uint8_t kRefEnclosed = kRefRoleCount + 2;  // function -> its lambda

const char *referenceKindName(uint8_t kind) {
  if (kind < kRefRoleCount)
    return refRoleName(static_cast<RefRole>(kind));
  switch (kind) {
  case kRefVtableEntry:
    return "vtable_entry";
  case kRefBaseClass:
    return "base_class";
  case kRefEnclosed:
    return "enclosed_lambda";
  default:
    return "unknown";
  }
}

/// One stored symbol record, flattened.
struct SymRec {
  SId usr, name, file, minTu;
  uint32_t line, flags, tus;
  SymbolKind kind;
  LinkageClass linkage;
};

/// One reference before anchoring: a stored record or one the producer adds.
struct RawRef {
  uint32_t from, to;
  uint32_t file;    // file id, kNoIndex unknown
  SId siteFile;     // the site's path in the reference index, or kNoId
  uint32_t line, col;
  uint32_t tus;     // contributing TUs (stored records), else 0
  SId minTu;
  uint8_t kind;
};

} // namespace

LinkFacts linkFactsFromReferences(const ReferenceIndex &refs,
                                  const CallGraph *graph,
                                  const TargetMap &map,
                                  const SnapshotMeta *meta) {
  constexpr SId kNoId = ReferenceIndex::kNoId;
  LinkFacts lf;
  lf.edgeSource = "references";
  // docs/dependency-audit.md, "Completeness".
  lf.invisible = {"implicit-special-member-calls",
                  "template-instantiation-bodies",
                  "name-based-lookup",
                  "qualified-virtual-calls",
                  "same-basename-internal-symbols",
                  "other-build-configurations"};
  lf.refKindName = referenceKindName;
  lf.ownedStrings = std::make_unique<StringInterner>();
  StringInterner &out = *lf.ownedStrings;
  lf.strings = &out;
  lf.hasIndexMeta = meta != nullptr;
  // The reference facts are recorded whatever --collapse-paths says.
  lf.edgesCollapsed = false;
  const StringInterner &rs = refs.strings();

  const uint32_t targetCount = map.size();
  lf.targetCount = targetCount;
  SetTable sets(targetCount);
  FileTable files;
  FileAttrs fa;
  attributeFiles(map, meta, sets, files, fa, lf);
  FileResolver resolver(files);
  resolver.freezeKnown();

  // --- Records, flattened --------------------------------------------------
  std::vector<SymRec> srec;
  srec.reserve(refs.symbolRecordCount());
  refs.forEachSymbolRecord([&](const ReferenceIndex::SymbolRecord &r,
                               uint32_t tus, SId minTu) {
    srec.push_back({r.usr, r.name, r.file, minTu, r.line, r.flags, tus,
                    r.kind, r.linkage});
  });
  struct RefRec {
    ReferenceIndex::ReferenceRecord rec;
    uint32_t tus;
    SId minTu;
  };
  std::vector<RefRec> rrec;
  refs.forEachReferenceRecord([&](const ReferenceIndex::ReferenceRecord &r,
                                  uint32_t tus, SId minTu) {
    rrec.push_back({r, tus, minTu});
  });

  // --- Symbols, in USR order -----------------------------------------------
  // Ordering the symbols by USR once makes every later tie-break by symbol
  // index a tie-break by USR, whatever order the records were stored in.
  const size_t idCount = rs.size();
  std::vector<uint32_t> symOfId(idCount, kNoIndex);
  std::vector<SId> usrs;
  auto note = [&](SId usr) {
    if (symOfId[usr] == kNoIndex) {
      symOfId[usr] = 0;
      usrs.push_back(usr);
    }
  };
  for (const SymRec &r : srec)
    note(r.usr);
  for (const RefRec &r : rrec) {
    note(r.rec.referrer);
    note(r.rec.target);
  }
  std::sort(usrs.begin(), usrs.end(), [&](SId a, SId b) {
    return rs.resolve(a) < rs.resolve(b);
  });
  const auto symbolCount = static_cast<uint32_t>(usrs.size());
  for (uint32_t i = 0; i < symbolCount; ++i)
    symOfId[usrs[i]] = i;

  // Records grouped by symbol (counting sort).
  std::vector<uint32_t> recBegin(symbolCount + 1, 0), recOrder(srec.size());
  for (const SymRec &r : srec)
    ++recBegin[symOfId[r.usr] + 1];
  for (uint32_t s = 0; s < symbolCount; ++s)
    recBegin[s + 1] += recBegin[s];
  {
    std::vector<uint32_t> cursor(recBegin.begin(), recBegin.end() - 1);
    for (uint32_t i = 0; i < srec.size(); ++i)
      recOrder[cursor[symOfId[srec[i].usr]]++] = i;
  }

  // --- Files: every path a record names, resolved once ----------------------
  constexpr uint32_t kNotYet = kNoIndex - 1;
  std::vector<uint32_t> fileOfId(idCount, kNotYet);
  auto fileOf = [&](SId id) {
    if (id == kNoId)
      return kNoIndex;
    uint32_t &f = fileOfId[id];
    if (f == kNotYet)
      f = resolver.resolve(rs.resolve(id));
    return f;
  };
  for (const SymRec &r : srec) {
    fileOf(r.file);
    fileOf(r.minTu);
  }
  for (const RefRec &r : rrec) {
    fileOf(r.rec.file);
    fileOf(r.minTu);
  }
  fa.grow(files.paths.size());
  const auto fileCount = static_cast<uint32_t>(files.paths.size());
  std::vector<uint32_t> fileRank(fileCount);
  {
    std::vector<uint32_t> order(fileCount);
    std::iota(order.begin(), order.end(), 0u);
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
      return files.paths[a] < files.paths[b];
    });
    for (uint32_t r = 0; r < fileCount; ++r)
      fileRank[order[r]] = r;
  }
  auto rankOf = [&](uint32_t f) { return f == kNoIndex ? 0u : fileRank[f]; };
  /// Who owns a header's declarations: listed or voted, else the target
  /// set it is private to.
  auto headerOwnerOf = [&](uint32_t f) -> uint32_t {
    if (f == kNoIndex || fa.isSource[f])
      return 0;
    return fa.headerOwner[f] != 0 ? fa.headerOwner[f] : fa.fileSet[f];
  };
  auto allCovered = [&](uint32_t set) {
    for (uint32_t t : sets.of(set))
      if (!lf.targetCovered[t])
        return false;
    return true;
  };

  // --- Per symbol: merge the records ----------------------------------------
  lf.symUsr.resize(symbolCount);
  lf.symName.resize(symbolCount);
  lf.symNode.assign(symbolCount, nullptr);
  lf.symClass.assign(symbolCount, SymbolClass::Unattributed);
  lf.symOwner.assign(symbolCount, 0);
  lf.symFile.assign(symbolCount, kNoIndex);
  lf.symUnplaced.assign(symbolCount, 0);
  lf.symDeclFile.assign(symbolCount, kNoIndex);
  lf.symDeclLine.assign(symbolCount, 0);
  lf.symFlags.assign(symbolCount, 0);
  lf.symKind.assign(symbolCount, static_cast<uint8_t>(SymbolKind::Function));
  std::vector<uint32_t> flagsOf(symbolCount, 0);
  std::vector<SId> declSiteFile(symbolCount, kNoId); // in the reference index
  // Undecided after the first pass: 0 decided, 1 no definition seen,
  // 2 inline, 3 defined only in a TU no target lists.
  enum : uint8_t { kDecided = 0, kUndefined = 1, kInline = 2, kUnmapped = 3 };
  std::vector<uint8_t> pending(symbolCount, kDecided);
  std::vector<uint8_t> unmappedBody(symbolCount, 0);
  std::vector<std::pair<uint32_t, uint32_t>> votes; // (header, owner set)
  std::vector<uint32_t> scratchSet;
  for (uint32_t s = 0; s < symbolCount; ++s) {
    lf.symUsr[s] = out.intern(rs.resolve(usrs[s]));
    const uint32_t rb = recBegin[s], re = recBegin[s + 1];
    if (rb == re) {
      // Named by a reference only (no record survived): an identity, no
      // more.
      lf.symName[s] = lf.symUsr[s];
      continue;
    }
    const std::string *name = nullptr;
    uint8_t kind = UINT8_MAX, linkage = UINT8_MAX, defLinkage = UINT8_MAX;
    uint32_t flags = 0;
    bool allSystem = true, anyDef = false, multiHeaderDef = false;
    bool unmappedDef = false, mainFileDef = false, unmappedHome = false;
    // The declaration to show: one in a header that is no definition, then
    // any other declaration, then a definition; by path and line within.
    uint32_t declPref = 3, declRank = 0, declLine = 0, declFile = kNoIndex;
    SId declPath = kNoId;
    uint32_t bodyFile = kNoIndex, homeFile = kNoIndex;
    scratchSet.clear();
    std::vector<uint32_t> homeTargets; // rarely more than one
    for (uint32_t i = rb; i < re; ++i) {
      const SymRec &r = srec[recOrder[i]];
      const std::string &recName = rs.resolve(r.name);
      if (!name || recName < *name)
        name = &recName;
      kind = std::min(kind, static_cast<uint8_t>(r.kind));
      linkage = std::min(linkage, static_cast<uint8_t>(r.linkage));
      flags |= r.flags & ~kSymbolPerDeclFlags;
      allSystem &= (r.flags & kSymSystemHeader) != 0;
      const bool definition = (r.flags & kSymDefinition) != 0;
      const bool inHeader = (r.flags & kSymInHeader) != 0;
      const uint32_t f = fileOf(r.file);
      const uint32_t pref = !definition ? (inHeader ? 0u : 1u) : 2u;
      const uint32_t rank = rankOf(f);
      if (pref < declPref || (pref == declPref && rank < declRank) ||
          (pref == declPref && rank == declRank && r.line < declLine)) {
        declPref = pref;
        declRank = rank;
        declLine = r.line;
        declFile = f;
        declPath = r.file;
      }
      if (r.flags & kSymVtableHome) {
        const uint32_t tuFile = fileOf(r.minTu);
        const uint32_t set = tuFile == kNoIndex ? 0 : fa.tuSet(tuFile);
        if (set != 0) {
          for (uint32_t t : sets.of(set))
            homeTargets.push_back(t);
          if (homeFile == kNoIndex || rankOf(tuFile) < rankOf(homeFile))
            homeFile = tuFile;
        } else {
          unmappedHome = true;
        }
      }
      if (!definition)
        continue;
      anyDef = true;
      defLinkage = std::min(defLinkage, static_cast<uint8_t>(r.linkage));
      uint32_t tuFile;
      if (!inHeader) {
        tuFile = f;
        mainFileDef = true;
      } else if (r.tus == 1 && r.minTu != kNoId) {
        tuFile = fileOf(r.minTu); // a textually included .inc, one TU
      } else {
        multiHeaderDef = true;
        continue;
      }
      const uint32_t set = tuFile == kNoIndex ? 0 : fa.tuSet(tuFile);
      if (set == 0) {
        unmappedDef = true;
        continue;
      }
      for (uint32_t t : sets.of(set))
        scratchSet.push_back(t);
      if (bodyFile == kNoIndex || rankOf(f) < rankOf(bodyFile))
        bodyFile = f;
    }
    lf.symName[s] = out.intern(*name);
    lf.symKind[s] = kind;
    lf.symDeclFile[s] = declFile;
    lf.symDeclLine[s] = declLine;
    declSiteFile[s] = declPath;
    flagsOf[s] = flags;
    const auto skind = static_cast<SymbolKind>(kind);
    const auto link = static_cast<LinkageClass>(
        defLinkage != UINT8_MAX ? defLinkage : linkage);
    auto internSet = [&](std::vector<uint32_t> &targets) {
      std::sort(targets.begin(), targets.end());
      targets.erase(std::unique(targets.begin(), targets.end()),
                    targets.end());
      return sets.intern(targets);
    };

    if (skind == SymbolKind::File || allSystem ||
        link == LinkageClass::None)
      continue; // Unattributed: a file scope, or the system's code
    if (flags & kSymLambda) {
      // Never a target; what it references is held where it is written.
      pending[s] = kInline;
      continue;
    }
    if (skind == SymbolKind::Class) {
      if (!homeTargets.empty()) {
        lf.symClass[s] = SymbolClass::Defined;
        lf.symOwner[s] = internSet(homeTargets);
        lf.symFile[s] = homeFile;
      } else if (unmappedHome) {
        pending[s] = kUnmapped;
      } else if (link == LinkageClass::External) {
        pending[s] = kUndefined; // a key function no indexed TU defines
      } else {
        pending[s] = kInline;
      }
      continue;
    }
    // A function or a variable.
    const bool strong =
        link == LinkageClass::External ||
        (link == LinkageClass::Internal && mainFileDef) ||
        ((flags & kSymExplicitInstantiation) && mainFileDef);
    if (!strong && anyDef) {
      pending[s] = kInline; // inline, template, or internal to a header
    } else if (!scratchSet.empty()) {
      lf.symClass[s] = SymbolClass::Defined;
      lf.symOwner[s] = internSet(scratchSet);
      lf.symFile[s] = bodyFile;
      if (declFile != kNoIndex && fa.fileSet[declFile] == 0 &&
          fa.headerOwner[declFile] == 0 && !fa.isSource[declFile] &&
          map.underRoots(files.paths[declFile]))
        votes.emplace_back(declFile, lf.symOwner[s]);
    } else if (unmappedDef) {
      pending[s] = kUnmapped;
    } else if (multiHeaderDef) {
      pending[s] = kInline; // one strong body in several TUs' headers
    } else if (link != LinkageClass::External ||
               templateLikeUsr(rs.resolve(usrs[s]))) {
      pending[s] = kInline;
    } else {
      pending[s] = kUndefined;
    }
  }
  settleHeaderOwners(votes, sets, files, map, fa);

  for (uint32_t s = 0; s < symbolCount; ++s) {
    const uint32_t decl = lf.symDeclFile[s];
    switch (pending[s]) {
    case kDecided:
      if (lf.symClass[s] == SymbolClass::Defined)
        ++lf.counts.defined;
      else
        ++lf.counts.unattributed;
      break;
    case kInline:
      lf.symClass[s] = SymbolClass::Inline;
      lf.symOwner[s] = headerOwnerOf(decl); // whose code, for indirect uses
      ++lf.counts.inlined;
      break;
    case kUnmapped:
      // Code of a source no target compiles: neither a provider's symbol
      // nor a carrier of references.
      unmappedBody[s] = 1;
      ++lf.counts.unattributed;
      break;
    case kUndefined: {
      const uint32_t owner = headerOwnerOf(decl);
      if (owner != 0 && !allCovered(owner)) {
        // Its owner's sources are not all indexed: the definition may be
        // in one that is not.
        lf.symClass[s] = SymbolClass::Presumed;
        lf.symOwner[s] = owner;
        ++lf.counts.presumed;
      } else {
        ++lf.counts.unattributed;
        if (owner != 0)
          ++lf.counts.undefinedInCoveredOwner;
        else
          lf.symUnplaced[s] = decl != kNoIndex && !(flagsOf[s] & kSymVirtual) &&
                              !resolver.unresolved(decl) &&
                              !fa.unmappedTu[decl] &&
                              map.underRoots(files.paths[decl]);
      }
      break;
    }
    }
  }

  // --- Raw references --------------------------------------------------------
  // Soft: a call of a virtual function the compiler dispatches through the
  // vtable. The call graph tells a dispatched call from one bound
  // statically (an object of known type); without it every call of a
  // virtual function is taken as dispatched.
  auto isVirtual = [&](uint32_t s) { return (flagsOf[s] & kSymVirtual) != 0; };
  llvm::DenseMap<uint64_t, uint8_t> boundCall; // (caller, callee) graph ids
  if (graph) {
    const StringInterner &gi = graph->interner();
    auto pairKey = [](SId a, SId b) {
      return (static_cast<uint64_t>(a) << 32) | b;
    };
    for (const RefRec &r : rrec) {
      if (r.rec.role != RefRole::Call || !isVirtual(symOfId[r.rec.target]))
        continue;
      auto caller = gi.find(rs.resolve(r.rec.referrer));
      auto callee = gi.find(rs.resolve(r.rec.target));
      if (caller && callee)
        boundCall.try_emplace(pairKey(*caller, *callee), 0);
    }
    if (!boundCall.empty())
      graph->forEachStoredEdge([&](const CallGraph::EdgeRef &e) {
        if (e.kind == EdgeKind::VirtualDispatch &&
            e.confidence != Confidence::Proven)
          return;
        auto it = boundCall.find(pairKey(e.caller, e.callee));
        if (it != boundCall.end())
          it->second = 1;
      });
    // Rekey by symbol pair for the pass below.
    llvm::DenseMap<uint64_t, uint8_t> bySym;
    for (const RefRec &r : rrec) {
      if (r.rec.role != RefRole::Call || !isVirtual(symOfId[r.rec.target]))
        continue;
      auto caller = gi.find(rs.resolve(r.rec.referrer));
      auto callee = gi.find(rs.resolve(r.rec.target));
      if (!caller || !callee)
        continue;
      auto it = boundCall.find(pairKey(*caller, *callee));
      if (it != boundCall.end() && it->second)
        bySym[pairKey(symOfId[r.rec.referrer], symOfId[r.rec.target])] = 1;
    }
    boundCall = std::move(bySym);
  }

  std::vector<RawRef> raw;
  std::vector<uint8_t> rawSoft;
  raw.reserve(rrec.size());
  rawSoft.reserve(rrec.size());
  for (const RefRec &r : rrec) {
    const uint32_t from = symOfId[r.rec.referrer], to = symOfId[r.rec.target];
    if (from == to)
      continue;
    uint8_t soft = 0;
    if (r.rec.role == RefRole::Call && isVirtual(to) &&
        boundCall.lookup((static_cast<uint64_t>(from) << 32) | to) == 0) {
      soft = 1;
      ++lf.counts.skippedVirtualCalls;
    }
    raw.push_back({from, to, fileOf(r.rec.file), r.rec.file, r.rec.line,
                   r.rec.col, r.tus, r.minTu,
                   static_cast<uint8_t>(r.rec.role)});
    rawSoft.push_back(soft);
  }

  // What a vtable holds: the class's virtual functions (a member's USR
  // extends its class's with "@F@"), and its bases' vtables.
  auto addSynthesized = [&](uint32_t from, uint32_t to, uint8_t kind,
                            uint32_t siteOf) {
    raw.push_back({from, to, lf.symDeclFile[siteOf], declSiteFile[siteOf],
                   lf.symDeclLine[siteOf], 0, 0, kNoId, kind});
    rawSoft.push_back(0);
  };
  for (uint32_t s = 0; s < symbolCount; ++s) {
    if (!isVirtual(s) ||
        lf.symKind[s] != static_cast<uint8_t>(SymbolKind::Function))
      continue;
    llvm::StringRef usr = rs.resolve(usrs[s]);
    const size_t at = usr.find("@F@");
    if (at == llvm::StringRef::npos)
      continue;
    auto cls = rs.find(usr.substr(0, at));
    if (!cls || symOfId[*cls] == kNoIndex)
      continue;
    addSynthesized(symOfId[*cls], s, kRefVtableEntry, s);
    ++lf.counts.vtableEntryRefs;
  }
  if (graph) {
    llvm::StringMap<llvm::SmallVector<uint32_t, 1>> classNamed;
    for (uint32_t s = 0; s < symbolCount; ++s)
      if (lf.symKind[s] == static_cast<uint8_t>(SymbolKind::Class) &&
          (flagsOf[s] & kSymDynamicClass))
        classNamed[out.resolve(lf.symName[s])].push_back(s);
    std::vector<std::pair<uint32_t, uint32_t>> bases; // (derived, base)
    for (const auto &entry : classNamed)
      for (const std::string &derived :
           graph->getDerivedClasses(entry.getKey().str())) {
        auto it = classNamed.find(derived);
        if (it == classNamed.end())
          continue;
        for (uint32_t d : it->second)
          for (uint32_t b : entry.getValue())
            if (d != b)
              bases.emplace_back(d, b);
      }
    std::sort(bases.begin(), bases.end());
    bases.erase(std::unique(bases.begin(), bases.end()), bases.end());
    for (const auto &db : bases) {
      addSynthesized(db.first, db.second, kRefBaseClass, db.first);
      ++lf.counts.baseClassRefs;
    }
    // A lambda is never named by a reference; the call graph knows which
    // function calls it, and that function carries it.
    const StringInterner &gi = graph->interner();
    for (uint32_t s = 0; s < symbolCount; ++s) {
      if (!(flagsOf[s] & kSymLambda))
        continue;
      auto id = gi.find(rs.resolve(usrs[s]));
      if (!id)
        continue;
      for (const CallGraph::EdgeRef &e : graph->callerRefsOf(*id)) {
        auto caller = rs.find(gi.resolve(e.caller));
        if (caller && symOfId[*caller] != kNoIndex &&
            symOfId[*caller] != s)
          addSynthesized(symOfId[*caller], s, kRefEnclosed, s);
      }
    }
  }

  // --- Anchors -----------------------------------------------------------
  // The target set whose objects hold a reference, 0 for one that travels
  // with an inline body, kNoIndex for one no target holds.
  auto anchorOf = [&](const RawRef &r) -> uint32_t {
    const uint32_t from = r.from;
    const uint32_t siteSet = r.file == kNoIndex ? 0 : fa.fileSet[r.file];
    if (flagsOf[from] & kSymLambda)
      return siteSet;
    if (lf.symKind[from] == static_cast<uint8_t>(SymbolKind::File)) {
      if (siteSet != 0)
        return siteSet;
      if (r.tus == 1 && r.minTu != kNoId) {
        const uint32_t tuFile = fileOf(r.minTu);
        if (tuFile != kNoIndex && fa.tuSet(tuFile) != 0)
          return fa.tuSet(tuFile);
      }
      ++lf.counts.fileScopeDropped;
      return kNoIndex;
    }
    switch (lf.symClass[from]) {
    case SymbolClass::Defined:
      return lf.symOwner[from];
    case SymbolClass::Inline:
      return 0;
    default:
      if (unmappedBody[from])
        ++lf.counts.unmappedBodyRefs;
      return kNoIndex;
    }
  };

  // --- Grouped by referencing symbol (counting sort) -------------------------
  const size_t rawCount = raw.size();
  std::vector<uint32_t> rawBegin(symbolCount + 1, 0);
  for (const RawRef &r : raw)
    ++rawBegin[r.from + 1];
  for (uint32_t s = 0; s < symbolCount; ++s)
    rawBegin[s + 1] += rawBegin[s];
  std::vector<uint32_t> byFrom(rawCount);
  {
    std::vector<uint32_t> cursor(rawBegin.begin(), rawBegin.end() - 1);
    for (size_t i = 0; i < rawCount; ++i)
      byFrom[cursor[raw[i].from]++] = static_cast<uint32_t>(i);
  }

  struct Kept {
    uint32_t to, anchor;
    uint64_t key;
    uint8_t kind;
    uint32_t raw;
  };
  std::vector<Kept> hard;
  std::vector<std::pair<uint32_t, uint32_t>> softPairs; // (to, anchor)
  lf.refBegin.assign(symbolCount + 1, 0);
  lf.softBegin.assign(symbolCount + 1, 0);
  lf.refTo.reserve(rawCount);
  lf.refAnchor.reserve(rawCount);
  lf.refKey.reserve(rawCount);
  lf.refSite.reserve(rawCount);
  lf.refKind.reserve(rawCount);
  std::string site;
  for (uint32_t s = 0; s < symbolCount; ++s) {
    hard.clear();
    softPairs.clear();
    for (uint32_t i = rawBegin[s]; i < rawBegin[s + 1]; ++i) {
      const RawRef &r = raw[byFrom[i]];
      const uint32_t anchor = anchorOf(r);
      if (anchor == kNoIndex)
        continue;
      if (rawSoft[byFrom[i]]) {
        softPairs.emplace_back(r.to, anchor);
        continue;
      }
      hard.push_back({r.to, anchor, siteKey(rankOf(r.file), r.line, r.col),
                      r.kind, byFrom[i]});
    }
    std::sort(softPairs.begin(), softPairs.end());
    softPairs.erase(std::unique(softPairs.begin(), softPairs.end()),
                    softPairs.end());
    for (const auto &p : softPairs) {
      lf.softTo.push_back(p.first);
      lf.softAnchor.push_back(p.second);
    }
    lf.softBegin[s + 1] = static_cast<uint32_t>(lf.softTo.size());
    // One reference per (to, anchor): the first site, in site order.
    std::sort(hard.begin(), hard.end(), [](const Kept &a, const Kept &b) {
      if (a.to != b.to)
        return a.to < b.to;
      if (a.anchor != b.anchor)
        return a.anchor < b.anchor;
      if (a.key != b.key)
        return a.key < b.key;
      return a.kind < b.kind;
    });
    hard.erase(std::unique(hard.begin(), hard.end(),
                           [](const Kept &a, const Kept &b) {
                             return a.to == b.to && a.anchor == b.anchor;
                           }),
               hard.end());
    // Symbol order is USR order, so `to` breaks ties by USR.
    std::sort(hard.begin(), hard.end(), [](const Kept &a, const Kept &b) {
      if (a.key != b.key)
        return a.key < b.key;
      if (a.kind != b.kind)
        return a.kind < b.kind;
      if (a.to != b.to)
        return a.to < b.to;
      return a.anchor < b.anchor;
    });
    for (const Kept &k : hard) {
      const RawRef &r = raw[k.raw];
      site.clear();
      if (r.siteFile != kNoId)
        site = rs.resolve(r.siteFile);
      site += ':';
      site += std::to_string(r.line);
      site += ':';
      site += std::to_string(r.col);
      lf.refTo.push_back(k.to);
      lf.refAnchor.push_back(k.anchor);
      lf.refKey.push_back(k.key);
      lf.refSite.push_back(out.intern(site));
      lf.refKind.push_back(k.kind);
      if (k.anchor != 0)
        ++lf.counts.anchoredRefs;
      else
        ++lf.counts.inlineBodyRefs;
      if (r.file == kNoIndex || resolver.unresolved(r.file))
        ++lf.counts.unresolvedSites;
    }
    lf.refBegin[s + 1] = static_cast<uint32_t>(lf.refTo.size());
  }

  // --- Registration and static initializers ----------------------------------
  // A registering symbol takes the address of a template's function in its
  // own body or in an inline body it reaches.
  {
    std::vector<uint32_t> revBegin(symbolCount + 1, 0), revFrom(lf.refTo.size());
    for (uint32_t to : lf.refTo)
      ++revBegin[to + 1];
    for (uint32_t s = 0; s < symbolCount; ++s)
      revBegin[s + 1] += revBegin[s];
    {
      std::vector<uint32_t> cursor(revBegin.begin(), revBegin.end() - 1);
      for (uint32_t s = 0; s < symbolCount; ++s)
        for (uint32_t r = lf.refBegin[s]; r < lf.refBegin[s + 1]; ++r)
          revFrom[cursor[lf.refTo[r]]++] = s;
    }
    std::vector<uint32_t> queue;
    queue.reserve(symbolCount);
    for (uint32_t s = 0; s < symbolCount; ++s) {
      bool registers = false, initializes = false;
      for (uint32_t r = lf.refBegin[s]; r < lf.refBegin[s + 1]; ++r) {
        const uint32_t to = lf.refTo[r];
        const uint8_t kind = lf.refKind[r];
        if (kind == static_cast<uint8_t>(RefRole::AddressOf) &&
            lf.symKind[to] == static_cast<uint8_t>(SymbolKind::Function) &&
            !(flagsOf[to] & kSymLambda) &&
            templateLikeUsr(out.resolve(lf.symUsr[to])))
          registers = true;
        if (kind == static_cast<uint8_t>(RefRole::Call))
          initializes = true;
      }
      if (registers) {
        lf.symFlags[s] |= kSymFlagRegisters;
        queue.push_back(s);
      }
      if (initializes &&
          lf.symKind[s] == static_cast<uint8_t>(SymbolKind::Variable) &&
          lf.symClass[s] == SymbolClass::Defined)
        lf.symFlags[s] |= kSymFlagStaticInit;
    }
    // Whoever uses a registering inline body registers too.
    for (size_t head = 0; head < queue.size(); ++head) {
      const uint32_t y = queue[head];
      if (lf.symClass[y] != SymbolClass::Inline)
        continue;
      for (uint32_t i = revBegin[y]; i < revBegin[y + 1]; ++i) {
        const uint32_t x = revFrom[i];
        if (lf.symFlags[x] & kSymFlagRegisters)
          continue;
        lf.symFlags[x] |= kSymFlagRegisters;
        queue.push_back(x);
      }
    }
  }

  lf.files = std::move(files.paths);
  lf.setBegin = std::move(sets.begin);
  lf.setTargets = std::move(sets.targets);
  return lf;
}

// ---------------------------------------------------------------------------
// Kernel
// ---------------------------------------------------------------------------

namespace {

/// A dense targets x targets relation, one bit row per target.
struct BitMatrix {
  uint32_t words = 0;
  std::vector<uint64_t> bits;

  explicit BitMatrix(uint32_t n) : words((n + 63) / 64) {
    bits.assign(static_cast<size_t>(words) * n, 0);
  }
  [[nodiscard]] const uint64_t *row(uint32_t r) const noexcept {
    return bits.data() + static_cast<size_t>(r) * words;
  }
  [[nodiscard]] uint64_t *row(uint32_t r) noexcept {
    return bits.data() + static_cast<size_t>(r) * words;
  }
  [[nodiscard]] bool test(uint32_t r, uint32_t c) const noexcept {
    return (row(r)[c >> 6] >> (c & 63)) & 1u;
  }
  void set(uint32_t r, uint32_t c) noexcept {
    row(r)[c >> 6] |= 1ull << (c & 63);
  }
  /// row(dst) |= row(src); whether dst changed.
  bool orInto(uint32_t dst, uint32_t src) noexcept {
    uint64_t *d = row(dst);
    const uint64_t *s = row(src);
    uint64_t changed = 0;
    for (uint32_t w = 0; w < words; ++w) {
      const uint64_t before = d[w];
      d[w] = before | s[w];
      changed |= d[w] ^ before;
    }
    return changed != 0;
  }
};

template <typename Fn>
void forEachBit(const uint64_t *row, uint32_t words, Fn &&fn) {
  for (uint32_t w = 0; w < words; ++w)
    for (uint64_t v = row[w]; v != 0; v &= v - 1)
      fn(w * 64 + static_cast<uint32_t>(llvm::countr_zero(v)));
}

/// Scratch planes sized once per audit; every traversal stamps instead of
/// clearing.
struct Workspace {
  std::vector<uint32_t> symStamp;    // per symbol: visited in this traversal
  std::vector<uint32_t> useStamp;    // per symbol: recorded as a use
  std::vector<uint32_t> softStamp;   // per symbol: counted as indirect use
  std::vector<uint32_t> queue;       // per symbol: BFS frontier
  std::vector<uint32_t> queueRoot;   // per symbol: root reference
  std::vector<uint32_t> fileStamp;   // per file
  std::vector<uint32_t> targetStamp; // per target
  std::vector<uint32_t> needScratch; // per target: needs of one closure
  std::vector<uint32_t> collect;     // per symbol: the closure's symbols
  uint32_t epoch = 0;

  Workspace(uint32_t symbols, uint32_t files, uint32_t targets)
      : symStamp(symbols, 0), useStamp(symbols, 0), softStamp(symbols, 0),
        queue(symbols),
        queueRoot(symbols), fileStamp(files, 0), targetStamp(targets, 0),
        needScratch(targets), collect(symbols) {}
};

/// Who references each symbol: the transpose of LinkFacts' references.
struct Referrers {
  std::vector<uint32_t> begin; // per symbol, size = symbols + 1
  std::vector<uint32_t> from;

  explicit Referrers(const LinkFacts &lf) {
    const uint32_t n = lf.symbolCount();
    begin.assign(n + 1, 0);
    from.resize(lf.refTo.size());
    for (uint32_t to : lf.refTo)
      ++begin[to + 1];
    for (uint32_t s = 0; s < n; ++s)
      begin[s + 1] += begin[s];
    std::vector<uint32_t> cursor(begin.begin(), begin.end() - 1);
    for (uint32_t s = 0; s < n; ++s)
      for (uint32_t r = lf.refBegin[s], e = lf.refBegin[s + 1]; r < e; ++r)
        from[cursor[lf.refTo[r]]++] = s;
  }
};

struct FanInCounts {
  uint32_t symbols = 0;
  uint32_t files = 0;
};

/// The provider's Defined symbols outside the closure `closure` that
/// reference into it, directly or through inline bodies (walked up to
/// whoever uses them). `collect`, when non-null, receives them (room for
/// every symbol).
FanInCounts fanInOf(const LinkFacts &lf, const Referrers &rev,
                    uint32_t provider, llvm::ArrayRef<uint32_t> closure,
                    Workspace &ws, uint32_t *collect) noexcept {
  const uint32_t epoch = ++ws.epoch;
  FanInCounts out;
  uint32_t head = 0, tail = 0;
  for (uint32_t k : closure) {
    ws.symStamp[k] = epoch;
    ws.queue[tail++] = k;
  }
  while (head < tail) {
    const uint32_t s = ws.queue[head++];
    for (uint32_t i = rev.begin[s], e = rev.begin[s + 1]; i < e; ++i) {
      const uint32_t f = rev.from[i];
      if (ws.symStamp[f] == epoch)
        continue;
      ws.symStamp[f] = epoch;
      const SymbolClass cls = lf.symClass[f];
      if (cls == SymbolClass::Inline) {
        ws.queue[tail++] = f;
      } else if (cls == SymbolClass::Defined &&
                 lf.setHas(lf.symOwner[f], provider)) {
        if (collect)
          collect[out.symbols] = f;
        ++out.symbols;
        const uint32_t file = lf.symFile[f];
        if (file != kNoIndex && ws.fileStamp[file] != epoch) {
          ws.fileStamp[file] = epoch;
          ++out.files;
        }
      }
    }
  }
  return out;
}

struct ClosureCounts {
  uint32_t symbols = 0;
  uint32_t files = 0;
  uint32_t needs = 0; // entries written to Workspace::needScratch
};

/// The closure of `seeds` inside `provider`: what the seeds transitively
/// reference among the provider's Defined symbols, walking through inline
/// bodies. References leaving the provider are recorded as the targets the
/// closure needs — owners the provider itself depends on when there are
/// any, else every owner. `collect`, when non-null, receives the closure's
/// symbols (room for every symbol).
ClosureCounts closureOf(const LinkFacts &lf, uint32_t provider,
                        const uint64_t *providerReach,
                        llvm::ArrayRef<uint32_t> seeds, Workspace &ws,
                        uint32_t *collect) noexcept {
  const uint32_t epoch = ++ws.epoch;
  ClosureCounts out;
  uint32_t head = 0, tail = 0;
  auto admit = [&](uint32_t s) noexcept {
    ws.symStamp[s] = epoch;
    ws.queue[tail++] = s;
    if (lf.symClass[s] != SymbolClass::Defined)
      return;
    if (collect)
      collect[out.symbols] = s;
    ++out.symbols;
    const uint32_t file = lf.symFile[s];
    if (file != kNoIndex && ws.fileStamp[file] != epoch) {
      ws.fileStamp[file] = epoch;
      ++out.files;
    }
  };
  auto need = [&](uint32_t t) noexcept {
    if (t == provider || ws.targetStamp[t] == epoch)
      return;
    ws.targetStamp[t] = epoch;
    ws.needScratch[out.needs++] = t;
  };
  for (uint32_t s : seeds)
    if (ws.symStamp[s] != epoch)
      admit(s);
  while (head < tail) {
    const uint32_t s = ws.queue[head++];
    for (uint32_t r = lf.refBegin[s], e = lf.refBegin[s + 1]; r < e; ++r) {
      const uint32_t to = lf.refTo[r];
      if (ws.symStamp[to] == epoch)
        continue;
      const SymbolClass cls = lf.symClass[to];
      if (cls == SymbolClass::Inline) {
        admit(to);
        continue;
      }
      ws.symStamp[to] = epoch;
      if (cls == SymbolClass::Unattributed)
        continue;
      const uint32_t owner = lf.symOwner[to];
      if (lf.setHas(owner, provider)) {
        if (cls == SymbolClass::Defined) {
          ws.symStamp[to] = 0; // admit stamps it
          admit(to);
        }
        continue;
      }
      bool anyReachable = false;
      for (uint32_t t : lf.targetsOf(owner))
        anyReachable |= (providerReach[t >> 6] >> (t & 63)) & 1u;
      for (uint32_t t : lf.targetsOf(owner))
        if (!anyReachable || ((providerReach[t >> 6] >> (t & 63)) & 1u))
          need(t);
    }
  }
  return out;
}

struct Use {
  uint32_t provider;
  uint32_t sym;
  uint32_t ref;
  uint32_t root;
  uint8_t flags;
};

struct AnchoredRef {
  uint64_t key;
  uint32_t ref;
  uint32_t from;
};

} // namespace

const char *declaredFormName(DeclaredForm form) {
  switch (form) {
  case DeclaredForm::None:
    return "none";
  case DeclaredForm::Direct:
    return "direct";
  case DeclaredForm::Transitive:
    return "transitive";
  case DeclaredForm::Closure:
    return "closure";
  }
  return "none";
}

uint32_t DependencyAudit::find(uint32_t consumer, uint32_t provider) const {
  if (consumer + 1 >= rowBegin.size())
    return kNoIndex;
  auto first = rows.begin() + rowBegin[consumer];
  auto last = rows.begin() + rowBegin[consumer + 1];
  auto it = std::lower_bound(first, last, provider,
                             [](const AuditRow &row, uint32_t p) {
                               return row.provider < p;
                             });
  if (it == last || it->provider != provider)
    return kNoIndex;
  return static_cast<uint32_t>(it - rows.begin());
}

DependencyAudit auditDependencies(const LinkFacts &lf, const TargetMap &map) {
  DependencyAudit audit;
  const uint32_t targetCount = map.size();
  const uint32_t symbolCount = lf.symbolCount();
  assert(lf.targetCount == targetCount && "facts built for another map");
  const StringInterner &strings = *lf.strings;
  auto usrOf = [&](uint32_t sym) -> const std::string & {
    return strings.resolve(lf.symUsr[sym]);
  };

  // --- Per-target sizes ----------------------------------------------------
  audit.providerSymbols.assign(targetCount, 0);
  audit.providerFiles.assign(targetCount, 0);
  audit.presumedSymbols.assign(targetCount, 0);
  audit.providerStaticInits.assign(targetCount, 0);
  audit.unplacedUses.assign(targetCount, 0);
  {
    std::vector<std::pair<uint32_t, uint32_t>> bodies; // (target, file)
    for (uint32_t s = 0; s < symbolCount; ++s) {
      if (lf.symClass[s] == SymbolClass::Defined) {
        const bool staticInit = (lf.symFlags[s] & kSymFlagStaticInit) != 0;
        for (uint32_t t : lf.targetsOf(lf.symOwner[s])) {
          ++audit.providerSymbols[t];
          audit.providerStaticInits[t] += staticInit;
          if (lf.symFile[s] != kNoIndex)
            bodies.emplace_back(t, lf.symFile[s]);
        }
      } else if (lf.symClass[s] == SymbolClass::Presumed) {
        for (uint32_t t : lf.targetsOf(lf.symOwner[s]))
          ++audit.presumedSymbols[t];
      }
    }
    std::sort(bodies.begin(), bodies.end());
    bodies.erase(std::unique(bodies.begin(), bodies.end()), bodies.end());
    for (const auto &b : bodies)
      ++audit.providerFiles[b.first];
  }

  // --- Target-level reachability over the declared dependencies -----------
  // reach(t): every target t's dependencies pull in, t itself excluded
  // unless the declarations form a cycle through it.
  // What a target forwards to its consumers (`interfaceDeps`) is reached
  // through it like what it links itself.
  BitMatrix reach(targetCount);
  for (uint32_t t = 0; t < targetCount; ++t) {
    for (uint32_t d : map.depsOf(t))
      reach.set(t, d);
    for (uint32_t d : map.interfaceDepsOf(t))
      reach.set(t, d);
  }
  for (bool changed = true; changed;) {
    changed = false;
    for (uint32_t t = targetCount; t-- > 0;) {
      for (uint32_t d : map.depsOf(t))
        changed |= reach.orInto(t, d);
      for (uint32_t d : map.interfaceDepsOf(t))
        changed |= reach.orInto(t, d);
    }
  }

  // The dependencies the build file is known to name. In the closure form
  // those are the listed targets no other listed target pulls in.
  std::vector<uint32_t> directBegin(targetCount + 1, 0), direct;
  for (uint32_t t = 0; t < targetCount; ++t) {
    auto deps = map.depsOf(t);
    for (uint32_t d : deps) {
      bool implied = false;
      if (map.depsAreClosure)
        for (uint32_t e : deps)
          if (e != d && reach.test(e, d)) {
            implied = true;
            break;
          }
      if (!implied)
        direct.push_back(d);
    }
    directBegin[t + 1] = static_cast<uint32_t>(direct.size());
  }

  // --- References each target's objects hold, in canonical order ----------
  std::vector<uint32_t> heldBegin(targetCount + 1, 0);
  for (uint32_t r = 0, e = static_cast<uint32_t>(lf.refTo.size()); r < e; ++r)
    for (uint32_t t : lf.targetsOf(lf.refAnchor[r]))
      ++heldBegin[t + 1];
  for (uint32_t t = 0; t < targetCount; ++t)
    heldBegin[t + 1] += heldBegin[t];
  std::vector<AnchoredRef> held(heldBegin[targetCount]);
  {
    std::vector<uint32_t> cursor(heldBegin.begin(), heldBegin.end() - 1);
    for (uint32_t s = 0; s < symbolCount; ++s)
      for (uint32_t r = lf.refBegin[s], e = lf.refBegin[s + 1]; r < e; ++r)
        for (uint32_t t : lf.targetsOf(lf.refAnchor[r]))
          held[cursor[t]++] = {lf.refKey[r], r, s};
  }
  for (uint32_t t = 0; t < targetCount; ++t)
    std::sort(held.begin() + heldBegin[t], held.begin() + heldBegin[t + 1],
              [&](const AnchoredRef &a, const AnchoredRef &b) {
                if (a.key != b.key)
                  return a.key < b.key;
                if (lf.refKind[a.ref] != lf.refKind[b.ref])
                  return lf.refKind[a.ref] < lf.refKind[b.ref];
                if (lf.refTo[a.ref] != lf.refTo[b.ref])
                  return usrOf(lf.refTo[a.ref]) < usrOf(lf.refTo[b.ref]);
                return a.from != b.from && usrOf(a.from) < usrOf(b.from);
              });

  // --- Soft references each target's objects hold (counted, unordered) ---
  std::vector<uint32_t> heldSoftBegin(targetCount + 1, 0);
  for (uint32_t r = 0, e = static_cast<uint32_t>(lf.softTo.size()); r < e; ++r)
    for (uint32_t t : lf.targetsOf(lf.softAnchor[r]))
      ++heldSoftBegin[t + 1];
  for (uint32_t t = 0; t < targetCount; ++t)
    heldSoftBegin[t + 1] += heldSoftBegin[t];
  std::vector<uint32_t> heldSoft(heldSoftBegin[targetCount]);
  {
    std::vector<uint32_t> cursor(heldSoftBegin.begin(),
                                 heldSoftBegin.end() - 1);
    for (uint32_t r = 0, e = static_cast<uint32_t>(lf.softTo.size()); r < e;
         ++r)
      for (uint32_t t : lf.targetsOf(lf.softAnchor[r]))
        heldSoft[cursor[t]++] = lf.softTo[r];
  }

  Workspace ws(symbolCount, static_cast<uint32_t>(lf.files.size()),
               targetCount);
  const Referrers rev(lf);
  std::vector<Use> uses;
  std::vector<uint32_t> softUses; // providers, one entry per indirect use
  std::vector<uint32_t> seedScratch;
  std::vector<uint32_t> reachCount(targetCount, 0);
  std::vector<uint32_t> directStamp(targetCount, 0);
  audit.rowBegin.assign(targetCount + 1, 0);

  for (uint32_t consumer = 0; consumer < targetCount; ++consumer) {
    // --- Phase A: what the consumer's objects reference -------------------
    const uint32_t epoch = ++ws.epoch;
    const uint64_t *consumerReach = reach.row(consumer);
    uses.clear();
    auto record = [&](uint32_t sym, uint32_t ref, uint32_t root, bool via) {
      ws.useStamp[sym] = epoch;
      const uint8_t flags =
          (via ? kSeedViaInline : 0) |
          (lf.symClass[sym] == SymbolClass::Presumed ? kSeedUncertain : 0) |
          ((lf.symFlags[sym] & kSymFlagRegisters) ? kSeedRegistration : 0);
      auto owners = lf.targetsOf(lf.symOwner[sym]);
      bool anyDeclared = false;
      for (uint32_t t : owners)
        anyDeclared |= (consumerReach[t >> 6] >> (t & 63)) & 1u;
      for (uint32_t t : owners)
        if (!anyDeclared || ((consumerReach[t >> 6] >> (t & 63)) & 1u))
          uses.push_back({t, sym, ref, root, flags});
    };
    uint32_t head = 0, tail = 0;
    auto visit = [&](uint32_t to, uint32_t ref, uint32_t root, bool via) {
      const SymbolClass cls = lf.symClass[to];
      if (cls == SymbolClass::Inline) {
        if (ws.symStamp[to] != epoch) {
          ws.symStamp[to] = epoch;
          ws.queue[tail] = to;
          ws.queueRoot[tail++] = root;
        }
      } else if (cls == SymbolClass::Unattributed) {
        if (lf.symUnplaced[to] && ws.useStamp[to] != epoch) {
          ws.useStamp[to] = epoch;
          ++audit.unplacedUses[consumer];
        }
      } else if (ws.useStamp[to] != epoch &&
                 !lf.setHas(lf.symOwner[to], consumer)) {
        record(to, ref, root, via);
      }
    };
    for (uint32_t i = heldBegin[consumer]; i < heldBegin[consumer + 1]; ++i)
      visit(lf.refTo[held[i].ref], held[i].ref, held[i].ref, false);
    while (head < tail) {
      const uint32_t s = ws.queue[head];
      const uint32_t root = ws.queueRoot[head++];
      for (uint32_t r = lf.refBegin[s], e = lf.refBegin[s + 1]; r < e; ++r)
        visit(lf.refTo[r], r, root, true);
    }
    // Indirect uses: symbols of another target the consumer's code touches
    // without a reference counted above — the inline bodies it walked
    // through, and the targets of the soft references it and they hold.
    softUses.clear();
    auto touch = [&](uint32_t sym) {
      if (ws.softStamp[sym] == epoch || ws.useStamp[sym] == epoch)
        return;
      ws.softStamp[sym] = epoch;
      const uint32_t owner = lf.symOwner[sym];
      if (lf.symClass[sym] == SymbolClass::Unattributed || owner == 0 ||
          lf.setHas(owner, consumer))
        return;
      for (uint32_t t : lf.targetsOf(owner))
        softUses.push_back(t);
    };
    for (uint32_t i = heldSoftBegin[consumer]; i < heldSoftBegin[consumer + 1];
         ++i)
      touch(heldSoft[i]);
    for (uint32_t q = 0; q < tail; ++q) {
      const uint32_t s = ws.queue[q];
      touch(s);
      for (uint32_t r = lf.softBegin[s], e = lf.softBegin[s + 1]; r < e; ++r)
        touch(lf.softTo[r]);
    }
    std::sort(softUses.begin(), softUses.end());

    constexpr uint8_t kOrderFlags = kSeedViaInline | kSeedUncertain;
    std::sort(uses.begin(), uses.end(), [&](const Use &a, const Use &b) {
      if (a.provider != b.provider)
        return a.provider < b.provider;
      if ((a.flags & kOrderFlags) != (b.flags & kOrderFlags))
        return (a.flags & kOrderFlags) < (b.flags & kOrderFlags);
      if (lf.refKey[a.ref] != lf.refKey[b.ref])
        return lf.refKey[a.ref] < lf.refKey[b.ref];
      if (lf.refKind[a.ref] != lf.refKind[b.ref])
        return lf.refKind[a.ref] < lf.refKind[b.ref];
      return a.sym != b.sym && usrOf(a.sym) < usrOf(b.sym);
    });

    // --- Rows: the declared dependencies merged with observed providers ---
    const uint32_t stamp = consumer + 1;
    for (uint32_t i = directBegin[consumer]; i < directBegin[consumer + 1]; ++i)
      directStamp[direct[i]] = stamp;
    auto deps = map.depsOf(consumer);
    const size_t firstRow = audit.rows.size();
    size_t di = 0, ui = 0;
    while (di < deps.size() || ui < uses.size()) {
      uint32_t provider;
      if (ui == uses.size() ||
          (di < deps.size() && deps[di] <= uses[ui].provider))
        provider = deps[di];
      else
        provider = uses[ui].provider;
      const bool listed = di < deps.size() && deps[di] == provider;
      if (listed)
        ++di;
      AuditRow row;
      row.consumer = consumer;
      row.provider = provider;
      if (directStamp[provider] == stamp)
        row.declared = DeclaredForm::Direct;
      else if (map.depsAreClosure)
        row.declared = listed || reach.test(consumer, provider)
                           ? DeclaredForm::Closure
                           : DeclaredForm::None;
      else
        row.declared = reach.test(consumer, provider)
                           ? DeclaredForm::Transitive
                           : DeclaredForm::None;
      row.seedBegin = static_cast<uint32_t>(audit.seedSym.size());
      seedScratch.clear();
      for (; ui < uses.size() && uses[ui].provider == provider; ++ui) {
        const Use &u = uses[ui];
        audit.seedSym.push_back(u.sym);
        audit.seedRef.push_back(u.ref);
        audit.seedRoot.push_back(u.root);
        audit.seedFlags.push_back(u.flags);
        if (u.flags & kSeedUncertain) {
          ++row.uncertainSymbols;
        } else {
          ++row.symbols;
          row.viaInline += (u.flags & kSeedViaInline) != 0;
          row.registrationSeeds += (u.flags & kSeedRegistration) != 0;
          seedScratch.push_back(u.sym);
        }
      }
      row.seedEnd = static_cast<uint32_t>(audit.seedSym.size());
      // Undeclared, and only through inline bodies the consumer reaches in
      // a target it does declare: whose header carries the dependency.
      if (row.symbols != 0 && row.viaInline == row.symbols &&
          (row.declared == DeclaredForm::Transitive ||
           row.declared == DeclaredForm::None)) {
        uint32_t first = kNoIndex;
        bool allThrough = true;
        for (uint32_t i = row.seedBegin; i < row.seedEnd && allThrough; ++i) {
          if (audit.seedFlags[i] & kSeedUncertain)
            continue;
          const uint32_t root = audit.seedRoot[i];
          const uint32_t home = lf.symOwner[lf.refTo[root]];
          bool declared = false;
          for (uint32_t t : lf.targetsOf(home))
            declared |= t != provider &&
                        ((consumerReach[t >> 6] >> (t & 63)) & 1u);
          allThrough = declared;
          if (first == kNoIndex)
            first = root;
        }
        if (allThrough)
          row.throughInlineRef = first;
      }
      {
        const auto range =
            std::equal_range(softUses.begin(), softUses.end(), provider);
        row.indirectUses = static_cast<uint32_t>(range.second - range.first);
      }

      // --- Phase B: the closure of the certain seeds inside the provider --
      row.needBegin = static_cast<uint32_t>(audit.needs.size());
      if (!seedScratch.empty()) {
        const ClosureCounts c = closureOf(lf, provider, reach.row(provider),
                                          seedScratch, ws, ws.collect.data());
        row.closureSymbols = c.symbols;
        row.closureFiles = c.files;
        audit.needs.insert(audit.needs.end(), ws.needScratch.begin(),
                           ws.needScratch.begin() + c.needs);
        std::sort(audit.needs.begin() + row.needBegin, audit.needs.end());
        // fanInOf reuses the queue; the closure stays in `collect`.
        const FanInCounts f = fanInOf(
            lf, rev, provider,
            llvm::ArrayRef<uint32_t>(ws.collect.data(), c.symbols), ws,
            nullptr);
        row.closureFanIn = f.symbols;
        row.closureFanInFiles = f.files;
      }
      row.needEnd = static_cast<uint32_t>(audit.needs.size());
      audit.rows.push_back(row);
    }

    // --- Phase C: what the consumer links only because of each edge -------
    // reachCount[x]: how many of the consumer's own reasons to link reach
    // x — its direct dependencies and every target it references.
    std::fill(reachCount.begin(), reachCount.end(), 0u);
    auto contribute = [&](uint32_t t) {
      ++reachCount[t];
      forEachBit(reach.row(t), reach.words, [&](uint32_t x) {
        if (x != t)
          ++reachCount[x];
      });
    };
    for (size_t i = firstRow; i < audit.rows.size(); ++i) {
      const AuditRow &row = audit.rows[i];
      if (row.declared == DeclaredForm::Direct ||
          row.symbols + row.uncertainSymbols != 0)
        contribute(row.provider);
    }
    for (size_t i = firstRow; i < audit.rows.size(); ++i) {
      AuditRow &row = audit.rows[i];
      row.dropBegin = row.dropEnd = static_cast<uint32_t>(audit.droppable.size());
      if (row.declared != DeclaredForm::Direct)
        continue;
      // Mark what the closure would still pull in.
      const uint32_t mark = ++ws.epoch;
      for (uint32_t n = row.needBegin; n < row.needEnd; ++n) {
        const uint32_t m = audit.needs[n];
        ws.targetStamp[m] = mark;
        forEachBit(reach.row(m), reach.words,
                   [&](uint32_t x) { ws.targetStamp[x] = mark; });
      }
      auto consider = [&](uint32_t x) {
        if (x == consumer || reachCount[x] != 1)
          return;
        ++row.onlyViaEdge;
        if (ws.targetStamp[x] == mark)
          return;
        audit.droppable.push_back(x);
        row.droppableSymbols += audit.providerSymbols[x];
        if (x == row.provider)
          row.providerDroppable = true;
      };
      // Ascending: the provider is visited in place among its reach set.
      bool providerSeen = false;
      forEachBit(reach.row(row.provider), reach.words, [&](uint32_t x) {
        if (!providerSeen && x >= row.provider) {
          providerSeen = true;
          if (x != row.provider)
            consider(row.provider);
        }
        consider(x);
      });
      if (!providerSeen)
        consider(row.provider);
      if (row.providerDroppable)
        row.droppableSymbols -= row.closureSymbols;
      row.dropEnd = static_cast<uint32_t>(audit.droppable.size());
    }
    audit.rowBegin[consumer + 1] = static_cast<uint32_t>(audit.rows.size());
  }
  return audit;
}

unsigned rowFindings(const DependencyAudit &audit, const LinkFacts &facts,
                     const AuditRow &row, ThinThresholds thresholds,
                     bool coverageStated) {
  unsigned findings = 0;
  const uint32_t providerSymbols = audit.providerSymbols[row.provider];
  const uint64_t providerTotal = static_cast<uint64_t>(providerSymbols) +
                                 audit.presumedSymbols[row.provider];
  // Unused: the index knows functions of the provider and the consumer's
  // objects call none of them, not even one whose owner is presumed, and
  // the consumer's code touches nothing of the provider's indirectly.
  if (row.declared == DeclaredForm::Direct && row.symbols == 0 &&
      row.uncertainSymbols == 0 && row.indirectUses == 0 && coverageStated &&
      facts.targetCovered[row.consumer] && facts.targetCovered[row.provider] &&
      providerTotal != 0)
    findings |= kFindingUnused;
  if (row.symbols != 0 && (row.declared == DeclaredForm::Transitive ||
                           row.declared == DeclaredForm::None))
    findings |= kFindingUndeclared;
  // Thin describes the shape of the edge: few symbols, and a closure that
  // is a small share of the provider. What cutting it would save is ranked
  // by the row's gain columns, not decided here. Presumed symbols count:
  // each is charged as a closure of itself.
  const uint64_t used =
      static_cast<uint64_t>(row.symbols) + row.uncertainSymbols;
  const uint64_t closure =
      static_cast<uint64_t>(row.closureSymbols) + row.uncertainSymbols;
  if ((row.declared == DeclaredForm::Direct ||
       row.declared == DeclaredForm::Closure) &&
      used != 0 &&
      used <= thresholds.maxSymbols &&
      closure * 100 <= thresholds.maxClosurePercent * providerTotal)
    findings |= kFindingThin;
  return findings;
}

void closureFanInOf(const LinkFacts &lf, const DependencyAudit &audit,
                    const AuditRow &row, std::vector<uint32_t> &out) {
  std::vector<uint32_t> closure;
  closureSymbolsOf(lf, audit, row, closure);
  out.clear();
  if (closure.empty())
    return;
  const Referrers rev(lf);
  Workspace ws(lf.symbolCount(), static_cast<uint32_t>(lf.files.size()),
               lf.targetCount);
  out.resize(lf.symbolCount());
  const FanInCounts f = fanInOf(lf, rev, row.provider, closure, ws, out.data());
  out.resize(f.symbols);
}

void closureSymbolsOf(const LinkFacts &lf, const DependencyAudit &audit,
                      const AuditRow &row, std::vector<uint32_t> &out) {
  out.clear();
  std::vector<uint32_t> seeds;
  for (uint32_t i = row.seedBegin; i < row.seedEnd; ++i)
    if (!(audit.seedFlags[i] & kSeedUncertain))
      seeds.push_back(audit.seedSym[i]);
  if (seeds.empty())
    return;
  // The provider's reach only decides which owner a leaving reference is
  // charged to; the closure's symbols do not depend on it.
  const std::vector<uint64_t> noReach((lf.targetCount + 63) / 64, 0);
  Workspace ws(lf.symbolCount(), static_cast<uint32_t>(lf.files.size()),
               lf.targetCount);
  out.resize(lf.symbolCount());
  const ClosureCounts c =
      closureOf(lf, row.provider, noReach.data(), seeds, ws, out.data());
  out.resize(c.symbols);
}

} // namespace vycor
