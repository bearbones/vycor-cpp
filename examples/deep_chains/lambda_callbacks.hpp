// Copyright (c) 2026 The vycor-cpp Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include <functional>

// Chain C: lambda-based callback surfaces exercised by runChainC() and
// Emitter::emit(). Every lambda passed here becomes a synthetic
// CallGraphNode named "lambda#file:line:col#enclosing"; the enclosing
// function emits a LambdaCall/Proven edge at the registration site, and
// calls inside the lambda body attribute to the synthetic node.
struct State {
  int acc = 0;
};

int registerValueCallback(std::function<int(int)> cb);
void registerRefCallback(std::function<void(State &)> cb);

// Emitter exists to prove that a `[this]` capture lambda whose body calls
// a non-static member method produces a DirectCall edge from the lambda
// node to Emitter::handle — not from Emitter::emit directly.
class Emitter {
public:
  int handle(int x) const;
  void emit();

private:
  int bias_ = 7;
};
