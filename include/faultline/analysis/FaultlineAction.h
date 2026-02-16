#pragma once

#include "faultline/core/Config.h"
#include "faultline/core/Diagnostic.h"

#include <clang/Frontend/FrontendAction.h>
#include <clang/Tooling/Tooling.h>

#include <memory>
#include <vector>

namespace faultline {

class FaultlineAction : public clang::ASTFrontendAction {
public:
    explicit FaultlineAction(const Config &cfg,
                             std::vector<Diagnostic> &diagnostics);

    std::unique_ptr<clang::ASTConsumer>
    CreateASTConsumer(clang::CompilerInstance &CI,
                      llvm::StringRef file) override;

private:
    const Config &config_;
    std::vector<Diagnostic> &diagnostics_;
};

class FaultlineActionFactory : public clang::tooling::FrontendActionFactory {
public:
    explicit FaultlineActionFactory(const Config &cfg,
                                   std::vector<Diagnostic> &diagnostics);

    std::unique_ptr<clang::FrontendAction> create() override;

private:
    const Config &config_;
    std::vector<Diagnostic> &diagnostics_;
};

} // namespace faultline
