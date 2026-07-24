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
// v2: AnnealIndexPayload gained odrEntries. v3: specializations.
// v4: defaultArgs.
constexpr uint32_t kVersion = 4;
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
  // ODR collection changes phase-1 record content (odrEntries present or
  // absent), so it is part of the journal identity.
  canon += "|odr=";
  canon += opts.enableOdrDiag ? '1' : '0';
  // These change phase-2 diagnostic content.
  canon += "|adl=";
  canon += opts.enableAdlDiag ? '1' : '0';
  canon += "|ctad=";
  canon += opts.enableCtadDiag ? '1' : '0';
  canon += "|spec=";
  canon += opts.enableSpecializationDiag ? '1' : '0';
  canon += "|defarg=";
  canon += opts.enableDefaultArgDiag ? '1' : '0';
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

// ---------------------------------------------------------------------------
// AnnealIndexPayload
// ---------------------------------------------------------------------------

AnnealIndexPayload AnnealIndexPayload::capture(const GlobalIndex &shard) {
  AnnealIndexPayload p;
  shard.forEachOverload(
      [&](const FunctionOverloadEntry &e) { p.overloads.push_back(e); });
  shard.forEachDeductionGuide(
      [&](const DeductionGuideEntry &e) { p.guides.push_back(e); });
  shard.forEachCoverageProperty(
      [&](const CoveragePropertyEntry &e) { p.coverage.push_back(e); });
  shard.forEachOdrEntry([&](const OdrEntry &e) { p.odrEntries.push_back(e); });
  shard.forEachSpecialization(
      [&](const SpecializationEntry &e) { p.specializations.push_back(e); });
  shard.forEachDefaultArg(
      [&](const DefaultArgEntry &e) { p.defaultArgs.push_back(e); });
  const auto &types = shard.typeRelations();
  types.forEachBase([&](const std::string &d, const std::string &b) {
    p.baseEdges.emplace_back(d, b);
  });
  types.forEachCtorEdge([&](const std::string &to, const std::string &from) {
    p.ctorEdges.emplace_back(to, from);
  });
  types.forEachConvOpEdge([&](const std::string &from, const std::string &to) {
    p.convOpEdges.emplace_back(from, to);
  });
  return p;
}

