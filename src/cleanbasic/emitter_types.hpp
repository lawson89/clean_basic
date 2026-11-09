#ifndef CLEANBASIC_EMITTER_TYPES_HPP
#define CLEANBASIC_EMITTER_TYPES_HPP

#include "src/cleanbasic/analyzer.hpp"
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cb {

constexpr std::string_view CPP_RUNTIME_PREAMBLE = R"(#include <cstdint>
#include <iostream>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <tuple>
#include <utility>
#include <functional>

struct [[nodiscard]] CbError {
    std::string msg = "";
    int64_t code = 0;
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
inline bool cbHasError(const CbError& val) { return !val.msg.empty() || val.code != 0; }

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

inline bool cbRangeCond(int64_t curr, int64_t end, int64_t step) {
    if (step < 0) {
        return curr >= end;
    }
    return curr <= end;
}

inline std::vector<std::string>& cbRuntimeArgs() {
    static std::vector<std::string> args;
    return args;
}

template <typename F> struct CbDeferTask { F f; ~CbDeferTask() { f(); } };
template <typename F> CbDeferTask<F> cbDefer(F f) { return {f}; }
)";

inline std::string joinStrings(const std::vector<std::string>& items, std::string_view sep = ", ") {
    std::string out;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i > 0) {
            out += sep;
        }
        out += items[i];
    }
    return out;
}
inline std::string sanitizeCppIdent(std::string_view name) {
    static const std::unordered_set<std::string_view> cppKeywords = {
        "alignas", "alignof", "and_eq", "asm", "atomic_cancel", "atomic_commit",
        "atomic_noexcept", "auto", "bitand", "bitor", "bool", "break", "case",
        "catch", "char", "char8_t", "char16_t", "char32_t", "class", "compl",
        "concept", "const", "consteval", "constexpr", "constinit", "const_cast",
        "continue", "co_await", "co_return", "co_yield", "decltype", "default",
        "delete", "do", "double", "dynamic_cast", "else", "enum", "explicit",
        "export", "extern", "false", "float", "for", "friend", "goto", "if",
        "inline", "int", "long", "mutable", "namespace", "new", "noexcept",
        "not_eq", "nullptr", "operator", "or_eq", "private", "protected", "public",
        "reflexpr", "register", "reinterpret_cast", "requires", "return", "short",
        "signed", "sizeof", "static", "static_assert", "static_cast", "struct",
        "switch", "synchronized", "template", "this", "thread_local", "throw",
        "true", "try", "typedef", "typeid", "typename", "union", "unsigned",
        "using", "virtual", "void", "volatile", "wchar_t", "while", "xor", "xor_eq"
    };
    if (cppKeywords.count(name) > 0) {
        return std::string(name) + "_";
    }
    return std::string(name);
}

class Emitter {
public:
    const AstArena& arena;
    const GlobalSymbolTable& symbols;
    std::vector<std::string> lines;
    int indentLevel = 0;
    uint32_t tempCounter = 0;
    uint32_t valCounter = 0;
    uint32_t deferCounter = 0;
    std::optional<ErrorBlock> activeErrorBlock = std::nullopt;
    std::string currentModuleId = "";

    Emitter(const AstArena& arena, const GlobalSymbolTable& symbols)
        : arena(arena), symbols(symbols) {}

    void emitLine(std::string_view line = "") {
        if (line.empty()) {
            lines.emplace_back("");
        } else {
            std::string res;
            res.reserve(static_cast<size_t>(indentLevel * 4) + line.size());
            for (int i = 0; i < indentLevel; ++i) {
                res.append("    ");
            }
            res.append(line);
            lines.push_back(std::move(res));
        }
    }

    std::string nextTemp(std::string_view prefix = "_tmp") {
        tempCounter += 1;
        return std::string(prefix) + "_" + std::to_string(tempCounter);
    }

