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
#include "vycor/morph/TransformPipeline.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include <catch2/catch_test_macros.hpp>

namespace {

std::string readFile(llvm::StringRef path) {
  auto buffer = llvm::MemoryBuffer::getFile(path);
  REQUIRE(buffer);
  return (*buffer)->getBuffer().str();
}

struct SourceFile {
  llvm::SmallString<128> path;
  llvm::FileRemover cleanup;
  explicit SourceFile(llvm::StringRef content) {
    int fd;
    REQUIRE_FALSE(
        llvm::sys::fs::createTemporaryFile("vycor-morph", "cpp", fd, path));
    cleanup.setFile(path);
    llvm::raw_fd_ostream out(fd, true);
    out << content;
  }
};

vycor::TransformRule literalRule(unsigned value, llvm::StringRef replacement) {
  auto tmpl = vycor::parseTemplate(replacement);
  REQUIRE(tmpl);
  return {"integerLiteral(equals(" + std::to_string(value) + "))", "literal",
          vycor::makeTemplateCallback(std::move(*tmpl), "literal", "node", {})};
}

} // namespace

TEST_CASE("morph previews and applies the published macro example",
          "[transforms]") {
  const auto example =
      std::string(PROJECT_SOURCE_DIR) + "/examples/macro_split/";
  const auto original = readFile(example + "input.cpp");
  const auto expected = readFile(example + "expected.cpp");
  SourceFile source(original);
  const std::vector<std::string> files{source.path.str().str()};
  auto rules = vycor::parseRulesFile(example + "rules.json");
  REQUIRE(rules);
  auto passes = vycor::buildPipeline(*rules);
  REQUIRE(passes);
  vycor::TransformPipeline pipeline;
  for (auto &pass : *passes)
    pipeline.addPass(std::move(pass));

  REQUIRE(pipeline.execute({}, files, true, {"-std=c++17"}) == 0);
  CHECK(readFile(source.path) == original);
  const auto &edits = pipeline.getReplacements();
  REQUIRE(edits.size() == 1);
  auto preview =
      clang::tooling::applyAllReplacements(original, edits.begin()->second);
  REQUIRE(preview);
  CHECK(*preview == expected);
  REQUIRE(pipeline.execute({}, files, false, {"-std=c++17"}) == 0);
  CHECK(readFile(source.path) == expected);
  REQUIRE(pipeline.execute({}, files, true, {"-std=c++17"}) == 0);
  CHECK(pipeline.getReplacements().empty());
}

TEST_CASE("morph composes sequential passes in original coordinates",
          "[transforms]") {
  const std::string original = "int a = 1, b = 2;\n";
  SourceFile source(original);
  const std::vector<std::string> files{source.path.str().str()};
  vycor::TransformPipeline pipeline;
  pipeline.addPass({literalRule(1, "12345")});
  pipeline.addPass({literalRule(12345, "3"), literalRule(2, "40")});

  REQUIRE(pipeline.execute({}, files, true, {}, /*hasFixedCompileArgs=*/true) ==
          0);
  CHECK(readFile(source.path) == original);
  REQUIRE(pipeline.getReplacements().size() == 1);
  auto finalText = clang::tooling::applyAllReplacements(
      original, pipeline.getReplacements().begin()->second);
  REQUIRE(finalText);
  CHECK(*finalText == "int a = 3, b = 40;\n");
  REQUIRE(pipeline.execute({}, files, false, {"-std=c++17"}) == 0);
  CHECK(readFile(source.path) == *finalText);
  CHECK(pipeline.execute({}, files, true) != 0);
  CHECK(pipeline.getReplacements().empty());
}

TEST_CASE("morph abandons staged changes when a later pass fails",
          "[transforms]") {
  const std::string original = "int a = 1;\n";
  SourceFile source(original);
  vycor::TransformPipeline pipeline;
  pipeline.addPass({literalRule(1, "12")});
  SECTION("conflicting replacements") {
    pipeline.addPass({literalRule(12, "2"), literalRule(12, "3")});
  }
  SECTION("unbound template expression") {
    pipeline.addPass({literalRule(12, "{{missing}}")});
  }
  SECTION("invalid matcher") {
    auto bad = literalRule(12, "2");
    bad.matcherExpression = "invalidMatcher()";
    pipeline.addPass({std::move(bad)});
  }
  REQUIRE(pipeline.execute({}, {source.path.str().str()}, false,
                           {"-std=c++17"}) != 0);
  CHECK(readFile(source.path) == original);
  CHECK(pipeline.getReplacements().empty());
}

