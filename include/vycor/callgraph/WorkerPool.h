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

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

#include <functional>
#include <system_error>
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
  /// Forwarded verbatim as --channel-types-json / --org-config so the
  /// worker rebuilds the same ChannelTypeConfig the parent merged (CLI
  /// JSON first, then org-config/registry types, deduped). Empty = off.
  std::string channelTypesJson;
  std::string orgConfig;
};

/// Resource limits for one worker process (--worker-timeout,
/// --worker-memory-limit). Shared by the megascope bake and anneal.
struct WorkerLimits {
  /// Seconds a worker may go without starting its next TU (a new
  /// `WORKER-TU` stderr marker) before it is killed and its batch handled
  /// as a crash, with the in-flight TU recorded as timed out. The deadline
  /// restarts at every marker, so a batch of N TUs may run up to
  /// N x timeoutSeconds, but a single hung TU is caught after one. 0 = no
  /// timeout.
  unsigned timeoutSeconds = 600;
  /// Per-worker data-segment limit in MiB (RLIMIT_DATA, applied in the
  /// child by llvm::sys::ExecuteNoWait); an allocation past it fails and
  /// the worker dies, which the crash protocol handles. 0 = no limit.
  unsigned memoryLimitMB = 0;
};

/// Exit code runWorkerProcess reports for a worker it killed on timeout.
constexpr int kWorkerTimedOut = -124;
/// Exit code runWorkerProcess reports when an interrupt (SIGINT/SIGTERM)
/// stopped the spawn or killed the worker.
constexpr int kWorkerInterrupted = -130;

/// Spawn `argv` (argv[0] is the program), stdin from the null device,
/// stdout and stderr appended to `logPath`, under `limits`, and wait for
/// it. Returns the exit code; -1 when it could not be spawned; -2 when a
/// signal killed it (llvm::sys::Wait's convention); kWorkerTimedOut when
/// it was killed for making no progress. The child is tracked for the
/// interrupt handler (callgraph/Interrupt.h). `tool` prefixes messages.
int runWorkerProcess(const std::vector<std::string> &argv,
                     const std::string &logPath, const WorkerLimits &limits,
                     const char *tool);

/// Why the dispatcher dropped a TU.
enum class WorkerFailure {
  Crashed,  // its worker exited nonzero or died by a signal
  TimedOut, // its worker was killed by the timeout while parsing it
};

/// Test seam: run one worker over `batch`, writing its snapshot shard to
/// `shardPath` and its stderr (WORKER-TU markers + diagnostics) to
/// `stderrPath`. Returns the process exit code; any nonzero value
/// (including the negative codes llvm::sys::ExecuteAndWait reports for
/// spawn failure or death by signal) triggers the crash/bisect protocol;
/// kWorkerTimedOut marks the failure as a timeout.
using WorkerRunner = std::function<int(const std::vector<std::string> &batch,
                                       const std::string &shardPath,
                                       const std::string &stderrPath)>;

/// Generic dispatcher core, shared by the megascope bake and the anneal
/// isolated phases. Batches `files`, keeps <= `workers` runner invocations
/// in flight, and calls `consumeShard(shardPath, batchTus, wallMs)` on the
/// calling thread for each batch whose runner exited 0 — return false to
/// treat the batch as failed anyway (unreadable/torn shard), which
/// retries it like a markerless crash. The crash/bisect protocol is as
/// documented on bakeIsolatedWithRunner; `onPoison` is invoked (calling
/// thread) for each TU the protocol drops, with the failure that dropped
/// it. Once an interrupt was requested no new batch is started. Shard and stderr files are
/// created inside `shardDir` (which must exist) and removed as consumed.
void dispatchIsolated(
    const WorkerRunner &runner, const std::vector<std::string> &files,
    unsigned workers, const std::string &shardDir,
    const std::function<bool(const std::string &shardPath,
                             const std::vector<std::string> &batchTus,
                             double wallMs)> &consumeShard,
    const std::function<void(const std::string &tu, WorkerFailure why)>
        &onPoison,
    unsigned batchSizeOverride = 0);

/// Dispatcher core: batches `files`, keeps <= `workers` runner invocations
/// in flight, absorbs each shard into the returned indexes as it lands
/// (single-threaded, on the calling thread), and applies the crash/bisect
/// protocol to failed batches (marker TU poisoned + batch re-dispatched
/// without it; markerless failures split in half; a markerless single-TU
/// batch is poisoned; each TU re-dispatched at most twice). Poisoned TUs
/// are recorded in `stats` with toolStatus -1 and in the outcomes as
/// Poisoned, or TimedOut when the timeout killed their worker; clean TUs with toolStatus 0
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
/// under the system temp dir (also removed on SIGINT/SIGTERM). Workers run
/// single-threaded so the last WORKER-TU marker is an exact poison
/// identifier; parallelism comes from the worker count.
BakedIndexes bakeIsolated(const std::string &selfExe, const McpBakeConfig &cfg,
                          const std::vector<std::string> &files,
                          unsigned workers, BuildStats *stats,
                          const WorkerLimits &limits = {});

/// Crash- and hang-safe single-TU parse (reindex_tu): bakes `file` in one
/// worker process under `limits` and returns that TU's indexes without
/// touching any live index. `outcomes[file]` says how the parse ended; on
/// a crash (Poisoned) or timeout (TimedOut) the indexes are empty, so the
/// caller can keep, drop, or replace the TU's old facts as it chooses.
BakedIndexes bakeTUIsolated(const std::string &selfExe,
                            const McpBakeConfig &cfg, const std::string &file,
                            const WorkerLimits &limits = {});

/// Create a unique directory <system temp>/<base>-XXXXXX (base
/// vycor-workers, vycor-anneal-workers) for one dispatch run. On failure
/// returns the error and `out` names the attempted prefix.
std::error_code createWorkerShardDir(llvm::StringRef base,
                                     llvm::SmallVectorImpl<char> &out);

/// Remove a directory made by createWorkerShardDir and everything in it.
void removeWorkerShardDir(llvm::StringRef dir);

} // namespace vycor
