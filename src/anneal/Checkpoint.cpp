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

#include "vycor/anneal/Checkpoint.h"

#include "vycor/Version.h"
#include "vycor/ext/Extensions.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstring>

namespace vycor {

namespace {

// Journal header: magic, format version, options fingerprint. A bumped
// version or fingerprint mismatch discards the journal (it is a cache).
constexpr char kMagic[4] = {'V', 'Y', 'C', 'J'};
constexpr uint32_t kVersion = 1;
constexpr size_t kHeaderSize = 4 + 4 + 8;

constexpr uint8_t kKindAttempt = 1;
constexpr uint8_t kKindPhase1 = 2;
constexpr uint8_t kKindPhase2 = 3;

uint32_t fnv32(const char *data, size_t size) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < size; ++i) {
    h ^= static_cast<unsigned char>(data[i]);
    h *= 16777619u;
  }
  return h;
}

uint64_t fnv64(const char *data, size_t size, uint64_t seed) {
  uint64_t h = seed;
  for (size_t i = 0; i < size; ++i) {
    h ^= static_cast<unsigned char>(data[i]);
    h *= 1099511628211ull;
  }
  return h;
}
constexpr uint64_t kFnv64Seed = 14695981039346656037ull;

// ---- little-endian writers into a std::string buffer ----------------------

void putU8(std::string &b, uint8_t v) { b.push_back(static_cast<char>(v)); }

void putU32(std::string &b, uint32_t v) {
  for (int i = 0; i < 4; ++i)
    b.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}

void putU64(std::string &b, uint64_t v) {
  for (int i = 0; i < 8; ++i)
    b.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}

void putStr(std::string &b, const std::string &s) {
  putU32(b, static_cast<uint32_t>(s.size()));
  b.append(s);
}

// ---- bounds-checked reader -------------------------------------------------

struct Reader {
  const char *data;
  size_t size;
  size_t pos = 0;
  bool ok = true;

  bool need(size_t n) {
    if (!ok || size - pos < n) {
      ok = false;
      return false;
    }
    return true;
  }
  uint8_t u8() {
    if (!need(1))
      return 0;
    return static_cast<uint8_t>(data[pos++]);
  }
  uint32_t u32() {
    if (!need(4))
      return 0;
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i)
      v |= static_cast<uint32_t>(static_cast<unsigned char>(data[pos++]))
           << (8 * i);
    return v;
  }
  uint64_t u64() {
    if (!need(8))
      return 0;
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
      v |= static_cast<uint64_t>(static_cast<unsigned char>(data[pos++]))
           << (8 * i);
    return v;
  }
  std::string str() {
    uint32_t n = u32();
    if (!need(n))
      return {};
    std::string s(data + pos, n);
    pos += n;
    return s;
  }
};

std::string attemptKey(uint8_t phase, const std::string &tu) {
  std::string key(1, static_cast<char>(phase));
  key.push_back('\0');
  key.append(tu);
  return key;
}

void putStamp(std::string &b, const FileStamp &stamp) {
  putU64(b, stamp.mtimeNs);
  putU64(b, stamp.size);
}

void putStringPairs(
    std::string &b, const std::vector<std::pair<std::string, std::string>> &v) {
  putU32(b, static_cast<uint32_t>(v.size()));
  for (const auto &p : v) {
    putStr(b, p.first);
    putStr(b, p.second);
  }
}

std::vector<std::pair<std::string, std::string>> readStringPairs(Reader &r) {
  std::vector<std::pair<std::string, std::string>> v;
  uint32_t n = r.u32();
  for (uint32_t i = 0; i < n && r.ok; ++i) {
    auto a = r.str();
    auto b = r.str();
    v.emplace_back(std::move(a), std::move(b));
  }
  return v;
}

} // namespace

// ---------------------------------------------------------------------------
// Fingerprints
// ---------------------------------------------------------------------------

