// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "lshaz/analysis/striped_array_summary.h"
#include "lshaz/core/config.h"
#include "lshaz/core/hot_path.h"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>

#include <map>
#include <set>
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
    StripedArrayAnalysis(clang::ASTContext &Ctx, const Config &cfg,
                         const HotPathOracle &oracle)
        : ctx_(Ctx), cfg_(cfg), oracle_(oracle) {}

    // Qualified names of functions handed to a thread primitive. A thread
    // entry's parameter is the thread identity by construction, since that is
    // what pthread_create passed it, so an index derived from it is striping
    // whatever the variable happens to be called.
    void setThreadEntries(std::set<std::string> entries) {
        threadEntries_ = std::move(entries);
    }

    void catalogue(const std::vector<clang::Decl *> &decls);
    void collectAliases(const std::vector<clang::Decl *> &decls);
    void collectUses(const clang::TranslationUnitDecl *TU);

    const StripedArraySummary &summary() const { return summary_; }

private:
    clang::ASTContext &ctx_;
    const Config &cfg_;
    const HotPathOracle &oracle_;
    StripedArraySummary summary_;
    // `T *p = &arr[K]` retargets subscripts through p onto arr; K>0 with a
    // line-aligned base is the only head padding that actually offsets
    // element 0 within its line.
    std::map<std::string, std::string> aliases_;
    std::set<std::string> threadEntries_;
};

} // namespace lshaz
