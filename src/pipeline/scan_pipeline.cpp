// SPDX-License-Identifier: Apache-2.0
#include "lshaz/pipeline/scan_pipeline.h"
#include "lshaz/pipeline/abs_path_db.h"
#include "lshaz/pipeline/compile_db.h"
#include "lshaz/pipeline/filter.h"

#include "lshaz/analysis/action.h"
#include "lshaz/core/dedup.h"
#include "lshaz/core/hot_path.h"
#include "lshaz/core/interaction.h"
#include "lshaz/core/perf_profile.h"
#include "lshaz/core/precision.h"
#include "lshaz/core/registry.h"
#include "lshaz/core/version.h"
#include "lshaz/hypothesis/calibration.h"
#include "lshaz/hypothesis/hypothesis.h"
#include "lshaz/hypothesis/pmu_trace.h"
#include "lshaz/ir/refiner.h"
#include "lshaz/ir/ir_analyzer.h"
#include "lshaz/ir/opt_remark.h"

#include <clang/Basic/Version.inc>
#include <clang/Tooling/ArgumentsAdjusters.h>
#include <clang/Tooling/CompilationDatabase.h>
#include <clang/Tooling/JSONCompilationDatabase.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/ADT/StringSet.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MD5.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/Program.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/CrashRecoveryContext.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <chrono>
#include <future>
#include <memory>
#include <semaphore>
#include <string>
#include <thread>
#include <map>
#include <unordered_set>
#include <vector>

#include <csignal>
#include <fnmatch.h>
#include <cstring>
#include <new>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace lshaz {

ScanPipeline::ScanPipeline(ProgressCallback progress)
    : progress_(std::move(progress)) {}

void ScanPipeline::report(const std::string &stage,
                           const std::string &detail) const {
    if (progress_)
        progress_(stage, detail);
}

// --- IR emission helpers (internal) ---

namespace {

// GCC-only flags that Clang doesn't understand. These cause IR emission
// failures on GCC-compiled codebases (postgres, Linux kernel, nginx, etc.).
// We strip them before invoking clang for IR generation.
bool isGCCOnlyFlag(llvm::StringRef arg) {
    // Exact matches
    static const llvm::StringSet<> exactFlags = {
        "-fno-strict-overflow",
        "-fno-delete-null-pointer-checks",
        "-fno-allow-store-data-races",
        "-fno-reorder-blocks",
        "-fno-ipa-cp-clone",
        "-fno-partial-inlining",
        "-fno-tree-loop-distribute-patterns",
        "-fconserve-stack",
        "-fno-stack-clash-protection",
        "-mno-fp-ret-in-387",
        "-mpreferred-stack-boundary=3",
        "-mskip-rax-setup",
        "-mrecord-mcount",
        "-mfentry",
        "-mindirect-branch=thunk-extern",
        "-mindirect-branch-register",
        "-mindirect-branch-cs-prefix",
        "-mfunction-return=thunk-extern",
        "-fno-jump-tables",
        "-fno-gcse",
        "-fno-tree-scev-cprop",
        "-fno-PIE",
        "-fno-asynchronous-unwind-tables",
        "-fzero-call-used-regs=used-gpr",
        "-fstrict-flex-arrays=3",
        "-fno-strict-flex-arrays",
        // clang-18 crashes on this with -c. Harmless while emission is
        // -S -emit-llvm, fatal the moment anything runs codegen.
        "-ffat-lto-objects",
        "-fno-fat-lto-objects",
    };
    if (exactFlags.contains(arg))
        return true;

    // Prefix matches for parameterized flags
    if (arg.starts_with("-Wshadow="))           // -Wshadow=compatible-local
        return true;
    if (arg.starts_with("-Wcast-function-type"))  // -Wcast-function-type
        return true;
    if (arg.starts_with("-Wimplicit-fallthrough="))  // -Wimplicit-fallthrough=3
        return true;
    if (arg.starts_with("-Wno-stringop-"))      // -Wno-stringop-truncation, etc.
        return true;
    if (arg.starts_with("-Wstringop-"))
        return true;
    if (arg.starts_with("-Wno-format-truncation"))
        return true;
    if (arg.starts_with("-Wformat-truncation"))
        return true;
    if (arg.starts_with("-Wno-maybe-uninitialized"))
        return true;
    if (arg.starts_with("-Wmaybe-uninitialized"))
        return true;
    if (arg.starts_with("-Wno-alloc-size-larger-than"))
        return true;
    if (arg.starts_with("-Walloc-size-larger-than"))
        return true;
    if (arg.starts_with("-Wno-restrict"))
        return true;
    if (arg.starts_with("-Wduplicated-"))       // -Wduplicated-cond, -Wduplicated-branches
        return true;
    if (arg.starts_with("-Wlogical-op"))
        return true;
    if (arg.starts_with("-Wno-aggressive-loop-optimizations"))
        return true;
    if (arg.starts_with("-mabi="))              // -mabi=lp64
        return true;
    if (arg.starts_with("-mcmodel="))           // -mcmodel=kernel
        return true;
    if (arg.starts_with("-mstack-protector-guard"))
        return true;
    if (arg.starts_with("-fplugin"))            // GCC plugins
        return true;
    if (arg.starts_with("-fdiagnostics-color="))  // GCC-specific color syntax
        return true;

    return false;
}

// Sanitize compile command for Clang IR emission.
// Strips GCC-only flags and adds suppressions for GCC attribute dialects.
std::vector<std::string> sanitizeForClangIR(
        const std::vector<std::string> &args,
        const std::string &srcPath) {
    std::vector<std::string> result;
    result.reserve(args.size() + 2);

    // Suppress warnings about unknown attributes (gnu_printf, etc.)
    // This is cleaner than AST rewriting and achieves the same goal.
    result.push_back("-Wno-unknown-attributes");
    result.push_back("-Wno-ignored-attributes");

    // start at 1: args[0] is the DB compiler path. leaking it into the
    // input list "worked" for directory scans only because an existing
    // path is classified as an object input and -S skips the link; the
    // FixedCompilationDatabase placeholder does not exist, so single-file
    // IR emission failed on every invocation.
    for (size_t i = 1; i < args.size(); ++i) {
        const std::string &arg = args[i];

        // Skip -c and -o <file> (we replace these)
        if (arg == "-c")
            continue;
        if (arg == "-o" && i + 1 < args.size()) {
            ++i;
            continue;
        }
        // Skip the source file (we append it at the end)
        if (arg == srcPath)
            continue;

        // Strip the TU's own -O flags: the tool's --ir-opt is prepended,
        // and clang's last-flag-wins meant any release-configured compile
        // DB silently overrode it; every "O0" emission on such a project
        // actually ran at the project's level.
        if (arg.size() >= 2 && arg[0] == '-' && arg[1] == 'O' &&
            (arg.size() == 2 || arg == "-O0" || arg == "-O1" ||
             arg == "-O2" || arg == "-O3" || arg == "-Os" || arg == "-Oz" ||
             arg == "-Og" || arg == "-Ofast"))
            continue;

        // Strip GCC-only flags
        if (isGCCOnlyFlag(arg))
            continue;

        // Strip GCC-only flags that take a separate argument
        if (arg == "-mpreferred-stack-boundary" ||
            arg == "-mstack-protector-guard" ||
            arg == "-mstack-protector-guard-reg" ||
            arg == "-mstack-protector-guard-offset") {
            if (i + 1 < args.size())
                ++i;  // skip the argument too
            continue;
        }

        result.push_back(arg);
    }

    return result;
}

struct IRJob {
    std::string srcPath;
    std::string compilerPath;
    std::vector<std::string> argv;
    std::string irFile;
    std::string errFile;
    std::string remarkFile;
    bool cached = false;
};

struct IRResult {
    int exitCode = -1;
    std::string errMsg;
};

IRResult emitOneIR(const IRJob &job) {
    if (job.cached)
        return {0, {}};

    std::vector<llvm::StringRef> argRefs;
    argRefs.reserve(job.argv.size());
    for (const auto &a : job.argv)
        argRefs.push_back(a);

    llvm::StringRef errRedirect(job.errFile);
    std::optional<llvm::StringRef> redirects[] = {
        std::nullopt, std::nullopt, errRedirect
    };

    IRResult result;
    bool failed = false;
    result.exitCode = llvm::sys::ExecuteAndWait(
        job.compilerPath, argRefs,
        /*Env=*/std::nullopt, redirects,
        /*SecondsToWait=*/120, /*MemoryLimit=*/0,
        &result.errMsg, &failed);
    if (failed)
        result.exitCode = -1;
    return result;
}

std::string resolveCompiler(const std::string &dbCompiler) {
    // For IR emission, we MUST use clang (GCC doesn't support -emit-llvm).
    // The dbCompiler from compile_commands.json is only used to check if
    // we should use clang vs clang++ (C vs C++ mode).
    llvm::StringRef stem = llvm::sys::path::stem(dbCompiler);
    bool isCxx = stem.contains("++") || stem.contains("clang++") ||
                 stem.contains("g++") || stem.contains("c++");

    // Try clang/clang++ in order of preference.
    const char *cxxFallbacks[] = {"clang++", "clang++-18", "clang++-17", "clang++-16"};
    const char *cFallbacks[] = {"clang", "clang-18", "clang-17", "clang-16"};
    const char **fallbacks = isCxx ? cxxFallbacks : cFallbacks;
    size_t count = isCxx ? std::size(cxxFallbacks) : std::size(cFallbacks);

    for (size_t i = 0; i < count; ++i) {
        auto fb = llvm::sys::findProgramByName(fallbacks[i]);
        if (fb && llvm::sys::fs::can_execute(*fb))
            return *fb;
    }

    // Last resort: try the other variant
    const char **altFallbacks = isCxx ? cFallbacks : cxxFallbacks;
    size_t altCount = isCxx ? std::size(cFallbacks) : std::size(cxxFallbacks);
    for (size_t i = 0; i < altCount; ++i) {
        auto fb = llvm::sys::findProgramByName(altFallbacks[i]);
        if (fb && llvm::sys::fs::can_execute(*fb))
            return *fb;
    }

    return {};
}

// --- Fork-based parallel IPC ---

// Minimal JSON serializer for child→parent IPC.
// Format: {"exitCode":N,"failedTUs":[{"file":"...","error":"..."}],"diagnostics":[...]}
std::string serializeShardResult(int exitCode,
                                  const std::vector<FailedTU> &failedTUs,
                                  const std::vector<Diagnostic> &diagnostics,
                                  const EscapeSummary &escapeSummary,
                                  const ThreadRoleSummary &threadRoles,
                                  const StripedArraySummary &striped,
                                  const ScanCoverage &coverage,
                                  const std::string &src = {}) {
    auto esc = [](const std::string &s) -> std::string {
        std::string out;
        out.reserve(s.size() + 4);
        for (char c : s) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default: out += c;
            }
        }
        return out;
    };

    std::string buf;
    buf.reserve(4096);
    buf += "{";
    if (!src.empty()) { buf += "\"src\":\""; buf += esc(src); buf += "\","; }
    buf += "\"exitCode\":";
    buf += std::to_string(exitCode);
    buf += ",\"failedTUs\":[";
    for (size_t i = 0; i < failedTUs.size(); ++i) {
        if (i) buf += ',';
        buf += "{\"file\":\""; buf += esc(failedTUs[i].file); buf += "\",";
        buf += "\"error\":\""; buf += esc(failedTUs[i].error); buf += "\"}";
    }
    buf += "],\"diagnostics\":[";
    for (size_t i = 0; i < diagnostics.size(); ++i) {
        const auto &d = diagnostics[i];
        if (i) buf += ',';
        buf += "{\"ruleID\":\"" + esc(d.ruleID) + "\"";
        buf += ",\"title\":\"" + esc(d.title) + "\"";
        buf += ",\"severity\":\"" + std::string(severityToString(d.severity)) + "\"";
        buf += ",\"confidence\":" + std::to_string(d.confidence);
        buf += ",\"evidenceTier\":\"" + std::string(evidenceTierName(d.evidenceTier)) + "\"";
        buf += ",\"suppressed\":" + std::string(d.suppressed ? "true" : "false");
        buf += ",\"location\":{\"file\":\"" + esc(d.location.file) + "\"";
        buf += ",\"line\":" + std::to_string(d.location.line);
        buf += ",\"column\":" + std::to_string(d.location.column) + "}";
        buf += ",\"functionName\":\"" + esc(d.functionName) + "\"";
        buf += ",\"hardwareReasoning\":\"" + esc(d.hardwareReasoning) + "\"";
        buf += ",\"structuralEvidence\":{";
        bool first = true;
        for (const auto &[k, v] : d.structuralEvidence) {
            if (!first) buf += ',';
            buf += '"'; buf += esc(k); buf += "\":\"";
            buf += esc(v); buf += '"';
            first = false;
        }
        buf += "}";
        buf += ",\"mitigation\":\"" + esc(d.mitigation) + "\"";
        buf += ",\"escalations\":[";
        for (size_t j = 0; j < d.escalations.size(); ++j) {
            if (j) buf += ',';
            buf += '"'; buf += esc(d.escalations[j]); buf += '"';
        }
        buf += "]";
        // Unserialized, this would make the cross-TU hotness verdict depend
        // on whether the shard ran forked or sequentially.
        buf += ",\"hot\":" + std::to_string(static_cast<unsigned>(d.hotness));
        // Unserialized fields are a jobs-dependent verdict: present on the
        // sequential path, gone on the forked one. This is the third such
        // field after the FL003 writer tier and the thread-writer escape
        // signal, so it crosses the boundary with the rest.
        buf += ",\"mc\":[";
        for (size_t j = 0; j < d.mechanismClaims.size(); ++j) {
            const auto &c = d.mechanismClaims[j];
            if (j) buf += ',';
            buf += "{\"e\":\"" + esc(c.effect) + "\",\"p\":\"" +
                   esc(c.precondition) + "\",\"k\":" +
                   std::to_string(c.established ? 1 : 0) + ",\"g\":" +
                   std::to_string(c.gating ? 1 : 0) + ",\"s\":\"" +
                   std::string(severityToString(c.supports)) + "\"}";
        }
        buf += "]}";
    }
    buf += "]";

    // Escape summary: {"typeName":{a:0/1,s:0/1,o:0/1,v:0/1,p:0/1,n:N},...}
    buf += ",\"escapeSummary\":{";
    {
        bool first = true;
        for (const auto &[name, sig] : escapeSummary) {
            if (!first) buf += ',';
            buf += '"'; buf += esc(name); buf += "\":{";
            buf += "\"a\":" + std::to_string(sig.hasAtomics ? 1 : 0);
            buf += ",\"s\":" + std::to_string(sig.hasSyncPrims ? 1 : 0);
            buf += ",\"o\":" + std::to_string(sig.hasSharedOwner ? 1 : 0);
            buf += ",\"v\":" + std::to_string(sig.hasVolatile ? 1 : 0);
            buf += ",\"p\":" + std::to_string(sig.hasPublication ? 1 : 0);
            buf += ",\"tw\":" + std::to_string(sig.hasThreadWriters ? 1 : 0);
            buf += ",\"gi\":" + std::to_string(sig.hasGlobalInstance ? 1 : 0);
            buf += ",\"tb\":" + std::to_string(sig.hasThreadBorneWriter ? 1 : 0);
            buf += ",\"sw\":" + std::to_string(sig.hasStandingWrites ? 1 : 0);
            buf += ",\"l\":" + std::to_string(sig.hasDeliberateLayout ? 1 : 0);
            buf += ",\"n\":" + std::to_string(sig.accessorCount);
            buf += '}';
            first = false;
        }
    }
    buf += "}";

    // Thread-role facts: {"entries":[...],"edges":{caller:[callees]},
    // "fieldWriters":{"Type::field":[writers]}}. Ordered containers give
    // a canonical byte sequence for identical facts.
    auto emitNameSets =
        [&](const std::map<std::string, std::set<std::string>> &m) {
            bool firstKey = true;
            for (const auto &[k, vals] : m) {
                if (!firstKey) buf += ',';
                buf += '"'; buf += esc(k); buf += "\":[";
                bool firstVal = true;
                for (const auto &v : vals) {
                    if (!firstVal) buf += ',';
                    buf += '"'; buf += esc(v); buf += '"';
                    firstVal = false;
                }
                buf += ']';
                firstKey = false;
            }
        };
    buf += ",\"threadRoles\":{\"entries\":[";
    {
        bool first = true;
        for (const auto &e : threadRoles.threadEntries) {
            if (!first) buf += ',';
            buf += '"'; buf += esc(e); buf += '"';
            first = false;
        }
    }
    buf += "],\"edges\":{";
    emitNameSets(threadRoles.callEdges);
    buf += "},\"fieldWriters\":{";
    emitNameSets(threadRoles.fieldWriters);
    buf += "},\"edgeDepth\":{";
    {
        bool firstCaller = true;
        for (const auto &[caller, edges] : threadRoles.edgeLoopDepth) {
            if (edges.empty()) continue;
            if (!firstCaller) buf += ',';
            buf += '"'; buf += esc(caller); buf += "\":{";
            bool firstEdge = true;
            for (const auto &[callee, d] : edges) {
                if (!firstEdge) buf += ',';
                buf += '"'; buf += esc(callee); buf += "\":" + std::to_string(d);
                firstEdge = false;
            }
            buf += '}';
            firstCaller = false;
        }
    }
    buf += "},\"ownDepth\":{";
    {
        bool first = true;
        for (const auto &[fn, d] : threadRoles.ownLoopDepth) {
            if (!first) buf += ',';
            buf += '"'; buf += esc(fn); buf += "\":" + std::to_string(d);
            first = false;
        }
    }
    buf += "},\"overridden\":[";
    {
        bool first = true;
        for (const auto &m : threadRoles.overriddenVirtuals) {
            if (!first) buf += ',';
            buf += '"'; buf += esc(m); buf += '"';
            first = false;
        }
    }
    buf += "]}";

    buf += ",\"striped\":{";
    {
        bool first = true;
        for (const auto &[k, a] : striped) {
            if (!first) buf += ',';
            buf += '"'; buf += esc(k); buf += "\":{";
            buf += "\"n\":\"" + esc(a.displayName) + "\"";
            buf += ",\"t\":\"" + esc(a.typeName) + "\"";
            buf += ",\"f\":\"" + esc(a.file) + "\"";
            buf += ",\"l\":" + std::to_string(a.line);
            buf += ",\"es\":" + std::to_string(a.elemSizeBytes);
            buf += ",\"ec\":" + std::to_string(a.elemCount);
            buf += ",\"al\":" + std::to_string(a.declAlignBytes);
            buf += ",\"at\":" + std::to_string(a.elementIsAtomic ? 1 : 0);
            buf += ",\"vo\":" + std::to_string(a.elementIsVolatile ? 1 : 0);
            buf += ",\"st\":" + std::to_string(a.isFileStatic ? 1 : 0);
            buf += ",\"tls\":" + std::to_string(a.tlsIndexed ? 1 : 0);
            buf += ",\"ho\":" + std::to_string(a.indexIsHandedOver ? 1 : 0);
            buf += ",\"oi\":" + std::to_string(a.indexIsOwnIdentity ? 1 : 0);
            buf += ",\"hp\":" + std::to_string(a.hasHeadPaddingOffset ? 1 : 0);
            buf += ",\"wt\":" + std::to_string(a.writerTier);
            buf += ",\"w\":[";
            bool fw = true;
            for (const auto &w : a.stripedWriters) {
                if (!fw) buf += ',';
                buf += '"'; buf += esc(w); buf += '"'; fw = false;
            }
            buf += "],\"ag\":[";
            bool fa = true;
            for (const auto &g : a.aggregators) {
                if (!fa) buf += ',';
                buf += '"'; buf += esc(g); buf += '"'; fa = false;
            }
            buf += "]}";
            first = false;
        }
    }
    buf += "}";

    buf += ",\"cov\":{\"fs\":" + std::to_string(coverage.functionsSeen) +
           ",\"fh\":" + std::to_string(coverage.functionsHot) +
           ",\"rs\":" + std::to_string(coverage.recordsSeen) + "}";

    buf += "}";
    return buf;
}

