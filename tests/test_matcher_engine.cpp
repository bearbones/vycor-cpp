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

#include "vycor/morph/MatcherEngine.h"

#include <catch2/catch_test_macros.hpp>
#include <string>

using namespace vycor;

TEST_CASE("MatcherEngine::parse valid expressions", "[parse]") {
  std::string error;

  SECTION("simple function matcher") {
    auto result = MatcherEngine::parse("functionDecl()", error);
    REQUIRE(result.has_value());
    CHECK(error.empty());
  }

  SECTION("matcher with hasName") {
    auto result =
        MatcherEngine::parse("functionDecl(hasName(\"foo\"))", error);
    REQUIRE(result.has_value());
    CHECK(error.empty());
  }

  SECTION("nested matcher") {
    auto result = MatcherEngine::parse(
        "callExpr(callee(functionDecl(hasName(\"bar\"))))", error);
    REQUIRE(result.has_value());
    CHECK(error.empty());
  }

  SECTION("matcher with bind") {
    // The dynamic parser supports .bind("id") syntax directly.
    auto result = MatcherEngine::parse(
        "functionDecl(hasName(\"baz\")).bind(\"root\")", error);
    REQUIRE(result.has_value());
    CHECK(error.empty());
  }

  SECTION("binary operator matcher") {
    auto result = MatcherEngine::parse(
        "binaryOperator(hasOperatorName(\"&&\"))", error);
    REQUIRE(result.has_value());
    CHECK(error.empty());
  }
}

TEST_CASE("MatcherEngine::parse invalid expressions", "[parse]") {
  std::string error;

  SECTION("empty string") {
    auto result = MatcherEngine::parse("", error);
    CHECK_FALSE(result.has_value());
    CHECK_FALSE(error.empty());
  }

  SECTION("unknown matcher name") {
    auto result = MatcherEngine::parse("notARealMatcher()", error);
    CHECK_FALSE(result.has_value());
    CHECK_FALSE(error.empty());
  }

  SECTION("syntax error") {
    auto result = MatcherEngine::parse("functionDecl(", error);
    CHECK_FALSE(result.has_value());
    CHECK_FALSE(error.empty());
  }
}

TEST_CASE("MatcherEngine::addRule", "[addRule]") {
  MatcherEngine engine;
  std::string error;

  SECTION("valid rule is accepted") {
    TransformRule rule;
    rule.matcherExpression = "functionDecl(hasName(\"foo\"))";
    rule.bindId = "fn";
    rule.callback = [](const auto &) {
      return std::vector<clang::tooling::Replacement>{};
    };
    CHECK(engine.addRule(rule, error));
    CHECK(error.empty());
  }

  SECTION("invalid matcher expression is rejected") {
    TransformRule rule;
    rule.matcherExpression = "notReal()";
    rule.bindId = "x";
    rule.callback = [](const auto &) {
      return std::vector<clang::tooling::Replacement>{};
    };
    CHECK_FALSE(engine.addRule(rule, error));
    CHECK_FALSE(error.empty());
  }
}
