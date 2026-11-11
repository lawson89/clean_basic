#ifndef CLEANBASIC_CLI_MAIN_CPP
#define CLEANBASIC_CLI_MAIN_CPP

#include "src/cleanbasic/lsp.hpp"
#include "src/cleanbasic/pipeline.hpp"

#include <iomanip>
#include <iostream>
#include <sstream>
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
    std::string optLevel = "0";
    bool isRelease = false;
    bool usePch = true;
    bool rebuildPch = false;
    bool staticLink = false;
    bool showTimings = false;
    std::string errorMessage;
};

inline bool isValidOptLevel(std::string_view level) {
    return level == "0" || level == "1" || level == "2" ||
           level == "3" || level == "s" || level == "z";
}

inline void printUsage(std::string_view progName) {
    std::cout << "Usage: " << progName << " build <file.cb> [-o output.cpp] [-c] [-b binary] [-t target] [--cc compiler] [--static] [-r|--release] [--opt level] [--time]" << std::endl
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
              << "  --static              Link a fully static binary executable on Linux/Windows (implies -c)" << std::endl
              << "  -r, --release         Compile with full optimizations (-O3)" << std::endl
              << "  --debug               Compile in debug mode (-O0, default)" << std::endl
              << "  --opt, -O <0|1|2|3|s> Set C++ optimization level (default: 0)" << std::endl
              << "  --no-pch              Disable precompiled headers during C++ compilation" << std::endl
              << "  --rebuild-pch         Force regeneration of cached C++ precompiled header" << std::endl
              << "  --time                Display detailed transpiler and compiler timing breakdown" << std::endl
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
                args.errorMessage = "Missing output file path after '" + std::string(arg) + "'";
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
                args.errorMessage = "Missing binary output path after '" + std::string(arg) + "'";
                return args;
            }
        } else if (arg == "-t" || arg == "--target") {
            if (idx + 1 < rawArgs.size()) {
                args.target = std::string(rawArgs[idx + 1]);
                args.compileBinary = true;
                idx += 2;
            } else {
                args.ok = false;
                args.errorMessage = "Missing target platform after '" + std::string(arg) + "'";
                return args;
            }
        } else if (arg == "--cc") {
            if (idx + 1 < rawArgs.size()) {
                args.compiler = std::string(rawArgs[idx + 1]);
                idx += 2;
            } else {
                args.ok = false;
                args.errorMessage = "Missing compiler command after '--cc'";
                return args;
            }
        } else if (arg == "-r" || arg == "--release") {
            args.isRelease = true;
            args.optLevel = "3";
            idx += 1;
        } else if (arg == "--debug") {
            args.isRelease = false;
            args.optLevel = "0";
            idx += 1;
        } else if (arg == "--opt" || arg == "-O") {
            if (idx + 1 < rawArgs.size()) {
                const std::string_view level = rawArgs[idx + 1];
                if (!isValidOptLevel(level)) {
                    args.ok = false;
                    args.errorMessage = "Invalid optimization level '" + std::string(level) + "'. Valid levels are: 0, 1, 2, 3, s, z";
                    return args;
                }
                args.optLevel = std::string(level);
                args.isRelease = (level == "3");
                idx += 2;
            } else {
                args.ok = false;
                args.errorMessage = "Missing optimization level after '" + std::string(arg) + "'";
                return args;
            }
        } else if (arg.size() > 2 && arg.substr(0, 2) == "-O") {
            const std::string_view level = arg.substr(2);
            if (!isValidOptLevel(level)) {
                args.ok = false;
                args.errorMessage = "Invalid optimization level '" + std::string(level) + "'. Valid levels are: 0, 1, 2, 3, s, z";
                return args;
            }
            args.optLevel = std::string(level);
            args.isRelease = (level == "3");
            idx += 1;
        } else if (arg.size() > 6 && arg.substr(0, 6) == "--opt=") {
            const std::string_view level = arg.substr(6);
            if (!isValidOptLevel(level)) {
                args.ok = false;
                args.errorMessage = "Invalid optimization level '" + std::string(level) + "'. Valid levels are: 0, 1, 2, 3, s, z";
                return args;
            }
            args.optLevel = std::string(level);
            args.isRelease = (level == "3");
            idx += 1;
        } else if (arg == "--no-pch") {
            args.usePch = false;
            idx += 1;
        } else if (arg == "--rebuild-pch") {
            args.rebuildPch = true;
            args.usePch = true;
            idx += 1;
        } else if (arg == "--static") {
            args.staticLink = true;
            args.compileBinary = true;
            idx += 1;
        } else if (arg == "--time" || arg == "--timing") {
            args.showTimings = true;
            idx += 1;
        } else if (arg == "-h" || arg == "--help") {
            args.showHelp = true;
            args.ok = true;
            return args;
        } else {
            args.ok = false;
            args.errorMessage = "Unknown option or unexpected argument: '" + std::string(arg) + "'";
            return args;
        }
    }

    args.ok = true;
    return args;
}

