#ifndef CLEANBASIC_STDLIB_JSON_BRIDGE_HPP
#define CLEANBASIC_STDLIB_JSON_BRIDGE_HPP

#include <json.hpp>

#include <cstdint>
#include <string>
#include <tuple>
#include <utility>
#include <variant>

struct CbJsonDoc {
    cb_vendor_json::JsonArena arena;
    int root = -1;
};

struct CbJsonVal {
    int id = -1;
};

inline CbJsonDoc cbJsonNewDoc() {
    CbJsonDoc doc{};
    doc.root = -1;
    return doc;
}

inline std::tuple<CbJsonDoc, CbError> cbJsonParse(const std::string& text) {
    auto res = cb_vendor_json::parseJson(text);
    if (!res.ok) {
        return {CbJsonDoc{}, CbError{res.error, 400}};
    }
    CbJsonDoc doc{};
    doc.arena = std::move(res.arena);
    doc.root = static_cast<int>(res.root);
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
    return cb_vendor_json::serializeJson(doc.arena, static_cast<cb_vendor_json::JsonId>(val.id));
}

inline std::string cbJsonStringify(const CbJsonDoc& doc) {
    return cbJsonStringifyVal(doc, CbJsonVal{doc.root});
}

inline CbJsonVal cbJsonNewObject(CbJsonDoc& doc) {
    return CbJsonVal{static_cast<int>(cb_vendor_json::jsonAddObject(doc.arena))};
}

inline CbJsonVal cbJsonNewArray(CbJsonDoc& doc) {
    return CbJsonVal{static_cast<int>(cb_vendor_json::jsonAddArray(doc.arena))};
}

inline CbJsonVal cbJsonNewStr(CbJsonDoc& doc, const std::string& val) {
    return CbJsonVal{static_cast<int>(cb_vendor_json::jsonAddString(doc.arena, val))};
}

inline CbJsonVal cbJsonNewInt(CbJsonDoc& doc, int val) {
    return CbJsonVal{static_cast<int>(cb_vendor_json::jsonAddInt(doc.arena, static_cast<int64_t>(val)))};
}

inline CbJsonVal cbJsonNewFloat(CbJsonDoc& doc, double val) {
    return CbJsonVal{static_cast<int>(cb_vendor_json::jsonAddFloat(doc.arena, val))};
}

inline CbJsonVal cbJsonNewBool(CbJsonDoc& doc, bool val) {
    return CbJsonVal{static_cast<int>(cb_vendor_json::jsonAddBool(doc.arena, val))};
}

inline CbJsonVal cbJsonNewNull(CbJsonDoc& doc) {
    return CbJsonVal{static_cast<int>(cb_vendor_json::jsonAddNull(doc.arena))};
}

inline void cbJsonObjectSet(CbJsonDoc& doc, CbJsonVal obj, const std::string& key, CbJsonVal val) {
    if (obj.id < 0 || val.id < 0) {
        return;
    }
    cb_vendor_json::jsonObjectSet(
        doc.arena,
        static_cast<cb_vendor_json::JsonId>(obj.id),
        key,
        static_cast<cb_vendor_json::JsonId>(val.id)
    );
}

inline void cbJsonArrayPush(CbJsonDoc& doc, CbJsonVal arr, CbJsonVal elem) {
    if (arr.id < 0 || elem.id < 0) {
        return;
    }
    cb_vendor_json::jsonArrayPush(
        doc.arena,
        static_cast<cb_vendor_json::JsonId>(arr.id),
        static_cast<cb_vendor_json::JsonId>(elem.id)
    );
}

inline bool cbJsonHasField(const CbJsonDoc& doc, CbJsonVal obj, const std::string& key) {
    if (obj.id < 0) {
        return false;
    }
    const auto fid = cb_vendor_json::jsonGetField(
        doc.arena,
        static_cast<cb_vendor_json::JsonId>(obj.id),
        key
    );
    return fid != cb_vendor_json::JSON_INVALID_ID;
}

