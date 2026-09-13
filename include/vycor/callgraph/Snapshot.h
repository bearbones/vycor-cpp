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

#include "vycor/callgraph/CallGraph.h"
#include "vycor/callgraph/ChannelIndex.h"
#include "vycor/callgraph/ControlFlowIndex.h"
#include "vycor/callgraph/FileStamp.h"
#include "vycor/callgraph/TuOutcome.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace vycor {

// ============================================================================
// Snapshot persistence — versioned binary serialization of the baked
// CallGraph + ControlFlowIndex, so megascope can warm-start instead of
// re-parsing every TU on every launch.
//
// The snapshot records the build configuration (collapse paths, lock type
// config) and a per-file stamp (mtime + size) for every indexed TU. On load,
// the caller compares stamps against the current tree and re-indexes only
// the TUs that changed (graph.removeTU + indexTU), giving incremental warm
// starts on top of the existing per-TU reindex machinery.
//
// Format: little-endian, magic "VYCS", format version u32. A bumped version
// or any decode error invalidates the snapshot — the caller falls back to a
// full build. Snapshots are a cache, never a source of truth.
// ============================================================================

/// Facts about the run that produced an index, recorded once per bake
/// and read back without decoding any index section. The environment
/// fingerprint is folded into every TU fingerprint (InputFingerprint.h);
/// it is kept here so `info` can say why a warm start rebuilt everything.
struct IndexProvenance {
  std::string analyzer;    // analyzerIdentity()
  std::string toolchain;   // toolchainIdentity()
  std::string environment; // environmentFingerprint(...)
  uint64_t bakeStartNs = 0; // wall clock at the start of the writing bake

  bool operator==(const IndexProvenance &o) const {
    return analyzer == o.analyzer && toolchain == o.toolchain &&
           environment == o.environment && bakeStartNs == o.bakeStartNs;
  }
};

/// What the index covers of what was asked for: the producer-side facts a
/// result envelope can cite. `requested` is the selected TU set; the rest
/// partition it by TuStatus (failed = crashed + poisoned + skipped).
struct IndexCoverage {
  uint64_t requested = 0;
  uint64_t indexed = 0;
  uint64_t partial = 0;
  uint64_t failed = 0;

  bool complete() const { return requested == indexed; }
};

struct SnapshotMeta {
  // Build configuration the snapshot was produced with. Any mismatch with
  // the current invocation invalidates the snapshot wholesale, because these
  // options change which edges/contexts exist.
  std::vector<std::string> collapsePaths;
  std::vector<std::string> lockAllowlist;
  bool lockBuiltins = true;
  // Channel type registrations (--channel-types-json). A mismatch
  // invalidates the snapshot for the same reason lockAllowlist does.
  std::vector<ChannelTypeSpec> channelTypes;

  // Stamps of every TU the bake was asked for (the requested scope),
  // whether or not its parse succeeded — see `outcomes`.
  std::vector<FileStamp> files;
  // v9: every file the frontend opened while parsing those TUs (headers,
  // .inc/.def, PCH inputs), deduplicated and stamped as the frontend saw
  // it, and per TU (parallel to `files`) the indices of the ones it
  // opened. A TU is dirty on warm start when its own stamp or any of its
  // dependencies' changed (SnapshotIO::dirtyTUs).
  std::vector<FileStamp> deps;
  std::vector<std::vector<uint32_t>> tuDeps;
  // v8: the bake's --entry-point list, so query verbs and serve default
  // to the same roots the index was built for (empty = "main").
  std::vector<std::string> entryPoints;
  // v10: per TU (parallel to `files`) the effective-input fingerprint the
  // TU was baked under (InputFingerprint.h) and how its parse ended. A
  // TU is also dirty on warm start when its fingerprint differs from the
  // one computed now, or when its outcome is anything but Indexed.
  std::vector<std::string> fingerprints;
  std::vector<TuOutcome> outcomes;
  IndexProvenance provenance;
};

/// Coverage counts over meta.files/outcomes. A TU without a recorded
/// outcome (a meta written by an older bake path) counts as skipped.
IndexCoverage coverageOf(const SnapshotMeta &meta);

/// Counts recorded in the v8 header so `info` and graph_summary can
/// describe an index without decoding it.
struct IndexSummary {
  uint64_t nodes = 0;
  uint64_t edges = 0;
  uint64_t callSites = 0;
  uint64_t channelSites = 0;
};

/// Sections a load may ask for (bitmask). Meta and the summary are always
/// read. Each query tool declares what it reads (ToolEntry::needs) and the
/// query verbs load only that; index/serve/batch load everything.
enum IndexSection : unsigned {
  kSectionGraph = 1,
  kSectionControlFlow = 2,
  kSectionChannels = 4,
  kSectionAll = 7,
};

