#ifndef CLEANBASIC_STDLIB_JSON_BRIDGE_HPP
#define CLEANBASIC_STDLIB_JSON_BRIDGE_HPP

#include <json.hpp>

#include <cstdint>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

struct CbJsonDoc {
    cbVendorJson::JsonArena arena;
    int64_t root = -1;
};

struct CbJsonVal {
    int64_t id = -1;
};

using JsonDoc = CbJsonDoc;
using JsonVal = CbJsonVal;

inline CbJsonDoc cbJsonNewDoc() {
    CbJsonDoc doc{};
    doc.root = -1;
    return doc;
}

inline std::tuple<CbJsonDoc, CbError> cbJsonParse(const std::string& text) {
    auto res = cbVendorJson::parseJson(text);
    if (!res.ok) {
        return {CbJsonDoc{}, CbError{res.error, 400}};
    }
    CbJsonDoc doc{};
    doc.arena = std::move(res.arena);
    doc.root = static_cast<int64_t>(res.root);
    return {std::move(doc), CbError{"", 0}};
}

inline CbJsonVal cbJsonRoot(const CbJsonDoc& doc) {
    return CbJsonVal{doc.root};
}

inline void cbJsonSetRoot(CbJsonDoc& doc, CbJsonVal val) {
    doc.root = val.id;
}

inline std::string cbJsonStringifyVal(const CbJsonDoc& doc, CbJsonVal val) {
    if (val.id < 0) {
        return "null";
    }
    return cbVendorJson::serializeJson(doc.arena, static_cast<cbVendorJson::JsonId>(val.id));
}

inline std::string cbJsonStringify(const CbJsonDoc& doc) {
    return cbJsonStringifyVal(doc, CbJsonVal{doc.root});
}

inline CbJsonVal cbJsonNewObject(CbJsonDoc& doc) {
    return CbJsonVal{static_cast<int64_t>(cbVendorJson::jsonAddObject(doc.arena))};
}

inline CbJsonVal cbJsonNewArray(CbJsonDoc& doc) {
    return CbJsonVal{static_cast<int64_t>(cbVendorJson::jsonAddArray(doc.arena))};
}

inline CbJsonVal cbJsonNewStr(CbJsonDoc& doc, const std::string& val) {
    return CbJsonVal{static_cast<int64_t>(cbVendorJson::jsonAddString(doc.arena, val))};
}

inline CbJsonVal cbJsonNewInt(CbJsonDoc& doc, int64_t val) {
    return CbJsonVal{static_cast<int64_t>(cbVendorJson::jsonAddInt(doc.arena, val))};
}

inline CbJsonVal cbJsonNewFloat(CbJsonDoc& doc, double val) {
    return CbJsonVal{static_cast<int64_t>(cbVendorJson::jsonAddFloat(doc.arena, val))};
}

inline CbJsonVal cbJsonNewBool(CbJsonDoc& doc, bool val) {
    return CbJsonVal{static_cast<int64_t>(cbVendorJson::jsonAddBool(doc.arena, val))};
}

inline CbJsonVal cbJsonNewNull(CbJsonDoc& doc) {
    return CbJsonVal{static_cast<int64_t>(cbVendorJson::jsonAddNull(doc.arena))};
}

inline void cbJsonObjectSet(CbJsonDoc& doc, CbJsonVal obj, const std::string& key, CbJsonVal val) {
    if (obj.id < 0 || val.id < 0) {
        return;
    }
    cbVendorJson::jsonObjectSet(
        doc.arena,
        static_cast<cbVendorJson::JsonId>(obj.id),
        key,
        static_cast<cbVendorJson::JsonId>(val.id)
    );
}

inline void cbJsonArrayPush(CbJsonDoc& doc, CbJsonVal arr, CbJsonVal elem) {
    if (arr.id < 0 || elem.id < 0) {
        return;
    }
    cbVendorJson::jsonArrayPush(
        doc.arena,
        static_cast<cbVendorJson::JsonId>(arr.id),
        static_cast<cbVendorJson::JsonId>(elem.id)
    );
}

inline bool cbJsonHasField(const CbJsonDoc& doc, CbJsonVal obj, const std::string& key) {
    if (obj.id < 0) {
        return false;
    }
    const auto fid = cbVendorJson::jsonGetField(
        doc.arena,
        static_cast<cbVendorJson::JsonId>(obj.id),
        key
    );
    return fid != cbVendorJson::JSON_INVALID_ID;
}

inline std::tuple<CbJsonVal, CbError> cbJsonGetField(
    const CbJsonDoc& doc,
    CbJsonVal obj,
    const std::string& key
) {
    if (obj.id < 0) {
        return {CbJsonVal{-1}, CbError{"Invalid JSON object handle", 404}};
    }
    const auto fid = cbVendorJson::jsonGetField(
        doc.arena,
        static_cast<cbVendorJson::JsonId>(obj.id),
        key
    );
    if (fid == cbVendorJson::JSON_INVALID_ID) {
        return {CbJsonVal{-1}, CbError{"Missing JSON object field '" + key + "'", 404}};
    }
    return {CbJsonVal{static_cast<int64_t>(fid)}, CbError{"", 0}};
}

inline std::tuple<std::string, CbError> cbJsonGetStr(const CbJsonDoc& doc, CbJsonVal val) {
    if (val.id < 0) {
        return {std::string(""), CbError{"Invalid JSON value handle", 422}};
    }
    const auto res = cbVendorJson::jsonGetString(
        doc.arena,
        static_cast<cbVendorJson::JsonId>(val.id)
    );
    if (!res.has_value()) {
        return {std::string(""), CbError{"JSON value is not a string", 422}};
    }
    return {res.value(), CbError{"", 0}};
}

