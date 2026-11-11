#ifndef CLEANBASIC_RUNTIME_PREAMBLE_HPP
#define CLEANBASIC_RUNTIME_PREAMBLE_HPP

#include <string_view>

namespace cb {

inline int cbHexDigitValue(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }
    if (c >= 'A' && c <= 'F') {
        return 10 + (c - 'A');
    }
    return -1;
}

constexpr std::string_view CPP_RUNTIME_PREAMBLE = R"(#ifndef CLEANBASIC_RUNTIME_PREAMBLE_INCLUDED
#define CLEANBASIC_RUNTIME_PREAMBLE_INCLUDED
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <tuple>
#include <utility>
#include <variant>
#include <algorithm>
#include <type_traits>
#undef min
#undef max
#undef ERROR

struct [[nodiscard]] CbError {
    std::string msg = "";
    int64_t code = 0;
    std::string file = "";
    int64_t line = 0;
};

inline std::string cbToStr(const std::string& val) { return val; }
inline std::string cbToStr(int val) { return std::to_string(val); }
inline std::string cbToStr(int64_t val) { return std::to_string(val); }
inline std::string cbToStr(double val) { return std::to_string(val); }
inline std::string cbToStr(bool val) {
    if (val) {
        return "true";
    }
    return "false";
}
inline std::string cbToStr(const CbError& val) { return val.msg; }
template <typename T>
inline std::string cbToStr(const std::vector<T>& val) {
    return "<list len=" + std::to_string(val.size()) + ">";
}
inline bool cbHasError(const CbError& val) { return !val.msg.empty() || val.code != 0; }
inline void cbStampError(CbError& err, const char* file, int64_t line) {
    if ((err.code != 0 || !err.msg.empty()) && err.line == 0) {
        err.file = file;
        err.line = line;
    }
}

inline CbError cbAssertCond(bool ok, const std::string& msg, const char* file, int64_t line) {
    if (ok) {
        return CbError{"", 0};
    }
    return CbError{msg, 1, std::string(file), line};
}

inline size_t cbStrUtf8SeqLen(const std::string& s, size_t idx) {
    if (idx >= s.size()) {
        return 0;
    }
    const unsigned char b0 = static_cast<unsigned char>(s[idx]);
    if (b0 <= 0x7Fu) {
        return 1;
    }
    if (b0 >= 0xC2u && b0 <= 0xDFu) {
        if (idx + 1 >= s.size()) {
            return 0;
        }
        const unsigned char b1 = static_cast<unsigned char>(s[idx + 1]);
        if ((b1 & 0xC0u) != 0x80u) {
            return 0;
        }
        return 2;
    }
    if (b0 >= 0xE0u && b0 <= 0xEFu) {
        if (idx + 2 >= s.size()) {
            return 0;
        }
        const unsigned char b1 = static_cast<unsigned char>(s[idx + 1]);
        const unsigned char b2 = static_cast<unsigned char>(s[idx + 2]);
        if ((b1 & 0xC0u) != 0x80u || (b2 & 0xC0u) != 0x80u) {
            return 0;
        }
        if (b0 == 0xE0u && b1 < 0xA0u) {
            return 0;
        }
        if (b0 == 0xEDu && b1 >= 0xA0u) {
            return 0;
        }
        return 3;
    }
    if (b0 >= 0xF0u && b0 <= 0xF4u) {
        if (idx + 3 >= s.size()) {
            return 0;
        }
        const unsigned char b1 = static_cast<unsigned char>(s[idx + 1]);
        const unsigned char b2 = static_cast<unsigned char>(s[idx + 2]);
        const unsigned char b3 = static_cast<unsigned char>(s[idx + 3]);
        if ((b1 & 0xC0u) != 0x80u || (b2 & 0xC0u) != 0x80u || (b3 & 0xC0u) != 0x80u) {
            return 0;
        }
        if (b0 == 0xF0u && b1 < 0x90u) {
            return 0;
        }
        if (b0 == 0xF4u && b1 > 0x8Fu) {
            return 0;
        }
        return 4;
    }
    return 0;
}

inline size_t cbStrUtf8Step(const std::string& s, size_t idx) {
    const size_t n = cbStrUtf8SeqLen(s, idx);
    if (n > 0) {
        return n;
    }
    return 1;
}

