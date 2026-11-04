#ifndef CLEANBASIC_EMITTER_HPP
#define CLEANBASIC_EMITTER_HPP

#include "src/cleanbasic/analyzer.hpp"
#include "src/cleanbasic/ast.hpp"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace cb {

constexpr std::string_view CPP_RUNTIME_PREAMBLE = R"(#include <cstdint>
#include <iostream>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <tuple>
#include <utility>
#include <functional>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>

struct cb_error {
    std::string msg = "";
    int64_t code = 0;
};

inline std::string cb_to_str(const std::string& val) { return val; }
inline std::string cb_to_str(int val) { return std::to_string(val); }
inline std::string cb_to_str(int64_t val) { return std::to_string(val); }
inline std::string cb_to_str(double val) { return std::to_string(val); }
inline std::string cb_to_str(bool val) {
    if (val) {
        return "true";
    }
    return "false";
}
inline std::string cb_to_str(const cb_error& val) { return val.msg; }
inline bool cb_has_error(const cb_error& val) { return !val.msg.empty() || val.code != 0; }

template <typename F> struct _DeferTask { F f; ~_DeferTask() { f(); } };
template <typename F> _DeferTask<F> _defer(F f) { return {f}; }

template <typename T>
struct Channel {
    std::queue<T> queue_;
    std::mutex mutex_;
    std::condition_variable cv_;

    void send(T val) {
        std::unique_lock<std::mutex> lock(mutex_);
        queue_.push(std::move(val));
        cv_.notify_one();
    }
    T recv() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return !queue_.empty(); });
        T val = std::move(queue_.front());
        queue_.pop();
        return val;
    }
};
)";

struct EmitterState {
    std::vector<std::string> lines;
    int indent_level = 0;
    uint32_t temp_counter = 0;
    uint32_t val_counter = 0;
    uint32_t defer_counter = 0;
    std::optional<ErrorBlock> active_error_block = std::nullopt;
};

inline void emit_line(EmitterState& state, std::string_view line = "") {
    if (line.empty()) {
        state.lines.emplace_back("");
    } else {
        std::string res;
        res.reserve(static_cast<size_t>(state.indent_level * 4) + line.size());
        for (int i = 0; i < state.indent_level; ++i) {
            res.append("    ");
        }
        res.append(line);
        state.lines.push_back(std::move(res));
    }
}

inline std::string next_temp(EmitterState& state, std::string_view prefix = "_tmp") {
    state.temp_counter += 1;
    return std::string(prefix) + "_" + std::to_string(state.temp_counter);
}

inline std::string map_type(
    AstArena& arena,
    const GlobalSymbolTable& symbols,
    TypeId tid,
    bool is_byref = false
);

inline std::string map_base_type(
    AstArena& arena,
    const GlobalSymbolTable& symbols,
    const TypeNode& t
) {
    if (t.name == "int") return "int";
    if (t.name == "float") return "double";
    if (t.name == "bool") return "bool";
    if (t.name == "str") return "std::string";
    if (t.name == "err") return "cb_error";
    if (t.name == "void") return "void";
    if (t.name == "auto") return "auto";
    if (t.name == "list") {
        const std::string elem = !t.type_args.empty() ? map_type(arena, symbols, t.type_args[0]) : "int";
        return "std::vector<" + elem + ">";
    }
    if (t.name == "dict") {
        const std::string k = (t.type_args.size() > 0) ? map_type(arena, symbols, t.type_args[0]) : "std::string";
        const std::string v = (t.type_args.size() > 1) ? map_type(arena, symbols, t.type_args[1]) : "int";
        return "std::unordered_map<" + k + ", " + v + ">";
    }
    if (t.name == "chan") {
        const std::string elem = !t.type_args.empty() ? map_type(arena, symbols, t.type_args[0]) : "int";
        return "std::shared_ptr<Channel<" + elem + ">>";
    }
    const size_t dot_idx = t.name.rfind('.');
    if (dot_idx != std::string::npos) {
        const std::string mod_prefix = t.name.substr(0, dot_idx);
        const std::string base_name = t.name.substr(dot_idx + 1);
        if (symbols.types.find(base_name) != symbols.types.end() ||
            symbols.modules.find(mod_prefix) != symbols.modules.end()) {
            return base_name;
        }
    }
    std::string sanitized = t.name;
    for (char& c : sanitized) {
        if (c == '.') c = '_';
    }
    return sanitized;
}

inline std::string map_type(
    AstArena& arena,
    const GlobalSymbolTable& symbols,
    TypeId tid,
    bool is_byref
) {
    const TypeId resolved_id = resolve_alias(arena, symbols, tid);
    const TypeNode& resolved = (resolved_id < arena.types.size()) ? arena.types[resolved_id] : TypeNode{};
    const std::string base_cpp = map_base_type(arena, symbols, resolved);
    std::string cpp = resolved.is_shared ? ("std::shared_ptr<" + base_cpp + ">") : base_cpp;
    if (is_byref) {
        cpp += "&";
    }
    return cpp;
}

inline std::string default_value_for_type(
    AstArena& arena,
    const GlobalSymbolTable& symbols,
    TypeId tid
) {
    const TypeId resolved_id = resolve_alias(arena, symbols, tid);
    const TypeNode& resolved = (resolved_id < arena.types.size()) ? arena.types[resolved_id] : TypeNode{};
    if (resolved.is_shared) {
        return "nullptr";
    }
    if (resolved.name == "int") return "0";
    if (resolved.name == "float") return "0.0";
    if (resolved.name == "bool") return "false";
    if (resolved.name == "str") return "std::string(\"\")";
    return map_type(arena, symbols, resolved_id) + "{}";
}

inline std::string cpp_fn_name(std::string_view name) {
    if (name == "main") return "cb_main";
    return std::string(name);
}

inline std::string fn_return_cpp_type(
    AstArena& arena,
    const GlobalSymbolTable& symbols,
    const FnDecl& fn
) {
    std::vector<std::string> ret_cpp;
    ret_cpp.reserve(fn.return_types.size() + (fn.returns_err ? 1 : 0));
    for (const TypeId rtid : fn.return_types) {
        ret_cpp.push_back(map_type(arena, symbols, rtid));
    }
    if (fn.returns_err) {
        ret_cpp.push_back("cb_error");
    }
    if (ret_cpp.empty()) {
        return "void";
    }
    if (ret_cpp.size() == 1) {
        return ret_cpp[0];
    }
    std::string joined;
    for (size_t i = 0; i < ret_cpp.size(); ++i) {
        if (i > 0) joined += ", ";
        joined += ret_cpp[i];
    }
    return "std::tuple<" + joined + ">";
}

inline std::string fn_signature(
    AstArena& arena,
    const GlobalSymbolTable& symbols,
    const FnDecl& fn
) {
    const std::string ret_str = fn_return_cpp_type(arena, symbols, fn);
    std::string params_str;
    for (size_t i = 0; i < fn.params.size(); ++i) {
        if (i > 0) params_str += ", ";
        const auto& p = fn.params[i];
        params_str += map_type(arena, symbols, p.type_id, p.is_byref);
        params_str += " ";
        params_str += p.name;
    }
    return ret_str + " " + cpp_fn_name(fn.name) + "(" + params_str + ")";
}

