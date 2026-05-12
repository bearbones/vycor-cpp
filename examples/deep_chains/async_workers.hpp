// Copyright (c) 2026 The vycor-cpp Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0

#pragma once

// Chain C: concurrency workers exercised by runChainC().
//
// worker_thread_entry and compute_hash are free functions handed to
// std::thread and std::async respectively. CallGraphBuilder must emit
// ThreadEntry/Proven edges with the appropriate ExecutionContext.
int worker_thread_entry(int x);
int compute_hash(int x);

// AsyncPipeline::dispatch is invoked from inside a lambda that is itself
// passed to std::thread; this proves lambda-body calls attribute to the
// synthetic lambda node, not to the enclosing runChainC.
struct AsyncPipeline {
  int dispatch() const;
};

// Helpers used from lambda bodies to demonstrate capture-through-lambda
// edges (`[=]`-captured integer is passed to a free function inside the
// lambda body, producing a DirectCall edge from the synthetic lambda node).
int scaled(int x, int factor);
