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
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallString.h"

#include <string>

namespace vycor {

// Node identity pair (design doc docs/design-f8-usr-identity.md): `usr` is
// the canonical identity that keys the graph (unique, cross-TU-stable,
// signature-encoding); `display` is the human-facing qualified name.
struct Ident {
  std::string usr;
  std::string display;
};

// Per-visitor (per-TU) memo for (usr, display) generation. USR generation
// allocates and the visitors resolve the same decls repeatedly, so the pair
// is computed once per Decl* per visitor instance.
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
  llvm::DenseMap<const clang::Decl *, Ident> memo_;
};

} // namespace vycor
