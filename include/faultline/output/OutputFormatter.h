#pragma once

#include "faultline/core/Diagnostic.h"
#include "faultline/core/ExecutionMetadata.h"

#include <string>
#include <vector>

namespace faultline {

class OutputFormatter {
public:
    virtual ~OutputFormatter() = default;
    virtual std::string format(const std::vector<Diagnostic> &diagnostics) = 0;
    virtual std::string format(const std::vector<Diagnostic> &diagnostics,
                               const ExecutionMetadata &meta) {
        return format(diagnostics);
    }
};

class CLIOutputFormatter : public OutputFormatter {
public:
    std::string format(const std::vector<Diagnostic> &diagnostics) override;
};

class JSONOutputFormatter : public OutputFormatter {
public:
    std::string format(const std::vector<Diagnostic> &diagnostics) override;
    std::string format(const std::vector<Diagnostic> &diagnostics,
                       const ExecutionMetadata &meta) override;
};

class SARIFOutputFormatter : public OutputFormatter {
public:
    std::string format(const std::vector<Diagnostic> &diagnostics) override;
    std::string format(const std::vector<Diagnostic> &diagnostics,
                       const ExecutionMetadata &meta) override;
};

} // namespace faultline
