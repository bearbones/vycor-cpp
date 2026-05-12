// Copyright (c) 2026 The vycor-cpp Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0

#pragma once

// Free functions that each stage stores via &address into a member.
// Address-taking a free function outside a call-argument context yields a
// Plausible FunctionPointer edge (CallGraphBuilder.cpp VisitDeclRefExpr).
namespace cbs {
int auditBefore(int x);
int logAfter(int x);
int defaultHasher(int x);
int alternateHasher(int x);
int normalizePayload(int x);
int finalFormat(int x);
int startupHook(int x);
int asyncCompleted(int x);
} // namespace cbs

using CallbackFn = int (*)(int);

// Callback registry — stages stash function addresses into `handler` and
// `auditor` fields. Those stores are Plausible edges. The registry's invoke()
// method does not add call-graph edges for the fn pointer call itself
// (indirect call with null getDirectCallee()), which mirrors real-world
// uncertainty about where the pointer ends up being invoked.
struct Registry {
  CallbackFn handler = nullptr;
  CallbackFn auditor = nullptr;
  int invoke(int x) const;
};
