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

// Order A: this TU exists to teach Phase 1 of the anneal pipeline about the
// float overload in Extension.hpp. Calling scale with a float literal picks
// scale(Vector, float) unambiguously, so the TU compiles cleanly.
//
// Note: if we tried to include both Extension.hpp and Logic.hpp here, the
// `long amount` call in Logic.hpp would become an *actual* ambiguous call
// and the TU would fail to compile — which is the point of the whole
// fragility case. anneal flags that same tie from order_b.cpp's perspective
// using the cross-TU index.
#include "Extension.hpp"

int main() {
    MathLib::Vector v;
    MathLib::scale(v, 1.0f);
}
