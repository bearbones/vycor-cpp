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

#include "vycor/impact/SemanticDiff.h"

#include "vycor/query/Serialize.h"

#include "llvm/ADT/StringRef.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace vycor {

// ---------------------------------------------------------------------------
// Identity
// ---------------------------------------------------------------------------

namespace {

constexpr llvm::StringRef kLambdaPrefix = "vycor-lambda:lambda#";

struct LambdaParts {
  std::string file;
  unsigned line = 0, col = 0;
  std::string enclosing; // display name of the enclosing function
};

bool allDigits(llvm::StringRef s) {
  if (s.empty())
    return false;
  for (char c : s)
    if (!std::isdigit(static_cast<unsigned char>(c)))
      return false;
  return true;
}

// "vycor-lambda:lambda#<file>:<line>:<col>#<enclosing>". The enclosing
// part may itself be a lambda name (nested lambdas) and so contain '#';
// the site is the shortest prefix ending in ":<line>:<col>" before a '#'.
std::optional<LambdaParts> parseLambda(llvm::StringRef usr) {
  if (!usr.starts_with(kLambdaPrefix))
    return std::nullopt;
  llvm::StringRef rest = usr.drop_front(kLambdaPrefix.size());
  size_t hash = rest.find('#');
  while (hash != llvm::StringRef::npos) {
    llvm::StringRef site = rest.substr(0, hash);
    size_t c2 = site.rfind(':');
    size_t c1 = c2 == llvm::StringRef::npos ? c2 : site.rfind(':', c2);
    if (c1 != llvm::StringRef::npos && c1 > 0 &&
        allDigits(site.substr(c1 + 1, c2 - c1 - 1)) &&
        allDigits(site.substr(c2 + 1))) {
      LambdaParts p;
      p.file = site.substr(0, c1).str();
      site.substr(c1 + 1, c2 - c1 - 1).getAsInteger(10, p.line);
      site.substr(c2 + 1).getAsInteger(10, p.col);
      p.enclosing = rest.substr(hash + 1).str();
      return p;
    }
    hash = rest.find('#', hash + 1);
  }
  return std::nullopt;
}

// A source position inside a clang USR: "<file>@<byte offset>", as clang
// spells a declaration that has no name of its own — above all a lambda's
// closure type, which reaches the graph as a template argument of every
// function instantiated with it. The offset moves with every edit above
// the lambda in its file.
struct LocationFragment {
  size_t begin = 0, end = 0; // [begin, end) of the digits after the '@'
  std::string file;
  unsigned long long offset = 0;
  std::string group; // "closure:<file><context up to the anonymous tag>"
};

bool isUsrSeparator(char c) {
  return c == '@' || c == '#' || c == '$' || c == '<' || c == '>' ||
         c == '(' || c == ')' || c == ',' || c == '&' || c == '*' ||
         c == ':';
}

std::vector<LocationFragment> locationFragments(llvm::StringRef usr) {
  std::vector<LocationFragment> out;
  for (size_t at = usr.find('@'); at != llvm::StringRef::npos;
       at = usr.find('@', at + 1)) {
    size_t d = at + 1;
    while (d < usr.size() && std::isdigit(static_cast<unsigned char>(usr[d])))
      ++d;
    if (d == at + 1)
      continue;
    if (d < usr.size() &&
        (std::isalnum(static_cast<unsigned char>(usr[d])) || usr[d] == '_'))
      continue; // digits that begin a longer name
    size_t f = at;
    while (f > 0 && !isUsrSeparator(usr[f - 1]))
      --f;
    llvm::StringRef file = usr.slice(f, at);
    if (file.empty() || !file.contains('.'))
      continue; // not a file name
    LocationFragment frag;
    frag.begin = at + 1;
    frag.end = d;
    frag.file = file.str();
    usr.slice(at + 1, d).getAsInteger(10, frag.offset);
    // The anonymous tag the offset locates ends the context that groups
    // it: "@Sa" (struct/class, lambdas included), "@Ua", "@Ea".
    llvm::StringRef rest = usr.drop_front(d);
    size_t tag = llvm::StringRef::npos;
    for (llvm::StringRef marker : {"@Sa", "@Ua", "@Ea"}) {
      size_t p = rest.find(marker);
      if (p != llvm::StringRef::npos && p < tag)
        tag = p;
    }
    frag.group = "closure:" + frag.file +
                 (tag == llvm::StringRef::npos ? "" : rest.substr(0, tag + 3).str());
    out.push_back(std::move(frag));
  }
  return out;
}

} // namespace

