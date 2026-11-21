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

#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"

// The code the compiler writes on its own, as both bake visitors read it:
// the call-graph edge visitor (CallGraphBuilder) and the symbol reference
// visitor (ReferenceVisitor). One enumeration, so a constructor or
// destructor the compiler calls is the same call in both.

namespace vycor {

// A special member the compiler defines: implicit, or defaulted on its
// first declaration.
[[nodiscard]] inline bool
isCompilerWrittenSpecialMember(const clang::FunctionDecl *fn) {
  // The operator== a defaulted operator<=> declares, member or friend.
  if (fn->isImplicit() && fn->isDefaulted())
    return true;
  // `using Base::Base;` declares derived constructors the compiler writes.
  if (const auto *ctor = llvm::dyn_cast<clang::CXXConstructorDecl>(fn))
    if (ctor->isImplicit() && ctor->isInheritingConstructor())
      return true;
  const auto *method = llvm::dyn_cast<clang::CXXMethodDecl>(fn);
  if (!method || method->isUserProvided())
    return false;
  if (llvm::isa<clang::CXXConstructorDecl>(method) ||
      llvm::isa<clang::CXXDestructorDecl>(method))
    return true;
  return method->isCopyAssignmentOperator() ||
         method->isMoveAssignmentOperator();
}

// The definition whose body an instantiation or implicit member holds, or
// null when there is none to read.
[[nodiscard]] inline const clang::FunctionDecl *
readableDefinition(const clang::FunctionDecl *fn) {
  const clang::FunctionDecl *def = nullptr;
  if (!fn->isDefined(def) || !def || def->isInvalidDecl() ||
      def->isDependentContext() || def->isDeleted())
    return nullptr;
  return def;
}

// The destructor that destroying an object of `type` (an array: each
// element) runs, or null when it runs none: a trivial destructor, a
// reference, a dependent or incomplete type, a deleted destructor.
[[nodiscard]] inline const clang::CXXDestructorDecl *
destructorRunFor(const clang::ASTContext &ctx, clang::QualType type) {
  if (type.isNull() || type->isReferenceType() ||
      type->isInstantiationDependentType())
    return nullptr;
  const clang::CXXRecordDecl *cls =
      ctx.getBaseElementType(type)->getAsCXXRecordDecl();
  if (!cls || !cls->hasDefinition())
    return nullptr;
  cls = cls->getDefinition();
  if (cls->isInvalidDecl() || cls->isDependentContext() ||
      cls->hasTrivialDestructor())
    return nullptr;
  const clang::CXXDestructorDecl *dtor = cls->getDestructor();
  return dtor && !dtor->isDeleted() ? dtor : nullptr;
}

// After its body a destructor destroys the members, then the bases (the
// complete-object destructor also the virtual bases): `visit(type)` for
// each, in that order. Returns false, visiting nothing, for a class whose
// layout cannot be read (incomplete, invalid, dependent) or a union.
template <typename Visit>
bool forEachDestroyedSubobject(const clang::CXXDestructorDecl *dtor,
                               Visit &&visit) {
  const clang::CXXRecordDecl *cls = dtor->getParent();
  if (!cls->hasDefinition())
    return false;
  cls = cls->getDefinition();
  if (cls->isInvalidDecl() || cls->isDependentContext() || cls->isUnion())
    return false;
  for (const clang::FieldDecl *field : cls->fields())
    visit(field->getType());
  for (const clang::CXXBaseSpecifier &base : cls->bases())
    if (!base.isVirtual())
      visit(base.getType());
  for (const clang::CXXBaseSpecifier &base : cls->vbases())
    visit(base.getType());
  return true;
}

namespace detail {

// The members of `cls` and of its nested classes that are explicit
// instantiation definitions.
template <typename Visit>
void forEachExplicitMember(const clang::CXXRecordDecl *cls, Visit &visit) {
  if (cls->isInvalidDecl() || cls->isDependentContext())
    return;
  for (const clang::Decl *member : cls->decls()) {
    if (const auto *nested = llvm::dyn_cast<clang::CXXRecordDecl>(member)) {
      if (!nested->isInjectedClassName() &&
          nested->isThisDeclarationADefinition())
        forEachExplicitMember(nested, visit);
      continue;
    }
    // Member templates: `template void Box<A>::put<int>(int);`,
    // `template void Outer<A>::In<int>::f();`.
    if (const auto *tmpl =
            llvm::dyn_cast<clang::FunctionTemplateDecl>(member)) {
      for (const clang::FunctionDecl *spec : tmpl->specializations())
        if (spec->getTemplateSpecializationKind() ==
            clang::TSK_ExplicitInstantiationDefinition)
          visit(spec);
      continue;
    }
    if (const auto *tmpl = llvm::dyn_cast<clang::ClassTemplateDecl>(member)) {
      for (const clang::ClassTemplateSpecializationDecl *spec :
           tmpl->specializations())
        if (const clang::CXXRecordDecl *def = spec->getDefinition())
          forEachExplicitMember(def, visit);
      continue;
    }
    const auto *method = llvm::dyn_cast<clang::CXXMethodDecl>(member);
    if (method && !method->isImplicit() &&
        method->getTemplateSpecializationKind() ==
            clang::TSK_ExplicitInstantiationDefinition)
      visit(method);
  }
}

} // namespace detail

// The functions an explicit instantiation definition of (a specialization
// of) `tmpl` makes the TU emit, called or not: `visit(fn)` for each. Found
// through the template's specializations, since an explicit instantiation
// of a function or of a member leaves no declaration in the enclosing
// context. Visits nothing unless `tmpl` is the canonical declaration, so a
// redeclared template is enumerated once.
template <typename Visit>
void forEachExplicitInstantiation(const clang::FunctionTemplateDecl *tmpl,
                                  Visit &&visit) {
  if (tmpl != tmpl->getCanonicalDecl())
    return;
  for (const clang::FunctionDecl *spec : tmpl->specializations())
    if (spec->getTemplateSpecializationKind() ==
        clang::TSK_ExplicitInstantiationDefinition)
      visit(spec);
}

template <typename Visit>
void forEachExplicitInstantiation(const clang::ClassTemplateDecl *tmpl,
                                  Visit &&visit) {
  if (tmpl != tmpl->getCanonicalDecl())
    return;
  for (const clang::ClassTemplateSpecializationDecl *spec :
       tmpl->specializations())
    if (const clang::CXXRecordDecl *def = spec->getDefinition())
      detail::forEachExplicitMember(def, visit);
}

} // namespace vycor
