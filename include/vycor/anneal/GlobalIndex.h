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

#include <mutex>
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

  // Merge another index's edges into this one (parallel shard merge and
  // checkpoint replay). Dedup is the same as the add* methods'.
  void absorb(const TypeRelationIndex &other);

  // Edge enumeration with resolved strings, for checkpoint serialization.
  // Deterministic per index instance (map iteration order), not across
  // instances.
  template <typename Fn> void forEachBase(Fn fn) const {
    for (const auto &kv : bases_)
      for (SId base : kv.second)
        fn(interner_.resolve(kv.first), interner_.resolve(base));
  }
  template <typename Fn> void forEachCtorEdge(Fn fn) const {
    for (const auto &kv : ctorEdges_)
      for (SId from : kv.second)
        fn(interner_.resolve(kv.first), interner_.resolve(from));
  }
  template <typename Fn> void forEachConvOpEdge(Fn fn) const {
    for (const auto &kv : convOpEdges_)
      for (SId to : kv.second)
        fn(interner_.resolve(kv.first), interner_.resolve(to));
  }

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

  // Guards the edge maps during add*/absorb: the parallel anneal phase 1
  // writes from several threads. Reads (isBaseOrSelf/isConvertible/forEach*)
  // don't lock — like CallGraph, the phase barrier guarantees writers have
  // quiesced before phase 2 reads begin.
  mutable std::mutex writeMutex_;

  // Normalized derived className -> list of direct base class names.
  std::unordered_map<SId, std::vector<SId>> bases_;

  // Normalized target type -> list of source types accepted by a non-explicit
  // single-argument converting constructor of that target.
  std::unordered_map<SId, std::vector<SId>> ctorEdges_;

  // Normalized source type -> list of target types reachable via a non-explicit
  // conversion operator on the source.
  std::unordered_map<SId, std::vector<SId>> convOpEdges_;
};

// One ODR-relevant definition site: a vague-linkage entity (inline
// function, in-class method body, class definition) whose definitions the
// linker silently merges across TUs WITHOUT comparing content — precisely
// the ODR-violation class ordinary builds never diagnose (duplicate strong
// symbols are a linker error; duplicate weak/COMDAT symbols are "pick
// one"). Collected during phase-1 indexing only when
// AnalysisOptions::enableOdrDiag is set; analyzeOdrViolations compares
// sites and hashes across the whole project.
struct OdrEntry {
  std::string qualifiedName;
  // Full function type spelling (params + cv/ref quals) for functions —
  // distinguishes overloads and const/non-const method pairs. "" for
  // classes.
  std::string signature;
  bool isClass = false;
  std::string enclosingClass; // methods: qualified parent class, else ""
  std::string filePath;       // definition site
  unsigned line = 0;
  uint64_t odrHash = 0;       // clang::ODRHash over the definition
};

// One explicit (full) class template specialization discovered during
// indexing. Backs the specialization-visibility check: a TU that
// implicitly instantiates the primary template while an explicit
// specialization exists elsewhere in the program — invisible to that TU —
// is ill-formed, no diagnostic required ([temp.expl.spec]): some TUs use
// the primary's instantiation and some the specialization, and neither
// the compiler (single-TU view) nor the linker (both symbols are vague
// linkage) will ever complain. Always collected — explicit
// specializations are rare enough that indexing them costs nothing.
struct SpecializationEntry {
  std::string templateName; // qualified primary template name, e.g. "std::hash"
  std::string argsString;   // canonical comma-joined template arguments
  std::string headerPath;   // where the specialization is declared
  unsigned line = 0;
};

// One written-out default argument on one function declaration site.
// Default arguments live on DECLARATIONS, and different headers can
// legally carry different ones for the same function — each TU silently
// calls with whatever value its includes provided. Only defaults actually
// written at the site are recorded (redeclarations inheriting a default
// from an earlier declaration in the same TU are skipped), so a
// cross-site conflict means two headers genuinely disagree.
struct DefaultArgEntry {
  std::string qualifiedName;
  std::string signature; // full function type spelling (overload identity)
  unsigned paramIndex = 0;
  std::string paramName;
  std::string defaultText; // source spelling of the default expression
  std::string filePath;
  unsigned line = 0;
};

