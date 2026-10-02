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

#include "vycor/anneal/Report.h"

#include "vycor/anneal/CheckSet.h"
#include "vycor/callgraph/AtomicFile.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Regex.h"
#include "llvm/Support/xxhash.h"

#include <algorithm>
#include <map>
#include <set>
#include <tuple>
#include <unordered_map>

namespace vycor {

namespace {

constexpr const char *kHelpBase =
    "https://github.com/bearbones/vycor-cpp/blob/main/docs/checks/";
constexpr const char *kFingerprintKey = "vycorFingerprint/v1";

bool isBuiltinCheck(const std::string &check) {
  for (const auto &info : builtinAnnealChecks())
    if (info.name == check)
      return true;
  return false;
}

std::string builtinSummary(const std::string &check) {
  for (const auto &info : builtinAnnealChecks())
    if (info.name == check)
      return info.summary;
  return "";
}

// "path:line:col" or "path:line" (index-only checks), split from the
// right so a path may itself hold ':'. Missing parts stay 0.
void splitLocation(const std::string &loc, std::string &path, unsigned &line,
                   unsigned &column) {
  path = loc;
  line = column = 0;
  llvm::StringRef rest(loc);
  unsigned nums[2] = {0, 0};
  int found = 0;
  while (found < 2) {
    size_t colon = rest.rfind(':');
    if (colon == llvm::StringRef::npos)
      break;
    unsigned value = 0;
    if (rest.substr(colon + 1).getAsInteger(10, value))
      break;
    nums[found++] = value;
    rest = rest.substr(0, colon);
  }
  if (found == 0)
    return;
  path = rest.str();
  if (found == 2) {
    line = nums[1];
    column = nums[0];
  } else {
    line = nums[0];
  }
}

// Message text with every ":<line>[:<col>]" removed: the identity of a
// finding whose check filled no entity field.
std::string messageIdentity(const std::string &message) {
  static const llvm::Regex lineRef(":[0-9]+(:[0-9]+)?");
  std::string out = message;
  std::string previous;
  while (out != previous) {
    previous = out;
    out = lineRef.sub("", out);
  }
  return out;
}

// RFC 3986 path encoding: unreserved characters and '/' stay.
std::string uriEncodePath(llvm::StringRef path) {
  std::string out;
  for (unsigned char c : path) {
    if (llvm::isAlnum(c) || c == '-' || c == '.' || c == '_' || c == '~' ||
        c == '/')
      out.push_back(static_cast<char>(c));
    else
      out += "%" + llvm::utohexstr(c, /*LowerCase=*/false, /*Width=*/2);
  }
  return out;
}

std::string fileUri(llvm::StringRef absPath) {
  std::string p = absPath.str();
  std::replace(p.begin(), p.end(), '\\', '/');
  if (p.empty() || p.front() != '/')
    p = "/" + p;
  return "file://" + uriEncodePath(p);
}

bool lessFinding(const Finding &a, const Finding &b) {
  return std::tie(a.file, a.line, a.column, a.check, a.kind, a.message) <
         std::tie(b.file, b.line, b.column, b.check, b.kind, b.message);
}

} // namespace

// ---------------------------------------------------------------------------
// Names and severities
// ---------------------------------------------------------------------------

const char *severityName(Severity severity) {
  switch (severity) {
  case Severity::Note:
    return "note";
  case Severity::Warning:
    return "warning";
  case Severity::Error:
    return "error";
  }
  return "warning";
}

bool parseFailOn(llvm::StringRef text, std::optional<Severity> &out) {
  if (text == "none") {
    out.reset();
    return true;
  }
  if (text == "note" || text == "any") {
    out = Severity::Note;
    return true;
  }
  if (text == "warning") {
    out = Severity::Warning;
    return true;
  }
  if (text == "error") {
    out = Severity::Error;
    return true;
  }
  return false;
}

const char *diagnosticKindName(Diagnostic::Kind kind) {
  switch (kind) {
  case Diagnostic::ADL_Fallback:
    return "ADL_Fallback";
  case Diagnostic::ADL_Ambiguity:
    return "ADL_Ambiguity";
  case Diagnostic::ADL_SameScore:
    return "ADL_SameScore";
  case Diagnostic::CTAD_Fallback:
    return "CTAD_Fallback";
  case Diagnostic::Coverage_GVAMismatch:
    return "Coverage_GVAMismatch";
  case Diagnostic::Coverage_DiscardableODR:
    return "Coverage_DiscardableODR";
  case Diagnostic::Coverage_AvailableExternally:
    return "Coverage_AvailableExternally";
  case Diagnostic::Coverage_PropertyDivergence:
    return "Coverage_PropertyDivergence";
  case Diagnostic::DeadCode_Pessimistic:
    return "DeadCode_Pessimistic";
  case Diagnostic::DeadCode_Optimistic:
    return "DeadCode_Optimistic";
  case Diagnostic::ODR_DuplicateDefinition:
    return "ODR_DuplicateDefinition";
  case Diagnostic::ODR_DivergentDefinition:
    return "ODR_DivergentDefinition";
  case Diagnostic::Specialization_Invisible:
    return "Specialization_Invisible";
  case Diagnostic::DefaultArg_Divergent:
    return "DefaultArg_Divergent";
  case Diagnostic::StaticInit_OrderDependency:
    return "StaticInit_OrderDependency";
  case Diagnostic::StaticInit_Hazard:
    return "StaticInit_Hazard";
  case Diagnostic::Exception_Escape:
    return "Exception_Escape";
  case Diagnostic::HeaderStatic_Duplicated:
    return "HeaderStatic_Duplicated";
  case Diagnostic::ExceptionSpec_Divergent:
    return "ExceptionSpec_Divergent";
  case Diagnostic::Custom:
    return "Custom";
  }
  return "Unknown";
}

std::string checkNameOf(const Diagnostic &diag) {
  switch (diag.kind) {
  case Diagnostic::ADL_Fallback:
  case Diagnostic::ADL_Ambiguity:
  case Diagnostic::ADL_SameScore:
    return "adl-visibility";
  case Diagnostic::CTAD_Fallback:
    return "ctad-visibility";
  case Diagnostic::Coverage_GVAMismatch:
  case Diagnostic::Coverage_DiscardableODR:
  case Diagnostic::Coverage_AvailableExternally:
  case Diagnostic::Coverage_PropertyDivergence:
    return "coverage-properties";
  case Diagnostic::DeadCode_Pessimistic:
  case Diagnostic::DeadCode_Optimistic:
    return "dead-code";
  case Diagnostic::ODR_DuplicateDefinition:
  case Diagnostic::ODR_DivergentDefinition:
    return "odr-violations";
  case Diagnostic::Specialization_Invisible:
    return "specialization-visibility";
  case Diagnostic::DefaultArg_Divergent:
    return "default-arg-divergence";
  case Diagnostic::StaticInit_OrderDependency:
    return "static-init-order";
  case Diagnostic::StaticInit_Hazard:
    return "static-init-hazards";
  case Diagnostic::Exception_Escape:
    return "exception-escape";
  case Diagnostic::HeaderStatic_Duplicated:
    return "header-static-duplication";
  case Diagnostic::ExceptionSpec_Divergent:
    return "exception-spec-divergence";
  case Diagnostic::Custom:
    return diag.checkName.empty() ? "custom" : diag.checkName;
  }
  return "custom";
}

Severity checkSeverity(const std::string &check) {
  // Ill-formed programs (IFNDR, ODR) are errors; heuristic or
  // whole-program-approximation checks whose findings are leads are notes.
  static const std::map<std::string, Severity> table = {
      {"adl-visibility", Severity::Warning},
      {"ctad-visibility", Severity::Warning},
      {"specialization-visibility", Severity::Error},
      {"default-arg-divergence", Severity::Warning},
      {"static-init-order", Severity::Warning},
      {"static-init-hazards", Severity::Warning},
      {"header-static-duplication", Severity::Warning},
      {"exception-spec-divergence", Severity::Error},
      {"exception-escape", Severity::Note},
      {"odr-violations", Severity::Error},
      {"coverage-properties", Severity::Note},
      {"dead-code", Severity::Note},
  };
  auto it = table.find(check);
  return it == table.end() ? Severity::Warning : it->second;
}

Severity diagnosticSeverity(const Diagnostic &diag) {
  if (diag.kind == Diagnostic::ADL_SameScore ||
      diag.kind == Diagnostic::DeadCode_Optimistic)
    return Severity::Note;
  return checkSeverity(checkNameOf(diag));
}

std::string checkHelpUri(const std::string &check) {
  return isBuiltinCheck(check) ? std::string(kHelpBase) + check + ".md" : "";
}

// ---------------------------------------------------------------------------
// Paths and fingerprints
// ---------------------------------------------------------------------------

std::string absolutePath(llvm::StringRef path, llvm::StringRef base) {
  if (path.empty())
    return "";
  llvm::SmallString<256> abs(path);
  if (!llvm::sys::path::is_absolute(abs)) {
    llvm::SmallString<256> joined(base);
    llvm::sys::path::append(joined, path);
    abs = joined;
  }
  llvm::sys::path::remove_dots(abs, /*remove_dot_dot=*/true);
  return std::string(abs);
}

std::string relativeToRoot(llvm::StringRef path, llvm::StringRef root) {
  if (root.empty() || path.empty())
    return path.str();
  llvm::StringRef r = root;
  while (r.size() > 1 && r.ends_with("/"))
    r = r.drop_back();
  if (path == r)
    return ".";
  if (r == "/")
    return path.drop_front().str();
  if (path.starts_with(r) && path.size() > r.size() && path[r.size()] == '/')
    return path.substr(r.size() + 1).str();
  return path.str();
}

std::string findingFingerprint(const std::string &check,
                               const std::string &kind,
                               const std::string &file,
                               const std::vector<std::string> &identity) {
  std::string key = "vycor-finding/v1";
  key.push_back('\0');
  key += check;
  key.push_back('\0');
  key += kind;
  key.push_back('\0');
  key += file;
  for (const auto &part : identity) {
    key.push_back('\0');
    key += part;
  }
  return llvm::utohexstr(llvm::xxh3_64bits(llvm::StringRef(key)),
                         /*LowerCase=*/true, /*Width=*/16);
}

std::vector<std::string> findingIdentity(const Diagnostic &diag,
                                         llvm::StringRef projectRoot) {
  if (!diag.entities.empty())
    return diag.entities;
  if (!diag.resolvedDecl.empty() || !diag.betterDecl.empty() ||
      !diag.missingHeader.empty())
    return {diag.resolvedDecl, diag.betterDecl,
            diag.missingHeader.empty()
                ? std::string()
                : relativeToRoot(absolutePath(diag.missingHeader, projectRoot),
                                 projectRoot)};
  return {messageIdentity(diag.message)};
}

std::vector<Finding> buildFindings(const std::vector<Diagnostic> &diags,
                                   const std::string &projectRoot) {
  std::vector<Finding> findings;
  findings.reserve(diags.size());
  for (const auto &diag : diags) {
    Finding f;
    f.check = checkNameOf(diag);
    f.kind = diagnosticKindName(diag.kind);
    f.severity = diagnosticSeverity(diag);
    std::string spelled;
    splitLocation(diag.callLocation, spelled, f.line, f.column);
    f.path = absolutePath(spelled, projectRoot);
    f.file = relativeToRoot(f.path, projectRoot);
    f.message = diag.message;
    f.fingerprint = findingFingerprint(f.check, f.kind, f.file,
                                       findingIdentity(diag, projectRoot));
    findings.push_back(std::move(f));
  }
  std::sort(findings.begin(), findings.end(), [](const Finding &a,
                                                 const Finding &b) {
    if (lessFinding(a, b))
      return true;
    if (lessFinding(b, a))
      return false;
    return a.fingerprint < b.fingerprint;
  });
  // The same header finding reached from several TUs is one finding.
  findings.erase(std::unique(findings.begin(), findings.end(),
                             [](const Finding &a, const Finding &b) {
                               return !lessFinding(a, b) &&
                                      !lessFinding(b, a) &&
                                      a.fingerprint == b.fingerprint;
                             }),
                 findings.end());
  std::unordered_map<std::string, unsigned> seen;
  for (auto &f : findings) {
    unsigned n = seen[f.fingerprint]++;
    if (n)
      f.fingerprint += "-" + std::to_string(n);
  }
  return findings;
}

// ---------------------------------------------------------------------------
// Inline suppressions
// ---------------------------------------------------------------------------

std::vector<Suppression> parseSuppressions(llvm::StringRef text,
                                           const std::string &path,
                                           const std::string &file) {
  static const llvm::Regex marker(
      "(//|/\\*).*vycor:[[:space:]]*ignore\\[([^]]*)\\]");
  std::vector<Suppression> out;
  unsigned lineNo = 0;
  while (!text.empty()) {
    auto [line, rest] = text.split('\n');
    text = rest;
    ++lineNo;
    if (!line.contains("vycor:"))
      continue;
    llvm::SmallVector<llvm::StringRef, 3> groups;
    if (!marker.match(line, &groups))
      continue;
    Suppression s;
    s.path = path;
    s.file = file;
    s.line = lineNo;
    llvm::SmallVector<llvm::StringRef, 4> names;
    groups[2].split(names, ',', -1, /*KeepEmpty=*/false);
    for (auto name : names) {
      name = name.trim();
      if (!name.empty())
        s.checks.push_back(name.str());
    }
    if (!s.checks.empty())
      out.push_back(std::move(s));
  }
  return out;
}

size_t applyInlineSuppressions(std::vector<Finding> &findings,
                               const std::vector<std::string> &extraFiles,
                               const std::string &projectRoot,
                               std::vector<Suppression> &unused,
                               const FileReader &reader) {
  FileReader read = reader ? reader : [](const std::string &path,
                                         std::string &text) {
    auto buf = llvm::MemoryBuffer::getFile(path, /*IsText=*/true);
    if (!buf)
      return false;
    text = (*buf)->getBuffer().str();
    return true;
  };

  // Scan order: finding files then the extra files, each once, sorted so
  // the unused list comes out in a stable order.
  std::set<std::string> paths;
  for (const auto &f : findings)
    if (!f.path.empty())
      paths.insert(f.path);
  for (const auto &p : extraFiles)
    paths.insert(absolutePath(p, projectRoot));

  std::map<std::string, std::vector<Suppression>> byPath;
  for (const auto &path : paths) {
    std::string text;
    if (!read(path, text))
      continue;
    auto parsed =
        parseSuppressions(text, path, relativeToRoot(path, projectRoot));
    if (!parsed.empty())
      byPath[path] = std::move(parsed);
  }

  size_t removed = 0;
  std::vector<Finding> kept;
  kept.reserve(findings.size());
  for (auto &f : findings) {
    bool suppressed = false;
    auto it = byPath.find(f.path);
    if (it != byPath.end() && f.line) {
      for (auto &s : it->second) {
        if (s.line != f.line && s.line + 1 != f.line)
          continue;
        for (const auto &name : s.checks)
          if (name == "*" || name == f.check) {
            s.used = true;
            suppressed = true;
          }
      }
    }
    if (suppressed)
      ++removed;
    else
      kept.push_back(std::move(f));
  }
  findings = std::move(kept);

  unused.clear();
  for (const auto &kv : byPath)
    for (const auto &s : kv.second)
      if (!s.used)
        unused.push_back(s);
  std::sort(unused.begin(), unused.end(),
            [](const Suppression &a, const Suppression &b) {
              return std::tie(a.file, a.line) < std::tie(b.file, b.line);
            });
  return removed;
}

// ---------------------------------------------------------------------------
// Baseline
// ---------------------------------------------------------------------------

bool writeBaselineFile(const std::string &path,
                       const std::vector<Finding> &findings,
                       std::string &error) {
  std::vector<const Finding *> sorted;
  for (const auto &f : findings)
    sorted.push_back(&f);
  std::sort(sorted.begin(), sorted.end(),
            [](const Finding *a, const Finding *b) {
              return a->fingerprint < b->fingerprint;
            });
  return writeFileAtomically(
      path,
      [&](llvm::raw_ostream &os) {
        llvm::json::OStream j(os, 2);
        j.object([&] {
          j.attribute("version", 1);
          j.attribute("tool", "vycor-cpp anneal");
          j.attributeArray("findings", [&] {
            for (const auto *f : sorted)
              j.object([&] {
                j.attribute("fingerprint", f->fingerprint);
                j.attribute("check", f->check);
                j.attribute("file", f->file);
                j.attribute("message", f->message);
              });
          });
        });
        os << "\n";
      },
      &error);
}

bool parseBaseline(llvm::StringRef text, std::vector<BaselineEntry> &entries,
                   std::string &error) {
  auto parsed = llvm::json::parse(text);
  if (!parsed) {
    error = "not JSON: " + llvm::toString(parsed.takeError());
    return false;
  }
  const auto *root = parsed->getAsObject();
  if (!root) {
    error = "not a JSON object";
    return false;
  }
  auto version = root->getInteger("version");
  if (!version || *version != 1) {
    error = "unsupported baseline version (expected 1)";
    return false;
  }
  const auto *list = root->getArray("findings");
  if (!list) {
    error = "no \"findings\" array";
    return false;
  }
  entries.clear();
  for (const auto &item : *list) {
    const auto *obj = item.getAsObject();
    auto fp = obj ? obj->getString("fingerprint") : std::nullopt;
    if (!fp) {
      error = "a baseline entry has no \"fingerprint\"";
      return false;
    }
    BaselineEntry e;
    e.fingerprint = fp->str();
    if (auto v = obj->getString("check"))
      e.check = v->str();
    if (auto v = obj->getString("file"))
      e.file = v->str();
    if (auto v = obj->getString("message"))
      e.message = v->str();
    entries.push_back(std::move(e));
  }
  return true;
}

bool readBaselineFile(const std::string &path,
                      std::vector<BaselineEntry> &entries,
                      std::string &error) {
  auto buf = llvm::MemoryBuffer::getFile(path, /*IsText=*/true);
  if (!buf) {
    error = "cannot read " + path + ": " + buf.getError().message();
    return false;
  }
  if (!parseBaseline((*buf)->getBuffer(), entries, error)) {
    error = path + ": " + error;
    return false;
  }
  return true;
}

size_t applyBaseline(std::vector<Finding> &findings,
                     const std::vector<BaselineEntry> &baseline,
                     std::vector<BaselineEntry> &stale) {
  std::unordered_map<std::string, unsigned> remaining;
  for (const auto &e : baseline)
    ++remaining[e.fingerprint];
  size_t removed = 0;
  std::vector<Finding> kept;
  kept.reserve(findings.size());
  for (auto &f : findings) {
    auto it = remaining.find(f.fingerprint);
    if (it != remaining.end() && it->second > 0) {
      --it->second;
      ++removed;
    } else {
      kept.push_back(std::move(f));
    }
  }
  findings = std::move(kept);
  stale.clear();
  for (const auto &e : baseline) {
    auto it = remaining.find(e.fingerprint);
    if (it != remaining.end() && it->second > 0) {
      --it->second;
      stale.push_back(e);
    }
  }
  std::sort(stale.begin(), stale.end(),
            [](const BaselineEntry &a, const BaselineEntry &b) {
              return std::tie(a.file, a.check, a.fingerprint) <
                     std::tie(b.file, b.check, b.fingerprint);
            });
  return removed;
}

// ---------------------------------------------------------------------------
// Changed lines
// ---------------------------------------------------------------------------

namespace {

bool patchPathMatches(llvm::StringRef patchFile, llvm::StringRef absPath) {
  llvm::SmallString<256> p(patchFile);
  llvm::sys::path::remove_dots(p, /*remove_dot_dot=*/true);
  llvm::StringRef rel = p;
  if (rel.empty())
    return false;
  if (llvm::sys::path::is_absolute(rel))
    return rel == absPath;
  return absPath == rel ||
         (absPath.ends_with(rel) &&
          absPath[absPath.size() - rel.size() - 1] == '/');
}

} // namespace

size_t filterToChangedLines(std::vector<Finding> &findings,
                            const std::vector<PatchRange> &ranges) {
  size_t removed = 0;
  std::vector<Finding> kept;
  kept.reserve(findings.size());
  for (auto &f : findings) {
    bool inside = false;
    if (f.line && !f.path.empty())
      for (const auto &r : ranges) {
        if (!patchPathMatches(r.file, f.path))
          continue;
        // A pure deletion's range already spans the two lines around it
        // (parseUnifiedDiff).
        if (f.line >= r.firstLine && f.line <= r.lastLine) {
          inside = true;
          break;
        }
      }
    if (inside)
      kept.push_back(std::move(f));
    else
      ++removed;
  }
  findings = std::move(kept);
  return removed;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

const char *annealTuStatusName(TuStatus status) {
  return status == TuStatus::Indexed ? "analyzed" : tuStatusName(status);
}

size_t failedTuCount(const AnnealRun &run) {
  size_t n = 0;
  for (const auto &tu : run.tus)
    if (tu.second.status != TuStatus::Indexed)
      ++n;
  return n;
}

void renderText(const AnnealRun &run, llvm::raw_ostream &os) {
  for (const auto &f : run.findings) {
    if (!f.file.empty()) {
      os << f.file;
      if (f.line) {
        os << ":" << f.line;
        if (f.column)
          os << ":" << f.column;
      }
      os << ": ";
    }
    os << "[" << f.check << "] " << f.message << "\n";
  }
  if (run.findings.empty() && failedTuCount(run) == 0)
    os << "anneal: no issues found.\n";
}

namespace {

void writeTus(llvm::json::OStream &j, const AnnealRun &run) {
  j.attributeArray("tus", [&] {
    for (const auto &[path, outcome] : run.tus)
      j.object([&] {
        j.attribute("file", relativeToRoot(path, run.projectRoot));
        j.attribute("status", annealTuStatusName(outcome.status));
        if (!outcome.detail.empty() && outcome.status != TuStatus::Indexed)
          j.attribute("detail", outcome.detail);
      });
  });
}

} // namespace

void renderJson(const AnnealRun &run, llvm::raw_ostream &os) {
  llvm::json::OStream j(os, 2);
  const size_t failed = failedTuCount(run);
  j.object([&] {
    j.attribute("version", 1);
    j.attributeObject("summary", [&] {
      j.attribute("tus", static_cast<int64_t>(run.tus.size()));
      j.attribute("analyzed", static_cast<int64_t>(run.tus.size() - failed));
      j.attribute("failed", static_cast<int64_t>(failed));
      j.attribute("findings", static_cast<int64_t>(run.findings.size()));
      j.attribute("suppressed", static_cast<int64_t>(run.suppressed));
      if (run.baselineUsed) {
        j.attribute("baselined", static_cast<int64_t>(run.baselined));
        j.attribute("staleBaseline",
                    static_cast<int64_t>(run.staleBaseline.size()));
      }
      if (run.changedLinesUsed)
        j.attribute("outsideChanges",
                    static_cast<int64_t>(run.outsideChanges));
    });
    writeTus(j, run);
    j.attributeArray("findings", [&] {
      for (const auto &f : run.findings)
        j.object([&] {
          j.attribute("check", f.check);
          j.attribute("kind", f.kind);
          j.attribute("severity", severityName(f.severity));
          j.attribute("file", f.file);
          j.attribute("line", static_cast<int64_t>(f.line));
          j.attribute("column", static_cast<int64_t>(f.column));
          j.attribute("message", f.message);
          j.attribute("fingerprint", f.fingerprint);
        });
    });
    if (run.baselineUsed)
      j.attributeArray("staleBaseline", [&] {
        for (const auto &e : run.staleBaseline)
          j.object([&] {
            j.attribute("fingerprint", e.fingerprint);
            j.attribute("check", e.check);
            j.attribute("file", e.file);
            j.attribute("message", e.message);
          });
      });
    if (run.reportUnusedSuppressions)
      j.attributeArray("unusedSuppressions", [&] {
        for (const auto &s : run.unusedSuppressions)
          j.object([&] {
            j.attribute("file", s.file);
            j.attribute("line", static_cast<int64_t>(s.line));
            j.attributeArray("checks", [&] {
              for (const auto &c : s.checks)
                j.value(c);
            });
          });
      });
  });
  os << "\n";
}

void renderSarif(const AnnealRun &run, llvm::raw_ostream &os) {
  // One rule per enabled check, plus any check a finding names that the
  // enabled set does not (an organization check reporting under another
  // name): sorted by id, so ruleIndex is stable.
  std::set<std::string> ruleSet(run.checks.begin(), run.checks.end());
  for (const auto &f : run.findings)
    ruleSet.insert(f.check);
  std::vector<std::string> rules(ruleSet.begin(), ruleSet.end());
  std::unordered_map<std::string, size_t> ruleIndex;
  for (size_t i = 0; i < rules.size(); ++i)
    ruleIndex[rules[i]] = i;

  std::string rootUri = fileUri(run.projectRoot);
  if (rootUri.back() != '/')
    rootUri += "/";

  auto artifactLocation = [&](llvm::json::OStream &j,
                              const std::string &absPath) {
    std::string rel = relativeToRoot(absPath, run.projectRoot);
    j.attributeObject("artifactLocation", [&] {
      if (rel != absPath) {
        j.attribute("uri", uriEncodePath(rel));
        j.attribute("uriBaseId", "%SRCROOT%");
      } else {
        j.attribute("uri", fileUri(absPath));
      }
    });
  };

  const size_t failed = failedTuCount(run);
  llvm::json::OStream j(os, 2);
  j.object([&] {
    j.attribute("$schema", "https://json.schemastore.org/sarif-2.1.0.json");
    j.attribute("version", "2.1.0");
    j.attributeArray("runs", [&] {
      j.object([&] {
        j.attributeObject("tool", [&] {
          j.attributeObject("driver", [&] {
            j.attribute("name", "vycor-cpp anneal");
            if (!run.toolVersion.empty())
              j.attribute("version", run.toolVersion);
            j.attribute("informationUri",
                        "https://github.com/bearbones/vycor-cpp");
            j.attributeArray("rules", [&] {
              for (const auto &id : rules)
                j.object([&] {
                  j.attribute("id", id);
                  j.attribute("name", id);
                  std::string summary = builtinSummary(id);
                  j.attributeObject("shortDescription", [&] {
                    j.attribute("text", summary.empty()
                                            ? "Organization check " + id
                                            : summary);
                  });
                  std::string help = checkHelpUri(id);
                  if (!help.empty())
                    j.attribute("helpUri", help);
                  j.attributeObject("defaultConfiguration", [&] {
                    j.attribute("level", severityName(checkSeverity(id)));
                  });
                });
            });
          });
        });
        j.attributeObject("originalUriBaseIds", [&] {
          j.attributeObject("%SRCROOT%",
                            [&] { j.attribute("uri", rootUri); });
        });
        j.attributeArray("invocations", [&] {
          j.object([&] {
            j.attribute("executionSuccessful", failed == 0);
            j.attributeArray("toolExecutionNotifications", [&] {
              for (const auto &[path, outcome] : run.tus) {
                if (outcome.status == TuStatus::Indexed)
                  continue;
                j.object([&] {
                  j.attribute("level", "error");
                  j.attributeObject("message", [&] {
                    std::string text =
                        "TU failed (" +
                        std::string(annealTuStatusName(outcome.status));
                    if (!outcome.detail.empty())
                      text += ": " + outcome.detail;
                    text += "): " + relativeToRoot(path, run.projectRoot);
                    j.attribute("text", text);
                  });
                  j.attributeArray("locations", [&] {
                    j.object([&] {
                      j.attributeObject("physicalLocation", [&] {
                        artifactLocation(j, path);
                      });
                    });
                  });
                });
              }
            });
          });
        });
        j.attributeArray("results", [&] {
          for (const auto &f : run.findings)
            j.object([&] {
              j.attribute("ruleId", f.check);
              j.attribute("ruleIndex",
                          static_cast<int64_t>(ruleIndex[f.check]));
              j.attribute("level", severityName(f.severity));
              j.attributeObject("message",
                                [&] { j.attribute("text", f.message); });
              if (!f.path.empty())
                j.attributeArray("locations", [&] {
                  j.object([&] {
                    j.attributeObject("physicalLocation", [&] {
                      artifactLocation(j, f.path);
                      if (f.line)
                        j.attributeObject("region", [&] {
                          j.attribute("startLine",
                                      static_cast<int64_t>(f.line));
                          if (f.column)
                            j.attribute("startColumn",
                                        static_cast<int64_t>(f.column));
                        });
                    });
                  });
                });
              j.attributeObject("partialFingerprints", [&] {
                j.attribute(kFingerprintKey, f.fingerprint);
              });
              j.attributeObject("properties",
                                [&] { j.attribute("kind", f.kind); });
            });
        });
      });
    });
  });
  os << "\n";
}

void renderSummary(const AnnealRun &run, llvm::raw_ostream &err) {
  const size_t failed = failedTuCount(run);
  err << "anneal: " << run.tus.size() << " TU(s): "
      << (run.tus.size() - failed) << " analyzed, " << failed << " failed\n";
  for (const auto &[path, outcome] : run.tus) {
    if (outcome.status == TuStatus::Indexed)
      continue;
    err << "anneal:   failed: " << relativeToRoot(path, run.projectRoot)
        << " (" << annealTuStatusName(outcome.status);
    if (!outcome.detail.empty())
      err << ": " << outcome.detail;
    err << ")\n";
  }
  err << "anneal: " << run.findings.size() << " finding(s) reported";
  if (run.suppressed)
    err << "; " << run.suppressed << " suppressed inline";
  if (run.baselineUsed)
    err << "; " << run.baselined << " in the baseline";
  if (run.changedLinesUsed)
    err << "; " << run.outsideChanges << " outside the changed lines";
  err << "\n";
  if (run.baselineUsed && !run.staleBaseline.empty()) {
    err << "anneal: " << run.staleBaseline.size()
        << " baseline entr" << (run.staleBaseline.size() == 1 ? "y" : "ies")
        << " no longer found (stale; rewrite the baseline with "
           "--write-baseline to drop):\n";
    for (const auto &e : run.staleBaseline)
      err << "anneal:   " << e.fingerprint << " [" << e.check << "] "
          << e.file << "\n";
  }
  if (run.reportUnusedSuppressions)
    for (const auto &s : run.unusedSuppressions) {
      err << s.file << ":" << s.line << ": unused suppression ignore[";
      for (size_t i = 0; i < s.checks.size(); ++i)
        err << (i ? "," : "") << s.checks[i];
      err << "]\n";
    }
}

int annealExitCode(const AnnealRun &run, std::optional<Severity> failOn,
                   bool allowParseFailures) {
  if (!allowParseFailures && failedTuCount(run) > 0)
    return kAnnealExitParseFailures;
  if (failOn)
    for (const auto &f : run.findings)
      if (f.severity >= *failOn)
        return kAnnealExitFindings;
  return kAnnealExitClean;
}

} // namespace vycor
