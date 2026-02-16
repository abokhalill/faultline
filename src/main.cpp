#include "faultline/analysis/FaultlineAction.h"
#include "faultline/core/Config.h"
#include "faultline/core/Diagnostic.h"
#include "faultline/core/Severity.h"
#include "faultline/output/OutputFormatter.h"

#include <clang/Tooling/CommonOptionsParser.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/FileSystem.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace clang::tooling;

static llvm::cl::OptionCategory FaultlineCat("faultline options");

static llvm::cl::opt<std::string> ConfigPath(
    "config",
    llvm::cl::desc("Path to faultline.config.yaml"),
    llvm::cl::value_desc("file"),
    llvm::cl::cat(FaultlineCat));

static llvm::cl::opt<bool> JSONFlag(
    "json",
    llvm::cl::desc("Emit JSON output"),
    llvm::cl::cat(FaultlineCat));

static llvm::cl::opt<std::string> OutputFile(
    "output",
    llvm::cl::desc("Write output to file instead of stdout"),
    llvm::cl::value_desc("file"),
    llvm::cl::cat(FaultlineCat));

static llvm::cl::opt<std::string> MinSev(
    "min-severity",
    llvm::cl::desc("Minimum severity to report (Informational|Medium|High|Critical)"),
    llvm::cl::init("Informational"),
    llvm::cl::cat(FaultlineCat));

static faultline::Severity parseSeverity(const std::string &s) {
    if (s == "Critical")      return faultline::Severity::Critical;
    if (s == "High")          return faultline::Severity::High;
    if (s == "Medium")        return faultline::Severity::Medium;
    return faultline::Severity::Informational;
}

int main(int argc, const char **argv) {
    auto parser = CommonOptionsParser::create(argc, argv, FaultlineCat);
    if (!parser) {
        llvm::errs() << parser.takeError();
        return 1;
    }

    // Load config.
    faultline::Config cfg = ConfigPath.empty()
        ? faultline::Config::defaults()
        : faultline::Config::loadFromFile(ConfigPath);

    // CLI overrides.
    if (JSONFlag)
        cfg.jsonOutput = true;
    if (!OutputFile.empty())
        cfg.outputFile = OutputFile;
    cfg.minSeverity = parseSeverity(MinSev);

    // Run analysis.
    ClangTool tool(parser->getCompilations(), parser->getSourcePathList());

    std::vector<faultline::Diagnostic> diagnostics;
    faultline::FaultlineActionFactory factory(cfg, diagnostics);

    int ret = tool.run(&factory);

    // Filter by minimum severity.
    diagnostics.erase(
        std::remove_if(diagnostics.begin(), diagnostics.end(),
                       [&](const faultline::Diagnostic &d) {
                           return static_cast<uint8_t>(d.severity) <
                                  static_cast<uint8_t>(cfg.minSeverity);
                       }),
        diagnostics.end());

    // Sort: Critical first, then by file/line.
    std::sort(diagnostics.begin(), diagnostics.end(),
              [](const faultline::Diagnostic &a, const faultline::Diagnostic &b) {
                  if (a.severity != b.severity)
                      return static_cast<uint8_t>(a.severity) >
                             static_cast<uint8_t>(b.severity);
                  if (a.location.file != b.location.file)
                      return a.location.file < b.location.file;
                  return a.location.line < b.location.line;
              });

    // Format output.
    std::unique_ptr<faultline::OutputFormatter> formatter;
    if (cfg.jsonOutput)
        formatter = std::make_unique<faultline::JSONOutputFormatter>();
    else
        formatter = std::make_unique<faultline::CLIOutputFormatter>();

    std::string output = formatter->format(diagnostics);

    // Emit.
    if (cfg.outputFile.empty()) {
        llvm::outs() << output;
    } else {
        std::error_code EC;
        llvm::raw_fd_ostream file(cfg.outputFile, EC, llvm::sys::fs::OF_Text);
        if (EC) {
            llvm::errs() << "faultline: error: cannot open output file '"
                         << cfg.outputFile << "': " << EC.message() << "\n";
            return 1;
        }
        file << output;
    }

    return ret == 0 ? (diagnostics.empty() ? 0 : 1) : 2;
}
