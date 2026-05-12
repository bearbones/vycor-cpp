// Copyright (c) 2026 The vycor-cpp Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0

// main.cpp — entry point for the deep_chains call-graph fixture.
//
// Three chains exercise distinct call-graph features:
//
//   Chain A: main -> Pipeline::run -> stage1_ingest -> stage2_parse
//                 -> stage3_transform -> stage4_dispatch -> stage5_sink
//
//   Chain B: main -> Pipeline::runAsync -> Scheduler::schedule
//                 -> Worker::execute -> NetworkWorker::execute
//                 -> tcpWriteBytes
//
//   Chain C: main -> runChainC -> {worker_thread_entry, compute_hash,
//                 lambda#...#runChainC, Emitter::emit}
//              where runChainC spawns std::thread and std::async and
//              registers lambda callbacks. Every spawner emits a
//              ThreadEntry edge; lambda bodies get their own synthetic
//              CallGraphNode.
//
// See expected_chains.json and tests/test_deep_chains.cpp for details.

#include "async_workers.hpp"
#include "callbacks.hpp"
#include "lambda_callbacks.hpp"
#include "pipeline.hpp"

#include <functional>
#include <future>
#include <thread>

int runChainC() {
  // ThreadEntry/ThreadSpawn: free function as std::thread target.
  std::thread t1(&worker_thread_entry, 42);

  // ThreadEntry/AsyncTask: free function as std::async target.
  auto fut = std::async(std::launch::async, &compute_hash, 99);

  // ThreadEntry/ThreadSpawn: lambda as std::thread target. Lambda body
  // constructs AsyncPipeline and calls dispatch(), so the synthetic
  // lambda node gains DirectCall edges to AsyncPipeline::AsyncPipeline
  // and AsyncPipeline::dispatch.
  std::thread t2([] {
    AsyncPipeline p;
    (void)p.dispatch();
  });

  // LambdaCall: lambda registered as a value callback. `[=]` captures
  // `factor` so the lambda body performs a DirectCall edge to scaled().
  int factor = 3;
  int cbSum = registerValueCallback(
      [=](int x) { return scaled(x, factor); });

  // LambdaCall: lambda stored in a local first, then passed. The edge
  // still targets the synthetic lambda node thanks to varLambdaSources_.
  auto refCb = [](State &s) { s.acc += 1; };
  registerRefCallback(refCb);

  // DirectCall: Emitter::emit, which itself registers a `[this]`-capture
  // lambda that calls Emitter::handle.
  Emitter e;
  e.emit();

  t1.join();
  t2.join();
  return cbSum + fut.get();
}

int main() {
  // Plausible: address-take a free function and stash it in a local,
  // so main itself emits a Plausible out-edge alongside its Proven
  // DirectCall edges to Pipeline methods.
  CallbackFn boot = &cbs::startupHook;
  (void)boot;

  Pipeline p;

  // Proven: direct call into Chain A.
  int a = p.run(7);

  // Proven: direct call into Chain B.
  int b = p.runAsync(1);

  // Proven: direct call into Chain C.
  int c = runChainC();

  return (a + b + c) & 1;
}
