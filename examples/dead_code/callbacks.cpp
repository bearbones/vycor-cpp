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

#include "callbacks.hpp"

// --- Transform functions ---

double double_it(double x) {
    return x * 2.0;
}

double triple_it(double x) {
    return x * 3.0;
}

double negate_it(double x) {
    return -x;
}

double square_it(double x) {
    return x * x;
}

// --- Higher-order functions ---

double apply_once(TransformFn fn, double x) {
    return fn(x);
}

double apply_chain(TransformFn first, TransformFn second, double x) {
    return second(first(x));
}

TransformFn select_transform(int choice) {
    switch (choice) {
    case 0:  return double_it;
    case 1:  return triple_it;
    default: return double_it;
    }
}
