// Copyright (c) 2026 The vycor-cpp Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0

#include "async_workers.hpp"

int worker_thread_entry(int x) { return x ^ 0x55; }

int compute_hash(int x) {
  int h = x;
  h = (h * 2654435761) & 0x7fffffff;
  return h;
}

int AsyncPipeline::dispatch() const {
  int acc = 0;
  for (int i = 0; i < 4; ++i)
    acc += worker_thread_entry(i);
  return acc;
}

int scaled(int x, int factor) { return x * factor + 1; }
