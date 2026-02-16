#pragma once

#include "faultline/core/Severity.h"

#include <cstddef>
#include <string>
#include <vector>

namespace faultline {

struct Config {
    // Cache model
    size_t cacheLineBytes       = 64;
    size_t cacheLineSpanWarn    = 64;   // FL001 threshold
    size_t cacheLineSpanCrit    = 128;  // FL001 escalation

    // Stack frame
    size_t stackFrameWarnBytes  = 2048; // FL021 threshold

    // Allocation
    size_t allocSizeEscalation  = 256;  // FL020 escalation

    // Branch depth
    unsigned branchDepthWarn    = 4;    // FL050 threshold

    // Minimum severity to emit
    Severity minSeverity        = Severity::Informational;

    // Output
    bool jsonOutput             = false;
    std::string outputFile;             // empty = stdout

    // Hot path patterns (fnmatch-style)
    std::vector<std::string> hotFunctionPatterns;
    std::vector<std::string> hotFilePatterns;

    // Rule enable/disable
    std::vector<std::string> disabledRules;

    // TLB
    size_t pageSize             = 4096;

    static Config loadFromFile(const std::string &path);
    static Config defaults();
};

} // namespace faultline
