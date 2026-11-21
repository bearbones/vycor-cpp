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
// (BakedIndexes::staticInits). They are held in memory only: the saved
// index does not carry them, so they exist for an in-memory bake and not
// for a loaded index.

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
  /// For a callee instantiated from a template, the identity of the
  /// template definition it was instantiated from; empty otherwise. The
  /// graph holds the definition's calls, not the instantiation's.
  std::string pattern;
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
  /// lists and callees sorted and unique. Idempotent. The order in which
  /// TUs were collected does not survive this.
  void canonicalize();
};

/// Append the static-initialization roots of a parsed TU to `out`
/// (uncanonicalized). Skips system headers, templates, thread-local
/// variables (initialized on first use, not before `main`) and
/// function-local statics.
void collectStaticInits(clang::ASTContext &ctx, const std::string &tuPath,
                        StaticInitFacts &out);

} // namespace vycor
