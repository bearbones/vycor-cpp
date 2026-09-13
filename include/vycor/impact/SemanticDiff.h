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

#include "vycor/callgraph/CallGraph.h"
#include "vycor/callgraph/ControlFlowIndex.h"
#include "vycor/callgraph/PathSearch.h"
#include "vycor/callgraph/Snapshot.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// Semantic comparison of two baked indexes (docs/change-impact.md): which
// functions, call relationships, and call-site context facts differ, by
// a cross-index identity that never depends on interner ids, insertion
// order, or line numbers. Transport-neutral; the CLI `diff` verb and the
// tests are its consumers.

namespace vycor {

// ---------------------------------------------------------------------------
// Identity
// ---------------------------------------------------------------------------

/// The comparison key of every node of one graph. A USR is its own key
/// unless it carries a source position:
///
/// - a lambda's synthetic usr (`vycor-lambda:lambda#file:line:col#
///   enclosing`) becomes `vycor-lambda:<file>#<enclosing key>#<ordinal>`,
///   the ordinal being its rank by (line, col) among the lambdas of the
///   same file and enclosing function;
/// - a clang USR that names an anonymous type by its byte offset
///   (`<file>@<offset>`, chiefly a lambda's closure type as a template
///   argument of the function instantiated with it: std::function's or
///   std::thread's constructor, an algorithm taking a comparator) has
///   every such offset replaced by `%<ordinal>`, the ordinal being the
///   offset's rank among the offsets of the same file and enclosing
///   context (group `closure:<file><context up to the anonymous tag>`).
///
/// So a line shift keeps every identity; adding or removing a lambda
/// shifts the ordinals after it, which the group counts expose
/// (markAmbiguousGroups).
class IdentityTable {
public:
  static IdentityTable build(const CallGraph &graph);

  /// The key of a usr (the usr itself when nothing rewrites it).
  const std::string &keyOf(const std::string &usr) const;
  /// The usr a key stands for in this graph, or nullopt when the key is
  /// not one of this graph's nodes.
  std::optional<std::string> usrOf(const std::string &key) const;
  /// Whether the key belongs to an ambiguous lambda group (see
  /// markAmbiguousGroups).
  bool isAmbiguous(const std::string &key) const;

  /// Group id (lambdas: `<file>#<enclosing key>`; anonymous types:
  /// `closure:<file>...`) -> the keys in it, in ordinal order.
  const std::map<std::string, std::vector<std::string>> &groups() const {
    return groups_;
  }
  /// The group a rewritten key belongs to; empty for a plain usr.
  std::string groupOf(const std::string &key) const;

