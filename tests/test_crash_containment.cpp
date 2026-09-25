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

// Crash and hang containment (docs/plans/2026-09-hardening/
// I-crash-hang-containment.md; docs/design-f12-subprocess-workers.md
// "Failure modes"). Each reproduction hangs, deadlocks, or overflows the
// stack without its fix — the before/after record is in the PR that added
// this file.

#include "vycor/anneal/Analyzer.h"
#include "vycor/callgraph/CallGraph.h"
#include "vycor/callgraph/ControlFlowIndex.h"
#include "vycor/callgraph/ControlFlowOracle.h"
#include "vycor/callgraph/CrashGuard.h"
#include "vycor/callgraph/PathSearch.h"
#include "vycor/callgraph/Snapshot.h"
#include "vycor/callgraph/WorkerPool.h"
#include "vycor/query/Limits.h"
#include "vycor/query/Tools.h"

#include "clang/Tooling/CompilationDatabase.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace vycor;

namespace {

using Clock = std::chrono::steady_clock;

double secondsSince(Clock::time_point t0) {
  return std::chrono::duration<double>(Clock::now() - t0).count();
}

struct ScratchDir {
  std::string dir;
  ScratchDir() {
    llvm::SmallString<128> p;
    REQUIRE(!llvm::sys::fs::createUniqueDirectory("vycor-containment", p));
    llvm::SmallString<256> abs;
    REQUIRE(!llvm::sys::fs::real_path(p, abs));
    dir = std::string(abs);
  }
  ~ScratchDir() { llvm::sys::fs::remove_directories(dir); }

  std::string write(const std::string &name, const std::string &content,
                    bool executable = false) const {
    std::string path = dir + "/" + name;
    {
      std::ofstream out(path, std::ios::binary | std::ios::trunc);
      REQUIRE(out.good());
      out << content;
    }
    if (executable)
      llvm::sys::fs::setPermissions(path, llvm::sys::fs::all_read |
                                              llvm::sys::fs::all_exe |
                                              llvm::sys::fs::owner_write);
    return path;
  }
};

// A fake worker: prints a WORKER-TU marker per TU (every `pause` seconds),
// hangs forever on any TU whose path contains "hang", and otherwise exits 0
// after copying `shard` (when given) to its --worker-out path. Accepts both
// a bare TU list and the production megascope/anneal argv.
std::string fakeWorkerScript(const ScratchDir &d, const std::string &shard,
                             const char *pause = "0") {
  return d.write("fake-worker.sh",
                 std::string("#!/bin/sh\n"
                             "out=''\n"
                             "tus=''\n"
                             "while [ $# -gt 0 ]; do\n"
                             "  case \"$1\" in\n"
                             "    --worker-out) out=\"$2\"; shift ;;\n"
                             "    --source) tus=\"$tus $2\"; shift ;;\n"
                             "    -*|megascope|anneal) ;;\n"
                             "    *) tus=\"$tus $1\" ;;\n"
                             "  esac\n"
                             "  shift\n"
                             "done\n"
                             "for tu in $tus; do\n"
                             "  echo \"WORKER-TU $tu\" >&2\n"
                             "  case \"$tu\" in *hang*) exec sleep 3600 ;; "
                             "esac\n"
                             "  sleep ") +
                     pause +
                     "\n"
                     "done\n" +
                     (shard.empty() ? std::string()
                                    : "cp '" + shard + "' \"$out\"\n") +
                     "exit 0\n",
                 /*executable=*/true);
}

} // namespace

// ============================================================================
// Hang: a worker that never finishes. Before worker timeouts the parent
// waited on it forever.
// ============================================================================

TEST_CASE("dispatchIsolated: a hung worker is killed and its TU timed out",
          "[containment][worker-timeout]") {
  ScratchDir d;
  const std::string script = fakeWorkerScript(d, "");
  WorkerLimits limits;
  limits.timeoutSeconds = 1;
  WorkerRunner runner = [&](const std::vector<std::string> &batch,
                            const std::string &, const std::string &log) {
    std::vector<std::string> argv{script};
    argv.insert(argv.end(), batch.begin(), batch.end());
    return runWorkerProcess(argv, log, limits, "test");
  };

  std::vector<std::string> consumed;
  std::map<std::string, WorkerFailure> dropped;
  auto t0 = Clock::now();
  dispatchIsolated(
      runner, {"/tu/ok.cpp", "/tu/hang.cpp"}, 1, d.dir,
      [&](const std::string &, const std::vector<std::string> &tus, double) {
        consumed.insert(consumed.end(), tus.begin(), tus.end());
        return true;
      },
      [&](const std::string &tu, WorkerFailure why) { dropped[tu] = why; },
      /*batchSizeOverride=*/2);

  CHECK(secondsSince(t0) < 30);
  // The hung TU is identified by its marker and dropped as timed out; the
  // rest of its batch is re-dispatched and lands.
  REQUIRE(dropped.size() == 1);
  CHECK(dropped.at("/tu/hang.cpp") == WorkerFailure::TimedOut);
  CHECK(consumed == std::vector<std::string>{"/tu/ok.cpp"});
}

