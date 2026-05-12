// Copyright (c) 2026 The vycor-cpp Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0

#include "lambda_callbacks.hpp"

int registerValueCallback(std::function<int(int)> cb) { return cb(1); }

void registerRefCallback(std::function<void(State &)> cb) {
  State s;
  cb(s);
}

int Emitter::handle(int x) const { return x + bias_; }

void Emitter::emit() {
  // `[this]`-capture: the lambda body calls a member function. The
  // resulting DirectCall edge must attribute from the synthetic lambda
  // node, not from Emitter::emit.
  registerValueCallback([this](int x) { return this->handle(x); });
}