// Child exit status reserved for "analysis ran, IPC handoff failed". Distinct
// from 1 (analysis itself reported an error) so the parent does not blame the
// source when the temp filesystem is at fault.
constexpr int kShardIPCWriteFailed = 42;

// Shard hit its address-space cap. Distinct from a crash because the operator
// action differs: raise --memory-limit-mb or lower --jobs.
constexpr int kShardMemoryExhausted = 43;

// Address space a shard may map, MiB. Derived from *total* memory, not
// available: available fluctuates with whatever else the box is doing, and a
// cap that moves between runs would make output depend on ambient load.
unsigned resolveShardMemoryLimitMB(unsigned requested, unsigned jobs) {
    if (requested != 0)
        return requested;
    const long pages = ::sysconf(_SC_PHYS_PAGES);
    const long pageSz = ::sysconf(_SC_PAGESIZE);
    if (pages <= 0 || pageSz <= 0)
        return 0; // cannot size it honestly; leave uncapped 
    const unsigned long long totalMB =
        (static_cast<unsigned long long>(pages) * pageSz) >> 20;
    // Leave headroom for the parent and the rest of the machine.
    const unsigned long long share = (totalMB * 3 / 4) / std::max(1u, jobs);
    return static_cast<unsigned>(std::max(2048ULL, share));
}

struct ShardIPC {
    std::string src;   // TU this record covers; empty in whole-shard form
    int exitCode = -1;
    std::vector<FailedTU> failedTUs;
    std::vector<Diagnostic> diagnostics;
    EscapeSummary escapeSummary;
    ThreadRoleSummary threadRoles;
    StripedArraySummary striped;
    ScanCoverage coverage;
};

namespace ipc {

static void skipWS(const std::string &s, size_t &i) {
    while (i < s.size() && (s[i]==' '||s[i]=='\n'||s[i]=='\r'||s[i]=='\t'))
        ++i;
}

static void skipValue(const std::string &s, size_t &i) {
    skipWS(s, i);
    if (i >= s.size()) return;
    if (s[i] == '"') {
        ++i;
        while (i < s.size() && s[i] != '"') {
            if (s[i] == '\\') ++i;
            ++i;
        }
        if (i < s.size()) ++i;
    } else if (s[i] == '[' || s[i] == '{') {
        char open = s[i];
        char close = (open == '[') ? ']' : '}';
        ++i;
        int depth = 1;
        while (i < s.size() && depth > 0) {
            if (s[i] == open) ++depth;
            else if (s[i] == close) --depth;
            ++i;
        }
    } else {
        while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']')
            ++i;
    }
}

static bool expect(const std::string &s, size_t &i, char c) {
    skipWS(s, i);
    if (i < s.size() && s[i] == c) { ++i; return true; }
    return false;
}

static std::string parseStr(const std::string &s, size_t &i) {
    skipWS(s, i);
    if (i >= s.size() || s[i] != '"') return {};
    ++i;
    std::string out;
    while (i < s.size() && s[i] != '"') {
        if (s[i] == '\\' && i + 1 < s.size()) {
            ++i;
            switch (s[i]) {
                case '"':  out += '"'; break;
                case '\\': out += '\\'; break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                default:   out += s[i]; break;
            }
        } else {
            out += s[i];
        }
        ++i;
    }
    if (i < s.size()) ++i;
    return out;
}

static double parseNum(const std::string &s, size_t &i) {
    skipWS(s, i);
    size_t start = i;
    if (i < s.size() && s[i] == '-') ++i;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
    if (i < s.size() && s[i] == '.') {
        ++i;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
    }
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        ++i;
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
    }
    if (start == i) return 0.0;
    return std::stod(s.substr(start, i - start));
}

static bool parseBool(const std::string &s, size_t &i) {
    skipWS(s, i);
    if (s.compare(i, 4, "true") == 0) { i += 4; return true; }
    if (s.compare(i, 5, "false") == 0) { i += 5; return false; }
    return false;
}


static Severity toSeverity(const std::string &s) {
    if (s == "Critical") return Severity::Critical;
    if (s == "High") return Severity::High;
    if (s == "Medium") return Severity::Medium;
    return Severity::Informational;
}

static EvidenceTier toTier(const std::string &s) {
    if (s == "proven") return EvidenceTier::Proven;
    if (s == "likely") return EvidenceTier::Likely;
    return EvidenceTier::Speculative;
}

static Diagnostic parseDiag(const std::string &s, size_t &i) {
    Diagnostic d;
    while (true) {
        skipWS(s, i);
        if (i >= s.size() || s[i] == '}') { if (i < s.size()) ++i; break; }
        std::string key = parseStr(s, i);
        expect(s, i, ':');
        if (key == "ruleID")            d.ruleID = parseStr(s, i);
        else if (key == "title")        d.title = parseStr(s, i);
        else if (key == "severity")     d.severity = toSeverity(parseStr(s, i));
        else if (key == "confidence")   d.confidence = parseNum(s, i);
        else if (key == "evidenceTier") d.evidenceTier = toTier(parseStr(s, i));
        else if (key == "suppressed")   d.suppressed = parseBool(s, i);
        else if (key == "functionName") d.functionName = parseStr(s, i);
        else if (key == "hardwareReasoning") d.hardwareReasoning = parseStr(s, i);
        else if (key == "mitigation")   d.mitigation = parseStr(s, i);
        else if (key == "location") {
            expect(s, i, '{');
            while (true) {
                skipWS(s, i);
                if (i >= s.size() || s[i] == '}') { if (i < s.size()) ++i; break; }
                std::string lk = parseStr(s, i);
                expect(s, i, ':');
                if (lk == "file")        d.location.file = parseStr(s, i);
                else if (lk == "line")   d.location.line = static_cast<unsigned>(parseNum(s, i));
                else if (lk == "column") d.location.column = static_cast<unsigned>(parseNum(s, i));
                else skipValue(s, i);
                expect(s, i, ',');
            }
        } else if (key == "structuralEvidence") {
            expect(s, i, '{');
            while (true) {
                skipWS(s, i);
                if (i >= s.size() || s[i] == '}') { if (i < s.size()) ++i; break; }
                std::string ek = parseStr(s, i);
                expect(s, i, ':');
                d.structuralEvidence[ek] = parseStr(s, i);
                expect(s, i, ',');
            }
        } else if (key == "escalations") {
            expect(s, i, '[');
            while (true) {
                skipWS(s, i);
                if (i >= s.size() || s[i] == ']') { if (i < s.size()) ++i; break; }
                d.escalations.push_back(parseStr(s, i));
                expect(s, i, ',');
            }
        } else if (key == "hot") {
            d.hotness = static_cast<uint8_t>(parseNum(s, i));
        } else if (key == "mc") {
            expect(s, i, '[');
            while (true) {
                skipWS(s, i);
                if (i >= s.size() || s[i] == ']') { if (i < s.size()) ++i; break; }
                expect(s, i, '{');
                MechanismClaim c;
                while (true) {
                    skipWS(s, i);
                    if (i >= s.size() || s[i] == '}') { if (i < s.size()) ++i; break; }
                    std::string ck = parseStr(s, i);
                    expect(s, i, ':');
                    if (ck == "e")      c.effect = parseStr(s, i);
                    else if (ck == "p") c.precondition = parseStr(s, i);
                    else if (ck == "k") c.established = parseNum(s, i) != 0;
                    else if (ck == "g") c.gating = parseNum(s, i) != 0;
                    else if (ck == "s") c.supports = toSeverity(parseStr(s, i));
                    else skipValue(s, i);
                    expect(s, i, ',');
                }
                d.mechanismClaims.push_back(std::move(c));
                expect(s, i, ',');
            }
        } else {
            skipValue(s, i);
        }
        expect(s, i, ',');
    }
    return d;
}

} // namespace ipc

