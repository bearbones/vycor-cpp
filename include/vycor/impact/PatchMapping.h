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

#include "vycor/callgraph/CallGraph.h"

#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <string>
#include <vector>

// Mapping changed source ranges to indexed functions (docs/change-impact.md,
// "Impact"). The unified-diff parser is text in, ranges out; running
// `git diff` is the CLI adapter's job (cli/MegascopeCli.cpp), never this
// module's.

namespace vycor {

/// One changed range of a file, on the after side of the patch. A pure
/// deletion has no after-side lines; it is recorded at the hunk's
/// after-side position with `deletionOnly` set.
struct PatchRange {
  std::string file; // as spelled by the patch, "a/" or "b/" prefix removed
  unsigned firstLine = 0;
  unsigned lastLine = 0;
  bool deletionOnly = false;
};

/// Parse a unified diff (`git diff`, `diff -u`) into after-side ranges,
/// in patch order. Files deleted by the patch (`+++ /dev/null`) yield
/// deletion-only ranges under their before-side name. Combined diffs and
/// binary patches contribute nothing.
std::vector<PatchRange> parseUnifiedDiff(llvm::StringRef text);

/// How a range was attributed to a function, most precise first.
enum class MappingVia : uint8_t {
  Argument,   // named by the caller
  CallSite,   // an indexed call site lies in the range
  Definition, // the function's recorded location lies in the range
  Extent,     // the range lies between the function's location and the
              // next recorded function of the file (an estimate: the
              // index knows where a function starts, not where it ends)
  File,       // the range precedes every function of the file: each of
              // the file's functions is a candidate
  Diff,       // taken from a semantic diff
};

/// "argument", "call_site", "definition", "extent", "file", "diff".
const char *mappingViaName(MappingVia v);

struct MappedFunction {
  std::string usr;
  std::string name;
  std::string file;
  unsigned line = 0;
  MappingVia via = MappingVia::Argument;
};

struct UnmappedRange {
  PatchRange range;
  std::string reason;
};

struct PatchMapping {
  /// Sorted by usr; a function reached by several ranges keeps its most
  /// precise `via`.
  std::vector<MappedFunction> functions;
  /// In (file, first line) order.
  std::vector<UnmappedRange> unmapped;
  size_t viaCallSite = 0, viaDefinition = 0, viaExtent = 0, viaFile = 0;
};

/// Attribute every range to indexed functions. Patch paths are matched to
/// the index's file spellings (as the compile commands spelled them) by
/// equality or path-component suffix, after `patchRoot` (a directory, may
/// be empty) is prepended; a patch path matching several indexed files
/// is unmapped. The file index (every node location and every edge's
/// call site, per file) is built from the whole graph on each call.
PatchMapping mapRangesToFunctions(const CallGraph &graph,
                                  const std::vector<PatchRange> &ranges,
                                  llvm::StringRef patchRoot = "");

} // namespace vycor
