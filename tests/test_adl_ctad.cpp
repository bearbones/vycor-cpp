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

#include "vycor/anneal/Analyzer.h"
#include "vycor/anneal/GlobalIndex.h"
#include "vycor/anneal/Indexer.h"

#include "clang/Tooling/CompilationDatabase.h"
#include "clang/Tooling/Tooling.h"

#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace vycor;

namespace {
std::string readFileContents(const std::string &path) {
  std::ifstream in(path);
  if (!in.is_open())
    return {};
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}
} // namespace

// ============================================================================
// GlobalIndex unit tests
// ============================================================================

TEST_CASE("GlobalIndex stores and retrieves function overloads",
          "[GlobalIndex]") {
  GlobalIndex index;

  SECTION("empty index returns no results") {
    auto results = index.findOverloads("MathLib::scale");
    CHECK(results.empty());
    CHECK(index.overloadCount() == 0);
  }

  SECTION("add and retrieve overloads") {
    index.addFunctionOverload({"MathLib::scale", "Core.hpp",
                               {"MathLib::Vector", "int"}, "void", 5});
    index.addFunctionOverload({"MathLib::scale", "Extension.hpp",
                               {"MathLib::Vector", "double"}, "void", 3});

    auto results = index.findOverloads("MathLib::scale");
    REQUIRE(results.size() == 2);
    CHECK(results[0]->headerPath == "Core.hpp");
    CHECK(results[1]->headerPath == "Extension.hpp");
    CHECK(index.overloadCount() == 2);
  }

  SECTION("different functions are separate") {
    index.addFunctionOverload(
        {"MathLib::scale", "Core.hpp", {"int"}, "void", 1});
    index.addFunctionOverload(
        {"MathLib::rotate", "Core.hpp", {"double"}, "void", 2});

    CHECK(index.findOverloads("MathLib::scale").size() == 1);
    CHECK(index.findOverloads("MathLib::rotate").size() == 1);
    CHECK(index.findOverloads("MathLib::translate").empty());
  }
}

TEST_CASE("GlobalIndex stores and retrieves deduction guides",
          "[GlobalIndex]") {
  GlobalIndex index;

  SECTION("empty index returns no results") {
    auto results = index.findDeductionGuides("Container");
    CHECK(results.empty());
    CHECK(index.guideCount() == 0);
  }

  SECTION("add and retrieve guides") {
    index.addDeductionGuide({"Container", "Guide.hpp", {"const char *"},
                             "Container<std::string>", 5});

    auto results = index.findDeductionGuides("Container");
    REQUIRE(results.size() == 1);
    CHECK(results[0]->templateName == "Container");
    CHECK(results[0]->deducedType == "Container<std::string>");
    CHECK(index.guideCount() == 1);
  }
}

// ============================================================================
// Indexer integration tests (using in-memory compilation)
// ============================================================================

TEST_CASE("Indexer collects function overloads from source",
          "[Indexer]") {
  GlobalIndex index;

  // Simple source with two overloads in a namespace.
  std::string code = R"(
    namespace MathLib {
      struct Vector {};
      void scale(Vector, int) {}
      void scale(Vector, double) {}
    }
  )";

  // Run the indexer on in-memory code.
  IndexerActionFactory factory(index);
  REQUIRE(clang::tooling::runToolOnCodeWithArgs(
      factory.create(), code, {"-std=c++17"}, "test_input.cpp"));

  auto overloads = index.findOverloads("MathLib::scale");
  REQUIRE(overloads.size() == 2);

  // Verify parameter types were extracted.
  bool hasInt = false, hasDouble = false;
  for (const auto *entry : overloads) {
    REQUIRE(entry->paramTypes.size() == 2);
    // Clang prints param types unqualified within their namespace context.
    CHECK((entry->paramTypes[0] == "Vector" ||
           entry->paramTypes[0] == "MathLib::Vector"));
    if (entry->paramTypes[1] == "int")
      hasInt = true;
    if (entry->paramTypes[1] == "double")
      hasDouble = true;
  }
  CHECK(hasInt);
  CHECK(hasDouble);
}

