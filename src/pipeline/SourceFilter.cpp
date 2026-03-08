// SPDX-License-Identifier: Apache-2.0
#include "lshaz/pipeline/SourceFilter.h"

#include <fnmatch.h>

#include <algorithm>
#include <set>

namespace lshaz {

bool matchesGlob(const std::string &path, const std::string &pattern) {
    if (pattern.empty())
        return false;
    // FNM_PATHNAME not set: * matches across /. Consistent with
    // hot_file_patterns in lshaz.config.yaml and --include/--exclude CLI.
    return fnmatch(pattern.c_str(), path.c_str(), 0) == 0;
}

static bool isHeader(const std::string &path) {
    auto dot = path.rfind('.');
    if (dot == std::string::npos) return false;
    auto ext = path.substr(dot);
    return ext == ".h" || ext == ".hpp" || ext == ".hxx" ||
           ext == ".hh" || ext == ".inl" || ext == ".inc";
}

static bool endsWith(const std::string &str, const std::string &suffix) {
    if (suffix.size() > str.size()) return false;
    return str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::vector<std::string> filterSources(
        const std::vector<std::string> &sources,
        const FilterOptions &filter) {

    // Incremental mode: restrict to TUs affected by changed files.
    const std::vector<std::string> *effective = &sources;
    std::vector<std::string> changedTUs;
    if (!filter.changedFiles.empty()) {
        bool hasHeader = std::any_of(filter.changedFiles.begin(),
                                     filter.changedFiles.end(), isHeader);
        if (hasHeader) {
            // Conservative: header change could affect any TU.
            // Fall through with all sources.
        } else {
            // Only include TUs whose path ends with a changed file.
            std::set<std::string> changed(filter.changedFiles.begin(),
                                          filter.changedFiles.end());
            for (const auto &src : sources) {
                for (const auto &cf : changed) {
                    if (endsWith(src, cf) || src == cf) {
                        changedTUs.push_back(src);
                        break;
                    }
                }
            }
            effective = &changedTUs;
        }
    }

    std::vector<std::string> result;
    result.reserve(effective->size());

    for (const auto &src : *effective) {
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
