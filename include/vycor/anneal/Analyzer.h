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
  // Emit Coverage_* diagnostics from analyzeCoverageProperties.
  bool enableCoverageDiag = false;

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

private:
  const GlobalIndex &index_;
  clang::SourceManager &sm_;
  std::vector<Diagnostic> &diagnostics_;
  AnalysisOptions opts_;

  // Set of header paths included in the current TU (populated lazily).
  mutable bool includedFilesPopulated_ = false;
  mutable std::set<std::string> includedFiles_;

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

// Run the full two-phase analysis: index all sources, then analyze for
// fragile ADL/CTAD resolution. Opts controls which diagnostic classes are
// emitted and whether the convertibility model is consulted.
std::vector<Diagnostic>
runAnalysis(const clang::tooling::CompilationDatabase &compDb,
            const std::vector<std::string> &sourceFiles,
            const AnalysisOptions &opts);

// Legacy bool-shape overload preserved for callers that only want to toggle
// the coverage diagnostics. Delegates to the AnalysisOptions variant.
std::vector<Diagnostic>
runAnalysis(const clang::tooling::CompilationDatabase &compDb,
            const std::vector<std::string> &sourceFiles,
            bool enableCoverageDiag = false);

} // namespace vycor
