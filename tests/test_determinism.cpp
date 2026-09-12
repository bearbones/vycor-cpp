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

// Deterministic output (docs/deterministic-output.md): the same sources
// baked in any TU order, on any thread count, cold or warm, answer every
// tool byte for byte the same. These compare raw serialized payloads —
// the order of every list — not sorted lines, which is what the CLI
// goldens compare.

#include "vycor/callgraph/ControlFlowIndex.h"
#include "vycor/callgraph/ControlFlowOracle.h"
#include "vycor/query/Tools.h"

#include "clang/Tooling/CompilationDatabase.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace vycor;

namespace {

const std::string kBase =
    std::string(PROJECT_SOURCE_DIR) + "/examples/deep_chains/";

std::vector<std::string> deepChainsFiles() {
  std::vector<std::string> files;
  for (const char *f :
       {"main.cpp", "pipeline.cpp", "stage1_ingest.cpp", "stage2_parse.cpp",
        "stage3_transform.cpp", "stage4_dispatch.cpp", "stage5_sink.cpp",
        "plugins.cpp", "workers.cpp", "tokenizer.cpp", "scheduler.cpp",
        "callbacks.cpp", "async_workers.cpp", "lambda_callbacks.cpp"})
    files.push_back(kBase + f);
  return files;
}

BakedIndexes bakeDeepChains(std::vector<std::string> files,
                            unsigned threads) {
  clang::tooling::FixedCompilationDatabase compDb(
      ".", {"-std=c++17", "-I" + kBase});
  return bakeIndexes(compDb, files, {}, threads);
}

std::string serialized(const llvm::json::Value &v) {
  std::string s;
  llvm::raw_string_ostream os(s);
  os << v; // object keys are sorted by the serializer
  os.flush();
  return s;
}

const ToolEntry &toolNamed(const std::vector<ToolEntry> &tools,
                           llvm::StringRef name) {
  for (const auto &t : tools)
    if (t.name == name)
      return t;
  FAIL("no tool named " << name.str());
  return tools.front();
}

using Call = std::pair<std::string, llvm::json::Object>;

/// One call of every list-emitting tool over the deep_chains fixture,
/// including cuts at limit boundaries.
std::vector<Call> battery() {
  using O = llvm::json::Object;
  return {
      {"search_functions", O{{"query", "stage"}, {"limit", 50}}},
      {"search_functions", O{{"query", "a"}, {"limit", 7}}},
      {"lookup_function", O{{"name", "stage2_parse"}}},
      {"get_callees", O{{"name", "Pipeline::run"}}},
      {"get_callees", O{{"name", "main"}}},
      {"get_callers", O{{"name", "stage3_transform"}}},
      {"get_callers", O{{"name", "cbs::startupHook"}}},
      {"find_call_chain",
       O{{"from", "main"}, {"to", "stage5_sink"}, {"max_depth", 10}}},
      {"find_call_chain",
       O{{"from", "main"}, {"to", "stage5_sink"}, {"max_depth", 10},
         {"max_paths", 1}}},
      {"query_exception_safety",
       O{{"function", "stage5_sink"}, {"exception_type", "std::exception"}}},
      {"query_throw_propagation",
       O{{"function", "stage5_sink"}, {"exception_type", "std::exception"},
         {"max_paths", 2}}},
      {"query_all_path_contexts",
       O{{"function", "stage5_sink"}, {"max_paths", 5}}},
      {"query_nearest_catches", O{{"function", "stage5_sink"}}},
      {"query_locks_held", O{{"function", "stage5_sink"}}},
      {"analyze_dead_code", O{}},
      {"analyze_dead_code", O{{"limit", 3}, {"offset", 2}}},
      {"get_class_hierarchy",
       O{{"class_name", "Plugin"}, {"include_overrides", true},
         {"include_transitive", true}}},
      {"list_entry_points", O{}},
      {"graph_summary", O{}},
      {"list_callback_sites", O{}},
      {"list_concurrency_entry_points", O{}},
  };
}

/// Every battery answer over one set of indexes, keyed by the call.
std::map<std::string, std::string> answers(BakedIndexes &ix) {
  ControlFlowOracle oracle(ix.graph, ix.cfIndex);
  std::vector<std::string> entryPoints{"main"};
  QueryCache cache;
  ToolContext ctx{ix.graph, oracle, ix.cfIndex, entryPoints, &ix.channels,
                  &cache};
  ctx.facts.freshness = IndexFreshness::Baked;
  auto tools = getRegisteredTools();
  std::map<std::string, std::string> out;
  for (const auto &[tool, args] : battery()) {
    std::string key =
        tool + " " + serialized(llvm::json::Value(llvm::json::Object(args)));
    out[key] = serialized(runTool(toolNamed(tools, tool), args, ctx));
  }
  return out;
}

void expectSame(const std::map<std::string, std::string> &a,
                const std::map<std::string, std::string> &b,
                const std::string &what) {
  REQUIRE(a.size() == b.size());
  for (const auto &[key, va] : a) {
    INFO(what << ": " << key);
    auto it = b.find(key);
    REQUIRE(it != b.end());
    CHECK(va == it->second);
  }
}

} // namespace

