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

#include "vycor/callgraph/Snapshot.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <chrono>
#include <unordered_map>

namespace vycor {

namespace {

constexpr char kMagic[4] = {'V', 'Y', 'C', 'S'};

// ----------------------------------------------------------------------------
// v5 layout: id-preserving. Each StringInterner's table is serialized in id
// order and bulk-installed on load, so node/edge/context records can store
// raw u32 interner ids that are valid verbatim in the loaded structures —
// no string hashing per record. The v4 global string pool is gone; strings
// that are not interner-backed (meta, node file/enclosingClass, scope/guard
// set tables — all small sections) are written inline (u32 len + bytes).
// ----------------------------------------------------------------------------

// ----------------------------------------------------------------------------
// Little-endian emit helpers (into a std::string buffer)
// ----------------------------------------------------------------------------

void putU8(std::string &out, uint8_t v) { out.push_back(static_cast<char>(v)); }

void putU32(std::string &out, uint32_t v) {
  for (int i = 0; i < 4; ++i)
    out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

void putU64(std::string &out, uint64_t v) {
  for (int i = 0; i < 8; ++i)
    out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

void putLenStr(std::string &out, const std::string &s) {
  putU32(out, static_cast<uint32_t>(s.size()));
  out.append(s);
}

// ----------------------------------------------------------------------------
// Bounds-checked reader. Any overrun or bad index flips ok to false; callers
// check once at section boundaries and treat failure as a corrupt snapshot.
// ----------------------------------------------------------------------------

struct Reader {
  const char *p;
  const char *end;
  bool ok = true;

  uint8_t u8() {
    if (p + 1 > end) {
      ok = false;
      return 0;
    }
    return static_cast<uint8_t>(*p++);
  }

  uint32_t u32() {
    if (p + 4 > end) {
      ok = false;
      return 0;
    }
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i)
      v |= static_cast<uint32_t>(static_cast<uint8_t>(*p++)) << (8 * i);
    return v;
  }

  uint64_t u64() {
    if (p + 8 > end) {
      ok = false;
      return 0;
    }
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
      v |= static_cast<uint64_t>(static_cast<uint8_t>(*p++)) << (8 * i);
    return v;
  }

  /// Raw bytes of the given length.
  std::string bytes(uint32_t len) {
    if (p + len > end) {
      ok = false;
      return std::string();
    }
    std::string s(p, p + len);
    p += len;
    return s;
  }

  /// Inline length-prefixed string.
  std::string lenStr() { return bytes(u32()); }

  /// Sanity bound for element counts: a count can never exceed the number
  /// of remaining bytes (every element is at least one byte).
  uint32_t count() {
    uint32_t n = u32();
    if (ok && n > static_cast<uint64_t>(end - p)) {
      ok = false;
      return 0;
    }
    return n;
  }
};

// ----------------------------------------------------------------------------
// Section emitters
// ----------------------------------------------------------------------------

void emitChannelTypeSpec(std::string &out, const ChannelTypeSpec &spec) {
  putLenStr(out, spec.qualifiedTypeName);
  putU32(out, static_cast<uint32_t>(spec.produceMethods.size()));
  for (const auto &m : spec.produceMethods)
    putLenStr(out, m);
  putU32(out, static_cast<uint32_t>(spec.consumeMethods.size()));
  for (const auto &m : spec.consumeMethods)
    putLenStr(out, m);
  putLenStr(out, spec.category);
}

ChannelTypeSpec readChannelTypeSpec(Reader &r) {
  ChannelTypeSpec spec;
  spec.qualifiedTypeName = r.lenStr();
  uint32_t n = r.count();
  for (uint32_t i = 0; r.ok && i < n; ++i)
    spec.produceMethods.push_back(r.lenStr());
  n = r.count();
  for (uint32_t i = 0; r.ok && i < n; ++i)
    spec.consumeMethods.push_back(r.lenStr());
  spec.category = r.lenStr();
  return spec;
}

void emitMeta(std::string &out, const SnapshotMeta &meta) {
  putU32(out, static_cast<uint32_t>(meta.collapsePaths.size()));
  for (const auto &s : meta.collapsePaths)
    putLenStr(out, s);
  putU32(out, static_cast<uint32_t>(meta.lockAllowlist.size()));
  for (const auto &s : meta.lockAllowlist)
    putLenStr(out, s);
  putU8(out, meta.lockBuiltins ? 1 : 0);
  putU32(out, static_cast<uint32_t>(meta.channelTypes.size()));
  for (const auto &spec : meta.channelTypes)
    emitChannelTypeSpec(out, spec);
  putU32(out, static_cast<uint32_t>(meta.files.size()));
  for (const auto &f : meta.files) {
    putLenStr(out, f.path);
    putU64(out, f.mtimeNs);
    putU64(out, f.size);
  }
}

bool readMeta(Reader &r, SnapshotMeta &meta) {
  uint32_t n = r.count();
  for (uint32_t i = 0; r.ok && i < n; ++i)
    meta.collapsePaths.push_back(r.lenStr());
  n = r.count();
  for (uint32_t i = 0; r.ok && i < n; ++i)
    meta.lockAllowlist.push_back(r.lenStr());
  meta.lockBuiltins = r.u8() != 0;
  n = r.count();
  for (uint32_t i = 0; r.ok && i < n; ++i)
    meta.channelTypes.push_back(readChannelTypeSpec(r));
  n = r.count();
  for (uint32_t i = 0; r.ok && i < n; ++i) {
    FileStamp fs;
    fs.path = r.lenStr();
    fs.mtimeNs = r.u64();
    fs.size = r.u64();
    meta.files.push_back(std::move(fs));
  }
  return r.ok;
}

void emitInternerTable(std::string &out, const StringInterner &interner) {
  putU32(out, static_cast<uint32_t>(interner.size()));
  interner.forEachString([&](const std::string &s) { putLenStr(out, s); });
}

/// Read a serialized interner table and bulk-install it into `interner`
/// (which must be freshly constructed, so ids match by position).
bool readInternerTable(Reader &r, StringInterner &interner) {
  uint32_t n = r.count();
  std::vector<std::string> table;
  table.reserve(n);
  for (uint32_t i = 0; r.ok && i < n; ++i)
    table.push_back(r.lenStr());
  return r.ok && interner.installStrings(std::move(table));
}

} // anonymous namespace

// ----------------------------------------------------------------------------
// SnapshotIO
// ----------------------------------------------------------------------------

bool SnapshotIO::save(const std::string &path, const CallGraph &graph,
                      const ControlFlowIndex &cfIndex,
                      const SnapshotMeta &meta, const ChannelIndex &channels) {
  using SId = StringInterner::Id;
  std::string data;

  emitMeta(data, meta);

  {
    // Reading private state directly; hold the graph lock for a consistent
    // view (save may be called while the serve loop is idle, but cheap
    // insurance against future callers). Every graph mutator holds this
    // lock too, so the interner cannot grow between the table emit and the
    // record emits below.
    std::lock_guard<std::mutex> lock(graph.mutex_);

    emitInternerTable(data, graph.interner_);

    // Nodes keyed by interned usr id, with the display-name id alongside
    // (v6); both strings are recovered from the interner on load and the
    // byName_ index is rebuilt from the pairs. Contributor TU sets are raw
    // ids; nodeContributors_/tuNodes_ are rebuilt from them on load.
    putU32(data, static_cast<uint32_t>(graph.nodes_.size()));
    for (const auto &[nameId, node] : graph.nodes_) {
      putU32(data, nameId);
      // addNode interns every display name, so the lookup cannot miss.
      auto displayId = graph.interner_.find(node.qualifiedName);
      putU32(data, displayId ? *displayId : nameId);
      putLenStr(data, node.file);
      putU32(data, node.line);
      uint8_t flags = (node.isEntryPoint ? 1 : 0) | (node.isVirtual ? 2 : 0);
      putU8(data, flags);
      putLenStr(data, node.enclosingClass);
      auto cit = graph.nodeContributors_.find(nameId);
      if (cit == graph.nodeContributors_.end()) {
        putU32(data, 0);
      } else {
        putU32(data, static_cast<uint32_t>(cit->second.size()));
        for (SId tuId : cit->second)
          putU32(data, tuId);
      }
    }

    // Invert tuEdges_ once: edge index -> contributor TU ids (an edge can
    // be registered by several TUs once dedup merges identical edges; a TU
    // may legitimately appear twice — refs counts registrations).
    std::unordered_map<size_t, std::vector<uint32_t>> edgeTus;
    for (const auto &[tuId, idxs] : graph.tuEdges_)
      for (size_t idx : idxs)
        if (graph.edges_[idx].refs != 0)
          edgeTus[idx].push_back(tuId);

    // Live edges only (tombstones are dropped; the loaded graph comes back
    // pre-compacted). refs is preserved verbatim: it can exceed the
    // TU-contributor count when untagged addEdge registrations exist.
    putU32(data, static_cast<uint32_t>(graph.liveEdgeCount_));
    for (size_t i = 0; i < graph.edges_.size(); ++i) {
      const auto &se = graph.edges_[i];
      if (se.refs == 0)
        continue;
      putU32(data, se.caller);
      putU32(data, se.callee);
      putU32(data, se.callSite);
      putU8(data, static_cast<uint8_t>(se.kind));
      putU8(data, static_cast<uint8_t>(se.confidence));
      putU8(data, static_cast<uint8_t>(se.execContext));
      putU32(data, se.indirectionDepth);
      putU32(data, se.refs);
      auto tit = edgeTus.find(i);
      if (tit == edgeTus.end()) {
        putU32(data, 0);
      } else {
        putU32(data, static_cast<uint32_t>(tit->second.size()));
        for (uint32_t tuId : tit->second)
          putU32(data, tuId);
      }
    }

    // Relationship maps as flat raw-id (a, b) pairs. Reverse maps
    // (overrideBases_, returnedBy_) are rebuilt from the forward pairs on
    // load, so only the forward maps are serialized.
    auto emitVecPairs =
        [&](const std::unordered_map<SId, std::vector<SId>> &map) {
          uint32_t pairCount = 0;
          for (const auto &kv : map)
            pairCount += static_cast<uint32_t>(kv.second.size());
          putU32(data, pairCount);
          for (const auto &kv : map)
            for (SId b : kv.second) {
              putU32(data, kv.first);
              putU32(data, b);
            }
        };
    auto emitSetPairs = [&](const std::unordered_map<SId, std::set<SId>> &map) {
      uint32_t pairCount = 0;
      for (const auto &kv : map)
        pairCount += static_cast<uint32_t>(kv.second.size());
      putU32(data, pairCount);
      for (const auto &kv : map)
        for (SId b : kv.second) {
          putU32(data, kv.first);
          putU32(data, b);
        }
    };

    emitVecPairs(graph.derivedClasses_);       // (base, derived)
    emitVecPairs(graph.methodOverrides_);      // (baseMethod, override)
    emitSetPairs(graph.effectiveImplClasses_); // (implMethod, concreteClass)
    emitSetPairs(graph.functionReturns_);      // (func, returnedFunc)
  }

  // Control flow: interner table, set tables in full table order (positions
  // are the stored indices, so no remap exists on load), then per-context
  // raw-id records.
  {
    std::lock_guard<std::mutex> lock(cfIndex.mutex_);

    emitInternerTable(data, cfIndex.interner_);

    // Scope/guard tables hold plain strings (never interned); they are
    // small, so inline strings are fine. Entry 0 (the seeded empty set) is
    // serialized too.
    putU32(data, static_cast<uint32_t>(cfIndex.scopeSets_.size()));
    for (const auto &set : cfIndex.scopeSets_) {
      putU32(data, static_cast<uint32_t>(set.size()));
      for (const auto &scope : set) {
        putLenStr(data, scope.tryLocation);
        putLenStr(data, scope.enclosingFunction);
        putU32(data, scope.nestingDepth);
        putU32(data, static_cast<uint32_t>(scope.handlers.size()));
        for (const auto &h : scope.handlers) {
          putLenStr(data, h.caughtType);
          putU8(data, h.isCatchAll ? 1 : 0);
          putLenStr(data, h.location);
          putLenStr(data, h.bodySummary);
        }
      }
    }

    putU32(data, static_cast<uint32_t>(cfIndex.guardSets_.size()));
    for (const auto &set : cfIndex.guardSets_) {
      putU32(data, static_cast<uint32_t>(set.size()));
      for (const auto &g : set) {
        putLenStr(data, g.conditionText);
        putLenStr(data, g.location);
        putU8(data, g.inTrueBranch ? 1 : 0);
        putU8(data, g.isAssertion ? 1 : 0);
      }
    }

    // RAII locals are already interned: raw id triples + kind.
    putU32(data, static_cast<uint32_t>(cfIndex.raiiSets_.size()));
    for (const auto &set : cfIndex.raiiSets_) {
      putU32(data, static_cast<uint32_t>(set.size()));
      for (const auto &l : set) {
        putU32(data, l.typeName);
        putU32(data, l.varName);
        putU32(data, l.declLocation);
        putU8(data, static_cast<uint8_t>(l.kind));
      }
    }

    // Live contexts only (tombstones dropped; the loaded index comes back
    // pre-compacted). The kNoString tuPath sentinel is written verbatim.
    putU32(data, static_cast<uint32_t>(cfIndex.liveCount_));
    for (const auto &se : cfIndex.contexts_) {
      if (!se.live)
        continue;
      putU32(data, se.caller);
      putU32(data, se.callee);
      putU32(data, se.callerDisplay);
      putU32(data, se.calleeDisplay);
      putU32(data, se.site);
      putU32(data, se.tuPath);
      putU32(data, se.scopeSet);
      putU32(data, se.guardSet);
      putU32(data, se.raiiSet);
      putU8(data, static_cast<uint8_t>(se.callerNoexcept));
      putU8(data, se.insideCatchBlock ? 1 : 0);
    }
  }

  // Channel index (v7): not interner-backed by design (ChannelIndex.h), so
  // records are plain length-prefixed strings rather than raw ids. Each
  // live site carries its refs count and the list of TU paths currently
  // contributing to it — same shape as CallGraph's edge/tuEdges_
  // serialization — so a loaded ChannelIndex's removeTU keeps working
  // correctly across several contributing TUs after a warm start.
  {
    std::lock_guard<std::mutex> lock(channels.mutex_);

    std::unordered_map<size_t, std::vector<std::string>> siteTus;
    for (const auto &[tuPath, idxs] : channels.byTu_)
      for (size_t idx : idxs)
        if (channels.sites_[idx].refs != 0)
          siteTus[idx].push_back(tuPath);

    putU32(data, static_cast<uint32_t>(channels.liveCount_));
    for (size_t i = 0; i < channels.sites_.size(); ++i) {
      const auto &se = channels.sites_[i];
      if (!se.live)
        continue;
      putLenStr(data, se.site.channelId);
      putLenStr(data, se.site.channelTypeName);
      putLenStr(data, se.site.category);
      putU8(data, static_cast<uint8_t>(se.site.op));
      putLenStr(data, se.site.siteFunctionUsr);
      putLenStr(data, se.site.siteFunctionDisplay);
      putLenStr(data, se.site.callSite);
      putU32(data, se.refs);
      putU32(data, static_cast<uint32_t>(se.site.enclosingGuards.size()));
      for (const auto &g : se.site.enclosingGuards) {
        putLenStr(data, g.conditionText);
        putLenStr(data, g.location);
        putU8(data, g.inTrueBranch ? 1 : 0);
        putU8(data, g.isAssertion ? 1 : 0);
      }
      auto tit = siteTus.find(i);
      if (tit == siteTus.end()) {
        putU32(data, 0);
      } else {
        putU32(data, static_cast<uint32_t>(tit->second.size()));
        for (const auto &tu : tit->second)
          putLenStr(data, tu);
      }
    }
  }

  // Assemble the file: header, then the data section. Write to a temp file
  // and rename so a crash mid-write never leaves a torn snapshot.
  std::string tmpPath = path + ".tmp";
  {
    std::error_code ec;
    llvm::raw_fd_ostream os(tmpPath, ec, llvm::sys::fs::OF_None);
    if (ec)
      return false;

    os.write(kMagic, 4);
    std::string header;
    putU32(header, kFormatVersion);
    os << header << data;
    os.flush();
    if (os.has_error())
      return false;
  }

  if (llvm::sys::fs::rename(tmpPath, path)) {
    llvm::sys::fs::remove(tmpPath);
    return false;
  }
  return true;
}

std::optional<SnapshotData> SnapshotIO::load(const std::string &path) {
  using SId = StringInterner::Id;

  auto bufOrErr = llvm::MemoryBuffer::getFile(path, /*IsText=*/false,
                                              /*RequiresNullTerminator=*/false);
  if (!bufOrErr)
    return std::nullopt;
  auto &buf = *bufOrErr;

  Reader r{buf->getBufferStart(), buf->getBufferEnd()};

  std::string magic = r.bytes(4);
  if (!r.ok || magic != std::string(kMagic, 4))
    return std::nullopt;
  if (r.u32() != kFormatVersion)
    return std::nullopt;

  SnapshotData out;
  if (!readMeta(r, out.meta))
    return std::nullopt;

  // Graph interner table: installed ids match the saved ids by position, so
  // every raw id below is valid verbatim — no interning per record.
  if (!readInternerTable(r, out.graph.interner_))
    return std::nullopt;

  {
    CallGraph &g = out.graph;
    const uint32_t internedCount = static_cast<uint32_t>(g.interner_.size());
    // Bounds-check a stored graph-interner id (corrupt-input guard).
    auto gid = [&](uint32_t id) {
      if (id >= internedCount)
        r.ok = false;
      return id;
    };

    // Freshly constructed and not yet shared, but keep the mutating-ops-
    // hold-mutex_ discipline while writing private state.
    std::lock_guard<std::mutex> lock(g.mutex_);

    // Nodes: direct install, rebuilding nodeContributors_/tuNodes_ from the
    // per-node contributor lists.
    uint32_t nodeCount = r.count();
    g.nodes_.reserve(nodeCount);
    g.nodeContributors_.reserve(nodeCount);
    g.outEdges_.reserve(nodeCount);
    g.inEdges_.reserve(nodeCount);
    for (uint32_t i = 0; r.ok && i < nodeCount; ++i) {
      SId nameId = gid(r.u32());
      SId displayId = gid(r.u32());
      CallGraphNode node;
      node.file = r.lenStr();
      node.line = r.u32();
      uint8_t flags = r.u8();
      node.isEntryPoint = (flags & 1) != 0;
      node.isVirtual = (flags & 2) != 0;
      node.enclosingClass = r.lenStr();
      uint32_t contribCount = r.count();
      if (!r.ok)
        break;
      node.usr = g.interner_.resolve(nameId);
      node.qualifiedName = g.interner_.resolve(displayId);
      // Rebuild the disambiguation index (no extra serialization needed).
      auto &candidates = g.byName_[displayId];
      if (std::find(candidates.begin(), candidates.end(), nameId) ==
          candidates.end())
        candidates.push_back(nameId);
      g.nodes_.emplace(nameId, std::move(node));
      for (uint32_t c = 0; r.ok && c < contribCount; ++c) {
        SId tuId = gid(r.u32());
        if (!r.ok)
          break;
        if (g.nodeContributors_[nameId].insert(tuId).second)
          g.tuNodes_[tuId].push_back(nameId);
      }
    }

    // Edges: direct install (indices are the new deque positions),
    // rebuilding edgeIndex_/outEdges_/inEdges_/tuEdges_ as we go. Saved
    // edges are all live, so refs == 0 marks a corrupt record.
    uint32_t edgeCount = r.count();
    g.edgeIndex_.reserve(edgeCount);
    for (uint32_t i = 0; r.ok && i < edgeCount; ++i) {
      CallGraph::StoredEdge se;
      se.caller = gid(r.u32());
      se.callee = gid(r.u32());
      se.callSite = gid(r.u32());
      se.kind = static_cast<EdgeKind>(r.u8());
      se.confidence = static_cast<Confidence>(r.u8());
      se.execContext = static_cast<ExecutionContext>(r.u8());
      se.indirectionDepth = r.u32();
      se.refs = r.u32();
      uint32_t contribCount = r.count();
      if (se.refs == 0)
        r.ok = false;
      if (!r.ok)
        break;
      size_t idx = g.edges_.size();
      g.outEdges_[se.caller].push_back(idx);
      g.inEdges_[se.callee].push_back(idx);
      g.edgeIndex_.emplace(CallGraph::keyOf(se), idx);
      g.edges_.push_back(se);
      ++g.liveEdgeCount_;
      for (uint32_t c = 0; r.ok && c < contribCount; ++c) {
        SId tuId = gid(r.u32());
        if (r.ok)
          g.tuEdges_[tuId].push_back(idx);
      }
    }

    // Relationship pairs: direct install into the forward AND reverse maps.
    // Saved data is already deduped (it came out of these same maps), so the
    // linear dedup find the public mutators do is skipped.
    auto readVecPairs =
        [&](std::unordered_map<SId, std::vector<SId>> &fwd,
            std::unordered_map<SId, std::vector<SId>> *rev) {
          uint32_t n = r.count();
          for (uint32_t i = 0; r.ok && i < n; ++i) {
            SId a = gid(r.u32());
            SId b = gid(r.u32());
            if (!r.ok)
              break;
            fwd[a].push_back(b);
            if (rev)
              (*rev)[b].push_back(a);
          }
        };
    readVecPairs(g.derivedClasses_, nullptr);
    readVecPairs(g.methodOverrides_, &g.overrideBases_);
    uint32_t n = r.count();
    for (uint32_t i = 0; r.ok && i < n; ++i) {
      SId impl = gid(r.u32());
      SId cls = gid(r.u32());
      if (r.ok)
        g.effectiveImplClasses_[impl].insert(cls);
    }
    n = r.count();
    for (uint32_t i = 0; r.ok && i < n; ++i) {
      SId fn = gid(r.u32());
      SId ret = gid(r.u32());
      if (!r.ok)
        break;
      g.functionReturns_[fn].insert(ret);
      g.returnedBy_[ret].push_back(fn);
    }
  }
  if (!r.ok)
    return std::nullopt;

  // Control flow: interner, set tables (positions preserved verbatim), then
  // the direct-install context loop — no interning, no key building per
  // context.
  if (!readInternerTable(r, out.cfIndex.interner_))
    return std::nullopt;

  {
    ControlFlowIndex &cf = out.cfIndex;
    const uint32_t internedCount = static_cast<uint32_t>(cf.interner_.size());
    auto cid = [&](uint32_t id) {
      if (id >= internedCount)
        r.ok = false;
      return id;
    };

    std::lock_guard<std::mutex> lock(cf.mutex_);

    // Set tables were saved in full table order (entry 0 = the seeded empty
    // set), so clear the constructor's seed and refill: stored indices need
    // no remap.
    cf.scopeSets_.clear();
    uint32_t setCount = r.count();
    for (uint32_t i = 0; r.ok && i < setCount; ++i) {
      std::vector<TryCatchScope> set;
      uint32_t tryCount = r.count();
      set.reserve(tryCount);
      for (uint32_t t = 0; r.ok && t < tryCount; ++t) {
        TryCatchScope scope;
        scope.tryLocation = r.lenStr();
        scope.enclosingFunction = r.lenStr();
        scope.nestingDepth = r.u32();
        uint32_t handlerCount = r.count();
        for (uint32_t h = 0; r.ok && h < handlerCount; ++h) {
          CatchHandlerInfo info;
          info.caughtType = r.lenStr();
          info.isCatchAll = r.u8() != 0;
          info.location = r.lenStr();
          info.bodySummary = r.lenStr();
          scope.handlers.push_back(std::move(info));
        }
        set.push_back(std::move(scope));
      }
      cf.scopeSets_.push_back(std::move(set));
    }

    cf.guardSets_.clear();
    setCount = r.count();
    for (uint32_t i = 0; r.ok && i < setCount; ++i) {
      std::vector<ConditionalGuard> set;
      uint32_t guardCount = r.count();
      set.reserve(guardCount);
      for (uint32_t gi = 0; r.ok && gi < guardCount; ++gi) {
        ConditionalGuard guard;
        guard.conditionText = r.lenStr();
        guard.location = r.lenStr();
        guard.inTrueBranch = r.u8() != 0;
        guard.isAssertion = r.u8() != 0;
        set.push_back(std::move(guard));
      }
      cf.guardSets_.push_back(std::move(set));
    }

    cf.raiiSets_.clear();
    setCount = r.count();
    for (uint32_t i = 0; r.ok && i < setCount; ++i) {
      std::vector<ControlFlowIndex::StoredRaiiLocal> set;
      uint32_t raiiCount = r.count();
      set.reserve(raiiCount);
      for (uint32_t l = 0; r.ok && l < raiiCount; ++l) {
        ControlFlowIndex::StoredRaiiLocal local;
        local.typeName = cid(r.u32());
        local.varName = cid(r.u32());
        local.declLocation = cid(r.u32());
        local.kind = static_cast<RaiiKind>(r.u8());
        set.push_back(local);
      }
      cf.raiiSets_.push_back(std::move(set));
    }

    // The empty set must live at index 0 of each table (addCallSiteContext
    // maps empty sets to 0 without a lookup); a snapshot violating that is
    // corrupt.
    if (cf.scopeSets_.empty() || !cf.scopeSets_[0].empty() ||
        cf.guardSets_.empty() || !cf.guardSets_[0].empty() ||
        cf.raiiSets_.empty() || !cf.raiiSets_[0].empty())
      r.ok = false;

    // Rebuild the dedup key maps so post-load addCallSiteContext dedups
    // against the loaded tables (tables are small; the empty entry 0 is
    // never keyed — intern*Set returns 0 for empty sets structurally).
    if (r.ok) {
      for (uint32_t i = 1; i < cf.scopeSets_.size(); ++i)
        cf.scopeSetIds_.emplace(
            ControlFlowIndex::scopeSetKey(cf.scopeSets_[i]), i);
      for (uint32_t i = 1; i < cf.guardSets_.size(); ++i)
        cf.guardSetIds_.emplace(
            ControlFlowIndex::guardSetKey(cf.guardSets_[i]), i);
      for (uint32_t i = 1; i < cf.raiiSets_.size(); ++i)
        cf.raiiSetIds_.emplace(ControlFlowIndex::raiiSetKey(cf.raiiSets_[i]),
                               i);
    }

    uint32_t ctxCount = r.count();
    // Same pre-sizing reserveContexts does (it locks mutex_, held here).
    cf.byCallee_.reserve(ctxCount);
    cf.byCaller_.reserve(ctxCount);
    cf.bySite_.reserve(ctxCount);
    for (uint32_t i = 0; r.ok && i < ctxCount; ++i) {
      SId caller = cid(r.u32());
      SId callee = cid(r.u32());
      SId callerDisplay = cid(r.u32());
      SId calleeDisplay = cid(r.u32());
      SId site = cid(r.u32());
      SId tuPath = r.u32();
      if (tuPath != ControlFlowIndex::kNoString)
        cid(tuPath);
      uint32_t scopeSet = r.u32();
      uint32_t guardSet = r.u32();
      uint32_t raiiSet = r.u32();
      auto noexceptSpec = static_cast<NoexceptSpec>(r.u8());
      bool insideCatch = r.u8() != 0;
      if (scopeSet >= cf.scopeSets_.size() ||
          guardSet >= cf.guardSets_.size() || raiiSet >= cf.raiiSets_.size())
        r.ok = false;
      if (!r.ok)
        break;
      cf.insertStored(caller, callee, callerDisplay, calleeDisplay, site,
                      tuPath, scopeSet, guardSet, raiiSet, noexceptSpec,
                      insideCatch);
    }
  }

  if (!r.ok)
    return std::nullopt;

  {
    ChannelIndex &ch = out.channels;
    std::lock_guard<std::mutex> lock(ch.mutex_);
    uint32_t count = r.count();
    for (uint32_t i = 0; r.ok && i < count; ++i) {
      ChannelSite site;
      site.channelId = r.lenStr();
      site.channelTypeName = r.lenStr();
      site.category = r.lenStr();
      site.op = static_cast<ChannelOperation>(r.u8());
      site.siteFunctionUsr = r.lenStr();
      site.siteFunctionDisplay = r.lenStr();
      site.callSite = r.lenStr();
      uint32_t refs = r.u32();
      uint32_t guardCount = r.count();
      for (uint32_t g = 0; r.ok && g < guardCount; ++g) {
        ConditionalGuard guard;
        guard.conditionText = r.lenStr();
        guard.location = r.lenStr();
        guard.inTrueBranch = r.u8() != 0;
        guard.isAssertion = r.u8() != 0;
        site.enclosingGuards.push_back(std::move(guard));
      }
      uint32_t tuCount = r.count();
      std::vector<std::string> tus;
      tus.reserve(tuCount);
      for (uint32_t t = 0; r.ok && t < tuCount; ++t)
        tus.push_back(r.lenStr());
      if (!r.ok || refs == 0) {
        r.ok = false;
        break;
      }
      site.tuPath = tus.empty() ? std::string() : tus.front();

      size_t idx = ch.sites_.size();
      ChannelIndex::SiteKey key{site.channelId, site.callSite,
                                site.siteFunctionUsr, site.op};
      std::string channelId = site.channelId;
      std::string funcUsr = site.siteFunctionUsr;
      std::string funcDisplay = site.siteFunctionDisplay;
      bool differentDisplay = funcDisplay != funcUsr;
      ch.sites_.push_back(
          ChannelIndex::StoredSite{std::move(site), refs, true});
      ch.index_.emplace(key, idx);
      ch.byChannel_[channelId].push_back(idx);
      ch.byFunctionUsr_[funcUsr].push_back(idx);
      if (differentDisplay)
        ch.byFunctionDisplay_[funcDisplay].push_back(idx);
      for (const auto &tu : tus)
        ch.byTu_[tu].push_back(idx);
      ++ch.liveCount_;
    }
  }

  if (!r.ok)
    return std::nullopt;

  return out;
}

std::vector<FileStamp>
SnapshotIO::stampFiles(const std::vector<std::string> &files) {
  std::vector<FileStamp> stamps;
  stamps.reserve(files.size());
  for (const auto &f : files) {
    FileStamp fs;
    fs.path = f;
    llvm::sys::fs::file_status st;
    if (!llvm::sys::fs::status(f, st)) {
      fs.mtimeNs = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              st.getLastModificationTime().time_since_epoch())
              .count());
      fs.size = st.getSize();
    }
    stamps.push_back(std::move(fs));
  }
  return stamps;
}

} // namespace vycor
