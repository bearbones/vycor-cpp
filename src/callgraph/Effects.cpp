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
#include "vycor/callgraph/Effects.h"

#include <algorithm>

namespace vycor {

llvm::StringRef effectKindName(EffectKind kind) {
  switch (kind) {
  case EffectKind::Allocation:
    return "allocation";
  case EffectKind::Lock:
    return "lock";
  case EffectKind::Io:
    return "io";
  }
  return "allocation";
}

std::optional<EffectKind> effectKindFromName(llvm::StringRef name) {
  for (EffectKind kind : kEffectKinds)
    if (name == effectKindName(kind))
      return kind;
  return std::nullopt;
}

namespace {

bool isIdentChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_';
}

// Offset of the `operator` keyword that starts the last name component, or
// npos. Everything from there on is an operator spelling whose `<` and `>`
// are not template brackets.
size_t operatorStart(llvm::StringRef name) {
  size_t pos = name.rfind("operator");
  while (pos != llvm::StringRef::npos) {
    bool startsComponent = pos == 0 || name[pos - 1] == ':';
    size_t after = pos + 8;
    bool endsKeyword = after >= name.size() || !isIdentChar(name[after]);
    if (startsComponent && endsKeyword)
      return pos;
    if (pos == 0)
      break;
    pos = name.substr(0, pos + 7).rfind("operator");
  }
  return llvm::StringRef::npos;
}

} // namespace

std::string effectLookupName(llvm::StringRef displayName) {
  size_t opStart = operatorStart(displayName);
  llvm::StringRef scoped = displayName.substr(0, opStart);
  llvm::StringRef op = opStart == llvm::StringRef::npos
                           ? llvm::StringRef()
                           : displayName.substr(opStart);

  // Template arguments out, tracking bracket depth.
  std::string flat;
  flat.reserve(scoped.size());
  unsigned depth = 0;
  for (char c : scoped) {
    if (c == '<') {
      ++depth;
    } else if (c == '>') {
      if (depth > 0)
        --depth;
    } else if (depth == 0) {
      flat.push_back(c);
    }
  }

  // Implementation namespaces out: a `__name::` component that is not the
  // last one.
  std::string out;
  out.reserve(flat.size() + op.size());
  llvm::StringRef rest(flat);
  while (!rest.empty()) {
    size_t sep = rest.find("::");
    if (sep == llvm::StringRef::npos) {
      out.append(rest.data(), rest.size());
      break;
    }
    llvm::StringRef component = rest.substr(0, sep);
    if (!component.starts_with("__")) {
      out.append(component.data(), component.size());
      out.append("::");
    }
    rest = rest.substr(sep + 2);
  }
  out.append(op.data(), op.size());
  return out;
}

bool isPlacementNewUsr(llvm::StringRef usr) {
  // "c:@F@operator new#<size>#<param>#...": each parameter type is
  // followed by '#'.
  if (!usr.consume_front("c:@F@operator new"))
    return false;
  usr.consume_front("[]");
  if (!usr.consume_front("#"))
    return false;
  size_t sizeEnd = usr.find('#');
  if (sizeEnd == llvm::StringRef::npos)
    return false;
  usr = usr.substr(sizeEnd + 1);
  while (!usr.empty()) {
    size_t end = usr.find('#');
    llvm::StringRef param = usr.substr(0, end);
    if (param != "$@N@std@E@align_val_t" && param != "&1$@N@std@S@nothrow_t")
      return true;
    if (end == llvm::StringRef::npos)
      break;
    usr = usr.substr(end + 1);
  }
  return false;
}

void EffectTable::add(EffectKind kind, llvm::StringRef name) {
  std::string key = effectLookupName(name);
  if (key.empty())
    return;
  byName_[key] |= effectBit(kind);
}

uint8_t EffectTable::effectsOf(llvm::StringRef displayName) const {
  if (byName_.empty())
    return 0;
  auto it = byName_.find(effectLookupName(displayName));
  return it == byName_.end() ? 0 : it->second;
}

