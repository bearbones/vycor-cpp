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
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cassert>
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
  putU32(out, static_cast<uint32_t>(meta.entryPoints.size()));
  for (const auto &s : meta.entryPoints)
    putLenStr(out, s);
  // v9: dependency table, then per TU (in meta.files order) its indices.
  putU32(out, static_cast<uint32_t>(meta.deps.size()));
  for (const auto &d : meta.deps) {
    putLenStr(out, d.path);
    putU64(out, d.mtimeNs);
    putU64(out, d.size);
  }
  static const std::vector<uint32_t> kNoDeps;
  for (size_t i = 0; i < meta.files.size(); ++i) {
    const auto &ids = i < meta.tuDeps.size() ? meta.tuDeps[i] : kNoDeps;
    putU32(out, static_cast<uint32_t>(ids.size()));
    for (uint32_t id : ids)
      putU32(out, id);
  }
  // v10: provenance, then per TU (in meta.files order) its fingerprint
  // and outcome. A TU the caller recorded neither for is written as an
  // unknown fingerprint (never matches) and a Skipped outcome.
  putLenStr(out, meta.provenance.analyzer);
  putLenStr(out, meta.provenance.toolchain);
  putLenStr(out, meta.provenance.environment);
  putU64(out, meta.provenance.bakeStartNs);
  static const TuOutcome kNoOutcome;
  for (size_t i = 0; i < meta.files.size(); ++i) {
    putLenStr(out, i < meta.fingerprints.size() ? meta.fingerprints[i]
                                                : std::string());
    const auto &oc = i < meta.outcomes.size() ? meta.outcomes[i] : kNoOutcome;
    putU8(out, static_cast<uint8_t>(oc.status));
    putLenStr(out, oc.detail);
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
  n = r.count();
  for (uint32_t i = 0; r.ok && i < n; ++i)
    meta.entryPoints.push_back(r.lenStr());
  n = r.count();
  for (uint32_t i = 0; r.ok && i < n; ++i) {
    FileStamp fs;
    fs.path = r.lenStr();
    fs.mtimeNs = r.u64();
    fs.size = r.u64();
    meta.deps.push_back(std::move(fs));
  }
  meta.tuDeps.resize(meta.files.size());
  for (size_t i = 0; r.ok && i < meta.files.size(); ++i) {
    uint32_t m = r.count();
    for (uint32_t j = 0; r.ok && j < m; ++j) {
      uint32_t id = r.u32();
      if (id >= meta.deps.size()) {
        r.ok = false;
        break;
      }
      meta.tuDeps[i].push_back(id);
    }
  }
  meta.provenance.analyzer = r.lenStr();
  meta.provenance.toolchain = r.lenStr();
  meta.provenance.environment = r.lenStr();
  meta.provenance.bakeStartNs = r.u64();
  meta.fingerprints.resize(meta.files.size());
  meta.outcomes.resize(meta.files.size());
  for (size_t i = 0; r.ok && i < meta.files.size(); ++i) {
    meta.fingerprints[i] = r.lenStr();
    uint8_t status = r.u8();
    if (status > static_cast<uint8_t>(TuStatus::Skipped)) {
      r.ok = false;
      break;
    }
    meta.outcomes[i].status = static_cast<TuStatus>(status);
    meta.outcomes[i].detail = r.lenStr();
  }
  return r.ok;
}

