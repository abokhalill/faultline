#pragma once

#include "faultline/core/Diagnostic.h"

#include <string>
#include <vector>

namespace faultline {

class OutputFormatter {
public:
    virtual ~OutputFormatter() = default;
    virtual std::string format(const std::vector<Diagnostic> &diagnostics) = 0;
};

class CLIOutputFormatter : public OutputFormatter {
public:
    std::string format(const std::vector<Diagnostic> &diagnostics) override;
};

class JSONOutputFormatter : public OutputFormatter {
public:
    std::string format(const std::vector<Diagnostic> &diagnostics) override;
};

} // namespace faultline
