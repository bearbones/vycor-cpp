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

// test_result_contract.cpp — the shared result and completeness contract
// (docs/result-contract.md): typed status, the indexScope envelope, exit
// codes derived from status rather than message text, the coverage gate
// on universal exception verdicts, and the per-transport mapping (CLI
// json/ndjson/tsv, batch, MCP, info). One case per acceptance row of
// docs/plans/2026-09-next/C-result-contract.md.

#include "vycor/callgraph/CallGraph.h"
#include "vycor/callgraph/ControlFlowIndex.h"
#include "vycor/callgraph/ControlFlowOracle.h"
#include "vycor/callgraph/Snapshot.h"
#include "vycor/cli/MegascopeCli.h"
#include "vycor/mcp/McpServer.h"
#include "vycor/query/Tools.h"

#include <catch2/catch_test_macros.hpp>
#include <clang/Tooling/CompilationDatabase.h>

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"

#include <cstdio>
#include <initializer_list>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

using namespace vycor;

namespace {

// One TU baked for real, so the exception tools see call-site contexts
// (the same approach as test_path_oracle.cpp).
struct Baked {
  BakedIndexes ix;
  std::string path;
  Baked() = default;
  Baked(Baked &&) = default;
  Baked &operator=(Baked &&) = default;
  ~Baked() {
    if (!path.empty())
      llvm::sys::fs::remove(path);
  }
};

Baked bake(const std::string &code) {
  Baked b;
  llvm::SmallString<128> tmp;
  int fd = -1;
  auto ec = llvm::sys::fs::createTemporaryFile("vycor_contract", "cpp", fd,
                                               tmp);
  REQUIRE_FALSE(ec);
  ::close(fd);
  {
    std::ofstream out(std::string(tmp.str()));
    out << code;
  }
  b.path = std::string(tmp.str());
  clang::tooling::FixedCompilationDatabase compDb(".", {"-std=c++17"});
  b.ix = bakeIndexes(compDb, {b.path}, {}, /*threadCount=*/1);
  return b;
}

// target reached twice from main: once under a catch-all, once bare.
const char *kMixedFixture = R"cpp(
void target() { throw 1; }
void guarded() { try { target(); } catch (...) {} }
void bare() { target(); }
int main() { guarded(); bare(); }
)cpp";

// Every path caught: the only caller wraps the call.
const char *kCaughtFixture = R"cpp(
void target() { throw 1; }
int main() { try { target(); } catch (...) {} }
)cpp";

const ToolEntry &toolNamed(const std::vector<ToolEntry> &tools,
                           llvm::StringRef name) {
  for (const auto &t : tools)
    if (t.name == name)
      return t;
  FAIL("no tool named " << name.str());
  return tools.front();
}

/// json::Value only takes an Object by rvalue; copy an lvalue in.
llvm::json::Value valueOf(const llvm::json::Object &o) {
  return llvm::json::Value(llvm::json::Object(o));
}

llvm::json::Object objectOf(const llvm::json::Value &v) {
  const auto *obj = v.getAsObject();
  REQUIRE(obj != nullptr);
  return *obj;
}

llvm::json::Object parseObject(llvm::StringRef text) {
  auto v = llvm::json::parse(text);
  REQUIRE(bool(v));
  return objectOf(*v);
}

/// A meta whose requested TUs partition as given (`docs/index-provenance.md`).
SnapshotMeta metaWith(std::vector<TuStatus> statuses,
                      llvm::StringRef environment = "") {
  SnapshotMeta meta;
  TuOutcomes outcomes;
  for (size_t i = 0; i < statuses.size(); ++i) {
    std::string path = "/src/tu" + std::to_string(i) + ".cpp";
    meta.files.push_back({path, 1, 2});
    outcomes[path] = TuOutcome{statuses[i], ""};
  }
  SnapshotIO::recordOutcomes(meta, outcomes);
  meta.provenance.environment = environment.str();
  meta.provenance.bakeStartNs = 1234;
  return meta;
}

