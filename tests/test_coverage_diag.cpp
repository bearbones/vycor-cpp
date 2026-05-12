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

#include "clang/Tooling/Tooling.h"

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

using namespace vycor;

// ============================================================================
// GlobalIndex — CoveragePropertyEntry storage
// ============================================================================

TEST_CASE("GlobalIndex stores and retrieves coverage properties",
          "[GlobalIndex][Coverage]") {
  GlobalIndex index;

  SECTION("empty index returns no results") {
    auto results = index.findClassMethods("MyClass");
    CHECK(results.empty());
    CHECK(index.coverageEntryCount() == 0);
    CHECK(index.allIndexedClasses().empty());
  }

  SECTION("add and retrieve by class name") {
    CoveragePropertyEntry e1;
    e1.qualifiedName = "MyClass::getValue";
    e1.enclosingClass = "MyClass";
    e1.headerPath = "MyClass.h";
    e1.sourceLine = 10;
    e1.gvaLinkage = 2;
    e1.signature = "int MyClass::getValue() const";

    CoveragePropertyEntry e2;
    e2.qualifiedName = "MyClass::doWork";
    e2.enclosingClass = "MyClass";
    e2.headerPath = "MyClass.h";
    e2.sourceLine = 15;
    e2.gvaLinkage = 2;
    e2.signature = "void MyClass::doWork(int)";

    index.addCoverageProperty(std::move(e1));
    index.addCoverageProperty(std::move(e2));

    auto results = index.findClassMethods("MyClass");
    REQUIRE(results.size() == 2);
    CHECK(results[0]->qualifiedName == "MyClass::getValue");
    CHECK(results[1]->qualifiedName == "MyClass::doWork");
    CHECK(index.coverageEntryCount() == 2);
  }

  SECTION("methods from different classes are separate") {
    CoveragePropertyEntry e1;
    e1.qualifiedName = "A::foo";
    e1.enclosingClass = "A";

    CoveragePropertyEntry e2;
    e2.qualifiedName = "B::bar";
    e2.enclosingClass = "B";

    index.addCoverageProperty(std::move(e1));
    index.addCoverageProperty(std::move(e2));

    CHECK(index.findClassMethods("A").size() == 1);
    CHECK(index.findClassMethods("B").size() == 1);
    CHECK(index.findClassMethods("C").empty());

    auto classes = index.allIndexedClasses();
    CHECK(classes.size() == 2);
  }
}

// ============================================================================
// Indexer — coverage property collection via in-memory compilation
// ============================================================================

TEST_CASE("Indexer collects coverage properties for class methods",
          "[Indexer][Coverage]") {
  GlobalIndex index;

  std::string code = R"(
    class MyClass {
    public:
      int getValue() const { return value_; }
      void doWork(int x) {
        value_ = x * 2;
        if (value_ > 100) value_ = 100;
        for (int i = 0; i < value_; ++i) { value_++; }
      }
    private:
      int value_ = 0;
    };
  )";

  IndexerActionFactory factory(index);
  REQUIRE(clang::tooling::runToolOnCodeWithArgs(factory.create(), code,
                                                {"-std=c++17"},
                                                "test_input.cpp"));

  auto methods = index.findClassMethods("MyClass");
  REQUIRE(methods.size() == 2);

  // Find each method by name.
  const CoveragePropertyEntry *getter = nullptr;
  const CoveragePropertyEntry *worker = nullptr;
  for (const auto *m : methods) {
    if (m->qualifiedName == "MyClass::getValue")
      getter = m;
    if (m->qualifiedName == "MyClass::doWork")
      worker = m;
  }
  REQUIRE(getter != nullptr);
  REQUIRE(worker != nullptr);

  // Both are implicitly inline (defined in class body).
  CHECK(getter->isInlined);
  CHECK(worker->isInlined);

  // The worker should have more statements than the getter.
  CHECK(worker->bodyStmtCount > getter->bodyStmtCount);

  // Both should have the same enclosing class.
  CHECK(getter->enclosingClass == "MyClass");
  CHECK(worker->enclosingClass == "MyClass");

  // Signatures should be populated.
  CHECK(getter->signature.find("getValue") != std::string::npos);
  CHECK(getter->signature.find("const") != std::string::npos);
  CHECK(worker->signature.find("doWork") != std::string::npos);
}