bool deserializeShardResult(const std::string &json, ShardIPC &out) {
    size_t i = 0;
    if (!ipc::expect(json, i, '{')) return false;
    while (true) {
        ipc::skipWS(json, i);
        if (i >= json.size() || json[i] == '}') break;
        std::string key = ipc::parseStr(json, i);
        ipc::expect(json, i, ':');
        if (key == "src") {
            out.src = ipc::parseStr(json, i);
        } else if (key == "exitCode") {
            out.exitCode = static_cast<int>(ipc::parseNum(json, i));
        } else if (key == "failedTUs") {
            ipc::expect(json, i, '[');
            while (true) {
                ipc::skipWS(json, i);
                if (i >= json.size() || json[i] == ']') { if (i < json.size()) ++i; break; }
                ipc::expect(json, i, '{');
                FailedTU ftu;
                while (true) {
                    ipc::skipWS(json, i);
                    if (json[i] == '}') { ++i; break; }
                    std::string subkey = ipc::parseStr(json, i);
                    ipc::expect(json, i, ':');
                    if (subkey == "file") {
                        ftu.file = ipc::parseStr(json, i);
                    } else if (subkey == "error") {
                        ftu.error = ipc::parseStr(json, i);
                    } else {
                        ipc::skipValue(json, i);
                    }
                    ipc::skipWS(json, i);
                    if (json[i] == ',') { ++i; continue; }
                    if (json[i] == '}') { ++i; break; }
                }
                out.failedTUs.push_back(std::move(ftu));
                ipc::skipWS(json, i);
                if (json[i] == ',') { ++i; }
            }
        } else if (key == "diagnostics") {
            ipc::expect(json, i, '[');
            while (true) {
                ipc::skipWS(json, i);
                if (i >= json.size() || json[i] == ']') { if (i < json.size()) ++i; break; }
                if (!ipc::expect(json, i, '{')) break;
                out.diagnostics.push_back(ipc::parseDiag(json, i));
                ipc::expect(json, i, ',');
            }
        } else if (key == "escapeSummary") {
            ipc::expect(json, i, '{');
            while (true) {
                ipc::skipWS(json, i);
                if (i >= json.size() || json[i] == '}') { if (i < json.size()) ++i; break; }
                std::string typeName = ipc::parseStr(json, i);
                ipc::expect(json, i, ':');
                ipc::expect(json, i, '{');
                TypeEscapeSignals sig;
                while (true) {
                    ipc::skipWS(json, i);
                    if (i >= json.size() || json[i] == '}') { if (i < json.size()) ++i; break; }
                    std::string sk = ipc::parseStr(json, i);
                    ipc::expect(json, i, ':');
                    auto val = static_cast<int>(ipc::parseNum(json, i));
                    if (sk == "a") sig.hasAtomics = val != 0;
                    else if (sk == "s") sig.hasSyncPrims = val != 0;
                    else if (sk == "o") sig.hasSharedOwner = val != 0;
                    else if (sk == "v") sig.hasVolatile = val != 0;
                    else if (sk == "p") sig.hasPublication = val != 0;
                    else if (sk == "tw") sig.hasThreadWriters = val != 0;
                    else if (sk == "gi") sig.hasGlobalInstance = val != 0;
                    else if (sk == "tb") sig.hasThreadBorneWriter = val != 0;
                    else if (sk == "sw") sig.hasStandingWrites = val != 0;
                    else if (sk == "l") sig.hasDeliberateLayout = val != 0;
                    else if (sk == "n") sig.accessorCount = static_cast<unsigned>(val);
                    ipc::expect(json, i, ',');
                }
                out.escapeSummary[typeName].merge(sig);
                ipc::expect(json, i, ',');
            }
        } else if (key == "threadRoles") {
            auto parseStrArray = [&](std::set<std::string> &dst) {
                ipc::expect(json, i, '[');
                while (true) {
                    ipc::skipWS(json, i);
                    if (i >= json.size() || json[i] == ']') {
                        if (i < json.size()) ++i;
                        break;
                    }
                    dst.insert(ipc::parseStr(json, i));
                    ipc::expect(json, i, ',');
                }
            };
            auto parseNameSets =
                [&](std::map<std::string, std::set<std::string>> &dst) {
                    ipc::expect(json, i, '{');
                    while (true) {
                        ipc::skipWS(json, i);
                        if (i >= json.size() || json[i] == '}') {
                            if (i < json.size()) ++i;
                            break;
                        }
                        std::string k = ipc::parseStr(json, i);
                        ipc::expect(json, i, ':');
                        parseStrArray(dst[k]);
                        ipc::expect(json, i, ',');
                    }
                };
            ipc::expect(json, i, '{');
            while (true) {
                ipc::skipWS(json, i);
                if (i >= json.size() || json[i] == '}') {
                    if (i < json.size()) ++i;
                    break;
                }
                std::string tk = ipc::parseStr(json, i);
                ipc::expect(json, i, ':');
                if (tk == "entries")
                    parseStrArray(out.threadRoles.threadEntries);
                else if (tk == "edges")
                    parseNameSets(out.threadRoles.callEdges);
                else if (tk == "fieldWriters")
                    parseNameSets(out.threadRoles.fieldWriters);
                else if (tk == "overridden")
                    parseStrArray(out.threadRoles.overriddenVirtuals);
                else if (tk == "edgeDepth") {
                    ipc::expect(json, i, '{');
                    while (true) {
                        ipc::skipWS(json, i);
                        if (i >= json.size() || json[i] == '}') {
                            if (i < json.size()) ++i;
                            break;
                        }
                        std::string caller = ipc::parseStr(json, i);
                        ipc::expect(json, i, ':');
                        auto &dst = out.threadRoles.edgeLoopDepth[caller];
                        ipc::expect(json, i, '{');
                        while (true) {
                            ipc::skipWS(json, i);
                            if (i >= json.size() || json[i] == '}') {
                                if (i < json.size()) ++i;
                                break;
                            }
                            std::string callee = ipc::parseStr(json, i);
                            ipc::expect(json, i, ':');
                            dst[callee] =
                                static_cast<unsigned>(ipc::parseNum(json, i));
                            ipc::expect(json, i, ',');
                        }
                        ipc::expect(json, i, ',');
                    }
                } else if (tk == "ownDepth") {
                    ipc::expect(json, i, '{');
                    while (true) {
                        ipc::skipWS(json, i);
                        if (i >= json.size() || json[i] == '}') {
                            if (i < json.size()) ++i;
                            break;
                        }
                        std::string fn = ipc::parseStr(json, i);
                        ipc::expect(json, i, ':');
                        out.threadRoles.ownLoopDepth[fn] =
                            static_cast<unsigned>(ipc::parseNum(json, i));
                        ipc::expect(json, i, ',');
                    }
                } else
                    ipc::skipValue(json, i);
                ipc::expect(json, i, ',');
            }
        } else if (key == "striped") {
            ipc::expect(json, i, '{');
            while (true) {
                ipc::skipWS(json, i);
                if (i >= json.size() || json[i] == '}') {
                    if (i < json.size()) ++i;
                    break;
                }
                std::string k = ipc::parseStr(json, i);
                ipc::expect(json, i, ':');
                ipc::expect(json, i, '{');
                StripedArraySite a;
                a.key = k;
                while (true) {
                    ipc::skipWS(json, i);
                    if (i >= json.size() || json[i] == '}') {
                        if (i < json.size()) ++i;
                        break;
                    }
                    std::string f = ipc::parseStr(json, i);
                    ipc::expect(json, i, ':');
                    auto strArray = [&](std::set<std::string> &dst) {
                        ipc::expect(json, i, '[');
                        while (true) {
                            ipc::skipWS(json, i);
                            if (i >= json.size() || json[i] == ']') {
                                if (i < json.size()) ++i;
                                break;
                            }
                            dst.insert(ipc::parseStr(json, i));
                            ipc::expect(json, i, ',');
                        }
                    };
                    if (f == "n") a.displayName = ipc::parseStr(json, i);
                    else if (f == "t") a.typeName = ipc::parseStr(json, i);
                    else if (f == "f") a.file = ipc::parseStr(json, i);
                    else if (f == "w") strArray(a.stripedWriters);
                    else if (f == "ag") strArray(a.aggregators);
                    else {
                        auto v = static_cast<uint64_t>(ipc::parseNum(json, i));
                        if (f == "l") a.line = static_cast<unsigned>(v);
                        else if (f == "es") a.elemSizeBytes = v;
                        else if (f == "ec") a.elemCount = v;
                        else if (f == "al") a.declAlignBytes = v;
                        else if (f == "at") a.elementIsAtomic = v != 0;
                        else if (f == "vo") a.elementIsVolatile = v != 0;
                        else if (f == "st") a.isFileStatic = v != 0;
                        else if (f == "tls") a.tlsIndexed = v != 0;
                        else if (f == "ho") a.indexIsHandedOver = v != 0;
                        else if (f == "oi") a.indexIsOwnIdentity = v != 0;
                        else if (f == "hp") a.hasHeadPaddingOffset = v != 0;
                        else if (f == "wt") a.writerTier =
                            static_cast<uint8_t>(v);
                    }
                    ipc::expect(json, i, ',');
                }
                auto it = out.striped.find(k);
                if (it == out.striped.end()) out.striped.emplace(k, std::move(a));
                else it->second.merge(a);
                ipc::expect(json, i, ',');
            }
        } else if (key == "cov") {
            ipc::expect(json, i, '{');
            while (true) {
                ipc::skipWS(json, i);
                if (i >= json.size() || json[i] == '}') {
                    if (i < json.size()) ++i;
                    break;
                }
                std::string k = ipc::parseStr(json, i);
                ipc::expect(json, i, ':');
                auto v = static_cast<uint64_t>(ipc::parseNum(json, i));
                if (k == "fs")      out.coverage.functionsSeen = v;
                else if (k == "fh") out.coverage.functionsHot = v;
                else if (k == "rs") out.coverage.recordsSeen = v;
                ipc::expect(json, i, ',');
            }
        } else {
            ipc::skipValue(json, i);
        }
        ipc::expect(json, i, ',');
    }
    return true;
}

} // anonymous namespace

// --- Pipeline stages ---

static std::unordered_set<std::string> loadProfileHotFunctions(
        const ScanRequest &req) {
    std::string profilePath = req.perfProfilePath;
    if (profilePath.empty())
        profilePath = req.config.perfProfilePath;
    if (profilePath.empty())
        return {};

    PerfProfileParser parser;
    if (!parser.parse(profilePath))
        return {};

    double threshold = req.hotnessThreshold;
    if (req.config.hotnessThresholdPct > 0 && threshold == 1.0)
        threshold = req.config.hotnessThresholdPct;

    return parser.hotFunctions(threshold);
}

static void runIRPass(
        const ScanRequest &req,
        const clang::tooling::CompilationDatabase &compDB,
        const std::vector<std::string> &sources,
        const std::unordered_set<std::string> &failedFiles,
        std::vector<Diagnostic> &diagnostics,
        ExecutionMetadata &meta,
        std::vector<OptRemark> &remarks,
        unsigned &remarkFilesFailed) {

    IRAnalyzer irAnalyzer;
    std::string optLevel = "-" + req.ir.optLevel;

    std::vector<IRJob> jobs;

    for (const auto &srcPath : sources) {
        // AST-failed TUs already reported; re-driving clang would just
        // duplicate the same errors.
        if (failedFiles.count(srcPath))
            continue;
        auto cmds = compDB.getCompileCommands(srcPath);
        if (cmds.empty())
            continue;

        const std::string &dbCompiler = cmds.front().CommandLine.front();
        std::string compilerPath = resolveCompiler(dbCompiler);
        if (compilerPath.empty())
            continue;

        std::vector<std::string> argv;
        argv.push_back(compilerPath);
        argv.push_back("-S");
        argv.push_back("-emit-llvm");
        argv.push_back("-g");
        argv.push_back(optLevel);

        // Sanitize compile commands: strip GCC-only flags, suppress
        // unknown attribute warnings (gnu_printf, etc.)
        for (const auto &cmd : cmds) {
            auto sanitized = sanitizeForClangIR(cmd.CommandLine, srcPath);
            argv.insert(argv.end(), sanitized.begin(), sanitized.end());
        }

        // Cache key.

        llvm::MD5 hasher;
        auto srcBuf = llvm::MemoryBuffer::getFile(srcPath);
        if (srcBuf)
            hasher.update((*srcBuf)->getBuffer());
        else
            hasher.update(srcPath);

        llvm::sys::fs::file_status srcStat;
        if (!llvm::sys::fs::status(srcPath, srcStat)) {
            auto mtime = srcStat.getLastModificationTime()
                             .time_since_epoch().count();
            hasher.update(llvm::StringRef(
                reinterpret_cast<const char *>(&mtime), sizeof(mtime)));
        }

        llvm::SmallString<256> depPath(srcPath);
        llvm::sys::path::replace_extension(depPath, ".d");
        auto depBuf = llvm::MemoryBuffer::getFile(depPath);
        if (depBuf)
            hasher.update((*depBuf)->getBuffer());

        for (const auto &a : argv)
            hasher.update(a);
        hasher.update(kToolVersion);
        llvm::MD5::MD5Result hashResult;
        hasher.final(hashResult);
        llvm::SmallString<32> hashStr;
        llvm::MD5::stringifyResult(hashResult, hashStr);

        llvm::SmallString<128> tmpDir;
        llvm::sys::path::system_temp_directory(true, tmpDir);
        llvm::SmallString<128> irPath(tmpDir), errPath(tmpDir), remPath(tmpDir);
        llvm::sys::path::append(irPath,
            "lshaz-" + std::string(hashStr) + ".ll");
        llvm::sys::path::append(errPath,
            "lshaz-" + std::string(hashStr) + ".err");
        llvm::sys::path::append(remPath,
            "lshaz-" + std::string(hashStr) + ".opt.yaml");

        // Both artifacts or neither: a hit on the IR alone would silently
        // drop this TU's remarks, so the finding set would depend on what
        // happened to be in /tmp.
        bool cached = req.ir.cacheEnabled && llvm::sys::fs::exists(irPath) &&
                      llvm::sys::fs::exists(remPath);

        argv.push_back("-fsave-optimization-record");
        argv.push_back("-Xclang");
        argv.push_back("-opt-record-passes");
        argv.push_back("-Xclang");
        argv.push_back(remarkPassFilter().str());
        argv.push_back("-foptimization-record-file=" + std::string(remPath));
        argv.push_back("-o");
        argv.push_back(std::string(irPath));
        argv.push_back(srcPath);

        jobs.push_back({srcPath, compilerPath, std::move(argv),
                        std::string(irPath), std::string(errPath),
                        std::string(remPath), cached});

        // Track compilers.
        bool seen = false;
        for (const auto &ci : meta.compilers)
            if (ci.path == compilerPath) { seen = true; break; }
        if (!seen)
            meta.compilers.push_back({compilerPath, {}});
    }

    if (jobs.empty())
        return;

    // Shard-based parallel IR emission.
    unsigned maxWorkers = req.ir.maxJobs;
    if (maxWorkers == 0)
        maxWorkers = std::max(1u, std::thread::hardware_concurrency());
    unsigned batchSize = std::max(1u, req.ir.batchSize);

    struct Shard { size_t begin; size_t end; };
    std::vector<Shard> shards;
    for (size_t i = 0; i < jobs.size(); i += batchSize)
        shards.push_back({i, std::min(i + batchSize, jobs.size())});

    unsigned shardWorkers = std::min(maxWorkers,
                                      static_cast<unsigned>(shards.size()));
    std::counting_semaphore<> sem(shardWorkers);

    struct ShardResult {
        IRAnalyzer analyzer;
        std::vector<std::pair<size_t, IRResult>> jobResults;
    };

    std::vector<std::future<ShardResult>> futures;
    futures.reserve(shards.size());

    for (const auto &shard : shards) {
        futures.push_back(std::async(std::launch::async,
            [&sem, &jobs](size_t begin, size_t end) -> ShardResult {
                sem.acquire();
                ShardResult sr;

                for (size_t i = begin; i < end; ++i)
                    sr.jobResults.push_back({i, emitOneIR(jobs[i])});

                llvm::LLVMContext llvmCtx;
                for (const auto &[idx, result] : sr.jobResults) {
                    if (result.exitCode == 0) {
                        llvm::SMDiagnostic parseErr;
                        auto mod = llvm::parseIRFile(
                            jobs[idx].irFile, parseErr, llvmCtx);
                        if (mod)
                            sr.analyzer.analyzeModule(*mod);
                    }
                }

                sem.release();
                return sr;
            },
            shard.begin, shard.end));
    }

    for (auto &future : futures) {
        auto sr = future.get();
        for (const auto &[idx, result] : sr.jobResults) {
            if (result.exitCode != 0) {
                auto errBuf = llvm::MemoryBuffer::getFile(jobs[idx].errFile);
                if (errBuf && !(*errBuf)->getBuffer().empty()) {
                    llvm::errs() << "lshaz: IR emission failed for "
                                 << jobs[idx].srcPath << ":\n"
                                 << (*errBuf)->getBuffer() << "\n";
                }
            }
            if (!jobs[idx].cached && result.exitCode != 0) {
                llvm::sys::fs::remove(jobs[idx].irFile);
                llvm::sys::fs::remove(jobs[idx].remarkFile);
            }
            llvm::sys::fs::remove(jobs[idx].errFile);
            // A container that breaks mid-stream keeps what it already
            // yielded, so the count is the only thing that says coverage
            // was partial.
            if (result.exitCode == 0 &&
                !parseOptRemarks(jobs[idx].remarkFile, remarks))
                ++remarkFilesFailed;
        }
        irAnalyzer.mergeFrom(std::move(sr.analyzer));
    }

    // Shards complete in scheduling order, so the stream is ordered here
    // rather than relied on downstream.
    std::sort(remarks.begin(), remarks.end(),
              [](const OptRemark &a, const OptRemark &b) {
                  if (a.file != b.file) return a.file < b.file;
                  if (a.line != b.line) return a.line < b.line;
                  if (a.column != b.column) return a.column < b.column;
                  if (a.name != b.name) return a.name < b.name;
                  return a.function < b.function;
              });

    if (!irAnalyzer.profiles().empty()) {
        DiagnosticRefiner refiner(irAnalyzer.profiles(),
                                  req.config.stackFrameWarnBytes);
        refiner.refine(diagnostics);
    }
}

