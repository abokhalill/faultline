// SPDX-License-Identifier: Apache-2.0
#include "lshaz/core/Diagnostic.h"

#include <algorithm>
#include <sstream>

namespace lshaz {

std::string Diagnostic::serializeEvidence() const {
    // Sort keys for deterministic output.
    std::vector<std::string> keys;
    keys.reserve(structuralEvidence.size());
    for (const auto &[k, _] : structuralEvidence)
        keys.push_back(k);
    std::sort(keys.begin(), keys.end());

    std::ostringstream os;
    bool first = true;
    for (const auto &k : keys) {
        if (!first) os << "; ";
        os << k << "=" << structuralEvidence.at(k);
        first = false;
    }
    return os.str();
}

} // namespace lshaz