IdentityTable IdentityTable::build(const CallGraph &graph) {
  IdentityTable t;
  struct Lambda {
    std::string usr;
    LambdaParts parts;
  };
  std::vector<Lambda> lambdas;
  for (const CallGraphNode *n : graph.allNodes()) {
    if (auto parts = parseLambda(n->usr))
      lambdas.push_back({n->usr, std::move(*parts)});
  }
  // Outer lambdas begin before the lambdas nested in them, so processing
  // in (file, line, col) order computes an enclosing lambda's key before
  // its nested ones ask for it.
  std::sort(lambdas.begin(), lambdas.end(), [](const Lambda &a,
                                               const Lambda &b) {
    return std::tie(a.parts.file, a.parts.line, a.parts.col, a.usr) <
           std::tie(b.parts.file, b.parts.line, b.parts.col, b.usr);
  });
  std::map<std::string, std::vector<std::string>> groupUsrs;
  for (const Lambda &l : lambdas) {
    std::string enclosingKey = l.parts.enclosing;
    if (llvm::StringRef(enclosingKey).starts_with("lambda#")) {
      auto it = t.keyByUsr_.find("vycor-lambda:" + enclosingKey);
      if (it != t.keyByUsr_.end())
        enclosingKey = it->second;
    }
    std::string group = l.parts.file + "#" + enclosingKey;
    auto &members = groupUsrs[group];
    std::string key = "vycor-lambda:" + group + "#" +
                      std::to_string(members.size());
    members.push_back(l.usr);
    t.keyByUsr_[l.usr] = key;
    t.usrByKey_[key] = l.usr;
    t.groups_[group].push_back(key);
    t.groupByKey_[key] = group;
    t.groupSizes_[group] = members.size();
  }

  // Byte offsets: every usr the graph mentions — node or edge endpoint —
  // in usr order, so the ordinals and the groups are order-independent.
  std::vector<std::string> mentioned;
  for (const CallGraphNode *n : graph.allNodes()) {
    mentioned.push_back(n->usr);
    for (const CallGraphEdge &e : graph.calleesOf(n->usr))
      mentioned.push_back(e.calleeUsr);
  }
  std::sort(mentioned.begin(), mentioned.end());
  mentioned.erase(std::unique(mentioned.begin(), mentioned.end()),
                  mentioned.end());
  std::map<std::string, std::set<unsigned long long>> offsetsByGroup;
  std::vector<std::pair<std::string, std::vector<LocationFragment>>> located;
  for (const std::string &usr : mentioned) {
    if (t.keyByUsr_.count(usr))
      continue;
    auto frags = locationFragments(usr);
    if (frags.empty())
      continue;
    for (const auto &f : frags)
      offsetsByGroup[f.group].insert(f.offset);
    located.emplace_back(usr, std::move(frags));
  }
  for (auto &[usr, frags] : located) {
    std::string key = usr;
    // Back to front, so earlier fragments' positions stay valid.
    for (auto it = frags.rbegin(); it != frags.rend(); ++it) {
      const auto &offsets = offsetsByGroup[it->group];
      size_t ordinal = static_cast<size_t>(
          std::distance(offsets.begin(), offsets.find(it->offset)));
      key.replace(it->begin, it->end - it->begin,
                  "%" + std::to_string(ordinal));
    }
    t.keyByUsr_[usr] = key;
    if (graph.findNode(usr))
      t.usrByKey_[key] = usr;
    // A usr locating several anonymous types belongs to its first group.
    const std::string &group = frags.front().group;
    t.groups_[group].push_back(key);
    t.groupByKey_[key] = group;
  }
  for (const auto &[group, offsets] : offsetsByGroup)
    t.groupSizes_[group] = offsets.size();

  for (const CallGraphNode *n : graph.allNodes()) {
    if (t.keyByUsr_.count(n->usr))
      continue;
    t.keyByUsr_[n->usr] = n->usr;
    t.usrByKey_[n->usr] = n->usr;
  }
  return t;
}

std::string IdentityTable::groupOf(const std::string &key) const {
  auto it = groupByKey_.find(key);
  return it == groupByKey_.end() ? std::string() : it->second;
}

const std::string &IdentityTable::keyOf(const std::string &usr) const {
  auto it = keyByUsr_.find(usr);
  return it == keyByUsr_.end() ? usr : it->second;
}

std::optional<std::string> IdentityTable::usrOf(const std::string &key) const {
  auto it = usrByKey_.find(key);
  if (it == usrByKey_.end())
    return std::nullopt;
  return it->second;
}

bool IdentityTable::isAmbiguous(const std::string &key) const {
  auto it = ambiguousKeys_.find(key);
  return it != ambiguousKeys_.end() && it->second;
}