inline std::string cbStrIndex(const std::string& s, int64_t idx) {
    if (idx < 0 || static_cast<size_t>(idx) >= s.size()) {
        return "";
    }
    return std::string(1, s[static_cast<size_t>(idx)]);
}

inline int64_t cbStrByteAtFast(const std::string& s, int64_t idx) {
    if (idx < 0 || static_cast<size_t>(idx) >= s.size()) {
        return -1;
    }
    return static_cast<int64_t>(static_cast<unsigned char>(s[static_cast<size_t>(idx)]));
}

inline std::vector<std::string> cbStrUtf8Chars(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    const size_t n = s.size();
    while (i < n) {
        const size_t step = cbStrUtf8Step(s, i);
        out.push_back(s.substr(i, step));
        i += step;
    }
    return out;
}

inline bool cbListGet(const std::vector<bool>& v, int64_t idx) {
    if (idx < 0 || static_cast<size_t>(idx) >= v.size()) {
        return false;
    }
    return v[static_cast<size_t>(idx)];
}

template <typename T>
inline const T& cbListGet(const std::vector<T>& v, int64_t idx) {
    static const T empty{};
    if (idx < 0 || static_cast<size_t>(idx) >= v.size()) {
        return empty;
    }
    return v[static_cast<size_t>(idx)];
}

template <typename T>
inline std::vector<T> cbListSlice(const std::vector<T>& v, int64_t start, int64_t end) {
    const int64_t n = static_cast<int64_t>(v.size());
    if (start < 0) start = 0;
    if (end > n) end = n;
    if (start >= end) return std::vector<T>{};
    return std::vector<T>(v.begin() + static_cast<size_t>(start), v.begin() + static_cast<size_t>(end));
}

template <typename T>
inline std::vector<T> cbListSliceFrom(const std::vector<T>& v, int64_t start) {
    return cbListSlice(v, start, static_cast<int64_t>(v.size()));
}

template <typename T, typename U>
inline bool cbListContains(const std::vector<T>& v, const U& val) {
    return std::find(v.begin(), v.end(), static_cast<T>(val)) != v.end();
}

template <typename T>
inline void cbListSort(std::vector<T>& v) {
    std::sort(v.begin(), v.end());
}

template <typename T>
inline T cbListRemoveAt(std::vector<T>& v, int64_t idx) {
    if (idx < 0 || static_cast<size_t>(idx) >= v.size()) {
        return T{};
    }
    const size_t uIdx = static_cast<size_t>(idx);
    T val = std::move(v[uIdx]);
    v.erase(v.begin() + static_cast<std::ptrdiff_t>(uIdx));
    return val;
}

template <typename K, typename V, typename KArg>
inline const V& cbDictAt(const std::unordered_map<K, V>& d, const KArg& k) {
    static const V empty{};
    const auto it = d.find(static_cast<K>(k));
    if (it != d.end()) {
        return it->second;
    }
    return empty;
}

template <typename K, typename V, typename KArg, typename VArg>
inline V cbDictGet(const std::unordered_map<K, V>& d, const KArg& k, const VArg& defVal) {
    const auto it = d.find(static_cast<K>(k));
    if (it != d.end()) {
        return it->second;
    }
    return static_cast<V>(defVal);
}

template <typename K, typename = void>
struct CbHasLess : std::false_type {};

template <typename K>
struct CbHasLess<K, std::void_t<decltype(std::declval<const K&>() < std::declval<const K&>())>> : std::true_type {};

template <typename K, typename V>
inline std::vector<K> cbDictKeys(const std::unordered_map<K, V>& d) {
    std::vector<K> keys;
    keys.reserve(d.size());
    for (const auto& kv : d) {
        keys.push_back(kv.first);
    }
    if constexpr (CbHasLess<K>::value) {
        std::sort(keys.begin(), keys.end());
    }
    return keys;
}

template <typename T>
inline T cbListPop(std::vector<T>& v) {
    if (v.empty()) {
        return T{};
    }
    T val = std::move(v.back());
    v.pop_back();
    return val;
}

inline std::vector<std::string>& cbRuntimeArgs() {
    static std::vector<std::string> args;
    return args;
}
#endif // CLEANBASIC_RUNTIME_PREAMBLE_INCLUDED
)";

} // namespace cb

#endif // CLEANBASIC_RUNTIME_PREAMBLE_HPP