/// The context every case starts from: complete coverage of one indexed
/// TU, a saved bake to cite, taken as-is.
struct Fixture {
  Baked baked;
  ControlFlowOracle oracle;
  std::vector<std::string> entryPoints{"main"};
  QueryCache cache;
  std::vector<ToolEntry> tools = getRegisteredTools();
  ToolContext ctx;

  explicit Fixture(const std::string &code)
      : baked(bake(code)), oracle(baked.ix.graph, baked.ix.cfIndex),
        ctx{baked.ix.graph, oracle, baked.ix.cfIndex, entryPoints,
            &baked.ix.channels, &cache} {
    ctx.facts = IndexFacts::of(metaWith({TuStatus::Indexed}, "e3b0"),
                               IndexFreshness::Unchecked);
  }

  llvm::json::Object run(llvm::StringRef tool,
                         std::initializer_list<llvm::json::Object::KV> kvs) {
    return objectOf(runTool(toolNamed(tools, tool), llvm::json::Object(kvs),
                            ctx));
  }
  const ToolEntry &entry(llvm::StringRef tool) {
    return toolNamed(tools, tool);
  }
};

llvm::json::Object scopeOf(const llvm::json::Object &payload) {
  const auto *scope = payload.getObject("indexScope");
  REQUIRE(scope != nullptr);
  return *scope;
}

std::string tempIndexPath() {
  llvm::SmallString<128> p;
  llvm::sys::fs::createUniquePath("vycor-contract-%%%%%%.vycs", p,
                                  /*MakeAbsolute=*/true);
  return p.str().str();
}

struct Run {
  int code = -1;
  std::string out, err;
};

Run runVerb(std::vector<std::string> args, const std::string &stdinText = "") {
  Run r;
  llvm::raw_string_ostream out(r.out), err(r.err);
  std::istringstream in(stdinText);
  r.code = runMegascopeQueryVerb(args, out, err, in);
  out.flush();
  err.flush();
  return r;
}

} // namespace

// ============================================================================
// Status: the typed kind of every answer
// ============================================================================

TEST_CASE("status names round-trip and errors carry their kind",
          "[query][contract]") {
  for (ResultStatus s : {ResultStatus::Ok, ResultStatus::Ambiguous,
                         ResultStatus::UsageError, ResultStatus::NotFound,
                         ResultStatus::Unavailable})
    CHECK(parseResultStatus(resultStatusName(s)) == s);
  CHECK_FALSE(parseResultStatus("bogus").has_value());
  CHECK(isErrorStatus(ResultStatus::UsageError));
  CHECK(isErrorStatus(ResultStatus::NotFound));
  CHECK(isErrorStatus(ResultStatus::Unavailable));
  CHECK_FALSE(isErrorStatus(ResultStatus::Ok));
  CHECK_FALSE(isErrorStatus(ResultStatus::Ambiguous));

  CHECK(objectOf(usageError("m")).getString("status") == "usage_error");
  CHECK(objectOf(notFoundError("m")).getString("status") == "not_found");
  CHECK(objectOf(unavailableError("m")).getString("status") ==
        "unavailable");
  CHECK(errorMessage(unavailableError("m")) == "m");
  CHECK(statusOf(usageError("m")) == ResultStatus::UsageError);
  CHECK(statusOf(notFoundError("m")) == ResultStatus::NotFound);
  CHECK(statusOf(unavailableError("m")) == ResultStatus::Unavailable);
}

