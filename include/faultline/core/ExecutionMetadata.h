#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace faultline {

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
};

} // namespace faultline