uint8_t EffectTable::effectsOf(llvm::StringRef displayName,
                               llvm::StringRef usr) const {
  uint8_t mask = effectsOf(displayName);
  if ((mask & effectBit(EffectKind::Allocation)) && isPlacementNewUsr(usr))
    mask &= static_cast<uint8_t>(~effectBit(EffectKind::Allocation));
  return mask;
}

std::vector<std::string> EffectTable::names(EffectKind kind) const {
  std::vector<std::string> out;
  for (const auto &entry : byName_)
    if (entry.second & effectBit(kind))
      out.push_back(entry.first().str());
  std::sort(out.begin(), out.end());
  return out;
}

EffectTable EffectTable::builtin() {
  // Entries are the functions whose effect does not depend on their
  // arguments. A container operation that allocates only past its
  // capacity is not listed; the allocation it performs is.
  static const char *const kAllocation[] = {
      "operator new",
      "operator new[]",
      "malloc",
      "calloc",
      "realloc",
      "reallocarray",
      "aligned_alloc",
      "posix_memalign",
      "memalign",
      "valloc",
      "strdup",
      "strndup",
      "std::malloc",
      "std::calloc",
      "std::realloc",
      "std::aligned_alloc",
      "std::make_unique",
      "std::make_unique_for_overwrite",
      "std::make_shared",
      "std::make_shared_for_overwrite",
      "std::allocate_shared",
      "std::allocator::allocate",
  };
  static const char *const kLock[] = {
      "pthread_mutex_lock",
      "pthread_mutex_timedlock",
      "pthread_rwlock_rdlock",
      "pthread_rwlock_wrlock",
      "pthread_spin_lock",
      "mtx_lock",
      "mtx_timedlock",
      "std::mutex::lock",
      "std::recursive_mutex::lock",
      "std::timed_mutex::lock",
      "std::recursive_timed_mutex::lock",
      "std::shared_mutex::lock",
      "std::shared_mutex::lock_shared",
      "std::shared_timed_mutex::lock",
      "std::shared_timed_mutex::lock_shared",
      "std::lock_guard::lock_guard",
      "std::unique_lock::unique_lock",
      "std::unique_lock::lock",
      "std::scoped_lock::scoped_lock",
      "std::shared_lock::shared_lock",
      "std::shared_lock::lock",
      "std::lock",
      "std::call_once",
  };
  static const char *const kIo[] = {
      // C stdio
      "fopen", "freopen", "fdopen", "fclose", "fread", "fwrite", "fflush",
      "fgets", "fputs", "fgetc", "fputc", "getc", "putc", "getchar",
      "putchar", "puts", "printf", "fprintf", "vprintf", "vfprintf", "scanf",
      "fscanf", "getline", "perror", "remove", "rename",
      "std::fopen", "std::fclose", "std::fread", "std::fwrite", "std::fflush",
      "std::fgets", "std::fputs", "std::puts", "std::printf", "std::fprintf",
      // POSIX descriptors, files and sockets
      "open", "openat", "creat", "close", "read", "write", "pread", "pwrite",
      "readv", "writev", "fsync", "fdatasync", "stat", "fstat", "lstat",
      "access", "unlink", "mkdir", "opendir", "readdir", "mmap", "socket",
      "connect", "accept", "send", "recv", "sendto", "recvfrom", "sendmsg",
      "recvmsg",
      // iostream files
      "std::basic_ifstream::basic_ifstream",
      "std::basic_ofstream::basic_ofstream",
      "std::basic_fstream::basic_fstream",
      "std::basic_ifstream::open",
      "std::basic_ofstream::open",
      "std::basic_fstream::open",
      "std::basic_filebuf::open",
  };
  EffectTable table;
  for (const char *name : kAllocation)
    table.add(EffectKind::Allocation, name);
  for (const char *name : kLock)
    table.add(EffectKind::Lock, name);
  for (const char *name : kIo)
    table.add(EffectKind::Io, name);
  return table;
}

} // namespace vycor
