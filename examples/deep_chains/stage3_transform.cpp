// Copyright (c) 2026 The vycor-cpp Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0

#include "stage3_transform.hpp"

#include "stage4_dispatch.hpp"

int stage3_transform(Registry &reg, int x) {
  // Plausible: address-take stored into a local variable.
  CallbackFn norm = &cbs::normalizePayload;
  (void)norm;

  // Proven: direct call to the next stage.
  return stage4_dispatch(reg, x * 2);
}