inline std::string emit_expr(
    AstArena& arena,
    const GlobalSymbolTable& symbols,
    ExprId expr_id,
    const std::unordered_map<std::string, VarSymbol>& scope,
    EmitterState& state
);

inline std::string emit_fstring(
    AstArena& arena,
    std::string_view raw_content,
    const std::unordered_map<std::string, VarSymbol>& scope
) {
    std::vector<std::string> cpp_parts;
    size_t i = 0;
    while (i < raw_content.size()) {
        const size_t open_brace = raw_content.find('{', i);
        if (open_brace == std::string_view::npos) {
            std::string literal_part(raw_content.substr(i));
            if (!literal_part.empty()) {
                cpp_parts.push_back("std::string(\"" + literal_part + "\")");
            }
            break;
        }
        if (open_brace > i) {
            std::string literal_part(raw_content.substr(i, open_brace - i));
            cpp_parts.push_back("std::string(\"" + literal_part + "\")");
        }
        const size_t close_brace = raw_content.find('}', open_brace);
        if (close_brace == std::string_view::npos) {
            std::string rest(raw_content.substr(open_brace));
            cpp_parts.push_back("std::string(\"" + rest + "\")");
            break;
        }
        const std::string expr_str(raw_content.substr(open_brace + 1, close_brace - open_brace - 1));
        const size_t start = expr_str.find_first_not_of(" \t\r\n");
        const size_t end = expr_str.find_last_not_of(" \t\r\n");
        const std::string var_name = (start != std::string::npos) ? expr_str.substr(start, end - start + 1) : "";

        const auto it = scope.find(var_name);
        bool is_str = false;
        if (it != scope.end()) {
            const TypeId tid = it->second.type_id;
            if (tid < arena.types.size() && arena.types[tid].name == "str") {
                is_str = true;
            }
        }
        if (is_str) {
            cpp_parts.push_back(var_name);
        } else {
            cpp_parts.push_back("std::to_string(" + var_name + ")");
        }
        i = close_brace + 1;
    }
    if (cpp_parts.empty()) {
        return "std::string(\"\")";
    }
    std::string result;
    for (size_t p = 0; p < cpp_parts.size(); ++p) {
        if (p > 0) result += " + ";
        result += cpp_parts[p];
    }
    return result;
}

inline std::vector<std::string> order_struct_fields(
    AstArena& arena,
    const GlobalSymbolTable& symbols,
    std::string_view type_name,
    const std::vector<InitField>& provided_fields,
    const std::unordered_map<std::string, VarSymbol>& scope,
    EmitterState& state
) {
    const size_t dot_idx = type_name.rfind('.');
    const std::string base_name = (dot_idx != std::string_view::npos) ? std::string(type_name.substr(dot_idx + 1)) : std::string(type_name);

    bool has_named = false;
    for (const auto& f : provided_fields) {
        if (f.has_name) {
            has_named = true;
            break;
        }
    }

    const auto it = symbols.types.find(base_name);
    if (!has_named || it == symbols.types.end() || it->second.field_order.empty()) {
        std::vector<std::string> rendered;
        rendered.reserve(provided_fields.size());
        for (const auto& f : provided_fields) {
            rendered.push_back(emit_expr(arena, symbols, f.value, scope, state));
        }
        return rendered;
    }

    std::unordered_map<std::string, ExprId> named_map;
    for (const auto& f : provided_fields) {
        if (f.has_name) {
            named_map[f.name] = f.value;
        }
    }

    std::vector<std::string> ordered;
    ordered.reserve(it->second.field_order.size());
    for (const auto& fdecl : it->second.field_order) {
        const auto fit = named_map.find(fdecl.name);
        if (fit != named_map.end()) {
            ordered.push_back(emit_expr(arena, symbols, fit->second, scope, state));
        } else {
            ordered.push_back(default_value_for_type(arena, symbols, fdecl.type_id));
        }
    }
    return ordered;
}

inline std::vector<std::string> emit_call_args_with_autoderef(
    AstArena& arena,
    const GlobalSymbolTable& symbols,
    const std::vector<ExprId>& pos_args,
    const std::vector<NamedArg>& named_args,
    const std::optional<FnSymbol>& fn_sym,
    const std::unordered_map<std::string, VarSymbol>& scope,
    EmitterState& state
) {
    std::vector<ExprId> all_exprs = pos_args;
    all_exprs.reserve(pos_args.size() + named_args.size());
    for (const auto& na : named_args) {
        all_exprs.push_back(na.value);
    }

    std::vector<std::string> rendered;
    rendered.reserve(all_exprs.size());

    for (size_t idx = 0; idx < all_exprs.size(); ++idx) {
        const ExprId arg_expr_id = all_exprs[idx];
        std::string arg_cpp = emit_expr(arena, symbols, arg_expr_id, scope, state);
        const TypeId arg_tid = infer_expr_type(arena, arg_expr_id, symbols, scope);
        const TypeNode& arg_type = (arg_tid < arena.types.size()) ? arena.types[arg_tid] : TypeNode{};

        if (fn_sym.has_value() && idx < fn_sym->params.size()) {
            const Param& param = fn_sym->params[idx];
            const TypeId param_tid = resolve_alias(arena, symbols, param.type_id);
            const TypeNode& param_type = (param_tid < arena.types.size()) ? arena.types[param_tid] : TypeNode{};

            if (arg_type.is_shared && !param_type.is_shared) {
                arg_cpp = "(*" + arg_cpp + ")";
            }
        }
        rendered.push_back(std::move(arg_cpp));
    }
    return rendered;
}