void AnnealIndexPayload::applyTo(GlobalIndex &into) const {
  for (const auto &e : overloads)
    into.addFunctionOverload(e);
  for (const auto &e : guides)
    into.addDeductionGuide(e);
  for (const auto &e : coverage)
    into.addCoverageProperty(e);
  for (const auto &e : odrEntries)
    into.addOdrEntry(e);
  for (const auto &e : specializations)
    into.addSpecialization(e);
  for (const auto &e : defaultArgs)
    into.addDefaultArg(e);
  auto &types = into.mutableTypeRelations();
  for (const auto &p : baseEdges)
    types.addBase(p.first, p.second);
  for (const auto &p : ctorEdges)
    types.addCtorEdge(p.first, p.second);
  for (const auto &p : convOpEdges)
    types.addConvOpEdge(p.first, p.second);
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
// Payload wire form (shared by journal records and worker shard files)
// ---------------------------------------------------------------------------

namespace {

void encodeIndexPayload(std::string &out, const AnnealIndexPayload &p) {
  putU32(out, static_cast<uint32_t>(p.overloads.size()));
  for (const auto &e : p.overloads) {
    putStr(out, e.qualifiedName);
    putStr(out, e.headerPath);
    putU32(out, static_cast<uint32_t>(e.paramTypes.size()));
    for (const auto &t : e.paramTypes)
      putStr(out, t);
    putStr(out, e.returnType);
    putU32(out, e.sourceLine);
  }
  putU32(out, static_cast<uint32_t>(p.guides.size()));
  for (const auto &e : p.guides) {
    putStr(out, e.templateName);
    putStr(out, e.headerPath);
    putU32(out, static_cast<uint32_t>(e.paramTypes.size()));
    for (const auto &t : e.paramTypes)
      putStr(out, t);
    putStr(out, e.deducedType);
    putU32(out, e.sourceLine);
  }
  putU32(out, static_cast<uint32_t>(p.coverage.size()));
  for (const auto &e : p.coverage) {
    putStr(out, e.qualifiedName);
    putStr(out, e.headerPath);
    putU32(out, e.sourceLine);
    putStr(out, e.enclosingClass);
    putU32(out, static_cast<uint32_t>(static_cast<int32_t>(e.gvaLinkage)));
    uint8_t flags = 0;
    flags |= e.isInlined ? 0x01 : 0;
    flags |= e.isConstexpr ? 0x02 : 0;
    flags |= e.isDefaulted ? 0x04 : 0;
    flags |= e.isTrivial ? 0x08 : 0;
    flags |= e.isVirtual ? 0x10 : 0;
    flags |= e.isStaticMethod ? 0x20 : 0;
    flags |= e.isImplicitlyInstantiable ? 0x40 : 0;
    putU8(out, flags);
    putU32(out, static_cast<uint32_t>(static_cast<int32_t>(e.templatedKind)));
    putU32(out, static_cast<uint32_t>(static_cast<int32_t>(e.storageClass)));
    putU32(out, static_cast<uint32_t>(static_cast<int32_t>(e.formalLinkage)));
    putU32(out, e.bodyStmtCount);
    putStr(out, e.signature);
  }
  putStringPairs(out, p.baseEdges);
  putStringPairs(out, p.ctorEdges);
  putStringPairs(out, p.convOpEdges);
  putU32(out, static_cast<uint32_t>(p.odrEntries.size()));
  for (const auto &e : p.odrEntries) {
    putStr(out, e.qualifiedName);
    putStr(out, e.signature);
    putU8(out, e.isClass ? 1 : 0);
    putStr(out, e.enclosingClass);
    putStr(out, e.filePath);
    putU32(out, e.line);
    putU64(out, e.odrHash);
  }
  putU32(out, static_cast<uint32_t>(p.specializations.size()));
  for (const auto &e : p.specializations) {
    putStr(out, e.templateName);
    putStr(out, e.argsString);
    putStr(out, e.headerPath);
    putU32(out, e.line);
  }
  putU32(out, static_cast<uint32_t>(p.defaultArgs.size()));
  for (const auto &e : p.defaultArgs) {
    putStr(out, e.qualifiedName);
    putStr(out, e.signature);
    putU32(out, e.paramIndex);
    putStr(out, e.paramName);
    putStr(out, e.defaultText);
    putStr(out, e.filePath);
    putU32(out, e.line);
  }
}

bool decodeIndexPayload(Reader &r, AnnealIndexPayload &p) {
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
    p.overloads.push_back(std::move(e));
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
    p.guides.push_back(std::move(e));
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
    p.coverage.push_back(std::move(e));
  }
  p.baseEdges = readStringPairs(r);
  p.ctorEdges = readStringPairs(r);
  p.convOpEdges = readStringPairs(r);
  uint32_t nOdr = r.u32();
  for (uint32_t i = 0; i < nOdr && r.ok; ++i) {
    OdrEntry e;
    e.qualifiedName = r.str();
    e.signature = r.str();
    e.isClass = r.u8() != 0;
    e.enclosingClass = r.str();
    e.filePath = r.str();
    e.line = r.u32();
    e.odrHash = r.u64();
    p.odrEntries.push_back(std::move(e));
  }
  uint32_t nSpec = r.u32();
  for (uint32_t i = 0; i < nSpec && r.ok; ++i) {
    SpecializationEntry e;
    e.templateName = r.str();
    e.argsString = r.str();
    e.headerPath = r.str();
    e.line = r.u32();
    p.specializations.push_back(std::move(e));
  }
  uint32_t nDef = r.u32();
  for (uint32_t i = 0; i < nDef && r.ok; ++i) {
    DefaultArgEntry e;
    e.qualifiedName = r.str();
    e.signature = r.str();
    e.paramIndex = r.u32();
    e.paramName = r.str();
    e.defaultText = r.str();
    e.filePath = r.str();
    e.line = r.u32();
    p.defaultArgs.push_back(std::move(e));
  }
  return r.ok;
}

void encodeDiagnostics(std::string &out,
                       const std::vector<Diagnostic> &diags) {
  putU32(out, static_cast<uint32_t>(diags.size()));
  for (const auto &d : diags) {
    putU32(out, static_cast<uint32_t>(d.kind));
    putStr(out, d.callLocation);
    putStr(out, d.resolvedDecl);
    putStr(out, d.betterDecl);
    putStr(out, d.missingHeader);
    putStr(out, d.message);
    putStr(out, d.checkName);
  }
}

bool decodeDiagnostics(Reader &r, std::vector<Diagnostic> &out) {
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
    out.push_back(std::move(d));
  }
  return r.ok;
}

} // namespace

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
      if (!decodeIndexPayload(r, rec.payload))
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
      if (!decodeDiagnostics(r, rec.diagnostics))
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
  rec->payload.applyTo(into);
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
  recordPhase1(tu, stamp, AnnealIndexPayload::capture(shard));
}

void AnnealCheckpoint::recordPhase1(const std::string &tu,
                                    const FileStamp &stamp,
                                    const AnnealIndexPayload &contribution) {
  std::string payload;
  putStr(payload, tu);
  putStamp(payload, stamp);
  encodeIndexPayload(payload, contribution);
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
  encodeDiagnostics(payload, diags);
  appendRecord(kKindPhase2, payload);

  std::lock_guard<std::mutex> lock(mutex_);
  attempts_.erase(attemptKey(kPhaseAnalyze, tu));
}

// ---------------------------------------------------------------------------
// Worker shard files
// ---------------------------------------------------------------------------

