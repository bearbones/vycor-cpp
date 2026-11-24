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

#include "StatementSubset.h"
#include "vycor/morph/TemplateEngine.h"

#include "clang/AST/ExprCXX.h"
#include "clang/Lex/Lexer.h"
#include "llvm/ADT/SmallVector.h"

namespace vycor {
namespace {

using clang::ast_matchers::BoundNodes;
using clang::ast_matchers::MatchFinder;
using clang::ast_matchers::internal::DynTypedMatcher;
using Match = MatchFinder::MatchResult;
using clang::Lexer;

// Validate matchers once. matchDynamic runs a compiled matcher on one direct
// child, so a nested statement can never join an outer block's run.
llvm::Expected<DynTypedMatcher>
statementMatcher(const std::string &expression) {
  std::string error;
  auto matcher = MatcherEngine::parse(expression, error);
  if (!matcher)
    return llvm::createStringError(llvm::inconvertibleErrorCode(), error);
  if (!clang::ASTNodeKind::getFromNodeKind<clang::Stmt>().isBaseOf(
          matcher->getSupportedKind()))
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(),
        "statement-subset requires a statement matcher: " + expression);
  return *matcher;
}

// Skip incompatible node kinds before constructing Clang's single-node finder.
auto matchChild(const DynTypedMatcher &matcher, const clang::Stmt &stmt,
                clang::ASTContext &context)
    -> llvm::SmallVector<BoundNodes, 1> {
  auto node = clang::DynTypedNode::create(stmt);
  if (!matcher.canMatchNodesOfKind(node.getNodeKind()))
    return {};
  return clang::ast_matchers::matchDynamic(matcher, node, context);
}

struct Case {
  DynTypedMatcher matcher;
  ParsedTemplate emit;
};

struct Subset {
  DynTypedMatcher anchor;
  std::vector<Case> cases;
  ParsedTemplate prefix, suffix;
  std::string freshLocal;
};

// Find the exact spelling of a complete statement, including its semicolon.
// Review trivia before discarding it. Preprocessor branches and comments
// can be invisible in the AST, even between two children of the same block.
std::optional<clang::CharSourceRange> statementRange(const clang::Stmt &stmt,
                                                     const Match &match) {
  const auto &sm = *match.SourceManager;
  const auto &lang = match.Context->getLangOpts();
  if (!llvm::isa<clang::Expr, clang::DeclStmt>(stmt))
    return {};
  auto begin = stmt.getBeginLoc(), last = stmt.getEndLoc();
  if (!begin.isFileID() || !last.isFileID() ||
      sm.getFileID(begin) != sm.getFileID(last))
    return {};
  clang::Token token;
  if (Lexer::getRawToken(last, token, sm, lang))
    return {};
  if (!token.is(clang::tok::semi)) {
    if (!llvm::isa<clang::Expr>(stmt))
      return {};
    auto semi = Lexer::findNextToken(last, sm, lang);
    if (!semi || !semi->is(clang::tok::semi))
      return {};
    last = semi->getLocation();
  }
  auto end = Lexer::getLocForEndOfToken(last, 0, sm, lang);
  if (!end.isFileID() || sm.getFileID(end) != sm.getFileID(begin))
    return {};
  auto [file, offset] = sm.getDecomposedLoc(begin);
  auto buffer = sm.getBufferData(file);
  unsigned stop = sm.getFileOffset(end);
  if (stop <= offset || stop > buffer.size())
    return {};
  Lexer lexer(sm.getLocForStartOfFile(file), lang, buffer.begin(),
              buffer.begin() + offset, buffer.end());
  lexer.SetCommentRetentionState(true);
  do {
    lexer.LexFromRawLexer(token);
    if (token.isOneOf(clang::tok::comment, clang::tok::hash, clang::tok::eof))
      return {};
  } while (sm.getFileOffset(token.getLocation()) + token.getLength() < stop);
  return clang::CharSourceRange::getCharRange(begin, end);
}

bool adjacent(clang::SourceLocation previousEnd, clang::SourceLocation next,
              const Match &match) {
  const auto &sm = *match.SourceManager;
  auto [file, offset] = sm.getDecomposedLoc(previousEnd);
  auto [nextFile, nextOffset] = sm.getDecomposedLoc(next);
  return file == nextFile && nextOffset >= offset &&
         sm.getBufferData(file).slice(offset, nextOffset).trim().empty();
}

// The fresh-local mode preserves the declaration through its name. Allow only
// plain default initialization or empty braces after it.
bool freshDeclaration(const clang::Stmt &stmt, const clang::VarDecl *var,
                      clang::CharSourceRange range, const Match &match) {
  const auto *decl = llvm::dyn_cast<clang::DeclStmt>(&stmt);
  if (!decl || !decl->isSingleDecl() || decl->getSingleDecl() != var || !var ||
      !var->hasLocalStorage() || var->getStorageClass() != clang::SC_None ||
      var->hasAttrs() || var->getType().hasQualifiers() ||
      !var->getType()->isRecordType() || !var->getLocation().isFileID() ||
      !match.Context->getLangOpts().CPlusPlus17)
    return false;
  const auto &sm = *match.SourceManager;
  const auto &lang = match.Context->getLangOpts();
  auto next = Lexer::findNextToken(var->getLocation(), sm, lang);
  if (next && next->is(clang::tok::equal))
    next = Lexer::findNextToken(next->getLocation(), sm, lang);
  if (next && next->is(clang::tok::l_brace)) {
    next = Lexer::findNextToken(next->getLocation(), sm, lang);
    if (!next || !next->is(clang::tok::r_brace))
      return false;
    next = Lexer::findNextToken(next->getLocation(), sm, lang);
  }
  return next && next->is(clang::tok::semi) &&
         Lexer::getLocForEndOfToken(next->getLocation(), 0, sm, lang) ==
             range.getEnd();
}

// Do not delay the destruction of an RHS temporary to the end of the new
// chain or evaluate an initializer that observes its own destination.
bool independentWrite(const clang::Stmt &stmt, const clang::VarDecl &var) {
  const clang::Expr *lhs, *rhs;
  if (const auto *op = llvm::dyn_cast<clang::BinaryOperator>(&stmt)) {
    if (op->getOpcode() != clang::BO_Assign || op->getOperatorLoc().isMacroID())
      return false;
    lhs = op->getLHS();
    rhs = op->getRHS();
  } else if (const auto *call =
                 llvm::dyn_cast<clang::CXXOperatorCallExpr>(&stmt)) {
    if (call->getOperator() != clang::OO_Equal || call->getNumArgs() != 2 ||
        call->getOperatorLoc().isMacroID())
      return false;
    lhs = call->getArg(0);
    rhs = call->getArg(1);
  } else {
    return false;
  }
  const auto *field =
      llvm::dyn_cast<clang::MemberExpr>(lhs->IgnoreParenImpCasts());
  if (!field || field->isArrow() ||
      !llvm::isa<clang::FieldDecl>(field->getMemberDecl()) ||
      field->getMemberLoc().isMacroID() || field->getOperatorLoc().isMacroID())
    return false;
  const auto *base = llvm::dyn_cast<clang::DeclRefExpr>(
      field->getBase()->IgnoreParenImpCasts());
  if (!base || base->getDecl() != &var || base->getLocation().isMacroID())
    return false;
  if (rhs->isTypeDependent() || rhs->isValueDependent())
    return false;
  llvm::SmallVector<const clang::Stmt *, 16> pending{rhs};
  while (!pending.empty()) {
    const auto *node = pending.pop_back_val();
    if (node->getBeginLoc().isMacroID() || node->getEndLoc().isMacroID() ||
        llvm::isa<clang::LambdaExpr, clang::StmtExpr, clang::ExprWithCleanups,
                  clang::CXXBindTemporaryExpr>(node))
      return false;
    if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(node))
      if (ref->getDecl() == &var)
        return false;
    for (const auto *child : node->children())
      if (child)
        pending.push_back(child);
  }
  return true;
}

