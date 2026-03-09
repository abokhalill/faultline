// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace lshaz {

// Entry point for `lshaz hyp <scan.json> [options]`.
// Constructs latency hypotheses from scan diagnostics.
// Returns process exit code.
int runHypCommand(int argc, const char **argv);

} // namespace lshaz