// v8 section table kinds, in file order. The bit a kind maps to in
// IndexSection is 1 << (kind - 1); meta (kind 0) is always present.
constexpr uint8_t kMetaKind = 0;
constexpr uint8_t kGraphKind = 1;
constexpr uint8_t kControlFlowKind = 2;
constexpr uint8_t kChannelsKind = 3;
constexpr uint32_t kSectionKinds = 4;

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
  // One buffer per v8 section, concatenated behind the header below.
  std::string sections[kSectionKinds];

  emitMeta(sections[kMetaKind], meta);

  {
    // Reading private state directly; hold the graph lock for a consistent
    // view (save may be called while the serve loop is idle, but cheap
    // insurance against future callers). Every graph mutator holds this
    // lock too, so the interner cannot grow between the table emit and the
    // record emits below.
    std::lock_guard<std::mutex> lock(graph.mutex_);
    std::string &data = sections[kGraphKind];

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
    std::string &data = sections[kControlFlowKind];

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
          putU8(data, h.rethrows ? 1 : 0);
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
    std::string &data = sections[kChannelsKind];

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

  // Assemble the file: header (version, summary counts, section table),
  // then the sections. Write to a temp file and rename so a crash
  // mid-write never leaves a torn snapshot.
  std::string tmpPath = path + ".tmp";
  {
    // The default index location lives in a directory that may not exist
    // yet (<build-path>/.vycor/); the rename below needs it to.
    llvm::sys::fs::create_directories(llvm::sys::path::parent_path(path));
    std::error_code ec;
    llvm::raw_fd_ostream os(tmpPath, ec, llvm::sys::fs::OF_None);
    if (ec)
      return false;

    os.write(kMagic, 4);
    std::string header;
    putU32(header, kFormatVersion);
    putU64(header, graph.nodeCount());
    putU64(header, graph.edgeCount());
    putU64(header, cfIndex.size());
    putU64(header, channels.size());
    putU32(header, kSectionKinds);
    uint64_t offset = kHeaderBytes;
    for (uint32_t kind = 0; kind < kSectionKinds; ++kind) {
      putU8(header, static_cast<uint8_t>(kind));
      putU64(header, offset);
      putU64(header, sections[kind].size());
      offset += sections[kind].size();
    }
    assert(header.size() + 4 == kHeaderBytes);
    os << header;
    for (const auto &section : sections)
      os << section;
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

std::optional<SnapshotData> SnapshotIO::load(const std::string &path,
                                             SnapshotLoadStats *stats,
                                             LoadMode mode, unsigned needs) {
  using SId = StringInterner::Id;
  using Clock = std::chrono::steady_clock;
  const bool mutableLoad = mode == LoadMode::Mutable;

  const auto loadStart = Clock::now();
  auto bufOrErr = llvm::MemoryBuffer::getFile(path, /*IsText=*/false,
                                              /*RequiresNullTerminator=*/false);
  if (!bufOrErr)
    return std::nullopt;
  auto &buf = *bufOrErr;

  Reader r{buf->getBufferStart(), buf->getBufferEnd()};

  // Section accounting: `mark(name)` closes the section that began at the
  // previous mark. Cheap enough to run unconditionally (nine calls).
  const char *sectionStart = r.p;
  auto sectionClock = loadStart;
  auto mark = [&](const char *name) {
    auto now = Clock::now();
    if (stats) {
      SnapshotLoadSection sec;
      sec.name = name;
      sec.bytes = static_cast<uint64_t>(r.p - sectionStart);
      sec.ms = std::chrono::duration<double, std::milli>(now - sectionClock)
                   .count();
      stats->sections.push_back(sec);
    }
    sectionStart = r.p;
    sectionClock = now;
  };
  auto finish = [&]() {
    if (stats) {
      stats->fileBytes = static_cast<uint64_t>(buf->getBufferSize());
      stats->totalMs =
          std::chrono::duration<double, std::milli>(Clock::now() - loadStart)
              .count();
    }
  };

  std::string magic = r.bytes(4);
  if (!r.ok || magic != std::string(kMagic, 4)) {
    finish();
    return std::nullopt;
  }
  if (r.u32() != kFormatVersion) {
    finish();
    return std::nullopt;
  }

  SnapshotData out;
  out.summary.nodes = r.u64();
  out.summary.edges = r.u64();
  out.summary.callSites = r.u64();
  out.summary.channelSites = r.u64();

  // Section table: every kind must be present exactly once and lie within
  // the file. Each section is decoded through its own bounded Reader, so
  // a record overrunning its section is caught as corruption.
  struct Range {
    const char *begin = nullptr;
    uint64_t length = 0;
    bool present = false;
  };
  Range ranges[kSectionKinds];
  const uint64_t fileSize = buf->getBufferSize();
  uint32_t tableCount = r.u32();
  if (!r.ok || tableCount != kSectionKinds) {
    finish();
    return std::nullopt;
  }
  for (uint32_t i = 0; r.ok && i < tableCount; ++i) {
    uint8_t kind = r.u8();
    uint64_t offset = r.u64();
    uint64_t length = r.u64();
    if (!r.ok || kind >= kSectionKinds || ranges[kind].present ||
        offset > fileSize || length > fileSize - offset) {
      r.ok = false;
      break;
    }
    ranges[kind] = Range{buf->getBufferStart() + offset, length, true};
  }
  if (!r.ok || static_cast<uint64_t>(r.p - buf->getBufferStart()) !=
                   kHeaderBytes) {
    finish();
    return std::nullopt;
  }
  auto beginSection = [&](uint8_t kind) {
    r = Reader{ranges[kind].begin, ranges[kind].begin + ranges[kind].length};
    sectionStart = r.p;
    sectionClock = Clock::now();
  };
  // A section must be consumed exactly; a short read is layout drift.
  auto sectionDone = [&]() {
    if (r.ok && r.p != r.end)
      r.ok = false;
  };
  auto skipSection = [&](const char *name, uint8_t kind) {
    if (stats) {
      SnapshotLoadSection sec;
      sec.name = name;
      sec.bytes = ranges[kind].length;
      sec.skipped = true;
      stats->sections.push_back(sec);
    }
  };

  beginSection(kMetaKind);
  if (!readMeta(r, out.meta)) {
    finish();
    return std::nullopt;
  }
  sectionDone();
  if (!r.ok) {
    finish();
    return std::nullopt;
  }
  mark("meta");

  if (needs & kSectionGraph) {
    beginSection(kGraphKind);
    // Graph interner table: installed ids match the saved ids by position, so
    // every raw id below is valid verbatim — no interning per record.
    if (!readInternerTable(r, out.graph.interner_)) {
      finish();
      return std::nullopt;
    }
    mark("graph_interner");

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
      g.readOnly_ = !mutableLoad;

      // Nodes: direct install, rebuilding nodeContributors_/tuNodes_ from the
      // per-node contributor lists (skipped over on a read-only load: they
      // exist for removeTU/absorb only, and were ~12 us per node — 1.2 s of
      // a 5.3 s load on the 938-TU testbed).
      uint32_t nodeCount = r.count();
      g.nodes_.reserve(nodeCount);
      if (mutableLoad)
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
          if (!r.ok || !mutableLoad)
            continue;
          if (g.nodeContributors_[nameId].insert(tuId).second)
            g.tuNodes_[tuId].push_back(nameId);
        }
      }

      mark("nodes");

      // Edges: direct install (indices are the new deque positions),
      // rebuilding edgeIndex_/outEdges_/inEdges_/tuEdges_ as we go. Saved
      // edges are all live, so refs == 0 marks a corrupt record.
      uint32_t edgeCount = r.count();
      if (mutableLoad)
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
        if (mutableLoad)
          g.edgeIndex_.emplace(CallGraph::keyOf(se), idx);
        g.edges_.push_back(se);
        ++g.liveEdgeCount_;
        for (uint32_t c = 0; r.ok && c < contribCount; ++c) {
          SId tuId = gid(r.u32());
          if (r.ok && mutableLoad)
            g.tuEdges_[tuId].push_back(idx);
        }
      }

      mark("edges");

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
      mark("graph_relations");
    }
    sectionDone();
    if (!r.ok) {
      finish();
      return std::nullopt;
    }
    out.loaded |= kSectionGraph;
  } else {
    skipSection("graph", kGraphKind);
  }

  if (needs & kSectionControlFlow) {
    beginSection(kControlFlowKind);
    // Control flow: interner, set tables (positions preserved verbatim), then
    // the direct-install context loop — no interning, no key building per
    // context.
    if (!readInternerTable(r, out.cfIndex.interner_)) {
      finish();
      return std::nullopt;
    }
    mark("cf_interner");

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
            info.rethrows = r.u8() != 0;
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
      if (r.ok && mutableLoad) {
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

      mark("cf_set_tables");

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
                        insideCatch, /*trackTu=*/mutableLoad);
      }
      mark("cf_contexts");
    }
    sectionDone();
    if (!r.ok) {
      finish();
      return std::nullopt;
    }
    out.loaded |= kSectionControlFlow;
  } else {
    skipSection("control_flow", kControlFlowKind);
  }

  if (needs & kSectionChannels) {
    beginSection(kChannelsKind);
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
        if (mutableLoad)
          for (const auto &tu : tus)
            ch.byTu_[tu].push_back(idx);
        ++ch.liveCount_;
      }
      mark("channels");
    }
    sectionDone();
    out.loaded |= kSectionChannels;
  } else {
    skipSection("channels", kChannelsKind);
  }

  finish();
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

