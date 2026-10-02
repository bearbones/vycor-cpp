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


#include "vycor/callgraph/RetainUntilExit.h"

#include <mutex>
#include <vector>

namespace vycor {

void retainPointerUntilExit(const void *p) {
  // Both leaked: they must outlive every static destructor that might
  // still run a retaining caller.
  static std::mutex *mutex = new std::mutex;
  static std::vector<const void *> *retained = new std::vector<const void *>;
  std::lock_guard<std::mutex> lock(*mutex);
  retained->push_back(p);
}

} // namespace vycor
