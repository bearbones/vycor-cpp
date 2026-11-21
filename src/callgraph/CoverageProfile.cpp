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
#include "vycor/callgraph/CoverageProfile.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"

#include <algorithm>
#include <tuple>

namespace vycor {

namespace {

// Path with `.` components and redundant separators removed; `..` is
// kept, since what it climbs out of is not known for a relative path.
std::string normalizedPath(llvm::StringRef path) {
  llvm::SmallString<256> out(path);
  llvm::sys::path::remove_dots(out, /*remove_dot_dot=*/false);
  return std::string(out);
}

// Number of trailing path components `a` and `b` share.
unsigned commonSuffix(llvm::StringRef a, llvm::StringRef b) {
  unsigned shared = 0;
  auto ai = llvm::sys::path::rbegin(a), ae = llvm::sys::path::rend(a);
  auto bi = llvm::sys::path::rbegin(b), be = llvm::sys::path::rend(b);
  for (; ai != ae && bi != be && *ai == *bi; ++ai, ++bi)
    ++shared;
  return shared;
}

bool asUint(const llvm::json::Value &value, uint32_t &out) {
  auto n = value.getAsInteger();
  if (!n || *n < 0 || *n > 0x7fffffff)
    return false;
  out = static_cast<uint32_t>(*n);
  return true;
}

// A count as llvm-cov writes it: an integer, or a double once it no
// longer fits one.
bool asCount(const llvm::json::Value &value, int64_t &out) {
  if (auto n = value.getAsInteger()) {
    out = std::max<int64_t>(*n, 0);
    return true;
  }
  if (auto d = value.getAsNumber()) {
    out = *d < 0 ? 0 : *d >= 9.2e18 ? INT64_MAX : static_cast<int64_t>(*d);
    return true;
  }
  return false;
}

} // namespace

uint32_t CoverageProfile::internFile(llvm::StringRef name) {
  std::string normal = normalizedPath(name);
  auto [it, inserted] =
      fileByName_.try_emplace(normal, static_cast<uint32_t>(fileNames_.size()));
  if (inserted) {
    filesByBase_[llvm::sys::path::filename(normal)].push_back(it->second);
    fileNames_.push_back(std::move(normal));
  }
  return it->second;
}

bool CoverageProfile::parse(llvm::StringRef buffer, CoverageProfile &out,
                            std::string &error) {
  out = CoverageProfile();
  auto parsed = llvm::json::parse(buffer);
  if (!parsed) {
    error = "not JSON: " + llvm::toString(parsed.takeError());
    return false;
  }
  const llvm::json::Object *root = parsed->getAsObject();
  const llvm::json::Array *data = root ? root->getArray("data") : nullptr;
  if (!root || !data ||
      root->getString("type").value_or("") != "llvm.coverage.json.export") {
    error = "not an `llvm-cov export` document (expected "
            "\"type\": \"llvm.coverage.json.export\" and a \"data\" array)";
    return false;
  }

  // Segments are collected per file as (file, position, count) and sorted
  // once; one document may list a file under several `data` entries.
  struct RawSegment {
    uint32_t file, line, col;
    int64_t count;
  };
  std::vector<RawSegment> rawSegments;
  bool anyFunctions = false;

  for (const llvm::json::Value &entryValue : *data) {
    const llvm::json::Object *entry = entryValue.getAsObject();
    if (!entry)
      continue;
    if (const llvm::json::Array *files = entry->getArray("files")) {
      for (const llvm::json::Value &fileValue : *files) {
        const llvm::json::Object *file = fileValue.getAsObject();
        auto name = file ? file->getString("filename") : std::nullopt;
        if (!name)
          continue;
        const uint32_t id = out.internFile(*name);
        const llvm::json::Array *segments = file->getArray("segments");
        if (!segments)
          continue;
        for (const llvm::json::Value &segmentValue : *segments) {
          // [line, col, count, hasCount, isRegionEntry, isGapRegion]
          const llvm::json::Array *segment = segmentValue.getAsArray();
          RawSegment raw{id, 0, 0, kNoCount};
          int64_t count = 0;
          if (!segment || segment->size() < 4 ||
              !asUint((*segment)[0], raw.line) ||
              !asUint((*segment)[1], raw.col) ||
              !asCount((*segment)[2], count)) {
            error = "malformed segment in " + name->str();
            return false;
          }
          if ((*segment)[3].getAsBoolean().value_or(false))
            raw.count = count;
          rawSegments.push_back(raw);
        }
      }
    }
    if (const llvm::json::Array *functions = entry->getArray("functions")) {
      anyFunctions = true;
      for (const llvm::json::Value &functionValue : *functions) {
        const llvm::json::Object *function = functionValue.getAsObject();
        const llvm::json::Array *names =
            function ? function->getArray("filenames") : nullptr;
        const llvm::json::Array *regions =
            function ? function->getArray("regions") : nullptr;
        if (!names || !regions || regions->empty())
          continue;
        // regions[0] is the body: [lineStart, colStart, lineEnd, colEnd,
        // count, fileId, expandedFileId, kind].
        const llvm::json::Array *body = (*regions)[0].getAsArray();
        Function fn{};
        uint32_t fileId = 0;
        if (!body || body->size() < 6 || !asUint((*body)[0], fn.startLine) ||
            !asUint((*body)[1], fn.startCol) ||
            !asUint((*body)[2], fn.endLine) ||
            !asUint((*body)[3], fn.endCol) || !asUint((*body)[5], fileId) ||
            fileId >= names->size()) {
          error = "malformed function record";
          return false;
        }
        auto fileName = (*names)[fileId].getAsString();
        const llvm::json::Value *count = function->get("count");
        if (!fileName || !count || !asCount(*count, fn.count)) {
          error = "malformed function record";
          return false;
        }
        fn.file = out.internFile(*fileName);
        fn.parent = kNone;
        out.functions_.push_back(fn);
      }
    }
  }
  if (!out.fileNames_.empty() && !anyFunctions) {
    error = "the document has no per-function data (exported with "
            "-summary-only or -skip-functions?)";
    return false;
  }

  const uint32_t fileCount = static_cast<uint32_t>(out.fileNames_.size());
  std::sort(rawSegments.begin(), rawSegments.end(),
            [](const RawSegment &a, const RawSegment &b) {
              return std::tie(a.file, a.line, a.col) <
                     std::tie(b.file, b.line, b.col);
            });
  out.segments_.reserve(rawSegments.size());
  out.segmentBegin_.assign(fileCount + 1, 0);
  for (const RawSegment &raw : rawSegments) {
    out.segments_.push_back({raw.line, raw.col, raw.count});
    ++out.segmentBegin_[raw.file + 1];
  }
  for (uint32_t f = 0; f < fileCount; ++f)
    out.segmentBegin_[f + 1] += out.segmentBegin_[f];
  out.finish();
  return true;
}

// Sort the function bodies, merge the records that share one (the
// instantiations of a template each have a record), and link every body to
// the one enclosing it.
void CoverageProfile::finish() {
  auto start = [](const Function &f) {
    return std::tie(f.file, f.startLine, f.startCol);
  };
  // Outer bodies before the ones they contain: same start, later end first.
  std::sort(functions_.begin(), functions_.end(),
            [&](const Function &a, const Function &b) {
              if (start(a) != start(b))
                return start(a) < start(b);
              return std::tie(a.endLine, a.endCol) >
                     std::tie(b.endLine, b.endCol);
            });
  size_t kept = 0;
  for (size_t i = 0; i < functions_.size(); ++i) {
    Function &last = functions_[kept ? kept - 1 : 0];
    const Function &fn = functions_[i];
    if (kept && start(last) == start(fn) && last.endLine == fn.endLine &&
        last.endCol == fn.endCol) {
      last.count = last.count > INT64_MAX - fn.count ? INT64_MAX
                                                     : last.count + fn.count;
      continue;
    }
    functions_[kept++] = fn;
  }
  functions_.resize(kept);

  const uint32_t fileCount = static_cast<uint32_t>(fileNames_.size());
  functionBegin_.assign(fileCount + 1, 0);
  for (const Function &fn : functions_)
    ++functionBegin_[fn.file + 1];
  for (uint32_t f = 0; f < fileCount; ++f)
    functionBegin_[f + 1] += functionBegin_[f];

  auto contains = [](const Function &outer, const Function &inner) {
    return outer.file == inner.file &&
           std::tie(outer.endLine, outer.endCol) >=
               std::tie(inner.endLine, inner.endCol);
  };
  for (uint32_t i = 0; i < functions_.size(); ++i) {
    // The previous body, or the nearest of its ancestors, that reaches
    // past this one's end.
    uint32_t candidate = i ? i - 1 : kNone;
    while (candidate != kNone && !contains(functions_[candidate], functions_[i]))
      candidate = functions_[candidate].parent;
    functions_[i].parent = candidate;
    executedFunctions_ += functions_[i].count > 0;
  }
}

bool CoverageProfile::load(const std::string &path, CoverageProfile &out,
                           std::string &error) {
  auto buffer = llvm::MemoryBuffer::getFile(path);
  if (!buffer) {
    error = "cannot read " + path + ": " + buffer.getError().message();
    return false;
  }
  if (!parse(buffer.get()->getBuffer(), out, error)) {
    error = path + ": " + error;
    return false;
  }
  return true;
}

uint32_t CoverageProfile::fileFor(llvm::StringRef path) const {
  auto memo = fileMemo_.find(path);
  if (memo != fileMemo_.end())
    return memo->second;
  const std::string normal = normalizedPath(path);
  uint32_t best = kNone;
  auto exact = fileByName_.find(normal);
  if (exact != fileByName_.end()) {
    best = exact->second;
  } else {
    auto candidates = filesByBase_.find(llvm::sys::path::filename(normal));
    if (candidates != filesByBase_.end()) {
      unsigned bestShared = 0;
      bool tied = false;
      for (uint32_t file : candidates->second) {
        const unsigned shared = commonSuffix(normal, fileNames_[file]);
        if (shared > bestShared) {
          bestShared = shared;
          best = file;
          tied = false;
        } else if (shared == bestShared) {
          tied = true;
        }
      }
      if (tied)
        best = kNone;
    }
  }
  fileMemo_.try_emplace(path, best);
  return best;
}

int64_t CoverageProfile::countAt(uint32_t file, uint32_t line,
                                 uint32_t col) const noexcept {
  const Segment *first = segments_.data() + segmentBegin_[file];
  const Segment *last = segments_.data() + segmentBegin_[file + 1];
  // The last segment starting at or before the location.
  const Segment *after = std::upper_bound(
      first, last, std::make_pair(line, col),
      [](const std::pair<uint32_t, uint32_t> &at, const Segment &segment) {
        return at < std::make_pair(segment.line, segment.col);
      });
  return after == first ? kNoCount : (after - 1)->count;
}

uint32_t CoverageProfile::functionAt(uint32_t file, uint32_t line,
                                     uint32_t col) const noexcept {
  const Function *base = functions_.data();
  const Function *first = base + functionBegin_[file];
  const Function *last = base + functionBegin_[file + 1];
  const auto at = std::make_pair(line, col);
  const Function *after = std::upper_bound(
      first, last, at,
      [](const std::pair<uint32_t, uint32_t> &where, const Function &fn) {
        return where < std::make_pair(fn.startLine, fn.startCol);
      });
  if (after == first)
    return kNone;
  // The last body starting at or before the location, or the nearest of
  // its ancestors that has not ended yet.
  uint32_t candidate = static_cast<uint32_t>(after - 1 - base);
  while (candidate != kNone &&
         std::make_pair(base[candidate].endLine, base[candidate].endCol) < at)
    candidate = base[candidate].parent;
  return candidate;
}

} // namespace vycor