struct SnapshotData {
  CallGraph graph;
  ControlFlowIndex cfIndex;
  ChannelIndex channels;
  SnapshotMeta meta;
  IndexSummary summary;
  unsigned loaded = 0; // IndexSection bits actually decoded
};

/// Where the load time goes, section by section in file order. Decode,
/// not I/O, is the cost (the file is mmapped), so `ms` is the time spent
/// walking that section's records and installing them; `bytes` is the
/// section's on-disk size. Reported by `--stats-json` and the query
/// verbs' `-v` (docs/megascope-cli-review.md §3.1.1: the measurement that
/// gates a sectioned v8 layout).
struct SnapshotLoadSection {
  const char *name;
  uint64_t bytes = 0;
  double ms = 0;
  bool skipped = false; // not requested by the load's IndexSection mask
};

struct SnapshotLoadStats {
  std::vector<SnapshotLoadSection> sections;
  uint64_t fileBytes = 0;
  double totalMs = 0;
};

/// What the loaded indexes will be used for. ReadOnly skips the state that
/// only mutation needs — CallGraph's edge dedup key map and per-TU
/// provenance (edgeIndex_, tuEdges_, nodeContributors_, tuNodes_), the
/// ControlFlowIndex set-dedup keys and per-TU map, ChannelIndex's per-TU
/// map — none of which a query reads (docs/megascope-cli-review.md §3.1.2).
/// A read-only graph asserts if mutated afterwards; `index`/`serve` and
/// worker shards must load Mutable.
enum class LoadMode { Mutable, ReadOnly };

class SnapshotIO {
public:
  /// Current on-disk format version. Bump on any layout change.
  /// v2: edges carry a contributor-TU list (deduped multi-TU edges).
  /// v3: EdgeKind::FunctionPointerReturn — deferred function-return join
  ///     rows that must be expanded at query time; older binaries would
  ///     misread them as ordinary edges.
  /// v4: control-flow contexts stored in deduplicated form (scope/guard/RAII
  ///     set tables + id refs).
  /// v5: id-preserving — interner tables serialized in id order; node/edge/
  ///     context records store raw interner ids and are bulk-installed on
  ///     load (no per-record re-interning). The v4 global string pool is
  ///     gone; the few non-interned strings (meta, node file/class, scope/
  ///     guard tables) are written inline.
  /// v6: USR identity (F8) — node records carry the display-name id next to
  ///     the usr key id; control-flow context records carry caller/callee
  ///     display ids next to the usr ids. The by-name indexes (CallGraph::
  ///     byName_, ControlFlowIndex display maps) are rebuilt on load.
  /// v7: channel/data-flow tracking — ChannelIndex sites (with refs and
  ///     per-TU contributor lists, same shape as CallGraph edges) serialized
  ///     alongside graph/cfIndex. Not interner-backed (ChannelIndex has no
  ///     StringInterner by design — see ChannelIndex.h); records are plain
  ///     length-prefixed strings.
  /// v8: sectioned — a fixed header (magic, version, IndexSummary, then a
  ///     table of {kind, offset, length} for meta / graph / control flow /
  ///     channels) lets a load decode only the sections a query needs;
  ///     meta carries the bake's entry points.
  /// v9: meta records each TU's dependencies (SnapshotMeta::deps/tuDeps)
  ///     so a header edit dirties its includers on warm start.
  /// v10: meta records the bake provenance and, per TU, the effective-
  ///     input fingerprint and the parse outcome (SnapshotMeta::
  ///     provenance/fingerprints/outcomes), so a compile-command change
  ///     or a failed parse is refreshed on warm start instead of cached.
  /// v11: catch handlers record whether their body rethrows
  ///     (CatchHandlerInfo::rethrows), and calls inside a handler body no
  ///     longer list the try whose handler they are in as protecting them
  ///     (the exception oracle's propagation order depends on both).
  /// v12: the control-flow section is laid out for reading in place —
  ///     its interner table is length-prefixed, the records are sorted
  ///     by call site, and string offsets, string-sorted ids, and
  ///     by-caller / by-callee position orders follow them — so a
  ///     ReadOnly load keeps the contexts in the mapped file
  ///     (ControlFlowIndex::isMapped; docs/control-flow-access.md). A
  ///     Mutable load decodes the records as before and skips the
  ///     orders.
  static constexpr uint32_t kFormatVersion = 12;
  /// Bytes before the first section: magic(4) + version(4) + summary(32) +
  /// table count(4) + 4 entries of kind(1) + offset(8) + length(8).
  static constexpr uint64_t kHeaderBytes = 4 + 4 + 32 + 4 + 4 * 17;

