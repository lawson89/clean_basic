#ifndef CLEANBASIC_PIPELINE_HPP
#define CLEANBASIC_PIPELINE_HPP

#include "src/cleanbasic/analyzer.hpp"
#include "src/cleanbasic/diagnostics.hpp"
#include "src/cleanbasic/emitter.hpp"
#include "src/cleanbasic/lexer.hpp"
#include "src/cleanbasic/parser.hpp"

#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cb {

struct CheckSourceResult {
    bool ok = false;
    AstArena arena;
    Program program;
    GlobalSymbolTable symbols;
    DiagnosticError error;
};

struct TranspileResult {
    bool ok = false;
    std::string cpp_code;
    GlobalSymbolTable symbols;
    DiagnosticError error;
};

struct TargetSpec {
    bool ok = false;
    bool is_cross = false;
    bool is_windows = false;
    std::string canonical_name;
    std::string zig_triple;
    std::string binary_ext;
    std::string error_message;
};

struct BuildOptions {
    std::string output_cpp_path;
    bool compile_binary = false;
    std::string binary_output;
    std::string target;
    std::string compiler;
};

struct BuildFileResult {
    bool ok = false;
    std::string output_cpp_path;
    std::string output_binary_path;
    std::string compile_command;
    std::vector<std::string> linker_flags;
    DiagnosticError error;
};

inline bool host_is_windows() {
#if defined(_WIN32)
    return true;
#else
    return false;
#endif
}

inline TargetSpec resolve_target_spec(std::string_view raw_target) {
    TargetSpec spec{};
    if (raw_target.empty() || raw_target == "native" || raw_target == "host") {
        spec.ok = true;
        spec.is_cross = false;
        spec.is_windows = host_is_windows();
        spec.canonical_name = "native";
        spec.zig_triple = "";
        if (spec.is_windows) {
            spec.binary_ext = ".exe";
        } else {
            spec.binary_ext = "";
        }
        return spec;
    }

    if (raw_target == "linux" || raw_target == "linux-x86_64" || raw_target == "x86_64-linux" || raw_target == "x86_64-linux-musl") {
        spec.ok = true;
        spec.is_cross = true;
        spec.is_windows = false;
        spec.canonical_name = "linux-x86_64";
        spec.zig_triple = "x86_64-linux-musl";
        spec.binary_ext = "";
        return spec;
    }

    if (raw_target == "linux-gnu" || raw_target == "linux-x86_64-gnu" || raw_target == "x86_64-linux-gnu") {
        spec.ok = true;
        spec.is_cross = true;
        spec.is_windows = false;
        spec.canonical_name = "linux-x86_64-gnu";
        spec.zig_triple = "x86_64-linux-gnu";
        spec.binary_ext = "";
        return spec;
    }

    if (raw_target == "linux-arm64" || raw_target == "linux-aarch64" || raw_target == "aarch64-linux" || raw_target == "aarch64-linux-musl") {
        spec.ok = true;
        spec.is_cross = true;
        spec.is_windows = false;
        spec.canonical_name = "linux-arm64";
        spec.zig_triple = "aarch64-linux-musl";
        spec.binary_ext = "";
        return spec;
    }

    if (raw_target == "windows" || raw_target == "win" || raw_target == "windows-x86_64" ||
        raw_target == "win64" || raw_target == "x86_64-windows" || raw_target == "x86_64-windows-gnu") {
        spec.ok = true;
        spec.is_cross = true;
        spec.is_windows = true;
        spec.canonical_name = "windows-x86_64";
        spec.zig_triple = "x86_64-windows-gnu";
        spec.binary_ext = ".exe";
        return spec;
    }

    if (raw_target == "windows-arm64" || raw_target == "windows-aarch64" ||
        raw_target == "aarch64-windows" || raw_target == "aarch64-windows-gnu") {
        spec.ok = true;
        spec.is_cross = true;
        spec.is_windows = true;
        spec.canonical_name = "windows-arm64";
        spec.zig_triple = "aarch64-windows-gnu";
        spec.binary_ext = ".exe";
        return spec;
    }

    if (raw_target == "macos" || raw_target == "macos-x86_64" || raw_target == "darwin" ||
        raw_target == "darwin-x86_64" || raw_target == "x86_64-macos") {
        spec.ok = true;
        spec.is_cross = true;
        spec.is_windows = false;
        spec.canonical_name = "macos-x86_64";
        spec.zig_triple = "x86_64-macos";
        spec.binary_ext = "";
        return spec;
    }

    if (raw_target == "macos-arm64" || raw_target == "macos-aarch64" || raw_target == "darwin-arm64" ||
        raw_target == "aarch64-macos") {
        spec.ok = true;
        spec.is_cross = true;
        spec.is_windows = false;
        spec.canonical_name = "macos-arm64";
        spec.zig_triple = "aarch64-macos";
        spec.binary_ext = "";
        return spec;
    }

    spec.ok = false;
    spec.error_message = "Unsupported target '" + std::string(raw_target) +
        "'. Supported targets: native, linux (linux-x86_64), linux-arm64, windows (windows-x86_64), windows-arm64, macos (macos-x86_64), macos-arm64.";
    return spec;
}

