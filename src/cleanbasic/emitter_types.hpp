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

struct CbError {
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
        if (t.name == "int") return "int";
        if (t.name == "float") return "double";
        if (t.name == "bool") return "bool";
        if (t.name == "str") return "std::string";
        if (t.name == "err") return "CbError";
        if (t.name == "void") return "void";
        if (t.name == "auto") return "auto";
        if (t.name == "cptr") return "void*";
        if (t.name == "list") {
            std::string elem = "int";
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
            std::string v = "int";
            if (t.typeArgs.size() > 1) {
                v = mapType(modId, t.typeArgs[1]);
            }
            return "std::unordered_map<" + k + ", " + v + ">";
        }

        const std::string key = resolveSymbolKey(symbols, modId, t.name);
        if (symbols.types.count(key)) {
            return symbols.types.at(key).cppName;
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

    std::string fnSignature(std::string_view modId, const FnDecl& fn) const {
        const std::string retStr = fnReturnCppType(modId, fn);
        std::vector<std::string> paramItems;
        paramItems.reserve(fn.params.size());
        for (const auto& p : fn.params) {
            paramItems.push_back(mapType(modId, p.typeId, p.isByref) + " " + p.name);
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
    std::string emitCallArgsJoined(const std::vector<ExprId>& posArgs, const std::vector<NamedArg>& namedArgs, const std::unordered_map<std::string, VarSymbol>& scope);
    std::string buildErrorCallValue(const FuncCall& call, const std::unordered_map<std::string, VarSymbol>& scope);
    std::string buildErrorBubbleReturn(const FnDecl& fn, std::string_view errVar);
    void emitStmt(const FnDecl& fn, StmtId stmtId, std::unordered_map<std::string, VarSymbol>& scope, std::unordered_set<std::string>& declaredVars);
    void emitStmtList(const FnDecl& fn, const std::vector<StmtId>& stmts, std::unordered_map<std::string, VarSymbol>& scope, std::unordered_set<std::string>& declaredVars);
    void emitActiveErrorHandlerOrBubble(const FnDecl& fn, const std::string& errTmp, std::unordered_map<std::string, VarSymbol>& scope, std::unordered_set<std::string>& declaredVars);
    void emitFallibleAssignment(const FnDecl& fn, const VarAssignment& stmt, const FnSymbol& fnSym, std::unordered_map<std::string, VarSymbol>& scope, std::unordered_set<std::string>& declaredVars);
    void emitVarAssignment(const FnDecl& fn, const VarAssignment& stmt, std::unordered_map<std::string, VarSymbol>& scope, std::unordered_set<std::string>& declaredVars);
    void emitExprStmt(const FnDecl& fn, const ExprStmt& stmt, std::unordered_map<std::string, VarSymbol>& scope, std::unordered_set<std::string>& declaredVars);
    void emitReturn(const FnDecl& fn, const ReturnStmt& stmt, const std::unordered_map<std::string, VarSymbol>& scope);
    void emitStructDecl(const TypeDecl& td);
    void emitFnDecl(const FnDecl& fn);
    std::string emitProgram(const Program& program);
};

using EmitterState = Emitter;

inline std::string mapType(
    const AstArena& arena,
    const GlobalSymbolTable& symbols,
    std::string_view currentModuleId,
    TypeId tid,
    bool isByref = false
) {
    Emitter emitter(arena, symbols);
    return emitter.mapType(currentModuleId, tid, isByref);
}

inline std::string mapBaseType(
    const AstArena& arena,
    const GlobalSymbolTable& symbols,
    std::string_view currentModuleId,
    const TypeNode& t
) {
    Emitter emitter(arena, symbols);
    return emitter.mapBaseType(currentModuleId, t);
}

inline std::string mapTypeNode(
    const AstArena& arena,
    const GlobalSymbolTable& symbols,
    std::string_view currentModuleId,
    const TypeNode& resolved,
    bool isByref = false
) {
    Emitter emitter(arena, symbols);
    return emitter.mapTypeNode(currentModuleId, resolved, isByref);
}

inline std::string defaultValueForType(
    const AstArena& arena,
    const GlobalSymbolTable& symbols,
    std::string_view currentModuleId,
    TypeId tid
) {
    Emitter emitter(arena, symbols);
    return emitter.defaultValueForType(currentModuleId, tid);
}

inline std::string fnReturnCppType(
    const AstArena& arena,
    const GlobalSymbolTable& symbols,
    std::string_view currentModuleId,
    const FnDecl& fn
) {
    Emitter emitter(arena, symbols);
    return emitter.fnReturnCppType(currentModuleId, fn);
}

inline std::string fnSignature(
    const AstArena& arena,
    const GlobalSymbolTable& symbols,
    std::string_view currentModuleId,
    const FnDecl& fn
) {
    Emitter emitter(arena, symbols);
    return emitter.fnSignature(currentModuleId, fn);
}

} // namespace cb

#endif // CLEANBASIC_EMITTER_TYPES_HPP
