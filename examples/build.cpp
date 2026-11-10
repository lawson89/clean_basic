#ifndef CLEANBASIC_RUNTIME_PREAMBLE_INCLUDED
#define CLEANBASIC_RUNTIME_PREAMBLE_INCLUDED
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

inline std::tuple<std::string, CbError> cbBytesToUtf8(const std::vector<uint8_t>& b) {
    size_t i = 0;
    const size_t n = b.size();
    while (i < n) {
        uint8_t byte = b[i];
        if (byte <= 0x7F) {
            i += 1;
        } else if ((byte & 0xE0) == 0xC0) {
            if (i + 1 >= n || (b[i + 1] & 0xC0) != 0x80) return {std::string(""), CbError{"Invalid UTF-8 byte sequence", 1}};
            i += 2;
        } else if ((byte & 0xF0) == 0xE0) {
            if (i + 2 >= n || (b[i + 1] & 0xC0) != 0x80 || (b[i + 2] & 0xC0) != 0x80) return {std::string(""), CbError{"Invalid UTF-8 byte sequence", 1}};
            i += 3;
        } else if ((byte & 0xF8) == 0xF0) {
            if (i + 3 >= n || (b[i + 1] & 0xC0) != 0x80 || (b[i + 2] & 0xC0) != 0x80 || (b[i + 3] & 0xC0) != 0x80) return {std::string(""), CbError{"Invalid UTF-8 byte sequence", 1}};
            i += 4;
        } else {
            return {std::string(""), CbError{"Invalid UTF-8 byte sequence", 1}};
        }
    }
    return {std::string(reinterpret_cast<const char*>(b.data()), b.size()), CbError{"", 0}};
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
        const unsigned char b0 = static_cast<unsigned char>(s[i]);
        size_t step = 1;
        if (b0 >= 0xC2u && b0 <= 0xDFu && i + 1 < n) {
            const unsigned char b1 = static_cast<unsigned char>(s[i + 1]);
            if ((b1 & 0xC0u) == 0x80u) {
                step = 2;
            }
        } else if (b0 >= 0xE0u && b0 <= 0xEFu && i + 2 < n) {
            const unsigned char b1 = static_cast<unsigned char>(s[i + 1]);
            const unsigned char b2 = static_cast<unsigned char>(s[i + 2]);
            if ((b1 & 0xC0u) == 0x80u && (b2 & 0xC0u) == 0x80u) {
                if (!(b0 == 0xE0u && b1 < 0xA0u) && !(b0 == 0xEDu && b1 >= 0xA0u)) {
                    step = 3;
                }
            }
        } else if (b0 >= 0xF0u && b0 <= 0xF4u && i + 3 < n) {
            const unsigned char b1 = static_cast<unsigned char>(s[i + 1]);
            const unsigned char b2 = static_cast<unsigned char>(s[i + 2]);
            const unsigned char b3 = static_cast<unsigned char>(s[i + 3]);
            if ((b1 & 0xC0u) == 0x80u && (b2 & 0xC0u) == 0x80u && (b3 & 0xC0u) == 0x80u) {
                if (!(b0 == 0xF0u && b1 < 0x90u) && !(b0 == 0xF4u && b1 > 0x8Fu)) {
                    step = 4;
                }
            }
        }
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
    auto hexVal = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::vector<uint8_t> out;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        int v1 = hexVal(hex[i]);
        int v2 = hexVal(hex[i + 1]);
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


#include "str/str_bridge.hpp"
#include "os/os_bridge.hpp"
#include "fs/fs_bridge.hpp"

struct cb_std_cli__FlagSpec;
struct cb_std_cli__OptionSpec;
struct cb_std_cli__ArgSpec;
struct cb_std_cli__Parser;
struct cb_std_cli__Args;
using cb_std_fs__File = CbFile;

struct cb_std_cli__FlagSpec {
    std::string name{};
    std::string shortName = std::string("");
    std::string help = std::string("");
};

struct cb_std_cli__OptionSpec {
    std::string name{};
    std::string shortName = std::string("");
    std::string help = std::string("");
    std::string defaultVal = std::string("");
    bool required = false;
};

struct cb_std_cli__ArgSpec {
    std::string name{};
    std::string help = std::string("");
    bool required = true;
};

struct cb_std_cli__Parser {
    std::string name{};
    std::string version = std::string("");
    std::string description = std::string("");
    std::vector<cb_std_cli__FlagSpec> flags{};
    std::vector<cb_std_cli__OptionSpec> options{};
    std::vector<cb_std_cli__ArgSpec> args{};
};

struct cb_std_cli__Args {
    std::unordered_map<std::string, bool> flags{};
    std::unordered_map<std::string, std::string> options{};
    std::unordered_map<std::string, std::string> positional{};
    std::vector<std::string> rest{};
    bool helpRequested = false;
    bool versionRequested = false;
};

// Forward Declarations
bool cb_std_str__contains(const std::string& s, const std::string& sub);
int64_t cb_std_str__find(const std::string& s, const std::string& sub);
bool cb_std_str__startsWith(const std::string& s, const std::string& prefix);
bool cb_std_str__asciiContainsIgnoreCase(const std::string& s, const std::string& sub);
std::string cb_std_str__join(const std::vector<std::string>& parts, const std::string& sep);
std::string cb_std_str__rtrim(const std::string& s);
std::string cb_std_str__padRight(const std::string& s, int64_t width, const std::string& pad);
std::string cb_std_str__slice(const std::string& s, int64_t start, int64_t stop);
std::string cb_std_str__sliceFrom(const std::string& s, int64_t start);
std::string cb_std_str__charAt(const std::string& s, int64_t idx);
std::vector<std::string> cb_std_os__args();
void cb_std_os__eprint(const std::string& msg);
cb_std_cli__Parser cb_std_cli__newParser(const std::string& name, const std::string& version, const std::string& description);
void cb_std_cli__addFlag(cb_std_cli__Parser& p, const std::string& name, const std::string& shortName, const std::string& help);
void cb_std_cli__addArg(cb_std_cli__Parser& p, const std::string& name, const std::string& help, bool required);
int64_t cb_std_cli__findFlagByName(const cb_std_cli__Parser& p, const std::string& name);
int64_t cb_std_cli__findFlagByShort(const cb_std_cli__Parser& p, const std::string& shortName);
int64_t cb_std_cli__findOptionByName(const cb_std_cli__Parser& p, const std::string& name);
int64_t cb_std_cli__findOptionByShort(const cb_std_cli__Parser& p, const std::string& shortName);
std::string cb_std_cli__usage(const cb_std_cli__Parser& p);
void cb_std_cli__printUsage(const cb_std_cli__Parser& p);
[[nodiscard]] std::tuple<cb_std_cli__Args, CbError> cb_std_cli__parseArgs(const cb_std_cli__Parser& p, const std::vector<std::string>& rawArgs);
[[nodiscard]] std::tuple<cb_std_cli__Args, CbError> cb_std_cli__parse(const cb_std_cli__Parser& p);
bool cb_std_cli__has(const cb_std_cli__Args& a, const std::string& name);
std::string cb_std_cli__get(const cb_std_cli__Args& a, const std::string& name, const std::string& defaultVal);
CbFile cb_std_fs__stdin();
[[nodiscard]] std::tuple<CbFile, CbError> cb_std_fs__open(const std::string& path);
[[nodiscard]] std::tuple<bool, CbError> cb_std_fs__readLine(const CbFile& f, std::string& outLine);
void cb_std_fs__close(CbFile& f);
[[nodiscard]] std::tuple<std::vector<std::string>, CbError> cb_std_fs__walkFiles(const std::string& path);
bool cb_std_fs__isDir(const std::string& path);
std::string cb_std_fs__fileName(const std::string& path);
bool matchLine(const std::string& line, const std::string& pattern, bool ignoreCase, bool lineNumbers, bool invert, bool countOnly, const std::string& source, int64_t lineNum);
[[nodiscard]] std::tuple<int64_t, CbError> search(const std::string& path, const std::string& pattern, bool ignoreCase, bool lineNumbers, bool invert, bool countOnly);
int64_t cbMain();

bool cb_std_str__contains(const std::string& s, const std::string& sub) {
    return cbStrContains(s, sub);
}

int64_t cb_std_str__find(const std::string& s, const std::string& sub) {
    return cbStrFind(s, sub);
}

bool cb_std_str__startsWith(const std::string& s, const std::string& prefix) {
    return cbStrStartsWith(s, prefix);
}

bool cb_std_str__asciiContainsIgnoreCase(const std::string& s, const std::string& sub) {
    return cbStrAsciiContainsIgnoreCase(s, sub);
}

std::string cb_std_str__join(const std::vector<std::string>& parts, const std::string& sep) {
    return cbStrJoin(parts, sep);
}

std::string cb_std_str__rtrim(const std::string& s) {
    return cbStrRtrim(s);
}

std::string cb_std_str__padRight(const std::string& s, int64_t width, const std::string& pad) {
    return cbStrPadRight(s, width, pad);
}

std::string cb_std_str__slice(const std::string& s, int64_t start, int64_t stop) {
    return cbStrSlice(s, start, stop);
}

std::string cb_std_str__sliceFrom(const std::string& s, int64_t start) {
    return cbStrSliceFrom(s, start);
}

std::string cb_std_str__charAt(const std::string& s, int64_t idx) {
    return cbStrCharAt(s, idx);
}

std::vector<std::string> cb_std_os__args() {
    return cbOsArgs();
}

void cb_std_os__eprint(const std::string& msg) {
    cbOsEprint(msg);
}

cb_std_cli__Parser cb_std_cli__newParser(const std::string& name, const std::string& version, const std::string& description) {
    std::vector<cb_std_cli__FlagSpec> flags = {};
    std::vector<cb_std_cli__OptionSpec> options = {};
    std::vector<cb_std_cli__ArgSpec> args = {};
    return cb_std_cli__Parser{name, version, description, flags, options, args};
}

void cb_std_cli__addFlag(cb_std_cli__Parser& p, const std::string& name, const std::string& shortName, const std::string& help) {
    p.flags.push_back(cb_std_cli__FlagSpec{name, shortName, help});
}

void cb_std_cli__addArg(cb_std_cli__Parser& p, const std::string& name, const std::string& help, bool required) {
    p.args.push_back(cb_std_cli__ArgSpec{name, help, required});
}

int64_t cb_std_cli__findFlagByName(const cb_std_cli__Parser& p, const std::string& name) {
    for (int64_t i = 0; i <= (static_cast<int64_t>(p.flags.size()) - 1); i += 1) {
        if ((cbListGet(p.flags, i).name == name)) {
            return i;
        }
    }
    return (-1);
}

int64_t cb_std_cli__findFlagByShort(const cb_std_cli__Parser& p, const std::string& shortName) {
    if ((shortName == std::string(""))) {
        return (-1);
    }
    for (int64_t i = 0; i <= (static_cast<int64_t>(p.flags.size()) - 1); i += 1) {
        if ((cbListGet(p.flags, i).shortName == shortName)) {
            return i;
        }
    }
    return (-1);
}

int64_t cb_std_cli__findOptionByName(const cb_std_cli__Parser& p, const std::string& name) {
    for (int64_t i = 0; i <= (static_cast<int64_t>(p.options.size()) - 1); i += 1) {
        if ((cbListGet(p.options, i).name == name)) {
            return i;
        }
    }
    return (-1);
}

int64_t cb_std_cli__findOptionByShort(const cb_std_cli__Parser& p, const std::string& shortName) {
    if ((shortName == std::string(""))) {
        return (-1);
    }
    for (int64_t i = 0; i <= (static_cast<int64_t>(p.options.size()) - 1); i += 1) {
        if ((cbListGet(p.options, i).shortName == shortName)) {
            return i;
        }
    }
    return (-1);
}

std::string cb_std_cli__usage(const cb_std_cli__Parser& p) {
    std::vector<std::string> lines = {};
    if (((p.description != std::string("")) && (p.version != std::string("")))) {
        lines.push_back(((((((std::string("") + p.name) + std::string(" ")) + p.version) + std::string(" - ")) + p.description) + std::string("")));
    } else if ((p.description != std::string(""))) {
        lines.push_back(((((std::string("") + p.name) + std::string(" - ")) + p.description) + std::string("")));
    } else if ((p.version != std::string(""))) {
        lines.push_back(((((std::string("") + p.name) + std::string(" ")) + p.version) + std::string("")));
    }
    std::string synopsis = ((std::string("Usage: ") + p.name) + std::string(""));
    if (((static_cast<int64_t>(p.flags.size()) > 0) || (static_cast<int64_t>(p.options.size()) > 0))) {
        synopsis = (synopsis + std::string(" [options]"));
    }
    for (const auto& a : p.args) {
        if (a.required) {
            synopsis = (synopsis + ((std::string(" <") + a.name) + std::string(">")));
        } else {
            synopsis = (synopsis + ((std::string(" [") + a.name) + std::string("]")));
        }
    }
    lines.push_back(synopsis);
    if ((static_cast<int64_t>(p.args.size()) > 0)) {
        lines.push_back(std::string(""));
        lines.push_back(std::string("Arguments:"));
        for (const auto& a : p.args) {
            std::string label = cb_std_str__padRight(((std::string("  <") + a.name) + std::string(">")), 22, std::string(" "));
            if ((a.help != std::string(""))) {
                lines.push_back(((((std::string("") + label) + std::string(" ")) + a.help) + std::string("")));
            } else {
                lines.push_back(cb_std_str__rtrim(label));
            }
        }
    }
    lines.push_back(std::string(""));
    lines.push_back(std::string("Options:"));
    for (const auto& f : p.flags) {
        std::string flagSpec = std::string("");
        if ((f.shortName != std::string(""))) {
            flagSpec = ((((std::string("  -") + f.shortName) + std::string(", --")) + f.name) + std::string(""));
        } else {
            flagSpec = ((std::string("      --") + f.name) + std::string(""));
        }
        std::string padded = cb_std_str__padRight(flagSpec, 22, std::string(" "));
        if ((f.help != std::string(""))) {
            lines.push_back(((((std::string("") + padded) + std::string(" ")) + f.help) + std::string("")));
        } else {
            lines.push_back(cb_std_str__rtrim(padded));
        }
    }
    for (const auto& o : p.options) {
        std::string optSpec = std::string("");
        if ((o.shortName != std::string(""))) {
            optSpec = ((((std::string("  -") + o.shortName) + std::string(", --")) + o.name) + std::string(" <val>"));
        } else {
            optSpec = ((std::string("      --") + o.name) + std::string(" <val>"));
        }
        std::string padded = cb_std_str__padRight(optSpec, 22, std::string(" "));
        std::string desc = o.help;
        if ((o.defaultVal != std::string(""))) {
            if ((desc != std::string(""))) {
                desc = (desc + ((std::string(" (default: ") + o.defaultVal) + std::string(")")));
            } else {
                desc = ((std::string("(default: ") + o.defaultVal) + std::string(")"));
            }
        } else if (o.required) {
            if ((desc != std::string(""))) {
                desc = (desc + std::string(" (required)"));
            } else {
                desc = std::string("(required)");
            }
        }
        if ((desc != std::string(""))) {
            lines.push_back(((((std::string("") + padded) + std::string(" ")) + desc) + std::string("")));
        } else {
            lines.push_back(cb_std_str__rtrim(padded));
        }
    }
    std::string helpPad = cb_std_str__padRight(std::string("  -h, --help"), 22, std::string(" "));
    lines.push_back(((std::string("") + helpPad) + std::string(" Show this help message")));
    return cb_std_str__join(lines, std::string("\n"));
}

void cb_std_cli__printUsage(const cb_std_cli__Parser& p) {
    (std::cout << cb_std_cli__usage(p) << std::endl);
}

[[nodiscard]] std::tuple<cb_std_cli__Args, CbError> cb_std_cli__parseArgs(const cb_std_cli__Parser& p, const std::vector<std::string>& rawArgs) {
    std::unordered_map<std::string, bool> flags = {};
    std::unordered_map<std::string, std::string> options = {};
    std::unordered_map<std::string, bool> providedOptions = {};
    std::unordered_map<std::string, std::string> positional = {};
    std::vector<std::string> rest = {};
    for (const auto& f : p.flags) {
        flags[f.name] = false;
    }
    for (const auto& o : p.options) {
        if ((o.defaultVal != std::string(""))) {
            options[o.name] = o.defaultVal;
        }
    }
    int64_t i = 0;
    bool flagsDone = false;
    int64_t posIdx = 0;
    while ((i < static_cast<int64_t>(rawArgs.size()))) {
        std::string arg = cbListGet(rawArgs, i);
        if (((!flagsDone) && (arg == std::string("--")))) {
            flagsDone = true;
            i = (i + 1);
            continue;
        }
        if ((((!flagsDone) && cb_std_str__startsWith(arg, std::string("--"))) && (static_cast<int64_t>(arg.size()) > 2))) {
            std::string body = cb_std_str__sliceFrom(arg, 2);
            int64_t eqIdx = cb_std_str__find(body, std::string("="));
            if ((eqIdx >= 0)) {
                std::string optName = cb_std_str__slice(body, 0, eqIdx);
                std::string optVal = cb_std_str__sliceFrom(body, (eqIdx + 1));
                int64_t oIdx = cb_std_cli__findOptionByName(p, optName);
                if ((oIdx >= 0)) {
                    options[optName] = optVal;
                    providedOptions[optName] = true;
                } else {
                    int64_t fIdx = cb_std_cli__findFlagByName(p, optName);
                    if ((fIdx >= 0)) {
                        if (((optVal == std::string("true")) || (optVal == std::string("1")))) {
                            flags[optName] = true;
                        } else if (((optVal == std::string("false")) || (optVal == std::string("0")))) {
                            flags[optName] = false;
                        } else {
                            return { cb_std_cli__Args{}, CbError{((((std::string("Flag '--") + optName) + std::string("' does not take value '")) + optVal) + std::string("'")), 1, std::string("/home/lawson89/code/clean_basic/stdlib/cli/cli.cb"), 255} };
                        }
                    } else {
                        return { cb_std_cli__Args{}, CbError{((std::string("Unknown option '--") + optName) + std::string("'")), 1, std::string("/home/lawson89/code/clean_basic/stdlib/cli/cli.cb"), 258} };
                    }
                }
            } else {
                int64_t fIdx = cb_std_cli__findFlagByName(p, body);
                if ((fIdx >= 0)) {
                    flags[body] = true;
                } else {
                    int64_t oIdx = cb_std_cli__findOptionByName(p, body);
                    if ((oIdx >= 0)) {
                        if (((i + 1) >= static_cast<int64_t>(rawArgs.size()))) {
                            return { cb_std_cli__Args{}, CbError{((std::string("Option '--") + body) + std::string("' requires a value")), 1, std::string("/home/lawson89/code/clean_basic/stdlib/cli/cli.cb"), 269} };
                        }
                        i = (i + 1);
                        options[body] = cbListGet(rawArgs, i);
                        providedOptions[body] = true;
                    } else if ((body == std::string("help"))) {
                        flags[std::string("help")] = true;
                        return { cb_std_cli__Args{flags, options, positional, rest, true, false}, CbError{"", 0} };
                    } else if (((body == std::string("version")) && (p.version != std::string("")))) {
                        flags[std::string("version")] = true;
                        return { cb_std_cli__Args{flags, options, positional, rest, false, true}, CbError{"", 0} };
                    } else {
                        return { cb_std_cli__Args{}, CbError{((std::string("Unknown option '--") + body) + std::string("'")), 1, std::string("/home/lawson89/code/clean_basic/stdlib/cli/cli.cb"), 281} };
                    }
                }
            }
            i = (i + 1);
            continue;
        }
        if (((((!flagsDone) && cb_std_str__startsWith(arg, std::string("-"))) && (static_cast<int64_t>(arg.size()) > 1)) && (!cb_std_str__startsWith(arg, std::string("--"))))) {
            std::string cluster = cb_std_str__sliceFrom(arg, 1);
            if (((static_cast<int64_t>(cluster.size()) >= 2) && (cb_std_str__charAt(cluster, 1) == std::string("=")))) {
                std::string shortCh = cb_std_str__charAt(cluster, 0);
                std::string optVal = cb_std_str__sliceFrom(cluster, 2);
                int64_t oIdx = cb_std_cli__findOptionByShort(p, shortCh);
                if ((oIdx >= 0)) {
                    std::string optName = cbListGet(p.options, oIdx).name;
                    options[optName] = optVal;
                    providedOptions[optName] = true;
                } else {
                    return { cb_std_cli__Args{}, CbError{((std::string("Unknown option '-") + shortCh) + std::string("'")), 1, std::string("/home/lawson89/code/clean_basic/stdlib/cli/cli.cb"), 300} };
                }
            } else {
                int64_t cIdx = 0;
                while ((cIdx < static_cast<int64_t>(cluster.size()))) {
                    std::string ch = cb_std_str__charAt(cluster, cIdx);
                    int64_t fIdx = cb_std_cli__findFlagByShort(p, ch);
                    if ((fIdx >= 0)) {
                        flags[cbListGet(p.flags, fIdx).name] = true;
                        cIdx = (cIdx + 1);
                    } else {
                        int64_t oIdx = cb_std_cli__findOptionByShort(p, ch);
                        if ((oIdx >= 0)) {
                            std::string optName = cbListGet(p.options, oIdx).name;
                            if (((cIdx + 1) < static_cast<int64_t>(cluster.size()))) {
                                options[optName] = cb_std_str__sliceFrom(cluster, (cIdx + 1));
                                providedOptions[optName] = true;
                                break;
                            } else {
                                if (((i + 1) >= static_cast<int64_t>(rawArgs.size()))) {
                                    return { cb_std_cli__Args{}, CbError{((std::string("Option '-") + ch) + std::string("' requires a value")), 1, std::string("/home/lawson89/code/clean_basic/stdlib/cli/cli.cb"), 320} };
                                }
                                i = (i + 1);
                                options[optName] = cbListGet(rawArgs, i);
                                providedOptions[optName] = true;
                                cIdx = (cIdx + 1);
                            }
                        } else if ((ch == std::string("h"))) {
                            flags[std::string("help")] = true;
                            return { cb_std_cli__Args{flags, options, positional, rest, true, false}, CbError{"", 0} };
                        } else if (((ch == std::string("V")) && (p.version != std::string("")))) {
                            flags[std::string("version")] = true;
                            return { cb_std_cli__Args{flags, options, positional, rest, false, true}, CbError{"", 0} };
                        } else {
                            return { cb_std_cli__Args{}, CbError{((std::string("Unknown option '-") + ch) + std::string("'")), 1, std::string("/home/lawson89/code/clean_basic/stdlib/cli/cli.cb"), 334} };
                        }
                    }
                }
            }
            i = (i + 1);
            continue;
        }
        if ((posIdx < static_cast<int64_t>(p.args.size()))) {
            cb_std_cli__ArgSpec spec = cbListGet(p.args, posIdx);
            positional[spec.name] = arg;
            posIdx = (posIdx + 1);
        } else {
            rest.push_back(arg);
        }
        i = (i + 1);
    }
    for (const auto& o : p.options) {
        if (((o.required && (!cbDictGet(providedOptions, o.name, false))) && (o.defaultVal == std::string("")))) {
            return { cb_std_cli__Args{}, CbError{((std::string("Missing required option '--") + o.name) + std::string("'")), 1, std::string("/home/lawson89/code/clean_basic/stdlib/cli/cli.cb"), 355} };
        }
    }
    for (const auto& a : p.args) {
        if ((a.required && (!(positional.count(a.name) > 0)))) {
            return { cb_std_cli__Args{}, CbError{((std::string("Missing required argument '<") + a.name) + std::string(">'")), 1, std::string("/home/lawson89/code/clean_basic/stdlib/cli/cli.cb"), 361} };
        }
    }
    return { cb_std_cli__Args{flags, options, positional, rest, false, false}, CbError{"", 0} };
}

[[nodiscard]] std::tuple<cb_std_cli__Args, CbError> cb_std_cli__parse(const cb_std_cli__Parser& p) {
    std::vector<std::string> argv = cb_std_os__args();
    std::vector<std::string> rawArgs = {};
    if ((static_cast<int64_t>(argv.size()) > 1)) {
        rawArgs = cbListSliceFrom(argv, 1);
    }
    auto [_val1, _err1] = cb_std_cli__parseArgs(p, rawArgs);
    if (cbHasError(_err1)) {
        cbStampError(_err1, "/home/lawson89/code/clean_basic/stdlib/cli/cli.cb", 375);
        return { cb_std_cli__Args{}, _err1 };
    }
    return { _val1, CbError{"", 0} };
}

bool cb_std_cli__has(const cb_std_cli__Args& a, const std::string& name) {
    if (cbDictGet(a.flags, name, false)) {
        return true;
    }
    if (((a.options.count(name) > 0) && (cbDictAt(a.options, name) != std::string("")))) {
        return true;
    }
    if ((a.positional.count(name) > 0)) {
        return true;
    }
    return false;
}

std::string cb_std_cli__get(const cb_std_cli__Args& a, const std::string& name, const std::string& defaultVal) {
    if ((a.options.count(name) > 0)) {
        return cbDictAt(a.options, name);
    }
    if ((a.positional.count(name) > 0)) {
        return cbDictAt(a.positional, name);
    }
    return defaultVal;
}

CbFile cb_std_fs__stdin() {
    return cbFsStdin();
}

[[nodiscard]] std::tuple<CbFile, CbError> cb_std_fs__open(const std::string& path) {
    auto [_val2, _err2] = cbFsOpen(path);
    if (cbHasError(_err2)) {
        cbStampError(_err2, "/home/lawson89/code/clean_basic/stdlib/fs/fs.cb", 98);
        return { CbFile{}, _err2 };
    }
    return { _val2, CbError{"", 0} };
}

[[nodiscard]] std::tuple<bool, CbError> cb_std_fs__readLine(const CbFile& f, std::string& outLine) {
    auto [_val3, _err3] = cbFsReadLine(f, outLine);
    if (cbHasError(_err3)) {
        cbStampError(_err3, "/home/lawson89/code/clean_basic/stdlib/fs/fs.cb", 108);
        return { {}, _err3 };
    }
    return { _val3, CbError{"", 0} };
}

void cb_std_fs__close(CbFile& f) {
    cbFsClose(f);
}

[[nodiscard]] std::tuple<std::vector<std::string>, CbError> cb_std_fs__walkFiles(const std::string& path) {
    auto [_val4, _err4] = cbFsWalkFiles(path);
    if (cbHasError(_err4)) {
        cbStampError(_err4, "/home/lawson89/code/clean_basic/stdlib/fs/fs.cb", 160);
        return { std::vector<std::string>{}, _err4 };
    }
    return { _val4, CbError{"", 0} };
}

bool cb_std_fs__isDir(const std::string& path) {
    return cbFsIsDir(path);
}

std::string cb_std_fs__fileName(const std::string& path) {
    return cbFsFileName(path);
}

bool matchLine(const std::string& line, const std::string& pattern, bool ignoreCase, bool lineNumbers, bool invert, bool countOnly, const std::string& source, int64_t lineNum) {
    bool matched = false;
    if (ignoreCase) {
        matched = cb_std_str__asciiContainsIgnoreCase(line, pattern);
    } else {
        matched = cb_std_str__contains(line, pattern);
    }
    if (invert) {
        matched = (!matched);
    }
    if (matched) {
        if ((!countOnly)) {
            if (lineNumbers) {
                (std::cout << ((((((std::string("") + source) + std::string(":")) + cbToStr(lineNum)) + std::string(":")) + line) + std::string("")) << std::endl);
            } else {
                (std::cout << ((((std::string("") + source) + std::string(":")) + line) + std::string("")) << std::endl);
            }
        }
    }
    return matched;
}

[[nodiscard]] std::tuple<int64_t, CbError> search(const std::string& path, const std::string& pattern, bool ignoreCase, bool lineNumbers, bool invert, bool countOnly) {
    CbFile f = cb_std_fs__stdin();
    std::string source = std::string("{stdout}");
    if ((path != std::string("-"))) {
        auto [_val5, _err5] = cb_std_fs__open(path);
        if (cbHasError(_err5)) {
            cbStampError(_err5, "examples/cbgrep.cb", 40);
            return { {}, _err5 };
        }
        f = _val5;
        source = path;
    }
    auto _defer_1 = cbDefer([&]() {
        cb_std_fs__close(f);
    });
    int64_t lineNum = 0;
    int64_t matchCount = 0;
    std::string line = std::string("");
    while (true) {
        auto [_val6, _err6] = cb_std_fs__readLine(f, line);
        if (cbHasError(_err6)) {
            cbStampError(_err6, "examples/cbgrep.cb", 50);
            return { {}, _err6 };
        }
        if (!_val6) { break; }
        lineNum = (lineNum + 1);
        if (matchLine(line, pattern, ignoreCase, lineNumbers, invert, countOnly, source, lineNum)) {
            matchCount = (matchCount + 1);
        }
    }
    if (countOnly) {
        if ((path != std::string("-"))) {
            (std::cout << ((((std::string("") + path) + std::string(":")) + cbToStr(matchCount)) + std::string("")) << std::endl);
        } else {
            (std::cout << cbToStr(matchCount) << std::endl);
        }
    }
    return { matchCount, CbError{"", 0} };
}

int64_t cbMain() {
    cb_std_cli__Parser p = cb_std_cli__newParser(std::string("cbgrep"), std::string("1.0"), std::string("Search for pattern in files"));
    cb_std_cli__addFlag(p, std::string("ignore-case"), std::string("i"), std::string("Case-insensitive search"));
    cb_std_cli__addFlag(p, std::string("line-number"), std::string("n"), std::string("Show line numbers"));
    cb_std_cli__addFlag(p, std::string("count"), std::string("c"), std::string("Only show a count of matching lines"));
    cb_std_cli__addFlag(p, std::string("invert-match"), std::string("v"), std::string("Invert match: select non-matching lines"));
    cb_std_cli__addArg(p, std::string("pattern"), std::string("Search pattern"), true);
    auto [_val7, _err7] = cb_std_cli__parse(p);
    if (cbHasError(_err7)) {
        cbStampError(_err7, "examples/cbgrep.cb", 87);
        CbError e = _err7;
        cb_std_os__eprint((std::string("cbgrep: ") + e.msg));
        return 2;
    }
    const auto args = _val7;
    if (args.helpRequested) {
        cb_std_cli__printUsage(p);
        return 0;
    }
    const std::string pattern = cb_std_cli__get(args, std::string("pattern"), std::string(""));
    const bool ignoreCase = cb_std_cli__has(args, std::string("ignore-case"));
    const bool lineNumbers = cb_std_cli__has(args, std::string("line-number"));
    const bool countOnly = cb_std_cli__has(args, std::string("count"));
    const bool invert = cb_std_cli__has(args, std::string("invert-match"));
    std::vector<std::string> targets = args.rest;
    if ((static_cast<int64_t>(targets.size()) == 0)) {
        targets.push_back(std::string("-"));
    }
    int64_t totalMatches = 0;
    for (const auto& target : targets) {
        if ((target == std::string("-"))) {
            auto [_val8, _err8] = search(std::string("-"), pattern, ignoreCase, lineNumbers, invert, countOnly);
            if (cbHasError(_err8)) {
                cbStampError(_err8, "examples/cbgrep.cb", 112);
                CbError e = _err8;
                cb_std_os__eprint((((std::string("cbgrep: ") + target) + std::string(": ")) + e.msg));
                continue;
            }
            auto mStdn = _val8;
            totalMatches = (totalMatches + mStdn);
        } else if (cb_std_fs__isDir(target)) {
            auto [_val9, _err9] = cb_std_fs__walkFiles(target);
            if (cbHasError(_err9)) {
                cbStampError(_err9, "examples/cbgrep.cb", 115);
                CbError e = _err9;
                cb_std_os__eprint((((std::string("cbgrep: ") + target) + std::string(": ")) + e.msg));
                continue;
            }
            auto files = _val9;
            for (const auto& path : files) {
                if ((cb_std_str__contains(path, std::string("/.")) || cb_std_str__startsWith(cb_std_fs__fileName(path), std::string(".")))) {
                    continue;
                }
                auto [_val10, _err10] = search(path, pattern, ignoreCase, lineNumbers, invert, countOnly);
                if (cbHasError(_err10)) {
                    cbStampError(_err10, "examples/cbgrep.cb", 124);
                    CbError e2 = _err10;
                    cb_std_os__eprint((((std::string("cbgrep: ") + path) + std::string(": ")) + e2.msg));
                    continue;
                }
                auto mFile = _val10;
                totalMatches = (totalMatches + mFile);
            }
        } else {
            auto [_val11, _err11] = search(target, pattern, ignoreCase, lineNumbers, invert, countOnly);
            if (cbHasError(_err11)) {
                cbStampError(_err11, "examples/cbgrep.cb", 128);
                CbError e = _err11;
                cb_std_os__eprint((((std::string("cbgrep: ") + target) + std::string(": ")) + e.msg));
                continue;
            }
            auto mTarget = _val11;
            totalMatches = (totalMatches + mTarget);
        }
    }
    if ((totalMatches > 0)) {
        return 0;
    }
    return 1;
}

int main(int argc, char** argv) {
    auto& _cbArgs = cbRuntimeArgs();
    _cbArgs.clear();
    for (int _i = 0; _i < argc; ++_i) {
        _cbArgs.emplace_back(argv[_i]);
    }
    return static_cast<int>(cbMain());
}
