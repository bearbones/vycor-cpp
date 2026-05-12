// Copyright (c) 2026 The vycor-cpp Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0

#include "stage5_sink.hpp"

int stage5_sink(Registry &reg, int x) {
  // Plausible: address-take stored in local.
  CallbackFn fmt = &cbs::finalFormat;
  (void)fmt;

  // Proven: direct call to Registry::invoke (non-polymorphic member call).
  return reg.invoke(x + 3);
}
