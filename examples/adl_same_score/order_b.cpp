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

// Order B: Only Core.hpp is visible here, so the call silently resolves to
// scale(Vector, int). The same-score tie against scale(Vector, float) in
// Extension.hpp is only visible to anneal's cross-TU index.
//
// The call is intentionally written here in the main file (rather than in
// an inline helper inside Logic.hpp) so that anneal's VisitCallExpr
// analyses it — the analyzer skips call sites that aren't in the main
// file.
#include "Core.hpp"

int main() {
    MathLib::Vector v;
    long amount = 3;
    scale(v, amount);
}
