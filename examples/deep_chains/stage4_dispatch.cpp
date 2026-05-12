// Copyright (c) 2026 The vycor-cpp Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0

#include "stage4_dispatch.hpp"

#include "plugins.hpp"
#include "stage5_sink.hpp"

int stage4_dispatch(Registry &reg, int x) {
  // Plausible: iterate plugins by base pointer and invoke a virtual method.
  // handleVirtualDispatch emits Plausible edges to Plugin::handle (base)
  // and every concrete override (PluginAlpha/Beta/Gamma::handle).
  auto plugins = makeDefaultPlugins();
  int acc = x;
  for (const auto &p : plugins)
    acc = p->handle(acc);

  // Proven: direct call to the next stage.
  return stage5_sink(reg, acc);
}