TEST_CASE("Indexer collects deduction guides from source",
          "[Indexer]") {
  GlobalIndex index;

  // Avoid #include <string> which requires system headers that may not
  // be available to the in-memory Clang tooling when built from source.
  std::string code = R"(
    struct MyString {
      const char *data;
      MyString(const char *s) : data(s) {}
    };

    template <typename T>
    struct Container {
      T value;
      Container(T v) : value(v) {}
    };

    Container(const char*) -> Container<MyString>;
  )";

  IndexerActionFactory factory(index);
  REQUIRE(clang::tooling::runToolOnCodeWithArgs(
      factory.create(), code, {"-std=c++17"}, "test_input.cpp"));

  auto guides = index.findDeductionGuides("Container");
  REQUIRE(guides.size() == 1);
  CHECK(guides[0]->templateName == "Container");
  // The deduced type should mention MyString.
  CHECK(guides[0]->deducedType.find("MyString") != std::string::npos);
}

TEST_CASE("Indexer skips implicit and compiler-generated declarations",
          "[Indexer]") {
  GlobalIndex index;

  // A struct with implicitly generated special member functions.
  std::string code = R"(
    struct Foo {
      int x;
    };
  )";

  IndexerActionFactory factory(index);
  REQUIRE(clang::tooling::runToolOnCodeWithArgs(
      factory.create(), code, {"-std=c++17"}, "test_input.cpp"));

  // Should not index implicit constructors, destructors, etc.
  CHECK(index.overloadCount() == 0);
}

// ============================================================================
// Analyzer integration tests — ADL
// ============================================================================

TEST_CASE("Analyzer detects fragile ADL when overload is invisible",
          "[Analyzer][ADL]") {
  // First, build a global index that knows about both overloads.
  // Note: Clang prints param types unqualified within namespace context,
  // so we use "Vector" not "MathLib::Vector" to match what the Indexer
  // would produce.
  GlobalIndex index;
  index.addFunctionOverload(
      {"MathLib::scale", "Core.hpp", {"Vector", "int"}, "void", 5});
  index.addFunctionOverload(
      {"MathLib::scale", "Extension.hpp", {"Vector", "double"}, "void", 3});

  // Now analyze code that only sees the int overload.
  std::string code = R"(
    namespace MathLib {
      struct Vector {};
      void scale(Vector, int) {}
    }
    void test() {
      MathLib::Vector v;
      scale(v, 3.14);
    }
  )";

  std::vector<Diagnostic> diagnostics;
  auto action = std::make_unique<AnalyzerAction>(index, diagnostics);
  REQUIRE(clang::tooling::runToolOnCodeWithArgs(
      std::move(action), code, {"-std=c++17"}, "test_input.cpp"));

  // Should detect that Extension.hpp has a scale(Vector, double) overload
  // that is not visible.
  REQUIRE(diagnostics.size() >= 1);
  CHECK(diagnostics[0].kind == Diagnostic::ADL_Fallback);
  CHECK(diagnostics[0].missingHeader == "Extension.hpp");
  CHECK(diagnostics[0].message.find("Extension.hpp") != std::string::npos);
}

TEST_CASE("Analyzer reports no diagnostic when all overloads are visible",
          "[Analyzer][ADL]") {
  GlobalIndex index;
  // Only one overload exists globally — no fragility.
  index.addFunctionOverload(
      {"MathLib::scale", "test_input.cpp", {"Vector", "int"}, "void", 5});

  std::string code = R"(
    namespace MathLib {
      struct Vector {};
      void scale(Vector, int) {}
    }
    void test() {
      MathLib::Vector v;
      scale(v, 42);
    }
  )";

  std::vector<Diagnostic> diagnostics;
  auto action = std::make_unique<AnalyzerAction>(index, diagnostics);
  REQUIRE(clang::tooling::runToolOnCodeWithArgs(
      std::move(action), code, {"-std=c++17"}, "test_input.cpp"));

  CHECK(diagnostics.empty());
}

