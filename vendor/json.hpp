#ifndef CB_VENDOR_JSON_HPP
#define CB_VENDOR_JSON_HPP

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace cb_vendor_json {

using JsonId = uint32_t;
constexpr JsonId JSON_INVALID_ID = UINT32_MAX;

struct JsonNull {};

struct JsonBool {
    bool value = false;
};

struct JsonNumber {
    int64_t int_val = 0;
    double float_val = 0.0;
    bool is_int = true;
};

struct JsonString {
    std::string value;
};

struct JsonArray {
    std::vector<JsonId> elements;
};

struct JsonField {
    std::string key;
    JsonId value = JSON_INVALID_ID;
};

struct JsonObject {
    std::vector<JsonField> fields;
};

using JsonVariant = std::variant<
    JsonNull,
    JsonBool,
    JsonNumber,
    JsonString,
    JsonArray,
    JsonObject
>;

struct JsonArena {
    std::vector<JsonVariant> nodes;
};

struct JsonParseResult {
    bool ok = false;
    JsonArena arena;
    JsonId root = JSON_INVALID_ID;
    std::string error;
};

// ==========================================
// JSON Arena Builders
// ==========================================

template <typename T>
inline JsonId json_add_node(JsonArena& arena, T&& node) {
    const auto id = static_cast<JsonId>(arena.nodes.size());
    arena.nodes.emplace_back(std::forward<T>(node));
    return id;
}

inline JsonId json_add_null(JsonArena& arena) {
    return json_add_node(arena, JsonNull{});
}

inline JsonId json_add_bool(JsonArena& arena, bool val) {
    return json_add_node(arena, JsonBool{val});
}

inline JsonId json_add_int(JsonArena& arena, int64_t val) {
    return json_add_node(arena, JsonNumber{val, static_cast<double>(val), true});
}

inline JsonId json_add_float(JsonArena& arena, double val) {
    return json_add_node(arena, JsonNumber{static_cast<int64_t>(val), val, false});
}

inline JsonId json_add_string(JsonArena& arena, std::string_view val) {
    return json_add_node(arena, JsonString{std::string(val)});
}

inline JsonId json_add_array(JsonArena& arena) {
    return json_add_node(arena, JsonArray{});
}

inline JsonId json_add_object(JsonArena& arena) {
    return json_add_node(arena, JsonObject{});
}

inline void json_array_push(JsonArena& arena, JsonId arr_id, JsonId elem_id) {
    if (arr_id >= arena.nodes.size()) {
        return;
    }
    if (!std::holds_alternative<JsonArray>(arena.nodes[arr_id])) {
        return;
    }
    std::get<JsonArray>(arena.nodes[arr_id]).elements.push_back(elem_id);
}

inline void json_object_set(JsonArena& arena, JsonId obj_id, std::string_view key, JsonId val_id) {
    if (obj_id >= arena.nodes.size()) {
        return;
    }
    if (!std::holds_alternative<JsonObject>(arena.nodes[obj_id])) {
        return;
    }
    auto& fields = std::get<JsonObject>(arena.nodes[obj_id]).fields;
    for (auto& field : fields) {
        if (field.key == key) {
            field.value = val_id;
            return;
        }
    }
    fields.push_back(JsonField{std::string(key), val_id});
}

// ==========================================
// JSON Arena Queries
// ==========================================

inline JsonId json_get_field(const JsonArena& arena, JsonId obj_id, std::string_view key) {
    if (obj_id >= arena.nodes.size()) {
        return JSON_INVALID_ID;
    }
    if (!std::holds_alternative<JsonObject>(arena.nodes[obj_id])) {
        return JSON_INVALID_ID;
    }
    const auto& fields = std::get<JsonObject>(arena.nodes[obj_id]).fields;
    for (const auto& field : fields) {
        if (field.key == key) {
            return field.value;
        }
    }
    return JSON_INVALID_ID;
}

inline std::optional<std::string> json_get_string(const JsonArena& arena, JsonId id) {
    if (id >= arena.nodes.size()) {
        return std::nullopt;
    }
    if (!std::holds_alternative<JsonString>(arena.nodes[id])) {
        return std::nullopt;
    }
    return std::get<JsonString>(arena.nodes[id]).value;
}

inline std::optional<int64_t> json_get_int(const JsonArena& arena, JsonId id) {
    if (id >= arena.nodes.size()) {
        return std::nullopt;
    }
    if (!std::holds_alternative<JsonNumber>(arena.nodes[id])) {
        return std::nullopt;
    }
    const auto& num = std::get<JsonNumber>(arena.nodes[id]);
    if (num.is_int) {
        return num.int_val;
    }
    return static_cast<int64_t>(num.float_val);
}

inline std::optional<double> json_get_float(const JsonArena& arena, JsonId id) {
    if (id >= arena.nodes.size()) {
        return std::nullopt;
    }
    if (!std::holds_alternative<JsonNumber>(arena.nodes[id])) {
        return std::nullopt;
    }
    const auto& num = std::get<JsonNumber>(arena.nodes[id]);
    if (num.is_int) {
        return static_cast<double>(num.int_val);
    }
    return num.float_val;
}

inline std::optional<bool> json_get_bool(const JsonArena& arena, JsonId id) {
    if (id >= arena.nodes.size()) {
        return std::nullopt;
    }
    if (!std::holds_alternative<JsonBool>(arena.nodes[id])) {
        return std::nullopt;
    }
    return std::get<JsonBool>(arena.nodes[id]).value;
}

inline bool json_is_null(const JsonArena& arena, JsonId id) {
    if (id >= arena.nodes.size()) {
        return false;
    }
    return std::holds_alternative<JsonNull>(arena.nodes[id]);
}

// ==========================================
// Recursive-Descent JSON Parser
// ==========================================

struct JsonParserState {
    std::string_view input;
    size_t pos = 0;
    bool has_error = false;
    std::string error;
    JsonArena arena;
};

inline void set_json_error(JsonParserState& state, std::string_view msg) {
    if (!state.has_error) {
        state.has_error = true;
        state.error = "JSON parse error at offset " + std::to_string(state.pos) + ": " + std::string(msg);
    }
}

inline bool json_is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

inline void json_skip_ws(JsonParserState& state) {
    while (state.pos < state.input.size() && json_is_ws(state.input[state.pos])) {
        state.pos += 1;
    }
}

inline std::optional<uint32_t> json_hex_digit_value(char c) {
    if (c >= '0' && c <= '9') {
        return static_cast<uint32_t>(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return static_cast<uint32_t>(10 + (c - 'a'));
    }
    if (c >= 'A' && c <= 'F') {
        return static_cast<uint32_t>(10 + (c - 'A'));
    }
    return std::nullopt;
}

constexpr char JSON_BACKSLASH = static_cast<char>(92);
constexpr char JSON_DQUOTE = static_cast<char>(34);

inline std::optional<uint32_t> json_parse_hex4(JsonParserState& state) {
    if (state.pos + 4 > state.input.size()) {
        set_json_error(state, "Incomplete unicode hex escape in string");
        return std::nullopt;
    }
    uint32_t code = 0;
    for (size_t i = 0; i < 4; ++i) {
        const auto digit = json_hex_digit_value(state.input[state.pos + i]);
        if (!digit.has_value()) {
            set_json_error(state, "Invalid hex digit in unicode escape");
            return std::nullopt;
        }
        code = (code << 4) | digit.value();
    }
    state.pos += 4;
    return code;
}

inline void json_append_utf8(std::string& out, uint32_t cp) {
    if (cp <= 0x7Fu) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FFu) {
        out.push_back(static_cast<char>(0xC0u | ((cp >> 6) & 0x1Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0xFFFFu) {
        out.push_back(static_cast<char>(0xE0u | ((cp >> 12) & 0x0Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0x10FFFFu) {
        out.push_back(static_cast<char>(0xF0u | ((cp >> 18) & 0x07u)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
}

inline std::optional<std::string> json_parse_raw_string(JsonParserState& state) {
    if (state.pos >= state.input.size() || state.input[state.pos] != JSON_DQUOTE) {
        set_json_error(state, "Expected double-quote to start string");
        return std::nullopt;
    }
    state.pos += 1;
    std::string result;
    while (state.pos < state.input.size()) {
        const char c = state.input[state.pos];
        state.pos += 1;
        if (c == JSON_DQUOTE) {
            return result;
        }
        if (static_cast<unsigned char>(c) < 0x20u) {
            set_json_error(state, "Unescaped control character in string literal");
            return std::nullopt;
        }
        if (c == JSON_BACKSLASH) {
            if (state.pos >= state.input.size()) {
                set_json_error(state, "Unterminated escape sequence in string literal");
                return std::nullopt;
            }
            const char esc = state.input[state.pos];
            state.pos += 1;
            if (esc == JSON_DQUOTE) {
                result.push_back(JSON_DQUOTE);
            } else if (esc == JSON_BACKSLASH) {
                result.push_back(JSON_BACKSLASH);
            } else if (esc == '/') {
                result.push_back('/');
            } else if (esc == 'b') {
                result.push_back('\b');
            } else if (esc == 'f') {
                result.push_back('\f');
            } else if (esc == 'n') {
                result.push_back('\n');
            } else if (esc == 'r') {
                result.push_back('\r');
            } else if (esc == 't') {
                result.push_back('\t');
            } else if (esc == 'u') {
                const auto first_code = json_parse_hex4(state);
                if (!first_code.has_value()) {
                    return std::nullopt;
                }
                uint32_t cp = first_code.value();
                if (cp >= 0xD800u && cp <= 0xDBFFu) {
                    if (state.pos + 2 <= state.input.size() &&
                        state.input[state.pos] == JSON_BACKSLASH &&
                        state.input[state.pos + 1] == 'u') {
                        state.pos += 2;
                        const auto low_code = json_parse_hex4(state);
                        if (!low_code.has_value()) {
                            return std::nullopt;
                        }
                        if (low_code.value() >= 0xDC00u && low_code.value() <= 0xDFFFu) {
                            cp = 0x10000u + (((cp - 0xD800u) << 10) | (low_code.value() - 0xDC00u));
                        }
                    }
                }
                json_append_utf8(result, cp);
            } else {
                set_json_error(state, "Invalid escape character in string literal");
                return std::nullopt;
            }
        } else {
            result.push_back(c);
        }
    }
    set_json_error(state, "Unterminated string literal");
    return std::nullopt;
}

inline JsonId json_parse_value(JsonParserState& state);

inline JsonId json_parse_object(JsonParserState& state) {
    state.pos += 1;
    const JsonId obj_id = json_add_object(state.arena);
    json_skip_ws(state);
    if (state.pos < state.input.size() && state.input[state.pos] == '}') {
        state.pos += 1;
        return obj_id;
    }

    while (state.pos < state.input.size()) {
        json_skip_ws(state);
        if (state.pos >= state.input.size() || state.input[state.pos] != '"') {
            set_json_error(state, "Expected string key in object");
            return JSON_INVALID_ID;
        }
        const auto key_opt = json_parse_raw_string(state);
        if (!key_opt.has_value()) {
            return JSON_INVALID_ID;
        }
        json_skip_ws(state);
        if (state.pos >= state.input.size() || state.input[state.pos] != ':') {
            set_json_error(state, "Expected ':' after object key");
            return JSON_INVALID_ID;
        }
        state.pos += 1;
        const JsonId val_id = json_parse_value(state);
        if (state.has_error || val_id == JSON_INVALID_ID) {
            return JSON_INVALID_ID;
        }
        json_object_set(state.arena, obj_id, key_opt.value(), val_id);

        json_skip_ws(state);
        if (state.pos >= state.input.size()) {
            set_json_error(state, "Unterminated object, expected ',' or '}'");
            return JSON_INVALID_ID;
        }
        if (state.input[state.pos] == '}') {
            state.pos += 1;
            return obj_id;
        }
        if (state.input[state.pos] != ',') {
            set_json_error(state, "Expected ',' or '}' in object");
            return JSON_INVALID_ID;
        }
        state.pos += 1;
        json_skip_ws(state);
        if (state.pos < state.input.size() && state.input[state.pos] == '}') {
            set_json_error(state, "Trailing comma is not allowed in JSON object");
            return JSON_INVALID_ID;
        }
    }
    set_json_error(state, "Unterminated JSON object");
    return JSON_INVALID_ID;
}

inline JsonId json_parse_array(JsonParserState& state) {
    state.pos += 1;
    const JsonId arr_id = json_add_array(state.arena);
    json_skip_ws(state);
    if (state.pos < state.input.size() && state.input[state.pos] == ']') {
        state.pos += 1;
        return arr_id;
    }

    while (state.pos < state.input.size()) {
        const JsonId elem_id = json_parse_value(state);
        if (state.has_error || elem_id == JSON_INVALID_ID) {
            return JSON_INVALID_ID;
        }
        json_array_push(state.arena, arr_id, elem_id);

        json_skip_ws(state);
        if (state.pos >= state.input.size()) {
            set_json_error(state, "Unterminated array, expected ',' or ']'");
            return JSON_INVALID_ID;
        }
        if (state.input[state.pos] == ']') {
            state.pos += 1;
            return arr_id;
        }
        if (state.input[state.pos] != ',') {
            set_json_error(state, "Expected ',' or ']' in array");
            return JSON_INVALID_ID;
        }
        state.pos += 1;
        json_skip_ws(state);
        if (state.pos < state.input.size() && state.input[state.pos] == ']') {
            set_json_error(state, "Trailing comma is not allowed in JSON array");
            return JSON_INVALID_ID;
        }
    }
    set_json_error(state, "Unterminated JSON array");
    return JSON_INVALID_ID;
}

inline JsonId json_parse_number(JsonParserState& state) {
    const size_t start = state.pos;
    bool is_int = true;

    if (state.pos < state.input.size() && state.input[state.pos] == '-') {
        state.pos += 1;
    }
    if (state.pos >= state.input.size()) {
        set_json_error(state, "Incomplete number literal");
        return JSON_INVALID_ID;
    }
    if (state.input[state.pos] == '0') {
        state.pos += 1;
        if (state.pos < state.input.size() &&
            state.input[state.pos] >= '0' && state.input[state.pos] <= '9') {
            set_json_error(state, "Leading zeros are not allowed in JSON numbers");
            return JSON_INVALID_ID;
        }
    } else if (state.input[state.pos] >= '1' && state.input[state.pos] <= '9') {
        while (state.pos < state.input.size() &&
               state.input[state.pos] >= '0' && state.input[state.pos] <= '9') {
            state.pos += 1;
        }
    } else {
        set_json_error(state, "Invalid digit in number literal");
        return JSON_INVALID_ID;
    }

    if (state.pos < state.input.size() && state.input[state.pos] == '.') {
        is_int = false;
        state.pos += 1;
        if (state.pos >= state.input.size() ||
            state.input[state.pos] < '0' || state.input[state.pos] > '9') {
            set_json_error(state, "Expected digit after decimal point in number literal");
            return JSON_INVALID_ID;
        }
        while (state.pos < state.input.size() &&
               state.input[state.pos] >= '0' && state.input[state.pos] <= '9') {
            state.pos += 1;
        }
    }

    if (state.pos < state.input.size() &&
        (state.input[state.pos] == 'e' || state.input[state.pos] == 'E')) {
        is_int = false;
        state.pos += 1;
        if (state.pos < state.input.size() &&
            (state.input[state.pos] == '+' || state.input[state.pos] == '-')) {
            state.pos += 1;
        }
        if (state.pos >= state.input.size() ||
            state.input[state.pos] < '0' || state.input[state.pos] > '9') {
            set_json_error(state, "Expected digit in exponent of number literal");
            return JSON_INVALID_ID;
        }
        while (state.pos < state.input.size() &&
               state.input[state.pos] >= '0' && state.input[state.pos] <= '9') {
            state.pos += 1;
        }
    }

    const std::string raw(state.input.substr(start, state.pos - start));
    if (is_int) {
        std::istringstream iss(raw);
        int64_t iv = 0;
        iss >> iv;
        if (!iss.fail() && iss.eof()) {
            return json_add_int(state.arena, iv);
        }
    }

    std::istringstream fss(raw);
    double fv = 0.0;
    fss >> fv;
    if (fss.fail()) {
        set_json_error(state, "Invalid numeric value");
        return JSON_INVALID_ID;
    }
    return json_add_float(state.arena, fv);
}

inline JsonId json_parse_value(JsonParserState& state) {
    json_skip_ws(state);
    if (state.pos >= state.input.size()) {
        set_json_error(state, "Unexpected end of JSON input");
        return JSON_INVALID_ID;
    }

    const char c = state.input[state.pos];
    if (c == '{') {
        return json_parse_object(state);
    }
    if (c == '[') {
        return json_parse_array(state);
    }
    if (c == '"') {
        const auto s = json_parse_raw_string(state);
        if (!s.has_value()) {
            return JSON_INVALID_ID;
        }
        return json_add_string(state.arena, s.value());
    }
    if (c == 't') {
        if (state.input.substr(state.pos, 4) == "true") {
            state.pos += 4;
            return json_add_bool(state.arena, true);
        }
        set_json_error(state, "Invalid literal starting with 't' (expected 'true')");
        return JSON_INVALID_ID;
    }
    if (c == 'f') {
        if (state.input.substr(state.pos, 5) == "false") {
            state.pos += 5;
            return json_add_bool(state.arena, false);
        }
        set_json_error(state, "Invalid literal starting with 'f' (expected 'false')");
        return JSON_INVALID_ID;
    }
    if (c == 'n') {
        if (state.input.substr(state.pos, 4) == "null") {
            state.pos += 4;
            return json_add_null(state.arena);
        }
        set_json_error(state, "Invalid literal starting with 'n' (expected 'null')");
        return JSON_INVALID_ID;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        return json_parse_number(state);
    }

    set_json_error(state, "Unexpected character in JSON input");
    return JSON_INVALID_ID;
}

inline JsonParseResult parse_json(std::string_view input) {
    JsonParserState state;
    state.input = input;

    const JsonId root = json_parse_value(state);
    if (state.has_error || root == JSON_INVALID_ID) {
        JsonParseResult fail;
        fail.ok = false;
        fail.error = state.error;
        return fail;
    }

    json_skip_ws(state);
    if (state.pos < state.input.size()) {
        set_json_error(state, "Unexpected trailing content after root JSON value");
        JsonParseResult fail;
        fail.ok = false;
        fail.error = state.error;
        return fail;
    }

    JsonParseResult res;
    res.ok = true;
    res.arena = std::move(state.arena);
    res.root = root;
    return res;
}

// ==========================================
// Compact JSON Serializer
// ==========================================

inline void json_append_escaped_string(std::string& out, std::string_view s) {
    out.push_back(JSON_DQUOTE);
    for (char ch : s) {
        const auto uc = static_cast<unsigned char>(ch);
        if (ch == JSON_DQUOTE || ch == JSON_BACKSLASH) {
            out.push_back(JSON_BACKSLASH);
            out.push_back(ch);
        } else if (ch == static_cast<char>(8)) {
            out.push_back(JSON_BACKSLASH); out.push_back('b');
        } else if (ch == static_cast<char>(12)) {
            out.push_back(JSON_BACKSLASH); out.push_back('f');
        } else if (ch == static_cast<char>(10)) {
            out.push_back(JSON_BACKSLASH); out.push_back('n');
        } else if (ch == static_cast<char>(13)) {
            out.push_back(JSON_BACKSLASH); out.push_back('r');
        } else if (ch == static_cast<char>(9)) {
            out.push_back(JSON_BACKSLASH); out.push_back('t');
        } else if (uc < 0x20u) {
            out.push_back(JSON_BACKSLASH);
            out.push_back('u');
            std::ostringstream oss;
            oss << std::hex << std::setw(4) << std::setfill('0') << static_cast<unsigned int>(uc);
            out += oss.str();
        } else {
            out.push_back(ch);
        }
    }
    out.push_back(JSON_DQUOTE);
}

inline void serialize_json_into(const JsonArena& arena, JsonId id, std::string& out) {
    if (id >= arena.nodes.size()) {
        out += "null";
        return;
    }

    const auto& node = arena.nodes[id];
    if (std::holds_alternative<JsonNull>(node)) {
        out += "null";
        return;
    }
    if (std::holds_alternative<JsonBool>(node)) {
        const auto& b = std::get<JsonBool>(node);
        if (b.value) {
            out += "true";
        } else {
            out += "false";
        }
        return;
    }
    if (std::holds_alternative<JsonNumber>(node)) {
        const auto& num = std::get<JsonNumber>(node);
        if (num.is_int) {
            out += std::to_string(num.int_val);
        } else {
            std::ostringstream oss;
            oss << std::setprecision(15) << num.float_val;
            out += oss.str();
        }
        return;
    }
    if (std::holds_alternative<JsonString>(node)) {
        const auto& s = std::get<JsonString>(node);
        json_append_escaped_string(out, s.value);
        return;
    }
    if (std::holds_alternative<JsonArray>(node)) {
        const auto& arr = std::get<JsonArray>(node);
        out.push_back('[');
        for (size_t i = 0; i < arr.elements.size(); ++i) {
            if (i > 0) {
                out.push_back(',');
            }
            serialize_json_into(arena, arr.elements[i], out);
        }
        out.push_back(']');
        return;
    }
    if (std::holds_alternative<JsonObject>(node)) {
        const auto& obj = std::get<JsonObject>(node);
        out.push_back('{');
        for (size_t i = 0; i < obj.fields.size(); ++i) {
            if (i > 0) {
                out.push_back(',');
            }
            json_append_escaped_string(out, obj.fields[i].key);
            out.push_back(':');
            serialize_json_into(arena, obj.fields[i].value, out);
        }
        out.push_back('}');
        return;
    }
}

inline std::string serialize_json(const JsonArena& arena, JsonId id) {
    std::string out;
    serialize_json_into(arena, id, out);
    return out;
}

} // namespace cb_vendor_json

#endif // CB_VENDOR_JSON_HPP
