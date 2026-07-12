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

struct FileStamp {
  std::string path;
  uint64_t mtimeNs = 0; // last modification, nanoseconds since epoch
  uint64_t size = 0;

  bool operator==(const FileStamp &o) const {
    return path == o.path && mtimeNs == o.mtimeNs && size == o.size;
  }
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

  // Stamps of every TU baked into the snapshot.
  std::vector<FileStamp> files;
};

struct SnapshotData {
  CallGraph graph;
  ControlFlowIndex cfIndex;
  ChannelIndex channels;
  SnapshotMeta meta;
};

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
  static constexpr uint32_t kFormatVersion = 7;

  /// Serialize graph + cfIndex + channels + meta to `path` (atomically, via
  /// a temp file and rename). `channels` defaults to empty so callers that
  /// don't use --channel-types-json (or the --isolate-workers worker-shard
  /// path, which doesn't support channel tracking yet) are unaffected.
  /// Returns false on I/O failure.
  static bool save(const std::string &path, const CallGraph &graph,
                   const ControlFlowIndex &cfIndex, const SnapshotMeta &meta,
                   const ChannelIndex &channels = ChannelIndex());

  /// Load a snapshot. Returns nullopt if the file is missing, has a
  /// different format version, or fails to decode.
  static std::optional<SnapshotData> load(const std::string &path);

  /// Stat the given files into stamps. Files that cannot be stat'ed get
  /// mtimeNs = 0 and size = 0 (which never matches a real stamp, forcing a
  /// reindex of that TU).
  static std::vector<FileStamp>
  stampFiles(const std::vector<std::string> &files);
};

} // namespace vycor
