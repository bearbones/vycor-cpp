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

// test_concurrency_index.cpp — Tests for RAII scope capture + catch handler
// body summaries produced by ControlFlowContextVisitor (Phase 3).

#include "vycor/callgraph/CallGraph.h"
#include "vycor/callgraph/CallGraphBuilder.h"
#include "vycor/callgraph/ControlFlowIndex.h"

#include <catch2/catch_test_macros.hpp>
#include <clang/Tooling/CompilationDatabase.h>

#include "llvm/Support/FileSystem.h"
#include "llvm/ADT/SmallString.h"

#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

using namespace vycor;

// ============================================================================
// Helpers
// ============================================================================

namespace {

// Write `code` to a unique temp .cpp, return the absolute path. Caller owns
// cleanup. On failure, the returned path is empty.
std::string writeTempSource(const std::string &code) {
  llvm::SmallString<128> tmp;
  int fd = -1;
  auto ec =
      llvm::sys::fs::createTemporaryFile("vycor_cf", "cpp", fd, tmp);
  if (ec)
    return {};
  // close() the fd immediately — we reopen as an ofstream to write.
  ::close(fd);
  std::ofstream out(std::string(tmp.str()));
  out << code;
  out.close();
  return std::string(tmp.str());
}

// Build a (CallGraph, ControlFlowIndex) pair from a single source string.
// Serial, no PCH, with the given lock config.
struct Built {
  CallGraph graph;
  ControlFlowIndex cfIndex;
  std::string path; // the temp source path (for cleanup / assertions)
};

Built buildFromSource(const std::string &code, const LockTypeConfig &lockCfg) {
  Built out;
  out.path = writeTempSource(code);
  REQUIRE(!out.path.empty());

  clang::tooling::FixedCompilationDatabase compDb(".", {"-std=c++17"});
  std::vector<std::string> files{out.path};

  out.graph = buildCallGraph(compDb, files);
  out.cfIndex = buildControlFlowIndex(compDb, files, out.graph, {},
                                       /*threadCount=*/1, nullptr, "",
                                       lockCfg);
  return out;
}

void cleanup(const Built &b) {
  if (!b.path.empty())
    llvm::sys::fs::remove(b.path);
}

// Find the first CallSiteContext targeting `calleeName`. Returns nullptr if
// none indexed.
const CallSiteContext *findContext(const ControlFlowIndex &cf,
                                   const std::string &calleeName) {
  auto ctxs = cf.contextsForCallee(calleeName);
  return ctxs.empty() ? nullptr : ctxs.front();
}

} // namespace

// ============================================================================
// RAII capture
// ============================================================================

TEST_CASE("Built-in std::lock_guard is captured as Lock kind",
          "[prism][raii]") {
  std::string code = R"(
    namespace std {
      class mutex {};
      template <class M> class lock_guard {
      public:
        lock_guard(M&) {}
        ~lock_guard() {}
      };
    }
    static std::mutex g_m;
    void worker();
    void run() {
      std::lock_guard<std::mutex> g(g_m);
      worker();
    }
    void worker() {}
  )";

  LockTypeConfig cfg; // useBuiltins=true by default
  auto b = buildFromSource(code, cfg);

  const auto *ctx = findContext(b.cfIndex, "worker");
  REQUIRE(ctx != nullptr);
  REQUIRE(ctx->liveRaiiLocals.size() >= 1);

  bool foundLock = false;
  for (const auto &local : ctx->liveRaiiLocals) {
    if (local.kind == RaiiKind::Lock &&
        local.typeName.find("lock_guard") != std::string::npos &&
        local.varName == "g") {
      foundLock = true;
    }
  }
  CHECK(foundLock);

  cleanup(b);
}

TEST_CASE("User allowlist promotes a custom type to Lock kind",
          "[prism][raii]") {
  std::string code = R"(
    class Arbiter {
    public:
      Arbiter() {}
      ~Arbiter() {}
    };
    void worker();
    void run() {
      Arbiter a;
      worker();
    }
    void worker() {}
  )";

  LockTypeConfig cfg;
  cfg.userAllowlist.push_back("Arbiter");
  auto b = buildFromSource(code, cfg);

  const auto *ctx = findContext(b.cfIndex, "worker");
  REQUIRE(ctx != nullptr);

  bool foundArbiterLock = false;
  for (const auto &local : ctx->liveRaiiLocals) {
    if (local.typeName.find("Arbiter") != std::string::npos &&
        local.kind == RaiiKind::Lock)
      foundArbiterLock = true;
  }
  CHECK(foundArbiterLock);

  cleanup(b);
}

