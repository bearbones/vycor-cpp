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
#include <vector>

namespace clang {
class ASTContext;
class FunctionDecl;
class VarDecl;
} // namespace clang

namespace vycor {

// ============================================================================
// EmissionGraph — what one translation unit's code generation emits.
//
// The TU's reference graph over function definitions, walked the way Clang's
// CodeGen decides what to emit: from what the TU must emit (strong
// definitions, the global variables ASTContext::DeclMustBeEmitted keeps,
// vtables the TU homes) through every function a walked body references.
// A body is walked as CodeGen lowers it: unevaluated operands, values the
// compiler computed, the discarded branch of `if constexpr` and the branch
// a folded condition never takes are no code; a virtual call reaches its
// target only where CodeGen devirtualizes it; a constructor or destructor
// that stores the vtable pointer of a class without a key function emits
// that vtable and every function in its slots. A function is *emitted* by
// the TU when it is a node with a body: CodeGen hands its body to the
// optimizer, which may inline it into its callers and drop the out-of-line
// copy, but not the code it references.
//
// Shared by the isa-leak check (body bits per emitted function) and the
// megascope bake's emission facts (ReferenceVisitor, docs/symbol-references.md
// "Emission facts").
// ============================================================================

// What a body holds that code-generation flags can change (isa-leak).
enum EmittedBodyBit : uint32_t {
  kBodyLoop = 1u << 0,          // a loop: a vectorization candidate
  kBodyFloat = 1u << 1,         // floating-point arithmetic, comparison or
                                // conversion (what FP flags change)
  kBodyVector = 1u << 2,        // vector-typed values (intrinsics, ext vectors)
  kBodyBulkMemory = 1u << 3,    // memcpy / memset / memmove
  kBodyBitBuiltin = 1u << 4,    // popcount / clz / ctz / bswap / rotate
  kBodyTargetBuiltin = 1u << 5, // a target-specific builtin (_mm_* and kin)
  kBodyCleanup = 1u << 6,       // a local or temporary with a destructor
  kBodyMayThrowCall = 1u << 7,  // a call not declared non-throwing
  kBodyTry = 1u << 8,           // a try block
  kBodySignedArith = 1u << 9,   // signed integer arithmetic
  kBodyTargetAttr = 1u << 10,   // own target / target_clones / cpu_* attribute
  kBodyMathCall = 1u << 11,     // a floating-point math builtin (sqrt, exp...)
  kBodyFloatMove = 1u << 12,    // a floating-point value at all, even only
                                // loaded, passed or returned (VEX under -mavx)
};

// EmissionGraph::edgeFlags bits.
enum EmissionEdgeFlag : uint8_t {
  // The source's body can inline the destination's (a direct call, not a
  // vtable slot or an atexit registration).
  kEmissionEdgeInlinable = 1,
};

struct EmissionGraph {
  // Struct of arrays, one entry per node. Node 0 is the TU itself; its
  // edges are the roots. A function node holds the definition CodeGen
  // emits, or null when the TU has no body for it; a variable node (a
  // global whose initialization or destruction runs code, or whose
  // constant data names functions) holds the variable and no function.
  std::vector<const clang::FunctionDecl *> function;
  std::vector<const clang::VarDecl *> variable;
  // EmittedBodyBit of the node's own body.
  std::vector<uint32_t> bits;
  // Statements and expressions CodeGen lowers for the node's body: the
  // AST nodes the walk visits, implicit ones (conversions, temporaries,
  // default arguments, member initializers) included, the code a folded
  // condition discards excluded. A size proxy for the emitted code.
  std::vector<uint32_t> statements;
  // Edges in CSR form: node i's are [edgeBegin[i], edgeBegin[i + 1]).
  std::vector<uint32_t> edgeBegin;
  std::vector<uint32_t> edgeDst;
  std::vector<uint8_t> edgeFlags; // EmissionEdgeFlag bits

  [[nodiscard]] uint32_t size() const noexcept {
    return static_cast<uint32_t>(function.size());
  }
};

// Walk `ctx`'s translation unit. C and C++ alike.
[[nodiscard]] EmissionGraph buildEmissionGraph(clang::ASTContext &ctx);

// A target / target_clones / target_version / cpu_specific / cpu_dispatch
// attribute: the body is compiled for its own feature set.
[[nodiscard]] bool hasTargetAttribute(const clang::FunctionDecl *fd);

} // namespace vycor
