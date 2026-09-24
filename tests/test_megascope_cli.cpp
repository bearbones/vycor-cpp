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

// megascope query verbs (vycor/cli/MegascopeCli.h): schema-derived flag
// parsing, the output contract (formats + exit codes), and the verb runner
// end to end against a saved index.

#include "vycor/cli/MegascopeCli.h"
#include "vycor/callgraph/Snapshot.h"
#include "vycor/query/Tools.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

using namespace vycor;

namespace {

const ToolEntry &toolNamed(const std::vector<ToolEntry> &tools,
                           llvm::StringRef name) {
  for (const auto &t : tools)
    if (t.name == name)
      return t;
  FAIL("no tool named " << name.str());
  return tools.front();
}

llvm::json::Object parseObject(llvm::StringRef text) {
  auto v = llvm::json::parse(text);
  REQUIRE(bool(v));
  REQUIRE(v->getAsObject() != nullptr);
  return *v->getAsObject();
}

std::vector<std::string> lines(const std::string &text) {
  std::vector<std::string> out;
  std::stringstream ss(text);
  std::string line;
  while (std::getline(ss, line))
    out.push_back(line);
  return out;
}

struct Run {
  int code = -1;
  std::string out;
  std::string err;
};

Run run(std::vector<std::string> args, const std::string &stdinText = "") {
  Run r;
  llvm::raw_string_ostream out(r.out), err(r.err);
  std::istringstream in(stdinText);
  r.code = runMegascopeQueryVerb(args, out, err, in);
  out.flush();
  err.flush();
  return r;
}

std::string tempIndexPath(llvm::StringRef tag) {
  llvm::SmallString<128> p;
  llvm::sys::fs::createUniquePath("vycor-cli-" + tag + "-%%%%%%.vycs", p,
                                  /*MakeAbsolute=*/true);
  return p.str().str();
}

/// main -> helper (direct), plus two functions that share the display
/// name "dup" (distinct USRs, distinct files) so name lookups are
/// ambiguous.
std::string saveFixtureIndex(llvm::StringRef tag, bool withFiles = true) {
  CallGraph g;
  g.addNode({"main", "/src/a.cpp", 10, true, false, ""}, "/src/a.cpp");
  g.addNode({"helper", "/src/a.cpp", 3, false, false, ""}, "/src/a.cpp");
  g.addNode({"dup", "/src/a.cpp", 20, false, false, "", "c:@F@dup#a"},
            "/src/a.cpp");
  g.addNode({"dup", "/src/b.cpp", 20, false, false, "", "c:@F@dup#b"},
            "/src/b.cpp");
  g.addEdge({"main", "helper", EdgeKind::DirectCall, Confidence::Proven,
             "/src/a.cpp:11:3", 0, ExecutionContext::Synchronous},
            "/src/a.cpp");
  ControlFlowIndex cf;
  SnapshotMeta meta;
  meta.collapsePaths = {"Client/Math"};
  if (withFiles)
    meta.files = {{"/src/a.cpp", 1, 2}, {"/src/b.cpp", 3, 4}};
  std::string path = tempIndexPath(tag);
  REQUIRE(SnapshotIO::save(path, g, cf, meta));
  return path;
}

struct IndexFile {
  std::string path;
  explicit IndexFile(llvm::StringRef tag, bool withFiles = true)
      : path(saveFixtureIndex(tag, withFiles)) {}
  ~IndexFile() { std::remove(path.c_str()); }
};

} // namespace

// ============================================================================
// Names and paths
// ============================================================================

TEST_CASE("index path resolution: flag, env, build path, cwd",
          "[megascope][cli]") {
  CHECK(resolveIndexPath("/x/i.vycs", "/env.vycs", "/b") == "/x/i.vycs");
  CHECK(resolveIndexPath("", "/env.vycs", "/b") == "/env.vycs");
  CHECK(resolveIndexPath("", "", "/b") == "/b/.vycor/megascope.vycs");
  CHECK(resolveIndexPath("", "", "") == defaultIndexPath("."));
  CHECK(llvm::StringRef(defaultIndexPath(".")).ends_with(
      ".vycor/megascope.vycs"));
}

TEST_CASE("tool verbs accept hyphens and underscores", "[megascope][cli]") {
  CHECK(canonicalToolName("get-callers") == "get_callers");
  CHECK(canonicalToolName("get_callers") == "get_callers");
  CHECK(isMegascopeQueryVerb("get-callers"));
  CHECK(isMegascopeQueryVerb("tools"));
  CHECK(isMegascopeQueryVerb("nonsense")); // reported by the runner
  CHECK_FALSE(isMegascopeQueryVerb("index"));
  CHECK_FALSE(isMegascopeQueryVerb("serve"));
  CHECK_FALSE(isMegascopeQueryVerb("--build-path"));
  CHECK_FALSE(isMegascopeQueryVerb(""));
}

// ============================================================================
// Schema -> flags
// ============================================================================

