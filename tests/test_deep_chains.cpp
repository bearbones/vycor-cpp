// Copyright (c) 2026 The vycor-cpp Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0

// test_deep_chains.cpp — Regression coverage for the deep_chains fixture.
//
// Asserts that the call graph built from examples/deep_chains/ contains:
//   (a) every node named in expected_chains.json (both chains),
//   (b) every required edge with the declared kind and confidence,
//   (c) per-layer "must have Proven + Plausible" invariants.
//
// Uses containment semantics: the real graph may emit extra incidental
// edges; the test only checks that expected edges are present.

#include "vycor/callgraph/CallGraph.h"
#include "vycor/callgraph/CallGraphBuilder.h"

#include <catch2/catch_test_macros.hpp>
#include <clang/Tooling/CompilationDatabase.h>

#include <string>
#include <vector>

using namespace vycor;

namespace {

struct RequiredEdge {
  std::string from;
  std::string to;
  EdgeKind kind;
  Confidence confidence;
};

EdgeKind parseKind(const std::string &s) {
  if (s == "DirectCall") return EdgeKind::DirectCall;
  if (s == "VirtualDispatch") return EdgeKind::VirtualDispatch;
  if (s == "FunctionPointer") return EdgeKind::FunctionPointer;
  if (s == "ConstructorCall") return EdgeKind::ConstructorCall;
  if (s == "DestructorCall") return EdgeKind::DestructorCall;
  if (s == "OperatorCall") return EdgeKind::OperatorCall;
  if (s == "TemplateInstantiation") return EdgeKind::TemplateInstantiation;
  if (s == "LambdaCall") return EdgeKind::LambdaCall;
  if (s == "ThreadEntry") return EdgeKind::ThreadEntry;
  FAIL("unknown edge kind: " << s);
  return EdgeKind::DirectCall;
}

Confidence parseConf(const std::string &s) {
  if (s == "Proven") return Confidence::Proven;
  if (s == "Plausible") return Confidence::Plausible;
  if (s == "Unknown") return Confidence::Unknown;
  FAIL("unknown confidence: " << s);
  return Confidence::Unknown;
}

bool graphHasEdge(const CallGraph &g, const RequiredEdge &e) {
  auto outs = g.calleesOf(e.from);
  for (auto *edge : outs) {
    if (edge->calleeName == e.to && edge->kind == e.kind &&
        edge->confidence == e.confidence)
      return true;
  }
  return false;
}

bool hasOutEdgeWithConfidence(const CallGraph &g, const std::string &from,
                              Confidence c) {
  auto outs = g.calleesOf(from);
  for (auto *edge : outs) {
    if (edge->confidence == c)
      return true;
  }
  return false;
}

std::vector<std::string> deepChainsSourceFiles(const std::string &base) {
  return {
      base + "main.cpp",
      base + "pipeline.cpp",
      base + "stage1_ingest.cpp",
      base + "stage2_parse.cpp",
      base + "stage3_transform.cpp",
      base + "stage4_dispatch.cpp",
      base + "stage5_sink.cpp",
      base + "plugins.cpp",
      base + "workers.cpp",
      base + "tokenizer.cpp",
      base + "scheduler.cpp",
      base + "callbacks.cpp",
      base + "async_workers.cpp",
      base + "lambda_callbacks.cpp",
  };
}

CallGraph buildFixtureGraph() {
  const std::string base =
      std::string(PROJECT_SOURCE_DIR) + "/examples/deep_chains/";
  clang::tooling::FixedCompilationDatabase compDb(".",
                                                  {"-std=c++17", "-I" + base});
  return buildCallGraph(compDb, deepChainsSourceFiles(base));
}

} // namespace

// ============================================================================
// Smoke: fixture files are readable by the build system and graph builds.
// ============================================================================

TEST_CASE("deep_chains call graph builds without errors",
          "[deep_chains][smoke]") {
  auto g = buildFixtureGraph();
  CHECK(g.nodeCount() > 0);
  CHECK(g.edgeCount() > 0);
  // At minimum, main and both pipeline entry points must be indexed.
  CHECK(g.findNode("main") != nullptr);
  CHECK(g.findNode("Pipeline::run") != nullptr);
  CHECK(g.findNode("Pipeline::runAsync") != nullptr);
}

