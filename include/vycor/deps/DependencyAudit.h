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

#pragma once

#include "vycor/callgraph/StringInterner.h"
#include "vycor/deps/TargetMap.h"

#include "llvm/ADT/ArrayRef.h"

#include <cstdint>
#include <string>
#include <vector>

// The dependency audit (docs/dependency-audit.md): for every pair of link
// targets, which symbols of the provider the consumer's objects need, what
// those symbols drag along inside the provider, and what the consumer
// would stop linking if only that closure were split out.
//
// Two layers, so the evidence can change without touching the analysis:
//
//   LinkFacts      — symbols with an owning target set and a class, and the
//                    references between them. Flat tables, built once.
//                    linkFactsFromCallEdges derives them from the call
//                    graph; a producer over reference facts replaces it
//                    later and fills the same tables.
//   auditDependencies — the kernel. Reads LinkFacts and the TargetMap only.

namespace vycor {

class CallGraph;
struct CallGraphNode;
struct SnapshotMeta;

constexpr uint32_t kNoIndex = ~0u;

/// How a symbol takes part in linking.
enum class SymbolClass : uint8_t {
  /// No owning target is known (system headers, endpoints without a
  /// declaration in a mapped or attributable file). Ignored.
  Unattributed,
  /// Its body is compiled into the owning targets' objects.
  Defined,
  /// No body was seen. The owner is where the header that declares it
  /// points; the symbol may equally be an inline function with no calls
  /// in it. Reported apart, as uncertain.
  Presumed,
  /// Its body lives in a header several targets compile: it is emitted
  /// into whichever object uses it, so its references belong to its
  /// caller's target and the closure walks through it.
  Inline,
};

struct LinkFacts {
  /// What the facts were derived from ("call-edges").
  const char *edgeSource = "";
  /// Kinds of link dependency this source cannot see, as stable tags.
  std::vector<const char *> invisible;
  /// Spelling of a `refKind` value.
  const char *(*refKindName)(uint8_t kind) = nullptr;
  /// Symbol identities and call-site strings resolve through this.
  const StringInterner *strings = nullptr;

  // --- Files -----------------------------------------------------------
  std::vector<std::string> files; // normalized path per file id

  // --- Target sets -----------------------------------------------------
  // A symbol or a file can belong to several targets (a source compiled
  // into two of them). Sets are interned: id 0 is the empty set, ids
  // 1..targetCount are the singletons {id - 1}, larger ids index the CSR.
  uint32_t targetCount = 0;
  std::vector<uint32_t> setBegin; // per set id, size = sets + 1
  std::vector<uint32_t> setTargets; // ascending within a set

  [[nodiscard]] llvm::ArrayRef<uint32_t> targetsOf(uint32_t set) const {
    return llvm::ArrayRef<uint32_t>(setTargets)
        .slice(setBegin[set], setBegin[set + 1] - setBegin[set]);
  }
  [[nodiscard]] bool setHas(uint32_t set, uint32_t target) const noexcept {
    if (set - 1u < targetCount) // singleton (0 wraps and falls through)
      return set - 1u == target;
    for (uint32_t i = setBegin[set], e = setBegin[set + 1]; i < e; ++i)
      if (setTargets[i] == target)
        return true;
    return false;
  }

  // --- Symbols (one plane per attribute) -------------------------------
  std::vector<StringInterner::Id> symUsr;
  std::vector<const CallGraphNode *> symNode; // null: endpoint without node
  std::vector<SymbolClass> symClass;
  /// Target-set id. Defined, Presumed: the owning targets. Inline: the
  /// owner of the header that declares it, or 0 — never a link owner, only
  /// whose code it is (indirect uses). Unattributed: 0.
  std::vector<uint32_t> symOwner;
  std::vector<uint32_t> symFile;  // defining file id (Defined), else kNoIndex
  /// Unattributed, yet declared in a file under the map's roots that is no
  /// unlisted source: the build's own code, owner unknown. It may belong
  /// to any target, so a consumer that references one is never told a
  /// dependency is unused.
  std::vector<uint8_t> symUnplaced;

