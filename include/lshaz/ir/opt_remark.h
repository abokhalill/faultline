// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <llvm/ADT/StringRef.h>

#include <string>
#include <vector>

namespace lshaz {

// One optimization the compiler declined to perform, with its own reason.
struct OptRemark {
    std::string pass;       // "regalloc"
    std::string name;       // "LoopSpillReloadCopies"
    std::string function;   // demangled, signature stripped
    std::string file;
    unsigned    line   = 0;
    unsigned    column = 0;
    std::string detail; // the compiler's own sentence, where it emits one
    unsigned    count  = 0; // NumVRCopies and friends, 0 when absent
};

// Whitelisted during parse: one mid-sized C file emits ~12.8k records.
bool remarkIsReportable(llvm::StringRef pass, llvm::StringRef name);

// Appends to `out`. An absent file is not an error: a cached or failed TU
// produces none. Returns false only for a container that exists and breaks.
bool parseOptRemarks(const std::string &path, std::vector<OptRemark> &out);

} // namespace lshaz
