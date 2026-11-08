#ifndef CLEANBASIC_CLI_MAIN_CPP
#define CLEANBASIC_CLI_MAIN_CPP

#include "src/cleanbasic/lsp.hpp"
#include "src/cleanbasic/pipeline.hpp"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace cb {

struct CliArgs {
    bool ok = false;
    bool showHelp = false;
    bool runLsp = false;
    std::string inputFile;
    std::string outputFile;
    bool compileBinary = false;
    std::string binaryOutput;
    std::string target;
    std::string compiler;
};

inline void printUsage(std::string_view progName) {
    std::cout << "Usage: " << progName << " build <file.cb> [-o output.cpp] [-c] [-b binary] [-t target] [--cc compiler]" << std::endl
              << "       " << progName << " <file.cb>" << std::endl
              << "       " << progName << " lsp" << std::endl << std::endl
              << "Options:" << std::endl
              << "  lsp, --lsp            Start the CleanBasic Language Server over stdio" << std::endl
              << "  -o, --output <file>   Output C++ file path (default: build.cpp)" << std::endl
              << "  -c, --compile         Compile generated C++ into a binary executable" << std::endl
              << "  -b, --bin <file>      Output binary executable path (implies -c)" << std::endl
              << "  -t, --target <target> Target platform: native, linux, linux-arm64, windows," << std::endl
              << "                        windows-arm64, macos, macos-arm64 (implies -c, uses zig c++)" << std::endl
              << "  --cc <compiler>       C++17 compiler command override (e.g., 'g++', 'clang++', 'zig c++')" << std::endl
              << "  -h, --help            Show this help message" << std::endl;
}

inline CliArgs parseCliArgs(const std::vector<std::string_view>& rawArgs) {
    CliArgs args{};
    if (rawArgs.empty()) {
        args.showHelp = true;
        return args;
    }

    if (rawArgs.size() == 1 && (rawArgs[0] == "-h" || rawArgs[0] == "--help")) {
        args.showHelp = true;
        args.ok = true;
        return args;
    }

    if (!rawArgs.empty() && (rawArgs[0] == "lsp" || rawArgs[0] == "--lsp")) {
        args.runLsp = true;
        args.ok = true;
        return args;
    }

    size_t idx = 0;
    if (rawArgs[idx] == "build") {
        idx += 1;
    }

    if (idx >= rawArgs.size()) {
        args.showHelp = true;
        return args;
    }

    args.inputFile = std::string(rawArgs[idx]);
    idx += 1;

    while (idx < rawArgs.size()) {
        const std::string_view arg = rawArgs[idx];
        if (arg == "-o" || arg == "--output") {
            if (idx + 1 < rawArgs.size()) {
                args.outputFile = std::string(rawArgs[idx + 1]);
                idx += 2;
            } else {
                args.ok = false;
                return args;
            }
        } else if (arg == "-c" || arg == "--compile") {
            args.compileBinary = true;
            idx += 1;
        } else if (arg == "-b" || arg == "--bin") {
            if (idx + 1 < rawArgs.size()) {
                args.binaryOutput = std::string(rawArgs[idx + 1]);
                args.compileBinary = true;
                idx += 2;
            } else {
                args.ok = false;
                return args;
            }
        } else if (arg == "-t" || arg == "--target") {
            if (idx + 1 < rawArgs.size()) {
                args.target = std::string(rawArgs[idx + 1]);
                args.compileBinary = true;
                idx += 2;
            } else {
                args.ok = false;
                return args;
            }
        } else if (arg == "--cc") {
            if (idx + 1 < rawArgs.size()) {
                args.compiler = std::string(rawArgs[idx + 1]);
                idx += 2;
            } else {
                args.ok = false;
                return args;
            }
        } else if (arg == "-h" || arg == "--help") {
            args.showHelp = true;
            args.ok = true;
            return args;
        } else {
            args.ok = false;
            return args;
        }
    }

    args.ok = true;
    return args;
}

inline int runCli(std::string_view progName, const std::vector<std::string_view>& rawArgs) {
    const CliArgs args = parseCliArgs(rawArgs);

    if (args.showHelp || !args.ok) {
        printUsage(progName);
        if (args.ok) {
            return 0;
        }
        return 1;
    }

    if (args.runLsp) {
        return runLspServer(std::cin, std::cout);
    }

    const BuildFileResult res = buildFile(
        args.inputFile,
        args.outputFile,
        args.compileBinary,
        args.binaryOutput,
        args.target,
        args.compiler
    );

    if (!res.ok) {
        std::cerr << "Error: " << formatDiagnostic(res.error) << std::endl;
        return 1;
    }

    std::string flagsStr;
    if (!res.linkerFlags.empty()) {
        flagsStr = " (linker flags:";
        for (const auto& flag : res.linkerFlags) {
            flagsStr += " " + flag;
        }
        flagsStr += ")";
    }

    std::cout << "Generated " << res.outputCppPath << flagsStr << std::endl;
    if (args.compileBinary && !res.outputBinaryPath.empty()) {
        std::cout << "Compiled binary " << res.outputBinaryPath << std::endl;
    }
    return 0;
}

}  // namespace cb

#ifndef CLEANBASIC_CLI_LIB_ONLY
int main(int argc, char** argv) {
    std::string_view progName = "cleanbasic";
    if (argc > 0) {
        progName = std::string_view(argv[0]);
    }
    std::vector<std::string_view> rawArgs;
    if (argc > 1) {
        rawArgs.reserve(static_cast<size_t>(argc - 1));
        for (int i = 1; i < argc; ++i) {
            rawArgs.emplace_back(argv[i]);
        }
    }
    return cb::runCli(progName, rawArgs);
}
#endif

#endif  // CLEANBASIC_CLI_MAIN_CPP
