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
    bool show_help = false;
    bool run_lsp = false;
    std::string input_file;
    std::string output_file;
    bool compile_binary = false;
    std::string binary_output;
    std::string target;
    std::string compiler;
};

inline void print_usage(std::string_view prog_name) {
    std::cout << "Usage: " << prog_name << " build <file.cb> [-o output.cpp] [-c] [-b binary] [-t target] [--cc compiler]" << std::endl
              << "       " << prog_name << " <file.cb>" << std::endl
              << "       " << prog_name << " lsp" << std::endl << std::endl
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

inline CliArgs parse_cli_args(const std::vector<std::string_view>& raw_args) {
    CliArgs args{};
    if (raw_args.empty()) {
        args.show_help = true;
        return args;
    }

    if (raw_args.size() == 1 && (raw_args[0] == "-h" || raw_args[0] == "--help")) {
        args.show_help = true;
        args.ok = true;
        return args;
    }

    if (!raw_args.empty() && (raw_args[0] == "lsp" || raw_args[0] == "--lsp")) {
        args.run_lsp = true;
        args.ok = true;
        return args;
    }

    size_t idx = 0;
    if (raw_args[idx] == "build") {
        idx += 1;
    }

    if (idx >= raw_args.size()) {
        args.show_help = true;
        return args;
    }

    args.input_file = std::string(raw_args[idx]);
    idx += 1;

    while (idx < raw_args.size()) {
        const std::string_view arg = raw_args[idx];
        if (arg == "-o" || arg == "--output") {
            if (idx + 1 < raw_args.size()) {
                args.output_file = std::string(raw_args[idx + 1]);
                idx += 2;
            } else {
                args.ok = false;
                return args;
            }
        } else if (arg == "-c" || arg == "--compile") {
            args.compile_binary = true;
            idx += 1;
        } else if (arg == "-b" || arg == "--bin") {
            if (idx + 1 < raw_args.size()) {
                args.binary_output = std::string(raw_args[idx + 1]);
                args.compile_binary = true;
                idx += 2;
            } else {
                args.ok = false;
                return args;
            }
        } else if (arg == "-t" || arg == "--target") {
            if (idx + 1 < raw_args.size()) {
                args.target = std::string(raw_args[idx + 1]);
                args.compile_binary = true;
                idx += 2;
            } else {
                args.ok = false;
                return args;
            }
        } else if (arg == "--cc") {
            if (idx + 1 < raw_args.size()) {
                args.compiler = std::string(raw_args[idx + 1]);
                idx += 2;
            } else {
                args.ok = false;
                return args;
            }
        } else if (arg == "-h" || arg == "--help") {
            args.show_help = true;
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

inline int run_cli(std::string_view prog_name, const std::vector<std::string_view>& raw_args) {
    const CliArgs args = parse_cli_args(raw_args);

    if (args.show_help || !args.ok) {
        print_usage(prog_name);
        if (args.ok) {
            return 0;
        }
        return 1;
    }

    if (args.run_lsp) {
        return run_lsp_server(std::cin, std::cout);
    }

    const BuildFileResult res = build_file(
        args.input_file,
        args.output_file,
        args.compile_binary,
        args.binary_output,
        args.target,
        args.compiler
    );

    if (!res.ok) {
        std::cerr << "Error: " << format_diagnostic(res.error) << std::endl;
        return 1;
    }

    std::string flags_str;
    if (!res.linker_flags.empty()) {
        flags_str = " (linker flags:";
        for (const auto& flag : res.linker_flags) {
            flags_str += " " + flag;
        }
        flags_str += ")";
    }

    std::cout << "Generated " << res.output_cpp_path << flags_str << std::endl;
    if (args.compile_binary && !res.output_binary_path.empty()) {
        std::cout << "Compiled binary " << res.output_binary_path << std::endl;
    }
    return 0;
}

}  // namespace cb

#ifndef CLEANBASIC_CLI_LIB_ONLY
int main(int argc, char** argv) {
    std::string_view prog_name = "cleanbasic";
    if (argc > 0) {
        prog_name = std::string_view(argv[0]);
    }
    std::vector<std::string_view> raw_args;
    if (argc > 1) {
        raw_args.reserve(static_cast<size_t>(argc - 1));
        for (int i = 1; i < argc; ++i) {
            raw_args.emplace_back(argv[i]);
        }
    }
    return cb::run_cli(prog_name, raw_args);
}
#endif

#endif  // CLEANBASIC_CLI_MAIN_CPP
