// Copyright (c) 2026 The vycor-cpp Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0

#pragma once

// Chain A and Chain B entry points.
class Pipeline {
public:
  // Chain A — concrete-to-virtual pipeline, 6 layers:
  //   main -> Pipeline::run -> stage1_ingest -> stage2_parse
  //        -> stage3_transform -> stage4_dispatch -> stage5_sink
  int run(int seed);

  // Chain B — virtual-scheduler chain, 6 layers:
  //   main -> Pipeline::runAsync -> Scheduler::schedule -> Worker::execute
  //        -> NetworkWorker::execute (or DiskWorker::execute) -> tcpWriteBytes
  int runAsync(int mode);
};
