// Copyright 2025 Alex Mason
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

// ============================================================================
// Bake configuration shared by every entry point that parses sources: the
// llvm::cl verbs in main.cpp (anneal, megascope index) and the query
// verbs' ephemeral mode (MegascopeCli.cpp), which bakes the selected TUs
// in memory when no index is wanted. Diagnostics go to llvm::errs().
// ============================================================================

#include "vycor/callgraph/ChannelIndex.h"
#include "vycor/callgraph/ControlFlowIndex.h"
#include "vycor/ext/OrgConfig.h"

#include <string>
#include <vector>

namespace vycor {

/// Parses a --channel-types-json file — a JSON array of channel type
/// registrations `[{"type": "Queue", "produce": ["push"], "consume":
/// ["pop"], "category": "queue"}, ...]`, "type" being the canonical type
/// name without the struct/class keyword (see ChannelIndex.h) — into
/// `outCfg`. Returns false (and prints a diagnostic) on any malformed
/// entry; callers treat that as a fatal CLI error.
bool parseChannelTypesJson(const std::string &path,
                           ChannelTypeConfig &outCfg);

/// Loads --org-config when `path` is set and installs its hook-shaped parts
/// (feature flag patterns, lock/channel types) into ExtensionRegistry.
/// Compiled ext/ registrars have already run by this point (static init),
/// so after this call the registry holds both sources. Returns false (with
/// a diagnostic) on an unreadable or malformed config; true when `path` is
/// empty.
bool loadOrgConfigIfSet(const std::string &path, OrgConfig &out);

/// Merges registry-held lock/channel types (compiled ext/ registrars plus
/// --org-config) into the CLI-built configs, and org collapse paths into
/// `collapsePaths`. CLI entries keep their position and duplicates are
/// dropped: the merged lists land in snapshot meta (config-match check),
/// so the result must be deterministic and must equal the plain CLI lists
/// when no extensions are registered.
void mergeExtensionConfig(const OrgConfig &orgCfg, LockTypeConfig &lockCfg,
                          ChannelTypeConfig &channelCfg,
                          std::vector<std::string> &collapsePaths);

} // namespace vycor
