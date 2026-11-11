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
#include <functional>
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
inline std::string cbToStr(std::nullptr_t) { return "null"; }
inline std::string cbToStr(const void* val) {
    if (val == nullptr) {
        return "null";
    }
    return "<cptr>";
}
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

template <typename L, typename R>
inline CbError cbAssertCmp(bool ok, const char* opText, const L& lhs, const R& rhs, const char* file, int64_t line) {
    if (ok) {
        return CbError{"", 0};
    }
    return CbError{
        std::string("Assertion failed (") + opText + "): got " + cbToStr(lhs) + ", expected " + cbToStr(rhs),
        1,
        std::string(file),
        line
    };
}

inline std::string cbToStr(const std::vector<uint8_t>& val) {
    return "<bytes len=" + std::to_string(val.size()) + ">";
}

inline std::vector<uint8_t> cbBytesSlice(const std::vector<uint8_t>& b, int64_t start, int64_t end) {
    const int64_t n = static_cast<int64_t>(b.size());
    if (start < 0) start = 0;
    if (end > n) end = n;
    if (start >= end) return std::vector<uint8_t>{};
    return std::vector<uint8_t>(b.begin() + static_cast<size_t>(start), b.begin() + static_cast<size_t>(end));
}

inline std::vector<uint8_t> cbBytesSliceFrom(const std::vector<uint8_t>& b, int64_t start) {
    return cbBytesSlice(b, start, static_cast<int64_t>(b.size()));
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

inline std::tuple<std::string, CbError> cbBytesToUtf8(const std::vector<uint8_t>& b) {
    std::string s(reinterpret_cast<const char*>(b.data()), b.size());
    size_t i = 0;
    const size_t n = s.size();
    while (i < n) {
        const size_t seqLen = cbStrUtf8SeqLen(s, i);
        if (seqLen == 0) {
            return {std::string(""), CbError{"Invalid UTF-8 byte sequence", 1}};
        }
        i += seqLen;
    }
    return {s, CbError{"", 0}};
}

inline std::vector<uint8_t> cbStrToBytes(const std::string& s) {
    return std::vector<uint8_t>(s.begin(), s.end());
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

inline std::string cbBytesToHex(const std::vector<uint8_t>& b) {
    static const char hexChars[] = "0123456789abcdef";
    std::string out;
    out.reserve(b.size() * 2);
    for (uint8_t byte : b) {
        out.push_back(hexChars[(byte >> 4) & 0x0F]);
        out.push_back(hexChars[byte & 0x0F]);
    }
    return out;
}

inline std::tuple<std::vector<uint8_t>, CbError> cbBytesFromHex(const std::string& hex) {
    if (hex.size() % 2 != 0) {
        return {std::vector<uint8_t>{}, CbError{"Invalid hexadecimal string: length must be even", 400}};
    }
    std::vector<uint8_t> out;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        const int v1 = cbHexDigitValue(hex[i]);
        const int v2 = cbHexDigitValue(hex[i + 1]);
        if (v1 == -1 || v2 == -1) {
            return {std::vector<uint8_t>{}, CbError{"Invalid hexadecimal character in string", 400}};
        }
        out.push_back(static_cast<uint8_t>((v1 << 4) | v2));
    }
    return {out, CbError{"", 0}};
}
inline int64_t cbBytesReadU16Le(const std::vector<uint8_t>& b, int64_t offset) {
    if (offset < 0 || offset + 2 > static_cast<int64_t>(b.size())) return 0;
    const size_t off = static_cast<size_t>(offset);
    return static_cast<int64_t>(static_cast<uint16_t>(b[off]) | (static_cast<uint16_t>(b[off + 1]) << 8));
}

inline int64_t cbBytesReadU16Be(const std::vector<uint8_t>& b, int64_t offset) {
    if (offset < 0 || offset + 2 > static_cast<int64_t>(b.size())) return 0;
    const size_t off = static_cast<size_t>(offset);
    return static_cast<int64_t>((static_cast<uint16_t>(b[off]) << 8) | static_cast<uint16_t>(b[off + 1]));
}

inline int64_t cbBytesReadU32Le(const std::vector<uint8_t>& b, int64_t offset) {
    if (offset < 0 || offset + 4 > static_cast<int64_t>(b.size())) return 0;
    const size_t off = static_cast<size_t>(offset);
    uint32_t val = static_cast<uint32_t>(b[off]) |
                   (static_cast<uint32_t>(b[off + 1]) << 8) |
                   (static_cast<uint32_t>(b[off + 2]) << 16) |
                   (static_cast<uint32_t>(b[off + 3]) << 24);
    return static_cast<int64_t>(val);
}

inline int64_t cbBytesReadU32Be(const std::vector<uint8_t>& b, int64_t offset) {
    if (offset < 0 || offset + 4 > static_cast<int64_t>(b.size())) return 0;
    const size_t off = static_cast<size_t>(offset);
    uint32_t val = (static_cast<uint32_t>(b[off]) << 24) |
                   (static_cast<uint32_t>(b[off + 1]) << 16) |
                   (static_cast<uint32_t>(b[off + 2]) << 8) |
                   static_cast<uint32_t>(b[off + 3]);
    return static_cast<int64_t>(val);
}

inline int64_t cbBytesReadU64Le(const std::vector<uint8_t>& b, int64_t offset) {
    if (offset < 0 || offset + 8 > static_cast<int64_t>(b.size())) return 0;
    const size_t off = static_cast<size_t>(offset);
    uint64_t val = 0;
    for (size_t i = 0; i < 8; ++i) {
        val |= (static_cast<uint64_t>(b[off + i]) << (i * 8));
    }
    return static_cast<int64_t>(val);
}

inline int64_t cbBytesReadU64Be(const std::vector<uint8_t>& b, int64_t offset) {
    if (offset < 0 || offset + 8 > static_cast<int64_t>(b.size())) return 0;
    const size_t off = static_cast<size_t>(offset);
    uint64_t val = 0;
    for (size_t i = 0; i < 8; ++i) {
        val = (val << 8) | static_cast<uint64_t>(b[off + i]);
    }
    return static_cast<int64_t>(val);
}

inline void cbBytesWriteU16Le(std::vector<uint8_t>& b, int64_t val, int64_t offset) {
    if (offset < 0) return;
    const size_t needed = static_cast<size_t>(offset + 2);
    if (b.size() < needed) b.resize(needed, 0);
    const size_t off = static_cast<size_t>(offset);
    b[off] = static_cast<uint8_t>(val & 0xFF);
    b[off + 1] = static_cast<uint8_t>((val >> 8) & 0xFF);
}

inline void cbBytesWriteU16Be(std::vector<uint8_t>& b, int64_t val, int64_t offset) {
    if (offset < 0) return;
    const size_t needed = static_cast<size_t>(offset + 2);
    if (b.size() < needed) b.resize(needed, 0);
    const size_t off = static_cast<size_t>(offset);
    b[off] = static_cast<uint8_t>((val >> 8) & 0xFF);
    b[off + 1] = static_cast<uint8_t>(val & 0xFF);
}

inline void cbBytesWriteU32Le(std::vector<uint8_t>& b, int64_t val, int64_t offset) {
    if (offset < 0) return;
    const size_t needed = static_cast<size_t>(offset + 4);
    if (b.size() < needed) b.resize(needed, 0);
    const size_t off = static_cast<size_t>(offset);
    b[off] = static_cast<uint8_t>(val & 0xFF);
    b[off + 1] = static_cast<uint8_t>((val >> 8) & 0xFF);
    b[off + 2] = static_cast<uint8_t>((val >> 16) & 0xFF);
    b[off + 3] = static_cast<uint8_t>((val >> 24) & 0xFF);
}

inline void cbBytesWriteU32Be(std::vector<uint8_t>& b, int64_t val, int64_t offset) {
    if (offset < 0) return;
    const size_t needed = static_cast<size_t>(offset + 4);
    if (b.size() < needed) b.resize(needed, 0);
    const size_t off = static_cast<size_t>(offset);
    b[off] = static_cast<uint8_t>((val >> 24) & 0xFF);
    b[off + 1] = static_cast<uint8_t>((val >> 16) & 0xFF);
    b[off + 2] = static_cast<uint8_t>((val >> 8) & 0xFF);
    b[off + 3] = static_cast<uint8_t>(val & 0xFF);
}

inline void cbBytesWriteU64Le(std::vector<uint8_t>& b, int64_t val, int64_t offset) {
    if (offset < 0) return;
    const size_t needed = static_cast<size_t>(offset + 8);
    if (b.size() < needed) b.resize(needed, 0);
    const size_t off = static_cast<size_t>(offset);
    uint64_t uval = static_cast<uint64_t>(val);
    for (size_t i = 0; i < 8; ++i) {
        b[off + i] = static_cast<uint8_t>((uval >> (i * 8)) & 0xFF);
    }
}

inline void cbBytesWriteU64Be(std::vector<uint8_t>& b, int64_t val, int64_t offset) {
    if (offset < 0) return;
    const size_t needed = static_cast<size_t>(offset + 8);
    if (b.size() < needed) b.resize(needed, 0);
    const size_t off = static_cast<size_t>(offset);
    uint64_t uval = static_cast<uint64_t>(val);
    for (size_t i = 0; i < 8; ++i) {
        b[off + 7 - i] = static_cast<uint8_t>((uval >> (i * 8)) & 0xFF);
    }
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

inline int64_t cbBytesGet(const std::vector<uint8_t>& b, int64_t idx) {
    if (idx < 0 || static_cast<size_t>(idx) >= b.size()) {
        return 0;
    }
    return static_cast<int64_t>(b[static_cast<size_t>(idx)]);
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

inline int64_t cbBitShl(int64_t x, int64_t bits) {
    if (bits < 0 || bits >= 64) {
        return 0;
    }
    return static_cast<int64_t>(static_cast<uint64_t>(x) << static_cast<uint64_t>(bits));
}

inline int64_t cbBitShr(int64_t x, int64_t bits) {
    if (bits < 0 || bits >= 64) {
        return 0;
    }
    return static_cast<int64_t>(static_cast<uint64_t>(x) >> static_cast<uint64_t>(bits));
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

inline int64_t cbBytesPop(std::vector<uint8_t>& b) {
    if (b.empty()) {
        return 0;
    }
    int64_t val = static_cast<int64_t>(b.back());
    b.pop_back();
    return val;
}

inline bool cbRangeCond(int64_t curr, int64_t end, int64_t step, bool inclusive = true) {
    if (step < 0) {
        if (inclusive) {
            return curr >= end;
        }
        return curr > end;
    }
    if (inclusive) {
        return curr <= end;
    }
    return curr < end;
}

inline std::vector<std::string>& cbRuntimeArgs() {
    static std::vector<std::string> args;
    return args;
}

template <typename F> struct CbDeferTask { F f; ~CbDeferTask() { f(); } };
template <typename F> CbDeferTask<F> cbDefer(F f) { return {f}; }
#endif // CLEANBASIC_RUNTIME_PREAMBLE_INCLUDED
)";

} // namespace cb

#endif // CLEANBASIC_RUNTIME_PREAMBLE_HPP
