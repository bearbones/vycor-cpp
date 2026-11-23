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

#include "vycor/morph/RulesParser.h"
#include "vycor/morph/TemplateEngine.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/MemoryBuffer.h"

namespace vycor {

// ---------------------------------------------------------------------------
// JSON Deserialization
// ---------------------------------------------------------------------------

// A misspelled filter (especially macro-names) would silently broaden a
// rewrite. Report the JSON path before any translation unit is opened.
static bool knownFields(const llvm::json::Value &value, llvm::json::Path path,
                        llvm::ArrayRef<llvm::StringRef> allowed) {
  if (auto *object = value.getAsObject()) {
    for (const auto &field : *object) {
      if (!llvm::is_contained(allowed, llvm::StringRef(field.first))) {
        path.field(field.first).report("unknown morph rule field");
        return false;
      }
    }
  }
  return true;
}

bool fromJSON(const llvm::json::Value &value, JsonFindSpec &out,
              llvm::json::Path path) {
  llvm::json::ObjectMapper o(value, path);
  return o && knownFields(value, path, {"matcher"}) &&
         o.map("matcher", out.matcher);
}

bool fromJSON(const llvm::json::Value &value, JsonReplaceSpec &out,
              llvm::json::Path path) {
  llvm::json::ObjectMapper o(value, path);
  out = JsonReplaceSpec{};
  if (!o ||
      !knownFields(value, path,
                   {"target", "with", "scope", "macro-names", "arg-only"}) ||
      !o.map("target", out.target) || !o.map("with", out.withTempl) ||
      !o.mapOptional("scope", out.scope) ||
      !o.mapOptional("macro-names", out.macroNames) ||
      !o.mapOptional("arg-only", out.argOnly))
    return false;
  if (out.scope == "node") {
    for (auto key : {"macro-names", "arg-only"}) {
      if (value.getAsObject()->get(key)) {
        path.field(key).report("requires macro-expansion scope");
        return false;
      }
    }
  }
  return true;
}

bool fromJSON(const llvm::json::Value &value, JsonRule &out,
              llvm::json::Path path) {
  llvm::json::ObjectMapper o(value, path);
  return o && knownFields(value, path, {"find", "replace"}) &&
         o.map("find", out.find) && o.map("replace", out.replace);
}

bool fromJSON(const llvm::json::Value &value, JsonPass &out,
              llvm::json::Path path) {
  llvm::json::ObjectMapper o(value, path);
  return o && knownFields(value, path, {"rules", "name", "context"}) &&
         o.map("rules", out.rules) && o.mapOptional("name", out.name) &&
         o.mapOptional("context", out.context);
}

bool fromJSON(const llvm::json::Value &value, JsonRulesFile &out,
              llvm::json::Path path) {
  llvm::json::ObjectMapper o(value, path);
  return o && knownFields(value, path, {"passes"}) &&
         o.map("passes", out.passes);
}

// ---------------------------------------------------------------------------
// File Parsing
// ---------------------------------------------------------------------------

llvm::Expected<JsonRulesFile> parseRulesFile(llvm::StringRef path) {
  auto bufOrErr = llvm::MemoryBuffer::getFile(path);
  if (!bufOrErr)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "failed to read rules file '" + path.str() +
                                       "': " + bufOrErr.getError().message());

  auto jsonOrErr = llvm::json::parse(bufOrErr.get()->getBuffer());
  if (!jsonOrErr)
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(),
        "JSON parse error in '" + path.str() +
            "': " + llvm::toString(jsonOrErr.takeError()));

  JsonRulesFile rules;
  llvm::json::Path::Root root("rules-json");
  if (!fromJSON(*jsonOrErr, rules, root))
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "invalid rules file structure in '" +
                                       path.str() +
                                       "': " + llvm::toString(root.getError()));

  return rules;
}

// ---------------------------------------------------------------------------
// Pipeline Construction
// ---------------------------------------------------------------------------

llvm::Expected<std::vector<std::vector<TransformRule>>>
buildPipeline(const JsonRulesFile &rulesFile) {
  std::vector<std::vector<TransformRule>> passes;

  for (const auto &jsonPass : rulesFile.passes) {
    std::vector<TransformRule> passRules;

    for (const auto &jsonRule : jsonPass.rules) {
      if (jsonRule.replace.target.empty())
        return llvm::createStringError(
            llvm::inconvertibleErrorCode(),
            "replace.target requires a nonempty binding ID");

      // Parse the replacement template
      auto tmplOrErr = parseTemplate(jsonRule.replace.withTempl);
      if (!tmplOrErr)
        return llvm::createStringError(
            llvm::inconvertibleErrorCode(),
            "template parse error in pass '" + jsonPass.name +
                "': " + llvm::toString(tmplOrErr.takeError()));

      // Validate scope
      const auto &scope = jsonRule.replace.scope;
      if (scope != "node" && scope != "macro-expansion")
        return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                       "invalid scope '" + scope +
                                           "'; expected 'node' or "
                                           "'macro-expansion'");

      TransformRule rule;
      rule.matcherExpression = jsonRule.find.matcher;
      rule.context = jsonPass.context;
      // bindId is empty — bindings are inline in the matcher expression
      rule.callback = makeTemplateCallback(
          std::move(*tmplOrErr), jsonRule.replace.target, scope,
          jsonRule.replace.macroNames, jsonRule.replace.argOnly);

      passRules.push_back(std::move(rule));
    }

    passes.push_back(std::move(passRules));
  }

  return passes;
}

} // namespace vycor
