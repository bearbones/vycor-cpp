// Copyright (c) 2026 The vycor-cpp Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0

// codecs.cpp — an initializer that reaches the registry through a
// function template.

#include "../core/registry.hpp"

struct Codec {
  static const char *name() { return "codec"; }
};

// registerType<Codec> is an instantiation; its body is walked through the
// template's definition, so the reach continues into registerPlugin.
static int codecSlot = reg::registerType<Codec>();

int codecOrdinal() { return codecSlot; }
