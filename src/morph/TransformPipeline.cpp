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

#include "vycor/morph/TransformPipeline.h"
#include "vycor/cli/SourceSelection.h"

#include "clang/Tooling/CompilationDatabase.h"
#include "clang/Tooling/Core/Replacement.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

namespace vycor {

namespace {

/// A CompilationDatabase that merges multiple databases with first-match-wins
/// semantics: for each source file, the first database that returns compile
/// commands is used.
class FallbackCompilationDatabase : public clang::tooling::CompilationDatabase {
public:
  void add(std::unique_ptr<clang::tooling::CompilationDatabase> db) {
    dbs_.push_back(std::move(db));
  }

  std::vector<clang::tooling::CompileCommand>
  getCompileCommands(llvm::StringRef FilePath) const override {
    for (auto &db : dbs_) {
      auto cmds = db->getCompileCommands(FilePath);
      if (!cmds.empty())
        return cmds;
    }
    return {};
  }

  std::vector<std::string> getAllFiles() const override {
    std::vector<std::string> all;
    for (auto &db : dbs_) {
      auto files = db->getAllFiles();
      all.insert(all.end(), files.begin(), files.end());
    }
    return all;
  }

  std::vector<clang::tooling::CompileCommand>
  getAllCompileCommands() const override {
    std::vector<clang::tooling::CompileCommand> all;
    for (auto &db : dbs_) {
      auto cmds = db->getAllCompileCommands();
      all.insert(all.end(), cmds.begin(), cmds.end());
    }
    return all;
  }

private:
  std::vector<std::unique_ptr<clang::tooling::CompilationDatabase>> dbs_;
};

// Replacements are ordered by original byte offset. Advance through each
// line once even when a file has many matches.
void printPreview(llvm::StringRef file, llvm::StringRef original,
                  const clang::tooling::Replacements &edits) {
  size_t line = 1, lineStart = 0;
  size_t newline = original.find('\n');
  for (const auto &edit : edits) {
    while (newline < edit.getOffset()) {
      ++line;
      lineStart = newline + 1;
      newline = original.find('\n', lineStart);
    }
    llvm::outs() << file << ":" << line << ":"
                 << edit.getOffset() - lineStart + 1 << ": replace with '"
                 << edit.getReplacementText() << "'\n";
  }
}

} // namespace

void TransformPipeline::addPass(std::vector<TransformRule> rules) {
  passes_.push_back(std::move(rules));
}

int TransformPipeline::execute(const std::vector<std::string> &buildPaths,
                               const std::vector<std::string> &sourceFiles,
                               bool dryRun,
                               const std::vector<std::string> &fixedCompileArgs,
                               bool hasFixedCompileArgs) {
  allReplacements_.clear();
  if (buildPaths.empty() && !hasFixedCompileArgs && fixedCompileArgs.empty()) {
    llvm::errs() << "Error: at least one build path or '-- <compile-flags>' is "
                    "required\n";
    return 1;
  }
  FallbackCompilationDatabase compDb;
  for (const auto &path : buildPaths) {
    std::string dbLoadError;
    auto db = loadCompilationDatabase(path, {}, dbLoadError);
    if (!db) {
      llvm::errs() << "Error loading compilation database from " << path << ": "
                   << dbLoadError << "\n";
      return 1;
    }
    compDb.add(std::move(db));
  }
  if (hasFixedCompileArgs || !fixedCompileArgs.empty()) {
    std::string dbLoadError;
    auto db = loadCompilationDatabase("", fixedCompileArgs, dbLoadError, true);
    if (!db) {
      llvm::errs() << "Error loading fixed compilation database: "
                   << dbLoadError << "\n";
      return 1;
    }
    compDb.add(std::move(db));
  }

  std::map<std::string, std::string> original, staged;
  decltype(allReplacements_) combined;
  for (size_t pass = 0; pass < passes_.size(); ++pass) {
    MatcherEngine engine;
    for (const auto &rule : passes_[pass]) {
      std::string error;
      if (!engine.addRule(rule, error)) {
        llvm::errs() << "morph: pass " << pass + 1 << ": " << error << "\n";
        return 1;
      }
    }

    if (int ret = engine.run(compDb, sourceFiles, staged))
      return ret;

    for (const auto &[file, edits] : engine.getReplacements()) {
      auto it = staged.find(file);
      if (it == staged.end()) {
        auto buf = llvm::MemoryBuffer::getFile(file);
        if (!buf) {
          llvm::errs() << "Error reading " << file << ": "
                       << buf.getError().message() << "\n";
          return 1;
        }
        original.emplace(file, (*buf)->getBuffer().str());
      }
      const auto &contents =
          it == staged.end() ? original.at(file) : it->second;
      auto rewritten = clang::tooling::applyAllReplacements(contents, edits);
      if (!rewritten) {
        llvm::errs() << "Error applying replacements to " << file << ": "
                     << llvm::toString(rewritten.takeError()) << "\n";
        return 1;
      }
      staged[file] = std::move(*rewritten);
      // This pass's offsets refer to staged text. Compose them back into
      // original-file coordinates for the preview and the library caller.
      auto [prior, first] = combined.try_emplace(file, edits);
      if (!first)
        prior->second = prior->second.merge(edits);
    }
  }

  // Check every touched file before publishing any result. Filesystem writes
  // are per file, so a later I/O failure can still leave earlier files edited.
  if (!dryRun) {
    for (const auto &[file, contents] : original) {
      auto current = llvm::MemoryBuffer::getFile(file);
      if (!current || (*current)->getBuffer() != contents) {
        llvm::errs() << "morph: input changed or became unreadable: " << file
                     << "\n";
        return 1;
      }
    }
  }
  for (const auto &[file, edits] : combined) {
    if (dryRun) {
      printPreview(file, original.at(file), edits);
      continue;
    }
    std::error_code ec;
    llvm::raw_fd_ostream out(file, ec);
    if (!ec) {
      out << staged.at(file);
      out.close();
      ec = out.error();
      out.clear_error();
    }
    if (ec) {
      llvm::errs() << "Error writing " << file << ": " << ec.message() << "\n";
      return 1;
    }
  }
  allReplacements_ = std::move(combined);
  if (!dryRun)
    llvm::outs() << "Applied replacements to " << allReplacements_.size()
                 << " file(s).\n";
  return 0;
}

const std::map<std::string, clang::tooling::Replacements> &
TransformPipeline::getReplacements() const {
  return allReplacements_;
}

} // namespace vycor