uint64_t annealOptionsFingerprint(const AnalysisOptions &opts) {
  // The tool version is part of the identity: record payloads carry raw
  // Diagnostic::Kind values (and entry field layouts) that are only
  // meaningful to the binary revision that wrote them. A checkpoint is a
  // kill-recovery artifact, not a long-term cache — cross-version reuse
  // isn't worth the mislabeled-replay risk.
  std::string canon = VYCOR_VERSION_STRING;
  canon += "|v1|ws=";
  canon += opts.warnSameScore ? '1' : '0';
  canon += "|mc=";
  canon += opts.modelConvertibility ? '1' : '0';
  // Organization checks change phase-2 record content; the enabled set
  // (registered minus disabled) is part of the identity.
  std::vector<std::string> names;
  for (const auto &check :
       ExtensionRegistry::instance().createAnnealChecks(opts.disabledChecks))
    names.push_back(check->name());
  std::sort(names.begin(), names.end());
  canon += "|checks=";
  for (const auto &n : names) {
    canon += n;
    canon += ',';
  }
  return fnv64(canon.data(), canon.size(), kFnv64Seed);
}

uint64_t annealStampSetHash(const std::vector<FileStamp> &stamps) {
  std::vector<std::string> keys;
  keys.reserve(stamps.size());
  for (const auto &s : stamps)
    keys.push_back(s.path + "|" + std::to_string(s.mtimeNs) + "|" +
                   std::to_string(s.size));
  std::sort(keys.begin(), keys.end());
  uint64_t h = kFnv64Seed;
  for (const auto &k : keys) {
    h = fnv64(k.data(), k.size(), h);
    h = fnv64("\n", 1, h);
  }
  return h;
}

// ---------------------------------------------------------------------------
// Open / load
// ---------------------------------------------------------------------------

std::unique_ptr<AnnealCheckpoint>
AnnealCheckpoint::open(const std::string &path, uint64_t optionsFingerprint) {
  std::unique_ptr<AnnealCheckpoint> ckpt(new AnnealCheckpoint());
  ckpt->path_ = path;

  bool reuse = false;
  if (auto bufOrErr = llvm::MemoryBuffer::getFile(path)) {
    const auto &buf = *bufOrErr.get();
    if (buf.getBufferSize() >= kHeaderSize &&
        std::memcmp(buf.getBufferStart(), kMagic, 4) == 0) {
      Reader header{buf.getBufferStart() + 4, kHeaderSize - 4};
      uint32_t version = header.u32();
      uint64_t fingerprint = header.u64();
      if (version == kVersion && fingerprint == optionsFingerprint) {
        ckpt->loadRecords(buf.getBufferStart() + kHeaderSize,
                          buf.getBufferSize() - kHeaderSize);
        reuse = true;
      } else {
        llvm::errs() << "anneal: checkpoint " << path
                     << " was written with a different format/options — "
                        "starting fresh\n";
      }
    }
  }

  std::error_code ec;
  if (reuse) {
    ckpt->out_ = std::make_unique<llvm::raw_fd_ostream>(
        path, ec, llvm::sys::fs::OF_Append);
  } else {
    ckpt->out_ =
        std::make_unique<llvm::raw_fd_ostream>(path, ec, llvm::sys::fs::OF_None);
    if (!ec) {
      std::string header;
      header.append(kMagic, 4);
      putU32(header, kVersion);
      putU64(header, optionsFingerprint);
      *ckpt->out_ << header;
      ckpt->out_->flush();
    }
  }
  if (ec)
    return nullptr;
  return ckpt;
}

AnnealCheckpoint::~AnnealCheckpoint() = default;