TEST_CASE("Indexer detects constexpr vs non-constexpr methods",
          "[Indexer][Coverage]") {
  GlobalIndex index;

  std::string code = R"(
    struct Calc {
      constexpr int add(int a, int b) const { return a + b; }
      int multiply(int a, int b) const {
        int result = 0;
        for (int i = 0; i < b; ++i) result += a;
        return result;
      }
    };
  )";

  IndexerActionFactory factory(index);
  REQUIRE(clang::tooling::runToolOnCodeWithArgs(factory.create(), code,
                                                {"-std=c++17"},
                                                "test_input.cpp"));

  auto methods = index.findClassMethods("Calc");
  REQUIRE(methods.size() == 2);

  const CoveragePropertyEntry *addMethod = nullptr;
  const CoveragePropertyEntry *mulMethod = nullptr;
  for (const auto *m : methods) {
    if (m->qualifiedName == "Calc::add")
      addMethod = m;
    if (m->qualifiedName == "Calc::multiply")
      mulMethod = m;
  }
  REQUIRE(addMethod != nullptr);
  REQUIRE(mulMethod != nullptr);

  CHECK(addMethod->isConstexpr);
  CHECK_FALSE(mulMethod->isConstexpr);
}

TEST_CASE("Indexer detects virtual vs non-virtual methods",
          "[Indexer][Coverage]") {
  GlobalIndex index;

  std::string code = R"(
    class Base {
    public:
      virtual int compute(int x) { return x * x; }
      int getType() const { return 42; }
    };
  )";

  IndexerActionFactory factory(index);
  REQUIRE(clang::tooling::runToolOnCodeWithArgs(factory.create(), code,
                                                {"-std=c++17"},
                                                "test_input.cpp"));

  auto methods = index.findClassMethods("Base");
  REQUIRE(methods.size() == 2);

  const CoveragePropertyEntry *virtualMethod = nullptr;
  const CoveragePropertyEntry *nonVirtualMethod = nullptr;
  for (const auto *m : methods) {
    if (m->qualifiedName == "Base::compute")
      virtualMethod = m;
    if (m->qualifiedName == "Base::getType")
      nonVirtualMethod = m;
  }
  REQUIRE(virtualMethod != nullptr);
  REQUIRE(nonVirtualMethod != nullptr);

  CHECK(virtualMethod->isVirtual);
  CHECK_FALSE(nonVirtualMethod->isVirtual);
}

TEST_CASE("Indexer skips implicit methods for coverage",
          "[Indexer][Coverage]") {
  GlobalIndex index;

  std::string code = R"(
    struct Pod {
      int x;
    };
  )";

  IndexerActionFactory factory(index);
  REQUIRE(clang::tooling::runToolOnCodeWithArgs(factory.create(), code,
                                                {"-std=c++17"},
                                                "test_input.cpp"));

  // Implicit constructors, destructors, etc. should not be indexed.
  CHECK(index.coverageEntryCount() == 0);
}

// ============================================================================
// Analyzer — coverage diagnostics
// ============================================================================

