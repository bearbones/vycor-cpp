// Copyright (c) 2026 The vycor-cpp Authors
// Original author: Alex Mason
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Example project for testing the prism control flow query system.
// Demonstrates various exception handling and guard patterns.

#include <cstddef>
#include <cstdint>
#include <new>
#include <stdexcept>
#include <vector>

// --- Low-level allocation that can throw ---

void allocateBuffer(size_t size) {
  std::vector<char> buf;
  buf.resize(size); // Can throw std::bad_alloc.
}

// --- Sign extension vulnerability scenario ---
// An int16_t is widened to size_t. If negative, this produces a huge value.

void processChunk(int16_t chunkSize) {
  allocateBuffer(static_cast<size_t>(chunkSize));
}

// --- Protected path: try/catch around the call ---

void safeProcessor() {
  try {
    processChunk(32000);
  } catch (const std::bad_alloc &e) {
    // Handled: log and continue.
  }
}

// --- Protected path with broader catch ---

void broadCatcher() {
  try {
    processChunk(100);
  } catch (const std::exception &e) {
    // Catches std::bad_alloc through base class.
  }
}

// --- Protected path with catch-all ---

void catchAllHandler() {
  try {
    processChunk(200);
  } catch (...) {
    // Catches everything.
  }
}

// --- Unprotected path: no try/catch ---

void unsafeProcessor() {
  processChunk(-1); // Dangerous: -1 becomes huge size_t value.
}

// --- Conditional guard: size check before call ---

void guardedProcessor(int16_t size) {
  if (size > 0 && size < 10000) {
    processChunk(size); // Guard prevents negative values.
  }
}

// --- Nested try/catch ---

void nestedHandler() {
  try {
    try {
      processChunk(500);
    } catch (const std::runtime_error &e) {
      // Inner catch: only catches runtime_error, not bad_alloc.
    }
  } catch (const std::bad_alloc &e) {
    // Outer catch: catches bad_alloc that escapes inner try.
  }
}

// --- noexcept function: would terminate if exception escapes ---

void noexceptCaller() noexcept {
  processChunk(100); // If this throws, std::terminate is called.
}

// --- Multiple callers of the same function ---

int main() {
  safeProcessor();     // Protected by try/catch(bad_alloc).
  broadCatcher();      // Protected by try/catch(exception).
  catchAllHandler();   // Protected by catch(...).
  unsafeProcessor();   // UNPROTECTED.
  guardedProcessor(5); // Guarded by conditional.
  nestedHandler();     // Protected by nested try/catch.
  noexceptCaller();    // noexcept barrier.
  return 0;
}
