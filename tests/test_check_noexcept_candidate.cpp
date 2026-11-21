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
#include "vycor/check/Report.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"

#include <catch2/catch_test_macros.hpp>
#include <clang/Tooling/CompilationDatabase.h>

#include <algorithm>
#include <fstream>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace vycor;
namespace {

using Files = std::vector<std::pair<std::string, std::string>>;

// Every .cpp file is a TU; everything else is a header beside them.
struct NoexceptFixture {
  std::string dir;
  std::vector<std::string> sources;

  explicit NoexceptFixture(const Files &files, const char *std = "-std=c++17") {
    llvm::SmallString<128> path;
    REQUIRE_FALSE(
        llvm::sys::fs::createUniqueDirectory("noexcept_candidate_test", path));
    llvm::SmallString<256> absolute;
    REQUIRE_FALSE(llvm::sys::fs::real_path(path, absolute));
    dir = std::string(absolute);
    llvm::json::Array entries;
    for (const auto &[name, body] : files) {
      write(name, body);
      if (name.size() < 4 || name.compare(name.size() - 4, 4, ".cpp") != 0)
        continue;
      sources.push_back(dir + "/" + name);
      entries.push_back(llvm::json::Object{
          {"directory", dir},
          {"file", sources.back()},
          {"arguments", llvm::json::Array{"clang++", std, "-I" + dir, "-c",
                                          sources.back()}}});
    }
    write("compile_commands.json",
          llvm::formatv("{0:2}", llvm::json::Value(std::move(entries))).str());
  }
  ~NoexceptFixture() { llvm::sys::fs::remove_directories(dir); }

  void write(const std::string &name, const std::string &body) const {
    std::ofstream file(dir + "/" + name);
    REQUIRE(file.good());
    file << body;
  }
  std::unique_ptr<clang::tooling::CompilationDatabase> db() const {
    std::string error;
    auto result =
        clang::tooling::CompilationDatabase::loadFromDirectory(dir, error);
    REQUIRE(result);
    return result;
  }
};

AnalysisOptions noexceptOptions(bool moves = false, bool virtuals = false) {
  AnalysisOptions opts;
  opts.threadCount = 1;
  opts.enableNoexceptCandidateDiag = true;
  opts.enableNoexceptCandidateMoveDiag = moves;
  opts.enableNoexceptCandidateVirtualDiag = virtuals;
  return opts;
}

bool isCandidateKind(Diagnostic::Kind kind) {
  return kind == Diagnostic::Noexcept_Candidate ||
         kind == Diagnostic::Noexcept_CandidateMove ||
         kind == Diagnostic::Noexcept_CandidateVirtual;
}

std::vector<Diagnostic> candidatesOnly(const std::vector<Diagnostic> &all) {
  std::vector<Diagnostic> out;
  for (const auto &d : all)
    if (isCandidateKind(d.kind))
      out.push_back(d);
  return out;
}

std::set<std::string> namesOf(const std::vector<Diagnostic> &all) {
  std::set<std::string> out;
  for (const auto &d : candidatesOnly(all))
    out.insert(d.resolvedDecl);
  return out;
}

// The functions the check reports for one TU's worth of source.
std::set<std::string> candidatesIn(const std::string &source,
                                   const AnalysisOptions &opts =
                                       noexceptOptions(),
                                   const char *std = "-std=c++17") {
  NoexceptFixture fixture({{"a.cpp", source}}, std);
  auto db = fixture.db();
  return namesOf(runAnalysis(*db, fixture.sources, opts));
}

using Names = std::set<std::string>;

NoexceptFactEntry fact(const std::string &name, uint32_t flags,
                       std::vector<std::string> callees = {},
                       std::vector<uint8_t> bits = {}) {
  NoexceptFactEntry f;
  f.usr = "c:@F@" + name;
  f.qualifiedName = name;
  f.filePath = "/src/" + name + ".cpp";
  f.line = 1;
  f.column = 1;
  f.flags = flags;
  for (auto &callee : callees)
    callee = "c:@F@" + callee;
  f.callees = std::move(callees);
  f.calleeBits = std::move(bits);
  return f;
}

std::vector<Diagnostic> analyze(GlobalIndex &index,
                                NoexceptCandidateOptions selection = {},
                                NoexceptCandidateStats *stats = nullptr) {
  std::vector<Diagnostic> out;
  analyzeNoexceptCandidates(index, selection, out, stats);
  return out;
}

constexpr uint32_t kNothrow = NoexceptFactEntry::DeclaredNothrow;
constexpr uint32_t kThrows = NoexceptFactEntry::MayThrow;
constexpr uint8_t kCallOnly = NoexceptFactEntry::Call;

} // namespace

// ---------------------------------------------------------------------------
// The proof, over hand-built facts
// ---------------------------------------------------------------------------

TEST_CASE("noexcept candidate: a chain is proven from its leaf up",
          "[CheckNoexceptCandidate]") {
  GlobalIndex index;
  index.addNoexceptFact(fact("leaf", 0));
  index.addNoexceptFact(fact("mid", 0, {"leaf"}));
  index.addNoexceptFact(fact("top", 0, {"mid"}));
  index.addNoexceptFact(fact("thrower", kThrows));
  index.addNoexceptFact(fact("callsThrower", 0, {"thrower"}));
  index.addNoexceptFact(fact("above", 0, {"callsThrower", "leaf"}));
  NoexceptCandidateStats stats;
  CHECK(namesOf(analyze(index, {}, &stats)) == Names{"leaf", "mid", "top"});
  CHECK(stats.proven == 3);
  CHECK(stats.refutedByBody == 1);
  CHECK(stats.refutedByCallee == 2);
  CHECK(stats.reported == 3);
}

TEST_CASE("noexcept candidate: a callee with no indexed definition is "
          "unknown and refutes its callers",
          "[CheckNoexceptCandidate]") {
  GlobalIndex index;
  index.addNoexceptFact(fact("callsMissing", 0, {"missing"}));
  index.addNoexceptFact(fact("above", 0, {"callsMissing"}));
  index.addNoexceptFact(fact("guarded", 0, {"missing"}, {kCallOnly}));
  NoexceptCandidateStats stats;
  CHECK(namesOf(analyze(index, {}, &stats)) == Names{"guarded"});
  CHECK(stats.refutedByCallee == 2);
}

