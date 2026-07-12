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

// test_channel_index.cpp — Slice 1: hand-built ChannelIndex tests (no AST).
// Slice 2 (below): AST integration via ControlFlowContextVisitor, exercised
// through buildCallGraph + buildControlFlowIndex on real temp source, same
// pattern as tests/test_concurrency_index.cpp.

#include "vycor/callgraph/CallGraph.h"
#include "vycor/callgraph/CallGraphBuilder.h"
#include "vycor/callgraph/ChannelIndex.h"
#include "vycor/callgraph/ControlFlowIndex.h"
#include "vycor/callgraph/ControlFlowOracle.h"
#include "vycor/mcp/McpTools.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <clang/Tooling/CompilationDatabase.h>
#include <fstream>
#include <unistd.h>

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"

using namespace vycor;

namespace {
ChannelSite makeSite(std::string channelId, ChannelOperation op,
                     std::string siteFunctionUsr, std::string callSite,
                     std::string tuPath = "") {
  ChannelSite site;
  site.channelId = std::move(channelId);
  site.channelTypeName = "moodycamel::ConcurrentQueue";
  site.category = "queue";
  site.op = op;
  site.siteFunctionUsr = siteFunctionUsr;
  site.siteFunctionDisplay = siteFunctionUsr;
  site.callSite = std::move(callSite);
  site.tuPath = std::move(tuPath);
  return site;
}
} // namespace

TEST_CASE("ChannelIndex stores and queries producer/consumer sites",
          "[ChannelIndex]") {
  ChannelIndex index;

  SECTION("empty index returns no results") {
    CHECK(index.producersOf("c:eventQueue_").empty());
    CHECK(index.consumersOf("c:eventQueue_").empty());
    CHECK(index.size() == 0);
  }

  SECTION("one producer and one consumer on the same channel") {
    index.addSite(makeSite("c:eventQueue_", ChannelOperation::Produce,
                           "fn:Server::onCreate", "server.cpp:10:5"));
    index.addSite(makeSite("c:eventQueue_", ChannelOperation::Consume,
                           "fn:Client::drain", "client.cpp:20:5"));

    auto producers = index.producersOf("c:eventQueue_");
    auto consumers = index.consumersOf("c:eventQueue_");
    REQUIRE(producers.size() == 1);
    REQUIRE(consumers.size() == 1);
    CHECK(producers[0].siteFunctionUsr == "fn:Server::onCreate");
    CHECK(consumers[0].siteFunctionUsr == "fn:Client::drain");
    CHECK(index.size() == 2);
  }

  SECTION("multi-producer/multi-consumer fan-in/fan-out on one channel") {
    index.addSite(makeSite("c:eventQueue_", ChannelOperation::Produce,
                           "fn:A::send", "a.cpp:1:1"));
    index.addSite(makeSite("c:eventQueue_", ChannelOperation::Produce,
                           "fn:B::send", "b.cpp:1:1"));
    index.addSite(makeSite("c:eventQueue_", ChannelOperation::Consume,
                           "fn:X::drain", "x.cpp:1:1"));
    index.addSite(makeSite("c:eventQueue_", ChannelOperation::Consume,
                           "fn:Y::drain", "y.cpp:1:1"));

    CHECK(index.producersOf("c:eventQueue_").size() == 2);
    CHECK(index.consumersOf("c:eventQueue_").size() == 2);
    CHECK(index.allChannelIds().size() == 1);
    CHECK(index.allSites().size() == 4);
  }

  SECTION("distinct channels stay distinct") {
    index.addSite(makeSite("c:fastQueue_", ChannelOperation::Produce,
                           "fn:Replicator::sendFast", "repl.cpp:5:5"));
    index.addSite(makeSite("c:slowQueue_", ChannelOperation::Produce,
                           "fn:Replicator::sendSlow", "repl.cpp:9:5"));

    CHECK(index.producersOf("c:fastQueue_").size() == 1);
    CHECK(index.producersOf("c:slowQueue_").size() == 1);
    auto ids = index.allChannelIds();
    CHECK(ids.size() == 2);
  }

  SECTION("sitesForFunction finds both producer and consumer roles") {
    index.addSite(makeSite("c:eventQueue_", ChannelOperation::Produce,
                           "fn:Hub::relay", "hub.cpp:1:1"));
    index.addSite(makeSite("c:otherQueue_", ChannelOperation::Consume,
                           "fn:Hub::relay", "hub.cpp:2:1"));

    auto sites = index.sitesForFunction("fn:Hub::relay");
    REQUIRE(sites.size() == 2);
  }
}

