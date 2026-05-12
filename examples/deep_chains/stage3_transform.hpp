// Copyright (c) 2026 The vycor-cpp Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0

#pragma once
#include "callbacks.hpp"

// Chain A, layer 4. Emits:
//   Proven DirectCall -> stage4_dispatch
//   Plausible FunctionPointer (&cbs::normalizePayload stored in a local)
int stage3_transform(Registry &reg, int x);