  /// Mark, on both tables, every group that both graphs hold with a
  /// different number of members (lambdas, or distinct offsets). Their
  /// keys are excluded from the comparison: an ordinal cannot say which
  /// member was added or removed.
  static void markAmbiguousGroups(IdentityTable &a, IdentityTable &b);

private:
  std::unordered_map<std::string, std::string> keyByUsr_;
  std::unordered_map<std::string, std::string> usrByKey_;
  std::map<std::string, std::vector<std::string>> groups_;
  std::map<std::string, size_t> groupSizes_;
  std::unordered_map<std::string, std::string> groupByKey_;
  std::unordered_map<std::string, bool> ambiguousKeys_;
};

// ---------------------------------------------------------------------------
// Results
// ---------------------------------------------------------------------------

enum class ChangeKind : uint8_t {
  FunctionRemoved,
  FunctionAdded,
  CallRemoved,
  CallAdded,
  CallChanged,
  ContextChanged,
};

/// "function_removed", "function_added", "call_removed", "call_added",
/// "call_changed", "context_changed".
const char *changeKindName(ChangeKind k);

/// One call site of a relationship, as a witness.
struct SiteWitness {
  std::string callSite;
  Confidence confidence = Confidence::Unknown;
  ExecutionContext execContext = ExecutionContext::Synchronous;
  unsigned indirectionDepth = 0;
  /// The context signature at this site (docs/change-impact.md), "absent"
  /// when the index holds no context for it, empty when contexts were
  /// not compared.
  std::string signature;
};

/// One side of a relationship (docs/change-impact.md, "Relationships").
struct RelationshipSide {
  /// In canonical edge order (call site, then attributes).
  std::vector<SiteWitness> sites;
  /// Sorted multiset of the sites' attributes ("Proven/Synchronous/0").
  std::vector<std::string> attributes;
  /// Sorted multiset of the sites' context signatures (empty when
  /// contexts were not compared).
  std::vector<std::string> signatures;
};

struct FunctionFacts {
  std::string key;
  std::string usr;
  std::string name;
  std::string file;
  unsigned line = 0;
  bool isEntryPoint = false;
  bool isVirtual = false;
  std::string enclosingClass;
};

struct ChangeRecord {
  ChangeKind change = ChangeKind::CallAdded;
  // Call changes: the relationship.
  std::string callerKey, calleeKey;
  std::string callerName, calleeName;
  EdgeKind kind = EdgeKind::DirectCall;
  std::optional<RelationshipSide> before, after;
  // Function changes: the function.
  FunctionFacts function;
  /// Coverage explanation for a function change whose file is a TU the
  /// other index does not hold cleanly: "tu_absent_before",
  /// "tu_absent_after", "tu_failed_before", "tu_failed_after",
  /// "tu_partial_before", "tu_partial_after". Empty otherwise.
  std::string explanation;
};

struct AmbiguousIdentity {
  std::string group; // "<file>#<enclosing key>"
  std::vector<std::string> beforeUsrs, afterUsrs;
  size_t edgesWithheldBefore = 0, edgesWithheldAfter = 0;
};

struct RenameCandidate {
  std::string name;
  std::string beforeKey, afterKey;
  std::string beforeFile, afterFile;
};

struct FunctionMove {
  std::string key, name;
  std::string beforeFile, afterFile;
  unsigned beforeLine = 0, afterLine = 0;
};

struct SideScope {
  std::string bake; // "<environment>@<bakeStartNs>", empty when unknown
  IndexCoverage coverage;
  std::string analyzer, toolchain;
};

struct Comparability {
  bool comparable = true;      // false: refused unless allowed
  bool absenceReliable = true; // both complete, same TU set
  bool analyzerSame = true;
  bool toolchainSame = true;
  bool configSame = true;
  bool contextsCompared = false;
  std::vector<std::string> reasons;
  SideScope before, after;
  std::vector<std::string> tusOnlyBefore, tusOnlyAfter;
  std::vector<std::string> failedBefore, failedAfter;
  std::vector<std::string> partialBefore, partialAfter;
};

struct SemanticDiffOptions {
  bool compareContexts = true;
  bool includeMoves = false;
  /// Call sites kept per relationship side in the record (0 = all).
  size_t maxSites = 8;
  /// Run even when the bake configurations differ.
  bool allowMismatch = false;
};

struct SemanticDiffResult {
  Comparability comparability;
  /// The comparison was refused (configuration mismatch without
  /// allowMismatch): nothing below was computed.
  bool refused = false;
  /// Ordered: change kind (enum order), caller key, callee key, edge kind.
  std::vector<ChangeRecord> changes;
  /// By group id.
  std::vector<AmbiguousIdentity> ambiguous;
  /// By before key.
  std::vector<RenameCandidate> renameCandidates;
  /// By key; filled only with includeMoves.
  std::vector<FunctionMove> moves;
  size_t movedFunctions = 0;
  size_t functionsBefore = 0, functionsAfter = 0;
  size_t relationshipsBefore = 0, relationshipsAfter = 0;

  size_t count(ChangeKind k) const;
};

/// The context signatures of one side, built in one pass over the
/// stored contexts so the control-flow index can be dropped afterwards
/// (docs/change-impact.md, "Costs"). `forEdge` answers what
/// `ControlFlowIndex::contextForEdge` would have chosen for the edge.
class ContextSignatures {
public:
  static ContextSignatures build(const ControlFlowIndex &cf);