TEST_CASE("CapabilityAttr marks a type as Lock automatically",
          "[prism][raii]") {
  std::string code = R"(
    class __attribute__((capability("mutex"))) MyLock {
    public:
      MyLock() {}
      ~MyLock() {}
    };
    void worker();
    void run() {
      MyLock m;
      worker();
    }
    void worker() {}
  )";

  LockTypeConfig cfg; // no user allowlist
  auto b = buildFromSource(code, cfg);

  const auto *ctx = findContext(b.cfIndex, "worker");
  REQUIRE(ctx != nullptr);

  bool foundAttrLock = false;
  for (const auto &local : ctx->liveRaiiLocals) {
    if (local.typeName.find("MyLock") != std::string::npos &&
        local.kind == RaiiKind::Lock)
      foundAttrLock = true;
  }
  CHECK(foundAttrLock);

  cleanup(b);
}

TEST_CASE("std::unique_ptr classifies as SmartPtr kind",
          "[prism][raii]") {
  std::string code = R"(
    namespace std {
      template <class T, class D = void> class unique_ptr {
      public:
        unique_ptr() {}
        ~unique_ptr() {}
      };
    }
    void worker();
    void run() {
      std::unique_ptr<int> p;
      worker();
    }
    void worker() {}
  )";

  LockTypeConfig cfg;
  auto b = buildFromSource(code, cfg);

  const auto *ctx = findContext(b.cfIndex, "worker");
  REQUIRE(ctx != nullptr);

  bool foundSmartPtr = false;
  for (const auto &local : ctx->liveRaiiLocals) {
    if (local.kind == RaiiKind::SmartPtr &&
        local.typeName.find("unique_ptr") != std::string::npos)
      foundSmartPtr = true;
  }
  CHECK(foundSmartPtr);

  cleanup(b);
}

TEST_CASE("RAII scope tracking respects nested CompoundStmt blocks",
          "[prism][raii]") {
  // The call to inner() sees both outer and inner locks live.
  // The call to outerOnly() sees only the outer lock live.
  std::string code = R"(
    namespace std {
      class mutex {};
      template <class M> class lock_guard {
      public:
        lock_guard(M&) {}
        ~lock_guard() {}
      };
    }
    static std::mutex outer_m;
    static std::mutex inner_m;
    void inner();
    void outerOnly();
    void run() {
      std::lock_guard<std::mutex> outer(outer_m);
      outerOnly();
      {
        std::lock_guard<std::mutex> inner_g(inner_m);
        inner();
      }
    }
    void inner() {}
    void outerOnly() {}
  )";

  LockTypeConfig cfg;
  auto b = buildFromSource(code, cfg);

  const auto *innerCtx = findContext(b.cfIndex, "inner");
  REQUIRE(innerCtx != nullptr);
  unsigned innerLocks = 0;
  for (const auto &l : innerCtx->liveRaiiLocals)
    if (l.kind == RaiiKind::Lock)
      ++innerLocks;
  CHECK(innerLocks == 2);

  const auto *outerCtx = findContext(b.cfIndex, "outerOnly");
  REQUIRE(outerCtx != nullptr);
  unsigned outerLocks = 0;
  for (const auto &l : outerCtx->liveRaiiLocals)
    if (l.kind == RaiiKind::Lock)
      ++outerLocks;
  CHECK(outerLocks == 1);

  cleanup(b);
}

// ============================================================================
// Catch handler body summary
// ============================================================================

TEST_CASE("Catch handler body summary is captured",
          "[prism][catch-body]") {
  std::string code = R"(
    namespace std {
      class exception { public: virtual ~exception() {} };
    }
    void log(const std::exception&);
    void dangerous();
    void run() {
      try {
        dangerous();
      } catch (const std::exception& e) {
        log(e);
        return;
      }
    }
    void dangerous() {}
    void log(const std::exception&) {}
  )";

  LockTypeConfig cfg;
  auto b = buildFromSource(code, cfg);

  const auto *ctx = findContext(b.cfIndex, "dangerous");
  REQUIRE(ctx != nullptr);
  REQUIRE(ctx->enclosingTryCatches.size() == 1);
  REQUIRE(ctx->enclosingTryCatches[0].handlers.size() == 1);

  const auto &handler = ctx->enclosingTryCatches[0].handlers[0];
  CHECK(handler.caughtType.find("exception") != std::string::npos);
  CHECK_FALSE(handler.bodySummary.empty());
  // The body should begin with '{' and include the log(e) call.
  CHECK(handler.bodySummary.find("log(e)") != std::string::npos);

  cleanup(b);
}

TEST_CASE("Catch(...) body is captured and isCatchAll is set",
          "[prism][catch-body]") {
  std::string code = R"(
    void dangerous();
    void run() {
      try {
        dangerous();
      } catch (...) {
        // swallow
      }
    }
    void dangerous() {}
  )";

  LockTypeConfig cfg;
  auto b = buildFromSource(code, cfg);

  const auto *ctx = findContext(b.cfIndex, "dangerous");
  REQUIRE(ctx != nullptr);
  REQUIRE(ctx->enclosingTryCatches.size() == 1);
  REQUIRE(ctx->enclosingTryCatches[0].handlers.size() == 1);

  const auto &handler = ctx->enclosingTryCatches[0].handlers[0];
  CHECK(handler.isCatchAll);
  CHECK_FALSE(handler.bodySummary.empty());

  cleanup(b);
}
