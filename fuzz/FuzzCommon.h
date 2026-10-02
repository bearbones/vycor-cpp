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

// FuzzCommon.h — shared plumbing for the libFuzzer targets (fuzz/README.md):
// the parsers under test read files, so each input is written to a
// per-process scratch file first, and the query harness runs every
// registered tool with arguments drawn from whatever index was loaded.

#pragma once

#include "vycor/callgraph/CallGraph.h"
#include "vycor/callgraph/ControlFlowIndex.h"
#include "vycor/callgraph/Snapshot.h"
#include "vycor/callgraph/Utf8.h"
#include "vycor/query/Tools.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace vycor {
namespace fuzz {

/// A scratch directory private to this process (tmpfs when available),
/// removed with everything in it when the process exits normally
/// (libFuzzer's -max_total_time and -runs end with exit()). A crash leaves
/// it behind (libFuzzer dies through _Exit, which runs no destructors);
/// libFuzzer saves the crashing input itself anyway.
inline const std::string &scratchDir() {
  struct Dir {
    std::string path;
    Dir() {
      std::string base = "/dev/shm";
      if (!llvm::sys::fs::is_directory(base)) {
        llvm::SmallString<128> tmp;
        llvm::sys::path::system_temp_directory(/*ErasedOnReboot=*/true, tmp);
        base = std::string(tmp);
      }
      llvm::SmallString<128> made;
      if (llvm::sys::fs::createUniqueDirectory(base + "/vycor-fuzz", made)) {
        std::fprintf(stderr, "fuzz: cannot create a scratch directory\n");
        std::abort();
      }
      path = std::string(made);
    }
    ~Dir() { llvm::sys::fs::remove_directories(path); }
  };
  static const Dir dir;
  return dir.path;
}

inline std::string scratchPath(const char *name) {
  return scratchDir() + "/" + name;
}

/// Replace `path` with `bytes` (no atomic publish: nothing else reads it).
inline void writeScratch(const std::string &path, const uint8_t *data,
                         size_t size) {
  std::FILE *f = std::fopen(path.c_str(), "wb");
  if (!f) {
    std::fprintf(stderr, "fuzz: cannot write %s\n", path.c_str());
    std::abort();
  }
  if (size && std::fwrite(data, 1, size, f) != size) {
    std::fprintf(stderr, "fuzz: short write to %s\n", path.c_str());
    std::abort();
  }
  std::fclose(f);
}

inline void writeScratch(const std::string &path, const std::string &bytes) {
  writeScratch(path, reinterpret_cast<const uint8_t *>(bytes.data()),
               bytes.size());
}

/// Arguments for `tool` drawn from the loaded index: every property of
/// its schema gets a value of its type, strings chosen by the property's
/// name (call sites for *site*, a class for *class*, a function name
/// otherwise). Malformed index content then flows through every handler
/// the way a real query would carry it.
struct ArgPool {
  std::vector<std::string> functions; // display names and usrs
  std::vector<std::string> sites;
  std::vector<std::string> classes;
  std::vector<std::string> channels;

  static ArgPool from(const CallGraph &graph, const ControlFlowIndex &cf,
                      const ChannelIndex *channels) {
    ArgPool pool;
    for (const CallGraphNode *n : graph.allNodes()) {
      if (pool.functions.size() >= 6)
        break;
      pool.functions.push_back(n->qualifiedName);
      pool.functions.push_back(n->usr);
      if (!n->enclosingClass.empty())
        pool.classes.push_back(n->enclosingClass);
    }
    cf.forEachContext([&](const CallSiteContext &ctx) {
      if (pool.sites.size() < 3)
        pool.sites.push_back(ctx.callSite);
    });
    if (channels)
      for (const auto &site : channels->allSites()) {
        if (pool.channels.size() >= 2)
          break;
        pool.channels.push_back(site.channelId);
      }
    if (pool.functions.empty())
      pool.functions.push_back("main");
    if (pool.sites.empty())
      pool.sites.push_back("main.cpp:1:1");
    if (pool.classes.empty())
      pool.classes.push_back("Plugin");
    if (pool.channels.empty())
      pool.channels.push_back("queue");
    return pool;
  }

  static const std::string &pick(const std::vector<std::string> &from,
                                 size_t salt) {
    return from[salt % from.size()];
  }

  llvm::json::Value stringFor(llvm::StringRef prop, size_t salt) const {
    if (prop.contains("site"))
      return pick(sites, salt);
    if (prop.contains("class"))
      return pick(classes, salt);
    if (prop.contains("channel"))
      return pick(channels, salt);
    if (prop.contains("exception"))
      return "std::exception";
    if (prop == "patch")
      return "--- a/main.cpp\n+++ b/main.cpp\n@@ -1,1 +1,1 @@\n-x\n+y\n";
    if (prop == "query" || prop == "filter")
      // A prefix that may cut a multi-byte character: made whole again,
      // as a query from JSON would be.
      return lookupText(pick(functions, salt).substr(0, 3));
    return pick(functions, salt);
  }

  llvm::json::Object argsFor(const ToolEntry &tool, size_t salt) const {
    llvm::json::Object args;
    const auto *schema = tool.inputSchema.getAsObject();
    const auto *props = schema ? schema->getObject("properties") : nullptr;
    if (!props)
      return args;
    size_t i = salt;
    for (const auto &[key, schema] : *props) {
      const auto *s = schema.getAsObject();
      auto type = s ? s->getString("type") : std::nullopt;
      if (!type)
        continue;
      ++i;
      if (*type == "string")
        args[key] = stringFor(key, i);
      else if (*type == "integer")
        args[key] = static_cast<int64_t>(i % 5 + 1);
      else if (*type == "boolean")
        args[key] = (i & 1) != 0;
      else if (*type == "array")
        args[key] = llvm::json::Array{stringFor(key, i)};
    }
    return args;
  }
};

} // namespace fuzz
} // namespace vycor
