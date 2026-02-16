#include "faultline/core/Config.h"

#include <llvm/Support/YAMLParser.h>
#include <llvm/Support/YAMLTraits.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/raw_ostream.h>

#include <fstream>
#include <sstream>

// YAML mapping for Config via llvm::yaml.
// Using LLVM's built-in YAML support — no third-party dependency.

namespace llvm {
namespace yaml {

template <>
struct MappingTraits<faultline::Config> {
    static void mapping(IO &io, faultline::Config &cfg) {
        io.mapOptional("cache_line_bytes",       cfg.cacheLineBytes);
        io.mapOptional("cache_line_span_warn",   cfg.cacheLineSpanWarn);
        io.mapOptional("cache_line_span_crit",   cfg.cacheLineSpanCrit);
        io.mapOptional("stack_frame_warn_bytes", cfg.stackFrameWarnBytes);
        io.mapOptional("alloc_size_escalation",  cfg.allocSizeEscalation);
        io.mapOptional("branch_depth_warn",      cfg.branchDepthWarn);
        io.mapOptional("json_output",            cfg.jsonOutput);
        io.mapOptional("output_file",            cfg.outputFile);
        io.mapOptional("hot_function_patterns",  cfg.hotFunctionPatterns);
        io.mapOptional("hot_file_patterns",      cfg.hotFilePatterns);
        io.mapOptional("disabled_rules",         cfg.disabledRules);
        io.mapOptional("page_size",              cfg.pageSize);
    }
};

} // namespace yaml
} // namespace llvm

namespace faultline {

Config Config::defaults() {
    return Config{};
}

Config Config::loadFromFile(const std::string &path) {
    auto bufOrErr = llvm::MemoryBuffer::getFile(path);
    if (!bufOrErr) {
        llvm::errs() << "faultline: warning: cannot open config '"
                     << path << "', using defaults\n";
        return defaults();
    }

    Config cfg = defaults();
    llvm::yaml::Input yin(bufOrErr.get()->getBuffer());
    yin >> cfg;

    if (yin.error()) {
        llvm::errs() << "faultline: warning: config parse error in '"
                     << path << "', using defaults\n";
        return defaults();
    }

    return cfg;
}

} // namespace faultline
