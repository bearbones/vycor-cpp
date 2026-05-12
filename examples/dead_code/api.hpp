// Copyright (c) 2026 The vycor-cpp Authors
// Original author: Alex Mason
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once
#include <vector>

// --- Public library API ---
//
// Demonstrates the difference between internally-used functions,
// public API functions (marked alive by external config), and
// truly dead functions.
//
//   sum                 — alive, called from main
//   mean                — dead internally, but could be marked public_api
//   normalize           — alive, called transitively by sum()
//   obscure_transform   — dead, never called, not public API

namespace mathutil {

// Called directly from main — alive.
double sum(const std::vector<double>& v);

// Never called from this project. An external config could mark it
// as public API (making it alive), but by default it's dead.
double mean(const std::vector<double>& v);

// Called by sum() internally — alive transitively.
double normalize(double val, double min, double max);

// Never called, not part of public API — dead.
double obscure_transform(double x, int n);

} // namespace mathutil
