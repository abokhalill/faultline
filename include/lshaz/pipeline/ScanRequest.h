#pragma once

#include "lshaz/core/Config.h"
#include "lshaz/core/Severity.h"
#include "lshaz/core/Diagnostic.h"

#include <cstdint>
#include <string>
#include <vector>

namespace lshaz {

enum class OutputFormat : uint8_t {
    CLI,
    JSON,
    SARIF,
};

struct IROptions {
    bool enabled           = true;
    std::string optLevel   = "O0";   // O0|O1|O2
    bool cacheEnabled      = true;
    unsigned maxJobs       = 0;      // 0 = hardware_concurrency
    unsigned batchSize     = 1;
};

struct FeedbackOptions {
    std::string calibrationStorePath;
    std::string pmuTracePath;
    std::string pmuPriorsPath;
};

struct FilterOptions {
    Severity minSeverity          = Severity::Informational;
    EvidenceTier minEvidenceTier  = EvidenceTier::Speculative;
    std::vector<std::string> includeFiles;
    std::vector<std::string> excludeFiles;
    unsigned maxFiles             = 0;  // 0 = unlimited
};

struct ScanRequest {
    // Compile database path (canonical input).
    std::string compileDBPath;

    // Source files to analyze. If empty, all TUs in compile DB are used.
    std::vector<std::string> sourceFiles;

    // Working directory for relative path resolution.
    std::string workingDirectory;

    Config config;
    IROptions ir;
    FeedbackOptions feedback;
    FilterOptions filter;
    OutputFormat outputFormat = OutputFormat::CLI;

    // Perf profile for hotness-guided analysis.
    std::string perfProfilePath;
    double hotnessThreshold = 1.0;
};

} // namespace lshaz
