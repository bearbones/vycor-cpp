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
#include "vycor/query/Identity.h"
#include "vycor/query/Serialize.h"
#include "Paging.h"
#include "Registry.h"
#include "Schema.h"

#include "llvm/ADT/StringRef.h"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include "vycor/ext/Extensions.h"

namespace vycor {

// ----------------------------------------------------------------------------
/// Sites in (call site, channel id, function usr) order
/// (docs/deterministic-output.md).
static void sortChannelSites(std::vector<ChannelSite> &sites) {
  std::sort(sites.begin(), sites.end(),
            [](const ChannelSite &a, const ChannelSite &b) {
              if (a.callSite != b.callSite)
                return a.callSite < b.callSite;
              if (a.channelId != b.channelId)
                return a.channelId < b.channelId;
              return a.siteFunctionUsr < b.siteFunctionUsr;
            });
}

/// The channel tools' precondition (docs/result-contract.md): a channel
/// index whose bake registered channel types. An index baked without
/// them holds no channel site, and an empty answer over it would claim
/// there are none.
static std::optional<llvm::json::Value>
channelsUnavailable(const ToolContext &ctx) {
  if (ctx.channels && ctx.facts.channelsIndexed)
    return std::nullopt;
  return unavailableError(
      "No channel facts indexed: the bake registered no channel types "
      "(--channel-types-json or an --org-config with channel types)");
}

// Tool 13: list_channels
// ----------------------------------------------------------------------------

static llvm::json::Value handleListChannels(const llvm::json::Object &args,
                                            const ToolContext &ctx) {
  if (auto err = channelsUnavailable(ctx))
    return std::move(*err);
  Page page;
  if (auto err = parsePage(args, kDefaultListLimit, page))
    return usageError(*err);
  llvm::json::Array channelsArr;
  size_t total = 0;
  if (ctx.channels) {
    // Id order: the channel map is a hash map.
    auto ids = ctx.channels->allChannelIds();
    std::sort(ids.begin(), ids.end());
    total = ids.size();
    for (size_t i = page.begin(total); i < page.end(total); ++i) {
      const auto &id = ids[i];
      auto producers = ctx.channels->producersOf(id);
      auto consumers = ctx.channels->consumersOf(id);
      llvm::json::Object entry;
      entry["channelId"] = id;
      const ChannelSite *sample =
          !producers.empty() ? &producers.front()
                             : (!consumers.empty() ? &consumers.front()
                                                    : nullptr);
      if (sample) {
        entry["channelType"] = sample->channelTypeName;
        entry["category"] = sample->category;
      }
      entry["producerCount"] = static_cast<int64_t>(producers.size());
      entry["consumerCount"] = static_cast<int64_t>(consumers.size());
      channelsArr.push_back(llvm::json::Value(std::move(entry)));
    }
  }
  llvm::json::Object obj;
  obj["count"] = static_cast<int64_t>(total);
  attachPage(obj, page, total);
  obj["channels"] = std::move(channelsArr);
  return llvm::json::Value(std::move(obj));
}

// ----------------------------------------------------------------------------
// Tool 14: query_channel
// ----------------------------------------------------------------------------

static llvm::json::Value handleQueryChannel(const llvm::json::Object &args,
                                            const ToolContext &ctx) {
  auto channelId = args.getString("channel_id");
  if (!channelId)
    return usageError("Requires 'channel_id' (from list_channels)");
  if (auto err = channelsUnavailable(ctx))
    return std::move(*err);
  Page page;
  if (auto err = parsePage(args, kDefaultListLimit, page))
    return usageError(*err);

  auto producers = ctx.channels->producersOf(channelId->str());
  auto consumers = ctx.channels->consumersOf(channelId->str());
  if (producers.empty() && consumers.empty())
    return notFoundError("No channel found with id '" + channelId->str() +
                          "'");

  // Site order: call site, then function usr (storage follows the bake's
  // TU order).
  sortChannelSites(producers);
  sortChannelSites(consumers);
  // One window pages both lists; each keeps its own total.
  llvm::json::Array producersArr, consumersArr;
  for (size_t i = page.begin(producers.size());
       i < page.end(producers.size()); ++i)
    producersArr.push_back(serializeChannelSite(producers[i]));
  for (size_t i = page.begin(consumers.size());
       i < page.end(consumers.size()); ++i)
    consumersArr.push_back(serializeChannelSite(consumers[i]));

  llvm::json::Object obj;
  obj["channelId"] = channelId->str();
  obj["producerTotal"] = static_cast<int64_t>(producers.size());
  obj["consumerTotal"] = static_cast<int64_t>(consumers.size());
  attachPageWindow(obj, page,
                   page.truncated(producers.size()) ||
                       page.truncated(consumers.size()));
  obj["producers"] = std::move(producersArr);
  obj["consumers"] = std::move(consumersArr);
  return llvm::json::Value(std::move(obj));
}

// ----------------------------------------------------------------------------
// Tool 15: query_channels_for_function
// ----------------------------------------------------------------------------

static llvm::json::Value
handleQueryChannelsForFunction(const llvm::json::Object &args,
                              const ToolContext &ctx) {
  auto function = identityName(args, "function");
  if (!function)
    return usageError("Requires 'function' (qualified name or usr)");

  if (auto err = channelsUnavailable(ctx))
    return std::move(*err);
  Page page;
  if (auto err = parsePage(args, kDefaultListLimit, page))
    return usageError(*err);

  auto sites = ctx.channels->sitesForFunction(function->str());
  // No sites is an answer only for a function the index holds; for a
  // name it does not, "no channel sites" would be a false negative.
  if (sites.empty() && !isKnownIdentity(ctx, function->str()))
    return unknownFunctionResult(ctx, "function", *function);
  sortChannelSites(sites);
  llvm::json::Array arr;
  for (size_t i = page.begin(sites.size()); i < page.end(sites.size()); ++i)
    arr.push_back(serializeChannelSite(sites[i]));
  llvm::json::Object obj;
  obj["function"] = function->str();
  obj["count"] = static_cast<int64_t>(sites.size());
  attachPage(obj, page, sites.size());
  obj["sites"] = std::move(arr);
  return llvm::json::Value(std::move(obj));
}

// ----------------------------------------------------------------------------
// Tool 16: explain_ordering — the tool aimed at the motivating case: two
// sends that land on different channels depending on a runtime guard, whose
// consumers run with no enforced relative order.
// ----------------------------------------------------------------------------

namespace {

struct ConcurrencyBoundary {
  std::string spawner;
  std::string target;
  std::string executionContext;
  std::string callSite;
};

constexpr size_t kMaxBoundaryWalkNodes = 2000;

// Bounded reverse BFS from `functionUsrOrName` over callersOf, recording
// every non-Synchronous edge encountered (a ThreadEntry/AsyncTask/
// PackagedTask/Invoke boundary) without walking further back through it.
// Reuses the execContext data CallGraph already carries on every edge
// (the same data list_concurrency_entry_points reports) rather than a new
// index — see the design note above handleExplainOrdering.
std::vector<ConcurrencyBoundary>
findConcurrencyBoundaries(const CallGraph &graph,
                          const std::string &functionUsrOrName) {
  std::vector<ConcurrencyBoundary> found;
  std::set<std::string> visited{functionUsrOrName};
  std::vector<std::string> queue{functionUsrOrName};
  size_t qi = 0;
  while (qi < queue.size() && queue.size() < kMaxBoundaryWalkNodes) {
    const std::string cur = queue[qi++];
    for (const auto &e : graph.callersOf(cur)) {
      if (e.execContext != ExecutionContext::Synchronous) {
        found.push_back({e.callerName, e.calleeName,
                         executionContextToString(e.execContext),
                         e.callSite});
        continue;
      }
      if (visited.insert(e.callerName).second)
        queue.push_back(e.callerName);
    }
  }
  return found;
}

std::optional<ChannelSite> findChannelSiteAt(const ChannelIndex &channels,
                                             const std::string &callSite) {
  for (const auto &s : channels.allSites())
    if (s.callSite == callSite)
      return s;
  return std::nullopt;
}

} // namespace

static llvm::json::Value handleExplainOrdering(const llvm::json::Object &args,
                                               const ToolContext &ctx) {
  auto siteAArg = args.getString("call_site_a");
  auto siteBArg = args.getString("call_site_b");
  if (!siteAArg || !siteBArg)
    return usageError(
        "Requires 'call_site_a' and 'call_site_b' (file:line:col, from "
        "query_channel or query_channels_for_function)");
  if (auto err = channelsUnavailable(ctx))
    return std::move(*err);

  auto siteA = findChannelSiteAt(*ctx.channels, siteAArg->str());
  auto siteB = findChannelSiteAt(*ctx.channels, siteBArg->str());
  if (!siteA)
    return notFoundError("No channel site indexed at '" + siteAArg->str() +
                          "'");
  if (!siteB)
    return notFoundError("No channel site indexed at '" + siteBArg->str() +
                          "'");

  bool sameChannel = siteA->channelId == siteB->channelId;
  auto boundariesA = findConcurrencyBoundaries(ctx.graph, siteA->siteFunctionUsr);
  auto boundariesB = findConcurrencyBoundaries(ctx.graph, siteB->siteFunctionUsr);

  auto serializeBoundaries = [](const std::vector<ConcurrencyBoundary> &bs) {
    llvm::json::Array arr;
    for (const auto &b : bs) {
      arr.push_back(llvm::json::Object{{"spawner", b.spawner},
                                       {"target", b.target},
                                       {"executionContext", b.executionContext},
                                       {"callSite", b.callSite}});
    }
    return arr;
  };

  llvm::json::Object obj;
  obj["sameChannel"] = sameChannel;
  obj["siteA"] = serializeChannelSite(*siteA);
  obj["siteB"] = serializeChannelSite(*siteB);
  obj["concurrencyBoundariesA"] = serializeBoundaries(boundariesA);
  obj["concurrencyBoundariesB"] = serializeBoundaries(boundariesB);
  obj["note"] =
      "sameChannel=false, together with differing guard conditions on "
      "siteA/siteB, proves the two sends are not guaranteed to be routed "
      "the same way. concurrencyBoundaries* lists ThreadEntry/AsyncTask/"
      "PackagedTask/Invoke edges found walking backward from each site's "
      "enclosing function toward entry points (within " +
      std::to_string(kMaxBoundaryWalkNodes) +
      " nodes). An EMPTY list is absence of evidence in the visible call "
      "graph, not proof of synchronous execution: it means no enforced "
      "ordering was found structurally, not that a race is proven.";
  return llvm::json::Value(std::move(obj));
}

void registerChannelTools(std::vector<ToolEntry> &tools) {
  // 13. list_channels
  {
    llvm::json::Object props;
    addPagingProps(props, kDefaultListLimit, "channels");
    llvm::json::Object schema;
    schema["type"] = "object";
    schema["properties"] = std::move(props);

    tools.push_back({"list_channels",
                     "List every tracked channel (queue/map/event-bus "
                     "instance registered via --channel-types-json) with "
                     "its type, category, and producer/consumer site "
                     "counts. Empty (not an error) when the server was "
                     "started without --channel-types-json. Use "
                     "query_channel on a channelId for the full site list.",
                     llvm::json::Value(std::move(schema)),
                     handleListChannels});
  }

  // 14. query_channel
  {
    llvm::json::Object props;
    props["channel_id"] = stringProp(
        "Channel identity from list_channels or a channelId in another "
        "channel tool's response.");
    addPagingProps(props, kDefaultListLimit,
                   "producers and consumers (each list)");
    llvm::json::Array req;
    req.push_back("channel_id");
    llvm::json::Object schema;
    schema["type"] = "object";
    schema["properties"] = std::move(props);
    schema["required"] = std::move(req);

    tools.push_back({"query_channel",
                     "List every producer and consumer call site on one "
                     "channel: {producers:[...], consumers:[...]}, each "
                     "site {channelType, category, operation, function, "
                     "functionUsr, callSite, guards:[{conditionText, "
                     "location, inTrueBranch, isAssertion}]}. A channel "
                     "with multiple producers/consumers lists all of them — "
                     "there is no single caller/callee edge to name. One "
                     "limit/offset window pages both lists; producerTotal "
                     "and consumerTotal are the full counts.",
                     llvm::json::Value(std::move(schema)),
                     handleQueryChannel});
  }

  // 15. query_channels_for_function
  {
    llvm::json::Object props;
    props["function"] = stringProp(
        "Qualified function name or USR whose channel producer/consumer "
        "call sites to list. Alias: 'name'.");
    addPagingProps(props, kDefaultListLimit, "sites");
    llvm::json::Array req;
    req.push_back("function");
    llvm::json::Object schema;
    schema["type"] = "object";
    schema["properties"] = std::move(props);
    schema["required"] = std::move(req);

    tools.push_back({"query_channels_for_function",
                     "List the channel sites (producer or consumer) inside "
                     "one function. Use this when you already know a "
                     "function pushes/pops on some tracked channel but not "
                     "which channel or where its counterpart is.",
                     llvm::json::Value(std::move(schema)),
                     handleQueryChannelsForFunction});
  }

  // 16. explain_ordering
  {
    llvm::json::Object props;
    props["call_site_a"] = stringProp(
        "First channel call site, 'file:line:col' (from query_channel or "
        "query_channels_for_function).");
    props["call_site_b"] = stringProp("Second channel call site.");
    llvm::json::Array req;
    req.push_back("call_site_a");
    req.push_back("call_site_b");
    llvm::json::Object schema;
    schema["type"] = "object";
    schema["properties"] = std::move(props);
    schema["required"] = std::move(req);

    tools.push_back(
        {"explain_ordering",
         "Compare two channel call sites for the evidence needed to explain "
         "an ordering assumption between them: whether they're the same "
         "channel, each site's enclosing guard conditions (e.g. only "
         "reachable when some flag is true), and which ThreadEntry/"
         "AsyncTask/PackagedTask/Invoke boundaries reach each site's "
         "enclosing function. Returns structural evidence for a human/LLM "
         "to build the explanation from — an empty concurrencyBoundaries "
         "list is absence of evidence, not proof of synchronous execution.",
         llvm::json::Value(std::move(schema)), handleExplainOrdering});
  }
}

} // namespace vycor
