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

#include "vycor/ext/OrgConfig.h"

#include "vycor/ext/Extensions.h"

#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"

namespace vycor {

static bool parseStringArray(const llvm::json::Object &obj,
                             llvm::StringRef key,
                             std::vector<std::string> &out,
                             std::string &error) {
  const llvm::json::Value *value = obj.get(key);
  if (!value)
    return true;
  const llvm::json::Array *arr = value->getAsArray();
  if (!arr) {
    error = ("'" + key + "' must be an array of strings").str();
    return false;
  }
  for (const auto &entry : *arr) {
    auto s = entry.getAsString();
    if (!s) {
      error = ("'" + key + "' must be an array of strings").str();
      return false;
    }
    out.push_back(s->str());
  }
  return true;
}

bool parseOrgConfigJson(llvm::StringRef buffer, OrgConfig &out,
                        std::string &error) {
  auto jsonOrErr = llvm::json::parse(buffer);
  if (!jsonOrErr) {
    error = "parse error: " + llvm::toString(jsonOrErr.takeError());
    return false;
  }
  const llvm::json::Object *root = jsonOrErr->getAsObject();
  if (!root) {
    error = "top level must be a JSON object";
    return false;
  }

  // Reject unknown keys so a typo ("locktypes") fails loudly instead of
  // silently configuring nothing.
  static const char *knownKeys[] = {"lockTypes", "channelTypes",
                                    "featureFlags", "collapsePaths",
                                    "disabledAnnealChecks",
                                    "staticInitHazards"};
  for (const auto &kv : *root) {
    bool known = false;
    for (const char *k : knownKeys)
      known = known || kv.first.str() == k;
    if (!known) {
      error = "unknown key '" + kv.first.str() +
              "' (expected one of: lockTypes, channelTypes, featureFlags, "
              "collapsePaths, disabledAnnealChecks, staticInitHazards)";
      return false;
    }
  }

  if (!parseStringArray(*root, "staticInitHazards", out.staticInitHazards,
                        error) ||
      !parseStringArray(*root, "lockTypes", out.lockTypes, error) ||
      !parseStringArray(*root, "collapsePaths", out.collapsePaths, error) ||
      !parseStringArray(*root, "disabledAnnealChecks",
                        out.disabledAnnealChecks, error))
    return false;

  if (const llvm::json::Value *channels = root->get("channelTypes")) {
    const llvm::json::Array *arr = channels->getAsArray();
    if (!arr) {
      error = "'channelTypes' must be an array of objects";
      return false;
    }
    for (const auto &entry : *arr) {
      const llvm::json::Object *obj = entry.getAsObject();
      if (!obj) {
        error = "'channelTypes' entries must be objects";
        return false;
      }
      ChannelTypeSpec spec;
      if (auto type = obj->getString("type")) {
        spec.qualifiedTypeName = type->str();
      } else {
        error = "'channelTypes' entry missing required 'type'";
        return false;
      }
      if (const llvm::json::Array *produce = obj->getArray("produce"))
        for (const auto &m : *produce)
          if (auto s = m.getAsString())
            spec.produceMethods.push_back(s->str());
      if (const llvm::json::Array *consume = obj->getArray("consume"))
        for (const auto &m : *consume)
          if (auto s = m.getAsString())
            spec.consumeMethods.push_back(s->str());
      if (auto category = obj->getString("category"))
        spec.category = category->str();
      out.channelTypes.push_back(std::move(spec));
    }
  }

  if (const llvm::json::Value *flags = root->get("featureFlags")) {
    const llvm::json::Array *arr = flags->getAsArray();
    if (!arr) {
      error = "'featureFlags' must be an array of objects";
      return false;
    }
    for (const auto &entry : *arr) {
      const llvm::json::Object *obj = entry.getAsObject();
      if (!obj) {
        error = "'featureFlags' entries must be objects";
        return false;
      }
      FeatureFlagPattern pat;
      if (auto pattern = obj->getString("pattern")) {
        pat.pattern = pattern->str();
      } else {
        error = "'featureFlags' entry missing required 'pattern'";
        return false;
      }
      if (auto group = obj->getInteger("nameGroup")) {
        if (*group < 0) {
          error = "'featureFlags' nameGroup must be >= 0";
          return false;
        }
        pat.nameGroup = static_cast<unsigned>(*group);
      }
      out.featureFlags.push_back(std::move(pat));
    }
  }

  return true;
}

bool loadOrgConfigFile(const std::string &path, OrgConfig &out,
                       std::string &error) {
  auto bufOrErr = llvm::MemoryBuffer::getFile(path);
  if (!bufOrErr) {
    error = "cannot read " + path + ": " + bufOrErr.getError().message();
    return false;
  }
  if (!parseOrgConfigJson(bufOrErr.get()->getBuffer(), out, error)) {
    error = path + ": " + error;
    return false;
  }
  return true;
}

bool applyOrgConfig(const OrgConfig &cfg, std::string &error) {
  auto &registry = ExtensionRegistry::instance();
  registry.addLockTypes(cfg.lockTypes);
  registry.addStaticInitHazards(cfg.staticInitHazards);
  registry.addChannelTypes(cfg.channelTypes);
  for (const auto &flag : cfg.featureFlags) {
    if (!registry.addFeatureFlagPattern(flag.pattern, flag.nameGroup)) {
      error = "featureFlags pattern is not a valid regex: " + flag.pattern;
      return false;
    }
  }
  return true;
}

} // namespace vycor