inline std::string resolve_binary_output_path(
    std::string_view parent_dir,
    std::string_view explicit_binary_output,
    const TargetSpec& target_spec
) {
    std::string bin_path;
    if (!explicit_binary_output.empty()) {
        bin_path = std::string(explicit_binary_output);
    } else {
        bin_path = std::string(parent_dir) + "/app";
    }
    if (target_spec.is_windows) {
        const bool has_exe_ext = bin_path.size() >= 4 &&
            (bin_path.substr(bin_path.size() - 4) == ".exe" || bin_path.substr(bin_path.size() - 4) == ".EXE");
        if (!has_exe_ext) {
            bin_path += ".exe";
        }
    }
    return bin_path;
}

inline std::string resolve_compiler_command(
    const TargetSpec& target_spec,
    std::string_view explicit_compiler
) {
    if (!explicit_compiler.empty()) {
        if (target_spec.is_cross && explicit_compiler == "zig c++") {
            return "zig c++ -target " + target_spec.zig_triple;
        }
        return std::string(explicit_compiler);
    }
    if (target_spec.is_cross) {
        return "zig c++ -target " + target_spec.zig_triple;
    }
    const std::string env_cxx = get_env_var("CXX");
    if (!env_cxx.empty()) {
        return env_cxx;
    }
#if defined(__APPLE__)
    return "clang++";
#else
    return "g++";
#endif
}

inline std::string build_compile_command(
    std::string_view compiler_cmd,
    std::string_view cb_root,
    std::string_view input_parent_dir,
    std::string_view out_cpp_path,
    std::string_view bin_path,
    const std::vector<std::string>& linker_flags
) {
    const std::string q(1, '"');
    std::string cmd = std::string(compiler_cmd) +
        " -O3 -std=c++17 -pthread" +
        " -I" + q + std::string(cb_root) + "/vendor" + q +
        " -I" + q + std::string(cb_root) + "/stdlib" + q +
        " -I" + q + std::string(input_parent_dir) + q +
        " " + q + std::string(out_cpp_path) + q +
        " -o " + q + std::string(bin_path) + q;
    for (const auto& flag : linker_flags) {
        cmd += " " + flag;
    }
    return cmd;
}

struct ModuleLoaderState {
    std::string root_base_dir = ".";
    std::unordered_map<std::string, std::string> open_buffers;
    std::vector<std::string> loading_stack;
    std::vector<std::string> loading_path_stack;
    std::unordered_set<std::string> loaded_modules;
    std::vector<DeclId> merged_decls;
    DiagnosticError error;
    bool has_error = false;
};

inline bool is_in_stack(const std::vector<std::string>& stack, std::string_view item) {
    for (const auto& s : stack) {
        if (s == item) {
            return true;
        }
    }
    return false;
}

