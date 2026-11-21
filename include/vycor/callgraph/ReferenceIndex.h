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

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace vycor {

// ============================================================================
// Reference and symbol facts — a parallel index alongside CallGraph,
// ControlFlowIndex and ChannelIndex (docs/symbol-references.md).
//
// The call graph answers "who calls whom". This index answers "who
// references what, and how" for every entity that can have linkage:
// functions, variables with static storage duration and classes.
//
//   - A *reference record* is keyed by (referrer, target, role): the
//     function (or variable initializer, or file scope) a reference appears
//     in, the entity it names, and what the reference does. It carries an
//     occurrence count and ONE witness site. It is not one record per site,
//     so the section stays bounded by the number of distinct pairs.
//   - A *symbol record* is one declaration of a symbol as one TU saw it: the
//     attributes the symbol has (kind, linkage class, flags, size) and where
//     that declaration is.
//
// Storage: flat records over interned strings (USRs, names, file paths, TU
// paths share one StringInterner), deduplicated by content with a per-record
// count of the TUs that contributed it, so a header's inline function
// indexed by 900 TUs is one record. Per-TU contributor lists make
// removeTUs/absorb exact: warm refresh equals a clean rebuild.
// ============================================================================

enum class RefRole : uint8_t {
  Call = 0,        // callee of a call (including constructor calls)
  AddressOf = 1,   // &x, array/function decay, binding to a non-const
                   // reference: any use that lets the entity be reached
                   // through an alias
  Read = 2,        // value read (lvalue-to-rvalue, const member call, ...)
  Write = 3,       // assignment, ++/--, non-const member call
  VtableUse = 4,   // constructing or destroying a polymorphic class
  TypeinfoUse = 5, // dynamic_cast, typeid, throw, catch
};
constexpr unsigned kRefRoleCount = 6;

enum class SymbolKind : uint8_t {
  Function = 0,
  Variable = 1,
  Class = 2,
  File = 3, // file scope as a referrer: references outside any function or
            // variable initializer (default member initializers, ...)
};
constexpr unsigned kSymbolKindCount = 4;

// What the symbol is at link level. Ordered strongest first: when TUs
// disagree, the merged view takes the smallest value.
enum class LinkageClass : uint8_t {
  External = 0,              // one strong definition
  TemplateInstantiation = 1, // implicit or explicit instantiation
  Vague = 2,                 // inline / in-class / template pattern /
                             // class without a key function
  Internal = 3,              // static, anonymous namespace, local
  None = 4,                  // not a linkable entity (file scope)
};
constexpr unsigned kLinkageClassCount = 5;

// SymbolRecord::flags. kSymDefinition, kSymInHeader and kSymSystemHeader
// describe the declaration the record is for; the rest describe the symbol
// and are OR-ed across its records in the merged view.
enum SymbolFlag : uint32_t {
  kSymDefinition = 1u << 0,  // this declaration is a definition
  kSymInHeader = 1u << 1,    // this declaration is not in the TU's main file
  kSymExternC = 1u << 2,     // C language linkage
  kSymVisibility = 1u << 3,  // explicit visibility attribute
  kSymDllExport = 1u << 4,
  kSymDllImport = 1u << 5,
  kSymUsedAttr = 1u << 6,    // used / retain / constructor / destructor
  kSymWeak = 1u << 7,        // weak / weak_import / alias / ifunc
  kSymMember = 1u << 8,      // member function or static data member
  kSymStaticLocal = 1u << 9, // function-local static
  kSymThreadLocal = 1u << 10,
  kSymConst = 1u << 11,      // variable of const-qualified type
  kSymAtomic = 1u << 12,     // variable of _Atomic or std::atomic type
  kSymDynamicClass = 1u << 13, // class with a vtable
  kSymVtableHome = 1u << 14, // the contributing TU defines the key function
  kSymMain = 1u << 15,       // the program entry point
  kSymExplicitInstantiation = 1u << 16,
  kSymVirtual = 1u << 17,    // virtual member function
  kSymLambda = 1u << 18,     // lambda call operator (synthetic identity)
  kSymSystemHeader = 1u << 19, // this declaration is in a system header
  kSymSpecialization = 1u << 20, // explicit specialization of a template
};
constexpr uint32_t kSymbolFlagMask = (1u << 21) - 1;
constexpr uint32_t kSymbolPerDeclFlags =
    kSymDefinition | kSymInHeader | kSymSystemHeader;

[[nodiscard]] const char *refRoleName(RefRole role) noexcept;
[[nodiscard]] std::optional<RefRole> refRoleFromName(llvm::StringRef name);
[[nodiscard]] const char *symbolKindName(SymbolKind kind) noexcept;
[[nodiscard]] const char *linkageClassName(LinkageClass linkage) noexcept;
// Spellings of the symbol-level flags set in `flags`, in bit order
// ("extern_c", "visibility", ..., "lambda"). The per-declaration flags and
// kSymVtableHome have no spelling: they are reported per declaration and
// as the vtable home TU.
[[nodiscard]] std::vector<const char *> symbolAttributeNames(uint32_t flags);

// String forms of a record: what tests and other hand-built producers pass,
// and what the lowering to interned records starts from.
struct SymbolFact {
  std::string usr;
  std::string name; // qualified display name
  std::string file; // file of this declaration
  uint32_t line = 0;
  SymbolKind kind = SymbolKind::Function;
  LinkageClass linkage = LinkageClass::External;
  uint32_t flags = 0;
  uint32_t size = 0; // function: body source lines; variable/class: bytes
};

struct ReferenceFact {
  std::string referrerUsr;
  std::string targetUsr;
  RefRole role = RefRole::Call;
  uint32_t count = 1; // occurrences inside the referrer
  std::string file;   // witness site
  uint32_t line = 0;
  uint32_t col = 0;
};

// One declaration of a symbol in the merged view.
struct SymbolDeclInfo {
  std::string file;
  uint32_t line = 0;
  bool definition = false;
  bool inHeader = false;
  uint32_t tuCount = 0; // TUs that saw this declaration
  std::string firstTu;  // the smallest of them
};

// A symbol merged over every record of its USR.
struct SymbolInfo {
  std::string usr;
  std::string name;
  SymbolKind kind = SymbolKind::Function;
  LinkageClass linkage = LinkageClass::External;
  uint32_t flags = 0; // symbol flags only (no per-declaration flags)
  uint32_t size = 0;
  // Sorted by (file, line, definition).
  std::vector<SymbolDeclInfo> declarations;
  bool declaredInHeader = false; // any declaration with inHeader
  // TUs holding a definition. A strong external symbol has one; an inline
  // function has one per TU that recorded it.
  uint32_t definingTuCount = 0;
  std::string firstDefiningTu;
  // The TU that defines a dynamic class's key function (emits the vtable
  // and typeinfo strongly). Empty when no indexed TU does.
  std::string vtableHomeTu;
};

// A reference merged over every record of its (referrer, target, role).
struct ReferenceInfo {
  std::string referrerUsr;
  std::string referrerName;
  SymbolKind referrerKind = SymbolKind::Function;
  std::string targetUsr;
  std::string targetName;
  SymbolKind targetKind = SymbolKind::Function;
  RefRole role = RefRole::Call;
  uint32_t count = 0;
  std::string file;
  uint32_t line = 0;
  uint32_t col = 0;
  uint32_t tuCount = 0; // TUs that contributed the reference
  std::string firstTu;  // the smallest of them
};

class ReferenceIndex {
public:
  using Id = StringInterner::Id;
  static constexpr Id kNoId = UINT32_MAX;

  // Interned records: what the bake visitor emits and what is stored.
  struct SymbolRecord {
    Id usr = 0;
    Id name = 0;
    Id file = 0;
    uint32_t line = 0;
    uint32_t flags = 0;
    uint32_t size = 0;
    SymbolKind kind = SymbolKind::Function;
    LinkageClass linkage = LinkageClass::External;
  };
  struct ReferenceRecord {
    Id referrer = 0;
    Id target = 0;
    Id file = 0;
    uint32_t line = 0;
    uint32_t col = 0;
    uint32_t count = 0;
    RefRole role = RefRole::Call;
  };

  ReferenceIndex() = default;
  // std::mutex is neither copyable nor movable: the moves take every field
  // except mutex_ (mirrors ChannelIndex).
  ReferenceIndex(ReferenceIndex &&other) noexcept;
  ReferenceIndex &operator=(ReferenceIndex &&other) noexcept;
  ReferenceIndex(const ReferenceIndex &) = delete;
  ReferenceIndex &operator=(const ReferenceIndex &) = delete;

  // ---- producers --------------------------------------------------------

  // Intern a string from the frontend (raw bytes) as index text (Utf8.h).
  Id internText(llvm::StringRef raw);

  // Register one TU's facts in one call: every record is deduplicated
  // against the stored ones by content and counted once for `tu` (an id
  // from internText). The records' ids must come from this index.
  void addTuFacts(Id tu, llvm::ArrayRef<SymbolRecord> symbols,
                  llvm::ArrayRef<ReferenceRecord> references);

  // String forms of the same (tests, hand-built indexes).
  void addSymbol(const SymbolFact &fact, const std::string &tuPath);
  void addReference(const ReferenceFact &fact, const std::string &tuPath);

  // Merge another index into this one (worker-shard merge, warm refresh):
  // every registration is replayed under the TU that made it, so removeTUs
  // on this index releases exactly what that TU contributed.
  void absorb(const ReferenceIndex &shard);

  // Remove everything the given TUs contributed. Returns the number of
  // records that lost their last contributor.
  size_t removeTU(const std::string &tuPath);
  size_t removeTUs(const std::vector<std::string> &tuPaths);

  // Drop the records no TU contributes any more.
  void compact();

  // ---- sizes ------------------------------------------------------------

  // Live records (at least one contributing TU).
  [[nodiscard]] size_t symbolRecordCount() const;
  // Distinct symbols (USRs) among them.
  [[nodiscard]] size_t symbolCount() const;
  [[nodiscard]] size_t referenceCount() const;
  [[nodiscard]] bool empty() const;
  // Decoded with `readOnly`: answers queries, cannot be changed or saved.
  [[nodiscard]] bool readOnly() const;

  // ---- queries ----------------------------------------------------------
  // Every list is sorted by a total order over its content, so results do
  // not depend on bake order (docs/deterministic-output.md).

  // The symbol with this USR, merged over its records.
  [[nodiscard]] std::optional<SymbolInfo> symbol(const std::string &usr) const;
  // USRs of the symbols whose display name is `name`, sorted.
  [[nodiscard]] std::vector<std::string>
  usrsNamed(const std::string &name) const;
  // Every symbol, sorted by (name, usr).
  [[nodiscard]] std::vector<SymbolInfo> allSymbols() const;

  // References to / from a USR, sorted by (referrerName, referrerUsr,
  // targetName, targetUsr, role).
  [[nodiscard]] std::vector<ReferenceInfo>
  referencesTo(const std::string &targetUsr) const;
  [[nodiscard]] std::vector<ReferenceInfo>
  referencesFrom(const std::string &referrerUsr) const;
  // Every reference, in the same order.
  [[nodiscard]] std::vector<ReferenceInfo> allReferences() const;

  // ---- bulk access --------------------------------------------------------
  // For whole-index consumers that build their own flat tables (the
  // dependency audit, deps/DependencyAudit.h) instead of materializing
  // every record as strings. Each live record once, with the number of TUs
  // that contribute it and the smallest of them (an id resolving through
  // strings(); kNoId when unknown). Storage order, which depends on bake
  // order: a consumer that emits an order sorts by content. `fn` runs under
  // the index lock and must not call back into the index.
  //   fn(const SymbolRecord &, uint32_t tuCount, Id minTu)
  template <typename Fn> void forEachSymbolRecord(Fn &&fn) const {
    std::lock_guard<std::mutex> lock(mutex_);
    ensureMinTu();
    for (const StoredSymbol &s : symbols_)
      if (s.refs != 0)
        fn(s.rec, s.refs, s.minTu);
  }
  // A reference record also carries its largest contributing TU: every
  // contributor lies between minTu and maxTu in path order.
  //   fn(const ReferenceRecord &, uint32_t tuCount, Id minTu, Id maxTu)
  template <typename Fn> void forEachReferenceRecord(Fn &&fn) const {
    std::lock_guard<std::mutex> lock(mutex_);
    ensureMinTu();
    for (const StoredReference &r : references_)
      if (r.refs != 0)
        fn(r.rec, r.refs, r.minTu, r.maxTu);
  }
  // The table every record id resolves through. Valid while the index is
  // not mutated.
  [[nodiscard]] const StringInterner &strings() const noexcept {
    return strings_;
  }

  // ---- persistence ------------------------------------------------------
  // The snapshot's references section (Snapshot.h, format v16). The bytes
  // are canonical: strings, records and contributor lists are written in
  // sorted order, so two indexes holding the same facts encode alike
  // whatever order their TUs were baked or absorbed in.

  // Append the encoding of the live records to `out`.
  void encode(std::string &out) const;
  // Replace this (empty) index with the one encoded in [begin, end). The
  // bytes are untrusted: every id, enum, count and ordering is checked
  // here, once, so queries read the records without checks. `readOnly`
  // skips the contributor lists and the dedup tables; such an index
  // answers queries and ignores mutation. The lists are then not checked
  // against the records' TU counts either (nothing reads them), so a
  // read-only load accepts some damaged sections a mutable load refuses;
  // the snapshot's section checksum is what catches those. Returns false
  // (index left empty) when the bytes do not decode.
  [[nodiscard]] bool decode(const char *begin, const char *end, bool readOnly);

private:
  friend class SnapshotIO;
  friend class MegascopeBridge;

  struct StoredSymbol {
    SymbolRecord rec;
    uint32_t refs = 0; // contributing TUs; 0 = dead
    // Smallest contributing TU: derived from the contributor lists
    // (ensureMinTu), or read from a snapshot.
    mutable Id minTu = kNoId;
  };
  struct StoredReference {
    ReferenceRecord rec;
    uint32_t refs = 0;
    mutable Id minTu = kNoId;
    mutable Id maxTu = kNoId; // largest contributing TU, derived alike
  };
  struct TuContribution {
    std::vector<uint32_t> symbols;    // indices into symbols_
    std::vector<uint32_t> references; // indices into references_
  };

  // Open-addressing set of record indices, hashed by record content. Dead
  // records stay in the table: a record registered again after its last
  // contributor left is revived in place, so removal never touches the
  // table and only compact() rebuilds it.
  struct RecordTable {
    std::vector<uint32_t> slots; // index + 1; 0 = empty
    size_t used = 0;
    void clear() {
      slots.clear();
      used = 0;
    }
  };

  // Lookup planes derived from the records: CSR adjacency keyed by string
  // id. Built once per mutation epoch (ensureLookup), never per query.
  struct Csr {
    std::vector<uint32_t> offsets; // size = string count + 1
    std::vector<uint32_t> items;
    [[nodiscard]] llvm::ArrayRef<uint32_t> at(Id key) const {
      if (static_cast<size_t>(key) + 1 >= offsets.size())
        return {};
      return llvm::ArrayRef<uint32_t>(items.data() + offsets[key],
                                      offsets[key + 1] - offsets[key]);
    }
    void clear() {
      offsets.clear();
      items.clear();
    }
  };

  static uint64_t hashOf(const SymbolRecord &r) noexcept;
  static uint64_t hashOf(const ReferenceRecord &r) noexcept;
  static bool same(const SymbolRecord &a, const SymbolRecord &b) noexcept;
  static bool same(const ReferenceRecord &a,
                   const ReferenceRecord &b) noexcept;

  uint32_t findOrInsertSymbol(const SymbolRecord &rec);
  uint32_t findOrInsertReference(const ReferenceRecord &rec);
  void rebuildTables();
  void invalidateDerived() noexcept {
    lookupValid_ = false;
    minTuValid_ = false;
  }
  // Recompute every live record's minTu (and a reference's maxTu) from the
  // contributor lists.
  void ensureMinTu() const;
  void ensureLookup() const;
  size_t removeTUsLocked(const std::vector<std::string> &tuPaths);

  [[nodiscard]] SymbolInfo symbolInfoLocked(Id usr) const;
  [[nodiscard]] std::vector<ReferenceInfo>
  referenceInfosLocked(llvm::ArrayRef<uint32_t> indices) const;
  [[nodiscard]] const std::string &text(Id id) const {
    return strings_.resolve(id);
  }

  mutable std::mutex mutex_;
  StringInterner strings_;
  std::vector<StoredSymbol> symbols_;
  std::vector<StoredReference> references_;
  RecordTable symbolTable_;
  RecordTable referenceTable_;
  std::unordered_map<Id, TuContribution> byTu_;
  size_t liveSymbols_ = 0;
  size_t liveReferences_ = 0;
  // A snapshot loaded read-only carries no contributor lists and no dedup
  // tables: it answers queries and must not be mutated.
  bool readOnly_ = false;

  // Derived planes (mutable: built lazily under mutex_ by const queries).
  mutable bool minTuValid_ = true;
  mutable bool lookupValid_ = false;
  mutable Csr symbolsByUsr_;   // usr id -> symbols_ indices
  mutable Csr refsByTarget_;   // target usr id -> references_ indices
  mutable Csr refsByReferrer_; // referrer usr id -> references_ indices
  mutable Csr usrsByName_;     // name id -> usr ids
};

} // namespace vycor
