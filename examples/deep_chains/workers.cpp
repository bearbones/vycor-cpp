// Copyright (c) 2026 The vycor-cpp Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0

#include "workers.hpp"

#include "callbacks.hpp"

const char *Worker::tag() const { return "worker"; }

int NetworkWorker::execute(int x) const {
  // Proven DirectCall to tcpWriteBytes.
  int out = tcpWriteBytes(x);
  // Plausible FunctionPointer: &asyncCompleted stored in a local, never invoked.
  CallbackFn after = &cbs::asyncCompleted;
  (void)after;
  return out;
}
const char *NetworkWorker::tag() const { return "network"; }

int DiskWorker::execute(int x) const {
  int out = diskFsyncRange(x);
  CallbackFn after = &cbs::logAfter;
  (void)after;
  return out;
}
const char *DiskWorker::tag() const { return "disk"; }

int tcpWriteBytes(int x) {
  // Leaf — Proven direct call only, Plausible address-take to keep the
  // "mix at every layer" invariant.
  CallbackFn hk = &cbs::finalFormat;
  (void)hk;
  return x + 13;
}

int diskFsyncRange(int x) {
  CallbackFn hk = &cbs::finalFormat;
  (void)hk;
  return x + 17;
}