inline std::string emit_expr(
    AstArena& arena,
    const GlobalSymbolTable& symbols,
    ExprId expr_id,
    const std::unordered_map<std::string, VarSymbol>& scope,
    EmitterState& state
) {
    if (expr_id >= arena.exprs.size()) {
        return "";
    }

    const auto& expr_node = arena.exprs[expr_id];
    const auto& payload = expr_node.payload;

    if (std::holds_alternative<NumberLiteral>(payload)) {
        return std::get<NumberLiteral>(payload).raw;
    }

    if (std::holds_alternative<StringLiteral>(payload)) {
        const auto& slit = std::get<StringLiteral>(payload);
        if (slit.is_fstring) {
            return emit_fstring(arena, slit.value, scope);
        }
        return "std::string(\"" + slit.value + "\")";
    }

    if (std::holds_alternative<BoolLiteral>(payload)) {
        if (std::get<BoolLiteral>(payload).value) {
            return "true";
        }
        return "false";
    }

    if (std::holds_alternative<NullLiteral>(payload)) {
        return "{}";
    }

    if (std::holds_alternative<Identifier>(payload)) {
        return std::get<Identifier>(payload).name;
    }

    if (std::holds_alternative<UnaryOp>(payload)) {
        const auto& uop = std::get<UnaryOp>(payload);
        const std::string op_str = (uop.op == "not") ? "!" : uop.op;
        return "(" + op_str + emit_expr(arena, symbols, uop.operand, scope, state) + ")";
    }

    if (std::holds_alternative<BinaryOp>(payload)) {
        const auto& bop = std::get<BinaryOp>(payload);
        if (bop.op == "&") {
            const TypeId left_tid = infer_expr_type(arena, bop.left, symbols, scope);
            const TypeId right_tid = infer_expr_type(arena, bop.right, symbols, scope);
            std::string left_cpp = emit_expr(arena, symbols, bop.left, scope, state);
            std::string right_cpp = emit_expr(arena, symbols, bop.right, scope, state);
            bool left_is_str = false;
            if (left_tid < arena.types.size() && arena.types[left_tid].name == "str") {
                left_is_str = true;
            }
            bool right_is_str = false;
            if (right_tid < arena.types.size() && arena.types[right_tid].name == "str") {
                right_is_str = true;
            }
            if (!left_is_str) {
                left_cpp = "cb_to_str(" + left_cpp + ")";
            }
            if (!right_is_str) {
                right_cpp = "cb_to_str(" + right_cpp + ")";
            }
            return "(" + left_cpp + " + " + right_cpp + ")";
        }
        std::string op_str = bop.op;
        if (op_str == "and") {
            op_str = "&&";
        } else if (op_str == "or") {
            op_str = "||";
        }
        const std::string left_cpp = emit_expr(arena, symbols, bop.left, scope, state);
        const std::string right_cpp = emit_expr(arena, symbols, bop.right, scope, state);
        return "(" + left_cpp + " " + op_str + " " + right_cpp + ")";
    }

    if (std::holds_alternative<FieldAccess>(payload)) {
        const auto& faccess = std::get<FieldAccess>(payload);
        const TypeId target_tid = infer_expr_type(arena, faccess.target, symbols, scope);
        const bool is_shared = (target_tid < arena.types.size()) && arena.types[target_tid].is_shared;
        const std::string target_cpp = emit_expr(arena, symbols, faccess.target, scope, state);
        return target_cpp + (is_shared ? "->" : ".") + faccess.field_name;
    }

    if (std::holds_alternative<IndexAccess>(payload)) {
        const auto& iaccess = std::get<IndexAccess>(payload);
        const TypeId target_tid = infer_expr_type(arena, iaccess.target, symbols, scope);
        const bool is_shared = (target_tid < arena.types.size()) && arena.types[target_tid].is_shared;
        const std::string target_cpp = emit_expr(arena, symbols, iaccess.target, scope, state);
        const std::string idx_cpp = emit_expr(arena, symbols, iaccess.index, scope, state);
        if (is_shared) {
            return "(*" + target_cpp + ")[" + idx_cpp + "]";
        }
        return target_cpp + "[" + idx_cpp + "]";
    }

    if (std::holds_alternative<StructInit>(payload)) {
        const auto& sinit = std::get<StructInit>(payload);
        const TypeId raw_tid = add_type(arena, TypeNode{expr_node.line, expr_node.col, sinit.type_name, {}, false});
        const TypeId resolved_t = resolve_alias(arena, symbols, raw_tid);
        const std::string cpp_type = map_type(arena, symbols, resolved_t);
        const std::string type_name_for_fields = (resolved_t < arena.types.size()) ? arena.types[resolved_t].name : sinit.type_name;
        const auto ordered = order_struct_fields(arena, symbols, type_name_for_fields, sinit.fields, scope, state);
        std::string joined;
        for (size_t i = 0; i < ordered.size(); ++i) {
            if (i > 0) joined += ", ";
            joined += ordered[i];
        }
        return cpp_type + "{" + joined + "}";
    }

    if (std::holds_alternative<NewExpr>(payload)) {
        const auto& nexpr = std::get<NewExpr>(payload);
        const TypeId resolved_t = resolve_alias(arena, symbols, nexpr.target_type);
        const TypeNode& res_node = (resolved_t < arena.types.size()) ? arena.types[resolved_t] : TypeNode{};
        const std::string base_cpp = map_base_type(arena, symbols, res_node);

        if (res_node.name == "chan") {
            const std::string elem_cpp = !res_node.type_args.empty() ? map_type(arena, symbols, res_node.type_args[0]) : "int";
            return "std::make_shared<Channel<" + elem_cpp + ">>()";
        }

        if (nexpr.args.empty()) {
            return "std::make_shared<" + base_cpp + ">()";
        }

        std::string lookup_type_name = res_node.name;
        const size_t dot_idx = lookup_type_name.rfind('.');
        if (dot_idx != std::string::npos) {
            lookup_type_name = lookup_type_name.substr(dot_idx + 1);
        }
        const auto it = symbols.types.find(lookup_type_name);
        if (it != symbols.types.end() && !it->second.field_order.empty()) {
            const auto ordered = order_struct_fields(arena, symbols, lookup_type_name, nexpr.args, scope, state);
            std::string joined;
            for (size_t i = 0; i < ordered.size(); ++i) {
                if (i > 0) joined += ", ";
                joined += ordered[i];
            }
            return "std::make_shared<" + base_cpp + ">(" + base_cpp + "{" + joined + "})";
        }

        std::string joined;
        for (size_t i = 0; i < nexpr.args.size(); ++i) {
            if (i > 0) joined += ", ";
            joined += emit_expr(arena, symbols, nexpr.args[i].value, scope, state);
        }
        return "std::make_shared<" + base_cpp + ">(" + joined + ")";
    }

    if (std::holds_alternative<ListLiteral>(payload)) {
        const auto& llit = std::get<ListLiteral>(payload);
        std::string joined;
        for (size_t i = 0; i < llit.elements.size(); ++i) {
            if (i > 0) joined += ", ";
            joined += emit_expr(arena, symbols, llit.elements[i], scope, state);
        }
        return "{" + joined + "}";
    }

    if (std::holds_alternative<DictLiteral>(payload)) {
        const auto& dlit = std::get<DictLiteral>(payload);
        std::string joined;
        for (size_t i = 0; i < dlit.entries.size(); ++i) {
            if (i > 0) joined += ", ";
            const std::string k = emit_expr(arena, symbols, dlit.entries[i].key, scope, state);
            const std::string v = emit_expr(arena, symbols, dlit.entries[i].value, scope, state);
            joined += "{" + k + ", " + v + "}";
        }
        return "{" + joined + "}";
    }

    if (std::holds_alternative<FuncCall>(payload)) {
        const auto& call = std::get<FuncCall>(payload);
        if (call.callee >= arena.exprs.size()) {
            return "";
        }
        const auto struct_opt = resolve_struct_constructor(arena, call, symbols);
        if (struct_opt.has_value()) {
            std::vector<InitField> init_fields;
            init_fields.reserve(call.args.size() + call.named_args.size());
            for (const ExprId pos_arg : call.args) {
                InitField f{};
                f.has_name = false;
                f.value = pos_arg;
                init_fields.push_back(std::move(f));
            }
            for (const auto& na : call.named_args) {
                InitField f{};
                f.has_name = true;
                f.name = na.name;
                f.value = na.value;
                init_fields.push_back(std::move(f));
            }
            const auto ordered = order_struct_fields(arena, symbols, struct_opt->name, init_fields, scope, state);
            std::string joined;
            for (size_t i = 0; i < ordered.size(); ++i) {
                if (i > 0) joined += ", ";
                joined += ordered[i];
            }
            return struct_opt->name + "{" + joined + "}";
        }
        const auto& callee_node = arena.exprs[call.callee];

        if (std::holds_alternative<Identifier>(callee_node.payload)) {
            const std::string& fn_name = std::get<Identifier>(callee_node.payload).name;

            if (fn_name == "len" && call.args.size() == 1) {
                const ExprId arg_expr = call.args[0];
                const TypeId arg_type_id = infer_expr_type(arena, arg_expr, symbols, scope);
                const bool is_shared = (arg_type_id < arena.types.size()) && arena.types[arg_type_id].is_shared;
                const std::string arg_cpp = emit_expr(arena, symbols, arg_expr, scope, state);
                if (is_shared) {
                    return "static_cast<int>(" + arg_cpp + "->size())";
                }
                return "static_cast<int>(" + arg_cpp + ".size())";
            }

            if (fn_name == "str" && call.args.size() == 1) {
                const ExprId arg_expr = call.args[0];
                const TypeId arg_type_id = infer_expr_type(arena, arg_expr, symbols, scope);
                const std::string arg_cpp = emit_expr(arena, symbols, arg_expr, scope, state);
                if (arg_type_id < arena.types.size() && arena.types[arg_type_id].name == "str") {
                    return arg_cpp;
                }
                if (arg_type_id < arena.types.size() && arena.types[arg_type_id].name == "err") {
                    return "cb_to_str(" + arg_cpp + ")";
                }
                return "std::to_string(" + arg_cpp + ")";
            }

            if (fn_name == "print") {
                if (call.args.empty()) {
                    return "(std::cout << std::endl)";
                }
                std::string stream_expr = "(std::cout";
                for (const ExprId arg_id : call.args) {
                    const TypeId arg_tid = infer_expr_type(arena, arg_id, symbols, scope);
                    std::string arg_cpp = emit_expr(arena, symbols, arg_id, scope, state);
                    if (arg_tid < arena.types.size() &&
                        (arena.types[arg_tid].name == "bool" || arena.types[arg_tid].name == "err")) {
                        arg_cpp = "cb_to_str(" + arg_cpp + ")";
                    }
                    stream_expr += " << " + arg_cpp;
                }
                stream_expr += " << std::endl)";
                return stream_expr;
            }

            std::optional<FnSymbol> fn_sym;
            const auto it = symbols.functions.find(fn_name);
            if (it != symbols.functions.end()) {
                fn_sym = it->second;
            }
            const auto rendered_args = emit_call_args_with_autoderef(
                arena, symbols, call.args, call.named_args, fn_sym, scope, state
            );
            std::string joined;
            for (size_t i = 0; i < rendered_args.size(); ++i) {
                if (i > 0) joined += ", ";
                joined += rendered_args[i];
            }
            return cpp_fn_name(fn_name) + "(" + joined + ")";
        }

        if (std::holds_alternative<FieldAccess>(callee_node.payload)) {
            const auto& faccess = std::get<FieldAccess>(callee_node.payload);
            const ExprId receiver = faccess.target;
            const std::string& method_name = faccess.field_name;
            const TypeId recv_tid = infer_expr_type(arena, receiver, symbols, scope);
            const TypeNode& recv_type = (recv_tid < arena.types.size()) ? arena.types[recv_tid] : TypeNode{};
            const std::string recv_cpp = emit_expr(arena, symbols, receiver, scope, state);
            const std::string accessor = recv_type.is_shared ? "->" : ".";

            if (recv_type.name == "list" && (method_name == "push" || method_name == "append") && !call.args.empty()) {
                const std::string arg_cpp = emit_expr(arena, symbols, call.args[0], scope, state);
                return recv_cpp + accessor + "push_back(" + arg_cpp + ")";
            }

            if (recv_type.name == "chan" && (method_name == "send" || method_name == "recv")) {
                std::string joined;
                for (size_t i = 0; i < call.args.size(); ++i) {
                    if (i > 0) joined += ", ";
                    joined += emit_expr(arena, symbols, call.args[i], scope, state);
                }
                return recv_cpp + "->" + method_name + "(" + joined + ")";
            }

            const auto it = symbols.functions.find(method_name);
            if (it != symbols.functions.end()) {
                std::vector<ExprId> all_pos;
                all_pos.reserve(1 + call.args.size());
                all_pos.push_back(receiver);
                for (const ExprId a : call.args) {
                    all_pos.push_back(a);
                }
                const auto rendered_args = emit_call_args_with_autoderef(
                    arena, symbols, all_pos, call.named_args, it->second, scope, state
                );
                std::string joined;
                for (size_t i = 0; i < rendered_args.size(); ++i) {
                    if (i > 0) joined += ", ";
                    joined += rendered_args[i];
                }
                return cpp_fn_name(method_name) + "(" + joined + ")";
            }

            std::string joined;
            for (size_t i = 0; i < call.args.size(); ++i) {
                if (i > 0) joined += ", ";
                joined += emit_expr(arena, symbols, call.args[i], scope, state);
            }
            return recv_cpp + accessor + method_name + "(" + joined + ")";
        }

        const std::string callee_cpp = emit_expr(arena, symbols, call.callee, scope, state);
        std::string joined;
        for (size_t i = 0; i < call.args.size(); ++i) {
            if (i > 0) joined += ", ";
            joined += emit_expr(arena, symbols, call.args[i], scope, state);
        }
        return callee_cpp + "(" + joined + ")";
    }

    return "";
}