TEST_CASE("Analyzer ignores explicitly qualified calls",
          "[Analyzer][ADL]") {
  GlobalIndex index;
  index.addFunctionOverload(
      {"MathLib::scale", "Core.hpp", {"Vector", "int"}, "void", 5});
  index.addFunctionOverload(
      {"MathLib::scale", "Extension.hpp", {"Vector", "double"}, "void", 3});

  // Explicitly qualified call — not an ADL concern.
  std::string code = R"(
    namespace MathLib {
      struct Vector {};
      void scale(Vector, int) {}
    }
    void test() {
      MathLib::Vector v;
      MathLib::scale(v, 3.14);
    }
  )";

  std::vector<Diagnostic> diagnostics;
  auto action = std::make_unique<AnalyzerAction>(index, diagnostics);
  REQUIRE(clang::tooling::runToolOnCodeWithArgs(
      std::move(action), code, {"-std=c++17"}, "test_input.cpp"));

  CHECK(diagnostics.empty());
}

TEST_CASE(
    "Analyzer does not warn when resolved operator overload is most specific",
    "[Analyzer][ADL][operator]") {
  // Reproduces the math-library false positive: the call is
  // float * Vector3 and the resolved operator*(float, const Vector3&) is the
  // most specific overload in the index. Unrelated overloads for Vector3D /
  // Matrix3 / double should not produce any warnings.
  GlobalIndex index;
  index.addFunctionOverload(
      {"MathLib::operator*", "Core.hpp", {"float", "const Vector3 &"},
       "Vector3", 10});
  index.addFunctionOverload(
      {"MathLib::operator*", "Precision.hpp",
       {"double", "const Vector3D &"}, "Vector3D", 20});
  index.addFunctionOverload(
      {"MathLib::operator*", "Matrix.hpp",
       {"const Matrix3 &", "const Matrix3 &"}, "Matrix3", 30});
  index.addFunctionOverload(
      {"MathLib::operator*", "Matrix.hpp",
       {"const Matrix3 &", "const Vector3 &"}, "Vector3", 45});

  std::string code = R"(
    namespace MathLib {
      struct Vector3 { float x, y, z; };
      Vector3 operator*(float f, const Vector3 &v) {
        return Vector3{v.x * f, v.y * f, v.z * f};
      }
    }
    void test() {
      float f = 2.0f;
      MathLib::Vector3 v3;
      auto result = f * v3;
      (void)result;
    }
  )";

  std::vector<Diagnostic> diagnostics;
  auto action = std::make_unique<AnalyzerAction>(index, diagnostics);
  REQUIRE(clang::tooling::runToolOnCodeWithArgs(
      std::move(action), code, {"-std=c++17"}, "test_input.cpp"));

  CHECK(diagnostics.empty());
}

TEST_CASE("Analyzer does not warn when call matches int overload exactly",
          "[Analyzer][ADL]") {
  // Narrowing-direction guard: the visible scale(Vector, int) is an exact
  // match for scale(v, 42). The invisible scale(Vector, double) would be a
  // worse match and should not be flagged.
  GlobalIndex index;
  index.addFunctionOverload(
      {"MathLib::scale", "Core.hpp", {"Vector", "int"}, "void", 5});
  index.addFunctionOverload(
      {"MathLib::scale", "Extension.hpp", {"Vector", "double"}, "void", 3});

  std::string code = R"(
    namespace MathLib {
      struct Vector {};
      void scale(Vector, int) {}
    }
    void test() {
      MathLib::Vector v;
      scale(v, 42);
    }
  )";

  std::vector<Diagnostic> diagnostics;
  auto action = std::make_unique<AnalyzerAction>(index, diagnostics);
  REQUIRE(clang::tooling::runToolOnCodeWithArgs(
      std::move(action), code, {"-std=c++17"}, "test_input.cpp"));

  CHECK(diagnostics.empty());
}