TEST_CASE("ChannelIndex dedups identical multi-TU registrations",
          "[ChannelIndex]") {
  ChannelIndex index;

  // Same physical call site (header-inlined function) indexed by two TUs:
  // must accumulate a refcount, not create two records.
  index.addSite(makeSite("c:eventQueue_", ChannelOperation::Produce,
                         "fn:Shared::send", "shared.h:10:5", "a.cpp"));
  index.addSite(makeSite("c:eventQueue_", ChannelOperation::Produce,
                         "fn:Shared::send", "shared.h:10:5", "b.cpp"));

  CHECK(index.producersOf("c:eventQueue_").size() == 1);
  CHECK(index.size() == 1);

  // Removing one contributing TU must not remove the site while the other
  // TU still references it.
  CHECK(index.removeTU("a.cpp") == 0);
  CHECK(index.producersOf("c:eventQueue_").size() == 1);

  // Removing the last contributing TU drops it.
  CHECK(index.removeTU("b.cpp") == 1);
  CHECK(index.producersOf("c:eventQueue_").empty());
  CHECK(index.size() == 0);
}

TEST_CASE("ChannelIndex removeTU only removes that TU's own sites",
          "[ChannelIndex]") {
  ChannelIndex index;
  index.addSite(makeSite("c:eventQueue_", ChannelOperation::Produce,
                         "fn:A::send", "a.cpp:1:1", "a.cpp"));
  index.addSite(makeSite("c:eventQueue_", ChannelOperation::Consume,
                         "fn:B::drain", "b.cpp:1:1", "b.cpp"));

  size_t removed = index.removeTU("a.cpp");
  CHECK(removed == 1);
  CHECK(index.producersOf("c:eventQueue_").empty());
  CHECK(index.consumersOf("c:eventQueue_").size() == 1);
  CHECK(index.size() == 1);
}

TEST_CASE("ChannelIndex absorb merges a worker shard", "[ChannelIndex]") {
  ChannelIndex master;
  ChannelIndex shard;

  shard.addSite(makeSite("c:eventQueue_", ChannelOperation::Produce,
                        "fn:A::send", "a.cpp:1:1", "a.cpp"));
  shard.addSite(makeSite("c:eventQueue_", ChannelOperation::Consume,
                        "fn:B::drain", "b.cpp:1:1", "b.cpp"));

  master.absorb(shard);

  CHECK(master.producersOf("c:eventQueue_").size() == 1);
  CHECK(master.consumersOf("c:eventQueue_").size() == 1);

  // The shard's own TU provenance carries over: removeTU against the
  // master for a TU that only ever existed in the shard still works.
  CHECK(master.removeTU("a.cpp") == 1);
  CHECK(master.producersOf("c:eventQueue_").empty());
}

// ============================================================================
// Slice 2 — AST integration: reproduces the motivating replication-bug
// shape (two sends gated by a streaming flag land on different queues; two
// independent drain functions consume them).
// ============================================================================

