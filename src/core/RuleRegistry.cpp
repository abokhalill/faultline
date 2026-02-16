#include "faultline/core/RuleRegistry.h"

#include <algorithm>

namespace faultline {

RuleRegistry &RuleRegistry::instance() {
    static RuleRegistry registry;
    return registry;
}

void RuleRegistry::registerRule(std::unique_ptr<Rule> rule) {
    rules_.push_back(std::move(rule));
}

const Rule *RuleRegistry::findByID(std::string_view id) const {
    auto it = std::find_if(rules_.begin(), rules_.end(),
                           [id](const auto &r) { return r->getID() == id; });
    return (it != rules_.end()) ? it->get() : nullptr;
}

} // namespace faultline