  /// The edge's signature, or "absent" when nothing is indexed at the
  /// site for that caller.
  const std::string &forEdge(const std::string &callSite,
                             const std::string &callerUsr,
                             const std::string &calleeUsr) const;

  size_t contextCount() const { return contexts_; }
  size_t shapeCount() const { return signatures_.size(); }

private:
  using Id = StringInterner::Id;
  struct Key {
    Id site, caller, callee; // callee kAnyCallee: the per-caller fallback
    bool operator==(const Key &o) const {
      return site == o.site && caller == o.caller && callee == o.callee;
    }
  };
  struct KeyHash {
    size_t operator()(const Key &k) const;
  };
  struct Choice {
    Id callee, tuPath, callerName;
    uint32_t signature; // index into signatures_
  };
  static constexpr Id kAnyCallee = UINT32_MAX;
  using Table = std::unordered_map<Key, Choice, KeyHash>;

  bool beats(const Choice &a, const Choice &b) const;
  void offer(Table &table, Key key, const Choice &c);

  StringInterner strings_;
  std::vector<std::string> signatures_;
  Table exact_;    // (site, caller, callee)
  Table fallback_; // (site, caller, any callee)
  size_t contexts_ = 0;
};

/// One side of a comparison. Contexts are compared through
/// `signatures` when given, else built from `cfIndex`; with neither the
/// control-flow section counts as not loaded and contexts are not
/// compared.
struct DiffSide {
  const CallGraph &graph;
  const ControlFlowIndex *cfIndex = nullptr;
  const SnapshotMeta *meta = nullptr; // null: no provenance to compare
  const ContextSignatures *signatures = nullptr;
};

/// The bake-level comparison (docs/change-impact.md, "Comparability").
Comparability checkComparability(const SnapshotMeta *before,
                                 const SnapshotMeta *after,
                                 bool contextsLoaded);

/// The semantic diff of two indexes. `identity` may be null (built
/// internally); pass the tables when the caller also needs them (route
/// diff, impact) so the ambiguity marks are shared.
SemanticDiffResult semanticDiff(const DiffSide &before, const DiffSide &after,
                                const SemanticDiffOptions &opts,
                                IdentityTable *beforeIdentity = nullptr,
                                IdentityTable *afterIdentity = nullptr);

/// The context signature of a call site (docs/change-impact.md): what
/// the exception and lock tools read there, without any location.
std::string contextSignature(const CallSiteContext &ctx);

/// The after-side usrs `impact_of_change` should start from for a diff:
/// the caller of every call or context change (when it still exists) and
/// every added function. Sorted, deduplicated.
std::vector<std::string> changedFunctionsAfter(const SemanticDiffResult &diff,
                                               const IdentityTable &after);

// ---------------------------------------------------------------------------
// Routes
// ---------------------------------------------------------------------------

struct RouteDiff {
  PathSearchResult before, after;
  /// Paths (after-side hops) whose key sequence no before path has.
  std::vector<CallPath> added;
  /// Paths (before-side hops) whose key sequence no after path has.
  std::vector<CallPath> removed;
  size_t unchanged = 0;
  /// Both searches complete (docs/path-analysis.md).
  bool complete = false;
  bool exhaustive = false;
};

/// The bounded path search on both graphs with the same limits, paths
/// compared as sequences of (caller key, callee key, kind). `target` and
/// the starts are resolved on each side; a name unknown to a side gives
/// that side no paths (targetKnown false), as find_call_chain does.
RouteDiff diffRoutes(const DiffSide &before, const DiffSide &after,
                     const IdentityTable &beforeIdentity,
                     const IdentityTable &afterIdentity,
                     const std::string &target,
                     const std::vector<std::string> &beforeStarts,
                     const std::vector<std::string> &afterStarts,
                     const SearchLimits &limits);

} // namespace vycor
