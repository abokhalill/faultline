#pragma once

#include "faultline/hypothesis/LatencyHypothesis.h"
#include "faultline/core/Diagnostic.h"

#include <optional>
#include <string>

namespace faultline {

class HypothesisConstructor {
public:
    static std::optional<LatencyHypothesis> construct(const Diagnostic &finding);

    static HazardClass mapRuleToHazardClass(std::string_view ruleID);
    static std::vector<double> extractFeatures(const Diagnostic &finding);

private:
    static EvidenceTier inferEvidenceTier(const Diagnostic &finding);
    static std::string generateHypothesisId(const Diagnostic &finding);
};

} // namespace faultline
