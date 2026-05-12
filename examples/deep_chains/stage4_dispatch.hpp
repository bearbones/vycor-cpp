// Copyright (c) 2026 The vycor-cpp Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0

#pragma once
#include "callbacks.hpp"

// Chain A, layer 5. Emits:
//   Proven DirectCall -> stage5_sink
//   Plausible VirtualDispatch through vector<unique_ptr<Plugin>> ->
//             PluginAlpha::handle, PluginBeta::handle, PluginGamma::handle
int stage4_dispatch(Registry &reg, int x);
