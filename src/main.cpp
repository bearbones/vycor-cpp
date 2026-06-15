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
#include "vycor/anneal/DeadCodeAnalyzer.h"
#include "vycor/callgraph/CallGraphBuilder.h"
#include "vycor/callgraph/ControlFlowIndex.h"
#include "vycor/callgraph/ControlFlowOracle.h"
#include "vycor/morph/RulesParser.h"
#include "vycor/morph/TransformPipeline.h"
#include "vycor/callgraph/CollapseFilter.h"
#include "vycor/callgraph/Snapshot.h"
#include "vycor/compat/PchCache.h"
#include "vycor/mcp/McpServer.h"

#include "clang/Tooling/CompilationDatabase.h"

#include <algorithm>
#include <set>
#include <unordered_map>
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

// ---------------------------------------------------------------------------
// Subcommands
// ---------------------------------------------------------------------------

static llvm::cl::SubCommand
    AnnealCmd("anneal",
              "Analyze sources for fragile ADL/CTAD resolutions");

static llvm::cl::SubCommand
    MorphCmd("morph",
              "Apply AST-based source transformations");

static llvm::cl::SubCommand
    PrismCmd("prism",
               "Query control flow and exception handling context");

static llvm::cl::SubCommand
    MegascopeCmd("megascope",
                "Start MCP server for interactive call graph queries");

// ---------------------------------------------------------------------------
// anneal options
// ---------------------------------------------------------------------------

static llvm::cl::opt<std::string>
    AnnealBuildPath("build-path",
                    llvm::cl::desc("Directory containing compile_commands.json"),
                    llvm::cl::value_desc("dir"),
                    llvm::cl::sub(AnnealCmd));

static llvm::cl::list<std::string>
    AnnealSourceFiles("source",
                      llvm::cl::desc("Source files to analyze"),
                      llvm::cl::value_desc("file"),
                      llvm::cl::OneOrMore,
                      llvm::cl::sub(AnnealCmd));

static llvm::cl::opt<bool>
    AnnealCoverageDiag("coverage-diag",
                       llvm::cl::desc("Enable coverage instrumentation diagnostics"),
                       llvm::cl::sub(AnnealCmd));

static llvm::cl::opt<bool>
    AnnealDeadCode("dead-code",
                   llvm::cl::desc("Enable dead code analysis via call graph"),
                   llvm::cl::sub(AnnealCmd));

static llvm::cl::list<std::string>
    AnnealEntryPoints("entry-point",
                      llvm::cl::desc("Entry point function names (default: main)"),
                      llvm::cl::value_desc("name"),
                      llvm::cl::sub(AnnealCmd));

static llvm::cl::opt<bool>
    AnnealWarnSameScore("warn-same-score",
                        llvm::cl::desc("Warn on ADL candidates that tie the "
                                       "resolved overload on every argument "
                                       "position"),
                        llvm::cl::sub(AnnealCmd));

static llvm::cl::opt<bool>
    AnnealModelConvertibility("model-convertibility",
                              llvm::cl::desc("Use indexed type relations "
                                             "(inheritance, converting ctors, "
                                             "conversion operators) to decide "
                                             "candidate viability"),
                              llvm::cl::sub(AnnealCmd));

// ---------------------------------------------------------------------------
// morph options
// ---------------------------------------------------------------------------

static llvm::cl::opt<std::string>
    MorphRulesJson("rules-json",
                    llvm::cl::desc("JSON file with transform rules"),
                    llvm::cl::value_desc("file"),
                    llvm::cl::sub(MorphCmd));

static llvm::cl::list<std::string>
    MorphBuildPaths("build-path",
                     llvm::cl::desc("Directory containing compile_commands.json"
                                    " (may be repeated; first match wins)"),
                     llvm::cl::value_desc("dir"),
                     llvm::cl::OneOrMore,
                     llvm::cl::sub(MorphCmd));

static llvm::cl::list<std::string>
    MorphSourceFiles("source",
                      llvm::cl::desc("Source files to transform"),
                      llvm::cl::value_desc("file"),
                      llvm::cl::OneOrMore,
                      llvm::cl::sub(MorphCmd));

static llvm::cl::opt<bool>
    MorphDryRun("dry-run",
                 llvm::cl::desc("Print replacements without applying them"),
                 llvm::cl::sub(MorphCmd));

