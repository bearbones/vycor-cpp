// Copyright (c) 2026 The vycor-cpp Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0

#pragma once
#include "callbacks.hpp"

// Chain A, layer 6 (leaf). Emits:
//   Proven DirectCall -> Registry::invoke (terminates chain)
//   Plausible FunctionPointer (&cbs::finalFormat stored in local)
int stage5_sink(Registry &reg, int x);
