#include "lshaz/analysis/LshazAction.h"
#include "lshaz/analysis/LshazASTConsumer.h"

#include <clang/Frontend/CompilerInstance.h>

namespace lshaz {

LshazAction::LshazAction(
    const Config &cfg,
    std::vector<Diagnostic> &diagnostics,
    const std::unordered_set<std::string> &profileHotFuncs)
    : config_(cfg), diagnostics_(diagnostics),
      profileHotFuncs_(profileHotFuncs) {}

std::unique_ptr<clang::ASTConsumer>
LshazAction::CreateASTConsumer(clang::CompilerInstance & /*CI*/,
                                   llvm::StringRef /*file*/) {
    return std::make_unique<LshazASTConsumer>(
        config_, diagnostics_, profileHotFuncs_);
}

// --- Factory ---

LshazActionFactory::LshazActionFactory(
    const Config &cfg, std::vector<Diagnostic> &diagnostics,
    std::unordered_set<std::string> profileHotFuncs)
    : config_(cfg), diagnostics_(diagnostics),
      profileHotFuncs_(std::move(profileHotFuncs)) {}

std::unique_ptr<clang::FrontendAction> LshazActionFactory::create() {
    return std::make_unique<LshazAction>(
        config_, diagnostics_, profileHotFuncs_);
}

} // namespace lshaz