// ---------------------------------------------------------------------------
// prism options
// ---------------------------------------------------------------------------

static llvm::cl::opt<std::string>
    PrismBuildPath("build-path",
                     llvm::cl::desc("Directory containing compile_commands.json"),
                     llvm::cl::value_desc("dir"),
                     llvm::cl::sub(PrismCmd));

static llvm::cl::list<std::string>
    PrismSourceFiles("source",
                       llvm::cl::desc("Source files to analyze"),
                       llvm::cl::value_desc("file"),
                       llvm::cl::OneOrMore,
                       llvm::cl::sub(PrismCmd));

static llvm::cl::list<std::string>
    PrismEntryPoints("entry-point",
                       llvm::cl::desc("Entry point function names (default: main)"),
                       llvm::cl::value_desc("name"),
                       llvm::cl::sub(PrismCmd));

enum PrismMode { PrismDump, PrismQuery };
static llvm::cl::opt<PrismMode>
    PrismModeOpt("mode",
                   llvm::cl::desc("Output mode"),
                   llvm::cl::values(
                       clEnumValN(PrismDump, "dump",
                                  "Dump full control flow index as JSON"),
                       clEnumValN(PrismQuery, "query",
                                  "Run a targeted query")),
                   llvm::cl::init(PrismDump),
                   llvm::cl::sub(PrismCmd));

enum PrismType {
  CfqExceptionProtection,
  CfqCallSiteContext,
  CfqAllPathContexts,
  CfqThrowPropagation,
  CfqNearestCatches
};
static llvm::cl::opt<PrismType>
    PrismQueryType("query-type",
                     llvm::cl::desc("Type of query to run (requires --mode query)"),
                     llvm::cl::values(
                         clEnumValN(CfqExceptionProtection,
                                    "exception-protection",
                                    "Is function always/sometimes/never under try/catch?"),
                         clEnumValN(CfqCallSiteContext,
                                    "call-site-context",
                                    "Exception context at a specific call site"),
                         clEnumValN(CfqAllPathContexts,
                                    "all-path-contexts",
                                    "All paths to function with exception context"),
                         clEnumValN(CfqThrowPropagation,
                                    "throw-propagation",
                                    "Is a thrown exception caught before unwinding?"),
                         clEnumValN(CfqNearestCatches,
                                    "nearest-catches",
                                    "Nearest try/catch on each path to function")),
                     llvm::cl::init(CfqExceptionProtection),
                     llvm::cl::sub(PrismCmd));

static llvm::cl::opt<std::string>
    PrismFunction("function",
                    llvm::cl::desc("Target function (qualified name)"),
                    llvm::cl::value_desc("name"),
                    llvm::cl::sub(PrismCmd));

static llvm::cl::opt<std::string>
    PrismCallSite("call-site",
                    llvm::cl::desc("Call site location (file:line:col)"),
                    llvm::cl::value_desc("location"),
                    llvm::cl::sub(PrismCmd));

static llvm::cl::opt<std::string>
    PrismExceptionType("exception-type",
                         llvm::cl::desc("Exception type for protection queries"),
                         llvm::cl::value_desc("type"),
                         llvm::cl::sub(PrismCmd));

static llvm::cl::opt<unsigned>
    PrismMaxPaths("max-paths",
                    llvm::cl::desc("Maximum number of paths to enumerate (default: 100)"),
                    llvm::cl::init(100),
                    llvm::cl::sub(PrismCmd));

static llvm::cl::list<std::string>
    PrismCollapsePaths("collapse-paths",
        llvm::cl::desc("Path patterns to collapse (internal edges skipped)"),
        llvm::cl::value_desc("pattern"),
        llvm::cl::sub(PrismCmd));

static llvm::cl::list<std::string>
    PrismSkipPaths("skip-paths",
        llvm::cl::desc("Path patterns to skip entirely (TUs matching are not processed)"),
        llvm::cl::value_desc("pattern"),
        llvm::cl::sub(PrismCmd));

static llvm::cl::opt<unsigned>
    PrismThreads("threads",
        llvm::cl::desc("Number of threads (0 = hardware_concurrency, 1 = serial)"),
        llvm::cl::init(0),
        llvm::cl::sub(PrismCmd));