static unsigned applyCalibrationSuppression(
        const FeedbackOptions &fb,
        std::vector<Diagnostic> &diagnostics,
        CalibrationFeedbackStore &store) {
    unsigned suppressed = 0;
    diagnostics.erase(
        std::remove_if(diagnostics.begin(), diagnostics.end(),
                       [&](const Diagnostic &d) {
                           auto hc = HypothesisConstructor
                               ::mapRuleToHazardClass(d.ruleID);
                           auto features = HypothesisConstructor
                               ::extractFeatures(d);
                           bool highSev =
                               d.severity == Severity::Critical ||
                               d.severity == Severity::High;
                           bool proven =
                               d.evidenceTier == EvidenceTier::Proven;
                           if (highSev && proven)
                               return false;
                           if (store.isKnownFalsePositive(features, hc)) {
                               ++suppressed;
                               return true;
                           }
                           return false;
                       }),
        diagnostics.end());
    return suppressed;
}

static void applyPMUFeedback(
        const FeedbackOptions &fb,
        std::vector<Diagnostic> &diagnostics,
        CalibrationFeedbackStore &store) {
    PMUTraceFeedbackLoop feedbackLoop(store);

    if (!fb.pmuPriorsPath.empty())
        feedbackLoop.loadPriors(fb.pmuPriorsPath);

    if (!fb.pmuTracePath.empty()) {
        auto traceBuf = llvm::MemoryBuffer::getFile(fb.pmuTracePath);
        if (traceBuf) {
            llvm::StringRef data = (*traceBuf)->getBuffer();
            llvm::SmallVector<llvm::StringRef, 0> lines;
            data.split(lines, '\n', -1, false);

            PMUTraceRecord currentRecord;
            auto flushRecord = [&]() {
                if (currentRecord.functionName.empty())
                    return;
                for (const auto &d : diagnostics) {
                    if (d.functionName == currentRecord.functionName ||
                        (d.location.file == currentRecord.sourceFile &&
                         d.location.line == currentRecord.sourceLine)) {
                        auto hc = HypothesisConstructor
                            ::mapRuleToHazardClass(d.ruleID);
                        auto features = HypothesisConstructor
                            ::extractFeatures(d);
                        feedbackLoop.ingestTrace(currentRecord, hc, features);
                        break;
                    }
                }
            };

            for (const auto &line : lines) {
                if (line.starts_with("#"))
                    continue;

                llvm::SmallVector<llvm::StringRef, 6> fields;
                line.split(fields, '\t');
                if (fields.size() < 5)
                    continue;

                std::string func = fields[0].str();
                std::string file = fields[1].str();
                unsigned srcLine = 0;
                fields[2].getAsInteger(10, srcLine);

                if (!currentRecord.functionName.empty() &&
                    (currentRecord.functionName != func ||
                     currentRecord.sourceLine != srcLine)) {
                    flushRecord();
                    currentRecord = {};
                }

                currentRecord.functionName = func;
                currentRecord.sourceFile = file;
                currentRecord.sourceLine = srcLine;

                PMUSample sample;
                sample.counterName = fields[3].str();
                fields[4].getAsInteger(10, sample.value);
                if (fields.size() > 5)
                    fields[5].getAsInteger(10, sample.duration_ns);
                currentRecord.samples.push_back(std::move(sample));
            }
            flushRecord();
        }
    }

    for (auto &d : diagnostics) {
        auto hc = HypothesisConstructor::mapRuleToHazardClass(d.ruleID);
        d.confidence = feedbackLoop.adjustConfidence(d.confidence, hc);
    }

    if (!fb.pmuPriorsPath.empty())
        feedbackLoop.savePriors(fb.pmuPriorsPath);
}

// Cross-TU thread-role escalation. FL002 joins at pair granularity via
// its pair_fields evidence; FL090's claim is struct-wide, so any two
// attributed fields of the type with disjoint roles evidence its
// concurrency assumption. Confidence-only by design: severity caps from
// the deliberate-layout demotion contract stay intact, and a demoted
// struct whose flagged pair still attributes to disjoint threads is
// exactly the "mitigation exists but missed this pair" signal.
static const char *roleMaskName(uint8_t mask) {
    switch (mask) {
        case ROLE_MAIN:   return "main-thread";
        case ROLE_WORKER: return "worker-thread";
        default:          return "mixed-role";
    }
}

static unsigned applyThreadRoleEscalation(
        std::vector<Diagnostic> &diagnostics,
        const ThreadRoleSummary &facts,
        const ThreadRoleVerdicts &verdicts) {
    if (verdicts.functionRoles.empty())
        return 0;

    unsigned escalated = 0;
    for (auto &d : diagnostics) {
        if (d.suppressed)
            continue;
        if (d.ruleID != "FL002" && d.ruleID != "FL090")
            continue;
        auto tit = d.structuralEvidence.find("type_name");
        if (tit == d.structuralEvidence.end() || tit->second.empty())
            continue;
        const std::string &type = tit->second;

        std::string fieldA, fieldB;
        if (d.ruleID == "FL002") {
            auto pit = d.structuralEvidence.find("pair_fields");
            if (pit == d.structuralEvidence.end())
                continue;
            // "a|b;c|d" - first disjoint pair in flagged order wins.
            const std::string &pf = pit->second;
            size_t pos = 0;
            while (pos < pf.size() && fieldA.empty()) {
                size_t semi = pf.find(';', pos);
                std::string pair = pf.substr(pos, semi == std::string::npos
                                                      ? std::string::npos
                                                      : semi - pos);
                size_t bar = pair.find('|');
                if (bar != std::string::npos) {
                    std::string a = pair.substr(0, bar);
                    std::string b = pair.substr(bar + 1);
                    if (verdicts.fieldsHaveDisjointWriterRoles(
                            facts, type + "::" + a, type + "::" + b)) {
                        fieldA = a;
                        fieldB = b;
                    }
                }
                if (semi == std::string::npos)
                    break;
                pos = semi + 1;
            }
        } else {
            // FL090: first (lexicographic, via ordered map) disjoint pair
            // among the type's attributed fields.
            const std::string prefix = type + "::";
            std::vector<std::pair<std::string, uint8_t>> attributed;
            for (auto it = facts.fieldWriters.lower_bound(prefix);
                 it != facts.fieldWriters.end() &&
                 it->first.compare(0, prefix.size(), prefix) == 0;
                 ++it) {
                uint8_t mask = verdicts.fieldWriterRoles(facts, it->first);
                if (mask != ROLE_NONE)
                    attributed.emplace_back(
                        it->first.substr(prefix.size()), mask);
            }
            for (size_t i = 0; i < attributed.size() && fieldA.empty(); ++i)
                for (size_t j = i + 1; j < attributed.size(); ++j)
                    if ((attributed[i].second & attributed[j].second) == 0) {
                        fieldA = attributed[i].first;
                        fieldB = attributed[j].first;
                        break;
                    }
        }
        if (fieldA.empty())
            continue;

        uint8_t ra = verdicts.fieldWriterRoles(facts, type + "::" + fieldA);
        uint8_t rb = verdicts.fieldWriterRoles(facts, type + "::" + fieldB);
        d.confidence = std::min(d.confidence + 0.08, 0.95);
        d.escalations.push_back(
            "cross-TU thread-role attribution: '" + fieldA +
            "' written only from " + roleMaskName(ra) + " code, '" + fieldB +
            "' only from " + roleMaskName(rb) +
            " code. Concurrent cross-thread writes evidenced, not assumed");
        ++escalated;
    }
    return escalated;
}

// FL060 first-touch heuristics assume nobody is managing placement. A
// codebase calling affinity/mempolicy APIs has an author doing exactly
// that; the same respect contract deliberate layout earns from FL002.
// Reduce-side only: the merged call edges already carry every direct
// callee name.
static std::string detectAffinityManagement(const ThreadRoleSummary &facts) {
    static const char *kAPIs[] = {
        "pthread_setaffinity_np", "sched_setaffinity", "mbind",
        "set_mempolicy",          "numa_bind",         "numa_run_on_node",
        "numa_set_membind",       "numa_alloc_onnode", "numa_tonode_memory",
    };
    // Ordered facts + fixed API order = deterministic first match.
    for (const char *api : kAPIs)
        for (const auto &[caller, callees] : facts.callEdges)
            if (callees.count(api))
                return api;
    return {};
}

// Same contract for FL070: in-tree madvise/mallopt means the author
// already manages paging policy.
static bool detectPagingManagement(const ThreadRoleSummary &facts) {
    for (const auto &[caller, callees] : facts.callEdges)
        if (callees.count("madvise") || callees.count("posix_madvise") ||
            callees.count("mallopt"))
            return true;
    return false;
}

static unsigned applyPagingRespect(std::vector<Diagnostic> &diagnostics,
                                   bool managed) {
    if (!managed)
        return 0;
    unsigned demoted = 0;
    for (auto &d : diagnostics) {
        if (d.suppressed || d.ruleID != "FL070")
            continue;
        d.severity = Severity::Informational;
        d.confidence = std::max(d.confidence - 0.10, 0.05);
        d.escalations.push_back(
            "paging policy managed in-tree (madvise/mallopt observed): "
            "hugepage inference demoted, verify against the author's "
            "policy");
        ++demoted;
    }
    return demoted;
}

static unsigned applyAffinityRespect(std::vector<Diagnostic> &diagnostics,
                                     const std::string &api) {
    if (api.empty())
        return 0;
    unsigned demoted = 0;
    for (auto &d : diagnostics) {
        if (d.suppressed || d.ruleID != "FL060")
            continue;
        if (d.severity == Severity::Critical)
            d.severity = Severity::High;
        else if (d.severity == Severity::High)
            d.severity = Severity::Medium;
        else
            d.severity = Severity::Informational;
        d.confidence = std::max(d.confidence - 0.10, 0.05);
        d.escalations.push_back(
            "explicit placement management in-tree (" + api +
            "): first-touch inference demoted. The author is already "
            "steering affinity; verify against their policy, not the "
            "default model");
        ++demoted;
    }
    return demoted;
}

