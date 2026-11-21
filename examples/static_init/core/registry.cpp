// Copyright (c) 2026 The vycor-cpp Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0

// registry.cpp — the registry itself, and the one initializer of the
// `core` target.

#include "registry.hpp"

namespace reg {

namespace {

struct Entry {
  const char *name;
  Entry *next;
};

void *gLock = nullptr;   // constant-initialized: not a dynamic initializer
Entry *gHead = nullptr;  // likewise

Entry *allocateEntry() {
  return static_cast<Entry *>(malloc(sizeof(Entry)));
}

} // namespace

int nextId() {
  static int next = 0;
  return ++next;
}

int registerPlugin(const char *name) {
  pthread_mutex_lock(&gLock);
  Entry *entry = allocateEntry();
  entry->name = name;
  entry->next = gHead;
  gHead = entry;
  pthread_mutex_unlock(&gLock);
  return nextId();
}

void announce(const char *name) { puts(name); }

// Reaches registerPlugin and everything under it: lock and allocation.
static int coreSlot = registerPlugin("core");

int coreOrdinal() { return coreSlot; }

} // namespace reg
