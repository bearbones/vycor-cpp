#pragma once

#include <vector>
namespace clang {
class ASTContext;
}
namespace vycor {
struct Diagnostic;
// TU-local, source-ordered lock scopes plus cycle-safe helper/override paths.
void analyzeCallbacksUnderLock(clang::ASTContext &context,
                               std::vector<Diagnostic> &diagnostics);
} // namespace vycor
