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

#include "vycor/callgraph/BuildStats.h"
#include "vycor/callgraph/ControlFlowIndex.h"

#include <functional>
#include <string>
#include <vector>

namespace vycor {

// ============================================================================
// Subprocess worker isolation (design doc F12). The parent re-invokes its
// own binary as `megascope --bake-worker` over batches of TUs; each worker
// bakes its batch in-process (crash guard still active — first line of
// defense) and writes a v5 id-preserving snapshot shard, which the parent
// absorbs into the master indexes. A worker killed by a poisoned TU costs
// exactly that TU: the last `WORKER-TU <path>` stderr marker identifies it
// and the batch is re-dispatched without it.
//
// Lives under callgraph/ (not mcp/): the dispatcher has no MCP dependency —
// it is a bake strategy over CallGraph/ControlFlowIndex/SnapshotIO.
// ============================================================================

/// Everything needed to reconstruct a worker's megascope argv. The worker
/// re-derives all bake state from these plus its explicit --source batch.
struct McpBakeConfig {
  std::string buildPath;
  std::vector<std::string> collapsePaths;
  /// Forwarded as --extra-arg; callers pass vycor::globalExtraArgs().
  std::vector<std::string> extraArgs;
  std::string sysroot;
  std::vector<std::string> lockTypes; // LockTypeConfig::userAllowlist
};

/// Test seam: run one worker over `batch`, writing its snapshot shard to
/// `shardPath` and its stderr (WORKER-TU markers + diagnostics) to
/// `stderrPath`. Returns the process exit code; any nonzero value
/// (including the negative codes llvm::sys::ExecuteAndWait reports for
/// spawn failure or death by signal) triggers the crash/bisect protocol.
using WorkerRunner = std::function<int(const std::vector<std::string> &batch,
                                       const std::string &shardPath,
                                       const std::string &stderrPath)>;

/// Dispatcher core: batches `files`, keeps <= `workers` runner invocations
/// in flight, absorbs each shard into the returned indexes as it lands
/// (single-threaded, on the calling thread), and applies the crash/bisect
/// protocol to failed batches (marker TU poisoned + batch re-dispatched
/// without it; markerless failures split in half; a markerless single-TU
/// batch is poisoned; each TU re-dispatched at most twice). Poisoned TUs
/// are recorded in `stats` with toolStatus -1; clean TUs with toolStatus 0
/// and the batch wall time divided evenly. `shardDir` must exist; shard and
/// stderr files are created inside it and removed as they are consumed.
/// `expected`, when non-null, sanity-checks each shard's recorded build
/// config (mismatch is loud but non-fatal — shard meta is otherwise
/// ignored). `batchSizeOverride` pins the batch size for tests; 0 = the
/// production heuristic max(1, files / (workers * 4)) capped at 32.
BakedIndexes bakeIsolatedWithRunner(const WorkerRunner &runner,
                                    const std::vector<std::string> &files,
                                    unsigned workers, BuildStats *stats,
                                    const std::string &shardDir,
                                    const McpBakeConfig *expected = nullptr,
                                    unsigned batchSizeOverride = 0);

/// Production entry: spawn `selfExe megascope --bake-worker ...` workers
/// over `files` (selfExe from llvm::sys::fs::getMainExecutable) and merge
/// their shards. Creates — and removes on return — a unique shard directory
/// under the system temp dir. Workers run single-threaded so the last
/// WORKER-TU marker is an exact poison identifier; parallelism comes from
/// the worker count.
BakedIndexes bakeIsolated(const std::string &selfExe, const McpBakeConfig &cfg,
                          const std::vector<std::string> &files,
                          unsigned workers, BuildStats *stats);

} // namespace vycor
