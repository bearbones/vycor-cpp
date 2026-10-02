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
#include "vycor/callgraph/TuOutcome.h"
#include "vycor/impact/PatchMapping.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// ============================================================================
// anneal's report: what one run found, in the shape a CI gate consumes.
//
// runAnalysis returns raw Diagnostics; this module turns them into
// Findings (check name, severity, location relative to the project root,
// a stable fingerprint), applies inline suppressions, a baseline, and the
// changed-lines filter, renders text / JSON / SARIF 2.1.0, and derives the
// exit code. Contract: docs/result-contract.md ("anneal exit codes") and
// docs/checks/README.md ("Finding identity").
//
// Fingerprint (version 1): the first 16 hex digits of xxh3-64 over
//   "vycor-finding/v1", check name, kind, file (relative to the project
//   root, '/'-separated), identity...
// NUL-separated. The identity is Diagnostic::entities when the check
// filled it; otherwise resolvedDecl, betterDecl, and missingHeader
// (relative to the root) when any is set; otherwise the message with every
// ":<line>[:<col>]" removed and paths under the root made relative. A
// call-site finding's enclosing function (Diagnostic::scope) is appended.
// No line number enters, so inserting unrelated lines leaves it
// unchanged; renaming an entity involved changes it.
// Findings that still share a fingerprint (the same fragile call twice in
// one function) keep it: a fingerprint names a kind of finding, not one
// occurrence, and the baseline counts occurrences per fingerprint.
// ============================================================================

