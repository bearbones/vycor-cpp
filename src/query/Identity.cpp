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


#include "vycor/query/Identity.h"

#include "llvm/ADT/StringExtras.h"

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace vycor {

// ============================================================================
// Name ranking and suggestions (search_functions, not-found identities)
// ============================================================================

std::vector<const CallGraphNode *> rankFunctionMatches(const ToolContext &ctx,
                                                       llvm::StringRef query) {
  std::string needle = query.lower();

  // Lowercased name index, built once per graph state and cached — the
  // per-query lowering of every node name was 14 ms on a 57k-node graph.
  // Node pointers are stable (nodes live in a node-based map) and the
  // cache is cleared whenever the graph mutates.
  struct SearchEntry {
    std::string lowerQualified;
    size_t unqualifiedOffset; // offset of the unqualified name within it
    const CallGraphNode *node;
  };
  using SearchIndex = std::vector<SearchEntry>;
  std::shared_ptr<const SearchIndex> index;
  if (ctx.cache) {
    auto it = ctx.cache->objects.find("search_index");
    if (it != ctx.cache->objects.end())
      index = std::static_pointer_cast<const SearchIndex>(it->second);
  }
  if (!index) {
    auto built = std::make_shared<SearchIndex>();
    auto nodes = ctx.graph.allNodes();
    built->reserve(nodes.size());
    for (auto *node : nodes) {
      llvm::StringRef qn(node->qualifiedName);
      size_t off = 0;
      auto sep = qn.rfind("::");
      if (sep != llvm::StringRef::npos)
        off = sep + 2;
      built->push_back({qn.lower(), off, node});
    }
    if (ctx.cache)
      ctx.cache->objects["search_index"] = built;
    index = std::move(built);
  }

  // Rank: exact name match, then prefix of the unqualified name, then any
  // substring. Within a tier, shorter qualified names first (closer match).
  struct Hit {
    const CallGraphNode *node;
    int tier;
  };
  std::vector<Hit> hits;
  for (const auto &entry : *index) {
    llvm::StringRef lower(entry.lowerQualified);
    if (lower.find(needle) == llvm::StringRef::npos)
      continue;
    int tier = 2;
    llvm::StringRef unqLower = lower.substr(entry.unqualifiedOffset);
    if (unqLower == needle)
      tier = 0;
    else if (unqLower.starts_with(needle))
      tier = 1;
    hits.push_back({entry.node, tier});
  }

  std::sort(hits.begin(), hits.end(), [](const Hit &a, const Hit &b) {
    if (a.tier != b.tier)
      return a.tier < b.tier;
    if (a.node->qualifiedName.size() != b.node->qualifiedName.size())
      return a.node->qualifiedName.size() < b.node->qualifiedName.size();
    if (a.node->qualifiedName != b.node->qualifiedName)
      return a.node->qualifiedName < b.node->qualifiedName;
    // Overloads share a name: the usr decides, so a `limit` cut falls
    // in the same place whatever order the nodes were indexed in.
    return a.node->usr < b.node->usr;
  });

  std::vector<const CallGraphNode *> out;
  out.reserve(hits.size());
  for (const auto &h : hits)
    out.push_back(h.node);
  return out;
}

static llvm::StringRef unqualifiedName(llvm::StringRef qualified) {
  auto sep = qualified.rfind("::");
  return sep == llvm::StringRef::npos ? qualified : qualified.substr(sep + 2);
}

// Case-insensitive optimal-string-alignment distance: Levenshtein plus
// the transposition of two adjacent characters as one edit ("mian" is one
// edit from "main", two under plain Levenshtein). Returns bound + 1 as
// soon as every alignment exceeds `bound`.
static unsigned typoDistance(llvm::StringRef a, llvm::StringRef b,
                             unsigned bound) {
  const size_t n = a.size(), m = b.size();
  std::vector<unsigned> prev2(m + 1), prev(m + 1), cur(m + 1);
  for (size_t j = 0; j <= m; ++j)
    prev[j] = static_cast<unsigned>(j);
  for (size_t i = 1; i <= n; ++i) {
    cur[0] = static_cast<unsigned>(i);
    unsigned rowMin = cur[0];
    const char ai = llvm::toLower(a[i - 1]);
    for (size_t j = 1; j <= m; ++j) {
      const char bj = llvm::toLower(b[j - 1]);
      unsigned d = std::min({prev[j] + 1, cur[j - 1] + 1,
                             prev[j - 1] + (ai == bj ? 0u : 1u)});
      if (i > 1 && j > 1 && ai == llvm::toLower(b[j - 2]) &&
          llvm::toLower(a[i - 2]) == bj)
        d = std::min(d, prev2[j - 2] + 1);
      cur[j] = d;
      rowMin = std::min(rowMin, d);
    }
    if (rowMin > bound)
      return bound + 1;
    std::swap(prev2, prev);
    std::swap(prev, cur);
  }
  return prev[m];
}

std::vector<const CallGraphNode *>
suggestFunctions(const ToolContext &ctx, llvm::StringRef name, size_t max) {
  std::vector<const CallGraphNode *> out;
  std::set<const CallGraphNode *> seen;
  auto take = [&](const std::vector<const CallGraphNode *> &ranked) {
    for (const auto *node : ranked) {
      if (out.size() >= max)
        return;
      if (seen.insert(node).second)
        out.push_back(node);
    }
  };
  if (name.empty() || max == 0)
    return out;
  // 1. The search_functions ranking of what was typed (a partial name).
  take(rankFunctionMatches(ctx, name));
  // 2. The same ranking of its unqualified tail (a wrong or missing
  //    namespace/class qualifier).
  llvm::StringRef tail = unqualifiedName(name);
  if (out.size() < max && tail.size() != name.size() && !tail.empty())
    take(rankFunctionMatches(ctx, tail));
  if (out.size() >= max)
    return out;

  // 3. Typos: unqualified names within an edit distance (transpositions
  //    count once) of about a third of the typed tail (at least 1, at
  //    most 3), nearest first; ties by
  //    the search ranking's (qualified-name length, name, usr).
  const unsigned bound = std::clamp<unsigned>(
      static_cast<unsigned>(tail.size() / 3), 1u, 3u);
  struct Near {
    unsigned distance;
    const CallGraphNode *node;
  };
  std::vector<Near> near;
  for (const auto *node : ctx.graph.allNodes()) {
    if (seen.count(node))
      continue;
    llvm::StringRef unq = unqualifiedName(node->qualifiedName);
    // Length difference is a lower bound on the distance: skip cheaply.
    size_t diff = unq.size() > tail.size() ? unq.size() - tail.size()
                                           : tail.size() - unq.size();
    if (diff > bound)
      continue;
    unsigned d = typoDistance(tail, unq, bound);
    if (d <= bound)
      near.push_back({d, node});
  }
  std::sort(near.begin(), near.end(), [](const Near &a, const Near &b) {
    if (a.distance != b.distance)
      return a.distance < b.distance;
    if (a.node->qualifiedName.size() != b.node->qualifiedName.size())
      return a.node->qualifiedName.size() < b.node->qualifiedName.size();
    if (a.node->qualifiedName != b.node->qualifiedName)
      return a.node->qualifiedName < b.node->qualifiedName;
    return a.node->usr < b.node->usr;
  });
  for (const auto &n : near) {
    if (out.size() >= max)
      break;
    out.push_back(n.node);
  }
  return out;
}

bool isKnownIdentity(const ToolContext &ctx, const std::string &ident) {
  if (ctx.graph.findNode(ident))
    return true;
  // An endpoint without a node (external or unresolved callee): usr and
  // display name coincide, and some edge names it.
  auto id = ctx.graph.interner().find(ident);
  if (!id)
    return false;
  return !ctx.graph.callerRefsOf(*id).empty() ||
         !ctx.graph.calleeRefsOf(*id).empty();
}

llvm::json::Value unknownFunctionResult(const ToolContext &ctx,
                                        llvm::StringRef paramName,
                                        llvm::StringRef name) {
  auto suggestions = suggestFunctions(ctx, name);
  std::string message = "Function not found: '" + name.str() +
                        "' names no function and no call-graph edge in "
                        "this index";
  if (!suggestions.empty())
    message += " (did you mean '" + suggestions.front()->qualifiedName + "'?)";
  message += ". Use search_functions to find the indexed name.";
  auto payload = notFoundError(message);
  auto *obj = payload.getAsObject();
  (*obj)["parameter"] = paramName.str();
  (*obj)["name"] = name.str();
  llvm::json::Array didYouMean;
  for (const auto *node : suggestions) {
    llvm::json::Object s;
    s["qualifiedName"] = node->qualifiedName;
    s["usr"] = node->usr;
    s["file"] = node->file;
    s["line"] = static_cast<int64_t>(node->line);
    didYouMean.push_back(llvm::json::Value(std::move(s)));
  }
  (*obj)["didYouMean"] = std::move(didYouMean);
  return payload;
}

llvm::StringRef identityAlias(llvm::StringRef nameParam) {
  if (nameParam == "name")
    return "function";
  if (nameParam == "function")
    return "name";
  return "";
}

std::optional<llvm::StringRef> identityName(const llvm::json::Object &args,
                                            llvm::StringRef nameParam) {
  if (auto v = args.getString(nameParam))
    return v;
  llvm::StringRef alias = identityAlias(nameParam);
  if (!alias.empty())
    return args.getString(alias);
  return std::nullopt;
}

// ============================================================================
// F8 identity resolution (docs/design-f8-usr-identity.md §4, PR C)
// ============================================================================

// Candidate-list cap for disambiguation responses. Generic library
// utilities can have hundreds of instantiations under one display name
// (llvm::cast: 858 on the 938-TU testbed ≈ 188 KB uncapped — see the
// review doc §"Template node growth"); above the cap the response keeps a
// deterministic prefix plus a by-file group summary and refinement hints.
// Non-error disambiguation response for an ambiguous display name: the
// client picks a candidate and re-queries with its `usr`, or refines with
// the `site`/`filter` parameters named in the note. Candidates are sorted
// by usr string so the response is deterministic.
llvm::json::Value makeAmbiguousNameResult(const ToolContext &ctx,
                                                 llvm::StringRef paramName,
                                                 llvm::StringRef name,
                                                 std::vector<std::string> usrs,
                                                 llvm::StringRef filterNote) {
  std::sort(usrs.begin(), usrs.end());
  const size_t total = usrs.size();
  const bool truncated = total > kMaxAmbiguousCandidates;

  // By-file group summary over ALL candidates (cheap discrimination when
  // the list is long: overloads and statics often split by file even when
  // every candidate prints the same display name).
  std::map<std::string, int64_t> byFile;
  llvm::json::Array candidates;
  size_t emitted = 0;
  for (const auto &usr : usrs) {
    const auto *node = ctx.graph.findNode(usr);
    if (node)
      ++byFile[node->file];
    if (emitted >= kMaxAmbiguousCandidates)
      continue;
    ++emitted;
    llvm::json::Object cand;
    cand["usr"] = usr;
    // Nodes are usr-keyed; findNode(usr) is the exact node.
    if (node) {
      cand["qualifiedName"] = node->qualifiedName;
      cand["file"] = node->file;
      cand["line"] = static_cast<int64_t>(node->line);
    } else {
      cand["qualifiedName"] = name.str();
    }
    candidates.push_back(llvm::json::Value(std::move(cand)));
  }
  llvm::json::Object obj;
  obj["ambiguous"] = true;
  obj["parameter"] = paramName.str();
  obj["name"] = name.str();
  obj["total_candidates"] = static_cast<int64_t>(total);
  obj["candidates"] = std::move(candidates);
  if (truncated) {
    obj["truncated"] = true;
    llvm::json::Array files;
    for (const auto &[file, count] : byFile) {
      llvm::json::Object f;
      f["file"] = file;
      f["count"] = count;
      files.push_back(llvm::json::Value(std::move(f)));
    }
    obj["candidates_by_file"] = std::move(files);
  }
  std::string note =
      "Multiple functions share this name. Re-run with the 'usr' parameter "
      "of the intended candidate, or narrow with 'site' (a call-site "
      "'file:line:col' resolves to the exact overload/instantiation called "
      "there) or 'filter' (a literal substring matched against candidate "
      "usr/name/file — not type-aware).";
  if (truncated)
    note += " Candidate list truncated to " +
            std::to_string(kMaxAmbiguousCandidates) + " of " +
            std::to_string(total) + ".";
  if (!filterNote.empty())
    note += " " + filterNote.str();
  obj["note"] = std::move(note);
  return llvm::json::Value(std::move(obj));
}

// Companion parameter name for an identity's usr parameter: "usr" ->
// "site"/"filter", "to_usr" -> "to_site"/"to_filter", "fn_a_usr" ->
// "fn_a_site"/"fn_a_filter". Keeps multi-identity tools (find_call_chain,
// query_same_lock) unambiguous about which identity a refinement applies to.
std::string companionParam(llvm::StringRef usrParam,
                                  llvm::StringRef suffix) {
  llvm::StringRef prefix = usrParam;
  prefix.consume_back("usr");
  return (prefix + suffix).str();
}

// Resolves an identity parameter per the F8 disambiguation contract:
//   1. `usrParam` present -> that string verbatim (no name lookup).
//   2. site parameter ("site"/"to_site"/...) present -> the calleeUsr of the
//      call-site context at that 'file:line:col' spelling (the stored edge
//      set already maps every call site to the exact overload/instantiation
//      called there — no name guessing). A macro-shared spelling with
//      several distinct callees returns the small disambiguation list; a
//      name given alongside must agree with the site's callee or the
//      response says so.
//   3. `nameParam` resolving to 1 USR -> that USR. A name (or usr) that
//      names no node resolves to itself only when some edge names it (an
//      external or unresolved callee known by name alone; attachUsr then
//      marks the response `resolvedAs: "name"`). Otherwise nullopt with
//      `ambiguous` set to the `not_found` payload (unknownFunctionResult,
//      with `didYouMean` suggestions): an unknown name must not answer
//      `ok` with an empty list, which reads as "nothing calls this".
//   4. N >= 2 USRs: a filter parameter ("filter"/"to_filter"/...) — a
//      literal substring matched against candidate usr, display name, and
//      file (deliberately NOT type-aware: USRs encode types in Clang's own
//      grammar, so matching agent-guessed C++ type spellings would be false
//      precision) — narrows the set first; a unique survivor resolves.
//      Otherwise nullopt with `ambiguous` set to the NON-error
//      disambiguation response the handler must return. Never a silent
//      union; never a hard error.
// Returns nullopt with `ambiguous` UNSET when no identity parameter is
// present; the handler emits its own missing-parameter error.
std::optional<std::string>
resolveIdentity(const llvm::json::Object &args, const ToolContext &ctx,
                llvm::StringRef nameParam, llvm::StringRef usrParam,
                std::optional<llvm::json::Value> &ambiguous) {
  if (auto usr = args.getString(usrParam)) {
    if (isKnownIdentity(ctx, usr->str()))
      return usr->str();
    ambiguous = unknownFunctionResult(ctx, usrParam, *usr);
    return std::nullopt;
  }
  // `name` and `function` are aliases (docs/result-contract.md).
  llvm::StringRef nameKey = nameParam;
  auto name = args.getString(nameParam);
  if (!name) {
    llvm::StringRef alias = identityAlias(nameParam);
    if (!alias.empty() && (name = args.getString(alias)))
      nameKey = alias;
  }

  if (auto site = args.getString(companionParam(usrParam, "site"))) {
    auto contexts = ctx.cfIndex.contextsAtSite(site->str());
    // Distinct callees at this spelling (macro expansion can stack several
    // contexts on one file:line:col; usually they call the same function).
    std::set<std::string> callees;
    for (const auto &c : contexts)
      callees.insert(c.calleeUsr.empty() ? c.calleeName : c.calleeUsr);
    if (callees.empty()) {
      ambiguous = notFoundError(
          "No call site found at '" + site->str() +
          "' (expected 'file:line:col' as spelled in the compile command; "
          "use query_call_site_context to inspect a site).");
      return std::nullopt;
    }
    // A name given alongside must agree with the site's callee.
    if (name) {
      std::set<std::string> named;
      for (auto &u : ctx.graph.usrsForName(name->str()))
        named.insert(std::move(u));
      named.insert(name->str());
      std::set<std::string> agreeing;
      for (const auto &c : callees)
        if (named.count(c))
          agreeing.insert(c);
      if (agreeing.empty()) {
        ambiguous = notFoundError(
            "Call site '" + site->str() + "' does not call '" +
            name->str() + "' (it calls: " + *callees.begin() +
            (callees.size() > 1 ? ", ..." : "") + ").");
        return std::nullopt;
      }
      callees = std::move(agreeing);
    }
    if (callees.size() == 1)
      return *callees.begin();
    ambiguous = makeAmbiguousNameResult(
        ctx, usrParam, name ? *name : llvm::StringRef(site->str()),
        std::vector<std::string>(callees.begin(), callees.end()),
        "Several distinct functions are called at this spelling "
        "(macro-expanded call site).");
    return std::nullopt;
  }

  if (!name)
    return std::nullopt;
  auto usrs = ctx.graph.usrsForName(name->str());
  if (usrs.size() >= 2) {
    std::string filterNote;
    if (auto filter = args.getString(companionParam(usrParam, "filter"))) {
      std::vector<std::string> kept;
      for (auto &u : usrs) {
        const auto *node = ctx.graph.findNode(u);
        if (llvm::StringRef(u).contains(*filter) ||
            (node && (llvm::StringRef(node->qualifiedName).contains(*filter) ||
                      llvm::StringRef(node->file).contains(*filter))))
          kept.push_back(std::move(u));
      }
      if (kept.size() == 1)
        return std::move(kept.front());
      if (kept.empty())
        filterNote = "The filter '" + filter->str() +
                     "' matched no candidate; showing the unfiltered set.";
      else {
        filterNote = "Candidates already narrowed by filter '" +
                     filter->str() + "'.";
        usrs = std::move(kept);
      }
    }
    ambiguous = makeAmbiguousNameResult(ctx, nameKey, *name,
                                        std::move(usrs), filterNote);
    return std::nullopt;
  }
  // One candidate: a node, or the string itself when it is interned but
  // names no node (usrsForName's unregistered-endpoint fallback, which
  // also covers strings that are no endpoint at all: a file path, a call
  // site). Zero: never interned.
  std::string resolved = usrs.empty() ? name->str() : std::move(usrs.front());
  if (isKnownIdentity(ctx, resolved))
    return resolved;
  ambiguous = unknownFunctionResult(ctx, nameKey, *name);
  return std::nullopt;
}

// Adds the resolved node usr to a response object when the identity names a
// registered node. An edge endpoint without a node (its identity string,
// usually a USR, is all the index holds) has no node to cite; the
// response marks it `resolvedAs: "name"` under the key's own prefix.
void attachUsr(llvm::json::Object &obj, const ToolContext &ctx,
                      const std::string &ident,
                      llvm::StringRef key) {
  if (const auto *node = ctx.graph.findNode(ident)) {
    obj[key] = node->usr;
    return;
  }
  llvm::StringRef prefix = key;
  std::string marker;
  if (prefix.consume_back("Usr"))
    marker = (prefix + "ResolvedAs").str();
  else if (prefix.consume_back("usr"))
    marker = (prefix + "resolvedAs").str();
  else
    marker = (key + "ResolvedAs").str();
  obj[marker] = "name";
}

} // namespace vycor