TEST_CASE("runWorkerProcess: the timeout restarts at every WORKER-TU marker",
          "[containment][worker-timeout]") {
  ScratchDir d;
  // Four TUs at 0.6 s each: 2.4 s in total, never 1 s without a marker.
  const std::string script = fakeWorkerScript(d, "", "0.6");
  WorkerLimits limits;
  limits.timeoutSeconds = 1;
  int rc = runWorkerProcess({script, "/tu/a.cpp", "/tu/b.cpp", "/tu/c.cpp",
                             "/tu/d.cpp"},
                            d.dir + "/log", limits, "test");
  CHECK(rc == 0);
}

TEST_CASE("bakeIsolated: a hung megascope worker is recorded as timeout",
          "[containment][worker-timeout]") {
  ScratchDir d;
  // Clean workers hand back an empty but valid shard.
  const std::string shard = d.dir + "/empty.snap";
  REQUIRE(SnapshotIO::save(shard, CallGraph(), ControlFlowIndex(),
                           SnapshotMeta()));
  const std::string script = fakeWorkerScript(d, shard);
  McpBakeConfig cfg;
  cfg.buildPath = d.dir;
  WorkerLimits limits;
  limits.timeoutSeconds = 1;

  auto t0 = Clock::now();
  BuildStats stats;
  auto out = bakeIsolated(script, cfg, {"/tu/hang.cpp", "/tu/ok.cpp"},
                          /*workers=*/2, &stats, limits);
  CHECK(secondsSince(t0) < 30);
  REQUIRE(out.outcomes.count("/tu/hang.cpp"));
  CHECK(out.outcomes.at("/tu/hang.cpp").status == TuStatus::TimedOut);
  CHECK(std::string(tuStatusName(TuStatus::TimedOut)) == "timeout");
  CHECK_FALSE(out.outcomes.count("/tu/ok.cpp")); // clean: shard had none

  // The single-TU entry reindex_tu uses reports the same outcome.
  auto one = bakeTUIsolated(script, cfg, "/tu/hang.cpp", limits);
  CHECK(one.outcomes.at("/tu/hang.cpp").status == TuStatus::TimedOut);
  CHECK(one.graph.nodeCount() == 0);
}

TEST_CASE("anneal isolatedRunner: a hung worker does not hang the analysis",
          "[containment][worker-timeout]") {
  ScratchDir d;
  const std::string script = fakeWorkerScript(d, "");
  WorkerLimits limits;
  limits.timeoutSeconds = 1;
  AnalysisOptions opts;
  opts.workerCount = 1;
  std::atomic<int> timedOut{0};
  opts.isolatedRunner = [&](uint8_t, const std::string &,
                            const std::vector<std::string> &batch,
                            const std::string &, const std::string &log) {
    std::vector<std::string> argv{script};
    argv.insert(argv.end(), batch.begin(), batch.end());
    int rc = runWorkerProcess(argv, log, limits, "anneal");
    if (rc == kWorkerTimedOut)
      ++timedOut;
    return rc;
  };
  clang::tooling::FixedCompilationDatabase db(".", {"-std=c++17"});

  auto t0 = Clock::now();
  auto diags = runAnalysis(db, {d.dir + "/hang.cpp"}, opts);
  CHECK(secondsSince(t0) < 30);
  CHECK(timedOut == 1);
  CHECK(diags.empty());
}

// ============================================================================
// In-process crash while another thread inserts. The siglongjmp guard
// jumped out of addEdge with the shared graph's lock held: the next insert
// on any thread (or the caller's next mutation) deadlocked, and the
// crashed TU's partial facts stayed in the graph.
// ============================================================================

namespace {

void crashOnTarget(const CallGraphEdge &e) {
  if (e.calleeName.find("vycor_crash_target") != std::string::npos)
    std::raise(SIGSEGV);
}

struct HookGuard {
  explicit HookGuard(CallGraph::EdgeInsertHook h) {
    CallGraph::setEdgeInsertHookForTesting(h);
  }
  ~HookGuard() { CallGraph::setEdgeInsertHookForTesting(nullptr); }
};

bool hasEdgeFrom(const CallGraph &g, const std::string &callerSubstr) {
  for (const auto *node : g.allNodes())
    if (node->usr.find(callerSubstr) != std::string::npos &&
        !g.calleesOf(node->usr).empty())
      return true;
  return false;
}

} // namespace

