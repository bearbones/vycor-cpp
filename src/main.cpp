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
#include "vycor/anneal/CheckSet.h"
#include "vycor/anneal/Checkpoint.h"
#include "vycor/anneal/DeadCodeAnalyzer.h"
#include "vycor/anneal/Indexer.h"
#include "vycor/callgraph/BuildStats.h"
#include "vycor/callgraph/CallGraphBuilder.h"
#include "vycor/callgraph/ControlFlowIndex.h"
#include "vycor/ext/Extensions.h"
#include "vycor/ext/OrgConfig.h"
#include "vycor/morph/RulesParser.h"
#include "vycor/morph/TransformPipeline.h"
#include "vycor/callgraph/CollapseFilter.h"
#include "vycor/callgraph/InputFingerprint.h"
#include "vycor/callgraph/Snapshot.h"
#include "vycor/callgraph/WorkerPool.h"
#include "vycor/cli/BakeConfig.h"
#include "vycor/cli/MegascopeCli.h"
#include "vycor/cli/SourceSelection.h"
#include "vycor/compat/PchCache.h"
#include "vycor/mcp/McpServer.h"
#include "vycor/Version.h"

#include "clang/Tooling/CompilationDatabase.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <optional>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"
#include "vycor/compat/ToolAdjusters.h"

#if defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#endif

/// Per-section decode timing of a snapshot load, for --stats-json and the
/// query verbs' -v (docs/megascope-cli-review.md §3.1.1).
static llvm::json::Array
loadSectionsJson(const vycor::SnapshotLoadStats &stats) {
  llvm::json::Array out;
  for (const auto &sec : stats.sections) {
    llvm::json::Object o;
    o["name"] = sec.name;
    o["bytes"] = static_cast<int64_t>(sec.bytes);
    o["ms"] = sec.ms;
    o["skipped"] = sec.skipped;
    out.push_back(std::move(o));
  }
  return out;
}


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
    MegascopeCmd("megascope",
                 "Index a project's call graph and query it (verbs: index, "
                 "serve, tools, info, batch, <tool>; `megascope help`)");

// ---------------------------------------------------------------------------
// options common to all subcommands
// ---------------------------------------------------------------------------

// Extra compiler args appended to every compile command. The escape hatch
// for host-toolchain mismatches the adjusters cannot fix generically.
// (The headerless-newest-GCC case — --gcc-install-dir on hosts whose
// newest /usr/lib/gcc directory has no matching libstdc++ headers — is now
// auto-detected by getGccInstallDirAdjuster; an explicit --extra-arg
// toolchain flag still overrides it.)
static llvm::cl::list<std::string>
    ExtraArgs("extra-arg",
        llvm::cl::desc("Additional compiler argument appended to every "
                       "compile command (repeatable)"),
        llvm::cl::value_desc("arg"),
        llvm::cl::sub(llvm::cl::SubCommand::getAll()));

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
                      // Required-ness enforced manually in main(): parse-time
                      // OneOrMore would break --list-checks.
                      llvm::cl::ZeroOrMore,
                      llvm::cl::sub(AnnealCmd));

static llvm::cl::opt<bool>
    AnnealCoverageDiag("coverage-diag",
                       llvm::cl::desc("Enable coverage instrumentation diagnostics"),
                       llvm::cl::sub(AnnealCmd));

static llvm::cl::opt<bool>
    AnnealListChecks("list-checks",
        llvm::cl::desc("Print every known check (built-in and organization) "
                       "with defaults, groups, and summaries, then exit"),
        llvm::cl::sub(AnnealCmd));

static llvm::cl::opt<std::string>
    AnnealChecks("checks",
        llvm::cl::desc("Comma-separated check specification applied after "
                       "any .vycor-anneal.json: names enable, -names "
                       "disable, group names (e.g. all, noisy, "
                       "compute-heavy) expand. See docs/checks/README.md."),
        llvm::cl::value_desc("spec"),
        llvm::cl::sub(AnnealCmd));

static llvm::cl::opt<std::string>
    AnnealChecksConfig("checks-config",
        llvm::cl::desc("Explicit checks-config file (default: search for "
                       ".vycor-anneal.json from the working directory "
                       "upward)"),
        llvm::cl::value_desc("file"),
        llvm::cl::sub(AnnealCmd));

