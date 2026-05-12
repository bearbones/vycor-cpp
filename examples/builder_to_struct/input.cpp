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

// Example: builder pattern class with nonmember lambda-based configuration.
//
// The transforms demonstrated here convert:
// 1. Builder-pattern class -> struct with field assignments
// 2. Nonmember function taking a lambda -> member function call

#include <string>

class Config {
public:
  Config& setName(const std::string& n) { name_ = n; return *this; }
  Config& setPort(int p) { port_ = p; return *this; }
  Config& setVerbose(bool v) { verbose_ = v; return *this; }

  const std::string& getName() const { return name_; }
  int getPort() const { return port_; }
  bool getVerbose() const { return verbose_; }

private:
  std::string name_;
  int port_ = 0;
  bool verbose_ = false;
};

// Nonmember function that takes a lambda to configure via builder pattern.
template <typename F>
void applyConfig(F&& f) {
  Config c;
  f(c);
  // ... use c ...
}

void example_single_chain() {
  applyConfig([](Config& c) {
    c.setName("server").setPort(8080).setVerbose(true);
  });
}

void example_separate_calls() {
  applyConfig([](Config& c) {
    c.setName("worker");
    c.setPort(9090);
    c.setVerbose(false);
  });
}