TEST_CASE("noexcept candidate: a noexcept callee ends the search",
          "[CheckNoexceptCandidate]") {
  GlobalIndex index;
  // The body of a noexcept function may throw (it terminates instead),
  // and what it calls is nobody else's obligation.
  index.addNoexceptFact(fact("barrier", kNothrow | kThrows, {"missing"}));
  index.addNoexceptFact(fact("caller", 0, {"barrier"}));
  NoexceptCandidateStats stats;
  CHECK(namesOf(analyze(index, {}, &stats)) == Names{"caller"});
  CHECK(stats.declaredNothrow == 1);
}

TEST_CASE("noexcept candidate: a declaration that disagrees with another "
          "is not a barrier",
          "[CheckNoexceptCandidate]") {
  GlobalIndex index;
  NoexceptFactEntry one = fact("split", kNothrow);
  NoexceptFactEntry two = fact("split", NoexceptFactEntry::DeclaredThrowing |
                                            kThrows);
  two.filePath = "/src/other.cpp";
  index.addNoexceptFact(one);
  index.addNoexceptFact(two);
  index.addNoexceptFact(fact("caller", 0, {"split"}));
  CHECK(namesOf(analyze(index)).empty());
}

TEST_CASE("noexcept candidate: mutual recursion with no other exit is "
          "proven as a whole",
          "[CheckNoexceptCandidate]") {
  GlobalIndex index;
  index.addNoexceptFact(fact("even", 0, {"odd"}));
  index.addNoexceptFact(fact("odd", 0, {"even"}));
  index.addNoexceptFact(fact("self", 0, {"self"}));
  CHECK(namesOf(analyze(index)) == Names{"even", "odd", "self"});
}

TEST_CASE("noexcept candidate: one throwing exit refutes the whole cycle",
          "[CheckNoexceptCandidate]") {
  GlobalIndex index;
  index.addNoexceptFact(fact("a", 0, {"b"}));
  index.addNoexceptFact(fact("b", 0, {"c"}));
  index.addNoexceptFact(fact("c", 0, {"a", "escape"}));
  index.addNoexceptFact(fact("escape", kThrows));
  index.addNoexceptFact(fact("entry", 0, {"a"}));
  CHECK(namesOf(analyze(index)).empty());

  GlobalIndex unknownExit;
  unknownExit.addNoexceptFact(fact("a", 0, {"b"}));
  unknownExit.addNoexceptFact(fact("b", 0, {"a", "missing"}));
  CHECK(namesOf(analyze(unknownExit)).empty());
}

TEST_CASE("noexcept candidate: every definition site of a function must be "
          "proven",
          "[CheckNoexceptCandidate]") {
  GlobalIndex index;
  NoexceptFactEntry quiet = fact("twice", 0);
  NoexceptFactEntry loud = fact("twice", kThrows);
  loud.filePath = "/src/other.cpp";
  index.addNoexceptFact(quiet);
  index.addNoexceptFact(loud);
  index.addNoexceptFact(fact("caller", 0, {"twice"}));
  CHECK(namesOf(analyze(index)).empty());
}

TEST_CASE("noexcept candidate: facts from a TU with errors taint what "
          "depends on them",
          "[CheckNoexceptCandidate]") {
  GlobalIndex index;
  index.addNoexceptFact(fact("shaky", NoexceptFactEntry::Unreliable));
  index.addNoexceptFact(fact("dependsOnShaky", 0, {"shaky"}));
  index.addNoexceptFact(fact("independent", 0));
  // A noexcept declaration is a fact about the language, not the proof.
  index.addNoexceptFact(
      fact("shakyBarrier", kNothrow | NoexceptFactEntry::Unreliable));
  index.addNoexceptFact(fact("behindBarrier", 0, {"shakyBarrier"}));
  NoexceptCandidateStats stats;
  CHECK(namesOf(analyze(index, {}, &stats)) ==
        Names{"behindBarrier", "independent"});
  CHECK(stats.withheldUnreliable == 2);
  CHECK(stats.refutedByCallee == 0);
}

TEST_CASE("noexcept candidate: with a TU missing a proof that crosses "
          "into another TU is withheld",
          "[CheckNoexceptCandidate]") {
  constexpr uint8_t kSameTu =
      NoexceptFactEntry::Exit | NoexceptFactEntry::Call;
  constexpr uint8_t kGuardedElsewhere =
      NoexceptFactEntry::Call | NoexceptFactEntry::Elsewhere;
  GlobalIndex index;
  index.addNoexceptFact(fact("leaf", 0));
  index.addNoexceptFact(fact("sameTu", 0, {"leaf"}, {kSameTu}));
  index.addNoexceptFact(fact("crosses", 0, {"leaf"})); // defined elsewhere
  index.addNoexceptFact(fact("aboveCrosses", 0, {"crosses"}, {kSameTu}));
  index.addNoexceptFact(fact("guarded", 0, {"leaf"}, {kGuardedElsewhere}));
  // An obligation on a noexcept definition exists only where the calling
  // TU did not see that declaration, so it too is a call into whatever
  // definition is linked.
  index.addNoexceptFact(fact("barrier", kNothrow));
  index.addNoexceptFact(fact("callsBarrier", 0, {"barrier"}));
  index.addNoexceptFact(fact("thrower", kThrows));
  index.addNoexceptFact(fact("callsThrower", 0, {"thrower"}));

  NoexceptCandidateStats complete;
  CHECK(namesOf(analyze(index, {}, &complete)) ==
        Names{"aboveCrosses", "callsBarrier", "crosses", "guarded", "leaf",
              "sameTu"});
  CHECK(complete.withheldCoverage == 0);

  NoexceptCandidateOptions partial;
  partial.coverageComplete = false;
  NoexceptCandidateStats stats;
  CHECK(namesOf(analyze(index, partial, &stats)) ==
        Names{"guarded", "leaf", "sameTu"});
  CHECK(stats.withheldCoverage == 3);
  CHECK(stats.reported == 3);
  // Withholding is about reporting; what was proven is counted the same.
  CHECK(stats.proven == complete.proven);
  CHECK(stats.refutedByCallee == complete.refutedByCallee);
}