void AnnealCheckpoint::loadRecords(const char *data, size_t size) {
  Reader stream{data, size};
  while (stream.ok && stream.pos < stream.size) {
    uint8_t kind = stream.u8();
    uint32_t len = stream.u32();
    if (!stream.need(len + 4))
      break; // truncated tail (killed mid-append) — keep what we have
    const char *payload = stream.data + stream.pos;
    stream.pos += len;
    uint32_t checksum = stream.u32();
    if (checksum != fnv32(payload, len))
      break; // corrupt tail

    Reader r{payload, len};
    switch (kind) {
    case kKindAttempt: {
      uint8_t phase = r.u8();
      std::string tu = r.str();
      FileStamp stamp;
      stamp.path = tu;
      stamp.mtimeNs = r.u64();
      stamp.size = r.u64();
      if (!r.ok)
        break;
      auto &state = attempts_[attemptKey(phase, tu)];
      if (state.stamp == stamp) {
        ++state.count;
      } else {
        state.stamp = stamp;
        state.count = 1;
      }
      break;
    }
    case kKindPhase1: {
      Phase1Record rec;
      std::string tu = r.str();
      rec.stamp.path = tu;
      rec.stamp.mtimeNs = r.u64();
      rec.stamp.size = r.u64();
      uint32_t nOv = r.u32();
      for (uint32_t i = 0; i < nOv && r.ok; ++i) {
        FunctionOverloadEntry e;
        e.qualifiedName = r.str();
        e.headerPath = r.str();
        uint32_t nP = r.u32();
        for (uint32_t j = 0; j < nP && r.ok; ++j)
          e.paramTypes.push_back(r.str());
        e.returnType = r.str();
        e.sourceLine = r.u32();
        rec.overloads.push_back(std::move(e));
      }
      uint32_t nGd = r.u32();
      for (uint32_t i = 0; i < nGd && r.ok; ++i) {
        DeductionGuideEntry e;
        e.templateName = r.str();
        e.headerPath = r.str();
        uint32_t nP = r.u32();
        for (uint32_t j = 0; j < nP && r.ok; ++j)
          e.paramTypes.push_back(r.str());
        e.deducedType = r.str();
        e.sourceLine = r.u32();
        rec.guides.push_back(std::move(e));
      }
      uint32_t nCov = r.u32();
      for (uint32_t i = 0; i < nCov && r.ok; ++i) {
        CoveragePropertyEntry e;
        e.qualifiedName = r.str();
        e.headerPath = r.str();
        e.sourceLine = r.u32();
        e.enclosingClass = r.str();
        e.gvaLinkage = static_cast<int>(static_cast<int32_t>(r.u32()));
        uint8_t flags = r.u8();
        e.isInlined = flags & 0x01;
        e.isConstexpr = flags & 0x02;
        e.isDefaulted = flags & 0x04;
        e.isTrivial = flags & 0x08;
        e.isVirtual = flags & 0x10;
        e.isStaticMethod = flags & 0x20;
        e.isImplicitlyInstantiable = flags & 0x40;
        e.templatedKind = static_cast<int>(static_cast<int32_t>(r.u32()));
        e.storageClass = static_cast<int>(static_cast<int32_t>(r.u32()));
        e.formalLinkage = static_cast<int>(static_cast<int32_t>(r.u32()));
        e.bodyStmtCount = r.u32();
        e.signature = r.str();
        rec.coverage.push_back(std::move(e));
      }
      rec.baseEdges = readStringPairs(r);
      rec.ctorEdges = readStringPairs(r);
      rec.convOpEdges = readStringPairs(r);
      if (!r.ok)
        break;
      // Completion clears the attempt counter for this TU/phase.
      attempts_.erase(attemptKey(kPhaseIndex, tu));
      phase1_[tu] = std::move(rec);
      break;
    }
    case kKindPhase2: {
      Phase2Record rec;
      std::string tu = r.str();
      rec.stamp.path = tu;
      rec.stamp.mtimeNs = r.u64();
      rec.stamp.size = r.u64();
      rec.indexSetHash = r.u64();
      uint32_t nDiag = r.u32();
      for (uint32_t i = 0; i < nDiag && r.ok; ++i) {
        Diagnostic d;
        d.kind = static_cast<Diagnostic::Kind>(r.u32());
        d.callLocation = r.str();
        d.resolvedDecl = r.str();
        d.betterDecl = r.str();
        d.missingHeader = r.str();
        d.message = r.str();
        d.checkName = r.str();
        rec.diagnostics.push_back(std::move(d));
      }
      if (!r.ok)
        break;
      attempts_.erase(attemptKey(kPhaseAnalyze, tu));
      phase2_[tu] = std::move(rec);
      break;
    }
    default:
      // Unknown record kind from a future minor revision: skip it (length
      // framing makes that safe).
      break;
    }
  }
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

unsigned AnnealCheckpoint::attempts(uint8_t phase, const std::string &tu,
                                    const FileStamp &stamp) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = attempts_.find(attemptKey(phase, tu));
  if (it == attempts_.end() || !(it->second.stamp == stamp))
    return 0;
  return it->second.count;
}

