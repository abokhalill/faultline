// SPDX-License-Identifier: Apache-2.0
#include "lshaz/pipeline/CompileDBResolver.h"

#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/Program.h>
#include <llvm/Support/raw_ostream.h>

namespace lshaz {

std::vector<std::string> CompileDBResolver::candidatePaths(
        const std::string &projectRoot) {
    const char *subdirs[] = {
        "build",
        ".",
        "out",
        "cmake-build-debug",
        "cmake-build-release",
        "out/build",
        "build/debug",
        "build/release",
    };

    std::vector<std::string> paths;
    paths.reserve(std::size(subdirs));

    for (const char *sub : subdirs) {
        llvm::SmallString<256> p(projectRoot);
        llvm::sys::path::append(p, sub, "compile_commands.json");
        paths.push_back(std::string(p));
    }
    return paths;
}

std::string CompileDBResolver::discover(const std::string &projectRoot) {
    for (const auto &path : candidatePaths(projectRoot)) {
        if (llvm::sys::fs::exists(path))
            return path;
    }
    return {};
}

std::string CompileDBResolver::generateViaCMake(const std::string &projectRoot) {
    // Verify CMakeLists.txt exists.
    llvm::SmallString<256> cmakeLists(projectRoot);
    llvm::sys::path::append(cmakeLists, "CMakeLists.txt");
    if (!llvm::sys::fs::exists(cmakeLists))
        return {};

    // Verify cmake is available.
    auto cmakePath = llvm::sys::findProgramByName("cmake");
    if (!cmakePath)
        return {};

    llvm::SmallString<256> buildDir(projectRoot);
    llvm::sys::path::append(buildDir, "build");
    llvm::sys::fs::create_directories(buildDir);

    llvm::errs() << "lshaz: running cmake to generate compile_commands.json...\n";

    std::vector<llvm::StringRef> args;
    args.push_back(*cmakePath);
    args.push_back("-S");
    args.push_back(projectRoot);
    args.push_back("-B");
    args.push_back(llvm::StringRef(buildDir));
    args.push_back("-DCMAKE_EXPORT_COMPILE_COMMANDS=ON");

    // Redirect stderr/stdout to temp files.
    llvm::SmallString<256> outFile(buildDir), errFile(buildDir);
    llvm::sys::path::append(outFile, "lshaz-cmake.out");
    llvm::sys::path::append(errFile, "lshaz-cmake.err");
    llvm::StringRef outRedirect(outFile);
    llvm::StringRef errRedirect(errFile);
    std::optional<llvm::StringRef> redirects[] = {
        std::nullopt, outRedirect, errRedirect
    };

    std::string execErr;
    bool failed = false;
    int exitCode = llvm::sys::ExecuteAndWait(
        *cmakePath, args,
        /*Env=*/std::nullopt, redirects,
        /*SecondsToWait=*/120, /*MemoryLimit=*/0,
        &execErr, &failed);

    // Cleanup cmake output files.
    llvm::sys::fs::remove(outFile);
    llvm::sys::fs::remove(errFile);

    if (exitCode != 0 || failed) {
        llvm::errs() << "lshaz: cmake configure failed (exit "
                     << exitCode << ")";
        if (!execErr.empty())
            llvm::errs() << ": " << execErr;
        llvm::errs() << "\n";
        return {};
    }

    // Check if compile_commands.json was generated.
    llvm::SmallString<256> dbPath(buildDir);
    llvm::sys::path::append(dbPath, "compile_commands.json");
    if (llvm::sys::fs::exists(dbPath)) {
        llvm::errs() << "lshaz: generated " << dbPath << "\n";
        return std::string(dbPath);
    }

    return {};
}

std::string CompileDBResolver::discoverOrGenerate(const std::string &projectRoot) {
    std::string found = discover(projectRoot);
    if (!found.empty())
        return found;
    return generateViaCMake(projectRoot);
}

} // namespace lshaz
