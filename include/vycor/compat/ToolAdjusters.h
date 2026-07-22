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

#ifndef VYCOR_COMPAT_TOOL_ADJUSTERS_H
#define VYCOR_COMPAT_TOOL_ADJUSTERS_H

#include "vycor/compat/PchCache.h"

#include "clang/Tooling/Tooling.h"
#include "clang/Tooling/ArgumentsAdjusters.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

namespace vycor {

/// Process-global extra compiler arguments appended to the end of every
/// compile command by makeClangTool(). Seeded from the VYCOR_EXTRA_ARGS
/// environment variable (whitespace-separated) so test binaries inherit
/// host-toolchain fixes too, then extended by the CLI (--extra-arg). These
/// describe the machine/toolchain the tool runs on (e.g.
/// --gcc-install-dir=... on hosts whose newest /usr/lib/gcc directory lacks
/// matching libstdc++ headers), so they are genuinely process-wide. Not
/// thread-safe to mutate after worker threads start; set before any build
/// begins.
inline std::vector<std::string> &globalExtraArgs() {
  static std::vector<std::string> args = [] {
    std::vector<std::string> seed;
    if (const char *env = std::getenv("VYCOR_EXTRA_ARGS")) {
      std::string cur;
      for (const char *p = env;; ++p) {
        if (*p == '\0' || *p == ' ' || *p == '\t') {
          if (!cur.empty()) {
            seed.push_back(cur);
            cur.clear();
          }
          if (*p == '\0')
            break;
        } else {
          cur.push_back(*p);
        }
      }
    }
    return seed;
  }();
  return args;
}

/// Append CLI-provided extra args to the env-seeded set.
inline void appendGlobalExtraArgs(const std::vector<std::string> &args) {
  auto &g = globalExtraArgs();
  g.insert(g.end(), args.begin(), args.end());
}

/// Strip compiler flags that are incompatible with our LibTooling build.
/// This lets us consume compilation databases produced by toolchains whose
/// Clang version or configuration differs from ours (e.g. hermetic Xcode
/// builds that use -gmodules).
/// When stripPch is false, PCH-related flags are preserved (use when a
/// PchCache adjuster will handle them instead).
inline clang::tooling::ArgumentsAdjuster
getStripIncompatibleFlagsAdjuster(bool stripPch = true) {
  return [stripPch](const clang::tooling::CommandLineArguments &args,
                    llvm::StringRef /*filename*/) {
    // Flags to remove outright.
    static const char *const StripFlags[] = {
        "-gmodules",
        "-fmodules",
        "-fcxx-modules",
        "-Werror",
    };
    // Flags whose *next* argument should also be removed.
    // -include-pch is only stripped when stripPch is true.
    static const char *const StripWithNext[] = {
        "-fmodule-file",
        "-fmodules-cache-path",
    };

    clang::tooling::CommandLineArguments filtered;
    filtered.reserve(args.size());
    bool skipNext = false;
    for (size_t i = 0; i < args.size(); ++i) {
      if (skipNext) {
        skipNext = false;
        continue;
      }
      bool skip = false;
      for (const auto *f : StripFlags) {
        if (args[i] == f) {
          skip = true;
          break;
        }
      }
      if (!skip) {
        for (const auto *f : StripWithNext) {
          if (args[i] == f) {
            skip = true;
            ++i; // skip next arg too
            break;
          }
        }
      }
      // Strip -include-pch (compiled PCH from build system).
      if (!skip && stripPch && args[i] == "-include-pch") {
        skip = true;
        ++i; // skip next arg too
      }
      // Strip -include args that reference PCH files (CMake-style PCH).
      // These use "-include/path/to/pch" (joined) or "-include /path/to/pch".
      if (stripPch) {
        if (!skip && (args[i] == "-Xarch_arm64" || args[i] == "-Xarch_x86_64")) {
          // Check if next arg is a PCH include.
          if (i + 1 < args.size() &&
              args[i + 1].find("-include") == 0 &&
              args[i + 1].find("pch") != std::string::npos) {
            skip = true;
            ++i; // skip the -include arg too
          }
        }
        if (!skip && args[i].find("-include") == 0 &&
            args[i].find("pch") != std::string::npos) {
          skip = true;
          // If it's just "-include" (separate), skip next arg too.
          if (args[i] == "-include")
            ++i;
        }
      }
      if (!skip)
        filtered.push_back(args[i]);
    }
    return filtered;
  };
}

/// Inject -isysroot and libc++ include paths on macOS when the compilation
/// database doesn't already provide them. If sysrootOverride is non-empty it
/// takes precedence; otherwise falls back to the build-time default detected
/// by CMake via xcrun.
inline clang::tooling::ArgumentsAdjuster
getSysrootAdjuster(const std::string &sysrootOverride = "") {
  return [sysrootOverride](const clang::tooling::CommandLineArguments &args,
                           llvm::StringRef /*filename*/) {
    for (const auto &a : args)
      if (a == "-isysroot")
        return args;

    const std::string sysroot = !sysrootOverride.empty() ? sysrootOverride
#ifdef VYCOR_DEFAULT_SYSROOT
        : std::string(VYCOR_DEFAULT_SYSROOT);
#else
        : std::string();
#endif
    if (sysroot.empty())
      return args;

    auto result = args;
    result.push_back("-isysroot");
    result.push_back(sysroot);
    return result;
  };
}

/// Inject -resource-dir pointing to this tool's Clang resource directory.
/// This ensures the tool's built-in headers (stdarg.h, etc.) are found even
/// when consuming compilation databases from a different toolchain.
inline clang::tooling::ArgumentsAdjuster getResourceDirAdjuster() {
  return [](const clang::tooling::CommandLineArguments &args,
            llvm::StringRef /*filename*/) {
    // Check if -resource-dir is already set.
    for (const auto &a : args)
      if (a.find("-resource-dir") == 0)
        return args;
#ifdef VYCOR_CLANG_RESOURCE_DIR
    auto result = args;
    // Use space-separated form: -resource-dir= (Joined) lacks CC1Option
    // visibility in LLVM 21, so ClangTool's internal cc1 pipeline rejects it.
    // The Separate form has CC1Option and works in both driver and cc1 modes.
    result.push_back("-resource-dir");
    result.push_back(VYCOR_CLANG_RESOURCE_DIR);
    return result;
#else
    return args;
#endif
  };
}

/// Linux hosts sometimes carry a /usr/lib/gcc/<triple>/<ver> directory for a
/// GCC major with no matching libstdc++ headers (e.g. libgcc-14-dev installs
/// /usr/lib/gcc/x86_64-linux-gnu/14 with only crt objects, while
/// libstdc++-XX-dev headers exist only for 13). Clang selects the
/// numerically newest candidate GCC installation, so every parse dies with
/// "fatal error: 'memory' file not found" even though a complete older
/// toolchain is right there. Returns the newest GCC install dir that HAS
/// matching libstdc++ headers when (and only when) clang's default pick
/// would be broken; "" means nothing needs fixing. Probed once per process.
inline const std::string &detectUsableGccInstallDir() {
  static const std::string result = []() -> std::string {
#ifndef __linux__
    return "";
#else
    namespace fs = llvm::sys::fs;
    namespace path = llvm::sys::path;

    // "14" or "14.2.1" -> {14,2,1}; empty when non-numeric (not a GCC
    // version directory).
    auto parseVer = [](llvm::StringRef name) {
      llvm::SmallVector<int, 4> parts;
      llvm::SmallVector<llvm::StringRef, 4> pieces;
      name.split(pieces, '.');
      for (llvm::StringRef p : pieces) {
        int v = 0;
        if (p.empty() || p.getAsInteger(10, v))
          return llvm::SmallVector<int, 4>{};
        parts.push_back(v);
      }
      return parts;
    };
    // Debian/Ubuntu name both the GCC dir and the libstdc++ include dir by
    // major ("13"); Arch-style layouts use the full version for both.
    auto hasLibstdcxx = [](llvm::StringRef verName, int major) {
      return fs::is_directory("/usr/include/c++/" + verName.str()) ||
             fs::is_directory("/usr/include/c++/" + std::to_string(major));
    };

    llvm::SmallVector<int, 4> bestVer, bestUsableVer;
    std::string bestUsableDir;
    bool bestIsUsable = false;

    std::error_code ec;
    for (fs::directory_iterator triple("/usr/lib/gcc", ec), tripleEnd;
         !ec && triple != tripleEnd; triple.increment(ec)) {
      for (fs::directory_iterator ver(triple->path(), ec), verEnd;
           !ec && ver != verEnd; ver.increment(ec)) {
        auto verName = path::filename(ver->path());
        auto parsed = parseVer(verName);
        if (parsed.empty() || !fs::is_directory(ver->path()))
          continue;
        bool usable = hasLibstdcxx(verName, parsed[0]);
        if (bestVer.empty() || bestVer < parsed) {
          bestVer = parsed;
          bestIsUsable = usable;
        } else if (bestVer == parsed) {
          bestIsUsable = bestIsUsable || usable;
        }
        if (usable && (bestUsableVer.empty() || bestUsableVer < parsed)) {
          bestUsableVer = parsed;
          bestUsableDir = ver->path();
        }
      }
    }

    // Clang's pick (the newest) works, or no complete install exists to
    // point at instead: stay out of the way.
    if (bestIsUsable || bestUsableDir.empty())
      return "";
    return bestUsableDir;
#endif
  }();
  return result;
}

/// Inject --gcc-install-dir steering clang away from a headerless GCC dir
/// (see detectUsableGccInstallDir). No-op unless the host has the mismatch,
/// and defers to any explicit toolchain/stdlib choice already in the args —
/// including ones appended via --extra-arg / VYCOR_EXTRA_ARGS, so this
/// adjuster must run AFTER the extra-args insertion in makeClangTool.
inline clang::tooling::ArgumentsAdjuster getGccInstallDirAdjuster() {
  return [](const clang::tooling::CommandLineArguments &args,
            llvm::StringRef /*filename*/) {
    const std::string &fix = detectUsableGccInstallDir();
    if (fix.empty())
      return args;
    for (const auto &a : args) {
      llvm::StringRef arg(a);
      if (arg.starts_with("--gcc-install-dir") ||
          arg.starts_with("--gcc-toolchain") ||
          arg.starts_with("--sysroot") || arg == "-isysroot" ||
          arg.starts_with("-stdlib") || arg == "-nostdinc" ||
          arg == "-nostdinc++")
        return args;
    }
    auto result = args;
    result.push_back("--gcc-install-dir=" + fix);
    return result;
  };
}

/// Argument adjuster that replaces PCH source includes with compiled PCH
/// binaries from a PchCache. The PCH source `-include <pch_src>` is replaced
/// with `-include-pch <compiled.pch>`.
inline clang::tooling::ArgumentsAdjuster
getPchCacheAdjuster(const PchCache &cache) {
  return [&cache](const clang::tooling::CommandLineArguments &args,
                  llvm::StringRef /*filename*/) {
    clang::tooling::CommandLineArguments result;
    result.reserve(args.size());
    for (size_t i = 0; i < args.size(); ++i) {
      llvm::StringRef arg(args[i]);

      // Handle -Xarch_* -include<pch> pairs.
      if (arg.starts_with("-Xarch_") && i + 1 < args.size()) {
        llvm::StringRef next(args[i + 1]);
        if (next.starts_with("-include") && !next.starts_with("-include-pch") &&
            next.contains("pch")) {
          std::string pchSrc;
          if (next.size() > 8) {
            pchSrc = next.substr(8).str();
          } else if (i + 2 < args.size()) {
            pchSrc = args[i + 2];
          }
          auto compiled = cache.getCompiledPch(pchSrc);
          if (!compiled.empty()) {
            // Replace the whole -Xarch -include<pch> with -include-pch
            result.push_back("-include-pch");
            result.push_back(compiled);
            i += (next == "-include") ? 2 : 1;
            continue;
          }
        }
      }

      // Handle standalone -include<pch> (joined or separate).
      if (arg.starts_with("-include") && !arg.starts_with("-include-pch") &&
          arg.contains("pch")) {
        std::string pchSrc;
        if (arg.size() > 8) {
          pchSrc = arg.substr(8).str();
        } else if (i + 1 < args.size()) {
          pchSrc = args[i + 1];
        }
        auto compiled = cache.getCompiledPch(pchSrc);
        if (!compiled.empty()) {
          result.push_back("-include-pch");
          result.push_back(compiled);
          if (arg == "-include")
            ++i; // skip separate path arg
          continue;
        }
      }

      result.push_back(args[i]);
    }
    return result;
  };
}

/// Create a ClangTool with standard argument adjusters applied.
/// When pchCache is provided, PCH source includes are replaced with compiled
/// .pch binaries instead of being stripped.
/// When sysroot is non-empty, it overrides the default macOS SDK path; an
/// empty string falls back to the build-time default from xcrun.
inline clang::tooling::ClangTool
makeClangTool(const clang::tooling::CompilationDatabase &compDb,
              const std::vector<std::string> &files,
              const PchCache *pchCache = nullptr,
              const std::string &sysroot = "") {
  clang::tooling::ClangTool tool(compDb, files);
  if (pchCache && !pchCache->empty()) {
    tool.appendArgumentsAdjuster(getPchCacheAdjuster(*pchCache));
    tool.appendArgumentsAdjuster(getStripIncompatibleFlagsAdjuster(/*stripPch=*/false));
  } else {
    tool.appendArgumentsAdjuster(getStripIncompatibleFlagsAdjuster());
  }
  tool.appendArgumentsAdjuster(
      clang::tooling::getClangStripDependencyFileAdjuster());
  tool.appendArgumentsAdjuster(getSysrootAdjuster(sysroot));
  tool.appendArgumentsAdjuster(getResourceDirAdjuster());
  if (!globalExtraArgs().empty()) {
    tool.appendArgumentsAdjuster(clang::tooling::getInsertArgumentAdjuster(
        globalExtraArgs(), clang::tooling::ArgumentInsertPosition::END));
  }
  // After the extra-args insertion on purpose: an explicit
  // --gcc-install-dir/--sysroot/-stdlib from the user must be visible to
  // (and suppress) this adjuster.
  tool.appendArgumentsAdjuster(getGccInstallDirAdjuster());
  return tool;
}

} // namespace vycor

#endif // VYCOR_COMPAT_TOOL_ADJUSTERS_H