TEST_CASE("parseToolArgs maps schema property types onto flags",
          "[megascope][cli]") {
  auto tools = getRegisteredTools();

  SECTION("strings and repeated arrays, both flag spellings") {
    const auto &tool = toolNamed(tools, "get_callers");
    auto args = parseToolArgs(tool, {"--name", "Foo::bar", "--edge-kinds",
                                     "DirectCall", "--edge_kinds",
                                     "VirtualDispatch",
                                     "--min_confidence=Plausible"});
    REQUIRE(bool(args));
    CHECK(args->getString("name") == "Foo::bar");
    CHECK(args->getString("min_confidence") == "Plausible");
    const auto *kinds = args->getArray("edge_kinds");
    REQUIRE(kinds != nullptr);
    REQUIRE(kinds->size() == 2);
    CHECK((*kinds)[0].getAsString() == "DirectCall");
    CHECK((*kinds)[1].getAsString() == "VirtualDispatch");
  }

  SECTION("integers and booleans") {
    const auto &tool = toolNamed(tools, "analyze_dead_code");
    auto args = parseToolArgs(tool, {"--limit", "5", "--offset=2",
                                     "--include-system",
                                     "--include-optimistic=false"});
    REQUIRE(bool(args));
    CHECK(args->getInteger("limit") == 5);
    CHECK(args->getInteger("offset") == 2);
    CHECK(args->getBoolean("include_system") == true);
    CHECK(args->getBoolean("include_optimistic") == false);
  }

  SECTION("--args seed is overridden by explicit flags; arrays replace") {
    const auto &tool = toolNamed(tools, "get_callers");
    llvm::json::Object seed;
    seed["name"] = "seeded";
    seed["min_confidence"] = "Proven";
    seed["edge_kinds"] = llvm::json::Array{"ThreadEntry"};
    auto args = parseToolArgs(tool, {"--name", "explicit", "--edge-kinds",
                                     "DirectCall"},
                              std::move(seed));
    REQUIRE(bool(args));
    CHECK(args->getString("name") == "explicit");
    CHECK(args->getString("min_confidence") == "Proven");
    REQUIRE(args->getArray("edge_kinds")->size() == 1);
    CHECK((*args->getArray("edge_kinds"))[0].getAsString() == "DirectCall");
  }

  SECTION("usage errors name the valid flags") {
    const auto &tool = toolNamed(tools, "get_callers");
    auto unknown = parseToolArgs(tool, {"--bogus", "x"});
    REQUIRE_FALSE(bool(unknown));
    std::string msg = llvm::toString(unknown.takeError());
    CHECK(msg.find("unknown flag '--bogus'") != std::string::npos);
    CHECK(msg.find("--edge-kinds") != std::string::npos);

    auto dangling = parseToolArgs(tool, {"--name"});
    REQUIRE_FALSE(bool(dangling));
    CHECK(llvm::toString(dangling.takeError()).find("requires a value") !=
          std::string::npos);
    // A following flag is not a value.
    auto swallowed = parseToolArgs(tool, {"--name", "--edge-kinds", "x"});
    REQUIRE_FALSE(bool(swallowed));
    llvm::consumeError(swallowed.takeError());

    auto positional = parseToolArgs(tool, {"Foo::bar"});
    REQUIRE_FALSE(bool(positional));
    llvm::consumeError(positional.takeError());

    const auto &dead = toolNamed(tools, "analyze_dead_code");
    auto notInt = parseToolArgs(dead, {"--limit", "many"});
    REQUIRE_FALSE(bool(notInt));
    CHECK(llvm::toString(notInt.takeError()).find("expects an integer") !=
          std::string::npos);
    auto badBool = parseToolArgs(dead, {"--include-system=maybe"});
    REQUIRE_FALSE(bool(badBool));
    llvm::consumeError(badBool.takeError());
  }

  SECTION("schema 'required' properties are enforced") {
    const auto &tool = toolNamed(tools, "reindex_tu");
    auto missing = parseToolArgs(tool, {});
    REQUIRE_FALSE(bool(missing));
    CHECK(llvm::toString(missing.takeError()).find("--file") !=
          std::string::npos);
    auto ok = parseToolArgs(tool, {"--file", "/src/a.cpp"});
    REQUIRE(bool(ok));
  }
}

TEST_CASE("tools declare the index sections they read", "[megascope][cli]") {
  auto tools = getRegisteredTools();
  CHECK(toolNamed(tools, "get_callers").needs == kSectionGraph);
  CHECK(toolNamed(tools, "graph_summary").needs == kSectionGraph);
  CHECK(toolNamed(tools, "query_exception_safety").needs ==
        (kSectionGraph | kSectionControlFlow));
  CHECK(toolNamed(tools, "query_same_lock").needs ==
        (kSectionGraph | kSectionControlFlow));
  CHECK(toolNamed(tools, "list_channels").needs ==
        (kSectionGraph | kSectionChannels));
  CHECK(toolNamed(tools, "reindex_tu").needs == kSectionAll);
  CHECK(sectionNames(kSectionGraph | kSectionChannels) ==
        std::vector<std::string>{"graph", "channels"});

  SECTION("the query verbs load only those sections") {
    IndexFile idx("needs");
    auto r = run({"graph-summary", "--index", idx.path, "-v"});
    CHECK(r.code == kExitResults);
    CHECK(r.err.find("control_flow skipped/") != std::string::npos);
    CHECK(r.err.find("channels skipped/") != std::string::npos);
    CHECK(r.err.find(" nodes ") != std::string::npos);
    CHECK(parseObject(r.out).getInteger("nodeCount") == 4);

    auto info = run({"info", "--index", idx.path, "-v"});
    CHECK(info.err.find("graph skipped/") != std::string::npos);
    CHECK(parseObject(info.out).getInteger("nodes") == 4);
    CHECK(parseObject(info.out).getArray("entry_points") != nullptr);

    auto tools = run({"tools", "--format", "json"});
    CHECK(tools.out.find("\"needs\":[\"graph\",\"control_flow\"]") !=
          std::string::npos);
  }
}

