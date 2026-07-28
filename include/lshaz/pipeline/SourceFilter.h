// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "lshaz/pipeline/ScanRequest.h"

#include <string>
#include <vector>

namespace lshaz {

bool matchesGlob(const std::string &path, const std::string &pattern);

std::vector<std::string> filterSources(
    const std::vector<std::string> &sources,
    const FilterOptions &filter);

// Same, reporting how many sources the vendored-tree filter removed so the
// caller can state it rather than quietly analyzing less than asked.
std::vector<std::string> filterSources(
    const std::vector<std::string> &sources,
    const FilterOptions &filter,
    unsigned &vendoredSkipped);

} // namespace lshaz
