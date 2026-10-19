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

#include "vycor/check/Analyzer.h"
#include "vycor/check/CheckSet.h"
#include "vycor/check/Checkpoint.h"
#include "vycor/check/DeadCodeAnalyzer.h"
#include "vycor/check/Indexer.h"
#include "vycor/check/Report.h"
#include "vycor/callgraph/BuildStats.h"
#include "vycor/callgraph/CallGraphBuilder.h"
#include "vycor/callgraph/ControlFlowIndex.h"
#include "vycor/ext/Extensions.h"
#include "vycor/ext/OrgConfig.h"
#include "vycor/morph/RulesParser.h"
#include "vycor/morph/TransformPipeline.h"
#include "vycor/callgraph/CollapseFilter.h"
#include "vycor/callgraph/InputFingerprint.h"
#include "vycor/callgraph/AtomicFile.h"
#include "vycor/callgraph/CrashGuard.h"
#include "vycor/callgraph/Interrupt.h"
#include "vycor/callgraph/MegascopeBridge.h"
#include "vycor/callgraph/Snapshot.h"
#include "vycor/callgraph/Utf8.h"
#include "vycor/callgraph/WorkerPool.h"
#include "vycor/cli/BakeConfig.h"
#include "vycor/cli/MegascopeCli.h"
#include "vycor/cli/SourceSelection.h"
#include "vycor/compat/PchCache.h"
#include "vycor/impact/PatchMapping.h"
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
    CheckCmd("check",
              "Analyze sources for fragile ADL/CTAD resolutions");

static llvm::cl::SubCommand
    MorphCmd("morph",
              "Apply AST-based source transformations");

static llvm::cl::SubCommand
    MegascopeCmd("megascope",
                 "Index a project's call graph and query it (verbs: index, "
                 "merge, tools, info, batch, dump, diff, <tool>; "
                 "`megascope help`)");

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
// check options
// ---------------------------------------------------------------------------

static llvm::cl::opt<std::string>
    CheckBuildPath("build-path",
                    llvm::cl::desc("Directory containing compile_commands.json "
                                   "(or path to a compilation database file; "
                                   "optional when compile flags follow '--')"),
                    llvm::cl::value_desc("dir|file"),
                    llvm::cl::sub(CheckCmd));

static llvm::cl::list<std::string>
    CheckSourceFiles("source",
                      llvm::cl::desc("Source files to analyze"),
                      llvm::cl::value_desc("file"),
                      // Required-ness enforced manually in main(): parse-time
                      // OneOrMore would break --list-checks.
                      llvm::cl::ZeroOrMore,
                      llvm::cl::sub(CheckCmd));

static llvm::cl::opt<bool>
    CheckCoverageDiag("coverage-diag",
                       llvm::cl::desc("Enable coverage instrumentation diagnostics"),
                       llvm::cl::sub(CheckCmd));

static llvm::cl::opt<bool>
    CheckListChecks("list-checks",
        llvm::cl::desc("Print every known check (built-in and organization) "
                       "with defaults, groups, and summaries, then exit"),
        llvm::cl::sub(CheckCmd));

static llvm::cl::opt<std::string>
    CheckSpec("checks",
        llvm::cl::desc("Comma-separated check specification applied after "
                       "any .vycor-check.json: names enable, -names "
                       "disable, group names (e.g. all, noisy, "
                       "compute-heavy) expand. See docs/checks/README.md."),
        llvm::cl::value_desc("spec"),
        llvm::cl::sub(CheckCmd));

static llvm::cl::opt<std::string>
    CheckConfig("checks-config",
        llvm::cl::desc("Explicit checks-config file (default: search for "
                       ".vycor-check.json from the working directory "
                       "upward)"),
        llvm::cl::value_desc("file"),
        llvm::cl::sub(CheckCmd));

static llvm::cl::opt<bool>
    CheckOdrDiag("odr-diag",
        llvm::cl::desc("Detect ODR violations among vague-linkage "
                       "definitions (inline functions, in-class method "
                       "bodies, class definitions) across TUs — the class "
                       "of mismatch linkers merge silently instead of "
                       "diagnosing"),
        llvm::cl::sub(CheckCmd));

static llvm::cl::opt<bool>
    CheckDeadCode("dead-code",
                   llvm::cl::desc("Enable dead code analysis via call graph"),
                   llvm::cl::sub(CheckCmd));

static llvm::cl::list<std::string>
    CheckEntryPoints("entry-point",
                      llvm::cl::desc("Entry point function names (default: main)"),
                      llvm::cl::value_desc("name"),
                      llvm::cl::sub(CheckCmd));

static llvm::cl::opt<bool>
    CheckWarnSameScore("warn-same-score",
                        llvm::cl::desc("Warn on ADL candidates that tie the "
                                       "resolved overload on every argument "
                                       "position"),
                        llvm::cl::sub(CheckCmd));

static llvm::cl::opt<bool>
    MegascopeNoWait("no-wait",
        llvm::cl::desc("Fail at once when another index process "
                       "holds the index's write lock (<index>.lock) "
                       "instead of waiting for it"),
        llvm::cl::init(false),
        llvm::cl::sub(MegascopeCmd));

static llvm::cl::opt<unsigned>
    CheckThreads("threads",
        llvm::cl::desc("Number of threads for the per-TU analysis phases "
                       "(0 = hardware_concurrency, 1 = serial)"),
        llvm::cl::init(0),
        llvm::cl::sub(CheckCmd));

static llvm::cl::opt<bool>
    CheckIsolateWorkers("isolate-workers",
        llvm::cl::desc("Run the per-TU parses in worker subprocesses (a "
                       "crashing TU costs only that TU; composes with "
                       "--checkpoint)"),
        llvm::cl::init(false),
        llvm::cl::sub(CheckCmd));

static llvm::cl::opt<unsigned>
    CheckWorkers("workers",
        llvm::cl::desc("Number of worker processes for --isolate-workers "
                       "(0 = the --threads value)"),
        llvm::cl::init(0),
        llvm::cl::sub(CheckCmd));

static llvm::cl::opt<unsigned>
    CheckWorkerTimeout("worker-timeout",
        llvm::cl::desc("With --isolate-workers: kill a worker that starts "
                       "no new TU for this many seconds and record the TU "
                       "it was parsing as timed out (0 = no timeout)"),
        llvm::cl::value_desc("seconds"),
        llvm::cl::init(vycor::WorkerLimits{}.timeoutSeconds),
        llvm::cl::sub(CheckCmd));

static llvm::cl::opt<unsigned>
    CheckWorkerMemoryLimit("worker-memory-limit",
        llvm::cl::desc("With --isolate-workers: per-worker data-segment "
                       "limit in MiB; a worker that exceeds it dies and its "
                       "TU is poisoned (0 = no limit)"),
        llvm::cl::value_desc("MiB"),
        llvm::cl::init(0),
        llvm::cl::sub(CheckCmd));

// Worker-mode plumbing (spawned by the --isolate-workers parent; not part
// of the user-facing surface). Mirrors megascope's --bake-worker.
static llvm::cl::opt<bool>
    CheckIndexWorker("index-worker",
        llvm::cl::desc("Internal: index the --source list and write an "
                       "check index shard instead of analyzing"),
        llvm::cl::Hidden,
        llvm::cl::sub(CheckCmd));

static llvm::cl::opt<bool>
    CheckAnalyzeWorker("analyze-worker",
        llvm::cl::desc("Internal: analyze the --source list against "
                       "--global-index and write a diagnostics shard"),
        llvm::cl::Hidden,
        llvm::cl::sub(CheckCmd));

static llvm::cl::opt<std::string>
    CheckWorkerOut("worker-out",
        llvm::cl::desc("Internal: shard output path for worker modes"),
        llvm::cl::value_desc("file"),
        llvm::cl::Hidden,
        llvm::cl::sub(CheckCmd));

static llvm::cl::opt<std::string>
    CheckGlobalIndexIn("global-index",
        llvm::cl::desc("Internal: merged-index handoff file for "
                       "--analyze-worker"),
        llvm::cl::value_desc("file"),
        llvm::cl::Hidden,
        llvm::cl::sub(CheckCmd));

