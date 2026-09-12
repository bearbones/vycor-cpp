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
#include "vycor/callgraph/ConditionalGuard.h"
#include "vycor/callgraph/ControlFlowIndex.h"
#include "vycor/callgraph/PathSearch.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/JSON.h"

#include <optional>

// Enum <-> string spellings and JSON serializers shared by the query tools.
// These spellings are part of the tool output contract.

namespace vycor {

const char *edgeKindToString(EdgeKind k);
EdgeKind parseEdgeKind(llvm::StringRef s);

const char *executionContextToString(ExecutionContext c);
std::optional<ExecutionContext> parseExecutionContext(llvm::StringRef s);

const char *confidenceToString(Confidence c);
Confidence parseConfidence(llvm::StringRef s);
int confidenceRank(Confidence c);

llvm::json::Value edgeToJson(const CallGraphEdge &e);

/// The canonical order of an edge list (docs/deterministic-output.md):
/// caller usr, callee usr, call site, kind, confidence, execution
/// context, indirection depth — the key the path search expands callers
/// by, applied to a whole edge. A list with one end fixed (get_callees,
/// get_callers) is therefore ordered by the other end's usr, then the
/// call site; a whole-graph list (callback sites, thread entries) by both
/// ends. Storage order follows TU and insertion order and is not stable
/// across bakes.
bool canonicalEdgeLess(const CallGraphEdge &a, const CallGraphEdge &b);
void sortEdgesCanonically(std::vector<CallGraphEdge> &edges);

const char *channelOperationToString(ChannelOperation op);
llvm::json::Value serializeGuard(const ConditionalGuard &g);
/// {tryLocation, enclosingFunction, nestingDepth, handlers: [{caughtType,
/// isCatchAll, rethrows, location, body}]} — the shape
/// query_call_site_context emits for enclosingScopes; shared by the path
/// tools and dump.
llvm::json::Value serializeTryCatchScope(const TryCatchScope &scope);
/// "none" | "noexcept" | "noexcept(false)" | "throw()" | "unknown".
const char *noexceptSpecToString(NoexceptSpec spec);
/// "lock" | "smart_ptr" | "other".
const char *raiiKindToString(RaiiKind k);
/// {typeName, varName, declLocation, kind}.
llvm::json::Value serializeRaiiLocal(const RaiiLocal &l);
llvm::json::Value serializeChannelSite(const ChannelSite &s);

/// One edge of a found path: {from, to, fromUsr, toUsr, callSite, kind,
/// confidence, executionContext?} — `from`/`to` are display names; the
/// exact identities ride in the *Usr twins. executionContext is emitted
/// only when not Synchronous (the find_call_chain hop convention).
llvm::json::Value serializePathHop(const PathHop &hop);
/// The producer-side completeness facts of a bounded path search:
/// `complete` (every path within the depth bound was enumerated),
/// `exhaustive` (and no depth cutoff), `stopReasons` (the StopReason
/// names, possibly empty), and `skippedHubs` [{name, usr, inDegree}] when
/// any hub was pruned. Package C owns the common result contract; these
/// are the fields it consumes.
void attachSearchFacts(llvm::json::Object &obj, unsigned stops,
                       bool complete, bool exhaustive,
                       const std::vector<SkippedHub> &skippedHubs);

} // namespace vycor