void IdentityTable::markAmbiguousGroups(IdentityTable &a, IdentityTable &b) {
  for (const auto &[group, keysA] : a.groups_) {
    auto it = b.groups_.find(group);
    if (it == b.groups_.end() ||
        a.groupSizes_.at(group) == b.groupSizes_.at(group))
      continue;
    for (const auto &k : keysA)
      a.ambiguousKeys_[k] = true;
    for (const auto &k : it->second)
      b.ambiguousKeys_[k] = true;
  }
}

// ---------------------------------------------------------------------------
// Signatures
// ---------------------------------------------------------------------------

std::string contextSignature(const CallSiteContext &ctx) {
  std::string s;
  s += ctx.enclosingTryCatches.empty() ? "protected=0" : "protected=1";
  s += ";handlers=";
  for (size_t i = 0; i < ctx.enclosingTryCatches.size(); ++i) {
    if (i)
      s += "|";
    const auto &scope = ctx.enclosingTryCatches[i];
    for (size_t h = 0; h < scope.handlers.size(); ++h) {
      if (h)
        s += ",";
      const auto &handler = scope.handlers[h];
      s += handler.isCatchAll ? "..." : handler.caughtType;
      if (handler.rethrows)
        s += "!";
    }
  }
  s += ";noexcept=";
  s += noexceptSpecToString(ctx.callerNoexcept);
  s += ctx.insideCatchBlock ? ";inCatch=1" : ";inCatch=0";
  std::vector<std::string> locks;
  for (const auto &local : ctx.liveRaiiLocals)
    if (local.kind == RaiiKind::Lock)
      locks.push_back(local.typeName);
  std::sort(locks.begin(), locks.end());
  s += ";locks=";
  for (size_t i = 0; i < locks.size(); ++i) {
    if (i)
      s += ",";
    s += locks[i];
  }
  s += ";guards=";
  for (size_t i = 0; i < ctx.enclosingGuards.size(); ++i) {
    if (i)
      s += "|";
    const auto &g = ctx.enclosingGuards[i];
    s += g.conditionText;
    s += g.inTrueBranch ? "#T" : "#F";
    if (g.isAssertion)
      s += "A";
  }
  return s;
}

// ---------------------------------------------------------------------------
// ContextSignatures
// ---------------------------------------------------------------------------

size_t ContextSignatures::KeyHash::operator()(const Key &k) const {
  uint64_t h = (uint64_t(k.site) << 32) ^ (uint64_t(k.caller) << 13) ^
               uint64_t(k.callee);
  return std::hash<uint64_t>()(h * 0x9E3779B97F4A7C15ull);
}

// contextForEdge's order: the smallest callee usr, then the TU that
// recorded the context, then the caller spelling; a full tie keeps the
// first recorded.
bool ContextSignatures::beats(const Choice &a, const Choice &b) const {
  if (a.callee != b.callee)
    return strings_.resolve(a.callee) < strings_.resolve(b.callee);
  if (a.tuPath != b.tuPath)
    return strings_.resolve(a.tuPath) < strings_.resolve(b.tuPath);
  if (a.callerName != b.callerName)
    return strings_.resolve(a.callerName) < strings_.resolve(b.callerName);
  return false;
}

void ContextSignatures::offer(Table &table, Key key, const Choice &c) {
  auto [it, fresh] = table.try_emplace(key, c);
  if (!fresh && beats(c, it->second))
    it->second = c;
}

ContextSignatures ContextSignatures::build(const ControlFlowIndex &cf) {
  ContextSignatures out;
  const StringInterner &in = cf.interner();
  // The index's ids map to ours once per distinct string; an unrecorded
  // tuPath is the empty string, as contextForEdge compares it.
  constexpr Id kUnmapped = UINT32_MAX;
  std::vector<Id> remap;
  const Id noTu = out.strings_.intern("");
  auto own = [&](Id id) -> Id {
    if (id == ControlFlowIndex::kNoString)
      return noTu;
    if (id >= remap.size())
      remap.resize(size_t(id) + 1, kUnmapped);
    if (remap[id] == kUnmapped)
      remap[id] = out.strings_.intern(in.resolve(id));
    return remap[id];
  };
  std::map<ControlFlowIndex::ContextShape, uint32_t> shapeIds;
  std::vector<ControlFlowIndex::ContextShape> shapes;
  cf.forEachContextRecord([&](const ControlFlowIndex::ContextRecord &r) {
    ++out.contexts_;
    auto [it, fresh] =
        shapeIds.emplace(r.shape, static_cast<uint32_t>(shapes.size()));
    if (fresh)
      shapes.push_back(r.shape);
    const Choice c{own(r.calleeUsr), own(r.tuPath), own(r.callerName),
                   it->second};
    const Id site = own(r.callSite);
    // A caller matches by usr or by display name (contextForEdge accepts
    // either), so the context is offered under both when they differ.
    const Id byUsr = own(r.callerUsr);
    const Id byName = own(r.callerName);
    const Id callers[2] = {byUsr, byName};
    const int n = byUsr == byName ? 1 : 2;
    for (int i = 0; i < n; ++i)
      out.offer(out.exact_, Key{site, callers[i], c.callee}, c);
  });
  // The per-caller fallback is the least of the per-callee choices: one
  // pass over the table, not one more lookup per context.
  for (const auto &[key, c] : out.exact_)
    out.offer(out.fallback_, Key{key.site, key.caller, kAnyCallee}, c);
  // Materialized outside the walk: contextOfShape takes the index lock.
  out.signatures_.reserve(shapes.size());
  for (const auto &shape : shapes)
    out.signatures_.push_back(contextSignature(cf.contextOfShape(shape)));
  return out;
}

