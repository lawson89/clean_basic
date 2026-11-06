#ifndef CLEANBASIC_LSP_HPP
#define CLEANBASIC_LSP_HPP

#include "src/cleanbasic/json.hpp"
#include "src/cleanbasic/pipeline.hpp"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cb {

struct LspDocumentSnapshot {
    bool valid = false;
    AstArena arena;
    Program program;
    GlobalSymbolTable symbols;
};

struct LspServerState {
    bool initialized = false;
    bool shutdown_requested = false;
    bool should_exit = false;
    int exit_code = 0;
    std::unordered_map<std::string, std::string> open_documents;
    std::unordered_map<std::string, LspDocumentSnapshot> snapshots;
};

inline std::string wrap_vb_code_block(std::string_view code) {
    const std::string fence(3, static_cast<char>(96));
    return fence + "vb\n" + std::string(code) + "\n" + fence;
}

inline std::string wrap_inline_code(std::string_view text) {
    const std::string tick(1, static_cast<char>(96));
    return tick + std::string(text) + tick;
}

inline bool is_ascii_alpha(char c) {
    if (c >= 'a' && c <= 'z') {
        return true;
    }
    if (c >= 'A' && c <= 'Z') {
        return true;
    }
    return false;
}

inline std::string percent_decode_uri_path(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    size_t i = 0;
    while (i < raw.size()) {
        if (raw[i] == '%' && i + 2 < raw.size()) {
            const auto hi = json_hex_digit_value(raw[i + 1]);
            const auto lo = json_hex_digit_value(raw[i + 2]);
            if (hi.has_value() && lo.has_value()) {
                const uint32_t byte_val = (hi.value() << 4) | lo.value();
                out.push_back(static_cast<char>(byte_val));
                i += 3;
                continue;
            }
        }
        out.push_back(raw[i]);
        i += 1;
    }
    return out;
}

inline std::string uri_to_file_path(std::string_view uri) {
    std::string_view rest = uri;
    const std::string_view scheme = "file://";
    if (rest.size() >= scheme.size() && rest.substr(0, scheme.size()) == scheme) {
        rest = rest.substr(scheme.size());
        const std::string_view localhost = "localhost/";
        if (rest.size() >= localhost.size() && rest.substr(0, localhost.size()) == localhost) {
            rest = rest.substr(localhost.size() - 1);
        }
        std::string decoded = percent_decode_uri_path(rest);
        if (decoded.size() >= 3 && decoded[0] == '/' && is_ascii_alpha(decoded[1]) && decoded[2] == ':') {
            decoded = decoded.substr(1);
        }
        return decoded;
    }
    return std::string(uri);
}

inline std::string file_path_to_uri(std::string_view path) {
    const std::string_view scheme = "file://";
    if (path.size() >= scheme.size() && path.substr(0, scheme.size()) == scheme) {
        return std::string(path);
    }
    if (!path.empty() && path[0] == '/') {
        return "file://" + std::string(path);
    }
    return "file:///" + std::string(path);
}

inline std::unordered_map<std::string, std::string> build_open_file_buffers(const LspServerState& state) {
    std::unordered_map<std::string, std::string> buffers;
    for (const auto& kv : state.open_documents) {
        const std::string file_path = uri_to_file_path(kv.first);
        buffers[file_path] = kv.second;
        const std::string norm_path = normalize_rel_path(file_path);
        if (!norm_path.empty()) {
            buffers[norm_path] = kv.second;
        }
    }
    return buffers;
}

