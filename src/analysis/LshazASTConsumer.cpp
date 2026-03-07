#include "lshaz/analysis/LshazASTConsumer.h"
#include "lshaz/analysis/StructLayoutVisitor.h"
#include "lshaz/core/RuleRegistry.h"

#include <clang/AST/Decl.h>
#include <clang/AST/DeclGroup.h>
#include <clang/Basic/SourceManager.h>

#include <unordered_set>

namespace lshaz {

namespace {

bool isInSystemHeader(const clang::Decl *D, const clang::SourceManager &SM) {
    auto loc = D->getLocation();
    if (loc.isInvalid())
        return true;
    return SM.isInSystemHeader(SM.getSpellingLoc(loc));
}

} // anonymous namespace

LshazASTConsumer::LshazASTConsumer(
    const Config &cfg,
    std::vector<Diagnostic> &diagnostics,
    const std::unordered_set<std::string> &profileHotFuncs)
    : config_(cfg), oracle_(cfg), diagnostics_(diagnostics) {
    if (!profileHotFuncs.empty())
        oracle_.loadProfileHotFunctions(profileHotFuncs);
}

void LshazASTConsumer::HandleTranslationUnit(clang::ASTContext &Ctx) {
    auto *TU = Ctx.getTranslationUnitDecl();
    const auto &SM = Ctx.getSourceManager();

    // Collect all non-system declarations, recursing into namespaces
    // and linkage-spec blocks (extern "C").
    std::vector<clang::Decl *> decls;
    std::function<void(clang::DeclContext *)> collect =
        [&](clang::DeclContext *DC) {
            for (auto *D : DC->decls()) {
                if (isInSystemHeader(D, SM))
                    continue;
                if (auto *NS = llvm::dyn_cast<clang::NamespaceDecl>(D)) {
                    collect(NS);
                    continue;
                }
                if (auto *LS = llvm::dyn_cast<clang::LinkageSpecDecl>(D)) {
                    collect(LS);
                    continue;
                }
                decls.push_back(D);
            }
        };
    collect(TU);

    std::unordered_set<std::string> disabled(config_.disabledRules.begin(),
                                              config_.disabledRules.end());

    // First pass: seed hot-path oracle.
    for (auto *D : decls) {
        if (auto *FD = llvm::dyn_cast<clang::FunctionDecl>(D))
            oracle_.isFunctionHot(FD);
    }

    // Second pass: run enabled rules.
    const auto &rules = RuleRegistry::instance().rules();
    for (auto *D : decls) {
        for (const auto &rule : rules) {
            if (disabled.count(std::string(rule->getID())))
                continue;
            rule->analyze(D, Ctx, oracle_, config_, diagnostics_);
        }
    }
}

} // namespace lshaz
