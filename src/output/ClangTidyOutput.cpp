// SPDX-License-Identifier: Apache-2.0
#include "lshaz/output/OutputFormatter.h"

#include <sstream>

namespace lshaz {

namespace {

const char *tidySeverity(Severity s) {
    switch (s) {
        case Severity::Critical:      return "error";
        case Severity::High:          return "warning";
        case Severity::Medium:        return "warning";
        case Severity::Informational: return "note";
    }
    return "warning";
}

} // anonymous namespace

std::string ClangTidyOutputFormatter::format(
    const std::vector<Diagnostic> &diagnostics) {
    std::ostringstream os;

    for (const auto &d : diagnostics) {
        // Primary diagnostic line: file:line:col: severity: message [check-name]
        os << d.location.file << ":" << d.location.line << ":"
           << d.location.column << ": "
           << tidySeverity(d.severity) << ": "
           << d.title;

        if (!d.hardwareReasoning.empty()) {
            // Truncate to first sentence for brevity.
            auto dot = d.hardwareReasoning.find(". ");
            if (dot != std::string::npos && dot < 120)
                os << " (" << d.hardwareReasoning.substr(0, dot + 1) << ")";
        }

        os << " [lshaz-" << d.ruleID << "]\n";

        // Note lines for escalations.
        for (const auto &esc : d.escalations) {
            os << d.location.file << ":" << d.location.line << ":"
               << d.location.column << ": note: " << esc
               << " [lshaz-" << d.ruleID << "]\n";
        }
    }

    return os.str();
}

} // namespace lshaz
