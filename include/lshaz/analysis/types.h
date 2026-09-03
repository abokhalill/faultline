// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <clang/AST/Type.h>

namespace lshaz {

// Strip array extents down to the element type.
//
// A field declared `_Atomic uint64_t c[N]` or `std::atomic<T> slots[N]` has
// field type ArrayType(element), so every predicate that inspects the field
// type directly sees an array and not an atomic. Arrays of atomics are the
// dominant striped-counter shape in threaded servers, which made them
// invisible to atomic, sync and volatile detection alike, and therefore to
// every rule gated on those.
inline clang::QualType peelArrays(clang::QualType QT) {
    while (const clang::ArrayType *AT = QT->getAsArrayTypeUnsafe())
        QT = AT->getElementType();
    return QT;
}

} // namespace lshaz
