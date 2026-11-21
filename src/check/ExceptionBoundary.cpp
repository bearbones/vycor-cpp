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

// exception-boundary-escape: one reverse breadth-first search per witness
// class over the noexcept-candidate fact graph, from what can throw back
// to every caller that can unwind into it. A boundary then reads its
// class and its shortest witness off flat arrays.

#include "vycor/check/Analyzer.h"

#include "NoexceptGraph.h"

#include "llvm/Demangle/Demangle.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace vycor {
namespace {

using noexcept_graph::edgeBits;
using noexcept_graph::edgeCallee;
using noexcept_graph::edgeCaller;
using F = NoexceptFactEntry;

// Witness classes, most certain first.
enum Class : uint8_t {
  kProven = 0,  // a throw expression in project code, no try on the path
  kLibrary = 1, // a standard library throw, no try on the path
  kTyped = 2,   // a throw, every path through a try with typed handlers
  kUnknown = 3, // an unresolved call or a callee with no definition
  kClassCount = 4,
  kNone = kClassCount,
};

// Boundary kinds as reported. The first two are ExceptionBoundaryEntry's.
enum Kind : uint8_t {
  kThreadEntry = ExceptionBoundaryEntry::ThreadEntry,
  kCCallback = ExceptionBoundaryEntry::CCallback,
  kExternC = 2,
  kNoExceptionsFrame = 3,
};

constexpr uint32_t kUnset = UINT32_MAX;

// The callee bits an edge needs to carry an exception of each class: the
// first two travel only over calls no try encloses. An unknown one
// travels over any call: a try with typed handlers does not make an
// unknown callee any less unknown, and a boundary it reaches is not
// counted as having no path to a throw.
constexpr uint8_t kBareMask = F::Exit | F::Bare;
constexpr std::array<uint8_t, kClassCount> kEdgeMask = {kBareMask, kBareMask,
                                                        F::Exit, F::Exit};

constexpr std::array<uint8_t, kClassCount> kSourceReasons = {
    F::ThrowBare, F::LibraryThrowBare,
    F::ThrowBare | F::LibraryThrowBare | F::ThrowTyped,
    F::DynamicBare | F::OtherBare | F::OpaqueTyped};

// A callee nothing defines: a C++ function (Itanium-mangled key, or a USR
// for one with internal linkage) is unknown; a C function is taken not to
// throw, as C code cannot.
bool unknownWhenUndefined(std::string_view key) {
  if (key.substr(0, 2) != "m:")
    return true;
  return key.substr(2, 2) == "_Z";
}

const char *kindKey(uint8_t kind) {
  switch (kind) {
  case kThreadEntry:
    return "thread-entry";
  case kCCallback:
    return "c-callback";
  case kExternC:
    return "extern-c";
  default:
    return "no-exceptions-frame";
  }
}

const char *classWord(uint8_t c) {
  switch (c) {
  case kProven:
    return "proven";
  case kLibrary:
    return "library";
  case kTyped:
    return "typed-handlers";
  default:
    return "unknown";
  }
}

// What a finding is about, before it is worded.
struct Finding {
  uint8_t kind = 0;
  uint8_t cls = kNone;
  uint32_t length = 0; // edges from the boundary to the source
  // The boundary function: a node, or for a hand-over whose target has no
  // fact of its own, the first entry of its group.
  uint32_t node = kUnset;
  uint32_t entry = kUnset;
  // First hop out of the boundary (kUnset: the boundary is the source),
  // or for an undefined callee recorded by a hand-over only, its key.
  uint32_t first = kUnset;
  std::string_view firstKey;
  uint32_t sites = 0;       // hand-over sites of this function and kind
  uint32_t noEhCaller = kUnset; // extern "C": a caller compiled without EH
  std::string_view name, file;
  unsigned line = 0, column = 0;
};

class Search {
public:
  explicit Search(const NoexceptGraph &g) : g_(g) {
    const size_t n = g.nodeCount;
    terminal_.assign(n, 0);
    reasons_.assign(n, 0);
    siteFact_.assign(n, kUnset);
    for (size_t i = 0; i < g.facts.size(); ++i) {
      const uint32_t node = g.nodeOfFact[i];
      const NoexceptFactEntry &f = *g.facts[i];
      reasons_[node] |= f.reasons;
      if (siteFact_[node] == kUnset ||
          F::reasonRank(f.reasons) <
              F::reasonRank(g.facts[siteFact_[node]]->reasons))
        siteFact_[node] = static_cast<uint32_t>(i);
    }
    for (uint32_t v = 0; v < g.definedCount; ++v)
      terminal_[v] = (g.flags[v] & F::DeclaredNothrow) &&
                     !(g.flags[v] & F::DeclaredThrowing);
    for (auto &d : dist_)
      d.assign(n, kUnset);
    for (auto &x : next_)
      x.assign(n, kUnset);
    queue_.reserve(n);
    for (uint8_t c = 0; c < kClassCount; ++c)
      run(c);
  }

