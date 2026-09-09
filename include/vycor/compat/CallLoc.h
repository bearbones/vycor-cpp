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

#ifndef VYCOR_COMPAT_CALL_LOC_H
#define VYCOR_COMPAT_CALL_LOC_H

#include "clang/AST/Expr.h"
#include "clang/AST/Stmt.h"
#include "clang/Basic/SourceLocation.h"

namespace vycor {

// Where a call expression starts as spelled: `f(x)` -> `f`, `p->f()` and
// `cb(1)` -> the receiver `p` / `cb`.
//
// Clang 21 turned CallExpr::getBeginLoc into a non-dispatching inline that
// answers with the callee token. For an operator call the callee is the
// implicit `operator()` / `operator->` reference, spelled at the `(` or
// `->`, so `cb(1)` reported column 66 where Clang 18/20 reported 64 (they
// forwarded to CXXOperatorCallExpr::getBeginLoc, which starts at the
// receiver). Dispatching through Stmt::getBeginLoc reaches the most-derived
// override on every version, so call-site locations in the index do not
// depend on the Clang the index was baked with.
inline clang::SourceLocation callBeginLoc(const clang::CallExpr *call) {
  return static_cast<const clang::Stmt *>(call)->getBeginLoc();
}

} // namespace vycor

#endif // VYCOR_COMPAT_CALL_LOC_H