// ============================================================================
// Real bakes: thread count, TU order, warm refresh
// ============================================================================

TEST_CASE("every tool answers the same over any bake of the same sources",
          "[determinism][deep_chains]") {
  auto files = deepChainsFiles();
  auto forward1 = bakeDeepChains(files, 1);
  const auto reference = answers(forward1);

  // A sanity check on the battery itself: the answers are not empty and
  // the lists it means to exercise are populated.
  {
    const std::string &callers =
        reference.at("get_callers {\"name\":\"stage3_transform\"}");
    CHECK(callers.find("\"callerCount\":") != std::string::npos);
    CHECK(callers.find("\"callerCount\":0") == std::string::npos);
    const std::string &sites = reference.at("list_callback_sites {}");
    CHECK(sites.find("\"targetCount\":0") == std::string::npos);
  }

  SECTION("four threads") {
    auto forward4 = bakeDeepChains(files, 4);
    expectSame(reference, answers(forward4), "threads 1 vs 4");
  }

  SECTION("reversed TU order, one thread and four") {
    std::vector<std::string> reversed(files.rbegin(), files.rend());
    auto reversed1 = bakeDeepChains(reversed, 1);
    expectSame(reference, answers(reversed1), "forward vs reversed");
    auto reversed4 = bakeDeepChains(reversed, 4);
    expectSame(reference, answers(reversed4), "forward vs reversed, 4");
  }

  SECTION("warm refresh: one TU removed and re-absorbed") {
    auto warm = bakeDeepChains(files, 2);
    const std::string tu = kBase + "stage3_transform.cpp";
    REQUIRE(warm.graph.removeTU(tu) > 0);
    warm.cfIndex.removeTU(tu);
    warm.channels.removeTU(tu);
    auto shard = bakeDeepChains({tu}, 1);
    warm.graph.absorb(shard.graph);
    warm.cfIndex.absorb(shard.cfIndex);
    warm.channels.absorb(shard.channels);
    expectSame(reference, answers(warm), "cold vs warm");
  }
}

// ============================================================================
// Ties: hand-built graphs inserted in both orders
// ============================================================================

namespace {

struct HandGraph {
  CallGraph graph;
  ControlFlowIndex cfIndex;
  std::vector<std::string> entryPoints{"main"};
  QueryCache cache;
  std::vector<ToolEntry> tools = getRegisteredTools();

