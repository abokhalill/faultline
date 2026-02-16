#pragma once

#include "faultline/core/Severity.h"

#include <string>
#include <vector>

namespace faultline {

struct SourceLocation {
    std::string file;
    unsigned line   = 0;
    unsigned column = 0;
};

struct Diagnostic {
    std::string    ruleID;
    std::string    title;
    Severity       severity     = Severity::Informational;
    double         confidence   = 0.0; // [0.0, 1.0]
    SourceLocation location;
    std::string    hardwareReasoning;
    std::string    structuralEvidence;
    std::string    mitigation;

    // Escalation trace: why severity was raised from base.
    std::vector<std::string> escalations;
};

} // namespace faultline
