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

#include <catch2/catch_test_macros.hpp>
#include <string>

using namespace vycor;

// ---------------------------------------------------------------------------
// Template Parsing
// ---------------------------------------------------------------------------

TEST_CASE("parseTemplate basic expressions", "[template][parse]") {
  SECTION("literal only") {
    auto result = parseTemplate("hello world");
    REQUIRE(result);
    CHECK(result->literals.size() == 1);
    CHECK(result->exprs.empty());
    CHECK(result->literals[0] == "hello world");
  }

  SECTION("single bare expression") {
    auto result = parseTemplate("{{root}}");
    REQUIRE(result);
    CHECK(result->literals.size() == 2);
    CHECK(result->exprs.size() == 1);
    CHECK(result->literals[0].empty());
    CHECK(result->literals[1].empty());
    CHECK(result->exprs[0].nodeId == "root");
    CHECK(result->exprs[0].pipes.empty());
  }

  SECTION("expression with surrounding text") {
    auto result = parseTemplate("before({{id}})after");
    REQUIRE(result);
    CHECK(result->literals.size() == 2);
    CHECK(result->literals[0] == "before(");
    CHECK(result->literals[1] == ")after");
    CHECK(result->exprs[0].nodeId == "id");
  }

  SECTION("multiple expressions") {
    auto result = parseTemplate("A{{x}}B{{y}}C");
    REQUIRE(result);
    CHECK(result->literals.size() == 3);
    CHECK(result->exprs.size() == 2);
    CHECK(result->literals[0] == "A");
    CHECK(result->literals[1] == "B");
    CHECK(result->literals[2] == "C");
    CHECK(result->exprs[0].nodeId == "x");
    CHECK(result->exprs[1].nodeId == "y");
  }
}

TEST_CASE("parseTemplate pipe chains", "[template][parse]") {
  SECTION("single pipe no arg") {
    auto result = parseTemplate("{{id | name}}");
    REQUIRE(result);
    REQUIRE(result->exprs.size() == 1);
    REQUIRE(result->exprs[0].pipes.size() == 1);
    CHECK(result->exprs[0].pipes[0].name == "name");
    CHECK(result->exprs[0].pipes[0].arg.empty());
  }

  SECTION("single pipe with arg") {
    auto result = parseTemplate("{{root | flatten(\"&&\")}}");
    REQUIRE(result);
    REQUIRE(result->exprs.size() == 1);
    REQUIRE(result->exprs[0].pipes.size() == 1);
    CHECK(result->exprs[0].pipes[0].name == "flatten");
    CHECK(result->exprs[0].pipes[0].arg == "&&");
  }

  SECTION("two-pipe chain") {
    auto result =
        parseTemplate("{{root | flatten(\"&&\") | join(\", \")}}");
    REQUIRE(result);
    REQUIRE(result->exprs.size() == 1);
    auto &pipes = result->exprs[0].pipes;
    REQUIRE(pipes.size() == 2);
    CHECK(pipes[0].name == "flatten");
    CHECK(pipes[0].arg == "&&");
    CHECK(pipes[1].name == "join");
    CHECK(pipes[1].arg == ", ");
  }

  SECTION("pipe arg containing pipe character in quotes") {
    auto result = parseTemplate("{{id | join(\" | \")}}");
    REQUIRE(result);
    REQUIRE(result->exprs.size() == 1);
    REQUIRE(result->exprs[0].pipes.size() == 1);
    CHECK(result->exprs[0].pipes[0].name == "join");
    CHECK(result->exprs[0].pipes[0].arg == " | ");
  }
}

TEST_CASE("parseTemplate full RBX_CHECK pattern", "[template][parse]") {
  auto result = parseTemplate(
      "RBX_CHECK_AND({{root | flatten(\"&&\") | join(\", \")}})");
  REQUIRE(result);
  CHECK(result->literals[0] == "RBX_CHECK_AND(");
  CHECK(result->literals[1] == ")");
  REQUIRE(result->exprs.size() == 1);
  CHECK(result->exprs[0].nodeId == "root");
  REQUIRE(result->exprs[0].pipes.size() == 2);
  CHECK(result->exprs[0].pipes[0].name == "flatten");
  CHECK(result->exprs[0].pipes[0].arg == "&&");
  CHECK(result->exprs[0].pipes[1].name == "join");
  CHECK(result->exprs[0].pipes[1].arg == ", ");
}

TEST_CASE("parseTemplate errors", "[template][parse]") {
  SECTION("unclosed braces") {
    auto result = parseTemplate("{{unclosed");
    CHECK_FALSE(result);
    llvm::consumeError(result.takeError());
  }

  SECTION("empty expression") {
    auto result = parseTemplate("{{}}");
    CHECK_FALSE(result);
    llvm::consumeError(result.takeError());
  }
}

// ---------------------------------------------------------------------------
// JSON Deserialization
// ---------------------------------------------------------------------------