bool AnnealCheckpoint::replayPhase1(const std::string &tu,
                                    const FileStamp &stamp,
                                    GlobalIndex &into) const {
  const Phase1Record *rec = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = phase1_.find(tu);
    if (it == phase1_.end() || !(it->second.stamp == stamp))
      return false;
    rec = &it->second;
  }
  // Replay outside the lock: the loaded maps are append-only per run and
  // this record can't be evicted.
  for (const auto &e : rec->overloads)
    into.addFunctionOverload(e);
  for (const auto &e : rec->guides)
    into.addDeductionGuide(e);
  for (const auto &e : rec->coverage)
    into.addCoverageProperty(e);
  auto &types = into.mutableTypeRelations();
  for (const auto &p : rec->baseEdges)
    types.addBase(p.first, p.second);
  for (const auto &p : rec->ctorEdges)
    types.addCtorEdge(p.first, p.second);
  for (const auto &p : rec->convOpEdges)
    types.addConvOpEdge(p.first, p.second);
  return true;
}

bool AnnealCheckpoint::replayPhase2(const std::string &tu,
                                    const FileStamp &stamp,
                                    uint64_t indexSetHash,
                                    std::vector<Diagnostic> &out) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = phase2_.find(tu);
  if (it == phase2_.end() || !(it->second.stamp == stamp) ||
      it->second.indexSetHash != indexSetHash)
    return false;
  out.insert(out.end(), it->second.diagnostics.begin(),
             it->second.diagnostics.end());
  return true;
}

// ---------------------------------------------------------------------------
// Appends
// ---------------------------------------------------------------------------

void AnnealCheckpoint::appendRecord(uint8_t kind, const std::string &payload) {
  std::string framed;
  framed.reserve(payload.size() + 9);
  putU8(framed, kind);
  putU32(framed, static_cast<uint32_t>(payload.size()));
  framed.append(payload);
  putU32(framed, fnv32(payload.data(), payload.size()));

  std::lock_guard<std::mutex> lock(mutex_);
  if (!out_)
    return;
  *out_ << framed;
  out_->flush(); // one syscall per record: survives SIGKILL from here on
}

void AnnealCheckpoint::recordAttempt(uint8_t phase, const std::string &tu,
                                     const FileStamp &stamp) {
  std::string payload;
  putU8(payload, phase);
  putStr(payload, tu);
  putStamp(payload, stamp);
  appendRecord(kKindAttempt, payload);

  // Mirror the load-time counting so same-process queries agree with what
  // a reload would see.
  std::lock_guard<std::mutex> lock(mutex_);
  auto &state = attempts_[attemptKey(phase, tu)];
  if (state.stamp == stamp) {
    ++state.count;
  } else {
    state.stamp = stamp;
    state.count = 1;
  }
}

