// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "lshaz/core/config.h"
#include "lshaz/core/severity.h"
#include "lshaz/core/diagnostic.h"

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

    // Incremental mode: only analyze TUs that match changed files.
    // If any changed file is a header (.h/.hpp/.hxx), all TUs are included
    // (conservative: no dependency graph resolution).
    std::vector<std::string> changedFiles;

    // Vendored third-party trees. On redis these were 36% of all findings,
    // in code the project neither owns nor patches. Directory naming is a
    // convention, not a fact, so the patterns are config-driven rather than
    // policy baked in here — a project whose own module happens to sit under
    // external/ must be able to say so. Never silent: the skipped count is
    // reported and --include-vendored restores them.
    bool skipVendored = true;
    std::vector<std::string> vendorPatterns;
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

    // When false, refuse to run build system commands (cmake, meson, bear)
    // to generate compile_commands.json. Only discover existing ones.
    // Default true for local projects, false for cloned remote repos.
    bool trustBuildSystem = true;

    // Parallel AST analysis. 0 = hardware_concurrency, 1 = sequential.
    unsigned analysisJobs = 0;

    // Per-shard address-space cap, MiB. A template-heavy TU can exceed the
    // box and take unrelated processes with it; capping converts that into
    // one attributable failed shard. 0 = derive from available memory.
    unsigned memoryLimitMB = 0;

    // Perf profile for hotness-guided analysis.
    std::string perfProfilePath;
    double hotnessThreshold = 1.0;
};

} // namespace lshaz
