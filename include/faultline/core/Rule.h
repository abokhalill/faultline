#pragma once

#include "faultline/core/Diagnostic.h"
#include "faultline/core/Severity.h"

#include <string>
#include <string_view>
#include <vector>

namespace clang {
class ASTContext;
class Decl;
} // namespace clang

namespace faultline {

class HotPathOracle;

class Rule {
public:
    virtual ~Rule() = default;

    virtual std::string_view getID() const = 0;
    virtual std::string_view getTitle() const = 0;
    virtual Severity getBaseSeverity() const = 0;
    virtual std::string_view getHardwareMechanism() const = 0;

    // Run analysis on a single top-level declaration.
    // Implementations append to `out` if hazards are found.
    virtual void analyze(const clang::Decl *D,
                         clang::ASTContext &Ctx,
                         const HotPathOracle &Oracle,
                         std::vector<Diagnostic> &out) = 0;
};

} // namespace faultline
