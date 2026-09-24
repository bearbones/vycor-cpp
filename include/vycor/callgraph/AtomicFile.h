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

#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/Support/raw_ostream.h"

#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace vycor {

// ============================================================================
// Crash- and race-safe file publication, shared by the index save
// (SnapshotIO::save), the anneal checkpoint journal, and the worker shard
// and handoff files.
// ============================================================================

/// Write a file through `body` and publish it at `path` atomically: the
/// bytes go to a uniquely named temp file in the target directory
/// (`<name>.tmp-XXXXXX`, so concurrent writers never share one), which is
/// flushed and fsync'ed, renamed over `path`, and the directory fsync'ed.
/// A reader sees the previous file or the new one, never a mixture, and a
/// crash leaves at most a stray temp file. The parent directory is
/// created when missing. On any failure (the stream reported a write
/// error, fsync or rename failed) the temp file is removed, `path` is left
/// as it was, the stream error is cleared, `error` (when given) says what
/// failed, and false is returned.
bool writeFileAtomically(const std::string &path,
                         llvm::function_ref<void(llvm::raw_ostream &)> body,
                         std::string *error = nullptr);

/// The temp-file prefix writeFileAtomically uses for `path`
/// (`<path>.tmp-`): a writer holding the path's IndexWriteLock may remove
/// leftovers from killed writers by this prefix.
std::string atomicTempPrefix(const std::string &path);

/// Remove leftover writeFileAtomically temp files of `path` (from writers
/// killed mid-write). Only safe while holding the path's IndexWriteLock.
/// Returns how many were removed.
size_t removeStaleAtomicTemps(const std::string &path);

/// Advisory exclusive lock on `<path>.lock`, held for a whole
/// load → bake → save sequence so two writers of one index serialize
/// instead of racing. Readers never lock (the rename is atomic for them).
/// POSIX flock(2) on the lock file (released by the kernel when the
/// process dies); the lock file itself is left in place.
class IndexWriteLock {
public:
  ~IndexWriteLock();
  IndexWriteLock(const IndexWriteLock &) = delete;
  IndexWriteLock &operator=(const IndexWriteLock &) = delete;

  /// Lock `<path>.lock`, creating it (and its directory) when missing.
  /// When another process holds it: with `wait`, call `onWait` once and
  /// block until it is released; without, fail at once with `busy` set.
  /// Returns null on failure, with `error` saying why; `busy` is false
  /// when the lock file itself could not be created or locked (a
  /// read-only index directory), which callers may treat as "no other
  /// writer can exist either".
  static std::unique_ptr<IndexWriteLock>
  acquire(const std::string &path, bool wait, std::string *error,
          bool *busy = nullptr,
          const std::function<void(const std::string &lockPath)> &onWait =
              nullptr);

  const std::string &lockPath() const { return lockPath_; }

private:
  IndexWriteLock() = default;
  std::string lockPath_;
  int fd_ = -1;
};

} // namespace vycor
