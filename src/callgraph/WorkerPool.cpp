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

#include "vycor/callgraph/Interrupt.h"
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
#include <cerrno>
#include <cstdio>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>

#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>

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

/// Incremental scan of a worker's log for new `WORKER-TU ` line prefixes:
/// the worker's progress signal for the timeout.
class MarkerWatch {
public:
  explicit MarkerWatch(std::string path) : path_(std::move(path)) {}

  /// True when at least one marker started since the last call.
  bool advanced() {
    uint64_t size = 0;
    if (llvm::sys::fs::file_size(path_, size) || size <= offset_)
      return false;
    std::FILE *f = std::fopen(path_.c_str(), "rb");
    if (!f)
      return false;
    bool found = false;
    if (std::fseek(f, static_cast<long>(offset_), SEEK_SET) == 0) {
      char buf[4096];
      size_t n;
      while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) {
        offset_ += n;
        for (size_t i = 0; i < n; ++i)
          found |= step(buf[i]);
      }
    }
    std::fclose(f);
    return found;
  }

private:
  bool step(char c) {
    static constexpr llvm::StringLiteral kPrefix("WORKER-TU ");
    if (c == '\n') {
      state_ = 0;
      return false;
    }
    if (state_ < 0)
      return false;
    if (c != kPrefix[static_cast<size_t>(state_)]) {
      state_ = -1;
      return false;
    }
    if (static_cast<size_t>(++state_) == kPrefix.size()) {
      state_ = -1;
      return true;
    }
    return false;
  }

  std::string path_;
  uint64_t offset_ = 0;
  int state_ = 0; // prefix chars matched at line start; -1 mid-line
};

/// llvm::sys::Wait's exit-code convention for a reaped status.
int exitCodeOf(int status) {
  if (WIFEXITED(status))
    return WEXITSTATUS(status);
  return -2; // killed by a signal
}

} // namespace

int runWorkerProcess(const std::vector<std::string> &argv,
                     const std::string &logPath, const WorkerLimits &limits,
                     const char *tool) {
  if (argv.empty())
    return -1;
  std::vector<llvm::StringRef> args(argv.begin(), argv.end());
  // stdin from the null device (empty redirect path = null device); stdout
  // joins the stderr log — the parent's own stdout may be an MCP channel
  // and must never see worker output (identical stdout/stderr paths are
  // dup'd onto one descriptor).
  std::optional<llvm::StringRef> redirects[3] = {
      llvm::StringRef(""), llvm::StringRef(logPath), llvm::StringRef(logPath)};
  std::string errMsg;
  bool execFailed = false;
  llvm::sys::ProcessInfo pi;
  long pid = detail::spawnTrackedChild([&]() -> long {
    pi = llvm::sys::ExecuteNoWait(argv.front(), args, /*Env=*/std::nullopt,
                                  redirects, limits.memoryLimitMB, &errMsg,
                                  &execFailed);
    return static_cast<long>(pi.Pid);
  });
  if (pid <= 0) {
    if (interruptRequested())
      return kWorkerInterrupted;
    llvm::errs() << tool << ": worker: failed to spawn " << argv.front()
                 << ": " << errMsg << "\n";
    return -1;
  }

  // waitpid directly rather than llvm::sys::Wait: its timeout is a
  // process-wide alarm(), which concurrent dispatch threads would steal
  // from one another.
  const pid_t child = static_cast<pid_t>(pid);
  int status = 0;
  int rc;
  if (limits.timeoutSeconds == 0) {
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    rc = exitCodeOf(status);
  } else {
    using Clock = std::chrono::steady_clock;
    const auto window = std::chrono::seconds(limits.timeoutSeconds);
    auto deadline = Clock::now() + window;
    auto nap = std::chrono::milliseconds(2);
    MarkerWatch progress(logPath);
    for (;;) {
      pid_t r = ::waitpid(child, &status, WNOHANG);
      if (r == child) {
        rc = exitCodeOf(status);
        break;
      }
      if (r < 0 && errno != EINTR) {
        rc = -1;
        break;
      }
      auto now = Clock::now();
      if (progress.advanced())
        deadline = now + window;
      else if (now >= deadline) {
        ::kill(child, SIGKILL);
        while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
        }
        llvm::errs() << tool << ": worker: no progress for "
                     << limits.timeoutSeconds
                     << "s (--worker-timeout) — killed\n";
        rc = kWorkerTimedOut;
        break;
      }
      std::this_thread::sleep_for(nap);
      nap = std::min(nap * 2, std::chrono::milliseconds(100));
    }
  }
  detail::untrackChild(pid);
  if (rc != 0 && rc != kWorkerTimedOut && interruptRequested())
    return kWorkerInterrupted;
  return rc;
}

