// Copyright (c) 2026 The vycor-cpp Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0

#include "stage2_parse.hpp"

#include "stage3_transform.hpp"
#include "tokenizer.hpp"

int runTokenizer(const Tokenizer &t, int raw) {
  // t is Tokenizer& — virtual call through base reference is Plausible,
  // fans out to every override of Tokenizer::tokenize.
  return t.tokenize(raw);
}

int stage2_parse(Registry &reg, int x) {
  // Plausible: address-take and store (mix-invariant at this layer).
  reg.auditor = &cbs::logAfter;

  // Plausible: invoke a virtual method through a base reference,
  // delegating to a helper that does the dispatch. The Plausible edge
  // lives on runTokenizer, but because stage2_parse has a Proven direct
  // call to runTokenizer, and the address-take above is Plausible, the
  // "mix at every layer" invariant is satisfied at stage2_parse itself.
  JsonTokenizer jt;
  int toks = runTokenizer(jt, x);

  // Proven: direct call to the next stage.
  return stage3_transform(reg, toks);
}
