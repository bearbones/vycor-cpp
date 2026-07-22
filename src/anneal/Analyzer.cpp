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
#include "vycor/anneal/Checkpoint.h"
#include "vycor/anneal/Indexer.h"
#include "vycor/anneal/TypeNormalize.h"
#include "vycor/compat/ClangVersion.h"
#include "vycor/compat/ToolAdjusters.h"
#include "vycor/ext/Extensions.h"

#include "llvm/Support/ThreadPool.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Tooling/CompilationDatabase.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSwitch.h"

#include <cctype>
#include <memory>
#include <set>
#include <unordered_map>

namespace vycor {

namespace {

// ---- Overload-resolution heuristics ---------------------------------------
//
// The GlobalIndex stores parameter types as strings produced by
// QualType::getAsString(). We don't have the original QualTypes for invisible
// overloads, so we can't ask Clang's Sema to rank them against a call. These
// helpers implement a conservative string-based model good enough to (a)
// suppress the math-library false positives where unrelated overloads share
// the same operator name and arity, (b) recognize when a call would become
// ambiguous if an invisible overload were brought into the TU, and (c)
// optionally consult the GlobalIndex's TypeRelationIndex to accept
// candidates that would only be viable via an inheritance, user-defined
// conversion, or converting-constructor conversion.
//
// The string-based model is intentionally coarser than Clang Sema. Known
// limitations — documented here so future changes don't silently widen the
// gap:
//   - No template argument deduction: function templates (and templated
//     ctors/conversion ops) are skipped by the indexer.
//   - Single-hop only: A -> B -> C is not chained.
//   - Name-based class identity: no ODR, no canonical aliasing beyond what
//     the indexer's getCanonicalType() pass gives us.
//   - No overload partial ordering: tied candidates are flagged via
//     ADL_SameScore (opt-in) rather than ranked against each other.

// The fixed set of builtin arithmetic type names. Kept local to the scorer
// because the per-position score distinguishes exact-match and
// arithmetic-convertible on the "builtin" fast path before the convertibility
// model is consulted.
static bool isArithmeticTypeName(const std::string &normalized) {
  return llvm::StringSwitch<bool>(normalized)
      .Case("bool", true)
      .Case("char", true)
      .Case("signedchar", true)
      .Case("unsignedchar", true)
      .Case("wchar_t", true)
      .Case("char8_t", true)
      .Case("char16_t", true)
      .Case("char32_t", true)
      .Case("short", true)
      .Case("shortint", true)
      .Case("unsignedshort", true)
      .Case("unsignedshortint", true)
      .Case("int", true)
      .Case("unsigned", true)
      .Case("unsignedint", true)
      .Case("long", true)
      .Case("longint", true)
      .Case("unsignedlong", true)
      .Case("unsignedlongint", true)
      .Case("longlong", true)
      .Case("longlongint", true)
      .Case("unsignedlonglong", true)
      .Case("unsignedlonglongint", true)
      .Case("float", true)
      .Case("double", true)
      .Case("longdouble", true)
      .Default(false);
}

// Per-position score under either the legacy or the convertibility-model
// scorer. Return values:
//   2 — exact normalized match (strong preference)
//   1 — convertible (legacy arithmetic rule, or a hit in the type relation
//       index when `modelConvertibility` is true)
//   0 — not convertible (filter: candidate is dropped by the caller)
//
// `res` is the resolved overload's parameter at the same position. We treat
// a candidate parameter matching the resolved one as a free pass because we
// know the resolved overload compiled against this argument — whatever
// conversion the resolved path used must also be available here.
static int scoreParamMatch(const std::string &arg, const std::string &cand,
                           const std::string &res,
                           const GlobalIndex &index,
                           bool modelConvertibility) {
  if (cand == arg)
    return 2;
  if (cand == res)
    return 1;
  if (isArithmeticTypeName(cand) && isArithmeticTypeName(arg))
    return 1;
  if (modelConvertibility &&
      index.typeRelations().isConvertible(arg, cand))
    return 1;
  return 0;
}

// Score a whole parameter list against the call's argument types. Returns
// an empty vector when the candidate is not viable (any position scored 0,
// or arity mismatch).
static std::vector<int>
scoreOverload(const std::vector<std::string> &argTypes,
              const std::vector<std::string> &resolvedParamTypes,
              const std::vector<std::string> &candidateParamTypes,
              const GlobalIndex &index, bool modelConvertibility) {
  if (candidateParamTypes.size() != argTypes.size() ||
      resolvedParamTypes.size() != argTypes.size())
    return {};
  std::vector<int> out;
  out.reserve(argTypes.size());
  for (size_t i = 0; i < argTypes.size(); ++i) {
    int s = scoreParamMatch(argTypes[i], candidateParamTypes[i],
                            resolvedParamTypes[i], index, modelConvertibility);
    if (s == 0)
      return {};
    out.push_back(s);
  }
  return out;
}

// Compare two per-position score vectors and set flags describing the
// relationship:
//   candidateBetter = exists i where candidate[i] > resolved[i]
//   resolvedBetter  = exists i where resolved[i]  > candidate[i]
// The combination (candidateBetter, resolvedBetter) classifies the pair:
//   (true,  false) → candidate Pareto-dominates   → ADL_Fallback
//   (true,  true ) → incomparable (tied-on-some)  → ADL_Ambiguity
//   (false, true ) → candidate strictly worse     → no diagnostic
//   (false, false) → tied on every position       → ADL_SameScore (opt-in)
static void compareScores(const std::vector<int> &resolvedScores,
                          const std::vector<int> &candidateScores,
                          bool &candidateBetter, bool &resolvedBetter) {
  candidateBetter = false;
  resolvedBetter = false;
  for (size_t i = 0; i < resolvedScores.size(); ++i) {
    if (candidateScores[i] > resolvedScores[i])
      candidateBetter = true;
    else if (resolvedScores[i] > candidateScores[i])
      resolvedBetter = true;
  }
}

// Normalize a whole parameter list in place.
static std::vector<std::string>
normalizeAll(const std::vector<std::string> &raw) {
  std::vector<std::string> out;
  out.reserve(raw.size());
  for (const auto &s : raw)
    out.push_back(normalizeTypeForMatching(s));
  return out;
}

} // namespace

// --- AnalyzerVisitor ---

AnalyzerVisitor::AnalyzerVisitor(const GlobalIndex &index,
                                 clang::SourceManager &sm,
                                 std::vector<Diagnostic> &diagnostics,
                                 AnalysisOptions opts)
    : index_(index), sm_(sm), diagnostics_(diagnostics),
      opts_(std::move(opts)) {}

bool AnalyzerVisitor::VisitCallExpr(clang::CallExpr *expr) {
  // Only analyze calls in the main file (not in included headers).
  if (!sm_.isInMainFile(expr->getBeginLoc()))
    return true;

  auto *callee = expr->getDirectCallee();
  if (!callee)
    return true;

  // We're interested in calls resolved via ADL — unqualified calls where
  // namespace lookup was triggered by argument types.
  // A simple heuristic: the call uses an unqualified name (no DeclRefExpr
  // with a nested-name-specifier) and the callee is in a namespace
  // associated with one of its argument types.

  // Check if the callee is a namespace-scope function (ADL candidate).
  if (!callee->getDeclContext()->isNamespace())
    return true;

  // Skip if the callee was explicitly qualified (e.g., MathLib::scale).
  if (auto *dre = llvm::dyn_cast_or_null<clang::DeclRefExpr>(
          expr->getCallee()->IgnoreParenImpCasts())) {
    if (dre->hasQualifier())
      return true;
  }

  std::string qualifiedName = callee->getQualifiedNameAsString();

  // Query the global index for all known overloads.
  auto overloads = index_.findOverloads(qualifiedName);
  if (overloads.size() <= 1)
    return true; // No alternative overloads exist globally.

  populateIncludedFiles();

  // Collect overloads that exist in the global index but are NOT visible
  // in this translation unit.
  std::string calleePath = getFilePath(callee->getLocation());

  // Build the resolved callee's parameter type signature for comparison.
  std::vector<std::string> resolvedParamTypes;
  for (unsigned i = 0; i < callee->getNumParams(); ++i)
    resolvedParamTypes.push_back(
        callee->getParamDecl(i)->getType().getAsString());

  // Build the call-site's actual argument types. IgnoreImpCasts strips
  // implicit conversions the compiler inserted to satisfy the resolved
  // overload — we want the type the user wrote so that we can compare it
  // fairly against candidate overloads.
  std::vector<std::string> argTypes;
  argTypes.reserve(expr->getNumArgs());
  for (unsigned i = 0; i < expr->getNumArgs(); ++i)
    argTypes.push_back(
        expr->getArg(i)->IgnoreImpCasts()->getType().getAsString());

  // Normalized copies for string-based matching.
  auto argTypesN = normalizeAll(argTypes);
  auto resolvedParamTypesN = normalizeAll(resolvedParamTypes);

  // The resolved overload's scores — we know Sema picked it, so every
  // position must be at least convertible. We pass the resolved param list
  // in as both the "candidate" and "resolved" reference; the identity
  // shortcut gives each position a score of 2.
  auto resolvedScores =
      scoreOverload(argTypesN, resolvedParamTypesN, resolvedParamTypesN,
                    index_, opts_.modelConvertibility);
  if (resolvedScores.empty())
    return true; // something odd with arity — bail out safely.

  auto buildSig = [](const std::string &qname,
                     const std::vector<std::string> &params) {
    std::string sig = qname + "(";
    for (size_t i = 0; i < params.size(); ++i) {
      if (i > 0)
        sig += ", ";
      sig += params[i];
    }
    sig += ")";
    return sig;
  };
  std::string resolvedSig = buildSig(qualifiedName, resolvedParamTypes);

  for (const auto *entry : overloads) {
    // Skip overloads that match the resolved callee's signature (same
    // parameter types). These are the "same" overload even if declared in a
    // different file.
    if (entry->paramTypes == resolvedParamTypes)
      continue;

    // Check if this overload's header is included in the current TU.
    if (isFileIncluded(entry->headerPath))
      continue;

    // Arity mismatch → cannot apply to this call.
    if (entry->paramTypes.size() !=
        static_cast<size_t>(expr->getNumArgs()))
      continue;

    // Rank the candidate against the call's actual argument types. An
    // empty score vector means the candidate hit a 0 at some position (the
    // convertibility filter) and should be dropped.
    auto candidateParamTypesN = normalizeAll(entry->paramTypes);
    auto candidateScores =
        scoreOverload(argTypesN, resolvedParamTypesN, candidateParamTypesN,
                      index_, opts_.modelConvertibility);
    if (candidateScores.empty())
      continue;

    bool candidateBetter = false, resolvedBetter = false;
    compareScores(resolvedScores, candidateScores, candidateBetter,
                  resolvedBetter);

    if (!candidateBetter) {
      // Same-score path: neither Pareto-dominates. Emit ADL_SameScore only
      // when opted in, and only when the score vectors are truly identical
      // (strictly-worse candidates get `resolvedBetter=true` and are dropped
      // silently below as before).
      if (opts_.warnSameScore && !resolvedBetter &&
          candidateScores == resolvedScores &&
          entry->paramTypes != resolvedParamTypes) {
        std::string candidateSig =
            buildSig(entry->qualifiedName, entry->paramTypes);
        Diagnostic diag;
        diag.kind = Diagnostic::ADL_SameScore;
        diag.callLocation = formatLocation(expr->getBeginLoc());
        diag.resolvedDecl = resolvedSig;
        diag.betterDecl = candidateSig;
        diag.missingHeader = entry->headerPath;
        diag.message =
            "Same-score ADL candidate: " + candidateSig + " exists in " +
            entry->headerPath +
            " but is not visible here. It ties the resolved overload " +
            resolvedSig +
            " on every argument position — inclusion order silently decides "
            "which one the compiler picks.";
        diagnostics_.push_back(std::move(diag));
      }
      continue; // not Pareto-better: nothing more to say on this candidate
    }

    std::string candidateSig = buildSig(entry->qualifiedName, entry->paramTypes);

    Diagnostic diag;
    diag.callLocation = formatLocation(expr->getBeginLoc());
    diag.resolvedDecl = resolvedSig;
    diag.betterDecl = candidateSig;
    diag.missingHeader = entry->headerPath;

    if (!resolvedBetter) {
      // Candidate Pareto-dominates resolved → fragile fallback.
      diag.kind = Diagnostic::ADL_Fallback;
      diag.message =
          "Fragile ADL resolution: " + candidateSig + " exists in " +
          entry->headerPath +
          " but is not visible here. The current call resolves to " +
          resolvedSig + ". Include " + entry->headerPath +
          " or explicitly qualify the call.";
    } else {
      // Each overload wins at some position → including the candidate's
      // header would make this call ambiguous.
      diag.kind = Diagnostic::ADL_Ambiguity;
      diag.message =
          "Potential ADL ambiguity: " + candidateSig + " exists in " +
          entry->headerPath +
          " but is not visible here. Including it would make the call to " +
          resolvedSig + " an ambiguous overload resolution.";
    }

    diagnostics_.push_back(std::move(diag));
  }

  return true;
}

bool AnalyzerVisitor::VisitVarDecl(clang::VarDecl *decl) {
  // Only analyze declarations in the main file.
  if (!sm_.isInMainFile(decl->getBeginLoc()))
    return true;

  // Check if this variable was declared with CTAD.
  auto *tsl = decl->getTypeSourceInfo();
  if (!tsl)
    return true;

  auto type = decl->getType();

  // Look for template specialization types that resulted from CTAD.
  const clang::TemplateSpecializationType *tst = nullptr;

  // Check the desugared type for a template specialization.
  if (auto *recordType = type->getAs<clang::RecordType>()) {
    if (auto *ctsd = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(
            recordType->getDecl())) {
      // Check if this specialization was via deduction (CTAD) rather than
      // explicit template arguments.
      // A heuristic: if the VarDecl has an initializer with a
      // CXXConstructExpr and the declared type uses auto or a deduced
      // template specialization, this is likely CTAD.
      if (!decl->hasInit())
        return true;

      auto *templateDecl = ctsd->getSpecializedTemplate();
      if (!templateDecl)
        return true;

      std::string templateName = templateDecl->getQualifiedNameAsString();

      // Query global index for deduction guides.
      auto guides = index_.findDeductionGuides(templateName);
      if (guides.empty())
        return true;

      populateIncludedFiles();

      std::string deducedType = type.getAsString();

      for (const auto *entry : guides) {
        // Skip guides that are visible in this TU.
        if (isFileIncluded(entry->headerPath))
          continue;

        // An invisible deduction guide exists. If it would produce
        // a different type, flag it.
        if (entry->deducedType == deducedType)
          continue;

        Diagnostic diag;
        diag.kind = Diagnostic::CTAD_Fallback;
        diag.callLocation = formatLocation(decl->getBeginLoc());
        diag.resolvedDecl = deducedType;
        diag.betterDecl = entry->deducedType;
        diag.missingHeader = entry->headerPath;
        diag.message =
            "Fragile CTAD resolution: deduction guide in " +
            entry->headerPath + " would deduce " + entry->deducedType +
            " but is not visible here. The current deduction produces " +
            deducedType + ". Include " + entry->headerPath +
            " to use the explicit guide.";

        diagnostics_.push_back(std::move(diag));
      }
    }
  }

  return true;
}

void AnalyzerVisitor::populateIncludedFiles() const {
  if (includedFilesPopulated_)
    return;
  includedFilesPopulated_ = true;

  // Collect all files that are part of this translation unit.
  for (auto it = sm_.fileinfo_begin(); it != sm_.fileinfo_end(); ++it) {
    includedFiles_.insert(std::string(it->first.getName()));
  }
}

bool AnalyzerVisitor::isFileIncluded(const std::string &path) const {
  return includedFiles_.count(path) > 0;
}

std::string
AnalyzerVisitor::formatLocation(clang::SourceLocation loc) const {
  auto spellingLoc = sm_.getSpellingLoc(loc);
  auto file = sm_.getFilename(spellingLoc);
  unsigned line = sm_.getSpellingLineNumber(spellingLoc);
  unsigned col = sm_.getSpellingColumnNumber(spellingLoc);
  return std::string(file) + ":" + std::to_string(line) + ":" +
         std::to_string(col);
}

std::string
AnalyzerVisitor::getFilePath(clang::SourceLocation loc) const {
  auto fileEntry = sm_.getFileEntryRefForID(
      sm_.getFileID(sm_.getSpellingLoc(loc)));
  if (fileEntry)
    return std::string(fileEntry->getName());
  return "<unknown>";
}

// --- AnalyzerConsumer ---

AnalyzerConsumer::AnalyzerConsumer(const GlobalIndex &index,
                                   clang::SourceManager &sm,
                                   std::vector<Diagnostic> &diagnostics,
                                   AnalysisOptions opts)
    : visitor_(index, sm, diagnostics, opts), index_(index),
      diagnostics_(diagnostics), opts_(std::move(opts)) {}

void AnalyzerConsumer::HandleTranslationUnit(clang::ASTContext &context) {
  visitor_.TraverseDecl(context.getTranslationUnitDecl());
  // Organization checks (ext/ registrars) run after the built-in analysis
  // of the same TU; fresh instances per TU so member state needs no reset.
  for (auto &check :
       ExtensionRegistry::instance().createAnnealChecks(opts_.disabledChecks))
    check->checkTU(context, index_, diagnostics_);
}

// --- AnalyzerAction ---

AnalyzerAction::AnalyzerAction(const GlobalIndex &index,
                               std::vector<Diagnostic> &diags,
                               AnalysisOptions opts)
    : index_(index), diagnostics_(diags), opts_(std::move(opts)) {}

std::unique_ptr<clang::ASTConsumer>
AnalyzerAction::CreateASTConsumer(clang::CompilerInstance &ci,
                                  llvm::StringRef /*file*/) {
  return std::make_unique<AnalyzerConsumer>(index_, ci.getSourceManager(),
                                           diagnostics_, opts_);
}

// --- AnalyzerActionFactory ---

AnalyzerActionFactory::AnalyzerActionFactory(const GlobalIndex &index,
                                             std::vector<Diagnostic> &diags,
                                             AnalysisOptions opts)
    : index_(index), diagnostics_(diags), opts_(std::move(opts)) {}

std::unique_ptr<clang::FrontendAction> AnalyzerActionFactory::create() {
  return std::make_unique<AnalyzerAction>(index_, diagnostics_, opts_);
}

// --- Coverage Analysis ---

static std::string gvaLinkageName(int gva) {
  switch (gva) {
  case 0:
    return "GVA_Internal";
  case 1:
    return "GVA_AvailableExternally";
  case 2:
    return "GVA_DiscardableODR";
  case 3:
    return "GVA_StrongODR";
  case 4:
    return "GVA_StrongExternal";
  default:
    return "Unknown(" + std::to_string(gva) + ")";
  }
}

void analyzeCoverageProperties(const GlobalIndex &index,
                               std::vector<Diagnostic> &diagnostics) {
  for (const auto &className : index.allIndexedClasses()) {
    auto methods = index.findClassMethods(className);
    if (methods.empty())
      continue;

    // Phase A: Flag individual methods with risky GVA linkage.
    for (const auto *m : methods) {
      if (m->gvaLinkage == 2 /* GVA_DiscardableODR */) {
        Diagnostic diag;
        diag.kind = Diagnostic::Coverage_DiscardableODR;
        diag.callLocation =
            m->headerPath + ":" + std::to_string(m->sourceLine);
        diag.resolvedDecl = m->signature;
        diag.message =
            "Coverage risk: " + m->signature +
            " has GVA_DiscardableODR linkage. COMDAT deduplication at link "
            "time may replace the instrumented definition with an "
            "uninstrumented one from another TU, producing hash 0x0 in "
            "coverage data.";
        diagnostics.push_back(std::move(diag));
      }
      if (m->gvaLinkage == 1 /* GVA_AvailableExternally */) {
        Diagnostic diag;
        diag.kind = Diagnostic::Coverage_AvailableExternally;
        diag.callLocation =
            m->headerPath + ":" + std::to_string(m->sourceLine);
        diag.resolvedDecl = m->signature;
        diag.message =
            "Coverage risk: " + m->signature +
            " has GVA_AvailableExternally linkage. The optimizer may "
            "inline the body and then discard the standalone definition, "
            "eliminating coverage instrumentation.";
        diagnostics.push_back(std::move(diag));
      }
    }

    if (methods.size() < 2)
      continue;

    // Phase B: Compare sibling methods for GVA linkage mismatch.
    std::unordered_map<int, std::vector<const CoveragePropertyEntry *>> byGVA;
    for (const auto *m : methods)
      byGVA[m->gvaLinkage].push_back(m);

    if (byGVA.size() > 1) {
      size_t maxGroupSize = 0;
      int majorityGVA = -1;
      for (const auto &kv : byGVA) {
        if (kv.second.size() > maxGroupSize) {
          maxGroupSize = kv.second.size();
          majorityGVA = kv.first;
        }
      }

      for (const auto &kv : byGVA) {
        if (kv.first == majorityGVA)
          continue;
        for (const auto *m : kv.second) {
          Diagnostic diag;
          diag.kind = Diagnostic::Coverage_GVAMismatch;
          diag.callLocation =
              m->headerPath + ":" + std::to_string(m->sourceLine);
          diag.resolvedDecl = m->signature;
          diag.betterDecl =
              "majority linkage: " + gvaLinkageName(majorityGVA);
          diag.message =
              "Coverage divergence in " + className + ": " + m->signature +
              " has " + gvaLinkageName(kv.first) + " linkage, but " +
              std::to_string(maxGroupSize) +
              " other method(s) in the same class have " +
              gvaLinkageName(majorityGVA) +
              " linkage. This difference may cause divergent coverage "
              "instrumentation behavior.";
          diagnostics.push_back(std::move(diag));
        }
      }
    }

    // Phase C: Property divergence within same-GVA groups.
    for (const auto &kv : byGVA) {
      if (kv.second.size() < 2)
        continue;
      for (size_t i = 0; i < kv.second.size(); ++i) {
        for (size_t j = i + 1; j < kv.second.size(); ++j) {
          const auto *a = kv.second[i];
          const auto *b = kv.second[j];
          // AST node counting: a simple `return x_` getter has ~5 nodes
          // (CompoundStmt, ReturnStmt, ImplicitCastExpr, MemberExpr,
          // CXXThisExpr). Threshold of 8 captures one-line getters/setters.
          bool aTrivial = (a->bodyStmtCount <= 8 || a->isTrivial);
          bool bTrivial = (b->bodyStmtCount <= 8 || b->isTrivial);
          if (aTrivial != bTrivial) {
            const auto *complex = aTrivial ? b : a;
            const auto *trivial = aTrivial ? a : b;
            Diagnostic diag;
            diag.kind = Diagnostic::Coverage_PropertyDivergence;
            diag.callLocation =
                complex->headerPath + ":" +
                std::to_string(complex->sourceLine);
            diag.resolvedDecl = complex->signature;
            diag.betterDecl = trivial->signature;
            diag.message =
                "Coverage property divergence in " + className + ": " +
                complex->signature + " (body complexity: " +
                std::to_string(complex->bodyStmtCount) +
                " stmts, inline=" +
                (complex->isInlined ? "true" : "false") + ", constexpr=" +
                (complex->isConstexpr ? "true" : "false") + ") vs " +
                trivial->signature + " (body complexity: " +
                std::to_string(trivial->bodyStmtCount) +
                " stmts, inline=" +
                (trivial->isInlined ? "true" : "false") + ", constexpr=" +
                (trivial->isConstexpr ? "true" : "false") +
                "). The more complex method may receive different "
                "optimization treatment affecting coverage instrumentation.";
            diagnostics.push_back(std::move(diag));
          }
        }
      }
    }
  }
}

// --- runAnalysis ---

namespace {

// Per-TU worker pool with bakeIndexes' semantics: threadCount 0 = all
// hardware threads, 1 = serial (also the single-file case).
template <typename TaskFn>
void runPerTuTasks(const std::vector<std::string> &tus, unsigned threadCount,
                   TaskFn task) {
  bool parallel = threadCount != 1 && tus.size() > 1;
  if (parallel) {
#if VYCOR_LLVM_AT_LEAST(19)
    llvm::DefaultThreadPool pool(llvm::hardware_concurrency(threadCount));
#else
    llvm::ThreadPool pool(llvm::hardware_concurrency(threadCount));
#endif
    for (const auto &tu : tus)
      pool.async([&task, tu] { task(tu); });
    pool.wait();
  } else {
    for (const auto &tu : tus)
      task(tu);
  }
}

} // namespace

std::vector<Diagnostic>
runAnalysis(const clang::tooling::CompilationDatabase &compDb,
            const std::vector<std::string> &sourceFiles,
            const AnalysisOptions &opts) {
  GlobalIndex index;

  // Checkpoint journal (opt-in): per-TU progress survives a killed run.
  std::unique_ptr<AnnealCheckpoint> ckpt;
  std::vector<FileStamp> stamps;
  std::unordered_map<std::string, const FileStamp *> stampFor;
  if (!opts.checkpointPath.empty()) {
    ckpt = AnnealCheckpoint::open(opts.checkpointPath,
                                  annealOptionsFingerprint(opts));
    if (!ckpt) {
      llvm::errs() << "anneal: WARNING: cannot open checkpoint "
                   << opts.checkpointPath << " — continuing without one\n";
    } else {
      // Stamps are taken before any parsing: a file modified mid-run gets a
      // stale stamp and is conservatively re-indexed on the next resume
      // (same policy as megascope snapshots).
      stamps = SnapshotIO::stampFiles(sourceFiles);
      for (const auto &s : stamps)
        stampFor[s.path] = &s;
    }
  }

  // Phase 1: index all translation units. Journaled TUs with a matching
  // stamp are replayed without a parse; TUs whose parse fatally died
  // kMaxAttempts times are skipped as poisoned.
  std::vector<std::string> toIndex;
  std::vector<FileStamp> contributing; // drives the phase-2 validity hash
  std::set<std::string> poisoned;
  size_t replayed1 = 0;
  for (const auto &file : sourceFiles) {
    const FileStamp *st = ckpt ? stampFor[file] : nullptr;
    if (ckpt && ckpt->replayPhase1(file, *st, index)) {
      contributing.push_back(*st);
      ++replayed1;
      continue;
    }
    if (ckpt && ckpt->attempts(AnnealCheckpoint::kPhaseIndex, file, *st) >=
                    AnnealCheckpoint::kMaxAttempts) {
      llvm::errs() << "anneal: WARNING: skipping " << file
                   << " — its phase-1 parse died "
                   << AnnealCheckpoint::kMaxAttempts
                   << " time(s) (see checkpoint); delete the checkpoint "
                      "file to retry it\n";
      poisoned.insert(file);
      continue;
    }
    if (st)
      contributing.push_back(*st);
    toIndex.push_back(file);
  }
  runPerTuTasks(toIndex, opts.threadCount, [&](const std::string &file) {
    if (ckpt) {
      const FileStamp *st = stampFor[file];
      // The attempt record lands on disk BEFORE the parse: if the parse
      // takes the process down, the next resume can see it happened.
      ckpt->recordAttempt(AnnealCheckpoint::kPhaseIndex, file, *st);
      GlobalIndex shard;
      auto tool = vycor::makeClangTool(compDb, {file});
      IndexerActionFactory factory(shard);
      tool.run(&factory);
      ckpt->recordPhase1(file, *st, shard);
      index.absorb(shard);
    } else {
      auto tool = vycor::makeClangTool(compDb, {file});
      IndexerActionFactory factory(index);
      tool.run(&factory);
    }
  });
  if (replayed1)
    llvm::errs() << "anneal: checkpoint: " << replayed1 << " of "
                 << sourceFiles.size()
                 << " TU(s) restored without re-parsing (phase 1)\n";

  // Phase 1.5: Coverage property analysis (index-only, no AST needed).
  // Always recomputed — it reads the whole index, which replay rebuilt.
  std::vector<Diagnostic> diagnostics;
  if (opts.enableCoverageDiag)
    analyzeCoverageProperties(index, diagnostics);

  // Phase 2: analyze each TU against the now-complete index. Per-file
  // slots keep output deterministic (source order) regardless of task
  // completion order. A phase-2 record is only valid while the WHOLE
  // contributing file set is unchanged: one edited TU can add overloads
  // that change another TU's diagnostics.
  const uint64_t setHash = ckpt ? annealStampSetHash(contributing) : 0;
  std::vector<std::vector<Diagnostic>> perFile(sourceFiles.size());
  std::unordered_map<std::string, size_t> slotFor;
  for (size_t i = 0; i < sourceFiles.size(); ++i)
    slotFor[sourceFiles[i]] = i;

  std::vector<std::string> toAnalyze;
  size_t replayed2 = 0;
  for (const auto &file : sourceFiles) {
    if (poisoned.count(file))
      continue; // its phase-2 parse would die the same way
    const FileStamp *st = ckpt ? stampFor[file] : nullptr;
    if (ckpt &&
        ckpt->replayPhase2(file, *st, setHash, perFile[slotFor[file]])) {
      ++replayed2;
      continue;
    }
    if (ckpt && ckpt->attempts(AnnealCheckpoint::kPhaseAnalyze, file, *st) >=
                    AnnealCheckpoint::kMaxAttempts) {
      llvm::errs() << "anneal: WARNING: skipping " << file
                   << " — its phase-2 parse died "
                   << AnnealCheckpoint::kMaxAttempts
                   << " time(s) (see checkpoint); delete the checkpoint "
                      "file to retry it\n";
      continue;
    }
    toAnalyze.push_back(file);
  }
  runPerTuTasks(toAnalyze, opts.threadCount, [&](const std::string &file) {
    if (ckpt)
      ckpt->recordAttempt(AnnealCheckpoint::kPhaseAnalyze, file,
                          *stampFor[file]);
    std::vector<Diagnostic> local;
    auto tool = vycor::makeClangTool(compDb, {file});
    AnalyzerActionFactory factory(index, local, opts);
    tool.run(&factory);
    if (ckpt)
      ckpt->recordPhase2(file, *stampFor[file], setHash, local);
    perFile[slotFor[file]] = std::move(local);
  });
  if (replayed2)
    llvm::errs() << "anneal: checkpoint: " << replayed2 << " of "
                 << sourceFiles.size()
                 << " TU(s) restored without re-parsing (phase 2)\n";

  for (auto &slot : perFile)
    diagnostics.insert(diagnostics.end(),
                       std::make_move_iterator(slot.begin()),
                       std::make_move_iterator(slot.end()));
  return diagnostics;
}

std::vector<Diagnostic>
runAnalysis(const clang::tooling::CompilationDatabase &compDb,
            const std::vector<std::string> &sourceFiles,
            bool enableCoverageDiag) {
  AnalysisOptions opts;
  opts.enableCoverageDiag = enableCoverageDiag;
  return runAnalysis(compDb, sourceFiles, opts);
}

} // namespace vycor
