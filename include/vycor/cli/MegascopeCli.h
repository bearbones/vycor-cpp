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

#include "vycor/query/Tools.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

#include <istream>
#include <string>
#include <vector>

// ============================================================================
// megascope query verbs (docs/megascope-cli-review.md, Part 2).
//
//   vycor-cpp megascope <tool>  [--index F] [tool flags...]
//   vycor-cpp megascope call <tool> --args '<json>'
//   vycor-cpp megascope tools   [--format json|ndjson|tsv]
//   vycor-cpp megascope info    [--index F] [--files]
//   vycor-cpp megascope batch   [--index F]     # NDJSON requests on stdin
//
// These verbs never touch llvm::cl: main.cpp peels the verb off argv and
// hands the rest to runMegascopeQueryVerb. Per-tool flags are derived from
// each tool's JSON Schema (parseToolArgs), so the CLI surface cannot drift
// from what MCP clients see. `index` and `serve` stay llvm::cl verbs in
// main.cpp because they share the bake option block with the legacy form.
//
// Output contract (2.3): stdout carries only the payload — compact JSON by
// default, `--pretty` on request, `--format ndjson` (one record per line,
// preceded by a {"_summary":...} line) or `--format tsv` for list-shaped
// results (ToolEntry::recordsKey). Exit codes are the CLI's primary
// signal; see MegascopeExit. `dump` streams every call-site context and
// channel site (ndjson by default).
//
// Ephemeral mode (2.1, 4.1): a query verb given --source/--source-list/
// --source-re (with --build-path) bakes the selected TUs in memory and
// answers from that — no index is read or written. What `prism` was.
// ============================================================================

namespace vycor {

enum class OutputFormat { Json, Ndjson, Tsv };

/// Process exit codes for the query verbs. Agents branch on these rather
/// than on parsing an error field.
enum MegascopeExit : int {
  kExitResults = 0,   // answered, non-empty
  kExitEmpty = 1,     // answered, empty (not found / no paths / no dead code)
  kExitUsage = 2,     // usage or argument error
  kExitIndex = 3,     // index missing, wrong format version, or unreadable
  kExitAmbiguous = 4, // ambiguous identity (candidates on stdout)
};

/// Default index location relative to a build directory.
std::string defaultIndexPath(llvm::StringRef buildPath);

/// Resolution chain for the index file read by the query verbs: `--index`,
/// then $VYCOR_INDEX (`envIndex`, already read by the caller), then
/// defaultIndexPath(buildPath) when a build path is known, else
/// defaultIndexPath(".") — i.e. run from the build directory and no flag is
/// needed at all. The writing verbs (`index`, `serve`) deliberately skip
/// the environment variable: it is a query-side convenience, and honoring
/// it as a write location would let one project's bake overwrite another
/// project's index.
std::string resolveIndexPath(llvm::StringRef explicitIndex,
                             llvm::StringRef envIndex,
                             llvm::StringRef buildPath);

/// Tool names are exposed with hyphens on the command line
/// (`get-callers`) and accepted with underscores too; this returns the
/// registered spelling.
std::string canonicalToolName(llvm::StringRef verb);

/// True when `verb` (argv[2] after "megascope") is handled by
/// runMegascopeQueryVerb rather than the llvm::cl bake path: any word that
/// is not an option and not `index`/`serve`. Unknown words are accepted
/// here so the runner can report them as a usage error naming the verbs.
bool isMegascopeQueryVerb(llvm::StringRef verb);

/// Parse tool flags derived from `tool.inputSchema.properties`: strings
/// take a value, integers are parsed, booleans are bare flags (or
/// `--flag=false`), arrays repeat. `--name value` and `--name=value` are
/// both accepted; hyphens and underscores are interchangeable. `seed` (from
/// `--args '<json>'`) supplies defaults that explicit flags override.
/// Unknown flags and malformed values are usage errors naming the valid
/// flags. The common query flags (--index, --format, ...) must already
/// have been removed from `argv`.
llvm::Expected<llvm::json::Object>
parseToolArgs(const ToolEntry &tool, llvm::ArrayRef<std::string> argv,
              llvm::json::Object seed = {});

/// `--help` text for a tool: description, then every schema property with
/// its type and description (the same text the MCP client sees).
void printToolHelp(const ToolEntry &tool, llvm::raw_ostream &os);

/// Exit code for a tool payload: error payloads map to kExitUsage when the
/// message carries one of the argument-error prefixes the Tools.h result
/// contract reserves ("Missing", "Requires", "Invalid") and kExitEmpty
/// otherwise ("Function not found: ..."); ambiguity payloads to
/// kExitAmbiguous; an empty records list to kExitEmpty; everything else
/// kExitResults.
int exitCodeFor(const llvm::json::Value &payload, llvm::StringRef recordsKey);

/// Write `payload` to `out` in `format` and return exitCodeFor(payload).
/// `recordsKey` names the list-shaped member for ndjson/tsv (empty = the
/// payload is a scalar record; ambiguity payloads always use
/// "candidates"). Error payloads additionally print their message to `err`
/// as `megascope <tool>: <message>` when `tool` is non-empty.
int emitToolResult(const llvm::json::Value &payload, llvm::StringRef recordsKey,
                   OutputFormat format, bool pretty, llvm::raw_ostream &out,
                   llvm::raw_ostream &err, llvm::StringRef tool = "");

/// Run one query verb to completion. `args` starts with the verb
/// (argv[2..] as main received it). `in` is only read by `batch`.
int runMegascopeQueryVerb(llvm::ArrayRef<std::string> args,
                          llvm::raw_ostream &out, llvm::raw_ostream &err,
                          std::istream &in);

/// `megascope diff --before A --after B ...` (docs/change-impact.md):
/// the semantic diff of two saved indexes, optionally with a route diff
/// (--to) and the impact of the changed functions (--impact). `args` are
/// the flags after the verb with the common query flags already split
/// out; `entryPoints` is the query's --entry-point list (may be empty).
/// Returns a MegascopeExit code.
int runDiffVerb(llvm::ArrayRef<std::string> args, OutputFormat format,
                bool pretty, const std::vector<std::string> &entryPoints,
                llvm::raw_ostream &out, llvm::raw_ostream &err);

/// The impact-of-change CLI adapter's own flags: removes `--patch-file F`
/// (`-` reads `in`), `--git-base A`, `--git-head B`, and `--repo DIR`
/// from `args` and seeds `seed["patch"]` with the file's text or the
/// output of `git -C DIR diff -U0 A B`. Returns kExitResults, or the
/// exit code to fail with (a message was written to `err`).
int seedImpactPatch(std::vector<std::string> &args, llvm::json::Object &seed,
                    std::istream &in, llvm::raw_ostream &err);

} // namespace vycor
