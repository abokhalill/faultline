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
    // Map-phase verdict, so it counts Candidates the reduce phase has not
    // ruled on yet. Reporting it alone let a scan say "2 hot" on the line
    // after the reducer dropped both and every hot-path rule went silent.
    uint64_t functionsHot  = 0;
    // Distinct functions whose Candidate status the reducer refused, so
    // every withdrawable hot-path finding on them is gone. Counting the
    // reducer's own hot set instead would read as a mass withdrawal on any
    // scan whose hotness came from attributes or globs, which never enter
    // cross-TU resolution at all.
    uint64_t functionsWithdrawnCold = 0;
    uint64_t recordsSeen   = 0;

    void merge(const ScanCoverage &o) {
        functionsSeen += o.functionsSeen;
        functionsHot  += o.functionsHot;
        recordsSeen   += o.recordsSeen;
    }
};

} // namespace lshaz
