#pragma once

#include "lshaz/hypothesis/CalibrationFeedback.h"
#include "lshaz/hypothesis/HazardClass.h"
#include "lshaz/hypothesis/PMUCounter.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace lshaz {

// A single PMU counter sample from production.
struct PMUSample {
    std::string counterName;
    uint64_t value = 0;
    uint64_t duration_ns = 0; // Measurement window duration
};

// A production PMU trace record tied to a specific code site.
struct PMUTraceRecord {
    std::string functionName;
    std::string sourceFile;
    unsigned sourceLine = 0;

    std::vector<PMUSample> samples;

    // Environment context.
    std::string cpuModel;
    std::string skuFamily;
    uint64_t timestampEpochSec = 0;

    // Optional: finding ID if this trace was collected for a specific finding.
    std::string findingId;
};

// Per-rule prior updated from production data.
struct HazardPrior {
    HazardClass hazardClass;
    double priorConfidence = 0.5;   // Bayesian prior P(hazard | evidence)
    double falsePositiveRate = 0.0; // Observed FP rate from production
    double truePositiveRate = 0.0;  // Observed TP rate from production
    uint32_t totalObservations = 0;
    uint32_t confirmedHazards = 0;
    uint32_t refutedHazards = 0;
};

// Counter thresholds for hazard confirmation from production data.
// If production PMU counters exceed these thresholds at a flagged site,
// the hazard is confirmed; below threshold → likely false positive.
struct CounterThreshold {
    std::string counterName;
    double confirmThreshold = 0.0;  // Above this = hazard confirmed
    double refuteThreshold = 0.0;   // Below this = hazard refuted
    // Between thresholds = inconclusive
};

// Closed-loop PMU trace feedback system.
//
// Ingests production PMU counter data collected at sites where lshaz
// emitted diagnostics. Uses counter evidence to:
//   1. Confirm or refute hazard predictions (update priors)
//   2. Feed labeled records back into CalibrationFeedbackStore
//   3. Track per-rule false positive rates over time
//   4. Adjust confidence scores for future analyses
class PMUTraceFeedbackLoop {
public:
    explicit PMUTraceFeedbackLoop(CalibrationFeedbackStore &calStore);

    // Ingest a production PMU trace record. Returns the verdict.
    LabelValue ingestTrace(const PMUTraceRecord &trace,
                           HazardClass hazardClass,
                           const std::vector<double> &featureVector);

    // Get the current prior for a hazard class.
    const HazardPrior *getPrior(HazardClass hc) const;

    // Get all priors.
    const std::unordered_map<std::string, HazardPrior> &allPriors() const {
        return priors_;
    }

    // Apply learned priors to adjust a diagnostic's confidence.
    // Returns the adjusted confidence value.
    double adjustConfidence(double baseConfidence, HazardClass hc) const;

    // Serialize priors to a file for persistence across runs.
    bool savePriors(const std::string &path) const;

    // Load priors from a previously saved file.
    bool loadPriors(const std::string &path);

private:
    // Map hazard class to its counter thresholds for confirmation.
    static std::vector<CounterThreshold> defaultThresholds(HazardClass hc);

    // Evaluate PMU samples against thresholds.
    LabelValue evaluateCounters(const std::vector<PMUSample> &samples,
                                 HazardClass hc) const;

    // Update the Bayesian prior for a hazard class given new evidence.
    void updatePrior(HazardClass hc, LabelValue verdict);

    CalibrationFeedbackStore &calStore_;
    // Key: hazardClassName string for stable serialization.
    std::unordered_map<std::string, HazardPrior> priors_;
};

} // namespace lshaz