TEST_CASE("Coverage analyzer detects GVA linkage mismatch",
          "[Analyzer][Coverage]") {
  GlobalIndex index;

  CoveragePropertyEntry e1;
  e1.qualifiedName = "MyClass::getValue";
  e1.enclosingClass = "MyClass";
  e1.headerPath = "MyClass.h";
  e1.sourceLine = 10;
  e1.gvaLinkage = 2; // DiscardableODR
  e1.isInlined = true;
  e1.bodyStmtCount = 2;
  e1.signature = "int MyClass::getValue() const";

  CoveragePropertyEntry e2;
  e2.qualifiedName = "MyClass::getType";
  e2.enclosingClass = "MyClass";
  e2.headerPath = "MyClass.h";
  e2.sourceLine = 12;
  e2.gvaLinkage = 2; // DiscardableODR
  e2.isInlined = true;
  e2.bodyStmtCount = 2;
  e2.signature = "int MyClass::getType() const";

  CoveragePropertyEntry e3;
  e3.qualifiedName = "MyClass::doWork";
  e3.enclosingClass = "MyClass";
  e3.headerPath = "MyClass.h";
  e3.sourceLine = 15;
  e3.gvaLinkage = 4; // StrongExternal
  e3.isInlined = false;
  e3.bodyStmtCount = 20;
  e3.signature = "void MyClass::doWork(int)";

  index.addCoverageProperty(std::move(e1));
  index.addCoverageProperty(std::move(e2));
  index.addCoverageProperty(std::move(e3));

  std::vector<Diagnostic> diags;
  analyzeCoverageProperties(index, diags);

  // Should find GVA mismatch: doWork (StrongExternal) vs 2 others
  // (DiscardableODR).
  bool foundMismatch = false;
  for (const auto &d : diags) {
    if (d.kind == Diagnostic::Coverage_GVAMismatch) {
      foundMismatch = true;
      CHECK(d.message.find("MyClass") != std::string::npos);
      CHECK(d.message.find("doWork") != std::string::npos);
    }
  }
  CHECK(foundMismatch);
}

TEST_CASE("Coverage analyzer emits DiscardableODR warning",
          "[Analyzer][Coverage]") {
  GlobalIndex index;

  CoveragePropertyEntry e1;
  e1.qualifiedName = "Foo::bar";
  e1.enclosingClass = "Foo";
  e1.headerPath = "Foo.h";
  e1.sourceLine = 5;
  e1.gvaLinkage = 2; // DiscardableODR
  e1.signature = "void Foo::bar()";

  index.addCoverageProperty(std::move(e1));

  std::vector<Diagnostic> diags;
  analyzeCoverageProperties(index, diags);

  bool found = false;
  for (const auto &d : diags) {
    if (d.kind == Diagnostic::Coverage_DiscardableODR) {
      found = true;
      CHECK(d.message.find("GVA_DiscardableODR") != std::string::npos);
      CHECK(d.message.find("COMDAT") != std::string::npos);
    }
  }
  CHECK(found);
}

TEST_CASE("Coverage analyzer emits AvailableExternally warning",
          "[Analyzer][Coverage]") {
  GlobalIndex index;

  CoveragePropertyEntry e1;
  e1.qualifiedName = "Foo::baz";
  e1.enclosingClass = "Foo";
  e1.headerPath = "Foo.h";
  e1.sourceLine = 5;
  e1.gvaLinkage = 1; // AvailableExternally
  e1.signature = "void Foo::baz()";

  index.addCoverageProperty(std::move(e1));

  std::vector<Diagnostic> diags;
  analyzeCoverageProperties(index, diags);

  bool found = false;
  for (const auto &d : diags) {
    if (d.kind == Diagnostic::Coverage_AvailableExternally) {
      found = true;
      CHECK(d.message.find("AvailableExternally") != std::string::npos);
    }
  }
  CHECK(found);
}

TEST_CASE("Coverage analyzer detects property divergence",
          "[Analyzer][Coverage]") {
  GlobalIndex index;

  CoveragePropertyEntry e1;
  e1.qualifiedName = "Widget::getName";
  e1.enclosingClass = "Widget";
  e1.headerPath = "Widget.h";
  e1.sourceLine = 10;
  e1.gvaLinkage = 2;
  e1.isInlined = true;
  e1.bodyStmtCount = 5; // trivial (one-line getter)
  e1.signature = "const char* Widget::getName() const";

  CoveragePropertyEntry e2;
  e2.qualifiedName = "Widget::process";
  e2.enclosingClass = "Widget";
  e2.headerPath = "Widget.h";
  e2.sourceLine = 15;
  e2.gvaLinkage = 2; // same GVA
  e2.isInlined = true;
  e2.bodyStmtCount = 25; // complex
  e2.signature = "void Widget::process(int)";

  index.addCoverageProperty(std::move(e1));
  index.addCoverageProperty(std::move(e2));

  std::vector<Diagnostic> diags;
  analyzeCoverageProperties(index, diags);

  bool foundDivergence = false;
  for (const auto &d : diags) {
    if (d.kind == Diagnostic::Coverage_PropertyDivergence) {
      foundDivergence = true;
      CHECK(d.message.find("Widget") != std::string::npos);
      CHECK(d.message.find("process") != std::string::npos);
    }
  }
  CHECK(foundDivergence);
}