TEST_CASE("noexcept candidate: the counts are definition sites",
          "[CheckNoexceptCandidate]") {
  GlobalIndex index;
  NoexceptFactEntry here = fact("twice", 0);
  NoexceptFactEntry there = fact("twice", 0);
  there.filePath = "/src/other.cpp";
  index.addNoexceptFact(here);
  index.addNoexceptFact(there);
  index.addNoexceptFact(fact("once", 0, {"twice"}));
  NoexceptFactEntry loud = fact("loud", kThrows);
  NoexceptFactEntry loudToo = fact("loud", 0);
  loudToo.filePath = "/src/other.cpp";
  index.addNoexceptFact(loud);
  index.addNoexceptFact(loudToo);

  NoexceptCandidateStats stats;
  auto result = analyze(index, {}, &stats);
  // One finding per site, and never more reported than proven.
  CHECK(result.size() == 3);
  CHECK(stats.proven == 3);
  CHECK(stats.reported == 3);
  CHECK(stats.refutedByBody == 2);
  std::set<std::string> sites;
  for (const auto &d : result)
    sites.insert(d.callLocation);
  CHECK(sites == std::set<std::string>{"/src/once.cpp:1:1",
                                       "/src/other.cpp:1:1",
                                       "/src/twice.cpp:1:1"});
}

TEST_CASE("noexcept candidate: the same fact absorbed twice is one fact",
          "[CheckNoexceptCandidate]") {
  GlobalIndex index;
  index.addNoexceptFact(fact("inlineFn", 0, {"a"}));
  index.addNoexceptFact(fact("inlineFn", kThrows, {"b"}));
  CHECK(index.noexceptFactCount() == 1);
  index.forEachNoexceptFact([](const NoexceptFactEntry &f) {
    CHECK((f.flags & NoexceptFactEntry::MayThrow) != 0);
    CHECK(f.callees == std::vector<std::string>{"c:@F@a", "c:@F@b"});
    CHECK(f.calleeBits.size() == 2);
  });
}

TEST_CASE("noexcept candidate: sub-kinds are selected independently",
          "[CheckNoexceptCandidate]") {
  GlobalIndex index;
  index.addNoexceptFact(fact("plain", 0));
  index.addNoexceptFact(fact("mover", NoexceptFactEntry::Move));
  index.addNoexceptFact(fact("virt", NoexceptFactEntry::Virtual));
  index.addNoexceptFact(fact("virtMove", NoexceptFactEntry::Virtual |
                                             NoexceptFactEntry::Move));
  index.addNoexceptFact(fact("hidden", NoexceptFactEntry::NotReportable));
  index.addNoexceptFact(fact("usesHidden", 0, {"hidden"}));

  CHECK(namesOf(analyze(index)) == Names{"plain", "usesHidden"});
  NoexceptCandidateOptions movesOnly{false, true, false};
  auto moves = analyze(index, movesOnly);
  REQUIRE(moves.size() == 1);
  CHECK(moves[0].resolvedDecl == "mover");
  CHECK(moves[0].kind == Diagnostic::Noexcept_CandidateMove);
  NoexceptCandidateOptions virtualsOnly{false, false, true};
  auto virtuals = analyze(index, virtualsOnly);
  CHECK(namesOf(virtuals) == Names{"virt", "virtMove"});
  for (const auto &d : virtuals)
    CHECK(d.kind == Diagnostic::Noexcept_CandidateVirtual);
  NoexceptCandidateOptions everything{true, true, true};
  CHECK(analyze(index, everything).size() == 5);
}

TEST_CASE("noexcept candidate: findings are ranked by where noexcept pays",
          "[CheckNoexceptCandidate]") {
  GlobalIndex index;
  // Callers. A guarded call still counts as a use.
  index.addNoexceptFact(fact("dtor", kNothrow, {"inDtor", "inTwoBarriers"}));
  index.addNoexceptFact(fact("handler", kNothrow, {"inTwoBarriers"}));
  index.addNoexceptFact(fact("u1", kThrows, {"popular", "lessPopular"},
                             {kCallOnly, kCallOnly}));
  index.addNoexceptFact(fact("u2", kThrows, {"popular", "lessPopular"},
                             {kCallOnly, kCallOnly}));
  index.addNoexceptFact(fact("u3", kThrows, {"popular"}, {kCallOnly}));
  // Candidates.
  index.addNoexceptFact(fact("inDtor", 0));
  index.addNoexceptFact(fact("inTwoBarriers", 0));
  index.addNoexceptFact(fact("swap", NoexceptFactEntry::Swap));
  index.addNoexceptFact(fact("mover", NoexceptFactEntry::Move));
  index.addNoexceptFact(fact("popular", 0));
  index.addNoexceptFact(fact("lessPopular", 0));
  index.addNoexceptFact(fact("alone", 0));
  index.addNoexceptFact(fact("selfOnly", 0, {"selfOnly"}));

  NoexceptCandidateOptions everything{true, true, true};
  auto result = analyze(index, everything);
  std::vector<std::string> order;
  for (size_t i = 0; i < result.size(); ++i) {
    CHECK(result[i].rank == i + 1);
    order.push_back(result[i].resolvedDecl);
  }
  CHECK(order == std::vector<std::string>{"inTwoBarriers", "inDtor", "mover",
                                          "swap", "popular", "lessPopular",
                                          "alone", "selfOnly"});
  CHECK(result[0].message.find("2 noexcept functions (such as 'dtor')") !=
        std::string::npos);
  CHECK(result[4].message.find("Called from 3 functions.") !=
        std::string::npos);

  // The report keeps that order whatever the file names say, after the
  // findings of checks that do not rank.
  Diagnostic unranked;
  unranked.kind = Diagnostic::ADL_Fallback;
  unranked.file = "/src/zzz.cpp";
  unranked.callLocation = "/src/zzz.cpp:9:9";
  unranked.message = "unranked";
  auto mixed = result;
  mixed.push_back(unranked);
  auto findings = buildFindings(mixed, "/src", false);
  REQUIRE(findings.size() == result.size() + 1);
  CHECK(findings[0].rank == 0);
  CHECK(findings[0].message == "unranked");
  for (size_t i = 0; i < result.size(); ++i) {
    CHECK(findings[i + 1].rank == i + 1);
    CHECK(findings[i + 1].severity == Severity::Note);
    CHECK(findings[i + 1].message.find("'" + order[i] + "'") !=
          std::string::npos);
  }

  // JSON carries the position as `rank`, SARIF as `properties.order`;
  // an unranked finding has neither.
  CheckRun run;
  run.projectRoot = "/src";
  run.checks = {"adl-visibility", "noexcept-candidate",
                "noexcept-candidate-move", "noexcept-candidate-virtual"};
  run.findings = findings;
  auto renderTo = [&](void (*fn)(const CheckRun &, llvm::raw_ostream &)) {
    std::string out;
    llvm::raw_string_ostream os(out);
    fn(run, os);
    os.flush();
    return llvm::json::parse(out);
  };
  auto json = renderTo(renderJson);
  REQUIRE(bool(json));
  const auto *jsonFindings = json->getAsObject()->getArray("findings");
  REQUIRE(jsonFindings->size() == findings.size());
  CHECK_FALSE((*jsonFindings)[0].getAsObject()->get("rank"));
  for (size_t i = 1; i < jsonFindings->size(); ++i)
    CHECK((*jsonFindings)[i].getAsObject()->getInteger("rank") ==
          static_cast<int64_t>(i));
  auto sarif = renderTo(renderSarif);
  REQUIRE(bool(sarif));
  const auto *results = (*sarif->getAsObject()->getArray("runs"))[0]
                            .getAsObject()
                            ->getArray("results");
  REQUIRE(results->size() == findings.size());
  for (size_t i = 0; i < results->size(); ++i) {
    const auto *properties = (*results)[i].getAsObject()->getObject("properties");
    if (i == 0)
      CHECK((!properties || !properties->get("order")));
    else {
      REQUIRE(properties);
      CHECK(properties->getInteger("order") == static_cast<int64_t>(i));
    }
  }
}