TEST_CASE(
    "Analyzer warns about potential ambiguity between incomparable overloads",
    "[Analyzer][ADL][ambiguity]") {
  // pick(Vector, int, double) is visible; pick(Vector, double, int) is
  // invisible. The call pick(v, 1, 2) with two int numeric arguments is
  // unambiguous right now (only the first overload is visible) but
  // including CoreB.hpp would make the resolution ambiguous: each overload
  // wins on exactly one numeric parameter position. The Vector argument
  // exists only to trigger ADL into the MathLib namespace.
  GlobalIndex index;
  index.addFunctionOverload(
      {"MathLib::pick", "CoreA.hpp", {"Vector", "int", "double"}, "void", 5});
  index.addFunctionOverload(
      {"MathLib::pick", "CoreB.hpp", {"Vector", "double", "int"}, "void", 7});

  std::string code = R"(
    namespace MathLib {
      struct Vector {};
      void pick(Vector, int, double) {}
    }
    void test() {
      MathLib::Vector v;
      pick(v, 1, 2);
    }
  )";

  std::vector<Diagnostic> diagnostics;
  auto action = std::make_unique<AnalyzerAction>(index, diagnostics);
  REQUIRE(clang::tooling::runToolOnCodeWithArgs(
      std::move(action), code, {"-std=c++17"}, "test_input.cpp"));

  REQUIRE(diagnostics.size() == 1);
  CHECK(diagnostics[0].kind == Diagnostic::ADL_Ambiguity);
  CHECK(diagnostics[0].missingHeader == "CoreB.hpp");
  CHECK(diagnostics[0].message.find("ambiguous") != std::string::npos);
}

TEST_CASE("Analyzer does not warn about ambiguity for unrelated overloads",
          "[Analyzer][ADL][ambiguity]") {
  // Same unrelated operator* overloads as the "most specific" test, but
  // this one asserts that the ambiguity branch also stays silent.
  GlobalIndex index;
  index.addFunctionOverload(
      {"MathLib::operator*", "Core.hpp", {"float", "const Vector3 &"},
       "Vector3", 10});
  index.addFunctionOverload(
      {"MathLib::operator*", "Precision.hpp",
       {"double", "const Vector3D &"}, "Vector3D", 20});
  index.addFunctionOverload(
      {"MathLib::operator*", "Matrix.hpp",
       {"const Matrix3 &", "const Matrix3 &"}, "Matrix3", 30});

  std::string code = R"(
    namespace MathLib {
      struct Vector3 { float x, y, z; };
      Vector3 operator*(float f, const Vector3 &v) {
        return Vector3{v.x * f, v.y * f, v.z * f};
      }
    }
    void test() {
      float f = 2.0f;
      MathLib::Vector3 v3;
      auto result = f * v3;
      (void)result;
    }
  )";

  std::vector<Diagnostic> diagnostics;
  auto action = std::make_unique<AnalyzerAction>(index, diagnostics);
  REQUIRE(clang::tooling::runToolOnCodeWithArgs(
      std::move(action), code, {"-std=c++17"}, "test_input.cpp"));

  for (const auto &d : diagnostics) {
    CHECK(d.kind != Diagnostic::ADL_Ambiguity);
  }
  CHECK(diagnostics.empty());
}

// ============================================================================
// Analyzer integration tests — CTAD
// ============================================================================

TEST_CASE("Analyzer detects fragile CTAD when guide is invisible",
          "[Analyzer][CTAD]") {
  GlobalIndex index;
  index.addDeductionGuide({"Container", "Guide.hpp", {"const char *"},
                           "Container<std::string>", 5});

  // Code that uses CTAD without the explicit guide visible.
  std::string code = R"(
    template <typename T>
    struct Container {
      T value;
      Container(T v) : value(v) {}
    };

    void test() {
      Container c("Hello");
    }
  )";

  std::vector<Diagnostic> diagnostics;
  auto action = std::make_unique<AnalyzerAction>(index, diagnostics);
  REQUIRE(clang::tooling::runToolOnCodeWithArgs(
      std::move(action), code, {"-std=c++17"}, "test_input.cpp"));

  // Should detect that Guide.hpp has a deduction guide producing
  // Container<std::string> which isn't visible here.
  REQUIRE(diagnostics.size() >= 1);
  CHECK(diagnostics[0].kind == Diagnostic::CTAD_Fallback);
  CHECK(diagnostics[0].missingHeader == "Guide.hpp");
}

TEST_CASE("Analyzer reports no CTAD diagnostic when no guides exist",
          "[Analyzer][CTAD]") {
  GlobalIndex index;
  // No deduction guides in the global index.

  std::string code = R"(
    template <typename T>
    struct Container {
      T value;
      Container(T v) : value(v) {}
    };

    void test() {
      Container c(42);
    }
  )";

  std::vector<Diagnostic> diagnostics;
  auto action = std::make_unique<AnalyzerAction>(index, diagnostics);
  REQUIRE(clang::tooling::runToolOnCodeWithArgs(
      std::move(action), code, {"-std=c++17"}, "test_input.cpp"));

  CHECK(diagnostics.empty());
}

