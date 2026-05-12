// Copyright (c) 2026 The vycor-cpp Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0

#include "callbacks.hpp"

namespace cbs {
int auditBefore(int x) { return x + 1; }
int logAfter(int x) { return x + 2; }
int defaultHasher(int x) { return x ^ 0x5a5a5a5a; }
int alternateHasher(int x) { return x ^ 0xa5a5a5a5; }
int normalizePayload(int x) { return x % 1024; }
int finalFormat(int x) { return x & 0xFFFF; }
int startupHook(int x) { return x + 7; }
int asyncCompleted(int x) { return x - 1; }
} // namespace cbs

int Registry::invoke(int x) const {
  int out = x;
  if (auditor != nullptr)
    out = auditor(out);
  if (handler != nullptr)
    out = handler(out);
  return out;
}
