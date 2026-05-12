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

#include "vycor/morph/MatcherEngine.h"

#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"

#include <string>
#include <vector>

namespace vycor {

// ---------------------------------------------------------------------------
// JSON Schema Types
// ---------------------------------------------------------------------------

struct JsonFindSpec {
  std::string matcher;
};

struct JsonReplaceSpec {
  std::string target;
  std::string scope;     // "node" or "macro-expansion", defaults to "node"
  std::string withTempl; // template string
  std::vector<std::string> macroNames; // for macro-expansion: only match these
  bool argOnly = true; // for macro-expansion: require matched node to be a
                       // macro argument (not a body token). Set false when
                       // matching structural patterns introduced by the macro.
};

struct JsonRule {
  JsonFindSpec find;
  JsonReplaceSpec replace;
};

struct JsonPass {
  std::string name;    // optional human-readable name
  std::string context; // optional context matcher expression
  std::vector<JsonRule> rules;
};

struct JsonRulesFile {
  std::vector<JsonPass> passes;
};

// JSON deserialization
bool fromJSON(const llvm::json::Value &value, JsonFindSpec &out,
              llvm::json::Path path);
bool fromJSON(const llvm::json::Value &value, JsonReplaceSpec &out,
              llvm::json::Path path);
bool fromJSON(const llvm::json::Value &value, JsonRule &out,
              llvm::json::Path path);
bool fromJSON(const llvm::json::Value &value, JsonPass &out,
              llvm::json::Path path);
bool fromJSON(const llvm::json::Value &value, JsonRulesFile &out,
              llvm::json::Path path);

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/// Parse a JSON rules file from disk.
llvm::Expected<JsonRulesFile> parseRulesFile(llvm::StringRef path);

/// Convert parsed JSON rules into TransformPipeline passes.
/// Each inner vector is one pass of TransformRules.
llvm::Expected<std::vector<std::vector<TransformRule>>>
buildPipeline(const JsonRulesFile &rules);

} // namespace vycor