  std::string run(llvm::StringRef tool, llvm::json::Object args) {
    ControlFlowOracle oracle(graph, cfIndex);
    ToolContext ctx{graph, oracle, cfIndex, entryPoints, nullptr, &cache};
    ctx.facts.freshness = IndexFreshness::Baked;
    return serialized(runTool(toolNamed(tools, tool), args, ctx));
  }
};

/// Two overloads of `f` (same qualified name, different usrs), two
/// callers of `g` at two sites, and two dead functions at one location
/// whose name order (dead_x, dead_y) is the reverse of their usr order
/// (dead_x is file-local, so its usr carries the file), inserted in the
/// given order.
HandGraph tieGraph(bool reversed) {
  HandGraph h;
  std::vector<CallGraphNode> nodes = {
      {"main", "main.cpp", 1, true, false, "", "c:@F@main#"},
      {"f", "f.cpp", 1, false, false, "", "c:@F@f#I#"},
      {"f", "f.cpp", 2, false, false, "", "c:@F@f#d#"},
      {"g", "g.cpp", 1, false, false, "", "c:@F@g#"},
      {"caller_a", "c.cpp", 1, false, false, "", "c:@F@caller_a#"},
      {"caller_b", "c.cpp", 2, false, false, "", "c:@F@caller_b#"},
      {"dead_x", "macro.cpp", 7, false, false, "", "c:macro.cpp@F@dead_x#"},
      {"dead_y", "macro.cpp", 7, false, false, "", "c:@F@dead_y#"},
  };
  std::vector<CallGraphEdge> edges = {
      {"c:@F@main#", "c:@F@caller_a#", EdgeKind::DirectCall,
       Confidence::Proven, "main.cpp:2:3", 0, ExecutionContext::Synchronous},
      {"c:@F@main#", "c:@F@caller_b#", EdgeKind::DirectCall,
       Confidence::Proven, "main.cpp:3:3", 0, ExecutionContext::Synchronous},
      {"c:@F@caller_b#", "c:@F@g#", EdgeKind::DirectCall, Confidence::Proven,
       "c.cpp:2:20", 0, ExecutionContext::Synchronous},
      {"c:@F@caller_a#", "c:@F@g#", EdgeKind::DirectCall, Confidence::Proven,
       "c.cpp:1:20", 0, ExecutionContext::Synchronous},
      {"c:@F@caller_a#", "c:@F@g#", EdgeKind::DirectCall, Confidence::Proven,
       "c.cpp:1:30", 0, ExecutionContext::Synchronous},
      {"c:@F@main#", "c:@F@f#d#", EdgeKind::DirectCall, Confidence::Proven,
       "main.cpp:4:3", 0, ExecutionContext::Synchronous},
      {"c:@F@main#", "c:@F@f#I#", EdgeKind::DirectCall, Confidence::Proven,
       "main.cpp:5:3", 0, ExecutionContext::Synchronous},
  };
  if (reversed) {
    std::reverse(nodes.begin(), nodes.end());
    std::reverse(edges.begin(), edges.end());
  }
  for (auto &n : nodes)
    h.graph.addNode(n, n.file);
  for (auto &e : edges)
    h.graph.addEdge(e, "tu.cpp");
  return h;
}

} // namespace