// FL003: per-thread striped arrays. slots pack floor(line/elemSize) per
// line; slots i and j written by different cores trade the line in
// Modified state every update. emitted in reduce because the writer-role
// join only exists post-merge.
static unsigned emitStripedArrayFindings(
        std::vector<Diagnostic> &diagnostics,
        const StripedArraySummary &striped,
        const ThreadRoleVerdicts &roles,
        uint64_t lineBytes, uint64_t l1dSizeBytes,
        bool alignedOwnerAvailable) {
    unsigned emitted = 0;
    for (const auto &[key, s] : striped) {
        if (s.stripedWriters.empty())
            continue;  // no thread-indexed write observed: not striping
        StripeVerdict v = gradeStripedArray(s, roles, lineBytes);
        if (v.mitigation == StripeMitigation::FullyPadded)
            continue;
        if (v.slotsPerLine < 2)
            continue;
        // Writers all on the main thread: the slots are packed, but no two
        // cores ever write them. The role join was previously consulted only
        // in the direction that raises severity, so this shape reported High
        // on the strength of its subscript alone.
        if (v.mainThreadOnly)
            continue;
        applyStripeROI(v, s, lineBytes, l1dSizeBytes, alignedOwnerAvailable);

        Diagnostic d;
        d.ruleID = "FL003";
        d.title = "Per-Thread Array False Sharing";
        // confidence answers "is the hazard real", severity answers "is it
        // worth acting on". a thread-identity subscript is itself evidence
        // the writer runs on several threads; the role join only confirms
        // it, and event-loop dispatch hides that path.
        if (v.multiRole) {
            d.confidence = 0.88;
            d.evidenceTier = EvidenceTier::Proven;
        } else if (s.tlsIndexed || v.writerCount >= 2 || s.elementIsAtomic) {
            d.confidence = s.tlsIndexed ? 0.78 : 0.72;
            d.evidenceTier = EvidenceTier::Likely;
        } else {
            d.confidence = 0.55;
            d.evidenceTier = EvidenceTier::Likely;
        }

        // a hazard whose only available fix costs more than it saves is
        // not an action item, however certain the mechanism is.
        if (v.fixShape == StripeFixShape::None) {
            d.severity = Severity::Informational;
        } else if (v.frequency == WriteFrequencyTier::Hot) {
            d.severity = v.multiRole ? Severity::Critical : Severity::High;
        } else if (v.frequency == WriteFrequencyTier::Unknown) {
            // unestablished is not low: demoting here would be the same
            // error as reading an unprovable flag as proven-absent.
            d.severity = v.multiRole ? Severity::Critical
                       : (s.tlsIndexed || v.writerCount >= 2 ||
                          s.elementIsAtomic) ? Severity::High
                                             : Severity::Medium;
            d.escalations.push_back(
                "write frequency unestablished: no hot-path signal reaches "
                "these writers, supply hot_function_patterns or "
                "--perf-profile to grade the fix against real call rate");
        } else {
            d.severity = Severity::Medium;
        }
        // aligned base + padded index origin is deliberate line-aware
        // layout: cap like the other mitigation-respect contracts. the
        // residual (slots 1.. still pack) is stated, not escalated.
        if (v.mitigation == StripeMitigation::HeadPadded &&
            d.severity > Severity::Medium)
            d.severity = Severity::Medium;
        // Demoted, not dropped: a worker that stamps itself into the object
        // first makes the owner id its own, undecidable here. Where the role
        // join already settled it, the grade stands.
        if (v.ownerIndexed && !v.multiRole) {
            if (d.severity > Severity::Medium)
                d.severity = Severity::Medium;
            d.confidence = std::max(d.confidence - 0.15, 0.35);
            d.escalations.push_back(
                "every thread-identity subscript reaches this array through a "
                "field of an object the caller handed in, which names the "
                "object's owner rather than the writing thread: one thread "
                "can drive every slot, so striping is not established");
        }

        d.location.file = s.file;
        d.location.line = s.line;
        d.location.column = 1;

        std::ostringstream hw;
        hw << (s.isFileStatic ? "Static array '" : "Array '")
           << s.displayName << "' packs " << v.slotsPerLine
           << " slots per " << lineBytes << "B line across "
           << v.contendedLines << " line(s) (" << s.elemCount
           << " x " << s.elemSizeBytes << "B). Slots are written under a "
           << "thread-identity index, so distinct cores update distinct "
           << "slots on the same line: every write takes the line in "
           << "Modified state and invalidates it in the other core.";
        if (!s.elementIsAtomic)
            hw << " Elements are non-atomic, striping makes each slot "
                  "single-writer, so this is coherence traffic without a "
                  "data race.";
        d.hardwareReasoning = hw.str();

        d.structuralEvidence = {
            {"symbol", s.displayName},
            {"elem_size", std::to_string(s.elemSizeBytes)},
            {"elem_count", std::to_string(s.elemCount)},
            {"slots_per_line", std::to_string(v.slotsPerLine)},
            {"contended_lines", std::to_string(v.contendedLines)},
            {"striped_writers", std::to_string(v.writerCount)},
            {"tls_indexed", s.tlsIndexed ? "yes" : "no"},
            {"index_identity", v.ownerIndexed ? "owner" : "writer"},
            {"atomic_elem", s.elementIsAtomic ? "yes" : "no"},
            {"scope", s.isFileStatic ? "file-static" : "member"},
            {"write_frequency", writeFrequencyName(v.frequency)},
            {"fix_shape", stripeFixName(v.fixShape)},
            {"footprint_current", std::to_string(v.currentFootprint)},
            {"footprint_padded", std::to_string(v.paddedFootprint)},
            {"l1d_cost_pct",
             std::to_string(static_cast<int>(v.l1dCostFraction * 100))},
        };
        if (!s.typeName.empty())
            d.structuralEvidence["type_name"] = s.typeName;

        for (const auto &w : s.stripedWriters)
            d.escalations.push_back("thread-indexed write from '" + w + "'");
        for (const auto &g : s.aggregators)
            d.escalations.push_back("loop-swept aggregation in '" + g +
                                    "' (read side, not a striped write)");
        if (v.mitigation == StripeMitigation::HeadPadded)
            d.escalations.push_back(
                "base is line-aligned with a padded index origin: slot 0 is "
                "isolated, slots 1.. still share lines");

        // Striping guarantees single-writer-per-slot, so atomicity is not
        // what establishes contention here, index provenance and distinct
        // writers are. The write-frequency tier is the other precondition:
        // unestablished it must not carry the top grade, which is the same
        // discipline FL040 and FL011 now follow.
        d.mechanismClaims = {
            {"several thread slots share one cache line",
             "an element stride narrower than the line, unpadded", true,
             Severity::Medium},
            {"per-write RFO transfer between the owning cores",
             "distinct thread-indexed writers reaching separate slots",
             v.writerCount >= 2 || s.tlsIndexed,
             v.multiRole ? Severity::Critical : Severity::High},
            {"the traffic is sustained at hot-path rates",
             "writes established on a hot path rather than assumed",
             v.frequency == WriteFrequencyTier::Hot, Severity::Critical},
        };

        {
            std::ostringstream mit;
            switch (v.fixShape) {
            case StripeFixShape::FullPad:
                mit << "Give each slot its own line (alignas(" << lineBytes
                    << ") element wrapper): " << v.currentFootprint << "B -> "
                    << v.paddedFootprint << "B, "
                    << static_cast<int>(v.l1dCostFraction * 100)
                    << "% of L1D. " << v.fixRationale << ".";
                break;
            case StripeFixShape::RelocateToOwner:
                mit << "Move the slot into the existing line-aligned "
                       "per-thread structure rather than padding this array: "
                    << v.fixRationale << ". Full padding would cost "
                    << static_cast<int>(v.l1dCostFraction * 100)
                    << "% of L1D for no additional isolation.";
                break;
            case StripeFixShape::HeadPad:
                mit << "Align the base and start indexing at a padded origin "
                       "to isolate the hottest slot: " << v.fixRationale
                    << ". Full padding would cost "
                    << static_cast<int>(v.l1dCostFraction * 100)
                    << "% of L1D.";
                break;
            case StripeFixShape::None:
                mit << "No worthwhile fix: " << v.fixRationale
                    << " (full padding " << v.currentFootprint << "B -> "
                    << v.paddedFootprint << "B = "
                    << static_cast<int>(v.l1dCostFraction * 100)
                    << "% of L1D). Re-evaluate if this write moves onto a "
                       "per-command path.";
                break;
            }
            d.mitigation = mit.str();
        }
        diagnostics.push_back(std::move(d));
        ++emitted;
    }
    return emitted;
}

// Findings from the compiler's own remark stream. Hot-filtered because one
// mid-sized C file emits ~12.8k records.
static unsigned emitOptRemarkFindings(
        std::vector<Diagnostic> &out,
        const std::vector<OptRemark> &remarks,
        const std::map<std::string, HotnessSource> &globalHot,
        const std::vector<std::string> &hotPatterns) {
    // Keyed on the site too: overloads share a qualified name on both sides
    // of the join, so merging them drops one. Capped per function because a
    // single function can carry 25 of these.
    constexpr unsigned kMaxSitesPerFunction = 3;
    std::set<std::tuple<std::string, std::string, std::string, unsigned>> seen;
    std::map<std::pair<std::string, std::string>, unsigned> total, shown;
    for (const auto &r : remarks)
        ++total[{r.function, r.name}];
    unsigned emitted = 0;

    for (const auto &r : remarks) {
        // Config patterns cover hot paths the call graph cannot reach,
        // which is what function-pointer dispatch produces.
        auto hot = globalHot.find(r.function);
        HotnessSource src = HotnessSource::None;
        if (hot != globalHot.end()) {
            src = hot->second;
        } else {
            for (const auto &p : hotPatterns)
                if (fnmatch(p.c_str(), r.function.c_str(), 0) == 0) {
                    src = HotnessSource::Declared;
                    break;
                }
            if (src == HotnessSource::None)
                continue;
        }
        if (!seen.emplace(r.function, r.name, r.file, r.line).second)
            continue;
        unsigned &n = shown[{r.function, r.name}];
        if (n >= kMaxSitesPerFunction)
            continue;
        ++n;

        Diagnostic d;
        d.ruleID = "C002";
        d.title = "Loop-Invariant Load Not Hoisted";
        d.severity = Severity::Medium;
        d.confidence = 0.80;
        d.evidenceTier = EvidenceTier::Likely;
        d.functionName = r.function;
        d.location.file = r.file;
        d.location.line = r.line;
        d.location.column = r.column;
        d.hotness = static_cast<uint8_t>(src);

        d.hardwareReasoning =
            "The address of this load does not change across the loop, but "
            "LICM could not hoist it: some store in the body may alias it, so "
            "the load repeats every iteration. The compiler is reporting that "
            "its alias analysis lost here, which is weaker than a claim that "
            "the pointers do alias.";

        d.structuralEvidence = {
            {"pass", r.pass},
            {"remark", r.name},
            {"function", r.function},
            {"hotness", hotnessSourceName(src)},
        };
        if (r.count)
            d.structuralEvidence["copies"] = std::to_string(r.count);
        if (!r.detail.empty())
            d.structuralEvidence["compiler_note"] = r.detail;
        const unsigned sites = total[{r.function, r.name}];
        d.structuralEvidence["sites_in_function"] = std::to_string(sites);
        if (sites > kMaxSitesPerFunction)
            d.escalations.push_back(
                std::to_string(sites) + " loops in this function carry the "
                "same remark; " + std::to_string(kMaxSitesPerFunction) +
                " are reported. Re-run after a fix rather than working the "
                "list, since one aliasing change can clear several");

        d.mitigation =
            "Hoist the load into a local before the loop where the invariance "
            "holds, or qualify the pointers with restrict if they genuinely "
            "do not alias. Do not add restrict to silence this without "
            "establishing that: it is a promise to the compiler, not a hint.";

        d.mechanismClaims = {
            {"a load repeated per iteration at a loop-invariant address",
             "the compiler recorded the decision in its own remark stream",
             true, Severity::Medium},
            {std::string("this code runs often enough for the cost to recur (") +
                 hotnessSourceName(src) + ", cross-TU)",
             "hotness established by profile or declaration, not inferred "
             "from shape alone",
             src >= HotnessSource::Declared,
             hotnessSupportedSeverity(src, Severity::Medium),
             /*gating=*/true},
        };
        out.push_back(std::move(d));
        ++emitted;
    }
    return emitted;
}

// FL092: unapplied in-tree mitigation. Synthesized when an FL002 with
// cross-thread writer attribution sits in a codebase that demonstrably
// applies cache-line isolation to other types. The precedent join is the
// merge-shaped evidence: the codebase itself validates both the hazard
// class and the fix idiom; this struct just never received it. Runs
// post-dedup (one compound per surviving component); never outranks the
// component's mitigation-adjusted severity.
static unsigned synthesizeUnappliedMitigation(
        std::vector<Diagnostic> &diagnostics,
        const EscapeSummary &globalEscape) {
    std::vector<std::string> mitigated;
    for (const auto &[name, sig] : globalEscape)
        if (sig.hasDeliberateLayout)
            mitigated.push_back(name);
    if (mitigated.empty())
        return 0;
    std::sort(mitigated.begin(), mitigated.end());

    std::vector<Diagnostic> compounds;
    std::set<std::string> emittedTypes;
    for (const auto &d : diagnostics) {
        // FL002 joins at pair granularity; FL090 at struct granularity,
        // large structs put the disjoint pair beyond FL002's pair-evidence
        // cap, and FL090's uncapped type-level attribution catches those.
        // One compound per type: FL002 wins the tie by sort order.
        if (d.suppressed || (d.ruleID != "FL002" && d.ruleID != "FL090"))
            continue;
        auto tit = d.structuralEvidence.find("type_name");
        if (tit == d.structuralEvidence.end() || tit->second.empty())
            continue;
        if (emittedTypes.count(tit->second))
            continue;
        auto git = globalEscape.find(tit->second);
        if (git != globalEscape.end() && git->second.hasDeliberateLayout)
            continue; // already carries the idiom; FL002's demotion applies
        bool attributed = false;
        for (const auto &e : d.escalations)
            if (e.rfind("cross-TU thread-role attribution", 0) == 0) {
                attributed = true;
                break;
            }
        if (!attributed)
            continue;

        Diagnostic c;
        c.ruleID = "FL092";
        c.title = "Unapplied In-Tree Mitigation";
        c.severity = d.severity;
        c.confidence = d.confidence;
        c.evidenceTier = d.evidenceTier;
        c.location = d.location;
        c.functionName = d.functionName;
        c.hardwareReasoning =
            "Struct '" + tit->second + "' has false sharing with "
            "cross-thread-attributed writers while this codebase already "
            "isolates " + std::to_string(mitigated.size()) +
            " other type(s) on dedicated cache lines (e.g. '" +
            mitigated.front() + "'). The MESI invalidation mechanism and "
            "its fix idiom are both established in-tree; this struct never "
            "received the treatment.";
        c.structuralEvidence = {
            {"component", d.ruleID},
            {"type_name", tit->second},
            {"mitigated_exemplar", mitigated.front()},
            {"mitigated_type_count", std::to_string(mitigated.size())},
        };
        c.mitigation =
            "Apply the codebase's existing isolation idiom (as on '" +
            mitigated.front() + "') to the disjoint-writer fields of '" +
            tit->second + "'.";
        // FL092 asserts nothing about hardware on its own: it inherits the
        // component's mechanism and adds that this codebase already knows
        // the fix. So it can never outrank the finding it was built from.
        c.mechanismClaims = {
            {"the component hazard's own mechanism",
             "the attributed finding established it", true,
             d.severitySupportedByClaims()},
            {"the fix idiom is established in-tree and was not applied here",
             "another type in this codebase is deliberately line-isolated",
             !mitigated.empty(), d.severity},
        };
        c.escalations = {
            "precedent join: " + std::to_string(mitigated.size()) +
            " deliberately line-isolated type(s) in this codebase"};
        emittedTypes.insert(tit->second);
        compounds.push_back(std::move(c));
    }

    for (auto &c : compounds)
        diagnostics.push_back(std::move(c));
    return static_cast<unsigned>(compounds.size());
}