inline std::string read_document_text(const LspServerState& state, std::string_view uri) {
    const auto it = state.open_documents.find(std::string(uri));
    if (it != state.open_documents.end()) {
        return it->second;
    }
    const std::string file_path = uri_to_file_path(uri);
    const auto open_buffers = build_open_file_buffers(state);
    const auto mem_opt = find_in_memory_source(open_buffers, file_path);
    if (mem_opt.has_value()) {
        return mem_opt.value();
    }
    std::ifstream in(file_path);
    if (!in.is_open()) {
        return "";
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

inline LspDocumentSnapshot get_or_build_snapshot(LspServerState& state, std::string_view uri) {
    const std::string uri_key(uri);
    const std::string file_path = uri_to_file_path(uri);
    const std::string text = read_document_text(state, uri);
    if (!text.empty()) {
        const auto open_buffers = build_open_file_buffers(state);
        const CheckSourceResult check_res = check_source(text, file_path, open_buffers);
        if (check_res.ok) {
            LspDocumentSnapshot snap{true, check_res.arena, check_res.program, check_res.symbols};
            state.snapshots[uri_key] = snap;
            return snap;
        }
    }
    const auto snap_it = state.snapshots.find(uri_key);
    if (snap_it != state.snapshots.end() && snap_it->second.valid) {
        return snap_it->second;
    }
    LspDocumentSnapshot empty_snap{};
    empty_snap.valid = false;
    return empty_snap;
}

inline JsonId clone_json_scalar_or_null(const JsonArena& src_arena, JsonId src_id, JsonArena& dst_arena) {
    if (src_id >= src_arena.nodes.size()) {
        return json_add_null(dst_arena);
    }
    const auto& var = src_arena.nodes[src_id];
    if (std::holds_alternative<JsonNull>(var)) {
        return json_add_null(dst_arena);
    }
    if (std::holds_alternative<JsonBool>(var)) {
        return json_add_bool(dst_arena, std::get<JsonBool>(var).value);
    }
    if (std::holds_alternative<JsonNumber>(var)) {
        const auto& num = std::get<JsonNumber>(var);
        if (num.is_int) {
            return json_add_int(dst_arena, num.int_val);
        }
        return json_add_float(dst_arena, num.float_val);
    }
    if (std::holds_alternative<JsonString>(var)) {
        return json_add_string(dst_arena, std::get<JsonString>(var).value);
    }
    return json_add_null(dst_arena);
}

inline JsonId make_lsp_range(
    JsonArena& arena,
    uint32_t start_line_1based,
    uint32_t start_col_1based,
    uint32_t end_line_1based,
    uint32_t end_col_1based
) {
    int64_t s_line = 0;
    if (start_line_1based > 0) {
        s_line = static_cast<int64_t>(start_line_1based - 1);
    }
    int64_t s_col = 0;
    if (start_col_1based > 0) {
        s_col = static_cast<int64_t>(start_col_1based - 1);
    }
    int64_t e_line = s_line;
    if (end_line_1based > 0) {
        e_line = static_cast<int64_t>(end_line_1based - 1);
    }
    if (e_line < s_line) {
        e_line = s_line;
    }
    int64_t e_col = s_col;
    if (end_col_1based > 0) {
        e_col = static_cast<int64_t>(end_col_1based - 1);
    }

    const JsonId start_pos = json_add_object(arena);
    json_object_set(arena, start_pos, "line", json_add_int(arena, s_line));
    json_object_set(arena, start_pos, "character", json_add_int(arena, s_col));

    const JsonId end_pos = json_add_object(arena);
    json_object_set(arena, end_pos, "line", json_add_int(arena, e_line));
    json_object_set(arena, end_pos, "character", json_add_int(arena, e_col));

    const JsonId range_obj = json_add_object(arena);
    json_object_set(arena, range_obj, "start", start_pos);
    json_object_set(arena, range_obj, "end", end_pos);
    return range_obj;
}

inline JsonId make_lsp_location(
    JsonArena& arena,
    std::string_view file_path,
    uint32_t line_1based,
    uint32_t col_1based,
    size_t name_len
) {
    uint32_t span_len = 1;
    if (name_len > 0) {
        span_len = static_cast<uint32_t>(name_len);
    }
    const JsonId loc = json_add_object(arena);
    json_object_set(arena, loc, "uri", json_add_string(arena, file_path_to_uri(file_path)));
    json_object_set(
        arena,
        loc,
        "range",
        make_lsp_range(arena, line_1based, col_1based, line_1based, col_1based + span_len)
    );
    return loc;
}

inline std::string format_cleanbasic_type(const AstArena& arena, TypeId tid) {
    if (tid == INVALID_ID || tid >= arena.types.size()) {
        return "void";
    }
    const TypeNode& tn = arena.types[tid];
    if (tn.name.empty() || tn.name == "void") {
        return "void";
    }
    const auto [mod_id, base_name] = split_symbol_key(tn.name);
    std::string out = base_name;
    if (!tn.type_args.empty()) {
        out += "[";
        for (size_t i = 0; i < tn.type_args.size(); ++i) {
            if (i > 0) {
                out += ", ";
            }
            out += format_cleanbasic_type(arena, tn.type_args[i]);
        }
        out += "]";
    }
    return out;
}

inline std::string format_fn_detail_signature(
    const AstArena& arena,
    const std::vector<Param>& params,
    TypeId return_type,
    bool is_fallible
) {
    std::string out = "(";
    for (size_t i = 0; i < params.size(); ++i) {
        if (i > 0) {
            out += ", ";
        }
        if (params[i].is_byref) {
            out += "byref ";
        }
        out += params[i].name + " as " + format_cleanbasic_type(arena, params[i].type_id);
    }
    out += ")";
    if (!is_void_type(arena, return_type)) {
        out += " as " + format_cleanbasic_type(arena, return_type);
    }
    if (is_fallible) {
        out += " can error";
    }
    return out;
}

inline std::string format_fn_hover_markdown(const AstArena& arena, const FnSymbol& fn) {
    std::string sig;
    if (fn.is_pub) {
        sig += "pub ";
    }
    if (fn.is_extern) {
        sig += "extern ";
    }
    sig += "fn " + fn.name + format_fn_detail_signature(arena, fn.params, fn.return_type, fn.is_fallible);
    return wrap_vb_code_block(sig);
}

inline std::string format_type_hover_markdown(const AstArena& arena, const TypeSymbol& ts) {
    std::string code;
    if (ts.is_pub) {
        code += "pub ";
    }
    if (ts.is_extern) {
        code += "extern ";
    }
    code += "type " + ts.name;
    if (ts.is_extern && ts.field_order.empty()) {
        return wrap_vb_code_block(code);
    }
    code += "\n";
    for (const auto& f : ts.field_order) {
        TypeId ftid = f.type_id;
        const auto fit = ts.fields.find(f.name);
        if (fit != ts.fields.end()) {
            ftid = fit->second;
        }
        code += "    " + f.name + " as " + format_cleanbasic_type(arena, ftid) + "\n";
    }
    code += "end type";
    return wrap_vb_code_block(code);
}

inline std::string format_alias_hover_markdown(
    const AstArena& arena,
    const GlobalSymbolTable& symbols,
    const AliasSymbol& al
) {
    std::string code;
    if (al.is_pub) {
        code += "pub ";
    }
    const std::string raw_target = format_cleanbasic_type(arena, al.target_type);
    const TypeId resolved_tid = resolve_alias(arena, symbols, al.module_id, al.target_type);
    const std::string resolved_target = format_cleanbasic_type(arena, resolved_tid);
    code += "alias " + al.name + " = " + raw_target;
    std::string md = wrap_vb_code_block(code);
    if (resolved_target != raw_target) {
        md.push_back(static_cast<char>(10));
        md += "Resolved type: " + wrap_inline_code(resolved_target);
    }
    return md;
}

inline std::string format_var_hover_markdown(const AstArena& arena, const VarSymbol& vs) {
    std::string prefix = "dim ";
    if (vs.is_byref) {
        prefix = "(param) byref ";
    } else if (vs.is_const) {
        prefix = "const ";
    }
    const std::string tname = format_cleanbasic_type(arena, vs.type_id);
    return wrap_vb_code_block(prefix + vs.name + " as " + tname);
}

inline std::optional<std::string> format_builtin_hover_markdown(std::string_view name) {
    if (name == "print") {
        return wrap_vb_code_block("fn print(values...)") +
            "\nWrites one or more values to standard output followed by a newline.";
    }
    if (name == "len") {
        return wrap_vb_code_block("fn len(container as any) as int") +
            "\nReturns the number of elements in a list, dict, or str.";
    }
    if (name == "str") {
        return wrap_vb_code_block("fn str(val as any) as str") +
            "\nConverts a numeric, boolean, or err value to a str.";
    }
    if (name == "error") {
        return wrap_vb_code_block("fn error(msg as str, [code=1]) as err") +
            "\nRaises and returns a structured err value from a can error function.";
    }
    return std::nullopt;
}

inline bool decl_belongs_to_file(
    const DeclNode& decl_node,
    std::string_view target_file_path,
    std::string_view target_norm_path
) {
    if (decl_node.file_name == target_file_path) {
        return true;
    }
    const std::string decl_norm = normalize_rel_path(decl_node.file_name);
    if (!decl_norm.empty() && !target_norm_path.empty() && decl_norm == target_norm_path) {
        return true;
    }
    if (decl_node.module_id.empty() && decl_node.file_name.empty()) {
        return true;
    }
    return false;
}

inline std::string find_module_id_for_file(
    const AstArena& arena,
    const Program& program,
    std::string_view file_path
) {
    const std::string norm_target = normalize_rel_path(file_path);
    for (const DeclId did : program.declarations) {
        if (did >= arena.decls.size()) {
            continue;
        }
        const DeclNode& dn = arena.decls[did];
        if (decl_belongs_to_file(dn, file_path, norm_target)) {
            return dn.module_id;
        }
    }
    return "";
}

inline std::optional<std::string> find_enclosing_fn_key(
    const AstArena& arena,
    const Program& program,
    std::string_view file_path,
    uint32_t cursor_line_1based
) {
    const std::string norm_target = normalize_rel_path(file_path);
    for (const DeclId did : program.declarations) {
        if (did >= arena.decls.size()) {
            continue;
        }
        const DeclNode& dn = arena.decls[did];
        if (!decl_belongs_to_file(dn, file_path, norm_target)) {
            continue;
        }
        if (std::holds_alternative<FnDecl>(dn.payload)) {
            const auto& fn = std::get<FnDecl>(dn.payload);
            if (cursor_line_1based >= dn.line && cursor_line_1based <= dn.end_line) {
                return symbol_key(dn.module_id, fn.name);
            }
        }
    }
    return std::nullopt;
}

struct CursorTokenContext {
    bool found = false;
    Token token;
    size_t token_index = 0;
    bool has_dot_receiver = false;
    std::string receiver_name;
    bool is_import_target = false;
};

inline CursorTokenContext resolve_cursor_token(
    std::string_view source_text,
    std::string_view file_path,
    uint32_t line_1based,
    uint32_t col_1based
) {
    CursorTokenContext ctx{};
    const LexResult lex_res = tokenize(source_text, file_path);
    if (lex_res.tokens.empty()) {
        return ctx;
    }

    for (size_t i = 0; i < lex_res.tokens.size(); ++i) {
        const Token& tok = lex_res.tokens[i];
        if (tok.kind == "EOF" || tok.kind == "NEWLINE") {
            continue;
        }
        if (tok.line != line_1based) {
            continue;
        }
        const uint32_t tok_start = tok.col;
        uint32_t tok_len = 1;
        if (!tok.value.empty()) {
            tok_len = static_cast<uint32_t>(tok.value.size());
        }
        const uint32_t tok_end = tok_start + tok_len;
        if (col_1based >= tok_start && col_1based <= tok_end) {
            ctx.found = true;
            ctx.token = tok;
            ctx.token_index = i;
            if (i >= 2 && lex_res.tokens[i - 1].kind == "DOT") {
                const Token& prev_tok = lex_res.tokens[i - 2];
                if (prev_tok.kind == "IDENT" || prev_tok.kind == "TYPE_KW") {
                    ctx.has_dot_receiver = true;
                    ctx.receiver_name = prev_tok.value;
                }
            }
            if (i >= 1 && lex_res.tokens[i - 1].kind == "KEYWORD" &&
                lex_res.tokens[i - 1].value == "import" && tok.kind == "STRING") {
                ctx.is_import_target = true;
            }
            return ctx;
        }
    }
    return ctx;
}

inline std::optional<std::string> find_module_file_path(
    const AstArena& arena,
    const Program& program,
    std::string_view target_module_id,
    std::string_view current_file_path,
    std::string_view import_target = ""
) {
    for (const DeclId did : program.declarations) {
        if (did >= arena.decls.size()) {
            continue;
        }
        const DeclNode& dn = arena.decls[did];
        if (dn.module_id == target_module_id && !dn.file_name.empty()) {
            return dn.file_name;
        }
    }
    if (!import_target.empty()) {
        if (is_std_import_target(import_target)) {
            const std::string std_sub = strip_cb_ext(import_target.substr(4));
            return cleanbasic_root_dir() + "/stdlib/" + std_sub + "/" + std_sub + ".cb";
        }
        const std::string dir = parent_dir_of(current_file_path);
        if (dir == ".") {
            return std::string(import_target);
        }
        return normalize_rel_path(dir + "/" + std::string(import_target));
    }
    return std::nullopt;
}

struct LspTextPositionParams {
    bool valid = false;
    std::string uri;
    std::string file_path;
    uint32_t line_0based = 0;
    uint32_t char_0based = 0;
    uint32_t line_1based = 1;
    uint32_t col_1based = 1;
};

inline LspTextPositionParams parse_doc_position_params(const JsonArena& req_arena, JsonId params_id) {
    LspTextPositionParams res{};
    const JsonId text_doc = json_get_field(req_arena, params_id, "textDocument");
    const auto uri_opt = json_get_string(req_arena, json_get_field(req_arena, text_doc, "uri"));
    const JsonId pos_obj = json_get_field(req_arena, params_id, "position");
    const auto line0_opt = json_get_int(req_arena, json_get_field(req_arena, pos_obj, "line"));
    const auto char0_opt = json_get_int(req_arena, json_get_field(req_arena, pos_obj, "character"));
    if (uri_opt.has_value()) {
        res.uri = uri_opt.value();
        res.file_path = uri_to_file_path(res.uri);
    }
    if (uri_opt.has_value() && line0_opt.has_value() && char0_opt.has_value()) {
        res.valid = true;
        res.line_0based = static_cast<uint32_t>(line0_opt.value());
        res.char_0based = static_cast<uint32_t>(char0_opt.value());
        res.line_1based = res.line_0based + 1;
        res.col_1based = res.char_0based + 1;
    }
    return res;
}

inline void write_jsonrpc_result(
    const JsonArena& req_arena,
    JsonId req_id,
    JsonArena& out_arena,
    JsonId result_id,
    std::ostream& out
) {
    const JsonId root = json_add_object(out_arena);
    json_object_set(out_arena, root, "jsonrpc", json_add_string(out_arena, "2.0"));
    json_object_set(out_arena, root, "id", clone_json_scalar_or_null(req_arena, req_id, out_arena));
    json_object_set(out_arena, root, "result", result_id);
    write_jsonrpc_message(out, serialize_json(out_arena, root));
}

inline JsonId make_lsp_doc_symbol(
    JsonArena& arena,
    std::string_view name,
    std::string_view detail,
    int64_t kind,
    uint32_t start_line,
    uint32_t start_col,
    uint32_t end_line,
    uint32_t name_line,
    uint32_t name_col
) {
    const uint32_t name_len = static_cast<uint32_t>(name.size());
    const JsonId sym_obj = json_add_object(arena);
    json_object_set(arena, sym_obj, "name", json_add_string(arena, name));
    json_object_set(arena, sym_obj, "detail", json_add_string(arena, detail));
    json_object_set(arena, sym_obj, "kind", json_add_int(arena, kind));
    json_object_set(arena, sym_obj, "range", make_lsp_range(arena, start_line, start_col, end_line, name_col + name_len));
    json_object_set(arena, sym_obj, "selectionRange", make_lsp_range(arena, name_line, name_col, name_line, name_col + name_len));
    return sym_obj;
}

inline void publish_empty_diagnostics(std::string_view uri, std::ostream& out) {
    JsonArena arena;
    const JsonId root = json_add_object(arena);
    json_object_set(arena, root, "jsonrpc", json_add_string(arena, "2.0"));
    json_object_set(arena, root, "method", json_add_string(arena, "textDocument/publishDiagnostics"));

    const JsonId params = json_add_object(arena);
    json_object_set(arena, params, "uri", json_add_string(arena, uri));
    json_object_set(arena, params, "diagnostics", json_add_array(arena));
    json_object_set(arena, root, "params", params);

    write_jsonrpc_message(out, serialize_json(arena, root));
}

inline void publish_document_diagnostics(
    LspServerState& state,
    std::string_view uri,
    std::string_view text,
    std::ostream& out
) {
    const std::string file_path = uri_to_file_path(uri);
    const auto open_buffers = build_open_file_buffers(state);
    const CheckSourceResult check_res = check_source(text, file_path, open_buffers);

    if (check_res.ok) {
        state.snapshots[std::string(uri)] = LspDocumentSnapshot{true, check_res.arena, check_res.program, check_res.symbols};
    }

    JsonArena arena;
    const JsonId root = json_add_object(arena);
    json_object_set(arena, root, "jsonrpc", json_add_string(arena, "2.0"));
    json_object_set(arena, root, "method", json_add_string(arena, "textDocument/publishDiagnostics"));

    const JsonId params = json_add_object(arena);
    json_object_set(arena, params, "uri", json_add_string(arena, uri));
    const JsonId diag_arr = json_add_array(arena);

    if (!check_res.ok) {
        int64_t line0 = 0;
        if (check_res.error.line > 0) {
            line0 = static_cast<int64_t>(check_res.error.line - 1);
        }
        int64_t col0 = 0;
        if (check_res.error.col > 0) {
            col0 = static_cast<int64_t>(check_res.error.col - 1);
        }

        const JsonId start_pos = json_add_object(arena);
        json_object_set(arena, start_pos, "line", json_add_int(arena, line0));
        json_object_set(arena, start_pos, "character", json_add_int(arena, col0));

        const JsonId end_pos = json_add_object(arena);
        json_object_set(arena, end_pos, "line", json_add_int(arena, line0));
        json_object_set(arena, end_pos, "character", json_add_int(arena, col0 + 1));

        const JsonId range_obj = json_add_object(arena);
        json_object_set(arena, range_obj, "start", start_pos);
        json_object_set(arena, range_obj, "end", end_pos);

        const JsonId diag_obj = json_add_object(arena);
        json_object_set(arena, diag_obj, "range", range_obj);
        json_object_set(arena, diag_obj, "severity", json_add_int(arena, 1));
        json_object_set(arena, diag_obj, "source", json_add_string(arena, "cleanbasic"));
        json_object_set(arena, diag_obj, "code", json_add_string(arena, check_res.error.kind));
        json_object_set(arena, diag_obj, "message", json_add_string(arena, check_res.error.message));

        json_array_push(arena, diag_arr, diag_obj);
    }

    json_object_set(arena, params, "diagnostics", diag_arr);
    json_object_set(arena, root, "params", params);

    write_jsonrpc_message(out, serialize_json(arena, root));
}

inline void handle_document_symbol_request(
    LspServerState& state,
    const JsonArena& req_arena,
    JsonId req_id,
    JsonId params_id,
    std::ostream& out
) {
    const LspTextPositionParams doc = parse_doc_position_params(req_arena, params_id);
    JsonArena out_arena;
    const JsonId symbols_arr = json_add_array(out_arena);

    if (!doc.uri.empty()) {
        const std::string norm_path = normalize_rel_path(doc.file_path);
        const LspDocumentSnapshot snap = get_or_build_snapshot(state, doc.uri);
        if (snap.valid) {
            for (const DeclId did : snap.program.declarations) {
                if (did >= snap.arena.decls.size()) {
                    continue;
                }
                const DeclNode& dn = snap.arena.decls[did];
                if (!decl_belongs_to_file(dn, doc.file_path, norm_path)) {
                    continue;
                }
                if (std::holds_alternative<TypeDecl>(dn.payload)) {
                    const auto& td = std::get<TypeDecl>(dn.payload);
                    const JsonId sym_obj = make_lsp_doc_symbol(
                        out_arena, td.name, "type", 23, dn.line, dn.col, dn.end_line, dn.name_line, dn.name_col
                    );
                    const JsonId children_arr = json_add_array(out_arena);
                    for (const auto& field : td.fields) {
                        json_array_push(
                            out_arena,
                            children_arr,
                            make_lsp_doc_symbol(
                                out_arena,
                                field.name,
                                format_cleanbasic_type(snap.arena, field.type_id),
                                8,
                                field.line,
                                field.col,
                                field.line,
                                field.line,
                                field.col
                            )
                        );
                    }
                    json_object_set(out_arena, sym_obj, "children", children_arr);
                    json_array_push(out_arena, symbols_arr, sym_obj);
                } else if (std::holds_alternative<FnDecl>(dn.payload)) {
                    const auto& fn = std::get<FnDecl>(dn.payload);
                    json_array_push(
                        out_arena,
                        symbols_arr,
                        make_lsp_doc_symbol(
                            out_arena,
                            fn.name,
                            format_fn_detail_signature(snap.arena, fn.params, fn.return_type, fn.is_fallible),
                            12,
                            dn.line,
                            dn.col,
                            dn.end_line,
                            dn.name_line,
                            dn.name_col
                        )
                    );
                } else if (std::holds_alternative<AliasDecl>(dn.payload)) {
                    const auto& al = std::get<AliasDecl>(dn.payload);
                    json_array_push(
                        out_arena,
                        symbols_arr,
                        make_lsp_doc_symbol(
                            out_arena,
                            al.name,
                            "= " + format_cleanbasic_type(snap.arena, al.target_type),
                            26,
                            dn.line,
                            dn.col,
                            dn.end_line,
                            dn.name_line,
                            dn.name_col
                        )
                    );
                }
            }
        }
    }

    write_jsonrpc_result(req_arena, req_id, out_arena, symbols_arr, out);
}

struct DefinitionTarget {
    std::string file_name;
    uint32_t line = 1;
    uint32_t col = 1;
    size_t name_len = 1;
};

struct ResolvedCursorSymbol {
    std::optional<std::string> hover_markdown;
    std::optional<DefinitionTarget> definition;
};

inline DefinitionTarget make_def_target(std::string_view file_name, uint32_t line, uint32_t col, size_t len) {
    return DefinitionTarget{std::string(file_name), line, col, len};
}

inline ResolvedCursorSymbol resolve_cursor_symbol_info(
    const LspDocumentSnapshot& snap,
    std::string_view file_path,
    const CursorTokenContext& cursor_ctx
) {
    ResolvedCursorSymbol res{};
    if (!snap.valid || !cursor_ctx.found) {
        return res;
    }

    const Token& tok = cursor_ctx.token;
    const std::string current_mod_id = find_module_id_for_file(snap.arena, snap.program, file_path);

    if (cursor_ctx.is_import_target && tok.kind == "STRING") {
        const std::string raw_target = strip_quotes(tok.value);
        if (raw_target.size() > 3 && raw_target.substr(raw_target.size() - 3) == ".cb") {
            const std::string target_mod_id = resolve_module_id(current_mod_id, raw_target);
            const auto mod_file_opt = find_module_file_path(snap.arena, snap.program, target_mod_id, file_path, raw_target);
            if (mod_file_opt.has_value()) {
                res.definition = make_def_target(mod_file_opt.value(), 1, 1, 1);
            }
        }
        return res;
    }

    if (tok.kind != "IDENT" && tok.kind != "TYPE_KW" && tok.kind != "KEYWORD") {
        return res;
    }

    const auto fn_key_opt = find_enclosing_fn_key(snap.arena, snap.program, file_path, tok.line);
    std::unordered_map<std::string, VarSymbol> active_scope;
    if (fn_key_opt.has_value()) {
        const auto scope_it = snap.symbols.fn_scopes.find(fn_key_opt.value());
        if (scope_it != snap.symbols.fn_scopes.end()) {
            active_scope = scope_it->second;
        }
    }

    const auto bind_fn = [&](const FnSymbol& fn) {
        res.hover_markdown = format_fn_hover_markdown(snap.arena, fn);
        if (!fn.file_name.empty()) {
            res.definition = make_def_target(fn.file_name, fn.line, fn.col, fn.name.size());
        }
    };
    const auto bind_type = [&](const TypeSymbol& ts) {
        res.hover_markdown = format_type_hover_markdown(snap.arena, ts);
        if (!ts.file_name.empty()) {
            res.definition = make_def_target(ts.file_name, ts.line, ts.col, ts.name.size());
        }
    };
    const auto bind_alias = [&](const AliasSymbol& al) {
        res.hover_markdown = format_alias_hover_markdown(snap.arena, snap.symbols, al);
        if (!al.file_name.empty()) {
            res.definition = make_def_target(al.file_name, al.line, al.col, al.name.size());
        }
    };

    if (cursor_ctx.has_dot_receiver) {
        const std::string& recv = cursor_ctx.receiver_name;
        const std::string& member = tok.value;

        std::string target_mod_id;
        if (is_module_alias_in_scope(snap.symbols, current_mod_id, recv, active_scope, target_mod_id)) {
            const std::string qkey = symbol_key(target_mod_id, member);
            const auto fn_it = snap.symbols.functions.find(qkey);
            if (fn_it != snap.symbols.functions.end()) {
                bind_fn(fn_it->second);
                return res;
            }
            const auto ty_it = snap.symbols.types.find(qkey);
            if (ty_it != snap.symbols.types.end()) {
                bind_type(ty_it->second);
                return res;
            }
            const auto al_it = snap.symbols.aliases.find(qkey);
            if (al_it != snap.symbols.aliases.end()) {
                bind_alias(al_it->second);
                return res;
            }
        }

        const auto var_it = active_scope.find(recv);
        if (var_it != active_scope.end()) {
            const TypeId recv_tid = var_it->second.type_id;
            const auto ts_opt = resolve_type_symbol(snap.arena, snap.symbols, current_mod_id, recv_tid);
            if (ts_opt.has_value()) {
                const TypeSymbol& ts = ts_opt.value();
                const auto fit = ts.fields.find(member);
                if (fit != ts.fields.end()) {
                    const std::string ftype = format_cleanbasic_type(snap.arena, fit->second);
                    res.hover_markdown = wrap_vb_code_block("(field) " + ts.name + "." + member + " as " + ftype);
                    if (!ts.file_name.empty()) {
                        for (const auto& fd : ts.field_order) {
                            if (fd.name == member) {
                                res.definition = make_def_target(ts.file_name, fd.line, fd.col, fd.name.size());
                                break;
                            }
                        }
                    }
                    return res;
                }
            }
            const auto ufcs_fn = resolve_ufcs_fn(snap.arena, snap.symbols, current_mod_id, recv_tid, member);
            if (ufcs_fn.has_value()) {
                bind_fn(ufcs_fn.value());
                return res;
            }
        }
    }

    const auto local_it = active_scope.find(tok.value);
    if (local_it != active_scope.end()) {
        res.hover_markdown = format_var_hover_markdown(snap.arena, local_it->second);
        if (!local_it->second.file_name.empty()) {
            res.definition = make_def_target(
                local_it->second.file_name, local_it->second.line, local_it->second.col, local_it->second.name.size()
            );
        }
        return res;
    }

    const std::string sym_key = resolve_symbol_key(snap.symbols, current_mod_id, tok.value);
    const auto fn_it = snap.symbols.functions.find(sym_key);
    if (fn_it != snap.symbols.functions.end() && !is_builtin_symbol_name(tok.value)) {
        bind_fn(fn_it->second);
        return res;
    }

    const auto al_it = snap.symbols.aliases.find(sym_key);
    if (al_it != snap.symbols.aliases.end()) {
        bind_alias(al_it->second);
        return res;
    }

    const auto ty_it = snap.symbols.types.find(sym_key);
    if (ty_it != snap.symbols.types.end() && !is_builtin_type_name(tok.value)) {
        bind_type(ty_it->second);
        return res;
    }

    for (const auto& kv : snap.symbols.types) {
        const auto fit = kv.second.fields.find(tok.value);
        if (fit != kv.second.fields.end()) {
            for (const auto& fd : kv.second.field_order) {
                if (fd.name == tok.value && fd.line == tok.line) {
                    const std::string ftype = format_cleanbasic_type(snap.arena, fit->second);
                    res.hover_markdown = wrap_vb_code_block("(field) " + kv.second.name + "." + tok.value + " as " + ftype);
                    return res;
                }
            }
        }
    }

    std::string target_mod_id;
    if (is_module_alias_in_scope(snap.symbols, current_mod_id, tok.value, active_scope, target_mod_id)) {
        const auto mod_file_opt = find_module_file_path(snap.arena, snap.program, target_mod_id, file_path);
        if (mod_file_opt.has_value()) {
            res.definition = make_def_target(mod_file_opt.value(), 1, 1, 1);
        }
    }

    const auto builtin_md = format_builtin_hover_markdown(tok.value);
    if (builtin_md.has_value()) {
        res.hover_markdown = builtin_md;
    }

    return res;
}

inline void handle_hover_request(
    LspServerState& state,
    const JsonArena& req_arena,
    JsonId req_id,
    JsonId params_id,
    std::ostream& out
) {
    const LspTextPositionParams doc = parse_doc_position_params(req_arena, params_id);
    JsonArena out_arena;
    if (!doc.valid) {
        write_jsonrpc_result(req_arena, req_id, out_arena, json_add_null(out_arena), out);
        return;
    }

    const std::string source_text = read_document_text(state, doc.uri);
    const LspDocumentSnapshot snap = get_or_build_snapshot(state, doc.uri);
    const CursorTokenContext cursor_ctx = resolve_cursor_token(source_text, doc.file_path, doc.line_1based, doc.col_1based);
    const ResolvedCursorSymbol info = resolve_cursor_symbol_info(snap, doc.file_path, cursor_ctx);
    if (!info.hover_markdown.has_value()) {
        write_jsonrpc_result(req_arena, req_id, out_arena, json_add_null(out_arena), out);
        return;
    }

    const JsonId contents = json_add_object(out_arena);
    json_object_set(out_arena, contents, "kind", json_add_string(out_arena, "markdown"));
    json_object_set(out_arena, contents, "value", json_add_string(out_arena, info.hover_markdown.value()));

    const JsonId hover_obj = json_add_object(out_arena);
    json_object_set(out_arena, hover_obj, "contents", contents);
    if (cursor_ctx.found) {
        const uint32_t tlen = static_cast<uint32_t>(cursor_ctx.token.value.size());
        json_object_set(
            out_arena,
            hover_obj,
            "range",
            make_lsp_range(out_arena, cursor_ctx.token.line, cursor_ctx.token.col, cursor_ctx.token.line, cursor_ctx.token.col + tlen)
        );
    }

    write_jsonrpc_result(req_arena, req_id, out_arena, hover_obj, out);
}

inline void handle_definition_request(
    LspServerState& state,
    const JsonArena& req_arena,
    JsonId req_id,
    JsonId params_id,
    std::ostream& out
) {
    const LspTextPositionParams doc = parse_doc_position_params(req_arena, params_id);
    JsonArena out_arena;
    if (!doc.valid) {
        write_jsonrpc_result(req_arena, req_id, out_arena, json_add_null(out_arena), out);
        return;
    }

    const std::string source_text = read_document_text(state, doc.uri);
    const LspDocumentSnapshot snap = get_or_build_snapshot(state, doc.uri);
    const CursorTokenContext cursor_ctx = resolve_cursor_token(source_text, doc.file_path, doc.line_1based, doc.col_1based);
    const ResolvedCursorSymbol info = resolve_cursor_symbol_info(snap, doc.file_path, cursor_ctx);
    if (!info.definition.has_value()) {
        write_jsonrpc_result(req_arena, req_id, out_arena, json_add_null(out_arena), out);
        return;
    }

    const DefinitionTarget& dt = info.definition.value();
    const JsonId loc_obj = make_lsp_location(out_arena, dt.file_name, dt.line, dt.col, dt.name_len);
    write_jsonrpc_result(req_arena, req_id, out_arena, loc_obj, out);
}

struct CursorLineSlice {
    std::string full_line;
    std::string prefix_to_cursor;
    size_t line_start_offset = 0;
    size_t line_end_offset = 0;
};

inline CursorLineSlice extract_cursor_line_slice(
    std::string_view source_text,
    uint32_t line_0based,
    uint32_t char_0based
) {
    CursorLineSlice slice{};
    uint32_t curr_line = 0;
    size_t i = 0;
    while (i < source_text.size() && curr_line < line_0based) {
        if (source_text[i] == '\n') {
            curr_line += 1;
        }
        i += 1;
    }
    slice.line_start_offset = i;
    size_t end_i = i;
    while (end_i < source_text.size() && source_text[end_i] != '\n') {
        end_i += 1;
    }
    slice.line_end_offset = end_i;
    std::string_view raw_line = source_text.substr(i, end_i - i);
    if (!raw_line.empty() && raw_line.back() == '\r') {
        raw_line.remove_suffix(1);
    }
    slice.full_line = std::string(raw_line);
    size_t prefix_len = static_cast<size_t>(char_0based);
    if (prefix_len > slice.full_line.size()) {
        prefix_len = slice.full_line.size();
    }
    slice.prefix_to_cursor = slice.full_line.substr(0, prefix_len);
    return slice;
}

inline std::string scrub_incomplete_line_in_source(
    std::string_view source_text,
    const CursorLineSlice& slice
) {
    std::string scrubbed(source_text.substr(0, slice.line_start_offset));
    const std::string trimmed = trim_ascii_ws(slice.prefix_to_cursor);
    if (starts_with_ignore_case(trimmed, "dim ") || starts_with_ignore_case(trimmed, "const ")) {
        const size_t eq_pos = slice.prefix_to_cursor.find('=');
        if (eq_pos != std::string::npos) {
            scrubbed += slice.prefix_to_cursor.substr(0, eq_pos + 1) + " 0";
        } else {
            scrubbed += "' scrubbed";
        }
    } else if (starts_with_ignore_case(trimmed, "if ") || starts_with_ignore_case(trimmed, "else if ")) {
        if (starts_with_ignore_case(trimmed, "else if ")) {
            scrubbed += "else if true";
        } else {
            scrubbed += "if true";
        }
    } else if (starts_with_ignore_case(trimmed, "return ")) {
        scrubbed += "return 0";
    } else {
        scrubbed += "' scrubbed";
    }
    scrubbed += std::string(source_text.substr(slice.line_end_offset));
    return scrubbed;
}

inline LspDocumentSnapshot get_or_build_scrubbed_snapshot(
    LspServerState& state,
    std::string_view uri,
    std::string_view source_text,
    uint32_t line_0based,
    uint32_t char_0based
) {
    const std::string uri_key(uri);
    const std::string file_path = uri_to_file_path(uri);
    const auto open_buffers = build_open_file_buffers(state);

    if (!source_text.empty()) {
        const CheckSourceResult direct_res = check_source(source_text, file_path, open_buffers);
        if (direct_res.ok) {
            LspDocumentSnapshot snap{true, direct_res.arena, direct_res.program, direct_res.symbols};
            state.snapshots[uri_key] = snap;
            return snap;
        }

        const CursorLineSlice slice = extract_cursor_line_slice(source_text, line_0based, char_0based);
        const std::string scrubbed_text = scrub_incomplete_line_in_source(source_text, slice);
        auto scrubbed_buffers = open_buffers;
        scrubbed_buffers[file_path] = scrubbed_text;
        const std::string norm_path = normalize_rel_path(file_path);
        if (!norm_path.empty()) {
            scrubbed_buffers[norm_path] = scrubbed_text;
        }

        const CheckSourceResult scrubbed_res = check_source(scrubbed_text, file_path, scrubbed_buffers);
        if (scrubbed_res.ok) {
            return LspDocumentSnapshot{true, scrubbed_res.arena, scrubbed_res.program, scrubbed_res.symbols};
        }

        if (starts_with_ignore_case(trim_ascii_ws(slice.prefix_to_cursor), "return ")) {
            std::string void_ret_scrubbed(source_text.substr(0, slice.line_start_offset));
            void_ret_scrubbed += "return";
            void_ret_scrubbed += std::string(source_text.substr(slice.line_end_offset));
            scrubbed_buffers[file_path] = void_ret_scrubbed;
            if (!norm_path.empty()) {
                scrubbed_buffers[norm_path] = void_ret_scrubbed;
            }
            const CheckSourceResult void_res = check_source(void_ret_scrubbed, file_path, scrubbed_buffers);
            if (void_res.ok) {
                return LspDocumentSnapshot{true, void_res.arena, void_res.program, void_res.symbols};
            }
        }
    }

    const auto snap_it = state.snapshots.find(uri_key);
    if (snap_it != state.snapshots.end() && snap_it->second.valid) {
        return snap_it->second;
    }
    LspDocumentSnapshot empty_snap{};
    empty_snap.valid = false;
    return empty_snap;
}

struct CompletionItemEntry {
    std::string label;
    int64_t kind = 1;
    std::string detail;
    std::string insert_text;
    std::string documentation;
};

struct CallSiteContext {
    bool inside_call = false;
    bool has_receiver = false;
    std::string receiver_name;
    std::string callee_name;
    uint32_t active_param = 0;
    bool at_arg_name_position = false;
    std::string arg_prefix;
};

inline CallSiteContext parse_enclosing_call_site(std::string_view line_prefix) {
    CallSiteContext ctx{};
    std::string prefix_str(line_prefix);
    const LexResult lex_res = tokenize(prefix_str, "line.cb");
    std::vector<Token> toks;
    for (const auto& t : lex_res.tokens) {
        if (t.kind != "EOF" && t.kind != "NEWLINE") {
            toks.push_back(t);
        }
    }
    if (toks.empty()) {
        return ctx;
    }

    int paren_depth = 0;
    int bracket_depth = 0;
    int brace_depth = 0;
    size_t open_paren_idx = toks.size();

    size_t idx = toks.size();
    while (idx > 0) {
        idx -= 1;
        const Token& t = toks[idx];
        if (t.kind == "RPAREN") {
            paren_depth += 1;
        } else if (t.kind == "LPAREN") {
            if (paren_depth > 0) {
                paren_depth -= 1;
            } else {
                open_paren_idx = idx;
                break;
            }
        } else if (t.kind == "RBRACKET") {
            bracket_depth += 1;
        } else if (t.kind == "LBRACKET") {
            if (bracket_depth > 0) {
                bracket_depth -= 1;
            }
        } else if (t.kind == "RBRACE") {
            brace_depth += 1;
        } else if (t.kind == "LBRACE") {
            if (brace_depth > 0) {
                brace_depth -= 1;
            }
        }
    }

    if (open_paren_idx == toks.size() || open_paren_idx == 0) {
        return ctx;
    }

    const Token& callee_tok = toks[open_paren_idx - 1];
    if (callee_tok.kind != "IDENT" && callee_tok.kind != "TYPE_KW" &&
        !(callee_tok.kind == "KEYWORD" && callee_tok.value == "error")) {
        return ctx;
    }

    ctx.inside_call = true;
    ctx.callee_name = callee_tok.value;
    if (open_paren_idx >= 3 && toks[open_paren_idx - 2].kind == "DOT") {
        const Token& recv_tok = toks[open_paren_idx - 3];
        if (recv_tok.kind == "IDENT" || recv_tok.kind == "TYPE_KW") {
            ctx.has_receiver = true;
            ctx.receiver_name = recv_tok.value;
        }
    }

    int inner_paren = 0;
    int inner_bracket = 0;
    int inner_brace = 0;
    uint32_t comma_count = 0;
    size_t last_delimiter_idx = open_paren_idx;

    for (size_t k = open_paren_idx + 1; k < toks.size(); ++k) {
        const Token& t = toks[k];
        if (t.kind == "LPAREN") {
            inner_paren += 1;
        } else if (t.kind == "RPAREN") {
            if (inner_paren > 0) {
                inner_paren -= 1;
            }
        } else if (t.kind == "LBRACKET") {
            inner_bracket += 1;
        } else if (t.kind == "RBRACKET") {
            if (inner_bracket > 0) {
                inner_bracket -= 1;
            }
        } else if (t.kind == "LBRACE") {
            inner_brace += 1;
        } else if (t.kind == "RBRACE") {
            if (inner_brace > 0) {
                inner_brace -= 1;
            }
        } else if (t.kind == "COMMA" && inner_paren == 0 && inner_bracket == 0 && inner_brace == 0) {
            comma_count += 1;
            last_delimiter_idx = k;
        }
    }

    ctx.active_param = comma_count;

    const size_t rem_tokens = toks.size() - (last_delimiter_idx + 1);
    if (rem_tokens == 0) {
        ctx.at_arg_name_position = true;
        ctx.arg_prefix = "";
    } else if (rem_tokens == 1) {
        const Token& last_tok = toks.back();
        const bool ends_with_ident_char = !line_prefix.empty() && is_ident_char(line_prefix.back());
        if (ends_with_ident_char && (last_tok.kind == "IDENT" || last_tok.kind == "TYPE_KW")) {
            ctx.at_arg_name_position = true;
            ctx.arg_prefix = last_tok.value;
        }
    }

    return ctx;
}

struct CompletionCursorAnalysis {
    bool is_dot = false;
    std::string dot_receiver;
    std::string prefix;
    bool is_after_as = false;
    CallSiteContext call_ctx;
};

inline CompletionCursorAnalysis analyze_completion_cursor(std::string_view line_prefix) {
    CompletionCursorAnalysis res{};
    res.call_ctx = parse_enclosing_call_site(line_prefix);

    size_t cursor_idx = line_prefix.size();
    size_t word_start = cursor_idx;
    while (word_start > 0 && is_ident_char(line_prefix[word_start - 1])) {
        word_start -= 1;
    }
    res.prefix = std::string(line_prefix.substr(word_start, cursor_idx - word_start));

    size_t before_word = word_start;
    while (before_word > 0 && (line_prefix[before_word - 1] == ' ' || line_prefix[before_word - 1] == '\t')) {
        before_word -= 1;
    }

    if (word_start > 0 && line_prefix[word_start - 1] == '.') {
        size_t dot_pos = word_start - 1;
        size_t recv_end = dot_pos;
        while (recv_end > 0 && (line_prefix[recv_end - 1] == ' ' || line_prefix[recv_end - 1] == '\t')) {
            recv_end -= 1;
        }
        size_t recv_start = recv_end;
        while (recv_start > 0 && is_ident_char(line_prefix[recv_start - 1])) {
            recv_start -= 1;
        }
        if (recv_end > recv_start) {
            res.is_dot = true;
            res.dot_receiver = std::string(line_prefix.substr(recv_start, recv_end - recv_start));
            return res;
        }
    }

    if (before_word >= 2) {
        size_t kw_end = before_word;
        size_t kw_start = kw_end;
        while (kw_start > 0 && is_ident_char(line_prefix[kw_start - 1])) {
            kw_start -= 1;
        }
        const std::string_view prev_word = line_prefix.substr(kw_start, kw_end - kw_start);
        if (prev_word == "as" && before_word < word_start) {
            res.is_after_as = true;
            return res;
        }
    }

    return res;
}

inline bool matches_completion_prefix(std::string_view candidate, std::string_view prefix) {
    if (prefix.empty()) {
        return true;
    }
    if (candidate.size() < prefix.size()) {
        return false;
    }
    return candidate.substr(0, prefix.size()) == prefix;
}

inline void add_unique_completion_item(
    std::vector<CompletionItemEntry>& items,
    std::unordered_map<std::string, bool>& seen,
    const CompletionItemEntry& entry
) {
    const std::string key = std::to_string(entry.kind) + ":" + entry.label + ":" + entry.insert_text;
    if (seen.find(key) != seen.end()) {
        return;
    }
    seen[key] = true;
    items.push_back(entry);
}

inline void add_completion_entry(
    std::vector<CompletionItemEntry>& items,
    std::unordered_map<std::string, bool>& seen,
    std::string_view label,
    int64_t kind,
    std::string_view detail,
    std::string_view insert_text = "",
    std::string_view documentation = ""
) {
    std::string ins(label);
    if (!insert_text.empty()) {
        ins = std::string(insert_text);
    }
    add_unique_completion_item(
        items,
        seen,
        CompletionItemEntry{std::string(label), kind, std::string(detail), ins, std::string(documentation)}
    );
}

inline void collect_module_symbol_completions(
    const LspDocumentSnapshot& snap,
    std::string_view target_module_id,
    std::string_view prefix,
    bool require_pub,
    bool include_functions,
    bool include_module_aliases,
    std::vector<CompletionItemEntry>& items,
    std::unordered_map<std::string, bool>& seen
) {
    if (include_functions) {
        for (const auto& kv : snap.symbols.functions) {
            const FnSymbol& fn = kv.second;
            if (fn.module_id != target_module_id || (require_pub && !fn.is_pub)) {
                continue;
            }
            if (!require_pub && is_builtin_symbol_name(fn.name)) {
                continue;
            }
            if (matches_completion_prefix(fn.name, prefix)) {
                add_completion_entry(
                    items,
                    seen,
                    fn.name,
                    3,
                    "fn" + format_fn_detail_signature(snap.arena, fn.params, fn.return_type, fn.is_fallible),
                    fn.name,
                    format_fn_hover_markdown(snap.arena, fn)
                );
            }
        }
    }
    for (const auto& kv : snap.symbols.types) {
        const TypeSymbol& ts = kv.second;
        if (ts.module_id != target_module_id || (require_pub && !ts.is_pub)) {
            continue;
        }
        if (!require_pub && is_builtin_type_name(ts.name)) {
            continue;
        }
        if (matches_completion_prefix(ts.name, prefix)) {
            add_completion_entry(items, seen, ts.name, 22, "type " + ts.name, ts.name, format_type_hover_markdown(snap.arena, ts));
        }
    }
    for (const auto& kv : snap.symbols.aliases) {
        const AliasSymbol& al = kv.second;
        if (al.module_id != target_module_id || (require_pub && !al.is_pub)) {
            continue;
        }
        if (matches_completion_prefix(al.name, prefix)) {
            add_completion_entry(
                items,
                seen,
                al.name,
                25,
                "alias " + al.name + " = " + format_cleanbasic_type(snap.arena, al.target_type),
                al.name,
                format_alias_hover_markdown(snap.arena, snap.symbols, al)
            );
        }
    }
    if (include_module_aliases) {
        const auto mod_map_it = snap.symbols.modules.find(std::string(target_module_id));
        if (mod_map_it != snap.symbols.modules.end()) {
            for (const auto& kv : mod_map_it->second) {
                if (matches_completion_prefix(kv.first, prefix)) {
                    add_completion_entry(items, seen, kv.first, 9, "module " + kv.second, kv.first);
                }
            }
        }
    }
}

inline std::optional<TypeId> resolve_variable_type_in_fn(
    const LspDocumentSnapshot& snap,
    const std::optional<std::string>& enclosing_fn_key,
    std::string_view var_name
) {
    if (!enclosing_fn_key.has_value()) {
        return std::nullopt;
    }
    const auto scope_it = snap.symbols.fn_scopes.find(enclosing_fn_key.value());
    if (scope_it == snap.symbols.fn_scopes.end()) {
        return std::nullopt;
    }
    const auto var_it = scope_it->second.find(std::string(var_name));
    if (var_it == scope_it->second.end()) {
        return std::nullopt;
    }
    return var_it->second.type_id;
}

inline std::string format_ufcs_method_detail(
    const AstArena& arena,
    const FnSymbol& fn
) {
    std::vector<Param> remaining_params;
    for (size_t i = 1; i < fn.params.size(); ++i) {
        remaining_params.push_back(fn.params[i]);
    }
    return "ufcs fn " + fn.name + format_fn_detail_signature(arena, remaining_params, fn.return_type, fn.is_fallible);
}

inline void collect_ufcs_completions_for_type(
    const LspDocumentSnapshot& snap,
    std::string_view current_module_id,
    TypeId recv_tid,
    std::string_view prefix,
    std::vector<CompletionItemEntry>& items,
    std::unordered_map<std::string, bool>& seen
) {
    if (recv_tid == INVALID_ID || recv_tid >= snap.arena.types.size()) {
        return;
    }
    const TypeId resolved_recv = resolve_alias(snap.arena, snap.symbols, current_module_id, recv_tid);
    if (resolved_recv == INVALID_ID || resolved_recv >= snap.arena.types.size()) {
        return;
    }
    const std::string& recv_type_key = snap.arena.types[resolved_recv].name;
    const auto [recv_mod_id, recv_base] = split_symbol_key(recv_type_key);

    std::unordered_map<std::string, bool> allowed_imported_mods;
    const auto mod_map_it = snap.symbols.modules.find(std::string(current_module_id));
    if (mod_map_it != snap.symbols.modules.end()) {
        for (const auto& kv : mod_map_it->second) {
            allowed_imported_mods[kv.second] = true;
        }
    }

    for (const auto& kv : snap.symbols.functions) {
        const FnSymbol& fn = kv.second;
        if (fn.params.empty() || !matches_completion_prefix(fn.name, prefix)) {
            continue;
        }
        const bool in_current_mod = (fn.module_id == current_module_id);
        const bool in_defining_mod = (!recv_mod_id.empty() && fn.module_id == recv_mod_id && fn.is_pub);
        const bool in_imported_mod = (allowed_imported_mods.find(fn.module_id) != allowed_imported_mods.end() && fn.is_pub);
        if (!in_current_mod && !in_defining_mod && !in_imported_mod) {
            continue;
        }

        const TypeId p0_resolved = resolve_alias(snap.arena, snap.symbols, fn.module_id, fn.params[0].type_id);
        if (p0_resolved == INVALID_ID || p0_resolved >= snap.arena.types.size()) {
            continue;
        }
        if (snap.arena.types[p0_resolved].name != recv_type_key) {
            continue;
        }

        add_completion_entry(
            items,
            seen,
            fn.name,
            2,
            format_ufcs_method_detail(snap.arena, fn),
            fn.name,
            format_fn_hover_markdown(snap.arena, fn)
        );
    }
}

inline void collect_dot_completions(
    const LspDocumentSnapshot& snap,
    std::string_view current_module_id,
    const std::optional<std::string>& enclosing_fn_key,
    std::string_view receiver_name,
    std::string_view prefix,
    std::vector<CompletionItemEntry>& items,
    std::unordered_map<std::string, bool>& seen
) {
    const auto var_tid_opt = resolve_variable_type_in_fn(snap, enclosing_fn_key, receiver_name);

    // 1. Module alias completion (if not shadowed by local variable)
    if (!var_tid_opt.has_value()) {
        const auto mod_map_it = snap.symbols.modules.find(std::string(current_module_id));
        if (mod_map_it != snap.symbols.modules.end()) {
            const auto alias_it = mod_map_it->second.find(std::string(receiver_name));
            if (alias_it != mod_map_it->second.end()) {
                collect_module_symbol_completions(snap, alias_it->second, prefix, true, true, false, items, seen);
                return;
            }
        }
        return;
    }

    const TypeId recv_tid = resolve_alias(snap.arena, snap.symbols, current_module_id, var_tid_opt.value());
    if (recv_tid == INVALID_ID || recv_tid >= snap.arena.types.size()) {
        return;
    }

    const TypeNode& recv_node = snap.arena.types[recv_tid];

    // 2. Built-in 'err' fields (.msg, .code)
    if (recv_tid == snap.symbols.type_err || recv_node.name == "err") {
        if (matches_completion_prefix("msg", prefix)) {
            add_completion_entry(items, seen, "msg", 5, "as str");
        }
        if (matches_completion_prefix("code", prefix)) {
            add_completion_entry(items, seen, "code", 5, "as int");
        }
        collect_ufcs_completions_for_type(snap, current_module_id, recv_tid, prefix, items, seen);
        return;
    }

    // 3. Built-in 'list[T]' methods (.push, .append)
    if (recv_node.name == "list") {
        std::string elem_type = "any";
        if (!recv_node.type_args.empty()) {
            elem_type = format_cleanbasic_type(snap.arena, recv_node.type_args[0]);
        }
        if (matches_completion_prefix("push", prefix)) {
            add_completion_entry(items, seen, "push", 2, "fn(item as " + elem_type + ")");
        }
        if (matches_completion_prefix("append", prefix)) {
            add_completion_entry(items, seen, "append", 2, "fn(item as " + elem_type + ")");
        }
        collect_ufcs_completions_for_type(snap, current_module_id, recv_tid, prefix, items, seen);
        return;
    }

    // 4. User Struct fields + UFCS methods
    const auto ts_opt = resolve_type_symbol(snap.arena, snap.symbols, current_module_id, recv_tid);
    if (ts_opt.has_value()) {
        const TypeSymbol& ts = ts_opt.value();
        for (const auto& f : ts.field_order) {
            if (!matches_completion_prefix(f.name, prefix)) {
                continue;
            }
            TypeId ftid = f.type_id;
            const auto fit = ts.fields.find(f.name);
            if (fit != ts.fields.end()) {
                ftid = fit->second;
            }
            add_completion_entry(items, seen, f.name, 5, "as " + format_cleanbasic_type(snap.arena, ftid));
        }
    }

    collect_ufcs_completions_for_type(snap, current_module_id, recv_tid, prefix, items, seen);
}

inline std::optional<TypeSymbol> resolve_call_struct_symbol(
    const LspDocumentSnapshot& snap,
    std::string_view current_module_id,
    const std::optional<std::string>& enclosing_fn_key,
    const CallSiteContext& call_ctx
) {
    std::string lookup_name = call_ctx.callee_name;
    if (call_ctx.has_receiver) {
        if (resolve_variable_type_in_fn(snap, enclosing_fn_key, call_ctx.receiver_name).has_value()) {
            return std::nullopt;
        }
        lookup_name = call_ctx.receiver_name + "." + call_ctx.callee_name;
    }
    return resolve_type_symbol_by_name(snap.arena, snap.symbols, current_module_id, lookup_name, call_ctx.has_receiver);
}

struct ResolvedCallFunction {
    FnSymbol fn;
    bool is_ufcs = false;
};

inline std::optional<ResolvedCallFunction> resolve_call_fn_symbol(
    const LspDocumentSnapshot& snap,
    std::string_view current_module_id,
    const std::optional<std::string>& enclosing_fn_key,
    const CallSiteContext& call_ctx
) {
    if (call_ctx.has_receiver) {
        const auto var_tid_opt = resolve_variable_type_in_fn(snap, enclosing_fn_key, call_ctx.receiver_name);
        if (!var_tid_opt.has_value()) {
            const auto mod_map_it = snap.symbols.modules.find(std::string(current_module_id));
            if (mod_map_it != snap.symbols.modules.end()) {
                const auto alias_it = mod_map_it->second.find(call_ctx.receiver_name);
                if (alias_it != mod_map_it->second.end()) {
                    const std::string fn_key = symbol_key(alias_it->second, call_ctx.callee_name);
                    const auto fn_it = snap.symbols.functions.find(fn_key);
                    if (fn_it != snap.symbols.functions.end() && fn_it->second.is_pub) {
                        return ResolvedCallFunction{fn_it->second, false};
                    }
                }
            }
        } else {
            const auto ufcs_opt = resolve_ufcs_fn(snap.arena, snap.symbols, current_module_id, var_tid_opt.value(), call_ctx.callee_name);
            if (ufcs_opt.has_value()) {
                return ResolvedCallFunction{ufcs_opt.value(), true};
            }
        }
        return std::nullopt;
    }

    const std::string fn_key = resolve_symbol_key(snap.symbols, current_module_id, call_ctx.callee_name);
    const auto fn_it = snap.symbols.functions.find(fn_key);
    if (fn_it != snap.symbols.functions.end()) {
        return ResolvedCallFunction{fn_it->second, false};
    }
    return std::nullopt;
}

inline void collect_named_argument_completions(
    const LspDocumentSnapshot& snap,
    std::string_view current_module_id,
    const std::optional<std::string>& enclosing_fn_key,
    const CallSiteContext& call_ctx,
    std::vector<CompletionItemEntry>& items,
    std::unordered_map<std::string, bool>& seen
) {
    if (!call_ctx.inside_call || !call_ctx.at_arg_name_position) {
        return;
    }

    if (!call_ctx.has_receiver && call_ctx.callee_name == "error") {
        if (matches_completion_prefix("code", call_ctx.arg_prefix)) {
            add_completion_entry(items, seen, "code=", 6, "as int");
        }
        return;
    }

    const auto ts_opt = resolve_call_struct_symbol(snap, current_module_id, enclosing_fn_key, call_ctx);
    if (ts_opt.has_value()) {
        const TypeSymbol& ts = ts_opt.value();
        for (const auto& f : ts.field_order) {
            if (!matches_completion_prefix(f.name, call_ctx.arg_prefix)) {
                continue;
            }
            TypeId ftid = f.type_id;
            const auto fit = ts.fields.find(f.name);
            if (fit != ts.fields.end()) {
                ftid = fit->second;
            }
            add_completion_entry(items, seen, f.name + "=", 5, "as " + format_cleanbasic_type(snap.arena, ftid));
        }
        return;
    }

    const auto fn_opt = resolve_call_fn_symbol(snap, current_module_id, enclosing_fn_key, call_ctx);
    if (fn_opt.has_value()) {
        const FnSymbol& fn = fn_opt.value().fn;
        size_t start_idx = 0;
        if (fn_opt.value().is_ufcs && !fn.params.empty()) {
            start_idx = 1;
        }
        for (size_t i = start_idx; i < fn.params.size(); ++i) {
            const Param& p = fn.params[i];
            if (matches_completion_prefix(p.name, call_ctx.arg_prefix)) {
                add_completion_entry(items, seen, p.name + "=", 6, "as " + format_cleanbasic_type(snap.arena, p.type_id));
            }
        }
    }
}

inline void collect_type_annotation_completions(
    const LspDocumentSnapshot& snap,
    std::string_view current_module_id,
    std::string_view prefix,
    std::vector<CompletionItemEntry>& items,
    std::unordered_map<std::string, bool>& seen
) {
    const std::vector<std::string> builtin_types = {
        "int", "float", "str", "bool", "cptr", "err", "list", "dict"
    };
    for (const auto& bt : builtin_types) {
        if (matches_completion_prefix(bt, prefix)) {
            add_completion_entry(items, seen, bt, 14, "built-in type");
        }
    }
    if (snap.valid) {
        collect_module_symbol_completions(snap, current_module_id, prefix, false, false, true, items, seen);
    }
}

inline void collect_general_scope_completions(
    const LspDocumentSnapshot& snap,
    std::string_view current_module_id,
    const std::optional<std::string>& enclosing_fn_key,
    std::string_view prefix,
    std::vector<CompletionItemEntry>& items,
    std::unordered_map<std::string, bool>& seen
) {
    if (snap.valid) {
        if (enclosing_fn_key.has_value()) {
            const auto scope_it = snap.symbols.fn_scopes.find(enclosing_fn_key.value());
            if (scope_it != snap.symbols.fn_scopes.end()) {
                for (const auto& kv : scope_it->second) {
                    const VarSymbol& vs = kv.second;
                    if (matches_completion_prefix(vs.name, prefix)) {
                        add_completion_entry(
                            items,
                            seen,
                            vs.name,
                            6,
                            "as " + format_cleanbasic_type(snap.arena, vs.type_id),
                            vs.name,
                            format_var_hover_markdown(snap.arena, vs)
                        );
                    }
                }
            }
        }
        collect_module_symbol_completions(snap, current_module_id, prefix, false, true, true, items, seen);
    }

    const std::vector<std::pair<std::string, std::string>> builtins = {
        {"print", "fn print(values...)"},
        {"len", "fn len(container as any) as int"},
        {"str", "fn str(val as any) as str"},
        {"error", "fn error(msg as str, [code=1]) as err"}
    };
    for (const auto& b : builtins) {
        if (matches_completion_prefix(b.first, prefix)) {
            std::string doc;
            const auto doc_opt = format_builtin_hover_markdown(b.first);
            if (doc_opt.has_value()) {
                doc = doc_opt.value();
            }
            add_completion_entry(items, seen, b.first, 3, b.second, b.first, doc);
        }
    }

    const std::vector<std::string> keywords = {
        "fn", "end fn", "pub", "extern", "type", "end type", "alias", "import",
        "import header", "import link", "dim", "const", "byref", "as",
        "if", "else if", "else", "end if", "for", "to", "step", "in", "end for",
        "select case", "case", "case else", "end select", "on error", "end error",
        "defer", "end defer", "return", "break", "can error", "and", "or", "not",
        "true", "false", "null"
    };
    for (const auto& kw : keywords) {
        if (matches_completion_prefix(kw, prefix)) {
            add_completion_entry(items, seen, kw, 14, "keyword");
        }
    }
}

inline void handle_completion_request(
    LspServerState& state,
    const JsonArena& req_arena,
    JsonId req_id,
    JsonId params_id,
    std::ostream& out
) {
    const LspTextPositionParams doc = parse_doc_position_params(req_arena, params_id);
    JsonArena out_arena;
    const JsonId items_arr = json_add_array(out_arena);

    if (!doc.valid) {
        write_jsonrpc_result(req_arena, req_id, out_arena, items_arr, out);
        return;
    }

    const std::string source_text = read_document_text(state, doc.uri);
    const LspDocumentSnapshot snap = get_or_build_scrubbed_snapshot(state, doc.uri, source_text, doc.line_0based, doc.char_0based);
    const CursorLineSlice slice = extract_cursor_line_slice(source_text, doc.line_0based, doc.char_0based);
    const CompletionCursorAnalysis cursor_info = analyze_completion_cursor(slice.prefix_to_cursor);

    std::string current_mod_id;
    std::optional<std::string> enclosing_fn_key;
    if (snap.valid) {
        current_mod_id = find_module_id_for_file(snap.arena, snap.program, doc.file_path);
        enclosing_fn_key = find_enclosing_fn_key(snap.arena, snap.program, doc.file_path, doc.line_1based);
    }

    std::vector<CompletionItemEntry> entries;
    std::unordered_map<std::string, bool> seen;

    if (cursor_info.is_dot) {
        if (snap.valid) {
            collect_dot_completions(
                snap, current_mod_id, enclosing_fn_key, cursor_info.dot_receiver, cursor_info.prefix, entries, seen
            );
        }
    } else if (cursor_info.is_after_as) {
        collect_type_annotation_completions(snap, current_mod_id, cursor_info.prefix, entries, seen);
    } else {
        if (snap.valid && cursor_info.call_ctx.inside_call && cursor_info.call_ctx.at_arg_name_position) {
            collect_named_argument_completions(snap, current_mod_id, enclosing_fn_key, cursor_info.call_ctx, entries, seen);
        }
        collect_general_scope_completions(snap, current_mod_id, enclosing_fn_key, cursor_info.prefix, entries, seen);
    }

    for (const auto& entry : entries) {
        const JsonId item_obj = json_add_object(out_arena);
        json_object_set(out_arena, item_obj, "label", json_add_string(out_arena, entry.label));
        json_object_set(out_arena, item_obj, "kind", json_add_int(out_arena, entry.kind));
        if (!entry.detail.empty()) {
            json_object_set(out_arena, item_obj, "detail", json_add_string(out_arena, entry.detail));
        }
        if (!entry.insert_text.empty()) {
            json_object_set(out_arena, item_obj, "insertText", json_add_string(out_arena, entry.insert_text));
        }
        if (!entry.documentation.empty()) {
            const JsonId doc_obj = json_add_object(out_arena);
            json_object_set(out_arena, doc_obj, "kind", json_add_string(out_arena, "markdown"));
            json_object_set(out_arena, doc_obj, "value", json_add_string(out_arena, entry.documentation));
            json_object_set(out_arena, item_obj, "documentation", doc_obj);
        }
        json_array_push(out_arena, items_arr, item_obj);
    }

    write_jsonrpc_result(req_arena, req_id, out_arena, items_arr, out);
}

struct SignatureHelpData {
    std::string label;
    std::string documentation;
    std::vector<std::string> param_labels;
    uint32_t active_parameter = 0;
};

inline std::optional<SignatureHelpData> resolve_signature_help(
    const LspDocumentSnapshot& snap,
    std::string_view current_module_id,
    const std::optional<std::string>& enclosing_fn_key,
    const CallSiteContext& call_ctx
) {
    if (!call_ctx.inside_call) {
        return std::nullopt;
    }

    if (!call_ctx.has_receiver) {
        if (call_ctx.callee_name == "error") {
            uint32_t act = call_ctx.active_param;
            if (act > 1) {
                act = 1;
            }
            std::string doc;
            const auto doc_opt = format_builtin_hover_markdown("error");
            if (doc_opt.has_value()) {
                doc = doc_opt.value();
            }
            return SignatureHelpData{"fn error(msg as str, code as int) as err", doc, {"msg as str", "code as int"}, act};
        }
        if (call_ctx.callee_name == "len") {
            return SignatureHelpData{"fn len(container as any) as int", "", {"container as any"}, 0};
        }
        if (call_ctx.callee_name == "str") {
            return SignatureHelpData{"fn str(val as any) as str", "", {"val as any"}, 0};
        }
        if (call_ctx.callee_name == "print") {
            return SignatureHelpData{"fn print(values...)", "", {"values..."}, 0};
        }
    }

    if (!snap.valid) {
        return std::nullopt;
    }

    // 3. Struct constructor
    const auto ts_opt = resolve_call_struct_symbol(snap, current_module_id, enclosing_fn_key, call_ctx);
    if (ts_opt.has_value()) {
        const TypeSymbol& ts = ts_opt.value();
        SignatureHelpData sig{};
        sig.label = ts.name + "(";
        for (size_t i = 0; i < ts.field_order.size(); ++i) {
            if (i > 0) {
                sig.label += ", ";
            }
            const FieldDecl& f = ts.field_order[i];
            TypeId ftid = f.type_id;
            const auto fit = ts.fields.find(f.name);
            if (fit != ts.fields.end()) {
                ftid = fit->second;
            }
            const std::string p_label = f.name + " as " + format_cleanbasic_type(snap.arena, ftid);
            sig.label += p_label;
            sig.param_labels.push_back(p_label);
        }
        sig.label += ") as " + ts.name;
        sig.documentation = format_type_hover_markdown(snap.arena, ts);
        sig.active_parameter = call_ctx.active_param;
        if (!sig.param_labels.empty() && sig.active_parameter >= sig.param_labels.size()) {
            sig.active_parameter = static_cast<uint32_t>(sig.param_labels.size() - 1);
        }
        return sig;
    }

    // 4. Regular function or UFCS function call
    const auto fn_opt = resolve_call_fn_symbol(snap, current_module_id, enclosing_fn_key, call_ctx);
    if (fn_opt.has_value()) {
        const FnSymbol& fn = fn_opt.value().fn;
        const bool is_ufcs = fn_opt.value().is_ufcs;

        std::vector<Param> visible_params;
        size_t start_idx = 0;
        if (is_ufcs && !fn.params.empty()) {
            start_idx = 1;
        }
        for (size_t i = start_idx; i < fn.params.size(); ++i) {
            visible_params.push_back(fn.params[i]);
        }

        SignatureHelpData sig{};
        sig.label = "fn " + fn.name + format_fn_detail_signature(snap.arena, visible_params, fn.return_type, fn.is_fallible);
        sig.documentation = format_fn_hover_markdown(snap.arena, fn);
        for (const auto& p : visible_params) {
            std::string p_label;
            if (p.is_byref) {
                p_label += "byref ";
            }
            p_label += p.name + " as " + format_cleanbasic_type(snap.arena, p.type_id);
            sig.param_labels.push_back(p_label);
        }
        sig.active_parameter = call_ctx.active_param;
        if (!sig.param_labels.empty() && sig.active_parameter >= sig.param_labels.size()) {
            sig.active_parameter = static_cast<uint32_t>(sig.param_labels.size() - 1);
        }
        return sig;
    }

    return std::nullopt;
}

inline void handle_signature_help_request(
    LspServerState& state,
    const JsonArena& req_arena,
    JsonId req_id,
    JsonId params_id,
    std::ostream& out
) {
    const LspTextPositionParams doc = parse_doc_position_params(req_arena, params_id);
    JsonArena out_arena;
    if (!doc.valid) {
        write_jsonrpc_result(req_arena, req_id, out_arena, json_add_null(out_arena), out);
        return;
    }

    const std::string source_text = read_document_text(state, doc.uri);
    const LspDocumentSnapshot snap = get_or_build_scrubbed_snapshot(state, doc.uri, source_text, doc.line_0based, doc.char_0based);
    const CursorLineSlice slice = extract_cursor_line_slice(source_text, doc.line_0based, doc.char_0based);
    const CallSiteContext call_ctx = parse_enclosing_call_site(slice.prefix_to_cursor);

    std::string current_mod_id;
    std::optional<std::string> enclosing_fn_key;
    if (snap.valid) {
        current_mod_id = find_module_id_for_file(snap.arena, snap.program, doc.file_path);
        enclosing_fn_key = find_enclosing_fn_key(snap.arena, snap.program, doc.file_path, doc.line_1based);
    }

    const auto sig_opt = resolve_signature_help(snap, current_mod_id, enclosing_fn_key, call_ctx);
    if (!sig_opt.has_value()) {
        write_jsonrpc_result(req_arena, req_id, out_arena, json_add_null(out_arena), out);
        return;
    }

    const SignatureHelpData& sig = sig_opt.value();
    const JsonId sig_info = json_add_object(out_arena);
    json_object_set(out_arena, sig_info, "label", json_add_string(out_arena, sig.label));
    if (!sig.documentation.empty()) {
        const JsonId doc_obj = json_add_object(out_arena);
        json_object_set(out_arena, doc_obj, "kind", json_add_string(out_arena, "markdown"));
        json_object_set(out_arena, doc_obj, "value", json_add_string(out_arena, sig.documentation));
        json_object_set(out_arena, sig_info, "documentation", doc_obj);
    }

    const JsonId params_arr = json_add_array(out_arena);
    for (const auto& p_label : sig.param_labels) {
        const JsonId p_obj = json_add_object(out_arena);
        json_object_set(out_arena, p_obj, "label", json_add_string(out_arena, p_label));
        json_array_push(out_arena, params_arr, p_obj);
    }
    json_object_set(out_arena, sig_info, "parameters", params_arr);

    const JsonId sigs_arr = json_add_array(out_arena);
    json_array_push(out_arena, sigs_arr, sig_info);

    const JsonId result_obj = json_add_object(out_arena);
    json_object_set(out_arena, result_obj, "signatures", sigs_arr);
    json_object_set(out_arena, result_obj, "activeSignature", json_add_int(out_arena, 0));
    json_object_set(out_arena, result_obj, "activeParameter", json_add_int(out_arena, static_cast<int64_t>(sig.active_parameter)));

    write_jsonrpc_result(req_arena, req_id, out_arena, result_obj, out);
}

inline void handle_lsp_message(LspServerState& state, std::string_view payload, std::ostream& out) {
    const JsonParseResult parsed = parse_json(payload);
    if (!parsed.ok) {
        return;
    }

    const JsonId method_id = json_get_field(parsed.arena, parsed.root, "method");
    const auto method_opt = json_get_string(parsed.arena, method_id);
    if (!method_opt.has_value()) {
        return;
    }

    const std::string& method = method_opt.value();
    const JsonId req_id = json_get_field(parsed.arena, parsed.root, "id");
    const JsonId params_id = json_get_field(parsed.arena, parsed.root, "params");

    if (method == "initialize") {
        state.initialized = true;
        JsonArena out_arena;

        const JsonId caps = json_add_object(out_arena);
        json_object_set(out_arena, caps, "textDocumentSync", json_add_int(out_arena, 1));
        json_object_set(out_arena, caps, "documentSymbolProvider", json_add_bool(out_arena, true));
        json_object_set(out_arena, caps, "definitionProvider", json_add_bool(out_arena, true));
        json_object_set(out_arena, caps, "hoverProvider", json_add_bool(out_arena, true));

        const JsonId comp_provider = json_add_object(out_arena);
        const JsonId comp_triggers = json_add_array(out_arena);
        json_array_push(out_arena, comp_triggers, json_add_string(out_arena, "."));
        json_object_set(out_arena, comp_provider, "triggerCharacters", comp_triggers);
        json_object_set(out_arena, caps, "completionProvider", comp_provider);

        const JsonId sig_provider = json_add_object(out_arena);
        const JsonId sig_triggers = json_add_array(out_arena);
        json_array_push(out_arena, sig_triggers, json_add_string(out_arena, "("));
        json_array_push(out_arena, sig_triggers, json_add_string(out_arena, ","));
        json_object_set(out_arena, sig_provider, "triggerCharacters", sig_triggers);
        json_object_set(out_arena, caps, "signatureHelpProvider", sig_provider);

        const JsonId server_info = json_add_object(out_arena);
        json_object_set(out_arena, server_info, "name", json_add_string(out_arena, "cleanbasic-lsp"));
        json_object_set(out_arena, server_info, "version", json_add_string(out_arena, "0.3.0"));

        const JsonId result = json_add_object(out_arena);
        json_object_set(out_arena, result, "capabilities", caps);
        json_object_set(out_arena, result, "serverInfo", server_info);

        write_jsonrpc_result(parsed.arena, req_id, out_arena, result, out);
        return;
    }

    if (method == "initialized") {
        state.initialized = true;
        return;
    }

    if (method == "shutdown") {
        state.shutdown_requested = true;
        JsonArena out_arena;
        write_jsonrpc_result(parsed.arena, req_id, out_arena, json_add_null(out_arena), out);
        return;
    }

    if (method == "exit") {
        state.should_exit = true;
        if (state.shutdown_requested) {
            state.exit_code = 0;
        } else {
            state.exit_code = 1;
        }
        return;
    }

    if (method == "textDocument/didOpen") {
        const JsonId text_doc = json_get_field(parsed.arena, params_id, "textDocument");
        const auto uri_opt = json_get_string(parsed.arena, json_get_field(parsed.arena, text_doc, "uri"));
        const auto text_opt = json_get_string(parsed.arena, json_get_field(parsed.arena, text_doc, "text"));
        if (uri_opt.has_value() && text_opt.has_value()) {
            state.open_documents[uri_opt.value()] = text_opt.value();
            publish_document_diagnostics(state, uri_opt.value(), text_opt.value(), out);
        }
        return;
    }

    if (method == "textDocument/didChange") {
        const JsonId text_doc = json_get_field(parsed.arena, params_id, "textDocument");
        const auto uri_opt = json_get_string(parsed.arena, json_get_field(parsed.arena, text_doc, "uri"));
        const JsonId changes_id = json_get_field(parsed.arena, params_id, "contentChanges");
        if (uri_opt.has_value() && changes_id < parsed.arena.nodes.size() &&
            std::holds_alternative<JsonArray>(parsed.arena.nodes[changes_id])) {
            const auto& arr = std::get<JsonArray>(parsed.arena.nodes[changes_id]);
            bool updated = false;
            std::string latest_text;
            for (const JsonId elem_id : arr.elements) {
                const auto text_opt = json_get_string(parsed.arena, json_get_field(parsed.arena, elem_id, "text"));
                if (text_opt.has_value()) {
                    latest_text = text_opt.value();
                    updated = true;
                }
            }
            if (updated) {
                state.open_documents[uri_opt.value()] = latest_text;
                publish_document_diagnostics(state, uri_opt.value(), latest_text, out);
            }
        }
        return;
    }

    if (method == "textDocument/didClose") {
        const JsonId text_doc = json_get_field(parsed.arena, params_id, "textDocument");
        const auto uri_opt = json_get_string(parsed.arena, json_get_field(parsed.arena, text_doc, "uri"));
        if (uri_opt.has_value()) {
            state.open_documents.erase(uri_opt.value());
            state.snapshots.erase(uri_opt.value());
            publish_empty_diagnostics(uri_opt.value(), out);
        }
        return;
    }

    if (method == "textDocument/documentSymbol") {
        handle_document_symbol_request(state, parsed.arena, req_id, params_id, out);
        return;
    }

    if (method == "textDocument/hover") {
        handle_hover_request(state, parsed.arena, req_id, params_id, out);
        return;
    }

    if (method == "textDocument/definition") {
        handle_definition_request(state, parsed.arena, req_id, params_id, out);
        return;
    }

    if (method == "textDocument/completion") {
        handle_completion_request(state, parsed.arena, req_id, params_id, out);
        return;
    }

    if (method == "textDocument/signatureHelp") {
        handle_signature_help_request(state, parsed.arena, req_id, params_id, out);
        return;
    }

    if (req_id != JSON_INVALID_ID) {
        JsonArena out_arena;
        const JsonId root = json_add_object(out_arena);
        json_object_set(out_arena, root, "jsonrpc", json_add_string(out_arena, "2.0"));
        json_object_set(out_arena, root, "id", clone_json_scalar_or_null(parsed.arena, req_id, out_arena));

        const JsonId err_obj = json_add_object(out_arena);
        json_object_set(out_arena, err_obj, "code", json_add_int(out_arena, -32601));
        json_object_set(out_arena, err_obj, "message", json_add_string(out_arena, "Method not found: " + method));
        json_object_set(out_arena, root, "error", err_obj);

        write_jsonrpc_message(out, serialize_json(out_arena, root));
    }
}

inline int run_lsp_server(std::istream& in, std::ostream& out) {
    LspServerState state{};
    while (!state.should_exit) {
        const auto msg_opt = read_jsonrpc_message(in);
        if (!msg_opt.has_value()) {
            break;
        }
        handle_lsp_message(state, msg_opt.value(), out);
    }
    return state.exit_code;
}

}  // namespace cb

#endif  // CLEANBASIC_LSP_HPP