const std::string &
ContextSignatures::forEdge(const std::string &callSite,
                           const std::string &callerUsr,
                           const std::string &calleeUsr) const {
  static const std::string absent = "absent";
  auto site = strings_.find(callSite);
  auto caller = strings_.find(callerUsr);
  if (!site || !caller)
    return absent;
  if (auto callee = strings_.find(calleeUsr)) {
    auto it = exact_.find(Key{*site, *caller, *callee});
    if (it != exact_.end())
      return signatures_[it->second.signature];
  }
  auto it = fallback_.find(Key{*site, *caller, kAnyCallee});
  if (it != fallback_.end())
    return signatures_[it->second.signature];
  return absent;
}

const char *changeKindName(ChangeKind k) {
  switch (k) {
  case ChangeKind::FunctionRemoved: return "function_removed";
  case ChangeKind::FunctionAdded: return "function_added";
  case ChangeKind::CallRemoved: return "call_removed";
  case ChangeKind::CallAdded: return "call_added";
  case ChangeKind::CallChanged: return "call_changed";
  case ChangeKind::ContextChanged: return "context_changed";
  }
  return "unknown";
}

size_t SemanticDiffResult::count(ChangeKind k) const {
  size_t n = 0;
  for (const auto &c : changes)
    if (c.change == k)
      ++n;
  return n;
}

// ---------------------------------------------------------------------------
// Comparability
// ---------------------------------------------------------------------------

namespace {

std::string bakeRef(const SnapshotMeta &meta) {
  if (meta.provenance.environment.empty())
    return "";
  return meta.provenance.environment + "@" +
         std::to_string(meta.provenance.bakeStartNs);
}

SideScope scopeOf(const SnapshotMeta *meta) {
  SideScope s;
  if (!meta)
    return s;
  s.bake = bakeRef(*meta);
  s.coverage = coverageOf(*meta);
  s.analyzer = meta->provenance.analyzer;
  s.toolchain = meta->provenance.toolchain;
  return s;
}

std::string plural(size_t n, const char *noun) {
  return std::to_string(n) + " " + noun + (n == 1 ? "" : "s");
}

std::string listSome(const std::vector<std::string> &v) {
  std::string s;
  for (size_t i = 0; i < v.size() && i < 3; ++i) {
    if (i)
      s += ", ";
    s += v[i];
  }
  if (v.size() > 3)
    s += ", ...";
  return s;
}

} // namespace

