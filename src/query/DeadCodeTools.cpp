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


#include "vycor/query/Tools.h"
#include "vycor/query/Identity.h"
#include "vycor/query/Serialize.h"
#include "Registry.h"
#include "Schema.h"

#include "llvm/ADT/StringRef.h"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include "vycor/anneal/DeadCodeAnalyzer.h"

namespace vycor {

// ============================================================================
// System path heuristic for analyze_dead_code filtering
// ============================================================================

static bool isSystemPath(llvm::StringRef path) {
  if (path.empty())
    return false;
  static constexpr llvm::StringLiteral prefixes[] = {
      "/usr/include/",      "/usr/lib/",
      "/usr/local/include/", "/usr/local/lib/",
      "/Library/Developer/", "/Applications/Xcode.app/",
      "/opt/homebrew/",
  };
  for (auto p : prefixes) {
    if (path.starts_with(p))
      return true;
  }
  // Compiler-internal include directories, e.g. /opt/llvm-20/lib/clang/20/include.
  if (path.contains("/lib/clang/") || path.contains("/lib/gcc/"))
    return true;
  return false;
}


// ============================================================================
// Tool 7: analyze_dead_code
// ============================================================================

static llvm::json::Value handleAnalyzeDeadCode(const llvm::json::Object &args,
                                               const ToolContext &ctx) {
  std::vector<std::string> entryPoints;
  if (auto *epsArr = args.getArray("entry_points")) {
    for (auto &v : *epsArr) {
      if (auto s = v.getAsString())
        entryPoints.push_back(s->str());
    }
  }
  if (entryPoints.empty())
    entryPoints = ctx.entryPoints;

  bool includeOptimistic = true;
  if (auto io = args.getBoolean("include_optimistic"))
    includeOptimistic = *io;

  bool includeSystem = false;
  if (auto is = args.getBoolean("include_system"))
    includeSystem = *is;

  std::string namePrefix;
  if (auto np = args.getString("name_prefix"))
    namePrefix = np->str();
  std::string filePrefix;
  if (auto fp = args.getString("file_prefix"))
    filePrefix = fp->str();

  int64_t limit = 500;
  if (auto l = args.getInteger("limit"))
    limit = *l;
  int64_t offset = 0;
  if (auto o = args.getInteger("offset"))
    offset = *o;
  if (limit < 0)
    limit = 0;
  if (offset < 0)
    offset = 0;

  // Two cache layers: the final JSON keyed on ALL args (identical repeat
  // calls are free), and the liveness map keyed on the analysis inputs
  // only (entry points + optimistic flag) so pagination/filter variations
  // share the whole-graph BFS.
  std::string fullKey;
  if (ctx.cache) {
    fullKey = "dead_code_json|";
    fullKey += includeOptimistic ? '1' : '0';
    fullKey += includeSystem ? '1' : '0';
    fullKey += '|' + namePrefix + '|' + filePrefix + '|' +
               std::to_string(limit) + '|' + std::to_string(offset);
    for (const auto &ep : entryPoints) {
      fullKey += '|';
      fullKey += ep;
    }
    auto it = ctx.cache->byKey.find(fullKey);
    if (it != ctx.cache->byKey.end())
      return it->second;
  }

  using LivenessMap = std::unordered_map<std::string, Liveness>;
  std::shared_ptr<const LivenessMap> resultsPtr;
  std::string cacheKey;
  if (ctx.cache) {
    cacheKey = "dead_code|";
    cacheKey += includeOptimistic ? '1' : '0';
    for (const auto &ep : entryPoints) {
      cacheKey += '|';
      cacheKey += ep;
    }
    auto it = ctx.cache->objects.find(cacheKey);
    if (it != ctx.cache->objects.end())
      resultsPtr = std::static_pointer_cast<const LivenessMap>(it->second);
  }
  if (!resultsPtr) {
    DeadCodeAnalyzer analyzer(ctx.graph, entryPoints);
    analyzer.analyzePessimistic();
    if (includeOptimistic)
      analyzer.analyzeOptimistic();
    auto computed = std::make_shared<LivenessMap>(analyzer.getResults());
    if (ctx.cache)
      ctx.cache->objects[cacheKey] = computed;
    resultsPtr = std::move(computed);
  }
  const LivenessMap &results = *resultsPtr;

  // Counts of all categories, computed before filtering — aliveCount and
  // optimisticallyAliveCount are meta-stats, not affected by the dead-list
  // filters below.
  int64_t aliveCount = 0, optimisticCount = 0;

  auto passesFilter = [&](const CallGraphNode *node,
                          const std::string &name) {
    if (!includeSystem && node && isSystemPath(node->file))
      return false;
    if (!namePrefix.empty() && !llvm::StringRef(name).starts_with(namePrefix))
      return false;
    if (!filePrefix.empty() &&
        (!node || !llvm::StringRef(node->file).starts_with(filePrefix)))
      return false;
    return true;
  };

  // Both lists come out of an unordered map; order them by location so
  // offset/limit pages are stable across processes and the output is
  // deterministic.
  struct Entry {
    llvm::StringRef file;
    unsigned line;
    const std::string *name;
    const CallGraphNode *node;
  };
  auto byLocation = [](const Entry &a, const Entry &b) {
    if (a.file != b.file)
      return a.file < b.file;
    if (a.line != b.line)
      return a.line < b.line;
    return *a.name < *b.name;
  };
  auto toJson = [](const Entry &e) {
    llvm::json::Object entry;
    entry["name"] = *e.name;
    if (e.node) {
      entry["file"] = e.node->file;
      entry["line"] = static_cast<int64_t>(e.node->line);
      entry["usr"] = e.node->usr;
    }
    return llvm::json::Value(std::move(entry));
  };

  std::vector<Entry> optimisticAll, deadAll;
  for (auto &kv : results) {
    auto *node = ctx.graph.findNode(kv.first);
    Entry e{node ? llvm::StringRef(node->file) : llvm::StringRef(),
            node ? node->line : 0u, &kv.first, node};
    switch (kv.second) {
    case Liveness::Alive:
      ++aliveCount;
      break;
    case Liveness::OptimisticallyAlive:
      ++optimisticCount;
      if (passesFilter(node, kv.first))
        optimisticAll.push_back(e);
      break;
    case Liveness::Dead:
      if (passesFilter(node, kv.first))
        deadAll.push_back(e);
      break;
    }
  }
  std::sort(optimisticAll.begin(), optimisticAll.end(), byLocation);
  std::sort(deadAll.begin(), deadAll.end(), byLocation);

  llvm::json::Array optimistic;
  for (const Entry &e : optimisticAll)
    optimistic.push_back(toJson(e));

  const int64_t totalDead = static_cast<int64_t>(deadAll.size());
  const int64_t start = std::min(offset, totalDead);
  const int64_t end = std::min(start + limit, totalDead);
  llvm::json::Array dead;
  for (int64_t i = start; i < end; ++i)
    dead.push_back(toJson(deadAll[i]));

  llvm::json::Object obj;
  obj["totalFunctions"] = static_cast<int64_t>(results.size());
  obj["aliveCount"] = aliveCount;
  obj["optimisticallyAliveCount"] = optimisticCount;
  obj["totalDead"] = totalDead;
  obj["deadCount"] = static_cast<int64_t>(dead.size());
  obj["offset"] = offset;
  obj["limit"] = limit;
  obj["truncated"] = end < totalDead;
  obj["dead"] = std::move(dead);
  obj["optimisticallyAlive"] = std::move(optimistic);
  // Omit alive list to keep response size down — caller usually wants dead.
  auto out = llvm::json::Value(std::move(obj));
  if (ctx.cache)
    ctx.cache->byKey.insert_or_assign(fullKey, out);
  return out;
}

void registerDeadCodeTools(std::vector<ToolEntry> &tools) {
  // 7. analyze_dead_code
  {
    llvm::json::Object props;
    props["entry_points"] = stringArrayProp(
        "Entry point function names (default: configured entry points). "
        "Entry points are reachability SEEDS: a name shared by several "
        "functions (overloads) seeds ALL of them (deliberate union "
        "semantics — no disambiguation round-trip here).");
    props["include_optimistic"] = boolProp(
        "Include optimistically-alive functions (default: true)");
    props["include_system"] = boolProp(
        "Include functions whose source file lives in a system include "
        "directory such as /usr/include or compiler-internal clang/gcc "
        "include dirs (default: false). Stdlib template instantiations "
        "otherwise dominate the response.");
    props["name_prefix"] = stringProp(
        "Only include dead functions whose qualified name starts with "
        "this prefix.");
    props["file_prefix"] = stringProp(
        "Only include dead functions whose source file path starts with "
        "this prefix.");
    props["limit"] = intProp(
        "Maximum number of dead entries to return after filtering "
        "(default: 500).");
    props["offset"] = intProp(
        "Number of filtered dead entries to skip before returning "
        "(default: 0). Use with limit to paginate.");
    llvm::json::Object schema;
    schema["type"] = "object";
    schema["properties"] = std::move(props);

    tools.push_back({"analyze_dead_code",
                     "Run dead code analysis via call graph reachability. "
                     "Reports dead, optimistically-alive, and alive functions "
                     "from the configured entry points. System-header "
                     "functions are excluded by default; use include_system "
                     "to include them.",
                     llvm::json::Value(std::move(schema)),
                     handleAnalyzeDeadCode});
  }
}

} // namespace vycor
