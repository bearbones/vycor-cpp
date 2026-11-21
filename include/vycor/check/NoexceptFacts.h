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

#ifndef VYCOR_CHECK_NOEXCEPT_FACTS_H
#define VYCOR_CHECK_NOEXCEPT_FACTS_H

namespace clang {
class ASTContext;
} // namespace clang

namespace vycor {

class GlobalIndex;

// noexcept-candidate, phase 1: walk every function body of a finished
// translation unit and add one NoexceptFactEntry per definition written in
// project code to `index`.
//
// The walk is the proof's ground truth, so it is exact rather than
// name-level: it follows the calls the AST spells out and the ones it does
// not (destructors of temporaries, locals and members, default arguments,
// default member initializers, operator new and delete), and it treats
// anything whose target is not statically known as able to throw. Bodies
// that are not definitions of their own (template instantiations, implicit
// and defaulted members, lambdas, inline functions from system headers) are
// walked here and folded into the functions that reach them.
//
// Called once per TU, after the parse and all pending instantiations.
void collectNoexceptFacts(clang::ASTContext &context, GlobalIndex &index);

} // namespace vycor

#endif // VYCOR_CHECK_NOEXCEPT_FACTS_H