TEST_CASE("statusOf derives a missing status from the payload's shape",
          "[query][contract]") {
  // An error that bypassed the typed constructors is the historical
  // default, not found; an unparseable status falls back the same way.
  llvm::json::Object untyped;
  untyped["error"] = "whatever";
  CHECK(statusOf(valueOf(untyped)) == ResultStatus::NotFound);
  untyped["status"] = "nonsense";
  CHECK(statusOf(valueOf(untyped)) == ResultStatus::NotFound);

  llvm::json::Object amb;
  amb["ambiguous"] = true;
  amb["candidates"] = llvm::json::Array{};
  CHECK(statusOf(valueOf(amb)) == ResultStatus::Ambiguous);

  llvm::json::Object plain;
  plain["count"] = 0;
  CHECK(statusOf(valueOf(plain)) == ResultStatus::Ok);
  CHECK(statusOf(llvm::json::Value("text")) == ResultStatus::Ok);
}

TEST_CASE("changing an error message cannot change an exit code",
          "[query][contract][cli]") {
  // The kind decides; the message is free-form. These messages carry the
  // prefixes the old classifier keyed on, deliberately crossed.
  CHECK(exitCodeFor(errorResult(ResultStatus::UsageError,
                                "Function not found: x"),
                    "") == kExitUsage);
  CHECK(exitCodeFor(errorResult(ResultStatus::NotFound,
                                "Missing required parameter 'x'"),
                    "") == kExitEmpty);
  CHECK(exitCodeFor(errorResult(ResultStatus::Unavailable,
                                "Invalid something"),
                    "") == kExitIndex);
  CHECK(exitCodeFor(errorResult(ResultStatus::UsageError, ""), "") ==
        kExitUsage);
}

// ============================================================================
// The envelope
// ============================================================================

TEST_CASE("IndexFacts::of cites the bake and the coverage",
          "[query][contract]") {
  SECTION("a saved bake") {
    auto f = IndexFacts::of(
        metaWith({TuStatus::Indexed, TuStatus::Partial, TuStatus::Crashed,
                  TuStatus::Skipped},
                 "abcd"),
        IndexFreshness::Unchecked);
    CHECK(f.bake == "abcd@1234");
    CHECK(f.freshness == IndexFreshness::Unchecked);
    CHECK(f.coverage.requested == 4);
    CHECK(f.coverage.indexed == 1);
    CHECK(f.coverage.partial == 1);
    CHECK(f.coverage.failed == 2);
    CHECK_FALSE(f.coverage.complete());
  }
  SECTION("no bake to cite (ephemeral)") {
    auto f = IndexFacts::of(metaWith({TuStatus::Indexed}),
                            IndexFreshness::Baked);
    CHECK(f.bake.empty());
    CHECK(f.coverage.complete());
  }
  SECTION("the default is vacuous coverage, freshness unknown") {
    IndexFacts f;
    CHECK(f.coverage.complete());
    CHECK(f.freshness == IndexFreshness::Unknown);
  }
}

TEST_CASE("completeResult stamps status and indexScope on any payload",
          "[query][contract]") {
  Fixture fx(kCaughtFixture);

  SECTION("success") {
    llvm::json::Object payload;
    payload["count"] = 1;
    auto out = objectOf(completeResult(valueOf(payload), fx.ctx));
    CHECK(out.getString("status") == "ok");
    CHECK(out.getInteger("count") == 1);
    auto scope = scopeOf(out);
    CHECK(scope.getString("bake") == "e3b0@1234");
    CHECK(scope.getString("freshness") == "unchecked");
    CHECK(scope.getInteger("requested") == 1);
    CHECK(scope.getInteger("indexed") == 1);
    CHECK(scope.getInteger("partial") == 0);
    CHECK(scope.getInteger("failed") == 0);
    CHECK(scope.getBoolean("complete") == true);
  }
  SECTION("errors and ambiguity keep their status and gain the scope") {
    auto err = objectOf(completeResult(notFoundError("x"), fx.ctx));
    CHECK(err.getString("status") == "not_found");
    CHECK(err.getString("error") == "x");
    CHECK(scopeOf(err).getBoolean("complete") == true);
    llvm::json::Object amb;
    amb["ambiguous"] = true;
    auto out = objectOf(completeResult(valueOf(amb), fx.ctx));
    CHECK(out.getString("status") == "ambiguous");
  }
  SECTION("idempotent; a non-object passes through") {
    llvm::json::Object payload;
    auto once = completeResult(valueOf(payload), fx.ctx);
    auto twice = completeResult(once, fx.ctx);
    CHECK(once == twice);
    CHECK(completeResult(llvm::json::Value(3), fx.ctx) ==
          llvm::json::Value(3));
  }
  SECTION("no bake, other freshness values") {
    fx.ctx.facts = IndexFacts::of(metaWith({TuStatus::Indexed}),
                                  IndexFreshness::Baked);
    auto out =
        objectOf(completeResult(llvm::json::Value(llvm::json::Object{}),
                                fx.ctx));
    CHECK_FALSE(scopeOf(out).get("bake"));
    CHECK(scopeOf(out).getString("freshness") == "baked");
    fx.ctx.facts = IndexFacts{};
    out = objectOf(completeResult(llvm::json::Value(llvm::json::Object{}),
                                  fx.ctx));
    CHECK(scopeOf(out).getString("freshness") == "unknown");
  }
}