static llvm::cl::opt<std::string>
    PrismPchDir("pch-dir",
        llvm::cl::desc("Directory for compiled PCH cache (enables PCH reuse)"),
        llvm::cl::value_desc("dir"),
        llvm::cl::sub(PrismCmd));

static llvm::cl::opt<std::string>
    PrismClang("clang",
        llvm::cl::desc("Path to clang++ binary for PCH compilation"),
        llvm::cl::value_desc("path"),
        llvm::cl::init(VYCOR_DEFAULT_CLANG),
        llvm::cl::sub(PrismCmd));

static llvm::cl::list<std::string>
    PrismLockTypes("lock-types",
        llvm::cl::desc("Qualified names of additional lock types (repeatable)"),
        llvm::cl::value_desc("qualified-name"),
        llvm::cl::sub(PrismCmd));

static llvm::cl::opt<std::string>
    PrismSysroot("sysroot",
        llvm::cl::desc("macOS SDK sysroot path (default: auto-detect via xcrun)"),
        llvm::cl::value_desc("dir"),
        llvm::cl::sub(PrismCmd));

// ---------------------------------------------------------------------------
// megascope options
// ---------------------------------------------------------------------------

static llvm::cl::opt<std::string>
    McpBuildPath("build-path",
                 llvm::cl::desc("Directory containing compile_commands.json"),
                 llvm::cl::value_desc("dir"),
                 llvm::cl::sub(MegascopeCmd));

static llvm::cl::list<std::string>
    McpSourceFiles("source",
                   llvm::cl::desc("Source files to analyze"),
                   llvm::cl::value_desc("file"),
                   llvm::cl::OneOrMore,
                   llvm::cl::sub(MegascopeCmd));

static llvm::cl::list<std::string>
    McpEntryPoints("entry-point",
                   llvm::cl::desc("Entry point function names (default: main)"),
                   llvm::cl::value_desc("name"),
                   llvm::cl::sub(MegascopeCmd));

static llvm::cl::list<std::string>
    McpCollapsePaths("collapse-paths",
        llvm::cl::desc("Path patterns to collapse (internal edges skipped)"),
        llvm::cl::value_desc("pattern"),
        llvm::cl::sub(MegascopeCmd));

static llvm::cl::list<std::string>
    McpSkipPaths("skip-paths",
        llvm::cl::desc("Path patterns to skip entirely (TUs matching are not processed)"),
        llvm::cl::value_desc("pattern"),
        llvm::cl::sub(MegascopeCmd));

static llvm::cl::opt<unsigned>
    McpThreads("threads",
        llvm::cl::desc("Number of threads (0 = hardware_concurrency, 1 = serial)"),
        llvm::cl::init(0),
        llvm::cl::sub(MegascopeCmd));

static llvm::cl::opt<std::string>
    McpPchDir("pch-dir",
        llvm::cl::desc("Directory for compiled PCH cache (enables PCH reuse)"),
        llvm::cl::value_desc("dir"),
        llvm::cl::sub(MegascopeCmd));

static llvm::cl::opt<std::string>
    McpClang("clang",
        llvm::cl::desc("Path to clang++ binary for PCH compilation"),
        llvm::cl::value_desc("path"),
        llvm::cl::init(VYCOR_DEFAULT_CLANG),
        llvm::cl::sub(MegascopeCmd));

static llvm::cl::opt<std::string>
    McpSysroot("sysroot",
        llvm::cl::desc("macOS SDK sysroot path (default: auto-detect via xcrun)"),
        llvm::cl::value_desc("dir"),
        llvm::cl::sub(MegascopeCmd));

static llvm::cl::list<std::string>
    McpLockTypes("lock-types",
        llvm::cl::desc("Qualified names of additional lock types (repeatable)"),
        llvm::cl::value_desc("qualified-name"),
        llvm::cl::sub(MegascopeCmd));

