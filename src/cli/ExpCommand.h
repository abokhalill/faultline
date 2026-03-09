// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace lshaz {

// Entry point for `lshaz exp <scan.json> [options]`.
// Synthesizes experiment bundles from scan diagnostics.
// Returns process exit code.
int runExpCommand(int argc, const char **argv);

} // namespace lshaz
