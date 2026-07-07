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

#include "vycor/callgraph/CallGraph.h"
#include "vycor/callgraph/ControlFlowIndex.h"
#include "vycor/callgraph/ControlFlowOracle.h"

#include "llvm/Support/JSON.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace vycor {

/// Whole-graph query results cached across tool calls. Owned by McpServer
/// and cleared wholesale whenever the indexes mutate (reindex_tu), so a
/// cached value is always consistent with the graph it was computed from.
/// Used by handlers whose cost scales with the whole graph rather than the
/// query (analyze_dead_code reruns full liveness; graph_summary
/// materializes calleesOf for every node).
struct QueryCache {
  // Final JSON results (argument-independent queries, e.g. graph_summary).
  std::map<std::string, llvm::json::Value> byKey;
  // Typed intermediate results shared across argument variations (e.g. the
  // dead-code liveness map, reused by every pagination/filter combination).
  std::map<std::string, std::shared_ptr<void>> objects;

  void clear() {
    byKey.clear();
    objects.clear();
  }
};

/// Context passed to every tool handler.
struct McpToolContext {
  const CallGraph &graph;
  const ControlFlowOracle &oracle;
  const ControlFlowIndex &cfIndex;
  const std::vector<std::string> &entryPoints;
  /// Optional whole-graph result cache; null in contexts that do not want
  /// caching (handlers must treat it as best-effort).
  QueryCache *cache = nullptr;
};

/// Signature for a tool handler function.
using McpToolHandler =
    std::function<llvm::json::Value(const llvm::json::Object &args,
                                    const McpToolContext &ctx)>;

/// Descriptor for a single MCP tool.
struct McpToolEntry {
  std::string name;
  std::string description;
  llvm::json::Value inputSchema; // JSON Schema object
  McpToolHandler handler;
};

/// Returns the list of all registered tools.
std::vector<McpToolEntry> getRegisteredTools();

} // namespace vycor
