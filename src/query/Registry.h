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

#include "vycor/query/Tools.h"

#include <vector>

namespace vycor {

// Each tool family appends its ToolEntry records (schema + handler) in
// the order they should appear in tools/list. Registry.cpp composes them.
void registerGraphTools(std::vector<ToolEntry> &tools);
void registerExceptionTools(std::vector<ToolEntry> &tools);
void registerLockTools(std::vector<ToolEntry> &tools);
void registerDeadCodeTools(std::vector<ToolEntry> &tools);
void registerChannelTools(std::vector<ToolEntry> &tools);
void registerImpactTools(std::vector<ToolEntry> &tools);

} // namespace vycor
