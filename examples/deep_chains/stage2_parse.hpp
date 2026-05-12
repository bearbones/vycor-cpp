// Copyright (c) 2026 The vycor-cpp Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0

#pragma once
#include "callbacks.hpp"

class Tokenizer;

// Chain A, layer 3. Emits:
//   Proven DirectCall -> stage3_transform
//   Plausible VirtualDispatch through Tokenizer& -> JsonTokenizer::tokenize,
//                                                   TextTokenizer::tokenize
int stage2_parse(Registry &reg, int x);

// Helper with Tokenizer& parameter — the t.tokenize() call inside here
// is Plausible, but this function is on the Chain A path so the callgraph
// test assertion is stated at stage2_parse.
int runTokenizer(const Tokenizer &t, int raw);
