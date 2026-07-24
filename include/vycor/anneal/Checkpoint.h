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

#pragma once

#include "vycor/anneal/Analyzer.h"
#include "vycor/anneal/GlobalIndex.h"
#include "vycor/callgraph/Snapshot.h" // FileStamp

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace llvm {
class raw_fd_ostream;
} // namespace llvm

namespace vycor {

// ============================================================================
// Anneal checkpoint journal (--checkpoint <file>)
//
// An append-only, per-TU journal so a killed anneal run resumes where it
// left off instead of restarting from zero. Two record families mirror the
// two analysis phases:
//
//   - phase-1 records carry the TU's complete GlobalIndex contribution
//     (overloads, deduction guides, coverage properties, type-relation
//     edges), so a resumed run can rebuild the cross-TU index for finished
//     TUs WITHOUT re-parsing them;
//   - phase-2 records carry the TU's diagnostics, stamped with a hash of
//     the whole contributing file set — any phase-1 change anywhere
//     invalidates every phase-2 record, because fragility analysis of one
//     TU depends on declarations from all the others.
//
// Records are validated per-TU against FileStamps (mtime+size, the same
// currency megascope snapshots use), so between-runs source edits
// invalidate exactly the affected records. Attempt records written BEFORE
// each parse make repeated fatal deaths (OOM kill, poisoned TU) detectable:
// a TU with kMaxAttempts starts and no completion on record is skipped
// with a warning instead of killing every resume at the same place.
//
// Crash-safety model: each record is length-prefixed and checksummed, and
// the journal is flushed after every append. A record cut short by a kill
// fails the length/checksum check on load; the loader keeps everything
// before it and discards the tail. (Flush-to-OS survives SIGKILL; only
// power loss can drop tail records, which then simply re-run.)
//
// Like snapshots, the journal is a cache, never a source of truth: a
// version/fingerprint mismatch or any decode doubt discards it and the
// affected TUs re-run.
// ============================================================================

// Hash of every AnalysisOptions field (plus the enabled organization-check
// names) that changes journal record CONTENT. Stored in the journal header;
// a mismatch discards the whole journal.
uint64_t annealOptionsFingerprint(const AnalysisOptions &opts);

// One TU's complete GlobalIndex contribution in value form — the shared
// payload shape of checkpoint phase-1 records, worker index shards, and
// the parent->worker full-index handoff file.
struct AnnealIndexPayload {
  std::vector<FunctionOverloadEntry> overloads;
  std::vector<DeductionGuideEntry> guides;
  std::vector<CoveragePropertyEntry> coverage;
  std::vector<OdrEntry> odrEntries; // populated when enableOdrDiag
  std::vector<SpecializationEntry> specializations;
  std::vector<DefaultArgEntry> defaultArgs;
  std::vector<StaticInitEntry> staticInits;
  // (derived,base), (toType,fromType), (fromType,toType) — the argument
  // order of the corresponding TypeRelationIndex::add* methods.
  std::vector<std::pair<std::string, std::string>> baseEdges;
  std::vector<std::pair<std::string, std::string>> ctorEdges;
  std::vector<std::pair<std::string, std::string>> convOpEdges;

  static AnnealIndexPayload capture(const GlobalIndex &shard);
  void applyTo(GlobalIndex &into) const;
};

// Hash of a contributing file set (sorted path|mtime|size triples) — the
// validity key for phase-2 records (see above).
uint64_t annealStampSetHash(const std::vector<FileStamp> &stamps);

class AnnealCheckpoint {
public:
  static constexpr uint8_t kPhaseIndex = 1;
  static constexpr uint8_t kPhaseAnalyze = 2;
  // Attempts (per phase, per TU, per stamp) after which a TU is considered
  // poisoned and skipped on resume.
  static constexpr unsigned kMaxAttempts = 2;

  // Opens the journal at `path`, creating it if absent. An existing journal
  // whose header (magic/version/fingerprint) doesn't match is discarded and
  // restarted fresh; a corrupt/truncated tail is dropped and everything
  // before it kept. Returns nullptr only when the file cannot be opened for
  // appending (caller should warn and continue without a checkpoint).
  static std::unique_ptr<AnnealCheckpoint>
  open(const std::string &path, uint64_t optionsFingerprint);

