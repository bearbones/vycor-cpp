// Copyright (c) 2026 The vycor-cpp Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0

#pragma once

// Small virtual hierarchy used by stage2_parse. stage2_parse accepts a
// Tokenizer& parameter, so the resulting virtual call is Plausible,
// fanning out to both JsonTokenizer::tokenize and TextTokenizer::tokenize.
class Tokenizer {
public:
  virtual ~Tokenizer() = default;
  virtual int tokenize(int raw) const = 0;
};

class JsonTokenizer : public Tokenizer {
public:
  int tokenize(int raw) const override;
};

class TextTokenizer : public Tokenizer {
public:
  int tokenize(int raw) const override;
};
