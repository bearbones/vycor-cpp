// Copyright (c) 2026 The vycor-cpp Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0

#include "pipeline.hpp"

#include "callbacks.hpp"
#include "scheduler.hpp"
#include "stage1_ingest.hpp"
#include "workers.hpp"

int Pipeline::run(int seed) {
  Registry reg;
  // Plausible: address-take for the layer-1 mix invariant.
  reg.handler = &cbs::startupHook;

  // Proven: direct call into Chain A.
  return stage1_ingest(reg, seed);
}

int Pipeline::runAsync(int mode) {
  // Plausible FunctionPointer via address-take into a local.
  CallbackFn hk = &cbs::asyncCompleted;
  (void)hk;

  // The worker is a NetworkWorker constructed as a concrete local —
  // CallGraphBuilder emits Proven VirtualDispatch edges to its virtual
  // methods (addConcreteTypeEdges). The subsequent Scheduler uses the
  // object through Worker&, which reintroduces Plausible edges at layer 4.
  NetworkWorker w;
  Scheduler sched(w);

  // Proven: direct call into Chain B.
  return sched.schedule(mode);
}
