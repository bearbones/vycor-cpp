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

#include "vycor/callgraph/AtomicFile.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

#include <cerrno>
#include <cstring>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#else
#include "llvm/Support/Process.h"
#endif

namespace vycor {

namespace {

void setError(std::string *error, const std::string &msg) {
  if (error)
    *error = msg;
}

std::string parentDir(const std::string &path) {
  llvm::StringRef parent = llvm::sys::path::parent_path(path);
  return parent.empty() ? std::string(".") : parent.str();
}

/// Make a finished rename durable: fsync the directory holding the entry.
/// Best effort (not every filesystem supports it); a failure here cannot
/// tear the file, only lose the rename on power loss.
void syncDirectory(const std::string &dir) {
#ifndef _WIN32
  int fd = ::open(dir.c_str(), O_RDONLY);
  if (fd < 0)
    return;
  (void)::fsync(fd);
  ::close(fd);
#else
  (void)dir;
#endif
}

} // namespace

std::string atomicTempPrefix(const std::string &path) {
  return path + ".tmp-";
}

bool writeFileAtomically(const std::string &path,
                         llvm::function_ref<void(llvm::raw_ostream &)> body,
                         std::string *error, bool durable) {
  const std::string dir = parentDir(path);
  if (std::error_code ec = llvm::sys::fs::create_directories(dir)) {
    setError(error, "cannot create " + dir + ": " + ec.message());
    return false;
  }
  int fd = -1;
  llvm::SmallString<256> tmpPath;
  if (std::error_code ec = llvm::sys::fs::createUniqueFile(
          atomicTempPrefix(path) + "%%%%%%", fd, tmpPath)) {
    setError(error, "cannot create a temp file next to " + path + ": " +
                        ec.message());
    return false;
  }
  const std::string tmp(tmpPath.str());
  auto fail = [&](const std::string &msg) {
    llvm::sys::fs::remove(tmp);
    setError(error, msg);
    return false;
  };

  {
    llvm::raw_fd_ostream os(fd, /*shouldClose=*/true);
    body(os);
    os.flush();
    if (os.has_error()) {
      std::string msg = "cannot write " + tmp + ": " + os.error().message();
      // An uncleared stream error is a fatal error in the destructor.
      os.clear_error();
      return fail(msg);
    }
#ifndef _WIN32
    if (durable && ::fsync(fd) != 0) {
      std::string msg = "cannot sync " + tmp + ": " + std::strerror(errno);
      return fail(msg);
    }
#endif
    os.close();
    if (os.has_error()) {
      std::string msg = "cannot close " + tmp + ": " + os.error().message();
      os.clear_error();
      return fail(msg);
    }
  }

  if (std::error_code ec = llvm::sys::fs::rename(tmp, path))
    return fail("cannot rename " + tmp + " to " + path + ": " + ec.message());
  if (durable)
    syncDirectory(dir);
  return true;
}

size_t removeStaleAtomicTemps(const std::string &path) {
  const std::string prefix = atomicTempPrefix(path);
  const llvm::StringRef prefixName = llvm::sys::path::filename(prefix);
  size_t removed = 0;
  std::error_code ec;
  std::vector<std::string> stale;
  for (llvm::sys::fs::directory_iterator it(parentDir(path), ec), end;
       !ec && it != end; it.increment(ec)) {
    llvm::StringRef name = llvm::sys::path::filename(it->path());
    // `<name>.tmp-` plus the six characters createUniqueFile filled in.
    if (name.starts_with(prefixName) &&
        name.size() == prefixName.size() + 6)
      stale.push_back(it->path());
  }
  for (const auto &p : stale)
    if (!llvm::sys::fs::remove(p))
      ++removed;
  return removed;
}

// ----------------------------------------------------------------------------
// IndexWriteLock
// ----------------------------------------------------------------------------

IndexWriteLock::~IndexWriteLock() {
  if (fd_ < 0)
    return;
#ifndef _WIN32
  ::flock(fd_, LOCK_UN);
  ::close(fd_);
#else
  llvm::sys::fs::unlockFile(fd_);
  llvm::sys::Process::SafelyCloseFileDescriptor(fd_);
#endif
}

std::unique_ptr<IndexWriteLock> IndexWriteLock::acquire(
    const std::string &path, bool wait, std::string *error, bool *busy,
    const std::function<void(const std::string &lockPath)> &onWait) {
  if (busy)
    *busy = false;
  std::unique_ptr<IndexWriteLock> lock(new IndexWriteLock());
  lock->lockPath_ = path + ".lock";
  const std::string dir = parentDir(path);
  if (std::error_code ec = llvm::sys::fs::create_directories(dir)) {
    setError(error, "cannot create " + dir + ": " + ec.message());
    return nullptr;
  }
  if (std::error_code ec = llvm::sys::fs::openFileForReadWrite(
          lock->lockPath_, lock->fd_, llvm::sys::fs::CD_OpenAlways,
          llvm::sys::fs::OF_None)) {
    // A lock file another user created (0644 under a usual umask, in a
    // shared build directory) cannot be opened for writing, but flock
    // needs no write access: lock it through a read-only descriptor.
    // Only a lock file that cannot be opened at all is an error.
    lock->fd_ = -1;
    if (llvm::sys::fs::openFileForRead(lock->lockPath_, lock->fd_)) {
      lock->fd_ = -1;
      setError(error, "cannot open " + lock->lockPath_ + ": " + ec.message());
      return nullptr;
    }
  }
#ifndef _WIN32
  if (::flock(lock->fd_, LOCK_EX | LOCK_NB) == 0)
    return lock;
  if (errno != EWOULDBLOCK) {
    setError(error, "cannot lock " + lock->lockPath_ + ": " +
                        std::strerror(errno));
    return nullptr;
  }
  if (!wait) {
    setError(error, lock->lockPath_ + " is held by another writer");
    if (busy)
      *busy = true;
    return nullptr;
  }
  if (onWait)
    onWait(lock->lockPath_);
  int rc;
  do
    rc = ::flock(lock->fd_, LOCK_EX);
  while (rc != 0 && errno == EINTR);
  if (rc != 0) {
    setError(error, "cannot lock " + lock->lockPath_ + ": " +
                        std::strerror(errno));
    return nullptr;
  }
  return lock;
#else
  if (!llvm::sys::fs::tryLockFile(lock->fd_))
    return lock;
  if (!wait) {
    setError(error, lock->lockPath_ + " is held by another writer");
    if (busy)
      *busy = true;
    return nullptr;
  }
  if (onWait)
    onWait(lock->lockPath_);
  if (std::error_code ec = llvm::sys::fs::lockFile(lock->fd_)) {
    setError(error, "cannot lock " + lock->lockPath_ + ": " + ec.message());
    return nullptr;
  }
  return lock;
#endif
}

} // namespace vycor
