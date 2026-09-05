// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "lshaz/analysis/thread_role.h"

#include <clang/AST/ASTContext.h>
#include <clang/Frontend/FrontendAction.h>
#include <clang/Tooling/Tooling.h>

#include <memory>
#include <string>
#include <vector>

namespace lshaz {

// Pass one of the scan. Parses each TU and records structure only: which
// function forwards a call's result out, which hands a parameter onward, and
// which types those calls produced or consumed. No rules, no layout, no escape
// analysis, no IR.
//
// It exists because a project's allocator, lock and mapping vocabulary is not
// knowable inside one TU: zmalloc's body is in zmalloc.c and every caller is
// somewhere else. Requiring the vocabulary in config instead made a human
// rediscover it per codebase, and that trap has now cost five subsystems.
// The one implementation both passes use. Pass one calls it from its own
// consumer; pass two calls it inline until the shard driver is parameterised
// on the action factory, at which point this call site goes away.
void collectAllocOwnership(clang::ASTContext &Ctx, ThreadRoleSummary &out);

class VocabularyAction : public clang::ASTFrontendAction {
public:
    explicit VocabularyAction(ThreadRoleSummary &out) : out_(out) {}

    std::unique_ptr<clang::ASTConsumer>
    CreateASTConsumer(clang::CompilerInstance &CI,
                      llvm::StringRef file) override;

private:
    ThreadRoleSummary &out_;
};

class VocabularyActionFactory : public clang::tooling::FrontendActionFactory {
public:
    std::unique_ptr<clang::FrontendAction> create() override {
        return std::make_unique<VocabularyAction>(facts_);
    }
    const ThreadRoleSummary &facts() const { return facts_; }

private:
    ThreadRoleSummary facts_;
};

} // namespace lshaz
