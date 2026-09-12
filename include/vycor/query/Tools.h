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
#include "vycor/callgraph/ChannelIndex.h"
#include "vycor/callgraph/ControlFlowIndex.h"
#include "vycor/callgraph/ControlFlowOracle.h"
#include "vycor/callgraph/Snapshot.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/JSON.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Transport-neutral query tools over the baked indexes. Every tool is a
// pure function (args, context) -> JSON payload; the MCP server
// (vycor/mcp/) and the CLI are thin adapters that own the indexes and
// translate this contract onto their wire format.

namespace vycor {

/// Whole-graph query results cached across tool calls. Owned by the adapter
/// (McpServer, the CLI batch loop) and cleared wholesale whenever the
/// indexes mutate (reindex_tu), so a cached value is always consistent with
/// the graph it was computed from. Used by handlers whose cost scales with
/// the whole graph rather than the query (analyze_dead_code reruns full
/// liveness; graph_summary materializes calleesOf for every node).
struct QueryCache {
  // Final JSON results (argument-independent queries, e.g. graph_summary).
  std::map<std::string, llvm::json::Value> byKey;
  // Typed intermediate results shared across argument variations (e.g. the
  // dead-code liveness map, reused by every pagination/filter combination).
  std::map<std::string, std::shared_ptr<void>> objects;

  void clear() {
    byKey.clear();
    objects.clear();
  }
};

/// Whether anyone compared the indexes with the sources
/// (docs/result-contract.md, `indexScope.freshness`).
enum class IndexFreshness : uint8_t {
  Unknown,   // the adapter did not say (a handler called directly)
  Unchecked, // a saved index loaded as-is; no comparison was made
  Baked,     // baked from the sources by this process (ephemeral, serve)
};

/// The producer-side facts a result cites about its index: which bake
/// wrote it, whether it was checked, and what it covers of the requested
/// TU set (docs/index-provenance.md). Carried on every tool payload as
/// `indexScope` by completeResult.
struct IndexFacts {
  IndexCoverage coverage;
  /// "<environment fingerprint>@<bake_start_ns>", or empty when there is
  /// no saved bake to cite (the ephemeral mode).
  std::string bake;
  IndexFreshness freshness = IndexFreshness::Unknown;

