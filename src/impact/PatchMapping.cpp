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

#include "vycor/impact/PatchMapping.h"

#include <algorithm>
#include <map>
#include <tuple>
#include <unordered_map>

namespace vycor {

namespace {

llvm::StringRef stripPatchPrefix(llvm::StringRef p) {
  // `git diff` spells paths as a/x and b/x; `diff -u` as given. A path
  // may carry a trailing "\t<timestamp>" (diff -u) — cut at the tab.
  p = p.split('\t').first.rtrim();
  if (p.starts_with("a/") || p.starts_with("b/"))
    p = p.drop_front(2);
  while (p.starts_with("./"))
    p = p.drop_front(2);
  return p;
}

bool parseHunkHeader(llvm::StringRef line, unsigned &newStart,
                     unsigned &newCount) {
  // "@@ -a[,b] +c[,d] @@ ..."
  if (!line.starts_with("@@ "))
    return false;
  size_t plus = line.find('+');
  if (plus == llvm::StringRef::npos)
    return false;
  llvm::StringRef after = line.drop_front(plus + 1);
  after = after.substr(0, after.find(' '));
  auto [start, count] = after.split(',');
  if (start.getAsInteger(10, newStart))
    return false;
  newCount = 1;
  if (!count.empty() && count.getAsInteger(10, newCount))
    return false;
  return true;
}

} // namespace

std::vector<PatchRange> parseUnifiedDiff(llvm::StringRef text) {
  std::vector<PatchRange> out;
  std::string beforeName, afterName;
  bool inFile = false, deletedFile = false;
  while (!text.empty()) {
    auto [line, rest] = text.split('\n');
    text = rest;
    if (line.starts_with("--- ")) {
      beforeName = stripPatchPrefix(line.drop_front(4)).str();
      inFile = false;
      continue;
    }
    if (line.starts_with("+++ ")) {
      llvm::StringRef name = stripPatchPrefix(line.drop_front(4));
      deletedFile = name == "/dev/null";
      afterName = deletedFile ? beforeName : name.str();
      inFile = !afterName.empty() && afterName != "/dev/null";
      continue;
    }
    if (line.starts_with("diff ")) {
      inFile = false;
      continue;
    }
    unsigned start = 0, count = 0;
    if (inFile && parseHunkHeader(line, start, count)) {
      PatchRange r;
      r.file = afterName;
      if (count == 0 || deletedFile) {
        // A pure deletion sits between after-side lines `start` and
        // `start + 1`; both are in the range so an anchor on either side
        // can claim it.
        r.firstLine = start == 0 ? 1 : start;
        r.lastLine = start == 0 ? 1 : start + 1;
        r.deletionOnly = true;
      } else {
        r.firstLine = start;
        r.lastLine = start + count - 1;
      }
      out.push_back(std::move(r));
    }
  }
  return out;
}

const char *mappingViaName(MappingVia v) {
  switch (v) {
  case MappingVia::Argument: return "argument";
  case MappingVia::CallSite: return "call_site";
  case MappingVia::Definition: return "definition";
  case MappingVia::Extent: return "extent";
  case MappingVia::File: return "file";
  case MappingVia::Diff: return "diff";
  }
  return "unknown";
}

namespace {

struct Located {
  unsigned line;
  std::string usr;
  bool operator<(const Located &o) const {
    return std::tie(line, usr) < std::tie(o.line, o.usr);
  }
};

struct FileEntry {
  std::vector<Located> defs;  // node locations
  std::vector<Located> sites; // call sites, usr = the caller
};

llvm::StringRef stripDot(llvm::StringRef p) {
  while (p.starts_with("./"))
    p = p.drop_front(2);
  return p;
}

// Equal, or one a component-suffix of the other.
bool pathsMatch(llvm::StringRef a, llvm::StringRef b) {
  a = stripDot(a);
  b = stripDot(b);
  if (a.empty() || b.empty())
    return false;
  if (a == b)
    return true;
  if (a.size() > b.size())
    std::swap(a, b);
  return b.ends_with(a) && b[b.size() - a.size() - 1] == '/';
}

// "file:line:col" -> (file, line); nullopt when not that shape.
std::optional<std::pair<std::string, unsigned>>
splitSite(const std::string &site) {
  llvm::StringRef s(site);
  size_t c2 = s.rfind(':');
  if (c2 == llvm::StringRef::npos || c2 == 0)
    return std::nullopt;
  size_t c1 = s.rfind(':', c2);
  if (c1 == llvm::StringRef::npos || c1 == 0)
    return std::nullopt;
  unsigned line = 0;
  if (s.substr(c1 + 1, c2 - c1 - 1).getAsInteger(10, line))
    return std::nullopt;
  return std::make_pair(s.substr(0, c1).str(), line);
}

} // namespace

PatchMapping mapRangesToFunctions(const CallGraph &graph,
                                  const std::vector<PatchRange> &ranges,
                                  llvm::StringRef patchRoot) {
  PatchMapping out;
  std::map<std::string, FileEntry> files;
  std::unordered_map<std::string, const CallGraphNode *> nodes;
  for (const CallGraphNode *n : graph.allNodes()) {
    nodes.emplace(n->usr, n);
    files[n->file].defs.push_back({n->line, n->usr});
    for (const CallGraphEdge &e : graph.calleesOf(n->usr))
      if (auto fl = splitSite(e.callSite))
        files[fl->first].sites.push_back({fl->second, e.callerUsr});
  }
  for (auto &[file, entry] : files) {
    std::sort(entry.defs.begin(), entry.defs.end());
    entry.defs.erase(std::unique(entry.defs.begin(), entry.defs.end(),
                                 [](const Located &a, const Located &b) {
                                   return a.line == b.line && a.usr == b.usr;
                                 }),
                     entry.defs.end());
    std::sort(entry.sites.begin(), entry.sites.end());
  }

  std::map<std::string, MappedFunction> mapped; // by usr, best via
  auto attribute = [&](const std::string &usr, MappingVia via) {
    auto it = mapped.find(usr);
    if (it != mapped.end()) {
      if (via < it->second.via)
        it->second.via = via;
      return;
    }
    MappedFunction f;
    f.usr = usr;
    auto n = nodes.find(usr);
    if (n != nodes.end()) {
      f.name = n->second->qualifiedName;
      f.file = n->second->file;
      f.line = n->second->line;
    } else {
      f.name = usr;
    }
    f.via = via;
    mapped.emplace(usr, std::move(f));
  };

  for (const PatchRange &range : ranges) {
    std::string wanted = range.file;
    if (!patchRoot.empty()) {
      std::string root = patchRoot.str();
      if (root.back() != '/')
        root += '/';
      wanted = root + wanted;
    }
    std::vector<const std::pair<const std::string, FileEntry> *> hits;
    for (const auto &kv : files)
      if (pathsMatch(kv.first, wanted))
        hits.push_back(&kv);
    if (hits.empty()) {
      out.unmapped.push_back(
          {range, "no function or call site indexed in this file"});
      continue;
    }
    if (hits.size() > 1) {
      out.unmapped.push_back(
          {range, "matches " + std::to_string(hits.size()) +
                      " indexed files; pass patch_root"});
      continue;
    }
    const FileEntry &entry = hits.front()->second;
    bool exact = false;
    for (const Located &d : entry.defs) {
      if (d.line >= range.firstLine && d.line <= range.lastLine) {
        attribute(d.usr, MappingVia::Definition);
        exact = true;
      }
    }
    for (const Located &s : entry.sites) {
      if (s.line >= range.firstLine && s.line <= range.lastLine) {
        attribute(s.usr, MappingVia::CallSite);
        exact = true;
      }
    }
    if (exact)
      continue;
    if (entry.defs.empty() && entry.sites.empty()) {
      out.unmapped.push_back({range, "no function indexed at these lines"});
      continue;
    }
    // Estimate from the anchors the index holds for the file: the
    // functions' recorded locations (where they are declared) and their
    // call sites. The nearest anchor before the range says whose body the
    // range is in; every anchor sharing that line (a macro) is a
    // candidate. When the nearest anchor after the range is a call site
    // of a different function and no declaration lies between, the range
    // may be in either body: both are candidates.
    auto lastBefore = [&](const std::vector<Located> &v) {
      auto it = std::upper_bound(
          v.begin(), v.end(), Located{range.firstLine, {}},
          [](const Located &a, const Located &b) { return a.line < b.line; });
      return it == v.begin() ? v.end() : std::prev(it);
    };
    auto firstAfter = [&](const std::vector<Located> &v) {
      return std::upper_bound(
          v.begin(), v.end(), Located{range.lastLine, {}},
          [](const Located &a, const Located &b) { return a.line < b.line; });
    };
    auto sameLine = [&](const std::vector<Located> &v,
                        std::vector<Located>::const_iterator at,
                        MappingVia via) {
      for (auto it = at; it != v.end() && it->line == at->line; ++it)
        attribute(it->usr, via);
      for (auto it = at; it != v.begin();) {
        --it;
        if (it->line != at->line)
          break;
        attribute(it->usr, via);
      }
    };
    auto prevDef = lastBefore(entry.defs);
    auto prevSite = lastBefore(entry.sites);
    const bool haveDef = prevDef != entry.defs.end();
    const bool haveSite = prevSite != entry.sites.end();
    if (!haveDef && !haveSite) {
      // Above every anchor: file-scope code. Everything the file
      // declares or calls from is a candidate.
      for (const Located &d : entry.defs) {
        attribute(d.usr, MappingVia::File);
      }
      std::string last;
      for (const Located &s : entry.sites) {
        if (s.usr == last)
          continue;
        last = s.usr;
        attribute(s.usr, MappingVia::File);
      }
      continue;
    }
    const bool defNearer =
        haveDef && (!haveSite || prevDef->line >= prevSite->line);
    if (defNearer)
      sameLine(entry.defs, prevDef, MappingVia::Extent);
    else
      sameLine(entry.sites, prevSite, MappingVia::Extent);
    const std::string &owner = defNearer ? prevDef->usr : prevSite->usr;
    auto nextDef = firstAfter(entry.defs);
    auto nextSite = firstAfter(entry.sites);
    if (nextSite != entry.sites.end() && nextSite->usr != owner &&
        (nextDef == entry.defs.end() || nextDef->line > nextSite->line))
      sameLine(entry.sites, nextSite, MappingVia::Extent);
  }
  // The counts are of changed functions at their most precise
  // attribution, not of anchors hit.
  for (auto &[usr, f] : mapped) {
    switch (f.via) {
    case MappingVia::CallSite: ++out.viaCallSite; break;
    case MappingVia::Definition: ++out.viaDefinition; break;
    case MappingVia::Extent: ++out.viaExtent; break;
    case MappingVia::File: ++out.viaFile; break;
    default: break;
    }
    out.functions.push_back(std::move(f));
  }
  std::sort(out.unmapped.begin(), out.unmapped.end(),
            [](const UnmappedRange &a, const UnmappedRange &b) {
              return std::tie(a.range.file, a.range.firstLine) <
                     std::tie(b.range.file, b.range.firstLine);
            });
  return out;
}

} // namespace vycor