  [[nodiscard]] uint32_t symbolCount() const noexcept {
    return static_cast<uint32_t>(symUsr.size());
  }

  // --- References, CSR by referencing symbol ---------------------------
  // Within one symbol's range, ascending by refKey, then kind, then the
  // referenced symbol's USR; one reference per (from, to, anchor).
  std::vector<uint32_t> refBegin; // size = symbols + 1
  std::vector<uint32_t> refTo;
  /// The target set whose objects hold this reference, or 0 when it sits
  /// in an inline body and so belongs to whichever target uses that body.
  std::vector<uint32_t> refAnchor;
  /// Order key of the reference site: file rank (by path), line, column.
  std::vector<uint64_t> refKey;
  std::vector<StringInterner::Id> refSite; // "file:line:col"
  std::vector<uint8_t> refKind;

  // --- Soft references, CSR by referencing symbol ----------------------
  // References the source recorded but that name no symbol the linker is
  // known to need (with call edges: a call through a vtable slot, a
  // function-pointer-through-return join). They seed nothing and no
  // closure walks them; a consumer that holds one onto a provider's symbol
  // is counted as using the provider indirectly. One per (from, to,
  // anchor), in no contractual order: they are only ever counted.
  std::vector<uint32_t> softBegin; // size = symbols + 1
  std::vector<uint32_t> softTo;
  std::vector<uint32_t> softAnchor; // as refAnchor

  // --- Coverage, per target --------------------------------------------
  std::vector<uint32_t> targetSources; // sources the map lists
  std::vector<uint32_t> targetIndexed; // of those, parsed cleanly
  /// Has sources and every one parsed cleanly. False without index meta.
  std::vector<uint8_t> targetCovered;

  struct Counts {
    uint32_t mappedSources = 0;  // distinct source files in the map
    uint32_t indexedSources = 0; // of those, parsed cleanly
    uint32_t failedSources = 0;  // in the index, parse not clean
    uint32_t unmappedTus = 0;    // indexed TUs no target lists
    uint32_t defined = 0, presumed = 0, inlined = 0, unattributed = 0;
    uint64_t anchoredRefs = 0;   // references held by a target's objects
    uint64_t inlineBodyRefs = 0; // references that travel with a caller
    uint64_t unmappedBodyRefs = 0; // held by an indexed TU no target lists
    uint64_t skippedVirtualCalls = 0; // calls through a vtable slot
    uint64_t skippedDeferred = 0;     // function-pointer-through-return
    uint64_t unresolvedSites = 0;     // site file matched nothing known
  } counts;
  /// Whether per-TU outcomes and opened-file tables were available.
  bool hasIndexMeta = false;
  /// The index was baked with `--collapse-paths`: calls between two files
  /// under a collapsed path were never recorded, so an absent reference
  /// proves nothing and a closure may be undercounted.
  bool edgesCollapsed = false;

