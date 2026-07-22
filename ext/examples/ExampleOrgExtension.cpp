// Example organization extension. NOT compiled where it sits — copy it to
// ext/ (one level up) to activate, then adapt freely. See ext/README.md and
// docs/EXTENDING.md.
//
// Demonstrates the two registration macros:
//   - VYCOR_EXTENSION_SETUP: lock types + feature-flag guard patterns
//   - VYCOR_REGISTER_ANNEAL_CHECK: a custom per-TU anneal check

#include "vycor/ext/Extensions.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/Expr.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/SourceManager.h"

#include <memory>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Declarative hooks: run once at process start, before any analysis.
// ---------------------------------------------------------------------------

VYCOR_EXTENSION_SETUP(ExampleOrgHooks) {
  // Treat our in-house RAII lock wrappers as locks in
  // query_raii_scopes_at_callsite / query_locks_held / query_same_lock.
  registry.addLockTypes({"myorg::SpinLockGuard", "myorg::SharedStateGuard"});

  // Annotate guards whose condition mentions our feature-flag system, so
  // prism/megascope report "reachable only with flag <name> on/off".
  registry.addFeatureFlagPattern("FFlag::([A-Za-z0-9_]+)", /*nameGroup=*/1);
  registry.addFeatureFlagPattern(
      "isFeatureEnabled\\(\"([A-Za-z0-9_.-]+)\"\\)", /*nameGroup=*/1);
}

// ---------------------------------------------------------------------------
// A custom anneal check: forbid direct use of a legacy API outside the
// directory that owns it. One instance per TU; state needs no reset.
// ---------------------------------------------------------------------------

class NoLegacyAllocatorCheck
    : public vycor::AnnealCheck,
      public clang::RecursiveASTVisitor<NoLegacyAllocatorCheck> {
public:
  std::string name() const override { return "no-legacy-allocator"; }

  void checkTU(clang::ASTContext &context, const vycor::GlobalIndex &,
               std::vector<vycor::Diagnostic> &out) override {
    out_ = &out;
    context_ = &context;
    TraverseDecl(context.getTranslationUnitDecl());
  }

  bool VisitCallExpr(clang::CallExpr *expr) {
    const auto *callee = expr->getDirectCallee();
    if (!callee || callee->getQualifiedNameAsString() != "myorg::legacyAlloc")
      return true;

    auto &sm = context_->getSourceManager();
    auto loc = sm.getSpellingLoc(expr->getBeginLoc());

    vycor::Diagnostic diag;
    diag.kind = vycor::Diagnostic::Custom;
    diag.checkName = name();
    diag.callLocation =
        (sm.getFilename(loc) + ":" +
         std::to_string(sm.getSpellingLineNumber(loc)) + ":" +
         std::to_string(sm.getSpellingColumnNumber(loc)))
            .str();
    diag.message = "myorg::legacyAlloc is deprecated; use myorg::Arena "
                   "(see MIGRATION.md)";
    out_->push_back(std::move(diag));
    return true;
  }

private:
  std::vector<vycor::Diagnostic> *out_ = nullptr;
  clang::ASTContext *context_ = nullptr;
};

} // namespace

VYCOR_REGISTER_ANNEAL_CHECK(NoLegacyAllocatorCheck)
