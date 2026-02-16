#include "faultline/analysis/FaultlineAction.h"
#include "faultline/analysis/FaultlineASTConsumer.h"

#include <clang/Frontend/CompilerInstance.h>

namespace faultline {

FaultlineAction::FaultlineAction(const Config &cfg,
                                 std::vector<Diagnostic> &diagnostics)
    : config_(cfg), diagnostics_(diagnostics) {}

std::unique_ptr<clang::ASTConsumer>
FaultlineAction::CreateASTConsumer(clang::CompilerInstance & /*CI*/,
                                   llvm::StringRef /*file*/) {
    return std::make_unique<FaultlineASTConsumer>(config_, diagnostics_);
}

// --- Factory ---

FaultlineActionFactory::FaultlineActionFactory(
    const Config &cfg, std::vector<Diagnostic> &diagnostics)
    : config_(cfg), diagnostics_(diagnostics) {}

std::unique_ptr<clang::FrontendAction> FaultlineActionFactory::create() {
    return std::make_unique<FaultlineAction>(config_, diagnostics_);
}

} // namespace faultline