void AnnealCheckpoint::recordPhase1(const std::string &tu,
                                    const FileStamp &stamp,
                                    const GlobalIndex &shard) {
  std::string payload;
  putStr(payload, tu);
  putStamp(payload, stamp);

  std::string entries;
  uint32_t count = 0;
  shard.forEachOverload([&](const FunctionOverloadEntry &e) {
    putStr(entries, e.qualifiedName);
    putStr(entries, e.headerPath);
    putU32(entries, static_cast<uint32_t>(e.paramTypes.size()));
    for (const auto &p : e.paramTypes)
      putStr(entries, p);
    putStr(entries, e.returnType);
    putU32(entries, e.sourceLine);
    ++count;
  });
  putU32(payload, count);
  payload.append(entries);

  entries.clear();
  count = 0;
  shard.forEachDeductionGuide([&](const DeductionGuideEntry &e) {
    putStr(entries, e.templateName);
    putStr(entries, e.headerPath);
    putU32(entries, static_cast<uint32_t>(e.paramTypes.size()));
    for (const auto &p : e.paramTypes)
      putStr(entries, p);
    putStr(entries, e.deducedType);
    putU32(entries, e.sourceLine);
    ++count;
  });
  putU32(payload, count);
  payload.append(entries);

  entries.clear();
  count = 0;
  shard.forEachCoverageProperty([&](const CoveragePropertyEntry &e) {
    putStr(entries, e.qualifiedName);
    putStr(entries, e.headerPath);
    putU32(entries, e.sourceLine);
    putStr(entries, e.enclosingClass);
    putU32(entries, static_cast<uint32_t>(static_cast<int32_t>(e.gvaLinkage)));
    uint8_t flags = 0;
    flags |= e.isInlined ? 0x01 : 0;
    flags |= e.isConstexpr ? 0x02 : 0;
    flags |= e.isDefaulted ? 0x04 : 0;
    flags |= e.isTrivial ? 0x08 : 0;
    flags |= e.isVirtual ? 0x10 : 0;
    flags |= e.isStaticMethod ? 0x20 : 0;
    flags |= e.isImplicitlyInstantiable ? 0x40 : 0;
    putU8(entries, flags);
    putU32(entries,
           static_cast<uint32_t>(static_cast<int32_t>(e.templatedKind)));
    putU32(entries,
           static_cast<uint32_t>(static_cast<int32_t>(e.storageClass)));
    putU32(entries,
           static_cast<uint32_t>(static_cast<int32_t>(e.formalLinkage)));
    putU32(entries, e.bodyStmtCount);
    putStr(entries, e.signature);
    ++count;
  });
  putU32(payload, count);
  payload.append(entries);

  std::vector<std::pair<std::string, std::string>> pairs;
  const auto &types = shard.typeRelations();
  types.forEachBase([&](const std::string &d, const std::string &b) {
    pairs.emplace_back(d, b);
  });
  putStringPairs(payload, pairs);
  pairs.clear();
  types.forEachCtorEdge([&](const std::string &to, const std::string &from) {
    pairs.emplace_back(to, from);
  });
  putStringPairs(payload, pairs);
  pairs.clear();
  types.forEachConvOpEdge([&](const std::string &from, const std::string &to) {
    pairs.emplace_back(from, to);
  });
  putStringPairs(payload, pairs);

  appendRecord(kKindPhase1, payload);

  // Mirror the load-time semantics for this process's own appends, so a
  // just-completed TU doesn't read as poisoned within the same run.
  std::lock_guard<std::mutex> lock(mutex_);
  attempts_.erase(attemptKey(kPhaseIndex, tu));
}

void AnnealCheckpoint::recordPhase2(const std::string &tu,
                                    const FileStamp &stamp,
                                    uint64_t indexSetHash,
                                    const std::vector<Diagnostic> &diags) {
  std::string payload;
  putStr(payload, tu);
  putStamp(payload, stamp);
  putU64(payload, indexSetHash);
  putU32(payload, static_cast<uint32_t>(diags.size()));
  for (const auto &d : diags) {
    putU32(payload, static_cast<uint32_t>(d.kind));
    putStr(payload, d.callLocation);
    putStr(payload, d.resolvedDecl);
    putStr(payload, d.betterDecl);
    putStr(payload, d.missingHeader);
    putStr(payload, d.message);
    putStr(payload, d.checkName);
  }
  appendRecord(kKindPhase2, payload);

  std::lock_guard<std::mutex> lock(mutex_);
  attempts_.erase(attemptKey(kPhaseAnalyze, tu));
}

} // namespace vycor
