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

// test_anneal_report.cpp — anneal's CI-gate report (anneal/Report.h):
// finding identity, inline suppressions, baselines, the changed-lines
// filter, the renderers, and the exit codes. The end-to-end CLI side
// (real binary, SARIF schema, execution-mode identity) is
// scripts/anneal-gate-check.py (ctest anneal_gate).

#include "vycor/anneal/Analyzer.h"
#include "vycor/anneal/CheckSet.h"
#include "vycor/anneal/Report.h"
#include "vycor/ext/Extensions.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <string>
#include <vector>

using namespace vycor;

namespace {

const std::string kRoot = "/work/proj";

Diagnostic adlAt(const std::string &loc,
                 const std::string &resolved = "M::scale(M::V, int)",
                 const std::string &better = "M::scale(M::V, double)") {
  Diagnostic d;
  d.kind = Diagnostic::ADL_Fallback;
  d.callLocation = loc;
  d.resolvedDecl = resolved;
  d.betterDecl = better;
  d.missingHeader = kRoot + "/include/ext.hpp";
  d.message = "Fragile ADL resolution: " + better;
  return d;
}

FileReader readerFor(std::map<std::string, std::string> files) {
  return [files](const std::string &path, std::string &text) {
    auto it = files.find(path);
    if (it == files.end())
      return false;
    text = it->second;
    return true;
  };
}

std::string render(void (*fn)(const AnnealRun &, llvm::raw_ostream &),
                   const AnnealRun &run) {
  std::string out;
  llvm::raw_string_ostream os(out);
  fn(run, os);
  os.flush();
  return out;
}

} // namespace

// ---- finding identity -------------------------------------------------------

TEST_CASE("Every built-in check maps to a kind, a severity, and a help page",
          "[AnnealReport]") {
  for (const auto &info : builtinAnnealChecks())
    CHECK(checkHelpUri(info.name) ==
          "https://github.com/bearbones/vycor-cpp/blob/main/docs/checks/" +
              info.name + ".md");
  CHECK(checkSeverity("odr-violations") == Severity::Error);
  CHECK(checkSeverity("adl-visibility") == Severity::Warning);
  CHECK(checkSeverity("dead-code") == Severity::Note);
  CHECK(checkSeverity("some-org-check") == Severity::Warning);
  CHECK(checkHelpUri("some-org-check").empty());

  // checkNameOf only names known checks (or the org check's own name).
  for (int k = Diagnostic::ADL_Fallback; k < Diagnostic::Custom; ++k) {
    Diagnostic d;
    d.kind = static_cast<Diagnostic::Kind>(k);
    bool known = false;
    for (const auto &info : builtinAnnealChecks())
      known |= info.name == checkNameOf(d);
    CHECK(known);
    CHECK(std::string(diagnosticKindName(d.kind)) != "Unknown");
  }
  Diagnostic custom;
  custom.kind = Diagnostic::Custom;
  custom.checkName = "myorg-no-legacy";
  CHECK(checkNameOf(custom) == "myorg-no-legacy");
}

TEST_CASE("Fingerprints ignore line numbers and follow the entities",
          "[AnnealReport]") {
  auto fp = [](const Diagnostic &d) {
    auto findings = buildFindings({d}, kRoot);
    REQUIRE(findings.size() == 1);
    return findings[0].fingerprint;
  };
  const std::string base = fp(adlAt(kRoot + "/src/use.cpp:4:3"));
  CHECK(base.size() == 16);

  SECTION("unrelated lines inserted above: same fingerprint") {
    CHECK(fp(adlAt(kRoot + "/src/use.cpp:9:3")) == base);
    CHECK(fp(adlAt(kRoot + "/src/use.cpp:9:7")) == base);
  }
  SECTION("a different entity: a different fingerprint") {
    CHECK(fp(adlAt(kRoot + "/src/use.cpp:4:3", "M::scale(M::W, int)")) !=
          base);
    CHECK(fp(adlAt(kRoot + "/src/use.cpp:4:3", "M::scale(M::V, int)",
                   "M::scale(M::V, float)")) != base);
  }
  SECTION("another file: a different fingerprint") {
    CHECK(fp(adlAt(kRoot + "/src/other.cpp:4:3")) != base);
  }
  SECTION("the same project under another root: the same fingerprint") {
    Diagnostic moved = adlAt("/elsewhere/proj/src/use.cpp:4:3");
    moved.missingHeader = "/elsewhere/proj/include/ext.hpp";
    auto findings = buildFindings({moved}, "/elsewhere/proj");
    REQUIRE(findings.size() == 1);
    CHECK(findings[0].fingerprint == base);
    CHECK(findings[0].file == "src/use.cpp");
  }
  SECTION("entities, when a check sets them, are the identity") {
    Diagnostic odr;
    odr.kind = Diagnostic::ODR_DuplicateDefinition;
    odr.callLocation = kRoot + "/a.hpp:3";
    odr.entities = {"dup|int ()|f"};
    odr.message = "ODR violation: 'dup' has 2 definitions (a.hpp:3, b.hpp:7)";
    Diagnostic moved = odr;
    moved.callLocation = kRoot + "/a.hpp:30";
    moved.message = "ODR violation: 'dup' has 3 definitions (a.hpp:30, ...)";
    CHECK(fp(odr) == fp(moved));
    moved.entities = {"dup2|int ()|f"};
    CHECK(fp(odr) != fp(moved));
  }
  SECTION("with no entity field, the message minus line references") {
    Diagnostic org;
    org.kind = Diagnostic::Custom;
    org.checkName = "myorg-check";
    org.callLocation = kRoot + "/a.cpp:3:1";
    org.message = "bad call to f() declared at b.hpp:12:4";
    Diagnostic moved = org;
    moved.callLocation = kRoot + "/a.cpp:30:1";
    moved.message = "bad call to f() declared at b.hpp:99:4";
    CHECK(fp(org) == fp(moved));
    moved.message = "bad call to g() declared at b.hpp:99:4";
    CHECK(fp(org) != fp(moved));
  }
}