inline std::string build_error_call_value(
    AstArena& arena,
    const GlobalSymbolTable& symbols,
    const FuncCall& call,
    const std::unordered_map<std::string, VarSymbol>& scope,
    EmitterState& state
) {
    std::string msg_cpp = R"(std::string(""))";
    if (call.args.size() == 1) {
        const ExprId arg_id = call.args[0];
        const TypeId arg_tid = infer_expr_type(arena, arg_id, symbols, scope);
        std::string part = emit_expr(arena, symbols, arg_id, scope, state);
        bool is_str = false;
        if (arg_tid < arena.types.size() && arena.types[arg_tid].name == "str") {
            is_str = true;
        }
        if (!is_str) {
            part = "cb_to_str(" + part + ")";
        }
        msg_cpp = part;
    } else if (call.args.size() > 1) {
        std::string combined = "(";
        for (size_t i = 0; i < call.args.size(); ++i) {
            if (i > 0) {
                combined += " + ";
            }
            const ExprId arg_id = call.args[i];
            const TypeId arg_tid = infer_expr_type(arena, arg_id, symbols, scope);
            std::string part = emit_expr(arena, symbols, arg_id, scope, state);
            bool is_str = false;
            if (arg_tid < arena.types.size() && arena.types[arg_tid].name == "str") {
                is_str = true;
            }
            if (!is_str) {
                part = "cb_to_str(" + part + ")";
            }
            combined += part;
        }
        combined += ")";
        msg_cpp = combined;
    }

    std::string code_cpp = "1";
    for (const auto& na : call.named_args) {
        if (na.name == "code") {
            code_cpp = emit_expr(arena, symbols, na.value, scope, state);
        }
    }
    return "cb_error{" + msg_cpp + ", " + code_cpp + "}";
}

