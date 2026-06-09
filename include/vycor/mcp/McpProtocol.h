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

#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

#include <optional>
#include <string>

namespace vycor {

// JSON-RPC 2.0 error codes.
constexpr int kParseError = -32700;
constexpr int kInvalidRequest = -32600;
constexpr int kMethodNotFound = -32601;
constexpr int kInvalidParams = -32602;
constexpr int kInternalError = -32603;

/// A parsed JSON-RPC 2.0 request.
struct McpRequest {
  llvm::json::Value id = nullptr; // number, string, or null for notifications
  std::string method;
  llvm::json::Object params;

  bool isNotification() const;
};

/// Wire framing for the stdio transport.
///
/// The MCP specification frames stdio messages as newline-delimited JSON:
/// one compact JSON-RPC message per line, no headers. Earlier vycor builds
/// (and LSP-derived clients) used Content-Length headers instead. readRequest
/// autodetects the framing per message from its first byte ('{' = newline
/// framing, anything else = header framing) and records it; all writes use
/// the most recently detected framing so responses match the client.
enum class McpFraming {
  Newline,       // MCP-standard: one JSON message per line
  ContentLength, // Legacy: LSP-style Content-Length headers
};

/// Framing used for outgoing messages. Defaults to Newline until a request
/// is read; thereafter follows the framing of the last request read.
McpFraming activeFraming();

/// Override the outgoing framing (used by tests and embedders).
void setActiveFraming(McpFraming framing);

/// Read one JSON-RPC request from an input stream, autodetecting newline
/// vs. Content-Length framing (see McpFraming).
/// Returns std::nullopt on EOF or unrecoverable framing error.
std::optional<McpRequest> readRequest(FILE *in, llvm::raw_ostream &errLog);

/// Write a JSON-RPC 2.0 success response to stdout.
void writeResult(const llvm::json::Value &id, llvm::json::Value result);

/// Write a JSON-RPC 2.0 error response to stdout.
void writeError(const llvm::json::Value &id, int code, llvm::StringRef message);

/// Write a raw JSON-RPC message (used for notifications from server).
void writeMessage(llvm::json::Value message);

} // namespace vycor
