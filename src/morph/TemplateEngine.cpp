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
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"

namespace vycor {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Read a contiguous spelling at the use site. Resolving the two endpoints
/// independently can splice a macro definition into an unrelated expression.
static llvm::Expected<llvm::StringRef>
getSourceText(clang::SourceRange range, const clang::SourceManager &SM,
              const clang::LangOptions &LangOpts) {
  bool invalid = false;
  auto text = clang::Lexer::getSourceText(
      clang::CharSourceRange::getTokenRange(range), SM, LangOpts, &invalid);
  if (invalid)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "bound node has no contiguous source text");
  return text;
}

/// Visit the leaves from left to right. Parentheses and other operators are
/// leaves in their own right. Append each spelling once, even for long chains.
static llvm::Expected<std::string>
flattenBinOp(const clang::Expr *expr, llvm::StringRef op,
             llvm::StringRef separator, const clang::SourceManager &SM,
             const clang::LangOptions &LangOpts) {
  llvm::SmallVector<const clang::Expr *, 16> pending{expr};
  std::string output;
  while (!pending.empty()) {
    const auto *current = pending.pop_back_val()->IgnoreImplicit();
    if (const auto *bin = llvm::dyn_cast<clang::BinaryOperator>(current)) {
      if (bin->getOpcodeStr() == op) {
        pending.push_back(bin->getRHS());
        pending.push_back(bin->getLHS());
        continue;
      }
    }
    auto text = getSourceText(current->getSourceRange(), SM, LangOpts);
    if (!text)
      return text.takeError();
    output += *text;
    if (!pending.empty())
      output += separator;
  }
  return output;
}

// ---------------------------------------------------------------------------
// Template Parsing
// ---------------------------------------------------------------------------

/// Parse a single pipe operation like "flatten("&&")" or "join(", ")" or
/// "name".
static llvm::Expected<PipeOp> parsePipeOp(llvm::StringRef raw) {
  raw = raw.trim();
  if (raw.empty())
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "empty pipe operation");

  auto paren = raw.find('(');
  auto name = raw.take_front(paren).trim();
  if (name != "name" && name != "text" && name != "flatten" && name != "join")
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "unknown pipe operation: " + name);
  llvm::StringRef arg;
  if (paren != llvm::StringRef::npos) {
    if (raw.back() != ')')
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "expected closing parenthesis in pipe: " +
                                         name);
    arg = raw.drop_front(paren + 1).drop_back().trim();
    if (arg.starts_with("\"") && arg.size() >= 2 && arg.ends_with("\""))
      arg = arg.drop_front().drop_back();
    else if (arg.contains('"') || arg.contains(')') || arg.contains('('))
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "malformed pipe argument: " + name);
  }
  if ((name == "text" || name == "name") && !arg.empty())
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "unexpected argument for pipe: " + name);
  if (name == "flatten" && arg.empty())
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "flatten requires an operator");
  return PipeOp{name.str(), arg.str()};
}

/// Read bindings and pipes up to the closing braces. Quoted separators may
/// themselves contain a pipe or closing braces.
static llvm::Expected<llvm::SmallVector<llvm::StringRef, 3>>
takeExpression(llvm::StringRef &remaining) {
  llvm::SmallVector<llvm::StringRef, 3> parts;
  bool quoted = false;
  size_t start = 0;
  for (size_t i = 0; i < remaining.size(); ++i) {
    if (remaining[i] == '"')
      quoted = !quoted;
    if (quoted)
      continue;
    if (remaining[i] == '|') {
      parts.push_back(remaining.slice(start, i));
      start = i + 1;
    } else if (remaining.substr(i).starts_with("}}")) {
      parts.push_back(remaining.slice(start, i));
      remaining = remaining.drop_front(i + 2);
      return parts;
    }
  }
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "unclosed {{ in template");
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

    auto parts = takeExpression(remaining);
    if (!parts)
      return parts.takeError();
    TemplateExpr expr;
    expr.nodeId = parts->front().trim().str();
    if (expr.nodeId.empty())
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "empty node ID in template expression");

    for (size_t i = 1; i < parts->size(); ++i) {
      auto pipeOrErr = parsePipeOp((*parts)[i]);
      if (!pipeOrErr)
        return pipeOrErr.takeError();
      if (pipeOrErr->name == "join") {
        if (i != 2 || expr.pipes.front().name != "flatten")
          return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                         "join must follow flatten");
      } else if (i != 1) {
        return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                       pipeOrErr->name +
                                           " must be the first pipe");
      }
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

  const auto &nodes = result.Nodes.getMap();
  auto it = nodes.find(texpr.nodeId);
  if (it == nodes.end())
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "bound node not found: " + texpr.nodeId);
  const auto &node = it->second;
  if (texpr.pipes.empty() || texpr.pipes.front().name == "text") {
    auto text = getSourceText(node.getSourceRange(), SM, LangOpts);
    if (!text)
      return text.takeError();
    return text->str();
  }
  if (texpr.pipes.front().name == "name") {
    if (const auto *decl = node.get<clang::NamedDecl>())
      return decl->getQualifiedNameAsString();
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "name requires a NamedDecl: " +
                                       texpr.nodeId);
  }
  if (const auto *expr = node.get<clang::Expr>()) {
    llvm::StringRef separator;
    if (texpr.pipes.size() == 2)
      separator = texpr.pipes[1].arg;
    return flattenBinOp(expr, texpr.pipes.front().arg, separator, SM, LangOpts);
  }
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "flatten requires an Expr: " + texpr.nodeId);
}