bool appendTemplate(std::string &out, const ParsedTemplate &tmpl,
                    const BoundNodes &bindings, const Match &match,
                    clang::SourceLocation location) {
  auto text = evaluateTemplate(tmpl, Match(bindings, match.Context));
  if (!text) {
    auto &diags = match.Context->getDiagnostics();
    diags.Report(location,
                 diags.getCustomDiagID(clang::DiagnosticsEngine::Error,
                                       "morph: statement-subset: %0"))
        << llvm::toString(text.takeError());
    return false;
  }
  out += *text;
  return true;
}

std::vector<clang::tooling::Replacement> rewriteBlock(const Subset &rule,
                                                      const Match &match) {
  std::vector<clang::tooling::Replacement> edits;
  auto *block = match.Nodes.getNodeAs<clang::CompoundStmt>("block");
  if (!block)
    return edits;
  for (auto it = block->body_begin(); it != block->body_end(); ++it) {
    const auto &first = **it;
    auto anchors = matchChild(rule.anchor, first, *match.Context);
    if (anchors.size() != 1)
      continue;
    auto range = statementRange(first, match);
    if (!range)
      continue;
    auto begin = range->getBegin(), end = range->getEnd();
    const clang::VarDecl *local = nullptr;
    if (!rule.freshLocal.empty()) {
      local = anchors.front().getNodeAs<clang::VarDecl>(rule.freshLocal);
      if (!local) {
        auto &diags = match.Context->getDiagnostics();
        diags.Report(
            first.getBeginLoc(),
            diags.getCustomDiagID(
                clang::DiagnosticsEngine::Error,
                "morph: fresh-local '%0' must bind a variable declaration"))
            << rule.freshLocal;
        return {};
      }
      if (!freshDeclaration(first, local, *range, match))
        continue;
      begin = Lexer::getLocForEndOfToken(local->getLocation(), 0,
                                         *match.SourceManager,
                                         match.Context->getLangOpts());
    }
    std::string output;
    auto next = it + 1;
    for (; next != block->body_end(); ++next) {
      const clang::Stmt &stmt = **next;
      if (local && !independentWrite(stmt, *local))
        break;
      auto itemRange = statementRange(stmt, match);
      if (!itemRange || !adjacent(end, itemRange->getBegin(), match))
        break;
      bool matched = false;
      for (const auto &item : rule.cases) {
        auto bindings = matchChild(item.matcher, stmt, *match.Context);
        if (bindings.empty())
          continue;
        if (bindings.size() != 1)
          break;
        if (next == it + 1 &&
            !appendTemplate(output, rule.prefix, anchors.front(), match, begin))
          return {};
        if (!appendTemplate(output, item.emit, bindings.front(), match,
                            stmt.getBeginLoc()))
          return {};
        end = itemRange->getEnd();
        matched = true;
        break;
      }
      if (!matched)
        break;
    }
    if (next == it + 1)
      continue;
    if (!appendTemplate(output, rule.suffix, anchors.front(), match, begin))
      return {};
    edits.emplace_back(*match.SourceManager,
                       clang::CharSourceRange::getCharRange(begin, end), output,
                       match.Context->getLangOpts());
    it = next - 1;
  }
  return edits;
}
} // namespace