inline std::string build_error_bubble_return(
    AstArena& arena,
    const GlobalSymbolTable& symbols,
    const FnDecl& fn,
    std::string_view err_var
) {
    if (fn.return_types.empty()) {
        return "return " + std::string(err_var) + ";";
    }
    std::string args;
    for (size_t i = 0; i < fn.return_types.size(); ++i) {
        if (i > 0) {
            args += ", ";
        }
        const TypeId resolved_id = resolve_alias(arena, symbols, fn.return_types[i]);
        std::string elem = "{}";
        if (resolved_id < arena.types.size()) {
            const TypeNode& tnode = arena.types[resolved_id];
            if (!tnode.is_shared && symbols.types.find(tnode.name) != symbols.types.end()) {
                elem = map_type(arena, symbols, resolved_id) + "{}";
            }
        }
        args += elem;
    }
    args += ", " + std::string(err_var);
    return "return { " + args + " };";
}

inline void emit_stmt(
    AstArena& arena,
    const GlobalSymbolTable& symbols,
    const FnDecl& fn,
    StmtId stmt_id,
    std::unordered_map<std::string, VarSymbol>& scope,
    std::unordered_set<std::string>& declared_vars,
    EmitterState& state
);

inline void emit_stmt_list(
    AstArena& arena,
    const GlobalSymbolTable& symbols,
    const FnDecl& fn,
    const std::vector<StmtId>& stmts,
    std::unordered_map<std::string, VarSymbol>& scope,
    std::unordered_set<std::string>& declared_vars,
    EmitterState& state
) {
    const std::optional<ErrorBlock> saved_error_block = state.active_error_block;
    for (const StmtId sid : stmts) {
        emit_stmt(arena, symbols, fn, sid, scope, declared_vars, state);
    }
    state.active_error_block = saved_error_block;
}

inline void emit_active_error_handler_or_bubble(
    AstArena& arena,
    const GlobalSymbolTable& symbols,
    const FnDecl& fn,
    const std::string& err_tmp,
    std::unordered_map<std::string, VarSymbol>& scope,
    std::unordered_set<std::string>& declared_vars,
    EmitterState& state
) {
    if (state.active_error_block.has_value()) {
        const ErrorBlock active_eb = state.active_error_block.value();
        emit_line(state, "if (cb_has_error(" + err_tmp + ")) {");
        state.indent_level += 1;
        emit_line(state, "cb_error " + active_eb.err_var + " = " + err_tmp + ";");
        auto err_scope = scope;
        TypeId err_tid = symbols.type_err;
        if (err_tid == INVALID_ID) {
            err_tid = add_type(arena, TypeNode{1, 1, "err", {}, false});
        }
        err_scope[active_eb.err_var] = VarSymbol{active_eb.err_var, err_tid, false, false};
        auto err_decls = declared_vars;
        err_decls.insert(active_eb.err_var);
        const std::optional<ErrorBlock> prev_eb = state.active_error_block;
        state.active_error_block = std::nullopt;
        emit_stmt_list(arena, symbols, fn, active_eb.body, err_scope, err_decls, state);
        state.active_error_block = prev_eb;
        state.indent_level -= 1;
        emit_line(state, "}");
    } else {
        const std::string early_ret = build_error_bubble_return(arena, symbols, fn, err_tmp);
        emit_line(state, "if (cb_has_error(" + err_tmp + ")) " + early_ret);
    }
}

inline void emit_fallible_assignment(
    AstArena& arena,
    const GlobalSymbolTable& symbols,
    const FnDecl& fn,
    const VarAssignment& stmt,
    const FnSymbol& fn_sym,
    std::unordered_map<std::string, VarSymbol>& scope,
    std::unordered_set<std::string>& declared_vars,
    EmitterState& state
) {
    const std::string call_cpp = emit_expr(arena, symbols, stmt.value, scope, state);
    state.val_counter += 1;
    const uint32_t cid = state.val_counter;

    std::vector<std::string> temp_vals;
    temp_vals.reserve(stmt.targets.size());
    if (stmt.targets.size() == 1) {
        temp_vals.push_back("_val" + std::to_string(cid));
    } else {
        for (size_t idx = 0; idx < stmt.targets.size(); ++idx) {
            temp_vals.push_back("_val" + std::to_string(cid) + "_" + std::to_string(idx + 1));
        }
    }

    const std::string err_var = "_err" + std::to_string(cid);

    std::string binding_vars;
    for (const auto& tv : temp_vals) {
        binding_vars += tv + ", ";
    }
    binding_vars += err_var;

    emit_line(state, "auto [" + binding_vars + "] = " + call_cpp + ";");

    emit_active_error_handler_or_bubble(arena, symbols, fn, err_var, scope, declared_vars, state);

    for (size_t idx = 0; idx < stmt.targets.size(); ++idx) {
        const ExprId tid = stmt.targets[idx];
        const std::string& val_tmp = temp_vals[idx];
        if (tid < arena.exprs.size() && std::holds_alternative<Identifier>(arena.exprs[tid].payload)) {
            const std::string& name = std::get<Identifier>(arena.exprs[tid].payload).name;
            TypeId ret_t = INVALID_ID;
            if (stmt.has_declared_type) {
                ret_t = resolve_alias(arena, symbols, stmt.declared_type);
            } else if (idx < fn_sym.return_types.size()) {
                ret_t = fn_sym.return_types[idx];
            } else {
                ret_t = add_type(arena, TypeNode{1, 1, "auto", {}, false});
            }
            const bool is_new_decl = stmt.is_dim || stmt.is_const || (declared_vars.find(name) == declared_vars.end());
            scope[name] = VarSymbol{name, ret_t, false, stmt.is_const};
            declared_vars.insert(name);
            if (is_new_decl) {
                if (stmt.is_const) {
                    emit_line(state, "const auto " + name + " = " + val_tmp + ";");
                } else {
                    emit_line(state, "auto " + name + " = " + val_tmp + ";");
                }
            } else {
                emit_line(state, name + " = " + val_tmp + ";");
            }
        } else {
            const std::string lhs_cpp = emit_expr(arena, symbols, tid, scope, state);
            emit_line(state, lhs_cpp + " = " + val_tmp + ";");
        }
    }
}