static llvm::cl::opt<std::string>
    CheckCheckpointFile("checkpoint",
        llvm::cl::desc("Journal per-TU progress to this file and resume "
                       "from it on the next run: a killed run picks up "
                       "where it left off, and a TU whose parse fatally "
                       "died twice is skipped instead of re-killing every "
                       "resume. Source edits invalidate exactly the "
                       "affected entries."),
        llvm::cl::value_desc("file"),
        llvm::cl::sub(CheckCmd));

static llvm::cl::opt<std::string>
    CheckOrgConfig("org-config",
        llvm::cl::desc("Organization config JSON (lock/channel types, "
                       "feature-flag patterns, disabled checks — see "
                       "docs/EXTENDING.md)"),
        llvm::cl::value_desc("file"),
        llvm::cl::sub(CheckCmd));

static llvm::cl::opt<std::string>
    CheckSourceList("source-list",
        llvm::cl::desc("Read source files from this file, one per line "
                       "('-' = stdin, '#' comments); unioned with --source"),
        llvm::cl::value_desc("file|-"),
        llvm::cl::sub(CheckCmd));

static llvm::cl::opt<std::string>
    CheckFormat("format",
        llvm::cl::desc("Report format: text (file:line:col: [check] "
                       "message), json, or sarif (SARIF 2.1.0)"),
        llvm::cl::value_desc("text|json|sarif"), llvm::cl::init("text"),
        llvm::cl::sub(CheckCmd));

static llvm::cl::opt<std::string>
    CheckOutput("output",
        llvm::cl::desc("Write the report to this file instead of stdout"),
        llvm::cl::value_desc("file"), llvm::cl::sub(CheckCmd));

static llvm::cl::opt<std::string>
    CheckFailOn("fail-on",
        llvm::cl::desc("Exit 1 when a reported finding is at or above this "
                       "severity: note (any finding, the default), warning, "
                       "error, or none"),
        llvm::cl::value_desc("severity"), llvm::cl::init("note"),
        llvm::cl::sub(CheckCmd));

static llvm::cl::opt<bool>
    CheckAllowParseFailures("allow-parse-failures",
        llvm::cl::desc("Report TUs that failed to parse as a warning instead "
                       "of exiting 3"),
        llvm::cl::sub(CheckCmd));

static llvm::cl::opt<std::string>
    CheckBaseline("baseline",
        llvm::cl::desc("Suppress the findings recorded in this baseline "
                       "file and report only new ones, plus a count of "
                       "baseline entries no longer found"),
        llvm::cl::value_desc("file"), llvm::cl::sub(CheckCmd));

static llvm::cl::opt<std::string>
    CheckWriteBaseline("write-baseline",
        llvm::cl::desc("Record the current findings' fingerprints in this "
                       "baseline file (the run then exits 0 unless a TU "
                       "failed)"),
        llvm::cl::value_desc("file"), llvm::cl::sub(CheckCmd));

static llvm::cl::opt<std::string>
    CheckPatchFile("patch-file",
        llvm::cl::desc("Report only findings on lines this unified diff "
                       "changes ('-' = stdin)"),
        llvm::cl::value_desc("file|-"), llvm::cl::sub(CheckCmd));

static llvm::cl::opt<std::string>
    CheckGitBase("git-base",
        llvm::cl::desc("Report only findings on lines changed since this "
                       "revision (`git diff <rev> [--git-head]`, run in "
                       "--project-root)"),
        llvm::cl::value_desc("rev"), llvm::cl::sub(CheckCmd));

static llvm::cl::opt<std::string>
    CheckGitHead("git-head",
        llvm::cl::desc("With --git-base: the revision to diff against "
                       "(default: the working tree)"),
        llvm::cl::value_desc("rev"), llvm::cl::sub(CheckCmd));

static llvm::cl::opt<std::string>
    CheckProjectRoot("project-root",
        llvm::cl::desc("Directory finding paths and fingerprints are "
                       "relative to (default: inferred git/common root)"),
        llvm::cl::value_desc("dir"), llvm::cl::sub(CheckCmd));

static llvm::cl::opt<bool>
    CheckAbsolutePaths("absolute-paths",
        llvm::cl::desc("Emit absolute file paths in findings and messages "
                       "instead of repo-relative paths"),
        llvm::cl::sub(CheckCmd));

static llvm::cl::opt<bool>
    CheckExceptionEscapeCallSiteLocations(
        "exception-escape-call-site-locations",
        llvm::cl::desc("Include (file:line) locations for call sites along "
                       "exception-escape call chains"),
        llvm::cl::sub(CheckCmd));

static llvm::cl::opt<bool>
    CheckVerbose("verbose",
        llvm::cl::desc("Also report inline suppressions that suppressed "
                       "nothing"),
        llvm::cl::sub(CheckCmd));
static llvm::cl::alias CheckVerboseShort("v",
                                          llvm::cl::desc("Alias for --verbose"),
                                          llvm::cl::aliasopt(CheckVerbose));

static llvm::cl::opt<bool>
    CheckModelConvertibility("model-convertibility",
                              llvm::cl::desc("Use indexed type relations "
                                             "(inheritance, converting ctors, "
                                             "conversion operators) to decide "
                                             "candidate viability"),
                              llvm::cl::sub(CheckCmd));

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
                                    " (may be repeated; optional when compile "
                                    "flags follow '--')"),
                     llvm::cl::value_desc("dir"),
                     llvm::cl::ZeroOrMore,
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
    MegascopeBuildPath("build-path",
                 llvm::cl::desc("Directory containing compile_commands.json "
                                "(or path to a compilation database file; "
                                "optional when compile flags follow '--')"),
                 llvm::cl::value_desc("dir|file"),
                 llvm::cl::sub(MegascopeCmd));

static llvm::cl::list<std::string>
    MegascopeSourceFiles("source",
                   llvm::cl::desc("Source files to index (repeatable). "
                                  "Default: every entry of the compilation "
                                  "database"),
                   llvm::cl::value_desc("file"),
                   llvm::cl::sub(MegascopeCmd));

static llvm::cl::opt<std::string>
    MegascopeSourceList("source-list",
        llvm::cl::desc("File with one source path per line ('-' = stdin; "
                       "'#' comments); unioned with --source"),
        llvm::cl::value_desc("file"),
        llvm::cl::sub(MegascopeCmd));

static llvm::cl::opt<std::string>
    MegascopeSourceRe("source-re",
        llvm::cl::desc("Keep only source paths matching this POSIX extended "
                       "regex (searched, not anchored)"),
        llvm::cl::value_desc("regex"),
        llvm::cl::sub(MegascopeCmd));

static llvm::cl::list<std::string>
    MegascopeEntryPoints("entry-point",
                   llvm::cl::desc("Entry point function names (default: main)"),
                   llvm::cl::value_desc("name"),
                   llvm::cl::sub(MegascopeCmd));

static llvm::cl::list<std::string>
    MegascopeCollapsePaths("collapse-paths",
        llvm::cl::desc("Path patterns to collapse (internal edges skipped)"),
        llvm::cl::value_desc("pattern"),
        llvm::cl::sub(MegascopeCmd));

static llvm::cl::list<std::string>
    MegascopeSkipPaths("skip-paths",
        llvm::cl::desc("Path patterns to skip entirely (TUs matching are not processed)"),
        llvm::cl::value_desc("pattern"),
        llvm::cl::sub(MegascopeCmd));

static llvm::cl::opt<unsigned>
    MegascopeThreads("threads",
        llvm::cl::desc("Number of threads (0 = hardware_concurrency, 1 = serial)"),
        llvm::cl::init(0),
        llvm::cl::sub(MegascopeCmd));

static llvm::cl::opt<std::string>
    MegascopePchDir("pch-dir",
        llvm::cl::desc("Directory for compiled PCH cache (enables PCH reuse)"),
        llvm::cl::value_desc("dir"),
        llvm::cl::sub(MegascopeCmd));