// One static-initialization root: a static-storage-duration variable
// definition, or an __attribute__((constructor)) function. Dynamic
// initializers run before main() — and, when the library is loaded with
// dlopen (Android System.loadLibrary, plugins), UNDER the dynamic
// linker's global lock, in an order that follows link order. Two checks
// consume these: static-init-order (cross-TU initialization-order
// dependencies) and static-init-hazards (initializers that transitively
// reach the dynamic linker or thread create/join). Function-local statics
// are excluded by construction: they initialize lazily and are safe.
struct StaticInitEntry {
  std::string qualifiedName;
  std::string filePath; // definition site
  unsigned line = 0;
  bool dynamicInit = false;    // initializer runs at load time
  bool isConstructorFn = false; // __attribute__((constructor)) function
  // Externally-visible static-storage globals the initializer expression
  // references directly (static-init-order's dependency edges).
  std::vector<std::string> referencedGlobals;
  // Functions/constructors the initializer expression calls directly —
  // BFS seeds for static-init-hazards' walk over the call graph (global
  // initializer expressions have no enclosing function, so the graph
  // itself has no edges out of them).
  std::vector<std::string> calledFunctions;
};

// Name-level per-function call summary, collected during the anneal parse
// itself (no second frontend pass — this is what lets the transitive
// static-init-order and exception-escape analyses stay out of the
// compute-heavy group). Merged by qualified name across TUs, which
// deliberately conflates overload sets: the consumers over-approximate,
// and their docs say so. Only functions with something to say (a call, a
// global reference, or an uncaught throw) get an entry.
struct FunctionSummaryEntry {
  std::string qualifiedName;
  std::string filePath; // first definition site seen
  unsigned line = 0;
  bool isNoexcept = false;       // cannot throw per its exception spec
  bool hasUncaughtThrow = false; // a throw not inside any try in the body
  std::vector<std::string> calledFunctions; // all direct calls
  // Direct calls NOT lexically inside a try block — the ones an exception
  // would escape through.
  std::vector<std::string> unguardedCalls;
  // Externally-visible static-storage globals the body references.
  std::vector<std::string> referencedGlobals;
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
    ODR_DuplicateDefinition,      // one vague-linkage entity, multiple
                                  // distinct definition sites with differing
                                  // bodies — the linker keeps one arbitrarily
    ODR_DivergentDefinition,      // one definition site whose body hashes
                                  // differently across TUs (preprocessor-
                                  // dependent definition)
    Specialization_Invisible,     // TU instantiates a primary template while
                                  // an explicit specialization exists in a
                                  // header this TU does not include (IFNDR)
    DefaultArg_Divergent,         // declaration sites disagree on a
                                  // parameter's default argument — each TU
                                  // silently calls with a different value
    StaticInit_OrderDependency,   // a dynamic initializer reads another
                                  // TU's dynamically-initialized global —
                                  // cross-TU init order is unspecified
    StaticInit_Hazard,            // a static initializer transitively
                                  // reaches dlopen/dlsym/thread create-join
                                  // — deadlock risk under the loader lock
    Exception_Escape,             // a noexcept function can transitively
                                  // reach an uncaught throw (std::terminate)
    Custom,                       // organization ext/ check (see checkName)
  };
  Kind kind;
  std::string callLocation;  // file:line:col of the call site
  std::string resolvedDecl;  // what the compiler chose
  std::string betterDecl;    // what the global index found
  std::string missingHeader; // which header to include
  std::string message;       // human-readable diagnostic
  std::string checkName;     // for Kind::Custom: AnnealCheck::name()
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

  // ODR definition-site tracking (see OdrEntry). Identical entries — the
  // common case of N TUs including one unchanged header — dedup at insert.
  void addOdrEntry(const OdrEntry &entry);

  // Explicit class template specializations (see SpecializationEntry).
  // Identical entries dedup at insert.
  void addSpecialization(const SpecializationEntry &entry);

  // Default-argument declaration sites (see DefaultArgEntry). Identical
  // entries dedup at insert.
  void addDefaultArg(const DefaultArgEntry &entry);

  // Static-initialization roots (see StaticInitEntry). Identical entries
  // (same name + site) dedup at insert.
  void addStaticInit(const StaticInitEntry &entry);

  // Function call summaries (see FunctionSummaryEntry). Entries with the
  // same qualified name MERGE: lists union, flags OR, first site wins.
  void addFunctionSummary(const FunctionSummaryEntry &entry);
  const FunctionSummaryEntry *
  findFunctionSummary(const std::string &name) const;
  // Entry for a given variable name, or nullptr (static-init-order resolves
  // referenced globals through this).
  const StaticInitEntry *findStaticInit(const std::string &name) const;
  std::vector<const SpecializationEntry *>
  findSpecializations(const std::string &templateName) const;

  // Total counts for testing/debugging.
  size_t overloadCount() const;
  size_t guideCount() const;
  size_t coverageEntryCount() const;
  size_t odrEntryCount() const;
  size_t specializationCount() const;
  size_t defaultArgCount() const;
  size_t staticInitCount() const;
  size_t functionSummaryCount() const;

  // Merge a per-TU shard into this index (parallel phase-1 merge and
  // checkpoint replay). Entries are appended exactly as if the shard's
  // add* calls had run against this index directly — including the
  // duplicate header declarations multiple TUs both see, matching the
  // historical single-index serial behaviour.
  void absorb(const GlobalIndex &shard);

  // Entry enumeration for checkpoint serialization. Deterministic per index
  // instance (map iteration order), not across instances.
  template <typename Fn> void forEachOverload(Fn fn) const {
    for (const auto &kv : overloads_)
      for (const auto &entry : kv.second)
        fn(entry);
  }
  template <typename Fn> void forEachDeductionGuide(Fn fn) const {
    for (const auto &kv : guides_)
      for (const auto &entry : kv.second)
        fn(entry);
  }
  template <typename Fn> void forEachCoverageProperty(Fn fn) const {
    for (const auto &kv : coverageProps_)
      for (const auto &entry : kv.second)
        fn(entry);
  }
  template <typename Fn> void forEachOdrEntry(Fn fn) const {
    for (const auto &entry : odrEntries_)
      fn(entry);
  }
  template <typename Fn> void forEachSpecialization(Fn fn) const {
    for (const auto &kv : specializations_)
      for (const auto &entry : kv.second)
        fn(entry);
  }
  template <typename Fn> void forEachDefaultArg(Fn fn) const {
    for (const auto &entry : defaultArgs_)
      fn(entry);
  }
  template <typename Fn> void forEachStaticInit(Fn fn) const {
    for (const auto &entry : staticInits_)
      fn(entry);
  }
  template <typename Fn> void forEachFunctionSummary(Fn fn) const {
    for (const auto &entry : functionSummaries_)
      fn(entry);
  }