void dispatchIsolated(
    const WorkerRunner &runner, const std::vector<std::string> &files,
    unsigned workers, const std::string &shardDir,
    const std::function<bool(const std::string &shardPath,
                             const std::vector<std::string> &batchTus,
                             double wallMs)> &consumeShard,
    const std::function<void(const std::string &tu, WorkerFailure why)>
        &onPoison,
    unsigned batchSizeOverride) {
  if (files.empty())
    return;
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
      // Stop dispatching once interrupted: the watcher is tearing the
      // process down (callgraph/Interrupt.h).
      res.exitCode = interruptRequested()
                         ? kWorkerInterrupted
                         : runner(b.tus, res.shardPath, res.stderrPath);
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

  const auto &poison = onPoison;

  // Re-enqueue the given TUs as one batch, dropping (and poisoning) any TU
  // already re-dispatched kMaxTuRetries times.
  auto requeue = [&](std::vector<std::string> tus, WorkerFailure why) {
    tus.erase(std::remove_if(tus.begin(), tus.end(),
                             [&](const std::string &tu) {
                               if (++retries[tu] > kMaxTuRetries) {
                                 poison(tu, why);
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
    // A timeout is handled exactly like a crash; only the recorded reason
    // differs.
    const WorkerFailure why = res.exitCode == kWorkerTimedOut
                                  ? WorkerFailure::TimedOut
                                  : WorkerFailure::Crashed;
    auto marker = lastWorkerTuMarker(res.stderrPath);
    bool markerInBatch =
        marker && std::find(res.batch.tus.begin(), res.batch.tus.end(),
                            *marker) != res.batch.tus.end();
    if (markerInBatch) {
      // The TU whose parse was in flight when the worker died is presumed
      // poisoned; everything else in the batch is re-dispatched (the shard
      // was never written, so already-parsed TUs are re-baked too).
      poison(*marker, why);
      std::vector<std::string> rest;
      rest.reserve(res.batch.tus.size() - 1);
      for (auto &tu : res.batch.tus)
        if (tu != *marker)
          rest.push_back(std::move(tu));
      requeue(std::move(rest), WorkerFailure::Crashed);
    } else if (res.batch.tus.size() == 1) {
      poison(res.batch.tus.front(), why);
    } else {
      // No marker (spawn failure, or death before the first parse): split
      // in half and re-dispatch both halves.
      size_t mid = res.batch.tus.size() / 2;
      requeue({res.batch.tus.begin(), res.batch.tus.begin() + mid}, why);
      requeue({res.batch.tus.begin() + mid, res.batch.tus.end()}, why);
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
      // Exit 0 but no consumable shard (torn write, deleted temp): treat
      // as a markerless failure so the batch is retried/bisected.
      if (!consumeShard(res.shardPath, res.batch.tus, res.wallMs)) {
        llvm::errs() << "worker: WARNING: worker exited cleanly but shard "
                     << res.shardPath << " is unreadable — retrying batch\n";
        res.exitCode = -1;
      }
    }
    // An interrupted batch is abandoned, not bisected: the process is
    // going down.
    if (res.exitCode != 0 && res.exitCode != kWorkerInterrupted)
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
}

BakedIndexes bakeIsolatedWithRunner(const WorkerRunner &runner,
                                    const std::vector<std::string> &files,
                                    unsigned workers, BuildStats *stats,
                                    const std::string &shardDir,
                                    const McpBakeConfig *expected,
                                    unsigned batchSizeOverride) {
  BakedIndexes out;
  unsigned poisonedCount = 0;
  dispatchIsolated(
      runner, files, workers, shardDir,
      [&](const std::string &shardPath,
          const std::vector<std::string> &batchTus, double wallMs) {
        auto snap = SnapshotIO::load(shardPath);
        if (!snap)
          return false;
        if (expected && (snap->meta.collapsePaths != expected->collapsePaths ||
                         snap->meta.lockAllowlist != expected->lockTypes))
          llvm::errs() << "megascope: worker: WARNING: shard " << shardPath
                       << " records a different build configuration\n";
        out.graph.absorb(snap->graph);
        out.cfIndex.absorb(snap->cfIndex);
        out.channels.absorb(snap->channels);
        // The worker records its batch's dependencies and per-TU
        // outcomes in the shard meta.
        for (auto &kv : SnapshotIO::dependenciesOf(snap->meta))
          out.deps[kv.first] = std::move(kv.second);
        for (auto &kv : SnapshotIO::outcomesOf(snap->meta))
          out.outcomes[kv.first] = std::move(kv.second);
        if (stats) {
          // Honest per-TU accounting isn't available across a batch; record
          // the batch wall divided evenly rather than faking parse times.
          double per = wallMs / static_cast<double>(batchTus.size());
          for (const auto &tu : batchTus)
            stats->addTuStat({tu, 0, per, 0});
        }
        return true;
      },
      [&](const std::string &tu, WorkerFailure why) {
        ++poisonedCount;
        const bool timedOut = why == WorkerFailure::TimedOut;
        out.outcomes[tu] =
            timedOut ? TuOutcome{TuStatus::TimedOut, "worker timed out"}
                     : TuOutcome{TuStatus::Poisoned, "worker crashed"};
        llvm::errs() << "megascope: worker: TU poisoned ("
                     << (timedOut ? "timed out" : "crashed worker")
                     << "): " << tu << "\n";
        if (stats)
          stats->addTuStat({tu, 0, 0.0, -1});
      },
      batchSizeOverride);

  if (poisonedCount > 0)
    llvm::errs() << "megascope: " << poisonedCount
                 << " TU(s) poisoned (crashed or timed out their worker) and "
                    "were skipped ("
                 << files.size() << " TUs total)\n";
  return out;
}

namespace {

WorkerRunner makeSubprocessRunner(const std::string &selfExe,
                                  const McpBakeConfig &cfg,
                                  const WorkerLimits &limits) {
  return [selfExe, cfg, limits](const std::vector<std::string> &batch,
                        const std::string &shardPath,
                        const std::string &stderrPath) -> int {
    std::vector<std::string> argv;
    argv.reserve(16 + 2 * batch.size());
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
    if (!cfg.channelTypesJson.empty()) {
      argv.push_back("--channel-types-json");
      argv.push_back(cfg.channelTypesJson);
    }
    if (!cfg.orgConfig.empty()) {
      argv.push_back("--org-config");
      argv.push_back(cfg.orgConfig);
    }
    for (const auto &f : batch) {
      argv.push_back("--source");
      argv.push_back(f);
    }

    return runWorkerProcess(argv, stderrPath, limits, "megascope");
  };
}

} // namespace

std::error_code createWorkerShardDir(llvm::StringRef base,
                                     llvm::SmallVectorImpl<char> &out) {
  llvm::SmallString<128> prefix;
  llvm::sys::path::system_temp_directory(/*ErasedOnReboot=*/true, prefix);
  llvm::sys::path::append(prefix, base);
  // Creates <prefix>-XXXXXX.
  if (auto ec = llvm::sys::fs::createUniqueDirectory(prefix, out)) {
    out.assign(prefix.begin(), prefix.end());
    return ec;
  }
  return {};
}

void removeWorkerShardDir(llvm::StringRef dir) {
  llvm::sys::fs::remove_directories(dir, /*IgnoreErrors=*/true);
}

BakedIndexes bakeIsolated(const std::string &selfExe, const McpBakeConfig &cfg,
                          const std::vector<std::string> &files,
                          unsigned workers, BuildStats *stats,
                          const WorkerLimits &limits) {
  llvm::SmallString<128> shardDir;
  if (auto ec = createWorkerShardDir("vycor-workers", shardDir)) {
    llvm::errs() << "megascope: ERROR: cannot create worker shard directory "
                    "under "
                 << shardDir << ": " << ec.message()
                 << " — isolated bake aborted (indexes will be empty)\n";
    return {};
  }
  InterruptCleanup cleanup{std::string(shardDir)};

  auto out = bakeIsolatedWithRunner(makeSubprocessRunner(selfExe, cfg, limits),
                                    files, workers, stats,
                                    std::string(shardDir), &cfg);
  removeWorkerShardDir(shardDir);
  return out;
}

BakedIndexes bakeTUIsolated(const std::string &selfExe,
                            const McpBakeConfig &cfg, const std::string &file,
                            const WorkerLimits &limits) {
  auto out = bakeIsolated(selfExe, cfg, {file}, 1, nullptr, limits);
  // A shard directory that could not be created leaves no outcome at all.
  if (!out.outcomes.count(file))
    out.outcomes[file] = TuOutcome{TuStatus::Skipped, "worker not run"};
  return out;
}

} // namespace vycor
