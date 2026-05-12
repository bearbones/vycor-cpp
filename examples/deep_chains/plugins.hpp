// Copyright (c) 2026 The vycor-cpp Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include <memory>
#include <string>
#include <vector>

// Plugin hierarchy used by stage4_dispatch. Dispatch iterates a vector of
// unique_ptr<Plugin> and invokes handle() through the base pointer —
// CallGraphBuilder's handleVirtualDispatch emits Plausible VirtualDispatch
// edges to the base method and each override.
class Plugin {
public:
  virtual ~Plugin() = default;
  virtual int handle(int x) const = 0;      // pure virtual
  virtual std::string describe() const;     // default impl
};

class PluginAlpha : public Plugin {
public:
  int handle(int x) const override;
  std::string describe() const override;
};

class PluginBeta : public Plugin {
public:
  int handle(int x) const override;
  // Deliberately does NOT override describe() — uses Plugin::describe base.
};

class PluginGamma : public Plugin {
public:
  int handle(int x) const override;
  std::string describe() const override;
};

// Factory returning a vector of plugins by base pointer. Callers cannot
// prove which derived types the vector contains — virtual calls through
// that vector remain Plausible.
std::vector<std::unique_ptr<Plugin>> makeDefaultPlugins();
