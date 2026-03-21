// SPDX-License-Identifier: Apache-2.0
#include "lshaz/analysis/LshazAction.h"
#include "lshaz/analysis/LshazASTConsumer.h"

#include <clang/Frontend/CompilerInstance.h>

namespace lshaz {

LshazAction::LshazAction(
    const Config &cfg,
    std::vector<Diagnostic> &diagnostics,
    EscapeSummary &escapeSummary,
    const std::unordered_set<std::string> &profileHotFuncs,
    std::vector<FailedTU> &failedTUs)
    : config_(cfg), diagnostics_(diagnostics), escapeSummary_(escapeSummary),
      profileHotFuncs_(profileHotFuncs), failedTUs_(failedTUs) {}

std::unique_ptr<clang::ASTConsumer>
LshazAction::CreateASTConsumer(clang::CompilerInstance & /*CI*/,
                                   llvm::StringRef file) {
    currentFile_ = file.str();
    return std::make_unique<LshazASTConsumer>(
        config_, diagnostics_, escapeSummary_, profileHotFuncs_);
}

void LshazAction::EndSourceFileAction() {
    auto &diags = getCompilerInstance().getDiagnostics();
    if (diags.hasFatalErrorOccurred() || diags.hasUncompilableErrorOccurred()) {
        FailedTU ftu;
        ftu.file = currentFile_;
        // Extract the first fatal error diagnostic message.
        // Note: DiagnosticsEngine doesn't expose iteration; we capture
        // a representative message via the client.
        if (diags.hasErrorOccurred()) {
            ftu.error = "compilation error (see build log for details)";
        } else if (diags.hasFatalErrorOccurred()) {
            ftu.error = "fatal compilation error (see build log for details)";
        }
        failedTUs_.push_back(std::move(ftu));
    }
}

// --- Factory ---

LshazActionFactory::LshazActionFactory(
    const Config &cfg, std::vector<Diagnostic> &diagnostics,
    std::unordered_set<std::string> profileHotFuncs)
    : config_(cfg), diagnostics_(diagnostics),
      profileHotFuncs_(std::move(profileHotFuncs)) {}

std::unique_ptr<clang::FrontendAction> LshazActionFactory::create() {
    return std::make_unique<LshazAction>(
        config_, diagnostics_, escapeSummary_, profileHotFuncs_, failedTUs_);
}

} // namespace lshaz
