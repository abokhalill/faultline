#pragma once

#include "lshaz/core/Config.h"
#include "lshaz/core/Diagnostic.h"

#include <clang/Frontend/FrontendAction.h>
#include <clang/Tooling/Tooling.h>

#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace lshaz {

class LshazAction : public clang::ASTFrontendAction {
public:
    LshazAction(const Config &cfg,
                    std::vector<Diagnostic> &diagnostics,
                    const std::unordered_set<std::string> &profileHotFuncs);

    std::unique_ptr<clang::ASTConsumer>
    CreateASTConsumer(clang::CompilerInstance &CI,
                      llvm::StringRef file) override;

private:
    const Config &config_;
    std::vector<Diagnostic> &diagnostics_;
    const std::unordered_set<std::string> &profileHotFuncs_;
};

class LshazActionFactory : public clang::tooling::FrontendActionFactory {
public:
    LshazActionFactory(const Config &cfg,
                           std::vector<Diagnostic> &diagnostics,
                           std::unordered_set<std::string> profileHotFuncs = {});

    std::unique_ptr<clang::FrontendAction> create() override;

private:
    const Config &config_;
    std::vector<Diagnostic> &diagnostics_;
    std::unordered_set<std::string> profileHotFuncs_;
};

} // namespace lshaz