TEST_CASE("ties break the same way whatever the insertion order",
          "[determinism][ties]") {
  auto a = tieGraph(false);
  auto b = tieGraph(true);

  SECTION("search_functions: overloads at a limit boundary") {
    llvm::json::Object args{{"query", "f"}, {"limit", 1}};
    std::string ra = a.run("search_functions", args);
    CHECK(ra == b.run("search_functions", args));
    // The usr tie-break, not the insertion order, picks the survivor.
    CHECK(ra.find("c:@F@f#I#") != std::string::npos);
    CHECK(ra.find("c:@F@f#d#") == std::string::npos);
  }

  SECTION("get_callers: caller usr, then call site") {
    llvm::json::Object args{{"name", "g"}};
    std::string ra = a.run("get_callers", args);
    CHECK(ra == b.run("get_callers", args));
    auto posA20 = ra.find("c.cpp:1:20");
    auto posA30 = ra.find("c.cpp:1:30");
    auto posB = ra.find("c.cpp:2:20");
    REQUIRE(posA20 != std::string::npos);
    CHECK(posA20 < posA30);
    CHECK(posA30 < posB);
  }

  SECTION("get_callees: callee usr, then call site") {
    llvm::json::Object args{{"name", "main"}};
    std::string ra = a.run("get_callees", args);
    CHECK(ra == b.run("get_callees", args));
    // caller_a < caller_b < f#I# < f#d# by usr ('I' < 'd').
    CHECK(ra.find("main.cpp:2:3") < ra.find("main.cpp:3:3"));
    CHECK(ra.find("main.cpp:3:3") < ra.find("main.cpp:5:3"));
    CHECK(ra.find("main.cpp:5:3") < ra.find("main.cpp:4:3"));
  }

  SECTION("graph_summary: equal fan-in breaks on name") {
    llvm::json::Object args;
    std::string ra = a.run("graph_summary", args);
    CHECK(ra == b.run("graph_summary", args));
  }

  SECTION("analyze_dead_code: one location, two functions, usr order") {
    llvm::json::Object args;
    std::string ra = a.run("analyze_dead_code", args);
    CHECK(ra == b.run("analyze_dead_code", args));
    // "c:@F@dead_y#" < "c:macro.cpp@F@dead_x#": the usr, not the name.
    CHECK(ra.find("dead_y") < ra.find("dead_x"));
    llvm::json::Object page{{"limit", 1}, {"offset", 1}};
    std::string pa = a.run("analyze_dead_code", page);
    CHECK(pa == b.run("analyze_dead_code", page));
    CHECK(pa.find("dead_x") != std::string::npos);
    CHECK(pa.find("dead_y") == std::string::npos);
  }

  SECTION("a name only a removed TU knew is unknown, as in a clean bake") {
    // The interner never forgets a string; the search must not take
    // "interned" for "known", or the warm index claims a complete,
    // exhaustive answer (no callers) for a function that no longer
    // exists, where the clean bake says the target is unknown.
    for (auto &h : {&a, &b}) {
      h->graph.addNode({"gone", "gone.cpp", 1, false, false, "",
                        "c:@F@gone#"},
                       "gone.cpp");
      h->graph.addEdge({"c:@F@main#", "c:@F@gone#", EdgeKind::DirectCall,
                        Confidence::Proven, "gone.cpp:2:3", 0,
                        ExecutionContext::Synchronous},
                       "gone.cpp");
      REQUIRE(h->graph.removeTU("gone.cpp") > 0);
    }
    auto fresh = tieGraph(false);
    for (const char *target : {"gone", "c:@F@gone#"}) {
      llvm::json::Object args{{"to", target}};
      std::string ra = a.run("find_call_chain", args);
      CHECK(ra == b.run("find_call_chain", args));
      CHECK(ra == fresh.run("find_call_chain", args));
      CHECK(ra.find("\"complete\":false") != std::string::npos);
    }
    // A declared-only callee (edge end, no node) stays a known target.
    llvm::json::Object declared{{"to", "c:@F@g#"}};
    CHECK(a.run("find_call_chain", declared).find("\"complete\":true") !=
          std::string::npos);
  }

  SECTION("list_callback_sites and thread entries") {
    for (auto &h : {&a, &b}) {
      h->graph.addEdge({"c:@F@caller_b#", "c:macro.cpp@F@dead_x#",
                        EdgeKind::FunctionPointer, Confidence::Plausible,
                        "c.cpp:2:40", 1, ExecutionContext::Synchronous},
                       "tu.cpp");
      h->graph.addEdge({"c:@F@caller_a#", "c:macro.cpp@F@dead_x#",
                        EdgeKind::FunctionPointer, Confidence::Plausible,
                        "c.cpp:1:40", 1, ExecutionContext::Synchronous},
                       "tu.cpp");
      h->graph.addEdge({"c:@F@caller_b#", "c:@F@dead_y#",
                        EdgeKind::ThreadEntry, Confidence::Proven,
                        "c.cpp:2:50", 0, ExecutionContext::ThreadSpawn},
                       "tu.cpp");
      h->graph.addEdge({"c:@F@caller_a#", "c:@F@dead_y#",
                        EdgeKind::ThreadEntry, Confidence::Proven,
                        "c.cpp:1:50", 0, ExecutionContext::ThreadSpawn},
                       "tu.cpp");
    }
    llvm::json::Object args;
    std::string sa = a.run("list_callback_sites", args);
    CHECK(sa == b.run("list_callback_sites", args));
    CHECK(sa.find("c.cpp:1:40") < sa.find("c.cpp:2:40"));
    std::string ta = a.run("list_concurrency_entry_points", args);
    CHECK(ta == b.run("list_concurrency_entry_points", args));
    CHECK(ta.find("c.cpp:1:50") < ta.find("c.cpp:2:50"));
  }
}