namespace {

std::string writeTempSource(const std::string &code) {
  llvm::SmallString<128> tmp;
  int fd = -1;
  auto ec = llvm::sys::fs::createTemporaryFile("vycor_channel", "cpp", fd, tmp);
  if (ec)
    return {};
  ::close(fd);
  std::ofstream out(std::string(tmp.str()));
  out << code;
  out.close();
  return std::string(tmp.str());
}

struct ChannelBuilt {
  CallGraph graph;
  ControlFlowIndex cfIndex;
  ChannelIndex channels;
  std::string path;
};

ChannelBuilt buildChannelsFromSource(const std::string &code,
                                     const ChannelTypeConfig &channelCfg) {
  ChannelBuilt out;
  out.path = writeTempSource(code);
  REQUIRE(!out.path.empty());

  clang::tooling::FixedCompilationDatabase compDb(".", {"-std=c++17"});
  std::vector<std::string> files{out.path};

  out.graph = buildCallGraph(compDb, files);
  out.cfIndex = buildControlFlowIndex(compDb, files, out.graph, {},
                                     /*threadCount=*/1, nullptr, "",
                                     LockTypeConfig{}, channelCfg,
                                     &out.channels);
  return out;
}

const char *kReplicationBugSource = R"cpp(
struct Queue {
  void push(int x) {}
  int pop() { return 0; }
};

struct Replicator {
  Queue fastQueue_;
  Queue slowQueue_;
  bool streaming;

  void onCreate(int evt) {
    if (streaming) {
      fastQueue_.push(evt);
    } else {
      slowQueue_.push(evt);
    }
  }
};

void drainFast(Replicator &r) {
  r.fastQueue_.pop();
}

void drainSlow(Replicator &r) {
  r.slowQueue_.pop();
}
)cpp";

} // namespace

TEST_CASE("ChannelIndex proves same-vs-different channel under a guard "
          "(replication-bug shape)",
          "[ChannelIndex][AST]") {
  ChannelTypeConfig cfg;
  ChannelTypeSpec spec;
  spec.qualifiedTypeName = "Queue";
  spec.produceMethods = {"push"};
  spec.consumeMethods = {"pop"};
  spec.category = "queue";
  cfg.registeredTypes.push_back(spec);

  auto built = buildChannelsFromSource(kReplicationBugSource, cfg);

  // Two distinct fields -> two distinct channels, not one.
  auto ids = built.channels.allChannelIds();
  REQUIRE(ids.size() == 2);

  auto onCreateSites = built.channels.sitesForFunction("Replicator::onCreate");
  REQUIRE(onCreateSites.size() == 2);

  const ChannelSite *fastProduce = nullptr;
  const ChannelSite *slowProduce = nullptr;
  for (const auto &s : onCreateSites) {
    CHECK(s.op == ChannelOperation::Produce);
    REQUIRE(s.enclosingGuards.size() == 1);
    if (s.enclosingGuards[0].inTrueBranch)
      fastProduce = &s;
    else
      slowProduce = &s;
  }
  REQUIRE(fastProduce != nullptr);
  REQUIRE(slowProduce != nullptr);

  // The guard gating each send is captured, and it's the same condition
  // text on both branches of the same if/else.
  CHECK(fastProduce->enclosingGuards[0].conditionText.find("streaming") !=
       std::string::npos);
  CHECK(slowProduce->enclosingGuards[0].conditionText.find("streaming") !=
       std::string::npos);

  // The crux fact: they are NOT the same channel.
  CHECK(fastProduce->channelId != slowProduce->channelId);

  // Each has exactly one independent consumer, in a different function,
  // with no relation recorded between the two channels.
  auto fastConsumers = built.channels.consumersOf(fastProduce->channelId);
  auto slowConsumers = built.channels.consumersOf(slowProduce->channelId);
  REQUIRE(fastConsumers.size() == 1);
  REQUIRE(slowConsumers.size() == 1);
  CHECK(fastConsumers[0].siteFunctionDisplay == "drainFast");
  CHECK(slowConsumers[0].siteFunctionDisplay == "drainSlow");
  CHECK(fastConsumers[0].enclosingGuards.empty());
  CHECK(slowConsumers[0].enclosingGuards.empty());

  llvm::sys::fs::remove(built.path);
}