  /// Facts of a loaded or in-memory meta: coverageOf(meta) and the
  /// provenance reference (empty when the meta records no bake).
  static IndexFacts of(const SnapshotMeta &meta, IndexFreshness freshness);
};

/// Context passed to every tool handler.
struct ToolContext {
  const CallGraph &graph;
  const ControlFlowOracle &oracle;
  const ControlFlowIndex &cfIndex;
  const std::vector<std::string> &entryPoints;
  /// Channel/data-flow index (list_channels, query_channel,
  /// query_channels_for_function, explain_ordering). Null when the adapter
  /// was started without a --channel-types-json config — trailing default
  /// so every existing positional ToolContext{...} call site (this repo
  /// has 30+, mostly in tests) keeps compiling unchanged; channel tool
  /// handlers must null-check it themselves.
  const ChannelIndex *channels = nullptr;
  /// Optional whole-graph result cache; null in contexts that do not want
  /// caching (handlers must treat it as best-effort).
  QueryCache *cache = nullptr;
  /// Counts from the index header (v8). Set by the one-shot query verbs,
  /// which may not have decoded the control-flow or channel sections;
  /// graph_summary reports these instead of the live index sizes when
  /// present. Null under serve, where the live sizes are authoritative.
  const IndexSummary *summary = nullptr;
  /// Where the indexes came from and how much of the requested scope
  /// they hold (docs/result-contract.md). Set by the adapter that owns
  /// the indexes; the default (vacuous coverage, freshness unknown) is
  /// what a handler called directly, as the unit tests do, sees.
  IndexFacts facts;
};

/// Signature for a tool handler function.
///
/// Result contract (docs/result-contract.md, shared by every transport):
///   - success: the payload object itself, no envelope;
///   - error:   `{"error": "<message>", "status": "<kind>"}` built with
///              usageError / notFoundError / unavailableError (or
///              errorResult with an explicit ResultStatus). The kind, not
///              the message, decides the exit code and the MCP isError
///              flag; message text is free-form;
///   - ambiguous identity: a non-error payload with `"ambiguous": true` and
///     a `candidates` list (see isAmbiguousResult and Identity.h).
/// Adapters run handlers through runTool, which stamps `status` and
/// `indexScope` onto whatever the handler returned (completeResult).
using ToolHandler =
    std::function<llvm::json::Value(const llvm::json::Object &args,
                                    const ToolContext &ctx)>;

/// Descriptor for a single query tool.
struct ToolEntry {
  std::string name;
  std::string description;
  llvm::json::Value inputSchema; // JSON Schema object
  // Null for adapter-implemented tools (reindex_tu mutates the indexes).
  ToolHandler handler;
  /// Name of the payload member holding this tool's record list (callers,
  /// matches, paths, ...), or empty when the payload is one scalar record.
  /// Drives the CLI's ndjson/tsv output and its empty-result exit code
  /// (vycor/cli/MegascopeCli.h); the MCP adapter ignores it.
  std::string recordsKey;
  /// IndexSection bits this tool reads. The query verbs decode only these
  /// sections (docs/megascope-cli-review.md §3.1.1); serve and batch load
  /// everything. Graph-only unless Registry.cpp says otherwise.
  unsigned needs = kSectionGraph;
};

/// Human names for IndexSection bits, in bit order ("graph",
/// "control_flow", "channels"); for `tools --format json` and -v.
std::vector<std::string> sectionNames(unsigned needs);

/// Returns the list of all registered tools, in tools/list order.
std::vector<ToolEntry> getRegisteredTools();

/// What kind of answer a payload is (`status`, docs/result-contract.md).
enum class ResultStatus : uint8_t {
  Ok,          // answered; the record list may be empty
  Ambiguous,   // several identities match: `candidates`, pick one
  UsageError,  // malformed arguments (missing, wrong type, invalid value)
  NotFound,    // well-formed, names something the index does not contain
  Unavailable, // the facts the tool needs are not loaded or never indexed
};

/// "ok", "ambiguous", "usage_error", "not_found", "unavailable".
const char *resultStatusName(ResultStatus status);
std::optional<ResultStatus> parseResultStatus(llvm::StringRef name);
/// UsageError, NotFound, or Unavailable.
bool isErrorStatus(ResultStatus status);

/// Build an error payload: `{"error": message, "status": <kind>}`. `kind`
/// must be an error status.
llvm::json::Value errorResult(ResultStatus kind, llvm::StringRef message);
llvm::json::Value usageError(llvm::StringRef message);
llvm::json::Value notFoundError(llvm::StringRef message);
llvm::json::Value unavailableError(llvm::StringRef message);

/// The error message when `result` is an error payload, else nullopt.
std::optional<llvm::StringRef> errorMessage(const llvm::json::Value &result);
bool isErrorResult(const llvm::json::Value &result);
/// True for the non-error disambiguation payload (`"ambiguous": true`).
bool isAmbiguousResult(const llvm::json::Value &result);

/// The status of a payload: its `status` member when present and valid,
/// else derived from its shape — an `error` member is NotFound (the
/// historical default for an unclassified error), `ambiguous: true` is
/// Ambiguous, anything else is Ok.
ResultStatus statusOf(const llvm::json::Value &result);

/// Stamp the envelope onto a handler's payload: `status` (statusOf) and
/// `indexScope` (ctx.facts: bake, freshness, coverage). Idempotent; a
/// non-object payload is returned untouched.
llvm::json::Value completeResult(llvm::json::Value payload,
                                 const ToolContext &ctx);
/// Run `tool`'s handler and complete its result. What every adapter
/// calls; a null handler (reindex_tu) is a usage error.
llvm::json::Value runTool(const ToolEntry &tool, const llvm::json::Object &args,
                          const ToolContext &ctx);

} // namespace vycor
