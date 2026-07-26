// SPDX-License-Identifier: Apache-2.0
#include "lshaz/analysis/StripedArraySummary.h"

namespace lshaz {

StripeVerdict gradeStripedArray(const StripedArraySite &s,
                                const ThreadRoleVerdicts &roles,
                                uint64_t lineBytes) {
    StripeVerdict v;
    v.mitigation = classifyStripeMitigation(s, lineBytes);
    v.writerCount = static_cast<unsigned>(s.stripedWriters.size());

    if (s.elemSizeBytes > 0 && s.elemSizeBytes < lineBytes) {
        v.slotsPerLine = lineBytes / s.elemSizeBytes;
        v.contendedLines =
            (s.elemCount + v.slotsPerLine - 1) / v.slotsPerLine;
    }

    // Any unattributed writer collapses the mask: a partial attribution
    // cannot prove two roles touch this array.
    uint8_t mask = 0;
    for (const auto &w : s.stripedWriters) {
        uint8_t r = roles.roleOf(w);
        if (r == ROLE_NONE) { mask = ROLE_NONE; break; }
        mask |= r;
    }
    v.writerRoles = mask;
    v.multiRole = (mask == (ROLE_MAIN | ROLE_WORKER));
    return v;
}

} // namespace lshaz
