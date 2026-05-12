// Copyright (c) 2026 The vycor-cpp Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0

#include "stage1_ingest.hpp"

#include "stage2_parse.hpp"

int stage1_ingest(Registry &reg, int x) {
  // Plausible: address-take a free function and assign to a member.
  // The store is not a call-argument, so VisitDeclRefExpr emits a
  // Plausible FunctionPointer edge from stage1_ingest -> cbs::defaultHasher.
  reg.handler = &cbs::defaultHasher;

  // Proven: direct call to the next stage.
  return stage2_parse(reg, x + 1);
}
