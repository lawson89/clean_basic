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

using cbVendorJson::JsonId;
using cbVendorJson::JSON_INVALID_ID;
using cbVendorJson::JsonNull;
using cbVendorJson::JsonBool;
using cbVendorJson::JsonNumber;
using cbVendorJson::JsonString;
using cbVendorJson::JsonArray;
using cbVendorJson::JsonField;
using cbVendorJson::JsonObject;
using cbVendorJson::JsonVariant;
using cbVendorJson::JsonArena;
using cbVendorJson::JsonParseResult;

using cbVendorJson::jsonAddNode;
using cbVendorJson::jsonAddNull;
using cbVendorJson::jsonAddBool;
using cbVendorJson::jsonAddInt;
using cbVendorJson::jsonAddFloat;
using cbVendorJson::jsonAddString;
using cbVendorJson::jsonAddArray;
using cbVendorJson::jsonAddObject;
using cbVendorJson::jsonArrayPush;
using cbVendorJson::jsonObjectSet;

using cbVendorJson::jsonGetField;
using cbVendorJson::jsonGetString;
using cbVendorJson::jsonGetInt;
using cbVendorJson::jsonGetFloat;
using cbVendorJson::jsonGetBool;
using cbVendorJson::jsonIsNull;

using cbVendorJson::JSON_BACKSLASH;
using cbVendorJson::JSON_DQUOTE;
using cbVendorJson::jsonHexDigitValue;
using cbVendorJson::parseJson;
using cbVendorJson::serializeJsonInto;
using cbVendorJson::serializeJson;

// ==========================================
// JSON-RPC 2.0 Content-Length Framing
// ==========================================

inline std::string trimAsciiWs(std::string_view sv) {
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

inline bool startsWithIgnoreCase(std::string_view text, std::string_view prefix) {
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

inline std::optional<size_t> parseDecimalSize(std::string_view sv) {
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

inline std::optional<std::string> readJsonrpcMessage(std::istream& in) {
    std::optional<size_t> contentLength = std::nullopt;
    bool sawHeaderLine = false;
    std::string line;

    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            if (!sawHeaderLine) {
                continue;
            }
            break;
        }
        sawHeaderLine = true;
        const std::string_view prefix = "Content-Length:";
        if (startsWithIgnoreCase(line, prefix)) {
            const std::string lenStr = trimAsciiWs(std::string_view(line).substr(prefix.size()));
            contentLength = parseDecimalSize(lenStr);
            if (!contentLength.has_value()) {
                return std::nullopt;
            }
        }
    }

    if (!contentLength.has_value()) {
        return std::nullopt;
    }

    std::string payload;
    payload.reserve(contentLength.value());
    for (size_t i = 0; i < contentLength.value(); ++i) {
        char ch = '\0';
        if (!in.get(ch)) {
            return std::nullopt;
        }
        payload.push_back(ch);
    }
    return payload;
}

inline void writeJsonrpcMessage(std::ostream& out, std::string_view jsonPayload) {
    out << "Content-Length: " << jsonPayload.size() << "\r\n\r\n" << jsonPayload;
    out.flush();
}

} // namespace cb

#endif // CLEANBASIC_JSON_HPP
