// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace lshaz {

// Entry point for `lshaz scan <path|url> [options]`.
// Returns process exit code.
int runScanCommand(int argc, const char **argv);

} // namespace lshaz