  bool isSource(uint8_t c, uint32_t v) const {
    if (v >= g_.definedCount)
      return c == kUnknown && unknownWhenUndefined(g_.keyOfNode[v]);
    return !terminal_[v] && (reasons_[v] & kSourceReasons[c]);
  }
  bool terminal(uint32_t v) const { return terminal_[v]; }
  uint32_t dist(uint8_t c, uint32_t v) const { return dist_[c][v]; }
  uint32_t next(uint8_t c, uint32_t v) const { return next_[c][v]; }
  uint8_t reasons(uint32_t v) const { return reasons_[v]; }
  const NoexceptFactEntry *siteFact(uint32_t v) const {
    return siteFact_[v] == kUnset ? nullptr : g_.facts[siteFact_[v]];
  }

  // The best class an exception reaches `v` with over an edge carrying
  // `bits`, and the path length from v. kNone when none does.
  std::pair<uint8_t, uint32_t> classOf(uint32_t v, uint8_t bits) const {
    for (uint8_t c = 0; c < kClassCount; ++c)
      if ((bits & kEdgeMask[c]) == kEdgeMask[c] && dist_[c][v] != kUnset)
        return {c, dist_[c][v]};
    return {kNone, 0};
  }

private:
  // Multi-source BFS from the class's sources, in node order, over caller
  // edges that carry the class. A function declared non-throwing stops
  // the search: the language ends the exception there.
  void run(uint8_t c) {
    auto &dist = dist_[c];
    auto &next = next_[c];
    queue_.clear();
    for (uint32_t v = 0; v < g_.nodeCount; ++v) {
      if (!isSource(c, v))
        continue;
      dist[v] = 0;
      queue_.push_back(v);
    }
    const uint8_t mask = kEdgeMask[c];
    for (size_t head = 0; head < queue_.size(); ++head) {
      const uint32_t v = queue_[head];
      for (uint32_t e = g_.callersBegin[v]; e < g_.callersBegin[v + 1]; ++e) {
        const uint64_t edge = g_.edges[e];
        if ((edgeBits(edge) & mask) != mask)
          continue;
        const uint32_t u = edgeCaller(edge);
        if (terminal_[u] || dist[u] != kUnset)
          continue;
        dist[u] = dist[v] + 1;
        next[u] = v;
        queue_.push_back(u);
      }
    }
  }

  const NoexceptGraph &g_;
  std::vector<uint8_t> terminal_;
  std::vector<uint8_t> reasons_;
  std::vector<uint32_t> siteFact_; // fact holding the most telling site
  std::array<std::vector<uint32_t>, kClassCount> dist_;
  std::array<std::vector<uint32_t>, kClassCount> next_;
  std::vector<uint32_t> queue_;
};

std::string displayKey(std::string_view key) {
  if (key.substr(0, 2) == "m:")
    return llvm::demangle(key.substr(2));
  return std::string(key);
}

class Writer {
public:
  Writer(const NoexceptGraph &g, const Search &s,
         const std::vector<const ExceptionBoundaryEntry *> &entries)
      : g_(g), s_(s), entries_(entries) {}

  std::string nodeName(uint32_t v) const {
    if (v < g_.definedCount)
      return g_.facts[g_.firstFactOfNode[v]]->qualifiedName;
    return displayKey(g_.keyOfNode[v]);
  }

