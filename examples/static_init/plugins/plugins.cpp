// Copyright (c) 2026 The vycor-cpp Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0

// plugins.cpp — one initializer of each shape static-init-cost measures.

#include "../core/registry.hpp"

// A call with no effect: reach 1.
static int ordinal = reg::nextId();

// A new-expression in the initializer itself: allocation, reach 1.
static int *scratch = new int[16];

// A lambda written in the initializer and called there: I/O through
// announce -> puts.
static int greeted = [] {
  reg::announce("plugins");
  return 1;
}();

// A user-provided constructor: I/O.
struct Banner {
  Banner() { reg::announce("banner"); }
};
static Banner banner;

// A constructor function: runs before main without being an initializer.
__attribute__((constructor)) static void onLoad() { reg::announce("loaded"); }

// Constant initialization: no work before main, never reported.
constexpr int kLimit = 8;
static int limit = kLimit;

int main() { return ordinal + *scratch + greeted + limit; }
