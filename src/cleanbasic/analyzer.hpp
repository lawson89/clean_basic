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
    std::unordered_map<std::string, TypeId> fields;
    std::vector<FieldDecl> field_order;
    bool is_pub = false;
    bool is_extern = false;
};

struct FnSymbol {
    std::string name;
    std::vector<Param> params;
    std::vector<TypeId> return_types;
    bool returns_err = false;
    bool is_fallible = false;
    bool is_pub = false;
    bool is_extern = false;
};

struct VarSymbol {
    std::string name;
    TypeId type_id = INVALID_ID;
    bool is_byref = false;
    bool is_const = false;
};

struct GlobalSymbolTable {
    std::unordered_map<std::string, TypeSymbol> types;
    std::unordered_map<std::string, TypeId> aliases;
    std::unordered_map<std::string, FnSymbol> functions;
    std::vector<std::string> headers;
    std::vector<std::string> link_libs;
    std::unordered_map<std::string, std::string> modules;
    std::unordered_map<std::string, std::unordered_map<std::string, VarSymbol>> fn_scopes;
    TypeId type_err = INVALID_ID;
};

struct AnalysisResult {
    bool ok = true;
    GlobalSymbolTable symbols;
    DiagnosticError error;
};

inline void register_builtins(AstArena& arena, GlobalSymbolTable& symbols) {
    const TypeId str_t = add_type(arena, TypeNode{1, 1, "str", {}, false});
    const TypeId any_t = add_type(arena, TypeNode{1, 1, "any", {}, false});
    const TypeId int_t = add_type(arena, TypeNode{1, 1, "int", {}, false});
    const TypeId err_t = add_type(arena, TypeNode{1, 1, "err", {}, false});
    symbols.type_err = err_t;

    TypeSymbol err_type{};
    err_type.name = "err";
    err_type.is_pub = true;
    err_type.fields["msg"] = str_t;
    err_type.fields["code"] = int_t;
    err_type.field_order.push_back(FieldDecl{1, 1, "msg", str_t});
    err_type.field_order.push_back(FieldDecl{1, 1, "code", int_t});
    symbols.types["err"] = std::move(err_type);

    FnSymbol print_fn{};
    print_fn.name = "print";
    print_fn.returns_err = false;
    symbols.functions["print"] = std::move(print_fn);

    FnSymbol len_fn{};
    len_fn.name = "len";
    len_fn.params.push_back(Param{1, 1, "container", any_t, false});
    len_fn.return_types.push_back(int_t);
    symbols.functions["len"] = std::move(len_fn);

    FnSymbol str_fn{};
    str_fn.name = "str";
    str_fn.params.push_back(Param{1, 1, "val", any_t, false});
    str_fn.return_types.push_back(str_t);
    symbols.functions["str"] = std::move(str_fn);
}

inline TypeId resolve_alias(
    AstArena& arena,
    const GlobalSymbolTable& symbols,
    TypeId tid
) {
    if (tid >= arena.types.size()) {
        return tid;
    }

    std::unordered_set<std::string> seen;
    TypeNode curr = arena.types[tid];

    while (!curr.name.empty() && symbols.aliases.find(curr.name) != symbols.aliases.end()) {
        if (seen.find(curr.name) != seen.end()) {
            break;
        }
        seen.insert(curr.name);
        const auto it = symbols.aliases.find(curr.name);
        if (it == symbols.aliases.end()) {
            break;
        }
        const TypeId aliased_id = it->second;
        if (aliased_id >= arena.types.size()) {
            break;
        }
        const TypeNode& aliased = arena.types[aliased_id];

        std::vector<TypeId> new_args;
        new_args.reserve(aliased.type_args.size());
        for (const TypeId arg_id : aliased.type_args) {
            new_args.push_back(resolve_alias(arena, symbols, arg_id));
        }

        TypeNode next_node{};
        next_node.line = curr.line;
        next_node.col = curr.col;
        next_node.name = aliased.name;
        next_node.type_args = std::move(new_args);
        next_node.is_shared = curr.is_shared || aliased.is_shared;
        curr = std::move(next_node);
    }

    if (!curr.type_args.empty()) {
        std::vector<TypeId> resolved_args;
        resolved_args.reserve(curr.type_args.size());
        for (const TypeId arg_id : curr.type_args) {
            resolved_args.push_back(resolve_alias(arena, symbols, arg_id));
        }
        curr.type_args = std::move(resolved_args);
    }

    return add_type(arena, std::move(curr));
}

