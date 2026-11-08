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

namespace cbVendorJson {

using JsonId = uint32_t;
constexpr JsonId JSON_INVALID_ID = UINT32_MAX;

struct JsonNull {};

struct JsonBool {
    bool value = false;
};

struct JsonNumber {
    int64_t intVal = 0;
    double floatVal = 0.0;
    bool isInt = true;
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
inline JsonId jsonAddNode(JsonArena& arena, T&& node) {
    const auto id = static_cast<JsonId>(arena.nodes.size());
    arena.nodes.emplace_back(std::forward<T>(node));
    return id;
}

inline JsonId jsonAddNull(JsonArena& arena) {
    return jsonAddNode(arena, JsonNull{});
}

inline JsonId jsonAddBool(JsonArena& arena, bool val) {
    return jsonAddNode(arena, JsonBool{val});
}

inline JsonId jsonAddInt(JsonArena& arena, int64_t val) {
    return jsonAddNode(arena, JsonNumber{val, static_cast<double>(val), true});
}

inline JsonId jsonAddFloat(JsonArena& arena, double val) {
    return jsonAddNode(arena, JsonNumber{static_cast<int64_t>(val), val, false});
}

inline JsonId jsonAddString(JsonArena& arena, std::string_view val) {
    return jsonAddNode(arena, JsonString{std::string(val)});
}

inline JsonId jsonAddArray(JsonArena& arena) {
    return jsonAddNode(arena, JsonArray{});
}

inline JsonId jsonAddObject(JsonArena& arena) {
    return jsonAddNode(arena, JsonObject{});
}

inline void jsonArrayPush(JsonArena& arena, JsonId arrId, JsonId elemId) {
    if (arrId >= arena.nodes.size()) {
        return;
    }
    if (!std::holds_alternative<JsonArray>(arena.nodes[arrId])) {
        return;
    }
    std::get<JsonArray>(arena.nodes[arrId]).elements.push_back(elemId);
}

inline void jsonObjectSet(JsonArena& arena, JsonId objId, std::string_view key, JsonId valId) {
    if (objId >= arena.nodes.size()) {
        return;
    }
    if (!std::holds_alternative<JsonObject>(arena.nodes[objId])) {
        return;
    }
    auto& fields = std::get<JsonObject>(arena.nodes[objId]).fields;
    for (auto& field : fields) {
        if (field.key == key) {
            field.value = valId;
            return;
        }
    }
    fields.push_back(JsonField{std::string(key), valId});
}

// ==========================================
// JSON Arena Queries
// ==========================================

inline JsonId jsonGetField(const JsonArena& arena, JsonId objId, std::string_view key) {
    if (objId >= arena.nodes.size()) {
        return JSON_INVALID_ID;
    }
    if (!std::holds_alternative<JsonObject>(arena.nodes[objId])) {
        return JSON_INVALID_ID;
    }
    const auto& fields = std::get<JsonObject>(arena.nodes[objId]).fields;
    for (const auto& field : fields) {
        if (field.key == key) {
            return field.value;
        }
    }
    return JSON_INVALID_ID;
}

inline std::optional<std::string> jsonGetString(const JsonArena& arena, JsonId id) {
    if (id >= arena.nodes.size()) {
        return std::nullopt;
    }
    if (!std::holds_alternative<JsonString>(arena.nodes[id])) {
        return std::nullopt;
    }
    return std::get<JsonString>(arena.nodes[id]).value;
}

inline std::optional<int64_t> jsonGetInt(const JsonArena& arena, JsonId id) {
    if (id >= arena.nodes.size()) {
        return std::nullopt;
    }
    if (!std::holds_alternative<JsonNumber>(arena.nodes[id])) {
        return std::nullopt;
    }
    const auto& num = std::get<JsonNumber>(arena.nodes[id]);
    if (num.isInt) {
        return num.intVal;
    }
    return static_cast<int64_t>(num.floatVal);
}

inline std::optional<double> jsonGetFloat(const JsonArena& arena, JsonId id) {
    if (id >= arena.nodes.size()) {
        return std::nullopt;
    }
    if (!std::holds_alternative<JsonNumber>(arena.nodes[id])) {
        return std::nullopt;
    }
    const auto& num = std::get<JsonNumber>(arena.nodes[id]);
    if (num.isInt) {
        return static_cast<double>(num.intVal);
    }
    return num.floatVal;
}

inline std::optional<bool> jsonGetBool(const JsonArena& arena, JsonId id) {
    if (id >= arena.nodes.size()) {
        return std::nullopt;
    }
    if (!std::holds_alternative<JsonBool>(arena.nodes[id])) {
        return std::nullopt;
    }
    return std::get<JsonBool>(arena.nodes[id]).value;
}

inline bool jsonIsNull(const JsonArena& arena, JsonId id) {
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
    bool hasError = false;
    std::string error;
    JsonArena arena;
};

inline void setJsonError(JsonParserState& state, std::string_view msg) {
    if (!state.hasError) {
        state.hasError = true;
        state.error = "JSON parse error at offset " + std::to_string(state.pos) + ": " + std::string(msg);
    }
}

inline bool jsonIsWs(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

inline void jsonSkipWs(JsonParserState& state) {
    while (state.pos < state.input.size() && jsonIsWs(state.input[state.pos])) {
        state.pos += 1;
    }
}

inline std::optional<uint32_t> jsonHexDigitValue(char c) {
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

inline std::optional<uint32_t> jsonParseHex4(JsonParserState& state) {
    if (state.pos + 4 > state.input.size()) {
        setJsonError(state, "Incomplete unicode hex escape in string");
        return std::nullopt;
    }
    uint32_t code = 0;
    for (size_t i = 0; i < 4; ++i) {
        const auto digit = jsonHexDigitValue(state.input[state.pos + i]);
        if (!digit.has_value()) {
            setJsonError(state, "Invalid hex digit in unicode escape");
            return std::nullopt;
        }
        code = (code << 4) | digit.value();
    }
    state.pos += 4;
    return code;
}

inline void jsonAppendUtf8(std::string& out, uint32_t cp) {
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

inline std::optional<std::string> jsonParseRawString(JsonParserState& state) {
    if (state.pos >= state.input.size() || state.input[state.pos] != JSON_DQUOTE) {
        setJsonError(state, "Expected double-quote to start string");
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
            setJsonError(state, "Unescaped control character in string literal");
            return std::nullopt;
        }
        if (c == JSON_BACKSLASH) {
            if (state.pos >= state.input.size()) {
                setJsonError(state, "Unterminated escape sequence in string literal");
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
                const auto firstCode = jsonParseHex4(state);
                if (!firstCode.has_value()) {
                    return std::nullopt;
                }
                uint32_t cp = firstCode.value();
                if (cp >= 0xD800u && cp <= 0xDBFFu) {
                    if (state.pos + 2 <= state.input.size() &&
                        state.input[state.pos] == JSON_BACKSLASH &&
                        state.input[state.pos + 1] == 'u') {
                        state.pos += 2;
                        const auto lowCode = jsonParseHex4(state);
                        if (!lowCode.has_value()) {
                            return std::nullopt;
                        }
                        if (lowCode.value() >= 0xDC00u && lowCode.value() <= 0xDFFFu) {
                            cp = 0x10000u + (((cp - 0xD800u) << 10) | (lowCode.value() - 0xDC00u));
                        }
                    }
                }
                jsonAppendUtf8(result, cp);
            } else {
                setJsonError(state, "Invalid escape character in string literal");
                return std::nullopt;
            }
        } else {
            result.push_back(c);
        }
    }
    setJsonError(state, "Unterminated string literal");
    return std::nullopt;
}

inline JsonId jsonParseValue(JsonParserState& state);

inline JsonId jsonParseObject(JsonParserState& state) {
    state.pos += 1;
    const JsonId objId = jsonAddObject(state.arena);
    jsonSkipWs(state);
    if (state.pos < state.input.size() && state.input[state.pos] == '}') {
        state.pos += 1;
        return objId;
    }

    while (state.pos < state.input.size()) {
        jsonSkipWs(state);
        if (state.pos >= state.input.size() || state.input[state.pos] != '"') {
            setJsonError(state, "Expected string key in object");
            return JSON_INVALID_ID;
        }
        const auto keyOpt = jsonParseRawString(state);
        if (!keyOpt.has_value()) {
            return JSON_INVALID_ID;
        }
        jsonSkipWs(state);
        if (state.pos >= state.input.size() || state.input[state.pos] != ':') {
            setJsonError(state, "Expected ':' after object key");
            return JSON_INVALID_ID;
        }
        state.pos += 1;
        const JsonId valId = jsonParseValue(state);
        if (state.hasError || valId == JSON_INVALID_ID) {
            return JSON_INVALID_ID;
        }
        jsonObjectSet(state.arena, objId, keyOpt.value(), valId);

        jsonSkipWs(state);
        if (state.pos >= state.input.size()) {
            setJsonError(state, "Unterminated object, expected ',' or '}'");
            return JSON_INVALID_ID;
        }
        if (state.input[state.pos] == '}') {
            state.pos += 1;
            return objId;
        }
        if (state.input[state.pos] != ',') {
            setJsonError(state, "Expected ',' or '}' in object");
            return JSON_INVALID_ID;
        }
        state.pos += 1;
        jsonSkipWs(state);
        if (state.pos < state.input.size() && state.input[state.pos] == '}') {
            setJsonError(state, "Trailing comma is not allowed in JSON object");
            return JSON_INVALID_ID;
        }
    }
    setJsonError(state, "Unterminated JSON object");
    return JSON_INVALID_ID;
}

inline JsonId jsonParseArray(JsonParserState& state) {
    state.pos += 1;
    const JsonId arrId = jsonAddArray(state.arena);
    jsonSkipWs(state);
    if (state.pos < state.input.size() && state.input[state.pos] == ']') {
        state.pos += 1;
        return arrId;
    }

    while (state.pos < state.input.size()) {
        const JsonId elemId = jsonParseValue(state);
        if (state.hasError || elemId == JSON_INVALID_ID) {
            return JSON_INVALID_ID;
        }
        jsonArrayPush(state.arena, arrId, elemId);

        jsonSkipWs(state);
        if (state.pos >= state.input.size()) {
            setJsonError(state, "Unterminated array, expected ',' or ']'");
            return JSON_INVALID_ID;
        }
        if (state.input[state.pos] == ']') {
            state.pos += 1;
            return arrId;
        }
        if (state.input[state.pos] != ',') {
            setJsonError(state, "Expected ',' or ']' in array");
            return JSON_INVALID_ID;
        }
        state.pos += 1;
        jsonSkipWs(state);
        if (state.pos < state.input.size() && state.input[state.pos] == ']') {
            setJsonError(state, "Trailing comma is not allowed in JSON array");
            return JSON_INVALID_ID;
        }
    }
    setJsonError(state, "Unterminated JSON array");
    return JSON_INVALID_ID;
}

inline JsonId jsonParseNumber(JsonParserState& state) {
    const size_t start = state.pos;
    bool isInt = true;

    if (state.pos < state.input.size() && state.input[state.pos] == '-') {
        state.pos += 1;
    }
    if (state.pos >= state.input.size()) {
        setJsonError(state, "Incomplete number literal");
        return JSON_INVALID_ID;
    }
    if (state.input[state.pos] == '0') {
        state.pos += 1;
        if (state.pos < state.input.size() &&
            state.input[state.pos] >= '0' && state.input[state.pos] <= '9') {
            setJsonError(state, "Leading zeros are not allowed in JSON numbers");
            return JSON_INVALID_ID;
        }
    } else if (state.input[state.pos] >= '1' && state.input[state.pos] <= '9') {
        while (state.pos < state.input.size() &&
               state.input[state.pos] >= '0' && state.input[state.pos] <= '9') {
            state.pos += 1;
        }
    } else {
        setJsonError(state, "Invalid digit in number literal");
        return JSON_INVALID_ID;
    }

    if (state.pos < state.input.size() && state.input[state.pos] == '.') {
        isInt = false;
        state.pos += 1;
        if (state.pos >= state.input.size() ||
            state.input[state.pos] < '0' || state.input[state.pos] > '9') {
            setJsonError(state, "Expected digit after decimal point in number literal");
            return JSON_INVALID_ID;
        }
        while (state.pos < state.input.size() &&
               state.input[state.pos] >= '0' && state.input[state.pos] <= '9') {
            state.pos += 1;
        }
    }

    if (state.pos < state.input.size() &&
        (state.input[state.pos] == 'e' || state.input[state.pos] == 'E')) {
        isInt = false;
        state.pos += 1;
        if (state.pos < state.input.size() &&
            (state.input[state.pos] == '+' || state.input[state.pos] == '-')) {
            state.pos += 1;
        }
        if (state.pos >= state.input.size() ||
            state.input[state.pos] < '0' || state.input[state.pos] > '9') {
            setJsonError(state, "Expected digit in exponent of number literal");
            return JSON_INVALID_ID;
        }
        while (state.pos < state.input.size() &&
               state.input[state.pos] >= '0' && state.input[state.pos] <= '9') {
            state.pos += 1;
        }
    }

    const std::string raw(state.input.substr(start, state.pos - start));
    if (isInt) {
        std::istringstream iss(raw);
        int64_t iv = 0;
        iss >> iv;
        if (!iss.fail() && iss.eof()) {
            return jsonAddInt(state.arena, iv);
        }
    }

    std::istringstream fss(raw);
    double fv = 0.0;
    fss >> fv;
    if (fss.fail()) {
        setJsonError(state, "Invalid numeric value");
        return JSON_INVALID_ID;
    }
    return jsonAddFloat(state.arena, fv);
}

inline JsonId jsonParseValue(JsonParserState& state) {
    jsonSkipWs(state);
    if (state.pos >= state.input.size()) {
        setJsonError(state, "Unexpected end of JSON input");
        return JSON_INVALID_ID;
    }

    const char c = state.input[state.pos];
    if (c == '{') {
        return jsonParseObject(state);
    }
    if (c == '[') {
        return jsonParseArray(state);
    }
    if (c == '"') {
        const auto s = jsonParseRawString(state);
        if (!s.has_value()) {
            return JSON_INVALID_ID;
        }
        return jsonAddString(state.arena, s.value());
    }
    if (c == 't') {
        if (state.input.substr(state.pos, 4) == "true") {
            state.pos += 4;
            return jsonAddBool(state.arena, true);
        }
        setJsonError(state, "Invalid literal starting with 't' (expected 'true')");
        return JSON_INVALID_ID;
    }
    if (c == 'f') {
        if (state.input.substr(state.pos, 5) == "false") {
            state.pos += 5;
            return jsonAddBool(state.arena, false);
        }
        setJsonError(state, "Invalid literal starting with 'f' (expected 'false')");
        return JSON_INVALID_ID;
    }
    if (c == 'n') {
        if (state.input.substr(state.pos, 4) == "null") {
            state.pos += 4;
            return jsonAddNull(state.arena);
        }
        setJsonError(state, "Invalid literal starting with 'n' (expected 'null')");
        return JSON_INVALID_ID;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        return jsonParseNumber(state);
    }

    setJsonError(state, "Unexpected character in JSON input");
    return JSON_INVALID_ID;
}

inline JsonParseResult parseJson(std::string_view input) {
    JsonParserState state;
    state.input = input;

    const JsonId root = jsonParseValue(state);
    if (state.hasError || root == JSON_INVALID_ID) {
        JsonParseResult fail;
        fail.ok = false;
        fail.error = state.error;
        return fail;
    }

    jsonSkipWs(state);
    if (state.pos < state.input.size()) {
        setJsonError(state, "Unexpected trailing content after root JSON value");
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

inline void jsonAppendEscapedString(std::string& out, std::string_view s) {
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

inline void serializeJsonInto(const JsonArena& arena, JsonId id, std::string& out) {
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
        if (num.isInt) {
            out += std::to_string(num.intVal);
        } else {
            std::ostringstream oss;
            oss << std::setprecision(15) << num.floatVal;
            out += oss.str();
        }
        return;
    }
    if (std::holds_alternative<JsonString>(node)) {
        const auto& s = std::get<JsonString>(node);
        jsonAppendEscapedString(out, s.value);
        return;
    }
    if (std::holds_alternative<JsonArray>(node)) {
        const auto& arr = std::get<JsonArray>(node);
        out.push_back('[');
        for (size_t i = 0; i < arr.elements.size(); ++i) {
            if (i > 0) {
                out.push_back(',');
            }
            serializeJsonInto(arena, arr.elements[i], out);
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
            jsonAppendEscapedString(out, obj.fields[i].key);
            out.push_back(':');
            serializeJsonInto(arena, obj.fields[i].value, out);
        }
        out.push_back('}');
        return;
    }
}

inline std::string serializeJson(const JsonArena& arena, JsonId id) {
    std::string out;
    serializeJsonInto(arena, id, out);
    return out;
}

} // namespace cbVendorJson

#endif // CB_VENDOR_JSON_HPP