namespace {

// Shard/handoff file framing: magic, u32 version, u32 entry count, then per
// entry: str tu, u32 payloadLen, payload, u32 fnv32(payload). All-or-nothing
// on read (workers write complete files then exit 0).
// v2: index payloads gained odrEntries. v3: specializations.
// v4: defaultArgs.
constexpr uint32_t kShardVersion = 4;

bool writeShardFile(const std::string &path, const char magic[4],
                    const std::vector<std::pair<std::string, std::string>>
                        &tuPayloads) {
  std::error_code ec;
  llvm::raw_fd_ostream out(path, ec, llvm::sys::fs::OF_None);
  if (ec)
    return false;
  std::string buf;
  buf.append(magic, 4);
  putU32(buf, kShardVersion);
  putU32(buf, static_cast<uint32_t>(tuPayloads.size()));
  for (const auto &[tu, payload] : tuPayloads) {
    putStr(buf, tu);
    putU32(buf, static_cast<uint32_t>(payload.size()));
    buf.append(payload);
    putU32(buf, fnv32(payload.data(), payload.size()));
  }
  out << buf;
  out.flush();
  return !out.has_error();
}

bool readShardFile(
    const std::string &path, const char magic[4],
    const std::function<bool(const std::string &tu, Reader &r)> &fn) {
  auto bufOrErr = llvm::MemoryBuffer::getFile(path);
  if (!bufOrErr)
    return false;
  const auto &buf = *bufOrErr.get();
  Reader stream{buf.getBufferStart(), buf.getBufferSize()};
  if (!stream.need(8) || std::memcmp(buf.getBufferStart(), magic, 4) != 0)
    return false;
  stream.pos = 4;
  if (stream.u32() != kShardVersion)
    return false;
  uint32_t count = stream.u32();
  for (uint32_t i = 0; i < count; ++i) {
    std::string tu = stream.str();
    uint32_t len = stream.u32();
    if (!stream.need(len + 4))
      return false;
    const char *payload = stream.data + stream.pos;
    stream.pos += len;
    if (stream.u32() != fnv32(payload, len))
      return false;
    Reader r{payload, len};
    if (!fn(tu, r))
      return false;
  }
  return stream.ok;
}

constexpr char kIndexShardMagic[4] = {'V', 'Y', 'A', 'I'};
constexpr char kDiagShardMagic[4] = {'V', 'Y', 'A', 'D'};
constexpr char kGlobalIndexMagic[4] = {'V', 'Y', 'G', 'I'};

} // namespace

bool writeAnnealIndexShard(
    const std::string &path,
    const std::vector<std::pair<std::string, AnnealIndexPayload>> &tus) {
  std::vector<std::pair<std::string, std::string>> encoded;
  encoded.reserve(tus.size());
  for (const auto &[tu, payload] : tus) {
    std::string bytes;
    encodeIndexPayload(bytes, payload);
    encoded.emplace_back(tu, std::move(bytes));
  }
  return writeShardFile(path, kIndexShardMagic, encoded);
}

bool readAnnealIndexShard(
    const std::string &path,
    const std::function<void(const std::string &tu,
                             const AnnealIndexPayload &payload)> &fn) {
  return readShardFile(path, kIndexShardMagic,
                       [&](const std::string &tu, Reader &r) {
                         AnnealIndexPayload payload;
                         if (!decodeIndexPayload(r, payload))
                           return false;
                         fn(tu, payload);
                         return true;
                       });
}

bool writeAnnealDiagShard(
    const std::string &path,
    const std::vector<std::pair<std::string, std::vector<Diagnostic>>> &tus) {
  std::vector<std::pair<std::string, std::string>> encoded;
  encoded.reserve(tus.size());
  for (const auto &[tu, diags] : tus) {
    std::string bytes;
    encodeDiagnostics(bytes, diags);
    encoded.emplace_back(tu, std::move(bytes));
  }
  return writeShardFile(path, kDiagShardMagic, encoded);
}

bool readAnnealDiagShard(
    const std::string &path,
    const std::function<void(const std::string &tu,
                             std::vector<Diagnostic> diags)> &fn) {
  return readShardFile(path, kDiagShardMagic,
                       [&](const std::string &tu, Reader &r) {
                         std::vector<Diagnostic> diags;
                         if (!decodeDiagnostics(r, diags))
                           return false;
                         fn(tu, std::move(diags));
                         return true;
                       });
}

bool writeGlobalIndexFile(const std::string &path, const GlobalIndex &index) {
  return writeShardFile(
      path, kGlobalIndexMagic,
      {{std::string("<merged>"), [&] {
          std::string bytes;
          encodeIndexPayload(bytes, AnnealIndexPayload::capture(index));
          return bytes;
        }()}});
}

bool readGlobalIndexFile(const std::string &path, GlobalIndex &into) {
  return readShardFile(path, kGlobalIndexMagic,
                       [&](const std::string &, Reader &r) {
                         AnnealIndexPayload payload;
                         if (!decodeIndexPayload(r, payload))
                           return false;
                         payload.applyTo(into);
                         return true;
                       });
}

} // namespace vycor