Comparability checkComparability(const SnapshotMeta *before,
                                 const SnapshotMeta *after,
                                 bool contextsLoaded) {
  Comparability c;
  c.before = scopeOf(before);
  c.after = scopeOf(after);
  c.contextsCompared = contextsLoaded;
  if (!contextsLoaded)
    c.reasons.push_back("call-site contexts were not compared (control-flow "
                        "section not loaded on both sides)");
  if (!before || !after) {
    c.absenceReliable = false;
    c.reasons.push_back(std::string(!before ? "before" : "after") +
                        ": no bake metadata; coverage unknown");
    return c;
  }
  if (before->collapsePaths != after->collapsePaths) {
    c.configSame = false;
    c.reasons.push_back("collapse paths differ");
  }
  if (before->lockAllowlist != after->lockAllowlist ||
      before->lockBuiltins != after->lockBuiltins) {
    c.configSame = false;
    c.reasons.push_back("lock type configuration differs");
  }
  if (before->channelTypes != after->channelTypes) {
    c.configSame = false;
    c.reasons.push_back("channel type registrations differ");
  }
  if (!c.configSame)
    c.comparable = false;
  if (before->provenance.analyzer != after->provenance.analyzer) {
    c.analyzerSame = false;
    c.reasons.push_back("analyzer differs (" + before->provenance.analyzer +
                        " vs " + after->provenance.analyzer +
                        "): the model itself may differ");
  }
  if (before->provenance.toolchain != after->provenance.toolchain) {
    c.toolchainSame = false;
    c.reasons.push_back("toolchain differs (" + before->provenance.toolchain +
                        " vs " + after->provenance.toolchain +
                        "): the frontend may resolve differently");
  }

  std::set<std::string> filesBefore, filesAfter;
  for (const auto &f : before->files)
    filesBefore.insert(f.path);
  for (const auto &f : after->files)
    filesAfter.insert(f.path);
  std::set_difference(filesBefore.begin(), filesBefore.end(),
                      filesAfter.begin(), filesAfter.end(),
                      std::back_inserter(c.tusOnlyBefore));
  std::set_difference(filesAfter.begin(), filesAfter.end(),
                      filesBefore.begin(), filesBefore.end(),
                      std::back_inserter(c.tusOnlyAfter));
  auto classify = [](const SnapshotMeta &meta, std::vector<std::string> &failed,
                     std::vector<std::string> &partial) {
    for (const auto &[path, outcome] : SnapshotIO::outcomesOf(meta)) {
      if (outcome.status == TuStatus::Partial)
        partial.push_back(path);
      else if (outcome.status != TuStatus::Indexed)
        failed.push_back(path);
    }
    std::sort(failed.begin(), failed.end());
    std::sort(partial.begin(), partial.end());
  };
  classify(*before, c.failedBefore, c.partialBefore);
  classify(*after, c.failedAfter, c.partialAfter);

  if (!c.tusOnlyBefore.empty()) {
    c.absenceReliable = false;
    c.reasons.push_back("before requested " +
                        plural(c.tusOnlyBefore.size(), "TU") +
                        " the after index does not (" +
                        listSome(c.tusOnlyBefore) + ")");
  }
  if (!c.tusOnlyAfter.empty()) {
    c.absenceReliable = false;
    c.reasons.push_back("after requested " +
                        plural(c.tusOnlyAfter.size(), "TU") +
                        " the before index does not (" +
                        listSome(c.tusOnlyAfter) + ")");
  }
  for (const char *side : {"before", "after"}) {
    const bool isBefore = llvm::StringRef(side) == "before";
    const auto &failed = isBefore ? c.failedBefore : c.failedAfter;
    const auto &partial = isBefore ? c.partialBefore : c.partialAfter;
    if (!failed.empty()) {
      c.absenceReliable = false;
      c.reasons.push_back(std::string(side) + ": " +
                          plural(failed.size(), "requested TU") +
                          " failed (" + listSome(failed) + ")");
    }
    if (!partial.empty()) {
      c.absenceReliable = false;
      c.reasons.push_back(std::string(side) + ": " +
                          plural(partial.size(), "requested TU") +
                          " parsed partially (" + listSome(partial) + ")");
    }
  }
  return c;
}

// ---------------------------------------------------------------------------
// The diff
// ---------------------------------------------------------------------------

