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

// --- Function pointer / callback indirection scenarios ---
//
// Liveness of a function passed as a pointer depends on whether
// we can trace the pointer to an actual call site.
//
// Layers of indirection:
//   Layer 1: &fn passed directly to a calling function      (proven)
//   Layer 2: &fn returned from a selector, then called       (proven, traceable)
//   Layer 3: &fn stored in variable, never called through    (optimistic only)

using TransformFn = double(*)(double);

// --- Transform functions ---

// Passed to apply_once() directly — 1 layer, proven alive.
double double_it(double x);

// Returned by select_transform(), then passed to apply_once() — 2 layers,
// but traceable through return value. Proven alive.
double triple_it(double x);

// Address taken (&negate_it) and stored in a local variable, but the
// variable is never called through. Optimistically alive (address-taken
// implies plausible use), pessimistically dead (no proven call).
double negate_it(double x);

// Address never taken, never called — dead in both modes.
double square_it(double x);

// --- Higher-order functions ---

// Called from main with a function pointer argument — alive.
double apply_once(TransformFn fn, double x);

// Never called — dead in both modes.
double apply_chain(TransformFn first, TransformFn second, double x);

// Called from main, returns a function pointer — alive.
// The returned pointer (triple_it) is then passed to apply_once.
TransformFn select_transform(int choice);