inline std::vector<std::string> build_module_candidate_paths(
    std::string_view current_dir,
    std::string_view root_base_dir,
    std::string_view imp_target
) {
    std::vector<std::string> candidates;
    if (is_std_import_target(imp_target)) {
        const std::string std_sub = strip_cb_ext(imp_target.substr(4));
        const std::string cb_root = cleanbasic_root_dir();
        candidates.push_back(cb_root + "/stdlib/" + std_sub + "/" + std_sub + ".cb");
        candidates.push_back(cb_root + "/stdlib/" + std_sub + ".cb");
        candidates.push_back("stdlib/" + std_sub + "/" + std_sub + ".cb");
        candidates.push_back("stdlib/" + std_sub + ".cb");
    } else {
        candidates.push_back(std::string(current_dir) + "/" + std::string(imp_target));
        candidates.push_back(std::string(root_base_dir) + "/" + std::string(imp_target));
        candidates.push_back(std::string(imp_target));
    }
    return candidates;
}

inline std::optional<std::string> find_in_memory_source(
    const std::unordered_map<std::string, std::string>& open_buffers,
    std::string_view candidate_path
) {
    if (open_buffers.empty()) {
        return std::nullopt;
    }
    const std::string exact_key(candidate_path);
    const auto exact_it = open_buffers.find(exact_key);
    if (exact_it != open_buffers.end()) {
        return exact_it->second;
    }
    const std::string norm_candidate = normalize_rel_path(candidate_path);
    if (!norm_candidate.empty()) {
        const auto norm_it = open_buffers.find(norm_candidate);
        if (norm_it != open_buffers.end()) {
            return norm_it->second;
        }
    }
    for (const auto& kv : open_buffers) {
        if (!norm_candidate.empty() && normalize_rel_path(kv.first) == norm_candidate) {
            return kv.second;
        }
    }
    return std::nullopt;
}

