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

#include "clang/Tooling/CompilationDatabase.h"
#include "clang/Tooling/Core/Replacement.h"
#include "clang/Tooling/Refactoring.h"
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

} // namespace

void TransformPipeline::addPass(std::vector<TransformRule> rules) {
  passes_.push_back(std::move(rules));
}

int TransformPipeline::execute(const std::vector<std::string> &buildPaths,
                               const std::vector<std::string> &sourceFiles,
                               bool dryRun) {
  FallbackCompilationDatabase compDb;
  for (const auto &path : buildPaths) {
    std::string dbLoadError;
    auto db = clang::tooling::CompilationDatabase::loadFromDirectory(
        path, dbLoadError);
    if (!db) {
      llvm::errs() << "Error loading compilation database from " << path
                   << ": " << dbLoadError << "\n";
      return 1;
    }
    compDb.add(std::move(db));
  }

  for (auto &passRules : passes_) {
    MatcherEngine engine;
    for (auto &rule : passRules) {
      std::string error;
      if (!engine.addRule(rule, error)) {
        llvm::errs() << "Error adding rule: " << error << "\n";
        return 1;
      }
    }

    if (int ret = engine.run(compDb, sourceFiles))
      return ret;

    // Merge replacements from this pass into the overall set.
    for (auto &[file, repls] : engine.getReplacements()) {
      for (auto &r : repls) {
        if (auto err = allReplacements_[file].add(r)) {
          llvm::errs() << "Replacement merge conflict: "
                       << llvm::toString(std::move(err)) << "\n";
        }
      }
    }

    // TODO: apply replacements to files between passes when !dryRun
    // For now, replacements are just collected.
  }

  if (dryRun) {
    // Print diagnostic-style output with line:col (not raw byte offsets)
    for (auto &[file, repls] : allReplacements_) {
      // Load the file to compute line numbers from offsets
      auto buf = llvm::MemoryBuffer::getFile(file);
      llvm::StringRef content;
      if (buf)
        content = buf.get()->getBuffer();

      for (auto &r : repls) {
        llvm::outs() << file << ":";
        if (!content.empty() && r.getOffset() <= content.size()) {
          llvm::StringRef before = content.substr(0, r.getOffset());
          unsigned line = before.count('\n') + 1;
          auto lastNl = before.rfind('\n');
          unsigned col = (lastNl == llvm::StringRef::npos)
                             ? r.getOffset() + 1
                             : r.getOffset() - lastNl;
          llvm::outs() << line << ":" << col;
        } else {
          llvm::outs() << r.getOffset() << ":" << r.getLength();
        }
        llvm::outs() << ": replace with '" << r.getReplacementText() << "'\n";
      }
    }
  } else {
    // Apply replacements to disk
    for (auto &[file, repls] : allReplacements_) {
      auto buf = llvm::MemoryBuffer::getFile(file);
      if (!buf) {
        llvm::errs() << "Error reading " << file << ": "
                     << buf.getError().message() << "\n";
        return 1;
      }
      auto result =
          clang::tooling::applyAllReplacements(buf.get()->getBuffer(), repls);
      if (!result) {
        llvm::errs() << "Error applying replacements to " << file << ": "
                     << llvm::toString(result.takeError()) << "\n";
        return 1;
      }
      std::error_code EC;
      llvm::raw_fd_ostream out(file, EC);
      if (EC) {
        llvm::errs() << "Error writing " << file << ": " << EC.message()
                     << "\n";
        return 1;
      }
      out << *result;
    }
    llvm::outs() << "Applied replacements to " << allReplacements_.size()
                 << " file(s).\n";
  }

  return 0;
}

const std::map<std::string, clang::tooling::Replacements> &
TransformPipeline::getReplacements() const {
  return allReplacements_;
}

} // namespace vycor