  /// Whether absence of a reference may be read as evidence at all.
  bool absenceIsEvidence() const noexcept {
    return hasIndexMeta && !edgesCollapsed;
  }
};

/// Stage-1 producer: link facts from the stored call edges
/// (docs/dependency-audit.md, "Attribution"). `meta` supplies the per-TU
/// outcomes and opened-file tables; without it no header is recognized as
/// private to a target and no target counts as covered.
LinkFacts linkFactsFromCallEdges(const CallGraph &graph, const TargetMap &map,
                                 const SnapshotMeta *meta);

/// How the build declares the provider for the consumer.
enum class DeclaredForm : uint8_t {
  None,       // not reachable through the consumer's dependencies
  Direct,     // named by the consumer's build file
  Transitive, // only reachable through another dependency
  Closure,    // listed, but the map cannot tell direct from transitive
};

const char *declaredFormName(DeclaredForm form);

enum : uint8_t {
  kSeedViaInline = 1, // reached through an inline body, not named by a source
  kSeedUncertain = 2, // the symbol's owner is presumed
};

struct AuditRow {
  uint32_t consumer = 0;
  uint32_t provider = 0;
  DeclaredForm declared = DeclaredForm::None;
  uint32_t symbols = 0;          // Defined provider symbols referenced
  uint32_t uncertainSymbols = 0; // Presumed provider symbols referenced
  uint32_t viaInline = 0;        // of `symbols`, reached only through inlines
  /// Provider symbols the consumer touches without a reference the
  /// linker is known to need: inline code declared in the provider's
  /// headers, static targets of soft references. Disjoint from the seeds.
  uint32_t indirectUses = 0;
  uint32_t closureSymbols = 0;   // Defined provider symbols the seeds need
  uint32_t closureFiles = 0;     // files holding those definitions
  uint32_t onlyViaEdge = 0;      // targets linked only because of this edge
  uint64_t droppableSymbols = 0; // Defined symbols in `droppable`, less K
  uint32_t seedBegin = 0, seedEnd = 0; // DependencyAudit::seed*
  uint32_t needBegin = 0, needEnd = 0; // DependencyAudit::needs
  uint32_t dropBegin = 0, dropEnd = 0; // DependencyAudit::droppable
  /// The consumer links the provider only because of this edge, and the
  /// closure does not pull it back in: splitting the closure out removes
  /// the provider from the consumer's link.
  bool providerDroppable = false;
};

struct DependencyAudit {
  /// Ascending by (consumer, provider): the declared dependencies of every
  /// target plus every pair with an observed reference.
  std::vector<AuditRow> rows;
  std::vector<uint32_t> rowBegin; // per consumer, size = targets + 1

  // Seeds of all rows. Within a row: certain before uncertain, named by a
  // source before via-inline, then by witness site, kind and USR.
  std::vector<uint32_t> seedSym;
  std::vector<uint32_t> seedRef;  // the reference that reaches the symbol
  std::vector<uint32_t> seedRoot; // the consumer-held reference it started at
  std::vector<uint8_t> seedFlags;

  /// Other targets a row's closure references, ascending.
  std::vector<uint32_t> needs;
  /// Targets a row's consumer would stop linking, ascending.
  std::vector<uint32_t> droppable;

  std::vector<uint32_t> providerSymbols; // Defined symbols per target
  std::vector<uint32_t> providerFiles;   // files holding them
  std::vector<uint32_t> presumedSymbols; // Presumed symbols per target
  /// Per target: distinct unplaced symbols (LinkFacts::symUnplaced) its
  /// objects reference, directly or through inline bodies.
  std::vector<uint32_t> unplacedUses;

  /// Row of (consumer, provider), or kNoIndex.
  [[nodiscard]] uint32_t find(uint32_t consumer, uint32_t provider) const;
};

DependencyAudit auditDependencies(const LinkFacts &facts,
                                  const TargetMap &map);

struct ThinThresholds {
  uint32_t maxSymbols = 5;
  uint32_t maxClosurePercent = 10;
};

enum : unsigned {
  kFindingUnused = 1,     // unused-link-dependency
  kFindingUndeclared = 2, // undeclared-link-dependency
  kFindingThin = 4,       // thin-dependency
};

/// The findings of one row. `coverageStated`: the adapter stated the
/// index's coverage (IndexFacts::stated). An unused finding needs it, plus
/// clean coverage of both targets: it fails closed.
unsigned rowFindings(const DependencyAudit &audit, const LinkFacts &facts,
                     const AuditRow &row, ThinThresholds thresholds,
                     bool coverageStated);

/// The Defined provider symbols in a row's closure (its seeds and what
/// they need inside the provider), in no particular order.
void closureSymbolsOf(const LinkFacts &facts, const DependencyAudit &audit,
                      const AuditRow &row, std::vector<uint32_t> &out);

} // namespace vycor
