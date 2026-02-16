#pragma once

#include <cstdint>
#include <string_view>

namespace faultline {

enum class EvidenceTier : uint8_t {
    Proven,   // Structurally guaranteed (e.g., sizeof > 64B is a fact)
    Likely,   // Strong heuristic evidence (e.g., escape analysis says shared)
    Unknown,  // Insufficient evidence to classify
};

constexpr std::string_view evidenceTierName(EvidenceTier t) {
    switch (t) {
        case EvidenceTier::Proven:  return "proven";
        case EvidenceTier::Likely:  return "likely";
        case EvidenceTier::Unknown: return "unknown";
    }
    return "unknown";
}

} // namespace faultline
