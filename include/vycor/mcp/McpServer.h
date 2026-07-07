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
#include "vycor/mcp/McpProtocol.h"
#include "vycor/mcp/McpTools.h"

#include "clang/Tooling/CompilationDatabase.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace vycor {

class PchCache;

struct McpBuildParams {
  std::shared_ptr<clang::tooling::CompilationDatabase> compDb;
  std::vector<std::string> collapsePaths;
  const PchCache *pchCache = nullptr;
  std::string sysroot;
  LockTypeConfig lockCfg;
};

class McpServer {
public:
  McpServer(CallGraph &&graph, ControlFlowIndex &&cfIndex,
            std::vector<std::string> entryPoints,
            McpBuildParams buildParams = {});

  /// Run the MCP stdio loop. Returns 0 on clean shutdown.
  int run();

  /// Re-index a single TU. Removes old edges/contexts, re-runs Phase 1+2+3.
  /// Returns {edgesRemoved, edgesAfter, contextsRemoved, contextsAfter}.
  struct ReindexResult {
    size_t edgesRemoved;
    size_t edgesAfter;
    size_t contextsRemoved;
    size_t contextsAfter;
  };
  ReindexResult reindexTU(const std::string &filePath);

private:
  CallGraph graph_;
  ControlFlowIndex cfIndex_;
  ControlFlowOracle oracle_;
  std::vector<std::string> entryPoints_;
  McpBuildParams buildParams_;
  // Whole-graph query results, valid until the next index mutation
  // (reindexTU clears it).
  QueryCache queryCache_;
  bool initialized_ = false;
  // Tool name -> handler, populated lazily on the first tools/call.
  std::unordered_map<std::string, McpToolHandler> handlers_;

  void dispatch(const McpRequest &req);

  llvm::json::Value handleInitialize(const llvm::json::Object &params);
  llvm::json::Value handleToolsList();
  llvm::json::Value handleToolsCall(const llvm::json::Object &params);
};

} // namespace vycor