TEST_CASE("bakeIndexes: a crash inside addEdge leaves no lock held and no "
          "partial facts",
          "[containment][crash-guard]") {
  ScratchDir d;
  // Two calls in crash.cpp: the first edge lands in the TU's facts before
  // the second one faults.
  const std::string crashTu = d.write("crash.cpp", R"cpp(
void vycor_crash_helper() {}
void vycor_crash_target() {}
void vycor_crash_caller() {
  vycor_crash_helper();
  vycor_crash_target();
}
)cpp");
  std::string body = "void ok_callee(int) {}\nvoid ok_caller() {\n";
  for (int i = 0; i < 2000; ++i)
    body += "  ok_callee(" + std::to_string(i) + ");\n";
  body += "}\n";
  const std::string okTu = d.write("ok.cpp", body);
  clang::tooling::FixedCompilationDatabase db(".", {"-std=c++17"});

  HookGuard hook(crashOnTarget);
  auto t0 = Clock::now();
  auto baked = bakeIndexes(db, {crashTu, okTu}, {}, /*threadCount=*/2);
  CHECK(secondsSince(t0) < 60);

  REQUIRE(baked.outcomes.count(crashTu));
  CHECK(baked.outcomes.at(crashTu).status == TuStatus::Crashed);
  CHECK(baked.outcomes.at(crashTu).detail == "signal " +
                                                 std::to_string(SIGSEGV));
  CHECK(baked.outcomes.at(okTu).status == TuStatus::Indexed);
  // Nothing of the crashed TU, not even the edge inserted before the fault.
  CHECK_FALSE(hasEdgeFrom(baked.graph, "vycor_crash_caller"));
  CHECK(baked.graph.findNode("vycor_crash_helper") == nullptr);
  CHECK(hasEdgeFrom(baked.graph, "ok_caller"));
  // The shared graph's lock is free: mutating it again returns.
  CHECK(baked.graph.removeTU(okTu) > 0);
}

TEST_CASE("bakeTU: a crashed reindex adds nothing to the live graph",
          "[containment][crash-guard]") {
  ScratchDir d;
  const std::string crashTu = d.write("crash.cpp", R"cpp(
void vycor_crash_helper() {}
void vycor_crash_target() {}
void vycor_crash_caller() {
  vycor_crash_helper();
  vycor_crash_target();
}
)cpp");
  clang::tooling::FixedCompilationDatabase db(".", {"-std=c++17"});
  CallGraph graph;
  ControlFlowIndex cf;
  graph.addNode({"keep", "keep.cpp", 1, false, false, ""}, "keep.cpp");

  HookGuard hook(crashOnTarget);
  auto outcome = bakeTU(graph, cf, db, crashTu);
  CHECK(outcome.status == TuStatus::Crashed);
  CHECK(graph.nodeCount() == 1);
  CHECK(cf.size() == 0);
}

namespace {
volatile bool g_stopRecursion = false;
int recurseForever(int n) {
  volatile char frame[512];
  frame[0] = static_cast<char>(n);
  if (g_stopRecursion)
    return frame[0];
  return recurseForever(n + 1) + frame[0];
}
} // namespace

TEST_CASE("runCrashGuarded recovers a stack overflow on a worker thread",
          "[containment][crash-guard]") {
  CrashGuardScope scope;
  bool ok = true;
  int sig = 0;
  std::thread t([&] {
    ok = runCrashGuarded([] { (void)recurseForever(0); }, &sig);
  });
  t.join();
  CHECK_FALSE(ok);
  CHECK(sig == SIGSEGV);
  // And the thread-pool style reuse: a clean function still runs.
  int ran = 0;
  CHECK(runCrashGuarded([&] { ran = 1; }));
  CHECK(ran == 1);
}

// ============================================================================
// Limits: max_depth=4294967296 narrowed to 0 ("no limit"), and the
// recursive path DFS then walked a long chain until the stack overflowed.
// ============================================================================

namespace {

CallGraph chainGraph(int n) {
  CallGraph g;
  for (int i = 0; i < n; ++i)
    g.addNode({"f" + std::to_string(i), "chain.cpp", static_cast<unsigned>(i),
               i == 0, false, ""});
  for (int i = 0; i + 1 < n; ++i)
    g.addEdge({"f" + std::to_string(i), "f" + std::to_string(i + 1),
               EdgeKind::DirectCall, Confidence::Proven,
               "chain.cpp:" + std::to_string(i) + ":1", 0,
               ExecutionContext::Synchronous});
  return g;
}

} // namespace

