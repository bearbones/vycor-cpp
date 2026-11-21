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

#include <cstdint>
#include <string>
#include <vector>

namespace clang {
class ASTContext;
} // namespace clang

// Static-initialization roots: what runs before `main`.
//
// The call graph records an edge only from inside a function, so a
// dynamic initializer at namespace scope (`Registry g = makeRegistry();`)
// has no node and no out-edges. These facts are the missing roots: one
// record per dynamic initializer and per `__attribute__((constructor))`
// function, each naming the functions it calls directly by the same
// identities the graph is keyed with, so a reduction can continue the walk
// through the graph (the static-init-cost metric, docs/metrics/).
//
// The megascope bake collects them in its one parse of each TU
// (BakedIndexes::staticInits) and the saved index carries them in its
// `static_inits` section (snapshot format v20, Snapshot.h), so a loaded
// index answers like the bake that wrote it. Worker shards carry their
// TUs' roots (absorb), and a warm refresh drops the roots of the TUs it
// re-parses or drops (removeTUs) before absorbing the fresh ones.

namespace vycor {

enum class StaticInitKind : uint8_t {
  /// A variable of static storage duration, at namespace or class scope,
  /// whose initializer is not a constant expression.
  Variable,
  /// A function marked `__attribute__((constructor))`.
  ConstructorFunction,
};

/// A function an initializer calls directly.
struct StaticInitCallee {
  /// Graph identity (the call graph's node key) and display name.
  std::string usr;
  std::string display;
  /// The TUs whose parse of the initializer calls it, sorted, when that
  /// is not all of the root's TUs; empty when every TU of the root does
  /// (the common case). A header's initializer can call different
  /// functions under different TUs (a macro, an overload declared by an
  /// earlier include), and removeTUs must drop only what a TU added.
  std::vector<std::string> tus;
};

struct StaticInitRoot {
  StaticInitKind kind = StaticInitKind::Variable;
  std::string name; // qualified display name
  std::string usr;
  std::string file;
  unsigned line = 0;
  /// The TUs that define this initializer, sorted. More than one for an
  /// initializer in a header.
  std::vector<std::string> tus;
  /// Functions the initializer expression calls directly, sorted by usr.
  /// Compiler-generated constructors are looked through, to the member and
  /// base constructors they run. Empty for a constructor function: its
  /// calls are the out-edges of its own graph node (`usr`).
  std::vector<StaticInitCallee> callees;
};

struct StaticInitFacts {
  std::vector<StaticInitRoot> roots;
  /// Whether a producer filled these facts at all. A default-constructed
  /// value says nothing about the program (a loaded index); a collected
  /// one with no roots says the program has no dynamic initializers.
  bool collected = false;

  /// Merge the records of one initializer seen in several TUs and put
  /// everything in canonical order: roots by (file, line, name, usr), TU
  /// lists and callees sorted and unique, each callee's TU list empty
  /// when every TU of its root calls it and the strict subset otherwise.
  /// Idempotent. The order in which TUs were collected does not survive
  /// this.
  void canonicalize();

  /// Append another set of facts (a worker shard, a refreshed TU set) and
  /// canonicalize. The result is collected only when both were: facts
  /// missing for some TUs are not facts about the program.
  void absorb(StaticInitFacts &&other);

  /// absorb without the canonicalize, for a loop over many shards: the
  /// facts are not canonical until canonicalize() runs after the last
  /// append, so the loop pays one sort instead of one per shard.
  void append(StaticInitFacts &&other);

  /// Drop the given TUs (raw paths, as the snapshot meta keeps them; the
  /// facts hold index text) from every root and callee, and the roots and
  /// callees left with none. Keeps the canonical form, so absorbing a
  /// fresh bake of the same TUs afterwards equals a clean bake. Returns
  /// how many roots were dropped.
  size_t removeTUs(const std::vector<std::string> &tuPaths);
};

/// Append the static-initialization roots of a parsed TU to `out`
/// (uncanonicalized). Skips system headers, templates, thread-local
/// variables (initialized on first use, not before `main`) and
/// function-local statics.
void collectStaticInits(clang::ASTContext &ctx, const std::string &tuPath,
                        StaticInitFacts &out);

} // namespace vycor
