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
#include "vycor/query/Serialize.h"

#include "llvm/Support/JSON.h"

#include <optional>
#include <set>
#include <string>

namespace vycor {

// ============================================================================
// Edge filter shared across get_callees, get_callers, find_call_chain,
// and impact_of_change (ImpactTools.cpp).
// ============================================================================

struct EdgeFilter {
  std::set<EdgeKind> kinds; // empty = allow all
  std::set<Confidence> includeConfidences; // non-empty overrides minConf
  bool useIncludeSet = false;
  Confidence minConf = Confidence::Unknown;
  std::set<ExecutionContext> execContexts; // empty = allow all

  bool allows(const CallGraphEdge &e) const {
    return allowsFields(e.kind, e.confidence, e.execContext);
  }

  // Id-space twin for traversal loops that never materialize strings.
  bool allowsRef(const CallGraph::EdgeRef &e) const {
    return allowsFields(e.kind, e.confidence, e.execContext);
  }

private:
  bool allowsFields(EdgeKind kind, Confidence conf,
                    ExecutionContext execCtx) const {
    if (!kinds.empty() && !kinds.count(kind))
      return false;
    if (!execContexts.empty() && !execContexts.count(execCtx))
      return false;
    if (useIncludeSet)
      return includeConfidences.count(conf) > 0;
    return confidenceRank(conf) >= confidenceRank(minConf);
  }
};

// Parse an EdgeFilter from tool args. Returns an error message on invalid
// input (specifically, unrecognized include_confidences values).
inline std::optional<std::string>
parseEdgeFilter(const llvm::json::Object &args, EdgeFilter &out) {
  if (auto *kindsArr = args.getArray("edge_kinds")) {
    for (auto &v : *kindsArr) {
      if (auto s = v.getAsString())
        out.kinds.insert(parseEdgeKind(*s));
    }
  }
  if (auto *confs = args.getArray("include_confidences")) {
    out.useIncludeSet = true;
    for (auto &v : *confs) {
      auto s = v.getAsString();
      if (!s)
        continue;
      if (*s != "Proven" && *s != "Plausible" && *s != "Unknown") {
        return "Invalid value in include_confidences: '" + s->str() +
               "' (expected Proven, Plausible, or Unknown)";
      }
      out.includeConfidences.insert(parseConfidence(*s));
    }
  } else if (auto mc = args.getString("min_confidence")) {
    out.minConf = parseConfidence(*mc);
  }
  if (auto *ctxArr = args.getArray("execution_contexts")) {
    for (auto &v : *ctxArr) {
      auto s = v.getAsString();
      if (!s)
        continue;
      auto parsed = parseExecutionContext(*s);
      if (!parsed) {
        return "Invalid value in execution_contexts: '" + s->str() +
               "' (expected Synchronous, ThreadSpawn, AsyncTask, "
               "PackagedTask, or Invoke)";
      }
      out.execContexts.insert(*parsed);
    }
  }
  return std::nullopt;
}


} // namespace vycor