TEST_CASE("noexcept candidate: output does not depend on fact arrival order",
          "[CheckNoexceptCandidate]") {
  std::vector<NoexceptFactEntry> facts = {
      fact("a", 0, {"b", "c"}), fact("b", 0, {"c"}), fact("c", 0),
      fact("d", kNothrow, {"a", "b"}), fact("e", kThrows, {"c"}),
      fact("f", 0, {"e"})};
  auto render = [](std::vector<NoexceptFactEntry> ordered) {
    GlobalIndex index;
    for (auto &f : ordered)
      index.addNoexceptFact(f);
    std::vector<std::string> out;
    for (const auto &d : analyze(index))
      out.push_back(std::to_string(d.rank) + " " + d.message);
    return out;
  };
  auto forward = render(facts);
  std::reverse(facts.begin(), facts.end());
  CHECK(render(facts) == forward);
  CHECK(forward.size() == 3);
}

// ---------------------------------------------------------------------------
// End to end
// ---------------------------------------------------------------------------

TEST_CASE("noexcept candidate: the proof crosses translation units",
          "[CheckNoexceptCandidate]") {
  NoexceptFixture fixture({
      {"api.h", "int leaf(int);\nint mid(int);\nint top(int);\n"
                "int unknown(int);\nint blocked(int);\n"},
      {"leaf.cpp", "#include \"api.h\"\nint leaf(int x) { return x + 1; }\n"},
      {"mid.cpp", "#include \"api.h\"\n"
                  "int mid(int x) { return leaf(x) * 2; }\n"
                  "int blocked(int x) { return unknown(x); }\n"},
      {"top.cpp", "#include \"api.h\"\n"
                  "int top(int x) { return mid(x); }\n"
                  "int aboveBlocked(int x) { return blocked(x); }\n"
                  "struct Session { ~Session() { top(1); } };\n"},
  });
  auto db = fixture.db();
  GlobalIndex index;
  auto result = candidatesOnly(
      runAnalysis(*db, fixture.sources, noexceptOptions(), &index));
  CHECK(namesOf(result) == Names{"leaf", "mid", "top"});
  REQUIRE(result.size() == 3);
  // top is the one a destructor calls, so it leads.
  CHECK(result[0].resolvedDecl == "top");
  CHECK(result[0].rank == 1);
  CHECK(result[0].message.find("'Session::~Session'") != std::string::npos);
  for (const auto &d : result) {
    CHECK(d.kind == Diagnostic::Noexcept_Candidate);
    CHECK(d.entities.size() == 1);
    CHECK(d.file.find(fixture.dir) == 0);
  }
  CHECK(result[0].callLocation == fixture.dir + "/top.cpp:2:5");
}

TEST_CASE("noexcept candidate: a callee without a visible definition is "
          "never assumed safe",
          "[CheckNoexceptCandidate]") {
  auto names = candidatesIn(R"cpp(
int external(int);
int externalNoexcept(int) noexcept;
int callsExternal(int x) { return external(x); }
int callsNoexcept(int x) { return externalNoexcept(x); }
int viaPointer(int (*fp)(int)) { return fp(1); }
int viaNoexceptPointer(int (*fp)(int) noexcept) { return fp(1); }
struct Opaque { Opaque(); int v; };
int constructs() { Opaque o; return o.v; }
int builtin(const char *s) { return __builtin_strlen(s) != 0; }
)cpp");
  CHECK(names == Names{"callsNoexcept", "viaNoexceptPointer", "builtin"});
}

TEST_CASE("noexcept candidate: only a catch-all handler discharges a call",
          "[CheckNoexceptCandidate]") {
  auto names = candidatesIn(R"cpp(
void thrower() { throw 1; }
void catchesAll() { try { thrower(); } catch (...) {} }
void catchesTyped() { try { thrower(); } catch (int) {} }
void catchesBoth() { try { thrower(); } catch (int) {} catch (...) {} }
void rethrows() { try { thrower(); } catch (...) { throw; } }
void handlerCalls() { try { thrower(); } catch (...) { thrower(); } }
void nested() {
  try {
    try { thrower(); } catch (int) {}
  } catch (...) {}
}
void localThrowCaught() { try { throw 1; } catch (...) {} }
void aboveCatchesAll() { catchesAll(); }
struct FnTry {
  int v;
  FnTry() try : v((thrower(), 1)) {} catch (...) {}
};
// A rethrow raises only what its try block can: nothing here.
int quiet(int x) { return x; }
int cleanupRethrow(int x) { try { return quiet(x); } catch (...) { throw; } }
void typedThenRethrow() {
  try { thrower(); } catch (int) {} catch (...) { throw; }
}
void nestedRethrow() {
  try { quiet(1); } catch (...) { try { throw; } catch (int) {} }
}
)cpp");
  // nestedRethrow's `throw;` is in a try of its own, so the outer catch
  // (...) is not taken to let anything out, and that rethrow is a throw.
  CHECK(names == Names{"catchesAll", "catchesBoth", "nested",
                       "localThrowCaught", "aboveCatchesAll", "quiet",
                       "cleanupRethrow"});
}