TEST_CASE("fromJSON JsonRulesFile", "[rules][json]") {
  SECTION("minimal valid rules file") {
    auto json = llvm::json::parse(R"JSON({
      "passes": [{
        "rules": [{
          "find": {
            "matcher": "binaryOperator(hasOperatorName(\"&&\")).bind(\"root\")"
          },
          "replace": {
            "target": "root",
            "with": "{{root}}"
          }
        }]
      }]
    })JSON");
    REQUIRE(json);

    JsonRulesFile rules;
    llvm::json::Path::Root root("test");
    REQUIRE(fromJSON(*json, rules, root));
    REQUIRE(rules.passes.size() == 1);
    REQUIRE(rules.passes[0].rules.size() == 1);
    CHECK(rules.passes[0].rules[0].find.matcher ==
          "binaryOperator(hasOperatorName(\"&&\")).bind(\"root\")");
    CHECK(rules.passes[0].rules[0].replace.target == "root");
    CHECK(rules.passes[0].rules[0].replace.scope == "node"); // default
    CHECK(rules.passes[0].rules[0].replace.withTempl == "{{root}}");
  }

  SECTION("full rules file with context and scope") {
    auto json = llvm::json::parse(R"JSON({
      "passes": [{
        "name": "test-pass",
        "context": "isExpansionInMainFile()",
        "rules": [{
          "find": {
            "matcher": "binaryOperator(hasOperatorName(\"&&\")).bind(\"root\")"
          },
          "replace": {
            "target": "root",
            "scope": "macro-expansion",
            "with": "REPLACED({{root | flatten(\"&&\") | join(\", \")}})"
          }
        }]
      }]
    })JSON");
    REQUIRE(json);

    JsonRulesFile rules;
    llvm::json::Path::Root root("test");
    REQUIRE(fromJSON(*json, rules, root));
    CHECK(rules.passes[0].name == "test-pass");
    CHECK(rules.passes[0].context == "isExpansionInMainFile()");
    CHECK(rules.passes[0].rules[0].replace.scope == "macro-expansion");
  }

  SECTION("missing required fields") {
    auto json = llvm::json::parse(R"JSON({
      "passes": [{
        "rules": [{
          "find": {},
          "replace": {"target": "x"}
        }]
      }]
    })JSON");
    REQUIRE(json);

    JsonRulesFile rules;
    llvm::json::Path::Root root("test");
    CHECK_FALSE(fromJSON(*json, rules, root));
  }
}

// ---------------------------------------------------------------------------
// Pipeline Construction
// ---------------------------------------------------------------------------

TEST_CASE("buildPipeline context combination", "[rules][pipeline]") {
  JsonRulesFile rules;
  JsonPass pass;
  pass.context = "isExpansionInMainFile()";
  JsonRule rule;
  rule.find.matcher = "binaryOperator(hasOperatorName(\"&&\")).bind(\"root\")";
  rule.replace.target = "root";
  rule.replace.scope = "node";
  rule.replace.withTempl = "{{root}}";
  pass.rules.push_back(rule);
  rules.passes.push_back(pass);

  auto pipeline = buildPipeline(rules);
  REQUIRE(pipeline);
  REQUIRE(pipeline->size() == 1);
  REQUIRE((*pipeline)[0].size() == 1);

  // The matcher expression should be wrapped in allOf with context
  CHECK((*pipeline)[0][0].matcherExpression ==
        "allOf(binaryOperator(hasOperatorName(\"&&\")).bind(\"root\"), "
        "isExpansionInMainFile())");
}

TEST_CASE("buildPipeline no context", "[rules][pipeline]") {
  JsonRulesFile rules;
  JsonPass pass;
  JsonRule rule;
  rule.find.matcher = "binaryOperator(hasOperatorName(\"&&\")).bind(\"root\")";
  rule.replace.target = "root";
  rule.replace.scope = "node";
  rule.replace.withTempl = "{{root}}";
  pass.rules.push_back(rule);
  rules.passes.push_back(pass);

  auto pipeline = buildPipeline(rules);
  REQUIRE(pipeline);
  // Without context, matcher stays as-is
  CHECK((*pipeline)[0][0].matcherExpression ==
        "binaryOperator(hasOperatorName(\"&&\")).bind(\"root\")");
}

TEST_CASE("buildPipeline invalid scope", "[rules][pipeline]") {
  JsonRulesFile rules;
  JsonPass pass;
  JsonRule rule;
  rule.find.matcher = "expr().bind(\"x\")";
  rule.replace.target = "x";
  rule.replace.scope = "invalid-scope";
  rule.replace.withTempl = "{{x}}";
  pass.rules.push_back(rule);
  rules.passes.push_back(pass);

  auto pipeline = buildPipeline(rules);
  CHECK_FALSE(pipeline);
  llvm::consumeError(pipeline.takeError());
}