inline std::optional<FnSymbol> resolve_func_call(
    const AstArena& arena,
    const FuncCall& call,
    const GlobalSymbolTable& symbols
) {
    if (call.callee >= arena.exprs.size()) {
        return std::nullopt;
    }
    const auto& callee_node = arena.exprs[call.callee];

    if (std::holds_alternative<Identifier>(callee_node.payload)) {
        const auto& ident = std::get<Identifier>(callee_node.payload);
        const auto it = symbols.functions.find(ident.name);
        if (it != symbols.functions.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    if (std::holds_alternative<FieldAccess>(callee_node.payload)) {
        const auto& access = std::get<FieldAccess>(callee_node.payload);
        const auto it = symbols.functions.find(access.field_name);
        if (it != symbols.functions.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    return std::nullopt;
}

inline std::optional<TypeSymbol> resolve_struct_constructor(
    const AstArena& arena,
    const FuncCall& call,
    const GlobalSymbolTable& symbols
) {
    if (call.callee >= arena.exprs.size()) {
        return std::nullopt;
    }
    const auto& callee_node = arena.exprs[call.callee];

    if (std::holds_alternative<Identifier>(callee_node.payload)) {
        const auto& ident = std::get<Identifier>(callee_node.payload);
        if (symbols.functions.find(ident.name) != symbols.functions.end()) {
            return std::nullopt;
        }
        const auto it = symbols.types.find(ident.name);
        if (it != symbols.types.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    if (std::holds_alternative<FieldAccess>(callee_node.payload)) {
        const auto& access = std::get<FieldAccess>(callee_node.payload);
        if (symbols.functions.find(access.field_name) != symbols.functions.end()) {
            return std::nullopt;
        }
        const auto it = symbols.types.find(access.field_name);
        if (it != symbols.types.end()) {
            return it->second;
        }
        if (access.target < arena.exprs.size() &&
            std::holds_alternative<Identifier>(arena.exprs[access.target].payload)) {
            const auto& mod_ident = std::get<Identifier>(arena.exprs[access.target].payload);
            if (symbols.modules.find(mod_ident.name) != symbols.modules.end()) {
                TypeSymbol external_struct{};
                external_struct.name = access.field_name;
                external_struct.is_pub = true;
                return external_struct;
            }
        }
        return std::nullopt;
    }

    return std::nullopt;
}

inline TypeId infer_expr_type(
    AstArena& arena,
    ExprId expr_id,
    const GlobalSymbolTable& symbols,
    const std::unordered_map<std::string, VarSymbol>& scope
);

inline std::vector<TypeId> infer_expr_types(
    AstArena& arena,
    ExprId expr_id,
    const GlobalSymbolTable& symbols,
    const std::unordered_map<std::string, VarSymbol>& scope
) {
    if (expr_id < arena.exprs.size()) {
        const auto& expr_node = arena.exprs[expr_id];
        if (std::holds_alternative<FuncCall>(expr_node.payload)) {
            const auto& call = std::get<FuncCall>(expr_node.payload);
            const auto fn_opt = resolve_func_call(arena, call, symbols);
            if (fn_opt.has_value() && !fn_opt->return_types.empty()) {
                std::vector<TypeId> out_types;
                out_types.reserve(fn_opt->return_types.size());
                for (const TypeId rtid : fn_opt->return_types) {
                    out_types.push_back(resolve_alias(arena, symbols, rtid));
                }
                return out_types;
            }
        }
    }
    return {infer_expr_type(arena, expr_id, symbols, scope)};
}

inline TypeId infer_expr_type(
    AstArena& arena,
    ExprId expr_id,
    const GlobalSymbolTable& symbols,
    const std::unordered_map<std::string, VarSymbol>& scope
) {
    if (expr_id >= arena.exprs.size()) {
        return add_type(arena, TypeNode{1, 1, "auto", {}, false});
    }

    const auto& expr_node = arena.exprs[expr_id];
    const auto& payload = expr_node.payload;

    if (std::holds_alternative<NumberLiteral>(payload)) {
        const auto& lit = std::get<NumberLiteral>(payload);
        return add_type(arena, TypeNode{expr_node.line, expr_node.col, lit.is_float ? "float" : "int", {}, false});
    }

    if (std::holds_alternative<StringLiteral>(payload)) {
        return add_type(arena, TypeNode{expr_node.line, expr_node.col, "str", {}, false});
    }

    if (std::holds_alternative<BoolLiteral>(payload)) {
        return add_type(arena, TypeNode{expr_node.line, expr_node.col, "bool", {}, false});
    }

    if (std::holds_alternative<NullLiteral>(payload)) {
        return add_type(arena, TypeNode{expr_node.line, expr_node.col, "null", {}, false});
    }

    if (std::holds_alternative<Identifier>(payload)) {
        const auto& ident = std::get<Identifier>(payload);
        const auto it = scope.find(ident.name);
        if (it != scope.end()) {
            return it->second.type_id;
        }
        return add_type(arena, TypeNode{expr_node.line, expr_node.col, "auto", {}, false});
    }

    if (std::holds_alternative<NewExpr>(payload)) {
        const auto& nexpr = std::get<NewExpr>(payload);
        const TypeId resolved_tid = resolve_alias(arena, symbols, nexpr.target_type);
        TypeNode res = (resolved_tid < arena.types.size()) ? arena.types[resolved_tid] : TypeNode{};
        res.line = expr_node.line;
        res.col = expr_node.col;
        res.is_shared = true;
        return add_type(arena, std::move(res));
    }

    if (std::holds_alternative<StructInit>(payload)) {
        const auto& sinit = std::get<StructInit>(payload);
        const TypeId raw_tid = add_type(arena, TypeNode{expr_node.line, expr_node.col, sinit.type_name, {}, false});
        return resolve_alias(arena, symbols, raw_tid);
    }

    if (std::holds_alternative<ListLiteral>(payload)) {
        const auto& llit = std::get<ListLiteral>(payload);
        TypeId elem_type = INVALID_ID;
        if (!llit.elements.empty()) {
            elem_type = infer_expr_type(arena, llit.elements[0], symbols, scope);
        } else {
            elem_type = add_type(arena, TypeNode{expr_node.line, expr_node.col, "int", {}, false});
        }
        return add_type(arena, TypeNode{expr_node.line, expr_node.col, "list", {elem_type}, false});
    }

    if (std::holds_alternative<DictLiteral>(payload)) {
        const auto& dlit = std::get<DictLiteral>(payload);
        TypeId k_type = INVALID_ID;
        TypeId v_type = INVALID_ID;
        if (!dlit.entries.empty()) {
            k_type = infer_expr_type(arena, dlit.entries[0].key, symbols, scope);
            v_type = infer_expr_type(arena, dlit.entries[0].value, symbols, scope);
        } else {
            k_type = add_type(arena, TypeNode{expr_node.line, expr_node.col, "str", {}, false});
            v_type = add_type(arena, TypeNode{expr_node.line, expr_node.col, "int", {}, false});
        }
        return add_type(arena, TypeNode{expr_node.line, expr_node.col, "dict", {k_type, v_type}, false});
    }

    if (std::holds_alternative<FieldAccess>(payload)) {
        const auto& faccess = std::get<FieldAccess>(payload);
        const TypeId target_tid = infer_expr_type(arena, faccess.target, symbols, scope);
        if (target_tid < arena.types.size()) {
            const std::string& tname = arena.types[target_tid].name;
            const size_t dot_idx = tname.rfind('.');
            const std::string base_name = (dot_idx != std::string::npos) ? tname.substr(dot_idx + 1) : tname;
            const auto it = symbols.types.find(base_name);
            if (it != symbols.types.end()) {
                const auto fit = it->second.fields.find(faccess.field_name);
                if (fit != it->second.fields.end()) {
                    return fit->second;
                }
            }
        }
        return add_type(arena, TypeNode{expr_node.line, expr_node.col, "auto", {}, false});
    }

    if (std::holds_alternative<IndexAccess>(payload)) {
        const auto& iaccess = std::get<IndexAccess>(payload);
        const TypeId target_tid = infer_expr_type(arena, iaccess.target, symbols, scope);
        if (target_tid < arena.types.size()) {
            const auto& tnode = arena.types[target_tid];
            if (tnode.name == "list" && !tnode.type_args.empty()) {
                return tnode.type_args[0];
            }
            if (tnode.name == "dict" && tnode.type_args.size() == 2) {
                return tnode.type_args[1];
            }
        }
        return add_type(arena, TypeNode{expr_node.line, expr_node.col, "auto", {}, false});
    }

    if (std::holds_alternative<FuncCall>(payload)) {
        const auto& call = std::get<FuncCall>(payload);
        const auto fn_opt = resolve_func_call(arena, call, symbols);
        if (fn_opt.has_value() && !fn_opt->return_types.empty()) {
            return resolve_alias(arena, symbols, fn_opt->return_types[0]);
        }
        const auto struct_opt = resolve_struct_constructor(arena, call, symbols);
        if (struct_opt.has_value()) {
            const TypeId raw_tid = add_type(arena, TypeNode{expr_node.line, expr_node.col, struct_opt->name, {}, false});
            return resolve_alias(arena, symbols, raw_tid);
        }
        return add_type(arena, TypeNode{expr_node.line, expr_node.col, "void", {}, false});
    }

    if (std::holds_alternative<UnaryOp>(payload)) {
        const auto& uop = std::get<UnaryOp>(payload);
        if (uop.op == "not") {
            return add_type(arena, TypeNode{expr_node.line, expr_node.col, "bool", {}, false});
        }
        return infer_expr_type(arena, uop.operand, symbols, scope);
    }

    if (std::holds_alternative<BinaryOp>(payload)) {
        const auto& bop = std::get<BinaryOp>(payload);
        if (bop.op == "and" || bop.op == "or" ||
            bop.op == "==" || bop.op == "!=" ||
            bop.op == "<" || bop.op == ">" ||
            bop.op == "<=" || bop.op == ">=") {
            return add_type(arena, TypeNode{expr_node.line, expr_node.col, "bool", {}, false});
        }
        if (bop.op == "&") {
            return add_type(arena, TypeNode{expr_node.line, expr_node.col, "str", {}, false});
        }
        const TypeId left_tid = infer_expr_type(arena, bop.left, symbols, scope);
        const TypeId right_tid = infer_expr_type(arena, bop.right, symbols, scope);
        const std::string_view left_name = (left_tid < arena.types.size()) ? arena.types[left_tid].name : "";
        const std::string_view right_name = (right_tid < arena.types.size()) ? arena.types[right_tid].name : "";

        if (left_name == "str" || right_name == "str") {
            return add_type(arena, TypeNode{expr_node.line, expr_node.col, "str", {}, false});
        }
        if (left_name == "float" || right_name == "float") {
            return add_type(arena, TypeNode{expr_node.line, expr_node.col, "float", {}, false});
        }
        return left_tid;
    }

    return add_type(arena, TypeNode{expr_node.line, expr_node.col, "auto", {}, false});
}

struct AnalyzerState {
    std::string file_name = "main.cb";
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
        const auto fn_sym_opt = resolve_func_call(arena, call, symbols);
        if (fn_sym_opt.has_value()) {
            const auto& fn_sym = fn_sym_opt.value();
            const bool callee_fallible = fn_sym.returns_err || fn_sym.is_fallible;
            const bool caller_fallible = fn.returns_err || fn.is_fallible;
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
            const bool caller_fallible = fn.returns_err || fn.is_fallible;
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
        const auto fn_sym_opt = resolve_func_call(arena, call, symbols);
        if (fn_sym_opt.has_value() && fn_sym_opt->returns_err && !allow_top_level_fallible) {
            if (!fn.returns_err) {
                std::string msg = "Function '" + fn.name + "' calls fallible function '" + fn_sym_opt->name +
                    "' without an 'on error' block, but '" + fn.name + "' does not declare 'can error' in its signature.";
                set_analyzer_error(state, expr_node.line, expr_node.col, msg);
                return;
            }
        }
        for (const ExprId arg_id : call.args) {
            validate_expr_calls(state, arena, fn, arg_id, symbols, false);
        }
        for (const auto& named_arg : call.named_args) {
            validate_expr_calls(state, arena, fn, named_arg.value, symbols, false);
        }
        return;
    }

    if (std::holds_alternative<BinaryOp>(payload)) {
        const auto& bop = std::get<BinaryOp>(payload);
        validate_expr_calls(state, arena, fn, bop.left, symbols, false);
        validate_expr_calls(state, arena, fn, bop.right, symbols, false);
        return;
    }

    if (std::holds_alternative<UnaryOp>(payload)) {
        const auto& uop = std::get<UnaryOp>(payload);
        validate_expr_calls(state, arena, fn, uop.operand, symbols, false);
        return;
    }

    if (std::holds_alternative<FieldAccess>(payload)) {
        const auto& faccess = std::get<FieldAccess>(payload);
        validate_expr_calls(state, arena, fn, faccess.target, symbols, false);
        return;
    }

    if (std::holds_alternative<IndexAccess>(payload)) {
        const auto& iaccess = std::get<IndexAccess>(payload);
        validate_expr_calls(state, arena, fn, iaccess.target, symbols, false);
        validate_expr_calls(state, arena, fn, iaccess.index, symbols, false);
        return;
    }

    if (std::holds_alternative<StructInit>(payload)) {
        const auto& sinit = std::get<StructInit>(payload);
        for (const auto& fld : sinit.fields) {
            validate_expr_calls(state, arena, fn, fld.value, symbols, false);
        }
        return;
    }

    if (std::holds_alternative<NewExpr>(payload)) {
        const auto& nexpr = std::get<NewExpr>(payload);
        for (const auto& arg : nexpr.args) {
            validate_expr_calls(state, arena, fn, arg.value, symbols, false);
        }
        return;
    }

    if (std::holds_alternative<ListLiteral>(payload)) {
        const auto& llit = std::get<ListLiteral>(payload);
        for (const ExprId el_id : llit.elements) {
            validate_expr_calls(state, arena, fn, el_id, symbols, false);
        }
        return;
    }

    if (std::holds_alternative<DictLiteral>(payload)) {
        const auto& dlit = std::get<DictLiteral>(payload);
        for (const auto& ent : dlit.entries) {
            validate_expr_calls(state, arena, fn, ent.key, symbols, false);
            validate_expr_calls(state, arena, fn, ent.value, symbols, false);
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

        const std::vector<TypeId> val_types = infer_expr_types(arena, vassign.value, symbols, scope);

        if (vassign.is_dim || vassign.is_const) {
            if (vassign.has_declared_type) {
                const TypeId resolved_decl = resolve_alias(arena, symbols, vassign.declared_type);
                if (!vassign.targets.empty()) {
                    const ExprId tgt_id = vassign.targets[0];
                    if (tgt_id < arena.exprs.size()) {
                        const auto& tgt_node = arena.exprs[tgt_id];
                        if (std::holds_alternative<Identifier>(tgt_node.payload)) {
                            const auto& ident = std::get<Identifier>(tgt_node.payload);
                            scope[ident.name] = VarSymbol{ident.name, resolved_decl, false, vassign.is_const};
                        }
                    }
                }
            } else {
                for (size_t idx = 0; idx < vassign.targets.size(); ++idx) {
                    const ExprId tgt_id = vassign.targets[idx];
                    if (tgt_id < arena.exprs.size()) {
                        const auto& tgt_node = arena.exprs[tgt_id];
                        if (std::holds_alternative<Identifier>(tgt_node.payload)) {
                            const auto& ident = std::get<Identifier>(tgt_node.payload);
                            TypeId inferred = INVALID_ID;
                            if (idx < val_types.size()) {
                                inferred = val_types[idx];
                            } else {
                                inferred = add_type(arena, TypeNode{stmt_node.line, stmt_node.col, "int", {}, false});
                            }
                            scope[ident.name] = VarSymbol{ident.name, inferred, false, vassign.is_const};
                        }
                    }
                }
            }
        } else {
            for (size_t idx = 0; idx < vassign.targets.size(); ++idx) {
                const ExprId tgt_id = vassign.targets[idx];
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

        validate_expr_calls(state, arena, fn, vassign.value, symbols, true);
        return;
    }

    if (std::holds_alternative<ExprStmt>(payload)) {
        auto& estmt = std::get<ExprStmt>(arena.stmts[stmt_id].payload);
        check_fallible_usage(state, arena, fn, estmt.expr, has_active_on_error, symbols, estmt.implicit_bubble);
        if (state.has_error) return;

        validate_expr_calls(state, arena, fn, estmt.expr, symbols, true);
        return;
    }

    if (std::holds_alternative<ErrorBlock>(payload)) {
        const auto& err_block = std::get<ErrorBlock>(payload);
        auto err_scope = scope;
        TypeId err_t = symbols.type_err;
        if (err_t == INVALID_ID) {
            err_t = add_type(arena, TypeNode{stmt_node.line, stmt_node.col, "err", {}, false});
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
        validate_expr_calls(state, arena, fn, if_stmt.condition, symbols, false);
        if (state.has_error) return;
        analyze_block(state, arena, fn, if_stmt.then_body, symbols, scope, has_active_on_error);
        if (state.has_error) return;

        for (const auto& branch : if_stmt.elif_branches) {
            validate_expr_calls(state, arena, fn, branch.condition, symbols, false);
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
            validate_expr_calls(state, arena, fn, for_stmt.range_spec.start, symbols, false);
            validate_expr_calls(state, arena, fn, for_stmt.range_spec.end, symbols, false);
            if (for_stmt.range_spec.has_step) {
                validate_expr_calls(state, arena, fn, for_stmt.range_spec.step, symbols, false);
            }
            if (!for_stmt.loop_vars.empty()) {
                const TypeId int_t = add_type(arena, TypeNode{stmt_node.line, stmt_node.col, "int", {}, false});
                scope[for_stmt.loop_vars[0]] = VarSymbol{for_stmt.loop_vars[0], int_t, false};
            }
        } else if (for_stmt.has_iterable) {
            validate_expr_calls(state, arena, fn, for_stmt.iterable, symbols, false);
            const TypeId iter_tid = infer_expr_type(arena, for_stmt.iterable, symbols, scope);
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
                        const TypeId auto_t = add_type(arena, TypeNode{stmt_node.line, stmt_node.col, "auto", {}, false});
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
        validate_expr_calls(state, arena, fn, sel_stmt.target, symbols, false);
        if (state.has_error) return;

        for (const auto& scase : sel_stmt.cases) {
            validate_expr_calls(state, arena, fn, scase.match_expr, symbols, false);
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

    if (std::holds_alternative<SpawnStmt>(payload)) {
        const auto& sstmt = std::get<SpawnStmt>(payload);
        validate_expr_calls(state, arena, fn, sstmt.call_expr, symbols, false);
        return;
    }

    if (std::holds_alternative<ReturnStmt>(payload)) {
        const auto& ret_stmt = std::get<ReturnStmt>(payload);
        for (const ExprId val_id : ret_stmt.values) {
            validate_expr_calls(state, arena, fn, val_id, symbols, false);
            if (state.has_error) return;
        }
        bool is_return_error_call = false;
        if (ret_stmt.values.size() == 1) {
            const ExprId first_id = ret_stmt.values[0];
            if (first_id < arena.exprs.size() && std::holds_alternative<FuncCall>(arena.exprs[first_id].payload)) {
                const auto& call = std::get<FuncCall>(arena.exprs[first_id].payload);
                if (is_error_builtin_call(arena, call)) {
                    is_return_error_call = true;
                }
            }
        }
        if (!is_return_error_call && !ret_stmt.values.empty()) {
            const size_t expected_count = fn.return_types.size();
            if (ret_stmt.values.size() != expected_count) {
                std::string msg = "Function '" + fn.name + "' expects " +
                    std::to_string(expected_count) + " return value(s), got " +
                    std::to_string(ret_stmt.values.size());
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
        const TypeId resolved_tid = resolve_alias(arena, symbols, param.type_id);
        scope[param.name] = VarSymbol{param.name, resolved_tid, param.is_byref};
    }

    analyze_block(state, arena, fn, fn.body, symbols, scope, false);
    symbols.fn_scopes[fn.name] = std::move(scope);
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

    // Step 1: Register top-level imports, aliases, types, and function signatures
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
                std::string alias;
                if (imp.has_alias) {
                    alias = imp.alias;
                } else {
                    alias = imp.target;
                    const size_t slash_idx = alias.find_last_of('/');
                    if (slash_idx != std::string::npos) {
                        alias = alias.substr(slash_idx + 1);
                    }
                    if (alias.size() > 3 && alias.substr(alias.size() - 3) == ".cb") {
                        alias = alias.substr(0, alias.size() - 3);
                    } else {
                        const size_t dot_idx = alias.rfind('.');
                        if (dot_idx != std::string::npos) {
                            alias = alias.substr(dot_idx + 1);
                        }
                    }
                }
                result.symbols.modules[alias] = imp.target;
            }
        } else if (std::holds_alternative<AliasDecl>(payload)) {
            const auto& adecl = std::get<AliasDecl>(payload);
            result.symbols.aliases[adecl.name] = adecl.target_type;
        } else if (std::holds_alternative<TypeDecl>(payload)) {
            const auto& tdecl = std::get<TypeDecl>(payload);
            TypeSymbol tsym{};
            tsym.name = tdecl.name;
            tsym.is_pub = tdecl.is_pub;
            tsym.is_extern = tdecl.is_extern;
            tsym.field_order = tdecl.fields;
            for (const auto& fld : tdecl.fields) {
                tsym.fields[fld.name] = resolve_alias(arena, result.symbols, fld.type_id);
            }
            result.symbols.types[tdecl.name] = std::move(tsym);
        } else if (std::holds_alternative<FnDecl>(payload)) {
            const auto& fdecl = std::get<FnDecl>(payload);
            FnSymbol fsym{};
            fsym.name = fdecl.name;
            fsym.is_pub = fdecl.is_pub;
            fsym.is_extern = fdecl.is_extern;
            fsym.returns_err = fdecl.returns_err || fdecl.is_fallible;
            fsym.is_fallible = fdecl.returns_err || fdecl.is_fallible;
            fsym.params.reserve(fdecl.params.size());
            for (const auto& p : fdecl.params) {
                Param resolved_p = p;
                resolved_p.type_id = resolve_alias(arena, result.symbols, p.type_id);
                fsym.params.push_back(std::move(resolved_p));
            }
            fsym.return_types.reserve(fdecl.return_types.size());
            for (const TypeId rtid : fdecl.return_types) {
                fsym.return_types.push_back(resolve_alias(arena, result.symbols, rtid));
            }
            result.symbols.functions[fdecl.name] = std::move(fsym);
        }
    }

    // Step 2: Validate function bodies, infer local variable types, and enforce static error rules
    for (const DeclId did : program.declarations) {
        if (did >= arena.decls.size()) continue;
        const auto& decl_node = arena.decls[did];
        if (std::holds_alternative<FnDecl>(decl_node.payload)) {
            const auto& fdecl = std::get<FnDecl>(decl_node.payload);
            if (!fdecl.is_extern) {
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