  // "'a' -> 'b' -> 'c'; 'c' throws at f.cpp:12."
  std::string witness(const Finding &f) const {
    std::vector<std::string> names;
    names.push_back(std::string(f.name));
    std::string tail;
    if (f.first == kUnset && f.firstKey.empty()) {
      tail = sourceText(f.cls, f.node, f.entry);
    } else if (f.first == kUnset) {
      if (f.firstKey != entries_[f.entry]->target)
        names.push_back(displayKey(f.firstKey));
      tail = "'" + names.back() + "' has no definition in the index";
    } else {
      uint32_t v = f.first;
      while (true) {
        names.push_back(nodeName(v));
        const uint32_t n = s_.next(f.cls, v);
        if (n == kUnset)
          break;
        v = n;
      }
      tail = sourceText(f.cls, v, kUnset);
    }
    std::string out;
    constexpr size_t kShown = 10;
    for (size_t i = 0; i < names.size(); ++i) {
      if (names.size() > kShown && i == kShown / 2) {
        out += " -> ... (" + std::to_string(names.size() - kShown) +
               " more)";
        i = names.size() - kShown / 2 - 1;
        continue;
      }
      out += i == 0 ? "'" : " -> '";
      out += names[i];
      out += "'";
    }
    return out + "; " + tail + ".";
  }

private:
  // Why the last function on the witness can throw. `node` is a fact
  // node, or kUnset with `entry` a hand-over summarized in place.
  std::string sourceText(uint8_t cls, uint32_t node, uint32_t entry) const {
    std::string name, site, function;
    uint8_t reasons = 0;
    if (node != kUnset) {
      name = nodeName(node);
      if (node >= g_.definedCount)
        return "'" + name + "' has no definition in the index";
      reasons = s_.reasons(node);
      if (const NoexceptFactEntry *f = s_.siteFact(node)) {
        site = f->reasonSite;
        function = f->reasonFunction;
      }
    } else {
      const ExceptionBoundaryEntry &e = *entries_[entry];
      name = e.qualifiedName;
      reasons = e.reasons;
      site = e.reasonSite;
      function = e.reasonFunction;
    }
    std::string at;
    if (!site.empty())
      at = " at " + site;
    if (!function.empty())
      at += " (in '" + function + "')";
    switch (cls) {
    case kProven:
      return "'" + name + "' throws" + at;
    case kLibrary:
      return "'" + name + "' reaches a throw in the standard library" + at;
    case kTyped:
      return "'" + name + "' throws" + at +
             ", and every such path found runs through a try whose handlers "
             "are all typed";
    default:
      break;
    }
    // The stored site is that of the most telling reason, which for a
    // function with a typed throw too is not this one.
    if (!(reasons & (F::DynamicBare | F::OtherBare)) &&
        (reasons & F::OpaqueTyped)) {
      if (F::reasonRank(reasons) < F::reasonRank(F::OpaqueTyped))
        at.clear();
      return "'" + name +
             "' makes a call the analysis cannot resolve or see into, in a "
             "try whose handlers are all typed" +
             at;
    }
    if (F::reasonRank(reasons) < F::reasonRank(F::DynamicBare))
      at.clear();
    if (reasons & F::DynamicBare)
      return "'" + name + "' makes a virtual or indirect call that may throw" +
             at;
    return "'" + name + "' does something the analysis cannot see into" + at;
  }

  const NoexceptGraph &g_;
  const Search &s_;
  const std::vector<const ExceptionBoundaryEntry *> &entries_;
};

std::string plural(size_t n, const char *noun) {
  return std::to_string(n) + " " + noun + (n == 1 ? "" : "s");
}

} // namespace