namespace {

constexpr uint64_t kNsPerSecond = 1000000000ull;

/// A recorded dependency stamp (the frontend's whole-second mtime) matches
/// the file as it is now when the sizes agree and the current mtime falls
/// in that second.
/// A recorded stamp with mtime 0 was marked unstable (markUnstableStamps)
/// or never taken; it matches nothing.
bool sameParsedVersion(const FileStamp &recorded, const FileStamp &now) {
  return recorded.mtimeNs != 0 && recorded.size == now.size &&
         recorded.mtimeNs == now.mtimeNs - now.mtimeNs % kNsPerSecond;
}

bool sameTuVersion(const FileStamp &recorded, const FileStamp &now) {
  return recorded.mtimeNs != 0 && recorded == now;
}

} // anonymous namespace

const char *tuStatusName(TuStatus status) {
  switch (status) {
  case TuStatus::Indexed:
    return "indexed";
  case TuStatus::Partial:
    return "partial";
  case TuStatus::Crashed:
    return "crashed";
  case TuStatus::Poisoned:
    return "poisoned";
  case TuStatus::Skipped:
    return "skipped";
  }
  return "skipped";
}

IndexCoverage coverageOf(const SnapshotMeta &meta) {
  IndexCoverage c;
  c.requested = meta.files.size();
  for (size_t i = 0; i < meta.files.size(); ++i) {
    TuStatus s = i < meta.outcomes.size() ? meta.outcomes[i].status
                                          : TuStatus::Skipped;
    if (s == TuStatus::Indexed)
      ++c.indexed;
    else if (s == TuStatus::Partial)
      ++c.partial;
    else
      ++c.failed;
  }
  return c;
}

