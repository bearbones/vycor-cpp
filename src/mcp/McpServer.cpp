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

#include "vycor/mcp/McpServer.h"
#include "vycor/Version.h"
#include "vycor/query/Tools.h"
#include "vycor/callgraph/CallGraphBuilder.h"
#include "vycor/callgraph/ControlFlowIndex.h"

#include "llvm/Support/raw_ostream.h"

#include <optional>
#include <unordered_map>

namespace vycor {

llvm::json::Value mcpTextResult(llvm::StringRef text, bool isError) {
  llvm::json::Object content;
  content["type"] = "text";
  content["text"] = text.str();

  llvm::json::Array contentArr;
  contentArr.push_back(llvm::json::Value(std::move(content)));

  llvm::json::Object result;
  result["content"] = std::move(contentArr);
  if (isError)
    result["isError"] = true;
  return llvm::json::Value(std::move(result));
}

llvm::json::Value wrapToolResult(const llvm::json::Value &payload) {
  std::string text;
  llvm::raw_string_ostream os(text);
  os << payload;
  os.flush();
  return mcpTextResult(text, isErrorStatus(statusOf(payload)));
}

McpServer::McpServer(CallGraph &&graph, ControlFlowIndex &&cfIndex,
                     ChannelIndex &&channels,
                     std::vector<std::string> entryPoints,
                     McpBuildParams buildParams)
    : graph_(std::move(graph)), cfIndex_(std::move(cfIndex)),
      channels_(std::move(channels)), oracle_(graph_, cfIndex_),
      entryPoints_(std::move(entryPoints)),
      buildParams_(std::move(buildParams)) {}

McpServer::ReindexResult McpServer::reindexTU(const std::string &filePath) {
  ReindexResult r{};
  queryCache_.clear(); // graph is about to mutate

  // Under worker isolation the parse runs in a subprocess first; a crash
  // or hang there costs only the TU, which ends up dropped exactly as an
  // in-process crash leaves it.
  std::optional<BakedIndexes> fresh;
  if (buildParams_.compDb && !buildParams_.workerExe.empty())
    fresh = bakeTUIsolated(buildParams_.workerExe, buildParams_.workerCfg,
                           filePath, buildParams_.workerLimits);

  r.edgesRemoved = graph_.removeTU(filePath);
  r.contextsRemoved = cfIndex_.removeTU(filePath);
  channels_.removeTU(filePath);

  if (fresh) {
    graph_.absorb(fresh->graph);
    cfIndex_.absorb(fresh->cfIndex);
    channels_.absorb(fresh->channels);
  } else if (buildParams_.compDb) {
    bakeTU(graph_, cfIndex_, *buildParams_.compDb, filePath,
           buildParams_.collapsePaths, buildParams_.pchCache,
           buildParams_.sysroot, buildParams_.lockCfg,
           buildParams_.channelCfg, &channels_);
  }

  r.edgesAfter = graph_.edgeCount();
  r.contextsAfter = cfIndex_.size();
  return r;
}

int McpServer::run() {
  llvm::errs() << "megascope: server started, waiting for requests...\n";

  while (true) {
    auto req = readRequest(stdin, llvm::errs());
    if (!req)
      break; // EOF or unrecoverable error.

    if (verbose_)
      llvm::errs() << "megascope: received method: " << req->method << "\n";

    // Notifications have no id and get no response.
    if (req->isNotification()) {
      if (req->method == "notifications/initialized") {
        initialized_ = true;
        if (verbose_)
          llvm::errs() << "megascope: client initialized\n";
      }
      // Silently ignore unknown notifications.
      continue;
    }

    dispatch(*req);
  }

  llvm::errs() << "megascope: shutting down\n";
  return 0;
}

void McpServer::dispatch(const McpRequest &req) {
  if (req.method == "initialize") {
    writeResult(req.id, handleInitialize(req.params));
    return;
  }
  if (req.method == "tools/list") {
    writeResult(req.id, handleToolsList());
    return;
  }
  if (req.method == "tools/call") {
    writeResult(req.id, handleToolsCall(req.params));
    return;
  }

  writeError(req.id, kMethodNotFound, "Unknown method: " + req.method);
}

llvm::json::Value McpServer::handleInitialize(
    const llvm::json::Object &params) {
  llvm::json::Object capabilities;
  capabilities["tools"] = llvm::json::Object{};

  llvm::json::Object serverInfo;
  serverInfo["name"] = "vycor-cpp";
  serverInfo["version"] = VYCOR_VERSION_STRING;

  // Echo the client's requested protocol version: the stdio transport and
  // tools capability are unchanged across spec revisions we care about, so
  // agreeing with the client maximizes interop. Fall back to a known-good
  // revision when the client omits the field.
  std::string protocolVersion = "2024-11-05";
  if (auto pv = params.getString("protocolVersion"))
    protocolVersion = pv->str();

  llvm::json::Object result;
  result["protocolVersion"] = protocolVersion;
  result["capabilities"] = std::move(capabilities);
  result["serverInfo"] = std::move(serverInfo);
  return llvm::json::Value(std::move(result));
}

llvm::json::Value McpServer::handleToolsList() {
  auto tools = getRegisteredTools();
  llvm::json::Array toolArray;
  for (auto &tool : tools) {
    llvm::json::Object toolObj;
    toolObj["name"] = std::move(tool.name);
    toolObj["description"] = std::move(tool.description);
    toolObj["inputSchema"] = std::move(tool.inputSchema);
    toolArray.push_back(llvm::json::Value(std::move(toolObj)));
  }

  llvm::json::Object result;
  result["tools"] = std::move(toolArray);
  return llvm::json::Value(std::move(result));
}

llvm::json::Value McpServer::handleToolsCall(
    const llvm::json::Object &params) {
  auto toolName = params.getString("name");
  if (!toolName) {
    return mcpTextResult("Missing 'name' field in tools/call request",
                         /*isError=*/true);
  }

  // Look up tool by name.
  if (tools_.empty()) {
    for (auto &entry : getRegisteredTools()) {
      std::string name = entry.name;
      tools_.emplace(std::move(name), std::move(entry));
    }
  }

  auto it = tools_.find(toolName->str());
  if (it == tools_.end()) {
    return mcpTextResult("Unknown tool: " + toolName->str(),
                         /*isError=*/true);
  }

  // Extract arguments.
  llvm::json::Object args;
  if (auto *argsVal = params.get("arguments")) {
    if (auto *argsObj = argsVal->getAsObject())
      args = *argsObj;
  }

  ToolContext ctx{graph_,       oracle_,    cfIndex_,
                  entryPoints_, &channels_, &queryCache_};
  ctx.facts = facts_;

  if (*toolName == "reindex_tu") {
    // Adapter-implemented (it mutates the indexes); answers through the
    // same contract as every tool. The coverage facts stay those of the
    // bake the server started from: the single-TU re-parse reports no
    // outcome (docs/index-provenance.md, follow-ups).
    auto filePath = args.getString("file");
    if (!filePath)
      return wrapToolResult(completeResult(
          usageError("Missing required 'file' argument"), ctx));
    if (!buildParams_.compDb)
      return wrapToolResult(completeResult(
          unavailableError("reindex_tu unavailable: no compilation database"),
          ctx));
    auto r = reindexTU(filePath->str());
    llvm::json::Object obj;
    obj["file"] = filePath->str();
    obj["edgesRemoved"] = static_cast<int64_t>(r.edgesRemoved);
    obj["edgesAfter"] = static_cast<int64_t>(r.edgesAfter);
    obj["contextsRemoved"] = static_cast<int64_t>(r.contextsRemoved);
    obj["contextsAfter"] = static_cast<int64_t>(r.contextsAfter);
    return wrapToolResult(
        completeResult(llvm::json::Value(std::move(obj)), ctx));
  }

  return wrapToolResult(runTool(it->second, args, ctx));
}

} // namespace vycor