void analyzeExceptionBoundaries(const GlobalIndex &index,
                                const ExceptionBoundaryOptions &options,
                                std::vector<Diagnostic> &diagnostics,
                                ExceptionBoundaryStats *stats) {
  ExceptionBoundaryStats counted;
  NoexceptGraph graph;
  if (!graph.build(index)) {
    if (stats)
      *stats = counted;
    return;
  }
  const Search search(graph);
  const uint32_t definedCount = graph.definedCount;
  const auto &flags = graph.flags;
  const auto lookup = [&](std::string_view key) {
    auto it = graph.ids.find(key);
    return it == graph.ids.end() ? kUnset : it->second;
  };

  // Hand-overs grouped by (kind, target), sites in order.
  std::vector<const ExceptionBoundaryEntry *> entries;
  entries.reserve(index.exceptionBoundaryCount());
  index.forEachExceptionBoundary(
      [&](const ExceptionBoundaryEntry &e) { entries.push_back(&e); });
  std::sort(entries.begin(), entries.end(),
            [](const ExceptionBoundaryEntry *a, const ExceptionBoundaryEntry *b) {
              return std::tie(a->kind, a->target, a->apiSite, a->api) <
                     std::tie(b->kind, b->target, b->apiSite, b->api);
            });

  std::vector<Finding> findings;
  std::vector<uint8_t> handedOver(graph.nodeCount, 0);
  const auto consider = [&](Finding &&f) {
    ++counted.boundaries;
    switch (f.cls) {
    case kProven:
      ++counted.proven;
      break;
    case kLibrary:
      ++counted.library;
      break;
    case kTyped:
      ++counted.typed;
      break;
    case kUnknown:
      ++counted.unknown;
      break;
    default:
      ++counted.clean;
      return;
    }
    findings.push_back(std::move(f));
  };

  for (size_t i = 0; i < entries.size();) {
    size_t end = i;
    while (end < entries.size() && entries[end]->kind == entries[i]->kind &&
           entries[end]->target == entries[i]->target)
      ++end;
    Finding f;
    f.kind = entries[i]->kind;
    for (size_t j = i; j < end; ++j) {
      const ExceptionBoundaryEntry &e = *entries[j];
      // A callback given to a C-linkage function that C++ code defines
      // with exceptions on unwinds through that function to its caller;
      // that function's own extern "C" definition is the boundary.
      if (!e.receiver.empty()) {
        const uint32_t r = lookup(e.receiver);
        if (r != kUnset && r < definedCount &&
            (flags[r] & F::CompiledWithExceptions) &&
            !(flags[r] & F::CompiledWithoutExceptions)) {
          ++counted.cppReceivers;
          continue;
        }
      }
      ++f.sites;
      if (f.sites == 1) {
        f.entry = static_cast<uint32_t>(j);
        f.name = e.qualifiedName;
        f.file = e.filePath;
        f.line = e.line;
        f.column = e.column;
      }
      // Its own body first, then each callee.
      for (uint8_t c = 0; c < kClassCount; ++c) {
        if (c > f.cls || (c == f.cls && f.length == 0))
          break;
        if (!(e.reasons & kSourceReasons[c]))
          continue;
        f.cls = c;
        f.length = 0;
        f.first = kUnset;
        f.firstKey = {};
        f.entry = static_cast<uint32_t>(j);
        break;
      }
      for (size_t k = 0; k < e.callees.size(); ++k) {
        const uint8_t bits =
            k < e.calleeBits.size() ? e.calleeBits[k]
                                    : static_cast<uint8_t>(F::AllCalleeBits);
        const uint32_t v = lookup(e.callees[k]);
        uint8_t c = kNone;
        uint32_t len = 0;
        if (v != kUnset) {
          std::tie(c, len) = search.classOf(v, bits);
          if (k == 0 && e.callees.size() == 1 && e.callees[0] == e.target)
            handedOver[v] = 1;
        } else if ((bits & F::Exit) && unknownWhenUndefined(e.callees[k])) {
          c = kUnknown;
        }
        if (c == kNone)
          continue;
        ++len;
        if (c < f.cls || (c == f.cls && len < f.length)) {
          f.cls = c;
          f.length = len;
          f.first = v;
          f.firstKey = v == kUnset ? std::string_view(e.callees[k])
                                   : std::string_view();
          f.entry = static_cast<uint32_t>(j);
        }
      }
    }
    i = end;
    if (f.sites == 0)
      continue;
    // A target with a fact of its own: the witness starts at that node,
    // whose name the entry repeats. A target nothing defines (declared
    // here, defined in a TU the run did not index) is the whole witness.
    if (f.first != kUnset && f.length > 0 &&
        graph.keyOfNode[f.first] == entries[f.entry]->target) {
      if (f.first < definedCount) {
        f.node = f.first;
        f.first = search.next(f.cls, f.node);
        --f.length;
      } else {
        f.firstKey = graph.keyOfNode[f.first];
        f.first = kUnset;
      }
    }
    consider(std::move(f));
  }

  // extern "C" definitions, and frames compiled without exceptions that
  // call into code compiled with them.
  const auto noEh = [&](uint32_t v) {
    return v < definedCount && (flags[v] & F::CompiledWithoutExceptions) &&
           !(flags[v] & F::CompiledWithExceptions);
  };
  for (uint32_t v = 0; v < definedCount; ++v) {
    if (!(flags[v] & F::ExternC) || search.terminal(v) || handedOver[v])
      continue;
    Finding f;
    f.kind = kExternC;
    f.node = v;
    for (uint8_t c = 0; c < kClassCount; ++c) {
      if (search.dist(c, v) == kUnset)
        continue;
      f.cls = c;
      f.length = search.dist(c, v);
      f.first = search.next(c, v);
      break;
    }
    for (uint32_t e = graph.callersBegin[v]; e < graph.callersBegin[v + 1];
         ++e) {
      const uint32_t u = edgeCaller(graph.edges[e]);
      if (noEh(u)) {
        f.noEhCaller = u;
        break;
      }
    }
    consider(std::move(f));
  }
  {
    // One finding per frame: its best call into code with exceptions.
    // Frames without exceptions are few; each gets a slot on first sight.
    std::vector<uint32_t> slot(definedCount, kUnset);
    std::vector<Finding> best;
    for (uint64_t edge : graph.edges) {
      const uint32_t u = edgeCaller(edge);
      const uint32_t v = edgeCallee(edge);
      if (!noEh(u) || search.terminal(u) || v >= definedCount ||
          !(flags[v] & F::CompiledWithExceptions) || (flags[v] & F::ExternC))
        continue;
      if (slot[u] == kUnset) {
        slot[u] = static_cast<uint32_t>(best.size());
        best.emplace_back();
        best.back().kind = kNoExceptionsFrame;
        best.back().node = u;
      }
      Finding &f = best[slot[u]];
      auto [c, len] = search.classOf(v, edgeBits(edge));
      if (c == kNone)
        continue;
      ++len;
      if (c < f.cls || (c == f.cls && len < f.length)) {
        f.cls = c;
        f.length = len;
        f.first = v;
      }
    }
    for (Finding &f : best)
      consider(std::move(f));
  }

  for (Finding &f : findings) {
    if (f.node == kUnset)
      continue;
    const NoexceptFactEntry &fact =
        *graph.facts[graph.firstFactOfNode[f.node]];
    f.name = fact.qualifiedName;
    f.file = fact.filePath;
    f.line = fact.line;
    f.column = fact.column;
  }

  std::sort(findings.begin(), findings.end(),
            [](const Finding &a, const Finding &b) {
              return std::tie(a.cls, a.kind, a.name, a.file, a.line,
                              a.column) < std::tie(b.cls, b.kind, b.name,
                                                   b.file, b.line, b.column);
            });

  const Writer writer(graph, search, entries);
  diagnostics.reserve(diagnostics.size() + findings.size());
  unsigned rank = 0;
  for (const Finding &f : findings) {
    Diagnostic diag;
    diag.kind = Diagnostic::Custom;
    diag.checkName = "exception-boundary-escape";
    diag.file = std::string(f.file);
    diag.callLocation = diag.file + ":" + std::to_string(f.line) + ":" +
                        std::to_string(f.column);
    diag.resolvedDecl = std::string(f.name);
    diag.rank = ++rank;
    diag.note = f.cls != kProven;
    diag.entities = {kindKey(f.kind), std::string(f.name)};

    const std::string name = "'" + std::string(f.name) + "'";
    std::string message;
    switch (f.kind) {
    case kThreadEntry:
    case kCCallback: {
      const ExceptionBoundaryEntry &e = *entries[f.entry];
      diag.entities.push_back(e.api);
      std::string where = e.apiSite;
      if (f.sites > 1)
        where += ", and " + plural(f.sites - 1, "other site");
      if (f.kind == kThreadEntry) {
        message = "exception-boundary-escape: an exception can leave thread "
                  "entry " +
                  name + ", started by " + e.api + " at " + where +
                  "; an exception that leaves a thread's initial function "
                  "calls std::terminate.";
      } else if (!e.receiver.empty()) {
        message = "exception-boundary-escape: an exception can leave " +
                  name + ", passed as a callback to the C function '" +
                  e.api + "' at " + where +
                  "; unwinding through C frames is undefined behavior.";
      } else {
        message = "exception-boundary-escape: an exception can leave " +
                  name + ", stored in the function-pointer field '" + e.api +
                  "' of a C struct at " + where +
                  "; unwinding through the C code that calls it is "
                  "undefined behavior.";
      }
      break;
    }
    case kExternC:
      message = "exception-boundary-escape: an exception can leave extern "
                "\"C\" function " +
                name + "; a C caller cannot catch it, and unwinding through "
                       "C frames is undefined behavior.";
      if (f.noEhCaller != kUnset)
        message += " It is called from code compiled without exceptions, "
                   "such as '" +
                   writer.nodeName(f.noEhCaller) + "'.";
      break;
    default:
      message = "exception-boundary-escape: an exception can unwind into " +
                name +
                ", compiled without exceptions; its frame has no unwind "
                "information, so the unwinder calls std::terminate there.";
      break;
    }
    message += " Witness: " + writer.witness(f);
    message += " Confidence: ";
    message += classWord(f.cls);
    switch (f.cls) {
    case kProven:
      message += " (no try encloses any call on the path).";
      break;
    case kLibrary:
      message += " (the throw is the standard library's, often for misuse "
                 "or exhaustion; no try encloses any call on the path).";
      break;
    case kTyped:
      message += " (escapes only if the thrown type matches none of the "
                 "handlers).";
      break;
    default:
      message += " (a call whose target is not known may throw).";
      if (!options.coverageComplete)
        message += " Some TUs did not parse cleanly, so a missing "
                   "definition may be in one of them.";
      break;
    }
    diag.message = std::move(message);
    diagnostics.push_back(std::move(diag));
  }
  counted.reported = findings.size();
  if (stats)
    *stats = counted;
}

} // namespace vycor
