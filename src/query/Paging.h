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

#include "Schema.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/JSON.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

// The shared paging contract of the list tools (docs/result-contract.md,
// "Paging"): `limit` (a per-tool default cap, at least 1) and `offset`
// select a window of the tool's record list in its canonical order
// (docs/deterministic-output.md); the payload reports `total`, `offset`,
// `limit`, `returned`, `truncated` (records exist past this window), and
// `nextOffset` (the offset of the next page, present only when truncated).

namespace vycor {

/// Per-tool default caps.
constexpr size_t kDefaultSearchLimit = 25;         // search_functions
constexpr size_t kDefaultEdgeListLimit = 200;      // get_callers, get_callees
constexpr size_t kDefaultListLimit = 200;          // the other list tools
constexpr size_t kDefaultCallbackTargetLimit = 100; // list_callback_sites
constexpr size_t kDefaultSitesPerTarget = 50;       // ... sites per target

struct Page {
  size_t offset = 0;
  size_t limit = 0;

  size_t begin(size_t total) const { return std::min(offset, total); }
  size_t end(size_t total) const {
    return begin(total) + std::min(limit, total - begin(total));
  }
  bool truncated(size_t total) const { return end(total) < total; }
};

/// Parse `limit` and `offset` into `page`, `limit` defaulting to
/// `defaultLimit`. A usage-error message when either is out of range.
inline std::optional<std::string> parsePage(const llvm::json::Object &args,
                                            size_t defaultLimit, Page &page) {
  page.limit = defaultLimit;
  page.offset = 0;
  if (auto l = args.getInteger("limit")) {
    if (*l < 1)
      return std::string("Invalid limit: must be at least 1");
    page.limit = static_cast<size_t>(*l);
  } else if (args.get("limit")) {
    return std::string("Invalid limit: must be an integer");
  }
  if (auto o = args.getInteger("offset")) {
    if (*o < 0)
      return std::string("Invalid offset: must be non-negative");
    page.offset = static_cast<size_t>(*o);
  } else if (args.get("offset")) {
    return std::string("Invalid offset: must be an integer");
  }
  return std::nullopt;
}

/// The window members without `total`/`returned`, for a payload that
/// pages several lists by one window (`truncated`: any list was cut).
inline void attachPageWindow(llvm::json::Object &obj, const Page &page,
                             bool truncated) {
  obj["offset"] = static_cast<int64_t>(page.offset);
  obj["limit"] = static_cast<int64_t>(page.limit);
  obj["truncated"] = truncated;
  if (truncated)
    obj["nextOffset"] = static_cast<int64_t>(page.offset + page.limit);
}

/// The paging members for one list of `total` records.
inline void attachPage(llvm::json::Object &obj, const Page &page,
                       size_t total) {
  obj["total"] = static_cast<int64_t>(total);
  obj["returned"] =
      static_cast<int64_t>(page.end(total) - page.begin(total));
  attachPageWindow(obj, page, page.truncated(total));
}

/// Schema properties for `limit` and `offset`; `records` names what is
/// paged ("callers", "targets").
inline void addPagingProps(llvm::json::Object &props, size_t defaultLimit,
                           llvm::StringRef records) {
  props["limit"] = intProp(
      ("Maximum " + records + " to return, at least 1 (default: " +
       std::to_string(defaultLimit) +
       "). `total` is the full count; `truncated` and `nextOffset` say "
       "whether and where the next page starts.")
          .str());
  props["offset"] = intProp(
      ("Number of " + records +
       " to skip, in the canonical order, before returning (default: 0). "
       "Pass the previous page's nextOffset to continue.")
          .str());
}

} // namespace vycor
