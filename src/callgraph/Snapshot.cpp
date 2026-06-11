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

#include <chrono>
#include <unordered_map>

namespace vycor {

namespace {

constexpr char kMagic[4] = {'V', 'Y', 'C', 'S'};

// ----------------------------------------------------------------------------
// String pool: every string in the data section is a u32 index into a table
// written once. The data section is emitted into a memory buffer first, so
// the pool is complete before the table hits the file.
// ----------------------------------------------------------------------------

class StringPool {
public:
  uint32_t id(const std::string &s) {
    auto it = ids_.find(s);
    if (it != ids_.end())
      return it->second;
    uint32_t newId = static_cast<uint32_t>(strings_.size());
    strings_.push_back(s);
    ids_.emplace(strings_.back(), newId);
    return newId;
  }

  const std::vector<std::string> &strings() const { return strings_; }

private:
  std::vector<std::string> strings_;
  std::unordered_map<std::string, uint32_t> ids_;
};

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

void putStr(std::string &out, StringPool &pool, const std::string &s) {
  putU32(out, pool.id(s));
}

// ----------------------------------------------------------------------------
// Bounds-checked reader. Any overrun or bad index flips ok to false; callers
// check once at section boundaries and treat failure as a corrupt snapshot.
// ----------------------------------------------------------------------------

struct Reader {
  const char *p;
  const char *end;
  const std::vector<std::string> *pool = nullptr;
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

  std::string str() {
    uint32_t id = u32();
    if (!ok || !pool || id >= pool->size()) {
      ok = false;
      return std::string();
    }
    return (*pool)[id];
  }

