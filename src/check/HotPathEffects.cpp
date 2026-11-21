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
#include "vycor/check/HotPathEffects.h"

#include "llvm/ADT/StringRef.h"

#include <string>

namespace vycor {

void appendHotPathDiagnostics(const CallGraph &graph,
                              const HotPathReport &report,
                              const char *profilePath,
                              std::vector<Diagnostic> &diagnostics) {
  const StringInterner &strings = graph.interner();
  diagnostics.reserve(diagnostics.size() + report.findings.size());
  for (const HotPathFinding &finding : report.findings) {
    llvm::ArrayRef<HotPathHop> path = report.path(finding);
    const HotPathHop &effect = path.back();
    const std::string &root = graph.displayNameOf(path.front().caller);
    const std::string effectName = effectKindName(finding.effect).str();
    const char *evidence = hotEvidenceName(finding.evidence);

    std::string chain = root;
    for (const HotPathHop &hop : path)
      chain += " -> " + graph.displayNameOf(hop.callee);

    std::string message = effectName + " reachable from hot root '" + root +
                          "': " + chain + " (" + evidence;
    if (finding.evidence == HotEvidence::Observed)
      message += ", the rarest call on the path ran " +
                 std::to_string(finding.weight) + " times";
    else if (finding.evidence == HotEvidence::Latent)
      message += ": a call on the path never ran";
    if (profilePath)
      message += std::string(" in ") + profilePath;
    message += ")";

    Diagnostic diag;
    diag.kind = Diagnostic::Custom;
    diag.checkName = "hot-path-effects";
    diag.callLocation = strings.resolve(effect.callSite);
    // "file:line:col"; the file may hold a colon of its own.
    diag.file = llvm::StringRef(diag.callLocation)
                    .rsplit(':')
                    .first.rsplit(':')
                    .first.str();
    diag.message = std::move(message);
    // Effect, evidence, the function making the call and the callee: the
    // same call that starts running in the profile is a new finding.
    diag.entities = {effectName, evidence, strings.resolve(effect.callee)};
    diag.scope = strings.resolve(effect.caller);
    diag.note = finding.evidence == HotEvidence::Latent;
    diagnostics.push_back(std::move(diag));
  }
}

} // namespace vycor