inline void emit_var_assignment(
    AstArena& arena,
    const GlobalSymbolTable& symbols,
    const FnDecl& fn,
    const VarAssignment& stmt,
    std::unordered_map<std::string, VarSymbol>& scope,
    std::unordered_set<std::string>& declared_vars,
    EmitterState& state
) {
    if (stmt.value < arena.exprs.size() && std::holds_alternative<FuncCall>(arena.exprs[stmt.value].payload)) {
        const auto& call = std::get<FuncCall>(arena.exprs[stmt.value].payload);
        const auto fn_sym = resolve_func_call(arena, call, symbols);
        if (fn_sym.has_value() && fn_sym->returns_err) {
            emit_fallible_assignment(arena, symbols, fn, stmt, *fn_sym, scope, declared_vars, state);
            return;
        }
    }

    std::string val_cpp = emit_expr(arena, symbols, stmt.value, scope, state);
    const auto val_types = infer_expr_types(arena, stmt.value, symbols, scope);

    if (stmt.has_declared_type && !stmt.targets.empty()) {
        const ExprId target_id = stmt.targets[0];
        if (target_id < arena.exprs.size() && std::holds_alternative<Identifier>(arena.exprs[target_id].payload)) {
            const std::string& name = std::get<Identifier>(arena.exprs[target_id].payload).name;
            const TypeId resolved_t = resolve_alias(arena, symbols, stmt.declared_type);
            scope[name] = VarSymbol{name, resolved_t, false};
            declared_vars.insert(name);
            const std::string cpp_type = map_type(arena, symbols, resolved_t);
            TypeNode res_node{};
            if (resolved_t < arena.types.size()) {
                res_node = arena.types[resolved_t];
            }
            if (res_node.name == "chan" && stmt.value < arena.exprs.size() && std::holds_alternative<NewExpr>(arena.exprs[stmt.value].payload)) {
                std::string elem_cpp = "int";
                if (!res_node.type_args.empty()) {
                    elem_cpp = map_type(arena, symbols, res_node.type_args[0]);
                }
                val_cpp = "std::make_shared<Channel<" + elem_cpp + ">>()";
            }
            std::string prefix;
            if (stmt.is_const) {
                prefix = "const ";
            }
            emit_line(state, prefix + cpp_type + " " + name + " = " + val_cpp + ";");
            return;
        }
    }

    if (stmt.targets.size() > 1) {
        std::vector<std::string> names;
        bool all_new_idents = true;
        for (const ExprId tid : stmt.targets) {
            if (tid >= arena.exprs.size() || !std::holds_alternative<Identifier>(arena.exprs[tid].payload)) {
                all_new_idents = false;
                break;
            }
            const std::string& name = std::get<Identifier>(arena.exprs[tid].payload).name;
            if (declared_vars.find(name) != declared_vars.end()) {
                all_new_idents = false;
                break;
            }
        }

        names.reserve(stmt.targets.size());
        for (size_t idx = 0; idx < stmt.targets.size(); ++idx) {
            const ExprId tid = stmt.targets[idx];
            if (tid < arena.exprs.size() && std::holds_alternative<Identifier>(arena.exprs[tid].payload)) {
                const std::string& name = std::get<Identifier>(arena.exprs[tid].payload).name;
                const TypeId inferred = (idx < val_types.size()) ? val_types[idx] : add_type(arena, TypeNode{1, 1, "auto", {}, false});
                scope[name] = VarSymbol{name, inferred, false};
                declared_vars.insert(name);
                names.push_back(name);
            } else {
                names.push_back(emit_expr(arena, symbols, tid, scope, state));
            }
        }

        std::string joined;
        for (size_t i = 0; i < names.size(); ++i) {
            if (i > 0) joined += ", ";
            joined += names[i];
        }

        if (all_new_idents) {
            emit_line(state, "auto [" + joined + "] = " + val_cpp + ";");
        } else {
            emit_line(state, "std::tie(" + joined + ") = " + val_cpp + ";");
        }
        return;
    }

    if (!stmt.targets.empty()) {
        const ExprId target_id = stmt.targets[0];
        const TypeId inferred_t = !val_types.empty() ? val_types[0] : add_type(arena, TypeNode{1, 1, "auto", {}, false});

        if (target_id < arena.exprs.size() && std::holds_alternative<Identifier>(arena.exprs[target_id].payload)) {
            const std::string& name = std::get<Identifier>(arena.exprs[target_id].payload).name;
            if (stmt.is_dim || stmt.is_const || declared_vars.find(name) == declared_vars.end()) {
                scope[name] = VarSymbol{name, inferred_t, false, stmt.is_const};
                declared_vars.insert(name);
                std::string inferred_name = "auto";
                if (inferred_t < arena.types.size()) {
                    inferred_name = arena.types[inferred_t].name;
                }
                std::string cpp_t = "auto";
                if (inferred_name != "auto") {
                    cpp_t = map_type(arena, symbols, inferred_t);
                }
                std::string prefix;
                if (stmt.is_const) {
                    prefix = "const ";
                }
                emit_line(state, prefix + cpp_t + " " + name + " = " + val_cpp + ";");
            } else {
                emit_line(state, name + " = " + val_cpp + ";");
            }
        } else {
            const std::string lhs_cpp = emit_expr(arena, symbols, target_id, scope, state);
            emit_line(state, lhs_cpp + " = " + val_cpp + ";");
        }
    }
}

inline void emit_expr_stmt(
    AstArena& arena,
    const GlobalSymbolTable& symbols,
    const FnDecl& fn,
    const ExprStmt& stmt,
    std::unordered_map<std::string, VarSymbol>& scope,
    std::unordered_set<std::string>& declared_vars,
    EmitterState& state
) {
    if (stmt.expr < arena.exprs.size() && std::holds_alternative<FuncCall>(arena.exprs[stmt.expr].payload)) {
        const auto& call = std::get<FuncCall>(arena.exprs[stmt.expr].payload);
        if (is_error_builtin_call(arena, call)) {
            const std::string err_val = build_error_call_value(arena, symbols, call, scope, state);
            emit_line(state, build_error_bubble_return(arena, symbols, fn, err_val));
            return;
        }
        const auto fn_sym = resolve_func_call(arena, call, symbols);
        if (fn_sym.has_value() && fn_sym->returns_err) {
            const std::string call_cpp = emit_expr(arena, symbols, stmt.expr, scope, state);
            state.val_counter += 1;
            const std::string err_var = "_err" + std::to_string(state.val_counter);

            if (!fn_sym->return_types.empty()) {
                std::string bindings;
                for (size_t i = 0; i < fn_sym->return_types.size(); ++i) {
                    bindings += next_temp(state, "_discard") + ", ";
                }
                bindings += err_var;
                emit_line(state, "auto [" + bindings + "] = " + call_cpp + ";");
            } else {
                emit_line(state, "cb_error " + err_var + " = " + call_cpp + ";");
            }

            emit_active_error_handler_or_bubble(arena, symbols, fn, err_var, scope, declared_vars, state);
            return;
        }
    }

    const std::string expr_cpp = emit_expr(arena, symbols, stmt.expr, scope, state);
    emit_line(state, expr_cpp + ";");
}

inline void emit_return(
    AstArena& arena,
    const GlobalSymbolTable& symbols,
    const FnDecl& fn,
    const ReturnStmt& stmt,
    const std::unordered_map<std::string, VarSymbol>& scope,
    EmitterState& state
) {
    if (stmt.values.empty()) {
        if (fn.returns_err && fn.return_types.empty()) {
            emit_line(state, R"(return cb_error{"", 0};)");
        } else {
            emit_line(state, "return;");
        }
        return;
    }

    if (stmt.values.size() == 1) {
        const ExprId first_id = stmt.values[0];
        if (first_id < arena.exprs.size() && std::holds_alternative<FuncCall>(arena.exprs[first_id].payload)) {
            const auto& call = std::get<FuncCall>(arena.exprs[first_id].payload);
            if (is_error_builtin_call(arena, call)) {
                const std::string err_val = build_error_call_value(arena, symbols, call, scope, state);
                emit_line(state, build_error_bubble_return(arena, symbols, fn, err_val));
                return;
            }
        }
    }

    std::vector<std::string> rendered_vals;
    rendered_vals.reserve(stmt.values.size() + 1);

    for (size_t idx = 0; idx < stmt.values.size(); ++idx) {
        const ExprId vid = stmt.values[idx];
        std::string val_cpp = emit_expr(arena, symbols, vid, scope, state);
        if (vid < arena.exprs.size() && std::holds_alternative<NullLiteral>(arena.exprs[vid].payload) &&
            idx < fn.return_types.size()) {
            const TypeId resolved_id = resolve_alias(arena, symbols, fn.return_types[idx]);
            if (resolved_id < arena.types.size()) {
                const TypeNode& tnode = arena.types[resolved_id];
                if (!tnode.is_shared && symbols.types.find(tnode.name) != symbols.types.end()) {
                    val_cpp = map_type(arena, symbols, resolved_id) + "{}";
                }
            }
        }
        rendered_vals.push_back(std::move(val_cpp));
    }

    if (fn.returns_err && rendered_vals.size() == fn.return_types.size()) {
        rendered_vals.push_back(R"(cb_error{"", 0})");
    }

    if (rendered_vals.size() == 1) {
        emit_line(state, "return " + rendered_vals[0] + ";");
    } else {
        std::string joined;
        for (size_t i = 0; i < rendered_vals.size(); ++i) {
            if (i > 0) joined += ", ";
            joined += rendered_vals[i];
        }
        emit_line(state, "return { " + joined + " };");
    }
}

