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

#include <string>
#include <vector>

namespace vycor {

/// A single pipe operation in a template expression chain.
struct PipeOp {
  std::string name; // "text", "flatten", "join", "name"
  std::string arg;  // e.g. "&&" for flatten, ", " for join
};

/// A template expression: {{nodeId | pipe1("arg") | pipe2}}.
struct TemplateExpr {
  std::string nodeId;
  std::vector<PipeOp> pipes;
};

/// A parsed replacement template.
/// The template string "A{{x}}B{{y}}C" parses into:
///   literals = ["A", "B", "C"]
///   exprs    = [TemplateExpr("x"), TemplateExpr("y")]
/// Reconstruction: literals[0] + eval(exprs[0]) + literals[1] + ...
struct ParsedTemplate {
  std::vector<std::string> literals;
  std::vector<TemplateExpr> exprs;
};

/// Parse a replacement template string.
/// Syntax: literal text with {{nodeId}} or {{nodeId | op("arg") | op}} expressions.
llvm::Expected<ParsedTemplate> parseTemplate(llvm::StringRef templateStr);

/// Evaluate a parsed template against a match result, producing replacement text.
llvm::Expected<std::string> evaluateTemplate(
    const ParsedTemplate &tmpl,
    const clang::ast_matchers::MatchFinder::MatchResult &result);

/// Create a ReplacementCallback that evaluates the template for each match
/// and creates Replacement objects targeting the specified bound node.
/// \param target     Bind ID of the node whose source range is replaced.
/// \param scope      "node" (replace node range) or "macro-expansion"
///                   (replace enclosing macro invocation range).
/// \param macroNames For "macro-expansion" scope: only create replacements
///                   when the expansion-site macro name is in this list.
///                   Empty means no name filtering.
ReplacementCallback makeTemplateCallback(ParsedTemplate tmpl,
                                         std::string target,
                                         std::string scope,
                                         std::vector<std::string> macroNames,
                                         bool argOnly = true);

} // namespace vycor
