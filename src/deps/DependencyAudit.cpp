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
#include "vycor/callgraph/Snapshot.h"
#include "vycor/callgraph/Utf8.h"
#include "vycor/query/Serialize.h"

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

/// Lexicographic order of two target sets' contents.
bool setContentLess(const SetTable &sets, uint32_t a, uint32_t b) {
  auto x = sets.of(a), y = sets.of(b);
  return std::lexicographical_compare(x.begin(), x.end(), y.begin(), y.end());
}

} // namespace

// ---------------------------------------------------------------------------
// Stage-1 producer: call edges -> link facts
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
  std::vector<uint32_t> scratchSet;

  // --- Files the map names: sources anchor, headers carry ownership -------
  // fileSet[f]: the target set whose objects hold code written in f, or 0
  // when f is compiled into whoever includes it.
  // stemOwner[f]: the one target set with a source named like header f
  // that includes it (0 none seen, kNoIndex several).
  std::vector<uint32_t> fileSet, headerOwner, stemOwner;
  std::vector<uint8_t> isSource;
  auto growFileAttrs = [&] {
    fileSet.resize(files.paths.size(), 0);
    headerOwner.resize(files.paths.size(), 0);
    stemOwner.resize(files.paths.size(), 0);
    isSource.resize(files.paths.size(), 0);
  };
  auto assignListed = [&](bool sourcesNotHeaders, std::vector<uint32_t> &attr) {
    std::vector<std::pair<uint32_t, uint32_t>> listed; // (file, target)
    for (uint32_t t = 0; t < targetCount; ++t)
      for (const std::string &p :
           sourcesNotHeaders ? map.sourcesOf(t) : map.headersOf(t))
        listed.emplace_back(files.add(p), t);
    growFileAttrs();
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
  assignListed(true, fileSet);
  for (uint32_t f = 0; f < fileSet.size(); ++f)
    isSource[f] = fileSet[f] != 0;
  assignListed(false, headerOwner);

  // --- Index meta: per-TU outcomes, and headers private to one target set -
  // fileStatus: 0 not in the index, 1 parsed cleanly, 2 parse not clean.
  std::vector<uint8_t> fileStatus;
  if (meta) {
    const size_t tuCount = meta->files.size();
    std::vector<uint32_t> tuFile(tuCount), depFile(meta->deps.size());
    for (size_t i = 0; i < tuCount; ++i)
      tuFile[i] = files.add(normalizeAuditPath(toIndexText(meta->files[i].path), ""));
    for (size_t j = 0; j < depFile.size(); ++j)
      depFile[j] = files.add(normalizeAuditPath(toIndexText(meta->deps[j].path), ""));
    growFileAttrs();
    fileStatus.assign(files.paths.size(), 0);

    constexpr uint32_t kUnset = kNoIndex;
    std::vector<uint32_t> depSet(depFile.size(), kUnset);
    std::vector<uint8_t> depClean(depFile.size(), 1);
    for (size_t i = 0; i < tuCount; ++i) {
      const bool clean = i < meta->outcomes.size() &&
                         meta->outcomes[i].status == TuStatus::Indexed;
      const uint32_t tuSet = isSource[tuFile[i]] ? fileSet[tuFile[i]] : 0;
      if (tuSet == 0)
        ++lf.counts.unmappedTus;
      fileStatus[tuFile[i]] = clean ? 1 : 2;
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
      if (isSource[f]) {
        // A mapped source that is not a TU of its own but is included by
        // one (a unity build): as covered as its includers.
        if (fileStatus[f] == 0)
          fileStatus[f] = depClean[j] ? 1 : 2;
      } else if (depSet[j] != 0 && map.underRoots(files.paths[f])) {
        // Opened only by TUs of one target set, and the build's own: its
        // code is compiled into that set's objects and nowhere else.
        fileSet[f] = depSet[j];
      }
    }

    // Stem affinity: a shared header named like a source that includes it
    // (tokenizer.hpp and tokenizer.cpp) most likely declares what that
    // source defines. Used only when nothing better names an owner.
    std::vector<uint64_t> stem(files.paths.size());
    for (size_t f = 0; f < stem.size(); ++f)
      stem[f] = llvm::xxh3_64bits(llvm::sys::path::stem(files.paths[f]));
    for (size_t i = 0; i < tuCount && i < meta->tuDeps.size(); ++i) {
      const uint32_t tuSet = isSource[tuFile[i]] ? fileSet[tuFile[i]] : 0;
      if (tuSet == 0)
        continue;
      for (uint32_t j : meta->tuDeps[i]) {
        if (j >= depFile.size())
          continue;
        const uint32_t f = depFile[j];
        if (isSource[f] || stem[f] != stem[tuFile[i]])
          continue;
        if (stemOwner[f] == 0)
          stemOwner[f] = tuSet;
        else if (stemOwner[f] != tuSet)
          stemOwner[f] = kNoIndex;
      }
    }
  }
  fileStatus.resize(files.paths.size(), 0);

  lf.targetSources.assign(targetCount, 0);
  lf.targetIndexed.assign(targetCount, 0);
  lf.targetCovered.assign(targetCount, 0);
  for (uint32_t t = 0; t < targetCount; ++t) {
    for (const std::string &p : map.sourcesOf(t)) {
      ++lf.targetSources[t];
      if (fileStatus[files.byPath.lookup(p)] == 1)
        ++lf.targetIndexed[t];
    }
    lf.targetCovered[t] = meta && lf.targetSources[t] != 0 &&
                          lf.targetIndexed[t] == lf.targetSources[t];
  }
  for (uint32_t f = 0; f < isSource.size(); ++f) {
    if (!isSource[f])
      continue;
    ++lf.counts.mappedSources;
    lf.counts.indexedSources += fileStatus[f] == 1;
    lf.counts.failedSources += fileStatus[f] == 2;
  }

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
  std::vector<uint32_t> rawFrom, rawTo, rawFile;
  std::vector<uint64_t> rawLineCol;
  std::vector<SId> rawSite;
  std::vector<uint8_t> rawKind;
  {
    const size_t expected = graph.edgeCount();
    rawFrom.reserve(expected);
    rawTo.reserve(expected);
    rawFile.reserve(expected);
    rawLineCol.reserve(expected);
    rawSite.reserve(expected);
    rawKind.reserve(expected);
  }
  graph.forEachStoredEdge([&](const CallGraph::EdgeRef &e) {
    // A call through a vtable slot needs no definition of its static
    // target at link time; the vtable does, and that reference is recorded
    // where the object is constructed (the Proven edges).
    if (e.kind == EdgeKind::VirtualDispatch &&
        e.confidence != Confidence::Proven) {
      ++lf.counts.skippedVirtualCalls;
      return;
    }
    // The callee is the function that returns the pointer, joined at query
    // time; the pointer's own address-of is recorded as its own edge.
    if (e.kind == EdgeKind::FunctionPointerReturn) {
      ++lf.counts.skippedDeferred;
      return;
    }
    if (e.caller == e.callee)
      return;
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
  });
  const uint32_t symbolCount = lf.symbolCount();
  const size_t rawCount = rawFrom.size();

  // Declaration file per symbol (kNoIndex without a node).
  std::vector<uint32_t> declFile(symbolCount, kNoIndex);
  for (uint32_t s = 0; s < symbolCount; ++s)
    if (lf.symNode[s])
      declFile[s] = resolver.resolve(lf.symNode[s]->file);

  growFileAttrs();
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

  // 1. A reference held by a target's own file: the body is there.
  // 2. Else declared in such a file (a function with no calls in it).
  std::vector<std::pair<uint32_t, uint32_t>> votes; // (header, owner set)
  for (uint32_t s = 0; s < symbolCount; ++s) {
    uint32_t owner = 0, bodyFile = kNoIndex;
    uint64_t bestKey = ~0ull;
    bool mixed = false;
    for (uint32_t i = rawBegin[s]; i < rawBegin[s + 1]; ++i) {
      const uint32_t r = byFrom[i];
      const uint32_t set = siteSetOf(r);
      if (set == 0)
        continue;
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
    } else if (decl != kNoIndex && fileSet[decl] != 0) {
      lf.symClass[s] = SymbolClass::Defined;
      lf.symOwner[s] = fileSet[decl];
      lf.symFile[s] = decl;
    }
  }

  // Header affinity: a shared header nobody lists belongs, for the
  // functions it declares without a visible body, to the target set that
  // defines the most of the functions it declares; failing that, to the
  // one whose source shares its name.
  std::sort(votes.begin(), votes.end());
  for (size_t i = 0; i < votes.size();) {
    const uint32_t header = votes[i].first;
    uint32_t best = 0;
    size_t bestCount = 0;
    while (i < votes.size() && votes[i].first == header) {
      size_t j = i;
      while (j < votes.size() && votes[j] == votes[i])
        ++j;
      const size_t count = j - i;
      if (count > bestCount ||
          (count == bestCount && setContentLess(sets, votes[i].second, best))) {
        bestCount = count;
        best = votes[i].second;
      }
      i = j;
    }
    headerOwner[header] = best;
  }
  for (uint32_t f = 0; f < headerOwner.size(); ++f)
    if (headerOwner[f] == 0 && fileSet[f] == 0 && stemOwner[f] != 0 &&
        stemOwner[f] != kNoIndex && map.underRoots(files.paths[f]))
      headerOwner[f] = stemOwner[f];

  // 3. Calls only at shared sites: an inline body. 4. No calls at all:
  //    template code is inline; otherwise the declaring header's owner is
  //    presumed.
  for (uint32_t s = 0; s < symbolCount; ++s) {
    if (lf.symClass[s] == SymbolClass::Defined) {
      ++lf.counts.defined;
      continue;
    }
    const uint32_t decl = declFile[s];
    if (rawBegin[s + 1] != rawBegin[s] ||
        templateLikeUsr(strings.resolve(lf.symUsr[s]))) {
      lf.symClass[s] = SymbolClass::Inline;
      ++lf.counts.inlined;
    } else if (decl != kNoIndex && headerOwner[decl] != 0) {
      lf.symClass[s] = SymbolClass::Presumed;
      lf.symOwner[s] = headerOwner[decl];
      ++lf.counts.presumed;
    } else {
      ++lf.counts.unattributed;
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
  std::vector<uint32_t> range, rangeAnchor;
  for (uint32_t s = 0; s < symbolCount; ++s) {
    range.assign(byFrom.begin() + rawBegin[s], byFrom.begin() + rawBegin[s + 1]);
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
      return rawKind[ra] < rawKind[rb];
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
  std::vector<uint32_t> queue;       // per symbol: BFS frontier
  std::vector<uint32_t> queueRoot;   // per symbol: root reference
  std::vector<uint32_t> fileStamp;   // per file
  std::vector<uint32_t> targetStamp; // per target
  std::vector<uint32_t> needScratch; // per target: needs of one closure
  uint32_t epoch = 0;

  Workspace(uint32_t symbols, uint32_t files, uint32_t targets)
      : symStamp(symbols, 0), useStamp(symbols, 0), queue(symbols),
        queueRoot(symbols), fileStamp(files, 0), targetStamp(targets, 0),
        needScratch(targets) {}
};

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
  {
    std::vector<std::pair<uint32_t, uint32_t>> bodies; // (target, file)
    for (uint32_t s = 0; s < symbolCount; ++s) {
      if (lf.symClass[s] == SymbolClass::Defined) {
        for (uint32_t t : lf.targetsOf(lf.symOwner[s])) {
          ++audit.providerSymbols[t];
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
  BitMatrix reach(targetCount);
  for (uint32_t t = 0; t < targetCount; ++t)
    for (uint32_t d : map.depsOf(t))
      reach.set(t, d);
  for (bool changed = true; changed;) {
    changed = false;
    for (uint32_t t = targetCount; t-- > 0;)
      for (uint32_t d : map.depsOf(t))
        changed |= reach.orInto(t, d);
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

  Workspace ws(symbolCount, static_cast<uint32_t>(lf.files.size()),
               targetCount);
  std::vector<Use> uses;
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
          (lf.symClass[sym] == SymbolClass::Presumed ? kSeedUncertain : 0);
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
      } else if (cls != SymbolClass::Unattributed &&
                 ws.useStamp[to] != epoch &&
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
    std::sort(uses.begin(), uses.end(), [&](const Use &a, const Use &b) {
      if (a.provider != b.provider)
        return a.provider < b.provider;
      if (a.flags != b.flags)
        return a.flags < b.flags;
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
          seedScratch.push_back(u.sym);
        }
      }
      row.seedEnd = static_cast<uint32_t>(audit.seedSym.size());

      // --- Phase B: the closure of the certain seeds inside the provider --
      row.needBegin = static_cast<uint32_t>(audit.needs.size());
      if (!seedScratch.empty()) {
        const ClosureCounts c = closureOf(lf, provider, reach.row(provider),
                                          seedScratch, ws, nullptr);
        row.closureSymbols = c.symbols;
        row.closureFiles = c.files;
        audit.needs.insert(audit.needs.end(), ws.needScratch.begin(),
                           ws.needScratch.begin() + c.needs);
        std::sort(audit.needs.begin() + row.needBegin, audit.needs.end());
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
  // objects call none of them, not even one whose owner is presumed.
  if (row.declared == DeclaredForm::Direct && row.symbols == 0 &&
      row.uncertainSymbols == 0 && coverageStated &&
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