TEST_CASE("buildFindings sorts, merges duplicates, and numbers collisions",
          "[AnnealReport]") {
  std::vector<Diagnostic> diags = {
      adlAt(kRoot + "/src/use.cpp:20:3"),
      adlAt(kRoot + "/src/use.cpp:4:3"),
      adlAt(kRoot + "/src/use.cpp:4:3"), // the same header finding, twice
      adlAt("/outside/x.cpp:1:1"),
  };
  Diagnostic noCol;
  noCol.kind = Diagnostic::HeaderStatic_Duplicated;
  noCol.callLocation = kRoot + "/include/state.hpp:7";
  noCol.entities = {"counter"};
  noCol.message = "Header-static duplication";
  diags.push_back(noCol);

  auto findings = buildFindings(diags, kRoot);
  REQUIRE(findings.size() == 4);
  CHECK(findings[0].file == "/outside/x.cpp");
  CHECK(findings[1].file == "include/state.hpp");
  CHECK(findings[1].line == 7);
  CHECK(findings[1].column == 0);
  CHECK(findings[1].check == "header-static-duplication");
  CHECK(findings[2].file == "src/use.cpp");
  CHECK(findings[2].line == 4);
  CHECK(findings[2].column == 3);
  CHECK(findings[3].line == 20);
  // The two calls in use.cpp share an identity: the second gets "-1".
  CHECK(findings[3].fingerprint == findings[2].fingerprint + "-1");
  CHECK(findings[2].severity == Severity::Warning);
  CHECK(findings[2].kind == "ADL_Fallback");
}

// ---- suppressions -------------------------------------------------------

TEST_CASE("Inline suppressions: same line, line above, lists, and '*'",
          "[AnnealReport]") {
  auto parsed = parseSuppressions(
      "int a; // vycor: ignore[adl-visibility]\n"
      "/* vycor:ignore[ dead-code , odr-violations ] */\n"
      "// vycor: ignore[*]\n"
      "const char *s = \"vycor: ignore[adl-visibility]\";\n"
      "// vycor: ignore[]\n",
      "/p/a.cpp", "a.cpp");
  REQUIRE(parsed.size() == 3);
  CHECK(parsed[0].line == 1);
  CHECK(parsed[0].checks == std::vector<std::string>{"adl-visibility"});
  CHECK(parsed[1].checks ==
        std::vector<std::string>{"dead-code", "odr-violations"});
  CHECK(parsed[2].checks == std::vector<std::string>{"*"});

  std::string use = "void use() {\n"                         // 1
                    "  // vycor: ignore[adl-visibility]\n"   // 2
                    "  scale(v, 3.14);\n"                    // 3
                    "  scale(v, 2.5); // vycor: ignore[*]\n" // 4
                    "\n"                                     // 5
                    "  // vycor: ignore[adl-visibility]\n"   // 6
                    "\n"                                     // 7
                    "  scale(v, 1.5);\n"                     // 8
                    "  // vycor: ignore[dead-code]\n"        // 9
                    "  scale(v, 0.5);\n"                     // 10
                    "}\n";
  std::vector<Diagnostic> diags;
  for (const char *loc : {":3:3", ":4:3", ":8:3", ":10:3"})
    diags.push_back(adlAt(kRoot + "/use.cpp" + loc));
  auto findings = buildFindings(diags, kRoot);
  std::vector<Suppression> unused;
  size_t removed = applyInlineSuppressions(
      findings, {kRoot + "/other.cpp"}, kRoot, unused,
      readerFor({{kRoot + "/use.cpp", use},
                 {kRoot + "/other.cpp", "// vycor: ignore[ctad-visibility]\n"}}));
  CHECK(removed == 2);
  REQUIRE(findings.size() == 2);
  CHECK(findings[0].line == 8);  // the suppression is two lines up
  CHECK(findings[1].line == 10); // a different check is named
  REQUIRE(unused.size() == 3);
  CHECK(unused[0].file == "other.cpp");
  CHECK(unused[1].file == "use.cpp");
  CHECK(unused[1].line == 6);
  CHECK(unused[2].line == 9);
}

