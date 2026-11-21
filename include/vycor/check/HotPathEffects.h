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

#include "vycor/callgraph/HotPath.h"
#include "vycor/check/GlobalIndex.h"

#include <vector>

// The hot-path-effects check (docs/checks/hot-path-effects.md): every
// finding of analyzeHotPaths as a diagnostic at the effect call. Latent
// findings (on a path the profile says never ran) are notes; observed and
// unprofiled ones keep the check's severity.

namespace vycor {

void appendHotPathDiagnostics(const CallGraph &graph,
                              const HotPathReport &report,
                              const char *profilePath,
                              std::vector<Diagnostic> &diagnostics);

} // namespace vycor
