// SPDX-License-Identifier: Apache-2.0
#include "lshaz/core/Diagnostic.h"

#include <clang/Basic/SourceManager.h>

namespace lshaz {

SourceLocation resolveSourceLocation(clang::SourceLocation loc,
                                     const clang::SourceManager &SM) {
    SourceLocation result;
    if (loc.isInvalid())
        return result;

    // For macro-expanded tokens (especially token-pasting via ##),
    // getSpellingLoc() points into <scratch space> — a per-TU virtual
    // buffer whose offsets are non-deterministic across shards.
    // getFileLoc() walks up the entire macro instantiation stack to
    // the physical file where the macro was invoked. For non-macro
    // locations it's a no-op.
    clang::SourceLocation physical = SM.getFileLoc(loc);

    auto presumed = SM.getPresumedLoc(physical);
    if (presumed.isValid()) {
        const char *fn = presumed.getFilename();
        if (fn && fn[0] != '\0') {
            result.file   = fn;
            result.line   = presumed.getLine();
            result.column = presumed.getColumn();
            return result;
        }
    }

    // Fallback: direct file loc (may produce empty file for synthetics).
    result.file   = SM.getFilename(physical).str();
    result.line   = SM.getSpellingLineNumber(physical);
    result.column = SM.getSpellingColumnNumber(physical);
    return result;
}

} // namespace lshaz
