// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <clang/Tooling/CompilationDatabase.h>
#include <llvm/Support/Path.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace lshaz {

/// Wrapper around a CompilationDatabase that resolves all relative paths
/// (source files, -I/-isystem include directories, -o outputs) to absolute
/// at construction time. This eliminates ClangTool's need to chdir() to
/// the compile command's Directory, removing the process-global chdir()
/// race that causes non-deterministic failures in parallel scans.
///
/// Downstream code sees only absolute paths and never needs to know about
/// the original relative paths in compile_commands.json.
class AbsolutePathCompilationDatabase
    : public clang::tooling::CompilationDatabase {
public:
    /// Takes ownership of the underlying database.
    explicit AbsolutePathCompilationDatabase(
        std::unique_ptr<clang::tooling::CompilationDatabase> inner);

    std::vector<clang::tooling::CompileCommand>
    getCompileCommands(llvm::StringRef FilePath) const override;

    std::vector<std::string> getAllFiles() const override;

    std::vector<clang::tooling::CompileCommand>
    getAllCompileCommands() const override;

private:
    static clang::tooling::CompileCommand
    resolveCommand(const clang::tooling::CompileCommand &cmd);

    static std::string
    resolvePath(const std::string &path, const std::string &directory);

    std::unique_ptr<clang::tooling::CompilationDatabase> inner_;

    // Index: absolute path -> original relative path (for reverse lookup).
    std::unordered_map<std::string, std::string> absToOrig_;

    // Cached resolved file list.
    std::vector<std::string> allFiles_;
};

} // namespace lshaz
