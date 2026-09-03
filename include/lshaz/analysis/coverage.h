// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace lshaz {

// What the scan actually examined.
//
// Most rules only fire on functions the HotPathOracle marks hot. On a
// codebase with no attributes, no configured globs and no --perf-profile,
// nothing is hot and those rules are silent, output identical to a clean
// scan. A scan that looked at a fraction of a codebase must not be
// indistinguishable from one that found nothing.
struct ScanCoverage {
    uint64_t functionsSeen = 0;  // non-system, non-dependent function decls
    uint64_t functionsHot  = 0;  // hot after attribute/glob/profile/call graph
    uint64_t recordsSeen   = 0;

    void merge(const ScanCoverage &o) {
        functionsSeen += o.functionsSeen;
        functionsHot  += o.functionsHot;
        recordsSeen   += o.recordsSeen;
    }
};

} // namespace lshaz
