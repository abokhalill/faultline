// SPDX-License-Identifier: Apache-2.0
#include "lshaz/core/Diagnostic.h"

#include <clang/Basic/SourceManager.h>

namespace lshaz {

SourceLocation resolveSourceLocation(clang::SourceLocation loc,
                                     const clang::SourceManager &SM) {
    SourceLocation result;
    if (loc.isInvalid())
        return result;

    // getPresumedLoc resolves #line directives and macro expansions,
    // producing a usable filename even for generated/catalog headers
    // where SM.getFilename(getSpellingLoc()) returns empty.
    auto presumed = SM.getPresumedLoc(SM.getSpellingLoc(loc));
    if (presumed.isValid()) {
        const char *fn = presumed.getFilename();
        if (fn && fn[0] != '\0') {
            result.file   = fn;
            result.line   = presumed.getLine();
            result.column = presumed.getColumn();
            return result;
        }
    }

    // Fallback: direct spelling loc (may produce empty file).
    auto spelling = SM.getSpellingLoc(loc);
    result.file   = SM.getFilename(spelling).str();
    result.line   = SM.getSpellingLineNumber(loc);
    result.column = SM.getSpellingColumnNumber(loc);
    return result;
}

} // namespace lshaz
