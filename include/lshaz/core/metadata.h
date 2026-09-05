// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace lshaz {

struct CompilerInfo {
    std::string path;
    std::string version;   // from --version if available
};

struct ExecutionMetadata {
    std::string toolVersion;
    std::string configPath;
    std::string irOptLevel;
    bool irEnabled = true;
    uint64_t timestampEpochSec = 0;
    std::vector<std::string> sourceFiles;
    std::vector<CompilerInfo> compilers;  // one per unique compiler used

    // Parse summary.
    unsigned totalTUs      = 0;
    // What the vocabulary prepass concluded, by name. A count on stderr said
    // how many were derived but never which, leaving the inference less
    // inspectable than the config list it replaced. Emitted here so two scans
    // can be diffed on what the tool decided, not just on what it found.
    std::vector<std::string> derivedAllocators;
    std::vector<std::string> derivedFreers;
    std::vector<std::string> derivedLocks;
    std::vector<std::string> derivedUnlocks;
    std::vector<std::string> derivedMappings;
    std::vector<std::string> derivedAtomicTypes;
    std::vector<std::string> undecidedWrappers;

    unsigned failedTUCount = 0;
    std::vector<std::string> failedTUs;
    std::vector<std::string> failedTUErrors;
};

} // namespace lshaz
