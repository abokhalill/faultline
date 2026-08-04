// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "lshaz/hypothesis/hazard_class.h"
#include "lshaz/hypothesis/latency_hypothesis.h"
#include "lshaz/hypothesis/pmu_counter.h"

#include <string>
#include <vector>

namespace lshaz {

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

} // namespace lshaz
