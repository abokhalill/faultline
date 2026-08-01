// SPDX-License-Identifier: Apache-2.0
#include "lshaz/core/HotPathOracle.h"
#include "lshaz/core/Config.h"
#include "lshaz/analysis/CallGraph.h"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Attr.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/Basic/SourceManager.h>

#include <fnmatch.h>

#include <vector>

namespace lshaz {

HotPathOracle::HotPathOracle(const Config &cfg) : config_(cfg) {}

bool HotPathOracle::isHot(const clang::Decl *D) const {
    if (const auto *FD = llvm::dyn_cast_or_null<clang::FunctionDecl>(D))
        return isFunctionHot(FD);
    return false;
}

bool HotPathOracle::isFunctionHot(const clang::FunctionDecl *FD) const {
    if (!FD)
        return false;

    const auto *canon = FD->getCanonicalDecl();
    if (hotCache_.count(canon))
        return true;

    if (matchesProfileFunction(FD)) {
        record(canon, HotnessSource::Profiled);
        return true;
    }
    if (hasHotAnnotation(FD) || matchesConfigPattern(FD)) {
        record(canon, HotnessSource::Declared);
        return true;
    }

    return false;
}

void HotPathOracle::record(const clang::FunctionDecl *FD,
                           HotnessSource src) const {
    hotCache_.insert(FD);
    auto &cur = sources_[FD];
    if (src > cur) cur = src;   // strongest evidence wins
}

HotnessSource HotPathOracle::hotnessSource(
        const clang::FunctionDecl *FD) const {
    if (!FD) return HotnessSource::None;
    auto it = sources_.find(FD->getCanonicalDecl());
    return it == sources_.end() ? HotnessSource::None : it->second;
}

const char *hotnessSourceName(HotnessSource s) {
    switch (s) {
        case HotnessSource::Profiled:        return "profile";
        case HotnessSource::Declared:        return "declared";
        case HotnessSource::InferredDeep:    return "inferred-deep";
        case HotnessSource::InferredShallow: return "inferred-shallow";
        case HotnessSource::None:            break;
    }
    return "none";
}

Severity hotnessSupportedSeverity(HotnessSource s, Severity base) {
    auto demote = [](Severity v, unsigned steps) {
        int r = static_cast<int>(v) - static_cast<int>(steps);
        return r < static_cast<int>(Severity::Informational)
                   ? Severity::Informational
                   : static_cast<Severity>(r);
    };
    switch (s) {
        // An operator naming the path, or a profile that saw it execute,
        // imposes no bound: whatever the rule graded, including escalations
        // above its own base, stands. Returning `base` here would silently
        // cap every escalation.
        case HotnessSource::Profiled:
        case HotnessSource::Declared:
            return Severity::Critical;
        // Nested loops or recursion: repetition is structurally certain,
        // its magnitude is not.
        case HotnessSource::InferredDeep:
            return demote(base, 1);
        // One loop level from an entry. Enough to look, not to assert cost.
        case HotnessSource::InferredShallow:
            return demote(base, 2);
        case HotnessSource::None:
            break;
    }
    return Severity::Informational;
}

void HotPathOracle::loadProfileHotFunctions(
    std::unordered_set<std::string> names) {
    profileHotFunctions_ = std::move(names);
}

bool HotPathOracle::matchesProfileFunction(
    const clang::FunctionDecl *FD) const {
    if (profileHotFunctions_.empty())
        return false;
    // Match by qualified name (demangled form).
    std::string qualName = FD->getQualifiedNameAsString();
    if (profileHotFunctions_.count(qualName))
        return true;
    // Match by bare name (perf symbols often lack namespace).
    std::string name = FD->getNameAsString();
    if (profileHotFunctions_.count(name))
        return true;
    return false;
}

void HotPathOracle::markHot(const clang::FunctionDecl *FD) {
    if (FD)
        record(FD->getCanonicalDecl(), HotnessSource::Declared);
}

bool HotPathOracle::hasHotAnnotation(const clang::FunctionDecl *FD) const {
    for (const auto *A : FD->attrs()) {
        // [[clang::annotate("lshaz_hot")]]
        if (const auto *Ann = llvm::dyn_cast<clang::AnnotateAttr>(A)) {
            if (Ann->getAnnotation() == "lshaz_hot")
                return true;
        }
        // __attribute__((hot)) — GCC/Clang standard hot attribute
        if (llvm::isa<clang::HotAttr>(A))
            return true;
    }
    return false;
}

bool HotPathOracle::matchesConfigPattern(const clang::FunctionDecl *FD) const {
    std::string qualName = FD->getQualifiedNameAsString();

    for (const auto &pat : config_.hotFunctionPatterns) {
        if (fnmatch(pat.c_str(), qualName.c_str(), 0) == 0)
            return true;
    }

    const auto &SM = FD->getASTContext().getSourceManager();
    auto loc = FD->getLocation();
    if (loc.isValid()) {
        std::string filename = SM.getFilename(SM.getSpellingLoc(loc)).str();
        for (const auto &pat : config_.hotFilePatterns) {
            if (fnmatch(pat.c_str(), filename.c_str(), 0) == 0)
                return true;
        }
    }

    return false;
}

void HotPathOracle::propagateViaCallGraph(const CallGraph &cg,
                                           unsigned maxDepth) {
    // Snapshot current hot roots (avoid iterator invalidation).
    std::unordered_set<const clang::FunctionDecl *> roots(hotCache_);

    if (roots.empty())
        return;

    auto reachable = cg.transitiveCallees(roots, maxDepth);
    for (const auto *fn : reachable) {
        // Reached from a declared/profiled root: inherits that standing.
        record(fn, HotnessSource::Declared);
    }
}

void HotPathOracle::inferFromCodeShape(const CallGraph &cg) {
    // Seeds: functions this TU hands to a thread primitive, plus main. A
    // thread body is a steady-state loop by construction in server code.
    const auto &entryNames = cg.threadEntryNames();
    std::unordered_map<const clang::FunctionDecl *, unsigned> depth;
    std::vector<const clang::FunctionDecl *> work;

    for (const auto *fn : cg.functions()) {
        const std::string qn = fn->getQualifiedNameAsString();
        if (fn->isMain() || entryNames.count(qn) ||
            entryNames.count(fn->getNameAsString())) {
            depth[fn] = 0;
            work.push_back(fn);
        }
    }
    if (work.empty())
        return;

    // Relax to a fixed point. Values are bounded by kMaxDepth and only
    // increase, so a cycle (recursion) settles instead of diverging.
    constexpr unsigned kMaxDepth = 4;
    while (!work.empty()) {
        const auto *caller = work.back();
        work.pop_back();
        const unsigned d = depth[caller];
        for (const auto *callee : cg.callees(caller)) {
            unsigned nd = d + cg.callSiteLoopDepth(caller, callee);
            // A call into an already-hot region is repetition even without a
            // loop here: recursion re-enters the same body.
            if (callee == caller && nd == d) nd = d + 1;
            if (nd > kMaxDepth) nd = kMaxDepth;
            auto it = depth.find(callee);
            if (it != depth.end() && it->second >= nd)
                continue;
            depth[callee] = nd;
            work.push_back(callee);
        }
    }

    for (const auto &[fn, d] : depth) {
        if (d == 0) continue;   // reachable but never repeated: not hot
        record(fn, d >= 2 ? HotnessSource::InferredDeep
                          : HotnessSource::InferredShallow);
    }
}

} // namespace lshaz
