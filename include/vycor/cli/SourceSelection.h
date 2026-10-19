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

#include "clang/Tooling/CompilationDatabase.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <istream>
#include <memory>
#include <string>
#include <vector>

// ============================================================================
// TU selection for the bake (docs/megascope-cli-review.md §2.4). Replaces
// the "write a Python snippet over compile_commands.json" step:
//
//   (no selection flag)  the TU set recorded in the existing index, or
//                        every C/C++ entry of the compilation database
//                        when there is no index yet
//   --source F ...       explicit files
//   --source-list FILE   one path per line ("-" = stdin; '#' comments)
//   --source-re REGEX    keep only paths matching (POSIX ERE, searched)
//   --skip-paths P ...   drop paths under a directory component (the
//                        existing CollapseFilter semantics)
//
// Explicit files and list entries are unioned (in order, deduplicated).
// Any of --source / --source-list / --source-re resolves against the
// database (`--source-re .` re-selects everything); only a bare
// invocation reuses the recorded set, so a narrow index is never widened
// by accident. The regex and the skip filter narrow whatever the base
// set is. Every path is made absolute with `.`/`..` removed before
// deduplication and filtering, so a relative list entry, the database
// spelling, and the stamps in the index agree.
// ============================================================================

namespace vycor {

struct SourceSelection {
  std::vector<std::string> explicitFiles; // --source
  std::string listFile;                   // --source-list ("-" = stdin)
  std::string regex;                      // --source-re
  std::vector<std::string> skipPaths;     // --skip-paths
  /// TU paths recorded in an existing index. Used as the base set only
  /// when explicitFiles, listFile, and regex are all empty.
  std::vector<std::string> recordedFiles;
};

struct SourceSelectionStats {
  size_t base = 0;        // candidates before filtering
  size_t regexDropped = 0;
  size_t skipDropped = 0;
  /// Database entries dropped from the default-all base set: not a
  /// C/C++ source by extension (assembly, resources) or no longer on disk.
  size_t dbSkipped = 0;
  const char *baseSource = ""; // "database", "index", "source", ...
};

/// Resolve `sel` against `db`. `stdinFor` backs `--source-list -`.
/// Errors: unreadable list file, invalid regex, a database that cannot
/// enumerate its files (compile_flags.txt) when nothing else names them.
/// An empty result is not an error here (the caller decides what "nothing
/// to index" means).
llvm::Expected<std::vector<std::string>>
selectSources(const clang::tooling::CompilationDatabase &db,
              const SourceSelection &sel, std::istream &stdinFor,
              SourceSelectionStats *stats = nullptr);

/// Load a compilation database for the CLI (`megascope index`, `check`,
/// `morph`, and ephemeral `megascope <tool> --source ...`):
///   1. When `hasFixedArgs` is true or `fixedArgs` (from
///      `-- [compile-flags...]` on the command line) is non-empty, returns
///      a `FixedCompilationDatabase` rooted at `buildPath` (or "." when
///      `buildPath` is empty) using those flags.
///   2. When `buildPath` is a regular file on disk, loads it directly via
///      `JSONCompilationDatabase::loadFromFile` (so build systems can pass
///      a target-specific `.json` file without wrapping it in a directory
///      named `compile_commands.json`).
///   3. Otherwise, loads from the directory `buildPath` via
///      `CompilationDatabase::loadFromDirectory`.
std::unique_ptr<clang::tooling::CompilationDatabase>
loadCompilationDatabase(llvm::StringRef buildPath,
                        llvm::ArrayRef<std::string> fixedArgs,
                        std::string &error, bool hasFixedArgs = false);

} // namespace vycor
