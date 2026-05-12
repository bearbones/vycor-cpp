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

// main.cpp — Entry point for the dead code analysis example project.
//
// This file exercises every liveness scenario:
//   - Direct calls (proven alive)
//   - Virtual dispatch through concrete types (proven alive)
//   - Virtual dispatch through factory return (optimistically alive only)
//   - Function pointer indirection (1-2 layers, proven)
//   - Address-taken without call (optimistically alive only)
//   - Template instantiation (alive only for instantiated specializations)
//   - Cross-TU transitive calls (proven alive)
//   - Public API (alive only if config marks it)

#include "api.hpp"
#include "callbacks.hpp"
#include "internal.hpp"
#include "shapes.hpp"
#include <iostream>
#include <vector>

int main() {
    // --- Direct call: mathutil::sum() is alive ---
    std::vector<double> data = {1.0, 2.0, 3.0, 4.0, 5.0};
    double s = mathutil::sum(data);

    // --- Virtual dispatch with proven concrete types ---
    // Circle and Triangle are constructed here, so their overrides
    // are proven alive when passed to print_shape_info().
    Circle c(5.0);
    print_shape_info(c);

    Triangle t(3.0, 4.0);
    print_shape_info(t);

    // --- Direct non-virtual call on concrete type ---
    double circ = c.circumference();

    // --- Virtual dispatch through factory (optimistic only) ---
    // make_shape() *can* construct Square, but we can't prove which
    // derived type it returns. All overrides of the returned Shape*
    // are optimistically alive, but only the one actually constructed
    // (Circle for kind=0) is pessimistically alive.
    auto shape = make_shape(0);
    print_shape_info(*shape);

    // --- Function pointer: 1 layer indirection ---
    // double_it passed directly to apply_once — proven alive.
    double r1 = apply_once(double_it, 42.0);

    // --- Function pointer: 2 layers indirection ---
    // select_transform() returns triple_it, which is then passed to
    // apply_once(). Traceable through return value — proven alive.
    TransformFn fn = select_transform(1);
    double r2 = apply_once(fn, r1);

    // --- Address taken but never called (optimistic only) ---
    // negate_it's address is taken, making it optimistically alive,
    // but the pointer is never invoked — pessimistically dead.
    TransformFn unused_fn = negate_it;
    (void)unused_fn;

    // --- Template instantiation ---
    // clamp<double> is instantiated here — alive.
    // clamp<int> and lerp<T> are never instantiated — dead.
    double clamped = internal::clamp(s, 0.0, 100.0);

    std::cout << "sum=" << s
              << " circ=" << circ
              << " r1=" << r1
              << " r2=" << r2
              << " clamped=" << clamped
              << "\n";

    return 0;
}