// Cross-TU escape suppression using aggregated EscapeSummary.
// For each diagnostic with thread_escape evidence, look up the type in the
// Two cores can only fight over a cache line if they reach the same object.
// A rule cannot decide that: hasGlobalInstance is a per-TU fact, the record
// lives in a header and its global lives in one .c, so at rule time the
// answer is false almost everywhere it matters.
static unsigned applySharingRouteVerdict(std::vector<Diagnostic> &diagnostics,
                                         const EscapeSummary &summary) {
    unsigned capped = 0;
    for (auto &d : diagnostics) {
        if (d.suppressed) continue;
        // Rules whose whole claim is cross-core contention on one object.
        if (d.ruleID != "FL002" && d.ruleID != "FL041") continue;
        auto it = d.structuralEvidence.find("type_name");
        if (it == d.structuralEvidence.end() || it->second.empty()) continue;
        // ';'-separated: a compound may name several types. Any one of them
        // being shared leaves the finding alone.
        bool anyShared = false, anyKnown = false;
        const std::string &ts = it->second;
        for (size_t start = 0; start < ts.size();) {
            size_t end = ts.find(';', start);
            std::string t = ts.substr(start, end == std::string::npos
                                                 ? std::string::npos
                                                 : end - start);
            if (!t.empty()) {
                auto sit = summary.find(t);
                if (sit != summary.end()) {
                    anyKnown = true;
                    if (sit->second.hasSharingRoute()) anyShared = true;
                }
            }
            if (end == std::string::npos) break;
            start = end + 1;
        }
        // Absent from the summary means unanalyzed, not disproven.
        if (!anyKnown || anyShared) continue;

        d.mechanismClaims.push_back(
            {"two threads reach the same instance",
             "some TU shows a shared instance and a thread-borne writer",
             false, Severity::Medium, /*gating=*/true});
        d.escalations.push_back(
            "no TU showed a thread reaching a shared instance of this type: "
            "co-location is real, cross-core contention is not established");
        ++capped;
    }
    return capped;
}

// global summary. If no TU provided structural or publication escape evidence,
// suppress.
static unsigned applyCrossTUEscapeSuppression(
        std::vector<Diagnostic> &diagnostics,
        const EscapeSummary &globalEscape,
        unsigned totalTUs) {
    if (totalTUs <= 1)
        return 0;

    unsigned suppressed = 0;
    for (auto &d : diagnostics) {
        if (d.suppressed) continue;

        auto eit = d.structuralEvidence.find("thread_escape");
        if (eit == d.structuralEvidence.end()) continue;
        if (eit->second != "true" && eit->second != "yes") continue;

        // Proven-tier findings are never suppressed.
        if (d.evidenceTier == EvidenceTier::Proven)
            continue;

        auto tit = d.structuralEvidence.find("type_name");
        if (tit == d.structuralEvidence.end())
            continue;

        auto git = globalEscape.find(tit->second);
        if (git != globalEscape.end() && git->second.hasAnyEscape())
            continue; // Global evidence confirms escape.

        d.suppressed = true;
        d.escalations.push_back(
            "cross-TU suppression: no escape evidence across " +
            std::to_string(totalTUs) + " TUs");
        ++suppressed;
    }
    return suppressed;
}

// severity desc, then file/line/column/ruleID: the output-order contract.
static bool outputOrder(const Diagnostic &a, const Diagnostic &b) {
    if (a.severity != b.severity)
        return static_cast<uint8_t>(a.severity) >
               static_cast<uint8_t>(b.severity);
    if (a.location.file != b.location.file)
        return a.location.file < b.location.file;
    if (a.location.line != b.location.line)
        return a.location.line < b.location.line;
    if (a.location.column != b.location.column)
        return a.location.column < b.location.column;
    if (a.ruleID != b.ruleID)
        return a.ruleID < b.ruleID;
    return diagnosticContentLess(a, b);
}

static void filterAndSort(const FilterOptions &filter,
                           std::vector<Diagnostic> &diagnostics) {
    diagnostics.erase(
        std::remove_if(diagnostics.begin(), diagnostics.end(),
                       [&](const Diagnostic &d) {
                           if (d.suppressed)
                               return true;
                           if (static_cast<uint8_t>(d.severity) <
                               static_cast<uint8_t>(filter.minSeverity))
                               return true;
                           if (static_cast<uint8_t>(d.evidenceTier) >
                               static_cast<uint8_t>(filter.minEvidenceTier))
                               return true;
                           return false;
                       }),
        diagnostics.end());

    std::sort(diagnostics.begin(), diagnostics.end(), outputOrder);
}

// --- Entry points ---

ScanResult ScanPipeline::execute(const ScanRequest &request) {
    std::string dbPath = request.compileDBPath;

    // Autodiscover compile_commands.json if not explicitly provided.
    if (dbPath.empty() && !request.workingDirectory.empty()) {
        report("compile_db", "Searching for compile_commands.json");
        if (request.trustBuildSystem)
            dbPath = CompileDBResolver::discoverOrGenerate(request.workingDirectory);
        else
            dbPath = CompileDBResolver::discover(request.workingDirectory);
        if (dbPath.empty()) {
            llvm::errs() << "lshaz: error: no compile_commands.json found in "
                         << request.workingDirectory;
            if (!request.trustBuildSystem)
                llvm::errs() << "\n  Build system execution disabled for "
                                "untrusted source. Use --trust-build-system "
                                "to allow cmake/meson/bear.";
            else
                llvm::errs() << " (also tried cmake generation)";
            llvm::errs() << "\n  searched: ";
            for (const auto &p : CompileDBResolver::candidatePaths(
                     request.workingDirectory))
                llvm::errs() << "\n    " << p;
            llvm::errs() << "\n";
            ScanResult result;
            result.status = ScanStatus::ToolError;
            return result;
        }
        report("compile_db", "Found " + dbPath);
    }

    if (dbPath.empty()) {
        llvm::errs() << "lshaz: error: no compile database path specified "
                     << "and no working directory for autodiscovery\n";
        ScanResult result;
        result.status = ScanStatus::ToolError;
        return result;
    }

    report("compile_db", "Loading " + dbPath);
    std::string dbError;
    auto jsonDB = clang::tooling::JSONCompilationDatabase::loadFromFile(
        dbPath, dbError,
        clang::tooling::JSONCommandLineSyntax::AutoDetect);
    if (!jsonDB) {
        llvm::errs() << "lshaz: error: " << dbError << "\n";
        ScanResult result;
        result.status = ScanStatus::ToolError;
        return result;
    }

    // Wrap in AbsolutePathCompilationDatabase to resolve all relative
    // paths at load time. This eliminates ClangTool's process-global
    // chdir() calls, which race between threads in parallel scans.
    AbsolutePathCompilationDatabase compDB(std::move(jsonDB));

    std::vector<std::string> sources = request.sourceFiles;
    if (sources.empty()) {
        sources = compDB.getAllFiles();
        std::sort(sources.begin(), sources.end());
    }

    unsigned vendored = 0;
    sources = filterSources(sources, request.filter, vendored);
    auto r = run(request, compDB, sources);
    r.vendoredTUsSkipped = vendored;
    return r;
}

ScanResult ScanPipeline::executeWithDB(
        const ScanRequest &request,
        const clang::tooling::CompilationDatabase &compDB,
        const std::vector<std::string> &sources) {
    unsigned vendored = 0;
    auto filtered = filterSources(sources, request.filter, vendored);
    auto r = run(request, compDB, filtered);
    r.vendoredTUsSkipped = vendored;
    return r;
}

// Compute the Clang resource directory for the LLVM this binary was linked
// against.  ClangTool normally derives it from the compiler path listed in
// compile_commands.json, which breaks when the project was built with gcc.
static std::string detectResourceDir() {
#ifdef LLVM_LIBRARY_DIR
    // LLVM_LIBRARY_DIR is injected via CMake (e.g. /opt/llvm/lib).
    // CLANG_VERSION_MAJOR comes from <clang/Basic/Version.inc>.
    // The resource dir lives at <lib-dir>/clang/<major-version>.
    std::string candidate = std::string(LLVM_LIBRARY_DIR) +
        "/clang/" + std::to_string(CLANG_VERSION_MAJOR);
    if (llvm::sys::fs::is_directory(candidate))
        return candidate;
#endif
    return {};
}

// Returns true if the compiler path looks like clang/clang++.
static bool compilerIsClang(const std::string &compiler) {
    llvm::StringRef stem = llvm::sys::path::stem(compiler);
    return stem.starts_with("clang");
}

// Only inject -resource-dir when the compile_commands.json compiler is not
// clang/clang++ (which already knows its own resource dir).
static void addResourceDirAdjuster(
        clang::tooling::ClangTool &tool,
        const clang::tooling::CompilationDatabase &compDB,
        const std::string &sourceFile) {
    static const std::string resDir = detectResourceDir();
    if (resDir.empty())
        return;
    auto cmds = compDB.getCompileCommands(sourceFile);
    if (!cmds.empty() && compilerIsClang(cmds.front().CommandLine.front()))
        return;
    static const std::string arg = "-resource-dir=" + resDir;
    tool.appendArgumentsAdjuster(
        clang::tooling::getInsertArgumentAdjuster(
            arg.c_str(),
            clang::tooling::ArgumentInsertPosition::BEGIN));
}

// --- Shared pipeline implementation ---

