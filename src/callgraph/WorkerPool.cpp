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

#include "vycor/callgraph/WorkerPool.h"

#include "vycor/callgraph/Snapshot.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>

namespace vycor {

namespace {

/// A TU is re-dispatched into a new batch at most this many times after a
/// batch it belonged to failed (design doc: "each TU is retried at most
/// twice").
constexpr int kMaxTuRetries = 2;

struct Batch {
  std::vector<std::string> tus;
};

struct BatchResult {
  Batch batch;
  int exitCode = 0;
  std::string shardPath;
  std::string stderrPath;
  double wallMs = 0.0;
};

/// Last `WORKER-TU <path>` line in a worker's stderr log, or nullopt (spawn
/// failure, or a crash before the first parse started).
std::optional<std::string> lastWorkerTuMarker(const std::string &stderrPath) {
  auto buf = llvm::MemoryBuffer::getFile(stderrPath, /*IsText=*/true);
  if (!buf)
    return std::nullopt;
  llvm::StringRef text = (*buf)->getBuffer();
  std::optional<std::string> last;
  while (!text.empty()) {
    auto [line, rest] = text.split('\n');
    text = rest;
    if (line.consume_front("WORKER-TU "))
      last = line.trim().str();
  }
  return last;
}

} // namespace

BakedIndexes bakeIsolatedWithRunner(const WorkerRunner &runner,
                                    const std::vector<std::string> &files,
                                    unsigned workers, BuildStats *stats,
                                    const std::string &shardDir,
                                    const McpBakeConfig *expected,
                                    unsigned batchSizeOverride) {
  BakedIndexes out;
  if (files.empty())
    return out;
  if (workers == 0)
    workers = 1;

  // Small enough for pipelined merging and cheap retry, large enough to
  // amortize process spawn and shard write (design doc §Batching).
  size_t batchSize =
      batchSizeOverride
          ? batchSizeOverride
          : std::min<size_t>(
                32, std::max<size_t>(
                        1, files.size() / (static_cast<size_t>(workers) * 4)));

  // Dispatch threads pull batches from `work` and block in the synchronous
  // runner (the real one sits in ExecuteAndWait); results funnel back to
  // this thread, which absorbs shards and drives the bisect protocol.
  // One blocking thread per in-flight process is the simplest correct way
  // to keep exactly <= workers subprocesses running: no poll interval, no
  // round-robin Wait, and the runner seam stays a plain function.
  std::mutex mu;
  std::condition_variable workCv, resultCv;
  std::deque<Batch> work;
  std::deque<BatchResult> results;
  bool stop = false;
  size_t outstanding = 0; // batches queued or running
  unsigned shardSeq = 0;

  for (size_t i = 0; i < files.size(); i += batchSize) {
    Batch b;
    b.tus.assign(files.begin() + i,
                 files.begin() + std::min(files.size(), i + batchSize));
    work.push_back(std::move(b));
  }
  outstanding = work.size();

  auto dispatchLoop = [&]() {
    for (;;) {
      Batch b;
      unsigned seq;
      {
        std::unique_lock<std::mutex> lock(mu);
        workCv.wait(lock, [&] { return stop || !work.empty(); });
        if (work.empty())
          return;
        b = std::move(work.front());
        work.pop_front();
        seq = shardSeq++;
      }
      BatchResult res;
      res.shardPath = shardDir + "/shard-" + std::to_string(seq) + ".snap";
      res.stderrPath = shardDir + "/worker-" + std::to_string(seq) + ".stderr";
      auto t0 = std::chrono::steady_clock::now();
      res.exitCode = runner(b.tus, res.shardPath, res.stderrPath);
      res.wallMs = std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now() - t0)
                       .count();
      res.batch = std::move(b);
      {
        std::lock_guard<std::mutex> lock(mu);
        results.push_back(std::move(res));
      }
      resultCv.notify_one();
    }
  };

