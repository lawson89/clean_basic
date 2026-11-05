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

struct BuildFileResult {
    bool ok = false;
    std::string output_cpp_path;
    std::vector<std::string> linker_flags;
    DiagnosticError error;
};

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

inline std::string parent_dir_of(std::string_view file_path) {
    const size_t slash_idx = file_path.find_last_of('/');
    if (slash_idx != std::string_view::npos) {
        return std::string(file_path.substr(0, slash_idx));
    }
    return ".";
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
            if (imp.kind == "module" && imp.target.size() > 3 &&
                imp.target.substr(imp.target.size() - 3) == ".cb") {
                std::string target_module_id = resolve_module_id(current_module_id, imp.target);

                std::string candidate_path = current_dir + "/" + imp.target;
                std::optional<std::string> mem_source = find_in_memory_source(loader.open_buffers, candidate_path);
                std::ifstream mod_in;
                if (!mem_source.has_value()) {
                    mod_in.open(candidate_path);
                }

                if (!mem_source.has_value() && !mod_in.is_open()) {
                    candidate_path = loader.root_base_dir + "/" + imp.target;
                    mem_source = find_in_memory_source(loader.open_buffers, candidate_path);
                    if (!mem_source.has_value()) {
                        mod_in.open(candidate_path);
                    }
                    if (mem_source.has_value() || mod_in.is_open()) {
                        target_module_id = resolve_module_id("", imp.target);
                    }
                }
                if (!mem_source.has_value() && !mod_in.is_open()) {
                    candidate_path = imp.target;
                    mem_source = find_in_memory_source(loader.open_buffers, candidate_path);
                    if (!mem_source.has_value()) {
                        mod_in.open(candidate_path);
                    }
                    if (mem_source.has_value() || mod_in.is_open()) {
                        target_module_id = resolve_module_id("", imp.target);
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

inline BuildFileResult build_file(
    const std::string& input_path,
    const std::string& output_path = "",
    bool compile_binary = false,
    const std::string& binary_output = ""
) {
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

    const size_t slash_pos = input_path.find_last_of('/');
    std::string parent_dir = ".";
    if (slash_pos != std::string::npos) {
        parent_dir = input_path.substr(0, slash_pos);
    }

    const TranspileResult trans_res = transpile_source(source, input_path);
    if (!trans_res.ok) {
        BuildFileResult res{};
        res.ok = false;
        res.error = trans_res.error;
        return res;
    }

    std::string out_cpp = parent_dir + "/build.cpp";
    if (!output_path.empty()) {
        out_cpp = output_path;
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

    if (compile_binary) {
        std::string bin_path = parent_dir + "/app";
        if (!binary_output.empty()) {
            bin_path = binary_output;
        }
        std::string cmd = "g++ -O3 -std=c++17 -pthread " + out_cpp + " -o " + bin_path;
        for (const auto& flag : linker_flags) {
            cmd += " " + flag;
        }
        const int ret = std::system(cmd.c_str());
        if (ret != 0) {
            BuildFileResult res{};
            res.ok = false;
            res.output_cpp_path = out_cpp;
            res.linker_flags = linker_flags;
            res.error = make_error("CompilationError", "g++ compilation failed with code " + std::to_string(ret), out_cpp, 1, 1);
            return res;
        }
    }

    BuildFileResult res{};
    res.ok = true;
    res.output_cpp_path = out_cpp;
    res.linker_flags = std::move(linker_flags);
    return res;
}

}  // namespace cb

#endif  // CLEANBASIC_PIPELINE_HPP