ScanResult ScanPipeline::run(
        const ScanRequest &request,
        const clang::tooling::CompilationDatabase &compDB,
        const std::vector<std::string> &sources) {
    ScanResult result;
    std::vector<OptRemark> optRemarks;
    unsigned remarkFilesFailed = 0;
    std::vector<FailedTU> failedTUsDetailed; // For header fingerprint detection

    result.metadata.toolVersion = kToolVersion;
    result.metadata.irOptLevel = request.ir.optLevel;
    result.metadata.irEnabled = request.ir.enabled;
    result.metadata.timestampEpochSec = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    result.metadata.sourceFiles = sources;
    result.totalTUsAnalyzed = static_cast<unsigned>(sources.size());

    report("analysis", std::to_string(sources.size()) + " translation unit(s)");

    auto profileHotFuncs = loadProfileHotFunctions(request);

    // AST analysis. Parallel when multiple TUs and jobs > 1.
    unsigned jobs = request.analysisJobs;
    if (jobs == 0)
        jobs = std::max(1u, std::thread::hardware_concurrency());
    if (jobs > static_cast<unsigned>(sources.size()))
        jobs = static_cast<unsigned>(sources.size());

    const unsigned shardMemoryLimitMB =
        resolveShardMemoryLimitMB(request.memoryLimitMB, jobs);
    if (shardMemoryLimitMB == 0) {
        llvm::errs() << "lshaz: WARNING cannot read available memory, shards "
                        "run uncapped; a heavy TU can OOM the host\n";
    } else if (jobs > 1) {
        report("memory", std::to_string(shardMemoryLimitMB) +
                         " MiB cap per shard");
    }

    int toolRet = 0;

    llvm::CrashRecoveryContext::Enable();

    unsigned completedTUs = 0;
    const unsigned totalTUs = static_cast<unsigned>(sources.size());

    if (jobs <= 1 || sources.size() <= 1) {
        // Sequential path: per-TU crash isolation.
        for (const auto &src : sources) {
            std::vector<std::string> singleTU = {src};
            std::vector<Diagnostic> tuDiags;
            LshazActionFactory factory(
                request.config, tuDiags, profileHotFuncs);

            llvm::CrashRecoveryContext CRC;
            bool crashed = !CRC.RunSafely([&]() {
                clang::tooling::ClangTool tool(compDB, singleTU);
                addResourceDirAdjuster(tool, compDB, src);
                int ret = tool.run(&factory);
                if (ret != 0) toolRet = ret;
            });

            if (crashed) {
                FailedTU ftu;
                ftu.file = src;
                ftu.error = "process crash during analysis";
                failedTUsDetailed.push_back(ftu);
                llvm::errs() << "lshaz: [crash] " << src
                             << " (recovered, continuing)\n";
            } else {
                auto &ff = factory.failedTUs();
                failedTUsDetailed.insert(failedTUsDetailed.end(),
                    ff.begin(), ff.end());
            }
            result.diagnostics.insert(result.diagnostics.end(),
                std::make_move_iterator(tuDiags.begin()),
                std::make_move_iterator(tuDiags.end()));
            mergeEscapeSummaries(result.escapeSummary, factory.escapeSummary());
            result.threadRoleFacts.merge(factory.threadRoles());
            mergeStripedArrays(result.stripedArrays, factory.stripedArrays());
            result.coverage.merge(factory.coverage());
            ++completedTUs;
            report("progress", std::to_string(completedTUs) + "/" +
                   std::to_string(totalTUs));
        }
    } else {
        // Parallel path: fork-based process isolation.
        //
        // ClangTool uses global mutable state (llvm::cl option tables,
        // CrashRecoveryContext signal handlers, FileManager stat caches)
        // that is not thread-safe. We fork() per shard so each child
        // gets its own address space via COW. Children serialize results
        // to temp files; parent reads them back after waitpid().
        std::vector<std::vector<std::string>> shards(jobs);
        for (size_t i = 0; i < sources.size(); ++i)
            shards[i % jobs].push_back(sources[i]);

        struct ChildSlot {
            pid_t pid = -1;
            std::string ipcPath;
            unsigned shardIdx = 0;
        };
        std::vector<ChildSlot> children;
        children.reserve(jobs);

        for (unsigned j = 0; j < jobs; ++j) {
            if (shards[j].empty()) continue;

            // Unique temp file for this shard.
            llvm::SmallString<128> tmpDir;
            llvm::sys::path::system_temp_directory(true, tmpDir);
            llvm::SmallString<128> ipcPath(tmpDir);
            llvm::sys::path::append(ipcPath,
                "lshaz-shard-" + std::to_string(j) + "-" +
                std::to_string(getpid()) + ".json");

            pid_t pid = fork();
            if (pid < 0) {
                // The shard's TUs are unanalyzed; they must be accounted as
                // failed or the scan reports coverage it never had.
                llvm::errs() << "lshaz: fork() failed for shard " << j
                             << ": " << strerror(errno) << "\n";
                for (const auto &src : shards[j]) {
                    FailedTU ftu;
                    ftu.file = src;
                    ftu.error = std::string("fork() failed: ") + strerror(errno);
                    failedTUsDetailed.push_back(ftu);
                }
                toolRet = 1;
                continue;
            }

            if (pid == 0) {
                // Deterministic stand-in for the OOM killer, which cannot be
                // summoned on demand. Announced on stderr so an injected run
                // is never mistaken for a real one. "<shard>" kills before any
                // TU; "<shard>:<n>" kills after n, exercising partial recovery.
                unsigned faultShard = ~0u, faultAfterTUs = 0;
                if (const char *f = ::getenv("LSHAZ_FAULT_KILL_SHARD")) {
                    faultShard = static_cast<unsigned>(atoi(f));
                    if (const char *colon = ::strchr(f, ':'))
                        faultAfterTUs = static_cast<unsigned>(atoi(colon + 1));
                }
                auto maybeFault = [&](unsigned done) {
                    if (faultShard != j || done != faultAfterTUs) return;
                    llvm::errs() << "lshaz: FAULT INJECTION active, killing "
                                    "shard " << j << " after " << done
                                 << " TU(s)\n";
                    ::raise(SIGKILL);
                };
                maybeFault(0);

                if (shardMemoryLimitMB != 0) {
                    struct rlimit rl;
                    rl.rlim_cur = static_cast<rlim_t>(shardMemoryLimitMB)
                                  << 20;
                    rl.rlim_max = rl.rlim_cur;
                    if (::setrlimit(RLIMIT_AS, &rl) != 0) {
                        llvm::errs() << "lshaz: shard " << j
                                     << " could not set its memory cap: "
                                     << strerror(errno) << "\n";
                        _exit(kShardIPCWriteFailed);
                    }
                    // Turn allocation failure into a named exit rather than a
                    // bad_alloc unwinding through Clang into an opaque crash.
                    llvm::install_bad_alloc_error_handler(
                        [](void *, const char *reason, bool) {
                            llvm::errs() << "lshaz: shard exhausted its memory "
                                            "cap: " << reason << "\n";
                            _exit(kShardMemoryExhausted);
                        });
                    std::set_new_handler([]() {
                        llvm::errs() << "lshaz: shard exhausted its memory "
                                        "cap in operator new\n";
                        _exit(kShardMemoryExhausted);
                    });
                }
                llvm::CrashRecoveryContext::Enable();

                // One record per TU, flushed as it completes. Writing only at
                // the end meant a single fatal TU discarded every TU the shard
                // had already finished, so one crash could lose an entire shard.
                // Records are newline-delimited so a torn tail is discardable
                // and the parent can name exactly which TUs went unreached.
                std::error_code ec;
                llvm::raw_fd_ostream out(std::string(ipcPath), ec);
                if (ec) {
                    // Exiting 0 here would hand the parent an absent IPC file
                    // indistinguishable from a shard that legitimately found
                    // nothing. Distinct code so the parent can say why.
                    llvm::errs() << "lshaz: shard " << j
                                 << " could not write IPC to " << ipcPath
                                 << ": " << ec.message() << "\n";
                    _exit(kShardIPCWriteFailed);
                }

                int childRet = 0;
                unsigned tusDone = 0;
                for (const auto &src : shards[j]) {
                    std::vector<std::string> singleTU = {src};
                    std::vector<Diagnostic> tuDiags;
                    std::vector<FailedTU> tuFailed;
                    LshazActionFactory factory(
                        request.config, tuDiags, profileHotFuncs);

                    int tuRet = 0;
                    llvm::CrashRecoveryContext CRC;
                    bool ok = CRC.RunSafely([&]() {
                        clang::tooling::ClangTool tool(compDB, singleTU);
                        addResourceDirAdjuster(tool, compDB, src);
                        int ret = tool.run(&factory);
                        if (ret != 0) tuRet = ret;
                    });
                    if (!ok) {
                        FailedTU ftu;
                        ftu.file = src;
                        ftu.error = "process crash during analysis";
                        tuFailed.push_back(ftu);
                    } else {
                        auto &ff = factory.failedTUs();
                        tuFailed.insert(tuFailed.end(), ff.begin(), ff.end());
                    }
                    if (tuRet != 0) childRet = tuRet;

                    out << serializeShardResult(
                               tuRet, tuFailed, tuDiags,
                               factory.escapeSummary(), factory.threadRoles(),
                               factory.stripedArrays(), factory.coverage(), src)
                        << "\n";
                    out.flush();
                    if (out.has_error()) {
                        llvm::errs() << "lshaz: shard " << j
                                     << " failed writing IPC to " << ipcPath
                                     << ": " << out.error().message() << "\n";
                        _exit(kShardIPCWriteFailed);
                    }
                    maybeFault(++tusDone);
                }

                out.close();
                _exit(childRet != 0 ? 1 : 0);
            }

            // --- Parent ---
            children.push_back({pid, std::string(ipcPath), j});
        }

        // Reap all children. A shard is accounted for in exactly one way:
        // its IPC parsed, or every TU it owned is marked failed. Any path
        // that does neither converts a lost shard into silently missing
        // coverage that reads identically to a clean scan.
        for (auto &child : children) {
            int status = 0;
            std::string lostReason;
            if (waitpid(child.pid, &status, 0) < 0) {
                lostReason = std::string("waitpid() failed: ") + strerror(errno);
            } else if (WIFSIGNALED(status)) {
                // Signalled children are unrecoverable even if a partial IPC
                // file exists: SIGKILL (the OOM killer's signal) can land
                // mid-write, leaving a prefix that may still parse.
                lostReason = "killed by signal " +
                             std::to_string(WTERMSIG(status));
            } else if (WIFEXITED(status) &&
                       WEXITSTATUS(status) == kShardIPCWriteFailed) {
                lostReason = "could not write its IPC file";
            } else if (WIFEXITED(status) &&
                       WEXITSTATUS(status) == kShardMemoryExhausted) {
                lostReason = "exceeded its " +
                             std::to_string(shardMemoryLimitMB) +
                             " MiB memory cap (raise --memory-limit-mb or "
                             "lower --jobs)";
            }

            // Records land one per line as each TU finishes, so a shard that
            // died still hands back everything it completed. Whatever is
            // missing is named individually rather than condemning the shard.
            std::unordered_set<std::string> covered;
            {
                auto buf = llvm::MemoryBuffer::getFile(child.ipcPath);
                if (!buf) {
                    if (lostReason.empty())
                        lostReason = "left no IPC file: " +
                                     buf.getError().message();
                } else {
                    llvm::StringRef body = (*buf)->getBuffer();
                    size_t nl;
                    while ((nl = body.find('\n')) != llvm::StringRef::npos) {
                        llvm::StringRef line = body.take_front(nl);
                        body = body.drop_front(nl + 1);
                        if (line.empty()) continue;
                        ShardIPC rec;
                        if (!deserializeShardResult(line.str(), rec)) {
                            if (lostReason.empty())
                                lostReason = "wrote unparseable IPC";
                            continue;
                        }
                        if (!rec.src.empty()) covered.insert(rec.src);
                        if (rec.exitCode != 0) toolRet = rec.exitCode;
                        result.diagnostics.insert(result.diagnostics.end(),
                            std::make_move_iterator(rec.diagnostics.begin()),
                            std::make_move_iterator(rec.diagnostics.end()));
                        failedTUsDetailed.insert(failedTUsDetailed.end(),
                            rec.failedTUs.begin(), rec.failedTUs.end());
                        mergeEscapeSummaries(result.escapeSummary,
                                             rec.escapeSummary);
                        result.threadRoleFacts.merge(rec.threadRoles);
                        mergeStripedArrays(result.stripedArrays, rec.striped);
                        result.coverage.merge(rec.coverage);
                    }
                    // A tail without its newline is a torn write, not a record.
                    if (!body.empty() && lostReason.empty())
                        lostReason = "IPC ends mid-record";
                }
            }

            std::vector<std::string> unreached;
            for (const auto &src : shards[child.shardIdx])
                if (!covered.count(src)) unreached.push_back(src);

            if (!unreached.empty()) {
                if (lostReason.empty())
                    lostReason = "exited without reporting every TU";
                llvm::errs() << "lshaz: shard " << child.shardIdx << ": "
                             << lostReason << "; " << unreached.size() << " of "
                             << shards[child.shardIdx].size()
                             << " TU(s) unanalyzed ("
                             << covered.size() << " recovered)\n";
                for (const auto &src : unreached) {
                    FailedTU ftu;
                    ftu.file = src;
                    ftu.error = "shard lost: " + lostReason;
                    failedTUsDetailed.push_back(ftu);
                }
                toolRet = 1;
            } else if (!lostReason.empty()) {
                // Every TU reported, but the shard still ended badly. Say so.
                llvm::errs() << "lshaz: shard " << child.shardIdx << ": "
                             << lostReason
                             << "; all TU(s) recovered nonetheless\n";
                toolRet = 1;
            }
            llvm::sys::fs::remove(child.ipcPath);
            completedTUs += static_cast<unsigned>(shards[child.shardIdx].size());
            report("progress", std::to_string(completedTUs) + "/" +
                   std::to_string(totalTUs));
        }
    }

    llvm::CrashRecoveryContext::Disable();

    // Canonicalize diagnostic order before any order-dependent pass.
    // In parallel mode, diagnostics arrive in non-deterministic order
    // (thread scheduling). Sorting by a stable key ensures cross-TU
    // suppression, dedup, and precision budget produce identical output
    // regardless of thread count or execution order.
    std::sort(result.diagnostics.begin(), result.diagnostics.end(),
              [](const Diagnostic &a, const Diagnostic &b) {
                  if (a.ruleID != b.ruleID) return a.ruleID < b.ruleID;
                  if (a.location.file != b.location.file)
                      return a.location.file < b.location.file;
                  if (a.location.line != b.location.line)
                      return a.location.line < b.location.line;
                  if (a.location.column != b.location.column)
                      return a.location.column < b.location.column;
                  if (a.functionName != b.functionName)
                      return a.functionName < b.functionName;
                  return diagnosticContentLess(a, b);
              });

    // FL040 reduce phase: aggregate per-TU write counts into a global verdict.
    // Shards emitted raw facts (tu_write_count, has_init). We sum writes across
    // all TUs for each (var, type) and apply the write-once threshold globally.
    // This must run before dedup so all duplicate instances get reclassified
    // consistently. Dedup then collapses them to a single canonical instance.
    {
        // Accumulate: key = "var|type", value = {total_writes, any_has_init}
        struct FL040Agg {
            unsigned totalWrites = 0;
            unsigned loopWrites = 0;
            bool anyHasInit = false;
        };
        std::unordered_map<std::string, FL040Agg> fl040Agg;

        for (const auto &d : result.diagnostics) {
            if (d.ruleID != "FL040") continue;
            auto varIt = d.structuralEvidence.find("var");
            auto typeIt = d.structuralEvidence.find("type");
            if (varIt == d.structuralEvidence.end()) continue;
            std::string key = varIt->second;
            if (typeIt != d.structuralEvidence.end())
                key += "|" + typeIt->second;

            auto wcIt = d.structuralEvidence.find("tu_write_count");
            auto lwIt = d.structuralEvidence.find("tu_loop_writes");
            auto hiIt = d.structuralEvidence.find("has_init");
            unsigned wc = 0, lw = 0;
            if (wcIt != d.structuralEvidence.end())
                wc = static_cast<unsigned>(std::stoul(wcIt->second));
            if (lwIt != d.structuralEvidence.end())
                lw = static_cast<unsigned>(std::stoul(lwIt->second));
            bool hi = (hiIt != d.structuralEvidence.end()
                       && hiIt->second == "yes");

            auto &agg = fl040Agg[key];
            agg.totalWrites += wc;
            agg.loopWrites += lw;
            if (hi) agg.anyHasInit = true;
        }

        // Reclassify each FL040 diagnostic based on global verdict.
        for (auto &d : result.diagnostics) {
            if (d.ruleID != "FL040") continue;
            auto varIt = d.structuralEvidence.find("var");
            auto typeIt = d.structuralEvidence.find("type");
            if (varIt == d.structuralEvidence.end()) continue;
            std::string key = varIt->second;
            if (typeIt != d.structuralEvidence.end())
                key += "|" + typeIt->second;

            auto it = fl040Agg.find(key);
            if (it == fl040Agg.end()) continue;

            const auto &agg = it->second;
            // Same logic as EscapeAnalysis::isWriteOnceGlobal, but on
            // the global sum across all TUs instead of a single TU.
            // One site inside a loop is one *site*, not one write,
            // never write-once.
            bool writeOnce = false;
            if (agg.loopWrites == 0) {
                if (agg.anyHasInit && agg.totalWrites == 0)
                    writeOnce = true;
                else if (agg.totalWrites <= 1)
                    writeOnce = true;
            }

            // Replace per-TU metadata with global verdict.
            d.structuralEvidence.erase("tu_write_count");
            d.structuralEvidence.erase("tu_loop_writes");
            d.structuralEvidence.erase("has_init");
            d.structuralEvidence["write_once"] = writeOnce ? "yes" : "no";
            d.structuralEvidence["global_write_count"] =
                std::to_string(agg.totalWrites);
            d.structuralEvidence["global_loop_writes"] =
                std::to_string(agg.loopWrites);

            bool atomicVar =
                d.structuralEvidence.count("atomics") &&
                d.structuralEvidence.at("atomics") == "yes";

            if (writeOnce) {
                d.severity = Severity::Informational;
                d.confidence = 0.30;
                d.evidenceTier = EvidenceTier::Speculative;
                d.escalations.push_back(
                    "write-once (global): across all TUs, at most one write "
                    "site, negligible runtime contention");
            } else if (!atomicVar && agg.totalWrites <= 1) {
                // single in-loop site on a plain type: repeated writes but
                // one write path; concurrent writers would be a data race,
                // not a latency hazard.
                d.severity = Severity::Informational;
                d.confidence = 0.35;
                d.evidenceTier = EvidenceTier::Speculative;
                d.escalations.push_back(
                    "single write site (in a loop) on a non-atomic global: "
                    "one write path, no multi-writer topology");
            } else if (d.severity == Severity::Critical &&
                       agg.loopWrites == 0 && agg.totalWrites < 4) {
                // Critical means sustained RFO pressure: a write in a loop,
                // or write responsibility spread over >=4 sites. 2-3 flat
                // sites is the start/stop lifecycle signature.
                d.severity = Severity::High;
                d.escalations.push_back(
                    "atomic global with " + std::to_string(agg.totalWrites) +
                    " flat write site(s) across all TUs, none in a loop: "
                    "lifecycle-signaling pattern, not sustained contention");
            }
        }
    }

    // IR analysis pass. gating on toolRet==0 meant one broken TU anywhere
    // silently disabled refinement for the entire scan, confidence on
    // every finding shifted because of an unrelated file. refine what
    // parsed; failures are already reported per-TU.
    if (request.ir.enabled && sources.size() > failedTUsDetailed.size()) {
        report("ir", "IR emission and analysis");
        std::unordered_set<std::string> failedFiles;
        failedFiles.reserve(failedTUsDetailed.size());
        for (const auto &ftu : failedTUsDetailed)
            failedFiles.insert(ftu.file);
        runIRPass(request, compDB, sources, failedFiles,
                  result.diagnostics, result.metadata, optRemarks,
                  remarkFilesFailed);
    }

    // Cross-TU escape suppression using aggregated per-type escape summaries.
    if (sources.size() > 1) {
        report("cross_tu",
               std::to_string(result.diagnostics.size()) + " raw finding(s), " +
               std::to_string(result.escapeSummary.size()) + " type(s) in escape summary");
        unsigned crossTUSuppressed = applyCrossTUEscapeSuppression(
            result.diagnostics, result.escapeSummary, result.totalTUsAnalyzed);
        if (crossTUSuppressed > 0)
            report("cross_tu", std::to_string(crossTUSuppressed) +
                   " finding(s) suppressed (no cross-TU escape evidence)");

        unsigned unshared = applySharingRouteVerdict(result.diagnostics,
                                                     result.escapeSummary);
        if (unshared > 0)
            report("cross_tu", std::to_string(unshared) +
                   " sharing finding(s) capped (no shared instance any TU "
                   "reached)");
    }

    // Thread-role reduce: verdicts from the merged facts. Runs on the
    // parent's aggregate regardless of jobs count; children never see
    // enough of the graph to classify anything.
    result.threadRoles = computeThreadRoles(result.threadRoleFacts,
                                            request.config.threadEntryPatterns,
                                            request.config.mainFunctionPatterns);
    if (!result.threadRoles.functionRoles.empty()) {
        report("thread_roles",
               std::to_string(result.threadRoleFacts.threadEntries.size()) +
               " thread entry point(s), " +
               std::to_string(result.threadRoles.functionRoles.size()) +
               " function(s) attributed");
        unsigned roleEscalated = applyThreadRoleEscalation(
            result.diagnostics, result.threadRoleFacts, result.threadRoles);
        if (roleEscalated > 0)
            report("thread_roles", std::to_string(roleEscalated) +
                   " finding(s) escalated (disjoint writer roles)");
    }

    // Resolve cross-TU hotness candidates. The map phase could not decide
    // for any function in a TU holding no entry point, so it deferred rather
    // than answering "cold" from facts it did not have. This is the only
    // place the whole call graph exists.
    {
        const auto globalHot = inferGlobalHotness(
            result.threadRoleFacts, request.config.mainFunctionPatterns);
        std::set<std::string> withdrawable;
        for (const auto &r : RuleRegistry::instance().rules())
            if (r->withdrawnWhenNotHot())
                withdrawable.insert(std::string(r->getID()));
        unsigned resolved = 0, dropped = 0;
        for (auto &d : result.diagnostics) {
            if (d.hotness != static_cast<uint8_t>(HotnessSource::Candidate))
                continue;
            auto it = globalHot.find(d.functionName);
            if (it == globalHot.end() && !withdrawable.count(d.ruleID)) {
                d.hotness = static_cast<uint8_t>(HotnessSource::None);
                continue;
            }
            if (it == globalHot.end()) {
                // Never reached from any entry in any TU. This is the verdict
                // the map phase would have reached had it been able to see
                // the whole program.
                d.suppressed = true;
                ++dropped;
                continue;
            }
            d.hotness = static_cast<uint8_t>(it->second);
            ++resolved;
            for (auto &c : d.mechanismClaims) {
                if (!c.gating || c.effect.rfind("this code runs often", 0) != 0)
                    continue;
                c.effect = std::string("this code runs often enough for the "
                                       "cost to recur (") +
                           hotnessSourceName(it->second) + ", cross-TU)";
                c.supports = hotnessSupportedSeverity(it->second, d.severity);
            }
        }
        if (resolved || dropped)
            report("hotness", std::to_string(resolved) +
                   " cross-TU candidate(s) confirmed hot, " +
                   std::to_string(dropped) + " dropped as cold");

        unsigned fromRemarks = emitOptRemarkFindings(
            result.diagnostics, optRemarks, globalHot,
            request.config.hotFunctionPatterns);
        if (!optRemarks.empty() || remarkFilesFailed)
            report("opt_remarks", std::to_string(fromRemarks) +
                   " finding(s) from " + std::to_string(optRemarks.size()) +
                   " reportable compiler remark(s)" +
                   (remarkFilesFailed
                        ? "; " + std::to_string(remarkFilesFailed) +
                              " remark file(s) ended truncated, coverage "
                              "for those TUs is partial"
                        : std::string()));
    }

    // Monomorphic virtual calls. Nothing overrides the callee anywhere in the
    // program, so the receiver cannot vary and the misprediction term is
    // absent: ~1ns for the lost inline instead of ~9ns.
    {
        unsigned mono = 0;
        for (auto &d : result.diagnostics) {
            if (d.suppressed || (d.ruleID != "FL030" && d.ruleID != "FL031"))
                continue;
            auto ev = d.structuralEvidence.find("virtual_call");
            if (ev == d.structuralEvidence.end())
                continue;
            if (result.threadRoleFacts.overriddenVirtuals.count(ev->second))
                continue;
            ++mono;
            d.escalations.push_back(
                "no override of '" + ev->second + "' anywhere in the program: "
                "dispatch is monomorphic, so only the ~1ns inlining barrier "
                "is paid");
            for (auto &c : d.mechanismClaims)
                if (c.gating && c.effect.rfind("receiver type varies", 0) == 0)
                    c.supports = Severity::Medium;
        }
        if (mono)
            report("devirt", std::to_string(mono) +
                   " virtual dispatch finding(s) monomorphic program-wide");
    }

    // an already line-aligned type in-tree makes relocation available at
    // zero footprint cost; same index FL092 joins against.
    bool alignedOwnerAvailable = false;
    for (const auto &[tn, sig] : result.escapeSummary)
        if (sig.hasDeliberateLayout) { alignedOwnerAvailable = true; break; }
    unsigned stripedEmitted = emitStripedArrayFindings(
        result.diagnostics, result.stripedArrays, result.threadRoles,
        request.config.cacheLineBytes, request.config.l1dSizeBytes,
        alignedOwnerAvailable);
    if (stripedEmitted > 0)
        report("striped_arrays", std::to_string(stripedEmitted) +
               " per-thread striped array finding(s)");

    // Affinity respect runs before dedup so all duplicates demote alike.
    std::string affinityAPI =
        detectAffinityManagement(result.threadRoleFacts);
    unsigned affinityDemoted =
        applyAffinityRespect(result.diagnostics, affinityAPI);
    if (affinityDemoted > 0)
        report("numa", std::to_string(affinityDemoted) +
               " FL060 finding(s) demoted (explicit affinity via " +
               affinityAPI + ")");

    unsigned pagingDemoted = applyPagingRespect(
        result.diagnostics,
        detectPagingManagement(result.threadRoleFacts));
    if (pagingDemoted > 0)
        report("tlb", std::to_string(pagingDemoted) +
               " FL070 finding(s) demoted (paging policy managed in-tree)");

    // Cross-TU deduplication.
    report("dedup", "");
    deduplicateDiagnostics(result.diagnostics);

    // Interaction synthesis.
    report("interactions", "");
    synthesizeInteractions(result.diagnostics);

    // FL092 precedent join.
    unsigned unapplied = synthesizeUnappliedMitigation(
        result.diagnostics, result.escapeSummary);
    if (unapplied > 0)
        report("interactions", std::to_string(unapplied) +
               " FL092 unapplied-mitigation compound(s)");

    // Precision budget.
    PrecisionBudget budget;
    budget.apply(result.diagnostics);

    // Calibration feedback. A requested store that cannot be read is a
    // hard error: proceeding would emit uncalibrated results while the
    // caller believes otherwise.
    std::unique_ptr<CalibrationFeedbackStore> calStore;
    if (!request.feedback.calibrationStorePath.empty()) {
        calStore = std::make_unique<CalibrationFeedbackStore>(
            request.feedback.calibrationStorePath);
        std::string storeErr;
        if (!calStore->load(storeErr)) {
            llvm::errs() << "lshaz: error: " << storeErr << "\n";
            result.status = ScanStatus::ToolError;
            return result;
        }
    }

    if (calStore) {
        result.suppressedByCalibration =
            applyCalibrationSuppression(request.feedback,
                                        result.diagnostics, *calStore);
    }

    // PMU trace feedback.
    if (calStore && (!request.feedback.pmuTracePath.empty() ||
                     !request.feedback.pmuPriorsPath.empty())) {
        report("pmu_feedback", "");
        applyPMUFeedback(request.feedback, result.diagnostics, *calStore);

        // PMU ingestion mutated the store; unpersisted labels calibrate
        // nothing and would silently vanish.
        std::string storeErr;
        if (!calStore->save(storeErr)) {
            llvm::errs() << "lshaz: error: " << storeErr << "\n";
            result.status = ScanStatus::ToolError;
            return result;
        }
    }

    // A finding may not outrank the mechanism claims it established. Rules
    // declaring no claims are unconstrained; where a rule does declare them
    // this holds by construction at output, so the invariant cannot be
    // reintroduced by a future rule the way it was by eleven past ones.4
    std::map<std::string, std::pair<unsigned, unsigned>> bindStats; // {bound, total}
    std::map<std::string, long> slackSum;
    for (auto &d : result.diagnostics) {
        if (d.suppressed || d.mechanismClaims.empty())
            continue;
        Severity ceiling = d.severitySupportedByClaims();
        auto &st = bindStats[d.ruleID];
        ++st.second;
        slackSum[d.ruleID] +=
            static_cast<int>(d.severity) - static_cast<int>(ceiling);
        if (d.severity <= ceiling)
            continue;
        ++st.first;
        d.escalations.push_back(
            "severity clamped from " + std::string(severityToString(d.severity)) +
            " to " + std::string(severityToString(ceiling)) +
            ": the effect justifying the higher grade has an unestablished "
            "precondition");
        d.severity = ceiling;
    }
    if (!bindStats.empty()) {
        unsigned boundAll = 0, totalAll = 0;
        std::string detail;
        for (const auto &[rule, st] : bindStats) {
            boundAll += st.first;
            totalAll += st.second;
            if (st.first == 0)
                continue;   // silent rules are summarised, not enumerated
            detail += (detail.empty() ? "" : " ") + rule + "=" +
                      std::to_string(st.first) + "/" +
                      std::to_string(st.second);
        }
        report("claims", std::to_string(boundAll) + "/" +
               std::to_string(totalAll) + " finding(s) clamped by their claims" +
               (detail.empty() ? " (no rule's lattice bound)" : "; " + detail));
    }

    // A finding outside the scanned tree is third-party by construction.
    // Toolchain and dependency headers reached through -I rather than
    // -isystem are analyzed like project code and are not skipped by the
    // vendored-path patterns, which only match directories inside the
    // project.
    if (!request.workingDirectory.empty()) {
        llvm::SmallString<256> rootBuf(request.workingDirectory);
        llvm::sys::fs::make_absolute(rootBuf);
        // "." makes absolute as "<cwd>/.", which prefix-matches nothing.
        llvm::sys::path::remove_dots(rootBuf, /*remove_dot_dot=*/true);
        std::string root(rootBuf.str());
        if (!root.empty() && root.back() != '/')
            root += '/';
        for (auto &d : result.diagnostics) {
            if (d.suppressed || d.location.file.empty())
                continue;
            if (d.location.file.rfind(root, 0) == 0)
                continue;
            d.suppressed = true;
            ++result.outOfTreeSuppressed;
        }
    }

    // Filter and sort.
    filterAndSort(request.filter, result.diagnostics);

    // Header fingerprint detection: identify missing header patterns.
    std::map<std::string, unsigned> missingHeaderCounts;
    for (const auto &ftu : failedTUsDetailed) {
        // Look for "fatal error: 'header.h' file not found" pattern.
        std::string err = ftu.error;
        auto fatalPos = err.find("fatal error:");
        if (fatalPos != std::string::npos) {
            auto start = err.find('\'', fatalPos);
            auto end = err.find('\'', start + 1);
            if (start != std::string::npos && end != std::string::npos && end > start + 1) {
                std::string header = err.substr(start + 1, end - start - 1);
                ++missingHeaderCounts[header];
            }
        }
    }

    // Emit warnings for headers missing from >= 3 TUs. B001 reports a
    // broken scan, so it bypasses severity/evidence filters by design,
    // but it must not break the sorted-output contract (re-sort below).
    bool b001Emitted = false;
    for (const auto &[header, count] : missingHeaderCounts) {
        if (count >= 3) {
            Diagnostic diag;
            diag.ruleID = "B001";
            diag.severity = Severity::Medium;
            diag.confidence = 1.0;
            diag.evidenceTier = EvidenceTier::Speculative;
            diag.location.file = "<pipeline>";
            diag.location.line = 0;
            std::ostringstream msg;
            msg << "Header '" << header << "' is missing in " << count
                << " TUs. This usually indicates the project needs a full build "
                   "before scanning (custom_target, configure_file).";
            diag.title = msg.str();
                diag.structuralEvidence["missing_header"] = header;
            diag.structuralEvidence["tu_count"] = std::to_string(count);
            result.diagnostics.push_back(std::move(diag));
            b001Emitted = true;
        }
    }
    if (b001Emitted)
        std::sort(result.diagnostics.begin(), result.diagnostics.end(),
                  outputOrder);

    // Extract file paths for compatibility with existing metadata/failedTUs.
    result.failedTUs.clear();
    result.failedTUErrors.clear();
    result.failedTUs.reserve(failedTUsDetailed.size());
    result.failedTUErrors.reserve(failedTUsDetailed.size());
    for (const auto &ftu : failedTUsDetailed) {
        result.failedTUs.push_back(ftu.file);
        result.failedTUErrors.push_back(
            ftu.error.empty() ? "unspecified" : ftu.error);
    }

    // Summary counts.
    result.totalTUsFailed = static_cast<unsigned>(result.failedTUs.size());
    result.metadata.totalTUs = result.totalTUsAnalyzed;
    result.metadata.failedTUCount = result.totalTUsFailed;
    result.metadata.failedTUs = result.failedTUs;
    result.metadata.failedTUErrors = result.failedTUErrors;

    // Status.
    bool parseError = (toolRet != 0 || result.totalTUsFailed > 0);
    bool hasFindings = !result.diagnostics.empty();

    if (parseError && hasFindings)
        result.status = ScanStatus::Findings;
    else if (parseError)
        result.status = ScanStatus::ParseError;
    else if (hasFindings)
        result.status = ScanStatus::Findings;
    else
        result.status = ScanStatus::Clean;

    return result;
}

} // namespace lshaz
