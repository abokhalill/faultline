// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "lshaz/core/Diagnostic.h"

#include <string>
#include <vector>

namespace lshaz {

// Minimal parser for lshaz's own JSON output format.
// Extracts Diagnostic objects from a scan result file.
// Not a general-purpose JSON parser — only understands lshaz schema.
bool parseScanResultFile(const std::string &path,
                         std::vector<Diagnostic> &out,
                         std::string &error);

} // namespace lshaz
