// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "lshaz/core/Severity.h"

#include <cstddef>
#include <string>
#include <vector>

namespace lshaz {

enum class TargetArch : uint8_t {
    X86_64,     // 64B cache lines, TSO, MESI coherence
    ARM64,      // 64B cache lines (Graviton), weak ordering
    ARM64Apple, // 128B cache lines (M-series P-cores), weak ordering
};

struct Config {
    // Target architecture (affects cache model, ordering cost model, rule text)
    TargetArch targetArch       = TargetArch::X86_64;

    // Deployment runs with SMT/Hyper-Threading enabled. Desks disable it
    // in BIOS; FL013's sibling-starvation cost is then excluded and its
    // severity drops a notch.
    bool smtEnabled             = true;

    // Cache model
    size_t cacheLineBytes       = 64;
    size_t cacheLineSpanWarn    = 64;   // FL001 threshold
    size_t cacheLineSpanCrit    = 128;  // FL001 escalation

    // Stack frame
    size_t stackFrameWarnBytes  = 2048; // FL021 threshold

    // Allocation
    size_t allocSizeEscalation  = 256;  // FL020 escalation

    // Branch depth
    unsigned branchDepthWarn    = 4;    // FL050 threshold

    // Minimum severity to emit
    Severity minSeverity        = Severity::Informational;

    // Output
    bool jsonOutput             = false;
    std::string outputFile;             // empty = stdout

    // Hot path patterns (fnmatch-style)
    // Derive hotness from loop-depth reachability when no attribute,
    // config pattern or profile supplies it. Without this, an
    // unconfigured scan leaves every hot-path rule inert.
    bool inferHotPaths = true;
    std::vector<std::string> hotFunctionPatterns;
    std::vector<std::string> hotFilePatterns;

    // Vendored third-party trees, skipped unless --include-vendored or
    // skip_vendored: false. Directory naming is convention, not fact, so a
    // project whose own code lives under one of these names must be able to
    // replace the list — set vendor_path_patterns to the trees that really
    // are vendored. A non-empty list replaces these defaults; use
    // skip_vendored to turn the behaviour off, since YAML's empty sequence
    // is indistinguishable from an absent key here.
    bool skipVendored = true;
    std::vector<std::string> vendorPathPatterns = {
        "*/deps/*", "*/third_party/*", "*/thirdparty/*", "*/third-party/*",
        "*/vendor/*", "*/external/*", "*/extern/*", "*/contrib/*",
        "*/node_modules/*", "*/subprojects/*",
    };

    // Deployment socket count. Unlike the preconditions other rules assume,
    // this one genuinely cannot be read from source; it is a property of
    // where the binary runs. 0 means unknown, and FL060 then reports with
    // the assumption labelled.
    unsigned numaSockets = 0;

    // Rule enable/disable
    std::vector<std::string> disabledRules;

    // TLB
    size_t pageSize             = 4096;

    // Profile-guided hotness (perf/LBR)
    std::string perfProfilePath;          // Path to perf profile data
    double hotnessThresholdPct  = 1.0;    // Functions with >= N% of samples are hot

    // Allocator topology: linked allocator library name.
    // "tcmalloc", "jemalloc", "mimalloc", or "" (default glibc).
    std::string linkedAllocator;

    // Opaque atomic wrapper type names (e.g. atomic_t, spinlock_t).
    std::vector<std::string> atomicTypeNames;

    // L1 data cache size; FL003 weighs padding footprint against it.
    size_t l1dSizeBytes = 32768;

    // Write-frequency roots for FL003 (fnmatch). Hot comes from the
    // hot-path oracle; these name the slower tiers so a real hazard on a
    // per-connection path is not priced as if it were per-command.
    std::vector<std::string> dispatchPathPatterns;
    std::vector<std::string> tickPathPatterns;

    // Thread-role attribution roots (fnmatch-style). Entry patterns name
    // worker-thread roots that thread-creation detection cannot see
    // (function-pointer dispatch); main patterns extend the ROLE_MAIN
    // seed beyond main() for callbacks dispatched from its event loop.
    std::vector<std::string> threadEntryPatterns;
    std::vector<std::string> mainFunctionPatterns;

    // Bespoke backoff/wait vocabularies (fnmatch on plain or qualified
    // names) granted FL013 relax immunity. The standard universe is
    // built in.
    std::vector<std::string> relaxFunctionPatterns;

    static Config loadFromFile(const std::string &path);
    static Config defaults();
};

} // namespace lshaz