TEST_CASE("noexcept candidate: recursion is proven or refuted as a whole",
          "[CheckNoexceptCandidate]") {
  NoexceptFixture fixture({
      {"r.h", "int even(int);\nint odd(int);\nint ping(int);\nint pong(int);\n"},
      {"even.cpp", "#include \"r.h\"\n"
                   "int even(int n) { return n == 0 ? 1 : odd(n - 1); }\n"
                   "int ping(int n) { return pong(n); }\n"
                   "int factorial(int n) {"
                   " return n <= 1 ? 1 : n * factorial(n - 1); }\n"},
      {"odd.cpp", "#include \"r.h\"\n"
                  "int odd(int n) { return n == 0 ? 0 : even(n - 1); }\n"
                  "int pong(int n) { if (n < 0) throw 1; return ping(n - 1); }\n"},
  });
  auto db = fixture.db();
  CHECK(namesOf(runAnalysis(*db, fixture.sources, noexceptOptions())) ==
        Names{"even", "odd", "factorial"});
}

TEST_CASE("noexcept candidate: virtual functions are a separate opt-in kind",
          "[CheckNoexceptCandidate]") {
  const char *source = R"cpp(
struct Shape {
  virtual int area() const { return 1; }
  virtual ~Shape() {}
};
struct Square final : Shape {
  int area() const override { return 4; }
};
int dispatches(const Shape &s) { return s.area(); }
int qualified(const Shape &s) { return s.Shape::area(); }
int finalClass(const Square &s) { return s.area(); }
int plain() { return 0; }
)cpp";
  // Dynamic dispatch can reach any override; a qualified call and a call
  // on a final class cannot.
  CHECK(candidatesIn(source) == Names{"qualified", "finalClass", "plain"});

  NoexceptFixture fixture({{"a.cpp", source}});
  auto db = fixture.db();
  auto result = candidatesOnly(runAnalysis(
      *db, fixture.sources, noexceptOptions(/*moves=*/false, /*virtuals=*/true)));
  Names virtuals;
  for (const auto &d : result)
    if (d.kind == Diagnostic::Noexcept_CandidateVirtual) {
      virtuals.insert(d.resolvedDecl);
      CHECK(d.message.find("every override") != std::string::npos);
    }
  CHECK(virtuals == Names{"Shape::area", "Square::area"});
  CHECK(namesOf(result).size() == 5);
}

TEST_CASE("noexcept candidate: move operations are a separate opt-in kind",
          "[CheckNoexceptCandidate]") {
  const char *source = R"cpp(
struct Buffer {
  int *data;
  Buffer(Buffer &&other) : data(other.data) { other.data = nullptr; }
  Buffer &operator=(Buffer &&other) {
    data = other.data;
    other.data = nullptr;
    return *this;
  }
  Buffer(const Buffer &other) : data(other.data) {}
  void swap(Buffer &other) { int *t = data; data = other.data; other.data = t; }
};
)cpp";
  CHECK(candidatesIn(source) == Names{"Buffer::Buffer", "Buffer::swap"});

  NoexceptFixture fixture({{"a.cpp", source}});
  auto db = fixture.db();
  AnalysisOptions opts;
  opts.threadCount = 1;
  opts.enableNoexceptCandidateMoveDiag = true;
  auto result = candidatesOnly(runAnalysis(*db, fixture.sources, opts));
  REQUIRE(result.size() == 2);
  for (const auto &d : result) {
    CHECK(d.kind == Diagnostic::Noexcept_CandidateMove);
    CHECK(d.checkName.empty());
    CHECK(checkNameOf(d) == "noexcept-candidate-move");
  }
  CHECK(namesOf(result) == Names{"Buffer::Buffer", "Buffer::operator="});
}

TEST_CASE("noexcept candidate: implicit destructor calls are part of the "
          "proof",
          "[CheckNoexceptCandidate]") {
  auto names = candidatesIn(R"cpp(
extern bool flag;
struct Loud { ~Loud() noexcept(false) { if (flag) throw 1; } };
struct Quiet { ~Quiet() {} };
struct External { ~External() noexcept(false); };
void take(const Loud &) noexcept;
Loud makeLoud() noexcept;

void localLoud() { Loud l; }
void localQuiet() { Quiet q; }
void localExternal() { External e; }
void temporaryLoud() { take(Loud()); }
void discardedLoud() { makeLoud(); }
void arrayLoud() { Loud many[2]; }
void staticLoud() { static Loud l; }
struct HoldsLoud {
  Loud member;
  ~HoldsLoud() noexcept(false) {}
};
struct DerivesLoud : Loud {
  DerivesLoud() {}
};
void localHolder() { HoldsLoud h; }
void localDerived() { DerivesLoud d; }
void guardedLoud() { try { Loud l; } catch (...) {} }
)cpp");
  // The constructor of DerivesLoud destroys nothing; its implicit
  // destructor does. A static's destructor runs at exit, not here.
  CHECK(names == Names{"localQuiet", "staticLoud", "guardedLoud",
                       "DerivesLoud::DerivesLoud"});
}

TEST_CASE("noexcept candidate: new and delete are part of the proof",
          "[CheckNoexceptCandidate]") {
  auto names = candidatesIn(R"cpp(
using size_t = decltype(sizeof(0));
namespace std { struct nothrow_t {}; }
void *operator new(size_t, const std::nothrow_t &) noexcept;
void *operator new(size_t, void *where) noexcept;
struct Loud { ~Loud() noexcept(false); };
struct Pooled {
  static void *operator new(size_t) noexcept;
  static void operator delete(void *) noexcept;
  int v;
};
struct ThrowingCtor { ThrowingCtor(); };
struct Poly { virtual ~Poly(); };
struct Incomplete;

int *allocates() { return new int(1); }
int *allocatesArray(int n) { return new int[n]; }
int *allocatesNothrow() { return new (std::nothrow_t{}) int(1); }
int *placement(void *where) { return new (where) int(1); }
Pooled *pooled() { return new Pooled; }
ThrowingCtor *constructs(void *where) { return new (where) ThrowingCtor; }
void frees(int *p) { delete p; }
void freesArray(int *p) { delete[] p; }
void freesLoud(Loud *p) { delete p; }
void freesPoly(Poly *p) { delete p; }
void freesIncomplete(Incomplete *p) { delete p; }
)cpp");
  CHECK(names == Names{"allocatesNothrow", "placement", "pooled", "frees",
                       "freesArray", "freesPoly"});
}