inline void printBuildTimings(const BuildTimings& t, bool compiledBinary) {
    double total = t.totalMs;
    if (total <= 0.0) {
        total = 0.0001;
    }
    const double transpilePct = (t.transpileTotalMs / total) * 100.0;
    const double compilePct = (t.cppCompileMs / total) * 100.0;

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1);
    oss << "Build Timing Breakdown:" << std::endl
        << "  Transpiler (CleanBasic):" << std::endl
        << "    - Lex & Parse:        " << t.lexParseMs << " ms" << std::endl
        << "    - Module Loading:     " << t.moduleLoadMs << " ms" << std::endl
        << "    - Semantic Analysis:  " << t.analyzeMs << " ms" << std::endl
        << "    - C++ Emission:       " << t.emitMs << " ms" << std::endl
        << "    Total Transpilation: " << t.transpileTotalMs << " ms (" << transpilePct << "%)" << std::endl;
    if (compiledBinary) {
        oss << "  Backend (C++ Compiler):" << std::endl;
        if (t.pchUsed) {
            if (t.pchCacheHit) {
                oss << "    - Precompiled Header: 0.0 ms (cache hit)" << std::endl;
            } else {
                const double pchPct = (t.pchBuildMs / total) * 100.0;
                oss << "    - Precompiled Header: " << t.pchBuildMs << " ms (" << pchPct << "%, built)" << std::endl;
            }
        } else {
            oss << "    - Precompiled Header: disabled" << std::endl;
        }
        oss << "    - C++ Compile & Link: " << t.cppCompileMs << " ms (" << compilePct << "%)" << std::endl;
    }
    oss << "  Total Build Time:       " << t.totalMs << " ms";
    std::cout << oss.str() << std::endl;
}

inline int runCli(std::string_view progName, const std::vector<std::string_view>& rawArgs) {
    const CliArgs args = parseCliArgs(rawArgs);

    if (!args.ok && !args.errorMessage.empty()) {
        std::cerr << "Error: " << args.errorMessage << std::endl;
    }

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

    BuildOptions buildOpts{};
    buildOpts.outputCppPath = args.outputFile;
    buildOpts.compileBinary = args.compileBinary;
    buildOpts.binaryOutput = args.binaryOutput;
    buildOpts.target = args.target;
    buildOpts.compiler = args.compiler;
    buildOpts.optLevel = args.optLevel;
    buildOpts.isRelease = args.isRelease;
    buildOpts.usePch = args.usePch;
    buildOpts.rebuildPch = args.rebuildPch;
    buildOpts.staticLink = args.staticLink;

    const BuildFileResult res = buildFileWithOptions(args.inputFile, buildOpts);

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
    if (args.showTimings) {
        printBuildTimings(res.timings, args.compileBinary);
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