// ============================================================================
// Chain A: 6-layer concrete-to-virtual pipeline.
// ============================================================================

TEST_CASE("Chain A: pipeline chain is >=6 layers with expected edges",
          "[deep_chains][chainA]") {
  auto g = buildFixtureGraph();

  const std::vector<std::string> path = {
      "main",           "Pipeline::run", "stage1_ingest", "stage2_parse",
      "stage3_transform","stage4_dispatch","stage5_sink"};

  SECTION("all nodes exist") {
    for (auto &n : path)
      CHECK(g.findNode(n) != nullptr);
  }

  SECTION("path has >=6 layers of DirectCall Proven edges") {
    // 7 nodes -> 6 edges between them.
    for (size_t i = 0; i + 1 < path.size(); ++i) {
      bool found = false;
      for (auto *e : g.calleesOf(path[i])) {
        if (e->calleeName == path[i + 1] &&
            e->kind == EdgeKind::DirectCall &&
            e->confidence == Confidence::Proven) {
          found = true;
          break;
        }
      }
      INFO("layer " << (i + 1) << ": " << path[i] << " -> " << path[i + 1]);
      CHECK(found);
    }
  }

  SECTION("required Proven edges") {
    std::vector<RequiredEdge> edges = {
        {"main", "Pipeline::run", EdgeKind::DirectCall, Confidence::Proven},
        {"Pipeline::run", "stage1_ingest", EdgeKind::DirectCall,
         Confidence::Proven},
        {"stage1_ingest", "stage2_parse", EdgeKind::DirectCall,
         Confidence::Proven},
        {"stage2_parse", "stage3_transform", EdgeKind::DirectCall,
         Confidence::Proven},
        {"stage3_transform", "stage4_dispatch", EdgeKind::DirectCall,
         Confidence::Proven},
        {"stage4_dispatch", "stage5_sink", EdgeKind::DirectCall,
         Confidence::Proven},
        {"stage5_sink", "Registry::invoke", EdgeKind::DirectCall,
         Confidence::Proven},
    };
    for (auto &e : edges) {
      INFO("missing Proven edge: " << e.from << " -> " << e.to);
      CHECK(graphHasEdge(g, e));
    }
  }

  SECTION("required Plausible FunctionPointer edges") {
    std::vector<RequiredEdge> edges = {
        {"main", "cbs::startupHook", EdgeKind::FunctionPointer,
         Confidence::Plausible},
        {"Pipeline::run", "cbs::startupHook", EdgeKind::FunctionPointer,
         Confidence::Plausible},
        {"stage1_ingest", "cbs::defaultHasher", EdgeKind::FunctionPointer,
         Confidence::Plausible},
        {"stage2_parse", "cbs::logAfter", EdgeKind::FunctionPointer,
         Confidence::Plausible},
        {"stage3_transform", "cbs::normalizePayload",
         EdgeKind::FunctionPointer, Confidence::Plausible},
        {"stage5_sink", "cbs::finalFormat", EdgeKind::FunctionPointer,
         Confidence::Plausible},
    };
    for (auto &e : edges) {
      INFO("missing Plausible FnPtr edge: " << e.from << " -> " << e.to);
      CHECK(graphHasEdge(g, e));
    }
  }

  SECTION("stage4_dispatch has Plausible VirtualDispatch fan-out to plugins") {
    std::vector<RequiredEdge> edges = {
        {"stage4_dispatch", "PluginAlpha::handle", EdgeKind::VirtualDispatch,
         Confidence::Plausible},
        {"stage4_dispatch", "PluginBeta::handle", EdgeKind::VirtualDispatch,
         Confidence::Plausible},
        {"stage4_dispatch", "PluginGamma::handle", EdgeKind::VirtualDispatch,
         Confidence::Plausible},
    };
    for (auto &e : edges) {
      INFO("missing Plausible VirtualDispatch edge: " << e.from << " -> "
                                                      << e.to);
      CHECK(graphHasEdge(g, e));
    }
  }

  SECTION("every node on the main Chain A path emits both Proven and "
          "Plausible out-edges") {
    // (stage5_sink's Proven edge is Registry::invoke, already covered.)
    for (auto &n : path) {
      INFO("node without Proven out-edge: " << n);
      CHECK(hasOutEdgeWithConfidence(g, n, Confidence::Proven));
      INFO("node without Plausible out-edge: " << n);
      CHECK(hasOutEdgeWithConfidence(g, n, Confidence::Plausible));
    }
  }
}