namespace {

struct RelKey {
  std::string caller, callee;
  EdgeKind kind;
  bool operator<(const RelKey &o) const {
    return std::tie(caller, callee, kind) < std::tie(o.caller, o.callee, o.kind);
  }
};

struct RelValue {
  std::string callerName, calleeName;
  RelationshipSide side;
};

using RelMap = std::map<RelKey, RelValue>;
using FnMap = std::map<std::string, FunctionFacts>;

std::string attributeOf(const SiteWitness &s) {
  return std::string(confidenceToString(s.confidence)) + "/" +
         executionContextToString(s.execContext) + "/" +
         std::to_string(s.indirectionDepth);
}

bool siteLess(const SiteWitness &a, const SiteWitness &b) {
  return std::tie(a.callSite, a.confidence, a.execContext,
                  a.indirectionDepth) <
         std::tie(b.callSite, b.confidence, b.execContext,
                  b.indirectionDepth);
}

std::vector<const CallGraphNode *> nodesByUsr(const CallGraph &g) {
  auto nodes = g.allNodes();
  std::sort(nodes.begin(), nodes.end(),
            [](const CallGraphNode *a, const CallGraphNode *b) {
              return a->usr < b->usr;
            });
  return nodes;
}

// Every node and every relationship of one side, keyed for comparison.
// Ambiguous identities are withheld and counted per group.
void collectSide(const DiffSide &side, const IdentityTable &identity,
                 const ContextSignatures *contexts, FnMap &functions,
                 RelMap &relationships,
                 std::map<std::string, size_t> &withheld) {
  for (const CallGraphNode *n : nodesByUsr(side.graph)) {
    const std::string &key = identity.keyOf(n->usr);
    if (identity.isAmbiguous(key))
      continue;
    FunctionFacts f;
    f.key = key;
    f.usr = n->usr;
    f.name = n->qualifiedName;
    f.file = n->file;
    f.line = n->line;
    f.isEntryPoint = n->isEntryPoint;
    f.isVirtual = n->isVirtual;
    f.enclosingClass = n->enclosingClass;
    functions.emplace(key, std::move(f));
  }
  auto groupOf = [&](const std::string &key) {
    return identity.groupOf(key);
  };
  for (const CallGraphNode *n : nodesByUsr(side.graph)) {
    for (const CallGraphEdge &e : side.graph.calleesOf(n->usr)) {
      const std::string &callerKey = identity.keyOf(e.callerUsr);
      const std::string &calleeKey = identity.keyOf(e.calleeUsr);
      if (identity.isAmbiguous(callerKey)) {
        ++withheld[groupOf(callerKey)];
        continue;
      }
      if (identity.isAmbiguous(calleeKey)) {
        ++withheld[groupOf(calleeKey)];
        continue;
      }
      SiteWitness w;
      w.callSite = e.callSite;
      w.confidence = e.confidence;
      w.execContext = e.execContext;
      w.indirectionDepth = e.indirectionDepth;
      if (contexts)
        w.signature =
            contexts->forEdge(e.callSite, e.callerUsr, e.calleeUsr);
      RelValue &v = relationships[RelKey{callerKey, calleeKey, e.kind}];
      if (v.callerName.empty()) {
        v.callerName = e.callerName;
        v.calleeName = e.calleeName;
      }
      v.side.sites.push_back(std::move(w));
    }
  }
  for (auto &[key, v] : relationships) {
    auto &s = v.side;
    std::sort(s.sites.begin(), s.sites.end(), siteLess);
    for (const auto &w : s.sites) {
      s.attributes.push_back(attributeOf(w));
      if (contexts)
        s.signatures.push_back(w.signature);
    }
    std::sort(s.attributes.begin(), s.attributes.end());
    std::sort(s.signatures.begin(), s.signatures.end());
  }
}

llvm::StringRef stripDot(llvm::StringRef p) {
  while (p.starts_with("./"))
    p = p.drop_front(2);
  return p;
}

// A node's file (as the compile command spelled it) names the TU
// `tuPath` (absolute) when equal or when one is the other's path suffix
// at a component boundary.
bool fileNamesTu(llvm::StringRef file, llvm::StringRef tuPath) {
  file = stripDot(file);
  if (file.empty())
    return false;
  if (file == tuPath)
    return true;
  if (tuPath.size() > file.size() && tuPath.ends_with(file) &&
      tuPath[tuPath.size() - file.size() - 1] == '/')
    return true;
  return file.size() > tuPath.size() && file.ends_with(tuPath) &&
         file[file.size() - tuPath.size() - 1] == '/';
}

std::string explainAbsence(const std::string &file,
                           const std::vector<std::string> &absent,
                           const std::vector<std::string> &failed,
                           const std::vector<std::string> &partial,
                           const char *suffix) {
  for (const auto &tu : absent)
    if (fileNamesTu(file, tu))
      return std::string("tu_absent_") + suffix;
  for (const auto &tu : failed)
    if (fileNamesTu(file, tu))
      return std::string("tu_failed_") + suffix;
  for (const auto &tu : partial)
    if (fileNamesTu(file, tu))
      return std::string("tu_partial_") + suffix;
  return "";
}

void trimSites(RelationshipSide &s, size_t maxSites) {
  if (maxSites && s.sites.size() > maxSites)
    s.sites.resize(maxSites);
}

} // namespace