inline void emit_stmt(
    AstArena& arena,
    const GlobalSymbolTable& symbols,
    const FnDecl& fn,
    StmtId stmt_id,
    std::unordered_map<std::string, VarSymbol>& scope,
    std::unordered_set<std::string>& declared_vars,
    EmitterState& state
) {
    if (stmt_id >= arena.stmts.size()) {
        return;
    }

    const auto& stmt_node = arena.stmts[stmt_id];
    const auto& payload = stmt_node.payload;

    if (std::holds_alternative<VarAssignment>(payload)) {
        emit_var_assignment(arena, symbols, fn, std::get<VarAssignment>(payload), scope, declared_vars, state);
        return;
    }

    if (std::holds_alternative<ExprStmt>(payload)) {
        emit_expr_stmt(arena, symbols, fn, std::get<ExprStmt>(payload), scope, declared_vars, state);
        return;
    }

    if (std::holds_alternative<IfStmt>(payload)) {
        const auto& if_s = std::get<IfStmt>(payload);
        const std::string cond_cpp = emit_expr(arena, symbols, if_s.condition, scope, state);
        emit_line(state, "if (" + cond_cpp + ") {");
        state.indent_level += 1;
        auto then_decls = declared_vars;
        emit_stmt_list(arena, symbols, fn, if_s.then_body, scope, then_decls, state);
        state.indent_level -= 1;

        for (const auto& branch : if_s.elif_branches) {
            const std::string elif_cond = emit_expr(arena, symbols, branch.condition, scope, state);
            emit_line(state, "} else if (" + elif_cond + ") {");
            state.indent_level += 1;
            auto elif_decls = declared_vars;
            emit_stmt_list(arena, symbols, fn, branch.body, scope, elif_decls, state);
            state.indent_level -= 1;
        }

        if (if_s.has_else) {
            emit_line(state, "} else {");
            state.indent_level += 1;
            auto else_decls = declared_vars;
            emit_stmt_list(arena, symbols, fn, if_s.else_body, scope, else_decls, state);
            state.indent_level -= 1;
        }

        emit_line(state, "}");
        return;
    }

    if (std::holds_alternative<ForStmt>(payload)) {
        const auto& for_s = std::get<ForStmt>(payload);
        if (for_s.range_spec.has_range && !for_s.loop_vars.empty()) {
            const std::string& loop_var = for_s.loop_vars[0];
            const std::string start_cpp = emit_expr(arena, symbols, for_s.range_spec.start, scope, state);
            const std::string end_cpp = emit_expr(arena, symbols, for_s.range_spec.end, scope, state);
            std::string step_cpp = "1";
            if (for_s.range_spec.has_step) {
                step_cpp = emit_expr(arena, symbols, for_s.range_spec.step, scope, state);
            }
            std::string cmp_op = "<";
            if (for_s.range_spec.inclusive) {
                cmp_op = "<=";
            }

            auto loop_scope = scope;
            const TypeId int_tid = add_type(arena, TypeNode{stmt_node.line, stmt_node.col, "int", {}, false});
            loop_scope[loop_var] = VarSymbol{loop_var, int_tid, false};
            auto loop_decls = declared_vars;
            loop_decls.insert(loop_var);

            emit_line(state, "for (int " + loop_var + " = " + start_cpp + "; " + loop_var + " " + cmp_op + " " + end_cpp + "; " + loop_var + " += " + step_cpp + ") {");
            state.indent_level += 1;
            emit_stmt_list(arena, symbols, fn, for_s.body, loop_scope, loop_decls, state);
            state.indent_level -= 1;
            emit_line(state, "}");
            return;
        }

        if (for_s.has_iterable) {
            std::string iter_cpp = emit_expr(arena, symbols, for_s.iterable, scope, state);
            const TypeId iter_tid = infer_expr_type(arena, for_s.iterable, symbols, scope);
            const TypeNode& iter_type = (iter_tid < arena.types.size()) ? arena.types[iter_tid] : TypeNode{};
            if (iter_type.is_shared) {
                iter_cpp = "(*" + iter_cpp + ")";
            }

            auto loop_scope = scope;
            auto loop_decls = declared_vars;
            for (const auto& lv : for_s.loop_vars) {
                loop_decls.insert(lv);
            }

            if (for_s.loop_vars.size() == 1) {
                const std::string& lv = for_s.loop_vars[0];
                const TypeId elem_t = !iter_type.type_args.empty() ? iter_type.type_args[0] : add_type(arena, TypeNode{1, 1, "auto", {}, false});
                loop_scope[lv] = VarSymbol{lv, elem_t, false};
                emit_line(state, "for (auto& " + lv + " : " + iter_cpp + ") {");
            } else {
                std::string vars_joined;
                for (size_t i = 0; i < for_s.loop_vars.size(); ++i) {
                    if (i > 0) vars_joined += ", ";
                    vars_joined += for_s.loop_vars[i];
                }
                if (iter_type.type_args.size() == 2) {
                    loop_scope[for_s.loop_vars[0]] = VarSymbol{for_s.loop_vars[0], iter_type.type_args[0], false};
                    loop_scope[for_s.loop_vars[1]] = VarSymbol{for_s.loop_vars[1], iter_type.type_args[1], false};
                }
                emit_line(state, "for (auto& [" + vars_joined + "] : " + iter_cpp + ") {");
            }

            state.indent_level += 1;
            emit_stmt_list(arena, symbols, fn, for_s.body, loop_scope, loop_decls, state);
            state.indent_level -= 1;
            emit_line(state, "}");
            return;
        }
    }

    if (std::holds_alternative<SelectStmt>(payload)) {
        const auto& sel_s = std::get<SelectStmt>(payload);
        const std::string sel_tmp = next_temp(state, "_select");
        const std::string target_cpp = emit_expr(arena, symbols, sel_s.target, scope, state);
        emit_line(state, "auto " + sel_tmp + " = " + target_cpp + ";");

        for (size_t idx = 0; idx < sel_s.cases.size(); ++idx) {
            const auto& c = sel_s.cases[idx];
            const std::string match_cpp = emit_expr(arena, symbols, c.match_expr, scope, state);
            const std::string kw = (idx == 0) ? "if" : "} else if";
            emit_line(state, kw + " (" + sel_tmp + " == " + match_cpp + ") {");
            state.indent_level += 1;
            auto c_decls = declared_vars;
            emit_stmt_list(arena, symbols, fn, c.body, scope, c_decls, state);
            state.indent_level -= 1;
        }

        if (sel_s.has_case_else) {
            if (!sel_s.cases.empty()) {
                emit_line(state, "} else {");
            } else {
                emit_line(state, "{");
            }
            state.indent_level += 1;
            auto else_decls = declared_vars;
            emit_stmt_list(arena, symbols, fn, sel_s.case_else, scope, else_decls, state);
            state.indent_level -= 1;
        }

        if (!sel_s.cases.empty() || sel_s.has_case_else) {
            emit_line(state, "}");
        }
        return;
    }

    if (std::holds_alternative<ErrorBlock>(payload)) {
        state.active_error_block = std::get<ErrorBlock>(payload);
        return;
    }

    if (std::holds_alternative<DeferStmt>(payload)) {
        const auto& def_s = std::get<DeferStmt>(payload);
        state.defer_counter += 1;
        const std::string defer_var = "_defer_" + std::to_string(state.defer_counter);
        emit_line(state, "auto " + defer_var + " = _defer([&]() {");
        state.indent_level += 1;
        auto def_decls = declared_vars;
        const std::optional<ErrorBlock> prev_eb = state.active_error_block;
        state.active_error_block = std::nullopt;
        emit_stmt_list(arena, symbols, fn, def_s.body, scope, def_decls, state);
        state.active_error_block = prev_eb;
        state.indent_level -= 1;
        emit_line(state, "});");
        return;
    }

    if (std::holds_alternative<SpawnStmt>(payload)) {
        const auto& sp_s = std::get<SpawnStmt>(payload);
        const std::string call_cpp = emit_expr(arena, symbols, sp_s.call_expr, scope, state);
        emit_line(state, "std::thread([=]() mutable { " + call_cpp + "; }).detach();");
        return;
    }

    if (std::holds_alternative<ReturnStmt>(payload)) {
        emit_return(arena, symbols, fn, std::get<ReturnStmt>(payload), scope, state);
        return;
    }

    if (std::holds_alternative<BreakStmt>(payload)) {
        emit_line(state, "break;");
        return;
    }
}