// ============================================================================
// Analyzer integration tests — same-score ties (opt-in diagnostic)
// ============================================================================

TEST_CASE(
    "Analyzer does not emit same-score diagnostic when flag is off",
    "[Analyzer][ADL][same-score]") {
  // Two overloads that tie on a `long` argument: neither int nor float is a
  // strictly better match. Without --warn-same-score the analyzer stays
  // silent — that's the pre-existing "dropped, not better" behaviour.
  GlobalIndex index;
  index.addFunctionOverload(
      {"MathLib::scale", "Core.hpp", {"Vector", "int"}, "void", 5});
  index.addFunctionOverload(
      {"MathLib::scale", "Extension.hpp", {"Vector", "float"}, "void", 7});

  std::string code = R"(
    namespace MathLib {
      struct Vector {};
      void scale(Vector, int) {}
    }
    void test() {
      MathLib::Vector v;
      long amount = 3;
      scale(v, amount);
    }
  )";

  std::vector<Diagnostic> diagnostics;
  auto action = std::make_unique<AnalyzerAction>(index, diagnostics);
  REQUIRE(clang::tooling::runToolOnCodeWithArgs(
      std::move(action), code, {"-std=c++17"}, "test_input.cpp"));

  for (const auto &d : diagnostics)
    CHECK(d.kind != Diagnostic::ADL_SameScore);
}

TEST_CASE(
    "Analyzer emits same-score diagnostic when flag is on",
    "[Analyzer][ADL][same-score]") {
  GlobalIndex index;
  index.addFunctionOverload(
      {"MathLib::scale", "Core.hpp", {"Vector", "int"}, "void", 5});
  index.addFunctionOverload(
      {"MathLib::scale", "Extension.hpp", {"Vector", "float"}, "void", 7});

  std::string code = R"(
    namespace MathLib {
      struct Vector {};
      void scale(Vector, int) {}
    }
    void test() {
      MathLib::Vector v;
      long amount = 3;
      scale(v, amount);
    }
  )";

  AnalysisOptions opts;
  opts.warnSameScore = true;

  std::vector<Diagnostic> diagnostics;
  auto action = std::make_unique<AnalyzerAction>(index, diagnostics, opts);
  REQUIRE(clang::tooling::runToolOnCodeWithArgs(
      std::move(action), code, {"-std=c++17"}, "test_input.cpp"));

  size_t sameScoreCount = 0;
  for (const auto &d : diagnostics) {
    if (d.kind == Diagnostic::ADL_SameScore) {
      CHECK(d.missingHeader == "Extension.hpp");
      CHECK(d.message.find("Same-score") != std::string::npos);
      ++sameScoreCount;
    }
  }
  CHECK(sameScoreCount == 1);
}

TEST_CASE(
    "Analyzer same-score diagnostic fires for each tied invisible candidate",
    "[Analyzer][ADL][same-score]") {
  // Three overloads indexed: the int one is visible, the float and unsigned
  // int ones are invisible. Both invisibles tie the resolved on every
  // position for a `long` argument → two ADL_SameScore diagnostics.
  GlobalIndex index;
  index.addFunctionOverload(
      {"MathLib::pick", "Core.hpp", {"Vector", "int"}, "void", 5});
  index.addFunctionOverload(
      {"MathLib::pick", "Extension.hpp", {"Vector", "float"}, "void", 7});
  index.addFunctionOverload(
      {"MathLib::pick", "Wide.hpp", {"Vector", "unsigned int"}, "void", 9});

  std::string code = R"(
    namespace MathLib {
      struct Vector {};
      void pick(Vector, int) {}
    }
    void test() {
      MathLib::Vector v;
      long amount = 3;
      pick(v, amount);
    }
  )";

  AnalysisOptions opts;
  opts.warnSameScore = true;

  std::vector<Diagnostic> diagnostics;
  auto action = std::make_unique<AnalyzerAction>(index, diagnostics, opts);
  REQUIRE(clang::tooling::runToolOnCodeWithArgs(
      std::move(action), code, {"-std=c++17"}, "test_input.cpp"));

  size_t sameScoreCount = 0;
  for (const auto &d : diagnostics)
    if (d.kind == Diagnostic::ADL_SameScore)
      ++sameScoreCount;
  CHECK(sameScoreCount == 2);
}