// ---- baseline -----------------------------------------------------------

TEST_CASE("Baseline: round trip, new findings, and stale entries",
          "[AnnealReport]") {
  auto findings = buildFindings({adlAt(kRoot + "/a.cpp:4:3"),
                                 adlAt(kRoot + "/b.cpp:4:3")},
                                kRoot);
  std::string text;
  {
    // The file shape writeBaselineFile writes.
    llvm::json::Array list;
    for (const auto &f : findings)
      list.push_back(llvm::json::Object{{"fingerprint", f.fingerprint},
                                        {"check", f.check},
                                        {"file", f.file},
                                        {"message", f.message}});
    text = llvm::formatv(
               "{0}", llvm::json::Value(llvm::json::Object{
                          {"version", 1}, {"findings", std::move(list)}}))
               .str();
  }
  std::vector<BaselineEntry> baseline;
  std::string error;
  REQUIRE(parseBaseline(text, baseline, error));
  REQUIRE(baseline.size() == 2);

  SECTION("everything baselined: nothing reported, nothing stale") {
    auto current = findings;
    std::vector<BaselineEntry> stale;
    CHECK(applyBaseline(current, baseline, stale) == 2);
    CHECK(current.empty());
    CHECK(stale.empty());
  }
  SECTION("a new finding is reported, a fixed one is stale") {
    auto current = buildFindings({adlAt(kRoot + "/a.cpp:40:3"),
                                  adlAt(kRoot + "/c.cpp:4:3")},
                                 kRoot);
    std::vector<BaselineEntry> stale;
    CHECK(applyBaseline(current, baseline, stale) == 1);
    REQUIRE(current.size() == 1);
    CHECK(current[0].file == "c.cpp");
    REQUIRE(stale.size() == 1);
    CHECK(stale[0].file == "b.cpp");
  }
  SECTION("malformed baselines are refused") {
    CHECK_FALSE(parseBaseline("[]", baseline, error));
    CHECK_FALSE(parseBaseline("{\"version\": 2, \"findings\": []}", baseline,
                              error));
    CHECK_FALSE(parseBaseline("{\"version\": 1}", baseline, error));
    CHECK_FALSE(parseBaseline("{\"version\": 1, \"findings\": [{}]}",
                              baseline, error));
    CHECK_FALSE(parseBaseline("not json", baseline, error));
  }
}

// ---- changed lines ------------------------------------------------------

TEST_CASE("The changed-lines filter keeps findings inside changed hunks",
          "[AnnealReport]") {
  auto ranges = parseUnifiedDiff("diff --git a/src/use.cpp b/src/use.cpp\n"
                                 "--- a/src/use.cpp\n+++ b/src/use.cpp\n"
                                 "@@ -4,1 +4,2 @@\n-x\n+y\n+z\n"
                                 "@@ -20,2 +21,0 @@\n-a\n-b\n");
  auto findings = buildFindings({adlAt(kRoot + "/src/use.cpp:3:1"),
                                 adlAt(kRoot + "/src/use.cpp:5:1"),
                                 adlAt(kRoot + "/src/use.cpp:22:1"),
                                 adlAt(kRoot + "/src/use.cpp:30:1"),
                                 adlAt(kRoot + "/other/use.cpp:5:1"),
                                 adlAt(kRoot + "/xsrc/use.cpp:5:1")},
                                kRoot);
  CHECK(filterToChangedLines(findings, ranges) == 4);
  REQUIRE(findings.size() == 2);
  CHECK(findings[0].line == 5);  // in the added lines 4-5
  CHECK(findings[1].line == 22); // next to the deletion after line 21
  CHECK(findings[0].file == "src/use.cpp");
}

