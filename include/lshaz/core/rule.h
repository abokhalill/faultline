// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "lshaz/core/config.h"
#include "lshaz/core/diagnostic.h"
#include "lshaz/core/severity.h"

#include <string>
#include <string_view>
#include <vector>

namespace clang {
class ASTContext;
class Decl;
} // namespace clang

namespace lshaz {

class EscapeAnalysis;
class HotPathOracle;

class Rule {
public:
    virtual ~Rule() = default;

    virtual std::string_view getID() const = 0;
    virtual std::string_view getTitle() const = 0;
    virtual Severity getBaseSeverity() const = 0;
    virtual std::string_view getHardwareMechanism() const = 0;

    // True when the rule's mechanism only bites if the code runs often, so
    // its severity is bounded by how well hotness is evidenced. Structural
    // rules (a struct spans two lines whether or not anyone touches it)
    // leave this false and keep their full grade.
    virtual bool requiresHotPath() const { return false; }

    virtual void analyze(const clang::Decl *D,
                         clang::ASTContext &Ctx,
                         const HotPathOracle &Oracle,
                         const Config &Cfg,
                         EscapeAnalysis &Escape,
                         std::vector<Diagnostic> &out) = 0;
};

} // namespace lshaz
