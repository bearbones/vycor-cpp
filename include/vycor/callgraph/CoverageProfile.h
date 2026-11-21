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

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <string>
#include <vector>

// Execution counts of a profiled run, read from `llvm-cov export` JSON
// (source-based coverage: -fprofile-instr-generate -fcoverage-mapping), and
// joined to the index by source location.
//
// Two questions, both answered from flat tables built once at load:
//
//   countAt(file, line, col)     how often the code at a location ran: the
//                                count of the last coverage segment at or
//                                before it, the way llvm-cov colours a line.
//   functionAt(file, line, col)  the function whose body contains a
//                                location, and its entry count.
//
// The index records a call at the spelling location of its first token,
// and the coverage mapping attributes a macro's body to the file that
// defines the macro, so a call written inside a macro reads the count of
// the macro body, summed over every expansion of it.
//
// File names are matched by path suffix (the longest run of trailing
// components, and only when one profile file is the best match), because
// the index spells a file as the compile command did, often relative to
// the build directory, while the profile carries absolute paths from the
// machine that ran it.

namespace vycor {

class CoverageProfile {
public:
  static constexpr int64_t kNoCount = -1;
  static constexpr uint32_t kNone = 0xffffffffu;

  /// Parse `llvm-cov export` JSON. False with `error` set when the buffer
  /// is not such a document; a document that names no file is one with no
  /// counts, not an error.
  static bool parse(llvm::StringRef buffer, CoverageProfile &out,
                    std::string &error);
  /// Read and parse `path`.
  static bool load(const std::string &path, CoverageProfile &out,
                   std::string &error);

  /// The profile file `path` names, or kNone when none or more than one
  /// matches equally well. Memoized per spelling.
  uint32_t fileFor(llvm::StringRef path) const;

  /// Resolve a whole set of index paths at once, so the match is
  /// two-sided: a profile file goes to the index paths that share the
  /// longest path suffix with it, and to none when distinct paths tie.
  /// (One-sided, a profile holding only lib/a/util.cpp would also match
  /// tools/b/util.cpp.) Later fileFor calls answer from the result.
  void bindFiles(llvm::ArrayRef<llvm::StringRef> paths) const;

  /// Execution count of the code at a location; kNoCount when the location
  /// is in no counted region (outside every function, or in code the
  /// profiled binary did not contain).
  int64_t countAt(uint32_t file, uint32_t line, uint32_t col) const noexcept;

  /// The innermost function body containing a location, or kNone.
  uint32_t functionAt(uint32_t file, uint32_t line,
                      uint32_t col) const noexcept;
  /// Entry count of a function returned by functionAt: summed over the
  /// records that share one body (the instantiations of a template).
  int64_t functionCount(uint32_t function) const noexcept {
    return functions_[function].count;
  }

  size_t fileCount() const { return fileNames_.size(); }
  /// Distinct function bodies, and those entered at least once.
  size_t functionCount() const { return functions_.size(); }
  size_t executedFunctionCount() const { return executedFunctions_; }
  const std::string &fileName(uint32_t file) const { return fileNames_[file]; }

private:
  struct Segment {
    uint32_t line;
    uint32_t col;
    int64_t count; // kNoCount: the segment ends a region
  };
  struct Function {
    uint32_t file;
    uint32_t startLine, startCol, endLine, endCol;
    // The nearest enclosing body in the same file (a lambda's function),
    // or kNone.
    uint32_t parent;
    int64_t count;
  };

  uint32_t internFile(llvm::StringRef name);
  void finish();

  std::vector<std::string> fileNames_;
  llvm::StringMap<uint32_t> fileByName_;
  // Base name -> files, for the suffix match.
  llvm::StringMap<std::vector<uint32_t>> filesByBase_;
  mutable llvm::StringMap<uint32_t> fileMemo_;

  // The best profile file for a normalized path, and how many trailing
  // components they share (UINT_MAX for an exact match).
  uint32_t match(const std::string &normal, unsigned &shared) const;

  // Segments of file f: segments_[segmentBegin_[f] .. segmentBegin_[f+1]),
  // sorted by position. Functions likewise, sorted by start.
  std::vector<Segment> segments_;
  std::vector<uint32_t> segmentBegin_;
  std::vector<Function> functions_;
  std::vector<uint32_t> functionBegin_;
  size_t executedFunctions_ = 0;
};

} // namespace vycor