inline void load_module_declarations_recursive(
    AstArena& arena,
    const std::vector<DeclId>& decl_ids,
    std::string_view current_module_id,
    std::string_view current_file_path,
    ModuleLoaderState& loader
) {
    if (loader.has_error) {
        return;
    }

    const std::string current_dir = parent_dir_of(current_file_path);

    for (const DeclId did : decl_ids) {
        if (did >= arena.decls.size()) {
            continue;
        }
        arena.decls[did].module_id = std::string(current_module_id);
        arena.decls[did].file_name = std::string(current_file_path);

        if (std::holds_alternative<ImportDecl>(arena.decls[did].payload)) {
            auto imp = std::get<ImportDecl>(arena.decls[did].payload);
            const bool is_cb_file = imp.target.size() > 3 && imp.target.substr(imp.target.size() - 3) == ".cb";
            const bool is_std_mod = is_std_import_target(imp.target);
            if (imp.kind == "module" && (is_cb_file || is_std_mod)) {
                std::string target_module_id = resolve_module_id(current_module_id, imp.target);
                const std::vector<std::string> candidates = build_module_candidate_paths(
                    current_dir,
                    loader.root_base_dir,
                    imp.target
                );
                std::string candidate_path = candidates.front();
                std::optional<std::string> mem_source;
                std::ifstream mod_in;

                for (size_t c_idx = 0; c_idx < candidates.size(); ++c_idx) {
                    candidate_path = candidates[c_idx];
                    mem_source = find_in_memory_source(loader.open_buffers, candidate_path);
                    if (!mem_source.has_value()) {
                        mod_in.open(candidate_path);
                    }
                    if (mem_source.has_value() || mod_in.is_open()) {
                        if (!is_std_mod && c_idx > 0) {
                            target_module_id = resolve_module_id("", imp.target);
                        }
                        break;
                    }
                }

                imp.resolved_module_id = target_module_id;
                arena.decls[did].payload = imp;

                const bool has_module_source = mem_source.has_value() || mod_in.is_open();
                const std::string norm_candidate = normalize_rel_path(candidate_path);
                if (is_in_stack(loader.loading_stack, target_module_id) ||
                    (has_module_source && is_in_stack(loader.loading_path_stack, norm_candidate))) {
                    loader.has_error = true;
                    loader.error = make_error(
                        "ImportError",
                        "Circular module import detected when importing '" + imp.target + "' (module '" + target_module_id + "').",
                        std::string(current_file_path),
                        arena.decls[did].line,
                        arena.decls[did].col
                    );
                    if (mod_in.is_open()) {
                        mod_in.close();
                    }
                    return;
                }

                if (has_module_source) {
                    if (loader.loaded_modules.find(target_module_id) == loader.loaded_modules.end()) {
                        std::string mod_source;
                        if (mem_source.has_value()) {
                            mod_source = mem_source.value();
                            if (mod_in.is_open()) {
                                mod_in.close();
                            }
                        } else {
                            std::ostringstream mod_ss;
                            mod_ss << mod_in.rdbuf();
                            mod_in.close();
                            mod_source = mod_ss.str();
                        }

                        const auto mod_lex = tokenize(mod_source, candidate_path);
                        if (!mod_lex.ok) {
                            loader.has_error = true;
                            loader.error = mod_lex.error;
                            return;
                        }
                        auto mod_parse = parse_tokens_with_arena(
                            mod_lex.tokens,
                            std::move(arena),
                            candidate_path
                        );
                        if (!mod_parse.ok) {
                            arena = std::move(mod_parse.arena);
                            loader.has_error = true;
                            loader.error = mod_parse.error;
                            return;
                        }
                        arena = std::move(mod_parse.arena);

                        loader.loading_stack.push_back(target_module_id);
                        loader.loading_path_stack.push_back(norm_candidate);
                        load_module_declarations_recursive(
                            arena,
                            mod_parse.program.declarations,
                            target_module_id,
                            candidate_path,
                            loader
                        );
                        loader.loading_path_stack.pop_back();
                        loader.loading_stack.pop_back();
                        if (loader.has_error) {
                            return;
                        }
                        loader.loaded_modules.insert(target_module_id);
                    } else if (mod_in.is_open()) {
                        mod_in.close();
                    }
                }
            }
        }
        loader.merged_decls.push_back(did);
    }
}

inline CheckSourceResult check_source(
    std::string_view source,
    std::string_view file_name = "main.cb",
    const std::unordered_map<std::string, std::string>& open_buffers = {}
) {
    const auto lex_res = tokenize(source, file_name);
    if (!lex_res.ok) {
        return CheckSourceResult{false, {}, {}, {}, lex_res.error};
    }

    auto parse_res = parse_tokens(lex_res.tokens, file_name);
    if (!parse_res.ok) {
        return CheckSourceResult{false, {}, {}, {}, parse_res.error};
    }

    ModuleLoaderState loader{};
    loader.root_base_dir = parent_dir_of(file_name);
    loader.open_buffers = open_buffers;
    const std::string norm_root = normalize_rel_path(file_name);
    if (!norm_root.empty()) {
        loader.loading_path_stack.push_back(norm_root);
    }

    load_module_declarations_recursive(
        parse_res.arena,
        parse_res.program.declarations,
        "",
        file_name,
        loader
    );
    if (loader.has_error) {
        return CheckSourceResult{false, {}, {}, {}, loader.error};
    }
    parse_res.program.declarations = std::move(loader.merged_decls);

    auto ana_res = analyze_program(parse_res.arena, parse_res.program);
    if (!ana_res.ok) {
        return CheckSourceResult{false, {}, {}, {}, ana_res.error};
    }

    return CheckSourceResult{
        true,
        std::move(parse_res.arena),
        std::move(parse_res.program),
        std::move(ana_res.symbols),
        {}
    };
}