TEST_CASE("morph composes edits to one header included through multiple names",
          "[transforms]") {
  SourceFile header("constexpr int shared = 1;\n");
  const std::string path = header.path.str().str();
  const std::string alias = path + ".alias";
  REQUIRE_FALSE(llvm::sys::fs::create_hard_link(path, alias));
  llvm::FileRemover removeAlias(alias);
  SourceFile first("#include \"" + path + "\"\n");
  SourceFile second("#include \"" + alias + "\"\n");
  vycor::TransformPipeline pipeline;
  pipeline.addPass({literalRule(1, "12345")});
  pipeline.addPass({literalRule(12345, "30")});
  REQUIRE(pipeline.execute({},
                           {first.path.str().str(), second.path.str().str()},
                           false, {"-std=c++17"}) == 0);
  CHECK(pipeline.getReplacements().size() == 1);
  CHECK(readFile(path) == "constexpr int shared = 30;\n");
  CHECK(readFile(alias) == readFile(path));
}

TEST_CASE("morph preserves a macro at the boundary of a bound expression",
          "[transforms]") {
  SourceFile source("#define ONE 1\nint a = ONE + 2;\n");
  auto tmpl = vycor::parseTemplate("({{sum}})");
  REQUIRE(tmpl);
  vycor::TransformPipeline pipeline;
  pipeline.addPass(
      {{"binaryOperator(hasOperatorName(\"+\"))", "sum",
        vycor::makeTemplateCallback(std::move(*tmpl), "sum", "node", {})}});
  REQUIRE(pipeline.execute({}, {source.path.str().str()}, false,
                           {"-std=c++17"}) == 0);
  CHECK(readFile(source.path) == "#define ONE 1\nint a = (ONE + 2);\n");
}

TEST_CASE("macro-expansion rules can opt in to body tokens", "[transforms]") {
  const std::string original = "#define OR(a,b) ((a) || (b))\nbool f(bool a, "
                               "bool b) { return OR(a,b); }\n";
  SourceFile source(original);
  auto tmpl = vycor::parseTemplate("true");
  REQUIRE(tmpl);
  bool argOnly = true;
  SECTION("argument tokens only") {}
  SECTION("macro body") { argOnly = false; }
  vycor::TransformPipeline pipeline;
  pipeline.addPass(
      {{"binaryOperator(hasOperatorName(\"||\"))", "op",
        vycor::makeTemplateCallback(std::move(*tmpl), "op", "macro-expansion",
                                    {"OR"}, argOnly)}});
  REQUIRE(pipeline.execute({}, {source.path.str().str()}, false,
                           {"-std=c++17"}) == 0);
  CHECK(readFile(source.path) ==
        (argOnly ? original
                 : "#define OR(a,b) ((a) || (b))\nbool f(bool a, bool b) { "
                   "return true; }\n"));
}

TEST_CASE("a nested macro cannot hide the rest of an assertion argument",
          "[transforms]") {
  const std::string prefix = "#define READY(x) (x)\n#define INNER(x) "
                             "((void)(x))\n#define ASSERT(x) INNER(x)\n"
                             "void f(bool a, bool b) { ASSERT(a || READY(a) && "
                             "b); ASSERT(a && READY(b) || b); ";
  SourceFile source(prefix + "ASSERT(READY(a) && b); }\n");
  auto tmpl =
      vycor::parseTemplate("both({{root | flatten(\"&&\") | join(\", \")}})");
  REQUIRE(tmpl);
  vycor::TransformPipeline pipeline;
  pipeline.addPass(
      {{"binaryOperator(hasOperatorName(\"&&\"))", "root",
        vycor::makeTemplateCallback(std::move(*tmpl), "root", "macro-expansion",
                                    {"ASSERT"})}});
  REQUIRE(pipeline.execute({}, {source.path.str().str()}, false,
                           {"-std=c++17"}) == 0);
  CHECK(readFile(source.path) == prefix + "both(READY(a), b); }\n");
}
