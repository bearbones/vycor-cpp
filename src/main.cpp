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
#include "vycor/callgraph/ControlFlowOracle.h"
#include "vycor/ext/Extensions.h"
#include "vycor/ext/OrgConfig.h"
#include "vycor/morph/RulesParser.h"
#include "vycor/morph/TransformPipeline.h"
#include "vycor/callgraph/CollapseFilter.h"
#include "vycor/callgraph/Snapshot.h"
#include "vycor/callgraph/WorkerPool.h"
#include "vycor/compat/PchCache.h"
#include "vycor/mcp/McpServer.h"
#include "vycor/Version.h"

#include "clang/Tooling/CompilationDatabase.h"

#include <algorithm>
#include <chrono>
#include <optional>
#include <set>
#include <thread>
#include <unordered_map>
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"
#include "vycor/compat/ToolAdjusters.h"

#if defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#endif

// ---------------------------------------------------------------------------
// --channel-types-json parsing
// ---------------------------------------------------------------------------

// Parses a JSON array of channel type registrations:
//   [{"type": "Queue", "produce": ["push"], "consume": ["pop"],
//     "category": "queue"}, ...]
// "type" must be the canonical type name WITHOUT the struct/class keyword
// (see ChannelIndex.h). Returns false (and prints a diagnostic) on any
// malformed entry; the caller should treat that as a fatal CLI error, same
// as a bad --rules-json would be for morph.
static bool parseChannelTypesJson(const std::string &path,
                                  vycor::ChannelTypeConfig &outCfg) {
  auto bufOrErr = llvm::MemoryBuffer::getFile(path);
  if (!bufOrErr) {
    llvm::errs() << "channel-types-json: cannot read " << path << ": "
                 << bufOrErr.getError().message() << "\n";
    return false;
  }
  auto jsonOrErr = llvm::json::parse(bufOrErr.get()->getBuffer());
  if (!jsonOrErr) {
    llvm::errs() << "channel-types-json: parse error in " << path << ": "
                 << llvm::toString(jsonOrErr.takeError()) << "\n";
    return false;
  }
  auto *arr = jsonOrErr->getAsArray();
  if (!arr) {
    llvm::errs() << "channel-types-json: " << path
                 << " must contain a top-level JSON array\n";
    return false;
  }
  for (const auto &entry : *arr) {
    auto *obj = entry.getAsObject();
    if (!obj) {
      llvm::errs() << "channel-types-json: each entry must be an object\n";
      return false;
    }
    vycor::ChannelTypeSpec spec;
    if (auto type = obj->getString("type")) {
      spec.qualifiedTypeName = type->str();
    } else {
      llvm::errs() << "channel-types-json: entry missing required 'type'\n";
      return false;
    }
    if (auto *produce = obj->getArray("produce"))
      for (const auto &m : *produce)
        if (auto s = m.getAsString())
          spec.produceMethods.push_back(s->str());
    if (auto *consume = obj->getArray("consume"))
      for (const auto &m : *consume)
        if (auto s = m.getAsString())
          spec.consumeMethods.push_back(s->str());
    if (auto category = obj->getString("category"))
      spec.category = category->str();
    outCfg.registeredTypes.push_back(std::move(spec));
  }
  return true;
}

// ---------------------------------------------------------------------------
// --org-config loading and merging (see docs/EXTENDING.md)
// ---------------------------------------------------------------------------

// Loads --org-config when set and installs its hook-shaped parts (feature
// flag patterns, lock/channel types) into ExtensionRegistry. Compiled ext/
// registrars have already run by this point (static init), so after this
// call the registry holds both sources. Returns false (with a diagnostic)
// on unreadable/malformed config.
static bool loadOrgConfigIfSet(const std::string &path,
                               vycor::OrgConfig &out) {
  if (path.empty())
    return true;
  std::string err;
  if (!vycor::loadOrgConfigFile(path, out, err) ||
      !vycor::applyOrgConfig(out, err)) {
    llvm::errs() << "org-config: " << err << "\n";
    return false;
  }
  return true;
}