inline TranspileResult transpile_source(
    std::string_view source,
    std::string_view file_name = "main.cb",
    const std::unordered_map<std::string, std::string>& open_buffers = {}
) {
    auto check_res = check_source(source, file_name, open_buffers);
    if (!check_res.ok) {
        return TranspileResult{false, "", {}, check_res.error};
    }

    std::string cpp_code = emit_program(check_res.arena, check_res.symbols, check_res.program);
    return TranspileResult{true, std::move(cpp_code), std::move(check_res.symbols), {}};
}

inline BuildFileResult build_file_with_options(
    const std::string& input_path,
    const BuildOptions& options
) {
    const TargetSpec target_spec = resolve_target_spec(options.target);
    if (!target_spec.ok) {
        BuildFileResult res{};
        res.ok = false;
        res.error = make_error("TargetError", target_spec.error_message, input_path, 1, 1);
        return res;
    }

    std::ifstream in_file(input_path);
    if (!in_file.is_open()) {
        BuildFileResult res{};
        res.ok = false;
        res.error = make_error("FileNotFoundError", "Source file not found: " + input_path, input_path, 1, 1);
        return res;
    }

    std::ostringstream sstr;
    sstr << in_file.rdbuf();
    in_file.close();
    const std::string source = sstr.str();

    const std::string parent_dir = parent_dir_of(input_path);

    const TranspileResult trans_res = transpile_source(source, input_path);
    if (!trans_res.ok) {
        BuildFileResult res{};
        res.ok = false;
        res.error = trans_res.error;
        return res;
    }

    std::string out_cpp = parent_dir + "/build.cpp";
    if (!options.output_cpp_path.empty()) {
        out_cpp = options.output_cpp_path;
    }
    std::ofstream out_file(out_cpp);
    if (!out_file.is_open()) {
        BuildFileResult res{};
        res.ok = false;
        res.error = make_error("IOError", "Failed to open output file: " + out_cpp, out_cpp, 1, 1);
        return res;
    }
    out_file << trans_res.cpp_code;
    out_file.close();

    std::vector<std::string> linker_flags;
    linker_flags.reserve(trans_res.symbols.link_libs.size());
    for (const auto& lib : trans_res.symbols.link_libs) {
        linker_flags.push_back("-l" + lib);
    }

    std::string bin_path;
    std::string compile_cmd;
    if (options.compile_binary) {
        bin_path = resolve_binary_output_path(parent_dir, options.binary_output, target_spec);
        const std::string cb_root = cleanbasic_root_dir();
        const std::string compiler_cmd = resolve_compiler_command(target_spec, options.compiler);
        compile_cmd = build_compile_command(
            compiler_cmd,
            cb_root,
            parent_dir,
            out_cpp,
            bin_path,
            linker_flags
        );
        const int ret = std::system(compile_cmd.c_str());
        if (ret != 0) {
            BuildFileResult res{};
            res.ok = false;
            res.output_cpp_path = out_cpp;
            res.output_binary_path = bin_path;
            res.compile_command = compile_cmd;
            res.linker_flags = linker_flags;
            res.error = make_error(
                "CompilationError",
                "C++ compilation failed with code " + std::to_string(ret) + " (command: " + compiler_cmd + ")",
                out_cpp,
                1,
                1
            );
            return res;
        }
    }

    BuildFileResult res{};
    res.ok = true;
    res.output_cpp_path = out_cpp;
    res.output_binary_path = bin_path;
    res.compile_command = compile_cmd;
    res.linker_flags = std::move(linker_flags);
    return res;
}

inline BuildFileResult build_file(
    const std::string& input_path,
    const std::string& output_path = "",
    bool compile_binary = false,
    const std::string& binary_output = "",
    const std::string& target = "",
    const std::string& compiler = ""
) {
    BuildOptions opts{};
    opts.output_cpp_path = output_path;
    opts.compile_binary = compile_binary;
    opts.binary_output = binary_output;
    opts.target = target;
    opts.compiler = compiler;
    return build_file_with_options(input_path, opts);
}

}  // namespace cb

#endif  // CLEANBASIC_PIPELINE_HPP
