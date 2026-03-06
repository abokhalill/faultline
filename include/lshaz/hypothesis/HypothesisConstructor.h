#pragma once

#include "lshaz/hypothesis/LatencyHypothesis.h"
#include "lshaz/core/Diagnostic.h"

#include <optional>
#include <string>

namespace lshaz {

class HypothesisConstructor {
public:
    static std::optional<LatencyHypothesis> construct(const Diagnostic &finding);

    static HazardClass mapRuleToHazardClass(std::string_view ruleID);
    static std::vector<double> extractFeatures(const Diagnostic &finding);

private:
    static EvidenceTier inferEvidenceTier(const Diagnostic &finding);
    static std::string generateHypothesisId(const Diagnostic &finding);
};

} // namespace lshaz
