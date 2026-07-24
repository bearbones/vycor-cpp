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

#include "vycor/callgraph/ChannelIndex.h"

#include "llvm/ADT/StringRef.h"

#include <string>
#include <vector>

namespace vycor {

// One feature-flag recognition pattern: an ERE applied (searching) to a
// guard's condition text. nameGroup selects the capture group holding the
// flag name (0 = the whole match).
struct FeatureFlagPattern {
  std::string pattern;
  unsigned nameGroup = 0;
};

// Declarative organization configuration (--org-config <file>), the
// check-it-into-your-fork counterpart of the ad-hoc CLI flags. Schema:
//
//   {
//     "lockTypes": ["myorg::SpinLock"],
//     "channelTypes": [{"type": "myorg::EventBus", "produce": ["post"],
//                       "consume": ["drain"], "category": "bus"}],
//     "featureFlags": [{"pattern": "FFlag::([A-Za-z0-9_]+)", "nameGroup": 1}],
//     "staticInitHazards": ["myorg::JniEnv::attach"],
//     "collapsePaths": ["ThirdParty/Math"],
//     "disabledAnnealChecks": ["some-ext-check"]
//   }
//
// Every key is optional. lockTypes/channelTypes/collapsePaths merge with
// (never replace) the corresponding CLI flags; featureFlags become guard
// classifiers (see Extensions.h); disabledAnnealChecks suppresses compiled
// ext/ checks by name.
struct OrgConfig {
  std::vector<std::string> lockTypes;
  // Extra loader-hostile functions for static-init-hazards.
  std::vector<std::string> staticInitHazards;
  std::vector<ChannelTypeSpec> channelTypes;
  std::vector<FeatureFlagPattern> featureFlags;
  std::vector<std::string> collapsePaths;
  std::vector<std::string> disabledAnnealChecks;
};

// Parse an org config from an in-memory JSON buffer. Returns false and
// fills `error` on malformed input (unknown keys are an error too, to catch
// typos like "locktypes").
bool parseOrgConfigJson(llvm::StringRef buffer, OrgConfig &out,
                        std::string &error);

// Read + parse an org config file.
bool loadOrgConfigFile(const std::string &path, OrgConfig &out,
                       std::string &error);

// Install the config's hook-shaped parts (lockTypes, channelTypes,
// featureFlags) into ExtensionRegistry::instance(). collapsePaths and
// disabledAnnealChecks are not registry concerns — the CLI layer consumes
// those directly. Returns false and fills `error` when a featureFlags
// pattern is not a valid regex.
bool applyOrgConfig(const OrgConfig &cfg, std::string &error);

} // namespace vycor
