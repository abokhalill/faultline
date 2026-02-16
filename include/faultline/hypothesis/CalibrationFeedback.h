#pragma once

#include "faultline/hypothesis/EvidenceTier.h"
#include "faultline/hypothesis/HazardClass.h"
#include "faultline/hypothesis/LatencyHypothesis.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace faultline {

struct LatencyPercentiles {
    double p50   = 0.0;
    double p99   = 0.0;
    double p99_9 = 0.0;
    double p99_99 = 0.0;
};

struct CounterDelta {
    std::string counterName;
    uint64_t treatment = 0;
    uint64_t control   = 0;
};

struct EnvironmentState {
    std::string kernel;
    std::string cpuModel;
    std::string skuFamily;
    std::vector<int> coresUsed;
    std::string numaTopology;
    std::string governor;
    bool turboDisabled = false;
};

struct ExperimentResult {
    std::string findingId;
    std::string hypothesisId;
    std::string schemaVersion;

    ExperimentVerdict verdict = ExperimentVerdict::Pending;
    double pValue     = 1.0;
    double effectSizeD = 0.0;
    double power       = 0.0;

    LatencyPercentiles treatmentLatency;
    LatencyPercentiles controlLatency;
    std::vector<CounterDelta> counterDeltas;

    EnvironmentState envState;

    uint64_t warmupIterations     = 0;
    uint64_t measurementIterations = 0;
    uint64_t ingestionTimestamp    = 0;
};

enum class LabelValue : uint8_t {
    Positive,    // Hazard confirmed exercised
    Negative,    // Hazard refuted
    Unlabeled,   // Inconclusive
    Excluded,    // Confounded or low quality
};

struct LabeledRecord {
    std::string findingId;
    std::string hypothesisId;
    HazardClass hazardClass;
    std::vector<double> featureVector;
    LabelValue label        = LabelValue::Unlabeled;
    double labelQuality     = 0.0;
    double effectSize       = 0.0;
    double pValue           = 1.0;
    std::string skuFamily;
    std::string kernelVersion;
    std::string schemaVersion;
    uint64_t ingestionTimestamp = 0;
};

struct CalibrationReport {
    std::string modelVersion;
    uint32_t trainingRecords  = 0;
    uint32_t testRecords      = 0;
    double brierScore         = 1.0;
    double maxCalibrationError = 1.0;
    double precisionHighCritical = 0.0;
    double recallCritical     = 0.0;
    double aucRoc             = 0.0;
    bool adversarialCorpusPass = false;
    std::string driftFlags;
};

class CalibrationFeedbackStore {
public:
    explicit CalibrationFeedbackStore(const std::string &storePath);

    // Ingest a raw experiment result. Returns the labeled record if accepted.
    std::optional<LabeledRecord> ingest(const ExperimentResult &result,
                                        const std::vector<double> &featureVector,
                                        HazardClass hazardClass);

    // Query labeled records for a hazard class.
    std::vector<LabeledRecord> queryByHazardClass(HazardClass hc) const;

    // Query labeled records for a SKU family.
    std::vector<LabeledRecord> queryBySKU(const std::string &skuFamily) const;

    // Total record count.
    size_t recordCount() const { return records_.size(); }

    // Check if a feature combination is in the known false positive registry.
    bool isKnownFalsePositive(const std::vector<double> &features,
                              HazardClass hc) const;

    // Register a known false positive combination.
    void registerFalsePositive(const std::vector<double> &features,
                               HazardClass hc,
                               const std::string &reason);

private:
    static LabelValue assignLabel(const ExperimentResult &result);
    static double computeLabelQuality(const ExperimentResult &result);
    bool validateSchema(const ExperimentResult &result) const;

    std::string storePath_;
    std::vector<LabeledRecord> records_;

    struct FalsePositiveEntry {
        std::vector<double> features;
        HazardClass hazardClass;
        std::string reason;
        uint32_t refutationCount = 0;
    };
    std::vector<FalsePositiveEntry> falsePositiveRegistry_;
};

} // namespace faultline
