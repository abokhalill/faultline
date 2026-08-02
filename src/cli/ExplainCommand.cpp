// SPDX-License-Identifier: Apache-2.0
#include "lshaz/core/RuleRegistry.h"
#include "RuleTiers.h"

#include <iterator>
#include "lshaz/core/Severity.h"

#include <llvm/Support/raw_ostream.h>

#include <cstring>

namespace lshaz {

namespace {
std::string_view ruleTier(std::string_view id) {
    for (const auto &e : kRuleTiers)
        if (e.id == id) return e.tier;
    return "experimental";   // absent from the ledger is not evidence
}
} // namespace


int runExplainCommand(int argc, const char **argv) {
    const auto &rules = RuleRegistry::instance().rules();

    if (argc < 1 || (argc == 1 && std::strcmp(argv[0], "--help") == 0)) {
        llvm::errs() << "Usage: lshaz explain <rule-id>\n"
                     << "       lshaz explain --list\n"
                     << "\n"
                     << "Show detailed documentation for a diagnostic rule.\n";
        return 0;
    }

    if (std::strcmp(argv[0], "--list") == 0) {
        llvm::outs() << "Available rules:\n\n";
        for (const auto &r : rules) {
            llvm::outs() << "  " << r->getID() << "  "
                         << r->getTitle() << "  ["
                         << severityToString(r->getBaseSeverity()) << "]  "
                         << ruleTier(r->getID()) << "\n";
        }
        // An operator deciding what to switch on needs the evidence state,
        // not just the rule list. Printed here rather than in a report so it
        // cannot drift from what the binary actually does.
        llvm::outs() << "\nMaturity is derived from evidence/rules.yaml and "
                        "cannot be declared by a rule.\n";
        unsigned onByDefault = 0;
        for (const auto &e : kRuleTiers)
            if (e.tier != "experimental") ++onByDefault;
        llvm::outs() << onByDefault << " of " << std::size(kRuleTiers)
                     << " rule(s) have evidence to run by default.\n";
        if (onByDefault == 0)
            llvm::outs() << "None yet. Experimental rules still run when "
                            "asked for explicitly; they are not gated on.\n";
        for (const auto &e : kRuleTiers)
            llvm::outs() << "  " << e.id << "  " << e.tier << " — "
                         << e.reason << "\n";
        return 0;
    }

    const char *id = argv[0];
    const Rule *rule = RuleRegistry::instance().findByID(id);
    if (!rule) {
        llvm::errs() << "lshaz explain: unknown rule '" << id << "'\n"
                     << "Run 'lshaz explain --list' for available rules.\n";
        return 1;
    }

    llvm::outs() << rule->getID() << ": " << rule->getTitle() << "\n"
                 << "Severity: " << severityToString(rule->getBaseSeverity())
                 << "\n\n"
                 << "Hardware Mechanism:\n  " << rule->getHardwareMechanism()
                 << "\n";

    return 0;
}

} // namespace lshaz
