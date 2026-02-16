#pragma once

#include "faultline/core/Diagnostic.h"
#include "faultline/ir/IRAnalyzer.h"

#include <vector>

namespace faultline {

class DiagnosticRefiner {
public:
    explicit DiagnosticRefiner(const IRAnalyzer::ProfileMap &profiles);

    // Refine diagnostics in-place using IR evidence.
    // May adjust confidence, add escalations, or suppress false positives.
    void refine(std::vector<Diagnostic> &diagnostics) const;

private:
    void refineFL010(Diagnostic &diag) const;
    void refineFL011(Diagnostic &diag) const;
    void refineFL020(Diagnostic &diag) const;
    void refineFL021(Diagnostic &diag) const;
    void refineFL030(Diagnostic &diag) const;
    void refineFL031(Diagnostic &diag) const;
    void refineFL012(Diagnostic &diag) const;
    void refineFL090(Diagnostic &diag) const;

    // Extract mangled function name from structural evidence.
    std::string extractFunctionName(const Diagnostic &diag) const;

    // Find best matching profile for a function name.
    const IRFunctionProfile *findProfile(const std::string &funcName) const;

    const IRAnalyzer::ProfileMap &profiles_;
};

} // namespace faultline