namespace vycor {

enum class Severity : uint8_t { Note = 0, Warning = 1, Error = 2 };

/// "note", "warning", "error" (also the SARIF `level` spellings).
const char *severityName(Severity severity);

/// `--fail-on`: note (= any finding), warning, error, or none (findings
/// never fail the run, `out` left empty). False for anything else.
bool parseFailOn(llvm::StringRef text, std::optional<Severity> &out);

/// The enum spelling of a diagnostic kind ("ADL_Fallback").
const char *diagnosticKindName(Diagnostic::Kind kind);

/// The named check a diagnostic belongs to (docs/checks/<name>.md): the
/// built-in check of its kind, or checkName for an organization check.
std::string checkNameOf(const Diagnostic &diag);

/// A check's default severity. Organization checks are warnings.
Severity checkSeverity(const std::string &check);

/// The severity of one diagnostic: its check's, except ADL_SameScore (a
/// tie, opt-in) and DeadCode_Optimistic (plausible paths only), which are
/// notes.
Severity diagnosticSeverity(const Diagnostic &diag);

/// The SARIF helpUri of a built-in check; "" for an organization check.
std::string checkHelpUri(const std::string &check);

struct Finding {
  std::string check;
  std::string kind;
  Severity severity = Severity::Warning;
  std::string path; // absolute, dot-free ("" when the diagnostic had none)
  std::string file; // display: relative to the project root when under it
  unsigned line = 0;
  unsigned column = 0;
  std::string message;
  std::string fingerprint;
};

/// `path` relative to `root` ('/'-separated) when it lies under it,
/// otherwise `path` unchanged. Both absolute and dot-free.
std::string relativeToRoot(llvm::StringRef path, llvm::StringRef root);

/// Absolute and dot-free; a relative path is taken against `base`.
std::string absolutePath(llvm::StringRef path, llvm::StringRef base);

/// The fingerprint of one finding before any collision suffix.
std::string findingFingerprint(const std::string &check,
                               const std::string &kind,
                               const std::string &file,
                               const std::vector<std::string> &identity);

/// The identity inputs of a diagnostic (see the header comment).
std::vector<std::string> findingIdentity(const Diagnostic &diag,
                                         llvm::StringRef projectRoot);

/// Findings for `diags`: located, fingerprinted, exact duplicates (the same
/// header finding reported from several TUs) merged, sorted by (file, line,
/// column, check, kind, message). `projectRoot` is absolute and dot-free;
/// a relative location (the analyzer records absolute ones) is taken
/// against it.
std::vector<Finding> buildFindings(const std::vector<Diagnostic> &diags,
                                   const std::string &projectRoot);

// ---- inline suppressions ---------------------------------------------------

/// `// vycor: ignore[check-a, check-b]` (or `ignore[*]`) in a comment on
/// a finding's line, or alone on the line above, suppresses it.
struct Suppression {
  std::string path; // absolute
  std::string file; // display
  unsigned line = 0;
  // The comment is alone on its line, so it also covers the next line
  // (a trailing `code; // vycor: ignore[...]` covers its own line only).
  bool ownLine = true;
  std::vector<std::string> checks; // "*" = every check
  bool used = false;
};

/// The suppressions in one file's text, in line order.
std::vector<Suppression> parseSuppressions(llvm::StringRef text,
                                           const std::string &path,
                                           const std::string &file);

/// Reads a file's text; false when it cannot.
using FileReader =
    std::function<bool(const std::string &path, std::string &text)>;

/// Remove the findings an inline suppression covers. Every file holding a
/// finding is scanned, plus `extraFiles` (the analyzed TUs), so the
/// returned list of unused suppressions covers them too. Returns the
/// number of findings removed.
size_t applyInlineSuppressions(std::vector<Finding> &findings,
                               const std::vector<std::string> &extraFiles,
                               const std::string &projectRoot,
                               std::vector<Suppression> &unused,
                               const FileReader &reader = nullptr);

// ---- baseline --------------------------------------------------------------

struct BaselineEntry {
  std::string fingerprint;
  unsigned count = 1; // findings carrying this fingerprint
  std::string check;
  std::string file;
  std::string message;
};

/// {"version": 2, "findings": [{fingerprint, count, check, file,
/// message}...]}: one entry per distinct fingerprint, sorted by it,
/// written atomically. The reader also takes version 1 (one entry per
/// finding, colliding fingerprints suffixed "-1", "-2", ...): suffixes are
/// dropped and entries merged, so `entries` holds each fingerprint once.
bool writeBaselineFile(const std::string &path,
                       const std::vector<Finding> &findings,
                       std::string &error);
bool readBaselineFile(const std::string &path,
                      std::vector<BaselineEntry> &entries, std::string &error);
bool parseBaseline(llvm::StringRef text, std::vector<BaselineEntry> &entries,
                   std::string &error);

/// The baseline is a multiset of fingerprints: per fingerprint, when the
/// run has N findings and the baseline count is B, max(N - B, 0) of them
/// are reported (new) and the rest removed. Which of identical findings is
/// reported is chosen without line order: those on `changedLines` (when
/// given) first, then report order. A fingerprint with B > N goes to
/// `stale` with count B - N. Returns the number removed.
size_t applyBaseline(std::vector<Finding> &findings,
                     const std::vector<BaselineEntry> &baseline,
                     std::vector<BaselineEntry> &stale,
                     const std::vector<PatchRange> *changedLines = nullptr);

/// Total stale occurrences (the sum of the entries' counts).
size_t staleBaselineCount(const std::vector<BaselineEntry> &stale);

// ---- changed lines ---------------------------------------------------------

/// Keep only the findings whose location falls in a changed after-side
/// range (parseUnifiedDiff gives a pure deletion the two lines around it).
/// A patch path
/// matches a finding's absolute path by equality or as a trailing run of
/// path components. Returns the number removed.
size_t filterToChangedLines(std::vector<Finding> &findings,
                            const std::vector<PatchRange> &ranges);

// ---- the run and its rendering ---------------------------------------------

struct AnnealRun {
  std::string projectRoot;                         // absolute, dot-free
  std::vector<std::pair<std::string, TuOutcome>> tus; // absolute, source order
  std::vector<Finding> findings;                   // what is reported
  std::vector<std::string> checks; // the enabled checks (SARIF rules)
  size_t suppressed = 0;
  size_t baselined = 0;
  size_t outsideChanges = 0;
  bool baselineUsed = false;
  bool changedLinesUsed = false;
  std::vector<BaselineEntry> staleBaseline;
  bool reportUnusedSuppressions = false; // -v
  std::vector<Suppression> unusedSuppressions;
  std::string toolVersion; // SARIF tool.driver.version
};

/// "analyzed" for a clean TU, otherwise tuStatusName ("partial",
/// "crashed", "poisoned", "skipped", "timeout").
const char *annealTuStatusName(TuStatus status);

size_t failedTuCount(const AnnealRun &run);

/// `file:line:col: [check] message` per finding; "anneal: no issues
/// found." only when there is no finding AND every TU was analyzed.
void renderText(const AnnealRun &run, llvm::raw_ostream &os);
void renderJson(const AnnealRun &run, llvm::raw_ostream &os);
void renderSarif(const AnnealRun &run, llvm::raw_ostream &os);

/// The stderr summary: TU counts naming each failed TU, what the
/// suppressions / baseline / changed-lines filter removed, stale baseline
/// entries, and (when requested) unused suppressions.
void renderSummary(const AnnealRun &run, llvm::raw_ostream &err);

constexpr int kAnnealExitClean = 0;
constexpr int kAnnealExitFindings = 1;
constexpr int kAnnealExitUsage = 2;
constexpr int kAnnealExitParseFailures = 3;

/// 3 when a TU failed and parse failures are not allowed, else 1 when a
/// finding is at or above `failOn` (no failOn: never), else 0.
int annealExitCode(const AnnealRun &run, std::optional<Severity> failOn,
                   bool allowParseFailures);

} // namespace vycor
