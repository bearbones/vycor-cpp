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

#include "vycor/anneal/GlobalIndex.h"

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Tooling/Tooling.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace vycor {

class CallGraph;

// Runs one anneal worker over `batch` (the --isolate-workers seam, mirror
// of callgraph's WorkerRunner): phase is AnnealCheckpoint::kPhaseIndex or
// kPhaseAnalyze; globalIndexPath is the merged-index handoff file for
// phase 2 (empty in phase 1); the worker writes its shard to shardPath and
// its stderr (WORKER-TU markers + diagnostics) to stderrPath. Any nonzero
// return triggers the crash/bisect protocol.
using AnnealWorkerRunner = std::function<int(
    uint8_t phase, const std::string &globalIndexPath,
    const std::vector<std::string> &batch, const std::string &shardPath,
    const std::string &stderrPath)>;

// Options controlling the behaviour of the anneal analysis pipeline. Opt-in
// fields remain false by default so existing callers see no behaviour
// change. Passed by const-ref through runAnalysis into the AST visitors.
struct AnalysisOptions {
  // The core visibility checks (on by default, matching historical
  // behaviour; the --checks selection layer in the CLI maps named checks
  // onto these booleans — see anneal/CheckSet.h).
  bool enableAdlDiag = true;  // adl-visibility: ADL_* diagnostics
  bool enableCtadDiag = true; // ctad-visibility: CTAD_Fallback

  // specialization-visibility: a TU implicitly instantiates a primary
  // template while an explicit specialization exists in a header that TU
  // does not include — IFNDR ([temp.expl.spec]), silently mixed
  // instantiations at link time. On by default: it is the same
  // visibility-fragility family as ADL/CTAD and fires only on a proven
  // invisible specialization.
  bool enableSpecializationDiag = true;

  // static-init-order: a dynamic initializer directly reading another
  // TU's dynamically-initialized global (the classic static initialization
  // order fiasco, proven cross-TU rather than pattern-guessed). On by
  // default: index-only and fires only on a proven cross-TU edge.
  bool enableStaticInitOrderDiag = true;

  // default-arg-divergence: declaration sites that disagree on a
  // parameter's default argument (each TU silently calls with whichever
  // value its includes provided). On by default: it fires only on an
  // actual cross-site conflict, which is near-certainly a bug.
  bool enableDefaultArgDiag = true;

  // exception-escape: noexcept functions that can transitively reach an
  // uncaught throw across TUs (std::terminate). Off by default: the
  // name-level call summaries it walks conflate overload sets and skip
  // virtual/function-pointer dispatch, so treat findings as leads.
  bool enableExceptionEscapeDiag = false;

  // Emit Coverage_* diagnostics from analyzeCoverageProperties.
  bool enableCoverageDiag = false;

  // Collect ODR definition-site hashes during phase 1 and emit ODR_*
  // diagnostics from analyzeOdrViolations: vague-linkage entities (inline
  // functions, in-class method bodies, class definitions) whose
  // definitions differ across sites or across TUs — the ODR-violation
  // class linkers silently merge instead of diagnosing.
  bool enableOdrDiag = false;

  // Emit ADL_SameScore diagnostics when an invisible candidate ties the
  // resolved overload on every argument position. Off by default because
  // it's a noisier signal than the Pareto-dominance cases.
  bool warnSameScore = false;

  // Use the indexed TypeRelationIndex (inheritance, converting ctors,
  // conversion operators) to decide whether an invisible overload is a
  // plausible candidate for a call. Off by default — the legacy
  // arithmetic-or-exact heuristic is retained when this is disabled.
  bool modelConvertibility = false;

  // Names of registered organization AnnealChecks (ExtensionRegistry) to
  // skip. Populated from OrgConfig::disabledAnnealChecks by the CLI.
  std::vector<std::string> disabledChecks;

  // Worker-pool size for the per-TU phases, with bakeIndexes' semantics:
  // 0 = all hardware threads, 1 = serial. Defaults to serial so library
  // callers see no behaviour change; the CLI passes its --threads value
  // (default 0).
  unsigned threadCount = 1;

  // When non-empty, per-TU progress is journaled to this file and replayed
  // on the next run (see anneal/Checkpoint.h): a run killed partway
  // resumes where it left off, and TUs whose parse fatally died twice are
  // skipped with a warning instead of re-killing every resume.
  std::string checkpointPath;

  // Subprocess worker isolation (megascope --isolate-workers equivalent):
  // when set, the per-TU parses run in worker subprocesses dispatched
  // through this callback under the batching + crash/bisect protocol
  // (callgraph/WorkerPool.h) instead of on the in-process pool — a TU that
  // crashes its worker costs only that TU. The CLI installs a
  // self-spawning runner; tests install in-process fakes. Composes with
  // checkpointPath: shard results are journaled as they land.
  AnnealWorkerRunner isolatedRunner;

  // Worker-process count for isolatedRunner dispatch. 0 falls back to
  // threadCount, and 0 again to hardware concurrency (megascope's
  // --workers semantics). Workers run their batch single-threaded so the
  // last WORKER-TU stderr marker is an exact poison identifier.
  unsigned workerCount = 0;
};

// AST visitor that performs shadow lookups at ADL call sites and CTAD usages.
class AnalyzerVisitor : public clang::RecursiveASTVisitor<AnalyzerVisitor> {
public:
  AnalyzerVisitor(const GlobalIndex &index, clang::SourceManager &sm,
                  std::vector<Diagnostic> &diagnostics,
                  AnalysisOptions opts = AnalysisOptions{});

  bool VisitCallExpr(clang::CallExpr *expr);
  bool VisitVarDecl(clang::VarDecl *decl);
  bool VisitClassTemplateDecl(clang::ClassTemplateDecl *decl);

  void setASTContext(clang::ASTContext *ctx) { astContext_ = ctx; }

private:
  const GlobalIndex &index_;
  clang::SourceManager &sm_;
  std::vector<Diagnostic> &diagnostics_;
  AnalysisOptions opts_;
  clang::ASTContext *astContext_ = nullptr;

  // Set of header paths included in the current TU (populated lazily).
  mutable bool includedFilesPopulated_ = false;
  mutable std::set<std::string> includedFiles_;

  // (template|args|specHeader) triples already reported this TU — the same
  // class template is visited once per redeclaration.
  std::set<std::string> reportedSpecs_;

  void populateIncludedFiles() const;
  bool isFileIncluded(const std::string &path) const;
  std::string formatLocation(clang::SourceLocation loc) const;
  std::string getFilePath(clang::SourceLocation loc) const;
};

// ASTConsumer that drives the AnalyzerVisitor, then any organization
// AnnealChecks registered with ExtensionRegistry (see vycor/ext/Extensions.h).
class AnalyzerConsumer : public clang::ASTConsumer {
public:
  AnalyzerConsumer(const GlobalIndex &index, clang::SourceManager &sm,
                   std::vector<Diagnostic> &diagnostics,
                   AnalysisOptions opts = AnalysisOptions{});
  void HandleTranslationUnit(clang::ASTContext &context) override;

private:
  AnalyzerVisitor visitor_;
  const GlobalIndex &index_;
  std::vector<Diagnostic> &diagnostics_;
  AnalysisOptions opts_;
};

// FrontendAction that creates an AnalyzerConsumer.
class AnalyzerAction : public clang::ASTFrontendAction {
public:
  AnalyzerAction(const GlobalIndex &index, std::vector<Diagnostic> &diags,
                 AnalysisOptions opts = AnalysisOptions{});
  std::unique_ptr<clang::ASTConsumer>
  CreateASTConsumer(clang::CompilerInstance &ci, llvm::StringRef file) override;

private:
  const GlobalIndex &index_;
  std::vector<Diagnostic> &diagnostics_;
  AnalysisOptions opts_;
};

// Factory for creating AnalyzerActions, for use with ClangTool.
class AnalyzerActionFactory : public clang::tooling::FrontendActionFactory {
public:
  AnalyzerActionFactory(const GlobalIndex &index,
                        std::vector<Diagnostic> &diags,
                        AnalysisOptions opts = AnalysisOptions{});
  std::unique_ptr<clang::FrontendAction> create() override;

private:
  const GlobalIndex &index_;
  std::vector<Diagnostic> &diagnostics_;
  AnalysisOptions opts_;
};

// Analyze coverage-relevant properties across classes in the index.
// Emits diagnostics for GVA linkage mismatches, discardable ODR, etc.
void analyzeCoverageProperties(const GlobalIndex &index,
                               std::vector<Diagnostic> &diagnostics);

// Compare ODR definition sites across the whole project (index-only, like
// analyzeCoverageProperties — no AST needed). Emits:
//  - ODR_DivergentDefinition: one definition site whose body hashes
//    differently across TUs (the definition depends on preprocessor state
//    that differs between compile commands);
//  - ODR_DuplicateDefinition: the same entity defined at multiple distinct
//    sites with differing content (two headers each define it — the linker
//    will keep one arbitrarily). Token-identical copies at different sites
//    are deliberately NOT flagged (vendored duplicates are benign and
//    common); method-level duplicates inside an already-flagged class are
//    suppressed as noise.
void analyzeOdrViolations(const GlobalIndex &index,
                          std::vector<Diagnostic> &diagnostics);

// Compare written-out default arguments across declaration sites
// (index-only, phase 1.5). Emits DefaultArg_Divergent when two sites
// spell DIFFERENT defaults for the same parameter of the same function.
// A site that omits a default where another writes one is NOT flagged
// (the omitting side fails to compile short calls — a visible failure —
// and the header-declares/cpp-adds-for-internal-use pattern is common
// and legal).
void analyzeDefaultArgDivergence(const GlobalIndex &index,
                                 std::vector<Diagnostic> &diagnostics);

// Static initialization order fiasco, cross-TU-proven (index-only, phase
// 1.5): a dynamic initializer directly references a global that is itself
// dynamically initialized in a DIFFERENT file — order between TUs is
// unspecified, so the reader may observe the zero/constant-initialized
// state. Constant/constinit targets are safe and not flagged.
void analyzeStaticInitOrder(const GlobalIndex &index,
                            std::vector<Diagnostic> &diagnostics);

// exception-escape (index-only, phase 1.5): BFS from every noexcept
// function through the name-level call summaries' UNGUARDED calls (calls
// inside a try are conservatively treated as handled) to a function whose
// body throws outside every try. clang-tidy's bugprone-exception-escape
// goes blind one call deep at the TU boundary; the summaries cross it.
void analyzeExceptionEscape(const GlobalIndex &index,
                            std::vector<Diagnostic> &diagnostics);

// static-init-hazards: walk the call graph from every static-init root
// (dynamic initializers via their directly-called functions; ELF
// constructor functions directly) looking for loader-hostile work:
// dlopen/dlsym/dlclose/dladdr, pthread_create/join, std::thread
// construction/join, std::async, std::call_once. Initializers run under
// the dynamic linker's global lock when the library is loaded via
// dlopen/System.loadLibrary, and whether a resulting deadlock fires can
// depend on link order. Requires a built CallGraph (the CLI builds one
// when the check is enabled, like dead-code); runAnalysis's optional
// indexOut parameter supplies the index.
void analyzeStaticInitHazards(const GlobalIndex &index,
                              const CallGraph &graph,
                              std::vector<Diagnostic> &diagnostics);

// Run the full two-phase analysis: index all sources, then analyze for
// fragile ADL/CTAD resolution. Opts controls which diagnostic classes are
// emitted and whether the convertibility model is consulted.
// When indexOut is non-null it is used as THE index (phase 1 populates
// it, later phases read it), letting the caller keep the merged index
// alive for post-analysis passes that need more context than phase 1.5
// has — the CLI hands it to analyzeStaticInitHazards together with a call
// graph. Pass a fresh GlobalIndex.
std::vector<Diagnostic>
runAnalysis(const clang::tooling::CompilationDatabase &compDb,
            const std::vector<std::string> &sourceFiles,
            const AnalysisOptions &opts, GlobalIndex *indexOut = nullptr);

// Legacy bool-shape overload preserved for callers that only want to toggle
// the coverage diagnostics. Delegates to the AnalysisOptions variant.
std::vector<Diagnostic>
runAnalysis(const clang::tooling::CompilationDatabase &compDb,
            const std::vector<std::string> &sourceFiles,
            bool enableCoverageDiag = false);

} // namespace vycor
