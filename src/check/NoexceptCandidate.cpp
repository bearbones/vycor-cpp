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

#include "vycor/check/Analyzer.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace vycor {
namespace {

// Why a function is not proven. Zero is the proof.
enum Why : uint8_t {
  kOwnThrow = 1u << 0,    // its own body can throw
  kCalleeThrow = 1u << 1, // an unguarded callee can
  kUnknown = 1u << 2,     // an unguarded callee has no definition indexed
  kUnreliable = 1u << 3,  // it, or a callee, came from a TU with errors
};

// What a refuted callee tells its callers.
constexpr uint8_t propagated(uint8_t why) {
  return static_cast<uint8_t>(
      (why & (kOwnThrow | kCalleeThrow) ? kCalleeThrow : 0) |
      (why & (kUnknown | kUnreliable)));
}

// Edges are packed (callee, caller, bits) so that one sort groups them by
// callee, which is the direction both the proof and the ranking read.
constexpr unsigned kBitsWidth = 3;
constexpr unsigned kIdWidth = 30;
static_assert(NoexceptFactEntry::AllCalleeBits < (1u << kBitsWidth),
              "a callee bit does not fit the packed edge");
constexpr uint64_t kIdMask = (uint64_t{1} << kIdWidth) - 1;
constexpr uint64_t packEdge(uint32_t callee, uint32_t caller, uint8_t bits) {
  return (uint64_t{callee} << (kIdWidth + kBitsWidth)) |
         (uint64_t{caller} << kBitsWidth) | bits;
}
constexpr uint32_t edgeCaller(uint64_t e) {
  return static_cast<uint32_t>((e >> kBitsWidth) & kIdMask);
}
constexpr uint32_t edgeCallee(uint64_t e) {
  return static_cast<uint32_t>(e >> (kIdWidth + kBitsWidth));
}

struct Candidate {
  uint32_t fact = 0;
  uint32_t nothrowCallers = 0;
  uint32_t fanIn = 0;
  uint32_t exampleCaller = UINT32_MAX; // node id
  uint8_t tier = 3;
  Diagnostic::Kind kind = Diagnostic::Noexcept_Candidate;
};

std::string plural(size_t n, const char *noun) {
  return std::to_string(n) + " " + noun + (n == 1 ? "" : "s");
}

} // namespace

