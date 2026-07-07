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
#include "vycor/mcp/McpTools.h"
#include "vycor/callgraph/CallGraphBuilder.h"
#include "vycor/callgraph/ControlFlowIndex.h"

#include "llvm/Support/raw_ostream.h"

#include <unordered_map>

namespace vycor {

McpServer::McpServer(CallGraph &&graph, ControlFlowIndex &&cfIndex,
                     std::vector<std::string> entryPoints,
                     McpBuildParams buildParams)
    : graph_(std::move(graph)), cfIndex_(std::move(cfIndex)),
      oracle_(graph_, cfIndex_), entryPoints_(std::move(entryPoints)),
      buildParams_(std::move(buildParams)) {}

McpServer::ReindexResult McpServer::reindexTU(const std::string &filePath) {
  ReindexResult r{};
  queryCache_.clear(); // graph is about to mutate
  r.edgesRemoved = graph_.removeTU(filePath);
  r.contextsRemoved = cfIndex_.removeTU(filePath);

  if (buildParams_.compDb) {
    bakeTU(graph_, cfIndex_, *buildParams_.compDb, filePath,
           buildParams_.collapsePaths, buildParams_.pchCache,
           buildParams_.sysroot, buildParams_.lockCfg);
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

    llvm::errs() << "megascope: received method: " << req->method << "\n";

    // Notifications have no id and get no response.
    if (req->isNotification()) {
      if (req->method == "notifications/initialized") {
        initialized_ = true;
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
  serverInfo["version"] = "0.1.0";

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
    llvm::json::Object content;
    content["type"] = "text";
    content["text"] = "Missing 'name' field in tools/call request";
    llvm::json::Array contentArr;
    contentArr.push_back(llvm::json::Value(std::move(content)));

    llvm::json::Object result;
    result["content"] = std::move(contentArr);
    result["isError"] = true;
    return llvm::json::Value(std::move(result));
  }

  // Look up tool by name.
  if (handlers_.empty()) {
    for (auto &entry : getRegisteredTools())
      handlers_[entry.name] = std::move(entry.handler);
  }

  auto it = handlers_.find(toolName->str());
  if (it == handlers_.end()) {
    llvm::json::Object content;
    content["type"] = "text";
    content["text"] = "Unknown tool: " + toolName->str();
    llvm::json::Array contentArr;
    contentArr.push_back(llvm::json::Value(std::move(content)));

    llvm::json::Object result;
    result["content"] = std::move(contentArr);
    result["isError"] = true;
    return llvm::json::Value(std::move(result));
  }

  // Extract arguments.
  llvm::json::Object args;
  if (auto *argsVal = params.get("arguments")) {
    if (auto *argsObj = argsVal->getAsObject())
      args = *argsObj;
  }

  if (*toolName == "reindex_tu") {
    auto filePath = args.getString("file");
    if (!filePath) {
      llvm::json::Object content;
      content["type"] = "text";
      content["text"] = "Missing required 'file' argument";
      llvm::json::Array contentArr;
      contentArr.push_back(llvm::json::Value(std::move(content)));
      llvm::json::Object result;
      result["content"] = std::move(contentArr);
      result["isError"] = true;
      return llvm::json::Value(std::move(result));
    }
    if (!buildParams_.compDb) {
      llvm::json::Object content;
      content["type"] = "text";
      content["text"] = "reindex_tu unavailable: no compilation database";
      llvm::json::Array contentArr;
      contentArr.push_back(llvm::json::Value(std::move(content)));
      llvm::json::Object result;
      result["content"] = std::move(contentArr);
      result["isError"] = true;
      return llvm::json::Value(std::move(result));
    }
    auto r = reindexTU(filePath->str());
    std::string msg = "Reindexed " + filePath->str() + "\n" +
                      "Edges removed: " + std::to_string(r.edgesRemoved) +
                      ", total edges: " + std::to_string(r.edgesAfter) + "\n" +
                      "Contexts removed: " + std::to_string(r.contextsRemoved) +
                      ", total contexts: " + std::to_string(r.contextsAfter);
    llvm::json::Object content;
    content["type"] = "text";
    content["text"] = std::move(msg);
    llvm::json::Array contentArr;
    contentArr.push_back(llvm::json::Value(std::move(content)));
    llvm::json::Object result;
    result["content"] = std::move(contentArr);
    return llvm::json::Value(std::move(result));
  }

  McpToolContext ctx{graph_, oracle_, cfIndex_, entryPoints_, &queryCache_};
  return it->second(args, ctx);
}

} // namespace vycor
