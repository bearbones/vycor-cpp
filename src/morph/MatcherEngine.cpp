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

#include "vycor/morph/MatcherEngine.h"
#include "vycor/compat/ToolAdjusters.h"

#include "clang/ASTMatchers/Dynamic/Diagnostics.h"
#include "clang/ASTMatchers/Dynamic/Parser.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Lex/PreprocessorOptions.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/MemoryBuffer.h"

namespace vycor {

namespace {

// MatchCallback implementation that delegates to a ReplacementCallback and
// accumulates replacements into a shared map.
class CallbackAdapter : public clang::ast_matchers::MatchFinder::MatchCallback {
public:
  CallbackAdapter(ReplacementCallback cb,
                  std::map<std::string, clang::tooling::Replacements> &repls,
                  std::map<llvm::sys::fs::UniqueID, std::string> &pathsByID)
      : cb_(std::move(cb)), replacements_(repls), pathsByID_(pathsByID) {}

  void
  run(const clang::ast_matchers::MatchFinder::MatchResult &result) override {
    auto &diags = result.Context->getDiagnostics();
    for (const auto &edit : cb_(result)) {
      if (!edit.isApplicable()) {
        diags.Report(diags.getCustomDiagID(clang::DiagnosticsEngine::Error,
                                           "morph: invalid replacement range"));
        return;
      }
      // Includes and input files may be relative to the TU's build directory.
      auto &fm = result.SourceManager->getFileManager();
      auto file = fm.getFileRef(edit.getFilePath());
      if (!file) {
        diags.Report(diags.getCustomDiagID(clang::DiagnosticsEngine::Error,
                                           "morph: cannot resolve file %0: %1"))
            << edit.getFilePath() << llvm::toString(file.takeError());
        return;
      }
      auto [entry, first] = pathsByID_.try_emplace(file->getUniqueID());
      if (first)
        entry->second = fm.getCanonicalName(*file).str();
      const auto &path = entry->second;
      clang::tooling::Replacement absolute(
          path, edit.getOffset(), edit.getLength(), edit.getReplacementText());
      if (auto err = replacements_[path].add(absolute)) {
        diags.Report(diags.getCustomDiagID(clang::DiagnosticsEngine::Error,
                                           "morph: replacement conflict: %0"))
            << llvm::toString(std::move(err));
        return;
      }
    }
  }

private:
  ReplacementCallback cb_;
  std::map<std::string, clang::tooling::Replacements> &replacements_;
  std::map<llvm::sys::fs::UniqueID, std::string> &pathsByID_;
};

// Remap physical FileEntries before Clang creates the SourceManager. The
// staged bytes are then visible through relative includes and symlink aliases.
class StagedAction : public clang::tooling::ToolAction {
public:
  StagedAction(clang::tooling::FrontendActionFactory &factory,
               const std::map<std::string, std::string> &staged)
      : factory_(factory), staged_(staged) {}

  bool runInvocation(std::shared_ptr<clang::CompilerInvocation> invocation,
                     clang::FileManager *files,
                     std::shared_ptr<clang::PCHContainerOperations> pch,
                     clang::DiagnosticConsumer *diagnostics) override {
    for (const auto &[path, contents] : staged_) {
      auto buffer = llvm::MemoryBuffer::getMemBuffer(contents, path);
      invocation->getPreprocessorOpts().addRemappedFile(path, buffer.release());
    }
    return factory_.runInvocation(std::move(invocation), files, std::move(pch),
                                  diagnostics);
  }

private:
  clang::tooling::FrontendActionFactory &factory_;
  const std::map<std::string, std::string> &staged_;
};

} // namespace

MatcherEngine::MatcherEngine() = default;
MatcherEngine::~MatcherEngine() = default;

std::optional<clang::ast_matchers::internal::DynTypedMatcher>
MatcherEngine::parse(const std::string &matcherExpression,
                     std::string &errorOut) {
  clang::ast_matchers::dynamic::Diagnostics diag;
  llvm::StringRef code = matcherExpression;
  auto result =
      clang::ast_matchers::dynamic::Parser::parseMatcherExpression(code, &diag);
  if (!result) {
    errorOut = diag.toStringFull();
    return std::nullopt;
  }
  return *result;
}

bool MatcherEngine::addRule(const TransformRule &rule, std::string &errorOut) {
  if (!rule.callback) {
    errorOut = "missing replacement callback";
    return false;
  }
  auto matcher = parse(rule.matcherExpression, errorOut);
  if (!matcher)
    return false;

  if (!rule.bindId.empty()) {
    auto bound = matcher->tryBind(rule.bindId);
    if (!bound) {
      errorOut = "Failed to bind matcher with id: " + rule.bindId;
      return false;
    }
    matcher = *bound;
  }

  if (!rule.context.empty()) {
    using namespace clang::ast_matchers::dynamic;
    Diagnostics diag;
    VariantValue value;
    llvm::StringRef code = rule.context;
    if (!Parser::parseExpression(code, &value, &diag)) {
      errorOut = diag.toStringFull();
      return false;
    }
    // Predicates such as isExpansionInMainFile() are polymorphic. Resolve
    // the overload against the find matcher before constructing allOf.
    auto kind = matcher->getSupportedKind();
    if (!value.isMatcher() || !value.getMatcher().hasTypedMatcher(kind)) {
      errorOut = "incompatible context '" + rule.context + "' for " +
                 rule.matcherExpression;
      return false;
    }
    using clang::ast_matchers::internal::DynTypedMatcher;
    matcher = DynTypedMatcher::constructVariadic(
        DynTypedMatcher::VO_AllOf, kind,
        {*matcher, value.getMatcher().getTypedMatcher(kind)});
  }

  auto cb = std::make_unique<CallbackAdapter>(rule.callback, replacements_,
                                              pathsByID_);
  if (!finder_.addDynamicMatcher(*matcher, cb.get())) {
    errorOut = "unsupported top-level matcher: " + rule.matcherExpression;
    return false;
  }
  callbacks_.push_back(std::move(cb));
  return true;
}

int MatcherEngine::run(const clang::tooling::CompilationDatabase &compDb,
                       const std::vector<std::string> &sourceFiles,
                       const std::map<std::string, std::string> &virtualFiles) {
  replacements_.clear();
  pathsByID_.clear();
  // Several paths can name one inode (including across passes). Keep edits
  // in a single coordinate space even when a different TU visits it first.
  for (const auto &[path, contents] : virtualFiles) {
    llvm::sys::fs::UniqueID id;
    if (auto ec = llvm::sys::fs::getUniqueID(path, id)) {
      llvm::errs() << "morph: cannot identify " << path << ": " << ec.message()
                   << "\n";
      return 1;
    }
    pathsByID_.emplace(id, path);
  }
  auto tool = vycor::makeClangTool(compDb, sourceFiles);
  auto factory = clang::tooling::newFrontendActionFactory(&finder_);
  StagedAction action(*factory, virtualFiles);
  return tool.run(&action);
}

const std::map<std::string, clang::tooling::Replacements> &
MatcherEngine::getReplacements() const {
  return replacements_;
}

} // namespace vycor