// ============================================================================
// Analyzer integration tests — convertibility modeling
// ============================================================================

TEST_CASE(
    "Analyzer licenses converting-ctor candidate only with conv model",
    "[Analyzer][ADL][convertibility]") {
  // Visible route(int); invisible route(Mile) where Mile has a non-explicit
  // ctor from double. Call site passes a double — Clang matches the int
  // overload via a narrowing standard conversion, and IgnoreImpCasts leaves
  // the arg type as "double". Legacy scoring drops the Mile candidate
  // (class vs. arithmetic, neither side is the resolved param); the conv
  // model must license it via ctorEdges["Mile"] and the scores tie →
  // ADL_SameScore is emitted when both flags are on, nothing otherwise.
  //
  // The harness function lives inside namespace Distance so that the
  // unqualified call `route(x)` resolves to Distance::route by ordinary
  // lookup — putting the call at global scope would not compile because
  // `double` cannot trigger ADL into the Distance namespace.
  GlobalIndex index;
  index.addFunctionOverload(
      {"Distance::route", "Core.hpp", {"int"}, "void", 5});
  index.addFunctionOverload(
      {"Distance::route", "Imperial.hpp", {"Mile"}, "void", 9});
  // Manually declare the non-explicit converting ctor edge that a real
  // indexer run would have produced. The key matches the unqualified form
  // that Clang prints for a parameter whose type is declared in the same
  // namespace — the same spelling the analyzer normalizes from the indexed
  // FunctionOverloadEntry.
  index.mutableTypeRelations().addCtorEdge("Mile", "double");

  std::string code = R"(
    namespace Distance {
      struct Mile { Mile(double); };
      void route(int) {}
      inline void harness() {
        double x = 3.14;
        route(x);
      }
    }
  )";

  SECTION("no flags → no diagnostic") {
    std::vector<Diagnostic> diagnostics;
    auto action = std::make_unique<AnalyzerAction>(index, diagnostics);
    REQUIRE(clang::tooling::runToolOnCodeWithArgs(
        std::move(action), code, {"-std=c++17"}, "test_input.cpp"));
    CHECK(diagnostics.empty());
  }

  SECTION("conv model on, warnSameScore off → no diagnostic") {
    AnalysisOptions opts;
    opts.modelConvertibility = true;
    std::vector<Diagnostic> diagnostics;
    auto action =
        std::make_unique<AnalyzerAction>(index, diagnostics, opts);
    REQUIRE(clang::tooling::runToolOnCodeWithArgs(
        std::move(action), code, {"-std=c++17"}, "test_input.cpp"));
    CHECK(diagnostics.empty());
  }

  SECTION("conv model on, warnSameScore on → ADL_SameScore") {
    AnalysisOptions opts;
    opts.modelConvertibility = true;
    opts.warnSameScore = true;
    std::vector<Diagnostic> diagnostics;
    auto action =
        std::make_unique<AnalyzerAction>(index, diagnostics, opts);
    REQUIRE(clang::tooling::runToolOnCodeWithArgs(
        std::move(action), code, {"-std=c++17"}, "test_input.cpp"));
    REQUIRE(diagnostics.size() == 1);
    CHECK(diagnostics[0].kind == Diagnostic::ADL_SameScore);
    CHECK(diagnostics[0].missingHeader == "Imperial.hpp");
  }
}

