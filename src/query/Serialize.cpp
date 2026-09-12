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


#include "vycor/query/Serialize.h"
#include "vycor/ext/Extensions.h"

#include <algorithm>

namespace vycor {

// ============================================================================
// Enum serialization helpers
// ============================================================================

const char *edgeKindToString(EdgeKind k) {
  switch (k) {
  case EdgeKind::DirectCall:
    return "DirectCall";
  case EdgeKind::VirtualDispatch:
    return "VirtualDispatch";
  case EdgeKind::FunctionPointer:
    return "FunctionPointer";
  case EdgeKind::ConstructorCall:
    return "ConstructorCall";
  case EdgeKind::DestructorCall:
    return "DestructorCall";
  case EdgeKind::OperatorCall:
    return "OperatorCall";
  case EdgeKind::TemplateInstantiation:
    return "TemplateInstantiation";
  case EdgeKind::LambdaCall:
    return "LambdaCall";
  case EdgeKind::ThreadEntry:
    return "ThreadEntry";
  case EdgeKind::FunctionPointerReturn:
    // Raw deferred edges are joined at query time and never materialized
    // into results (CallGraph.h) — labeled honestly in case a future raw
    // dump path reaches here.
    return "FunctionPointerReturn";
  }
  return "Unknown";
}

const char *executionContextToString(ExecutionContext c) {
  switch (c) {
  case ExecutionContext::Synchronous:
    return "Synchronous";
  case ExecutionContext::ThreadSpawn:
    return "ThreadSpawn";
  case ExecutionContext::AsyncTask:
    return "AsyncTask";
  case ExecutionContext::PackagedTask:
    return "PackagedTask";
  case ExecutionContext::Invoke:
    return "Invoke";
  }
  return "Synchronous";
}

std::optional<ExecutionContext>
parseExecutionContext(llvm::StringRef s) {
  if (s == "Synchronous") return ExecutionContext::Synchronous;
  if (s == "ThreadSpawn") return ExecutionContext::ThreadSpawn;
  if (s == "AsyncTask") return ExecutionContext::AsyncTask;
  if (s == "PackagedTask") return ExecutionContext::PackagedTask;
  if (s == "Invoke") return ExecutionContext::Invoke;
  return std::nullopt;
}

const char *confidenceToString(Confidence c) {
  switch (c) {
  case Confidence::Proven:
    return "Proven";
  case Confidence::Plausible:
    return "Plausible";
  case Confidence::Unknown:
    return "Unknown";
  }
  return "Unknown";
}

EdgeKind parseEdgeKind(llvm::StringRef s) {
  if (s == "DirectCall") return EdgeKind::DirectCall;
  if (s == "VirtualDispatch") return EdgeKind::VirtualDispatch;
  if (s == "FunctionPointer") return EdgeKind::FunctionPointer;
  if (s == "ConstructorCall") return EdgeKind::ConstructorCall;
  if (s == "DestructorCall") return EdgeKind::DestructorCall;
  if (s == "OperatorCall") return EdgeKind::OperatorCall;
  if (s == "TemplateInstantiation") return EdgeKind::TemplateInstantiation;
  if (s == "LambdaCall") return EdgeKind::LambdaCall;
  if (s == "ThreadEntry") return EdgeKind::ThreadEntry;
  return EdgeKind::DirectCall; // fallback
}

Confidence parseConfidence(llvm::StringRef s) {
  if (s == "Proven") return Confidence::Proven;
  if (s == "Plausible") return Confidence::Plausible;
  return Confidence::Unknown;
}

int confidenceRank(Confidence c) {
  switch (c) {
  case Confidence::Proven: return 2;
  case Confidence::Plausible: return 1;
  case Confidence::Unknown: return 0;
  }
  return 0;
}

// ============================================================================
// Serialize a CallGraphEdge to JSON
// ============================================================================

bool canonicalEdgeLess(const CallGraphEdge &a, const CallGraphEdge &b) {
  if (a.callerUsr != b.callerUsr)
    return a.callerUsr < b.callerUsr;
  if (a.calleeUsr != b.calleeUsr)
    return a.calleeUsr < b.calleeUsr;
  if (a.callSite != b.callSite)
    return a.callSite < b.callSite;
  if (a.kind != b.kind)
    return a.kind < b.kind;
  if (a.confidence != b.confidence)
    return a.confidence < b.confidence;
  if (a.execContext != b.execContext)
    return a.execContext < b.execContext;
  if (a.indirectionDepth != b.indirectionDepth)
    return a.indirectionDepth < b.indirectionDepth;
  // Materialized edges carry usrs; hand-built graphs may not.
  if (a.callerName != b.callerName)
    return a.callerName < b.callerName;
  return a.calleeName < b.calleeName;
}

void sortEdgesCanonically(std::vector<CallGraphEdge> &edges) {
  std::sort(edges.begin(), edges.end(), canonicalEdgeLess);
}

llvm::json::Value edgeToJson(const CallGraphEdge &e) {
  llvm::json::Object obj;
  obj["callerName"] = e.callerName;
  obj["calleeName"] = e.calleeName;
  obj["kind"] = edgeKindToString(e.kind);
  obj["confidence"] = confidenceToString(e.confidence);
  obj["callSite"] = e.callSite;
  if (e.indirectionDepth > 0)
    obj["indirectionDepth"] = static_cast<int64_t>(e.indirectionDepth);
  if (e.execContext != ExecutionContext::Synchronous)
    obj["executionContext"] = executionContextToString(e.execContext);
  return llvm::json::Value(std::move(obj));
}


// ============================================================================
// Tools 13-16: channel/data-flow tracing
//
// Answers a question get_callers/get_callees can't: where does an object
// go after it's pushed onto a queue/map/channel, and who picks it up —
// possibly on another thread, with no ordering guarantee relative to some
// other producer. See ChannelIndex.h for the design (a channel has N
// producers/M consumers, which a caller->callee edge can't represent).
// All four tools are no-ops (empty results, not errors) when the server
// was started without --channel-types-json.
// ============================================================================

const char *channelOperationToString(ChannelOperation op) {
  return op == ChannelOperation::Produce ? "produce" : "consume";
}

llvm::json::Value serializeGuard(const ConditionalGuard &g) {
  llvm::json::Object obj;
  obj["conditionText"] = g.conditionText;
  obj["location"] = g.location;
  obj["inTrueBranch"] = g.inTrueBranch;
  obj["isAssertion"] = g.isAssertion;
  // Organization guard classifiers (feature flags etc., Extensions.h):
  // e.g. {"kind": "feature-flag", "name": "NewNav"} — with inTrueBranch
  // this tells a client "reachable only with NewNav on/off".
  if (auto ann = classifyGuard(g)) {
    llvm::json::Object annotation;
    annotation["kind"] = ann->kind;
    annotation["name"] = ann->name;
    obj["annotation"] = std::move(annotation);
  }
  return llvm::json::Value(std::move(obj));
}

llvm::json::Value serializeTryCatchScope(const TryCatchScope &scope) {
  llvm::json::Object s;
  s["tryLocation"] = scope.tryLocation;
  s["enclosingFunction"] = scope.enclosingFunction;
  s["nestingDepth"] = static_cast<int64_t>(scope.nestingDepth);
  llvm::json::Array handlers;
  for (const auto &h : scope.handlers) {
    llvm::json::Object ho;
    ho["caughtType"] = h.caughtType;
    ho["isCatchAll"] = h.isCatchAll;
    ho["rethrows"] = h.rethrows;
    ho["location"] = h.location;
    ho["body"] = h.bodySummary;
    handlers.push_back(llvm::json::Value(std::move(ho)));
  }
  s["handlers"] = std::move(handlers);
  return llvm::json::Value(std::move(s));
}

llvm::json::Value serializePathHop(const PathHop &hop) {
  llvm::json::Object h;
  h["from"] = hop.caller;
  h["to"] = hop.callee;
  h["fromUsr"] = hop.callerUsr;
  h["toUsr"] = hop.calleeUsr;
  h["callSite"] = hop.callSite;
  h["kind"] = edgeKindToString(hop.kind);
  h["confidence"] = confidenceToString(hop.confidence);
  if (hop.execContext != ExecutionContext::Synchronous)
    h["executionContext"] = executionContextToString(hop.execContext);
  return llvm::json::Value(std::move(h));
}

void attachSearchFacts(llvm::json::Object &obj, unsigned stops,
                       bool complete, bool exhaustive,
                       const std::vector<SkippedHub> &skippedHubs) {
  obj["complete"] = complete;
  obj["exhaustive"] = exhaustive;
  llvm::json::Array reasons;
  for (const auto &name : stopReasonNames(stops))
    reasons.push_back(name);
  obj["stopReasons"] = std::move(reasons);
  if (!skippedHubs.empty()) {
    llvm::json::Array hubs;
    for (const auto &hub : skippedHubs) {
      llvm::json::Object h;
      h["name"] = hub.name;
      h["usr"] = hub.usr;
      h["inDegree"] = static_cast<int64_t>(hub.inDegree);
      hubs.push_back(llvm::json::Value(std::move(h)));
    }
    obj["skippedHubs"] = std::move(hubs);
  }
}

const char *noexceptSpecToString(NoexceptSpec spec) {
  switch (spec) {
  case NoexceptSpec::None: return "none";
  case NoexceptSpec::Noexcept: return "noexcept";
  case NoexceptSpec::NoexceptFalse: return "noexcept(false)";
  case NoexceptSpec::ThrowNone: return "throw()";
  case NoexceptSpec::Unknown: return "unknown";
  }
  return "none";
}

const char *raiiKindToString(RaiiKind k) {
  switch (k) {
  case RaiiKind::Lock: return "lock";
  case RaiiKind::SmartPtr: return "smart_ptr";
  case RaiiKind::Other: return "other";
  }
  return "other";
}

llvm::json::Value serializeRaiiLocal(const RaiiLocal &l) {
  llvm::json::Object obj;
  obj["typeName"] = l.typeName;
  obj["varName"] = l.varName;
  obj["declLocation"] = l.declLocation;
  obj["kind"] = raiiKindToString(l.kind);
  return llvm::json::Value(std::move(obj));
}

llvm::json::Value serializeChannelSite(const ChannelSite &s) {
  llvm::json::Object obj;
  obj["channelId"] = s.channelId;
  obj["channelType"] = s.channelTypeName;
  obj["category"] = s.category;
  obj["operation"] = channelOperationToString(s.op);
  obj["function"] = s.siteFunctionDisplay;
  obj["functionUsr"] = s.siteFunctionUsr;
  obj["callSite"] = s.callSite;
  llvm::json::Array guards;
  for (const auto &g : s.enclosingGuards)
    guards.push_back(serializeGuard(g));
  obj["guards"] = std::move(guards);
  return llvm::json::Value(std::move(obj));
}

} // namespace vycor
