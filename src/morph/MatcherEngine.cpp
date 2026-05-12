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
#include "vycor/compat/ClangVersion.h"
#include "vycor/compat/ToolAdjusters.h"

#include "clang/ASTMatchers/Dynamic/Diagnostics.h"
#include "clang/ASTMatchers/Dynamic/Parser.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/raw_ostream.h"

namespace vycor {

namespace {

// MatchCallback implementation that delegates to a ReplacementCallback and
// accumulates replacements into a shared map.
class CallbackAdapter : public clang::ast_matchers::MatchFinder::MatchCallback {
public:
  CallbackAdapter(ReplacementCallback cb,
                  std::map<std::string, clang::tooling::Replacements> &repls)
      : cb_(std::move(cb)), replacements_(repls) {}

  void run(const clang::ast_matchers::MatchFinder::MatchResult &result) override {
    auto newRepls = cb_(result);
    for (auto &r : newRepls) {
      auto &fileRepls = replacements_[std::string(r.getFilePath())];
      if (auto err = fileRepls.add(r)) {
        llvm::errs() << "Replacement conflict: " << llvm::toString(std::move(err))
                      << "\n";
      }
    }
  }

private:
  ReplacementCallback cb_;
  std::map<std::string, clang::tooling::Replacements> &replacements_;
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

  auto cb =
      std::make_unique<CallbackAdapter>(rule.callback, replacements_);
  finder_.addDynamicMatcher(*matcher, cb.get());
  callbacks_.push_back(std::move(cb));
  return true;
}

int MatcherEngine::run(const clang::tooling::CompilationDatabase &compDb,
                       const std::vector<std::string> &sourceFiles) {
  auto tool = vycor::makeClangTool(compDb, sourceFiles);
  return tool.run(
      clang::tooling::newFrontendActionFactory(&finder_).get());
}

const std::map<std::string, clang::tooling::Replacements> &
MatcherEngine::getReplacements() const {
  return replacements_;
}

} // namespace vycor