  ~AnnealCheckpoint();
  AnnealCheckpoint(const AnnealCheckpoint &) = delete;
  AnnealCheckpoint &operator=(const AnnealCheckpoint &) = delete;

  // ---- queries over the loaded journal -----------------------------------

  // Attempts recorded for (phase, tu) with exactly this stamp since the
  // last completion record for that phase+tu. A changed stamp (edited
  // file) or an intervening completion resets the count to zero.
  unsigned attempts(uint8_t phase, const std::string &tu,
                    const FileStamp &stamp) const;

  // Replays the TU's phase-1 contribution into `into` when a record with a
  // matching stamp exists; returns false (and touches nothing) otherwise.
  bool replayPhase1(const std::string &tu, const FileStamp &stamp,
                    GlobalIndex &into) const;

  // Appends the TU's phase-2 diagnostics to `out` when a record with a
  // matching stamp AND matching index-set hash exists.
  bool replayPhase2(const std::string &tu, const FileStamp &stamp,
                    uint64_t indexSetHash,
                    std::vector<Diagnostic> &out) const;

  // ---- appends (thread-safe; each record flushed before returning) ------

  void recordAttempt(uint8_t phase, const std::string &tu,
                     const FileStamp &stamp);
  void recordPhase1(const std::string &tu, const FileStamp &stamp,
                    const GlobalIndex &shard);
  void recordPhase1(const std::string &tu, const FileStamp &stamp,
                    const AnnealIndexPayload &payload);
  void recordPhase2(const std::string &tu, const FileStamp &stamp,
                    uint64_t indexSetHash,
                    const std::vector<Diagnostic> &diags);

private:
  AnnealCheckpoint() = default;

  struct Phase1Record {
    FileStamp stamp;
    AnnealIndexPayload payload;
  };
  struct Phase2Record {
    FileStamp stamp;
    uint64_t indexSetHash = 0;
    std::vector<Diagnostic> diagnostics;
  };
  // Most-recent attempt tracking; see attempts().
  struct AttemptState {
    FileStamp stamp;
    unsigned count = 0;
  };

  // Parses the journal byte stream (past the header) into the maps above.
  void loadRecords(const char *data, size_t size);
  void appendRecord(uint8_t kind, const std::string &payload);

  std::string path_;
  mutable std::mutex mutex_;
  std::unique_ptr<llvm::raw_fd_ostream> out_;

  // Later records win (a TU re-run after a source edit overwrites its
  // earlier entry at load time).
  std::unordered_map<std::string, Phase1Record> phase1_;
  std::unordered_map<std::string, Phase2Record> phase2_;
  // Keyed "phase\0tu".
  std::unordered_map<std::string, AttemptState> attempts_;
};

// ============================================================================
// Worker shard files (anneal --isolate-workers)
//
// Workers write a complete shard then exit 0; the parent only reads shards
// of cleanly-exited workers, so unlike the journal these are all-or-nothing
// files (any decode problem returns false and the dispatcher retries the
// batch). Per-TU grouping is what lets the parent absorb, order, and — when
// --checkpoint is also set — journal each TU's result exactly like the
// in-process path.
// ============================================================================

// Phase-1 index shard: each entry is (tuPath, that TU's contribution).
bool writeAnnealIndexShard(
    const std::string &path,
    const std::vector<std::pair<std::string, AnnealIndexPayload>> &tus);
bool readAnnealIndexShard(
    const std::string &path,
    const std::function<void(const std::string &tu,
                             const AnnealIndexPayload &payload)> &fn);

// Phase-2 diagnostics shard: each entry is (tuPath, its diagnostics).
bool writeAnnealDiagShard(
    const std::string &path,
    const std::vector<std::pair<std::string, std::vector<Diagnostic>>> &tus);
bool readAnnealDiagShard(
    const std::string &path,
    const std::function<void(const std::string &tu,
                             std::vector<Diagnostic> diags)> &fn);

// Full merged index, for the parent -> analyze-worker handoff
// (anneal --analyze-worker --global-index <file>).
bool writeGlobalIndexFile(const std::string &path, const GlobalIndex &index);
bool readGlobalIndexFile(const std::string &path, GlobalIndex &into);

} // namespace vycor