static llvm::cl::opt<std::string>
    MegascopeClang("clang",
        llvm::cl::desc("Path to clang++ binary for PCH compilation"),
        llvm::cl::value_desc("path"),
        llvm::cl::init(VYCOR_DEFAULT_CLANG),
        llvm::cl::sub(MegascopeCmd));

static llvm::cl::opt<std::string>
    MegascopeSysroot("sysroot",
        llvm::cl::desc("macOS SDK sysroot path (default: auto-detect via xcrun)"),
        llvm::cl::value_desc("dir"),
        llvm::cl::sub(MegascopeCmd));

static llvm::cl::list<std::string>
    MegascopeLockTypes("lock-types",
        llvm::cl::desc("Qualified names of additional lock types (repeatable)"),
        llvm::cl::value_desc("qualified-name"),
        llvm::cl::sub(MegascopeCmd));

static llvm::cl::opt<std::string>
    MegascopeChannelTypesJson("channel-types-json",
        llvm::cl::desc("JSON file registering channel/queue types to trace "
                       "producer/consumer call sites for (see "
                       "ChannelIndex.h for the schema)."),
        llvm::cl::value_desc("file"),
        llvm::cl::sub(MegascopeCmd));

static llvm::cl::opt<std::string>
    MegascopeOrgConfig("org-config",
        llvm::cl::desc("Organization config JSON (lock/channel types, "
                       "feature-flag patterns, collapse paths — see "
                       "docs/EXTENDING.md). Merged with the equivalent "
                       "CLI flags."),
        llvm::cl::value_desc("file"),
        llvm::cl::sub(MegascopeCmd));

static llvm::cl::opt<std::string>
    MegascopeIndex("index",
        llvm::cl::desc("Index file: load the baked graph if present "
                       "(re-indexing only changed TUs) and save after "
                       "building. Supports .vycs, .mgsc, and .shard.json. "
                       "Default: <build-path>/.vycor/megascope.vycs"),
        llvm::cl::value_desc("file"),
        llvm::cl::sub(MegascopeCmd));

// Older spelling and cross-tool --output / -o aliases; cl::alias must not
// carry cl::sub (it inherits MegascopeIndex's subcommand).
static llvm::cl::alias
    MegascopeSnapshotAlias("snapshot", llvm::cl::desc("Alias for --index"),
                     llvm::cl::aliasopt(MegascopeIndex), llvm::cl::NotHidden);
static llvm::cl::alias
    MegascopeOutputAlias("output", llvm::cl::desc("Alias for --index"),
                         llvm::cl::aliasopt(MegascopeIndex),
                         llvm::cl::NotHidden);

static llvm::cl::opt<std::string>
    MegascopeEmitShard("emit-shard",
        llvm::cl::desc("Write megascope ModuleShard JSON to this file "
                       "('-' for stdout)"),
        llvm::cl::value_desc("file|-"),
        llvm::cl::sub(MegascopeCmd));

static llvm::cl::opt<std::string>
    MegascopeEmitMgsc("emit-mgsc",
        llvm::cl::desc("Write a binary megascope .mgsc snapshot to this file"),
        llvm::cl::value_desc("file"),
        llvm::cl::sub(MegascopeCmd));

static llvm::cl::opt<bool>
    MegascopeVerbose("v",
        llvm::cl::desc("Verbose: name each re-indexed TU and why"),
        llvm::cl::init(false),
        llvm::cl::sub(MegascopeCmd));

static llvm::cl::opt<bool>
    MegascopeIsolateWorkers("isolate-workers",
        llvm::cl::desc("Bake the indexes in subprocess workers (a crashing "
                       "or hanging TU costs only that TU; parent RSS stays "
                       "bounded). Default: on when "
                       "--threads is not 1 and --pch-dir is unset; "
                       "--isolate-workers=false bakes in-process"),
        llvm::cl::init(false),
        llvm::cl::sub(MegascopeCmd));

static llvm::cl::opt<bool>
    MegascopeForce("force",
        llvm::cl::desc("Rebuild the index from scratch instead of "
                       "refreshing the TUs whose sources or headers "
                       "changed"),
        llvm::cl::init(false),
        llvm::cl::sub(MegascopeCmd));

static llvm::cl::opt<bool>
    MegascopeRetryFailed("retry-failed",
        llvm::cl::desc("Re-parse the TUs whose last parse failed even "
                       "when nothing else changed (by default they are "
                       "retried only alongside a refresh that rewrites "
                       "the index anyway)"),
        llvm::cl::init(false),
        llvm::cl::sub(MegascopeCmd));

static llvm::cl::opt<unsigned>
    MegascopeWorkers("workers",
        llvm::cl::desc("Number of worker processes for --isolate-workers "
                       "(0 = the --threads value)"),
        llvm::cl::init(0),
        llvm::cl::sub(MegascopeCmd));

static llvm::cl::opt<unsigned>
    MegascopeWorkerTimeout("worker-timeout",
        llvm::cl::desc("Under worker isolation: kill a worker that starts "
                       "no new TU for this many seconds and record the TU "
                       "it was parsing as timed out (0 = no timeout)"),
        llvm::cl::value_desc("seconds"),
        llvm::cl::init(vycor::WorkerLimits{}.timeoutSeconds),
        llvm::cl::sub(MegascopeCmd));

static llvm::cl::opt<unsigned>
    MegascopeWorkerMemoryLimit("worker-memory-limit",
        llvm::cl::desc("Under worker isolation: per-worker data-segment "
                       "limit in MiB; a worker that exceeds it dies and its "
                       "TU is poisoned (0 = no limit)"),
        llvm::cl::value_desc("MiB"),
        llvm::cl::init(0),
        llvm::cl::sub(MegascopeCmd));

// Worker-mode plumbing (spawned by the --isolate-workers parent; not part
// of the user-facing surface).
static llvm::cl::opt<bool>
    MegascopeBakeWorker("bake-worker",
        llvm::cl::desc("Internal: bake the --source list and write a "
                       "snapshot shard instead of saving an index"),
        llvm::cl::Hidden,
        llvm::cl::sub(MegascopeCmd));

static llvm::cl::opt<std::string>
    MegascopeWorkerOut("worker-out",
        llvm::cl::desc("Internal: shard output path for --bake-worker"),
        llvm::cl::value_desc("file"),
        llvm::cl::Hidden,
        llvm::cl::sub(MegascopeCmd));

static llvm::cl::opt<std::string>
    MegascopeStatsJson("stats-json",
        llvm::cl::desc("Write index-build efficiency statistics (per-phase "
                       "and per-TU timings, parse outcomes, graph sizes, "
                       "snapshot timings, peak RSS) as JSON to this file "
                       "once the index is saved"),
        llvm::cl::value_desc("file"),
        llvm::cl::sub(MegascopeCmd));