TEST_CASE("records keys name real list members", "[megascope][cli]") {
  auto tools = getRegisteredTools();
  CHECK(toolNamed(tools, "query_locks_held").recordsKey == "paths");
  CHECK(toolNamed(tools, "get_callers").recordsKey == "callers");
  // Peer lists (producers/consumers) must not make one of them "the" list.
  CHECK(toolNamed(tools, "query_channel").recordsKey.empty());
  CHECK(toolNamed(tools, "lookup_function").recordsKey.empty());
}

TEST_CASE("printToolHelp lists every schema property with its description",
          "[megascope][cli]") {
  auto tools = getRegisteredTools();
  const auto &tool = toolNamed(tools, "get_callers");
  std::string text;
  llvm::raw_string_ostream os(text);
  printToolHelp(tool, os);
  os.flush();
  CHECK(text.find("megascope get-callers") != std::string::npos);
  CHECK(text.find("--name <string>") != std::string::npos);
  CHECK(text.find("--edge-kinds <string>  (repeatable)") !=
        std::string::npos);
  CHECK(text.find("--index <file>") != std::string::npos);
  // The schema description text reaches the CLI unchanged.
  const auto *props = tool.inputSchema.getAsObject()->getObject("properties");
  auto desc = props->getObject("name")->getString("description");
  REQUIRE(desc.has_value());
  CHECK(text.find(desc->substr(0, 30).str()) != std::string::npos);
}

// ============================================================================
// Output contract
// ============================================================================

namespace {

llvm::json::Value callersPayload(size_t n) {
  llvm::json::Array callers;
  for (size_t i = 0; i < n; ++i) {
    llvm::json::Object c;
    c["callerName"] = "f" + std::to_string(i);
    c["line"] = static_cast<int64_t>(10 + i);
    callers.push_back(llvm::json::Value(std::move(c)));
  }
  llvm::json::Object obj;
  obj["function"] = "helper";
  obj["callerCount"] = static_cast<int64_t>(n);
  obj["callers"] = std::move(callers);
  return llvm::json::Value(std::move(obj));
}

std::string emit(const llvm::json::Value &payload, llvm::StringRef key,
                 OutputFormat format, bool pretty, int *code = nullptr,
                 std::string *errText = nullptr) {
  std::string out, err;
  llvm::raw_string_ostream os(out), es(err);
  int c = emitToolResult(payload, key, format, pretty, os, es, "get-callers");
  os.flush();
  es.flush();
  if (code)
    *code = c;
  if (errText)
    *errText = err;
  return out;
}

} // namespace

TEST_CASE("exit codes follow the payload contract", "[megascope][cli]") {
  CHECK(exitCodeFor(callersPayload(2), "callers") == kExitResults);
  CHECK(exitCodeFor(callersPayload(0), "callers") == kExitEmpty);
  // A scalar record is a result even without a records key.
  llvm::json::Object scalar;
  scalar["usr"] = "c:@F@f#";
  CHECK(exitCodeFor(llvm::json::Value(std::move(scalar)), "") ==
        kExitResults);
  CHECK(exitCodeFor(usageError("Missing required parameter 'name'"), "") ==
        kExitUsage);
  CHECK(exitCodeFor(usageError("Invalid call_site format"), "") ==
        kExitUsage);
  CHECK(exitCodeFor(notFoundError("Function not found: nope"), "") ==
        kExitEmpty);
  CHECK(exitCodeFor(unavailableError("No channel index loaded"), "") ==
        kExitIndex);
  llvm::json::Object amb;
  amb["ambiguous"] = true;
  amb["candidates"] = llvm::json::Array{llvm::json::Object{{"usr", "a"}}};
  CHECK(exitCodeFor(llvm::json::Value(std::move(amb)), "") ==
        kExitAmbiguous);
}

TEST_CASE("json output is one compact line, --pretty indents",
          "[megascope][cli]") {
  int code = 0;
  std::string out =
      emit(callersPayload(2), "callers", OutputFormat::Json, false, &code);
  CHECK(code == kExitResults);
  REQUIRE(lines(out).size() == 1);
  auto obj = parseObject(out);
  CHECK(obj.getArray("callers")->size() == 2);

  std::string pretty =
      emit(callersPayload(2), "callers", OutputFormat::Json, true);
  CHECK(lines(pretty).size() > 1);
  CHECK(parseObject(pretty).getArray("callers")->size() == 2);
}

