// Copyright (c) 2026 The vycor-cpp Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0

#pragma once

// Worker hierarchy used by Chain B. A Scheduler receives a Worker& and
// invokes execute() through that reference — CallGraphBuilder emits a
// Plausible VirtualDispatch edge fanning out to every override.
class Worker {
public:
  virtual ~Worker() = default;
  virtual int execute(int x) const = 0;     // pure virtual
  virtual const char *tag() const;           // default impl
};

class NetworkWorker : public Worker {
public:
  int execute(int x) const override;
  const char *tag() const override;
};

class DiskWorker : public Worker {
public:
  int execute(int x) const override;
  const char *tag() const override;
};

// Free helpers called from worker implementations.
int tcpWriteBytes(int x);
int diskFsyncRange(int x);
