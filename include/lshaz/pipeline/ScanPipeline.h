#pragma once

#include "lshaz/pipeline/ScanRequest.h"
#include "lshaz/pipeline/ScanResult.h"

#include <functional>
#include <string>
#include <vector>

namespace clang { namespace tooling { class CompilationDatabase; } }

namespace lshaz {

using ProgressCallback = std::function<void(const std::string &stage,
                                            const std::string &detail)>;

class ScanPipeline {
public:
    explicit ScanPipeline(ProgressCallback progress = nullptr);

    // Primary entry point: loads compile DB from request.compileDBPath.
    ScanResult execute(const ScanRequest &request);

    // Legacy entry point: accepts a pre-existing CompilationDatabase.
    // Used by the existing CLI which obtains its DB via CommonOptionsParser.
    ScanResult executeLegacy(
        const ScanRequest &request,
        const clang::tooling::CompilationDatabase &compDB,
        const std::vector<std::string> &sources);

private:
    ProgressCallback progress_;

    void report(const std::string &stage, const std::string &detail) const;

    // Shared implementation: both execute() and executeLegacy() converge here.
    ScanResult run(const ScanRequest &request,
                   const clang::tooling::CompilationDatabase &compDB,
                   const std::vector<std::string> &sources);
};

} // namespace lshaz