// Merges registry-held lock/channel types (compiled ext/ registrars plus
// --org-config) into the CLI-built configs, and org collapse paths into
// collapsePaths. CLI entries keep their position and duplicates are
// dropped: the merged lists land in snapshot meta (config-match check), so
// the result must be deterministic and must equal the plain CLI lists when
// no extensions are registered.
static void mergeExtensionConfig(const vycor::OrgConfig &orgCfg,
                                 vycor::LockTypeConfig &lockCfg,
                                 vycor::ChannelTypeConfig &channelCfg,
                                 std::vector<std::string> &collapsePaths) {
  const auto &registry = vycor::ExtensionRegistry::instance();
  for (const auto &name : registry.lockTypes())
    if (std::find(lockCfg.userAllowlist.begin(), lockCfg.userAllowlist.end(),
                  name) == lockCfg.userAllowlist.end())
      lockCfg.userAllowlist.push_back(name);
  for (const auto &spec : registry.channelTypes())
    if (std::find(channelCfg.registeredTypes.begin(),
                  channelCfg.registeredTypes.end(),
                  spec) == channelCfg.registeredTypes.end())
      channelCfg.registeredTypes.push_back(spec);
  for (const auto &pattern : orgCfg.collapsePaths)
    if (std::find(collapsePaths.begin(), collapsePaths.end(), pattern) ==
        collapsePaths.end())
      collapsePaths.push_back(pattern);
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
    PrismCmd("prism",
               "Query control flow and exception handling context");

static llvm::cl::SubCommand
    MegascopeCmd("megascope",
                "Start MCP server for interactive call graph queries");

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
                      llvm::cl::OneOrMore,
                      llvm::cl::sub(AnnealCmd));

