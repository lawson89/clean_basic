#ifndef CLEANBASIC_LSP_HPP
#define CLEANBASIC_LSP_HPP

#include "src/cleanbasic/json.hpp"
#include "src/cleanbasic/pipeline.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <unordered_map>

namespace cb {

struct LspServerState {
    bool initialized = false;
    bool shutdown_requested = false;
    bool should_exit = false;
    int exit_code = 0;
    std::unordered_map<std::string, std::string> open_documents;
};

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
    const LspServerState& state,
    std::string_view uri,
    std::string_view text,
    std::ostream& out
) {
    const std::string file_path = uri_to_file_path(uri);
    const auto open_buffers = build_open_file_buffers(state);
    const CheckSourceResult check_res = check_source(text, file_path, open_buffers);

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
        const JsonId root = json_add_object(out_arena);
        json_object_set(out_arena, root, "jsonrpc", json_add_string(out_arena, "2.0"));
        json_object_set(out_arena, root, "id", clone_json_scalar_or_null(parsed.arena, req_id, out_arena));

        const JsonId caps = json_add_object(out_arena);
        json_object_set(out_arena, caps, "textDocumentSync", json_add_int(out_arena, 1));

        const JsonId server_info = json_add_object(out_arena);
        json_object_set(out_arena, server_info, "name", json_add_string(out_arena, "cleanbasic-lsp"));
        json_object_set(out_arena, server_info, "version", json_add_string(out_arena, "0.1.0"));

        const JsonId result = json_add_object(out_arena);
        json_object_set(out_arena, result, "capabilities", caps);
        json_object_set(out_arena, result, "serverInfo", server_info);
        json_object_set(out_arena, root, "result", result);

        write_jsonrpc_message(out, serialize_json(out_arena, root));
        return;
    }

    if (method == "initialized") {
        state.initialized = true;
        return;
    }

    if (method == "shutdown") {
        state.shutdown_requested = true;

        JsonArena out_arena;
        const JsonId root = json_add_object(out_arena);
        json_object_set(out_arena, root, "jsonrpc", json_add_string(out_arena, "2.0"));
        json_object_set(out_arena, root, "id", clone_json_scalar_or_null(parsed.arena, req_id, out_arena));
        json_object_set(out_arena, root, "result", json_add_null(out_arena));

        write_jsonrpc_message(out, serialize_json(out_arena, root));
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
            publish_empty_diagnostics(uri_opt.value(), out);
        }
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