TEST_CASE(
    "Analyzer does not license candidate when only explicit ctor exists",
    "[Analyzer][ADL][convertibility]") {
  // Same scenario as the converting-ctor test, but no ctor edge is
  // registered — simulating an `explicit Mile(double)` which the indexer
  // filters out via its `isExplicit()` guard. The candidate stays filtered
  // even with both opt-in flags on.
  GlobalIndex index;
  index.addFunctionOverload(
      {"Distance::route", "Core.hpp", {"int"}, "void", 5});
  index.addFunctionOverload(
      {"Distance::route", "Imperial.hpp", {"Mile"}, "void", 9});
  // Deliberately no addCtorEdge call here — explicit ctors aren't indexed.

  std::string code = R"(
    namespace Distance {
      struct Mile { explicit Mile(double); };
      void route(int) {}
      inline void harness() {
        double x = 3.14;
        route(x);
      }
    }
  )";

  AnalysisOptions opts;
  opts.modelConvertibility = true;
  opts.warnSameScore = true;

  std::vector<Diagnostic> diagnostics;
  auto action = std::make_unique<AnalyzerAction>(index, diagnostics, opts);
  REQUIRE(clang::tooling::runToolOnCodeWithArgs(
      std::move(action), code, {"-std=c++17"}, "test_input.cpp"));

  CHECK(diagnostics.empty());
}

TEST_CASE(
    "TypeRelationIndex tracks inheritance and conversion edges",
    "[GlobalIndex][TypeRelation]") {
  TypeRelationIndex rels;

  SECTION("isBaseOrSelf handles identity, direct, and transitive bases") {
    rels.addBase("Derived", "Base");
    rels.addBase("GrandChild", "Derived");
    CHECK(rels.isBaseOrSelf("Derived", "Derived"));
    CHECK(rels.isBaseOrSelf("Derived", "Base"));
    CHECK(rels.isBaseOrSelf("GrandChild", "Base"));
    CHECK_FALSE(rels.isBaseOrSelf("Base", "Derived"));
    CHECK_FALSE(rels.isBaseOrSelf("Unrelated", "Base"));
  }

  SECTION("isConvertible identity and arithmetic") {
    CHECK(rels.isConvertible("int", "int"));
    CHECK(rels.isConvertible("int", "double"));
    CHECK(rels.isConvertible("long", "float"));
    CHECK_FALSE(rels.isConvertible("int", "Mile"));
  }

  SECTION("isConvertible accepts inheritance for references and pointers") {
    rels.addBase("Derived", "Base");
    CHECK(rels.isConvertible("Derived", "Base"));
    CHECK(rels.isConvertible("Derived*", "Base*"));
    CHECK_FALSE(rels.isConvertible("Base", "Derived"));
  }

  SECTION("isConvertible uses converting ctor and conv-op edges") {
    rels.addCtorEdge("Mile", "double");
    rels.addConvOpEdge("Temp", "double");
    CHECK(rels.isConvertible("double", "Mile"));
    CHECK(rels.isConvertible("Temp", "double"));
    CHECK_FALSE(rels.isConvertible("int", "Mile"));
    CHECK_FALSE(rels.isConvertible("Temp", "float"));
  }
}

TEST_CASE("Indexer populates TypeRelationIndex from bases and conversions",
          "[Indexer][TypeRelation]") {
  GlobalIndex index;

  std::string code = R"(
    namespace N {
      struct Base {};
      struct Derived : Base {};
      struct Mile {
        Mile(double);
      };
      struct Explicit {
        explicit Explicit(int);
      };
      struct Temp {
        operator double() const;
      };
    }
  )";

  IndexerActionFactory factory(index);
  REQUIRE(clang::tooling::runToolOnCodeWithArgs(
      factory.create(), code, {"-std=c++17"}, "test_input.cpp"));

  const auto &rels = index.typeRelations();
  CHECK(rels.isBaseOrSelf("N::Derived", "N::Base"));
  CHECK(rels.isConvertible("N::Derived", "N::Base"));
  CHECK(rels.isConvertible("double", "N::Mile"));
  // Explicit ctors are filtered — no edge registered.
  CHECK_FALSE(rels.isConvertible("int", "N::Explicit"));
  CHECK(rels.isConvertible("N::Temp", "double"));
}

// ============================================================================
// Example fixture smoke tests
// ============================================================================

TEST_CASE("adl_same_score example fixture files are loadable",
          "[Analyzer][ADL][smoke]") {
  const std::string base =
      std::string(PROJECT_SOURCE_DIR) + "/examples/adl_same_score/";
  CHECK_FALSE(readFileContents(base + "Core.hpp").empty());
  CHECK_FALSE(readFileContents(base + "Extension.hpp").empty());
  CHECK_FALSE(readFileContents(base + "order_a.cpp").empty());
  CHECK_FALSE(readFileContents(base + "order_b.cpp").empty());
}
