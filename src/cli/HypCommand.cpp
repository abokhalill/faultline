// SPDX-License-Identifier: Apache-2.0
#include "HypCommand.h"
#include "ScanResultParser.h"

#include "lshaz/hypothesis/HypothesisConstructor.h"
#include "lshaz/hypothesis/LatencyHypothesis.h"

#include <llvm/Support/raw_ostream.h>

#include <cstring>
#include <sstream>

namespace lshaz {

namespace {

std::string hypothesisToJson(const LatencyHypothesis &h) {
    std::ostringstream os;
    os << "    {\n"
       << "      \"hypothesisId\": \"" << h.hypothesisId << "\",\n"
       << "      \"findingId\": \"" << h.findingId << "\",\n"
       << "      \"hazardClass\": \"" << hazardClassName(h.hazardClass) << "\",\n"
       << "      \"H0\": \"" << h.H0 << "\",\n"
       << "      \"H1\": \"" << h.H1 << "\",\n"
       << "      \"primaryMetric\": {\n"
       << "        \"name\": \"" << h.primaryMetric.name << "\",\n"
       << "        \"unit\": \"" << h.primaryMetric.unit << "\",\n"
       << "        \"percentile\": \"" << h.primaryMetric.percentile << "\"\n"
       << "      },\n"
       << "      \"minimumDetectableEffect\": " << h.minimumDetectableEffect << ",\n"
       << "      \"significanceLevel\": " << h.significanceLevel << ",\n"
       << "      \"power\": " << h.power << ",\n"
       << "      \"evidenceTier\": \"" << evidenceTierName(h.evidenceTier) << "\",\n"
       << "      \"verdict\": \"" << verdictName(h.verdict) << "\"\n"
       << "    }";
    return os.str();
}

} // anonymous namespace

int runHypCommand(int argc, const char **argv) {
    if (argc < 1 || (argc == 1 && std::strcmp(argv[0], "--help") == 0)) {
        llvm::errs()
            << "Usage: lshaz hyp <scan-result.json> [options]\n\n"
            << "Construct latency hypotheses from scan diagnostics.\n\n"
            << "Options:\n"
            << "  --rule <id>    Only hypothesize for a specific rule ID\n"
            << "  --min-conf <f> Minimum confidence threshold (default: 0.0)\n"
            << "  --help         Show this help\n";
        return 0;
    }

    const char *inputPath = argv[0];
    std::string filterRule;
    double minConf = 0.0;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--rule") == 0 && i + 1 < argc)
            filterRule = argv[++i];
        else if (std::strcmp(argv[i], "--min-conf") == 0 && i + 1 < argc)
            minConf = std::stod(argv[++i]);
    }

    std::vector<Diagnostic> diagnostics;
    std::string error;
    if (!parseScanResultFile(inputPath, diagnostics, error)) {
        llvm::errs() << "lshaz hyp: " << error << "\n";
        return 1;
    }

    std::vector<LatencyHypothesis> hypotheses;
    for (const auto &d : diagnostics) {
        if (!filterRule.empty() && d.ruleID != filterRule)
            continue;
        if (d.confidence < minConf)
            continue;
        if (auto h = HypothesisConstructor::construct(d))
            hypotheses.push_back(std::move(*h));
    }

    // Output as JSON array.
    llvm::outs() << "{\n"
                 << "  \"hypotheses\": [\n";
    for (size_t i = 0; i < hypotheses.size(); ++i) {
        llvm::outs() << hypothesisToJson(hypotheses[i]);
        if (i + 1 < hypotheses.size()) llvm::outs() << ",";
        llvm::outs() << "\n";
    }
    llvm::outs() << "  ],\n"
                 << "  \"totalDiagnostics\": " << diagnostics.size() << ",\n"
                 << "  \"hypothesesGenerated\": " << hypotheses.size() << "\n"
                 << "}\n";

    return 0;
}

} // namespace lshaz
