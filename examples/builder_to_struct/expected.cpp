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

// Example: expected output after builder-to-struct + lambda-to-member transform.

#include <string>

struct Config {
  std::string name;
  int port = 0;
  bool verbose = false;

  void apply();
};

void Config::apply() {
  // ... use *this ...
}

void example_single_chain() {
  Config c;
  c.name = "server";
  c.port = 8080;
  c.verbose = true;
  c.apply();
}

void example_separate_calls() {
  Config c;
  c.name = "worker";
  c.port = 9090;
  c.verbose = false;
  c.apply();
}
