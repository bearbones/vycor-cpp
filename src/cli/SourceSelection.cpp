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

#include "vycor/cli/SourceSelection.h"
#include "vycor/callgraph/CollapseFilter.h"

#include "clang/Tooling/JSONCompilationDatabase.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/LineIterator.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Regex.h"

#include <algorithm>
#include <optional>
#include <unordered_set>

namespace vycor {

namespace {

llvm::Error selectionError(const llvm::Twine &msg) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), msg);
}

/// Absolute, `.`/`..` removed — the spelling the compilation database and
/// the index stamps use, so dedupe and the filters see one name per TU.
std::string canonical(llvm::StringRef path) {
  llvm::SmallString<256> p(path);
  llvm::sys::fs::make_absolute(p);
  llvm::sys::path::remove_dots(p, /*remove_dot_dot=*/true);
  return std::string(p.str());
}

void appendUnique(std::vector<std::string> &out,
                  std::unordered_set<std::string> &seen,
                  llvm::StringRef path) {
  std::string c = canonical(path);
  if (seen.insert(c).second)
    out.push_back(std::move(c));
}

/// One path per line; blank lines and '#' comments ignored; surrounding
/// whitespace trimmed.
void appendListLine(llvm::StringRef line, std::vector<std::string> &out,
                    std::unordered_set<std::string> &seen) {
  llvm::StringRef s = line.trim();
  if (!s.empty() && !s.starts_with("#"))
    appendUnique(out, seen, s);
}

/// A compilation database can carry assembly, resource, and since-deleted
/// entries; the default-all base set keeps only what the bake can parse.
bool isCppSourceOnDisk(llvm::StringRef path) {
  static const char *const kExts[] = {".c",  ".cc", ".cp",  ".cpp", ".cxx",
                                      ".c++", ".C", ".m",   ".mm",  ".cu"};
  llvm::StringRef ext = llvm::sys::path::extension(path);
  bool known = std::any_of(std::begin(kExts), std::end(kExts),
                           [&](const char *e) { return ext == e; });
  return known && llvm::sys::fs::is_regular_file(path);
}

} // namespace

llvm::Expected<std::vector<std::string>>
selectSources(const clang::tooling::CompilationDatabase &db,
              const SourceSelection &sel, std::istream &stdinFor,
              SourceSelectionStats *stats) {
  // Fail fast on a bad regex before reading any list or the database.
  std::optional<llvm::Regex> re;
  if (!sel.regex.empty()) {
    re.emplace(sel.regex);
    std::string reError;
    if (!re->isValid(reError))
      return selectionError("invalid --source-re '" + sel.regex + "': " +
                            reError);
  }

  std::vector<std::string> files;
  std::unordered_set<std::string> seen;
  const char *baseSource = "database";
  size_t dbSkipped = 0;

  for (const auto &f : sel.explicitFiles)
    appendUnique(files, seen, f);
  if (!sel.listFile.empty()) {
    if (sel.listFile == "-") {
      std::string line;
      while (std::getline(stdinFor, line))
        appendListLine(line, files, seen);
    } else {
      auto buf = llvm::MemoryBuffer::getFile(sel.listFile);
      if (!buf)
        return selectionError("cannot read --source-list " + sel.listFile +
                              ": " + buf.getError().message());
      for (llvm::line_iterator it(**buf, /*SkipBlanks=*/true, '#');
           !it.is_at_end(); ++it)
        appendListLine(*it, files, seen);
    }
  }

  if (!sel.explicitFiles.empty() && !sel.listFile.empty()) {
    baseSource = "source + source-list";
  } else if (!sel.explicitFiles.empty()) {
    baseSource = "source";
  } else if (!sel.listFile.empty()) {
    baseSource = "source-list";
  } else if (!re && !sel.recordedFiles.empty()) {
    baseSource = "index";
    for (const auto &f : sel.recordedFiles)
      appendUnique(files, seen, f);
  } else {
    auto all = db.getAllFiles();
    if (all.empty())
      return selectionError(
          "the compilation database lists no files (a compile_flags.txt "
          "database cannot be enumerated); pass --source or --source-list");
    for (const auto &f : all) {
      if (isCppSourceOnDisk(f))
        appendUnique(files, seen, f);
      else
        ++dbSkipped;
    }
    // getAllFiles() is hash order; the bake and the index should not
    // depend on it.
    std::sort(files.begin(), files.end());
  }

  if (stats) {
    stats->base = files.size();
    stats->baseSource = baseSource;
    stats->dbSkipped = dbSkipped;
  }

  if (re) {
    size_t before = files.size();
    files.erase(std::remove_if(files.begin(), files.end(),
                               [&](const std::string &f) {
                                 return !re->match(f);
                               }),
                files.end());
    if (stats)
      stats->regexDropped = before - files.size();
  }

  if (!sel.skipPaths.empty()) {
    CollapseFilter skip(sel.skipPaths);
    size_t before = files.size();
    files.erase(std::remove_if(files.begin(), files.end(),
                               [&](const std::string &f) {
                                 return skip.isCollapsed(f);
                               }),
                files.end());
    if (stats)
      stats->skipDropped = before - files.size();
  }
  return files;
}

std::unique_ptr<clang::tooling::CompilationDatabase>
loadCompilationDatabase(llvm::StringRef buildPath,
                        llvm::ArrayRef<std::string> fixedArgs,
                        std::string &error, bool hasFixedArgs) {
  if (hasFixedArgs || !fixedArgs.empty()) {
    llvm::StringRef dir = buildPath.empty() ? llvm::StringRef(".") : buildPath;
    // Strip object-emission and depfile-generation flags (-c, -o <out>,
    // -MD/-MMD/-MP/-MF <dep>/-MT <target>/-MQ <target>) in case a build
    // rule forwards its compile command flags verbatim after `--`.
    std::vector<std::string> cleaned;
    cleaned.reserve(fixedArgs.size());
    for (size_t i = 0; i < fixedArgs.size(); ++i) {
      llvm::StringRef arg = fixedArgs[i];
      if (arg == "-c" || arg == "-M" || arg == "-MM" || arg == "-MD" ||
          arg == "-MMD" || arg == "-MP" || arg == "-MG")
        continue;
      if (arg == "-o" || arg == "-MF" || arg == "-MT" || arg == "-MQ") {
        if (i + 1 < fixedArgs.size())
          ++i; // skip the output/depfile/target argument too
        continue;
      }
      if ((arg.starts_with("-o") && arg.size() > 2) ||
          (arg.starts_with("-MF") && arg.size() > 3) ||
          (arg.starts_with("-MT") && arg.size() > 3) ||
          (arg.starts_with("-MQ") && arg.size() > 3))
        continue;
      cleaned.push_back(fixedArgs[i]);
    }
    return std::make_unique<clang::tooling::FixedCompilationDatabase>(dir,
                                                                      cleaned);
  }
  if (buildPath.empty()) {
    error = "--build-path is required (or pass compiler flags after '--')";
    return nullptr;
  }
  if (llvm::sys::fs::is_regular_file(buildPath)) {
    return clang::tooling::JSONCompilationDatabase::loadFromFile(
        buildPath, error, clang::tooling::JSONCommandLineSyntax::AutoDetect);
  }
  return clang::tooling::CompilationDatabase::loadFromDirectory(buildPath,
                                                                error);
}

} // namespace vycor
