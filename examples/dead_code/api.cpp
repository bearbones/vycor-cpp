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

#include "api.hpp"
#include "internal.hpp"

namespace mathutil {

double sum(const std::vector<double>& v) {
    if (v.empty()) return 0.0;
    double raw = internal::accumulate_helper(v.data(),
                                             static_cast<int>(v.size()));
    return normalize(raw, 0.0, 1e12);
}

double mean(const std::vector<double>& v) {
    if (v.empty()) return 0.0;
    return sum(v) / static_cast<double>(v.size());
}

double normalize(double val, double min, double max) {
    if (max <= min) return val;
    if (val < min) return min;
    if (val > max) return max;
    return val;
}

double obscure_transform(double x, int n) {
    double result = x;
    for (int i = 0; i < n; ++i) {
        result = result * 0.99 + 0.01;
    }
    return result;
}

} // namespace mathutil