TEST_CASE("explain_ordering MCP tool proves same-vs-different channel "
          "end-to-end (AST -> ChannelIndex -> MCP JSON)",
          "[ChannelIndex][AST][mcp]") {
  ChannelTypeConfig cfg;
  ChannelTypeSpec spec;
  spec.qualifiedTypeName = "Queue";
  spec.produceMethods = {"push"};
  spec.consumeMethods = {"pop"};
  spec.category = "queue";
  cfg.registeredTypes.push_back(spec);

  auto built = buildChannelsFromSource(kReplicationBugSource, cfg);
  auto onCreateSites = built.channels.sitesForFunction("Replicator::onCreate");
  REQUIRE(onCreateSites.size() == 2);
  const std::string &siteA = onCreateSites[0].callSite;
  const std::string &siteB = onCreateSites[1].callSite;

  ControlFlowOracle oracle(built.graph, built.cfIndex);
  std::vector<std::string> eps = {"main"};
  McpToolContext ctx{built.graph, oracle, built.cfIndex, eps, &built.channels};

  auto tools = getRegisteredTools();
  auto findTool = [&](const std::string &name) -> const McpToolEntry & {
    for (auto &t : tools)
      if (t.name == name)
        return t;
    FAIL("tool not registered: " << name);
    throw std::logic_error("unreachable");
  };

  // list_channels sees both distinct channels.
  auto listResult = findTool("list_channels").handler({}, ctx);
  auto *listContent = listResult.getAsObject()
                          ->getArray("content")
                          ->front()
                          .getAsObject();
  auto listPayload = llvm::json::parse(*listContent->getString("text"));
  REQUIRE(bool(listPayload));
  CHECK(listPayload->getAsObject()->getInteger("count") == 2);

  // explain_ordering on the two producer sites proves they're different
  // channels — the core fact the motivating flaky-test investigation
  // couldn't get an LLM to prove on its own.
  llvm::json::Object args;
  args["call_site_a"] = siteA;
  args["call_site_b"] = siteB;
  auto explainResult = findTool("explain_ordering").handler(args, ctx);
  auto *explainContent = explainResult.getAsObject()
                             ->getArray("content")
                             ->front()
                             .getAsObject();
  auto explainPayload = llvm::json::parse(*explainContent->getString("text"));
  REQUIRE(bool(explainPayload));
  CHECK(explainPayload->getAsObject()->getBoolean("sameChannel") == false);

  llvm::sys::fs::remove(built.path);
}

TEST_CASE("ChannelIndex stays empty with no registered types (opt-in, "
          "no-op by default)",
          "[ChannelIndex][AST]") {
  auto built = buildChannelsFromSource(kReplicationBugSource, ChannelTypeConfig{});
  CHECK(built.channels.size() == 0);
  CHECK(built.channels.allChannelIds().empty());
  llvm::sys::fs::remove(built.path);
}

TEST_CASE("ChannelIndex compact drops tombstones without losing live data",
          "[ChannelIndex]") {
  ChannelIndex index;
  index.addSite(makeSite("c:eventQueue_", ChannelOperation::Produce,
                        "fn:A::send", "a.cpp:1:1", "a.cpp"));
  index.addSite(makeSite("c:eventQueue_", ChannelOperation::Consume,
                        "fn:B::drain", "b.cpp:1:1", "b.cpp"));

  index.removeTU("a.cpp");
  index.compact();

  CHECK(index.size() == 1);
  CHECK(index.producersOf("c:eventQueue_").empty());
  auto consumers = index.consumersOf("c:eventQueue_");
  REQUIRE(consumers.size() == 1);
  CHECK(consumers[0].siteFunctionUsr == "fn:B::drain");
}
