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

// Example: expected output after macro boolean-split transform.

#define ASSERT(cond) do { if (!(cond)) __builtin_abort(); } while (0)
#define ASSERT_ALL(...) /* placeholder: checks each argument individually */

void test_two_operands(int a, int b) {
  // Two operands: split into individual ASSERTs.
  ASSERT(a > 0);
  ASSERT(b > 0);
}

void test_three_operands(int a, int b, int c) {
  // Three operands: convert to ASSERT_ALL.
  ASSERT_ALL(a > 0, b > 0, c > 0);
}

void test_four_operands(int a, int b, int c, int d) {
  // Four operands: convert to ASSERT_ALL.
  ASSERT_ALL(a > 0, b > 0, c > 0, d > 0);
}

void test_no_change(int a, int b) {
  // OR operator: leave alone.
  ASSERT(a > 0 || b > 0);

  // Single condition: leave alone.
  ASSERT(a > 0);
}