TEST_CASE("noexcept candidate: instantiated templates are folded into "
          "their callers",
          "[CheckNoexceptCandidate]") {
  auto names = candidatesIn(R"cpp(
template <class T> T twice(T x) { return x + x; }
template <class T> T risky(T x) { if (x < 0) throw 1; return x; }
template <class T> T forwards(T x) { return twice(x); }
template <class T> struct Box {
  T value;
  T get() const { return value; }
  T checked() const { if (!value) throw 1; return value; }
};
int external(int);
template <class T> T callsExternal(T x) { return external(x); }

int usesTwice(int x) { return twice(x); }
int usesRisky(int x) { return risky(x); }
int usesForwards(int x) { return forwards(x); }
int usesBox(const Box<int> &b) { return b.get(); }
int usesChecked(const Box<int> &b) { return b.checked(); }
int usesExternal(int x) { return callsExternal(x); }
)cpp");
  // Neither the templates nor their instantiations are reported: a
  // noexcept on the template would have to hold for every argument.
  CHECK(names == Names{"usesTwice", "usesForwards", "usesBox"});
}

TEST_CASE("noexcept candidate: an inline function is proven wherever it "
          "is defined",
          "[CheckNoexceptCandidate]") {
  NoexceptFixture fixture({
      {"util.h", "#pragma once\n"
                 "inline int clampLow(int x) { return x < 0 ? 0 : x; }\n"
                 "int external(int);\n"
                 "inline int wraps(int x) { return external(x); }\n"},
      {"a.cpp", "#include \"util.h\"\nint a(int x) { return clampLow(x); }\n"},
      {"b.cpp", "#include \"util.h\"\nint b(int x) { return clampLow(x); }\n"
                "int c(int x) { return wraps(x); }\n"},
  });
  auto db = fixture.db();
  auto result = candidatesOnly(
      runAnalysis(*db, fixture.sources, noexceptOptions()));
  CHECK(namesOf(result) == Names{"clampLow", "a", "b"});
  // One finding for the header function, not one per including TU.
  CHECK(result.size() == 3);
  CHECK(result[0].resolvedDecl == "clampLow");
  CHECK(result[0].message.find("Called from 2 functions.") !=
        std::string::npos);
}

TEST_CASE("noexcept candidate: lambdas and default arguments run in the "
          "caller",
          "[CheckNoexceptCandidate]") {
  auto names = candidatesIn(R"cpp(
int external();
int withDefault(int x = external()) { return x; }
int usesDefault() { return withDefault(); }
int passesArgument() { return withDefault(1); }

int callsLambda(int x) {
  auto add = [](int y) { return y + 1; };
  return add(x);
}
int callsThrowingLambda(int x) {
  auto check = [](int y) { if (y) throw 1; return y; };
  return check(x);
}
void buildsLambda() {
  auto never = [] { throw 1; };
  (void)never;
}
struct Init {
  int v = external();
  Init() {}
};
int staticInit() { static int once = external(); return once; }
)cpp");
  CHECK(names == Names{"withDefault", "passesArgument", "callsLambda",
                       "buildsLambda"});
}

TEST_CASE("noexcept candidate: unevaluated and discarded code is not run",
          "[CheckNoexceptCandidate]") {
  auto names = candidatesIn(R"cpp(
int external();
int sizes() { return sizeof(external()) + noexcept(external()); }
int discarded() { if constexpr (false) { return external(); } return 0; }
int taken() { if constexpr (true) { return external(); } return 0; }
constexpr int square(int x) { return x * x; }
int folded() { constexpr int v = square(3); return v; }
struct Poly { virtual ~Poly() {} };
struct Derived : Poly {};
int casts(Poly &p) { return &dynamic_cast<Derived &>(p) != nullptr; }
int castsPointer(Poly *p) { return dynamic_cast<Derived *>(p) != nullptr; }
)cpp");
  CHECK(names == Names{"sizes", "discarded", "square", "folded",
                       "castsPointer"});
}

TEST_CASE("noexcept candidate: functions that should not be told to add "
          "noexcept are left out",
          "[CheckNoexceptCandidate]") {
  auto names = candidatesIn(R"cpp(
#define DEFINE_GETTER(name) int name() { return 0; }
DEFINE_GETTER(fromMacro)
int main() { return 0; }
extern "C" int cFunction(int x) { return x; }
int already() noexcept { return 0; }
int saysItThrows() noexcept(false) { return 0; }
int legacyEmpty() throw() { return 0; }
struct Defaulted {
  Defaulted() = default;
  ~Defaulted() = default;
};
struct UserDtor { ~UserDtor() {} };
int reported() { return 0; }
int usesMacro() { return fromMacro(); }
)cpp");
  CHECK(names == Names{"reported", "usesMacro"});
  CHECK(candidatesIn("int old() { return 0; }\n", noexceptOptions(),
                     "-std=c++98")
            .empty());
}

TEST_CASE("noexcept candidate: a body the linker may replace proves "
          "nothing",
          "[CheckNoexceptCandidate]") {
  // The copy that runs may be another one: a weak definition gives way to
  // a strong one, and a gnu_inline body is only an inlining hint for a
  // definition written elsewhere.
  auto names = candidatesIn(R"cpp(
__attribute__((weak)) int weakDef() { return 0; }
extern inline __attribute__((gnu_inline)) int gnuInline() { return 0; }
int callsWeak() { return weakDef(); }
int callsGnuInline() { return gnuInline(); }
int plain() { return 0; }
)cpp");
  CHECK(names == Names{"plain"});
}

