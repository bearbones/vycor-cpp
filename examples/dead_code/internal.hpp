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
#include <string>

// --- Internal helpers and templates ---
//
// Demonstrates dead code among internal utilities:
//   accumulate_helper — alive, called by mathutil::sum()
//   old_normalize     — dead, leftover from a refactor
//   log_value         — dead, unused debugging utility
//   clamp<T>          — alive only for instantiated specializations
//   lerp<T>           — dead, template never instantiated

namespace internal {

// Called by mathutil::sum() — alive.
double accumulate_helper(const double* data, int n);

// Was used before a refactor, now dead.
double old_normalize(double val, double range);

// Debugging utility, never called — dead.
void log_value(const std::string& label, double val);

// Template: alive only for types that are actually instantiated.
// clamp<double> is instantiated in main. clamp<int> is not.
template<typename T>
T clamp(T val, T lo, T hi) {
    if (val < lo) return lo;
    if (val > hi) return hi;
    return val;
}

// Template never instantiated — dead.
template<typename T>
T lerp(T a, T b, double t) {
    return static_cast<T>(a + (b - a) * t);
}

} // namespace internal