static llvm::cl::opt<std::string>
    MegascopeDumpNodes("dump-nodes",
        llvm::cl::desc("Write the node inventory as TSV (usr, display name, "
                       "file, line, comma-joined caller usrs) to this file "
                       "once the index is ready. "
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
  // JSON schema), while `index` shares the bake option block with the
  // verb-less `--bake-worker` form and hands the remaining argv to
  // llvm::cl.
  bool indexVerb = false;
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
    if (verb == "serve") {
      // The MCP server is gone: the query verbs answer the same tools,
      // and `batch` keeps one loaded index for many requests.
      llvm::errs()
          << "vycor-cpp megascope serve has been removed (the MCP server is "
             "no longer shipped):\n"
             "  build or refresh the index -> megascope index --build-path "
             "<dir> [selection flags]\n"
             "  query it                   -> megascope <tool> [flags], or "
             "megascope batch for\n"
             "                                NDJSON requests on stdin over "
             "one loaded index\n"
             "Run `vycor-cpp megascope help`.\n";
      return vycor::kExitUsage;
    }
    if (vycor::isMegascopeQueryVerb(verb)) {
      std::vector<std::string> rest(argv + 2, argv + argc);
      return vycor::runMegascopeQueryVerb(rest, llvm::outs(), llvm::errs(),
                                          std::cin);
    }
    if (verb == "index") {
      indexVerb = true;
      peeledArgv.assign(argv, argv + argc);
      peeledArgv.erase(peeledArgv.begin() + 2);
      argv = peeledArgv.data();
      --argc;
    }
  }

  // Peel direct compiler flags after `--` (FixedCompilationDatabase mode for
  // Buck2/Bazel per-TU and per-target actions) before llvm::cl parses options.
  bool hasFixedCompileArgs = false;
  std::vector<std::string> fixedCompileArgs;
  std::vector<const char *> clArgv;
  clArgv.reserve(static_cast<size_t>(argc));
  for (int i = 0; i < argc; ++i) {
    if (llvm::StringRef(argv[i]) == "--") {
      hasFixedCompileArgs = true;
      for (int j = i + 1; j < argc; ++j)
        fixedCompileArgs.emplace_back(argv[j]);
      break;
    }
    clArgv.push_back(argv[i]);
  }
  argc = static_cast<int>(clArgv.size());
  argv = clArgv.data();

  llvm::cl::AddExtraVersionPrinter([](llvm::raw_ostream &os) {
    os << "vycor-cpp version " << VYCOR_VERSION_STRING << "\n";
    os << "Host compiler: " << VYCOR_HOST_COMPILER_ID << " "
       << VYCOR_HOST_COMPILER_VERSION << "\n";
  });

  // A malformed command line exits 2 for check (its usage code — exit 1
  // means findings there) and 1, as before, for everything else.
  const bool checkArgv = argc > 1 && llvm::StringRef(argv[1]) == "check";
  if (!llvm::cl::ParseCommandLineOptions(
          argc, argv,
          "vycor-cpp: AST-based C++ analysis and transformation tool\n"
          "\nSubcommands:\n"
          "  check     Detect fragile ADL/CTAD resolution across translation "
          "units\n"
          "  morph     Apply rule-driven AST matcher transformations\n"
          "  megascope  Index a project's call graph and query it "
          "(`megascope help`)\n",
          &llvm::errs()))
    return checkArgv ? vycor::kCheckExitUsage : 1;

  vycor::appendGlobalExtraArgs({ExtraArgs.begin(), ExtraArgs.end()});

  // SIGINT/SIGTERM: a parent kills its workers and removes its scratch
  // files; a worker dies with its parent (callgraph/Interrupt.h). Before
  // any thread exists, so every later thread inherits the blocked mask.
  if (MegascopeBakeWorker || CheckIndexWorker || CheckAnalyzeWorker) {
    vycor::bindWorkerToParent();
    // A worker that crashes (or aborts at --worker-memory-limit) must die,
    // so the parent's marker/poison/bisect path handles the TU
    // (callgraph/CrashGuard.h).
    vycor::disableCrashGuard();
  } else if (CheckCmd || MegascopeCmd)
    vycor::installInterruptHandler();

  // ---- check ---------------------------------------------------------------
  if (CheckCmd) {
    if (CheckListChecks) {
      // Org registrations (ext/ static init + --org-config) participate.
      vycor::OrgConfig listOrgCfg;
      if (!loadOrgConfigIfSet(CheckOrgConfig, listOrgCfg))
        return vycor::kCheckExitUsage;
      auto defaults = vycor::defaultCheckSet();
      llvm::outs() << "checks (docs/checks/<name>.md):\n";
      for (const auto &check : vycor::builtinChecks()) {
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
      for (const auto &check : vycor::builtinChecks())
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
    // Usage errors exit 2 (docs/result-contract.md, "check exit codes").
    const int kUsage = vycor::kCheckExitUsage;
    if (CheckBuildPath.empty() && !hasFixedCompileArgs) {
      llvm::errs() << "check: --build-path is required (or pass compile flags "
                      "after '--')\n";
      return kUsage;
    }
    if (CheckFormat != "text" && CheckFormat != "json" &&
        CheckFormat != "sarif") {
      llvm::errs() << "check: --format must be text, json, or sarif (got '"
                   << CheckFormat << "')\n";
      return kUsage;
    }
    std::optional<vycor::Severity> failOn;
    if (!vycor::parseFailOn(CheckFailOn, failOn)) {
      llvm::errs() << "check: --fail-on must be note, warning, error, or "
                      "none (got '"
                   << CheckFailOn << "')\n";
      return kUsage;
    }
    if (!CheckPatchFile.empty() && !CheckGitBase.empty()) {
      llvm::errs() << "check: --patch-file and --git-base are "
                      "alternatives\n";
      return kUsage;
    }
    if (!CheckGitHead.empty() && CheckGitBase.empty()) {
      llvm::errs() << "check: --git-head requires --git-base\n";
      return kUsage;
    }
    if (CheckSourceList == "-" && CheckPatchFile == "-") {
      llvm::errs() << "check: --source-list - and --patch-file - cannot "
                      "both read stdin\n";
      return kUsage;
    }

    std::string dbError;
    auto compDb = vycor::loadCompilationDatabase(
        CheckBuildPath, fixedCompileArgs, dbError, hasFixedCompileArgs);
    if (!compDb) {
      llvm::errs() << "check: error loading compilation database from "
                   << (CheckBuildPath.empty() ? "<command-line>"
                                               : std::string(CheckBuildPath))
                   << ": " << dbError << "\n";
      return kUsage;
    }

    // --source and --source-list, unioned in order; paths made absolute
    // and dot-free (cli/SourceSelection.h). Workers get the parent's
    // already-resolved list.
    std::vector<std::string> files;
    {
      vycor::SourceSelection selection;
      selection.explicitFiles.assign(CheckSourceFiles.begin(),
                                     CheckSourceFiles.end());
      selection.listFile = CheckSourceList;
      if (selection.explicitFiles.empty() && selection.listFile.empty()) {
        llvm::errs() << "check: at least one --source file (or "
                        "--source-list) is required\n";
        return kUsage;
      }
      auto selected = vycor::selectSources(*compDb, selection, std::cin);
      if (!selected) {
        llvm::errs() << "check: " << llvm::toString(selected.takeError())
                     << "\n";
        return kUsage;
      }
      files = std::move(*selected);
      if (files.empty()) {
        llvm::errs() << "check: the source list names no files\n";
        return kUsage;
      }
    }

    vycor::OrgConfig orgCfg;
    if (!loadOrgConfigIfSet(CheckOrgConfig, orgCfg))
      return kUsage;

    // ---- named-check selection (--checks / .vycor-check.json) -----------
    // Sources in order, later winning: discovered/explicit config file,
    // --checks, then the legacy toggle flags as appended enables.
    std::set<std::string> enabledChecks = vycor::defaultCheckSet();
    {
      std::vector<std::string> spec;
      std::string cfgPath = !CheckConfig.empty()
                                ? std::string(CheckConfig)
                                : vycor::findChecksConfig(".");
      if (!cfgPath.empty()) {
        auto buf = llvm::MemoryBuffer::getFile(cfgPath);
        if (!buf) {
          llvm::errs() << "check: checks-config: cannot read " << cfgPath
                       << ": " << buf.getError().message() << "\n";
          return kUsage;
        }
        std::string err;
        if (!vycor::parseChecksConfigJson(
                std::string((*buf)->getBuffer()), spec, err)) {
          llvm::errs() << "check: checks-config: " << cfgPath << ": " << err
                       << "\n";
          return kUsage;
        }
      }
      std::string cli = CheckSpec;
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
      if (CheckCoverageDiag)
        spec.push_back("coverage-properties");
      if (CheckOdrDiag)
        spec.push_back("odr-violations");
      if (CheckDeadCode)
        spec.push_back("dead-code");
      std::string err;
      if (!vycor::resolveCheckSpec(spec, enabledChecks, err)) {
        llvm::errs() << "check: checks: " << err << "\n";
        return kUsage;
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
    opts.warnSameScore = CheckWarnSameScore;
    opts.modelConvertibility = CheckModelConvertibility;
    opts.includeCallSiteLocations = CheckExceptionEscapeCallSiteLocations;
    opts.absolutePaths = CheckAbsolutePaths;
    // Organization checks not in the enabled set are disabled, on top of
    // the org config's own disable list.
    opts.disabledChecks = orgCfg.disabledChecks;
    for (const auto &name :
         vycor::ExtensionRegistry::instance().allCheckNames())
      if (!enabledChecks.count(name))
        opts.disabledChecks.push_back(name);
    opts.threadCount = CheckThreads;
    opts.checkpointPath = CheckCheckpointFile;

    std::string projectRoot;
    {
      llvm::SmallString<256> cwd;
      llvm::sys::fs::current_path(cwd);
      projectRoot = vycor::inferProjectRoot(CheckProjectRoot, CheckBuildPath,
                                            files, cwd);
    }
    opts.projectRoot = projectRoot;

    // ---- worker modes (spawned by an --isolate-workers parent) -----------
    // Single-threaded over the batch so the last WORKER-TU stderr marker is
    // an exact poison identifier; write the shard, exit. Mirrors
    // megascope's --bake-worker.
    if (CheckIndexWorker || CheckAnalyzeWorker) {
      if (CheckWorkerOut.empty()) {
        llvm::errs() << "check: worker mode requires --worker-out\n";
        return 1;
      }
      if (CheckIndexWorker) {
        std::vector<std::pair<std::string, vycor::CheckIndexPayload>> shards;
        std::vector<vycor::TuOutcome> outcomes;
        shards.reserve(files.size());
        for (const auto &file : files) {
          llvm::errs() << "WORKER-TU " << file << "\n";
          vycor::GlobalIndex shard;
          auto tool = vycor::makeClangTool(*compDb, {file});
          vycor::IndexerActionFactory factory(
            shard, opts.enableOdrDiag,
            opts.enableStaticInitOrderDiag || opts.enableExceptionEscapeDiag);
          outcomes.push_back(vycor::outcomeForToolStatus(tool.run(&factory)));
          shards.emplace_back(file, vycor::CheckIndexPayload::capture(shard));
        }
        if (!vycor::writeCheckIndexShard(CheckWorkerOut, shards,
                                          outcomes)) {
          llvm::errs() << "check: worker: cannot write shard to "
                       << CheckWorkerOut << "\n";
          return 1;
        }
      } else {
        vycor::GlobalIndex indexIn;
        if (CheckGlobalIndexIn.empty() ||
            !vycor::readGlobalIndexFile(CheckGlobalIndexIn, indexIn)) {
          llvm::errs() << "check: worker: --analyze-worker requires a "
                          "readable --global-index\n";
          return 1;
        }
        std::vector<std::pair<std::string, std::vector<vycor::Diagnostic>>>
            perTu;
        std::vector<vycor::TuOutcome> outcomes;
        perTu.reserve(files.size());
        for (const auto &file : files) {
          llvm::errs() << "WORKER-TU " << file << "\n";
          std::vector<vycor::Diagnostic> local;
          auto tool = vycor::makeClangTool(*compDb, {file});
          vycor::AnalyzerActionFactory factory(indexIn, local, opts);
          outcomes.push_back(vycor::outcomeForToolStatus(tool.run(&factory)));
          perTu.emplace_back(file, std::move(local));
        }
        if (!vycor::writeCheckDiagShard(CheckWorkerOut, perTu, outcomes)) {
          llvm::errs() << "check: worker: cannot write shard to "
                       << CheckWorkerOut << "\n";
          return 1;
        }
      }
      return 0;
    }

    // ---- parent-side worker isolation ------------------------------------
    if (CheckIsolateWorkers) {
      static int selfExeAnchor; // address anchors getMainExecutable
      std::string selfExe =
          llvm::sys::fs::getMainExecutable(argv[0], &selfExeAnchor);
      opts.workerCount =
          CheckWorkers ? CheckWorkers.getValue() : CheckThreads.getValue();
      // Fully-resolved check set for workers: "-all" first so worker-side
      // defaults and any discovered config file are overridden.
      std::string workerChecks = "-all";
      for (const auto &name : enabledChecks)
        workerChecks += "," + name;
      vycor::WorkerLimits limits;
      limits.timeoutSeconds = CheckWorkerTimeout;
      limits.memoryLimitMB = CheckWorkerMemoryLimit;
      opts.isolatedRunner = [selfExe, workerChecks, limits, projectRoot,
                             fixedCompileArgs, hasFixedCompileArgs](
                                uint8_t phase,
                                const std::string &globalIndexPath,
                                const std::vector<std::string> &batch,
                                const std::string &shardPath,
                                const std::string &stderrPath) -> int {
        std::vector<std::string> workerArgv;
        workerArgv.reserve(18 + 2 * batch.size() + fixedCompileArgs.size());
        workerArgv.push_back(selfExe);
        workerArgv.push_back("check");
        workerArgv.push_back(phase == vycor::CheckCheckpoint::kPhaseIndex
                                 ? "--index-worker"
                                 : "--analyze-worker");
        workerArgv.push_back("--worker-out");
        workerArgv.push_back(shardPath);
        if (!CheckBuildPath.empty()) {
          workerArgv.push_back("--build-path");
          workerArgv.push_back(CheckBuildPath);
        }
        if (!projectRoot.empty()) {
          workerArgv.push_back("--project-root");
          workerArgv.push_back(projectRoot);
        }
        if (!globalIndexPath.empty()) {
          workerArgv.push_back("--global-index");
          workerArgv.push_back(globalIndexPath);
        }
        if (CheckWarnSameScore)
          workerArgv.push_back("--warn-same-score");
        if (CheckModelConvertibility)
          workerArgv.push_back("--model-convertibility");
        if (CheckExceptionEscapeCallSiteLocations)
          workerArgv.push_back("--exception-escape-call-site-locations");
        if (CheckAbsolutePaths)
          workerArgv.push_back("--absolute-paths");
        workerArgv.push_back("--checks=" + workerChecks);
        if (!CheckOrgConfig.empty()) {
          workerArgv.push_back("--org-config");
          workerArgv.push_back(CheckOrgConfig);
        }
        for (const auto &a : vycor::globalExtraArgs())
          workerArgv.push_back("--extra-arg=" + a);
        for (const auto &f : batch) {
          workerArgv.push_back("--source");
          workerArgv.push_back(f);
        }
        if (hasFixedCompileArgs || !fixedCompileArgs.empty()) {
          workerArgv.push_back("--");
          for (const auto &a : fixedCompileArgs)
            workerArgv.push_back(a);
        }

        return vycor::runWorkerProcess(workerArgv, stderrPath, limits,
                                       "check");
      };
    }

    // The changed-lines filter and the baseline are read before the
    // analysis runs, so a bad path fails fast instead of after it.
    std::optional<std::vector<vycor::PatchRange>> changedRanges;
    if (!CheckPatchFile.empty() || !CheckGitBase.empty()) {
      std::string patch, error;
      if (!CheckGitBase.empty()) {
        if (!vycor::gitDiffText(projectRoot, CheckGitBase, CheckGitHead,
                                patch, error)) {
          llvm::errs() << "check: " << error << "\n";
          return kUsage;
        }
      } else if (CheckPatchFile == "-") {
        patch.assign(std::istreambuf_iterator<char>(std::cin),
                     std::istreambuf_iterator<char>());
      } else {
        auto buf = llvm::MemoryBuffer::getFile(CheckPatchFile);
        if (!buf) {
          llvm::errs() << "check: cannot read --patch-file "
                       << CheckPatchFile << ": "
                       << buf.getError().message() << "\n";
          return kUsage;
        }
        patch = (*buf)->getBuffer().str();
      }
      changedRanges = vycor::parseUnifiedDiff(patch);
    }
    std::optional<std::vector<vycor::BaselineEntry>> baseline;
    if (!CheckBaseline.empty()) {
      baseline.emplace();
      std::string error;
      if (!vycor::readBaselineFile(CheckBaseline, *baseline, error)) {
        llvm::errs() << "check: --baseline: " << error << "\n";
        return kUsage;
      }
    }

    // Keep the merged index alive for graph-backed post passes.
    vycor::GlobalIndex mergedIndex;
    vycor::AnalysisReport analysisReport;
    auto diagnostics = vycor::runAnalysis(*compDb, files, opts, &mergedIndex,
                                          &analysisReport);

    // Graph-backed checks (dead-code, static-init-hazards) share one call
    // graph build.
    const bool wantDeadCode = enabledChecks.count("dead-code") > 0;
    const bool wantInitHazards =
        enabledChecks.count("static-init-hazards") > 0;
    bool hasInitHazardRoots = false;
    if (wantInitHazards) {
      mergedIndex.forEachStaticInit([&](const vycor::StaticInitEntry &e) {
        if (e.dynamicInit && (e.isConstructorFn || !e.calledFunctions.empty()))
          hasInitHazardRoots = true;
      });
    }
    if (wantDeadCode || hasInitHazardRoots) {
      vycor::CallGraph graph;
      bool loadedFromSnap = false;
      if (!CheckBuildPath.empty()) {
        std::string snapPath = vycor::defaultIndexPath(CheckBuildPath);
        if (auto snap = vycor::SnapshotIO::load(
                snapPath, nullptr, vycor::LoadMode::ReadOnly,
                vycor::kSectionGraph)) {
          if (!wantDeadCode || snap->meta.files.size() == files.size()) {
            auto stamps = vycor::SnapshotIO::stampFiles(files);
            auto dirty = vycor::SnapshotIO::dirtyTUs(snap->meta, stamps);
            bool allClean = !dirty.empty();
            for (bool d : dirty) {
              if (d) {
                allClean = false;
                break;
              }
            }
            if (allClean) {
              graph = std::move(snap->graph);
              loadedFromSnap = true;
            }
          }
        }
      }
      if (!loadedFromSnap) {
        const unsigned cgThreads =
            CheckIsolateWorkers ? opts.workerCount : opts.threadCount;
        graph = vycor::buildCallGraph(*compDb, files, {}, cgThreads);
      }

      if (wantDeadCode) {
        std::vector<std::string> entryPoints(CheckEntryPoints.begin(),
                                             CheckEntryPoints.end());
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
      if (hasInitHazardRoots)
        vycor::analyzeStaticInitHazards(mergedIndex, graph, diagnostics);
    }

    // Never report on a run an interrupt cut short (callgraph/Interrupt.h).
    vycor::exitIfInterrupted();

    // ---- the report (check/Report.h) ------------------------------------
    vycor::CheckRun run;
    run.projectRoot = projectRoot;
    run.absolutePaths = CheckAbsolutePaths;
    run.tus = std::move(analysisReport.tus);
    run.checks.assign(enabledChecks.begin(), enabledChecks.end());
    run.toolVersion = VYCOR_VERSION_STRING;
    run.reportUnusedSuppressions = CheckVerbose;
    run.findings =
        vycor::buildFindings(diagnostics, projectRoot, CheckAbsolutePaths);
    // The analyzed TUs are scanned for unused suppressions only under -v.
    run.suppressed = vycor::applyInlineSuppressions(
        run.findings,
        CheckVerbose ? files : std::vector<std::string>{}, projectRoot,
        run.unusedSuppressions);
    const size_t failedTus = vycor::failedTuCount(run);

    bool baselineWritten = false;
    if (!CheckWriteBaseline.empty()) {
      if (failedTus && !CheckAllowParseFailures) {
        llvm::errs() << "check: --write-baseline: not written — " << failedTus
                     << " TU(s) failed to parse, so the findings are "
                        "incomplete (--allow-parse-failures writes it "
                        "anyway)\n";
      } else {
        std::string error;
        if (!vycor::writeBaselineFile(CheckWriteBaseline, run.findings,
                                      error)) {
          llvm::errs() << "check: --write-baseline: " << error << "\n";
          return kUsage;
        }
        llvm::errs() << "check: wrote " << run.findings.size()
                     << " finding(s) to the baseline " << CheckWriteBaseline
                     << "\n";
        baselineWritten = true;
      }
    }
    if (baseline) {
      run.baselineUsed = true;
      run.baselined =
          vycor::applyBaseline(run.findings, *baseline, run.staleBaseline,
                               changedRanges ? &*changedRanges : nullptr);
    }
    if (changedRanges) {
      run.changedLinesUsed = true;
      run.outsideChanges =
          vycor::filterToChangedLines(run.findings, *changedRanges);
    }

    auto render = [&](llvm::raw_ostream &os) {
      if (CheckFormat == "json")
        vycor::renderJson(run, os);
      else if (CheckFormat == "sarif")
        vycor::renderSarif(run, os);
      else
        vycor::renderText(run, os);
    };
    // The TU summary is printed whatever happens to the report, so a run
    // whose --output cannot be written still names its failed TUs.
    std::string outputError;
    bool outputWritten = true;
    if (!CheckOutput.empty()) {
      outputWritten =
          vycor::writeFileAtomically(CheckOutput, render, &outputError);
    } else {
      render(llvm::outs());
      llvm::outs().flush();
    }
    vycor::renderSummary(run, llvm::errs());
    if (failedTus && CheckAllowParseFailures)
      llvm::errs() << "check: WARNING: " << failedTus
                   << " TU(s) failed; findings in them may be missing "
                      "(--allow-parse-failures)\n";
    if (!outputWritten) {
      llvm::errs() << "check: --output: " << outputError << "\n";
      return kUsage;
    }

    // A baseline write accepts the current findings: only a parse failure
    // still fails the run.
    return vycor::checkExitCode(run,
                                 baselineWritten ? std::nullopt : failOn,
                                 CheckAllowParseFailures);
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
    if (MorphBuildPaths.empty() && !hasFixedCompileArgs) {
      llvm::errs() << "morph: --build-path is required (or pass compile flags "
                      "after '--')\n";
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
    return pipeline.execute(buildPaths, files, MorphDryRun, fixedCompileArgs,
                            hasFixedCompileArgs);
  }

  // ---- megascope -------------------------------------------------------------
  if (MegascopeCmd) {
    if (!indexVerb && !MegascopeBakeWorker) {
      llvm::errs() << "megascope: missing verb — `megascope index` builds "
                      "or refreshes the index; run `vycor-cpp megascope "
                      "help` for the query verbs\n";
      return vycor::kExitUsage;
    }
    if (MegascopeBuildPath.empty() && !hasFixedCompileArgs) {
      llvm::errs() << "megascope: --build-path is required (or pass compile "
                      "flags after '--')\n";
      return 1;
    }

    // The index location defaults to <build-path>/.vycor/megascope.vycs —
    // not $VYCOR_INDEX, which is a query-side convenience and must never
    // become a write target (see resolveIndexPath). A bake worker writes
    // only its shard (--worker-out). When --emit-shard or --emit-mgsc is
    // passed without --index, only the requested shard/mgsc output is written.
    std::string indexPath = MegascopeIndex;
    if (MegascopeBakeWorker)
      indexPath.clear();
    else if (indexPath.empty() && MegascopeEmitShard.empty() &&
             MegascopeEmitMgsc.empty())
      indexPath = vycor::defaultIndexPath(MegascopeBuildPath);

    const bool indexIsMgsc =
        !indexPath.empty() &&
        vycor::MegascopeBridge::isMegascopeSnapshotPath(indexPath);
    const bool indexIsShardJson =
        !indexPath.empty() &&
        vycor::MegascopeBridge::isMegascopeShardJsonPath(indexPath);

    std::string dbError;
    auto compDb = vycor::loadCompilationDatabase(
        MegascopeBuildPath, fixedCompileArgs, dbError, hasFixedCompileArgs);
    if (!compDb) {
      llvm::errs() << "megascope: error loading compilation database from "
                   << (MegascopeBuildPath.empty()
                           ? "<command-line>"
                           : std::string(MegascopeBuildPath))
                   << ": " << dbError << "\n";
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
    // so a bare `index` never silently widens a narrow index to the whole
    // database (and re-saves the result). Workers never carry an index.
    std::optional<vycor::SnapshotData> snap;
    vycor::SnapshotLoadStats snapLoadStats;
    double snapLoadMs = 0;
    // The whole load → dirty check → bake → save sequence runs under the
    // index's write lock, so two `index` runs serialize instead of
    // racing; readers never lock. Held until the process exits.
    std::unique_ptr<vycor::IndexWriteLock> writeLock;
    if (!indexPath.empty() && !MegascopeBakeWorker) {
      std::string lockError;
      bool busy = false;
      writeLock = vycor::IndexWriteLock::acquire(
          indexPath, !MegascopeNoWait, &lockError, &busy,
          [](const std::string &lock) {
            llvm::errs() << "megascope: waiting for another writer holding "
                         << lock << " (pass --no-wait to fail instead)...\n";
          });
      if (!writeLock && busy) {
        llvm::errs() << "megascope: " << lockError << "\n";
        return 1;
      }
      if (!writeLock) {
        // A directory we cannot create the lock file in is one no other
        // writer can save to either (an unchanged `index` over a
        // read-only index still works; a save would fail and say so).
        llvm::errs() << "megascope: WARNING: " << lockError
                     << " — continuing without the write lock\n";
      } else if (size_t stale = vycor::removeStaleAtomicTemps(indexPath)) {
        // Under the lock no other writer is mid-save: any temp file next
        // to the index was left by a killed one.
        llvm::errs() << "megascope: removed " << stale
                     << " temp file(s) left by an interrupted save\n";
      }
    }
    if (!indexPath.empty() && !MegascopeBakeWorker && !indexIsMgsc &&
        !indexIsShardJson) {
      auto t0 = StatsClock::now();
      // Meta and header counts only: enough for TU selection and the
      // dirty check. The graph is decoded further down only when a warm
      // refresh or --dump-nodes needs it, so an unchanged `index` never pays
      // for it (5 s on a 938-TU index).
      snap = vycor::SnapshotIO::load(indexPath, &snapLoadStats,
                                     vycor::LoadMode::ReadOnly, 0);
      if (snap)
        snapLoadMs = msSince(t0);
    }

    vycor::SourceSelection selection;
    selection.explicitFiles.assign(MegascopeSourceFiles.begin(),
                                   MegascopeSourceFiles.end());
    selection.listFile = MegascopeSourceList;
    selection.regex = MegascopeSourceRe;
    selection.skipPaths.assign(MegascopeSkipPaths.begin(),
                               MegascopeSkipPaths.end());
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
    std::vector<std::string> collapsePaths(MegascopeCollapsePaths.begin(),
                                           MegascopeCollapsePaths.end());

    // Pre-compile PCH headers if --pch-dir is set.
    std::unique_ptr<vycor::PchCache> pchCache;
    if (!MegascopePchDir.empty()) {
      llvm::errs() << "megascope: building PCH cache...\n";
      pchCache = std::make_unique<vycor::PchCache>(
          MegascopePchDir.getValue(), MegascopeClang.getValue());
      pchCache->buildFromCompileCommands(*compDb, files);
    }
    const vycor::PchCache *pchPtr = pchCache.get();

    std::string sysroot = MegascopeSysroot.getValue();

    vycor::LockTypeConfig lockCfg;
    lockCfg.userAllowlist.assign(MegascopeLockTypes.begin(),
                                 MegascopeLockTypes.end());
    vycor::ChannelTypeConfig channelCfg;
    if (!MegascopeChannelTypesJson.empty() &&
        !parseChannelTypesJson(MegascopeChannelTypesJson, channelCfg)) {
      return 1;
    }
    vycor::OrgConfig orgCfg;
    if (!loadOrgConfigIfSet(MegascopeOrgConfig, orgCfg))
      return 1;
    mergeExtensionConfig(orgCfg, lockCfg, channelCfg, collapsePaths);
    // ---- worker mode (spawned by an --isolate-workers parent) ------------
    // Bake the batch with the existing in-process pipeline (crash guard
    // disabled in workers, see main's entry), write the v5 snapshot shard,
    // exit. No index, no warm start, no stats-json. The WORKER-TU stderr
    // marker before each parse is the parent's poison identifier when this
    // process dies.
    if (MegascopeBakeWorker) {
      if (MegascopeWorkerOut.empty()) {
        llvm::errs() << "megascope: --bake-worker requires --worker-out\n";
        return 1;
      }
      auto baked = vycor::bakeIndexes(
          *compDb, files, collapsePaths, MegascopeThreads, pchPtr, sysroot,
          lockCfg, /*stats=*/nullptr,
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
      std::string saveError;
      if (!vycor::SnapshotIO::save(MegascopeWorkerOut, baked.graph,
                                   baked.cfIndex, meta, baked.channels,
                                   &saveError, /*durable=*/false)) {
        llvm::errs() << "megascope: worker: cannot write shard to "
                     << MegascopeWorkerOut << ": " << saveError << "\n";
        return 1;
      }
      return 0;
    }

    vycor::CallGraph graph;
    vycor::ControlFlowIndex cfIndex;
    vycor::ChannelIndex channels;
    bool needFullBuild = true;
    // Set when a bake parsed nothing it was asked to: its result must not
    // replace the index (SnapshotIO::unpublishableBake).
    std::string bakeRefusal;

    // Efficiency stats, dumped to --stats-json once the index is saved.
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
    const auto bakeEnv =
        vycor::BakeEnvironment::current(sysroot, MegascopePchDir);
    const std::string envFingerprint = vycor::environmentFingerprint(bakeEnv);
    auto fingerprintStart = StatsClock::now();
    auto currentFingerprints =
        vycor::fingerprintTUs(*compDb, files, envFingerprint);
    const double fingerprintMs = msSince(fingerprintStart);
    // Every requested TU's outcome, from this bake or kept from the index.
    vycor::TuOutcomes outcomes;

    // Worker isolation is the default for the whole-project bakes: a TU
    // that crashes or hangs the frontend costs only itself. In-process
    // stays for --threads 1 (the in-process crash guard, docs/
    // design-f12-subprocess-workers.md "Failure modes") and for --pch-dir,
    // which workers do not receive.
    const bool isolate = MegascopeIsolateWorkers.getNumOccurrences()
                             ? MegascopeIsolateWorkers.getValue()
                             : MegascopeThreads != 1 && MegascopePchDir.empty();
    vycor::WorkerLimits workerLimits;
    workerLimits.timeoutSeconds = MegascopeWorkerTimeout;
    workerLimits.memoryLimitMB = MegascopeWorkerMemoryLimit;
    static int selfExeAnchor; // address anchors getMainExecutable
    const std::string selfExe =
        llvm::sys::fs::getMainExecutable(argv[0], &selfExeAnchor);
    vycor::BakeWorkerConfig bakeCfg;
    bakeCfg.buildPath = MegascopeBuildPath;
    bakeCfg.fixedCompileArgs = fixedCompileArgs;
    bakeCfg.hasFixedCompileArgs = hasFixedCompileArgs;
    bakeCfg.collapsePaths = collapsePaths;
    bakeCfg.extraArgs = vycor::globalExtraArgs();
    bakeCfg.sysroot = sysroot;
    bakeCfg.lockTypes = lockCfg.userAllowlist;
    bakeCfg.channelTypesJson = MegascopeChannelTypesJson;
    bakeCfg.orgConfig = MegascopeOrgConfig;

    // One bake for the cold build and the warm refresh alike: the
    // in-process parallel pipeline, or subprocess workers.
    auto runBake = [&](const std::vector<std::string> &toBake) {
      if (isolate) {
        unsigned workerCount =
            MegascopeWorkers ? MegascopeWorkers.getValue()
                             : MegascopeThreads.getValue();
        if (workerCount == 0)
          workerCount = std::thread::hardware_concurrency();
        return vycor::bakeIsolated(selfExe, bakeCfg, toBake, workerCount,
                                   &buildStats, workerLimits);
      }
      return vycor::bakeIndexes(*compDb, toBake, collapsePaths,
                                MegascopeThreads, pchPtr, sysroot, lockCfg,
                                &buildStats, nullptr, channelCfg);
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
            !MegascopeRetryFailed && !MegascopeForce) {
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
        if (MegascopeForce) {
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

          if (toDrop.empty() && toBake.empty() && MegascopeDumpNodes.empty() &&
              MegascopeEmitShard.empty() && MegascopeEmitMgsc.empty()) {
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
                           << " (" << snapLoadStats.error
                           << ") — full build\n";
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
                if (MegascopeVerbose) {
                  for (size_t i = 0; i < dirtyFlags.size(); ++i)
                    if (dirtyFlags[i])
                      llvm::errs() << "megascope:   " << currentStamps[i].path
                                   << ": " << dirtyWhy.detail[i] << "\n";
                }
                auto bakeStart = StatsClock::now();
                auto fresh = runBake(toBake);
                warmBakeMs = msSince(bakeStart);
                bakeRefusal =
                    vycor::SnapshotIO::unpublishableBake(toBake,
                                                         fresh.outcomes);
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
      } else if (llvm::sys::fs::exists(indexPath) && !indexIsMgsc &&
                 !indexIsShardJson) {
        llvm::errs() << "megascope: cannot load index " << indexPath << " ("
                     << snapLoadStats.error << ") — full build\n";
      } else {
        llvm::errs() << "megascope: no index yet at " << indexPath
                     << " — full build\n";
      }
    }

    if (needFullBuild) {
      llvm::errs() << "megascope: baking call graph + control flow index ("
                   << files.size() << " files, "
                   << MegascopeThreads << " threads)...\n";
      auto bakeStart = StatsClock::now();
      vycor::BakedIndexes baked = runBake(files);
      bakeMs = msSince(bakeStart);
      bakeRefusal =
          vycor::SnapshotIO::unpublishableBake(files, baked.outcomes);
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
    if (!indexesChanged && MegascopeEmitShard.empty() &&
        MegascopeEmitMgsc.empty()) {
      llvm::errs() << "megascope: index unchanged — skipping re-save\n";
      // Nothing re-indexed: the coverage is what the index records
      // (failed TUs left as recorded included).
      coverage = vycor::coverageOf(snap->meta);
    } else {
      vycor::SnapshotMeta meta;
      meta.collapsePaths = collapsePaths;
      meta.lockAllowlist = lockCfg.userAllowlist;
      meta.lockBuiltins = lockCfg.useBuiltins;
      meta.channelTypes = channelCfg.registeredTypes;
      meta.files = std::move(currentStamps);
      meta.fingerprints = std::move(currentFingerprints);
      vycor::SnapshotIO::recordDependencies(meta, deps);
      vycor::SnapshotIO::recordOutcomes(meta, outcomes);
      meta.entryPoints.assign(MegascopeEntryPoints.begin(),
                              MegascopeEntryPoints.end());
      meta.provenance.analyzer = vycor::analyzerIdentity();
      meta.provenance.toolchain = vycor::toolchainIdentity();
      meta.provenance.environment = envFingerprint;
      meta.provenance.bakeStartNs = bakeStartNs;
      unstableStamps =
          vycor::SnapshotIO::markUnstableStamps(meta, bakeStartNs);
      coverage = vycor::coverageOf(meta);
      if (!coverage.complete())
        llvm::errs() << "megascope: WARNING: " << coverage.indexed << " of "
                     << coverage.requested << " TU(s) indexed cleanly ("
                     << coverage.partial << " partial, " << coverage.failed
                     << " failed); the rest are retried with the next "
                        "refresh that rewrites the index, or "
                        "--retry-failed\n";
      // An interrupted bake is partial: never let it replace the index.
      vycor::exitIfInterrupted();
      auto snapSaveStart = StatsClock::now();
      std::string saveError;
      if (!bakeRefusal.empty()) {
        saveFailed = true;
        llvm::errs() << "megascope: ERROR: " << bakeRefusal << " — "
                     << indexPath << " is left as it was\n";
      } else {
        if (!indexPath.empty() && indexesChanged) {
          bool savedOk = false;
          if (indexIsShardJson) {
            savedOk = vycor::MegascopeBridge::saveShardJson(
                indexPath, graph, cfIndex, meta, channels, &saveError);
          } else if (indexIsMgsc) {
            savedOk = vycor::MegascopeBridge::saveSnapshot(
                indexPath, graph, cfIndex, meta, channels, &saveError);
          } else {
            savedOk = vycor::SnapshotIO::save(indexPath, graph, cfIndex, meta,
                                              channels, &saveError);
          }
          if (savedOk) {
            snapSaveMs = msSince(snapSaveStart);
            llvm::errs() << "megascope: index saved to " << indexPath << "\n";
          } else {
            saveFailed = true;
            llvm::errs() << "megascope: WARNING: could not save index to "
                         << indexPath << ": " << saveError
                         << " (the previous index is left as it was)\n";
          }
        }
        if (!MegascopeEmitShard.empty()) {
          if (MegascopeEmitShard == "-") {
            llvm::outs() << ((files.size() <= 1)
                                 ? vycor::MegascopeBridge::toShardJson(
                                       graph, cfIndex, channels, meta,
                                       /*pretty=*/true)
                                 : vycor::MegascopeBridge::toShardsJson(
                                       graph, cfIndex, channels, meta,
                                       /*pretty=*/true));
            if (indexPath.empty() && MegascopeEmitMgsc.empty())
              return saveFailed ? 1 : 0;
          } else {
            std::string shardErr;
            if (!vycor::MegascopeBridge::saveShardJson(
                    MegascopeEmitShard, graph, cfIndex, meta, channels,
                    &shardErr)) {
              saveFailed = true;
              llvm::errs() << "megascope: ERROR: could not write shard JSON to "
                           << MegascopeEmitShard << ": " << shardErr << "\n";
            } else {
              llvm::errs() << "megascope: shard JSON saved to "
                           << MegascopeEmitShard << "\n";
            }
          }
        }
        if (!MegascopeEmitMgsc.empty()) {
          std::string mgscErr;
          if (!vycor::MegascopeBridge::saveSnapshot(
                  MegascopeEmitMgsc, graph, cfIndex, meta, channels,
                  &mgscErr)) {
            saveFailed = true;
            llvm::errs() << "megascope: ERROR: could not write .mgsc snapshot "
                            "to "
                         << MegascopeEmitMgsc << ": " << mgscErr << "\n";
          } else {
            llvm::errs() << "megascope: .mgsc snapshot saved to "
                         << MegascopeEmitMgsc << "\n";
          }
        }
      }
    }

    if (!MegascopeStatsJson.empty()) {
      llvm::json::Object root;
      root["mode"] = needFullBuild ? "cold" : "warm";
      root["files"] = static_cast<int64_t>(files.size());
      root["threads"] = static_cast<int64_t>(MegascopeThreads);
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
        o["file"] = vycor::toIndexText(t.file); // raw path (Utf8.h)
        o["phase"] = static_cast<int64_t>(t.phase);
        o["ms"] = t.ms;
        o["status"] = static_cast<int64_t>(t.toolStatus);
        tus.push_back(std::move(o));
      }
      root["tu"] = std::move(tus);

      std::error_code ec;
      llvm::raw_fd_ostream out(MegascopeStatsJson, ec);
      if (ec) {
        llvm::errs() << "megascope: WARNING: cannot write stats to "
                     << MegascopeStatsJson << ": " << ec.message() << "\n";
      } else {
        out << llvm::json::Value(std::move(root)) << "\n";
        llvm::errs() << "megascope: stats written to " << MegascopeStatsJson
                     << "\n";
      }
    }

    if (!MegascopeDumpNodes.empty()) {
      std::error_code ec;
      llvm::raw_fd_ostream out(MegascopeDumpNodes, ec);
      if (ec) {
        llvm::errs() << "megascope: WARNING: cannot write node dump to "
                     << MegascopeDumpNodes << ": " << ec.message() << "\n";
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
                     << " nodes) written to " << MegascopeDumpNodes << "\n";
      }
    }

    // The index file is the product: failing to write it is the error.
    if (saveFailed)
      return 1;
    std::string reportedIndexPath =
        !indexPath.empty() ? indexPath
        : (!MegascopeEmitMgsc.empty() ? std::string(MegascopeEmitMgsc)
                                      : std::string(MegascopeEmitShard));
    llvm::json::Object summary;
    summary["index"] =
        vycor::toIndexText(reportedIndexPath); // raw path (Utf8.h)
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

  llvm::errs() << "No subcommand specified. Use 'check', 'morph', "
                  "or 'megascope'.\n"
               << "Run with --help for usage information.\n";
  return 1;
}
