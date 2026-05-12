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

#include "vycor/compat/PchCache.h"

#include "clang/Tooling/CompilationDatabase.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <functional>
#include <set>

namespace vycor {

namespace {

/// Apply the same flag adjustments to a compile command that ToolAdjusters
/// would apply to a ClangTool invocation. This ensures the shell-invoked
/// clang++ sees the same flags as ClangTool's cc1 pipeline.
static std::vector<std::string>
applyToolAdjusters(const std::vector<std::string> &args) {
  // Simulate getStripIncompatibleFlagsAdjuster (stripPch=true for PCH
  // source flags — we're compiling the PCH, not using it).
  static const char *const stripFlags[] = {
      "-gmodules", "-fmodules", "-fcxx-modules", "-Werror",
  };
  static const char *const stripWithNext[] = {
      "-fmodule-file", "-fmodules-cache-path", "-include-pch",
  };

  std::vector<std::string> result;
  for (size_t i = 0; i < args.size(); ++i) {
    bool skip = false;
    for (const auto *f : stripFlags) {
      if (args[i] == f) {
        skip = true;
        break;
      }
    }
    if (!skip) {
      for (const auto *f : stripWithNext) {
        if (args[i] == f) {
          skip = true;
          ++i;
          break;
        }
      }
    }
    // Strip PCH source includes.
    if (!skip && (args[i] == "-Xarch_arm64" || args[i] == "-Xarch_x86_64")) {
      if (i + 1 < args.size() && args[i + 1].find("-include") == 0 &&
          args[i + 1].find("pch") != std::string::npos) {
        skip = true;
        ++i;
      }
    }
    if (!skip && args[i].find("-include") == 0 &&
        args[i].find("pch") != std::string::npos) {
      skip = true;
      if (args[i] == "-include")
        ++i;
    }
    if (!skip && args[i] == "-Winvalid-pch")
      skip = true;
    if (!skip)
      result.push_back(args[i]);
  }

  // Simulate getResourceDirAdjuster — inject -resource-dir if not already set.
  bool hasResourceDir = false;
  for (const auto &a : result) {
    if (a.find("-resource-dir") == 0) {
      hasResourceDir = true;
      break;
    }
  }
#ifdef VYCOR_CLANG_RESOURCE_DIR
  if (!hasResourceDir) {
    result.push_back("-resource-dir");
    result.push_back(VYCOR_CLANG_RESOURCE_DIR);
  }
#endif

  return result;
}

} // anonymous namespace

PchCache::PchCache(std::string cacheDir, std::string clangBin)
    : cacheDir_(std::move(cacheDir)), clangBin_(std::move(clangBin)) {}

/// Check if an argument is a PCH source include (not a compiled -include-pch).
/// Matches: -include<path_with_pch>, -include <path_with_pch>
/// Also matches -Xarch_* -include<pch> pairs.
static bool isPchSourceInclude(const std::vector<std::string> &args,
                               size_t idx, std::string &outPchPath,
                               size_t &outSkip) {
  llvm::StringRef arg(args[idx]);
  outSkip = 0;

  // Skip -Xarch_* prefix if present.
  if (arg.starts_with("-Xarch_") && idx + 1 < args.size()) {
    arg = llvm::StringRef(args[idx + 1]);
    outSkip = 1;
  }

  // -include-pch is a compiled PCH — not what we're looking for.
  if (arg.starts_with("-include-pch"))
    return false;

  // Joined form: -include/path/to/pch.hxx
  if (arg.starts_with("-include") && arg.size() > 8) {
    llvm::StringRef path = arg.substr(8);
    if (path.contains("pch")) {
      outPchPath = path.str();
      return true;
    }
    return false;
  }

  // Separate form: -include <path>
  if (arg == "-include" && idx + outSkip + 1 < args.size()) {
    llvm::StringRef path(args[idx + outSkip + 1]);
    if (path.contains("pch")) {
      outPchPath = path.str();
      outSkip += 1;
      return true;
    }
  }

  return false;
}

void PchCache::buildFromCompileCommands(
    const clang::tooling::CompilationDatabase &compDb,
    const std::vector<std::string> &files) {
  // Map: PCH source header → first TU's args (for cache key computation).
  std::unordered_map<std::string, std::vector<std::string>> pchToFlags;

  for (const auto &file : files) {
    auto cmds = compDb.getCompileCommands(file);
    if (cmds.empty())
      continue;

    auto &cmd = cmds[0];
    std::vector<std::string> args;
    // cmd.CommandLine includes the compiler as first element.
    for (size_t i = 1; i < cmd.CommandLine.size(); ++i)
      args.push_back(cmd.CommandLine[i]);

    // Find PCH source includes in this TU's flags.
    for (size_t i = 0; i < args.size(); ++i) {
      std::string pchPath;
      size_t skip = 0;
      if (isPchSourceInclude(args, i, pchPath, skip)) {
        if (pchToFlags.find(pchPath) == pchToFlags.end()) {
          pchToFlags[pchPath] = args;
        }
        i += skip;
      }
    }
  }

  if (pchToFlags.empty())
    return;

  // Create cache directory.
  llvm::sys::fs::create_directories(cacheDir_);

  llvm::errs() << "pch-cache: found " << pchToFlags.size()
               << " unique PCH headers to compile\n";

  for (auto &[pchSrc, tuCmdArgs] : pchToFlags) {
    auto key = computeCacheKey(pchSrc, tuCmdArgs);
    std::string outPath = cacheDir_ + "/" + key + ".pch";

    // Check if already compiled.
    if (llvm::sys::fs::exists(outPath)) {
      llvm::errs() << "pch-cache: cached " << pchSrc << "\n";
      cache_[pchSrc] = outPath;
      continue;
    }

    // Apply the same flag adjustments that ToolAdjusters apply to
    // ClangTool invocations. This ensures the external clang++ sees
    // the same resource-dir and stripped flags, producing a PCH
    // compatible with the ClangTool's TU parsing path.
    auto adjustedFlags = applyToolAdjusters(tuCmdArgs);
    auto pchFlags = extractPchCompileFlags(adjustedFlags);

    // Construct: clang++ -x c++-header <flags> <pch_src> -Xclang -emit-pch -o <out>
    std::string cmd = clangBin_;
    cmd += " -x c++-header -Wno-error";
    for (const auto &f : pchFlags) {
      cmd += " '";
      cmd += f;
      cmd += "'";
    }
    cmd += " '";
    cmd += pchSrc;
    cmd += "' -Xclang -emit-pch -o '";
    cmd += outPath;
    cmd += "' 2>&1";

    llvm::errs() << "pch-cache: compiling " << pchSrc << " ...\n";
    int rc = std::system(cmd.c_str());

    if (rc == 0 && llvm::sys::fs::exists(outPath)) {
      uint64_t size;
      llvm::sys::fs::file_size(outPath, size);
      llvm::errs() << "pch-cache: ok (" << size / 1024 / 1024 << " MB)\n";
      cache_[pchSrc] = outPath;
    } else {
      llvm::errs() << "pch-cache: FAILED (exit " << rc << ")\n";
      llvm::sys::fs::remove(outPath);
    }
  }

  llvm::errs() << "pch-cache: " << cache_.size() << " of "
               << pchToFlags.size() << " PCH files compiled successfully\n";
}

std::string
PchCache::getCompiledPch(const std::string &pchSourceHeader) const {
  auto it = cache_.find(pchSourceHeader);
  if (it != cache_.end())
    return it->second;
  return {};
}

std::vector<std::string> PchCache::extractPchCompileFlags(
    const std::vector<std::string> &tuArgs) {
  std::vector<std::string> flags;

  // Keep flags relevant to header compilation, skip source files and
  // output flags.
  static const std::set<std::string> skipFlags = {
      "-c", "-o", "-MT", "-MF", "-MQ", "-MD", "-MMD", "-MP",
  };
  static const std::set<std::string> skipWithNext = {
      "-o", "-MT", "-MF", "-MQ",
  };

  for (size_t i = 0; i < tuArgs.size(); ++i) {
    llvm::StringRef arg(tuArgs[i]);

    // Skip source files and object files.
    if (arg.ends_with(".cpp") || arg.ends_with(".cc") ||
        arg.ends_with(".c") || arg.ends_with(".cxx") ||
        arg.ends_with(".o") || arg.ends_with(".obj")) {
      if (!arg.starts_with("-"))
        continue;
    }

    // Skip dependency file flags.
    if (skipFlags.count(arg.str())) {
      continue;
    }
    if (skipWithNext.count(arg.str())) {
      ++i; // skip next arg too
      continue;
    }

    // Skip existing PCH includes (we're compiling the PCH itself).
    if (arg.starts_with("-include") && arg.contains("pch")) {
      if (arg == "-include")
        ++i; // skip separate path arg
      continue;
    }
    // Skip -Xarch_* -include*pch* pairs.
    if (arg.starts_with("-Xarch_") && i + 1 < tuArgs.size()) {
      llvm::StringRef next(tuArgs[i + 1]);
      if (next.starts_with("-include") && next.contains("pch")) {
        ++i;
        continue;
      }
    }

    // Skip -Winvalid-pch (not useful for PCH compilation).
    if (arg == "-Winvalid-pch")
      continue;

    flags.push_back(tuArgs[i]);
  }

  return flags;
}

std::string
PchCache::computeCacheKey(const std::string &pchPath,
                          const std::vector<std::string> &flags) {
  // Simple hash of the PCH path and flags for cache naming.
  std::hash<std::string> hasher;
  size_t h = hasher(pchPath);
  for (const auto &f : flags)
    h ^= hasher(f) + 0x9e3779b9 + (h << 6) + (h >> 2);

  // Use the PCH filename stem + hash for readability.
  llvm::StringRef stem = llvm::sys::path::stem(pchPath);
  return stem.str() + "_" + std::to_string(h);
}

} // namespace vycor