TEST_CASE("noexcept candidate: code that runs without a call expression "
          "is part of the proof",
          "[CheckNoexceptCandidate]") {
  auto names = candidatesIn(R"cpp(
#include <cstddef>
int thrower() { throw 1; }
// The first use of a thread_local on a thread runs its initializer.
thread_local int tl = thrower();
int readsTl() { return tl; }
struct W { static thread_local int m; };
thread_local int W::m = thrower();
int readsMember() { return W::m; }
extern thread_local int elsewhere;
int readsExtern() { return elsewhere; }
thread_local int constant = 3;
int readsConstant() { return constant; }
// A destructor is registered on first use, through the same wrapper that
// runs tl's initializer.
struct Dtor { constexpr Dtor() {} ~Dtor() {} };
thread_local Dtor needsDestruction;
Dtor *readsDestructed() { return &needsDestruction; }
// A cleanup function runs at scope exit.
void boom(int *) { throw 1; }
void withCleanup() { int x __attribute__((cleanup(boom))) = 0; (void)x; }
// A VLA bound in a type is evaluated.
unsigned long sizeofVla() { return sizeof(int[thrower()]); }
void castVla() { (void)(int (*)[thrower()])0; }
void parameterVla(int (*)[thrower()]) {}
// So is a non-constant offsetof index.
struct S { int a[10]; };
unsigned long offsetofIndex() { return __builtin_offsetof(S, a[thrower()]); }
unsigned long offsetofConstant() { return __builtin_offsetof(S, a[2]); }
// A bad array size throws before a potentially-throwing allocator runs.
struct Arena { char buf[64]; };
void *operator new[](std::size_t, Arena &a) { return a.buf; }
int *arrayNew(Arena &a, int n) { return new (a) int[n]; }
int *arrayNewConstant(Arena &a) { return new (a) int[4]; }
)cpp");
  CHECK(names == Names{"Dtor::Dtor", "arrayNewConstant", "offsetofConstant",
                       "readsConstant"});

  // An immediate invocation is evaluated at compile time; the object it
  // produces is still destroyed at run time.
  auto immediate = candidatesIn(R"cpp(
struct Dt {
  constexpr Dt() {}
  constexpr ~Dt() noexcept(false) {
    if (!__builtin_is_constant_evaluated()) throw 1;
  }
};
consteval Dt make() { return {}; }
void usesConsteval() { make(); }
void bindsConsteval() { const Dt &d = make(); (void)d; }
int plain() { return 0; }
)cpp",
                                noexceptOptions(), "-std=c++20");
  CHECK(immediate == Names{"Dt::Dt", "plain"});
}

TEST_CASE("noexcept candidate: only an exception specification binds "
          "overriders",
          "[CheckNoexceptCandidate]") {
  const char *source = R"cpp(
struct V {
  virtual void f() __attribute__((nothrow));
  virtual void g() noexcept;
  virtual ~V();
};
void callF(V &v) { v.f(); }
void callG(V &v) { v.g(); }
struct B { virtual ~B() __attribute__((nothrow)); };
void deleteB(B *b) { delete b; }
)cpp";
  // The attribute is not inherited by an override.
  CHECK(candidatesIn(source) == Names{"callG"});
  // Clang accepts a looser override in MSVC compatibility mode.
  CHECK(candidatesIn(source, noexceptOptions(), "-fms-compatibility")
            .empty());
}

TEST_CASE("noexcept candidate: before C++17 a noexcept pointer proves "
          "nothing",
          "[CheckNoexceptCandidate]") {
  const char *source = R"cpp(
void (*gp)() noexcept = nullptr;
void callGp() { gp(); }
int plain() { return 0; }
)cpp";
  CHECK(candidatesIn(source, noexceptOptions(), "-std=c++14") ==
        Names{"plain"});
  CHECK(candidatesIn(source) == Names{"callGp", "plain"});
}

TEST_CASE("noexcept candidate: functions are matched across TUs by "
          "linkage name",
          "[CheckNoexceptCandidate]") {
  // A USR spells neither a member pointer's class, nor a noexcept inside
  // a parameter type, nor an asm label, so it would let f(int B::*) stand
  // for the undefined f(int A::*), g(void (*)() noexcept) for the
  // undefined g(void (*)()), and b.cpp's h and k for the real_h and real_k
  // that the callers below reach.
  NoexceptFixture fixture({
      {"a.cpp", "struct A { int x; };\n"
                "struct B { int y; };\n"
                "void f(int A::*);\n"
                "void f(int B::*) {}\n"
                "void viaMemberPointer() { f(&A::x); }\n"
                "void g(void (*)());\n"
                "void g(void (*)() noexcept) {}\n"
                "void viaNoexceptParameter() {\n"
                "  g(static_cast<void (*)()>(nullptr));\n"
                "}\n"
                "void h() asm(\"real_h\");\n"
                "void viaAsmLabel() { h(); }\n"
                "void k();\n"
                "void k() asm(\"real_k\");\n"
                "void viaLateAsmLabel() { k(); }\n"},
      {"b.cpp", "void h() {}\n"
                "void k() {}\n"},
  });
  auto db = fixture.db();
  CHECK(namesOf(runAnalysis(*db, fixture.sources, noexceptOptions())) ==
        Names{"f", "g", "h", "k"});

  // What the linkage name does keep together: constructors (including a
  // base subobject's), destructors, a C function reached through
  // extern "C".
  NoexceptFixture same({
      {"h.h", "struct Base { Base(int); ~Base(); };\n"
              "struct Derived : Base { Derived(); };\n"
              "extern \"C\" int cfunc(int);\n"},
      {"a.cpp", "#include \"h.h\"\n"
                "Base::Base(int) {}\n"
                "Base::~Base() {}\n"
                "extern \"C\" int cfunc(int x) { return x; }\n"},
      {"b.cpp", "#include \"h.h\"\n"
                "Derived::Derived() : Base(1) {}\n"
                "void makesBase() { Base b(2); }\n"
                "int callsC() { return cfunc(1); }\n"},
  });
  auto sameDb = same.db();
  CHECK(namesOf(runAnalysis(*sameDb, same.sources, noexceptOptions())) ==
        Names{"Base::Base", "Derived::Derived", "callsC", "makesBase"});
}

TEST_CASE("noexcept candidate: a TU with errors withholds what depends on "
          "it",
          "[CheckNoexceptCandidate]") {
  NoexceptFixture fixture({
      {"h.h", "int helper();\n"},
      {"good.cpp", "#include \"h.h\"\n"
                   "int dependent() { return helper(); }\n"
                   "int independent() { return 0; }\n"},
      {"broken.cpp", "#include \"h.h\"\n"
                     "int helper() { return 0; }\n"
                     "int broken() { return undeclared_name; }\n"},
  });
  auto db = fixture.db();
  CHECK(namesOf(runAnalysis(*db, fixture.sources, noexceptOptions())) ==
        Names{"independent"});
}

