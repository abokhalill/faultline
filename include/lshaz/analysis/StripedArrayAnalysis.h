// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "lshaz/analysis/StripedArraySummary.h"
#include "lshaz/core/Config.h"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>

#include <map>
#include <string>
#include <vector>

namespace lshaz {

// "<Type>::<field>" | "<defining-file>::<var>" (internal linkage) |
// "::<var>" (external). Internal-linkage names collide across TUs.
std::string stripedKeyForDecl(const clang::ValueDecl *D,
                              clang::ASTContext &Ctx);

// Collects striped-array facts in one traversal. Declaration side runs
// over the caller's already-collected decls; use side walks function
// bodies once.
class StripedArrayAnalysis {
public:
    StripedArrayAnalysis(clang::ASTContext &Ctx, const Config &cfg)
        : ctx_(Ctx), cfg_(cfg) {}

    void catalogue(const std::vector<clang::Decl *> &decls);
    void collectAliases(const std::vector<clang::Decl *> &decls);
    void collectUses(const clang::TranslationUnitDecl *TU);

    const StripedArraySummary &summary() const { return summary_; }

private:
    clang::ASTContext &ctx_;
    const Config &cfg_;
    StripedArraySummary summary_;
    // `T *p = &arr[K]` retargets subscripts through p onto arr; K>0 with a
    // line-aligned base is the only head padding that actually offsets
    // element 0 within its line.
    std::map<std::string, std::string> aliases_;
};

} // namespace lshaz
