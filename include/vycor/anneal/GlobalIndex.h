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

#include "vycor/callgraph/StringInterner.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace vycor {

// A single function overload discovered during indexing.
struct FunctionOverloadEntry {
  std::string qualifiedName; // e.g. "MathLib::scale"
  std::string headerPath;   // file where declared
  std::vector<std::string> paramTypes; // e.g. {"MathLib::Vector", "double"}
  std::string returnType;
  unsigned sourceLine = 0;
};

// A single deduction guide discovered during indexing.
struct DeductionGuideEntry {
  std::string templateName;            // e.g. "Container"
  std::string headerPath;             // file where declared
  std::vector<std::string> paramTypes; // e.g. {"const char *"}
  std::string deducedType;            // e.g. "Container<std::string>"
  unsigned sourceLine = 0;
};

// Coverage-relevant properties of a header-defined class member function.
// Used to diagnose why certain inline methods get dummy coverage records
// (hash 0x0) while sibling methods in the same class do not.
struct CoveragePropertyEntry {
  std::string qualifiedName;    // e.g. "MyClass::getValue"
  std::string headerPath;
  unsigned sourceLine = 0;
  std::string enclosingClass;   // qualified name of parent CXXRecordDecl

  // GVA linkage — the primary signal for coverage instrumentation fate.
  // Maps to clang::GVALinkage: 0=Internal, 1=AvailableExternally,
  // 2=DiscardableODR, 3=StrongODR, 4=StrongExternal
  int gvaLinkage = -1;

  bool isInlined = false;
  bool isConstexpr = false;
  bool isDefaulted = false;
  bool isTrivial = false;
  bool isVirtual = false;
  bool isStaticMethod = false;
  bool isImplicitlyInstantiable = false;
  int templatedKind = 0;
  int storageClass = 0;
  int formalLinkage = 0;
  unsigned bodyStmtCount = 0;   // body complexity heuristic
  std::string signature;        // e.g. "int MyClass::getValue() const"
};

// Type relation index: the conservative convertibility model consulted by
// the analyzer's overload scorer. Populated during phase 1 indexing from
// CXXRecordDecl bases, non-explicit converting constructors, and non-explicit
// conversion operators. Keys and values are normalized through
// `normalizeTypeForMatching` so lookup is spelling-insensitive.
//
// Scope and limitations (the scorer documents these again at its call site):
//  - single-hop only: we do not chain A -> B -> C
//  - string-based type identity (no ODR / typedef transparency beyond what
//    the indexer canonicalizes before inserting)
//  - no template deduction
//  - explicit ctors and conversion operators are filtered out by the indexer
//
// This structure is intentionally coarser than Clang Sema. It exists to
// drop candidates that clearly cannot apply and to license candidates that
// are viable via a standard conversion path the analyzer would otherwise
// miss — nothing more.
struct TypeRelationIndex {
  void addBase(const std::string &derived, const std::string &base);
  void addCtorEdge(const std::string &toType, const std::string &fromType);
  void addConvOpEdge(const std::string &fromType, const std::string &toType);

  // Transitive base-class check (cycle-safe). Returns true when
  // `derived == maybeBase` or when `maybeBase` appears anywhere on the
  // chain of base classes reachable from `derived`.
  bool isBaseOrSelf(const std::string &derived,
                    const std::string &maybeBase) const;

  // Conservative single-hop convertibility check used by the analyzer. The
  // rules (in order): identity after normalization, both arithmetic builtins,
  // pointer/reference-stripped derived-to-base, a converting constructor
  // edge, a conversion-operator edge. Returns false otherwise.
  bool isConvertible(const std::string &from, const std::string &to) const;

private:
  using SId = StringInterner::Id;
  StringInterner interner_;

  // Normalized derived className -> list of direct base class names.
  std::unordered_map<SId, std::vector<SId>> bases_;

  // Normalized target type -> list of source types accepted by a non-explicit
  // single-argument converting constructor of that target.
  std::unordered_map<SId, std::vector<SId>> ctorEdges_;

  // Normalized source type -> list of target types reachable via a non-explicit
  // conversion operator on the source.
  std::unordered_map<SId, std::vector<SId>> convOpEdges_;
};

// A diagnostic emitted when analysis finds an issue.
struct Diagnostic {
  enum Kind {
    ADL_Fallback,
    ADL_Ambiguity,                // including the missing header would make
                                  // this call an ambiguous overload resolution
    ADL_SameScore,                // an invisible overload ties the resolved
                                  // one on every argument position — inclusion
                                  // order silently decides which wins
    CTAD_Fallback,
    Coverage_GVAMismatch,         // siblings have different GVA linkage
    Coverage_DiscardableODR,      // method has GVA_DiscardableODR (COMDAT risk)
    Coverage_AvailableExternally, // method may be discarded by optimizer
    Coverage_PropertyDivergence,  // siblings diverge on complexity/properties
    DeadCode_Pessimistic,         // function unreachable via proven paths
    DeadCode_Optimistic,          // function reachable only via plausible paths
  };
  Kind kind;
  std::string callLocation;  // file:line:col of the call site
  std::string resolvedDecl;  // what the compiler chose
  std::string betterDecl;    // what the global index found
  std::string missingHeader; // which header to include
  std::string message;       // human-readable diagnostic
};

// Project-wide database of all function overloads and deduction guides.
class GlobalIndex {
public:
  void addFunctionOverload(FunctionOverloadEntry entry);
  void addDeductionGuide(DeductionGuideEntry entry);

  // Find all overloads for a given qualified function name.
  std::vector<const FunctionOverloadEntry *>
  findOverloads(const std::string &qualifiedName) const;

  // Find all deduction guides for a given template name.
  std::vector<const DeductionGuideEntry *>
  findDeductionGuides(const std::string &templateName) const;

  // Coverage property tracking.
  void addCoverageProperty(CoveragePropertyEntry entry);

  std::vector<const CoveragePropertyEntry *>
  findClassMethods(const std::string &enclosingClass) const;

  std::vector<std::string> allIndexedClasses() const;

  // Type relation index — populated by the indexer, queried by the analyzer.
  const TypeRelationIndex &typeRelations() const { return types_; }
  TypeRelationIndex &mutableTypeRelations() { return types_; }

  // Total counts for testing/debugging.
  size_t overloadCount() const;
  size_t guideCount() const;
  size_t coverageEntryCount() const;

private:
  using SId = StringInterner::Id;
  StringInterner interner_;
  std::unordered_map<SId, std::vector<FunctionOverloadEntry>> overloads_;
  std::unordered_map<SId, std::vector<DeductionGuideEntry>> guides_;
  std::unordered_map<SId, std::vector<CoveragePropertyEntry>> coverageProps_;
  TypeRelationIndex types_;
};

} // namespace vycor