llvm::Expected<TransformRule>
buildStatementSubset(const JsonStatementSubset &spec,
                     const std::string &context) {
  if (spec.cases.empty())
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(),
        "statement-subset requires at least one case");
  auto anchor = statementMatcher(spec.anchor);
  if (!anchor)
    return anchor.takeError();
  auto prefix = parseTemplate(spec.prefix);
  if (!prefix)
    return prefix.takeError();
  auto suffix = parseTemplate(spec.suffix);
  if (!suffix)
    return suffix.takeError();
  Subset subset{std::move(*anchor),
                {},
                std::move(*prefix),
                std::move(*suffix),
                spec.freshLocal};
  for (const auto &item : spec.cases) {
    auto matcher = statementMatcher(item.matcher);
    if (!matcher)
      return matcher.takeError();
    auto emit = parseTemplate(item.emit);
    if (!emit)
      return emit.takeError();
    subset.cases.push_back({std::move(*matcher), std::move(*emit)});
  }
  // An instantiation shares its spelling with the primary template. A match
  // on one concrete T must never rewrite that spelling for every other T.
  return TransformRule{"compoundStmt(unless(isInTemplateInstantiation()))",
                       "block",
                       [rule = std::move(subset)](const Match &m) {
                         return rewriteBlock(rule, m);
                       },
                       context};
}
} // namespace vycor