// ============================================================================
// Acceptance rows, through runTool
// ============================================================================

TEST_CASE("success and complete empty are both ok", "[query][contract]") {
  Fixture fx(kMixedFixture);

  SECTION("non-empty") {
    auto out = fx.run("get_callers", {{"name", "target"}});
    CHECK(out.getString("status") == "ok");
    CHECK(out.getArray("callers")->size() == 2);
    CHECK(exitCodeFor(valueOf(out), "callers") == kExitResults);
    CHECK(scopeOf(out).getBoolean("complete") == true);
  }
  SECTION("empty: a known function nobody calls") {
    auto out = fx.run("get_callers", {{"name", "main"}});
    CHECK(out.getString("status") == "ok");
    CHECK(out.getArray("callers")->empty());
    CHECK_FALSE(out.get("error"));
    CHECK(exitCodeFor(valueOf(out), "callers") == kExitEmpty);
  }
  SECTION("empty: a search with no match is ok, not not_found") {
    auto out = fx.run("search_functions", {{"query", "zzz_nothing"}});
    CHECK(out.getString("status") == "ok");
    CHECK(out.getArray("matches")->empty());
    CHECK(exitCodeFor(valueOf(out), "matches") == kExitEmpty);
  }
}

TEST_CASE("ambiguity is a status, not an error", "[query][contract]") {
  CallGraph g;
  g.addNode({"dup", "/src/a.cpp", 1, false, false, "", "c:@F@dup#a"});
  g.addNode({"dup", "/src/b.cpp", 1, false, false, "", "c:@F@dup#b"});
  ControlFlowIndex cf;
  ControlFlowOracle oracle(g, cf);
  std::vector<std::string> eps{"main"};
  ToolContext ctx{g, oracle, cf, eps};
  auto tools = getRegisteredTools();
  auto out = objectOf(runTool(toolNamed(tools, "get_callers"),
                              llvm::json::Object{{"name", "dup"}}, ctx));
  CHECK(out.getString("status") == "ambiguous");
  CHECK(out.getBoolean("ambiguous") == true);
  CHECK(out.getArray("candidates")->size() == 2);
  CHECK_FALSE(out.get("error"));
  CHECK(exitCodeFor(valueOf(out), "callers") == kExitAmbiguous);
  CHECK_FALSE(objectOf(wrapToolResult(valueOf(out)))
                  .getBoolean("isError")
                  .has_value());
}

