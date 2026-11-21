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

#include "vycor/callgraph/ReferenceIndex.h"
#include "vycor/callgraph/Utf8.h"
#include "vycor/query/Identity.h"
#include "vycor/query/Tools.h"
#include "Paging.h"
#include "Registry.h"
#include "Schema.h"

#include "llvm/ADT/StringRef.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// The symbol reference tools (docs/symbol-references.md): lookup_symbol
// and get_references answer from the snapshot's references section alone.

namespace vycor {

namespace {

/// The reference tools' precondition: a context that carries the
/// reference index (an index file older than format v15 never loads, so a
/// loaded index always has the section).
std::optional<llvm::json::Value> referencesUnavailable(const ToolContext &ctx) {
  if (ctx.references)
    return std::nullopt;
  return unavailableError("No symbol reference facts loaded");
}

llvm::json::Array attributesOf(uint32_t flags) {
  llvm::json::Array out;
  for (const char *name : symbolAttributeNames(flags))
    out.push_back(name);
  return out;
}

/// Where a candidate is declared, for telling same-named symbols apart:
/// its definition when the index holds one, else its first declaration.
const SymbolDeclInfo *primaryDeclaration(const SymbolInfo &sym) {
  for (const SymbolDeclInfo &d : sym.declarations)
    if (d.definition)
      return &d;
  return sym.declarations.empty() ? nullptr : &sym.declarations.front();
}

llvm::json::Value ambiguousSymbol(const ReferenceIndex &refs,
                                  llvm::StringRef name,
                                  const std::vector<std::string> &usrs,
                                  llvm::StringRef filterNote) {
  llvm::json::Array candidates;
  for (size_t i = 0; i < usrs.size() && i < kMaxAmbiguousCandidates; ++i) {
    llvm::json::Object cand;
    cand["usr"] = usrs[i];
    if (auto sym = refs.symbol(usrs[i])) {
      cand["qualifiedName"] = sym->name;
      cand["kind"] = symbolKindName(sym->kind);
      cand["linkage"] = linkageClassName(sym->linkage);
      if (const SymbolDeclInfo *d = primaryDeclaration(*sym)) {
        cand["file"] = d->file;
        cand["line"] = static_cast<int64_t>(d->line);
      }
    }
    candidates.push_back(llvm::json::Value(std::move(cand)));
  }
  const bool truncated = usrs.size() > kMaxAmbiguousCandidates;
  llvm::json::Object obj;
  obj["ambiguous"] = true;
  obj["parameter"] = "name";
  obj["name"] = name.str();
  obj["total_candidates"] = static_cast<int64_t>(usrs.size());
  obj["candidates"] = std::move(candidates);
  if (truncated)
    obj["truncated"] = true;
  std::string note =
      "Multiple symbols share this name. Re-run with the 'usr' parameter "
      "of the intended candidate, or narrow with 'filter' (a literal "
      "substring matched against candidate usr/name/file).";
  if (!filterNote.empty())
    note += " " + filterNote.str();
  if (truncated)
    note += " Candidate list truncated to " +
            std::to_string(kMaxAmbiguousCandidates) + " of " +
            std::to_string(usrs.size()) + ".";
  obj["note"] = std::move(note);
  return llvm::json::Value(std::move(obj));
}

/// Resolve `usr` / `name` (+ `filter`) to one symbol. On anything else
/// `answer` holds the payload to return: a usage error, `not_found`, or
/// the disambiguation list.
std::optional<SymbolInfo> resolveSymbol(const llvm::json::Object &args,
                                        const ReferenceIndex &refs,
                                        llvm::json::Value &answer) {
  if (auto usr = args.getString("usr")) {
    if (auto sym = refs.symbol(lookupText(*usr)))
      return sym;
    answer = notFoundError("Symbol not found: " + usr->str());
    return std::nullopt;
  }
  auto name = args.getString("name");
  if (!name || name->empty()) {
    answer = usageError("Missing required parameter 'name' (or 'usr')");
    return std::nullopt;
  }
  std::vector<std::string> usrs = refs.usrsNamed(lookupText(*name));
  if (usrs.empty()) {
    answer = notFoundError("Symbol not found: " + name->str());
    return std::nullopt;
  }
  std::string filterNote;
  if (usrs.size() > 1) {
    if (auto filter = args.getString("filter")) {
      std::vector<std::string> kept;
      for (const std::string &u : usrs) {
        bool hit = llvm::StringRef(u).contains(*filter) ||
                   name->contains(*filter);
        if (!hit)
          if (auto sym = refs.symbol(u))
            for (const SymbolDeclInfo &d : sym->declarations)
              hit = hit || llvm::StringRef(d.file).contains(*filter);
        if (hit)
          kept.push_back(u);
      }
      if (kept.empty()) {
        filterNote = "The filter '" + filter->str() +
                     "' matched no candidate; showing the unfiltered set.";
      } else {
        if (kept.size() > 1)
          filterNote = "Candidates already narrowed by filter '" +
                       filter->str() + "'.";
        usrs = std::move(kept);
      }
    }
  }
  if (usrs.size() > 1) {
    answer = ambiguousSymbol(refs, *name, usrs, filterNote);
    return std::nullopt;
  }
  if (auto sym = refs.symbol(usrs.front()))
    return sym;
  answer = notFoundError("Symbol not found: " + name->str());
  return std::nullopt;
}

/// Whether a reference to `sym` was made by a TU other than the one that
/// defines it. Only a symbol with exactly one defining TU has an answer.
std::optional<bool> fromOtherTu(const SymbolInfo &sym,
                                const ReferenceInfo &ref) {
  if (sym.definingTuCount != 1)
    return std::nullopt;
  return ref.tuCount > 1 || ref.firstTu != sym.firstDefiningTu;
}

// ============================================================================
// lookup_symbol
// ============================================================================

llvm::json::Value handleLookupSymbol(const llvm::json::Object &args,
                                     const ToolContext &ctx) {
  if (auto unavailable = referencesUnavailable(ctx))
    return std::move(*unavailable);
  const ReferenceIndex &refs = *ctx.references;
  llvm::json::Value answer(nullptr);
  auto sym = resolveSymbol(args, refs, answer);
  if (!sym)
    return answer;

  llvm::json::Object obj;
  obj["qualifiedName"] = sym->name;
  obj["usr"] = sym->usr;
  obj["kind"] = symbolKindName(sym->kind);
  obj["linkage"] = linkageClassName(sym->linkage);
  obj["attributes"] = attributesOf(sym->flags);
  // One stored number, two meanings: a function's body in source lines,
  // a variable's or class's object size in bytes.
  if (sym->size != 0)
    obj[sym->kind == SymbolKind::Function ? "bodyLines" : "sizeBytes"] =
        static_cast<int64_t>(sym->size);
  obj["declaredInHeader"] = sym->declaredInHeader;
  obj["definingTuCount"] = static_cast<int64_t>(sym->definingTuCount);
  if (sym->definingTuCount != 0)
    obj["definingTu"] = sym->firstDefiningTu;
  if (!sym->vtableHomeTu.empty())
    obj["vtableHomeTu"] = sym->vtableHomeTu;

  llvm::json::Array decls;
  for (const SymbolDeclInfo &d : sym->declarations) {
    llvm::json::Object e;
    e["file"] = d.file;
    e["line"] = static_cast<int64_t>(d.line);
    e["definition"] = d.definition;
    e["inHeader"] = d.inHeader;
    e["tuCount"] = static_cast<int64_t>(d.tuCount);
    e["firstTu"] = d.firstTu;
    decls.push_back(llvm::json::Value(std::move(e)));
  }
  obj["declarations"] = std::move(decls);

  // How the symbol is used: referrers per role, and whether any of them
  // sits in another TU than the definition.
  std::array<int64_t, kRefRoleCount> perRole{};
  bool other = false;
  for (const ReferenceInfo &r : refs.referencesTo(sym->usr)) {
    ++perRole[static_cast<unsigned>(r.role)];
    other = other || fromOtherTu(*sym, r).value_or(false);
  }
  llvm::json::Object referrers;
  for (unsigned i = 0; i < kRefRoleCount; ++i)
    referrers[refRoleName(static_cast<RefRole>(i))] = perRole[i];
  obj["referrers"] = std::move(referrers);
  if (sym->definingTuCount == 1)
    obj["referencedFromOtherTu"] = other;
  return llvm::json::Value(std::move(obj));
}

// ============================================================================
// get_references
// ============================================================================

llvm::json::Value handleGetReferences(const llvm::json::Object &args,
                                      const ToolContext &ctx) {
  if (auto unavailable = referencesUnavailable(ctx))
    return std::move(*unavailable);
  const ReferenceIndex &refs = *ctx.references;

  bool to = true;
  if (auto direction = args.getString("direction")) {
    if (*direction == "from")
      to = false;
    else if (*direction != "to")
      return usageError("Invalid direction '" + direction->str() +
                        "': expected 'to' or 'from'");
  } else if (args.get("direction")) {
    return usageError("Invalid direction: must be a string");
  }

  // Role filter: a bit per RefRole; absent means every role.
  unsigned roleMask = (1u << kRefRoleCount) - 1;
  if (const llvm::json::Value *rolesArg = args.get("roles")) {
    const auto *roles = rolesArg->getAsArray();
    if (!roles)
      return usageError("Invalid roles: must be an array of role names");
    roleMask = 0;
    for (const llvm::json::Value &v : *roles) {
      auto name = v.getAsString();
      auto role = name ? refRoleFromName(*name) : std::nullopt;
      if (!role)
        return usageError(
            "Invalid role" + (name ? " '" + name->str() + "'" : std::string()) +
            ": expected call, address_of, read, write, vtable_use or "
            "typeinfo_use");
      roleMask |= 1u << static_cast<unsigned>(*role);
    }
    if (roleMask == 0)
      return usageError("Invalid roles: the list is empty");
  }

  Page page;
  if (auto err = parsePage(args, kDefaultListLimit, page))
    return usageError(*err);

  llvm::json::Value answer(nullptr);
  auto sym = resolveSymbol(args, refs, answer);
  if (!sym)
    return answer;

  std::vector<ReferenceInfo> all =
      to ? refs.referencesTo(sym->usr) : refs.referencesFrom(sym->usr);
  all.erase(std::remove_if(all.begin(), all.end(),
                           [roleMask](const ReferenceInfo &r) {
                             return !(roleMask &
                                      (1u << static_cast<unsigned>(r.role)));
                           }),
            all.end());

  bool other = false;
  if (to)
    for (const ReferenceInfo &r : all)
      other = other || fromOtherTu(*sym, r).value_or(false);

  llvm::json::Array records;
  for (size_t i = page.begin(all.size()); i < page.end(all.size()); ++i) {
    const ReferenceInfo &r = all[i];
    llvm::json::Object e;
    if (to) {
      e["referrer"] = r.referrerName;
      e["referrerUsr"] = r.referrerUsr;
      e["referrerKind"] = symbolKindName(r.referrerKind);
    } else {
      e["target"] = r.targetName;
      e["targetUsr"] = r.targetUsr;
      e["targetKind"] = symbolKindName(r.targetKind);
    }
    e["role"] = refRoleName(r.role);
    e["count"] = static_cast<int64_t>(r.count);
    e["file"] = r.file;
    e["line"] = static_cast<int64_t>(r.line);
    e["col"] = static_cast<int64_t>(r.col);
    e["tuCount"] = static_cast<int64_t>(r.tuCount);
    e["firstTu"] = r.firstTu;
    if (to)
      if (auto o = fromOtherTu(*sym, r))
        e["fromOtherTu"] = *o;
    records.push_back(llvm::json::Value(std::move(e)));
  }

  llvm::json::Object obj;
  obj["qualifiedName"] = sym->name;
  obj["usr"] = sym->usr;
  obj["kind"] = symbolKindName(sym->kind);
  obj["direction"] = to ? "to" : "from";
  if (to && sym->definingTuCount == 1)
    obj["referencedFromOtherTu"] = other;
  attachPage(obj, page, all.size());
  obj["references"] = std::move(records);
  return llvm::json::Value(std::move(obj));
}

void addSymbolIdentityProps(llvm::json::Object &props) {
  props["name"] = stringProp(
      "Qualified symbol name: a function, a global or static variable "
      "('ns::counter', a function-local static as 'fn::name'), or a class. "
      "Provide 'name' or 'usr' (usr wins when both are present).");
  props["usr"] = stringProp(
      "Exact symbol USR (from a prior answer or a disambiguation "
      "response). Bypasses name resolution.");
  props["filter"] = stringProp(
      "Literal substring matched against candidate usr, qualified name, "
      "and declaring files to narrow an ambiguous name; resolves when "
      "exactly one candidate survives.");
}

} // namespace

void registerReferenceTools(std::vector<ToolEntry> &tools) {
  // lookup_symbol
  {
    llvm::json::Object props;
    addSymbolIdentityProps(props);
    llvm::json::Object schema;
    schema["type"] = "object";
    schema["properties"] = std::move(props);
    tools.push_back(
        {"lookup_symbol",
         "Link-level facts about a function, global variable, or class: "
         "linkage class (external, template_instantiation, vague, "
         "internal), attributes (extern_c, visibility, used, weak, member, "
         "...), every declaration with whether it sits in a header, the "
         "TU(s) that define it, a dynamic class's vtable home TU, how many "
         "referrers use it per role, and whether any reference comes from "
         "a TU other than the defining one. Several symbols with the same "
         "name answer {ambiguous:true, candidates:[...]} — re-query with "
         "the 'usr' of the intended candidate.",
         llvm::json::Value(std::move(schema)), handleLookupSymbol});
  }

  // get_references
  {
    llvm::json::Object props;
    addSymbolIdentityProps(props);
    props["direction"] = stringProp(
        "'to' (default): who references the symbol. 'from': what the "
        "symbol (a function body, a variable's initializer) references.");
    props["roles"] = stringArrayProp(
        "Keep only these roles: call, address_of, read, write, "
        "vtable_use, typeinfo_use (default: all).");
    addPagingProps(props, kDefaultListLimit, "references");
    llvm::json::Object schema;
    schema["type"] = "object";
    schema["properties"] = std::move(props);
    tools.push_back(
        {"get_references",
         "Non-call and call references to (or from) a symbol: one record "
         "per (referrer, target, role) with the occurrence count, one "
         "witness site, and the TUs that contributed it. Roles: call, "
         "address_of (address taken or bound to a non-const reference), "
         "read, write, vtable_use (construction/destruction of a "
         "polymorphic class), typeinfo_use (dynamic_cast, typeid, throw, "
         "catch). Use it to find every reader/writer of a global, or "
         "whether a function is only used inside its own TU.",
         llvm::json::Value(std::move(schema)), handleGetReferences});
  }
}

} // namespace vycor