inline void emit_struct_decl(
    AstArena& arena,
    const GlobalSymbolTable& symbols,
    const TypeDecl& td,
    EmitterState& state
) {
    emit_line(state, "struct " + td.name + " {");
    state.indent_level += 1;
    for (const auto& f : td.fields) {
        emit_line(state, map_type(arena, symbols, f.type_id) + " " + f.name + "{};");
    }
    state.indent_level -= 1;
    emit_line(state, "};");
}

inline void emit_fn_decl(
    AstArena& arena,
    const GlobalSymbolTable& symbols,
    const FnDecl& fn,
    EmitterState& state
) {
    emit_line(state, fn_signature(arena, symbols, fn) + " {");
    state.indent_level += 1;

    std::unordered_map<std::string, VarSymbol> scope;
    std::unordered_set<std::string> declared_vars;
    for (const auto& p : fn.params) {
        const TypeId resolved_t = resolve_alias(arena, symbols, p.type_id);
        scope[p.name] = VarSymbol{p.name, resolved_t, p.is_byref};
        declared_vars.insert(p.name);
    }

    emit_stmt_list(arena, symbols, fn, fn.body, scope, declared_vars, state);

    if (fn.returns_err && fn.return_types.empty()) {
        bool ends_with_return = false;
        if (!fn.body.empty()) {
            const StmtId last_sid = fn.body.back();
            if (last_sid < arena.stmts.size()) {
                const auto& last_payload = arena.stmts[last_sid].payload;
                if (std::holds_alternative<ReturnStmt>(last_payload)) {
                    ends_with_return = true;
                } else if (std::holds_alternative<ExprStmt>(last_payload)) {
                    const auto& es = std::get<ExprStmt>(last_payload);
                    if (es.expr < arena.exprs.size() && std::holds_alternative<FuncCall>(arena.exprs[es.expr].payload)) {
                        if (is_error_builtin_call(arena, std::get<FuncCall>(arena.exprs[es.expr].payload))) {
                            ends_with_return = true;
                        }
                    }
                }
            }
        }
        if (!ends_with_return) {
            emit_line(state, R"(return cb_error{"", 0};)");
        }
    }

    state.indent_level -= 1;
    emit_line(state, "}");
}

inline std::string emit_program(
    AstArena& arena,
    const GlobalSymbolTable& symbols,
    const Program& program
) {
    EmitterState state{};

    if (!symbols.link_libs.empty()) {
        std::string flags;
        for (size_t i = 0; i < symbols.link_libs.size(); ++i) {
            if (i > 0) flags += " ";
            flags += "-l" + symbols.link_libs[i];
        }
        emit_line(state, "// CleanBasic Linker Flags: " + flags);
    }

    state.lines.push_back(std::string(CPP_RUNTIME_PREAMBLE));

    if (!symbols.headers.empty()) {
        emit_line(state);
        for (const auto& hdr : symbols.headers) {
            emit_line(state, "#include \"" + hdr + "\"");
        }
    }

    std::vector<TypeDecl> struct_decls;
    std::vector<AliasDecl> alias_decls;
    std::vector<FnDecl> fn_decls;

    for (const DeclId did : program.declarations) {
        if (did >= arena.decls.size()) continue;
        const auto& decl_node = arena.decls[did];
        if (std::holds_alternative<TypeDecl>(decl_node.payload)) {
            const auto& td = std::get<TypeDecl>(decl_node.payload);
            if (!td.is_extern) struct_decls.push_back(td);
        } else if (std::holds_alternative<AliasDecl>(decl_node.payload)) {
            alias_decls.push_back(std::get<AliasDecl>(decl_node.payload));
        } else if (std::holds_alternative<FnDecl>(decl_node.payload)) {
            const auto& fd = std::get<FnDecl>(decl_node.payload);
            if (!fd.is_extern) fn_decls.push_back(fd);
        }
    }

    if (!struct_decls.empty() || !alias_decls.empty()) {
        emit_line(state);
        for (const auto& td : struct_decls) {
            emit_line(state, "struct " + td.name + ";");
        }
        for (const auto& ad : alias_decls) {
            emit_line(state, "using " + ad.name + " = " + map_type(arena, symbols, ad.target_type) + ";");
        }
    }

    if (!struct_decls.empty()) {
        emit_line(state);
        for (const auto& td : struct_decls) {
            emit_struct_decl(arena, symbols, td, state);
            emit_line(state);
        }
    }

    if (!fn_decls.empty()) {
        emit_line(state, "// Forward Declarations");
        for (const auto& fd : fn_decls) {
            emit_line(state, fn_signature(arena, symbols, fd) + ";");
        }
        emit_line(state);
    }

    for (const auto& fd : fn_decls) {
        emit_fn_decl(arena, symbols, fd, state);
        emit_line(state);
    }

    if (symbols.functions.find("main") != symbols.functions.end()) {
        emit_line(state, "int main() {");
        state.indent_level += 1;
        emit_line(state, "cb_main();");
        emit_line(state, "return 0;");
        state.indent_level -= 1;
        emit_line(state, "}");
    }

    std::string out;
    for (const auto& line : state.lines) {
        out += line;
        out += "\n";
    }
    return out;
}

}  // namespace cb

#endif  // CLEANBASIC_EMITTER_HPP
