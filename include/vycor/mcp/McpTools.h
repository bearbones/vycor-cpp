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
#include <string>
#include <vector>

namespace vycor {

/// Context passed to every tool handler.
struct McpToolContext {
  const CallGraph &graph;
  const ControlFlowOracle &oracle;
  const ControlFlowIndex &cfIndex;
  const std::vector<std::string> &entryPoints;
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