llvm::Expected<std::string>
evaluateTemplate(const ParsedTemplate &tmpl,
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

// Follow forwarding wrappers to the invocation the user wrote. A node wholly
// spelled inside that invocation must cover its entire argument, otherwise an
// inner match could discard the surrounding expression. Body tokens have no
// such argument spelling (for example, an operator supplied by the macro).
static std::optional<clang::CharSourceRange>
macroInvocation(clang::SourceRange node, const clang::SourceManager &SM,
                const clang::LangOptions &lang,
                llvm::ArrayRef<std::string> names, bool argOnly) {
  auto loc = node.getBegin();
  if (!loc.isMacroID() || (argOnly && !SM.isMacroArgExpansion(loc)))
    return std::nullopt;
  auto call =
      clang::Lexer::makeFileCharRange(SM.getExpansionRange(loc), SM, lang);
  if (call.isInvalid())
    return std::nullopt;
  if (!names.empty()) {
    clang::Token token;
    if (clang::Lexer::getRawToken(call.getBegin(), token, SM, lang) ||
        !token.is(clang::tok::raw_identifier) ||
        !llvm::is_contained(names, token.getRawIdentifier()))
      return std::nullopt;
  }

  // Follow arguments back to their spelling, but project a nested macro's
  // body tokens onto that invocation. In ASSERT(a || READY && b), READY's
  // definition can be in any file. The relevant token is READY in the call.
  auto argumentToken = [&](clang::SourceLocation token, bool atEnd) {
    while (token.isMacroID()) {
      if (SM.isMacroArgExpansion(token))
        token = SM.getImmediateSpellingLoc(token);
      else {
        auto expansion = SM.getImmediateExpansionRange(token);
        token = atEnd ? expansion.getEnd() : expansion.getBegin();
      }
    }
    return token;
  };
  auto begin = argumentToken(loc, false);
  auto end = argumentToken(node.getEnd(), true);
  if (!argOnly &&
      (begin == call.getBegin() ||
       clang::Lexer::getLocForEndOfToken(end, 0, SM, lang) == call.getEnd()))
    return call; // The macro body supplied a boundary of the target.

  auto open = clang::Lexer::findNextToken(call.getBegin(), SM, lang);
  if (!open || !open->is(clang::tok::l_paren))
    return std::nullopt;
  auto first = clang::Lexer::findNextToken(open->getLocation(), SM, lang);
  auto close = clang::Lexer::findNextToken(end, SM, lang);
  if (!first || first->getLocation() != begin || !close ||
      !close->is(clang::tok::r_paren) ||
      clang::Lexer::getLocForEndOfToken(close->getLocation(), 0, SM, lang) !=
          call.getEnd())
    return std::nullopt;
  return call;
}

ReplacementCallback makeTemplateCallback(ParsedTemplate tmpl,
                                         std::string target, std::string scope,
                                         std::vector<std::string> macroNames,
                                         bool argOnly) {
  return [tmpl = std::move(tmpl), target = std::move(target),
          scope = std::move(scope), macroNames = std::move(macroNames),
          argOnly](const clang::ast_matchers::MatchFinder::MatchResult &result)
             -> std::vector<clang::tooling::Replacement> {
    // Find target node (before template eval to allow early-out filters)
    const auto &map = result.Nodes.getMap();
    auto &diags = result.Context->getDiagnostics();
    auto it = map.find(target);
    if (it == map.end()) {
      diags.Report(diags.getCustomDiagID(clang::DiagnosticsEngine::Error,
                                         "morph: target '%0' was not bound"))
          << target;
      return {};
    }

    const auto &SM = *result.SourceManager;
    const auto &LangOpts = result.Context->getLangOpts();
    clang::SourceLocation targetLoc = it->second.getSourceRange().getBegin();

    // Determine replacement range
    clang::CharSourceRange range;
    if (scope == "macro-expansion") {
      auto call = macroInvocation(it->second.getSourceRange(), SM, LangOpts,
                                  macroNames, argOnly);
      if (!call)
        return {};
      range = *call;
    } else {
      // "node" scope (default)
      range =
          clang::CharSourceRange::getTokenRange(it->second.getSourceRange());
    }

    // Evaluate template (after filters to avoid unnecessary work)
    auto textOrErr = evaluateTemplate(tmpl, result);
    if (!textOrErr) {
      diags.Report(targetLoc, diags.getCustomDiagID(
                                  clang::DiagnosticsEngine::Error, "morph: %0"))
          << llvm::toString(textOrErr.takeError());
      return {};
    }

    range = clang::Lexer::makeFileCharRange(range, SM, LangOpts);
    if (range.isInvalid()) {
      diags.Report(targetLoc, diags.getCustomDiagID(
                                  clang::DiagnosticsEngine::Error,
                                  "morph: target has no editable file range"));
      return {};
    }
    clang::tooling::Replacement rep(SM, range, *textOrErr, LangOpts);
    return {std::move(rep)};
  };
}

} // namespace vycor