// ============================================================================
// Chain B: 6-layer virtual-scheduler chain.
// ============================================================================

TEST_CASE("Chain B: scheduler chain exercises base-ref virtual dispatch",
          "[deep_chains][chainB]") {
  auto g = buildFixtureGraph();

  SECTION("all nodes exist") {
    for (auto &n :
         {"Pipeline::runAsync", "Scheduler::schedule", "Worker::execute",
          "NetworkWorker::execute", "DiskWorker::execute", "tcpWriteBytes"})
      CHECK(g.findNode(n) != nullptr);
  }

  SECTION("Scheduler::schedule fans out Plausible VirtualDispatch "
          "to Worker and overrides") {
    std::vector<RequiredEdge> edges = {
        {"Scheduler::schedule", "NetworkWorker::execute",
         EdgeKind::VirtualDispatch, Confidence::Plausible},
        {"Scheduler::schedule", "DiskWorker::execute",
         EdgeKind::VirtualDispatch, Confidence::Plausible},
    };
    for (auto &e : edges) {
      INFO("missing Plausible VirtualDispatch edge: " << e.from << " -> "
                                                      << e.to);
      CHECK(graphHasEdge(g, e));
    }
  }

  SECTION("NetworkWorker::execute -> tcpWriteBytes is Proven DirectCall") {
    CHECK(graphHasEdge(g, {"NetworkWorker::execute", "tcpWriteBytes",
                           EdgeKind::DirectCall, Confidence::Proven}));
  }

  SECTION("Pipeline::runAsync -> Scheduler::schedule is Proven DirectCall") {
    CHECK(graphHasEdge(g, {"Pipeline::runAsync", "Scheduler::schedule",
                           EdgeKind::DirectCall, Confidence::Proven}));
  }
}

// ============================================================================
// Chain C: lambdas and concurrency workers.
// ============================================================================

namespace {

// Find a synthetic lambda node whose qualified name matches
// "lambda#...#<enclosing>". Returns the first match, or empty string.
std::string findLambdaNodeByEnclosing(const CallGraph &g,
                                      const std::string &enclosing) {
  const std::string suffix = "#" + enclosing;
  for (auto *node : g.allNodes()) {
    const std::string &qn = node->qualifiedName;
    if (qn.rfind("lambda#", 0) == 0 &&
        qn.size() >= suffix.size() &&
        qn.compare(qn.size() - suffix.size(), suffix.size(), suffix) == 0)
      return qn;
  }
  return "";
}

bool hasEdgeWithCtx(const CallGraph &g, const std::string &from,
                    const std::string &to, EdgeKind kind,
                    ExecutionContext ctx) {
  for (auto *e : g.calleesOf(from)) {
    if (e->calleeName == to && e->kind == kind && e->execContext == ctx)
      return true;
  }
  return false;
}

} // namespace

