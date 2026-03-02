#pragma once

#include "faultline/ir/IRFunctionProfile.h"

#include <llvm/IR/Module.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace faultline {

class IRAnalyzer {
public:
    using ProfileMap = std::unordered_map<std::string, IRFunctionProfile>;

    // Analyze a pre-built LLVM module.
    // Non-const: DominatorTree/LoopInfo require mutable Function.
    void analyzeModule(llvm::Module &M);

    const ProfileMap &profiles() const { return profiles_; }

    // Lookup by mangled name.
    const IRFunctionProfile *lookup(const std::string &mangledName) const;

private:
    void analyzeFunction(llvm::Function &F);
    bool isHeapAllocFunction(llvm::StringRef name) const;
    bool isHeapFreeFunction(llvm::StringRef name) const;

    ProfileMap profiles_;
};

} // namespace faultline