static llvm::cl::opt<std::string>
    McpSnapshot("snapshot",
        llvm::cl::desc("Snapshot file for warm starts: load the baked graph "
                       "if present (reindexing only changed TUs), and save "
                       "after building"),
        llvm::cl::value_desc("file"),
        llvm::cl::sub(MegascopeCmd));

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, const char **argv) {
  llvm::cl::ParseCommandLineOptions(
      argc, argv,
      "vycor-cpp: AST-based C++ analysis and transformation tool\n"
      "\nSubcommands:\n"
      "  anneal     Detect fragile ADL/CTAD resolution across translation units\n"
      "  morph     Apply rule-driven AST matcher transformations\n"
      "  prism    Query control flow and exception handling context\n"
      "  megascope  Start MCP server for interactive call graph queries\n");

  // ---- anneal ---------------------------------------------------------------
  if (AnnealCmd) {
    if (AnnealBuildPath.empty()) {
      llvm::errs() << "anneal: --build-path is required\n";
      return 1;
    }
    if (AnnealSourceFiles.empty()) {
      llvm::errs() << "anneal: at least one --source file is required\n";
      return 1;
    }

    std::string dbError;
    auto compDb = clang::tooling::CompilationDatabase::loadFromDirectory(
        AnnealBuildPath, dbError);
    if (!compDb) {
      llvm::errs() << "anneal: error loading compilation database from "
                   << AnnealBuildPath << ": " << dbError << "\n";
      return 1;
    }

    std::vector<std::string> files(AnnealSourceFiles.begin(),
                                   AnnealSourceFiles.end());
    vycor::AnalysisOptions opts;
    opts.enableCoverageDiag = AnnealCoverageDiag;
    opts.warnSameScore = AnnealWarnSameScore;
    opts.modelConvertibility = AnnealModelConvertibility;
    auto diagnostics = vycor::runAnalysis(*compDb, files, opts);

    // Dead code analysis.
    if (AnnealDeadCode) {
      auto graph = vycor::buildCallGraph(*compDb, files);

      std::vector<std::string> entryPoints(AnnealEntryPoints.begin(),
                                           AnnealEntryPoints.end());
      if (entryPoints.empty())
        entryPoints.push_back("main");

      vycor::DeadCodeAnalyzer analyzer(graph, entryPoints);
      analyzer.analyzePessimistic();
      analyzer.analyzeOptimistic();

      auto deadDiags = analyzer.getDiagnostics();
      for (const auto &diag : deadDiags) {
        diagnostics.push_back(diag);
      }
    }

    if (diagnostics.empty()) {
      llvm::outs() << "anneal: no issues found.\n";
      return 0;
    }

    for (const auto &diag : diagnostics)
      llvm::outs() << diag.callLocation << ": " << diag.message << "\n";

    return 0;
  }

  // ---- morph ---------------------------------------------------------------
  if (MorphCmd) {
    if (MorphRulesJson.empty()) {
      llvm::errs() << "morph: --rules-json is required\n";
      return 1;
    }
    if (MorphSourceFiles.empty()) {
      llvm::errs() << "morph: at least one --source file is required\n";
      return 1;
    }

    auto rulesOrErr = vycor::parseRulesFile(MorphRulesJson);
    if (!rulesOrErr) {
      llvm::errs() << "morph: " << llvm::toString(rulesOrErr.takeError())
                   << "\n";
      return 1;
    }

    auto passRulesOrErr = vycor::buildPipeline(*rulesOrErr);
    if (!passRulesOrErr) {
      llvm::errs() << "morph: " << llvm::toString(passRulesOrErr.takeError())
                   << "\n";
      return 1;
    }

    vycor::TransformPipeline pipeline;
    for (auto &pass : *passRulesOrErr)
      pipeline.addPass(std::move(pass));

    std::vector<std::string> buildPaths(MorphBuildPaths.begin(),
                                        MorphBuildPaths.end());
    std::vector<std::string> files(MorphSourceFiles.begin(),
                                   MorphSourceFiles.end());
    return pipeline.execute(buildPaths, files, MorphDryRun);
  }

  // ---- prism --------------------------------------------------------------
  if (PrismCmd) {
    if (PrismBuildPath.empty()) {
      llvm::errs() << "prism: --build-path is required\n";
      return 1;
    }
    if (PrismSourceFiles.empty()) {
      llvm::errs() << "prism: at least one --source file is required\n";
      return 1;
    }

    std::string dbError;
    auto compDb = clang::tooling::CompilationDatabase::loadFromDirectory(
        PrismBuildPath, dbError);
    if (!compDb) {
      llvm::errs() << "prism: error loading compilation database from "
                   << PrismBuildPath << ": " << dbError << "\n";
      return 1;
    }

    std::vector<std::string> files(PrismSourceFiles.begin(),
                                   PrismSourceFiles.end());
    std::vector<std::string> collapsePaths(PrismCollapsePaths.begin(),
                                           PrismCollapsePaths.end());

    // Filter out TUs matching --skip-paths.
    if (!PrismSkipPaths.empty()) {
      vycor::CollapseFilter skipFilter(
          {PrismSkipPaths.begin(), PrismSkipPaths.end()});
      size_t before = files.size();
      files.erase(std::remove_if(files.begin(), files.end(),
                                  [&](const std::string &f) {
                                    return skipFilter.isCollapsed(f);
                                  }),
                  files.end());
      llvm::errs() << "skip-paths: " << (before - files.size())
                   << " of " << before << " TUs skipped\n";
    }

    // Pre-compile PCH headers if --pch-dir is set.
    std::unique_ptr<vycor::PchCache> pchCache;
    if (!PrismPchDir.empty()) {
      pchCache = std::make_unique<vycor::PchCache>(
          PrismPchDir.getValue(), PrismClang.getValue());
      pchCache->buildFromCompileCommands(*compDb, files);
    }
    const vycor::PchCache *pchPtr = pchCache.get();

    std::string sysroot = PrismSysroot.getValue();

    // Phases 1-3: bake call graph and control flow index (Phase 2+3 share
    // one parse per TU).
    vycor::LockTypeConfig lockCfg;
    lockCfg.userAllowlist.assign(PrismLockTypes.begin(),
                                 PrismLockTypes.end());
    auto baked = vycor::bakeIndexes(*compDb, files, collapsePaths,
                                    PrismThreads, pchPtr, sysroot, lockCfg);
    auto graph = std::move(baked.graph);
    auto cfIndex = std::move(baked.cfIndex);

    // Dump mode: serialize the full index as JSON.
    if (PrismModeOpt == PrismDump) {
      llvm::outs() << vycor::ControlFlowOracle::dumpIndexToJson(cfIndex);
      return 0;
    }

    // Query mode: run a specific query.
    vycor::ControlFlowOracle oracle(graph, cfIndex);

    std::vector<std::string> entryPoints(PrismEntryPoints.begin(),
                                         PrismEntryPoints.end());
    if (entryPoints.empty())
      entryPoints.push_back("main");

    switch (PrismQueryType) {
    case CfqCallSiteContext: {
      if (PrismCallSite.empty()) {
        llvm::errs() << "prism: --call-site is required for "
                        "call-site-context query\n";
        return 1;
      }
      auto info = oracle.queryCallSite(PrismCallSite);
      // Simple JSON output for call site info.
      llvm::outs() << "{\n"
                   << "  \"callSite\": \"" << info.callSite << "\",\n"
                   << "  \"caller\": \"" << info.caller << "\",\n"
                   << "  \"callee\": \"" << info.callee << "\",\n"
                   << "  \"isUnderTryCatch\": "
                   << (info.isUnderTryCatch ? "true" : "false") << ",\n"
                   << "  \"wouldTerminateIfThrows\": "
                   << (info.wouldTerminateIfThrows ? "true" : "false") << ",\n"
                   << "  \"enclosingScopeCount\": "
                   << info.enclosingScopes.size() << ",\n"
                   << "  \"enclosingGuardCount\": "
                   << info.enclosingGuards.size() << "\n"
                   << "}\n";
      return 0;
    }

    case CfqExceptionProtection: {
      if (PrismFunction.empty()) {
        llvm::errs() << "prism: --function is required for "
                        "exception-protection query\n";
        return 1;
      }
      auto result = oracle.queryExceptionProtection(
          PrismFunction, PrismExceptionType, entryPoints);
      llvm::outs() << vycor::ControlFlowOracle::toJson(
          result, "exception-protection", PrismFunction,
          PrismExceptionType);
      return 0;
    }

    case CfqAllPathContexts: {
      if (PrismFunction.empty()) {
        llvm::errs() << "prism: --function is required for "
                        "all-path-contexts query\n";
        return 1;
      }
      auto result = oracle.queryExceptionProtection(
          PrismFunction, PrismExceptionType, entryPoints);
      llvm::outs() << vycor::ControlFlowOracle::toJson(
          result, "all-path-contexts", PrismFunction,
          PrismExceptionType);
      return 0;
    }

    case CfqThrowPropagation: {
      if (PrismFunction.empty()) {
        llvm::errs() << "prism: --function is required for "
                        "throw-propagation query\n";
        return 1;
      }
      auto result = oracle.queryThrowPropagation(
          PrismFunction, PrismExceptionType, entryPoints);
      llvm::outs() << vycor::ControlFlowOracle::toJson(
          result, "throw-propagation", PrismFunction,
          PrismExceptionType);
      return 0;
    }

    case CfqNearestCatches: {
      if (PrismFunction.empty()) {
        llvm::errs() << "prism: --function is required for "
                        "nearest-catches query\n";
        return 1;
      }
      auto catches = oracle.queryNearestCatches(PrismFunction);
      llvm::outs() << "{\n"
                   << "  \"query\": \"nearest-catches\",\n"
                   << "  \"function\": \"" << PrismFunction.getValue()
                   << "\",\n"
                   << "  \"results\": [\n";
      for (size_t i = 0; i < catches.size(); ++i) {
        const auto &c = catches[i];
        llvm::outs() << "    {\n"
                     << "      \"framesFromTarget\": " << c.framesFromTarget
                     << ",\n"
                     << "      \"tryLocation\": \""
                     << c.scope.tryLocation << "\",\n"
                     << "      \"enclosingFunction\": \""
                     << c.scope.enclosingFunction << "\",\n"
                     << "      \"pathSegment\": [";
        for (size_t j = 0; j < c.pathSegment.size(); ++j) {
          llvm::outs() << "\"" << c.pathSegment[j] << "\"";
          if (j + 1 < c.pathSegment.size())
            llvm::outs() << ", ";
        }
        llvm::outs() << "]\n"
                     << "    }";
        if (i + 1 < catches.size())
          llvm::outs() << ",";
        llvm::outs() << "\n";
      }
      llvm::outs() << "  ]\n}\n";
      return 0;
    }
    }

    return 0;
  }

  // ---- megascope -------------------------------------------------------------
  if (MegascopeCmd) {
    if (McpBuildPath.empty()) {
      llvm::errs() << "megascope: --build-path is required\n";
      return 1;
    }
    if (McpSourceFiles.empty()) {
      llvm::errs() << "megascope: at least one --source file is required\n";
      return 1;
    }

    std::string dbError;
    auto compDb = clang::tooling::CompilationDatabase::loadFromDirectory(
        McpBuildPath, dbError);
    if (!compDb) {
      llvm::errs() << "megascope: error loading compilation database from "
                   << McpBuildPath << ": " << dbError << "\n";
      return 1;
    }

    std::vector<std::string> files(McpSourceFiles.begin(),
                                   McpSourceFiles.end());
    std::vector<std::string> collapsePaths(McpCollapsePaths.begin(),
                                           McpCollapsePaths.end());

    // Filter out TUs matching --skip-paths.
    if (!McpSkipPaths.empty()) {
      vycor::CollapseFilter skipFilter(
          {McpSkipPaths.begin(), McpSkipPaths.end()});
      size_t before = files.size();
      files.erase(std::remove_if(files.begin(), files.end(),
                                  [&](const std::string &f) {
                                    return skipFilter.isCollapsed(f);
                                  }),
                  files.end());
      llvm::errs() << "megascope: skip-paths: " << (before - files.size())
                   << " of " << before << " TUs skipped\n";
    }

    // Pre-compile PCH headers if --pch-dir is set.
    std::unique_ptr<vycor::PchCache> pchCache;
    if (!McpPchDir.empty()) {
      llvm::errs() << "megascope: building PCH cache...\n";
      pchCache = std::make_unique<vycor::PchCache>(
          McpPchDir.getValue(), McpClang.getValue());
      pchCache->buildFromCompileCommands(*compDb, files);
    }
    const vycor::PchCache *pchPtr = pchCache.get();

    std::string sysroot = McpSysroot.getValue();

    vycor::LockTypeConfig lockCfg;
    lockCfg.userAllowlist.assign(McpLockTypes.begin(), McpLockTypes.end());

    vycor::CallGraph graph;
    vycor::ControlFlowIndex cfIndex;
    bool needFullBuild = true;

    // Stamps are taken before any parsing: a file modified mid-build gets a
    // stale stamp and is conservatively re-indexed on the next warm start.
    auto currentStamps = vycor::SnapshotIO::stampFiles(files);

    if (!McpSnapshot.empty()) {
      if (auto snap = vycor::SnapshotIO::load(McpSnapshot)) {
        bool configMatch =
            snap->meta.collapsePaths == collapsePaths &&
            snap->meta.lockAllowlist == lockCfg.userAllowlist &&
            snap->meta.lockBuiltins == lockCfg.useBuiltins;
        if (!configMatch) {
          llvm::errs() << "megascope: snapshot build configuration differs "
                          "— full rebuild\n";
        } else {
          graph = std::move(snap->graph);
          cfIndex = std::move(snap->cfIndex);
          needFullBuild = false;

          std::unordered_map<std::string, const vycor::FileStamp *> baked;
          for (const auto &fs : snap->meta.files)
            baked[fs.path] = &fs;
          std::set<std::string> current(files.begin(), files.end());

          size_t dropped = 0, refreshed = 0;
          for (const auto &fs : snap->meta.files) {
            if (!current.count(fs.path)) {
              graph.removeTU(fs.path);
              cfIndex.removeTU(fs.path);
              ++dropped;
            }
          }
          for (const auto &stamp : currentStamps) {
            auto it = baked.find(stamp.path);
            if (it != baked.end() && *it->second == stamp)
              continue; // Unchanged since the snapshot was taken.
            if (it != baked.end()) {
              graph.removeTU(stamp.path);
              cfIndex.removeTU(stamp.path);
            }
            vycor::bakeTU(graph, cfIndex, *compDb, stamp.path,
                          collapsePaths, pchPtr, sysroot, lockCfg);
            ++refreshed;
          }
          llvm::errs() << "megascope: warm start from " << McpSnapshot
                       << " (" << refreshed << " TU(s) re-indexed, "
                       << dropped << " dropped, "
                       << graph.nodeCount() << " nodes, "
                       << graph.edgeCount() << " edges, "
                       << cfIndex.size() << " call sites)\n";
        }
      } else {
        llvm::errs() << "megascope: no usable snapshot at " << McpSnapshot
                     << " — full build\n";
      }
    }

    if (needFullBuild) {
      llvm::errs() << "megascope: baking call graph + control flow index ("
                   << files.size() << " files, "
                   << McpThreads << " threads)...\n";
      auto baked = vycor::bakeIndexes(*compDb, files, collapsePaths,
                                      McpThreads, pchPtr, sysroot, lockCfg);
      graph = std::move(baked.graph);
      cfIndex = std::move(baked.cfIndex);
      llvm::errs() << "megascope: indexes built ("
                   << graph.nodeCount() << " nodes, "
                   << graph.edgeCount() << " edges, "
                   << cfIndex.size() << " call sites)\n";
    }

    if (!McpSnapshot.empty()) {
      vycor::SnapshotMeta meta;
      meta.collapsePaths = collapsePaths;
      meta.lockAllowlist = lockCfg.userAllowlist;
      meta.lockBuiltins = lockCfg.useBuiltins;
      meta.files = std::move(currentStamps);
      if (vycor::SnapshotIO::save(McpSnapshot, graph, cfIndex, meta))
        llvm::errs() << "megascope: snapshot saved to " << McpSnapshot << "\n";
      else
        llvm::errs() << "megascope: WARNING: could not save snapshot to "
                     << McpSnapshot << "\n";
    }

    std::vector<std::string> entryPoints(McpEntryPoints.begin(),
                                         McpEntryPoints.end());
    if (entryPoints.empty())
      entryPoints.push_back("main");

    vycor::McpBuildParams buildParams;
    buildParams.compDb = std::shared_ptr<clang::tooling::CompilationDatabase>(
        std::move(compDb));
    buildParams.collapsePaths = collapsePaths;
    buildParams.pchCache = pchPtr;
    buildParams.sysroot = sysroot;
    buildParams.lockCfg = std::move(lockCfg);

    vycor::McpServer server(std::move(graph), std::move(cfIndex),
                                 std::move(entryPoints),
                                 std::move(buildParams));
    return server.run();
  }

  llvm::errs() << "No subcommand specified. Use 'anneal', 'morph', "
                  "'prism', or 'megascope'.\n"
               << "Run with --help for usage information.\n";
  return 1;
}