TEST_CASE("ndjson output is a _summary line then one record per line",
          "[megascope][cli]") {
  std::string out =
      emit(callersPayload(3), "callers", OutputFormat::Ndjson, false);
  auto ls = lines(out);
  REQUIRE(ls.size() == 4);
  auto head = parseObject(ls[0]);
  const auto *summary = head.getObject("_summary");
  REQUIRE(summary != nullptr);
  CHECK(summary->getInteger("callerCount") == 3);
  CHECK(summary->getString("function") == "helper");
  CHECK(summary->get("callers") == nullptr);
  CHECK(parseObject(ls[1]).getString("callerName") == "f0");
  CHECK(parseObject(ls[3]).getString("callerName") == "f2");

  SECTION("a scalar payload is a single line") {
    llvm::json::Object scalar;
    scalar["usr"] = "c:@F@f#";
    std::string one = emit(llvm::json::Value(std::move(scalar)), "",
                           OutputFormat::Ndjson, false);
    CHECK(lines(one).size() == 1);
  }

  SECTION("ambiguity candidates stream one per line") {
    llvm::json::Object amb;
    amb["ambiguous"] = true;
    amb["name"] = "dup";
    amb["candidates"] = llvm::json::Array{llvm::json::Object{{"usr", "a"}},
                                          llvm::json::Object{{"usr", "b"}}};
    int code = 0;
    std::string text = emit(llvm::json::Value(std::move(amb)), "",
                            OutputFormat::Ndjson, false, &code);
    CHECK(code == kExitAmbiguous);
    auto al = lines(text);
    REQUIRE(al.size() == 3);
    CHECK(parseObject(al[0]).getObject("_summary")->getBoolean("ambiguous") ==
          true);
    CHECK(parseObject(al[2]).getString("usr") == "b");
  }
}

TEST_CASE("tsv output has a sorted header and escaped cells",
          "[megascope][cli]") {
  auto payload = callersPayload(2);
  // A tab and a nested value in one record exercise escaping and JSON
  // fallback; a field missing from the other record leaves an empty cell.
  auto *first = (*payload.getAsObject()->getArray("callers"))[0].getAsObject();
  (*first)["note"] = "has\ttab";
  (*first)["nested"] = llvm::json::Object{{"k", 1}};
  std::string out = emit(payload, "callers", OutputFormat::Tsv, false);
  auto ls = lines(out);
  REQUIRE(ls.size() == 3);
  CHECK(ls[0] == "callerName\tline\tnested\tnote");
  CHECK(ls[1] == "f0\t10\t{\"k\":1}\thas\\ttab");
  CHECK(ls[2] == "f1\t11\t\t");

  SECTION("an empty record list prints nothing (columns unknown)") {
    int code = 0;
    std::string none =
        emit(callersPayload(0), "callers", OutputFormat::Tsv, false, &code);
    CHECK(code == kExitEmpty);
    CHECK(none.empty());
  }

  SECTION("errors go to stderr and a two-column error block on stdout") {
    int code = 0;
    std::string err;
    std::string text = emit(notFoundError("Function not found: x"), "callers",
                            OutputFormat::Tsv, false, &code, &err);
    CHECK(code == kExitEmpty);
    CHECK(err == "megascope get-callers: Function not found: x\n");
    CHECK(text == "error\tstatus\nFunction not found: x\tnot_found\n");
  }
}

TEST_CASE("error payloads reach stdout as JSON and stderr as a message",
          "[megascope][cli]") {
  int code = 0;
  std::string err;
  std::string out = emit(usageError("Missing required parameter 'name'"),
                         "callers", OutputFormat::Json, false, &code, &err);
  CHECK(code == kExitUsage);
  CHECK(parseObject(out).getString("error") ==
        "Missing required parameter 'name'");
  CHECK(err.find("megascope get-callers: Missing") != std::string::npos);
}

// ============================================================================
// Verb runner, end to end against a saved index
// ============================================================================

