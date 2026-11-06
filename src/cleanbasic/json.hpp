#ifndef CLEANBASIC_JSON_HPP
#define CLEANBASIC_JSON_HPP

#include "src/cleanbasic/ast.hpp"
#include <json.hpp>

#include <cctype>
#include <cstddef>
#include <istream>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>

namespace cb {

using cb_vendor_json::JsonId;
using cb_vendor_json::JSON_INVALID_ID;
using cb_vendor_json::JsonNull;
using cb_vendor_json::JsonBool;
using cb_vendor_json::JsonNumber;
using cb_vendor_json::JsonString;
using cb_vendor_json::JsonArray;
using cb_vendor_json::JsonField;
using cb_vendor_json::JsonObject;
using cb_vendor_json::JsonVariant;
using cb_vendor_json::JsonArena;
using cb_vendor_json::JsonParseResult;

using cb_vendor_json::json_add_node;
using cb_vendor_json::json_add_null;
using cb_vendor_json::json_add_bool;
using cb_vendor_json::json_add_int;
using cb_vendor_json::json_add_float;
using cb_vendor_json::json_add_string;
using cb_vendor_json::json_add_array;
using cb_vendor_json::json_add_object;
using cb_vendor_json::json_array_push;
using cb_vendor_json::json_object_set;

using cb_vendor_json::json_get_field;
using cb_vendor_json::json_get_string;
using cb_vendor_json::json_get_int;
using cb_vendor_json::json_get_float;
using cb_vendor_json::json_get_bool;
using cb_vendor_json::json_is_null;

using cb_vendor_json::JSON_BACKSLASH;
using cb_vendor_json::JSON_DQUOTE;
using cb_vendor_json::json_hex_digit_value;
using cb_vendor_json::parse_json;
using cb_vendor_json::serialize_json_into;
using cb_vendor_json::serialize_json;

// ==========================================
// JSON-RPC 2.0 Content-Length Framing
// ==========================================

inline std::string trim_ascii_ws(std::string_view sv) {
    size_t start = 0;
    while (start < sv.size() && (sv[start] == ' ' || sv[start] == '\t' || sv[start] == '\r' || sv[start] == '\n')) {
        start += 1;
    }
    size_t end = sv.size();
    while (end > start && (sv[end - 1] == ' ' || sv[end - 1] == '\t' || sv[end - 1] == '\r' || sv[end - 1] == '\n')) {
        end -= 1;
    }
    return std::string(sv.substr(start, end - start));
}

inline bool starts_with_ignore_case(std::string_view text, std::string_view prefix) {
    if (text.size() < prefix.size()) {
        return false;
    }
    for (size_t i = 0; i < prefix.size(); ++i) {
        const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(text[i])));
        const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(prefix[i])));
        if (a != b) {
            return false;
        }
    }
    return true;
}

inline std::optional<size_t> parse_decimal_size(std::string_view sv) {
    if (sv.empty()) {
        return std::nullopt;
    }
    size_t value = 0;
    for (char c : sv) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        value = (value * 10u) + static_cast<size_t>(c - '0');
    }
    return value;
}

inline std::optional<std::string> read_jsonrpc_message(std::istream& in) {
    std::optional<size_t> content_length = std::nullopt;
    bool saw_header_line = false;
    std::string line;

    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            if (!saw_header_line) {
                continue;
            }
            break;
        }
        saw_header_line = true;
        const std::string_view prefix = "Content-Length:";
        if (starts_with_ignore_case(line, prefix)) {
            const std::string len_str = trim_ascii_ws(std::string_view(line).substr(prefix.size()));
            content_length = parse_decimal_size(len_str);
            if (!content_length.has_value()) {
                return std::nullopt;
            }
        }
    }

    if (!content_length.has_value()) {
        return std::nullopt;
    }

    std::string payload;
    payload.reserve(content_length.value());
    for (size_t i = 0; i < content_length.value(); ++i) {
        char ch = '\0';
        if (!in.get(ch)) {
            return std::nullopt;
        }
        payload.push_back(ch);
    }
    return payload;
}

inline void write_jsonrpc_message(std::ostream& out, std::string_view json_payload) {
    out << "Content-Length: " << json_payload.size() << "\r\n\r\n" << json_payload;
    out.flush();
}

} // namespace cb

#endif // CLEANBASIC_JSON_HPP
