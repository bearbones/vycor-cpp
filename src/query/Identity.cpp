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

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace vycor {

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
//   3. `nameParam` resolving to 0 or 1 USRs -> the unique USR (or the name
//      itself when unknown: downstream queries treat it as an unregistered
//      endpoint, exactly as the by-name path does today).
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
  if (auto usr = args.getString(usrParam))
    return usr->str();
  auto name = args.getString(nameParam);

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
    ambiguous = makeAmbiguousNameResult(ctx, nameParam, *name,
                                        std::move(usrs), filterNote);
    return std::nullopt;
  }
  if (usrs.size() == 1)
    return std::move(usrs.front());
  return name->str();
}

// Adds the resolved node usr to a response object when the identity names a
// registered node (unregistered endpoints have no node to cite).
void attachUsr(llvm::json::Object &obj, const ToolContext &ctx,
                      const std::string &ident,
                      llvm::StringRef key) {
  if (const auto *node = ctx.graph.findNode(ident))
    obj[key] = node->usr;
}

} // namespace vycor