    std::string mapBaseType(std::string_view modId, const TypeNode& t) const {
        if (t.name == "int") return "int64_t";
        if (t.name == "float") return "double";
        if (t.name == "bool") return "bool";
        if (t.name == "str") return "std::string";
        if (t.name == "bytes") return "std::vector<uint8_t>";
        if (t.name == "err") return "CbError";
        if (t.name == "void") return "void";
        if (t.name == "auto") return "auto";
        if (t.name == "cptr") return "void*";
        if (t.name == "list") {
            std::string elem = "int64_t";
            if (!t.typeArgs.empty()) {
                elem = mapType(modId, t.typeArgs[0]);
            }
            return "std::vector<" + elem + ">";
        }
        if (t.name == "dict") {
            std::string k = "std::string";
            if (t.typeArgs.size() > 0) {
                k = mapType(modId, t.typeArgs[0]);
            }
            std::string v = "int64_t";
            if (t.typeArgs.size() > 1) {
                v = mapType(modId, t.typeArgs[1]);
            }
            return "std::unordered_map<" + k + ", " + v + ">";
        }

        const std::string key = resolveSymbolKey(symbols, modId, t.name);
        if (symbols.types.count(key)) {
            return symbols.types.at(key).cppName;
        }
        if (symbols.enums.count(key)) {
            return symbols.enums.at(key).cppName;
        }
        if (symbols.aliases.count(key)) {
            return symbols.aliases.at(key).cppName;
        }
        std::string sanitized = t.name;
        for (char& c : sanitized) {
            if (c == '.') c = '_';
        }
        return sanitized;
    }

    std::string mapTypeNode(std::string_view modId, const TypeNode& resolved, bool isByref = false) const {
        const std::string baseCpp = mapBaseType(modId, resolved);
        std::string cpp = baseCpp;
        if (isByref) {
            cpp += "&";
        }
        return cpp;
    }

    std::string mapType(std::string_view modId, TypeId tid, bool isByref = false) const {
        const TypeNode resolved = resolveAliasNode(arena, symbols, modId, tid);
        return mapTypeNode(modId, resolved, isByref);
    }

    std::string mapType(TypeId tid, bool isByref = false) const {
        return mapType(currentModuleId, tid, isByref);
    }

    std::string defaultValueForType(std::string_view modId, TypeId tid) const {
        const TypeNode resolved = resolveAliasNode(arena, symbols, modId, tid);
        if (resolved.name == "int") return "0";
        if (resolved.name == "float") return "0.0";
        if (resolved.name == "bool") return "false";
        if (resolved.name == "str") return "std::string("")";
        if (resolved.name == "bytes") return "std::vector<uint8_t>{}";
        if (isEnumType(symbols, tid, arena, modId)) {
            return "static_cast<" + mapTypeNode(modId, resolved) + ">(0)";
        }
        return mapTypeNode(modId, resolved) + "{}";
    }

    std::string defaultValueForType(TypeId tid) const {
        return defaultValueForType(currentModuleId, tid);
    }

    std::string fnReturnCppType(std::string_view modId, const FnDecl& fn) const {
        std::vector<std::string> retCpp;
        if (fn.returnType != INVALID_ID && !arena.isVoid(fn.returnType)) {
            retCpp.push_back(mapType(modId, fn.returnType));
        }
        if (fn.isFallible) {
            retCpp.push_back("CbError");
        }
        if (retCpp.empty()) {
            return "void";
        }
        if (retCpp.size() == 1) {
            return retCpp[0];
        }
        return "std::tuple<" + joinStrings(retCpp) + ">";
    }

    bool isPassByConstRefType(std::string_view modId, const TypeNode& resolved) const {
        const TypeNode actual = resolveAliasFromNode(arena, symbols, modId, resolved);
        if (actual.name == "int" || actual.name == "float" ||
            actual.name == "bool" || actual.name == "cptr" ||
            actual.name == "void" || actual.name == "auto") {
            return false;
        }
        const std::string k = resolveSymbolKey(symbols, modId, actual.name);
        if (symbols.enums.count(actual.name) > 0 || symbols.enums.count(k) > 0) {
            return false;
        }
        return true;
    }

    bool isPassByConstRefType(std::string_view modId, TypeId tid) const {
        if (tid >= arena.types.size()) return false;
        const TypeNode resolved = resolveAliasNode(arena, symbols, modId, tid);
        return isPassByConstRefType(modId, resolved);
    }