TEST_CASE("query verbs answer from a saved index", "[megascope][cli]") {
  IndexFile idx("verbs");

  SECTION("a tool verb with schema flags") {
    auto r = run({"get-callers", "--index", idx.path, "--name", "helper"});
    CHECK(r.code == kExitResults);
    CHECK(r.err.empty());
    auto obj = parseObject(r.out);
    REQUIRE(obj.getArray("callers") != nullptr);
    REQUIRE(obj.getArray("callers")->size() == 1);
    CHECK((*obj.getArray("callers"))[0].getAsObject()->getString(
              "callerName") == "main");
  }

  SECTION("underscore spelling, --format, and -v") {
    auto r = run({"get_callers", "--index", idx.path, "--name", "helper",
                  "--format", "ndjson", "-v"});
    CHECK(r.code == kExitResults);
    CHECK(lines(r.out).size() == 2);
    CHECK(r.err.find("megascope: loaded") != std::string::npos);
    auto t = run({"get-callers", "--index=" + idx.path, "--name=helper",
                  "--format=tsv"});
    CHECK(t.code == kExitResults);
    REQUIRE(lines(t.out).size() == 2);
    CHECK(llvm::StringRef(lines(t.out)[0]).contains("callerName"));
  }

  SECTION("common flags accept underscores and refuse a dangling value") {
    auto r = run({"get-callers", "--index", idx.path, "--name", "helper",
                  "--entry_point", "main", "--build_path", "/nowhere"});
    CHECK(r.code == kExitResults);
    // `--index` followed by another flag is a usage error, not exit 3.
    auto dangling = run({"get-callers", "--index", "--name", "helper"});
    CHECK(dangling.code == kExitUsage);
    CHECK(dangling.err.find("requires a value") != std::string::npos);
  }

  SECTION("call <tool> --args") {
    auto r = run({"call", "lookup_function", "--index", idx.path, "--args",
                  R"({"name":"helper"})"});
    CHECK(r.code == kExitResults);
    CHECK(parseObject(r.out).getString("qualifiedName") == "helper");
    auto bad = run({"call", "lookup_function", "--index", idx.path, "--args",
                    "[1,2]"});
    CHECK(bad.code == kExitUsage);
    CHECK(bad.err.find("--args must be a JSON object") != std::string::npos);
    auto none = run({"call", "--index", idx.path});
    CHECK(none.code == kExitUsage);
    auto bogus = run({"call", "bogus", "--index", idx.path});
    CHECK(bogus.code == kExitUsage);
    CHECK(bogus.err.find("unknown verb or tool 'bogus'") != std::string::npos);
  }

  SECTION("empty answers exit 1, ambiguity exits 4 with candidates") {
    auto empty = run({"lookup-function", "--index", idx.path, "--name",
                      "nope"});
    CHECK(empty.code == kExitEmpty);
    CHECK(parseObject(empty.out).getString("error").has_value());
    CHECK(empty.err.find("Function not found") != std::string::npos);

    auto amb = run({"lookup-function", "--index", idx.path, "--name", "dup"});
    CHECK(amb.code == kExitAmbiguous);
    auto obj = parseObject(amb.out);
    CHECK(obj.getBoolean("ambiguous") == true);
    REQUIRE(obj.getArray("candidates") != nullptr);
    CHECK(obj.getArray("candidates")->size() == 2);
    // usr resolves it.
    auto one = run({"lookup-function", "--index", idx.path, "--usr",
                    "c:@F@dup#b"});
    CHECK(one.code == kExitResults);
    CHECK(parseObject(one.out).getString("file") == "/src/b.cpp");
  }

  SECTION("usage errors exit 2 before the index is touched") {
    auto missingArg = run({"get-callers", "--index", "/nonexistent/x.vycs",
                           "--bogus"});
    CHECK(missingArg.code == kExitUsage);
    CHECK(missingArg.err.find("unknown flag '--bogus'") != std::string::npos);
    auto handlerUsage = run({"get-callers", "--index", idx.path});
    CHECK(handlerUsage.code == kExitUsage);
    CHECK(handlerUsage.err.find("Missing required parameter") !=
          std::string::npos);
    auto badFormat = run({"get-callers", "--index", idx.path, "--name", "x",
                          "--format", "xml"});
    CHECK(badFormat.code == kExitUsage);
    auto unknownVerb = run({"frobnicate", "--index", idx.path});
    CHECK(unknownVerb.code == kExitUsage);
    CHECK(unknownVerb.err.find("unknown verb or tool 'frobnicate'") !=
          std::string::npos);
    auto serveOnly = run({"reindex-tu", "--index", idx.path, "--file", "x"});
    CHECK(serveOnly.code == kExitUsage);
    CHECK(serveOnly.err.find("serve") != std::string::npos);
  }

  SECTION("index problems exit 3") {
    auto missing = run({"get-callers", "--index", "/nonexistent/x.vycs",
                        "--name", "helper"});
    CHECK(missing.code == kExitIndex);
    CHECK(missing.err.find("no index at /nonexistent/x.vycs") !=
          std::string::npos);
    std::string garbage = tempIndexPath("garbage");
    {
      std::error_code ec;
      llvm::raw_fd_ostream os(garbage, ec);
      REQUIRE_FALSE(ec);
      os << "not a snapshot";
    }
    auto bad = run({"get-callers", "--index", garbage, "--name", "helper"});
    std::remove(garbage.c_str());
    CHECK(bad.code == kExitIndex);
    CHECK(bad.err.find("cannot load index") != std::string::npos);
  }

  SECTION("$VYCOR_INDEX supplies the index when no flag is given") {
    setenv("VYCOR_INDEX", idx.path.c_str(), 1);
    auto r = run({"lookup-function", "--name", "helper"});
    unsetenv("VYCOR_INDEX");
    CHECK(r.code == kExitResults);
    CHECK(parseObject(r.out).getString("qualifiedName") == "helper");
  }

  SECTION("--help for a tool never loads the index") {
    auto r = run({"get-callers", "--index", "/nonexistent/x.vycs", "--help"});
    CHECK(r.code == kExitResults);
    CHECK(r.out.find("--edge-kinds") != std::string::npos);
    auto verbs = run({"help"});
    CHECK(verbs.code == kExitResults);
    CHECK(verbs.out.find("batch") != std::string::npos);
    auto none = run({});
    CHECK(none.code == kExitUsage);
  }

  SECTION("info describes the index") {
    auto r = run({"info", "--index", idx.path});
    CHECK(r.code == kExitResults);
    auto obj = parseObject(r.out);
    CHECK(obj.getString("index") == idx.path);
    CHECK(obj.getInteger("format_version") == SnapshotIO::kFormatVersion);
    CHECK(obj.getInteger("file_count") == 2);
    CHECK(obj.getInteger("nodes") == 4);
    CHECK(obj.getInteger("edges") == 1);
    CHECK(obj.get("files") == nullptr);
    const auto *cfg = obj.getObject("config");
    REQUIRE(cfg != nullptr);
    CHECK(cfg->getArray("collapse_paths")->size() == 1);
    // Provenance and coverage come from the meta section alone; the
    // fixture recorded no outcomes, so its two TUs count as failed.
    const auto *prov = obj.getObject("provenance");
    REQUIRE(prov != nullptr);
    CHECK(prov->getString("analyzer") == "");
    CHECK(prov->getInteger("bake_start_ns") == 0);
    const auto *cov = obj.getObject("coverage");
    REQUIRE(cov != nullptr);
    CHECK(cov->getInteger("requested") == 2);
    CHECK(cov->getInteger("indexed") == 0);
    CHECK(cov->getInteger("failed") == 2);
    CHECK(cov->getBoolean("complete") == false);

    IndexFile bare("nofiles", /*withFiles=*/false);
    auto noFiles = run({"info", "--index", bare.path, "--files"});
    CHECK(noFiles.code == kExitResults); // a presentation flag, not an answer
    CHECK(parseObject(noFiles.out).getArray("files")->empty());

    auto files = run({"info", "--index", idx.path, "--files", "--format",
                      "ndjson"});
    CHECK(files.code == kExitResults);
    auto ls = lines(files.out);
    REQUIRE(ls.size() == 3);
    CHECK(parseObject(ls[1]).getString("path") == "/src/a.cpp");
    CHECK(parseObject(ls[1]).getString("status") == "skipped");
    CHECK(parseObject(ls[1]).getString("fingerprint") == "");
    CHECK(parseObject(ls[2]).getInteger("size") == 4);
  }

  SECTION("batch answers NDJSON requests in order on one index") {
    std::string requests =
        R"({"id":1,"tool":"lookup_function","args":{"name":"helper"}})"
        "\n"
        "\n" // blank lines are skipped
        R"({"id":"two","tool":"get-callers","args":{"name":"nope"}})"
        "\n"
        R"({"id":3,"tool":"no_such_tool"})"
        "\n"
        "this is not json\n"
        R"({"tool":"lookup_function","arguments":{"name":"dup"}})"
        "\n";
    auto r = run({"batch", "--index", idx.path}, requests);
    CHECK(r.code == kExitResults);
    auto ls = lines(r.out);
    REQUIRE(ls.size() == 5);

    auto first = parseObject(ls[0]);
    CHECK(first.getInteger("id") == 1);
    CHECK(first.getString("tool") == "lookup_function");
    CHECK(first.getInteger("exit") == kExitResults);
    CHECK(first.getObject("result")->getString("qualifiedName") == "helper");

    auto second = parseObject(ls[1]);
    CHECK(second.getString("id") == "two");
    CHECK(second.getString("tool") == "get_callers");
    // An unknown callee is not_found (exit 1), not an empty caller list
    // that would read as "nothing calls it".
    CHECK(second.getInteger("exit") == kExitEmpty);
    CHECK(second.getString("status") == "not_found");
    CHECK(second.getObject("result")->getArray("callers") == nullptr);
    CHECK(second.getObject("result")->getArray("didYouMean") != nullptr);

    auto third = parseObject(ls[2]);
    CHECK(third.getInteger("id") == 3);
    CHECK(third.getString("error")->contains("unknown tool 'no_such_tool'"));
    CHECK(third.getInteger("exit") == kExitUsage);

    auto fourth = parseObject(ls[3]);
    CHECK(fourth.getString("error")->starts_with("line 5:"));

    auto fifth = parseObject(ls[4]);
    CHECK(fifth.get("id") == nullptr);
    CHECK(fifth.getInteger("exit") == kExitAmbiguous);
    CHECK(fifth.getObject("result")->getArray("candidates")->size() == 2);
  }
}