static llvm::cl::opt<bool>
    AnnealCoverageDiag("coverage-diag",
                       llvm::cl::desc("Enable coverage instrumentation diagnostics"),
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

static llvm::cl::opt<std::string>
    PrismChannelTypesJson("channel-types-json",
        llvm::cl::desc("JSON file registering channel/queue types to trace "
                       "producer/consumer call sites for (see "
                       "ChannelIndex.h for the schema). --mode dump "
                       "includes channel records when set."),
        llvm::cl::value_desc("file"),
        llvm::cl::sub(PrismCmd));

static llvm::cl::opt<std::string>
    PrismOrgConfig("org-config",
        llvm::cl::desc("Organization config JSON (lock/channel types, "
                       "feature-flag patterns, collapse paths — see "
                       "docs/EXTENDING.md). Merged with the equivalent "
                       "CLI flags."),
        llvm::cl::value_desc("file"),
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
    McpChannelTypesJson("channel-types-json",
        llvm::cl::desc("JSON file registering channel/queue types to trace "
                       "producer/consumer call sites for (see "
                       "ChannelIndex.h for the schema). Only populated on a "
                       "fresh full build — not supported yet with "
                       "--snapshot warm start or --isolate-workers."),
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
    McpSnapshot("snapshot",
        llvm::cl::desc("Snapshot file for warm starts: load the baked graph "
                       "if present (reindexing only changed TUs), and save "
                       "after building"),
        llvm::cl::value_desc("file"),
        llvm::cl::sub(MegascopeCmd));

static llvm::cl::opt<bool>
    McpIsolateWorkers("isolate-workers",
        llvm::cl::desc("Bake the indexes in subprocess workers (a crashing "
                       "TU costs only that TU; parent RSS stays bounded)"),
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
      "  prism    Query control flow and exception handling context\n"
      "  megascope  Start MCP server for interactive call graph queries\n");

  vycor::appendGlobalExtraArgs({ExtraArgs.begin(), ExtraArgs.end()});

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
          vycor::IndexerActionFactory factory(shard, opts.enableOdrDiag);
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

    auto diagnostics = vycor::runAnalysis(*compDb, files, opts);

    // Dead code analysis.
    if (enabledChecks.count("dead-code")) {
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
    vycor::ChannelTypeConfig channelCfg;
    if (!PrismChannelTypesJson.empty() &&
        !parseChannelTypesJson(PrismChannelTypesJson, channelCfg)) {
      return 1;
    }
    vycor::OrgConfig orgCfg;
    if (!loadOrgConfigIfSet(PrismOrgConfig, orgCfg))
      return 1;
    mergeExtensionConfig(orgCfg, lockCfg, channelCfg, collapsePaths);
    auto baked = vycor::bakeIndexes(*compDb, files, collapsePaths,
                                    PrismThreads, pchPtr, sysroot, lockCfg,
                                    nullptr, nullptr, channelCfg);
    auto graph = std::move(baked.graph);
    auto cfIndex = std::move(baked.cfIndex);
    auto channels = std::move(baked.channels);

    // Dump mode: serialize the full index as JSON.
    if (PrismModeOpt == PrismDump) {
      llvm::outs() << vycor::ControlFlowOracle::dumpIndexToJson(cfIndex);
      if (!channelCfg.registeredTypes.empty()) {
        llvm::json::Array sites;
        for (const auto &s : channels.allSites()) {
          llvm::json::Array guards;
          for (const auto &g : s.enclosingGuards) {
            llvm::json::Object guardObj{
                {"conditionText", g.conditionText},
                {"location", g.location},
                {"inTrueBranch", g.inTrueBranch},
                {"isAssertion", g.isAssertion}};
            // Organization guard classifiers (feature flags etc.).
            if (auto ann = vycor::classifyGuard(g))
              guardObj["annotation"] = llvm::json::Object{
                  {"kind", ann->kind}, {"name", ann->name}};
            guards.push_back(std::move(guardObj));
          }
          sites.push_back(llvm::json::Object{
              {"channelId", s.channelId},
              {"channelType", s.channelTypeName},
              {"category", s.category},
              {"operation",
               s.op == vycor::ChannelOperation::Produce ? "produce"
                                                        : "consume"},
              {"function", s.siteFunctionDisplay},
              {"functionUsr", s.siteFunctionUsr},
              {"callSite", s.callSite},
              {"guards", std::move(guards)}});
        }
        llvm::outs() << llvm::json::Value(llvm::json::Object{
            {"channelSiteCount", static_cast<int64_t>(sites.size())},
            {"channelSites", std::move(sites)}})
                     << "\n";
      }
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
    vycor::ChannelTypeConfig channelCfg;
    if (!McpChannelTypesJson.empty() &&
        !parseChannelTypesJson(McpChannelTypesJson, channelCfg)) {
      return 1;
    }
    vycor::OrgConfig orgCfg;
    if (!loadOrgConfigIfSet(McpOrgConfig, orgCfg))
      return 1;
    mergeExtensionConfig(orgCfg, lockCfg, channelCfg, collapsePaths);
    if (!channelCfg.registeredTypes.empty() && McpIsolateWorkers) {
      llvm::errs()
          << "megascope: WARNING: --channel-types-json is not yet supported "
             "with --isolate-workers — channel tracking will be empty "
             "unless this run does a fresh non-isolated full build\n";
    }

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
          /*stats=*/nullptr, [](const std::string &f) {
            llvm::errs() << "WORKER-TU " << f << "\n";
          });
      vycor::SnapshotMeta meta;
      meta.collapsePaths = collapsePaths;
      meta.lockAllowlist = lockCfg.userAllowlist;
      meta.lockBuiltins = lockCfg.useBuiltins;
      // meta.files stays empty: the parent ignores shard meta except as a
      // config sanity check.
      if (!vycor::SnapshotIO::save(McpWorkerOut, baked.graph, baked.cfIndex,
                                   meta)) {
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
    using StatsClock = std::chrono::steady_clock;
    auto msSince = [](StatsClock::time_point t0) {
      return std::chrono::duration<double, std::milli>(StatsClock::now() -
                                                       t0)
          .count();
    };
    double snapLoadMs = 0, snapSaveMs = 0, warmRefreshMs = 0, bakeMs = 0;
    bool snapLoaded = false;
    size_t warmRefreshed = 0, warmDropped = 0;

    // Stamps are taken before any parsing: a file modified mid-build gets a
    // stale stamp and is conservatively re-indexed on the next warm start.
    auto currentStamps = vycor::SnapshotIO::stampFiles(files);

    auto snapLoadStart = StatsClock::now();
    if (!McpSnapshot.empty()) {
      if (auto snap = vycor::SnapshotIO::load(McpSnapshot)) {
        snapLoadMs = msSince(snapLoadStart);
        bool configMatch =
            snap->meta.collapsePaths == collapsePaths &&
            snap->meta.lockAllowlist == lockCfg.userAllowlist &&
            snap->meta.lockBuiltins == lockCfg.useBuiltins &&
            snap->meta.channelTypes == channelCfg.registeredTypes;
        if (!configMatch) {
          llvm::errs() << "megascope: snapshot build configuration differs "
                          "— full rebuild\n";
        } else {
          graph = std::move(snap->graph);
          cfIndex = std::move(snap->cfIndex);
          channels = std::move(snap->channels);
          needFullBuild = false;
          snapLoaded = true;
          auto refreshStart = StatsClock::now();

          std::unordered_map<std::string, const vycor::FileStamp *> baked;
          for (const auto &fs : snap->meta.files)
            baked[fs.path] = &fs;
          std::set<std::string> current(files.begin(), files.end());

          size_t dropped = 0, refreshed = 0;
          for (const auto &fs : snap->meta.files) {
            if (!current.count(fs.path)) {
              graph.removeTU(fs.path);
              cfIndex.removeTU(fs.path);
              channels.removeTU(fs.path);
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
              channels.removeTU(stamp.path);
            }
            vycor::bakeTU(graph, cfIndex, *compDb, stamp.path,
                          collapsePaths, pchPtr, sysroot, lockCfg,
                          channelCfg, &channels);
            ++refreshed;
          }
          warmRefreshMs = msSince(refreshStart);
          warmRefreshed = refreshed;
          warmDropped = dropped;
          indexesChanged = refreshed > 0 || dropped > 0;
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
      auto bakeStart = StatsClock::now();
      vycor::BakedIndexes baked;
      if (McpIsolateWorkers) {
        // Full builds run in subprocess workers; the warm-start dirty-TU
        // refresh above stays in-process (bakeTU) regardless of the flag.
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
        baked = vycor::bakeIsolated(selfExe, bakeCfg, files, workerCount,
                                    &buildStats);
      } else {
        baked = vycor::bakeIndexes(*compDb, files, collapsePaths, McpThreads,
                                   pchPtr, sysroot, lockCfg, &buildStats,
                                   nullptr, channelCfg);
      }
      bakeMs = msSince(bakeStart);
      graph = std::move(baked.graph);
      cfIndex = std::move(baked.cfIndex);
      // Empty when this build went through bakeIsolated (--isolate-workers
      // doesn't thread channelCfg through the shard/worker protocol yet —
      // see the warning printed above).
      channels = std::move(baked.channels);
      llvm::errs() << "megascope: indexes built ("
                   << graph.nodeCount() << " nodes, "
                   << graph.edgeCount() << " edges, "
                   << cfIndex.size() << " call sites)\n";
    }

    if (!McpSnapshot.empty() && !indexesChanged) {
      llvm::errs() << "megascope: snapshot unchanged — skipping re-save\n";
    } else if (!McpSnapshot.empty()) {
      vycor::SnapshotMeta meta;
      meta.collapsePaths = collapsePaths;
      meta.lockAllowlist = lockCfg.userAllowlist;
      meta.lockBuiltins = lockCfg.useBuiltins;
      meta.channelTypes = channelCfg.registeredTypes;
      meta.files = std::move(currentStamps);
      auto snapSaveStart = StatsClock::now();
      if (vycor::SnapshotIO::save(McpSnapshot, graph, cfIndex, meta,
                                  channels)) {
        snapSaveMs = msSince(snapSaveStart);
        llvm::errs() << "megascope: snapshot saved to " << McpSnapshot << "\n";
      } else {
        llvm::errs() << "megascope: WARNING: could not save snapshot to "
                     << McpSnapshot << "\n";
      }
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
      snap["dropped_tus"] = static_cast<int64_t>(warmDropped);
      root["snapshot"] = std::move(snap);

      llvm::json::Object g;
      g["nodes"] = static_cast<int64_t>(graph.nodeCount());
      g["edges"] = static_cast<int64_t>(graph.edgeCount());
      g["call_sites"] = static_cast<int64_t>(cfIndex.size());
      g["interner_strings"] = static_cast<int64_t>(graph.interner().size());
      g["interner_payload_bytes"] =
          static_cast<int64_t>(graph.interner().payloadBytes());
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
    buildParams.channelCfg = std::move(channelCfg);

    vycor::McpServer server(std::move(graph), std::move(cfIndex),
                                 std::move(channels), std::move(entryPoints),
                                 std::move(buildParams));
    return server.run();
  }

  llvm::errs() << "No subcommand specified. Use 'anneal', 'morph', "
                  "'prism', or 'megascope'.\n"
               << "Run with --help for usage information.\n";
  return 1;
}
