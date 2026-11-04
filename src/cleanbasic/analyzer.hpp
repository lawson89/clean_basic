#ifndef CLEANBASIC_ANALYZER_HPP
#define CLEANBASIC_ANALYZER_HPP

#include "src/cleanbasic/ast.hpp"
#include "src/cleanbasic/diagnostics.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace cb {

struct TypeSymbol {
    std::string name;
    std::string module_id;
    std::string key;
    std::unordered_map<std::string, TypeId> fields;
    std::vector<FieldDecl> field_order;
    bool is_pub = false;
    bool is_extern = false;
    std::string cpp_name;
};

struct FnSymbol {
    std::string name;
    std::string module_id;
    std::string key;
    std::vector<Param> params;
    TypeId return_type = INVALID_ID;
    bool is_fallible = false;
    bool is_pub = false;
    bool is_extern = false;
    std::string cpp_name;
};

struct AliasSymbol {
    std::string name;
    std::string module_id;
    std::string key;
    TypeId target_type;
    bool is_pub = false;
    std::string cpp_name;
};

struct VarSymbol {
    std::string name;
    TypeId type_id = INVALID_ID;
    bool is_byref = false;
    bool is_const = false;
};

struct GlobalSymbolTable {
    std::unordered_map<std::string, TypeSymbol> types;
    std::unordered_map<std::string, AliasSymbol> aliases;
    std::unordered_map<std::string, FnSymbol> functions;
    std::vector<std::string> headers;
    std::vector<std::string> link_libs;
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> modules;
    std::unordered_map<std::string, std::unordered_map<std::string, VarSymbol>> fn_scopes;
    TypeId type_int = INVALID_ID;
    TypeId type_float = INVALID_ID;
    TypeId type_str = INVALID_ID;
    TypeId type_bool = INVALID_ID;
    TypeId type_void = INVALID_ID;
    TypeId type_auto = INVALID_ID;
    TypeId type_null = INVALID_ID;
    TypeId type_any = INVALID_ID;
    TypeId type_err = INVALID_ID;
    TypeId type_cptr = INVALID_ID;
};

struct AnalysisResult {
    bool ok = true;
    GlobalSymbolTable symbols;
    DiagnosticError error;
};

inline void register_builtins(AstArena& arena, GlobalSymbolTable& symbols) {
    const TypeId int_t = find_or_add_type(arena, "int");
    const TypeId float_t = find_or_add_type(arena, "float");
    const TypeId str_t = find_or_add_type(arena, "str");
    const TypeId bool_t = find_or_add_type(arena, "bool");
    const TypeId void_t = find_or_add_type(arena, "void");
    const TypeId auto_t = find_or_add_type(arena, "auto");
    const TypeId null_t = find_or_add_type(arena, "null");
    const TypeId any_t = find_or_add_type(arena, "any");
    const TypeId err_t = find_or_add_type(arena, "err");
    const TypeId cptr_t = find_or_add_type(arena, "cptr");

    symbols.type_int = int_t;
    symbols.type_float = float_t;
    symbols.type_str = str_t;
    symbols.type_bool = bool_t;
    symbols.type_void = void_t;
    symbols.type_auto = auto_t;
    symbols.type_null = null_t;
    symbols.type_any = any_t;
    symbols.type_err = err_t;
    symbols.type_cptr = cptr_t;

    TypeSymbol err_type{};
    err_type.name = "err";
    err_type.module_id = "";
    err_type.key = symbol_key("", "err");
    err_type.is_pub = true;
    err_type.fields["msg"] = str_t;
    err_type.fields["code"] = int_t;
    err_type.field_order.push_back(FieldDecl{1, 1, "msg", str_t});
    err_type.field_order.push_back(FieldDecl{1, 1, "code", int_t});
    err_type.cpp_name = "cb_err";
    symbols.types[err_type.key] = std::move(err_type);

    FnSymbol print_fn{};
    print_fn.name = "print";
    print_fn.module_id = "";
    print_fn.key = symbol_key("", "print");
    print_fn.is_pub = true;
    print_fn.is_fallible = false;
    print_fn.cpp_name = "cb_print";
    symbols.functions[print_fn.key] = std::move(print_fn);

    FnSymbol len_fn{};
    len_fn.name = "len";
    len_fn.module_id = "";
    len_fn.key = symbol_key("", "len");
    len_fn.is_pub = true;
    len_fn.params.push_back(Param{1, 1, "container", any_t, false});
    len_fn.return_type = int_t;
    len_fn.cpp_name = "cb_len";
    symbols.functions[len_fn.key] = std::move(len_fn);

    FnSymbol str_fn{};
    str_fn.name = "str";
    str_fn.module_id = "";
    str_fn.key = symbol_key("", "str");
    str_fn.is_pub = true;
    str_fn.params.push_back(Param{1, 1, "val", any_t, false});
    str_fn.return_type = str_t;
    str_fn.cpp_name = "cb_str";
    symbols.functions[str_fn.key] = std::move(str_fn);
}

inline bool is_builtin_type_name(std::string_view name) {
    return name == "int" || name == "float" || name == "str" || name == "bool" ||
           name == "void" || name == "auto" || name == "null" || name == "any" ||
           name == "err" || name == "list" || name == "dict" || name == "cptr";
}

inline bool is_builtin_symbol_name(std::string_view name) {
    return name == "err" || name == "print" || name == "len" || name == "str";
}

inline bool is_module_alias_in_scope(
    const GlobalSymbolTable& symbols,
    std::string_view current_module_id,
    std::string_view ident_name,
    const std::unordered_map<std::string, VarSymbol>& scope,
    std::string& out_target_module_id
) {
    if (scope.find(std::string(ident_name)) != scope.end()) {
        return false;
    }
    const auto mod_it = symbols.modules.find(std::string(current_module_id));
    if (mod_it == symbols.modules.end()) {
        return false;
    }
    const auto alias_it = mod_it->second.find(std::string(ident_name));
    if (alias_it == mod_it->second.end()) {
        return false;
    }
    out_target_module_id = alias_it->second;
    return true;
}

