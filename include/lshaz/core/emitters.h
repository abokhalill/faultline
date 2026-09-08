// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "lshaz/core/severity.h"

#include <string_view>
#include <vector>

namespace lshaz {
    
struct EmitterDoc {
    std::string_view id;
    std::string_view title;
    Severity baseSeverity;
    std::string_view mechanism;
};

const std::vector<EmitterDoc> &nonRuleEmitters();

const EmitterDoc *findEmitterByID(std::string_view id);

} // namespace lshaz