void analyzeNoexceptCandidates(const GlobalIndex &index,
                               const NoexceptCandidateOptions &options,
                               std::vector<Diagnostic> &diagnostics,
                               NoexceptCandidateStats *stats) {
  NoexceptCandidateStats counted;
  const size_t factCount = index.noexceptFactCount();
  if (factCount == 0) {
    if (stats)
      *stats = counted;
    return;
  }

  // Facts arrive in whatever order the TUs finished. Everything below
  // (node ids, edge order, ranks) derives from this sorted view, so the
  // output is a function of the facts alone.
  std::vector<const NoexceptFactEntry *> facts;
  facts.reserve(factCount);
  size_t edgeTotal = 0;
  index.forEachNoexceptFact([&](const NoexceptFactEntry &f) {
    facts.push_back(&f);
    edgeTotal += f.callees.size();
  });
  std::sort(facts.begin(), facts.end(),
            [](const NoexceptFactEntry *a, const NoexceptFactEntry *b) {
              return std::tie(a->usr, a->filePath, a->line, a->column) <
                     std::tie(b->usr, b->filePath, b->line, b->column);
            });

  // One node per USR. Definition sites sharing a USR (a function defined
  // differently in two binaries of one build, two overloads the USR does
  // not tell apart) are one node: a caller cannot say which it gets, so
  // the node is proven only if every site is.
  std::unordered_map<std::string_view, uint32_t> ids;
  ids.reserve(factCount * 2);
  std::vector<uint32_t> nodeOfFact(factCount);
  std::vector<uint32_t> firstFactOfNode;
  firstFactOfNode.reserve(factCount);
  for (size_t i = 0; i < factCount; ++i) {
    if (i > 0 && facts[i]->usr == facts[i - 1]->usr) {
      nodeOfFact[i] = nodeOfFact[i - 1];
      continue;
    }
    nodeOfFact[i] = static_cast<uint32_t>(firstFactOfNode.size());
    ids.emplace(facts[i]->usr, nodeOfFact[i]);
    firstFactOfNode.push_back(static_cast<uint32_t>(i));
  }
  const auto definedCount = static_cast<uint32_t>(firstFactOfNode.size());

  std::vector<uint64_t> edges;
  edges.reserve(edgeTotal);
  for (size_t i = 0; i < factCount; ++i) {
    const NoexceptFactEntry &f = *facts[i];
    for (size_t c = 0; c < f.callees.size(); ++c) {
      auto next = static_cast<uint32_t>(ids.size());
      uint32_t callee = ids.try_emplace(f.callees[c], next).first->second;
      uint8_t bits = c < f.calleeBits.size()
                         ? f.calleeBits[c] & NoexceptFactEntry::AllCalleeBits
                         : NoexceptFactEntry::AllCalleeBits;
      edges.push_back(packEdge(callee, nodeOfFact[i], bits));
    }
  }
  const size_t nodeCount = ids.size();
  if (nodeCount > kIdMask) {
    // Past what an edge can address. Report nothing rather than a proof
    // over a truncated graph.
    if (stats)
      *stats = counted;
    return;
  }

  // Group by callee; fold the sightings of one (callee, caller) pair.
  std::sort(edges.begin(), edges.end());
  {
    size_t out = 0;
    for (size_t i = 0; i < edges.size(); ++i) {
      if (out > 0 && (edges[out - 1] >> kBitsWidth) == (edges[i] >> kBitsWidth))
        edges[out - 1] |= edges[i];
      else
        edges[out++] = edges[i];
    }
    edges.resize(out);
  }
  std::vector<uint32_t> callersBegin(nodeCount + 1, 0);
  for (uint64_t e : edges)
    ++callersBegin[edgeCallee(e) + 1];
  for (size_t i = 0; i < nodeCount; ++i)
    callersBegin[i + 1] += callersBegin[i];

  // Per-node state. `terminal` is a function every sighting declares
  // non-throwing: the language stops exceptions at it, so its body and
  // callees are not part of anyone's proof.
  std::vector<uint32_t> flags(nodeCount, 0);
  for (size_t i = 0; i < factCount; ++i)
    flags[nodeOfFact[i]] |= facts[i]->flags;
  std::vector<uint8_t> terminal(nodeCount, 0);
  std::vector<uint8_t> why(nodeCount, 0);
  std::vector<uint8_t> queued(nodeCount, 0);
  std::vector<uint32_t> work;
  work.reserve(nodeCount); // a node is on the list at most once at a time
  for (uint32_t n = 0; n < nodeCount; ++n) {
    if (n >= definedCount) {
      why[n] = kUnknown;
    } else if ((flags[n] & NoexceptFactEntry::DeclaredNothrow) &&
               !(flags[n] & NoexceptFactEntry::DeclaredThrowing)) {
      terminal[n] = 1;
    } else {
      if (flags[n] & NoexceptFactEntry::MayThrow)
        why[n] |= kOwnThrow;
      if (flags[n] & NoexceptFactEntry::Unreliable)
        why[n] |= kUnreliable;
    }
    if (why[n]) {
      queued[n] = 1;
      work.push_back(n);
    }
  }

  // Greatest fixed point: every function starts out proven and loses that
  // only to a reason that reaches it over unguarded calls. A cycle none of
  // whose members has another way to throw is never reached, so it stays
  // proven as a whole. Reasons only accumulate (three bits), so each node
  // is revisited a bounded number of times.
  while (!work.empty()) {
    uint32_t callee = work.back();
    work.pop_back();
    queued[callee] = 0;
    const uint8_t reason = propagated(why[callee]);
    for (uint32_t e = callersBegin[callee]; e < callersBegin[callee + 1];
         ++e) {
      if (!(edges[e] & NoexceptFactEntry::Exit))
        continue;
      uint32_t caller = edgeCaller(edges[e]);
      if (terminal[caller] || (why[caller] | reason) == why[caller])
        continue;
      why[caller] |= reason;
      if (!queued[caller]) {
        queued[caller] = 1;
        work.push_back(caller);
      }
    }
  }

  // Counted per definition site, as findings are: a function with two
  // sites is two definitions someone would have to edit.
  for (size_t i = 0; i < factCount; ++i) {
    const uint32_t n = nodeOfFact[i];
    if (terminal[n])
      ++counted.declaredNothrow;
    else if (why[n] == 0)
      ++counted.proven;
    else if (why[n] & kOwnThrow)
      ++counted.refutedByBody;
    else if (why[n] & (kCalleeThrow | kUnknown))
      ++counted.refutedByCallee;
    else
      ++counted.withheldUnreliable;
  }

  // With a TU missing from the index, a call that leaves the TU it was
  // recorded in may reach a definition nobody indexed (the same function
  // defined differently in another binary of the build, say). A proof
  // that crosses such a call is withheld, and so is every proof resting
  // on it. Proofs that stay within one TU's own code are unaffected.
  //
  // A callee the index holds as declared non-throwing is no exception. An
  // Exit edge exists only where the calling TU saw no such declaration,
  // so that callee's noexcept is a property of the one definition that
  // was indexed, not of what the caller links against.
  std::vector<uint8_t> crossesTu(nodeCount, 0);
  if (!options.coverageComplete) {
    const auto proven = [&](uint32_t n) { return !terminal[n] && !why[n]; };
    constexpr uint64_t kCrossing =
        NoexceptFactEntry::Exit | NoexceptFactEntry::Elsewhere;
    for (uint64_t e : edges) {
      uint32_t caller = edgeCaller(e);
      if ((e & kCrossing) != kCrossing || crossesTu[caller] ||
          !proven(caller) || why[edgeCallee(e)] != 0)
        continue;
      crossesTu[caller] = 1;
      work.push_back(caller);
    }
    while (!work.empty()) {
      uint32_t callee = work.back();
      work.pop_back();
      for (uint32_t e = callersBegin[callee]; e < callersBegin[callee + 1];
           ++e) {
        uint32_t caller = edgeCaller(edges[e]);
        if (!(edges[e] & NoexceptFactEntry::Exit) || crossesTu[caller] ||
            !proven(caller))
          continue;
        crossesTu[caller] = 1;
        work.push_back(caller);
      }
    }
  }

  // One finding per definition site of a proven function.
  std::vector<Candidate> candidates;
  for (size_t i = 0; i < factCount; ++i) {
    const NoexceptFactEntry &f = *facts[i];
    uint32_t node = nodeOfFact[i];
    if (terminal[node] || why[node] != 0)
      continue;
    if (f.flags & (NoexceptFactEntry::NotReportable |
                   NoexceptFactEntry::DeclaredNothrow))
      continue;
    Candidate c;
    c.fact = static_cast<uint32_t>(i);
    if (f.flags & NoexceptFactEntry::Virtual) {
      if (!options.virtuals)
        continue;
      c.kind = Diagnostic::Noexcept_CandidateVirtual;
    } else if (f.flags & NoexceptFactEntry::Move) {
      if (!options.moves)
        continue;
      c.kind = Diagnostic::Noexcept_CandidateMove;
    } else if (!options.general) {
      continue;
    }
    if (crossesTu[node]) {
      ++counted.withheldCoverage;
      continue;
    }
    for (uint32_t e = callersBegin[node]; e < callersBegin[node + 1]; ++e) {
      uint32_t caller = edgeCaller(edges[e]);
      if (caller == node)
        continue;
      ++c.fanIn;
      if (!terminal[caller])
        continue;
      ++c.nothrowCallers;
      // Callers are visited in node order, which is USR order: the first
      // is a stable example.
      if (c.exampleCaller == UINT32_MAX)
        c.exampleCaller = caller;
    }
    if (c.nothrowCallers > 0)
      c.tier = 1;
    else if (f.flags & (NoexceptFactEntry::Swap | NoexceptFactEntry::Move))
      c.tier = 2;
    candidates.push_back(c);
  }

  // Most useful first: where an exception would already be fatal, then
  // the operations generic code tests for noexcept, then by how widely
  // the function is used.
  std::sort(candidates.begin(), candidates.end(),
            [&](const Candidate &a, const Candidate &b) {
              if (a.tier != b.tier)
                return a.tier < b.tier;
              if (a.nothrowCallers != b.nothrowCallers)
                return a.nothrowCallers > b.nothrowCallers;
              if (a.fanIn != b.fanIn)
                return a.fanIn > b.fanIn;
              const NoexceptFactEntry &fa = *facts[a.fact];
              const NoexceptFactEntry &fb = *facts[b.fact];
              return std::tie(fa.qualifiedName, fa.filePath, fa.line,
                              fa.column, fa.usr) <
                     std::tie(fb.qualifiedName, fb.filePath, fb.line,
                              fb.column, fb.usr);
            });

  diagnostics.reserve(diagnostics.size() + candidates.size());
  unsigned rank = 0;
  for (const Candidate &c : candidates) {
    const NoexceptFactEntry &f = *facts[c.fact];
    Diagnostic diag;
    diag.kind = c.kind;
    diag.file = f.filePath;
    diag.callLocation = f.filePath + ":" + std::to_string(f.line) + ":" +
                        std::to_string(f.column);
    diag.resolvedDecl = f.qualifiedName;
    diag.entities = {f.usr};
    diag.rank = ++rank;

    std::string message = "noexcept candidate: '" + f.qualifiedName +
                          "' cannot throw and is not declared noexcept: no "
                          "throw leaves its body, and every call it makes is "
                          "to a noexcept function, to a function proven the "
                          "same way, or inside a try with a catch (...).";
    if (c.nothrowCallers > 0) {
      message += " Called from " +
                 plural(c.nothrowCallers, "noexcept function") + " (such as '" +
                 facts[firstFactOfNode[c.exampleCaller]]->qualifiedName +
                 "'), " + plural(c.fanIn, "caller") + " in all.";
    } else if (c.fanIn > 0) {
      message += " Called from " + plural(c.fanIn, "function") + ".";
    }
    if (c.kind == Diagnostic::Noexcept_CandidateVirtual)
      message += " It is virtual: declaring it noexcept requires every "
                 "override to be noexcept too.";
    else if (c.kind == Diagnostic::Noexcept_CandidateMove)
      message += " Containers copy instead of moving on reallocation when "
                 "the move operation is not noexcept.";
    diag.message = std::move(message);
    diagnostics.push_back(std::move(diag));
  }
  counted.reported = candidates.size();
  if (stats)
    *stats = counted;
}

} // namespace vycor