inline std::tuple<CbJsonVal, CbError> cbJsonGetField(
    const CbJsonDoc& doc,
    CbJsonVal obj,
    const std::string& key
) {
    if (obj.id < 0) {
        return {CbJsonVal{-1}, CbError{"Invalid JSON object handle", 404}};
    }
    const auto fid = cb_vendor_json::jsonGetField(
        doc.arena,
        static_cast<cb_vendor_json::JsonId>(obj.id),
        key
    );
    if (fid == cb_vendor_json::JSON_INVALID_ID) {
        return {CbJsonVal{-1}, CbError{"Missing JSON object field '" + key + "'", 404}};
    }
    return {CbJsonVal{static_cast<int>(fid)}, CbError{"", 0}};
}

inline std::tuple<std::string, CbError> cbJsonGetStr(const CbJsonDoc& doc, CbJsonVal val) {
    if (val.id < 0) {
        return {std::string(""), CbError{"Invalid JSON value handle", 422}};
    }
    const auto res = cb_vendor_json::jsonGetString(
        doc.arena,
        static_cast<cb_vendor_json::JsonId>(val.id)
    );
    if (!res.has_value()) {
        return {std::string(""), CbError{"JSON value is not a string", 422}};
    }
    return {res.value(), CbError{"", 0}};
}

inline std::tuple<int, CbError> cbJsonGetInt(const CbJsonDoc& doc, CbJsonVal val) {
    if (val.id < 0) {
        return {0, CbError{"Invalid JSON value handle", 422}};
    }
    const auto res = cb_vendor_json::jsonGetInt(
        doc.arena,
        static_cast<cb_vendor_json::JsonId>(val.id)
    );
    if (!res.has_value()) {
        return {0, CbError{"JSON value is not a number", 422}};
    }
    return {static_cast<int>(res.value()), CbError{"", 0}};
}

inline std::tuple<double, CbError> cbJsonGetFloat(const CbJsonDoc& doc, CbJsonVal val) {
    if (val.id < 0) {
        return {0.0, CbError{"Invalid JSON value handle", 422}};
    }
    const auto res = cb_vendor_json::jsonGetFloat(
        doc.arena,
        static_cast<cb_vendor_json::JsonId>(val.id)
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
    const auto res = cb_vendor_json::jsonGetBool(
        doc.arena,
        static_cast<cb_vendor_json::JsonId>(val.id)
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
    return cb_vendor_json::jsonIsNull(
        doc.arena,
        static_cast<cb_vendor_json::JsonId>(val.id)
    );
}

inline int cbJsonArrayLen(const CbJsonDoc& doc, CbJsonVal arr) {
    if (arr.id < 0) {
        return 0;
    }
    const auto uid = static_cast<cb_vendor_json::JsonId>(arr.id);
    if (uid >= doc.arena.nodes.size()) {
        return 0;
    }
    if (!std::holds_alternative<cb_vendor_json::JsonArray>(doc.arena.nodes[uid])) {
        return 0;
    }
    return static_cast<int>(std::get<cb_vendor_json::JsonArray>(doc.arena.nodes[uid]).elements.size());
}

inline std::tuple<CbJsonVal, CbError> cbJsonArrayAt(
    const CbJsonDoc& doc,
    CbJsonVal arr,
    int index
) {
    if (arr.id < 0 || index < 0) {
        return {CbJsonVal{-1}, CbError{"JSON array index out of bounds", 404}};
    }
    const auto uid = static_cast<cb_vendor_json::JsonId>(arr.id);
    if (uid >= doc.arena.nodes.size() ||
        !std::holds_alternative<cb_vendor_json::JsonArray>(doc.arena.nodes[uid])) {
        return {CbJsonVal{-1}, CbError{"JSON value is not an array", 422}};
    }
    const auto& elems = std::get<cb_vendor_json::JsonArray>(doc.arena.nodes[uid]).elements;
    const auto uIdx = static_cast<size_t>(index);
    if (uIdx >= elems.size()) {
        return {CbJsonVal{-1}, CbError{"JSON array index out of bounds", 404}};
    }
    return {CbJsonVal{static_cast<int>(elems[uIdx])}, CbError{"", 0}};
}

#endif // CLEANBASIC_STDLIB_JSON_BRIDGE_HPP