  std::vector<std::thread> threads;
  threads.reserve(workers);
  for (unsigned i = 0; i < workers; ++i)
    threads.emplace_back(dispatchLoop);

  std::unordered_map<std::string, int> retries;
  unsigned poisonedCount = 0;

  auto poison = [&](const std::string &tu) {
    ++poisonedCount;
    llvm::errs() << "megascope: worker: TU poisoned (crashed worker): " << tu
                 << "\n";
    if (stats)
      stats->addTuStat({tu, 0, 0.0, -1});
  };

  // Re-enqueue the given TUs as one batch, dropping (and poisoning) any TU
  // already re-dispatched kMaxTuRetries times.
  auto requeue = [&](std::vector<std::string> tus) {
    tus.erase(std::remove_if(tus.begin(), tus.end(),
                             [&](const std::string &tu) {
                               if (++retries[tu] > kMaxTuRetries) {
                                 poison(tu);
                                 return true;
                               }
                               return false;
                             }),
              tus.end());
    if (tus.empty())
      return;
    {
      std::lock_guard<std::mutex> lock(mu);
      ++outstanding;
      work.push_back(Batch{std::move(tus)});
    }
    workCv.notify_one();
  };

  auto handleFailure = [&](BatchResult &res) {
    auto marker = lastWorkerTuMarker(res.stderrPath);
    bool markerInBatch =
        marker && std::find(res.batch.tus.begin(), res.batch.tus.end(),
                            *marker) != res.batch.tus.end();
    if (markerInBatch) {
      // The TU whose parse was in flight when the worker died is presumed
      // poisoned; everything else in the batch is re-dispatched (the shard
      // was never written, so already-parsed TUs are re-baked too).
      poison(*marker);
      std::vector<std::string> rest;
      rest.reserve(res.batch.tus.size() - 1);
      for (auto &tu : res.batch.tus)
        if (tu != *marker)
          rest.push_back(std::move(tu));
      requeue(std::move(rest));
    } else if (res.batch.tus.size() == 1) {
      poison(res.batch.tus.front());
    } else {
      // No marker (spawn failure, or death before the first parse): split
      // in half and re-dispatch both halves.
      size_t mid = res.batch.tus.size() / 2;
      requeue({res.batch.tus.begin(), res.batch.tus.begin() + mid});
      requeue({res.batch.tus.begin() + mid, res.batch.tus.end()});
    }
  };

  // Single-threaded parent loop: pop one finished batch, absorb or bisect,
  // repeat until nothing is queued or running.
  for (;;) {
    BatchResult res;
    {
      std::unique_lock<std::mutex> lock(mu);
      if (outstanding == 0)
        break;
      resultCv.wait(lock, [&] { return !results.empty(); });
      res = std::move(results.front());
      results.pop_front();
      --outstanding;
    }

    if (res.exitCode == 0) {
      if (auto snap = SnapshotIO::load(res.shardPath)) {
        if (expected && (snap->meta.collapsePaths != expected->collapsePaths ||
                         snap->meta.lockAllowlist != expected->lockTypes))
          llvm::errs() << "megascope: worker: WARNING: shard "
                       << res.shardPath
                       << " records a different build configuration\n";
        out.graph.absorb(snap->graph);
        out.cfIndex.absorb(snap->cfIndex);
        if (stats) {
          // Honest per-TU accounting isn't available across a batch; record
          // the batch wall divided evenly rather than faking parse times.
          double per = res.wallMs / static_cast<double>(res.batch.tus.size());
          for (const auto &tu : res.batch.tus)
            stats->addTuStat({tu, 0, per, 0});
        }
      } else {
        // Exit 0 but no readable shard (torn write, deleted temp): treat as
        // a markerless failure so the batch is retried/bisected.
        llvm::errs() << "megascope: worker: WARNING: worker exited cleanly "
                        "but shard "
                     << res.shardPath << " is unreadable — retrying batch\n";
        res.exitCode = -1;
      }
    }
    if (res.exitCode != 0)
      handleFailure(res);

    llvm::sys::fs::remove(res.shardPath);
    llvm::sys::fs::remove(res.stderrPath);
  }

