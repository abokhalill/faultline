#pragma once

#include <string>
#include <vector>

namespace lshaz {

class CompileDBResolver {
public:
    // Search for compile_commands.json in standard locations relative to
    // the given project root. Returns the first found path, or empty string.
    static std::string discover(const std::string &projectRoot);

    // Candidate paths searched, in priority order.
    static std::vector<std::string> candidatePaths(const std::string &projectRoot);

    // Attempt to generate compile_commands.json by running cmake.
    // Returns the path to the generated file, or empty string on failure.
    // Requires cmake on PATH and CMakeLists.txt in projectRoot.
    static std::string generateViaCMake(const std::string &projectRoot);

    // Discover, then fall back to CMake generation if needed.
    static std::string discoverOrGenerate(const std::string &projectRoot);
};

} // namespace lshaz