TEST_CASE("usage errors are typed by the handler", "[query][contract]") {
  Fixture fx(kMixedFixture);
  auto missing = fx.run("get_callers", {});
  CHECK(missing.getString("status") == "usage_error");
  CHECK(exitCodeFor(valueOf(missing), "callers") == kExitUsage);
  auto invalid = fx.run("find_call_chain", {{"to", "target"},
                                            {"max_paths", 0}});
  CHECK(invalid.getString("status") == "usage_error");
  auto badSite = fx.run("query_call_site_context", {{"call_site", "nope"}});
  CHECK(badSite.getString("status") == "usage_error");
  auto badKind = fx.run("query_raii_scopes_at_callsite",
                        {{"call_site", "/x.cpp:1:1"},
                         {"kinds", llvm::json::Array{"bogus"}}});
  CHECK(badKind.getString("status") == "usage_error");
  // A serve-only tool asked through runTool.
  auto serveOnly = fx.run("reindex_tu", {{"file", "/x.cpp"}});
  CHECK(serveOnly.getString("status") == "usage_error");
}

TEST_CASE("a named thing the index lacks is not_found", "[query][contract]") {
  Fixture fx(kMixedFixture);
  auto fn = fx.run("lookup_function", {{"name", "nope"}});
  CHECK(fn.getString("status") == "not_found");
  CHECK(exitCodeFor(valueOf(fn), "") == kExitEmpty);
  auto site = fx.run("query_call_site_context",
                     {{"call_site", "/not/indexed.cpp:1:1"}});
  CHECK(site.getString("status") == "not_found");
  auto raii = fx.run("query_raii_scopes_at_callsite",
                     {{"call_site", "/not/indexed.cpp:1:1"}});
  CHECK(raii.getString("status") == "not_found");
  auto chan = fx.run("query_channel", {{"channel_id", "nope"}});
  CHECK(chan.getString("status") == "not_found");
  auto order = fx.run("explain_ordering", {{"call_site_a", "/a.cpp:1:1"},
                                           {"call_site_b", "/b.cpp:1:1"}});
  CHECK(order.getString("status") == "not_found");
  // A site that exists but does not call the named function: the pair
  // names an edge the index lacks (exit 1, as before the contract).
  auto pair = fx.run("get_callers",
                     {{"name", "guarded"}, {"site", fx.baked.path + ":4:15"}});
  CHECK(pair.getString("status") == "not_found");
  CHECK(pair.getString("error")->contains("does not call"));
  CHECK(exitCodeFor(valueOf(pair), "callers") == kExitEmpty);
}

TEST_CASE("absent semantic information is unavailable, exit 3",
          "[query][contract][cli]") {
  Fixture fx(kMixedFixture);
  fx.ctx.channels = nullptr; // started without channel types
  auto chan = fx.run("query_channel", {{"channel_id", "c1"}});
  CHECK(chan.getString("status") == "unavailable");
  CHECK(exitCodeFor(valueOf(chan), "") == kExitIndex);
  auto order = fx.run("explain_ordering", {{"call_site_a", "/a.cpp:1:1"},
                                           {"call_site_b", "/b.cpp:1:1"}});
  CHECK(order.getString("status") == "unavailable");
  auto mcp = objectOf(wrapToolResult(valueOf(chan)));
  CHECK(mcp.getBoolean("isError") == true);
}

TEST_CASE("index failure exits 3 before any tool runs", "[contract][cli]") {
  std::string missing = tempIndexPath();
  auto r = runVerb({"get-callers", "--index", missing, "--name", "main"});
  CHECK(r.code == kExitIndex);
  CHECK(r.out.empty());
  CHECK(r.err.find("no index at") != std::string::npos);
}

