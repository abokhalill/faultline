#pragma once

#include "lshaz/core/Diagnostic.h"
#include "lshaz/core/Severity.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace lshaz {

// Per-rule precision policy.
//
// Each rule has a configurable max emission rate (diagnostics per TU),
// minimum confidence floor, and maximum severity cap. Rules exceeding
// their emission budget are demoted to Informational. Rules below
// their confidence floor are suppressed.
//
// Calibration feedback can lower a rule's severity cap if its
// historical false positive rate exceeds the threshold.
struct RulePrecisionPolicy {
    std::string ruleID;
    unsigned maxEmissionsPerTU = 0;      // 0 = unlimited
    double minConfidence       = 0.0;    // suppress below this
    Severity maxSeverity       = Severity::Critical; // cap severity
    double fpRateThreshold     = 0.30;   // auto-demote above this FP rate
};

class PrecisionBudget {
public:
    // Load default policies for all known rules.
    PrecisionBudget();

    // Override policy for a specific rule.
    void setPolicy(const RulePrecisionPolicy &policy);

    // Apply precision governance to diagnostics.
    // - Suppress diagnostics below rule's confidence floor
    // - Cap severity to rule's max severity
    // - Enforce per-rule emission limits
    // - Track emission counts for budget enforcement
    void apply(std::vector<Diagnostic> &diagnostics) const;

    const RulePrecisionPolicy *getPolicy(const std::string &ruleID) const;

private:
    std::unordered_map<std::string, RulePrecisionPolicy> policies_;
};

} // namespace lshaz
