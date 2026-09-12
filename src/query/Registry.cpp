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


#include "vycor/query/Tools.h"
#include "Registry.h"

#include <cassert>
#include <map>

namespace vycor {

const char *resultStatusName(ResultStatus status) {
  switch (status) {
  case ResultStatus::Ok: return "ok";
  case ResultStatus::Ambiguous: return "ambiguous";
  case ResultStatus::UsageError: return "usage_error";
  case ResultStatus::NotFound: return "not_found";
  case ResultStatus::Unavailable: return "unavailable";
  }
  return "ok";
}

std::optional<ResultStatus> parseResultStatus(llvm::StringRef name) {
  for (ResultStatus s : {ResultStatus::Ok, ResultStatus::Ambiguous,
                         ResultStatus::UsageError, ResultStatus::NotFound,
                         ResultStatus::Unavailable})
    if (name == resultStatusName(s))
      return s;
  return std::nullopt;
}

bool isErrorStatus(ResultStatus status) {
  return status == ResultStatus::UsageError ||
         status == ResultStatus::NotFound ||
         status == ResultStatus::Unavailable;
}

llvm::json::Value errorResult(ResultStatus kind, llvm::StringRef message) {
  assert(isErrorStatus(kind) && "errorResult needs an error status");
  llvm::json::Object obj;
  obj["error"] = message.str();
  obj["status"] = resultStatusName(kind);
  return llvm::json::Value(std::move(obj));
}

llvm::json::Value usageError(llvm::StringRef message) {
  return errorResult(ResultStatus::UsageError, message);
}

llvm::json::Value notFoundError(llvm::StringRef message) {
  return errorResult(ResultStatus::NotFound, message);
}

llvm::json::Value unavailableError(llvm::StringRef message) {
  return errorResult(ResultStatus::Unavailable, message);
}

std::optional<llvm::StringRef> errorMessage(const llvm::json::Value &result) {
  const auto *obj = result.getAsObject();
  if (!obj)
    return std::nullopt;
  return obj->getString("error");
}

bool isErrorResult(const llvm::json::Value &result) {
  return errorMessage(result).has_value();
}

bool isAmbiguousResult(const llvm::json::Value &result) {
  const auto *obj = result.getAsObject();
  if (!obj)
    return false;
  auto b = obj->getBoolean("ambiguous");
  return b && *b;
}

ResultStatus statusOf(const llvm::json::Value &result) {
  const auto *obj = result.getAsObject();
  if (!obj)
    return ResultStatus::Ok;
  if (auto name = obj->getString("status"))
    if (auto parsed = parseResultStatus(*name))
      return *parsed;
  if (obj->get("error"))
    return ResultStatus::NotFound;
  if (isAmbiguousResult(result))
    return ResultStatus::Ambiguous;
  return ResultStatus::Ok;
}

static const char *freshnessName(IndexFreshness f) {
  switch (f) {
  case IndexFreshness::Unknown: return "unknown";
  case IndexFreshness::Unchecked: return "unchecked";
  case IndexFreshness::Baked: return "baked";
  }
  return "unknown";
}

IndexFacts IndexFacts::of(const SnapshotMeta &meta, IndexFreshness freshness) {
  IndexFacts f;
  f.coverage = coverageOf(meta);
  if (!meta.provenance.environment.empty())
    f.bake = meta.provenance.environment + "@" +
             std::to_string(meta.provenance.bakeStartNs);
  f.freshness = freshness;
  f.channelsIndexed = !meta.channelTypes.empty();
  return f;
}

llvm::json::Value completeResult(llvm::json::Value payload,
                                 const ToolContext &ctx) {
  auto *obj = payload.getAsObject();
  if (!obj)
    return payload;
  (*obj)["status"] = resultStatusName(statusOf(payload));
  llvm::json::Object scope;
  if (!ctx.facts.bake.empty())
    scope["bake"] = ctx.facts.bake;
  scope["freshness"] = freshnessName(ctx.facts.freshness);
  const IndexCoverage &cov = ctx.facts.coverage;
  scope["requested"] = static_cast<int64_t>(cov.requested);
  scope["indexed"] = static_cast<int64_t>(cov.indexed);
  scope["partial"] = static_cast<int64_t>(cov.partial);
  scope["failed"] = static_cast<int64_t>(cov.failed);
  scope["complete"] = cov.complete();
  (*obj)["indexScope"] = std::move(scope);
  return payload;
}

llvm::json::Value runTool(const ToolEntry &tool, const llvm::json::Object &args,
                          const ToolContext &ctx) {
  if (!tool.handler)
    return completeResult(
        usageError(tool.name + " mutates the index and is only available "
                               "through `serve`"),
        ctx);
  return completeResult(tool.handler(args, ctx), ctx);
}

// The list-shaped member of each tool's payload (ToolEntry::recordsKey).
// Tools absent here answer with one scalar record — including query_channel,
// whose producers/consumers lists are peers (naming one would report a
// consumer-only channel as empty). Where a payload carries a primary list
// plus a secondary one (analyze_dead_code, get_class_hierarchy) the primary
// is named; the other stays in the ndjson _summary line.
// Sections beyond the graph a tool reads (ToolEntry::needs). Exception
// and lock tools walk call-site contexts; channel tools read the channel
// index; everything else, graph_summary included (it reports the header
// counts), touches only the graph.
static const std::map<std::string, unsigned> kExtraNeeds = {
    {"query_exception_safety", kSectionControlFlow},
    {"query_call_site_context", kSectionControlFlow},
    {"query_raii_scopes_at_callsite", kSectionControlFlow},
    {"query_throw_propagation", kSectionControlFlow},
    {"query_all_path_contexts", kSectionControlFlow},
    {"query_nearest_catches", kSectionControlFlow},
    {"query_locks_held", kSectionControlFlow},
    {"query_same_lock", kSectionControlFlow},
    {"list_channels", kSectionChannels},
    {"query_channel", kSectionChannels},
    {"query_channels_for_function", kSectionChannels},
    {"explain_ordering", kSectionChannels},
};

static const std::map<std::string, std::string> kRecordsKeys = {
    {"search_functions", "matches"},
    {"get_callees", "callees"},
    {"get_callers", "callers"},
    {"find_call_chain", "paths"},
    {"get_class_hierarchy", "derivedClasses"},
    {"list_entry_points", "entryPoints"},
    {"list_callback_sites", "targets"},
    {"list_concurrency_entry_points", "entries"},
    {"query_raii_scopes_at_callsite", "locals"},
    {"query_throw_propagation", "paths"},
    {"query_all_path_contexts", "paths"},
    {"query_nearest_catches", "catches"},
    {"query_locks_held", "paths"},
    {"query_same_lock", "sharedLocks"},
    {"analyze_dead_code", "dead"},
    {"list_channels", "channels"},
    {"query_channels_for_function", "sites"},
};

std::vector<std::string> sectionNames(unsigned needs) {
  std::vector<std::string> out;
  if (needs & kSectionGraph)
    out.push_back("graph");
  if (needs & kSectionControlFlow)
    out.push_back("control_flow");
  if (needs & kSectionChannels)
    out.push_back("channels");
  return out;
}

std::vector<ToolEntry> getRegisteredTools() {
  std::vector<ToolEntry> tools;
  registerGraphTools(tools);
  registerExceptionTools(tools);
  registerLockTools(tools);
  registerDeadCodeTools(tools);
  registerChannelTools(tools);
  for (auto &tool : tools) {
    auto it = kRecordsKeys.find(tool.name);
    if (it != kRecordsKeys.end())
      tool.recordsKey = it->second;
    auto needs = kExtraNeeds.find(tool.name);
    tool.needs = kSectionGraph |
                 (needs != kExtraNeeds.end() ? needs->second : 0u);
  }

  // reindex_tu — handler is null: it mutates the indexes, so each adapter
  // (McpServer today) implements it against its own owned state.
  {
    llvm::json::Object schema;
    schema["type"] = "object";
    schema["required"] = llvm::json::Array{"file"};
    llvm::json::Object props;
    props["file"] = llvm::json::Object{
        {"type", "string"},
        {"description", "Absolute path of the TU to re-index"}};
    schema["properties"] = std::move(props);

    tools.push_back({"reindex_tu",
                     "Re-index a single translation unit after source changes. "
                     "Removes stale edges/contexts and re-runs all three "
                     "analysis phases for the given file. Returns counts of "
                     "edges and contexts removed and current totals.",
                     llvm::json::Value(std::move(schema)),
                     nullptr});
    tools.back().needs = kSectionAll; // mutates every index
  }

  return tools;
}

} // namespace vycor
