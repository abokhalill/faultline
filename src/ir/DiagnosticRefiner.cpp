#include "faultline/ir/DiagnosticRefiner.h"

#include <llvm/IR/Instructions.h>

#include <algorithm>
#include <sstream>

namespace faultline {

DiagnosticRefiner::DiagnosticRefiner(const IRAnalyzer::ProfileMap &profiles)
    : profiles_(profiles) {}

std::string DiagnosticRefiner::extractFunctionName(const Diagnostic &diag) const {
    // Parse function name from structuralEvidence fields.
    // Patterns: "function=X", "caller=X"
    for (const char *key : {"function=", "caller="}) {
        auto pos = diag.structuralEvidence.find(key);
        if (pos == std::string::npos)
            continue;
        pos += std::strlen(key);
        auto end = diag.structuralEvidence.find(';', pos);
        if (end == std::string::npos)
            end = diag.structuralEvidence.size();
        return diag.structuralEvidence.substr(pos, end - pos);
    }
    return {};
}

const IRFunctionProfile *DiagnosticRefiner::findProfile(
    const std::string &funcName) const {
    if (funcName.empty())
        return nullptr;

    // Try exact match on demangled name first.
    for (const auto &[mangled, profile] : profiles_) {
        if (profile.demangledName == funcName ||
            profile.demangledName.find(funcName) != std::string::npos)
            return &profile;
    }

    // Try mangled name.
    auto it = profiles_.find(funcName);
    if (it != profiles_.end())
        return &it->second;

    return nullptr;
}

void DiagnosticRefiner::refine(std::vector<Diagnostic> &diagnostics) const {
    for (auto &diag : diagnostics) {
        if (diag.ruleID == "FL010") refineFL010(diag);
        else if (diag.ruleID == "FL011") refineFL011(diag);
        else if (diag.ruleID == "FL012") refineFL012(diag);
        else if (diag.ruleID == "FL020") refineFL020(diag);
        else if (diag.ruleID == "FL021") refineFL021(diag);
        else if (diag.ruleID == "FL030") refineFL030(diag);
        else if (diag.ruleID == "FL031") refineFL031(diag);
        else if (diag.ruleID == "FL090") refineFL090(diag);
    }
}

void DiagnosticRefiner::refineFL010(Diagnostic &diag) const {
    auto funcName = extractFunctionName(diag);
    const auto *profile = findProfile(funcName);
    if (!profile)
        return;

    unsigned diagLine = diag.location.line;
    std::string diagFile = diag.location.file;

    // Site-level correlation: find IR atomics at the exact source line.
    bool siteConfirmed = false;
    std::string siteOpName;
    for (const auto &ai : profile->atomics) {
        if (ai.sourceLine == 0)
            continue;
        bool lineMatch = (ai.sourceLine == diagLine);
        bool fileMatch = diagFile.empty() || ai.sourceFile.empty() ||
                         diagFile.find(ai.sourceFile) != std::string::npos ||
                         ai.sourceFile.find(diagFile) != std::string::npos;
        if (lineMatch && fileMatch) {
            bool isSeqCst = (ai.ordering ==
                static_cast<unsigned>(llvm::AtomicOrdering::SequentiallyConsistent));
            if (isSeqCst) {
                siteConfirmed = true;
                switch (ai.op) {
                    case IRAtomicInfo::Store:   siteOpName = "store"; break;
                    case IRAtomicInfo::RMW:     siteOpName = "rmw"; break;
                    case IRAtomicInfo::CmpXchg: siteOpName = "cmpxchg"; break;
                    case IRAtomicInfo::Fence:   siteOpName = "fence"; break;
                    default:                   siteOpName = "atomic"; break;
                }
                break;
            }
        }
    }

    if (siteConfirmed) {
        diag.confidence = std::min(diag.confidence + 0.10, 0.98);
        diag.evidenceTier = EvidenceTier::Proven;
        diag.escalations.push_back(
            "IR site-confirmed: seq_cst " + siteOpName +
            " at line " + std::to_string(diagLine) +
            " survives lowering");
    } else if (profile->seqCstCount > 0) {
        diag.confidence = std::min(diag.confidence + 0.05, 0.92);
        diag.escalations.push_back(
            "IR confirmed: " + std::to_string(profile->seqCstCount) +
            " seq_cst instruction(s) in function (no exact line match)");
    } else if (!profile->atomics.empty()) {
        diag.confidence = std::max(diag.confidence - 0.20, 0.30);
        diag.escalations.push_back(
            "IR refinement: no seq_cst instructions emitted — "
            "compiler may have optimized ordering");
    }

    if (profile->fenceCount > 0) {
        diag.escalations.push_back(
            "IR confirmed: " + std::to_string(profile->fenceCount) +
            " explicit fence instruction(s)");
    }
}

void DiagnosticRefiner::refineFL011(Diagnostic &diag) const {
    auto funcName = extractFunctionName(diag);
    const auto *profile = findProfile(funcName);
    if (!profile)
        return;

    unsigned atomicWriteCount = 0;
    unsigned loopAtomics = 0;
    unsigned siteMatched = 0;
    for (const auto &ai : profile->atomics) {
        if (ai.op == IRAtomicInfo::Store || ai.op == IRAtomicInfo::RMW ||
            ai.op == IRAtomicInfo::CmpXchg) {
            ++atomicWriteCount;
            if (ai.isInLoop)
                ++loopAtomics;
            if (ai.sourceLine > 0)
                ++siteMatched;
        }
    }

    if (atomicWriteCount > 0) {
        diag.confidence = std::min(diag.confidence + 0.10, 0.95);
        if (siteMatched > 0)
            diag.evidenceTier = EvidenceTier::Proven;

        std::ostringstream ss;
        ss << "IR confirmed: " << atomicWriteCount
           << " atomic write instruction(s)";
        if (loopAtomics > 0)
            ss << ", " << loopAtomics << " in loop back-edge blocks";
        if (siteMatched > 0)
            ss << " (" << siteMatched << " with debug-loc site mapping)";
        diag.escalations.push_back(ss.str());
    }
}

void DiagnosticRefiner::refineFL020(Diagnostic &diag) const {
    auto funcName = extractFunctionName(diag);
    const auto *profile = findProfile(funcName);
    if (!profile)
        return;

    unsigned heapCalls = 0;
    unsigned loopHeapCalls = 0;
    for (const auto &csi : profile->heapAllocCalls) {
        if (csi.isIndirect)
            continue;
        ++heapCalls;
        if (csi.isInLoop)
            ++loopHeapCalls;
    }

    if (heapCalls > 0) {
        diag.confidence = std::min(diag.confidence + 0.05, 0.98);

        std::ostringstream ss;
        ss << "IR confirmed: " << heapCalls
           << " heap alloc/free call(s) after inlining";
        if (loopHeapCalls > 0)
            ss << ", " << loopHeapCalls << " in loop blocks";
        diag.escalations.push_back(ss.str());
    } else {
        // AST saw allocation but IR doesn't — may have been inlined/optimized away.
        diag.confidence = std::max(diag.confidence - 0.15, 0.40);
        diag.escalations.push_back(
            "IR refinement: no heap alloc calls found after inlining — "
            "allocation may have been optimized away");
    }
}

void DiagnosticRefiner::refineFL021(Diagnostic &diag) const {
    // Extract function name from evidence: "estimated_frame=..." doesn't have
    // function= key, so parse from hardwareReasoning.
    std::string funcName;
    auto pos = diag.hardwareReasoning.find("Function '");
    if (pos != std::string::npos) {
        pos += 10; // strlen("Function '")
        auto end = diag.hardwareReasoning.find('\'', pos);
        if (end != std::string::npos)
            funcName = diag.hardwareReasoning.substr(pos, end - pos);
    }

    const auto *profile = findProfile(funcName);
    if (!profile)
        return;

    uint64_t irStackSize = profile->totalAllocaBytes;
    constexpr uint64_t threshold = 2048;

    // IR-precise frame below threshold: suppress the AST-based diagnostic.
    if (irStackSize < threshold && irStackSize > 0) {
        diag.suppressed = true;
        diag.escalations.push_back(
            "IR suppressed: actual stack frame " + std::to_string(irStackSize) +
            "B (below " + std::to_string(threshold) +
            "B threshold) — AST estimate was inaccurate");
        return;
    }

    // Replace AST estimate with IR-precise value.
    std::ostringstream ss;
    ss << "IR confirmed: stack frame " << irStackSize
       << "B from " << profile->allocas.size() << " alloca(s)";

    // List large allocas.
    for (const auto &a : profile->allocas) {
        if (a.sizeBytes >= 256)
            ss << " [" << a.name << "=" << a.sizeBytes << "B]";
    }
    diag.escalations.push_back(ss.str());

    // Adjust confidence based on IR/AST agreement.
    // Parse AST estimate from evidence.
    uint64_t astEstimate = 0;
    auto estPos = diag.structuralEvidence.find("estimated_frame=");
    if (estPos != std::string::npos) {
        estPos += 16;
        astEstimate = std::stoull(diag.structuralEvidence.substr(estPos));
    }

    if (irStackSize > 0) {
        diag.confidence = std::min(diag.confidence + 0.10, 0.95);
        diag.evidenceTier = EvidenceTier::Proven;

        // Update evidence with IR-precise size.
        std::ostringstream newEv;
        newEv << diag.structuralEvidence
              << "; ir_frame=" << irStackSize << "B"
              << "; ir_allocas=" << profile->allocas.size();
        diag.structuralEvidence = newEv.str();

        // If IR shows significantly different size, note it.
        if (astEstimate > 0 && irStackSize > astEstimate * 2) {
            diag.escalations.push_back(
                "IR stack frame (" + std::to_string(irStackSize) +
                "B) exceeds AST estimate (" + std::to_string(astEstimate) +
                "B) — compiler-generated temporaries or alignment padding");
        }
    }
}

void DiagnosticRefiner::refineFL030(Diagnostic &diag) const {
    auto funcName = extractFunctionName(diag);
    const auto *profile = findProfile(funcName);
    if (!profile)
        return;

    if (profile->indirectCallCount > 0) {
        diag.confidence = std::min(diag.confidence + 0.10, 0.95);

        std::ostringstream ss;
        ss << "IR confirmed: " << profile->indirectCallCount
           << " indirect call(s) remain after devirtualization";
        diag.escalations.push_back(ss.str());
    } else if (profile->directCallCount > 0) {
        // All calls resolved to direct — devirtualization succeeded.
        diag.confidence = std::max(diag.confidence - 0.25, 0.30);
        diag.escalations.push_back(
            "IR refinement: all calls devirtualized to direct — "
            "BTB pressure eliminated by compiler");
    }
}

void DiagnosticRefiner::refineFL031(Diagnostic &diag) const {
    auto funcName = extractFunctionName(diag);
    const auto *profile = findProfile(funcName);
    if (!profile)
        return;

    // std::function invocation compiles to an indirect call.
    if (profile->indirectCallCount > 0) {
        diag.confidence = std::min(diag.confidence + 0.10, 0.95);
        diag.escalations.push_back(
            "IR confirmed: " + std::to_string(profile->indirectCallCount) +
            " indirect call(s) — type-erased dispatch not eliminated");
    } else {
        diag.confidence = std::max(diag.confidence - 0.20, 0.35);
        diag.escalations.push_back(
            "IR refinement: no indirect calls found — "
            "std::function may have been devirtualized or inlined");
    }
}

void DiagnosticRefiner::refineFL012(Diagnostic &diag) const {
    auto funcName = extractFunctionName(diag);
    const auto *profile = findProfile(funcName);
    if (!profile)
        return;

    unsigned diagLine = diag.location.line;

    bool hasMutexCall = false;
    bool hasAtomicCmpXchg = false;
    bool siteCorrelated = false;

    for (const auto &csi : profile->heapAllocCalls) {
        if (csi.calleeName.find("pthread_mutex") != std::string::npos ||
            csi.calleeName.find("__gthread_mutex") != std::string::npos)
            hasMutexCall = true;
    }
    for (const auto &ai : profile->atomics) {
        if (ai.op == IRAtomicInfo::CmpXchg) {
            hasAtomicCmpXchg = true;
            if (ai.sourceLine == diagLine && diagLine > 0)
                siteCorrelated = true;
        }
    }

    if (hasMutexCall || hasAtomicCmpXchg) {
        diag.confidence = std::min(diag.confidence + 0.05, 0.95);
        std::string detail;
        if (hasMutexCall) detail = "pthread_mutex call";
        else detail = "atomic cmpxchg (lock internals)";

        if (siteCorrelated) {
            diag.evidenceTier = EvidenceTier::Proven;
            detail += " at line " + std::to_string(diagLine);
        }
        diag.escalations.push_back(
            "IR confirmed: " + detail + " present in lowered IR");
    }
}

void DiagnosticRefiner::refineFL090(Diagnostic &diag) const {
    // FL090 is struct-level, not function-level. Scan all profiles for
    // aggregate IR signals that correlate with the compound hazard.
    unsigned totalAtomicWrites = 0;
    unsigned totalIndirectCalls = 0;
    unsigned totalFences = 0;

    for (const auto &[name, profile] : profiles_) {
        for (const auto &ai : profile.atomics) {
            if (ai.op == IRAtomicInfo::Store || ai.op == IRAtomicInfo::RMW ||
                ai.op == IRAtomicInfo::CmpXchg)
                ++totalAtomicWrites;
        }
        totalIndirectCalls += profile.indirectCallCount;
        totalFences += profile.fenceCount;
    }

    if (totalAtomicWrites > 0 || totalFences > 0) {
        std::ostringstream ss;
        ss << "IR aggregate: " << totalAtomicWrites << " atomic write(s), "
           << totalFences << " fence(s), "
           << totalIndirectCalls << " indirect call(s) across module";
        diag.escalations.push_back(ss.str());
    }
}

} // namespace faultline