  /// Raw bytes (used for the string table itself, before pool exists).
  std::string bytes(uint32_t len) {
    if (p + len > end) {
      ok = false;
      return std::string();
    }
    std::string s(p, p + len);
    p += len;
    return s;
  }

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

void emitMeta(std::string &out, StringPool &pool, const SnapshotMeta &meta) {
  putU32(out, static_cast<uint32_t>(meta.collapsePaths.size()));
  for (const auto &s : meta.collapsePaths)
    putStr(out, pool, s);
  putU32(out, static_cast<uint32_t>(meta.lockAllowlist.size()));
  for (const auto &s : meta.lockAllowlist)
    putStr(out, pool, s);
  putU8(out, meta.lockBuiltins ? 1 : 0);
  putU32(out, static_cast<uint32_t>(meta.files.size()));
  for (const auto &f : meta.files) {
    putStr(out, pool, f.path);
    putU64(out, f.mtimeNs);
    putU64(out, f.size);
  }
}

bool readMeta(Reader &r, SnapshotMeta &meta) {
  uint32_t n = r.count();
  for (uint32_t i = 0; r.ok && i < n; ++i)
    meta.collapsePaths.push_back(r.str());
  n = r.count();
  for (uint32_t i = 0; r.ok && i < n; ++i)
    meta.lockAllowlist.push_back(r.str());
  meta.lockBuiltins = r.u8() != 0;
  n = r.count();
  for (uint32_t i = 0; r.ok && i < n; ++i) {
    FileStamp fs;
    fs.path = r.str();
    fs.mtimeNs = r.u64();
    fs.size = r.u64();
    meta.files.push_back(std::move(fs));
  }
  return r.ok;
}

} // anonymous namespace

// ----------------------------------------------------------------------------
// SnapshotIO
// ----------------------------------------------------------------------------

bool SnapshotIO::save(const std::string &path, const CallGraph &graph,
                      const ControlFlowIndex &cfIndex,
                      const SnapshotMeta &meta) {
  StringPool pool;
  std::string data;

  emitMeta(data, pool, meta);

  {
    // Reading private state directly; hold the graph lock for a consistent
    // view (save may be called while the serve loop is idle, but cheap
    // insurance against future callers).
    std::lock_guard<std::mutex> lock(graph.mutex_);

    // Invert tuEdges_: edge index -> contributor TU pool ids (an edge can
    // be registered by several TUs once dedup merges identical edges).
    std::unordered_map<size_t, std::vector<uint32_t>> edgeTus;
    for (const auto &[tuId, idxs] : graph.tuEdges_) {
      uint32_t tuPoolId = pool.id(graph.interner_.resolve(tuId));
      for (size_t idx : idxs)
        edgeTus[idx].push_back(tuPoolId);
    }

    // Nodes with their contributor TU sets.
    putU32(data, static_cast<uint32_t>(graph.nodes_.size()));
    for (const auto &[nameId, node] : graph.nodes_) {
      putStr(data, pool, node.qualifiedName);
      putStr(data, pool, node.file);
      putU32(data, node.line);
      uint8_t flags = (node.isEntryPoint ? 1 : 0) | (node.isVirtual ? 2 : 0);
      putU8(data, flags);
      putStr(data, pool, node.enclosingClass);
      auto cit = graph.nodeContributors_.find(nameId);
      if (cit == graph.nodeContributors_.end()) {
        putU32(data, 0);
      } else {
        putU32(data, static_cast<uint32_t>(cit->second.size()));
        for (StringInterner::Id tuId : cit->second)
          putStr(data, pool, graph.interner_.resolve(tuId));
      }
    }

    // Live edges (tombstones are dropped; the loaded graph comes back
    // pre-compacted). Each edge carries its contributor TU list so the
    // loaded graph's removeTU behaves identically.
    putU32(data, static_cast<uint32_t>(graph.liveEdgeCount_));
    for (size_t i = 0; i < graph.edges_.size(); ++i) {
      const auto &se = graph.edges_[i];
      if (se.refs == 0)
        continue;
      putStr(data, pool, graph.interner_.resolve(se.caller));
      putStr(data, pool, graph.interner_.resolve(se.callee));
      putU8(data, static_cast<uint8_t>(se.kind));
      putU8(data, static_cast<uint8_t>(se.confidence));
      putStr(data, pool, graph.interner_.resolve(se.callSite));
      putU32(data, se.indirectionDepth);
      putU8(data, static_cast<uint8_t>(se.execContext));
      auto tit = edgeTus.find(i);
      if (tit == edgeTus.end()) {
        putU32(data, 0);
      } else {
        putU32(data, static_cast<uint32_t>(tit->second.size()));
        for (uint32_t tuPoolId : tit->second)
          putU32(data, tuPoolId);
      }
    }

    // Relationship maps, as flat (a, b) pairs re-added through the public
    // API on load.
    auto emitPairs =
        [&](const std::unordered_map<StringInterner::Id,
                                     std::vector<StringInterner::Id>> &map) {
          uint32_t pairCount = 0;
          for (const auto &kv : map)
            pairCount += static_cast<uint32_t>(kv.second.size());
          putU32(data, pairCount);
          for (const auto &[a, bs] : map) {
            uint32_t aId = pool.id(graph.interner_.resolve(a));
            for (StringInterner::Id b : bs) {
              putU32(data, aId);
              putStr(data, pool, graph.interner_.resolve(b));
            }
          }
        };
    auto emitSetPairs =
        [&](const std::unordered_map<StringInterner::Id,
                                     std::set<StringInterner::Id>> &map) {
          uint32_t pairCount = 0;
          for (const auto &kv : map)
            pairCount += static_cast<uint32_t>(kv.second.size());
          putU32(data, pairCount);
          for (const auto &[a, bs] : map) {
            uint32_t aId = pool.id(graph.interner_.resolve(a));
            for (StringInterner::Id b : bs) {
              putU32(data, aId);
              putStr(data, pool, graph.interner_.resolve(b));
            }
          }
        };

    emitPairs(graph.derivedClasses_);   // (base, derived)
    emitPairs(graph.methodOverrides_);  // (baseMethod, override)
    emitSetPairs(graph.effectiveImplClasses_); // (implMethod, concreteClass)
    emitSetPairs(graph.functionReturns_);      // (func, returnedFunc)
  }

  // Control flow contexts (live only, via the public accessor).
  {
    auto contexts = cfIndex.allContexts();
    putU32(data, static_cast<uint32_t>(contexts.size()));
    for (const CallSiteContext *ctx : contexts) {
      putStr(data, pool, ctx->callerName);
      putStr(data, pool, ctx->calleeName);
      putStr(data, pool, ctx->callSite);
      putStr(data, pool, ctx->tuPath);
      putU32(data, static_cast<uint32_t>(ctx->enclosingTryCatches.size()));
      for (const auto &scope : ctx->enclosingTryCatches) {
        putStr(data, pool, scope.tryLocation);
        putStr(data, pool, scope.enclosingFunction);
        putU32(data, scope.nestingDepth);
        putU32(data, static_cast<uint32_t>(scope.handlers.size()));
        for (const auto &h : scope.handlers) {
          putStr(data, pool, h.caughtType);
          putU8(data, h.isCatchAll ? 1 : 0);
          putStr(data, pool, h.location);
          putStr(data, pool, h.bodySummary);
        }
      }
      putU32(data, static_cast<uint32_t>(ctx->enclosingGuards.size()));
      for (const auto &g : ctx->enclosingGuards) {
        putStr(data, pool, g.conditionText);
        putStr(data, pool, g.location);
        putU8(data, g.inTrueBranch ? 1 : 0);
        putU8(data, g.isAssertion ? 1 : 0);
      }
      putU8(data, static_cast<uint8_t>(ctx->callerNoexcept));
      putU8(data, ctx->insideCatchBlock ? 1 : 0);
      putU32(data, static_cast<uint32_t>(ctx->liveRaiiLocals.size()));
      for (const auto &l : ctx->liveRaiiLocals) {
        putStr(data, pool, l.typeName);
        putStr(data, pool, l.varName);
        putStr(data, pool, l.declLocation);
        putU8(data, static_cast<uint8_t>(l.kind));
      }
    }
  }

  // Assemble the file: header, string table, data section. Write to a temp
  // file and rename so a crash mid-write never leaves a torn snapshot.
  std::string tmpPath = path + ".tmp";
  {
    std::error_code ec;
    llvm::raw_fd_ostream os(tmpPath, ec, llvm::sys::fs::OF_None);
    if (ec)
      return false;

    os.write(kMagic, 4);
    std::string header;
    putU32(header, kFormatVersion);
    putU32(header, static_cast<uint32_t>(pool.strings().size()));
    os << header;
    for (const auto &s : pool.strings()) {
      std::string len;
      putU32(len, static_cast<uint32_t>(s.size()));
      os << len << s;
    }
    os << data;
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

  // String table.
  std::vector<std::string> pool;
  uint32_t stringCount = r.count();
  pool.reserve(stringCount);
  for (uint32_t i = 0; r.ok && i < stringCount; ++i) {
    uint32_t len = r.u32();
    pool.push_back(r.bytes(len));
  }
  if (!r.ok)
    return std::nullopt;
  r.pool = &pool;

  SnapshotData out;
  if (!readMeta(r, out.meta))
    return std::nullopt;

  // Nodes.
  uint32_t nodeCount = r.count();
  for (uint32_t i = 0; r.ok && i < nodeCount; ++i) {
    CallGraphNode node;
    node.qualifiedName = r.str();
    node.file = r.str();
    node.line = r.u32();
    uint8_t flags = r.u8();
    node.isEntryPoint = (flags & 1) != 0;
    node.isVirtual = (flags & 2) != 0;
    node.enclosingClass = r.str();
    uint32_t contribCount = r.count();
    if (contribCount == 0) {
      out.graph.addNode(std::move(node));
    } else {
      for (uint32_t c = 0; r.ok && c < contribCount; ++c)
        out.graph.addNode(node, r.str());
    }
  }

  // Edges. Re-adding once per contributor reconstructs the dedup refcount
  // and TU provenance through the public API.
  uint32_t edgeCount = r.count();
  for (uint32_t i = 0; r.ok && i < edgeCount; ++i) {
    CallGraphEdge e;
    e.callerName = r.str();
    e.calleeName = r.str();
    e.kind = static_cast<EdgeKind>(r.u8());
    e.confidence = static_cast<Confidence>(r.u8());
    e.callSite = r.str();
    e.indirectionDepth = r.u32();
    e.execContext = static_cast<ExecutionContext>(r.u8());
    uint32_t contribCount = r.count();
    if (contribCount == 0) {
      out.graph.addEdge(std::move(e));
    } else {
      for (uint32_t c = 0; r.ok && c < contribCount; ++c)
        out.graph.addEdge(e, r.str());
    }
  }

  // Relationship pairs.
  uint32_t n = r.count();
  for (uint32_t i = 0; r.ok && i < n; ++i) {
    std::string a = r.str(), b = r.str();
    out.graph.addDerivedClass(a, b);
  }
  n = r.count();
  for (uint32_t i = 0; r.ok && i < n; ++i) {
    std::string a = r.str(), b = r.str();
    out.graph.addMethodOverride(a, b);
  }
  n = r.count();
  for (uint32_t i = 0; r.ok && i < n; ++i) {
    std::string impl = r.str(), cls = r.str();
    out.graph.addEffectiveImpl(cls, impl);
  }
  n = r.count();
  for (uint32_t i = 0; r.ok && i < n; ++i) {
    std::string fn = r.str(), ret = r.str();
    out.graph.addFunctionReturn(fn, ret);
  }

  // Control flow contexts.
  uint32_t ctxCount = r.count();
  for (uint32_t i = 0; r.ok && i < ctxCount; ++i) {
    CallSiteContext ctx;
    ctx.callerName = r.str();
    ctx.calleeName = r.str();
    ctx.callSite = r.str();
    ctx.tuPath = r.str();
    uint32_t tryCount = r.count();
    for (uint32_t t = 0; r.ok && t < tryCount; ++t) {
      TryCatchScope scope;
      scope.tryLocation = r.str();
      scope.enclosingFunction = r.str();
      scope.nestingDepth = r.u32();
      uint32_t handlerCount = r.count();
      for (uint32_t h = 0; r.ok && h < handlerCount; ++h) {
        CatchHandlerInfo info;
        info.caughtType = r.str();
        info.isCatchAll = r.u8() != 0;
        info.location = r.str();
        info.bodySummary = r.str();
        scope.handlers.push_back(std::move(info));
      }
      ctx.enclosingTryCatches.push_back(std::move(scope));
    }
    uint32_t guardCount = r.count();
    for (uint32_t g = 0; r.ok && g < guardCount; ++g) {
      ConditionalGuard guard;
      guard.conditionText = r.str();
      guard.location = r.str();
      guard.inTrueBranch = r.u8() != 0;
      guard.isAssertion = r.u8() != 0;
      ctx.enclosingGuards.push_back(std::move(guard));
    }
    ctx.callerNoexcept = static_cast<NoexceptSpec>(r.u8());
    ctx.insideCatchBlock = r.u8() != 0;
    uint32_t raiiCount = r.count();
    for (uint32_t l = 0; r.ok && l < raiiCount; ++l) {
      RaiiLocal local;
      local.typeName = r.str();
      local.varName = r.str();
      local.declLocation = r.str();
      local.kind = static_cast<RaiiKind>(r.u8());
      ctx.liveRaiiLocals.push_back(std::move(local));
    }
    if (r.ok)
      out.cfIndex.addCallSiteContext(std::move(ctx));
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