inline std::tuple<int64_t, CbError> cbJsonGetInt(const CbJsonDoc& doc, CbJsonVal val) {
    if (val.id < 0) {
        return {0, CbError{"Invalid JSON value handle", 422}};
    }
    const auto res = cbVendorJson::jsonGetInt(
        doc.arena,
        static_cast<cbVendorJson::JsonId>(val.id)
    );
    if (!res.has_value()) {
        return {0, CbError{"JSON value is not a number", 422}};
    }
    return {res.value(), CbError{"", 0}};
}

inline std::tuple<double, CbError> cbJsonGetFloat(const CbJsonDoc& doc, CbJsonVal val) {
    if (val.id < 0) {
        return {0.0, CbError{"Invalid JSON value handle", 422}};
    }
    const auto res = cbVendorJson::jsonGetFloat(
        doc.arena,
        static_cast<cbVendorJson::JsonId>(val.id)
    );
    if (!res.has_value()) {
        return {0.0, CbError{"JSON value is not a number", 422}};
    }
    return {res.value(), CbError{"", 0}};
}

inline std::tuple<bool, CbError> cbJsonGetBool(const CbJsonDoc& doc, CbJsonVal val) {
    if (val.id < 0) {
        return {false, CbError{"Invalid JSON value handle", 422}};
    }
    const auto res = cbVendorJson::jsonGetBool(
        doc.arena,
        static_cast<cbVendorJson::JsonId>(val.id)
    );
    if (!res.has_value()) {
        return {false, CbError{"JSON value is not a boolean", 422}};
    }
    return {res.value(), CbError{"", 0}};
}

inline bool cbJsonIsNull(const CbJsonDoc& doc, CbJsonVal val) {
    if (val.id < 0) {
        return false;
    }
    return cbVendorJson::jsonIsNull(
        doc.arena,
        static_cast<cbVendorJson::JsonId>(val.id)
    );
}

inline int64_t cbJsonArrayLen(const CbJsonDoc& doc, CbJsonVal arr) {
    if (arr.id < 0) {
        return 0;
    }
    const auto uid = static_cast<cbVendorJson::JsonId>(arr.id);
    if (uid >= doc.arena.nodes.size()) {
        return 0;
    }
    if (!std::holds_alternative<cbVendorJson::JsonArray>(doc.arena.nodes[uid])) {
        return 0;
    }
    return static_cast<int64_t>(std::get<cbVendorJson::JsonArray>(doc.arena.nodes[uid]).elements.size());
}

inline std::tuple<CbJsonVal, CbError> cbJsonArrayAt(
    const CbJsonDoc& doc,
    CbJsonVal arr,
    int64_t index
) {
    if (arr.id < 0 || index < 0) {
        return {CbJsonVal{-1}, CbError{"JSON array index out of bounds", 404}};
    }
    const auto uid = static_cast<cbVendorJson::JsonId>(arr.id);
    if (uid >= doc.arena.nodes.size() ||
        !std::holds_alternative<cbVendorJson::JsonArray>(doc.arena.nodes[uid])) {
        return {CbJsonVal{-1}, CbError{"JSON value is not an array", 422}};
    }
    const auto& elems = std::get<cbVendorJson::JsonArray>(doc.arena.nodes[uid]).elements;
    const auto uIdx = static_cast<size_t>(index);
    if (uIdx >= elems.size()) {
        return {CbJsonVal{-1}, CbError{"JSON array index out of bounds", 404}};
    }
    return {CbJsonVal{static_cast<int64_t>(elems[uIdx])}, CbError{"", 0}};
}

inline std::string cbJsonKind(const CbJsonDoc& doc, CbJsonVal val) {
    if (val.id < 0) {
        return "invalid";
    }
    const auto uid = static_cast<cbVendorJson::JsonId>(val.id);
    if (uid >= doc.arena.nodes.size()) {
        return "invalid";
    }
    const auto& node = doc.arena.nodes[uid];
    if (std::holds_alternative<cbVendorJson::JsonNull>(node)) {
        return "null";
    }
    if (std::holds_alternative<cbVendorJson::JsonBool>(node)) {
        return "bool";
    }
    if (std::holds_alternative<cbVendorJson::JsonNumber>(node)) {
        const auto& num = std::get<cbVendorJson::JsonNumber>(node);
        if (num.isInt) {
            return "int";
        }
        return "float";
    }
    if (std::holds_alternative<cbVendorJson::JsonString>(node)) {
        return "str";
    }
    if (std::holds_alternative<cbVendorJson::JsonArray>(node)) {
        return "array";
    }
    if (std::holds_alternative<cbVendorJson::JsonObject>(node)) {
        return "object";
    }
    return "invalid";
}

inline std::vector<std::string> cbJsonObjectKeys(const CbJsonDoc& doc, CbJsonVal obj) {
    std::vector<std::string> keys;
    if (obj.id < 0) {
        return keys;
    }
    const auto uid = static_cast<cbVendorJson::JsonId>(obj.id);
    if (uid >= doc.arena.nodes.size()) {
        return keys;
    }
    if (!std::holds_alternative<cbVendorJson::JsonObject>(doc.arena.nodes[uid])) {
        return keys;
    }
    const auto& fields = std::get<cbVendorJson::JsonObject>(doc.arena.nodes[uid]).fields;
    keys.reserve(fields.size());
    for (const auto& f : fields) {
        keys.push_back(f.key);
    }
    return keys;
}

#endif // CLEANBASIC_STDLIB_JSON_BRIDGE_HPP
