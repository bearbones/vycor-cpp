// Copyright (c) 2026 The vycor-cpp Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0

#include "plugins.hpp"

#include <vector>

std::string Plugin::describe() const { return "plugin"; }

int PluginAlpha::handle(int x) const { return x * 2; }
std::string PluginAlpha::describe() const { return "alpha"; }

int PluginBeta::handle(int x) const { return x + 100; }

int PluginGamma::handle(int x) const { return x ^ 0xFF; }
std::string PluginGamma::describe() const { return "gamma"; }

std::vector<std::unique_ptr<Plugin>> makeDefaultPlugins() {
  std::vector<std::unique_ptr<Plugin>> out;
  out.push_back(std::make_unique<PluginAlpha>());
  out.push_back(std::make_unique<PluginBeta>());
  out.push_back(std::make_unique<PluginGamma>());
  return out;
}