TEST_CASE("tools lists every registered tool", "[megascope][cli]") {
  auto tools = getRegisteredTools();

  auto text = run({"tools"});
  CHECK(text.code == kExitResults);
  CHECK(text.out.find("get-callers") != std::string::npos);
  CHECK(text.out.find("reindex-tu") != std::string::npos);
  CHECK(text.out.find("[serve only]") != std::string::npos);
  CHECK(lines(text.out).size() == tools.size());
  // One sentence per tool, and "e.g. " inside a sentence does not end it.
  for (const auto &line : lines(text.out)) {
    CHECK_FALSE(llvm::StringRef(line).ends_with("e.g."));
    CHECK_FALSE(llvm::StringRef(line).ends_with("(e.g."));
  }

  auto json = run({"tools", "--format", "json"});
  CHECK(json.code == kExitResults);
  auto obj = parseObject(json.out);
  CHECK(obj.getInteger("count") == static_cast<int64_t>(tools.size()));
  const auto *arr = obj.getArray("tools");
  REQUIRE(arr != nullptr);
  bool sawCallers = false;
  for (const auto &t : *arr) {
    const auto *o = t.getAsObject();
    if (o->getString("name") == "get_callers") {
      sawCallers = true;
      CHECK(o->getString("verb") == "get-callers");
      CHECK(o->getString("records") == "callers");
      CHECK(o->getBoolean("cli") == true);
      CHECK(o->getObject("inputSchema") != nullptr);
    }
  }
  CHECK(sawCallers);

  auto nd = run({"tools", "--format", "ndjson"});
  CHECK(lines(nd.out).size() == tools.size() + 1);

  auto stray = run({"tools", "extra"});
  CHECK(stray.code == kExitUsage);
}