inline std::string resolve_symbol_key(
    const GlobalSymbolTable& symbols,
    std::string_view current_module_id,
    std::string_view name
) {
    if (name.find("::") != std::string_view::npos) {
        return std::string(name);
    }
    const size_t dot_idx = name.find('.');
    if (dot_idx != std::string_view::npos) {
        const std::string prefix = std::string(name.substr(0, dot_idx));
        const std::string sym = std::string(name.substr(dot_idx + 1));
        const auto mod_it = symbols.modules.find(std::string(current_module_id));
        if (mod_it != symbols.modules.end()) {
            const auto alias_it = mod_it->second.find(prefix);
            if (alias_it != mod_it->second.end()) {
                return symbol_key(alias_it->second, sym);
            }
        }
        return symbol_key(prefix, sym);
    }

    const std::string key = symbol_key(current_module_id, name);
    if (symbols.types.count(key) > 0 || symbols.aliases.count(key) > 0 || symbols.functions.count(key) > 0) {
        return key;
    }
    if (is_builtin_symbol_name(name)) {
        return symbol_key("", name);
    }
    return key;
}

inline TypeId infer_expr_type(
    const AstArena& arena,
    ExprId expr_id,
    const GlobalSymbolTable& symbols,
    const std::unordered_map<std::string, VarSymbol>& scope,
    std::string_view current_module_id
);

inline std::optional<FnSymbol> resolve_func_call(
    const AstArena& arena,
    const FuncCall& call,
    const GlobalSymbolTable& symbols,
    std::string_view current_module_id,
    const std::unordered_map<std::string, VarSymbol>& scope = {}
) {
    if (call.callee >= arena.exprs.size()) {
        return std::nullopt;
    }
    const auto& callee_node = arena.exprs[call.callee];

    if (std::holds_alternative<Identifier>(callee_node.payload)) {
        const auto& ident = std::get<Identifier>(callee_node.payload);
        const std::string key = resolve_symbol_key(symbols, current_module_id, ident.name);
        const auto it = symbols.functions.find(key);
        if (it != symbols.functions.end()) {
            if (ident.name.find('.') != std::string::npos && !it->second.is_pub) {
                return std::nullopt;
            }
            return it->second;
        }
        return std::nullopt;
    }

    if (std::holds_alternative<FieldAccess>(callee_node.payload)) {
        const auto& access = std::get<FieldAccess>(callee_node.payload);
        if (access.target < arena.exprs.size() &&
            std::holds_alternative<Identifier>(arena.exprs[access.target].payload)) {
            const auto& tgt_ident = std::get<Identifier>(arena.exprs[access.target].payload);
            std::string target_mod_id;
            if (is_module_alias_in_scope(symbols, current_module_id, tgt_ident.name, scope, target_mod_id)) {
                const std::string key = symbol_key(target_mod_id, access.field_name);
                const auto it = symbols.functions.find(key);
                if (it != symbols.functions.end() && it->second.is_pub) {
                    return it->second;
                }
                return std::nullopt;
            }
        }

        // UFCS Step 1: Current module
        const std::string local_key = symbol_key(current_module_id, access.field_name);
        const auto local_it = symbols.functions.find(local_key);
        if (local_it != symbols.functions.end()) {
            return local_it->second;
        }

        // UFCS Step 2: Receiver's defining module
        const TypeId recv_tid = infer_expr_type(arena, access.target, symbols, scope, current_module_id);
        std::string recv_type_key;
        if (recv_tid < arena.types.size()) {
            recv_type_key = resolve_symbol_key(symbols, current_module_id, arena.types[recv_tid].name);
            const auto [recv_mod_id, recv_base] = split_symbol_key(recv_type_key);
            if (!recv_mod_id.empty()) {
                const std::string recv_fn_key = symbol_key(recv_mod_id, access.field_name);
                const auto recv_fn_it = symbols.functions.find(recv_fn_key);
                if (recv_fn_it != symbols.functions.end()) {
                    if (recv_fn_it->second.is_pub) {
                        return recv_fn_it->second;
                    }
                    return std::nullopt;
                }
            }
        }

        // UFCS Step 3: Directly imported modules of current_module_id
        const auto mod_it = symbols.modules.find(std::string(current_module_id));
        if (mod_it != symbols.modules.end()) {
            std::vector<FnSymbol> pub_candidates;
            std::unordered_set<std::string> seen_keys;
            for (const auto& [alias_name, imported_mod_id] : mod_it->second) {
                const std::string cand_key = symbol_key(imported_mod_id, access.field_name);
                if (seen_keys.find(cand_key) != seen_keys.end()) {
                    continue;
                }
                seen_keys.insert(cand_key);
                const auto cand_it = symbols.functions.find(cand_key);
                if (cand_it != symbols.functions.end() && cand_it->second.is_pub) {
                    pub_candidates.push_back(cand_it->second);
                }
            }
            if (pub_candidates.size() == 1) {
                return pub_candidates[0];
            }
            if (pub_candidates.size() > 1 && !recv_type_key.empty()) {
                std::vector<FnSymbol> type_matched;
                for (const auto& cand : pub_candidates) {
                    if (!cand.params.empty() && cand.params[0].type_id < arena.types.size()) {
                        const std::string param_key = resolve_symbol_key(
                            symbols, cand.module_id, arena.types[cand.params[0].type_id].name
                        );
                        if (param_key == recv_type_key) {
                            type_matched.push_back(cand);
                        }
                    }
                }
                if (type_matched.size() == 1) {
                    return type_matched[0];
                }
            }
        }
        return std::nullopt;
    }

    return std::nullopt;
}

