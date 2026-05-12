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

#include "internal.hpp"
#include <iostream>

namespace internal {

double accumulate_helper(const double* data, int n) {
    double total = 0.0;
    for (int i = 0; i < n; ++i) {
        total += data[i];
    }
    return total;
}

double old_normalize(double val, double range) {
    if (range == 0.0) return 0.0;
    return val / range;
}

void log_value(const std::string& label, double val) {
    std::cout << "[DEBUG] " << label << " = " << val << "\n";
}

} // namespace internal
