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

// isa-leak (docs/checks/isa-leak.md): vague-linkage functions emitted by
// translation units built with different code-generation flags. The linker
// keeps one COMDAT copy per function for every caller, so a body compiled
// with -mavx2 (or -ffast-math, or -fno-exceptions) in one TU can end up
// serving callers in TUs that asked for something else.
//
// Phase 1 (collectIsaLeakFacts) records, per compile command, one
// CodegenProfile — the settings that change machine code, read from the
// parsed invocation, so -march=haswell and its -m<feature> spelling agree —
// and the vague-linkage functions that command emits, each with a few bits
// saying what in its body (or in the bodies it can inline) the settings
// can change. The index-only pass (analyzeIsaLeaks) groups each function's
// emitting commands by the part of their profiles its body is sensitive to
// and reports a function whose commands fall into more than one group.

#pragma once

#include "vycor/callgraph/EmissionGraph.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace clang {
class ASTContext;
} // namespace clang

namespace vycor {

class GlobalIndex;
struct Diagnostic;

// The code-generation settings of one compile command that can change the
// machine code of a function body.
struct CodegenProfile {
  std::string triple;
  // Enabled target features after the CPU's implied features are applied
  // (-march, -mcpu, -m<feature>, -mno-<feature>), sorted, without the '+'.
  std::vector<std::string> features;
  // Floating-point semantics, canonical "key=value;" list (fp-contract,
  // reassociation, no-NaNs/Infs/signed-zeros, reciprocal, approximate
  // functions, math errno, rounding, exception behavior, evaluation
  // method, -mfpmath).
  std::string fp;
  bool exceptions = false;   // C++ exceptions (-fexceptions / -fno-exceptions)
  uint8_t signedOverflow = 0; // 0 undefined, 1 wraps (-fwrapv), 2 traps
};

// The facets of a profile the check compares (--isa-leak-facets).
enum IsaLeakFacet : unsigned {
  kIsaLeakIsa = 1u << 0,      // target features
  kIsaLeakFp = 1u << 1,       // floating-point semantics
  kIsaLeakEh = 1u << 2,       // exceptions on or off
  kIsaLeakOverflow = 1u << 3, // signed-overflow behavior
  kIsaLeakDefaultFacets = kIsaLeakIsa | kIsaLeakFp | kIsaLeakEh,
};

// Parses a comma-separated facet list (isa, fp, eh, overflow, all,
// default). False with `error` set on an unknown name.
bool parseIsaLeakFacets(const std::string &spec, unsigned &facets,
                        std::string &error);

// What a body contains that a code-generation setting can change. Computed
// over the function's own body plus every body visible in the TU that it
// calls (directly or transitively), since an optimizing build can inline
// those into it.
// The bits are the emission walk's (callgraph/EmissionGraph.h).
using IsaLeakBodyBit = EmittedBodyBit;

// One vague-linkage function a TU emitted: what the collector hands over.
struct EmittedFunction {
  std::string linkageName; // mangled: the COMDAT key the linker merges by
  std::string qualifiedName;
  std::string filePath; // definition site
  unsigned line = 0;
  uint32_t bodyBits = 0; // own body plus what it can inline
  uint32_t ownBits = 0;  // own body only
};

// One sighting: record `function` emitted by `tu` under `profile`, as
// indexes into the tables that travel with it.
struct IsaLeakSighting {
  uint32_t function = 0;
  uint32_t profile = 0;
  uint32_t tu = 0;
};

// The value form payloads carry (checkpoint journal, worker shards): string
// tables once, sightings as flat id triples.
struct IsaLeakPayload {
  std::vector<CodegenProfile> profiles;
  std::vector<std::string> tus;
  std::vector<EmittedFunction> functions;
  std::vector<IsaLeakSighting> sightings;
  bool empty() const { return sightings.empty(); }
};

// The isa-leak facts of an index: a profile table and, per linkage name, one
// record with every (profile, TU) that emitted it. Thread-safe writers;
// readers run after the phase barrier.
class IsaLeakFacts {
public:
  struct Record {
    EmittedFunction function; // location: the smallest seen; bits: OR-ed
    // (profile id, TU id), in insertion order; may repeat.
    std::vector<std::pair<uint32_t, uint32_t>> sightings;
  };

  // Interns a profile; returns its id in this store.
  uint32_t addProfile(const CodegenProfile &profile);
  // One TU's emitted functions under `profile` (an id of this store).
  void addTu(const std::string &tuPath, uint32_t profile,
             std::vector<EmittedFunction> &&functions);
  // Adds another store's facts, remapping its ids.
  void absorb(const IsaLeakFacts &other);
  // Value form, and back (ids remapped into this store; out-of-range
  // sightings are dropped).
  void capture(IsaLeakPayload &out) const;
  void apply(const IsaLeakPayload &payload);
  void clear();

  const std::vector<CodegenProfile> &profiles() const { return profiles_; }
  const std::vector<Record> &records() const { return records_; }
  const std::string &tuPath(uint32_t id) const { return tus_[id]; }
  size_t functionCount() const { return records_.size(); }
  size_t sightingCount() const;

private:
  uint32_t recordLocked(const EmittedFunction &f);
  uint32_t tuIdLocked(const std::string &tu);

  mutable std::mutex mutex_;
  std::vector<CodegenProfile> profiles_;
  std::unordered_map<std::string, uint32_t> profileByKey_;
  std::vector<std::string> tus_;
  std::unordered_map<std::string, uint32_t> tuIds_;
  std::vector<Record> records_;
  std::unordered_map<std::string, uint32_t> recordByName_;
};

// Phase 1: the profile of the TU's compile command and the vague-linkage
// functions it emits, added to index.mutableIsaLeakFacts(). Called once per
// TU, after the parse and all pending instantiations.
void collectIsaLeakFacts(clang::ASTContext &context, GlobalIndex &index);

// The index-only pass: one diagnostic per function and target triple whose
// emitting compile commands disagree on a facet in `facets` that its body
// is sensitive to — IsaLeak_Divergent when its own body is, IsaLeak_Inlined
// when only bodies it can inline are.
void analyzeIsaLeaks(const GlobalIndex &index, unsigned facets,
                     std::vector<Diagnostic> &diagnostics);

} // namespace vycor
