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

#include "clang/AST/Decl.h"
#include "clang/Index/USRGeneration.h"
#include "llvm/ADT/SmallString.h"

#include <string>
#include <unordered_map>

namespace clang {
class SourceManager;
} // namespace clang

namespace vycor {

// Node identity pair (design doc docs/design-f8-usr-identity.md): `usr` is
// the canonical identity that keys the graph (unique, cross-TU-stable,
// signature-encoding); `display` is the human-facing qualified name.
struct Ident {
  std::string usr;
  std::string display;
};

// The function a throw expression is recorded as calling: the runtime
// entry point the compiler emits a call to (`__cxa_throw`, or
// `__cxa_rethrow` for a bare `throw;`). No declaration of it is in the
// TU, so the identity is spelled here: the USR a declaration of that C
// function would have.
inline Ident throwCalleeIdent(bool rethrow) {
  return rethrow ? Ident{"c:@F@__cxa_rethrow", "__cxa_rethrow"}
                 : Ident{"c:@F@__cxa_throw", "__cxa_throw"};
}

// Per-visitor (per-TU) memo for (usr, display) generation. USR generation
// allocates and the visitors resolve the same decls repeatedly, so the pair
// is computed once per Decl* per visitor instance. Uses std::unordered_map
// so returned `const Ident &` references remain valid across rehashes.
class UsrCache {
public:
  const Ident &identFor(const clang::NamedDecl *decl) {
    auto it = memo_.find(decl);
    if (it != memo_.end())
      return it->second;
    Ident id;
    id.display = decl->getQualifiedNameAsString();
    llvm::SmallString<128> buf;
    // generateUSRForDecl returns true on FAILURE.
    if (clang::index::generateUSRForDecl(decl, buf))
      id.usr = "vycor-synth:" + id.display;
    else
      id.usr = std::string(buf.str());
    return memo_.try_emplace(decl, std::move(id)).first->second;
  }

private:
  std::unordered_map<const clang::Decl *, Ident> memo_;
};

// Identity of a lambda's call operator, the one the call graph gives its
// node: the closure's spelling location plus the qualified name of the
// nearest enclosing function that is not itself a lambda ("" at file scope).
// `loc` is the closure class's begin location.
Ident lambdaClosureIdent(clang::SourceManager &sm, clang::SourceLocation loc,
                         const std::string &enclosing);

// The enclosing-function name a lambda's identity carries: the qualified
// name, with the template arguments of a function template instantiation
// (`each<int>`), whose closure is a type of its own: two instantiations'
// lambdas are two nodes. Code as written (a template's definition, an
// explicit specialization) is named as before.
std::string lambdaEnclosingName(const clang::FunctionDecl *fn);

} // namespace vycor