std::vector<bool>
SnapshotIO::dirtyTUs(const SnapshotMeta &meta,
                     const std::vector<FileStamp> &current,
                     const std::vector<std::string> *fingerprints,
                     DirtyReport *report) {
  std::unordered_map<std::string, size_t> recorded;
  recorded.reserve(meta.files.size());
  for (size_t i = 0; i < meta.files.size(); ++i)
    recorded[meta.files[i].path] = i;

  // Stat every dependency once, not once per includer.
  std::vector<std::string> depPaths;
  depPaths.reserve(meta.deps.size());
  for (const auto &d : meta.deps)
    depPaths.push_back(d.path);
  auto depsNow = stampFiles(depPaths);
  std::vector<bool> depChanged(meta.deps.size(), false);
  for (size_t i = 0; i < meta.deps.size(); ++i)
    depChanged[i] = !sameParsedVersion(meta.deps[i], depsNow[i]);

  std::vector<bool> dirty(current.size(), false);
  DirtyReport why;
  why.reasons.assign(current.size(), DirtyReason::Clean);
  for (size_t i = 0; i < current.size(); ++i) {
    auto it = recorded.find(current[i].path);
    if (it == recorded.end() || !sameTuVersion(meta.files[it->second],
                                               current[i])) {
      dirty[i] = true;
      why.reasons[i] = DirtyReason::Stamp;
      continue;
    }
    const size_t r = it->second;
    // A fingerprint the meta does not carry counts as changed: the TU was
    // baked under inputs nobody recorded.
    if (fingerprints && (r >= meta.fingerprints.size() ||
                         i >= fingerprints->size() ||
                         meta.fingerprints[r] != (*fingerprints)[i])) {
      dirty[i] = true;
      why.reasons[i] = DirtyReason::Inputs;
      ++why.viaInputs;
      continue;
    }
    if (r < meta.tuDeps.size()) {
      for (uint32_t id : meta.tuDeps[r]) {
        if (id < depChanged.size() && depChanged[id]) {
          dirty[i] = true;
          why.reasons[i] = DirtyReason::Deps;
          ++why.viaDeps;
          break;
        }
      }
      if (dirty[i])
        continue;
    }
    // Retry anything but a clean parse. A meta with no outcome column at
    // all (built in memory, never through recordOutcomes) has nothing to
    // retry; a saved one always carries the column.
    if (!meta.outcomes.empty() &&
        (r >= meta.outcomes.size() ||
         meta.outcomes[r].status != TuStatus::Indexed)) {
      dirty[i] = true;
      why.reasons[i] = DirtyReason::Retry;
      ++why.retried;
    }
  }
  if (report)
    *report = std::move(why);
  return dirty;
}

