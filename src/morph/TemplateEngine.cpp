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

#include "vycor/morph/TemplateEngine.h"

#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Lex/Lexer.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/raw_ostream.h"

namespace vycor {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Get the source text for a SourceRange, resolving through all macro
/// expansion levels to the original file location.
/// getSpellingLoc only unwraps one level; getExpansionLoc collapses to the
/// macro call start. Walking getImmediateSpellingLoc until isFileID is the
/// only reliable way to reach the original source text for deeply nested
/// macro argument chains.
static std::string getSourceText(clang::SourceRange range,
                                 const clang::SourceManager &SM,
                                 const clang::LangOptions &LangOpts) {
  clang::SourceLocation begin = range.getBegin();
  while (begin.isMacroID())
    begin = SM.getImmediateSpellingLoc(begin);
  clang::SourceLocation end = range.getEnd();
  while (end.isMacroID())
    end = SM.getImmediateSpellingLoc(end);
  auto charRange = clang::CharSourceRange::getTokenRange(begin, end);
  auto text = clang::Lexer::getSourceText(charRange, SM, LangOpts);
  return text.str();
}

/// Recursively flatten a binary operator chain into its leaf operand texts.
/// Stops at non-matching operators, non-BinaryOperator nodes, and ParenExprs.
static std::vector<std::string>
flattenBinOp(const clang::Expr *expr, llvm::StringRef op,
             const clang::SourceManager &SM,
             const clang::LangOptions &LangOpts) {
  expr = expr->IgnoreImplicit();
  if (const auto *binOp = llvm::dyn_cast<clang::BinaryOperator>(expr)) {
    if (binOp->getOpcodeStr() == op) {
      auto left = flattenBinOp(binOp->getLHS(), op, SM, LangOpts);
      auto right = flattenBinOp(binOp->getRHS(), op, SM, LangOpts);
      left.insert(left.end(), right.begin(), right.end());
      return left;
    }
  }
  return {getSourceText(expr->getSourceRange(), SM, LangOpts)};
}

// ---------------------------------------------------------------------------
// Template Parsing
// ---------------------------------------------------------------------------

/// Parse a single pipe operation like "flatten("&&")" or "join(", ")" or "name".
static llvm::Expected<PipeOp> parsePipeOp(llvm::StringRef raw) {
  raw = raw.trim();
  if (raw.empty())
    return llvm::createStringError(llvm::inconvertibleErrorCode(), "empty pipe operation");

  auto parenPos = raw.find('(');
  if (parenPos == llvm::StringRef::npos) {
    // No-arg pipe: "text", "name"
    return PipeOp{raw.str(), ""};
  }

  std::string name = raw.substr(0, parenPos).trim().str();
  llvm::StringRef rest = raw.substr(parenPos + 1);

  // Expect: "arg") — find closing paren
  // The arg is typically quoted: ("&&") or (", ")
  auto closePos = rest.rfind(')');
  if (closePos == llvm::StringRef::npos)
    return llvm::createStringError(llvm::inconvertibleErrorCode(), "unclosed parenthesis in pipe: " + name);

  llvm::StringRef argRaw = rest.substr(0, closePos).trim();

  // Strip surrounding quotes if present
  std::string arg;
  if (argRaw.size() >= 2 && argRaw.front() == '"' && argRaw.back() == '"')
    arg = argRaw.substr(1, argRaw.size() - 2).str();
  else
    arg = argRaw.str();

  return PipeOp{std::move(name), std::move(arg)};
}

/// Split a template expression body on '|' respecting quoted strings.
static std::vector<llvm::StringRef> splitPipes(llvm::StringRef body) {
  std::vector<llvm::StringRef> parts;
  bool inQuotes = false;
  size_t start = 0;
  for (size_t i = 0; i < body.size(); ++i) {
    if (body[i] == '"')
      inQuotes = !inQuotes;
    if (body[i] == '|' && !inQuotes) {
      parts.push_back(body.substr(start, i - start));
      start = i + 1;
    }
  }
  parts.push_back(body.substr(start));
  return parts;
}

llvm::Expected<ParsedTemplate> parseTemplate(llvm::StringRef templateStr) {
  ParsedTemplate result;
  llvm::StringRef remaining = templateStr;

  while (!remaining.empty()) {
    auto openPos = remaining.find("{{");
    if (openPos == llvm::StringRef::npos) {
      // No more expressions — rest is literal
      result.literals.push_back(remaining.str());
      break;
    }

    // Literal before the expression
    result.literals.push_back(remaining.substr(0, openPos).str());
    remaining = remaining.substr(openPos + 2);

    // Find closing }}
    auto closePos = remaining.find("}}");
    if (closePos == llvm::StringRef::npos)
      return llvm::createStringError(llvm::inconvertibleErrorCode(), "unclosed {{ in template");

    llvm::StringRef exprBody = remaining.substr(0, closePos);
    remaining = remaining.substr(closePos + 2);

    // Parse expression: nodeId | pipe1 | pipe2 ...
    auto parts = splitPipes(exprBody);
    if (parts.empty())
      return llvm::createStringError(llvm::inconvertibleErrorCode(), "empty expression in template");

    TemplateExpr expr;
    expr.nodeId = parts[0].trim().str();
    if (expr.nodeId.empty())
      return llvm::createStringError(llvm::inconvertibleErrorCode(), "empty node ID in template expression");

    for (size_t i = 1; i < parts.size(); ++i) {
      auto pipeOrErr = parsePipeOp(parts[i]);
      if (!pipeOrErr)
        return pipeOrErr.takeError();
      expr.pipes.push_back(std::move(*pipeOrErr));
    }

    result.exprs.push_back(std::move(expr));
  }

  // Ensure there's always one more literal than expressions
  if (result.literals.size() == result.exprs.size())
    result.literals.push_back("");

  return result;
}

// ---------------------------------------------------------------------------
// Template Evaluation
// ---------------------------------------------------------------------------

static llvm::Expected<std::string>
evaluateExpr(const TemplateExpr &texpr,
             const clang::ast_matchers::MatchFinder::MatchResult &result) {
  const auto &SM = *result.SourceManager;
  const auto &LangOpts = result.Context->getLangOpts();

  // Look up bound node
  auto map = result.Nodes.getMap();
  auto it = map.find(texpr.nodeId);
  if (it == map.end())
    return llvm::createStringError(llvm::inconvertibleErrorCode(), "bound node not found: " + texpr.nodeId);

  const auto &node = it->second;

  // No pipes → default to source text
  if (texpr.pipes.empty())
    return getSourceText(node.getSourceRange(), SM, LangOpts);

  // Process pipe chain
  std::string textVal;
  std::vector<std::string> listVal;
  bool isList = false;
  bool initialized = false;

  for (const auto &pipe : texpr.pipes) {
    if (pipe.name == "text") {
      if (initialized)
        return llvm::createStringError(llvm::inconvertibleErrorCode(), "'text' must be first in pipe chain");
      textVal = getSourceText(node.getSourceRange(), SM, LangOpts);
      initialized = true;
      isList = false;
    } else if (pipe.name == "name") {
      if (initialized)
        return llvm::createStringError(llvm::inconvertibleErrorCode(), "'name' must be first in pipe chain");
      const auto *nd = node.get<clang::NamedDecl>();
      if (!nd)
        return llvm::createStringError(
            llvm::inconvertibleErrorCode(),
            "node '" + texpr.nodeId + "' is not a NamedDecl for 'name' pipe");
      textVal = nd->getQualifiedNameAsString();
      initialized = true;
      isList = false;
    } else if (pipe.name == "flatten") {
      if (initialized)
        return llvm::createStringError(llvm::inconvertibleErrorCode(), "'flatten' must be first in pipe chain");
      const auto *astExpr = node.get<clang::Expr>();
      if (!astExpr)
        return llvm::createStringError(
            llvm::inconvertibleErrorCode(),
            "node '" + texpr.nodeId + "' is not an Expr for 'flatten' pipe");
      listVal = flattenBinOp(astExpr, pipe.arg, SM, LangOpts);
      initialized = true;
      isList = true;
    } else if (pipe.name == "join") {
      if (!initialized || !isList)
        return llvm::createStringError(
            llvm::inconvertibleErrorCode(),
            "'join' requires a preceding list-producing pipe");
      textVal = llvm::join(listVal.begin(), listVal.end(), pipe.arg);
      isList = false;
    } else {
      return llvm::createStringError(llvm::inconvertibleErrorCode(), "unknown pipe operation: " + pipe.name);
    }
  }

  if (isList)
    return llvm::join(listVal.begin(), listVal.end(), "");
  return textVal;
}

llvm::Expected<std::string> evaluateTemplate(
    const ParsedTemplate &tmpl,
    const clang::ast_matchers::MatchFinder::MatchResult &result) {
  std::string output;
  output += tmpl.literals[0];

  for (size_t i = 0; i < tmpl.exprs.size(); ++i) {
    auto valOrErr = evaluateExpr(tmpl.exprs[i], result);
    if (!valOrErr)
      return valOrErr.takeError();
    output += *valOrErr;
    output += tmpl.literals[i + 1];
  }

  return output;
}

// ---------------------------------------------------------------------------
// Callback Generation
// ---------------------------------------------------------------------------

ReplacementCallback makeTemplateCallback(ParsedTemplate tmpl,
                                         std::string target,
                                         std::string scope,
                                         std::vector<std::string> macroNames,
                                         bool argOnly) {
  return [tmpl = std::move(tmpl), target = std::move(target),
          scope = std::move(scope), macroNames = std::move(macroNames),
          argOnly](
             const clang::ast_matchers::MatchFinder::MatchResult &result)
             -> std::vector<clang::tooling::Replacement> {
    // Find target node (before template eval to allow early-out filters)
    auto map = result.Nodes.getMap();
    auto it = map.find(target);
    if (it == map.end()) {
      llvm::errs() << "Target node '" << target << "' not found in match\n";
      return {};
    }

    const auto &SM = *result.SourceManager;
    const auto &LangOpts = result.Context->getLangOpts();
    clang::SourceLocation targetLoc = it->second.getSourceRange().getBegin();

    // Determine replacement range
    clang::CharSourceRange range;
    if (scope == "macro-expansion") {
      // Filter 1: When arg-only is set (default), only match nodes from
      // the macro ARGUMENT (user's code), not body tokens. When arg-only
      // is false, also match structural patterns introduced by the macro
      // (e.g. the || in RBX_CHECK_OR's body).
      if (!targetLoc.isMacroID())
        return {};
      if (argOnly && !SM.isMacroArgExpansion(targetLoc))
        return {};

      range = SM.getExpansionRange(targetLoc);
      auto callText = clang::Lexer::getSourceText(range, SM, LangOpts);

      // Filter 2: Check macro name at the expansion site.
      // isExpandedFromMacro() doesn't work for arg-chaining wrappers
      // (e.g. RBX_CHECK → CHECK → INTERNAL_CATCH_TEST) because the
      // token is a macro arg at every level and the name check only
      // fires for body expansions. Instead, read the macro name
      // directly from the call-site text.
      if (!macroNames.empty()) {
        auto parenPos = callText.find('(');
        llvm::StringRef name = (parenPos != llvm::StringRef::npos)
                                   ? callText.substr(0, parenPos).rtrim()
                                   : callText;
        if (!llvm::is_contained(macroNames, name))
          return {};
      }

      // Filter 3: Only match nodes that are the TOP-LEVEL expression of
      // the macro argument. Compare the target node's source text against
      // the macro argument text (between the outermost parens of the call).
      // If they differ, the node is nested inside a larger expression
      // (e.g. the && inside RBX_CHECK(a || b && c)) and should be skipped.
      //
      // This check is only meaningful when the matched node's tokens all
      // come from the user's source (same FileID). When the node mixes
      // body tokens (spelled in a macro definition) with arg tokens
      // (spelled in the source), the macro itself defines the operator
      // structure, and the top-level-arg check doesn't apply.
      {
        auto openParen = callText.find('(');
        auto closeParen = callText.rfind(')');
        if (openParen != llvm::StringRef::npos &&
            closeParen != llvm::StringRef::npos && closeParen > openParen) {
          clang::SourceRange nodeRange = it->second.getSourceRange();
          clang::SourceLocation filBegin = nodeRange.getBegin();
          while (filBegin.isMacroID())
            filBegin = SM.getImmediateSpellingLoc(filBegin);
          clang::SourceLocation filEnd = nodeRange.getEnd();
          while (filEnd.isMacroID())
            filEnd = SM.getImmediateSpellingLoc(filEnd);

          // Only compare when the node's resolved file locations fall
          // within the macro call's range (i.e., the tokens are from the
          // user's argument text). If they resolve to the macro definition
          // or a different location, the matched node uses body tokens and
          // the text comparison doesn't apply.
          clang::SourceLocation callStart = range.getBegin();
          if (SM.getFileID(filBegin) == SM.getFileID(callStart) &&
              SM.getFileID(filEnd) == SM.getFileID(callStart)) {
            unsigned callOff = SM.getFileOffset(callStart);
            unsigned nBeginOff = SM.getFileOffset(filBegin);
            unsigned nEndOff = SM.getFileOffset(filEnd);
            if (nBeginOff >= callOff && nEndOff >= callOff) {
              auto argText =
                  callText.substr(openParen + 1, closeParen - openParen - 1)
                      .trim();
              auto nodeCharRange =
                  clang::CharSourceRange::getTokenRange(filBegin, filEnd);
              auto nodeText =
                  clang::Lexer::getSourceText(nodeCharRange, SM, LangOpts);
              if (argText != nodeText.trim())
                return {};
            }
          }
        }
      }
    } else {
      // "node" scope (default)
      range = clang::CharSourceRange::getTokenRange(
          it->second.getSourceRange());
    }

    // Evaluate template (after filters to avoid unnecessary work)
    auto textOrErr = evaluateTemplate(tmpl, result);
    if (!textOrErr) {
      llvm::errs() << "Template evaluation error: "
                   << llvm::toString(textOrErr.takeError()) << "\n";
      return {};
    }

    clang::tooling::Replacement rep(SM, range, *textOrErr, LangOpts);
    return {std::move(rep)};
  };
}

} // namespace vycor