  /// Serialize graph + cfIndex + channels + meta to `path` (atomically, via
  /// a temp file and rename). `channels` defaults to empty so callers that
  /// don't use --channel-types-json are unaffected.
  /// Returns false on I/O failure.
  static bool save(const std::string &path, const CallGraph &graph,
                   const ControlFlowIndex &cfIndex, const SnapshotMeta &meta,
                   const ChannelIndex &channels = ChannelIndex());

  /// Load a snapshot. Returns nullopt if the file is missing, has a
  /// different format version, or fails to decode. `stats`, when given,
  /// receives the per-section decode timing (filled even on failure, up
  /// to the section that failed). `needs` selects the sections to decode
  /// (meta and the summary always are); a section left out stays empty
  /// and is absent from SnapshotData::loaded.
  static std::optional<SnapshotData>
  load(const std::string &path, SnapshotLoadStats *stats = nullptr,
       LoadMode mode = LoadMode::Mutable, unsigned needs = kSectionAll);

  /// Stat the given files into stamps. Files that cannot be stat'ed get
  /// mtimeNs = 0 and size = 0 (which never matches a real stamp, forcing a
  /// reindex of that TU).
  static std::vector<FileStamp>
  stampFiles(const std::vector<std::string> &files);

  /// Why a selected TU is dirty on warm start, first reason that applies
  /// in this order.
  enum class DirtyReason : uint8_t {
    Clean = 0,
    Stamp,  // not recorded, own stamp changed, or recorded stamp unknown
    Inputs, // recorded fingerprint differs from the current
    Deps,   // a file its parse opened changed
    Retry,  // its last parse did not end Indexed
  };

  /// The reasons behind a dirtyTUs answer: per TU (parallel to `current`)
  /// and counted, beyond the own-stamp reason. Retry is the one reason
  /// the caller may decline (main.cpp: a refresh that would otherwise
  /// touch nothing leaves failed TUs as recorded unless --retry-failed).
  struct DirtyReport {
    size_t viaInputs = 0; // DirtyReason::Inputs
    size_t viaDeps = 0;   // DirtyReason::Deps
    size_t retried = 0;   // DirtyReason::Retry
    std::vector<DirtyReason> reasons;
    /// Per TU, the evidence behind its reason, for a diagnostic line:
    /// "not recorded", "own stamp changed", the first changed
    /// dependency with its recorded and current stamps, ... Empty for
    /// a clean TU.
    std::vector<std::string> detail;
  };

  /// Which of `current` (stamps of the selected TUs, taken before the
  /// parse) must be re-indexed against `meta`: not recorded, own stamp
  /// changed, recorded effective-input fingerprint different from the
  /// entry of `fingerprints` (parallel to `current`; null skips the
  /// check), any recorded dependency changed, or the last outcome not
  /// Indexed. Dependency stamps are compared at whole-second mtime
  /// resolution (what the frontend records); a dependency that no longer
  /// exists counts as changed, and so does a stamp recorded as unstable
  /// (markUnstableStamps). Returns one flag per entry of `current`.
  static std::vector<bool>
  dirtyTUs(const SnapshotMeta &meta, const std::vector<FileStamp> &current,
           const std::vector<std::string> *fingerprints = nullptr,
           DirtyReport *report = nullptr);

  /// Stamps cannot prove content: a file modified again in the same
  /// second as its recorded mtime, at the same size, looks unchanged to
  /// the whole-second dependency stamp (and to the TU stamp on a
  /// filesystem with coarse mtimes). The window is only open for files
  /// whose mtime is not older than the bake that stamped them, so those
  /// stamps (TU and dependency alike) are recorded with mtime 0, which
  /// never matches: the TU is re-indexed once on the next warm start and
  /// stamped for good then. Returns how many stamps were marked.
  static size_t markUnstableStamps(SnapshotMeta &meta, uint64_t bakeStartNs);

  /// meta.deps/tuDeps expanded to per-TU stamp lists, keyed by TU path.
  static TuDependencies dependenciesOf(const SnapshotMeta &meta);

  /// Replace meta.deps/tuDeps from `deps` for the TUs in meta.files (a TU
  /// absent from `deps` gets an empty list; TUs absent from meta.files
  /// are ignored). A dependency two TUs saw with different stamps is
  /// recorded with the older one, so the TU that parsed the earlier
  /// version is still dirtied.
  static void recordDependencies(SnapshotMeta &meta,
                                 const TuDependencies &deps);

  /// meta.outcomes keyed by TU path (a TU without a recorded outcome is
  /// absent).
  static TuOutcomes outcomesOf(const SnapshotMeta &meta);

  /// Replace meta.outcomes from `outcomes` for the TUs in meta.files. A
  /// requested TU with no outcome at all is recorded as Skipped ("no
  /// outcome recorded"): a parse that never reported back must not pass
  /// for a healthy cached TU.
  static void recordOutcomes(SnapshotMeta &meta, const TuOutcomes &outcomes);
};

} // namespace vycor
