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

#include "vycor/mcp/McpProtocol.h"

#include "llvm/Support/raw_ostream.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace vycor {

bool McpRequest::isNotification() const {
  return id.getAsNull().has_value();
}

// ---------------------------------------------------------------------------
// Reading: Content-Length framed JSON-RPC from FILE*
// ---------------------------------------------------------------------------

/// Read a single header line (up to \r\n). Returns false on EOF.
static bool readHeaderLine(FILE *in, std::string &line) {
  line.clear();
  while (true) {
    int ch = std::fgetc(in);
    if (ch == EOF)
      return false;
    if (ch == '\r') {
      int next = std::fgetc(in);
      if (next == '\n')
        return true; // End of header line.
      // Malformed — put back and include \r.
      if (next != EOF)
        std::ungetc(next, in);
      line += '\r';
      continue;
    }
    if (ch == '\n')
      return true; // Tolerate bare \n.
    line += static_cast<char>(ch);
  }
}

/// Parse "Content-Length: <N>" from a header line. Returns -1 on failure.
static int parseContentLength(const std::string &line) {
  const char *prefix = "Content-Length:";
  size_t prefixLen = std::strlen(prefix);
  if (line.size() < prefixLen)
    return -1;
  // Case-insensitive prefix match.
  for (size_t i = 0; i < prefixLen; ++i) {
    if (std::tolower(static_cast<unsigned char>(line[i])) !=
        std::tolower(static_cast<unsigned char>(prefix[i])))
      return -1;
  }
  // Skip whitespace after colon.
  size_t pos = prefixLen;
  while (pos < line.size() && line[pos] == ' ')
    ++pos;
  if (pos >= line.size())
    return -1;
  return std::atoi(line.c_str() + pos);
}

std::optional<McpRequest> readRequest(FILE *in, llvm::raw_ostream &errLog) {
  // Read headers until empty line.
  int contentLength = -1;
  std::string headerLine;
  while (true) {
    if (!readHeaderLine(in, headerLine))
      return std::nullopt; // EOF.
    if (headerLine.empty())
      break; // End of headers.
    int len = parseContentLength(headerLine);
    if (len >= 0)
      contentLength = len;
    // Ignore other headers.
  }

  if (contentLength < 0) {
    errLog << "megascope: missing Content-Length header\n";
    return std::nullopt;
  }

  // Read exactly contentLength bytes.
  std::string body(contentLength, '\0');
  size_t bytesRead = std::fread(&body[0], 1, contentLength, in);
  if (bytesRead != static_cast<size_t>(contentLength)) {
    errLog << "megascope: truncated message body (expected " << contentLength
           << ", got " << bytesRead << ")\n";
    return std::nullopt;
  }

  // Parse JSON.
  auto parsed = llvm::json::parse(body);
  if (!parsed) {
    errLog << "megascope: JSON parse error: "
           << llvm::toString(parsed.takeError()) << "\n";
    writeError(nullptr, kParseError, "JSON parse error");
    // Return nullopt to signal the caller should try reading the next message,
    // but for simplicity we treat parse errors as fatal in the protocol sense.
    return std::nullopt;
  }

  auto *obj = parsed->getAsObject();
  if (!obj) {
    errLog << "megascope: request is not a JSON object\n";
    writeError(nullptr, kInvalidRequest, "Request must be a JSON object");
    return std::nullopt;
  }

  // Extract method (required).
  auto method = obj->getString("method");
  if (!method) {
    auto idVal = obj->get("id");
    writeError(idVal ? std::move(*idVal) : llvm::json::Value(nullptr),
               kInvalidRequest, "Missing 'method' field");
    return std::nullopt;
  }

  McpRequest req;
  req.method = method->str();

  // Extract id (optional — absent means notification).
  if (auto *idVal = obj->get("id"))
    req.id = std::move(*idVal);
  else
    req.id = nullptr;

  // Extract params (optional, default to empty object).
  if (auto *paramsVal = obj->get("params")) {
    if (auto *paramsObj = paramsVal->getAsObject())
      req.params = std::move(*paramsObj);
  }

  return req;
}

// ---------------------------------------------------------------------------
// Writing: Content-Length framed JSON-RPC to stdout
// ---------------------------------------------------------------------------

static void writeRaw(const llvm::json::Value &msg) {
  std::string body;
  llvm::raw_string_ostream os(body);
  os << msg;
  os.flush();

  std::fprintf(stdout, "Content-Length: %zu\r\n\r\n", body.size());
  std::fwrite(body.data(), 1, body.size(), stdout);
  std::fflush(stdout);
}

void writeResult(const llvm::json::Value &id, llvm::json::Value result) {
  llvm::json::Object response;
  response["jsonrpc"] = "2.0";
  response["id"] = id;
  response["result"] = std::move(result);
  writeRaw(llvm::json::Value(std::move(response)));
}

void writeError(const llvm::json::Value &id, int code,
                llvm::StringRef message) {
  llvm::json::Object error;
  error["code"] = code;
  error["message"] = message.str();

  llvm::json::Object response;
  response["jsonrpc"] = "2.0";
  response["id"] = id;
  response["error"] = std::move(error);
  writeRaw(llvm::json::Value(std::move(response)));
}

void writeMessage(llvm::json::Value message) { writeRaw(message); }

} // namespace vycor
