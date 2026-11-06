#ifndef CLEANBASIC_STDLIB_JSON_BRIDGE_HPP
#define CLEANBASIC_STDLIB_JSON_BRIDGE_HPP

#include <json.hpp>

#include <cstdint>
#include <string>
#include <tuple>
#include <utility>
#include <variant>

struct cb_json_doc {
    cb_vendor_json::JsonArena arena;
    int root = -1;
};

struct cb_json_val {
    int id = -1;
};

inline cb_json_doc cb_json_new_doc() {
    cb_json_doc doc{};
    doc.root = -1;
    return doc;
}

inline std::tuple<cb_json_doc, cb_error> cb_json_parse(const std::string& text) {
    auto res = cb_vendor_json::parse_json(text);
    if (!res.ok) {
        return {cb_json_doc{}, cb_error{res.error, 400}};
    }
    cb_json_doc doc{};
    doc.arena = std::move(res.arena);
    doc.root = static_cast<int>(res.root);
    return {std::move(doc), cb_error{"", 0}};
}

inline cb_json_val cb_json_root(const cb_json_doc& doc) {
    return cb_json_val{doc.root};
}

inline void cb_json_set_root(cb_json_doc& doc, cb_json_val val) {
    doc.root = val.id;
}

inline std::string cb_json_stringify_val(const cb_json_doc& doc, cb_json_val val) {
    if (val.id < 0) {
        return "null";
    }
    return cb_vendor_json::serialize_json(doc.arena, static_cast<cb_vendor_json::JsonId>(val.id));
}

inline std::string cb_json_stringify(const cb_json_doc& doc) {
    return cb_json_stringify_val(doc, cb_json_val{doc.root});
}

inline cb_json_val cb_json_new_object(cb_json_doc& doc) {
    return cb_json_val{static_cast<int>(cb_vendor_json::json_add_object(doc.arena))};
}

inline cb_json_val cb_json_new_array(cb_json_doc& doc) {
    return cb_json_val{static_cast<int>(cb_vendor_json::json_add_array(doc.arena))};
}

inline cb_json_val cb_json_new_str(cb_json_doc& doc, const std::string& val) {
    return cb_json_val{static_cast<int>(cb_vendor_json::json_add_string(doc.arena, val))};
}

inline cb_json_val cb_json_new_int(cb_json_doc& doc, int val) {
    return cb_json_val{static_cast<int>(cb_vendor_json::json_add_int(doc.arena, static_cast<int64_t>(val)))};
}

inline cb_json_val cb_json_new_float(cb_json_doc& doc, double val) {
    return cb_json_val{static_cast<int>(cb_vendor_json::json_add_float(doc.arena, val))};
}

inline cb_json_val cb_json_new_bool(cb_json_doc& doc, bool val) {
    return cb_json_val{static_cast<int>(cb_vendor_json::json_add_bool(doc.arena, val))};
}

inline cb_json_val cb_json_new_null(cb_json_doc& doc) {
    return cb_json_val{static_cast<int>(cb_vendor_json::json_add_null(doc.arena))};
}

inline void cb_json_object_set(cb_json_doc& doc, cb_json_val obj, const std::string& key, cb_json_val val) {
    if (obj.id < 0 || val.id < 0) {
        return;
    }
    cb_vendor_json::json_object_set(
        doc.arena,
        static_cast<cb_vendor_json::JsonId>(obj.id),
        key,
        static_cast<cb_vendor_json::JsonId>(val.id)
    );
}

inline void cb_json_array_push(cb_json_doc& doc, cb_json_val arr, cb_json_val elem) {
    if (arr.id < 0 || elem.id < 0) {
        return;
    }
    cb_vendor_json::json_array_push(
        doc.arena,
        static_cast<cb_vendor_json::JsonId>(arr.id),
        static_cast<cb_vendor_json::JsonId>(elem.id)
    );
}

inline bool cb_json_has_field(const cb_json_doc& doc, cb_json_val obj, const std::string& key) {
    if (obj.id < 0) {
        return false;
    }
    const auto fid = cb_vendor_json::json_get_field(
        doc.arena,
        static_cast<cb_vendor_json::JsonId>(obj.id),
        key
    );
    return fid != cb_vendor_json::JSON_INVALID_ID;
}