private:
  using SId = StringInterner::Id;
  StringInterner interner_;

  // Guards the entry maps during add*/absorb (parallel anneal phase 1
  // writes from several threads; the interner has its own lock). Reads
  // don't lock — the phase barrier guarantees writers have quiesced before
  // phase 1.5/2 reads begin (see TypeRelationIndex::writeMutex_).
  mutable std::mutex writeMutex_;

  std::unordered_map<SId, std::vector<FunctionOverloadEntry>> overloads_;
  std::unordered_map<SId, std::vector<DeductionGuideEntry>> guides_;
  std::unordered_map<SId, std::vector<CoveragePropertyEntry>> coverageProps_;
  // Plain values, no interning: entry volume is bounded by non-system
  // vague-linkage definitions and hasn't earned ControlFlowIndex-style
  // compaction (same call ChannelIndex made).
  std::vector<OdrEntry> odrEntries_;
  std::unordered_set<std::string> odrKeys_; // full-identity dedup keys
  std::unordered_map<SId, std::vector<SpecializationEntry>> specializations_;
  std::vector<DefaultArgEntry> defaultArgs_;
  std::unordered_set<std::string> defaultArgKeys_;
  // deque-free by-name lookup: indices into staticInits_ (stable; the
  // vector only grows).
  std::vector<StaticInitEntry> staticInits_;
  std::unordered_map<std::string, size_t> staticInitByName_;
  std::unordered_set<std::string> staticInitKeys_;
  std::vector<FunctionSummaryEntry> functionSummaries_;
  std::unordered_map<std::string, size_t> functionSummaryByName_;
  TypeRelationIndex types_;
};

} // namespace vycor