// ---- rendering and exit codes -------------------------------------------

TEST_CASE("Text output names the check; 'no issues found' needs every TU",
          "[AnnealReport]") {
  AnnealRun run;
  run.projectRoot = kRoot;
  run.tus = {{kRoot + "/a.cpp", {TuStatus::Indexed, ""}}};
  CHECK(render(renderText, run) == "anneal: no issues found.\n");

  run.tus.push_back({kRoot + "/b.cpp", {TuStatus::Partial, "parse errors"}});
  CHECK(render(renderText, run).empty());

  run.findings = buildFindings({adlAt(kRoot + "/a.cpp:4:3")}, kRoot);
  CHECK(render(renderText, run) ==
        "a.cpp:4:3: [adl-visibility] Fragile ADL resolution: "
        "M::scale(M::V, double)\n");

  std::string summary = render(renderSummary, run);
  CHECK(summary.find("2 TU(s): 1 analyzed, 1 failed") != std::string::npos);
  CHECK(summary.find("failed: b.cpp (partial: parse errors)") !=
        std::string::npos);
}

TEST_CASE("Exit codes: parse failures, then findings at --fail-on",
          "[AnnealReport]") {
  AnnealRun run;
  run.projectRoot = kRoot;
  run.tus = {{kRoot + "/a.cpp", {TuStatus::Indexed, ""}}};
  std::optional<Severity> any = Severity::Note;
  CHECK(annealExitCode(run, any, false) == kAnnealExitClean);

  run.findings = buildFindings({adlAt(kRoot + "/a.cpp:4:3")}, kRoot);
  CHECK(annealExitCode(run, any, false) == kAnnealExitFindings);
  CHECK(annealExitCode(run, Severity::Warning, false) == kAnnealExitFindings);
  CHECK(annealExitCode(run, Severity::Error, false) == kAnnealExitClean);
  CHECK(annealExitCode(run, std::nullopt, false) == kAnnealExitClean);

  run.tus.push_back({kRoot + "/b.cpp", {TuStatus::TimedOut, "worker timed out"}});
  CHECK(annealExitCode(run, any, false) == kAnnealExitParseFailures);
  CHECK(annealExitCode(run, any, true) == kAnnealExitFindings);

  std::optional<Severity> parsed;
  CHECK(parseFailOn("none", parsed));
  CHECK(!parsed);
  CHECK(parseFailOn("error", parsed));
  CHECK(parsed == Severity::Error);
  CHECK_FALSE(parseFailOn("fatal", parsed));
}

TEST_CASE("JSON and SARIF carry fingerprints, rules, and TU outcomes",
          "[AnnealReport]") {
  AnnealRun run;
  run.projectRoot = kRoot;
  run.tus = {{kRoot + "/a.cpp", {TuStatus::Indexed, ""}},
             {kRoot + "/b c.cpp", {TuStatus::Partial, "parse errors"}}};
  run.checks = {"adl-visibility", "dead-code"};
  run.findings = buildFindings({adlAt(kRoot + "/a.cpp:4:3")}, kRoot);

  auto json = llvm::json::parse(render(renderJson, run));
  REQUIRE(bool(json));
  const auto *doc = json->getAsObject();
  CHECK(doc->getObject("summary")->getInteger("failed") == 1);
  CHECK((*doc->getArray("tus"))[1].getAsObject()->getString("status") ==
        "partial");
  const auto *f = (*doc->getArray("findings"))[0].getAsObject();
  CHECK(f->getString("check") == "adl-visibility");
  CHECK(f->getString("fingerprint") == run.findings[0].fingerprint);

  auto sarif = llvm::json::parse(render(renderSarif, run));
  REQUIRE(bool(sarif));
  const auto *sarifRun =
      (*sarif->getAsObject()->getArray("runs"))[0].getAsObject();
  const auto *rules =
      sarifRun->getObject("tool")->getObject("driver")->getArray("rules");
  REQUIRE(rules->size() == 2);
  CHECK((*rules)[1].getAsObject()->getString("id") == "dead-code");
  const auto *result = (*sarifRun->getArray("results"))[0].getAsObject();
  CHECK(result->getInteger("ruleIndex") == 0);
  CHECK(result->getObject("partialFingerprints")
            ->getString("vycorFingerprint/v1") == run.findings[0].fingerprint);
  const auto *notification =
      (*(*sarifRun->getArray("invocations"))[0]
            .getAsObject()
            ->getArray("toolExecutionNotifications"))[0]
          .getAsObject();
  const auto *loc = (*notification->getArray("locations"))[0]
                        .getAsObject()
                        ->getObject("physicalLocation")
                        ->getObject("artifactLocation");
  CHECK(loc->getString("uri") == "b%20c.cpp"); // percent-encoded
  CHECK(loc->getString("uriBaseId") == "%SRCROOT%");
}