TEST_CASE("a partial bake demotes universal exception verdicts",
          "[query][contract][oracle]") {
  Fixture fx(kCaughtFixture);

  SECTION("complete coverage: the universal verdict stands") {
    auto out = fx.run("query_exception_safety", {{"function", "target"}});
    CHECK(out.getString("protection") == "always_caught");
    CHECK(out.getBoolean("exhaustive") == true);
    CHECK(scopeOf(out).getBoolean("complete") == true);
  }
  SECTION("one requested TU failed: observed, with the reason") {
    fx.ctx.facts = IndexFacts::of(
        metaWith({TuStatus::Indexed, TuStatus::Crashed}, "e3b0"),
        IndexFreshness::Unchecked);
    auto out = fx.run("query_exception_safety", {{"function", "target"}});
    CHECK(out.getString("protection") == "observed_caught");
    CHECK(out.getInteger("caughtPaths") == 1);
    CHECK(out.getInteger("totalPaths") == 1);
    // The search itself ran to completion: the index is what is short.
    CHECK(out.getBoolean("exhaustive") == true);
    CHECK(out.getArray("stopReasons")->empty());
    auto scope = scopeOf(out);
    CHECK(scope.getBoolean("complete") == false);
    CHECK(scope.getInteger("failed") == 1);
    std::string summary = out.getString("summary")->str();
    CHECK(summary.find("the index does not cover every requested TU") !=
          std::string::npos);
    CHECK(summary.find("search is not exhaustive") == std::string::npos);
    // Same gate on the propagation view.
    auto prop = fx.run("query_throw_propagation",
                       {{"function", "target"}});
    CHECK(prop.getString("protection") == "observed_caught");
  }
  SECTION("a partial TU counts against coverage too") {
    fx.ctx.facts = IndexFacts::of(
        metaWith({TuStatus::Indexed, TuStatus::Partial}, "e3b0"),
        IndexFreshness::Unchecked);
    auto out = fx.run("query_exception_safety", {{"function", "target"}});
    CHECK(out.getString("protection") == "observed_caught");
  }
}

TEST_CASE("a partial bake demotes never_caught and noexcept_barrier",
          "[query][contract][oracle]") {
  Fixture bare(R"cpp(
void target() { throw 1; }
int main() { target(); }
)cpp");
  bare.ctx.facts = IndexFacts::of(
      metaWith({TuStatus::Indexed, TuStatus::Skipped}, "e3b0"),
      IndexFreshness::Unchecked);
  auto out = bare.run("query_exception_safety", {{"function", "target"}});
  CHECK(out.getString("protection") == "observed_uncaught");
  CHECK(out.getInteger("uncaughtPaths") == 1);

  Fixture barrier(R"cpp(
void target() { throw 1; }
void wall() noexcept { target(); }
int main() { wall(); }
)cpp");
  auto full = barrier.run("query_exception_safety", {{"function", "target"}});
  CHECK(full.getString("protection") == "noexcept_barrier");
  barrier.ctx.facts = IndexFacts::of(
      metaWith({TuStatus::Indexed, TuStatus::Skipped}, "e3b0"),
      IndexFreshness::Unchecked);
  auto part = barrier.run("query_exception_safety", {{"function", "target"}});
  CHECK(part.getString("protection") == "unknown");
  CHECK(part.getInteger("terminatingPaths") == 1);
}

TEST_CASE("sometimes_caught needs two witnesses and is never demoted",
          "[query][contract][oracle]") {
  Fixture fx(kMixedFixture);
  fx.ctx.facts = IndexFacts::of(
      metaWith({TuStatus::Indexed, TuStatus::Crashed}, "e3b0"),
      IndexFreshness::Unchecked);
  auto out = fx.run("query_exception_safety", {{"function", "target"}});
  CHECK(out.getString("protection") == "sometimes_caught");
  CHECK(out.getInteger("caughtPaths") == 1);
  CHECK(out.getInteger("uncaughtPaths") == 1);
}