    std::string mapParamType(std::string_view modId, const Param& p) const {
        const TypeNode resolved = resolveAliasNode(arena, symbols, modId, p.typeId);
        const std::string baseCpp = mapBaseType(modId, resolved);
        if (p.isByref) {
            return baseCpp + "&";
        }
        if (isPassByConstRefType(modId, resolved)) {
            return "const " + baseCpp + "&";
        }
        return baseCpp;
    }

    std::string fnSignature(std::string_view modId, const FnDecl& fn) const {
        std::string retStr = fnReturnCppType(modId, fn);
        if (fn.isFallible && !fn.isExtern) {
            retStr = "[[nodiscard]] " + retStr;
        }
        std::vector<std::string> paramItems;
        paramItems.reserve(fn.params.size());
        for (const auto& p : fn.params) {
            paramItems.push_back(mapParamType(modId, p) + " " + p.name);
        }
        const std::string key = resolveSymbolKey(symbols, modId, fn.name);
        std::string cppName = makeCppFnName(modId, fn.name, fn.isExtern);
        if (symbols.functions.count(key) > 0) {
            cppName = symbols.functions.at(key).cppName;
        }
        return retStr + " " + cppName + "(" + joinStrings(paramItems) + ")";
    }

    // Forward declarations of emitter methods
    std::string emitExpr(ExprId exprId, const std::unordered_map<std::string, VarSymbol>& scope);
    std::string emitAsStrExpr(ExprId exprId, const std::unordered_map<std::string, VarSymbol>& scope);
    std::vector<std::string> emitExprList(const std::vector<ExprId>& exprIds, const std::unordered_map<std::string, VarSymbol>& scope);
    std::vector<std::string> orderStructFields(std::string_view typeName, const std::vector<ExprId>& posArgs, const std::vector<NamedArg>& namedArgs, const std::unordered_map<std::string, VarSymbol>& scope);
    std::string emitCallArgsJoined(const FnSymbol& fnSym, const std::vector<ExprId>& posArgs, const std::vector<NamedArg>& namedArgs, const std::unordered_map<std::string, VarSymbol>& scope);
    std::string buildErrorCallValue(const FuncCall& call, const std::unordered_map<std::string, VarSymbol>& scope);
    std::string buildErrorBubbleReturn(const FnDecl& fn, std::string_view errVar);
    void emitStmt(const FnDecl& fn, StmtId stmtId, std::unordered_map<std::string, VarSymbol>& scope, std::unordered_set<std::string>& declaredVars);
    void emitStmtList(const FnDecl& fn, const std::vector<StmtId>& stmts, std::unordered_map<std::string, VarSymbol>& scope, std::unordered_set<std::string>& declaredVars);
    void emitActiveErrorHandlerOrBubble(const FnDecl& fn, const std::string& errTmp, std::unordered_map<std::string, VarSymbol>& scope, std::unordered_set<std::string>& declaredVars);
    void emitFallibleAssignment(const FnDecl& fn, const VarAssignment& stmt, const FnSymbol& fnSym, std::unordered_map<std::string, VarSymbol>& scope, std::unordered_set<std::string>& declaredVars);
    void emitVarAssignment(const FnDecl& fn, const VarAssignment& stmt, std::unordered_map<std::string, VarSymbol>& scope, std::unordered_set<std::string>& declaredVars);
    void emitExprStmt(const FnDecl& fn, const ExprStmt& stmt, std::unordered_map<std::string, VarSymbol>& scope, std::unordered_set<std::string>& declaredVars);
    void emitReturn(const FnDecl& fn, const ReturnStmt& stmt, std::unordered_map<std::string, VarSymbol>& scope, std::unordered_set<std::string>& declaredVars);
    void emitStructDecl(const TypeDecl& td);
    void emitEnumDecl(const EnumDecl& ed);
    void emitConstDecl(const ConstDecl& cd);
    void emitFnDecl(const FnDecl& fn);
    std::string emitProgram(const Program& program);
};

using EmitterState = Emitter;

} // namespace cb

#endif // CLEANBASIC_EMITTER_TYPES_HPP