inline std::tuple<cb_json_val, cb_error> cb_json_get_field(
    const cb_json_doc& doc,
    cb_json_val obj,
    const std::string& key
) {
    if (obj.id < 0) {
        return {cb_json_val{-1}, cb_error{"Invalid JSON object handle", 404}};
    }
    const auto fid = cb_vendor_json::json_get_field(
        doc.arena,
        static_cast<cb_vendor_json::JsonId>(obj.id),
        key
    );
    if (fid == cb_vendor_json::JSON_INVALID_ID) {
        return {cb_json_val{-1}, cb_error{"Missing JSON object field '" + key + "'", 404}};
    }
    return {cb_json_val{static_cast<int>(fid)}, cb_error{"", 0}};
}

inline std::tuple<std::string, cb_error> cb_json_get_str(const cb_json_doc& doc, cb_json_val val) {
    if (val.id < 0) {
        return {std::string(""), cb_error{"Invalid JSON value handle", 422}};
    }
    const auto res = cb_vendor_json::json_get_string(
        doc.arena,
        static_cast<cb_vendor_json::JsonId>(val.id)
    );
    if (!res.has_value()) {
        return {std::string(""), cb_error{"JSON value is not a string", 422}};
    }
    return {res.value(), cb_error{"", 0}};
}

inline std::tuple<int, cb_error> cb_json_get_int(const cb_json_doc& doc, cb_json_val val) {
    if (val.id < 0) {
        return {0, cb_error{"Invalid JSON value handle", 422}};
    }
    const auto res = cb_vendor_json::json_get_int(
        doc.arena,
        static_cast<cb_vendor_json::JsonId>(val.id)
    );
    if (!res.has_value()) {
        return {0, cb_error{"JSON value is not a number", 422}};
    }
    return {static_cast<int>(res.value()), cb_error{"", 0}};
}

inline std::tuple<double, cb_error> cb_json_get_float(const cb_json_doc& doc, cb_json_val val) {
    if (val.id < 0) {
        return {0.0, cb_error{"Invalid JSON value handle", 422}};
    }
    const auto res = cb_vendor_json::json_get_float(
        doc.arena,
        static_cast<cb_vendor_json::JsonId>(val.id)
    );
    if (!res.has_value()) {
        return {0.0, cb_error{"JSON value is not a number", 422}};
    }
    return {res.value(), cb_error{"", 0}};
}

inline std::tuple<bool, cb_error> cb_json_get_bool(const cb_json_doc& doc, cb_json_val val) {
    if (val.id < 0) {
        return {false, cb_error{"Invalid JSON value handle", 422}};
    }
    const auto res = cb_vendor_json::json_get_bool(
        doc.arena,
        static_cast<cb_vendor_json::JsonId>(val.id)
    );
    if (!res.has_value()) {
        return {false, cb_error{"JSON value is not a boolean", 422}};
    }
    return {res.value(), cb_error{"", 0}};
}

inline bool cb_json_is_null(const cb_json_doc& doc, cb_json_val val) {
    if (val.id < 0) {
        return false;
    }
    return cb_vendor_json::json_is_null(
        doc.arena,
        static_cast<cb_vendor_json::JsonId>(val.id)
    );
}

inline int cb_json_array_len(const cb_json_doc& doc, cb_json_val arr) {
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

inline std::tuple<cb_json_val, cb_error> cb_json_array_at(
    const cb_json_doc& doc,
    cb_json_val arr,
    int index
) {
    if (arr.id < 0 || index < 0) {
        return {cb_json_val{-1}, cb_error{"JSON array index out of bounds", 404}};
    }
    const auto uid = static_cast<cb_vendor_json::JsonId>(arr.id);
    if (uid >= doc.arena.nodes.size() ||
        !std::holds_alternative<cb_vendor_json::JsonArray>(doc.arena.nodes[uid])) {
        return {cb_json_val{-1}, cb_error{"JSON value is not an array", 422}};
    }
    const auto& elems = std::get<cb_vendor_json::JsonArray>(doc.arena.nodes[uid]).elements;
    const auto u_idx = static_cast<size_t>(index);
    if (u_idx >= elems.size()) {
        return {cb_json_val{-1}, cb_error{"JSON array index out of bounds", 404}};
    }
    return {cb_json_val{static_cast<int>(elems[u_idx])}, cb_error{"", 0}};
}

#endif // CLEANBASIC_STDLIB_JSON_BRIDGE_HPP
