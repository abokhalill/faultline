// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <clang/AST/Decl.h>
#include <clang/AST/Type.h>

#include <string>
#include <vector>

namespace lshaz {

// Codebases that wrap atomics in an opaque struct or typedef -- kernel
// atomic_t and spinlock_t, nginx ngx_atomic_t -- are invisible to the
// std::atomic / _Atomic tests, so `atomic_type_names` in config names them.
//
// That remedy only ever reached rules that happened to construct a
// CacheLineMap, because the names were passed to its constructor and
// nowhere else. FL010, FL011, FL013, FL040 and FL060 all reason about
// atomics and all ignored the configuration: on exactly the C codebases
// the option exists for, five rules stayed blind while four saw. One
// predicate, so a rule cannot silently opt out of a documented feature.
//
// Deliberately name-based and pre-canonicalization: the wrapper is opaque
// by construction, so the spelling is the only evidence available.
inline bool isConfiguredAtomic(clang::QualType QT,
                               const std::vector<std::string> &names) {
    if (QT.isNull() || names.empty())
        return false;

    auto matches = [&](const std::string &n) {
        if (n.empty()) return false;
        for (const auto &cand : names)
            if (cand == n) return true;
        return false;
    };

    // Spelled record name, then the typedef chain, then the canonical
    // record -- a typedef'd anonymous struct only has the last of these.
    if (const auto *RT = QT->getAs<clang::RecordType>())
        if (matches(RT->getDecl()->getNameAsString()))
            return true;

    clang::QualType walk = QT;
    while (const auto *TDT = walk->getAs<clang::TypedefType>()) {
        if (matches(TDT->getDecl()->getNameAsString()))
            return true;
        walk = TDT->desugar();
    }

    if (const auto *RT = QT.getCanonicalType()->getAs<clang::RecordType>())
        if (matches(RT->getDecl()->getNameAsString()))
            return true;

    return false;
}

} // namespace lshaz
