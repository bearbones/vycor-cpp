// Copyright (c) 2026 The vycor-cpp Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0

#pragma once
#include "callbacks.hpp"

class Worker;

// Chain B infrastructure. Scheduler owns a Worker reference and, when
// schedule() is called, invokes Worker::execute() through that reference —
// Plausible VirtualDispatch fanning out to NetworkWorker and DiskWorker.
//
// Chain B layers:
//   main -> Pipeline::runAsync -> Scheduler::schedule -> Worker::execute
//        -> NetworkWorker::execute -> tcpWriteBytes
// (6 layers, with the Plausible virtual dispatch at layer 3 -> 4.)
class Scheduler {
public:
  explicit Scheduler(const Worker &w);
  int schedule(int x) const;

private:
  const Worker &worker_;
};

// Selector returning a free function pointer. stage B uses the return
// value in a tracked local, which turns the subsequent invocation into
// Proven-via-return FunctionPointer edges.
CallbackFn selectAsyncHandler(int mode);
