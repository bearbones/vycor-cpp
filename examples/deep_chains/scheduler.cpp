// Copyright (c) 2026 The vycor-cpp Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0

#include "scheduler.hpp"

#include "workers.hpp"

Scheduler::Scheduler(const Worker &w) : worker_(w) {}

int Scheduler::schedule(int x) const {
  // Plausible: address-take a free function for the mix invariant.
  CallbackFn hk = &cbs::startupHook;
  (void)hk;

  // Plausible VirtualDispatch: worker_ is Worker& — fans out to
  // NetworkWorker::execute and DiskWorker::execute.
  return worker_.execute(x + 5);
}

CallbackFn selectAsyncHandler(int mode) {
  // Returned function pointers are tracked via
  // CallGraph::addFunctionReturn, so a caller that stores the return in a
  // local and later invokes it gets Proven FunctionPointer edges with
  // indirectionDepth=2 (see CallGraphBuilder.cpp ~L315-326).
  switch (mode) {
  case 0:
    return &cbs::normalizePayload;
  case 1:
    return &cbs::alternateHasher;
  default:
    return &cbs::defaultHasher;
  }
}