TEST_CASE("a truncated search is observed even over a complete index",
          "[query][contract][oracle]") {
  Fixture fx(kMixedFixture);
  // Two paths exist; cap at one.
  auto out = fx.run("query_exception_safety",
                    {{"function", "target"}, {"max_paths", 1}});
  CHECK(out.getInteger("totalPaths") == 1);
  CHECK(out.getBoolean("exhaustive") == false);
  auto stops = out.getArray("stopReasons");
  REQUIRE(stops != nullptr);
  CHECK(stops->size() == 1);
  CHECK((*stops)[0].getAsString() == "path_limit");
  std::string protection = out.getString("protection")->str();
  CHECK((protection == "observed_caught" ||
         protection == "observed_uncaught"));
  CHECK(scopeOf(out).getBoolean("complete") == true);
  std::string summary = out.getString("summary")->str();
  CHECK(summary.find("the search is not exhaustive") != std::string::npos);
  CHECK(summary.find("does not cover") == std::string::npos);

  SECTION("both conditions failing are both named") {
    fx.ctx.facts = IndexFacts::of(
        metaWith({TuStatus::Indexed, TuStatus::Crashed}, "e3b0"),
        IndexFreshness::Unchecked);
    auto both = fx.run("query_exception_safety",
                       {{"function", "target"}, {"max_paths", 1}});
    std::string s = both.getString("summary")->str();
    CHECK(s.find("the search is not exhaustive and the index does not "
                 "cover every requested TU") != std::string::npos);
  }
}

// ============================================================================
// Transports
// ============================================================================

TEST_CASE("every CLI format carries the envelope the same way",
          "[contract][cli]") {
  Fixture fx(kMixedFixture);
  auto payload = llvm::json::Value(fx.run("get_callers", {{"name", "target"}}));
  auto scalar =
      llvm::json::Value(fx.run("lookup_function", {{"name", "target"}}));

  auto emit = [&](const llvm::json::Value &p, llvm::StringRef key,
                  OutputFormat f) {
    std::string out, err;
    llvm::raw_string_ostream os(out), es(err);
    emitToolResult(p, key, f, false, os, es);
    os.flush();
    return out;
  };

  SECTION("json: inside the payload") {
    auto obj = parseObject(emit(payload, "callers", OutputFormat::Json));
    CHECK(obj.getString("status") == "ok");
    CHECK(scopeOf(obj).getString("freshness") == "unchecked");
  }
  SECTION("ndjson: once, in the _summary line, never per record") {
    std::string text = emit(payload, "callers", OutputFormat::Ndjson);
    std::istringstream ss(text);
    std::string line;
    REQUIRE(std::getline(ss, line));
    auto head = parseObject(line);
    const auto *summary = head.getObject("_summary");
    REQUIRE(summary != nullptr);
    CHECK(summary->getString("status") == "ok");
    CHECK(summary->getObject("indexScope") != nullptr);
    size_t records = 0;
    while (std::getline(ss, line)) {
      ++records;
      auto rec = parseObject(line);
      CHECK_FALSE(rec.get("status"));
      CHECK_FALSE(rec.get("indexScope"));
    }
    CHECK(records == 2);
  }
  SECTION("tsv: record rows unchanged, scalar rows gain the columns") {
    std::string rows = emit(payload, "callers", OutputFormat::Tsv);
    CHECK(rows.find("status") == std::string::npos);
    CHECK(rows.find("indexScope") == std::string::npos);
    std::string one = emit(scalar, "", OutputFormat::Tsv);
    std::istringstream ss(one);
    std::string header;
    REQUIRE(std::getline(ss, header));
    CHECK(header.find("\tstatus") != std::string::npos);
    CHECK(header.find("indexScope\t") != std::string::npos);
    std::string err = emit(usageError("bad"), "", OutputFormat::Tsv);
    CHECK(err == "error\tstatus\nbad\tusage_error\n");
  }
}