SemanticDiffResult semanticDiff(const DiffSide &before, const DiffSide &after,
                                const SemanticDiffOptions &opts,
                                IdentityTable *beforeIdentity,
                                IdentityTable *afterIdentity) {
  SemanticDiffResult r;
  const bool contexts = opts.compareContexts &&
                        (before.signatures || before.cfIndex) &&
                        (after.signatures || after.cfIndex);
  r.comparability = checkComparability(before.meta, after.meta, contexts);
  if (!r.comparability.comparable && !opts.allowMismatch) {
    r.refused = true;
    return r;
  }
  std::optional<ContextSignatures> localSigB, localSigA;
  const ContextSignatures *sigB = nullptr, *sigA = nullptr;
  if (contexts) {
    sigB = before.signatures;
    if (!sigB)
      sigB = &localSigB.emplace(ContextSignatures::build(*before.cfIndex));
    sigA = after.signatures;
    if (!sigA)
      sigA = &localSigA.emplace(ContextSignatures::build(*after.cfIndex));
  }

  IdentityTable localBefore, localAfter;
  IdentityTable &idB = beforeIdentity ? *beforeIdentity : localBefore;
  IdentityTable &idA = afterIdentity ? *afterIdentity : localAfter;
  idB = IdentityTable::build(before.graph);
  idA = IdentityTable::build(after.graph);
  IdentityTable::markAmbiguousGroups(idB, idA);

  FnMap fnB, fnA;
  RelMap relB, relA;
  std::map<std::string, size_t> withheldB, withheldA;
  collectSide(before, idB, sigB, fnB, relB, withheldB);
  collectSide(after, idA, sigA, fnA, relA, withheldA);
  r.functionsBefore = fnB.size();
  r.functionsAfter = fnA.size();
  r.relationshipsBefore = relB.size();
  r.relationshipsAfter = relA.size();

  // Ambiguous lambda groups: listed with what was withheld on each side.
  for (const auto &[group, keysB] : idB.groups()) {
    if (keysB.empty() || !idB.isAmbiguous(keysB.front()))
      continue;
    AmbiguousIdentity a;
    a.group = group;
    for (const auto &k : keysB)
      if (auto usr = idB.usrOf(k))
        a.beforeUsrs.push_back(*usr);
    auto itA = idA.groups().find(group);
    if (itA != idA.groups().end())
      for (const auto &k : itA->second)
        if (auto usr = idA.usrOf(k))
          a.afterUsrs.push_back(*usr);
    a.edgesWithheldBefore = withheldB[group];
    a.edgesWithheldAfter = withheldA[group];
    r.ambiguous.push_back(std::move(a));
  }

  // Functions.
  const Comparability &c = r.comparability;
  std::map<std::string, std::vector<const FunctionFacts *>> removedByName,
      addedByName;
  {
    auto b = fnB.begin(), a = fnA.begin();
    while (b != fnB.end() || a != fnA.end()) {
      int cmp = b == fnB.end() ? 1 : a == fnA.end() ? -1
                : b->first < a->first ? -1 : a->first < b->first ? 1 : 0;
      if (cmp < 0) {
        ChangeRecord rec;
        rec.change = ChangeKind::FunctionRemoved;
        rec.callerKey = b->first;
        rec.function = b->second;
        rec.explanation =
            explainAbsence(b->second.file, c.tusOnlyBefore, c.failedAfter,
                           c.partialAfter, "after");
        removedByName[b->second.name].push_back(&b->second);
        r.changes.push_back(std::move(rec));
        ++b;
      } else if (cmp > 0) {
        ChangeRecord rec;
        rec.change = ChangeKind::FunctionAdded;
        rec.callerKey = a->first;
        rec.function = a->second;
        rec.explanation =
            explainAbsence(a->second.file, c.tusOnlyAfter, c.failedBefore,
                           c.partialBefore, "before");
        addedByName[a->second.name].push_back(&a->second);
        r.changes.push_back(std::move(rec));
        ++a;
      } else {
        if (b->second.file != a->second.file ||
            b->second.line != a->second.line) {
          ++r.movedFunctions;
          if (opts.includeMoves)
            r.moves.push_back({b->first, a->second.name, b->second.file,
                               a->second.file, b->second.line,
                               a->second.line});
        }
        ++b;
        ++a;
      }
    }
  }
  for (const auto &[name, removed] : removedByName) {
    auto it = addedByName.find(name);
    if (it == addedByName.end())
      continue;
    for (const FunctionFacts *rf : removed)
      for (const FunctionFacts *af : it->second)
        r.renameCandidates.push_back(
            {name, rf->key, af->key, rf->file, af->file});
  }
  std::sort(r.renameCandidates.begin(), r.renameCandidates.end(),
            [](const RenameCandidate &x, const RenameCandidate &y) {
              return std::tie(x.beforeKey, x.afterKey) <
                     std::tie(y.beforeKey, y.afterKey);
            });

  // Relationships.
  {
    auto b = relB.begin(), a = relA.begin();
    while (b != relB.end() || a != relA.end()) {
      int cmp = b == relB.end() ? 1 : a == relA.end() ? -1
                : b->first < a->first ? -1 : a->first < b->first ? 1 : 0;
      ChangeRecord rec;
      const RelKey &key = cmp <= 0 ? b->first : a->first;
      rec.callerKey = key.caller;
      rec.calleeKey = key.callee;
      rec.kind = key.kind;
      if (cmp < 0) {
        rec.change = ChangeKind::CallRemoved;
        rec.callerName = b->second.callerName;
        rec.calleeName = b->second.calleeName;
        rec.before = b->second.side;
        trimSites(*rec.before, opts.maxSites);
        r.changes.push_back(std::move(rec));
        ++b;
      } else if (cmp > 0) {
        rec.change = ChangeKind::CallAdded;
        rec.callerName = a->second.callerName;
        rec.calleeName = a->second.calleeName;
        rec.after = a->second.side;
        trimSites(*rec.after, opts.maxSites);
        r.changes.push_back(std::move(rec));
        ++a;
      } else {
        const RelationshipSide &sb = b->second.side;
        const RelationshipSide &sa = a->second.side;
        rec.callerName = a->second.callerName;
        rec.calleeName = a->second.calleeName;
        if (sb.attributes != sa.attributes) {
          ChangeRecord changed = rec;
          changed.change = ChangeKind::CallChanged;
          changed.before = sb;
          changed.after = sa;
          trimSites(*changed.before, opts.maxSites);
          trimSites(*changed.after, opts.maxSites);
          r.changes.push_back(std::move(changed));
        }
        if (contexts && sb.signatures != sa.signatures) {
          rec.change = ChangeKind::ContextChanged;
          rec.before = sb;
          rec.after = sa;
          trimSites(*rec.before, opts.maxSites);
          trimSites(*rec.after, opts.maxSites);
          r.changes.push_back(std::move(rec));
        }
        ++b;
        ++a;
      }
    }
  }

  std::stable_sort(r.changes.begin(), r.changes.end(),
                   [](const ChangeRecord &x, const ChangeRecord &y) {
                     return std::tie(x.change, x.callerKey, x.calleeKey,
                                     x.kind) <
                            std::tie(y.change, y.callerKey, y.calleeKey,
                                     y.kind);
                   });
  return r;
}