inline std::optional<TypeSymbol> resolve_struct_constructor(
    const AstArena& arena,
    const FuncCall& call,
    const GlobalSymbolTable& symbols,
    std::string_view current_module_id,
    const std::unordered_map<std::string, VarSymbol>& scope = {}
) {
    if (call.callee >= arena.exprs.size()) {
        return std::nullopt;
    }
    const auto& callee_node = arena.exprs[call.callee];

    if (std::holds_alternative<Identifier>(callee_node.payload)) {
        const auto& ident = std::get<Identifier>(callee_node.payload);
        std::string key = resolve_symbol_key(symbols, current_module_id, ident.name);
        if (symbols.functions.find(key) != symbols.functions.end()) {
            return std::nullopt;
        }
        std::unordered_set<std::string> seen;
        std::string active_mod(current_module_id);
        while (symbols.aliases.find(key) != symbols.aliases.end()) {
            if (seen.find(key) != seen.end()) {
                break;
            }
            seen.insert(key);
            const auto& asym = symbols.aliases.at(key);
            active_mod = asym.module_id;
            if (asym.target_type >= arena.types.size()) {
                break;
            }
            key = resolve_symbol_key(symbols, active_mod, arena.types[asym.target_type].name);
        }
        const auto it = symbols.types.find(key);
        if (it != symbols.types.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    if (std::holds_alternative<FieldAccess>(callee_node.payload)) {
        const auto& access = std::get<FieldAccess>(callee_node.payload);
        if (access.target < arena.exprs.size() &&
            std::holds_alternative<Identifier>(arena.exprs[access.target].payload)) {
            const auto& mod_ident = std::get<Identifier>(arena.exprs[access.target].payload);
            std::string target_mod_id;
            if (!is_module_alias_in_scope(symbols, current_module_id, mod_ident.name, scope, target_mod_id)) {
                return std::nullopt;
            }
            std::string key = symbol_key(target_mod_id, access.field_name);
            if (symbols.functions.find(key) != symbols.functions.end()) {
                return std::nullopt;
            }
            std::unordered_set<std::string> seen;
            std::string active_mod = target_mod_id;
            bool first_hop = true;
            while (symbols.aliases.find(key) != symbols.aliases.end()) {
                if (seen.find(key) != seen.end()) {
                    break;
                }
                seen.insert(key);
                const auto& asym = symbols.aliases.at(key);
                if (first_hop && !asym.is_pub) {
                    return std::nullopt;
                }
                first_hop = false;
                active_mod = asym.module_id;
                if (asym.target_type >= arena.types.size()) {
                    break;
                }
                key = resolve_symbol_key(symbols, active_mod, arena.types[asym.target_type].name);
            }
            const auto it = symbols.types.find(key);
            if (it != symbols.types.end()) {
                if (first_hop && !it->second.is_pub) {
                    return std::nullopt;
                }
                return it->second;
            }
        }
        return std::nullopt;
    }

    return std::nullopt;
}

inline TypeNode resolve_alias_node(
    const AstArena& arena,
    const GlobalSymbolTable& symbols,
    std::string_view current_module_id,
    TypeId tid
) {
    if (tid >= arena.types.size()) {
        return TypeNode{};
    }

    std::unordered_set<std::string> seen;
    TypeNode curr = arena.types[tid];
    std::string active_mod_id(current_module_id);

    while (!curr.name.empty()) {
        const std::string key = resolve_symbol_key(symbols, active_mod_id, curr.name);
        const auto it = symbols.aliases.find(key);
        if (it == symbols.aliases.end()) {
            break;
        }
        if (curr.name.find('.') != std::string::npos && !it->second.is_pub) {
            break;
        }
        if (seen.find(key) != seen.end()) {
            break;
        }
        seen.insert(key);
        active_mod_id = it->second.module_id;
        const TypeId aliased_id = it->second.target_type;
        if (aliased_id >= arena.types.size()) {
            break;
        }
        const TypeNode& aliased = arena.types[aliased_id];

        TypeNode next_node{};
        next_node.line = curr.line;
        next_node.col = curr.col;
        next_node.name = aliased.name;
        next_node.type_args = aliased.type_args;
        curr = std::move(next_node);
    }

    if (!curr.name.empty() && !is_builtin_type_name(curr.name)) {
        const std::string type_key = resolve_symbol_key(symbols, active_mod_id, curr.name);
        const auto tit = symbols.types.find(type_key);
        if (tit != symbols.types.end()) {
            if (curr.name.find('.') == std::string::npos || tit->second.is_pub) {
                curr.name = tit->second.key;
            }
        }
    }

    return curr;
}

inline TypeId resolve_alias(
    const AstArena& arena,
    const GlobalSymbolTable& symbols,
    std::string_view current_module_id,
    TypeId tid
) {
    if (tid >= arena.types.size()) {
        return tid;
    }
    const TypeNode resolved_node = resolve_alias_node(arena, symbols, current_module_id, tid);
    const TypeId found = find_type(arena, resolved_node.name, resolved_node.type_args);
    if (found != INVALID_ID) {
        return found;
    }
    return tid;
}

inline TypeId infer_expr_type(
    const AstArena& arena,
    ExprId expr_id,
    const GlobalSymbolTable& symbols,
    const std::unordered_map<std::string, VarSymbol>& scope,
    std::string_view current_module_id
) {
    if (expr_id >= arena.exprs.size()) {
        return symbols.type_auto;
    }

    const auto& expr_node = arena.exprs[expr_id];
    const auto& payload = expr_node.payload;

    if (std::holds_alternative<NumberLiteral>(payload)) {
        const auto& lit = std::get<NumberLiteral>(payload);
        if (lit.is_float) {
            return symbols.type_float;
        }
        return symbols.type_int;
    }

    if (std::holds_alternative<StringLiteral>(payload)) {
        return symbols.type_str;
    }

    if (std::holds_alternative<BoolLiteral>(payload)) {
        return symbols.type_bool;
    }

    if (std::holds_alternative<NullLiteral>(payload)) {
        return symbols.type_null;
    }

    if (std::holds_alternative<Identifier>(payload)) {
        const auto& ident = std::get<Identifier>(payload);
        const auto it = scope.find(ident.name);
        if (it != scope.end()) {
            return it->second.type_id;
        }
        return symbols.type_auto;
    }


    if (std::holds_alternative<ListLiteral>(payload)) {
        const auto& llit = std::get<ListLiteral>(payload);
        TypeId elem_type = symbols.type_int;
        if (!llit.elements.empty()) {
            elem_type = infer_expr_type(arena, llit.elements[0], symbols, scope, current_module_id);
        }
        const TypeId list_tid = find_type(arena, "list", {elem_type});
        if (list_tid != INVALID_ID) {
            return list_tid;
        }
        return symbols.type_auto;
    }

    if (std::holds_alternative<DictLiteral>(payload)) {
        const auto& dlit = std::get<DictLiteral>(payload);
        TypeId k_type = symbols.type_str;
        TypeId v_type = symbols.type_int;
        if (!dlit.entries.empty()) {
            k_type = infer_expr_type(arena, dlit.entries[0].key, symbols, scope, current_module_id);
            v_type = infer_expr_type(arena, dlit.entries[0].value, symbols, scope, current_module_id);
        }
        const TypeId dict_tid = find_type(arena, "dict", {k_type, v_type});
        if (dict_tid != INVALID_ID) {
            return dict_tid;
        }
        return symbols.type_auto;
    }

    if (std::holds_alternative<FieldAccess>(payload)) {
        const auto& faccess = std::get<FieldAccess>(payload);
        const TypeId target_tid = infer_expr_type(arena, faccess.target, symbols, scope, current_module_id);
        if (target_tid < arena.types.size()) {
            const std::string& tname = arena.types[target_tid].name;
            const std::string type_key = resolve_symbol_key(symbols, current_module_id, tname);
            const auto it = symbols.types.find(type_key);
            if (it != symbols.types.end()) {
                const auto fit = it->second.fields.find(faccess.field_name);
                if (fit != it->second.fields.end()) {
                    return fit->second;
                }
            }
        }
        return symbols.type_auto;
    }

    if (std::holds_alternative<IndexAccess>(payload)) {
        const auto& iaccess = std::get<IndexAccess>(payload);
        const TypeId target_tid = infer_expr_type(arena, iaccess.target, symbols, scope, current_module_id);
        if (target_tid < arena.types.size()) {
            const auto& tnode = arena.types[target_tid];
            if (tnode.name == "list" && !tnode.type_args.empty()) {
                return tnode.type_args[0];
            }
            if (tnode.name == "dict" && tnode.type_args.size() == 2) {
                return tnode.type_args[1];
            }
        }
        return symbols.type_auto;
    }

    if (std::holds_alternative<FuncCall>(payload)) {
        const auto& call = std::get<FuncCall>(payload);
        const auto fn_opt = resolve_func_call(arena, call, symbols, current_module_id, scope);
        if (fn_opt.has_value() && fn_opt->return_type != INVALID_ID) {
            return resolve_alias(arena, symbols, fn_opt->module_id, fn_opt->return_type);
        }
        const auto struct_opt = resolve_struct_constructor(arena, call, symbols, current_module_id, scope);
        if (struct_opt.has_value()) {
            const TypeId raw_tid = find_type(arena, struct_opt->key, {});
            if (raw_tid != INVALID_ID) {
                return raw_tid;
            }
        }
        return symbols.type_void;
    }

    if (std::holds_alternative<UnaryOp>(payload)) {
        const auto& uop = std::get<UnaryOp>(payload);
        if (uop.op == "not") {
            return symbols.type_bool;
        }
        return infer_expr_type(arena, uop.operand, symbols, scope, current_module_id);
    }

    if (std::holds_alternative<BinaryOp>(payload)) {
        const auto& bop = std::get<BinaryOp>(payload);
        if (bop.op == "and" || bop.op == "or" ||
            bop.op == "==" || bop.op == "!=" ||
            bop.op == "<" || bop.op == ">" ||
            bop.op == "<=" || bop.op == ">=") {
            return symbols.type_bool;
        }
        if (bop.op == "&") {
            return symbols.type_str;
        }
        const TypeId left_tid = infer_expr_type(arena, bop.left, symbols, scope, current_module_id);
        const TypeId right_tid = infer_expr_type(arena, bop.right, symbols, scope, current_module_id);
        std::string_view left_name = "";
        if (left_tid < arena.types.size()) {
            left_name = arena.types[left_tid].name;
        }
        std::string_view right_name = "";
        if (right_tid < arena.types.size()) {
            right_name = arena.types[right_tid].name;
        }

        if (left_name == "str" || right_name == "str") {
            return symbols.type_str;
        }
        if (left_name == "float" || right_name == "float") {
            return symbols.type_float;
        }
        return left_tid;
    }

    return symbols.type_auto;
}

struct AnalyzerState {
    std::string file_name = "main.cb";
    std::string module_id = "";
    DiagnosticError error;
    bool has_error = false;
};

inline void set_analyzer_error(
    AnalyzerState& state,
    uint32_t line,
    uint32_t col,
    std::string_view msg
) {
    if (!state.has_error) {
        state.has_error = true;
        state.error = make_error("AnalyzerError", std::string(msg), state.file_name, line, col);
    }
}

inline void check_fallible_usage(
    AnalyzerState& state,
    const AstArena& arena,
    const FnDecl& fn,
    ExprId expr_id,
    bool has_active_on_error,
    const GlobalSymbolTable& symbols,
    bool& out_implicit_bubble
) {
    out_implicit_bubble = false;
    if (expr_id >= arena.exprs.size()) {
        return;
    }
    const auto& expr_node = arena.exprs[expr_id];
    if (std::holds_alternative<FuncCall>(expr_node.payload)) {
        const auto& call = std::get<FuncCall>(expr_node.payload);
        const auto fn_sym_opt = resolve_func_call(arena, call, symbols, state.module_id);
        if (fn_sym_opt.has_value()) {
            const auto& fn_sym = fn_sym_opt.value();
            const bool callee_fallible = fn_sym.is_fallible;
            const bool caller_fallible = fn.is_fallible;
            if (callee_fallible) {
                if (!has_active_on_error) {
                    if (caller_fallible) {
                        out_implicit_bubble = true;
                    } else {
                        std::string msg = "Unhandled error: function returns an error but caller does not declare 'can error' or have an active 'on error' block (function '" +
                            fn.name + "' calls fallible function '" + fn_sym.name + "').";
                        set_analyzer_error(state, expr_node.line, expr_node.col, msg);
                        return;
                    }
                }
            }
        }
    }
}

inline bool is_error_builtin_call(const AstArena& arena, const FuncCall& call);

inline bool stmt_list_has_exit(const AstArena& arena, const std::vector<StmtId>& stmts) {
    if (stmts.empty()) {
        return false;
    }
    const StmtId last_sid = stmts.back();
    if (last_sid >= arena.stmts.size()) {
        return false;
    }
    const auto& last_payload = arena.stmts[last_sid].payload;
    if (std::holds_alternative<ReturnStmt>(last_payload)) {
        return true;
    }
    if (std::holds_alternative<BreakStmt>(last_payload)) {
        return true;
    }
    if (std::holds_alternative<ExprStmt>(last_payload)) {
        const auto& estmt = std::get<ExprStmt>(last_payload);
        if (estmt.expr < arena.exprs.size() && std::holds_alternative<FuncCall>(arena.exprs[estmt.expr].payload)) {
            const auto& call = std::get<FuncCall>(arena.exprs[estmt.expr].payload);
            if (is_error_builtin_call(arena, call)) {
                return true;
            }
        }
    }
    return false;
}

inline bool is_error_builtin_call(const AstArena& arena, const FuncCall& call) {
    if (call.callee >= arena.exprs.size()) {
        return false;
    }
    const auto& callee_node = arena.exprs[call.callee];
    if (std::holds_alternative<Identifier>(callee_node.payload)) {
        return std::get<Identifier>(callee_node.payload).name == "error";
    }
    return false;
}

inline void validate_expr_calls(
    AnalyzerState& state,
    const AstArena& arena,
    const FnDecl& fn,
    ExprId expr_id,
    const GlobalSymbolTable& symbols,
    const std::unordered_map<std::string, VarSymbol>& scope,
    bool allow_top_level_fallible
) {
    if (expr_id >= arena.exprs.size() || state.has_error) {
        return;
    }

    const auto& expr_node = arena.exprs[expr_id];
    const auto& payload = expr_node.payload;

    if (std::holds_alternative<FuncCall>(payload)) {
        const auto& call = std::get<FuncCall>(payload);
        if (is_error_builtin_call(arena, call)) {
            const bool caller_fallible = fn.is_fallible;
            if (!caller_fallible) {
                std::string msg = "Cannot call 'error(...)' inside infallible function '" + fn.name +
                    "'; add 'can error' to the function signature.";
                set_analyzer_error(state, expr_node.line, expr_node.col, msg);
                return;
            }
            if (call.args.empty()) {
                set_analyzer_error(state, expr_node.line, expr_node.col, "'error(...)' requires at least one message argument.");
                return;
            }
            for (const auto& named_arg : call.named_args) {
                if (named_arg.name != "code") {
                    std::string msg = "Unknown named argument '" + named_arg.name +
                        "' in 'error(...)'; only 'code=' is supported.";
                    set_analyzer_error(state, expr_node.line, expr_node.col, msg);
                    return;
                }
            }
        }
        const auto fn_sym_opt = resolve_func_call(arena, call, symbols, state.module_id, scope);
        if (fn_sym_opt.has_value() && fn_sym_opt->is_fallible && !allow_top_level_fallible) {
            if (!fn.is_fallible) {
                std::string msg = "Function '" + fn.name + "' calls fallible function '" + fn_sym_opt->name +
                    "' without an 'on error' block, but '" + fn.name + "' does not declare 'can error' in its signature.";
                set_analyzer_error(state, expr_node.line, expr_node.col, msg);
                return;
            }
        }
        if (call.callee < arena.exprs.size() && std::holds_alternative<FieldAccess>(arena.exprs[call.callee].payload)) {
            const auto& callee_fa = std::get<FieldAccess>(arena.exprs[call.callee].payload);
            if (callee_fa.target < arena.exprs.size()) {
                const auto& tgt_node = arena.exprs[callee_fa.target];
                if (std::holds_alternative<Identifier>(tgt_node.payload)) {
                    const auto& tgt_ident = std::get<Identifier>(tgt_node.payload);
                    std::string target_mod_id;
                    if (is_module_alias_in_scope(symbols, state.module_id, tgt_ident.name, scope, target_mod_id)) {
                        const std::string sym_key = symbol_key(target_mod_id, callee_fa.field_name);
                        const auto fit = symbols.functions.find(sym_key);
                        if (fit != symbols.functions.end() && !fit->second.is_pub) {
                            std::string msg = "Function '" + callee_fa.field_name + "' is private to module '" + target_mod_id + "' (missing 'pub').";
                            set_analyzer_error(state, expr_node.line, expr_node.col, msg);
                            return;
                        }
                        const auto tit = symbols.types.find(sym_key);
                        if (tit != symbols.types.end() && !tit->second.is_pub) {
                            std::string msg = "Type '" + callee_fa.field_name + "' is private to module '" + target_mod_id + "' (missing 'pub').";
                            set_analyzer_error(state, expr_node.line, expr_node.col, msg);
                            return;
                        }
                        const auto ait = symbols.aliases.find(sym_key);
                        if (ait != symbols.aliases.end() && !ait->second.is_pub) {
                            std::string msg = "Alias '" + callee_fa.field_name + "' is private to module '" + target_mod_id + "' (missing 'pub').";
                            set_analyzer_error(state, expr_node.line, expr_node.col, msg);
                            return;
                        }
                    }
                } else {
                    validate_expr_calls(state, arena, fn, callee_fa.target, symbols, scope, false);
                    if (state.has_error) {
                        return;
                    }
                }
            }
        }
        for (const ExprId arg_id : call.args) {
            validate_expr_calls(state, arena, fn, arg_id, symbols, scope, false);
        }
        for (const auto& named_arg : call.named_args) {
            validate_expr_calls(state, arena, fn, named_arg.value, symbols, scope, false);
        }
        return;
    }

    if (std::holds_alternative<BinaryOp>(payload)) {
        const auto& bop = std::get<BinaryOp>(payload);
        validate_expr_calls(state, arena, fn, bop.left, symbols, scope, false);
        validate_expr_calls(state, arena, fn, bop.right, symbols, scope, false);
        return;
    }

    if (std::holds_alternative<UnaryOp>(payload)) {
        const auto& uop = std::get<UnaryOp>(payload);
        validate_expr_calls(state, arena, fn, uop.operand, symbols, scope, false);
        return;
    }

    if (std::holds_alternative<FieldAccess>(payload)) {
        const auto& faccess = std::get<FieldAccess>(payload);
        validate_expr_calls(state, arena, fn, faccess.target, symbols, scope, false);
        return;
    }

    if (std::holds_alternative<IndexAccess>(payload)) {
        const auto& iaccess = std::get<IndexAccess>(payload);
        validate_expr_calls(state, arena, fn, iaccess.target, symbols, scope, false);
        validate_expr_calls(state, arena, fn, iaccess.index, symbols, scope, false);
        return;
    }


    if (std::holds_alternative<ListLiteral>(payload)) {
        const auto& llit = std::get<ListLiteral>(payload);
        for (const ExprId el_id : llit.elements) {
            validate_expr_calls(state, arena, fn, el_id, symbols, scope, false);
        }
        return;
    }

    if (std::holds_alternative<DictLiteral>(payload)) {
        const auto& dlit = std::get<DictLiteral>(payload);
        for (const auto& ent : dlit.entries) {
            validate_expr_calls(state, arena, fn, ent.key, symbols, scope, false);
            validate_expr_calls(state, arena, fn, ent.value, symbols, scope, false);
        }
        return;
    }
}

inline void analyze_block(
    AnalyzerState& state,
    AstArena& arena,
    const FnDecl& fn,
    const std::vector<StmtId>& stmts,
    GlobalSymbolTable& symbols,
    std::unordered_map<std::string, VarSymbol>& scope,
    bool inherited_on_error = false
);

inline void analyze_stmt(
    AnalyzerState& state,
    AstArena& arena,
    const FnDecl& fn,
    StmtId stmt_id,
    GlobalSymbolTable& symbols,
    std::unordered_map<std::string, VarSymbol>& scope,
    bool& has_active_on_error
) {
    if (stmt_id >= arena.stmts.size() || state.has_error) {
        return;
    }

    const auto& stmt_node = arena.stmts[stmt_id];
    const auto& payload = stmt_node.payload;

    if (std::holds_alternative<VarAssignment>(payload)) {
        auto& vassign = std::get<VarAssignment>(arena.stmts[stmt_id].payload);
        check_fallible_usage(state, arena, fn, vassign.value, has_active_on_error, symbols, vassign.implicit_bubble);
        if (state.has_error) return;

        const TypeId val_type = infer_expr_type(arena, vassign.value, symbols, scope, state.module_id);

        if (vassign.is_dim || vassign.is_const) {
            if (vassign.has_declared_type) {
                const TypeId resolved_decl = resolve_alias(arena, symbols, state.module_id, vassign.declared_type);
                if (vassign.target != INVALID_ID) {
                    const ExprId tgt_id = vassign.target;
                    if (tgt_id < arena.exprs.size()) {
                        const auto& tgt_node = arena.exprs[tgt_id];
                        if (std::holds_alternative<Identifier>(tgt_node.payload)) {
                            const auto& ident = std::get<Identifier>(tgt_node.payload);
                            scope[ident.name] = VarSymbol{ident.name, resolved_decl, false, vassign.is_const};
                        }
                    }
                }
            } else {
                if (vassign.target != INVALID_ID) {
                    const ExprId tgt_id = vassign.target;
                    if (tgt_id < arena.exprs.size()) {
                        const auto& tgt_node = arena.exprs[tgt_id];
                        if (std::holds_alternative<Identifier>(tgt_node.payload)) {
                            const auto& ident = std::get<Identifier>(tgt_node.payload);
                            TypeId inferred = (val_type != INVALID_ID) ? val_type : symbols.type_int;
                            scope[ident.name] = VarSymbol{ident.name, inferred, false, vassign.is_const};
                        }
                    }
                }
            }
        } else {
            if (vassign.target != INVALID_ID) {
                const ExprId tgt_id = vassign.target;
                if (tgt_id < arena.exprs.size()) {
                    const auto& tgt_node = arena.exprs[tgt_id];
                    if (std::holds_alternative<Identifier>(tgt_node.payload)) {
                        const auto& ident = std::get<Identifier>(tgt_node.payload);
                        const auto existing = scope.find(ident.name);
                        if (existing == scope.end()) {
                            std::string msg = "Undeclared variable '" + ident.name + "'. Use 'dim' or 'const' to declare variables.";
                            set_analyzer_error(state, tgt_node.line, tgt_node.col, msg);
                            return;
                        }
                        if (existing->second.is_const) {
                            std::string msg = "Cannot reassign const variable '" + ident.name + "'.";
                            set_analyzer_error(state, tgt_node.line, tgt_node.col, msg);
                            return;
                        }
                    }
                }
            }
        }

        validate_expr_calls(state, arena, fn, vassign.value, symbols, scope, true);
        return;
    }

    if (std::holds_alternative<ExprStmt>(payload)) {
        auto& estmt = std::get<ExprStmt>(arena.stmts[stmt_id].payload);
        check_fallible_usage(state, arena, fn, estmt.expr, has_active_on_error, symbols, estmt.implicit_bubble);
        if (state.has_error) return;

        validate_expr_calls(state, arena, fn, estmt.expr, symbols, scope, true);
        return;
    }

    if (std::holds_alternative<ErrorBlock>(payload)) {
        const auto& err_block = std::get<ErrorBlock>(payload);
        auto err_scope = scope;
        TypeId err_t = symbols.type_err;
        if (err_t == INVALID_ID) {
            err_t = add_type(arena, TypeNode{stmt_node.line, stmt_node.col, "err", {}});
        }
        err_scope[err_block.err_var] = VarSymbol{err_block.err_var, err_t, false, false};
        analyze_block(state, arena, fn, err_block.body, symbols, err_scope, false);
        if (state.has_error) return;

        if (!stmt_list_has_exit(arena, err_block.body)) {
            std::string msg = "'on error' block must terminate with an explicit control-flow exit ('return', 'error(...)', or 'break').";
            set_analyzer_error(state, stmt_node.line, stmt_node.col, msg);
            return;
        }

        has_active_on_error = true;
        return;
    }

    if (std::holds_alternative<IfStmt>(payload)) {
        const auto& if_stmt = std::get<IfStmt>(payload);
        validate_expr_calls(state, arena, fn, if_stmt.condition, symbols, scope, false);
        if (state.has_error) return;
        analyze_block(state, arena, fn, if_stmt.then_body, symbols, scope, has_active_on_error);
        if (state.has_error) return;

        for (const auto& branch : if_stmt.elif_branches) {
            validate_expr_calls(state, arena, fn, branch.condition, symbols, scope, false);
            if (state.has_error) return;
            analyze_block(state, arena, fn, branch.body, symbols, scope, has_active_on_error);
            if (state.has_error) return;
        }

        if (if_stmt.has_else) {
            analyze_block(state, arena, fn, if_stmt.else_body, symbols, scope, has_active_on_error);
        }
        return;
    }

    if (std::holds_alternative<ForStmt>(payload)) {
        const auto& for_stmt = std::get<ForStmt>(payload);
        if (for_stmt.range_spec.has_range) {
            validate_expr_calls(state, arena, fn, for_stmt.range_spec.start, symbols, scope, false);
            validate_expr_calls(state, arena, fn, for_stmt.range_spec.end, symbols, scope, false);
            if (for_stmt.range_spec.has_step) {
                validate_expr_calls(state, arena, fn, for_stmt.range_spec.step, symbols, scope, false);
            }
            if (!for_stmt.loop_vars.empty()) {
                const TypeId int_t = add_type(arena, TypeNode{stmt_node.line, stmt_node.col, "int", {}});
                scope[for_stmt.loop_vars[0]] = VarSymbol{for_stmt.loop_vars[0], int_t, false};
            }
        } else if (for_stmt.has_iterable) {
            validate_expr_calls(state, arena, fn, for_stmt.iterable, symbols, scope, false);
            const TypeId iter_tid = infer_expr_type(arena, for_stmt.iterable, symbols, scope, state.module_id);
            if (iter_tid < arena.types.size()) {
                const auto& iter_t = arena.types[iter_tid];
                if (iter_t.name == "list" && !iter_t.type_args.empty() && !for_stmt.loop_vars.empty()) {
                    scope[for_stmt.loop_vars[0]] = VarSymbol{for_stmt.loop_vars[0], iter_t.type_args[0], false};
                } else if (iter_t.name == "dict" && iter_t.type_args.size() == 2) {
                    if (for_stmt.loop_vars.size() >= 1) {
                        scope[for_stmt.loop_vars[0]] = VarSymbol{for_stmt.loop_vars[0], iter_t.type_args[0], false};
                    }
                    if (for_stmt.loop_vars.size() >= 2) {
                        scope[for_stmt.loop_vars[1]] = VarSymbol{for_stmt.loop_vars[1], iter_t.type_args[1], false};
                    }
                } else {
                    for (const auto& lv : for_stmt.loop_vars) {
                        const TypeId auto_t = add_type(arena, TypeNode{stmt_node.line, stmt_node.col, "auto", {}});
                        scope[lv] = VarSymbol{lv, auto_t, false};
                    }
                }
            }
        }
        if (state.has_error) return;
        analyze_block(state, arena, fn, for_stmt.body, symbols, scope, has_active_on_error);
        return;
    }

    if (std::holds_alternative<SelectStmt>(payload)) {
        const auto& sel_stmt = std::get<SelectStmt>(payload);
        validate_expr_calls(state, arena, fn, sel_stmt.target, symbols, scope, false);
        if (state.has_error) return;

        const TypeId target_tid = infer_expr_type(arena, sel_stmt.target, symbols, scope, state.module_id);
        if (target_tid != symbols.type_int && target_tid != symbols.type_bool) {
            set_analyzer_error(state, stmt_node.line, stmt_node.col, "select case expression must be of type 'int' or 'bool'");
            return;
        }

        for (const auto& scase : sel_stmt.cases) {
            validate_expr_calls(state, arena, fn, scase.match_expr, symbols, scope, false);
            if (state.has_error) return;
            analyze_block(state, arena, fn, scase.body, symbols, scope, has_active_on_error);
            if (state.has_error) return;
        }

        if (sel_stmt.has_case_else) {
            analyze_block(state, arena, fn, sel_stmt.case_else, symbols, scope, has_active_on_error);
        }
        return;
    }

    if (std::holds_alternative<DeferStmt>(payload)) {
        const auto& dstmt = std::get<DeferStmt>(payload);
        analyze_block(state, arena, fn, dstmt.body, symbols, scope, false);
        return;
    }



    if (std::holds_alternative<ReturnStmt>(payload)) {
        const auto& ret_stmt = std::get<ReturnStmt>(payload);
        if (ret_stmt.has_value) {
            validate_expr_calls(state, arena, fn, ret_stmt.value, symbols, scope, false);
            if (state.has_error) return;
        }
        bool is_return_error_call = false;
        if (ret_stmt.has_value) {
            const ExprId first_id = ret_stmt.value;
            if (first_id < arena.exprs.size() && std::holds_alternative<FuncCall>(arena.exprs[first_id].payload)) {
                const auto& call = std::get<FuncCall>(arena.exprs[first_id].payload);
                if (is_error_builtin_call(arena, call)) {
                    is_return_error_call = true;
                }
            }
        }
        if (!is_return_error_call) {
            const bool expected_has_value = (fn.return_type != INVALID_ID && !is_void_type(arena, fn.return_type));
            if (ret_stmt.has_value && !expected_has_value) {
                std::string msg = "Function '" + fn.name + "' returns void, but a value was returned.";
                set_analyzer_error(state, stmt_node.line, stmt_node.col, msg);
                return;
            } else if (!ret_stmt.has_value && expected_has_value) {
                std::string msg = "Function '" + fn.name + "' expects a return value, but none was provided.";
                set_analyzer_error(state, stmt_node.line, stmt_node.col, msg);
                return;
            }
        }
        return;
    }
}

inline void analyze_block(
    AnalyzerState& state,
    AstArena& arena,
    const FnDecl& fn,
    const std::vector<StmtId>& stmts,
    GlobalSymbolTable& symbols,
    std::unordered_map<std::string, VarSymbol>& scope,
    bool inherited_on_error
) {
    bool active_on_error = inherited_on_error;
    for (const StmtId sid : stmts) {
        if (state.has_error) {
            return;
        }
        analyze_stmt(state, arena, fn, sid, symbols, scope, active_on_error);
    }
}

inline void analyze_fn(
    AnalyzerState& state,
    AstArena& arena,
    const FnDecl& fn,
    GlobalSymbolTable& symbols
) {
    std::unordered_map<std::string, VarSymbol> scope;
    for (const auto& param : fn.params) {
        const TypeId resolved_tid = resolve_alias(arena, symbols, state.module_id, param.type_id);
        scope[param.name] = VarSymbol{param.name, resolved_tid, param.is_byref};
    }

    analyze_block(state, arena, fn, fn.body, symbols, scope, false);
    symbols.fn_scopes[symbol_key(state.module_id, fn.name)] = std::move(scope);
}

inline AnalysisResult analyze_program(
    AstArena& arena,
    const Program& program,
    std::optional<GlobalSymbolTable> initial_symbols = std::nullopt
) {
    AnalysisResult result{};
    result.ok = true;
    if (initial_symbols.has_value()) {
        result.symbols = std::move(initial_symbols.value());
    } else {
        register_builtins(arena, result.symbols);
    }

    AnalyzerState state{};
    state.file_name = program.file_name;

    // Pass 1a: Register module imports, aliases, and type declarations
    for (const DeclId did : program.declarations) {
        if (did >= arena.decls.size()) continue;
        const auto& decl_node = arena.decls[did];
        const auto& payload = decl_node.payload;

        if (std::holds_alternative<ImportDecl>(payload)) {
            const auto& imp = std::get<ImportDecl>(payload);
            if (imp.kind == "header") {
                bool found = false;
                for (const auto& h : result.symbols.headers) {
                    if (h == imp.target) { found = true; break; }
                }
                if (!found) {
                    result.symbols.headers.push_back(imp.target);
                }
            } else if (imp.kind == "link") {
                bool found = false;
                for (const auto& l : result.symbols.link_libs) {
                    if (l == imp.target) { found = true; break; }
                }
                if (!found) {
                    result.symbols.link_libs.push_back(imp.target);
                }
            } else if (imp.kind == "module") {
                std::string alias = default_import_alias(imp.target);
                if (imp.has_alias) {
                    alias = imp.alias;
                }
                if (result.symbols.modules[decl_node.module_id].count(alias) > 0) {
                    result.ok = false;
                    result.error = make_error("AnalyzerError", "Duplicate module import alias '" + alias + "'.", decl_node.file_name, decl_node.line, decl_node.col);
                    return result;
                }
                std::string resolved_mod = imp.resolved_module_id;
                if (resolved_mod.empty()) {
                    resolved_mod = resolve_module_id(decl_node.module_id, imp.target);
                }
                result.symbols.modules[decl_node.module_id][alias] = resolved_mod;
            }
        } else if (std::holds_alternative<AliasDecl>(payload)) {
            const auto& adecl = std::get<AliasDecl>(payload);
            AliasSymbol asym{};
            asym.name = adecl.name;
            asym.module_id = decl_node.module_id;
            asym.key = symbol_key(decl_node.module_id, adecl.name);
            asym.target_type = adecl.target_type;
            asym.is_pub = adecl.is_pub;
            asym.cpp_name = make_cpp_type_name(decl_node.module_id, adecl.name, false);
            result.symbols.aliases[asym.key] = std::move(asym);
        } else if (std::holds_alternative<TypeDecl>(payload)) {
            const auto& tdecl = std::get<TypeDecl>(payload);
            TypeSymbol tsym{};
            tsym.name = tdecl.name;
            tsym.module_id = decl_node.module_id;
            tsym.key = symbol_key(decl_node.module_id, tdecl.name);
            tsym.is_pub = tdecl.is_pub;
            tsym.is_extern = tdecl.is_extern;
            tsym.cpp_name = make_cpp_type_name(decl_node.module_id, tdecl.name, tdecl.is_extern);
            tsym.field_order = tdecl.fields;
            const std::string key_copy = tsym.key;
            result.symbols.types[key_copy] = std::move(tsym);
            // Pre-register canonical struct types in the arena
            find_or_add_type(arena, key_copy, {});
        }
    }

    // Pass 1b: Resolve struct field types and register function signatures now that all types and aliases are registered
    for (const DeclId did : program.declarations) {
        if (did >= arena.decls.size()) continue;
        const auto& decl_node = arena.decls[did];
        const auto& payload = decl_node.payload;

        if (std::holds_alternative<TypeDecl>(payload)) {
            const auto& tdecl = std::get<TypeDecl>(payload);
            const std::string key = symbol_key(decl_node.module_id, tdecl.name);
            auto& tsym = result.symbols.types[key];
            for (const auto& fld : tdecl.fields) {
                tsym.fields[fld.name] = resolve_alias(arena, result.symbols, decl_node.module_id, fld.type_id);
            }
        } else if (std::holds_alternative<FnDecl>(payload)) {
            const auto& fdecl = std::get<FnDecl>(payload);
            FnSymbol fsym{};
            fsym.name = fdecl.name;
            fsym.module_id = decl_node.module_id;
            fsym.key = symbol_key(decl_node.module_id, fdecl.name);
            fsym.is_pub = fdecl.is_pub;
            fsym.is_extern = fdecl.is_extern;
            fsym.is_fallible = fdecl.is_fallible;
            fsym.cpp_name = make_cpp_fn_name(decl_node.module_id, fdecl.name, fdecl.is_extern);
            fsym.params.reserve(fdecl.params.size());
            for (const auto& p : fdecl.params) {
                Param resolved_p = p;
                resolved_p.type_id = resolve_alias(arena, result.symbols, decl_node.module_id, p.type_id);
                fsym.params.push_back(std::move(resolved_p));
            }
            if (fdecl.return_type != INVALID_ID) {
                fsym.return_type = resolve_alias(arena, result.symbols, decl_node.module_id, fdecl.return_type);
            }
            result.symbols.functions[fsym.key] = std::move(fsym);
        }
    }

    // Step 2: Validate function bodies, infer local variable types, and enforce static error rules
    for (const DeclId did : program.declarations) {
        if (did >= arena.decls.size()) continue;
        const auto& decl_node = arena.decls[did];
        if (std::holds_alternative<FnDecl>(decl_node.payload)) {
            const auto& fdecl = std::get<FnDecl>(decl_node.payload);
            if (!fdecl.is_extern) {
                state.file_name = decl_node.file_name;
                state.module_id = decl_node.module_id;
                analyze_fn(state, arena, fdecl, result.symbols);
                if (state.has_error) {
                    result.ok = false;
                    result.error = state.error;
                    return result;
                }
            }
        }
    }
    return result;
}

}  // namespace cb

#endif  // CLEANBASIC_ANALYZER_HPP
