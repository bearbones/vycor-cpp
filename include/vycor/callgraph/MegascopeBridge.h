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
#include "vycor/callgraph/Snapshot.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/JSON.h"

#include <string>
#include <vector>

namespace vycor {

// ============================================================================
// MegascopeBridge — lowers vycor-cpp's C++ CallGraph, ControlFlowIndex,
// ChannelIndex, and SnapshotMeta into the language-agnostic `megascope`
// ModuleShard / ProgramIndex model (matching megascope/src/model.rs) and
// bridges directly to libmegascope's C FFI (`include/megascope.h`).
// ============================================================================

class MegascopeBridge {
public:
  /// True when `path` names a `megascope` binary snapshot (`.mgsc` or `.pyrx`
  /// extension, or an existing file on disk starting with `PYRX\x00\x01\x00\x00`).
  static bool isMegascopeSnapshotPath(llvm::StringRef path);

  /// True when `path` names a `megascope` JSON module shard (`.shard.json` or
  /// `.shards.json`).
  static bool isMegascopeShardJsonPath(llvm::StringRef path);

  /// Lower `graph`, `cfIndex`, `channels`, and `meta` into a single
  /// `megascope::ModuleShard` JSON object. When `tuFilter` is non-empty,
  /// only facts contributed by that TU are included.
  static llvm::json::Object
  toShardObject(const CallGraph &graph, const ControlFlowIndex &cfIndex,
                const ChannelIndex &channels, const SnapshotMeta &meta,
                llvm::StringRef tuFilter = "");

  /// Lower `graph`, `cfIndex`, `channels`, and `meta` into a JSON array of
  /// per-TU `megascope::ModuleShard` objects (one per TU in `meta.files`, or
  /// a single combined shard when `meta.files` has <= 1 entry).
  static llvm::json::Array
  toShardsArray(const CallGraph &graph, const ControlFlowIndex &cfIndex,
                const ChannelIndex &channels, const SnapshotMeta &meta);

  /// Serialize a single `megascope::ModuleShard` JSON string.
  static std::string
  toShardJson(const CallGraph &graph, const ControlFlowIndex &cfIndex,
              const ChannelIndex &channels, const SnapshotMeta &meta,
              bool pretty = false);

  /// Serialize a JSON array of `megascope::ModuleShard`s (or a single shard
  /// object when `meta.files.size() <= 1`).
  static std::string
  toShardsJson(const CallGraph &graph, const ControlFlowIndex &cfIndex,
               const ChannelIndex &channels, const SnapshotMeta &meta,
               bool pretty = false);

  /// Build the `meta_json` string expected by `megascope_bake_shards_json` and
  /// `megascope_merge_snapshots` (`{"projectRoot":..., "entryPoints":..., "collapsePaths":...}`).
  static std::string toMetaJson(const SnapshotMeta &meta,
                                llvm::StringRef projectRoot = "");

  /// Atomically write `ModuleShard` JSON to `path`.
  static bool saveShardJson(const std::string &path, const CallGraph &graph,
                            const ControlFlowIndex &cfIndex,
                            const SnapshotMeta &meta,
                            const ChannelIndex &channels = ChannelIndex(),
                            std::string *error = nullptr, bool durable = true,
                            bool pretty = true);

  /// Bake `graph`, `cfIndex`, `channels`, and `meta` into a binary `.mgsc`
  /// snapshot at `path` via `libmegascope` (`megascope_bake_shards_json`).
  static bool saveSnapshot(const std::string &path, const CallGraph &graph,
                           const ControlFlowIndex &cfIndex,
                           const SnapshotMeta &meta,
                           const ChannelIndex &channels = ChannelIndex(),
                           std::string *error = nullptr);

  /// Merge multiple `.mgsc`, `.pyrx`, `.shard.json`, or `.vycs` shards into a
  /// single unified `.mgsc` snapshot at `outputPath` via `libmegascope`
  /// (`megascope_merge_snapshots`), supporting cross-language C++ + Python
  /// index merging.
  static bool mergeSnapshots(const std::string &outputPath,
                             const std::vector<std::string> &inputPaths,
                             const std::vector<std::string> &entryPoints = {},
                             const std::vector<std::string> &collapsePaths = {},
                             std::string *error = nullptr);

  /// Run any `megascope` verb (`info`, `dump`, `diff`, `tools`, `get-callers`,
  /// `get-callees`, `analyze-dead-code`, etc.) against a `.mgsc` snapshot file
  /// (`indexPath`) or in-memory `shardsJson` via `megascope_run_verb_json`.
  /// Returns the `megascope` exit code (`0..=4`).
  static int runVerbJson(llvm::StringRef verb, const llvm::json::Object &args,
                         llvm::StringRef indexPath, llvm::StringRef shardsJson,
                         std::string &outputOut, std::string &errorOut);
};

inline std::string
toMegascopeShardJson(const CallGraph &graph, const ControlFlowIndex &cfIndex,
                     const ChannelIndex &channels, const SnapshotMeta &meta,
                     bool pretty = false) {
  return MegascopeBridge::toShardJson(graph, cfIndex, channels, meta, pretty);
}

inline std::string
toMegascopeShardsJson(const CallGraph &graph, const ControlFlowIndex &cfIndex,
                      const ChannelIndex &channels, const SnapshotMeta &meta,
                      bool pretty = false) {
  return MegascopeBridge::toShardsJson(graph, cfIndex, channels, meta, pretty);
}

} // namespace vycor