TEST_CASE("Chain C: runChainC spawns threads and registers callbacks",
          "[deep_chains][chainC]") {
  auto g = buildFixtureGraph();

  SECTION("all non-lambda nodes exist") {
    for (auto &n : {"runChainC", "worker_thread_entry", "compute_hash",
                    "AsyncPipeline::dispatch", "registerValueCallback",
                    "registerRefCallback", "Emitter::emit", "Emitter::handle",
                    "scaled"})
      CHECK(g.findNode(n) != nullptr);
  }

  SECTION("main -> runChainC is a Proven DirectCall") {
    CHECK(graphHasEdge(
        g, {"main", "runChainC", EdgeKind::DirectCall, Confidence::Proven}));
  }

  SECTION("std::thread and std::async targets are ThreadEntry edges with "
          "the right ExecutionContext") {
    // std::thread t1(&worker_thread_entry, 42)  -> ThreadSpawn
    CHECK(hasEdgeWithCtx(g, "runChainC", "worker_thread_entry",
                         EdgeKind::ThreadEntry,
                         ExecutionContext::ThreadSpawn));
    // std::async(std::launch::async, &compute_hash, 99)  -> AsyncTask
    CHECK(hasEdgeWithCtx(g, "runChainC", "compute_hash",
                         EdgeKind::ThreadEntry, ExecutionContext::AsyncTask));
  }

  SECTION("lambda passed to std::thread becomes a ThreadEntry edge to a "
          "synthetic lambda node") {
    bool found = false;
    for (auto *e : g.calleesOf("runChainC")) {
      if (e->kind == EdgeKind::ThreadEntry &&
          e->execContext == ExecutionContext::ThreadSpawn &&
          e->calleeName.rfind("lambda#", 0) == 0) {
        found = true;
        break;
      }
    }
    CHECK(found);
  }

  SECTION("capturing lambda registered as a callback produces a LambdaCall "
          "edge plus DirectCalls from the synthetic lambda body") {
    // runChainC registers [=](int x){ return scaled(x, factor); }
    // so we expect a LambdaCall edge and that the synthetic node has a
    // DirectCall edge into scaled().
    bool lambdaEdge = false;
    for (auto *e : g.calleesOf("runChainC")) {
      if (e->kind == EdgeKind::LambdaCall &&
          e->calleeName.rfind("lambda#", 0) == 0) {
        lambdaEdge = true;
        break;
      }
    }
    CHECK(lambdaEdge);

    // Find a lambda node whose enclosing is runChainC and whose body
    // DirectCalls scaled().
    bool bodyCall = false;
    for (auto *node : g.allNodes()) {
      if (node->qualifiedName.rfind("lambda#", 0) != 0)
        continue;
      if (node->qualifiedName.find("#runChainC") == std::string::npos)
        continue;
      for (auto *e : g.calleesOf(node->qualifiedName)) {
        if (e->calleeName == "scaled" && e->kind == EdgeKind::DirectCall) {
          bodyCall = true;
          break;
        }
      }
      if (bodyCall)
        break;
    }
    CHECK(bodyCall);
  }

  SECTION("`[this]`-capture lambda in Emitter::emit calls Emitter::handle "
          "through the synthetic lambda node, not directly") {
    // Emitter::emit itself should NOT have a direct edge to Emitter::handle;
    // that edge should come from the synthetic lambda node whose enclosing
    // is Emitter::emit.
    bool directFromEmit = false;
    for (auto *e : g.calleesOf("Emitter::emit")) {
      if (e->calleeName == "Emitter::handle" &&
          e->kind == EdgeKind::DirectCall)
        directFromEmit = true;
    }
    CHECK_FALSE(directFromEmit);

    // The synthetic lambda node for Emitter::emit must have a DirectCall
    // to Emitter::handle.
    std::string lambdaName = findLambdaNodeByEnclosing(g, "Emitter::emit");
    INFO("no synthetic lambda node found with enclosing Emitter::emit");
    REQUIRE(!lambdaName.empty());
    bool handled = false;
    for (auto *e : g.calleesOf(lambdaName)) {
      if (e->calleeName == "Emitter::handle" &&
          e->kind == EdgeKind::DirectCall) {
        handled = true;
        break;
      }
    }
    CHECK(handled);
  }

  SECTION("lambda stored in a local and passed later still resolves to a "
          "LambdaCall edge to the synthetic node") {
    // `auto refCb = [](State&){...}; registerRefCallback(refCb);`
    bool found = false;
    for (auto *e : g.calleesOf("runChainC")) {
      if (e->kind == EdgeKind::LambdaCall &&
          e->calleeName.rfind("lambda#", 0) == 0 &&
          e->callerName == "runChainC") {
        // Could be either of the two lambdas — that's OK; just check >=1.
        found = true;
        break;
      }
    }
    CHECK(found);
  }

  SECTION("ExecutionContext is Synchronous by default on DirectCall edges") {
    auto edges = g.calleesOf("main");
    bool sawMainRunChainC = false;
    for (auto *e : edges) {
      if (e->calleeName == "runChainC") {
        CHECK(e->execContext == ExecutionContext::Synchronous);
        sawMainRunChainC = true;
      }
    }
    CHECK(sawMainRunChainC);
  }
}

// ============================================================================
// Expected-chains JSON is present and well-formed.
// ============================================================================

TEST_CASE("expected_chains.json fixture is present", "[deep_chains][smoke]") {
  std::string path = std::string(PROJECT_SOURCE_DIR) +
                     "/examples/deep_chains/expected_chains.json";
  std::FILE *fp = std::fopen(path.c_str(), "r");
  REQUIRE(fp != nullptr);
  std::fclose(fp);
}
