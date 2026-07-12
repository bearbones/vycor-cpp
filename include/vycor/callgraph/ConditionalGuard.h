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

#include <string>

namespace vycor {

// Split out of ControlFlowIndex.h so ChannelIndex.h can use it without
// depending on (and cyclically including) the rest of ControlFlowIndex —
// both indexes attach guard context to their call/channel sites.
struct ConditionalGuard {
  std::string conditionText; // Source text of the condition
  std::string location;      // file:line:col of the if/assert
  bool inTrueBranch = true;  // true = if-branch, false = else-branch
  bool isAssertion = false;  // assert(), DCHECK(), etc.
};

} // namespace vycor