// ============================================================================
// dump
// ============================================================================

namespace {

// An index whose control-flow section has two contexts (one under a
// try/catch with a guard) so dump has something to stream.
std::string saveContextIndex(llvm::StringRef tag) {
  CallGraph g;
  g.addNode({"main", "/src/a.cpp", 10, true, false, ""}, "/src/a.cpp");
  g.addNode({"helper", "/src/a.cpp", 3, false, false, ""}, "/src/a.cpp");
  g.addEdge({"main", "helper", EdgeKind::DirectCall, Confidence::Proven,
             "/src/a.cpp:11:3", 0, ExecutionContext::Synchronous},
            "/src/a.cpp");
  ControlFlowIndex cf;
  {
    CallSiteContext ctx;
    ctx.callerName = "main";
    ctx.calleeName = "helper";
    ctx.callSite = "/src/a.cpp:11:3";
    ctx.tuPath = "/src/a.cpp";
    TryCatchScope scope;
    scope.tryLocation = "/src/a.cpp:10:3";
    scope.enclosingFunction = "main";
    CatchHandlerInfo h;
    h.caughtType = "std::exception";
    h.location = "/src/a.cpp:13:3";
    scope.handlers.push_back(std::move(h));
    ctx.enclosingTryCatches.push_back(std::move(scope));
    ConditionalGuard guard;
    guard.conditionText = "n > \"0\"";
    guard.location = "/src/a.cpp:10:7";
    guard.inTrueBranch = true;
    ctx.enclosingGuards.push_back(std::move(guard));
    cf.addCallSiteContext(std::move(ctx));
  }
  {
    CallSiteContext ctx;
    ctx.callerName = "main";
    ctx.calleeName = "printf";
    ctx.callSite = "/src/a.cpp:12:3";
    ctx.tuPath = "/src/a.cpp";
    ctx.callerNoexcept = NoexceptSpec::Noexcept;
    cf.addCallSiteContext(std::move(ctx));
  }
  SnapshotMeta meta;
  meta.files = {{"/src/a.cpp", 1, 2}};
  std::string path = tempIndexPath(tag);
  REQUIRE(SnapshotIO::save(path, g, cf, meta));
  return path;
}

} // namespace

TEST_CASE("dump streams every call-site context", "[megascope][cli]") {
  std::string path = saveContextIndex("dump");

  SECTION("ndjson is the default: a summary line then one record each") {
    auto r = run({"dump", "--index", path});
    CHECK(r.code == kExitResults);
    CHECK(r.err.empty());
    auto ls = lines(r.out);
    REQUIRE(ls.size() == 3);
    auto head = parseObject(ls[0]);
    REQUIRE(head.getObject("_summary") != nullptr);
    CHECK(head.getObject("_summary")->getInteger("callSiteCount") == 2);
    CHECK(head.getObject("_summary")->getInteger("channelSiteCount") == 0);
    auto first = parseObject(ls[1]);
    CHECK(first.getString("kind") == "call_site");
    CHECK(first.getString("callerName") == "main");
    CHECK(first.getString("calleeName") == "helper");
    CHECK(first.getString("tu") == "/src/a.cpp");
    CHECK(first.getString("callerNoexcept") == "none");
    REQUIRE(first.getArray("enclosingTryCatches") != nullptr);
    REQUIRE(first.getArray("enclosingTryCatches")->size() == 1);
    auto *scope = (*first.getArray("enclosingTryCatches"))[0].getAsObject();
    CHECK(scope->getString("tryLocation") == "/src/a.cpp:10:3");
    REQUIRE(first.getArray("enclosingGuards") != nullptr);
    // The quote in the condition survives (D3: real JSON, not hand-built).
    CHECK((*first.getArray("enclosingGuards"))[0]
              .getAsObject()
              ->getString("conditionText") == "n > \"0\"");
    auto second = parseObject(ls[2]);
    CHECK(second.getString("calleeName") == "printf");
    CHECK(second.getString("callerNoexcept") == "noexcept");
  }

  SECTION("json is one document with the same records") {
    auto r = run({"dump", "--index", path, "--format", "json"});
    CHECK(r.code == kExitResults);
    CHECK(lines(r.out).size() == 1);
    auto obj = parseObject(r.out);
    CHECK(obj.getInteger("callSiteCount") == 2);
    REQUIRE(obj.getArray("callSites") != nullptr);
    CHECK(obj.getArray("callSites")->size() == 2);
    REQUIRE(obj.getArray("channelSites") != nullptr);
    CHECK(obj.getArray("channelSites")->empty());
    auto pretty = run({"dump", "--index", path, "--format", "json",
                       "--pretty"});
    CHECK(pretty.code == kExitResults);
    CHECK(lines(pretty.out).size() > 3);
    CHECK(parseObject(pretty.out).getInteger("callSiteCount") == 2);
  }

  SECTION("tsv and stray arguments are usage errors") {
    auto tsv = run({"dump", "--index", path, "--format", "tsv"});
    CHECK(tsv.code == kExitUsage);
    CHECK(tsv.err.find("tsv") != std::string::npos);
    auto stray = run({"dump", "--index", path, "extra"});
    CHECK(stray.code == kExitUsage);
  }

  SECTION("an index without contexts is empty") {
    IndexFile bare("dump-empty");
    auto r = run({"dump", "--index", bare.path});
    CHECK(r.code == kExitEmpty);
    CHECK(lines(r.out).size() == 1);
  }
  std::remove(path.c_str());
}

