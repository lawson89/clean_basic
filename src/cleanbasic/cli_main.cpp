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
};

inline void print_usage(std::string_view prog_name) {
    std::cout << "Usage: " << prog_name << " build <file.cb> [-o output.cpp] [-c]\n"
              << "       " << prog_name << " <file.cb>\n"
              << "       " << prog_name << " lsp\n\n"
              << "Options:\n"
              << "  lsp, --lsp           Start the CleanBasic Language Server over stdio\n"
              << "  -o, --output <file>  Output C++ file path (default: build.cpp)\n"
              << "  -c, --compile        Also compile build.cpp into a native binary\n"
              << "  -h, --help           Show this help message\n";
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

    if (raw_args.size() == 1 && (raw_args[0] == "lsp" || raw_args[0] == "--lsp")) {
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
        args.compile_binary
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
    return 0;
}

}  // namespace cb

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
