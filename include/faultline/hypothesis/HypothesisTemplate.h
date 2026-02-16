#pragma once

#include "faultline/hypothesis/HazardClass.h"
#include "faultline/hypothesis/LatencyHypothesis.h"
#include "faultline/hypothesis/PMUCounter.h"

#include <string>
#include <vector>

namespace faultline {

struct HypothesisTemplate {
    HazardClass hazardClass;
    std::string H0Template;
    std::string H1Template;
    MetricSpec primaryMetric;
    PMUCounterSet counterSet;
    double defaultMDE = 0.05;
    std::vector<ConfoundControl> confoundRequirements;
    bool interactionEligible = false;
};

class HypothesisTemplateRegistry {
public:
    static const HypothesisTemplateRegistry &instance();

    const HypothesisTemplate *lookup(HazardClass hc) const;
    const std::vector<HypothesisTemplate> &templates() const { return templates_; }

private:
    HypothesisTemplateRegistry();
    std::vector<HypothesisTemplate> templates_;
};

} // namespace faultline
