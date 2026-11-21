// Copyright (c) 2026 The vycor-cpp Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0

// registry.hpp — the registration API of the static_init fixture
// (examples/static_init/README.md).
//
// The libc primitives are declared here rather than included so the
// fixture's facts do not depend on the host's system headers.
#pragma once

extern "C" {
void *malloc(unsigned long size);
int puts(const char *text);
int pthread_mutex_lock(void *mutex);
int pthread_mutex_unlock(void *mutex);
}

namespace reg {

/// Next registration ordinal. No allocation, lock or I/O.
int nextId();

/// Append `name` to the registry: takes the registry lock and allocates
/// the entry.
int registerPlugin(const char *name);

/// Print `name`.
void announce(const char *name);

/// Register a type by its static name.
template <class T> int registerType() {
  const char *name = T::name();
  return registerPlugin(name);
}

/// A dynamic initializer in a header: one variable, initialized by every
/// TU that includes this file.
inline int headerSlot = registerPlugin("header");

} // namespace reg
