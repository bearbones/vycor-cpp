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
#include "vycor/morph/TransformPipeline.h"

#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <sstream>
#include <string>

// Helpers to load example files for comparison.
static std::string readFile(const std::string &path) {
  std::ifstream in(path);
  if (!in.is_open())
    return {};
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// These integration tests are initially set up as TDD stubs.
// They define the expected behavior but the transform callbacks are not yet
// implemented, so they are expected to fail until the callbacks are written.

TEST_CASE("Example files are loadable", "[transforms][smoke]") {
  // Verify that the example files exist and can be read.
  // Paths are relative to the project root; tests should be run from there.
  SECTION("macro_split examples exist") {
    auto input = readFile(std::string(PROJECT_SOURCE_DIR) + "/examples/macro_split/input.cpp");
    auto expected = readFile(std::string(PROJECT_SOURCE_DIR) + "/examples/macro_split/expected.cpp");
    CHECK_FALSE(input.empty());
    CHECK_FALSE(expected.empty());
    CHECK(input != expected);
  }

  SECTION("builder_to_struct examples exist") {
    auto input = readFile(std::string(PROJECT_SOURCE_DIR) + "/examples/builder_to_struct/input.cpp");
    auto expected = readFile(std::string(PROJECT_SOURCE_DIR) + "/examples/builder_to_struct/expected.cpp");
    CHECK_FALSE(input.empty());
    CHECK_FALSE(expected.empty());
    CHECK(input != expected);
  }

  SECTION("TransformPipeline executes with fixed compile args and no build-path") {
    vycor::TransformPipeline pipeline;
    std::string inputPath =
        std::string(PROJECT_SOURCE_DIR) + "/examples/macro_split/input.cpp";
    CHECK(pipeline.execute({}, {inputPath}, /*dryRun=*/true, {"-std=c++17"},
                           /*hasFixedCompileArgs=*/true) == 0);
    CHECK(pipeline.execute({}, {inputPath}, /*dryRun=*/true, {},
                           /*hasFixedCompileArgs=*/true) == 0);
    CHECK(pipeline.execute({}, {inputPath}, /*dryRun=*/true, {},
                           /*hasFixedCompileArgs=*/false) != 0);
  }
}

