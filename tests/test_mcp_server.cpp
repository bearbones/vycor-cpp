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
// The MCP adapter's translation of the transport-neutral tool result
// contract (vycor/query/Tools.h) onto tools/call results.

#include "vycor/mcp/McpServer.h"
#include "vycor/query/Tools.h"

#include "llvm/Support/JSON.h"

#include <catch2/catch_test_macros.hpp>

using namespace vycor;

namespace {

const llvm::json::Object &firstContent(const llvm::json::Value &result) {
  const auto *obj = result.getAsObject();
  REQUIRE(obj != nullptr);
  const auto *content = obj->getArray("content");
  REQUIRE(content != nullptr);
  REQUIRE(content->size() == 1);
  const auto *first = content->front().getAsObject();
  REQUIRE(first != nullptr);
  CHECK(first->getString("type") == "text");
  return *first;
}

} // namespace

TEST_CASE("wrapToolResult stringifies a payload into one text block",
          "[mcp][adapter]") {
  llvm::json::Object payload;
  payload["count"] = 2;
  payload["items"] = llvm::json::Array{"a", "b"};
  auto result = wrapToolResult(llvm::json::Value(std::move(payload)));

  const auto &content = firstContent(result);
  CHECK_FALSE(result.getAsObject()->getBoolean("isError").has_value());
  auto text = content.getString("text");
  REQUIRE(text.has_value());
  // Compact: no newlines or padding, so the text is a single JSON line.
  CHECK(text->find('\n') == llvm::StringRef::npos);
  auto parsed = llvm::json::parse(*text);
  REQUIRE(bool(parsed));
  CHECK(parsed->getAsObject()->getInteger("count") == 2);
  CHECK(parsed->getAsObject()->getArray("items")->size() == 2);
}

TEST_CASE("wrapToolResult maps an error payload to isError with the "
          "payload as text",
          "[mcp][adapter]") {
  auto result = wrapToolResult(usageError("Missing required 'name'"));
  const auto &content = firstContent(result);
  CHECK(result.getAsObject()->getBoolean("isError") == true);
  // The JSON payload, so a client reads `error` and `status` the same
  // way it reads any other result (docs/result-contract.md).
  auto parsed = llvm::json::parse(*content.getString("text"));
  REQUIRE(bool(parsed));
  CHECK(parsed->getAsObject()->getString("error") ==
        "Missing required 'name'");
  CHECK(parsed->getAsObject()->getString("status") == "usage_error");
  CHECK(wrapToolResult(notFoundError("Function not found: x"))
            .getAsObject()
            ->getBoolean("isError") == true);
}

TEST_CASE("wrapToolResult keeps an ambiguity payload a non-error result",
          "[mcp][adapter]") {
  llvm::json::Object payload;
  payload["ambiguous"] = true;
  payload["candidates"] = llvm::json::Array{};
  auto v = llvm::json::Value(std::move(payload));
  CHECK(isAmbiguousResult(v));
  CHECK_FALSE(isErrorResult(v));
  auto result = wrapToolResult(v);
  CHECK_FALSE(result.getAsObject()->getBoolean("isError").has_value());
}

TEST_CASE("query result predicates", "[query][contract]") {
  CHECK(isErrorResult(notFoundError("x")));
  CHECK(errorMessage(usageError("boom")) == "boom");
  CHECK_FALSE(isErrorResult(llvm::json::Value(llvm::json::Object{})));
  CHECK_FALSE(isErrorResult(llvm::json::Value("a string")));
  CHECK_FALSE(isAmbiguousResult(notFoundError("x")));
  CHECK_FALSE(errorMessage(llvm::json::Value(1)).has_value());
}
