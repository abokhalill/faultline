// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "lshaz/core/Rule.h"

#include <memory>
#include <string_view>
#include <vector>

namespace lshaz {

class RuleRegistry {
public:
    static RuleRegistry &instance();

    void registerRule(std::unique_ptr<Rule> rule);

    const std::vector<std::unique_ptr<Rule>> &rules() const { return rules_; }

    const Rule *findByID(std::string_view id) const;

private:
    RuleRegistry() = default;
    std::vector<std::unique_ptr<Rule>> rules_;
};

// Macro for static self-registration in rule .cpp files.
#define LSHAZ_REGISTER_RULE(RuleClass)                                     \
    namespace {                                                                \
    struct RuleClass##Registrar {                                              \
        RuleClass##Registrar() {                                               \
            ::lshaz::RuleRegistry::instance().registerRule(                 \
                std::make_unique<RuleClass>());                                \
        }                                                                      \
    };                                                                         \
    static RuleClass##Registrar g_##RuleClass##Registrar;                      \
    } // anonymous namespace

} // namespace lshaz