// ---- organization checks ------------------------------------------------

namespace {

struct RegistryReset {
  RegistryReset() { ExtensionRegistry::instance().clear(); }
  ~RegistryReset() { ExtensionRegistry::instance().clear(); }
};

// An organization check that does not fill checkName: the analyzer
// attributes its diagnostics to name() anyway.
class NoLegacyCallCheck : public AnnealCheck {
public:
  explicit NoLegacyCallCheck(std::string name) : name_(std::move(name)) {}
  std::string name() const override { return name_; }
  void checkTU(clang::ASTContext &context, const GlobalIndex &,
               std::vector<Diagnostic> &out) override {
    for (auto *decl : context.getTranslationUnitDecl()->decls()) {
      auto *fn = llvm::dyn_cast<clang::FunctionDecl>(decl);
      if (!fn || fn->getNameAsString() != "legacy_alloc")
        continue;
      Diagnostic diag;
      diag.kind = Diagnostic::Custom;
      diag.callLocation = "/work/proj/src/mem.cpp:2:1";
      diag.entities = {"legacy_alloc"};
      diag.message = "legacy_alloc is banned";
      out.push_back(std::move(diag));
    }
  }

private:
  std::string name_;
};

std::vector<Diagnostic> analyzeWithOrgCheck(const std::string &checkName) {
  ExtensionRegistry::instance().clear();
  ExtensionRegistry::instance().addAnnealCheck(
      [checkName] { return std::make_unique<NoLegacyCallCheck>(checkName); });
  GlobalIndex index;
  std::vector<Diagnostic> diagnostics;
  REQUIRE(clang::tooling::runToolOnCodeWithArgs(
      std::make_unique<AnalyzerAction>(index, diagnostics, AnalysisOptions{}),
      "void legacy_alloc();\n", {"-std=c++17"}, "mem.cpp"));
  return diagnostics;
}

} // namespace

TEST_CASE("Organization checks inherit check name, fingerprint, and SARIF "
          "rule through name()",
          "[AnnealReport][Extensions]") {
  RegistryReset reset;
  auto diags = analyzeWithOrgCheck("myorg-no-legacy-alloc");
  REQUIRE(diags.size() == 1);
  CHECK(diags[0].checkName == "myorg-no-legacy-alloc");

  AnnealRun run;
  run.projectRoot = kRoot;
  run.tus = {{kRoot + "/src/mem.cpp", {TuStatus::Indexed, ""}}};
  run.checks = {"adl-visibility", "myorg-no-legacy-alloc"};
  run.findings = buildFindings(diags, kRoot);
  REQUIRE(run.findings.size() == 1);
  const auto &finding = run.findings[0];
  CHECK(finding.check == "myorg-no-legacy-alloc");
  CHECK(finding.kind == "Custom");
  CHECK(finding.severity == Severity::Warning);
  CHECK(finding.fingerprint ==
        findingFingerprint("myorg-no-legacy-alloc", "Custom", "src/mem.cpp",
                           {"legacy_alloc"}));
  CHECK(render(renderText, run) ==
        "src/mem.cpp:2:1: [myorg-no-legacy-alloc] legacy_alloc is banned\n");

  auto sarif = llvm::json::parse(render(renderSarif, run));
  REQUIRE(bool(sarif));
  const auto *sarifRun =
      (*sarif->getAsObject()->getArray("runs"))[0].getAsObject();
  const auto *rules =
      sarifRun->getObject("tool")->getObject("driver")->getArray("rules");
  REQUIRE(rules->size() == 2);
  const auto *rule = (*rules)[1].getAsObject();
  CHECK(rule->getString("id") == "myorg-no-legacy-alloc");
  CHECK(!rule->getString("helpUri")); // no upstream page for an org check
  CHECK(rule->getObject("defaultConfiguration")->getString("level") ==
        "warning");
  const auto *result = (*sarifRun->getArray("results"))[0].getAsObject();
  CHECK(result->getString("ruleId") == "myorg-no-legacy-alloc");
  CHECK(result->getInteger("ruleIndex") == 1);

  // Renaming the check renames the rule and changes the fingerprint.
  auto renamed = buildFindings(analyzeWithOrgCheck("myorg-renamed"), kRoot);
  REQUIRE(renamed.size() == 1);
  CHECK(renamed[0].check == "myorg-renamed");
  CHECK(renamed[0].fingerprint != finding.fingerprint);
}
