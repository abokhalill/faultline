#pragma once

#include "faultline/hypothesis/HazardClass.h"
#include "faultline/hypothesis/LatencyHypothesis.h"
#include "faultline/hypothesis/PMUCounter.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace faultline {

struct InteractionTemplate {
    std::string id;                        // e.g., "IX-001"
    std::vector<HazardClass> components;
    std::string amplificationMechanism;
    PMUCounterSet counterSet;              // Union of component counter sets
    double interactionThreshold = 0.20;    // δ as fraction of sum of individual effects
};

struct InteractionCandidate {
    std::string declarationScope;          // Struct or function name
    std::vector<std::string> findingIds;
    std::vector<HazardClass> hazardClasses;
    const InteractionTemplate *matchedTemplate = nullptr;
};

struct InteractionResult {
    std::string interactionId;
    std::string templateId;
    double effectA       = 0.0;   // Individual effect of hazard A
    double effectB       = 0.0;   // Individual effect of hazard B
    double effectCombined = 0.0;  // Combined effect
    double interactionEffect = 0.0; // effectCombined - (effectA + effectB)
    double interactionD  = 0.0;   // Cohen's d for interaction term
    double pValue        = 1.0;
    bool superAdditive   = false;
    uint32_t replicationCount = 0;
    std::vector<std::string> confirmedSKUs;
};

struct InteractionCatalogEntry {
    InteractionTemplate tmpl;
    std::vector<InteractionResult> results;
    double meanInteractionD = 0.0;
    bool confirmedSuperAdditive = false;
};

class InteractionEligibilityMatrix {
public:
    static const InteractionEligibilityMatrix &instance();

    bool isEligible(HazardClass a, HazardClass b) const;
    const InteractionTemplate *findTemplate(HazardClass a, HazardClass b) const;
    const std::vector<InteractionTemplate> &templates() const { return templates_; }

private:
    InteractionEligibilityMatrix();
    std::vector<InteractionTemplate> templates_;
};

class InteractionDetector {
public:
    static std::vector<InteractionCandidate> detect(
        const std::vector<LatencyHypothesis> &hypotheses);

    static std::optional<LatencyHypothesis> constructInteractionHypothesis(
        const InteractionCandidate &candidate);
};

class InteractionCatalog {
public:
    void addResult(const std::string &templateId,
                   const InteractionResult &result);

    std::optional<InteractionCatalogEntry> lookup(
        const std::string &templateId) const;

    const std::vector<InteractionCatalogEntry> &entries() const {
        return entries_;
    }

private:
    std::vector<InteractionCatalogEntry> entries_;
};

} // namespace faultline