TEST_CASE("Coverage analyzer reports nothing for uniform methods",
          "[Analyzer][Coverage]") {
  GlobalIndex index;

  CoveragePropertyEntry e1;
  e1.qualifiedName = "Simple::getA";
  e1.enclosingClass = "Simple";
  e1.headerPath = "Simple.h";
  e1.sourceLine = 5;
  e1.gvaLinkage = 2;
  e1.bodyStmtCount = 2;
  e1.signature = "int Simple::getA() const";

  CoveragePropertyEntry e2;
  e2.qualifiedName = "Simple::getB";
  e2.enclosingClass = "Simple";
  e2.headerPath = "Simple.h";
  e2.sourceLine = 6;
  e2.gvaLinkage = 2;
  e2.bodyStmtCount = 2;
  e2.signature = "int Simple::getB() const";

  index.addCoverageProperty(std::move(e1));
  index.addCoverageProperty(std::move(e2));

  std::vector<Diagnostic> diags;
  analyzeCoverageProperties(index, diags);

  // Should NOT emit GVAMismatch or PropertyDivergence.
  for (const auto &d : diags) {
    CHECK(d.kind != Diagnostic::Coverage_GVAMismatch);
    CHECK(d.kind != Diagnostic::Coverage_PropertyDivergence);
  }
}

// ============================================================================
// End-to-end: compile → index → analyze
// ============================================================================

TEST_CASE("End-to-end coverage diagnostic pipeline",
          "[Analyzer][Coverage][E2E]") {
  GlobalIndex index;

  std::string code = R"(
    class Sensor {
    public:
      int id_ = 0;
      int getId() const { return id_; }
      double calibrate(double input) {
        double result = input;
        for (int i = 0; i < 10; ++i) {
          result = result * 0.99 + 0.01;
          if (result > 1.0) result = 1.0;
          if (result < 0.0) result = 0.0;
        }
        return result;
      }
    };
  )";

  // Phase 1: Index.
  IndexerActionFactory factory(index);
  REQUIRE(clang::tooling::runToolOnCodeWithArgs(factory.create(), code,
                                                {"-std=c++17"},
                                                "test_input.cpp"));

  auto methods = index.findClassMethods("Sensor");
  REQUIRE(methods.size() == 2);

  // Verify that the indexer captured different body complexities.
  const CoveragePropertyEntry *getter = nullptr;
  const CoveragePropertyEntry *calibrate = nullptr;
  for (const auto *m : methods) {
    if (m->qualifiedName == "Sensor::getId")
      getter = m;
    if (m->qualifiedName == "Sensor::calibrate")
      calibrate = m;
  }
  REQUIRE(getter != nullptr);
  REQUIRE(calibrate != nullptr);
  CHECK(getter->bodyStmtCount <= 8);
  CHECK(calibrate->bodyStmtCount > 8);

  // Phase 1.5: Coverage analysis.
  std::vector<Diagnostic> diags;
  analyzeCoverageProperties(index, diags);

  // Should have at least DiscardableODR diagnostics for inline methods.
  CHECK_FALSE(diags.empty());

  // Both methods are inline in a class body, so they should have the same
  // GVA linkage. Property divergence should be flagged for the complexity
  // difference between the trivial getter and the complex calibrate.
  bool foundDivergence = false;
  for (const auto &d : diags) {
    if (d.kind == Diagnostic::Coverage_PropertyDivergence &&
        d.message.find("calibrate") != std::string::npos) {
      foundDivergence = true;
    }
  }
  CHECK(foundDivergence);
}