TEST_CASE("find_call_chain: an overflowing max_depth is clamped, not "
          "unlimited",
          "[containment][limits]") {
  constexpr int kChain = 100000;
  auto graph = chainGraph(kChain);
  ControlFlowIndex cfIndex;
  ControlFlowOracle oracle(graph, cfIndex);
  std::vector<std::string> eps = {"f0"};
  ToolContext ctx{graph, oracle, cfIndex, eps};
  const ToolEntry *tool = nullptr;
  auto tools = getRegisteredTools();
  for (const auto &t : tools)
    if (t.name == "find_call_chain")
      tool = &t;
  REQUIRE(tool);

  llvm::json::Object args;
  args["to"] = "f" + std::to_string(kChain - 1);
  args["from"] = "f0";
  args["max_depth"] = int64_t(4294967296);
  args["max_fan_in"] = int64_t(0);
  auto result = runTool(*tool, args, ctx);
  auto *obj = result.getAsObject();
  REQUIRE(obj);
  CHECK(obj->getString("status") == "ok");
  auto *paths = obj->getArray("paths");
  REQUIRE(paths);
  CHECK(paths->empty()); // the path is 99999 edges, past the cap
  // Complete within the (clamped) bound, but not exhaustive: longer walks
  // exist (docs/path-analysis.md).
  CHECK(obj->getBoolean("exhaustive") == std::optional<bool>(false));
  auto *stops = obj->getArray("stopReasons");
  REQUIRE(stops);
  CHECK_FALSE(stops->empty());
}

TEST_CASE("findCallerPaths caps maxDepth 0 at kPathSearchDepthCap",
          "[containment][limits]") {
  auto graph = chainGraph(50000);
  SearchLimits limits;
  limits.maxDepth = 0;  // "no limit"
  limits.maxWork = 0;   // no work budget either
  limits.maxFanIn = 0;
  auto r = findCallerPaths(graph, "f49999", {"f0"}, limits);
  CHECK(r.paths.empty());
  CHECK((r.stops & static_cast<unsigned>(StopReason::DepthLimit)) != 0);

  // Within the cap the walk still finds the path.
  auto near = findCallerPaths(graph, "f" + std::to_string(kPathSearchDepthCap),
                              {"f0"}, limits);
  REQUIRE(near.paths.size() == 1);
  CHECK(near.paths[0].hops.size() == kPathSearchDepthCap);
}

TEST_CASE("readLimit clamps before narrowing", "[containment][limits]") {
  llvm::json::Object args;
  args["max_depth"] = int64_t(4294967296);
  args["neg"] = int64_t(-5);
  unsigned depth = 20;
  CHECK_FALSE(readLimitAs(args, "max_depth", 1, kMaxSearchDepth,
                          BelowMin::Reject, depth));
  CHECK(depth == kMaxSearchDepth);

  unsigned untouched = 7;
  CHECK_FALSE(readLimitAs(args, "absent", 1, 10, BelowMin::Reject,
                          untouched));
  CHECK(untouched == 7);

  auto err = readLimitAs(args, "neg", 1, 10, BelowMin::Reject, untouched);
  REQUIRE(err);
  CHECK(*err == "Invalid neg: must be positive");
  err = readLimitAs(args, "neg", 0, 10, BelowMin::Reject, untouched);
  REQUIRE(err);
  CHECK(*err == "Invalid neg: must be non-negative");
  CHECK_FALSE(readLimitAs(args, "neg", 0, 10, BelowMin::Clamp, untouched));
  CHECK(untouched == 0);
}

// ============================================================================
// Read-only graphs refuse mutation in every build type (they used to
// assert, which Release compiles out).
// ============================================================================

TEST_CASE("a read-only graph ignores mutations", "[containment][readonly]") {
  ScratchDir d;
  CallGraph g;
  g.addNode({"a", "a.cpp", 1, true, false, ""}, "a.cpp");
  g.addNode({"b", "a.cpp", 2, false, false, ""}, "a.cpp");
  g.addEdge({"a", "b", EdgeKind::DirectCall, Confidence::Proven, "a.cpp:1:1",
             0, ExecutionContext::Synchronous},
            "a.cpp");
  const std::string path = d.dir + "/ro.vycs";
  REQUIRE(SnapshotIO::save(path, g, ControlFlowIndex(), SnapshotMeta()));
  auto snap = SnapshotIO::load(path, nullptr, LoadMode::ReadOnly);
  REQUIRE(snap);
  CallGraph &ro = snap->graph;
  REQUIRE(ro.edgeCount() == 1);

  ro.addNode({"c", "c.cpp", 1, false, false, ""}, "c.cpp");
  ro.addEdge({"b", "c", EdgeKind::DirectCall, Confidence::Proven,
              "a.cpp:2:1", 0, ExecutionContext::Synchronous},
             "c.cpp");
  ro.addDerivedClass("B", "D", "c.cpp");
  CHECK(ro.removeTU("a.cpp") == 0);
  CHECK(ro.nodeCount() == 2);
  CHECK(ro.edgeCount() == 1);
  CHECK(ro.findNode("c") == nullptr);
  CHECK(ro.getDerivedClasses("B").empty());
}