std::vector<std::string> changedFunctionsAfter(const SemanticDiffResult &diff,
                                               const IdentityTable &after) {
  std::vector<std::string> out;
  for (const auto &c : diff.changes) {
    switch (c.change) {
    case ChangeKind::FunctionAdded:
      out.push_back(c.function.usr);
      break;
    case ChangeKind::FunctionRemoved:
      break;
    default:
      if (auto usr = after.usrOf(c.callerKey))
        out.push_back(*usr);
      break;
    }
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

// ---------------------------------------------------------------------------
// Routes
// ---------------------------------------------------------------------------

namespace {

std::string routeKey(const CallPath &p, const IdentityTable &identity) {
  std::string k;
  for (const auto &hop : p.hops) {
    k += identity.keyOf(hop.callerUsr);
    k += ">";
    k += identity.keyOf(hop.calleeUsr);
    k += "#";
    k += edgeKindToString(hop.kind);
    k += ";";
  }
  return k;
}

} // namespace

RouteDiff diffRoutes(const DiffSide &before, const DiffSide &after,
                     const IdentityTable &beforeIdentity,
                     const IdentityTable &afterIdentity,
                     const std::string &target,
                     const std::vector<std::string> &beforeStarts,
                     const std::vector<std::string> &afterStarts,
                     const SearchLimits &limits) {
  RouteDiff r;
  r.before = findCallerPaths(before.graph, target, beforeStarts, limits);
  r.after = findCallerPaths(after.graph, target, afterStarts, limits);
  std::map<std::string, size_t> beforeCounts;
  for (const auto &p : r.before.paths)
    ++beforeCounts[routeKey(p, beforeIdentity)];
  std::map<std::string, size_t> matched;
  for (const auto &p : r.after.paths) {
    std::string k = routeKey(p, afterIdentity);
    auto it = beforeCounts.find(k);
    if (it != beforeCounts.end() && it->second > 0) {
      --it->second;
      ++matched[k];
      ++r.unchanged;
    } else {
      r.added.push_back(p);
    }
  }
  for (const auto &p : r.before.paths) {
    std::string k = routeKey(p, beforeIdentity);
    auto it = matched.find(k);
    if (it != matched.end() && it->second > 0) {
      --it->second;
      continue;
    }
    r.removed.push_back(p);
  }
  r.complete = r.before.complete() && r.after.complete();
  r.exhaustive = r.before.exhaustive() && r.after.exhaustive();
  return r;
}

} // namespace vycor
