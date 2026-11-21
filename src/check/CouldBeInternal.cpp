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

#include "vycor/check/CouldBeInternal.h"

#include "llvm/Support/Regex.h"

#include <algorithm>
#include <optional>
#include <set>
#include <tuple>

namespace vycor {

const char *const kDefaultTestPathPattern =
    "(^|/)(test|tests|testing|unittest|unittests)/"
    "|(^|/)test_[^/]*$"
    "|[_-](test|tests|unittest)\\.[^/]*$"
    "|[a-z0-9]Tests?\\.[^/]*$";

namespace {

// Attributes that make a symbol's linkage someone else's decision: the
// language (C linkage, members, entry points), an explicit export or
// retention request, or a template's identity.
constexpr uint32_t kExcludedFlags =
    kSymExternC | kSymVisibility | kSymDllExport | kSymDllImport |
    kSymUsedAttr | kSymWeak | kSymMember | kSymStaticLocal | kSymMain |
    kSymExplicitInstantiation | kSymVirtual | kSymLambda |
    kSymSpecialization;

// A program may replace the global allocation functions; the replacement
// must have external linkage.
bool isReplaceableAllocation(const std::string &name) {
  return name == "operator new" || name == "operator new[]" ||
         name == "operator delete" || name == "operator delete[]";
}

std::string site(const std::string &file, uint32_t line) {
  return file + ":" + std::to_string(line);
}

} // namespace

std::string describeCoverageGap(size_t programTus, size_t coveredProgramTus,
                                const TuOutcomes &outcomes) {
  std::vector<std::string> parts;
  if (programTus == 0)
    parts.push_back("the compilation database does not list the program's "
                    "translation units");
  else if (coveredProgramTus < programTus)
    parts.push_back("the facts cover " + std::to_string(coveredProgramTus) +
                    " of the " + std::to_string(programTus) +
                    " translation units in the compilation database");
  size_t failed = 0, partial = 0;
  for (const auto &entry : outcomes) {
    if (entry.second.status == TuStatus::Partial)
      ++partial;
    else if (entry.second.status != TuStatus::Indexed)
      ++failed;
  }
  if (failed)
    parts.push_back(std::to_string(failed) +
                    " translation unit(s) did not parse");
  if (partial)
    parts.push_back(std::to_string(partial) +
                    " translation unit(s) parsed with errors");
  std::string out;
  for (const auto &part : parts) {
    if (!out.empty())
      out += "; ";
    out += part;
  }
  return out;
}

std::vector<Diagnostic> findCouldBeInternal(const ReferenceIndex &refs,
                                            const CouldBeInternalOptions &opts,
                                            std::string &error) {
  std::vector<Diagnostic> out;
  std::optional<llvm::Regex> testRe;
  if (!opts.testPathPattern.empty()) {
    testRe.emplace(opts.testPathPattern);
    std::string why;
    if (!testRe->isValid(why)) {
      error = "invalid test path pattern '" + opts.testPathPattern +
              "': " + why;
      return out;
    }
  }
  std::string root = opts.projectRoot;
  while (root.size() > 1 && root.back() == '/')
    root.pop_back();
  const auto isTestPath = [&](const std::string &path) {
    if (!testRe)
      return false;
    llvm::StringRef rel(path);
    if (!root.empty() && rel.size() > root.size() && rel.starts_with(root) &&
        rel[root.size()] == '/')
      rel = rel.drop_front(root.size() + 1);
    return testRe->match(rel);
  };
  const std::set<std::string> entryPoints(opts.entryPoints.begin(),
                                          opts.entryPoints.end());
  const std::string unproven =
      opts.coverageGap.empty() ? std::string()
                               : " Not proven: " + opts.coverageGap + ".";

  struct Ordered {
    std::string file;
    uint32_t line;
    std::string name;
    std::string usr;
    Diagnostic diag;
  };
  std::vector<Ordered> found;

  for (const SymbolInfo &sym : refs.allSymbols()) {
    if (sym.kind != SymbolKind::Function && sym.kind != SymbolKind::Variable)
      continue;
    if (sym.linkage != LinkageClass::External || (sym.flags & kExcludedFlags))
      continue;
    // One strong definition in one TU: a symbol defined by several TUs (or
    // by none the facts hold) has no single home to make it internal in.
    if (sym.definingTuCount != 1 || entryPoints.count(sym.name) ||
        isReplaceableAllocation(sym.name))
      continue;

    const SymbolDeclInfo *definition = nullptr;
    const SymbolDeclInfo *headerDecl = nullptr;
    bool definedInHeader = false;
    for (const SymbolDeclInfo &decl : sym.declarations) {
      if (decl.definition) {
        if (decl.inHeader)
          definedInHeader = true;
        else if (!definition)
          definition = &decl;
      } else if (decl.inHeader && !headerDecl) {
        headerDecl = &decl;
      }
    }
    // A definition in a header (or a .inc file) is not the source file's
    // to make internal: every includer would get its own copy.
    if (!definition || definedInHeader)
      continue;

    const std::string &home = sym.firstDefiningTu;
    // Test-support code (a helper defined in a test source) is shared by
    // the tests that use it: their references are ordinary uses.
    const bool homeIsTest = isTestPath(definition->file) || isTestPath(home);
    bool fromOtherTu = false;
    bool fromProduction = false;
    std::string testWitness;
    for (const ReferenceInfo &ref : refs.referencesTo(sym.usr)) {
      if (ref.tuCount <= 1 && ref.firstTu == home)
        continue; // only the defining TU holds it
      fromOtherTu = true;
      // A witness in a test source is test code; a witness elsewhere is
      // test code only when one TU, a test source, holds it.
      const bool test = !homeIsTest &&
                        (isTestPath(ref.file) ||
                         (ref.tuCount == 1 && isTestPath(ref.firstTu)));
      if (!test) {
        fromProduction = true;
        break;
      }
      if (testWitness.empty())
        testWitness = site(ref.file, ref.line);
    }
    if (fromProduction)
      continue;

    const std::string what =
        std::string(sym.kind == SymbolKind::Function ? "function" : "variable") +
        " '" + sym.name + "'";
    Diagnostic diag;
    diag.file = definition->file;
    diag.callLocation = site(definition->file, definition->line);
    diag.resolvedDecl = sym.name;
    diag.entities = {sym.usr};
    if (fromOtherTu) {
      diag.kind = Diagnostic::Internal_TestOnly;
      diag.message = what + " is referenced from other translation units "
                            "only by tests (" +
                     testWitness +
                     "); outside the tests it could have internal linkage.";
    } else if (headerDecl) {
      diag.kind = Diagnostic::Internal_DeclaredInHeader;
      diag.message = what + " is declared in a header (" +
                     site(headerDecl->file, headerDecl->line) +
                     "), but no other translation unit references it; "
                     "without that declaration it could have internal "
                     "linkage.";
    } else {
      diag.kind = unproven.empty() ? Diagnostic::Internal_Unreferenced
                                   : Diagnostic::Internal_Unproven;
      diag.message = what + " has external linkage, but no header declares "
                            "it and no other translation unit references "
                            "it; it could have internal linkage (static, or "
                            "an anonymous namespace).";
    }
    diag.message += unproven;
    found.push_back({definition->file, definition->line, sym.name, sym.usr,
                     std::move(diag)});
  }

  std::sort(found.begin(), found.end(), [](const Ordered &a, const Ordered &b) {
    return std::tie(a.file, a.line, a.name, a.usr) <
           std::tie(b.file, b.line, b.name, b.usr);
  });
  out.reserve(found.size());
  for (Ordered &entry : found)
    out.push_back(std::move(entry.diag));
  return out;
}

} // namespace vycor
