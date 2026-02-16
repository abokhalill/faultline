#pragma once

#include <cstdint>
#include <string_view>

namespace faultline {

enum class Severity : uint8_t {
    Informational = 0,
    Medium        = 1,
    High          = 2,
    Critical      = 3,
};

constexpr std::string_view severityToString(Severity s) {
    switch (s) {
        case Severity::Informational: return "Informational";
        case Severity::Medium:        return "Medium";
        case Severity::High:          return "High";
        case Severity::Critical:      return "Critical";
    }
    return "Unknown";
}

constexpr bool operator>=(Severity a, Severity b) {
    return static_cast<uint8_t>(a) >= static_cast<uint8_t>(b);
}

} // namespace faultline