// ============================================================================
// Ephemeral mode: --source et al. bake in memory
// ============================================================================

namespace {

// A compilation database over examples/deep_chains in a scratch directory
// (the checked-in one carries its author's absolute paths).
struct ScratchBuildDir {
  std::string dir;
  std::string base =
      std::string(PROJECT_SOURCE_DIR) + "/examples/deep_chains/";
  ScratchBuildDir() {
    llvm::SmallString<128> tmp;
    llvm::sys::path::system_temp_directory(/*ErasedOnReboot=*/true, tmp);
    llvm::sys::path::append(tmp, "vycor-cli-ephemeral");
    llvm::SmallString<128> made;
    REQUIRE(!llvm::sys::fs::createUniqueDirectory(tmp, made));
    dir = std::string(made);
    llvm::json::Array entries;
    for (const char *tu : {"main.cpp", "callbacks.cpp"}) {
      llvm::json::Object e;
      e["directory"] = base;
      e["file"] = base + tu;
      e["arguments"] = llvm::json::Array{"clang++", "-std=c++17",
                                         "-I" + base, "-c", base + tu};
      entries.push_back(std::move(e));
    }
    std::error_code ec;
    llvm::raw_fd_ostream os(dir + "/compile_commands.json", ec);
    REQUIRE(!ec);
    os << llvm::json::Value(std::move(entries));
  }
  ~ScratchBuildDir() {
    std::remove((dir + "/compile_commands.json").c_str());
    llvm::sys::fs::remove(dir);
  }
};

} // namespace

TEST_CASE("query verbs bake in memory when given --source",
          "[megascope][cli][ephemeral]") {
  ScratchBuildDir build;

  SECTION("a tool answers from the in-memory bake") {
    auto r = run({"get-callees", "--build-path", build.dir, "--source",
                  build.base + "main.cpp", "--name", "main", "--threads",
                  "1", "-v"});
    CHECK(r.code == kExitResults);
    CHECK(r.err.find("baked 1 TU(s) in memory") != std::string::npos);
    auto obj = parseObject(r.out);
    REQUIRE(obj.getArray("callees") != nullptr);
    CHECK(!obj.getArray("callees")->empty());
  }

  SECTION("--source-re selects from the database; dump streams the bake") {
    auto r = run({"dump", "--build-path", build.dir, "--source-re",
                  "callbacks\\.cpp$", "--threads", "1"});
    CHECK(r.code == kExitResults);
    auto ls = lines(r.out);
    REQUIRE(ls.size() >= 2);
    auto head = parseObject(ls[0]);
    CHECK(head.getObject("_summary")->getInteger("callSiteCount") ==
          static_cast<int64_t>(ls.size() - 1));
    auto tu = parseObject(ls[1]).getString("tu");
    REQUIRE(tu.has_value());
    CHECK(tu->ends_with("callbacks.cpp"));
  }

  SECTION("selection that matches nothing") {
    auto r = run({"get-callers", "--build-path", build.dir, "--source-re",
                  "nothing-here", "--name", "main"});
    CHECK(r.code == kExitIndex);
    CHECK(r.err.find("matches no TU") != std::string::npos);
  }

  SECTION("usage errors") {
    auto withIndex = run({"get-callers", "--build-path", build.dir,
                          "--source", build.base + "main.cpp", "--index",
                          "/nowhere.vycs", "--name", "main"});
    CHECK(withIndex.code == kExitUsage);
    CHECK(withIndex.err.find("drop --index") != std::string::npos);
    auto noBuild = run({"get-callers", "--source", build.base + "main.cpp",
                        "--name", "main"});
    CHECK(noBuild.code == kExitUsage);
    CHECK(noBuild.err.find("--build-path is required") != std::string::npos);
    auto info = run({"info", "--build-path", build.dir, "--source",
                     build.base + "main.cpp"});
    CHECK(info.code == kExitUsage);
    auto threads = run({"get-callers", "--build-path", build.dir, "--source",
                        build.base + "main.cpp", "--threads", "many",
                        "--name", "main"});
    CHECK(threads.code == kExitUsage);
    CHECK(threads.err.find("--threads") != std::string::npos);
    auto badDb = run({"get-callers", "--build-path", build.dir + "/none",
                      "--source", build.base + "main.cpp", "--name", "main"});
    CHECK(badDb.code == kExitIndex);
    // A bake qualifier without a selection flag is not silently ignored.
    IndexFile idx("ephemeral-stray");
    auto stray = run({"get-callers", "--index", idx.path, "--threads", "2",
                      "--name", "helper"});
    CHECK(stray.code == kExitUsage);
    CHECK(stray.err.find("--threads only applies") != std::string::npos);
  }
}
