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
// Framing state
// ---------------------------------------------------------------------------

static McpFraming g_framing = McpFraming::Newline;

McpFraming activeFraming() { return g_framing; }

void setActiveFraming(McpFraming framing) { g_framing = framing; }

// ---------------------------------------------------------------------------
// Reading: newline-delimited or Content-Length framed JSON-RPC from FILE*
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

/// Read one framed message body. Returns false on EOF or unrecoverable
/// framing error. Detects the framing per message and updates g_framing.
static bool readMessageBody(FILE *in, llvm::raw_ostream &errLog,
                            std::string &body) {
  // Skip blank lines and stray whitespace between messages, then sniff the
  // first byte: '{' means newline-delimited JSON (MCP-standard stdio
  // framing); anything else is treated as the start of a Content-Length
  // header block (legacy LSP-style clients).
  int first;
  do {
    first = std::fgetc(in);
  } while (first == '\n' || first == '\r' || first == ' ' || first == '\t');
  if (first == EOF)
    return false;

  body.clear();
  if (first == '{') {
    g_framing = McpFraming::Newline;
    body += '{';
    int ch;
    while ((ch = std::fgetc(in)) != EOF && ch != '\n')
      body += static_cast<char>(ch);
    while (!body.empty() && body.back() == '\r')
      body.pop_back();
    return true;
  }

  g_framing = McpFraming::ContentLength;
  std::ungetc(first, in);

  // Read headers until empty line.
  int contentLength = -1;
  std::string headerLine;
  while (true) {
    if (!readHeaderLine(in, headerLine))
      return false; // EOF.
    if (headerLine.empty())
      break; // End of headers.
    int len = parseContentLength(headerLine);
    if (len >= 0)
      contentLength = len;
    // Ignore other headers.
  }

  if (contentLength < 0) {
    errLog << "megascope: missing Content-Length header\n";
    return false;
  }

  // Read exactly contentLength bytes.
  body.assign(contentLength, '\0');
  size_t bytesRead = std::fread(&body[0], 1, contentLength, in);
  if (bytesRead != static_cast<size_t>(contentLength)) {
    errLog << "megascope: truncated message body (expected " << contentLength
           << ", got " << bytesRead << ")\n";
    return false;
  }
  return true;
}

std::optional<McpRequest> readRequest(FILE *in, llvm::raw_ostream &errLog) {
  std::string body;
  llvm::json::Value parsedValue = nullptr;
  llvm::json::Object *obj = nullptr;

  // Malformed messages get a JSON-RPC error response, then we keep reading —
  // both framings stay synchronized after a bad message, so a single garbled
  // request must not take the server down.
  while (true) {
    if (!readMessageBody(in, errLog, body))
      return std::nullopt; // EOF or unrecoverable framing error.

    auto parsed = llvm::json::parse(body);
    if (!parsed) {
      errLog << "megascope: JSON parse error: "
             << llvm::toString(parsed.takeError()) << "\n";
      writeError(nullptr, kParseError, "JSON parse error");
      continue;
    }

    parsedValue = std::move(*parsed);
    obj = parsedValue.getAsObject();
    if (!obj) {
      errLog << "megascope: request is not a JSON object\n";
      writeError(nullptr, kInvalidRequest, "Request must be a JSON object");
      continue;
    }
    // Extract method (required).
    if (!obj->getString("method")) {
      auto *idVal = obj->get("id");
      writeError(idVal ? *idVal : llvm::json::Value(nullptr),
                 kInvalidRequest, "Missing 'method' field");
      continue;
    }
    break;
  }

  auto method = obj->getString("method");

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
// Writing: JSON-RPC to stdout, framed to match the client (see McpFraming)
// ---------------------------------------------------------------------------

static void writeRaw(const llvm::json::Value &msg) {
  // llvm::json prints compact (single-line) JSON by default, which is what
  // newline framing requires — no embedded newlines in the body.
  std::string body;
  llvm::raw_string_ostream os(body);
  os << msg;
  os.flush();

  if (g_framing == McpFraming::ContentLength) {
    std::fprintf(stdout, "Content-Length: %zu\r\n\r\n", body.size());
    std::fwrite(body.data(), 1, body.size(), stdout);
  } else {
    std::fwrite(body.data(), 1, body.size(), stdout);
    std::fputc('\n', stdout);
  }
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