TEST_CASE("noexcept candidate: a TU that fails anywhere withholds proofs "
          "that cross TUs",
          "[CheckNoexceptCandidate]") {
  const Files sound = {
      {"h.h", "int leaf();\n"},
      {"leaf.cpp", "#include \"h.h\"\n"
                   "int leaf() { return 0; }\n"
                   "int sameTu() { return leaf(); }\n"},
      {"other.cpp", "#include \"h.h\"\n"
                    "inline int local() { return 1; }\n"
                    "int crosses() { return leaf(); }\n"
                    "int aboveCrosses() { return crosses(); }\n"
                    "int selfContained() { return local(); }\n"},
  };
  {
    NoexceptFixture fixture(sound);
    auto db = fixture.db();
    GlobalIndex index;
    CHECK(namesOf(runAnalysis(*db, fixture.sources, noexceptOptions(),
                              &index)) ==
          Names{"aboveCrosses", "crosses", "leaf", "local", "sameTu",
                "selfContained"});
    // Only the call that leaves its TU is marked, and the mark survives
    // the shard codec.
    const auto shard = fixture.dir + "/worker.bin";
    REQUIRE(writeCheckIndexShard(
        shard, {{"a.cpp", CheckIndexPayload::capture(index)}},
        {{TuStatus::Indexed, ""}}));
    GlobalIndex replayed;
    REQUIRE(readCheckIndexShard(shard, [&](const auto &, const auto &payload) {
      payload.applyTo(replayed);
    }));
    size_t seen = 0;
    replayed.forEachNoexceptFact([&](const NoexceptFactEntry &f) {
      if (f.qualifiedName != "crosses" && f.qualifiedName != "sameTu" &&
          f.qualifiedName != "aboveCrosses")
        return;
      ++seen;
      REQUIRE(f.callees.size() == 1);
      REQUIRE(f.calleeBits.size() == 1);
      CHECK(bool(f.calleeBits[0] & NoexceptFactEntry::Elsewhere) ==
            (f.qualifiedName == "crosses"));
    });
    CHECK(seen == 3);
    NoexceptCandidateOptions partial;
    partial.coverageComplete = false;
    CHECK(namesOf(analyze(replayed, partial)) ==
          Names{"leaf", "local", "sameTu", "selfContained"});
  }
  Files withBroken = sound;
  withBroken.push_back({"broken.cpp", "int broken() { return nowhere; }\n"});
  NoexceptFixture fixture(withBroken);
  auto db = fixture.db();
  AnalysisReport report;
  auto result = runAnalysis(*db, fixture.sources, noexceptOptions(), nullptr,
                            &report);
  CHECK(namesOf(result) == Names{"leaf", "local", "sameTu", "selfContained"});
}

TEST_CASE("noexcept candidate: with a TU missing a noexcept definition the "
          "caller did not see declared is a crossing too",
          "[CheckNoexceptCandidate]") {
  // a.cpp declares g without noexcept, so its call is an obligation; the
  // only reason to discharge it is b.cpp's definition, and with a TU
  // missing that may not be the g that a.cpp links against.
  const Files sound = {
      {"a.cpp", "void g();\n"
                "int h(int);\n"
                "void viaNoexceptDef() { g(); }\n"
                "int viaProvenDef(int x) { return h(x); }\n"
                "int alone() { return 0; }\n"},
      {"b.cpp", "void g() noexcept {}\n"
                "int h(int x) { return x; }\n"},
  };
  {
    NoexceptFixture fixture(sound);
    auto db = fixture.db();
    CHECK(namesOf(runAnalysis(*db, fixture.sources, noexceptOptions())) ==
          Names{"alone", "h", "viaNoexceptDef", "viaProvenDef"});
  }
  Files withBroken = sound;
  withBroken.push_back({"broken.cpp", "int broken( {\n"});
  NoexceptFixture fixture(withBroken);
  auto db = fixture.db();
  CHECK(namesOf(runAnalysis(*db, fixture.sources, noexceptOptions())) ==
        Names{"alone", "h"});
}

TEST_CASE("noexcept candidate: facts survive absorb and worker shards",
          "[CheckNoexceptCandidate]") {
  NoexceptFixture fixture({
      {"a.cpp", "int leaf(int x) { return x; }\n"
                "int mid(int x) { return leaf(x); }\n"
                "void barrier() noexcept { mid(1); }\n"},
  });
  auto db = fixture.db();
  GlobalIndex original;
  auto render = [](const std::vector<Diagnostic> &all) {
    std::vector<std::string> out;
    for (const auto &d : candidatesOnly(all))
      out.push_back(std::to_string(d.rank) + " " + d.callLocation + " " +
                    d.message);
    return out;
  };
  auto expected =
      render(runAnalysis(*db, fixture.sources, noexceptOptions(), &original));
  REQUIRE(expected.size() == 2);
  CHECK(original.noexceptFactCount() == 3);

  GlobalIndex merged;
  merged.absorb(original);
  merged.absorb(original);
  CHECK(merged.noexceptFactCount() == original.noexceptFactCount());
  CHECK(render(analyze(merged)) == expected);

  const auto shard = fixture.dir + "/worker.bin";
  REQUIRE(writeCheckIndexShard(
      shard, {{"a.cpp", CheckIndexPayload::capture(original)}},
      {{TuStatus::Indexed, ""}}));
  GlobalIndex replayed;
  REQUIRE(readCheckIndexShard(shard, [&](const auto &, const auto &payload) {
    payload.applyTo(replayed);
  }));
  CHECK(render(analyze(replayed)) == expected);
}

TEST_CASE("noexcept candidate: the checks are opt-in and change the "
          "checkpoint fingerprint",
          "[CheckNoexceptCandidate]") {
  AnalysisOptions opts;
  const auto off = checkOptionsFingerprint(opts);
  opts.enableNoexceptCandidateDiag = true;
  const auto general = checkOptionsFingerprint(opts);
  opts.enableNoexceptCandidateMoveDiag = true;
  const auto withMoves = checkOptionsFingerprint(opts);
  opts.enableNoexceptCandidateVirtualDiag = true;
  const auto withVirtuals = checkOptionsFingerprint(opts);
  CHECK(off != general);
  CHECK(general != withMoves);
  CHECK(withMoves != withVirtuals);

  for (const char *name : {"noexcept-candidate", "noexcept-candidate-move",
                           "noexcept-candidate-virtual"}) {
    CHECK_FALSE(defaultCheckSet().count(name));
    CHECK(checkSeverity(name) == Severity::Note);
    CHECK_FALSE(checkHelpUri(name).empty());
  }
  std::set<std::string> checks;
  std::string error;
  REQUIRE(resolveCheckSpec({"-all", "noexcept-candidate"}, checks, error));
  CHECK(checks == std::set<std::string>{"noexcept-candidate"});

  // Off means no facts are collected at all.
  NoexceptFixture fixture({{"a.cpp", "int f() { return 0; }\n"}});
  auto db = fixture.db();
  GlobalIndex index;
  AnalysisOptions none;
  none.threadCount = 1;
  CHECK(candidatesOnly(runAnalysis(*db, fixture.sources, none, &index))
            .empty());
  CHECK(index.noexceptFactCount() == 0);
}
