#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace faultline {

enum class CounterTier : uint8_t {
    Universal,  // Available on all x86-64 since Sandy Bridge
    Standard,   // Available on most server SKUs
    Extended,   // Requires specific microarchitecture
    Uncore,     // Per-socket, not per-core
};

struct PMUCounter {
    std::string name;
    CounterTier tier = CounterTier::Universal;
    std::string justification;
    std::string skuOverride;  // Empty = universal name; otherwise SKU-specific event
};

struct PMUCounterSet {
    std::vector<PMUCounter> required;
    std::vector<PMUCounter> optional;

    PMUCounterSet merged(const PMUCounterSet &other) const {
        PMUCounterSet result = *this;
        result.required.insert(result.required.end(),
                               other.required.begin(), other.required.end());
        result.optional.insert(result.optional.end(),
                               other.optional.begin(), other.optional.end());
        return result;
    }
};

} // namespace faultline