  {
    std::lock_guard<std::mutex> lock(mu);
    stop = true;
  }
  workCv.notify_all();
  for (auto &t : threads)
    t.join();

  if (poisonedCount > 0)
    llvm::errs() << "megascope: " << poisonedCount
                 << " TU(s) poisoned (crashed their worker) and were skipped ("
                 << files.size() << " TUs total)\n";
  return out;
}

namespace {

WorkerRunner makeSubprocessRunner(const std::string &selfExe,
                                  const McpBakeConfig &cfg) {
  return [selfExe, cfg](const std::vector<std::string> &batch,
                        const std::string &shardPath,
                        const std::string &stderrPath) -> int {
    std::vector<std::string> argv;
    argv.reserve(12 + 2 * batch.size());
    argv.push_back(selfExe);
    argv.push_back("megascope");
    argv.push_back("--bake-worker");
    argv.push_back("--worker-out");
    argv.push_back(shardPath);
    argv.push_back("--build-path");
    argv.push_back(cfg.buildPath);
    // Single-threaded worker: keeps the last WORKER-TU marker an exact
    // poison identifier (a parallel worker's markers interleave).
    argv.push_back("--threads");
    argv.push_back("1");
    for (const auto &p : cfg.collapsePaths) {
      argv.push_back("--collapse-paths");
      argv.push_back(p);
    }
    for (const auto &a : cfg.extraArgs)
      argv.push_back("--extra-arg=" + a);
    if (!cfg.sysroot.empty()) {
      argv.push_back("--sysroot");
      argv.push_back(cfg.sysroot);
    }
    for (const auto &t : cfg.lockTypes) {
      argv.push_back("--lock-types");
      argv.push_back(t);
    }
    for (const auto &f : batch) {
      argv.push_back("--source");
      argv.push_back(f);
    }

    std::vector<llvm::StringRef> args(argv.begin(), argv.end());
    // stdin from the null device (empty redirect path = null device); stdout
    // joins the stderr log — the parent's own stdout may be an MCP channel
    // and must never see worker output (ExecuteAndWait dups identical
    // stdout/stderr paths onto one descriptor).
    std::optional<llvm::StringRef> redirects[3] = {
        llvm::StringRef(""), llvm::StringRef(stderrPath),
        llvm::StringRef(stderrPath)};
    std::string errMsg;
    bool execFailed = false;
    int rc = llvm::sys::ExecuteAndWait(selfExe, args, /*Env=*/std::nullopt,
                                       redirects, /*SecondsToWait=*/0,
                                       /*MemoryLimit=*/0, &errMsg, &execFailed);
    if (execFailed)
      llvm::errs() << "megascope: worker: failed to spawn " << selfExe << ": "
                   << errMsg << "\n";
    return rc;
  };
}

} // namespace

BakedIndexes bakeIsolated(const std::string &selfExe, const McpBakeConfig &cfg,
                          const std::vector<std::string> &files,
                          unsigned workers, BuildStats *stats) {
  llvm::SmallString<128> tmpBase;
  llvm::sys::path::system_temp_directory(/*ErasedOnReboot=*/true, tmpBase);
  llvm::sys::path::append(tmpBase, "vycor-workers");
  llvm::SmallString<128> shardDir;
  if (auto ec = llvm::sys::fs::createUniqueDirectory(tmpBase, shardDir)) {
    llvm::errs() << "megascope: ERROR: cannot create worker shard directory "
                    "under "
                 << tmpBase << ": " << ec.message()
                 << " — isolated bake aborted (indexes will be empty)\n";
    return {};
  }

  auto out =
      bakeIsolatedWithRunner(makeSubprocessRunner(selfExe, cfg), files,
                             workers, stats, std::string(shardDir), &cfg);
  llvm::sys::fs::remove_directories(shardDir);
  return out;
}

} // namespace vycor