TEST_CASE("batch preserves per-request status", "[contract][cli]") {
  // A saved index: two files without outcomes, which coverage counts as
  // skipped, so the scope is visibly partial.
  CallGraph g;
  g.addNode({"main", "/src/a.cpp", 10, true, false, ""}, "/src/a.cpp");
  g.addNode({"helper", "/src/a.cpp", 3, false, false, ""}, "/src/a.cpp");
  g.addEdge({"main", "helper", EdgeKind::DirectCall, Confidence::Proven,
             "/src/a.cpp:11:3", 0, ExecutionContext::Synchronous},
            "/src/a.cpp");
  ControlFlowIndex cf;
  SnapshotMeta meta = metaWith({TuStatus::Indexed, TuStatus::Crashed}, "ab");
  std::string path = tempIndexPath();
  REQUIRE(SnapshotIO::save(path, g, cf, meta));

  auto r = runVerb({"batch", "--index", path},
                   "{\"id\":1,\"tool\":\"get_callers\",\"args\":{\"name\":"
                   "\"helper\"}}\n"
                   "{\"id\":2,\"tool\":\"lookup_function\",\"args\":{\"name\":"
                   "\"nope\"}}\n"
                   "{\"id\":3,\"tool\":\"get_callers\",\"args\":{}}\n"
                   "{\"id\":4,\"tool\":\"no_such\"}\n"
                   "not json\n");
  CHECK(r.code == kExitResults);
  std::istringstream ss(r.out);
  std::string line;
  std::vector<llvm::json::Object> lines;
  while (std::getline(ss, line))
    lines.push_back(parseObject(line));
  REQUIRE(lines.size() == 5);
  CHECK(lines[0].getString("status") == "ok");
  CHECK(lines[0].getInteger("exit") == kExitResults);
  CHECK(lines[0].getObject("result")->getString("status") == "ok");
  auto scope = scopeOf(*lines[0].getObject("result"));
  CHECK(scope.getBoolean("complete") == false);
  CHECK(scope.getString("freshness") == "unchecked");
  CHECK(scope.getString("bake") == "ab@1234");
  CHECK(lines[1].getString("status") == "not_found");
  CHECK(lines[1].getInteger("exit") == kExitEmpty);
  CHECK(lines[2].getString("status") == "usage_error");
  CHECK(lines[2].getInteger("exit") == kExitUsage);
  CHECK(lines[3].getString("status") == "usage_error");
  CHECK(lines[3].getInteger("exit") == kExitUsage);
  CHECK(lines[3].getString("error")->find("unknown tool") !=
        std::string::npos);
  CHECK(lines[4].getString("status") == "usage_error");

  SECTION("the one-shot verb and info agree with batch") {
    auto one = runVerb({"lookup-function", "--index", path, "--name", "nope"});
    CHECK(one.code == kExitEmpty);
    CHECK(parseObject(one.out).getString("status") == "not_found");
    auto info = runVerb({"info", "--index", path});
    CHECK(info.code == kExitResults);
    auto obj = parseObject(info.out);
    CHECK(obj.getString("freshness") == "unchecked");
    CHECK(obj.getObject("provenance")->getString("bake") == "ab@1234");
    CHECK(obj.getObject("coverage")->getBoolean("complete") == false);
  }
  std::remove(path.c_str());
}

TEST_CASE("MCP maps status onto isError and keeps the payload as text",
          "[contract][mcp]") {
  Fixture fx(kMixedFixture);
  auto ok = fx.run("get_callers", {{"name", "target"}});
  auto wrapped = objectOf(wrapToolResult(valueOf(ok)));
  CHECK_FALSE(wrapped.getBoolean("isError").has_value());
  const auto *content = wrapped.getArray("content");
  REQUIRE(content != nullptr);
  auto text = parseObject(*(*content)[0].getAsObject()->getString("text"));
  CHECK(text.getString("status") == "ok");
  CHECK(text.getObject("indexScope") != nullptr);

  for (ResultStatus s : {ResultStatus::UsageError, ResultStatus::NotFound,
                         ResultStatus::Unavailable}) {
    auto e = objectOf(wrapToolResult(
        completeResult(errorResult(s, "m"), fx.ctx)));
    CHECK(e.getBoolean("isError") == true);
    auto body = parseObject(
        *(*e.getArray("content"))[0].getAsObject()->getString("text"));
    CHECK(body.getString("error") == "m");
    CHECK(body.getString("status") == resultStatusName(s));
    CHECK(body.getObject("indexScope") != nullptr);
  }
}