size_t SnapshotIO::markUnstableStamps(SnapshotMeta &meta,
                                      uint64_t bakeStartNs) {
  // Whole seconds: the dependency stamps are whole seconds already, and a
  // TU stamped in the bake's first second on a coarse filesystem has the
  // same exposure.
  const uint64_t bakeSecondNs = bakeStartNs - bakeStartNs % kNsPerSecond;
  size_t marked = 0;
  auto mark = [&](FileStamp &fs) {
    if (fs.mtimeNs != 0 && fs.mtimeNs >= bakeSecondNs) {
      fs.mtimeNs = 0;
      ++marked;
    }
  };
  for (auto &fs : meta.files)
    mark(fs);
  for (auto &fs : meta.deps)
    mark(fs);
  return marked;
}

TuDependencies SnapshotIO::dependenciesOf(const SnapshotMeta &meta) {
  TuDependencies out;
  for (size_t i = 0; i < meta.files.size() && i < meta.tuDeps.size(); ++i) {
    auto &list = out[meta.files[i].path];
    for (uint32_t id : meta.tuDeps[i])
      if (id < meta.deps.size())
        list.push_back(meta.deps[id]);
  }
  return out;
}

void SnapshotIO::recordDependencies(SnapshotMeta &meta,
                                    const TuDependencies &deps) {
  meta.deps.clear();
  meta.tuDeps.assign(meta.files.size(), {});
  std::unordered_map<std::string, uint32_t> ids;
  for (size_t i = 0; i < meta.files.size(); ++i) {
    auto it = deps.find(meta.files[i].path);
    if (it == deps.end())
      continue;
    for (const auto &d : it->second) {
      auto [pos, inserted] =
          ids.emplace(d.path, static_cast<uint32_t>(meta.deps.size()));
      if (inserted) {
        meta.deps.push_back(d);
      } else {
        // Two parses saw different versions: keep the older stamp so the
        // TU that parsed it is dirtied by the newer file.
        FileStamp &kept = meta.deps[pos->second];
        if (d.mtimeNs < kept.mtimeNs)
          kept = d;
      }
      meta.tuDeps[i].push_back(pos->second);
    }
  }
}

TuOutcomes SnapshotIO::outcomesOf(const SnapshotMeta &meta) {
  TuOutcomes out;
  for (size_t i = 0; i < meta.files.size() && i < meta.outcomes.size(); ++i)
    out[meta.files[i].path] = meta.outcomes[i];
  return out;
}

void SnapshotIO::recordOutcomes(SnapshotMeta &meta,
                                const TuOutcomes &outcomes) {
  meta.outcomes.assign(meta.files.size(), TuOutcome{});
  for (size_t i = 0; i < meta.files.size(); ++i) {
    auto it = outcomes.find(meta.files[i].path);
    if (it != outcomes.end())
      meta.outcomes[i] = it->second;
    else
      meta.outcomes[i] = TuOutcome{TuStatus::Skipped, "no outcome recorded"};
  }
}

} // namespace vycor