static llvm::cl::opt<bool>
    AnnealOdrDiag("odr-diag",
        llvm::cl::desc("Detect ODR violations among vague-linkage "
                       "definitions (inline functions, in-class method "
                       "bodies, class definitions) across TUs — the class "
                       "of mismatch linkers merge silently instead of "
                       "diagnosing"),
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

static llvm::cl::opt<unsigned>
    AnnealThreads("threads",
        llvm::cl::desc("Number of threads for the per-TU analysis phases "
                       "(0 = hardware_concurrency, 1 = serial)"),
        llvm::cl::init(0),
        llvm::cl::sub(AnnealCmd));

static llvm::cl::opt<bool>
    AnnealIsolateWorkers("isolate-workers",
        llvm::cl::desc("Run the per-TU parses in worker subprocesses (a "
                       "crashing TU costs only that TU; composes with "
                       "--checkpoint)"),
        llvm::cl::init(false),
        llvm::cl::sub(AnnealCmd));

static llvm::cl::opt<unsigned>
    AnnealWorkers("workers",
        llvm::cl::desc("Number of worker processes for --isolate-workers "
                       "(0 = the --threads value)"),
        llvm::cl::init(0),
        llvm::cl::sub(AnnealCmd));

// Worker-mode plumbing (spawned by the --isolate-workers parent; not part
// of the user-facing surface). Mirrors megascope's --bake-worker.
static llvm::cl::opt<bool>
    AnnealIndexWorker("index-worker",
        llvm::cl::desc("Internal: index the --source list and write an "
                       "anneal index shard instead of analyzing"),
        llvm::cl::Hidden,
        llvm::cl::sub(AnnealCmd));

static llvm::cl::opt<bool>
    AnnealAnalyzeWorker("analyze-worker",
        llvm::cl::desc("Internal: analyze the --source list against "
                       "--global-index and write a diagnostics shard"),
        llvm::cl::Hidden,
        llvm::cl::sub(AnnealCmd));

static llvm::cl::opt<std::string>
    AnnealWorkerOut("worker-out",
        llvm::cl::desc("Internal: shard output path for worker modes"),
        llvm::cl::value_desc("file"),
        llvm::cl::Hidden,
        llvm::cl::sub(AnnealCmd));

static llvm::cl::opt<std::string>
    AnnealGlobalIndexIn("global-index",
        llvm::cl::desc("Internal: merged-index handoff file for "
                       "--analyze-worker"),
        llvm::cl::value_desc("file"),
        llvm::cl::Hidden,
        llvm::cl::sub(AnnealCmd));

static llvm::cl::opt<std::string>
    AnnealCheckpointFile("checkpoint",
        llvm::cl::desc("Journal per-TU progress to this file and resume "
                       "from it on the next run: a killed run picks up "
                       "where it left off, and a TU whose parse fatally "
                       "died twice is skipped instead of re-killing every "
                       "resume. Source edits invalidate exactly the "
                       "affected entries."),
        llvm::cl::value_desc("file"),
        llvm::cl::sub(AnnealCmd));

static llvm::cl::opt<std::string>
    AnnealOrgConfig("org-config",
        llvm::cl::desc("Organization config JSON (lock/channel types, "
                       "feature-flag patterns, disabled checks — see "
                       "docs/EXTENDING.md)"),
        llvm::cl::value_desc("file"),
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
// megascope options
// ---------------------------------------------------------------------------

static llvm::cl::opt<std::string>
    McpBuildPath("build-path",
                 llvm::cl::desc("Directory containing compile_commands.json"),
                 llvm::cl::value_desc("dir"),
                 llvm::cl::sub(MegascopeCmd));

static llvm::cl::list<std::string>
    McpSourceFiles("source",
                   llvm::cl::desc("Source files to index (repeatable). "
                                  "Default: every entry of the compilation "
                                  "database"),
                   llvm::cl::value_desc("file"),
                   llvm::cl::sub(MegascopeCmd));

static llvm::cl::opt<std::string>
    McpSourceList("source-list",
        llvm::cl::desc("File with one source path per line ('-' = stdin; "
                       "'#' comments); unioned with --source"),
        llvm::cl::value_desc("file"),
        llvm::cl::sub(MegascopeCmd));

static llvm::cl::opt<std::string>
    McpSourceRe("source-re",
        llvm::cl::desc("Keep only source paths matching this POSIX extended "
                       "regex (searched, not anchored)"),
        llvm::cl::value_desc("regex"),
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
    McpChannelTypesJson("channel-types-json",
        llvm::cl::desc("JSON file registering channel/queue types to trace "
                       "producer/consumer call sites for (see "
                       "ChannelIndex.h for the schema)."),
        llvm::cl::value_desc("file"),
        llvm::cl::sub(MegascopeCmd));

static llvm::cl::opt<std::string>
    McpOrgConfig("org-config",
        llvm::cl::desc("Organization config JSON (lock/channel types, "
                       "feature-flag patterns, collapse paths — see "
                       "docs/EXTENDING.md). Merged with the equivalent "
                       "CLI flags."),
        llvm::cl::value_desc("file"),
        llvm::cl::sub(MegascopeCmd));

static llvm::cl::opt<std::string>
    McpIndex("index",
        llvm::cl::desc("Index file: load the baked graph if present "
                       "(re-indexing only changed TUs) and save after "
                       "building. The index/serve verbs default it to "
                       "<build-path>/.vycor/megascope.vycs"),
        llvm::cl::value_desc("file"),
        llvm::cl::sub(MegascopeCmd));

// Pre-verb spelling; cl::alias must not carry cl::sub (it inherits
// McpIndex's subcommand).
static llvm::cl::alias
    McpSnapshotAlias("snapshot", llvm::cl::desc("Alias for --index"),
                     llvm::cl::aliasopt(McpIndex), llvm::cl::NotHidden);

static llvm::cl::opt<bool>
    McpVerbose("v",
        llvm::cl::desc("Verbose: per-request logging in the serve loop"),
        llvm::cl::init(false),
        llvm::cl::sub(MegascopeCmd));

static llvm::cl::opt<bool>
    McpMcp("mcp",
        llvm::cl::desc("serve: speak MCP over stdio (the only transport "
                       "today; on by default)"),
        llvm::cl::init(true),
        llvm::cl::sub(MegascopeCmd));

static llvm::cl::opt<bool>
    McpIsolateWorkers("isolate-workers",
        llvm::cl::desc("Bake the indexes in subprocess workers (a crashing "
                       "TU costs only that TU; parent RSS stays bounded)"),
        llvm::cl::init(false),
        llvm::cl::sub(MegascopeCmd));

static llvm::cl::opt<bool>
    McpForce("force",
        llvm::cl::desc("Rebuild the index from scratch instead of "
                       "refreshing the TUs whose sources or headers "
                       "changed"),
        llvm::cl::init(false),
        llvm::cl::sub(MegascopeCmd));

static llvm::cl::opt<bool>
    McpRetryFailed("retry-failed",
        llvm::cl::desc("Re-parse the TUs whose last parse failed even "
                       "when nothing else changed (by default they are "
                       "retried only alongside a refresh that rewrites "
                       "the index anyway)"),
        llvm::cl::init(false),
        llvm::cl::sub(MegascopeCmd));

static llvm::cl::opt<unsigned>
    McpWorkers("workers",
        llvm::cl::desc("Number of worker processes for --isolate-workers "
                       "(0 = the --threads value)"),
        llvm::cl::init(0),
        llvm::cl::sub(MegascopeCmd));

// Worker-mode plumbing (spawned by the --isolate-workers parent; not part
// of the user-facing surface).
static llvm::cl::opt<bool>
    McpBakeWorker("bake-worker",
        llvm::cl::desc("Internal: bake the --source list and write a "
                       "snapshot shard instead of serving"),
        llvm::cl::Hidden,
        llvm::cl::sub(MegascopeCmd));

static llvm::cl::opt<std::string>
    McpWorkerOut("worker-out",
        llvm::cl::desc("Internal: shard output path for --bake-worker"),
        llvm::cl::value_desc("file"),
        llvm::cl::Hidden,
        llvm::cl::sub(MegascopeCmd));

static llvm::cl::opt<std::string>
    McpStatsJson("stats-json",
        llvm::cl::desc("Write index-build efficiency statistics (per-phase "
                       "and per-TU timings, parse outcomes, graph sizes, "
                       "snapshot timings, peak RSS) as JSON to this file "
                       "once the server is ready"),
        llvm::cl::value_desc("file"),
        llvm::cl::sub(MegascopeCmd));

static llvm::cl::opt<std::string>
    McpDumpNodes("dump-nodes",
        llvm::cl::desc("Write the node inventory as TSV (usr, display name, "
                       "file, line, comma-joined caller usrs) to this file "
                       "once the index is ready, then serve normally. "
                       "Measurement aid for identity/growth analysis "
                       "(docs/design-f8-usr-identity.md risk note)"),
        llvm::cl::value_desc("file"),
        llvm::cl::sub(MegascopeCmd));

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, const char **argv) {
  // megascope verbs (docs/megascope-cli-review.md §2.1). llvm::cl
  // subcommands are single-level, so the verb is peeled off argv here:
  // query verbs never reach llvm::cl (their flags come from each tool's
  // JSON schema), while `index`/`serve` share the bake option block with
  // the legacy verb-less form and hand the remaining argv to llvm::cl.
  enum class MegascopeVerb { Legacy, Index, Serve };
  MegascopeVerb megascopeVerb = MegascopeVerb::Legacy;
  std::vector<const char *> peeledArgv;
  if (argc >= 2 && llvm::StringRef(argv[1]) == "prism") {
    // Folded into megascope (docs/megascope-cli-review.md §4.1): the query
    // verbs bake in memory when given the selection flags, and `dump`
    // streams what `--mode dump` printed.
    llvm::errs()
        << "vycor-cpp prism has been folded into megascope:\n"
           "  --mode dump   -> megascope dump --build-path <dir> "
           "--source <file>...\n"
           "  --mode query  -> megascope query-exception-safety | "
           "query-call-site-context |\n"
           "                   query-all-path-contexts | "
           "query-throw-propagation |\n"
           "                   query-nearest-catches, with the same "
           "--build-path/--source\n"
           "Run `vycor-cpp megascope help`.\n";
    return vycor::kExitUsage;
  }
  if (argc >= 2 && llvm::StringRef(argv[1]) == "megascope") {
    if (argc == 2)
      return vycor::runMegascopeQueryVerb({}, llvm::outs(), llvm::errs(),
                                          std::cin);
    llvm::StringRef verb = argv[2];
    if (vycor::isMegascopeQueryVerb(verb)) {
      std::vector<std::string> rest(argv + 2, argv + argc);
      return vycor::runMegascopeQueryVerb(rest, llvm::outs(), llvm::errs(),
                                          std::cin);
    }
    if (verb == "index" || verb == "serve") {
      megascopeVerb =
          verb == "index" ? MegascopeVerb::Index : MegascopeVerb::Serve;
      peeledArgv.assign(argv, argv + argc);
      peeledArgv.erase(peeledArgv.begin() + 2);
      argv = peeledArgv.data();
      --argc;
    }
  }

  llvm::cl::AddExtraVersionPrinter([](llvm::raw_ostream &os) {
    os << "vycor-cpp version " << VYCOR_VERSION_STRING << "\n";
    os << "Host compiler: " << VYCOR_HOST_COMPILER_ID << " "
       << VYCOR_HOST_COMPILER_VERSION << "\n";
  });

  llvm::cl::ParseCommandLineOptions(
      argc, argv,
      "vycor-cpp: AST-based C++ analysis and transformation tool\n"
      "\nSubcommands:\n"
      "  anneal     Detect fragile ADL/CTAD resolution across translation units\n"
      "  morph     Apply rule-driven AST matcher transformations\n"
      "  megascope  Index a project's call graph and query it "
      "(`megascope help`)\n");

  vycor::appendGlobalExtraArgs({ExtraArgs.begin(), ExtraArgs.end()});

  // ---- anneal ---------------------------------------------------------------
  if (AnnealCmd) {
    if (AnnealListChecks) {
      // Org registrations (ext/ static init + --org-config) participate.
      vycor::OrgConfig listOrgCfg;
      if (!loadOrgConfigIfSet(AnnealOrgConfig, listOrgCfg))
        return 1;
      auto defaults = vycor::defaultCheckSet();
      llvm::outs() << "anneal checks (docs/checks/<name>.md):\n";
      for (const auto &check : vycor::builtinAnnealChecks()) {
        llvm::outs() << llvm::format("  %-28s %-4s", check.name.c_str(),
                                     defaults.count(check.name) ? "on"
                                                                : "off");
        if (!check.groups.empty()) {
          llvm::outs() << "[";
          for (size_t i = 0; i < check.groups.size(); ++i)
            llvm::outs() << (i ? "," : "") << check.groups[i];
          llvm::outs() << "] ";
        }
        llvm::outs() << check.summary << "\n";
      }
      auto orgNames = vycor::ExtensionRegistry::instance().allCheckNames();
      if (!orgNames.empty()) {
        llvm::outs() << "organization checks (default on):\n";
        for (const auto &name : orgNames)
          llvm::outs() << "  " << name << "\n";
      }
      llvm::outs() << "groups: all";
      std::set<std::string> groupNames;
      for (const auto &check : vycor::builtinAnnealChecks())
        for (const auto &group : check.groups)
          groupNames.insert(group);
      for (const auto &kv :
           vycor::ExtensionRegistry::instance().checkGroups())
        groupNames.insert(kv.first);
      for (const auto &group : groupNames)
        llvm::outs() << ", " << group;
      llvm::outs() << "\n";
      return 0;
    }
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
    vycor::OrgConfig orgCfg;
    if (!loadOrgConfigIfSet(AnnealOrgConfig, orgCfg))
      return 1;

    // ---- named-check selection (--checks / .vycor-anneal.json) -----------
    // Sources in order, later winning: discovered/explicit config file,
    // --checks, then the legacy toggle flags as appended enables.
    std::set<std::string> enabledChecks = vycor::defaultCheckSet();
    {
      std::vector<std::string> spec;
      std::string cfgPath = !AnnealChecksConfig.empty()
                                ? std::string(AnnealChecksConfig)
                                : vycor::findChecksConfig(".");
      if (!cfgPath.empty()) {
        auto buf = llvm::MemoryBuffer::getFile(cfgPath);
        if (!buf) {
          llvm::errs() << "anneal: checks-config: cannot read " << cfgPath
                       << ": " << buf.getError().message() << "\n";
          return 1;
        }
        std::string err;
        if (!vycor::parseChecksConfigJson(
                std::string((*buf)->getBuffer()), spec, err)) {
          llvm::errs() << "anneal: checks-config: " << cfgPath << ": " << err
                       << "\n";
          return 1;
        }
      }
      std::string cli = AnnealChecks;
      size_t pos = 0;
      while (pos != std::string::npos && pos < cli.size()) {
        size_t comma = cli.find(',', pos);
        std::string entry = cli.substr(
            pos, comma == std::string::npos ? std::string::npos : comma - pos);
        while (!entry.empty() && entry.front() == ' ')
          entry.erase(entry.begin());
        while (!entry.empty() && entry.back() == ' ')
          entry.pop_back();
        if (!entry.empty())
          spec.push_back(entry);
        pos = comma == std::string::npos ? std::string::npos : comma + 1;
      }
      if (AnnealCoverageDiag)
        spec.push_back("coverage-properties");
      if (AnnealOdrDiag)
        spec.push_back("odr-violations");
      if (AnnealDeadCode)
        spec.push_back("dead-code");
      std::string err;
      if (!vycor::resolveCheckSpec(spec, enabledChecks, err)) {
        llvm::errs() << "anneal: checks: " << err << "\n";
        return 1;
      }
    }

    vycor::AnalysisOptions opts;
    opts.enableAdlDiag = enabledChecks.count("adl-visibility") > 0;
    opts.enableCtadDiag = enabledChecks.count("ctad-visibility") > 0;
    opts.enableSpecializationDiag =
        enabledChecks.count("specialization-visibility") > 0;
    opts.enableDefaultArgDiag =
        enabledChecks.count("default-arg-divergence") > 0;
    opts.enableStaticInitOrderDiag =
        enabledChecks.count("static-init-order") > 0;
    opts.enableExceptionEscapeDiag =
        enabledChecks.count("exception-escape") > 0;
    opts.enableHeaderStaticDiag =
        enabledChecks.count("header-static-duplication") > 0;
    opts.enableExceptionSpecDiag =
        enabledChecks.count("exception-spec-divergence") > 0;
    opts.enableCoverageDiag = enabledChecks.count("coverage-properties") > 0;
    opts.enableOdrDiag = enabledChecks.count("odr-violations") > 0;
    opts.warnSameScore = AnnealWarnSameScore;
    opts.modelConvertibility = AnnealModelConvertibility;
    // Organization checks not in the enabled set are disabled, on top of
    // the org config's own disable list.
    opts.disabledChecks = orgCfg.disabledAnnealChecks;
    for (const auto &name :
         vycor::ExtensionRegistry::instance().allCheckNames())
      if (!enabledChecks.count(name))
        opts.disabledChecks.push_back(name);
    opts.threadCount = AnnealThreads;
    opts.checkpointPath = AnnealCheckpointFile;

    // ---- worker modes (spawned by an --isolate-workers parent) -----------
    // Single-threaded over the batch so the last WORKER-TU stderr marker is
    // an exact poison identifier; write the shard, exit. Mirrors
    // megascope's --bake-worker.
    if (AnnealIndexWorker || AnnealAnalyzeWorker) {
      if (AnnealWorkerOut.empty()) {
        llvm::errs() << "anneal: worker mode requires --worker-out\n";
        return 1;
      }
      if (AnnealIndexWorker) {
        std::vector<std::pair<std::string, vycor::AnnealIndexPayload>> shards;
        shards.reserve(files.size());
        for (const auto &file : files) {
          llvm::errs() << "WORKER-TU " << file << "\n";
          vycor::GlobalIndex shard;
          auto tool = vycor::makeClangTool(*compDb, {file});
          vycor::IndexerActionFactory factory(
            shard, opts.enableOdrDiag,
            opts.enableStaticInitOrderDiag || opts.enableExceptionEscapeDiag);
          tool.run(&factory);
          shards.emplace_back(file, vycor::AnnealIndexPayload::capture(shard));
        }
        if (!vycor::writeAnnealIndexShard(AnnealWorkerOut, shards)) {
          llvm::errs() << "anneal: worker: cannot write shard to "
                       << AnnealWorkerOut << "\n";
          return 1;
        }
      } else {
        vycor::GlobalIndex indexIn;
        if (AnnealGlobalIndexIn.empty() ||
            !vycor::readGlobalIndexFile(AnnealGlobalIndexIn, indexIn)) {
          llvm::errs() << "anneal: worker: --analyze-worker requires a "
                          "readable --global-index\n";
          return 1;
        }
        std::vector<std::pair<std::string, std::vector<vycor::Diagnostic>>>
            perTu;
        perTu.reserve(files.size());
        for (const auto &file : files) {
          llvm::errs() << "WORKER-TU " << file << "\n";
          std::vector<vycor::Diagnostic> local;
          auto tool = vycor::makeClangTool(*compDb, {file});
          vycor::AnalyzerActionFactory factory(indexIn, local, opts);
          tool.run(&factory);
          perTu.emplace_back(file, std::move(local));
        }
        if (!vycor::writeAnnealDiagShard(AnnealWorkerOut, perTu)) {
          llvm::errs() << "anneal: worker: cannot write shard to "
                       << AnnealWorkerOut << "\n";
          return 1;
        }
      }
      return 0;
    }

    // ---- parent-side worker isolation ------------------------------------
    if (AnnealIsolateWorkers) {
      static int selfExeAnchor; // address anchors getMainExecutable
      std::string selfExe =
          llvm::sys::fs::getMainExecutable(argv[0], &selfExeAnchor);
      opts.workerCount =
          AnnealWorkers ? AnnealWorkers.getValue() : AnnealThreads.getValue();
      // Fully-resolved check set for workers: "-all" first so worker-side
      // defaults and any discovered config file are overridden.
      std::string workerChecks = "-all";
      for (const auto &name : enabledChecks)
        workerChecks += "," + name;
      opts.isolatedRunner = [selfExe, workerChecks](uint8_t phase,
                                      const std::string &globalIndexPath,
                                      const std::vector<std::string> &batch,
                                      const std::string &shardPath,
                                      const std::string &stderrPath) -> int {
        std::vector<std::string> workerArgv;
        workerArgv.reserve(14 + 2 * batch.size());
        workerArgv.push_back(selfExe);
        workerArgv.push_back("anneal");
        workerArgv.push_back(phase == vycor::AnnealCheckpoint::kPhaseIndex
                                 ? "--index-worker"
                                 : "--analyze-worker");
        workerArgv.push_back("--worker-out");
        workerArgv.push_back(shardPath);
        workerArgv.push_back("--build-path");
        workerArgv.push_back(AnnealBuildPath);
        if (!globalIndexPath.empty()) {
          workerArgv.push_back("--global-index");
          workerArgv.push_back(globalIndexPath);
        }
        if (AnnealWarnSameScore)
          workerArgv.push_back("--warn-same-score");
        if (AnnealModelConvertibility)
          workerArgv.push_back("--model-convertibility");
        workerArgv.push_back("--checks=" + workerChecks);
        if (!AnnealOrgConfig.empty()) {
          workerArgv.push_back("--org-config");
          workerArgv.push_back(AnnealOrgConfig);
        }
        for (const auto &a : vycor::globalExtraArgs())
          workerArgv.push_back("--extra-arg=" + a);
        for (const auto &f : batch) {
          workerArgv.push_back("--source");
          workerArgv.push_back(f);
        }

        std::vector<llvm::StringRef> args(workerArgv.begin(),
                                          workerArgv.end());
        // stdin from the null device; stdout joins the stderr log (same
        // rationale as megascope's runner: keep worker output off the
        // parent's stdout).
        std::optional<llvm::StringRef> redirects[3] = {
            llvm::StringRef(""), llvm::StringRef(stderrPath),
            llvm::StringRef(stderrPath)};
        std::string errMsg;
        bool execFailed = false;
        int rc = llvm::sys::ExecuteAndWait(selfExe, args, /*Env=*/std::nullopt,
                                           redirects, /*SecondsToWait=*/0,
                                           /*MemoryLimit=*/0, &errMsg,
                                           &execFailed);
        if (execFailed)
          llvm::errs() << "anneal: worker: failed to spawn " << selfExe
                       << ": " << errMsg << "\n";
        return rc;
      };
    }

    // Keep the merged index alive for graph-backed post passes.
    vycor::GlobalIndex mergedIndex;
    auto diagnostics = vycor::runAnalysis(*compDb, files, opts, &mergedIndex);

    // Graph-backed checks (dead-code, static-init-hazards) share one call
    // graph build.
    const bool wantDeadCode = enabledChecks.count("dead-code") > 0;
    const bool wantInitHazards =
        enabledChecks.count("static-init-hazards") > 0;
    if (wantDeadCode || wantInitHazards) {
      auto graph = vycor::buildCallGraph(*compDb, files);

      if (wantDeadCode) {
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
      if (wantInitHazards)
        vycor::analyzeStaticInitHazards(mergedIndex, graph, diagnostics);
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

  // ---- megascope -------------------------------------------------------------
  if (MegascopeCmd) {
    if (McpBuildPath.empty()) {
      llvm::errs() << "megascope: --build-path is required\n";
      return 1;
    }
    // The serve loop owns stdin; a list piped in (or any /dev/stdin-style
    // alias, pipe, or device) would be drained before the first request
    // could arrive. Only a regular file is safe to read here.
    if (!McpSourceList.empty() && megascopeVerb != MegascopeVerb::Index) {
      llvm::sys::fs::file_status st;
      bool regular = McpSourceList != "-" &&
                     !llvm::sys::fs::status(McpSourceList, st) &&
                     llvm::sys::fs::is_regular_file(st);
      if (!regular) {
        llvm::errs() << "megascope: --source-list must be a regular file "
                        "with the serve verb (stdin, pipes, and devices "
                        "are only available with `megascope index`)\n";
        return 1;
      }
    }
    if (!McpMcp && megascopeVerb != MegascopeVerb::Index) {
      llvm::errs() << "megascope: MCP is the only serve transport\n";
      return 1;
    }

    // The verb forms default the index location to
    // <build-path>/.vycor/megascope.vycs — not $VYCOR_INDEX, which is a
    // query-side convenience and must never become a write target (see
    // resolveIndexPath). The legacy verb-less form keeps --snapshot's
    // opt-in semantics.
    std::string indexPath = McpIndex;
    if (megascopeVerb != MegascopeVerb::Legacy && indexPath.empty())
      indexPath = vycor::defaultIndexPath(McpBuildPath);

    std::string dbError;
    auto compDb = clang::tooling::CompilationDatabase::loadFromDirectory(
        McpBuildPath, dbError);
    if (!compDb) {
      llvm::errs() << "megascope: error loading compilation database from "
                   << McpBuildPath << ": " << dbError << "\n";
      return 1;
    }

    using StatsClock = std::chrono::steady_clock;
    auto msSince = [](StatsClock::time_point t0) {
      return std::chrono::duration<double, std::milli>(StatsClock::now() -
                                                       t0)
          .count();
    };

    // An existing index is loaded before the TU selection: with no
    // selection flag at all its recorded TU set is what gets refreshed,
    // so a bare `serve` never silently widens a narrow index to the whole
    // database (and re-saves the result). Workers never carry an index.
    std::optional<vycor::SnapshotData> snap;
    vycor::SnapshotLoadStats snapLoadStats;
    double snapLoadMs = 0;
    if (!indexPath.empty() && !McpBakeWorker) {
      auto t0 = StatsClock::now();
      // Meta and header counts only: enough for TU selection and the
      // dirty check. The graph is decoded further down only when a warm
      // refresh or serve needs it, so an unchanged `index` never pays
      // for it (5 s on a 938-TU index).
      snap = vycor::SnapshotIO::load(indexPath, &snapLoadStats,
                                     vycor::LoadMode::ReadOnly, 0);
      if (snap)
        snapLoadMs = msSince(t0);
    }

    vycor::SourceSelection selection;
    selection.explicitFiles.assign(McpSourceFiles.begin(),
                                   McpSourceFiles.end());
    selection.listFile = McpSourceList;
    selection.regex = McpSourceRe;
    selection.skipPaths.assign(McpSkipPaths.begin(), McpSkipPaths.end());
    if (snap)
      for (const auto &fs : snap->meta.files)
        selection.recordedFiles.push_back(fs.path);
    vycor::SourceSelectionStats selStats;
    auto selected =
        vycor::selectSources(*compDb, selection, std::cin, &selStats);
    if (!selected) {
      llvm::errs() << "megascope: " << llvm::toString(selected.takeError())
                   << "\n";
      return 1;
    }
    std::vector<std::string> files = std::move(*selected);
    auto describeSelection = [&]() {
      std::string d = std::to_string(files.size()) + " of " +
                      std::to_string(selStats.base) + " TUs selected from " +
                      selStats.baseSource + " (" +
                      std::to_string(selStats.regexDropped) +
                      " dropped by --source-re, " +
                      std::to_string(selStats.skipDropped) +
                      " by --skip-paths";
      if (selStats.dbSkipped)
        d += ", " + std::to_string(selStats.dbSkipped) +
             " database entries skipped: not C/C++ or missing";
      return d + ")";
    };
    if (files.empty()) {
      llvm::errs() << "megascope: no TUs to index — " << describeSelection()
                   << "\n";
      return 1;
    }
    if (llvm::StringRef(selStats.baseSource) == "index") {
      llvm::errs() << "megascope: no --source/--source-list/--source-re — "
                      "refreshing the " << selStats.base
                   << " TUs recorded in " << indexPath
                   << " (pass --source-re . to re-select the whole "
                      "database)\n";
    }
    if (selStats.regexDropped || selStats.skipDropped || selStats.dbSkipped)
      llvm::errs() << "megascope: " << describeSelection() << "\n";
    std::vector<std::string> collapsePaths(McpCollapsePaths.begin(),
                                           McpCollapsePaths.end());

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
    vycor::ChannelTypeConfig channelCfg;
    if (!McpChannelTypesJson.empty() &&
        !parseChannelTypesJson(McpChannelTypesJson, channelCfg)) {
      return 1;
    }
    vycor::OrgConfig orgCfg;
    if (!loadOrgConfigIfSet(McpOrgConfig, orgCfg))
      return 1;
    mergeExtensionConfig(orgCfg, lockCfg, channelCfg, collapsePaths);
    // ---- worker mode (spawned by an --isolate-workers parent) ------------
    // Bake the batch with the existing in-process pipeline (crash guard
    // still enabled — first line of defense stays in-process), write the v5
    // snapshot shard, exit. No server loop, no snapshot warm start, no
    // stats-json. The WORKER-TU stderr marker before each parse is the
    // parent's poison identifier when this process dies.
    if (McpBakeWorker) {
      if (McpWorkerOut.empty()) {
        llvm::errs() << "megascope: --bake-worker requires --worker-out\n";
        return 1;
      }
      auto baked = vycor::bakeIndexes(
          *compDb, files, collapsePaths, McpThreads, pchPtr, sysroot, lockCfg,
          /*stats=*/nullptr,
          [](const std::string &f) {
            llvm::errs() << "WORKER-TU " << f << "\n";
          },
          channelCfg);
      vycor::SnapshotMeta meta;
      meta.collapsePaths = collapsePaths;
      meta.lockAllowlist = lockCfg.userAllowlist;
      meta.lockBuiltins = lockCfg.useBuiltins;
      meta.channelTypes = channelCfg.registeredTypes;
      // The parent takes the batch's dependency lists from the shard meta
      // (bakeIsolatedWithRunner) and re-stamps the TUs itself.
      meta.files = vycor::SnapshotIO::stampFiles(files);
      vycor::SnapshotIO::recordDependencies(meta, baked.deps);
      vycor::SnapshotIO::recordOutcomes(meta, baked.outcomes);
      if (!vycor::SnapshotIO::save(McpWorkerOut, baked.graph, baked.cfIndex,
                                   meta, baked.channels)) {
        llvm::errs() << "megascope: worker: cannot write shard to "
                     << McpWorkerOut << "\n";
        return 1;
      }
      return 0;
    }

    vycor::CallGraph graph;
    vycor::ControlFlowIndex cfIndex;
    vycor::ChannelIndex channels;
    bool needFullBuild = true;

    // Efficiency stats, dumped to --stats-json once the server is ready.
    vycor::BuildStats buildStats;
    // Whether this process changed the indexes relative to the loaded
    // snapshot (full build, or warm-start refresh/drop). An unchanged warm
    // start skips the snapshot re-save — measured 3.1s per start on a
    // 301 MB / 6.37M-call-site snapshot.
    bool indexesChanged = true;
    double snapSaveMs = 0, warmRefreshMs = 0, bakeMs = 0;
    double warmRemoveMs = 0, warmBakeMs = 0, warmAbsorbMs = 0;
    bool snapLoaded = false;
    // An unchanged `index` reports the header counts and never decodes
    // the graph.
    bool graphSkipped = false;
    size_t warmRefreshed = 0, warmDropped = 0, warmViaDeps = 0;
    size_t warmViaInputs = 0, warmRetried = 0, unstableStamps = 0;
    // What the saved (or kept) index covers of the selection.
    vycor::IndexCoverage coverage;

    // Stamps are taken before any parsing: a file modified mid-build gets a
    // stale stamp and is conservatively re-indexed on the next warm start.
    // Dependency stamps (the files each parse opened) come from the
    // frontend itself and are recorded in the meta at save time. The bake
    // start is the reference for the unstable-stamp window
    // (SnapshotIO::markUnstableStamps).
    const uint64_t bakeStartNs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    auto currentStamps = vycor::SnapshotIO::stampFiles(files);
    vycor::TuDependencies deps;
    // Effective inputs, taken with the stamps: the environment once, each
    // TU's compile commands on top (InputFingerprint.h). A TU whose
    // recorded fingerprint differs is dirty like an edited one.
    const auto bakeEnv = vycor::BakeEnvironment::current(sysroot, McpPchDir);
    const std::string envFingerprint = vycor::environmentFingerprint(bakeEnv);
    auto fingerprintStart = StatsClock::now();
    auto currentFingerprints =
        vycor::fingerprintTUs(*compDb, files, envFingerprint);
    const double fingerprintMs = msSince(fingerprintStart);
    // Every requested TU's outcome, from this bake or kept from the index.
    vycor::TuOutcomes outcomes;

    // One bake for the cold build and the warm refresh alike: the
    // in-process parallel pipeline, or subprocess workers under
    // --isolate-workers.
    auto runBake = [&](const std::vector<std::string> &toBake) {
      if (McpIsolateWorkers) {
        unsigned workerCount =
            McpWorkers ? McpWorkers.getValue() : McpThreads.getValue();
        if (workerCount == 0)
          workerCount = std::thread::hardware_concurrency();
        static int selfExeAnchor; // address anchors getMainExecutable
        std::string selfExe =
            llvm::sys::fs::getMainExecutable(argv[0], &selfExeAnchor);
        vycor::McpBakeConfig bakeCfg;
        bakeCfg.buildPath = McpBuildPath;
        bakeCfg.collapsePaths = collapsePaths;
        bakeCfg.extraArgs = vycor::globalExtraArgs();
        bakeCfg.sysroot = sysroot;
        bakeCfg.lockTypes = lockCfg.userAllowlist;
        bakeCfg.channelTypesJson = McpChannelTypesJson;
        bakeCfg.orgConfig = McpOrgConfig;
        return vycor::bakeIsolated(selfExe, bakeCfg, toBake, workerCount,
                                   &buildStats);
      }
      return vycor::bakeIndexes(*compDb, toBake, collapsePaths, McpThreads,
                                pchPtr, sysroot, lockCfg, &buildStats,
                                nullptr, channelCfg);
    };

    if (!indexPath.empty()) {
      if (snap) {
        bool configMatch =
            snap->meta.collapsePaths == collapsePaths &&
            snap->meta.lockAllowlist == lockCfg.userAllowlist &&
            snap->meta.lockBuiltins == lockCfg.useBuiltins &&
            snap->meta.channelTypes == channelCfg.registeredTypes;
        std::unordered_set<std::string> recorded;
        for (const auto &fs : snap->meta.files)
          recorded.insert(fs.path);
        // Dirty = own stamp changed, any file its parse opened did
        // (header dependency stamps), its compile inputs did
        // (fingerprint), or its last parse did not end Indexed (retry).
        // Past half the selection *changed* the cold bake wins: it skips
        // the mutable load and the per-TU removals. Retries do not count
        // toward that: a project whose broken TUs outnumber its healthy
        // ones should re-parse the broken ones, not everything.
        vycor::SnapshotIO::DirtyReport dirtyWhy;
        auto dirtyFlags = vycor::SnapshotIO::dirtyTUs(
            snap->meta, currentStamps, &currentFingerprints, &dirtyWhy);
        const size_t dirty = static_cast<size_t>(
            std::count(dirtyFlags.begin(), dirtyFlags.end(), true));
        const size_t changed = dirty - dirtyWhy.retried;
        size_t dropped = 0;
        {
          std::unordered_set<std::string> selected(files.begin(),
                                                   files.end());
          for (const auto &fs : snap->meta.files)
            if (!selected.count(fs.path))
              ++dropped;
        }
        // Retrying a failed TU costs the full mutable load and save
        // (seconds on a large index), so a refresh that would otherwise
        // touch nothing leaves failed TUs as recorded; they ride along
        // with any refresh that rewrites the index anyway (a changed or
        // dropped TU), or --retry-failed/--force.
        if (changed == 0 && dropped == 0 && dirtyWhy.retried > 0 &&
            !McpRetryFailed && !McpForce) {
          for (size_t i = 0; i < dirtyFlags.size(); ++i)
            if (dirtyWhy.reasons[i] ==
                vycor::SnapshotIO::DirtyReason::Retry)
              dirtyFlags[i] = false;
          llvm::errs() << "megascope: " << dirtyWhy.retried
                       << " TU(s) whose last parse failed are left as "
                          "recorded (pass --retry-failed to re-parse "
                          "them)\n";
          dirtyWhy.retried = 0;
        }
        if (McpForce) {
          llvm::errs() << "megascope: --force — full rebuild\n";
        } else if (!configMatch) {
          llvm::errs() << "megascope: snapshot build configuration differs "
                          "— full rebuild\n";
        } else if (snap->meta.provenance.environment != envFingerprint) {
          // Every TU is dirty through its fingerprint; say why.
          llvm::errs() << "megascope: bake environment differs (index: "
                       << snap->meta.provenance.analyzer << ", "
                       << snap->meta.provenance.toolchain << "; now: "
                       << vycor::analyzerIdentity() << ", "
                       << vycor::toolchainIdentity()
                       << "; or the extra args / sysroot / PCH / GCC "
                          "installation changed) — full rebuild\n";
        } else if (changed * 2 > files.size()) {
          llvm::errs() << "megascope: " << changed << " of " << files.size()
                       << " selected TUs are new or changed — full "
                          "rebuild instead of a warm refresh\n";
          // Say which, and why: an unexpected cold rebuild is usually a
          // stamp the bake did not expect to move (a header a package
          // manager rewrote, a clock, a copied tree).
          size_t listed = 0;
          for (size_t i = 0; i < dirtyFlags.size(); ++i) {
            if (!dirtyFlags[i] ||
                dirtyWhy.reasons[i] == vycor::SnapshotIO::DirtyReason::Retry)
              continue;
            if (++listed > 12) {
              llvm::errs() << "megascope:   ... and " << (changed - 12)
                           << " more\n";
              break;
            }
            llvm::errs() << "megascope:   " << currentStamps[i].path << ": "
                         << dirtyWhy.detail[i] << "\n";
          }
        } else {
          std::set<std::string> current(files.begin(), files.end());
          std::vector<std::string> toDrop, toBake;
          for (const auto &fs : snap->meta.files)
            if (!current.count(fs.path))
              toDrop.push_back(fs.path);
          for (size_t i = 0; i < currentStamps.size(); ++i)
            if (dirtyFlags[i])
              toBake.push_back(currentStamps[i].path);
          needFullBuild = false;
          snapLoaded = true;

          if (toDrop.empty() && toBake.empty() &&
              megascopeVerb == MegascopeVerb::Index &&
              McpDumpNodes.empty()) {
            // Nothing to refresh and nobody to hand the graph to: report
            // the header counts and leave the sections undecoded.
            graphSkipped = true;
            indexesChanged = false;
            llvm::errs() << "megascope: warm start from " << indexPath
                         << " (0 TU(s) re-indexed, 0 dropped, "
                         << snap->summary.nodes << " nodes, "
                         << snap->summary.edges << " edges, "
                         << snap->summary.callSites << " call sites)\n";
          } else {
            auto t0 = StatsClock::now();
            snapLoadStats = vycor::SnapshotLoadStats();
            auto full = vycor::SnapshotIO::load(indexPath, &snapLoadStats,
                                                vycor::LoadMode::Mutable);
            snapLoadMs = msSince(t0);
            if (!full) {
              llvm::errs() << "megascope: cannot decode index " << indexPath
                           << " — full build\n";
              needFullBuild = true;
              snapLoaded = false;
            } else {
              graph = std::move(full->graph);
              cfIndex = std::move(full->cfIndex);
              channels = std::move(full->channels);
              deps = vycor::SnapshotIO::dependenciesOf(snap->meta);
              outcomes = vycor::SnapshotIO::outcomesOf(snap->meta);
              auto refreshStart = StatsClock::now();

              // One batched removal: each hub's adjacency vector is
              // scrubbed once for the whole set (74 TUs on the 938-TU
              // testbed: 17.7 s one at a time).
              std::vector<std::string> toRemove = toDrop;
              for (const auto &path : toBake)
                if (recorded.count(path))
                  toRemove.push_back(path);
              graph.removeTUs(toRemove);
              cfIndex.removeTUs(toRemove);
              channels.removeTUs(toRemove);
              for (const auto &path : toDrop) {
                deps.erase(path);
                outcomes.erase(path);
              }
              for (const auto &path : toBake) {
                deps.erase(path);
                outcomes.erase(path);
              }
              warmRemoveMs = msSince(refreshStart);
              if (!toBake.empty()) {
                // The dirty set takes the same parallel (or isolated)
                // bake as a cold build and is merged with the
                // worker-shard absorb.
                llvm::errs() << "megascope: re-indexing " << toBake.size()
                             << " TU(s): " << dirtyWhy.viaDeps
                             << " for changed headers, " << dirtyWhy.viaInputs
                             << " for changed compile inputs, "
                             << dirtyWhy.retried
                             << " retried after a failed parse...\n";
                if (McpVerbose) {
                  for (size_t i = 0; i < dirtyFlags.size(); ++i)
                    if (dirtyFlags[i])
                      llvm::errs() << "megascope:   " << currentStamps[i].path
                                   << ": " << dirtyWhy.detail[i] << "\n";
                }
                auto bakeStart = StatsClock::now();
                auto fresh = runBake(toBake);
                warmBakeMs = msSince(bakeStart);
                auto absorbStart = StatsClock::now();
                graph.absorb(fresh.graph);
                cfIndex.absorb(fresh.cfIndex);
                channels.absorb(fresh.channels);
                for (auto &kv : fresh.deps)
                  deps[kv.first] = std::move(kv.second);
                for (auto &kv : fresh.outcomes)
                  outcomes[kv.first] = std::move(kv.second);
                warmAbsorbMs = msSince(absorbStart);
              }
              warmRefreshMs = msSince(refreshStart);
              warmRefreshed = toBake.size();
              warmViaDeps = dirtyWhy.viaDeps;
              warmViaInputs = dirtyWhy.viaInputs;
              warmRetried = dirtyWhy.retried;
              warmDropped = toDrop.size();
              indexesChanged = !toBake.empty() || !toDrop.empty();
              llvm::errs() << "megascope: warm start from " << indexPath
                           << " (" << toBake.size() << " TU(s) re-indexed, "
                           << toDrop.size() << " dropped, "
                           << graph.nodeCount() << " nodes, "
                           << graph.edgeCount() << " edges, "
                           << cfIndex.size() << " call sites)\n";
            }
          }
        }
      } else if (llvm::sys::fs::exists(indexPath)) {
        llvm::errs() << "megascope: cannot load index " << indexPath
                     << " (wrong format version or unreadable) — full "
                        "build\n";
      } else {
        llvm::errs() << "megascope: no index yet at " << indexPath
                     << " — full build\n";
      }
    }

    if (needFullBuild) {
      llvm::errs() << "megascope: baking call graph + control flow index ("
                   << files.size() << " files, "
                   << McpThreads << " threads)...\n";
      auto bakeStart = StatsClock::now();
      vycor::BakedIndexes baked = runBake(files);
      bakeMs = msSince(bakeStart);
      graph = std::move(baked.graph);
      cfIndex = std::move(baked.cfIndex);
      channels = std::move(baked.channels);
      deps = std::move(baked.deps);
      outcomes = std::move(baked.outcomes);
      llvm::errs() << "megascope: indexes built ("
                   << graph.nodeCount() << " nodes, "
                   << graph.edgeCount() << " edges, "
                   << cfIndex.size() << " call sites)\n";
    }

    // Counts for the reports below: the header's when the graph was never
    // decoded, else the live indexes'.
    uint64_t liveNodes = graphSkipped ? snap->summary.nodes
                                      : graph.nodeCount();
    uint64_t liveEdges = graphSkipped ? snap->summary.edges
                                      : graph.edgeCount();
    uint64_t liveCallSites = graphSkipped ? snap->summary.callSites
                                          : cfIndex.size();
    uint64_t liveChannelSites = graphSkipped ? snap->summary.channelSites
                                             : channels.size();

    bool saveFailed = false;
    // What every served payload says about these indexes
    // (docs/result-contract.md): the bake that wrote or kept the index,
    // and its coverage. Baked here, so `baked`.
    vycor::IndexFacts serveFacts;
    if (!indexPath.empty() && !indexesChanged) {
      llvm::errs() << "megascope: index unchanged — skipping re-save\n";
      // Nothing re-indexed: the coverage is what the index records
      // (failed TUs left as recorded included).
      coverage = vycor::coverageOf(snap->meta);
      serveFacts =
          vycor::IndexFacts::of(snap->meta, vycor::IndexFreshness::Baked);
    } else if (!indexPath.empty()) {
      vycor::SnapshotMeta meta;
      meta.collapsePaths = collapsePaths;
      meta.lockAllowlist = lockCfg.userAllowlist;
      meta.lockBuiltins = lockCfg.useBuiltins;
      meta.channelTypes = channelCfg.registeredTypes;
      meta.files = std::move(currentStamps);
      meta.fingerprints = std::move(currentFingerprints);
      vycor::SnapshotIO::recordDependencies(meta, deps);
      vycor::SnapshotIO::recordOutcomes(meta, outcomes);
      meta.entryPoints.assign(McpEntryPoints.begin(), McpEntryPoints.end());
      meta.provenance.analyzer = vycor::analyzerIdentity();
      meta.provenance.toolchain = vycor::toolchainIdentity();
      meta.provenance.environment = envFingerprint;
      meta.provenance.bakeStartNs = bakeStartNs;
      unstableStamps =
          vycor::SnapshotIO::markUnstableStamps(meta, bakeStartNs);
      coverage = vycor::coverageOf(meta);
      serveFacts = vycor::IndexFacts::of(meta, vycor::IndexFreshness::Baked);
      if (!coverage.complete())
        llvm::errs() << "megascope: WARNING: " << coverage.indexed << " of "
                     << coverage.requested << " TU(s) indexed cleanly ("
                     << coverage.partial << " partial, " << coverage.failed
                     << " failed); the rest are retried with the next "
                        "refresh that rewrites the index, or "
                        "--retry-failed\n";
      auto snapSaveStart = StatsClock::now();
      if (vycor::SnapshotIO::save(indexPath, graph, cfIndex, meta,
                                  channels)) {
        snapSaveMs = msSince(snapSaveStart);
        llvm::errs() << "megascope: index saved to " << indexPath << "\n";
      } else {
        saveFailed = true;
        llvm::errs() << "megascope: WARNING: could not save index to "
                     << indexPath << "\n";
      }
    } else {
      // No index file: the coverage of this in-memory bake, with no
      // saved bake to cite.
      vycor::SnapshotMeta meta;
      meta.channelTypes = channelCfg.registeredTypes;
      meta.files = currentStamps;
      vycor::SnapshotIO::recordOutcomes(meta, outcomes);
      coverage = vycor::coverageOf(meta);
      serveFacts = vycor::IndexFacts::of(meta, vycor::IndexFreshness::Baked);
    }

    if (!McpStatsJson.empty()) {
      llvm::json::Object root;
      root["mode"] = needFullBuild ? "cold" : "warm";
      root["files"] = static_cast<int64_t>(files.size());
      root["threads"] = static_cast<int64_t>(McpThreads);
      root["bake_wall_ms"] = bakeMs;
      root["phase1_wall_ms"] = buildStats.phase1WallMs;
      root["phase2_wall_ms"] = buildStats.phase2WallMs;
      root["parse_errors"] = static_cast<int64_t>(buildStats.parseErrorCount());
      root["crashes"] = static_cast<int64_t>(buildStats.crashCount());

      llvm::json::Object snap;
      snap["loaded"] = snapLoaded;
      snap["load_ms"] = snapLoadMs;
      snap["save_ms"] = snapSaveMs;
      snap["warm_refresh_ms"] = warmRefreshMs;
      snap["refreshed_tus"] = static_cast<int64_t>(warmRefreshed);
      snap["refreshed_for_headers"] = static_cast<int64_t>(warmViaDeps);
      snap["refreshed_for_inputs"] = static_cast<int64_t>(warmViaInputs);
      snap["retried"] = static_cast<int64_t>(warmRetried);
      snap["fingerprint_ms"] = fingerprintMs;
      snap["unstable_stamps"] = static_cast<int64_t>(unstableStamps);
      snap["dropped_tus"] = static_cast<int64_t>(warmDropped);
      snap["warm_remove_ms"] = warmRemoveMs;
      snap["warm_bake_ms"] = warmBakeMs;
      snap["warm_absorb_ms"] = warmAbsorbMs;
      snap["graph_skipped"] = graphSkipped;
      snap["load_sections"] = loadSectionsJson(snapLoadStats);
      root["snapshot"] = std::move(snap);
      llvm::json::Object cov;
      cov["requested"] = static_cast<int64_t>(coverage.requested);
      cov["indexed"] = static_cast<int64_t>(coverage.indexed);
      cov["partial"] = static_cast<int64_t>(coverage.partial);
      cov["failed"] = static_cast<int64_t>(coverage.failed);
      root["coverage"] = std::move(cov);

      llvm::json::Object g;
      g["nodes"] = static_cast<int64_t>(liveNodes);
      g["edges"] = static_cast<int64_t>(liveEdges);
      g["call_sites"] = static_cast<int64_t>(liveCallSites);
      if (!graphSkipped) {
        g["interner_strings"] =
            static_cast<int64_t>(graph.interner().size());
        g["interner_payload_bytes"] =
            static_cast<int64_t>(graph.interner().payloadBytes());
      }
      root["graph"] = std::move(g);

#if defined(__unix__) || defined(__APPLE__)
      struct rusage ru;
      if (getrusage(RUSAGE_SELF, &ru) == 0) {
        // ru_maxrss is KB on Linux, bytes on macOS.
#if defined(__APPLE__)
        root["peak_rss_kb"] = static_cast<int64_t>(ru.ru_maxrss / 1024);
#else
        root["peak_rss_kb"] = static_cast<int64_t>(ru.ru_maxrss);
#endif
      }
#endif

      llvm::json::Array tus;
      for (const auto &t : buildStats.tuStats) {
        llvm::json::Object o;
        o["file"] = t.file;
        o["phase"] = static_cast<int64_t>(t.phase);
        o["ms"] = t.ms;
        o["status"] = static_cast<int64_t>(t.toolStatus);
        tus.push_back(std::move(o));
      }
      root["tu"] = std::move(tus);

      std::error_code ec;
      llvm::raw_fd_ostream out(McpStatsJson, ec);
      if (ec) {
        llvm::errs() << "megascope: WARNING: cannot write stats to "
                     << McpStatsJson << ": " << ec.message() << "\n";
      } else {
        out << llvm::json::Value(std::move(root)) << "\n";
        llvm::errs() << "megascope: stats written to " << McpStatsJson
                     << "\n";
      }
    }

    if (!McpDumpNodes.empty()) {
      std::error_code ec;
      llvm::raw_fd_ostream out(McpDumpNodes, ec);
      if (ec) {
        llvm::errs() << "megascope: WARNING: cannot write node dump to "
                     << McpDumpNodes << ": " << ec.message() << "\n";
      } else {
        // Tabs/newlines cannot appear in USRs or qualified names; no
        // escaping needed. Caller USRs come from the same materialized
        // edge set callersOf serves to queries (virtual dispatch and
        // function-return joins included), so offline analysis of a
        // collapse policy sees query-level precision, not stored edges.
        for (const vycor::CallGraphNode *node : graph.allNodes()) {
          out << node->usr << '\t' << node->qualifiedName << '\t'
              << node->file << '\t' << node->line << '\t';
          bool first = true;
          for (const auto &edge : graph.callersOf(node->usr)) {
            if (!first)
              out << ',';
            out << edge.callerUsr;
            first = false;
          }
          out << '\n';
        }
        llvm::errs() << "megascope: node dump (" << graph.nodeCount()
                     << " nodes) written to " << McpDumpNodes << "\n";
      }
    }

    if (megascopeVerb == MegascopeVerb::Index) {
      // The index file is the product: failing to write it is the error.
      if (saveFailed)
        return 1;
      llvm::json::Object summary;
      summary["index"] = indexPath;
      summary["mode"] = needFullBuild ? "cold" : "warm";
      summary["files"] = static_cast<int64_t>(files.size());
      summary["refreshed"] = static_cast<int64_t>(warmRefreshed);
      summary["refreshed_for_headers"] = static_cast<int64_t>(warmViaDeps);
      summary["refreshed_for_inputs"] = static_cast<int64_t>(warmViaInputs);
      summary["retried"] = static_cast<int64_t>(warmRetried);
      summary["dropped"] = static_cast<int64_t>(warmDropped);
      summary["indexed"] = static_cast<int64_t>(coverage.indexed);
      summary["partial"] = static_cast<int64_t>(coverage.partial);
      summary["failed"] = static_cast<int64_t>(coverage.failed);
      summary["nodes"] = static_cast<int64_t>(liveNodes);
      summary["edges"] = static_cast<int64_t>(liveEdges);
      summary["call_sites"] = static_cast<int64_t>(liveCallSites);
      summary["channel_sites"] = static_cast<int64_t>(liveChannelSites);
      llvm::outs() << llvm::json::Value(std::move(summary)) << "\n";
      return 0;
    }

    // --entry-point, else the roots recorded in the index (v8 meta), else
    // main — the same resolution the query verbs use.
    std::vector<std::string> entryPoints(McpEntryPoints.begin(),
                                         McpEntryPoints.end());
    if (entryPoints.empty() && snap)
      entryPoints = snap->meta.entryPoints;
    if (entryPoints.empty())
      entryPoints.push_back("main");

    vycor::McpBuildParams buildParams;
    buildParams.compDb = std::shared_ptr<clang::tooling::CompilationDatabase>(
        std::move(compDb));
    buildParams.collapsePaths = collapsePaths;
    buildParams.pchCache = pchPtr;
    buildParams.sysroot = sysroot;
    buildParams.lockCfg = std::move(lockCfg);
    buildParams.channelCfg = std::move(channelCfg);

    vycor::McpServer server(std::move(graph), std::move(cfIndex),
                                 std::move(channels), std::move(entryPoints),
                                 std::move(buildParams));
    server.setVerbose(McpVerbose);
    server.setIndexFacts(std::move(serveFacts));
    return server.run();
  }

  llvm::errs() << "No subcommand specified. Use 'anneal', 'morph', "
                  "or 'megascope'.\n"
               << "Run with --help for usage information.\n";
  return 1;
}
