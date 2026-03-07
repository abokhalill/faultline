// SPDX-License-Identifier: Apache-2.0
#include "lshaz/pipeline/SourceFilter.h"

#include <fnmatch.h>

namespace lshaz {

bool matchesGlob(const std::string &path, const std::string &pattern) {
    if (pattern.empty())
        return false;
    // FNM_PATHNAME not set: * matches across /. Consistent with
    // hot_file_patterns in lshaz.config.yaml and --include/--exclude CLI.
    return fnmatch(pattern.c_str(), path.c_str(), 0) == 0;
}

std::vector<std::string> filterSources(
        const std::vector<std::string> &sources,
        const FilterOptions &filter) {
    std::vector<std::string> result;
    result.reserve(sources.size());

    for (const auto &src : sources) {
        if (!filter.includeFiles.empty()) {
            bool matched = false;
            for (const auto &pat : filter.includeFiles) {
                if (matchesGlob(src, pat)) { matched = true; break; }
            }
            if (!matched) continue;
        }

        bool excluded = false;
        for (const auto &pat : filter.excludeFiles) {
            if (matchesGlob(src, pat)) { excluded = true; break; }
        }
        if (excluded) continue;

        result.push_back(src);

        if (filter.maxFiles > 0 && result.size() >= filter.maxFiles)
            break;
    }
    return result;
}

} // namespace lshaz
