#ifndef CLEANBASIC_EMITTER_HPP
#define CLEANBASIC_EMITTER_HPP

#include "src/cleanbasic/analyzer.hpp"
#include "src/cleanbasic/runtime_preamble.hpp"
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cb {

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
    static const std::unordered_set<std::string_view> cMacroNames = {
        "errno", "stdin", "stdout", "stderr", "EOF", "NULL", "unix", "linux"
    };
    if (cMacroNames.count(name) > 0) {
        return std::string(name) + "_cb";
    }
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

/**
 * CleanBasic Unity C++17 Emitter
 * 
 * Generates a single self-contained C++17 file (build.cpp) in 7 ordered stages:
 * 1. Linker Flags Header (// CleanBasic Linker Flags: ...)
 * 2. C++17 Runtime Preamble (CbError, cbToStr, cbDefer, etc.)
 * 3. External C/C++ Headers (import header "...")
 * 4. Forward Declarations (Structs, Enums, Union shells)
 * 5. Early Constants (POD primitives and enums)
 * 6. POD Struct/Union Definitions & Function Forward Declarations
 * 7. Function Definitions & Entry Point Wrapper (int main() -> cbMain())
 * 
 * Constants are partitioned to ensure they are defined before use in struct default initializers.
 * 'asLvalue' controls whether expressions (like list/dict indexing) emit bounds-safe read-only 
 * helper calls (cbListGet) vs. mutable C++ reference indexing (v[i]) for assignments.
 */
class Emitter {
    std::string escapeFileName(std::string_view path) const {
        std::string out = "\"";
        for (char c : path) {
            if (c == '\\') { out += '\\'; out += '\\'; }
            else if (c == '"') { out += '\\'; out += '"'; }
            else out += c;
        }
        out += "\"";
        return out;
    }

public:
    const AstArena& arena;
    const GlobalSymbolTable& symbols;
    std::vector<std::string> lines;
    int indentLevel = 0;
    uint32_t tempCounter = 0;
    uint32_t deferCounter = 0;
    uint32_t deferDepth = 0;
    std::vector<std::string> catchErrStack;
    std::string currentModuleId = "";
    std::string currentFileName = "main.cb";

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

    std::pair<std::string, std::string> nextValErrTemps() {
        tempCounter += 1;
        const std::string suffix = std::to_string(tempCounter);
        return {"_val" + suffix, "_err" + suffix};
    }

    template <typename Map>
    std::string lookupSymbolCppName(const Map& table, std::string_view modId, std::string_view name) const {
        const std::string key = resolveSymbolKey(symbols, modId, name);
        const auto it = table.find(key);
        if (it != table.end()) {
            return it->second.cppName;
        }
        return std::string(name);
    }

    std::string lookupTypeCppName(std::string_view modId, std::string_view name) const {
        return lookupSymbolCppName(symbols.types, modId, name);
    }

    std::string lookupEnumCppName(std::string_view modId, std::string_view name) const {
        return lookupSymbolCppName(symbols.enums, modId, name);
    }

    std::string lookupUnionCppName(std::string_view modId, std::string_view name) const {
        return lookupSymbolCppName(symbols.unions, modId, name);
    }

    std::string unionDataVariantCppType(const UnionSymbol& usym) const {
        std::vector<std::string> varCppTypes;
        varCppTypes.reserve(usym.variants.size());
        for (const auto& uv : usym.variants) {
            varCppTypes.push_back(mapType(usym.moduleId, uv.type));
        }
        if (varCppTypes.empty()) {
            varCppTypes.push_back("int64_t");
        }
        return "std::variant<" + joinStrings(varCppTypes) + ">";
    }

    std::string lookupAliasCppName(std::string_view modId, std::string_view name) const {
        return lookupSymbolCppName(symbols.aliases, modId, name);
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
        if (t.name == "fn") {
            std::string ret = "void";
            if (t.returnType != INVALID_ID) {
                ret = mapType(modId, t.returnType);
            }
            if (t.isFallible) {
                if (ret == "void") {
                    ret = "CbError";
                } else {
                    ret = "std::tuple<" + ret + ", CbError>";
                }
            }
            std::string params;
            for (size_t i = 0; i < t.typeArgs.size(); ++i) {
                if (i > 0) params += ", ";
                bool isByref = false;
                if (i < t.paramByref.size()) {
                    isByref = t.paramByref[i];
                }
                Param p{1, 1, "", t.typeArgs[i], isByref, false, INVALID_ID};
                params += mapParamType(modId, p);
            }
            return "std::function<" + ret + "(" + params + ")>";
        }
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
        if (symbols.unions.count(key)) {
            return symbols.unions.at(key).cppName;
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
        std::string cpp = mapBaseType(modId, resolved);
        if (isByref) {
            cpp += "&";
        }
        return cpp;
    }

    std::string mapType(std::string_view modId, TypeId tid, bool isByref = false) const {
        const TypeNode resolved = resolveAliasNode(arena, symbols, modId, tid);
        return mapTypeNode(modId, resolved, isByref);
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
        if (symbols.enums.count(actual.name) > 0) {
            return false;
        }
        return true;
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
            paramItems.push_back(mapParamType(modId, p) + " " + sanitizeCppIdent(p.name));
        }
        const std::string key = resolveSymbolKey(symbols, modId, fn.name);
        std::string cppName = makeCppFnName(modId, fn.name, fn.isExtern);
        const auto it = symbols.functions.find(key);
        if (it != symbols.functions.end()) {
            cppName = it->second.cppName;
        }
        return retStr + " " + cppName + "(" + joinStrings(paramItems) + ")";
    }

    std::string emitAsStrExpr(
        ExprId exprId,
        const std::unordered_map<std::string, VarSymbol>& scope
    ) {
    const TypeId tid = inferExprType(arena, exprId, symbols, scope, currentModuleId);
    std::string cpp = emitExpr(exprId, scope);
    if (tid < arena.types.size() && arena.types[tid].name == "str") {
        return cpp;
    }
    return "cbToStr(" + cpp + ")";
}

    std::vector<std::string> emitExprList(
    const std::vector<ExprId>& exprIds,
    const std::unordered_map<std::string, VarSymbol>& scope
) {
    std::vector<std::string> rendered;
    rendered.reserve(exprIds.size());
    for (const ExprId eid : exprIds) {
        rendered.push_back(emitExpr(eid, scope));
    }
    return rendered;
}

    std::vector<std::string> orderStructFields(
    std::string_view typeName,
    const std::vector<ExprId>& posArgs,
    const std::vector<NamedArg>& namedArgs,
    const std::unordered_map<std::string, VarSymbol>& scope
) {
    const std::string typeKey = resolveSymbolKey(symbols, currentModuleId, typeName);
    const auto it = symbols.types.find(typeKey);
    if (it == symbols.types.end() || it->second.fieldOrder.empty()) {
        return emitExprList(posArgs, scope);
    }
    if (namedArgs.empty() && posArgs.size() == it->second.fieldOrder.size()) {
        return emitExprList(posArgs, scope);
    }
    if (namedArgs.empty() && posArgs.empty()) {
        bool anyDefault = false;
        for (const auto& fdecl : it->second.fieldOrder) {
            if (fdecl.hasDefault) {
                anyDefault = true;
                break;
            }
        }
        if (!anyDefault) {
            return {};
        }
    }

    std::unordered_map<std::string, ExprId> namedMap;
    for (const auto& na : namedArgs) {
        namedMap[na.name] = na.value;
    }

    std::vector<std::string> ordered;
    ordered.reserve(it->second.fieldOrder.size());
    for (size_t i = 0; i < it->second.fieldOrder.size(); ++i) {
        const auto& fdecl = it->second.fieldOrder[i];
        if (i < posArgs.size()) {
            ordered.push_back(emitExpr(posArgs[i], scope));
        } else {
            const auto fit = namedMap.find(fdecl.name);
            if (fit != namedMap.end()) {
                ordered.push_back(emitExpr(fit->second, scope));
            } else if (fdecl.hasDefault && fdecl.defaultValue != INVALID_ID) {
                const std::string savedMod = currentModuleId;
                currentModuleId = it->second.moduleId;
                std::unordered_map<std::string, VarSymbol> emptyScope;
                ordered.push_back(emitExpr(fdecl.defaultValue, emptyScope));
                currentModuleId = savedMod;
            } else {
                ordered.push_back(defaultValueForType(it->second.moduleId, fdecl.typeId));
            }
        }
    }
    return ordered;
}

    std::string emitCallArgsJoined(
    const FnSymbol& fnSym,
    const std::vector<ExprId>& posArgs,
    const std::vector<NamedArg>& namedArgs,
    const std::unordered_map<std::string, VarSymbol>& scope
) {
    if (fnSym.params.empty() || (namedArgs.empty() && posArgs.size() >= fnSym.params.size())) {
        std::vector<std::string> rendered;
        rendered.reserve(posArgs.size() + namedArgs.size());
        for (size_t i = 0; i < posArgs.size(); ++i) {
            bool isByrefParam = false;
            if (i < fnSym.params.size() && fnSym.params[i].isByref) {
                isByrefParam = true;
            }
            rendered.push_back(emitExpr(posArgs[i], scope, isByrefParam));
        }
        for (const auto& na : namedArgs) {
            rendered.push_back(emitExpr(na.value, scope));
        }
        return joinStrings(rendered);
    }

    std::unordered_map<std::string, ExprId> namedMap;
    for (const auto& na : namedArgs) {
        namedMap[na.name] = na.value;
    }

    std::vector<std::string> rendered;
    rendered.reserve(fnSym.params.size());
    for (size_t i = 0; i < fnSym.params.size(); ++i) {
        const auto& p = fnSym.params[i];
        if (i < posArgs.size()) {
            rendered.push_back(emitExpr(posArgs[i], scope, p.isByref));
        } else {
            const auto nit = namedMap.find(p.name);
            if (nit != namedMap.end()) {
                rendered.push_back(emitExpr(nit->second, scope, p.isByref));
            } else if (p.hasDefault && p.defaultValue != INVALID_ID) {
                const std::string savedMod = currentModuleId;
                currentModuleId = fnSym.moduleId;
                std::unordered_map<std::string, VarSymbol> emptyScope;
                rendered.push_back(emitExpr(p.defaultValue, emptyScope));
                currentModuleId = savedMod;
            }
        }
    }
    return joinStrings(rendered);
}

    std::string emitExpr(
        ExprId exprId,
        const std::unordered_map<std::string, VarSymbol>& scope,
        bool asLvalue = false
    ) {
    if (exprId >= arena.exprs.size()) {
        return "";
    }

    if (const auto* num = arena.getExpr<NumberLiteral>(exprId)) {
        std::string cleaned;
        cleaned.reserve(num->raw.size());
        for (char c : num->raw) {
            if (c != '_') {
                cleaned.push_back(c);
            }
        }
        if (!num->isFloat && cleaned.size() >= 2 && cleaned[0] == '0' &&
            (cleaned[1] == 'x' || cleaned[1] == 'X' || cleaned[1] == 'b' || cleaned[1] == 'B')) {
            return "static_cast<int64_t>(" + cleaned + "ULL)";
        }
        return cleaned;
    }

    if (const auto* str = arena.getExpr<StringLiteral>(exprId)) {
        const std::string q(1, '"');
        return "std::string(" + q + str->value + q + ")";
    }

    if (const auto* b = arena.getExpr<BytesLiteral>(exprId)) {
        std::string res = "std::vector<uint8_t>{";
        for (size_t i = 0; i < b->value.size(); ++i) {
            res += std::to_string(static_cast<int>(b->value[i]));
            if (i + 1 < b->value.size()) {
                res += ", ";
            }
        }
        res += "}";
        return res;
    }

    if (const auto* b = arena.getExpr<BoolLiteral>(exprId)) {
        if (b->value) {
            return "true";
        }
        return "false";
    }

    if (arena.getExpr<NullLiteral>(exprId) != nullptr) {
        return "nullptr";
    }

    if (const auto* ident = arena.getExpr<Identifier>(exprId)) {
        const auto cOpt = resolveConstSymbol(symbols, currentModuleId, scope, ident->name);
        if (cOpt.has_value()) {
            return cOpt->cppName;
        }
        const auto bareFnOpt = resolveBareFnRef(arena, exprId, symbols, scope, currentModuleId);
        if (bareFnOpt.has_value()) {
            return bareFnOpt->cppName;
        }
        return sanitizeCppIdent(ident->name);
    }

    if (const auto* uop = arena.getExpr<UnaryOp>(exprId)) {
        std::string opStr = uop->op;
        if (uop->op == "not") {
            opStr = "!";
        }
        return "(" + opStr + emitExpr(uop->operand, scope) + ")";
    }

    if (const auto* bop = arena.getExpr<BinaryOp>(exprId)) {
        if (bop->op == "&") {
            const std::string leftCpp = emitAsStrExpr(bop->left, scope);
            const std::string rightCpp = emitAsStrExpr(bop->right, scope);
            return "(" + leftCpp + " + " + rightCpp + ")";
        }
        std::string opStr = bop->op;
        if (opStr == "and") {
            opStr = "&&";
        } else if (opStr == "or") {
            opStr = "||";
        }
        const std::string leftCpp = emitExpr(bop->left, scope);
        const std::string rightCpp = emitExpr(bop->right, scope);
        return "(" + leftCpp + " " + opStr + " " + rightCpp + ")";
    }

    if (const auto* faccess = arena.getExpr<FieldAccess>(exprId)) {
        const auto enumOpt = resolveEnumMemberAccess(arena, symbols, currentModuleId, scope, *faccess);
        if (enumOpt.has_value() && enumOpt->memberValues.count(faccess->fieldName) > 0) {
            return enumOpt->cppName + "::" + sanitizeCppIdent(faccess->fieldName);
        }
        const auto cOpt = resolveModuleConstAccess(arena, symbols, currentModuleId, scope, *faccess);
        if (cOpt.has_value()) {
            return cOpt->cppName;
        }
        const auto bareFnOpt = resolveBareFnRef(arena, exprId, symbols, scope, currentModuleId);
        if (bareFnOpt.has_value()) {
            return bareFnOpt->cppName;
        }
        const TypeId tgtTid = inferExprType(arena, faccess->target, symbols, scope, currentModuleId);
        const auto tsOpt = resolveTypeSymbol(arena, symbols, currentModuleId, tgtTid);
        std::string emittedFieldName = faccess->fieldName;
        if (!tsOpt.has_value() || !tsOpt->isExtern) {
            emittedFieldName = sanitizeCppIdent(faccess->fieldName);
        }
        return emitExpr(faccess->target, scope, asLvalue) + "." + emittedFieldName;
    }

    if (const auto* iaccess = arena.getExpr<IndexAccess>(exprId)) {
        const TypeId tgtTid = inferExprType(arena, iaccess->target, symbols, scope, currentModuleId);
        const TypeNode resolvedTgt = resolveAliasNode(arena, symbols, currentModuleId, tgtTid);
        const std::string tgtCpp = emitExpr(iaccess->target, scope, asLvalue);
        const std::string idxCpp = emitExpr(iaccess->index, scope, false);
        if (asLvalue) {
            return tgtCpp + "[" + idxCpp + "]";
        }
        if (resolvedTgt.name == "str") {
            return "cbStrIndex(" + tgtCpp + ", " + idxCpp + ")";
        }
        if (resolvedTgt.name == "list") {
            return "cbListGet(" + tgtCpp + ", " + idxCpp + ")";
        }
        if (resolvedTgt.name == "bytes") {
            return "cbBytesGet(" + tgtCpp + ", " + idxCpp + ")";
        }
        if (resolvedTgt.name == "dict") {
            return "cbDictAt(" + tgtCpp + ", " + idxCpp + ")";
        }
        return tgtCpp + "[" + idxCpp + "]";
    }

    if (const auto* llit = arena.getExpr<ListLiteral>(exprId)) {
        return "{" + joinStrings(emitExprList(llit->elements, scope)) + "}";
    }

    if (const auto* dlit = arena.getExpr<DictLiteral>(exprId)) {
        std::vector<std::string> entries;
        entries.reserve(dlit->entries.size());
        for (const auto& ent : dlit->entries) {
            const std::string k = emitExpr(ent.key, scope);
            const std::string v = emitExpr(ent.value, scope);
            entries.push_back("{" + k + ", " + v + "}");
        }
        return "{" + joinStrings(entries) + "}";
    }

    if (const auto* call = arena.getExpr<FuncCall>(exprId)) {
        if (cb::isAssertBuiltinCall(arena, *call)) {
            return buildAssertCallValue(*call, scope);
        }
        if (cb::isErrorBuiltinCall(arena, *call)) {
            return buildErrorCallValue(*call, scope);
        }
        if (call->callee >= arena.exprs.size()) {
            return "";
        }
        const auto structOpt = resolveStructConstructor(arena, *call, symbols, currentModuleId, scope);
        if (structOpt.has_value()) {
            const auto ordered = orderStructFields(structOpt->key, call->args, call->namedArgs, scope);
            return structOpt->cppName + "{" + joinStrings(ordered) + "}";
        }

        const auto unionVarOpt = resolveUnionVariantConstructor(arena, *call, symbols, currentModuleId, scope);
        if (unionVarOpt.has_value() && !call->args.empty()) {
            const std::string argCpp = emitExpr(call->args[0], scope);
            const std::string varDataCpp = unionDataVariantCppType(unionVarOpt->unionSym);
            return unionVarOpt->unionSym.cppName + "{" + varDataCpp + "(std::in_place_index<" + std::to_string(unionVarOpt->variantIndex) + ">, " + argCpp + ")}";
        }

        const auto fnSym = resolveFuncCall(arena, *call, symbols, currentModuleId, scope);
        if (fnSym.has_value()) {
            if (fnSym->isBuiltinReceiver) {
                const auto* access = arena.getExpr<FieldAccess>(call->callee);
                const bool recvMutated = (!fnSym->params.empty() && fnSym->params[0].isByref);
                const std::string recvCpp = emitExpr(access->target, scope, recvMutated);
                if (fnSym->name == "len") {
                    return "static_cast<int64_t>(" + recvCpp + ".size())";
                } else if (fnSym->name == "push") {
                    const TypeId recvTid = inferExprType(arena, access->target, symbols, scope, currentModuleId);
                    const TypeNode recvNode = resolveAliasNode(arena, symbols, currentModuleId, recvTid);
                    if (recvNode.name == "bytes") {
                        return recvCpp + ".push_back(static_cast<uint8_t>(" + emitExpr(call->args[0], scope) + "))";
                    }
                    return recvCpp + ".push_back(" + emitExpr(call->args[0], scope) + ")";
                } else if (fnSym->name == "pop") {
                    const TypeId recvTid = inferExprType(arena, access->target, symbols, scope, currentModuleId);
                    const TypeNode recvNode = resolveAliasNode(arena, symbols, currentModuleId, recvTid);
                    if (recvNode.name == "bytes") {
                        return "cbBytesPop(" + recvCpp + ")";
                    }
                    return "cbListPop(" + recvCpp + ")";
                } else if (fnSym->name == "clear") {
                    return recvCpp + ".clear()";
                } else if (fnSym->name == "get") {
                    return "cbDictGet(" + recvCpp + ", " + emitExpr(call->args[0], scope) + ", " + emitExpr(call->args[1], scope) + ")";
                } else if (fnSym->name == "has") {
                    return "(" + recvCpp + ".count(" + emitExpr(call->args[0], scope) + ") > 0)";
                } else if (fnSym->name == "remove") {
                    return "(" + recvCpp + ".erase(" + emitExpr(call->args[0], scope) + ") > 0)";
                } else if (fnSym->name == "toStr") {
                    return "cbToStr(" + recvCpp + ")";
                } else if (fnSym->name == "toInt") {
                    return "static_cast<int64_t>(" + recvCpp + ")";
                } else if (fnSym->name == "toFloat") {
                    return "static_cast<double>(" + recvCpp + ")";
                } else if (fnSym->name == "slice") {
                    const TypeId recvTid = inferExprType(arena, access->target, symbols, scope, currentModuleId);
                    const TypeNode recvNode = resolveAliasNode(arena, symbols, currentModuleId, recvTid);
                    if (recvNode.name == "list") {
                        return "cbListSlice(" + recvCpp + ", " + emitExpr(call->args[0], scope) + ", " + emitExpr(call->args[1], scope) + ")";
                    }
                    return "cbBytesSlice(" + recvCpp + ", " + emitExpr(call->args[0], scope) + ", " + emitExpr(call->args[1], scope) + ")";
                } else if (fnSym->name == "sliceFrom") {
                    const TypeId recvTid = inferExprType(arena, access->target, symbols, scope, currentModuleId);
                    const TypeNode recvNode = resolveAliasNode(arena, symbols, currentModuleId, recvTid);
                    if (recvNode.name == "list") {
                        return "cbListSliceFrom(" + recvCpp + ", " + emitExpr(call->args[0], scope) + ")";
                    }
                    return "cbBytesSliceFrom(" + recvCpp + ", " + emitExpr(call->args[0], scope) + ")";
                } else if (fnSym->name == "contains") {
                    return "cbListContains(" + recvCpp + ", " + emitExpr(call->args[0], scope) + ")";
                } else if (fnSym->name == "sort") {
                    return "cbListSort(" + recvCpp + ")";
                } else if (fnSym->name == "removeAt") {
                    return "cbListRemoveAt(" + recvCpp + ", " + emitExpr(call->args[0], scope) + ")";
                } else if (fnSym->name == "keys") {
                    return "cbDictKeys(" + recvCpp + ")";
                } else if (fnSym->name == "bitAnd") {
                    return "((" + recvCpp + ") & (" + emitExpr(call->args[0], scope) + "))";
                } else if (fnSym->name == "bitOr") {
                    return "((" + recvCpp + ") | (" + emitExpr(call->args[0], scope) + "))";
                } else if (fnSym->name == "bitXor") {
                    return "((" + recvCpp + ") ^ (" + emitExpr(call->args[0], scope) + "))";
                } else if (fnSym->name == "bitNot") {
                    return "(~(" + recvCpp + "))";
                } else if (fnSym->name == "shl") {
                    return "cbBitShl(" + recvCpp + ", " + emitExpr(call->args[0], scope) + ")";
                } else if (fnSym->name == "shr") {
                    return "cbBitShr(" + recvCpp + ", " + emitExpr(call->args[0], scope) + ")";
                } else if (fnSym->name == "kind") {
                    return "cbUnionKind(" + recvCpp + ")";
                } else if (fnSym->name == "toUtf8") {
                    return "cbBytesToUtf8(" + recvCpp + ")";
                } else if (fnSym->name == "toBytes") {
                    return "cbStrToBytes(" + recvCpp + ")";
                } else if (fnSym->name == "byteAt") {
                    return "cbStrByteAtFast(" + recvCpp + ", " + emitExpr(call->args[0], scope) + ")";
                } else if (fnSym->name == "toHex") {
                    return "cbBytesToHex(" + recvCpp + ")";
                } else if (fnSym->name == "fromHex") {
                    if (call->args.empty()) {
                        return "cbBytesFromHex(" + recvCpp + ")";
                    }
                    return "cbBytesFromHex(" + emitExpr(call->args[0], scope) + ")";
                } else if (fnSym->name.rfind("readU", 0) == 0) {
                    return "cbBytesR" + fnSym->name.substr(1) + "(" + recvCpp + ", " + emitExpr(call->args[0], scope) + ")";
                } else if (fnSym->name.rfind("writeU", 0) == 0) {
                    return "cbBytesW" + fnSym->name.substr(1) + "(" + recvCpp + ", " + emitExpr(call->args[0], scope) + ", " + emitExpr(call->args[1], scope) + ")";
                }
            }

            if (fnSym->name == "print" && fnSym->moduleId == "") {
                if (call->args.empty()) {
                    return "(std::cout << std::endl)";
                }
                std::string streamExpr = "(std::cout";
                for (const ExprId argId : call->args) {
                    const TypeId argTid = inferExprType(arena, argId, symbols, scope, currentModuleId);
                    std::string argCpp = emitExpr(argId, scope);
                    if (argTid < arena.types.size() &&
                        (arena.types[argTid].name == "bool" || arena.types[argTid].name == "err")) {
                        argCpp = "cbToStr(" + argCpp + ")";
                    }
                    streamExpr += " << " + argCpp;
                }
                streamExpr += " << std::endl)";
                return streamExpr;
            }

            // Normal call or UFCS
            ExprId ufcsRecv = INVALID_ID;
            if (isUfcsCallExpr(arena, *call, symbols, currentModuleId, scope, &ufcsRecv)) {
                std::vector<ExprId> allPos;
                allPos.reserve(1 + call->args.size());
                allPos.push_back(ufcsRecv);
                for (const ExprId a : call->args) {
                    allPos.push_back(a);
                }
                if (fnSym->isFnValueCall) {
                return emitExpr(call->callee, scope) + "(" + emitCallArgsJoined(*fnSym, allPos, call->namedArgs, scope) + ")";
            }
            return fnSym->cppName + "(" + emitCallArgsJoined(*fnSym, allPos, call->namedArgs, scope) + ")";
            }

            if (fnSym->isFnValueCall) {
                return emitExpr(call->callee, scope) + "(" + emitCallArgsJoined(*fnSym, call->args, call->namedArgs, scope) + ")";
            }
            return fnSym->cppName + "(" + emitCallArgsJoined(*fnSym, call->args, call->namedArgs, scope) + ")";
        }

        const std::string calleeCpp = emitExpr(call->callee, scope);
        return calleeCpp + "(" + joinStrings(emitExprList(call->args, scope)) + ")";
    }

    return "";
}

    std::string buildErrorCallValue(
        const FuncCall& call,
        const std::unordered_map<std::string, VarSymbol>& scope
    ) {
        std::string msgCpp = R"(std::string(""))";
        if (call.args.size() == 1) {
            msgCpp = emitAsStrExpr(call.args[0], scope);
        } else if (call.args.size() > 1) {
            std::vector<std::string> parts;
            parts.reserve(call.args.size());
            for (const ExprId argId : call.args) {
                parts.push_back(emitAsStrExpr(argId, scope));
            }
            msgCpp = "(" + joinStrings(parts, " + ") + ")";
        }

        std::string codeCpp = "1";
        for (const auto& na : call.namedArgs) {
            if (na.name == "code") {
                codeCpp = emitExpr(na.value, scope);
            }
        }
        const auto& calleeNode = arena.exprs[call.callee];
        return "CbError{" + msgCpp + ", " + codeCpp + ", std::string(" + escapeFileName(currentFileName) + "), " + std::to_string(calleeNode.line) + "}";
    }

    std::string buildAssertCallValue(
        const FuncCall& call,
        const std::unordered_map<std::string, VarSymbol>& scope
    ) {
        const auto& calleeNode = arena.exprs[call.callee];
        const std::string lineStr = std::to_string(calleeNode.line);
        const std::string q(1, '"');
        if (call.args.size() == 1) {
            if (const auto* bop = arena.getExpr<BinaryOp>(call.args[0])) {
                if (bop->op == "==" || bop->op == "!=" || bop->op == "<" || bop->op == ">" || bop->op == "<=" || bop->op == ">=") {
                    const std::string leftCpp = emitExpr(bop->left, scope);
                    const std::string rightCpp = emitExpr(bop->right, scope);
                    const std::string opLabel = "left " + bop->op + " right";
                    return "[&]() -> CbError { const auto _lhs = " + leftCpp + "; const auto _rhs = " + rightCpp +
                        "; return cbAssertCmp((_lhs " + bop->op + " _rhs), " + q + opLabel + q + ", _lhs, _rhs, " +
                        escapeFileName(currentFileName) + ", " + lineStr + "); }()";
                }
            }
        }
        const std::string condCpp = emitExpr(call.args[0], scope);
        std::string msgCpp = "std::string(" + q + "Assertion failed" + q + ")";
        if (call.args.size() > 1) {
            msgCpp = emitAsStrExpr(call.args[1], scope);
        }
        return "cbAssertCond(" + condCpp + ", " + msgCpp + ", " + escapeFileName(currentFileName) + ", " + lineStr + ")";
    }
    std::string buildErrorBubbleReturn(
    const FnDecl& fn,
    std::string_view errVar
) {
    if (deferDepth > 0) {
        return "return;";
    }
    if (fn.returnType == INVALID_ID || arena.isVoid(fn.returnType)) {
        return "return " + std::string(errVar) + ";";
    }
    const TypeNode tnode = resolveAliasNode(arena, symbols, currentModuleId, fn.returnType);
    std::string elem = "{}";
    if (tnode.name != "int" && tnode.name != "float" && tnode.name != "bool" && tnode.name != "cptr") {
        elem = defaultValueForType(currentModuleId, fn.returnType);
    }
    return "return { " + elem + ", " + std::string(errVar) + " };";
}

    void emitStmtList(
    const FnDecl& fn,
    const std::vector<StmtId>& stmts,
    std::unordered_map<std::string, VarSymbol>& scope,
    std::unordered_set<std::string>& declaredVars
) {
    for (const StmtId sid : stmts) {
        emitStmt(fn, sid, scope, declaredVars);
    }
}

    void emitErrorCheck(uint32_t line, const FnDecl& fn, const std::string& errTmp, bool breaksToCatch) {
        if (breaksToCatch) {
            std::string catchVar = "_catch_err";
            if (!catchErrStack.empty()) {
                catchVar = catchErrStack.back();
            }
            emitLine("if (cbHasError(" + errTmp + ")) {");
            indentLevel += 1;
            emitLine("cbStampError(" + errTmp + ", " + escapeFileName(currentFileName) + ", " + std::to_string(line) + ");");
            emitLine(catchVar + " = " + errTmp + ";");
            emitLine("break;");
            indentLevel -= 1;
            emitLine("}");
        } else {
            const std::string earlyRet = buildErrorBubbleReturn(fn, errTmp);
            emitLine("if (cbHasError(" + errTmp + ")) {");
            indentLevel += 1;
            emitLine("cbStampError(" + errTmp + ", " + escapeFileName(currentFileName) + ", " + std::to_string(line) + ");");
            emitLine(earlyRet);
            indentLevel -= 1;
            emitLine("}");
        }
    }

    void emitCatchBreakIfPending() {
        if (!catchErrStack.empty()) {
            emitLine("if (cbHasError(" + catchErrStack.back() + ")) { break; }");
        }
    }

    void emitFallibleAssignment(uint32_t line, 
        const FnDecl& fn,
        const VarAssignment& stmt,
        const FnSymbol& fnSym,
        std::unordered_map<std::string, VarSymbol>& scope,
        std::unordered_set<std::string>& declaredVars
    ) {
        ExprId callExprId = stmt.value;
        if (stmt.compoundRhs != INVALID_ID) {
            callExprId = stmt.compoundRhs;
        }
        const std::string callCpp = emitExpr(callExprId, scope);
        const auto [valTmp, errVar] = nextValErrTemps();
        const bool hasValue = (fnSym.returnType != INVALID_ID && !arena.isVoid(fnSym.returnType));

    if (hasValue) {
        emitLine("auto [" + valTmp + ", " + errVar + "] = " + callCpp + ";");
    } else {
        emitLine("auto " + errVar + " = " + callCpp + ";");
    }

    emitErrorCheck(line, fn, errVar, stmt.breaksToCatch);

    if (stmt.target != INVALID_ID) {
        const ExprId tid = stmt.target;
        std::string rhs = "0";
        if (hasValue) {
            rhs = valTmp;
        }
        if (stmt.compoundRhs != INVALID_ID) {
            const std::string lhsCpp = emitExpr(tid, scope, true);
            if (stmt.compoundOp == "&") {
                emitLine(lhsCpp + " += cbToStr(" + rhs + ");");
            } else {
                emitLine(lhsCpp + " " + stmt.compoundOp + "= " + rhs + ";");
            }
            return;
        }
        if (const auto* ident = arena.getExpr<Identifier>(tid)) {
            const std::string& name = ident->name;
            TypeId retT = symbols.typeAuto;
            if (stmt.hasDeclaredType) {
                retT = resolveAlias(arena, symbols, currentModuleId, stmt.declaredType);
            } else if (hasValue) {
                retT = resolveAlias(arena, symbols, fnSym.moduleId, fnSym.returnType);
            }
            const bool isNewDecl = stmt.isDim || stmt.isConst || (declaredVars.find(name) == declaredVars.end());
            scope[name] = VarSymbol{name, retT, false, stmt.isConst};
            declaredVars.insert(name);
            const std::string cppVarName = sanitizeCppIdent(name);
            if (isNewDecl) {
                std::string prefix = "auto ";
                if (stmt.isConst) {
                    prefix = "const auto ";
                }
                emitLine(prefix + cppVarName + " = " + rhs + ";");
            } else {
                emitLine(cppVarName + " = " + rhs + ";");
            }
        } else {
            const std::string lhsCpp = emitExpr(tid, scope, true);
            emitLine(lhsCpp + " = " + rhs + ";");
        }
    }
}

    void emitVarAssignment(uint32_t line, 
    const FnDecl& fn,
    const VarAssignment& stmt,
    std::unordered_map<std::string, VarSymbol>& scope,
    std::unordered_set<std::string>& declaredVars
) {
    ExprId checkExprId = stmt.value;
    if (stmt.compoundRhs != INVALID_ID) {
        checkExprId = stmt.compoundRhs;
    }
    if (const auto* call = arena.getExpr<FuncCall>(checkExprId)) {
        const auto fnSym = resolveFuncCall(arena, *call, symbols, currentModuleId, scope);
        if (fnSym.has_value() && fnSym->isFallible) {
            emitFallibleAssignment(line, fn, stmt, *fnSym, scope, declaredVars);
            return;
        }
    }

    std::string valCpp = emitExpr(stmt.value, scope);
    const TypeId valType = inferExprType(arena, stmt.value, symbols, scope, currentModuleId);

    if (stmt.hasDeclaredType && stmt.target != INVALID_ID) {
        const ExprId targetId = stmt.target;
        if (const auto* ident = arena.getExpr<Identifier>(targetId)) {
            const std::string& name = ident->name;
            const TypeId resolvedT = resolveAlias(arena, symbols, currentModuleId, stmt.declaredType);
            const TypeNode resNode = resolveAliasNode(arena, symbols, currentModuleId, stmt.declaredType);
            scope[name] = VarSymbol{name, resolvedT, false, stmt.isConst};
            declaredVars.insert(name);
            const std::string cppType = mapTypeNode(currentModuleId, resNode);
            std::string prefix;
            if (stmt.isConst) {
                prefix = "const ";
            }
            emitLine(prefix + cppType + " " + sanitizeCppIdent(name) + " = " + valCpp + ";");
            return;
        }
    }

    if (stmt.target != INVALID_ID) {
        const ExprId targetId = stmt.target;
        TypeId inferredT = symbols.typeAuto;
        if (valType != INVALID_ID) {
            inferredT = valType;
        }

        if (const auto* ident = arena.getExpr<Identifier>(targetId)) {
            const std::string& name = ident->name;
            const std::string cppVarName = sanitizeCppIdent(name);
            if (stmt.isDim || stmt.isConst || declaredVars.find(name) == declaredVars.end()) {
                scope[name] = VarSymbol{name, inferredT, false, stmt.isConst};
                declaredVars.insert(name);
                std::string inferredName = "auto";
                if (inferredT < arena.types.size()) {
                    inferredName = arena.types[inferredT].name;
                }
                std::string cppT = "auto";
                if (inferredName != "auto") {
                    cppT = mapType(currentModuleId, inferredT);
                }
                std::string prefix;
                if (stmt.isConst) {
                    prefix = "const ";
                }
                emitLine(prefix + cppT + " " + cppVarName + " = " + valCpp + ";");
            } else {
                emitLine(cppVarName + " = " + valCpp + ";");
            }
        } else {
            const std::string lhsCpp = emitExpr(targetId, scope, true);
            emitLine(lhsCpp + " = " + valCpp + ";");
        }
    }
}

    bool emitIntrinsicFallibleOrErrorCall(
        uint32_t line,
        const FnDecl& fn,
        const FuncCall& call,
        bool breaksToCatch,
        std::unordered_map<std::string, VarSymbol>& scope
    ) {
        if (cb::isAssertBuiltinCall(arena, call)) {
            const auto [discardTmp, errVar] = nextValErrTemps();
            emitLine("auto " + errVar + " = " + buildAssertCallValue(call, scope) + ";");
            emitErrorCheck(line, fn, errVar, breaksToCatch);
            return true;
        }
        if (cb::isErrorBuiltinCall(arena, call)) {
            const std::string errVal = buildErrorCallValue(call, scope);
            if (breaksToCatch && !catchErrStack.empty()) {
                const auto [discardTmp, errVar] = nextValErrTemps();
                emitLine("auto " + errVar + " = " + errVal + ";");
                emitErrorCheck(line, fn, errVar, true);
            } else {
                emitLine(buildErrorBubbleReturn(fn, errVal));
            }
            return true;
        }
        return false;
    }

    void emitExprStmt(
        uint32_t line,
        const FnDecl& fn,
        const ExprStmt& stmt,
        std::unordered_map<std::string, VarSymbol>& scope
    ) {
        if (const auto* call = arena.getExpr<FuncCall>(stmt.expr)) {
            if (emitIntrinsicFallibleOrErrorCall(line, fn, *call, stmt.breaksToCatch, scope)) {
                return;
            }
            const auto fnSym = resolveFuncCall(arena, *call, symbols, currentModuleId, scope);
            if (fnSym.has_value() && fnSym->isFallible) {
                const std::string callCpp = emitExpr(stmt.expr, scope);
                const auto [discardTmp, errVar] = nextValErrTemps();

                if (fnSym->returnType != INVALID_ID && !arena.isVoid(fnSym->returnType)) {
                    emitLine("auto [" + discardTmp + ", " + errVar + "] = " + callCpp + ";");
                } else {
                    emitLine("auto " + errVar + " = " + callCpp + ";");
                }

                emitErrorCheck(line, fn, errVar, stmt.breaksToCatch);
                return;
            }
        }

        const std::string exprCpp = emitExpr(stmt.expr, scope);
        emitLine(exprCpp + ";");
    }

    void emitReturn(
        uint32_t line,
        const FnDecl& fn,
        const ReturnStmt& stmt,
        std::unordered_map<std::string, VarSymbol>& scope
    ) {
        if (deferDepth > 0) {
            emitLine("return;");
            return;
        }
        if (!stmt.hasValue) {
            if (fn.isFallible && (fn.returnType == INVALID_ID || arena.isVoid(fn.returnType))) {
                emitLine(R"(return CbError{"", 0};)");
            } else {
                emitLine("return;");
            }
            return;
        }

        const ExprId firstId = stmt.value;
        if (const auto* call = arena.getExpr<FuncCall>(firstId)) {
            if (emitIntrinsicFallibleOrErrorCall(line, fn, *call, stmt.breaksToCatch, scope)) {
                return;
            }
            const auto calleeSym = resolveFuncCall(arena, *call, symbols, currentModuleId, scope);
            if (calleeSym.has_value() && calleeSym->isFallible) {
                const auto [valTmp, errTmp] = nextValErrTemps();
                if (calleeSym->returnType == INVALID_ID || arena.isVoid(calleeSym->returnType)) {
                    emitLine("CbError " + errTmp + " = " + emitExpr(stmt.value, scope) + ";");
                    emitErrorCheck(line, fn, errTmp, stmt.breaksToCatch);
                    if (fn.isFallible) {
                        emitLine(R"(return CbError{"", 0};)");
                    } else {
                        emitLine("return;");
                    }
                } else {
                    emitLine("auto [" + valTmp + ", " + errTmp + "] = " + emitExpr(stmt.value, scope) + ";");
                    emitErrorCheck(line, fn, errTmp, stmt.breaksToCatch);
                    if (fn.isFallible) {
                        emitLine("return { " + valTmp + R"(, CbError{"", 0} };)");
                    } else {
                        emitLine("return " + valTmp + ";");
                    }
                }
                return;
            }
        }

        const std::string valCpp = emitExpr(stmt.value, scope);
        if (fn.isFallible) {
            emitLine("return { " + valCpp + R"(, CbError{"", 0} };)");
        } else {
            emitLine("return " + valCpp + ";");
        }
    }

    void emitStmt(
    const FnDecl& fn,
    StmtId stmtId,
    std::unordered_map<std::string, VarSymbol>& scope,
    std::unordered_set<std::string>& declaredVars
) {
    if (stmtId >= arena.stmts.size()) {
        return;
    }
    const auto& stmtNode = arena.stmts[stmtId];

    if (const auto* vassign = arena.getStmt<VarAssignment>(stmtId)) {
        emitVarAssignment(stmtNode.line, fn, *vassign, scope, declaredVars);
        return;
    }

    if (const auto* estmt = arena.getStmt<ExprStmt>(stmtId)) {
        emitExprStmt(stmtNode.line, fn, *estmt, scope);
        return;
    }

    if (const auto* ifS = arena.getStmt<IfStmt>(stmtId)) {
        const std::string condCpp = emitExpr(ifS->condition, scope);
        emitLine("if (" + condCpp + ") {");
        indentLevel += 1;
        auto thenDecls = declaredVars;
        emitStmtList(fn, ifS->thenBody, scope, thenDecls);
        indentLevel -= 1;

        for (const auto& branch : ifS->elifBranches) {
            const std::string elifCond = emitExpr(branch.condition, scope);
            emitLine("} else if (" + elifCond + ") {");
            indentLevel += 1;
            auto elifDecls = declaredVars;
            emitStmtList(fn, branch.body, scope, elifDecls);
            indentLevel -= 1;
        }

        if (ifS->hasElse) {
            emitLine("} else {");
            indentLevel += 1;
            auto elseDecls = declaredVars;
            emitStmtList(fn, ifS->elseBody, scope, elseDecls);
            indentLevel -= 1;
        }

        emitLine("}");
        emitCatchBreakIfPending();
        return;
    }

    if (const auto* forS = arena.getStmt<ForStmt>(stmtId)) {
        if (forS->rangeSpec.hasRange && !forS->loopVars.empty()) {
            const std::string& loopVar = forS->loopVars[0];
            const std::string cppLoopVar = sanitizeCppIdent(loopVar);
            const std::string startCpp = emitExpr(forS->rangeSpec.start, scope);
            const std::string endCpp = emitExpr(forS->rangeSpec.end, scope);
            std::string stepCpp = "1";
            bool isKnownPositiveStep = true;
            bool isKnownNegativeStep = false;
            if (forS->rangeSpec.hasStep) {
                stepCpp = emitExpr(forS->rangeSpec.step, scope);
                isKnownPositiveStep = false;
                if (arena.getExpr<NumberLiteral>(forS->rangeSpec.step) != nullptr) {
                    isKnownPositiveStep = true;
                } else if (const auto* uop = arena.getExpr<UnaryOp>(forS->rangeSpec.step)) {
                    if (uop->op == "-" && arena.getExpr<NumberLiteral>(uop->operand) != nullptr) {
                        isKnownNegativeStep = true;
                    } else if (uop->op == "+" && arena.getExpr<NumberLiteral>(uop->operand) != nullptr) {
                        isKnownPositiveStep = true;
                    }
                }
            }
            std::string condCpp;
            if (isKnownPositiveStep) {
                if (forS->rangeSpec.inclusive) {
                    condCpp = cppLoopVar + " <= " + endCpp;
                } else {
                    condCpp = cppLoopVar + " < " + endCpp;
                }
            } else if (isKnownNegativeStep) {
                if (forS->rangeSpec.inclusive) {
                    condCpp = cppLoopVar + " >= " + endCpp;
                } else {
                    condCpp = cppLoopVar + " > " + endCpp;
                }
            } else {
                std::string inclStr = "true";
                if (!forS->rangeSpec.inclusive) {
                    inclStr = "false";
                }
                condCpp = "cbRangeCond(" + cppLoopVar + ", " + endCpp + ", " + stepCpp + ", " + inclStr + ")";
            }

            auto loopScope = scope;
            TypeId intTid = symbols.typeInt;
            if (intTid == INVALID_ID) {
                intTid = arena.findType("int");
            }
            loopScope[loopVar] = VarSymbol{loopVar, intTid, false};
            auto loopDecls = declaredVars;
            loopDecls.insert(loopVar);

            emitLine("for (int64_t " + cppLoopVar + " = " + startCpp + "; " + condCpp + "; " + cppLoopVar + " += " + stepCpp + ") {");
            indentLevel += 1;
            emitStmtList(fn, forS->body, loopScope, loopDecls);
            indentLevel -= 1;
            emitLine("}");
            emitCatchBreakIfPending();
            return;
        }

        if (forS->iterable != INVALID_ID) {
            std::string iterCpp = emitExpr(forS->iterable, scope);
            if (forS->implicitBubble || forS->breaksToCatch) {
                const auto [valTmp, errVar] = nextValErrTemps();
                emitLine("auto [" + valTmp + ", " + errVar + "] = " + iterCpp + ";");
                emitErrorCheck(stmtNode.line, fn, errVar, forS->breaksToCatch);
                iterCpp = valTmp;
            }
            const TypeId iterTid = inferExprType(arena, forS->iterable, symbols, scope, currentModuleId);
            const TypeNode iterType = resolveAliasNode(arena, symbols, currentModuleId, iterTid);

            auto loopScope = scope;
            auto loopDecls = declaredVars;
            for (const auto& lv : forS->loopVars) {
                loopDecls.insert(lv);
            }

            if (forS->loopVars.size() == 1) {
                const std::string& lv = forS->loopVars[0];
                if (iterType.name == "str") {
                    loopScope[lv] = VarSymbol{lv, symbols.typeStr, false, true};
                    emitLine("for (const auto& " + lv + " : cbStrUtf8Chars(" + iterCpp + ")) {");
                    indentLevel += 1;
                    emitStmtList(fn, forS->body, loopScope, loopDecls);
                    indentLevel -= 1;
                    emitLine("}");
                    emitCatchBreakIfPending();
                    return;
                }
                TypeId elemT = symbols.typeAuto;
                if (!iterType.typeArgs.empty()) {
                    elemT = iterType.typeArgs[0];
                } else if (iterType.name == "bytes") {
                    elemT = symbols.typeInt;
                }
                loopScope[lv] = VarSymbol{lv, elemT, false, true};
                if (iterType.name == "bytes") {
                    emitLine("for (int64_t " + lv + " : " + iterCpp + ") {");
                } else {
                    emitLine("for (const auto& " + lv + " : " + iterCpp + ") {");
                }
                indentLevel += 1;
                emitStmtList(fn, forS->body, loopScope, loopDecls);
                indentLevel -= 1;
                emitLine("}");
                emitCatchBreakIfPending();
                return;
            }
            if (forS->loopVars.size() == 2 && iterType.name == "str") {
                const std::string& idxVar = forS->loopVars[0];
                const std::string& elemVar = forS->loopVars[1];
                loopScope[idxVar] = VarSymbol{idxVar, symbols.typeInt, false, true};
                loopScope[elemVar] = VarSymbol{elemVar, symbols.typeStr, false, true};
                const std::string iterRef = nextTemp("_iter");
                emitLine("{");
                indentLevel += 1;
                emitLine("const auto " + iterRef + " = cbStrUtf8Chars(" + iterCpp + ");");
                emitLine("for (int64_t " + idxVar + " = 0; " + idxVar + " < static_cast<int64_t>(" + iterRef + ".size()); ++" + idxVar + ") {");
                indentLevel += 1;
                emitLine("const auto& " + elemVar + " = " + iterRef + "[static_cast<size_t>(" + idxVar + ")];");
                emitStmtList(fn, forS->body, loopScope, loopDecls);
                indentLevel -= 1;
                emitLine("}");
                indentLevel -= 1;
                emitLine("}");
                emitCatchBreakIfPending();
                return;
            }
            if (forS->loopVars.size() == 2 && (iterType.name == "list" || iterType.name == "bytes")) {
                const std::string& idxVar = forS->loopVars[0];
                const std::string& elemVar = forS->loopVars[1];
                TypeId elemT = symbols.typeAuto;
                if (!iterType.typeArgs.empty()) {
                    elemT = iterType.typeArgs[0];
                } else if (iterType.name == "bytes") {
                    elemT = symbols.typeInt;
                }
                loopScope[idxVar] = VarSymbol{idxVar, symbols.typeInt, false, true};
                loopScope[elemVar] = VarSymbol{elemVar, elemT, false, true};
                const std::string iterRef = nextTemp("_iter");
                emitLine("{");
                indentLevel += 1;
                emitLine("const auto& " + iterRef + " = " + iterCpp + ";");
                emitLine("for (int64_t " + idxVar + " = 0; " + idxVar + " < static_cast<int64_t>(" + iterRef + ".size()); ++" + idxVar + ") {");
                indentLevel += 1;
                if (iterType.name == "bytes") {
                    emitLine("int64_t " + elemVar + " = static_cast<int64_t>(" + iterRef + "[static_cast<size_t>(" + idxVar + ")]);");
                } else {
                    emitLine("const auto& " + elemVar + " = " + iterRef + "[static_cast<size_t>(" + idxVar + ")];");
                }
                emitStmtList(fn, forS->body, loopScope, loopDecls);
                indentLevel -= 1;
                emitLine("}");
                indentLevel -= 1;
                emitLine("}");
                emitCatchBreakIfPending();
                return;
            }
            if (iterType.typeArgs.size() == 2) {
                loopScope[forS->loopVars[0]] = VarSymbol{forS->loopVars[0], iterType.typeArgs[0], false, true};
                loopScope[forS->loopVars[1]] = VarSymbol{forS->loopVars[1], iterType.typeArgs[1], false, true};
            }
            emitLine("for (const auto& [" + joinStrings(forS->loopVars) + "] : " + iterCpp + ") {");
            indentLevel += 1;
            emitStmtList(fn, forS->body, loopScope, loopDecls);
            indentLevel -= 1;
            emitLine("}");
            emitCatchBreakIfPending();
            return;
        }
    }

    if (const auto* whileS = arena.getStmt<WhileStmt>(stmtId)) {
        auto whileScope = scope;
        auto whileDecls = declaredVars;
        if (whileS->implicitBubble || whileS->breaksToCatch) {
            emitLine("while (true) {");
            indentLevel += 1;
            const std::string callCpp = emitExpr(whileS->condition, whileScope);
            const auto [valTmp, errVar] = nextValErrTemps();
            emitLine("auto [" + valTmp + ", " + errVar + "] = " + callCpp + ";");
            emitErrorCheck(stmtNode.line, fn, errVar, whileS->breaksToCatch);
            emitLine("if (!" + valTmp + ") { break; }");
            emitStmtList(fn, whileS->body, whileScope, whileDecls);
            indentLevel -= 1;
            emitLine("}");
        } else {
            const std::string condCpp = emitExpr(whileS->condition, scope);
            emitLine("while (" + condCpp + ") {");
            indentLevel += 1;
            emitStmtList(fn, whileS->body, whileScope, whileDecls);
            indentLevel -= 1;
            emitLine("}");
        }
        emitCatchBreakIfPending();
        return;
    }

    if (const auto* selS = arena.getStmt<SelectStmt>(stmtId)) {
        const TypeId targetTid = inferExprType(arena, selS->target, symbols, scope, currentModuleId);
        const std::string targetCpp = emitExpr(selS->target, scope);

        const auto unionTargetOpt = resolveUnionSymbol(arena, symbols, currentModuleId, targetTid);
        if (unionTargetOpt.has_value()) {
            emitLine("{");
            indentLevel += 1;
            const std::string selVar = nextTemp("_sel");
            emitLine("const auto " + selVar + " = " + targetCpp + ";");
            emitLine("switch (static_cast<int64_t>(" + selVar + "._data.index())) {");
            indentLevel += 1;
            for (size_t idx = 0; idx < selS->cases.size(); ++idx) {
                const auto& c = selS->cases[idx];
                size_t lastVarIndex = 0;
                TypeId lastPayloadTid = INVALID_ID;
                for (size_t mIdx = 0; mIdx < c.matchExprs.size(); ++mIdx) {
                    std::string varName;
                    if (const auto* ident = arena.getExpr<Identifier>(c.matchExprs[mIdx])) {
                        varName = ident->name;
                    } else if (const auto* fa = arena.getExpr<FieldAccess>(c.matchExprs[mIdx])) {
                        varName = fa->fieldName;
                    }
                    const auto vIdxIt = unionTargetOpt->variantIndices.find(varName);
                    if (vIdxIt != unionTargetOpt->variantIndices.end()) {
                        lastVarIndex = vIdxIt->second;
                    }
                    const auto vTypeIt = unionTargetOpt->variantTypes.find(varName);
                    if (vTypeIt != unionTargetOpt->variantTypes.end()) {
                        lastPayloadTid = vTypeIt->second;
                    }
                    if (mIdx + 1 == c.matchExprs.size()) {
                        emitLine("case " + std::to_string(lastVarIndex) + ": {");
                    } else {
                        emitLine("case " + std::to_string(lastVarIndex) + ":");
                    }
                }
                indentLevel += 1;
                auto caseScope = scope;
                auto cDecls = declaredVars;
                if (!c.bindingName.empty()) {
                    emitLine("auto " + sanitizeCppIdent(c.bindingName) + " = std::get<" + std::to_string(lastVarIndex) + ">(" + selVar + "._data);");
                    caseScope[c.bindingName] = VarSymbol{c.bindingName, lastPayloadTid, false, false};
                    cDecls.insert(c.bindingName);
                }
                emitStmtList(fn, c.body, caseScope, cDecls);
                emitLine("break;");
                indentLevel -= 1;
                emitLine("}");
            }
            if (selS->hasCaseElse) {
                emitLine("default: {");
                indentLevel += 1;
                auto elseDecls = declaredVars;
                emitStmtList(fn, selS->caseElse, scope, elseDecls);
                emitLine("break;");
                indentLevel -= 1;
                emitLine("}");
            }
            indentLevel -= 1;
            emitLine("}");
            indentLevel -= 1;
            emitLine("}");
            emitCatchBreakIfPending();
            return;
        }

        if (targetTid == symbols.typeStr) {
            emitLine("{");
            indentLevel += 1;
            const std::string selVar = nextTemp("_sel");
            emitLine("const auto " + selVar + " = " + targetCpp + ";");
            bool isFirst = true;
            for (size_t idx = 0; idx < selS->cases.size(); ++idx) {
                const auto& c = selS->cases[idx];
                std::string cond;
                for (size_t mIdx = 0; mIdx < c.matchExprs.size(); ++mIdx) {
                    if (mIdx > 0) {
                        cond += " || ";
                    }
                    cond += selVar + " == " + emitExpr(c.matchExprs[mIdx], scope);
                }
                if (cond.empty()) {
                    cond = "false";
                }
                if (isFirst) {
                    emitLine("if (" + cond + ") {");
                    isFirst = false;
                } else {
                    emitLine("} else if (" + cond + ") {");
                }
                indentLevel += 1;
                auto cDecls = declaredVars;
                emitStmtList(fn, c.body, scope, cDecls);
                indentLevel -= 1;
            }
            if (selS->hasCaseElse) {
                if (isFirst) {
                    emitLine("{");
                    indentLevel += 1;
                    auto elseDecls = declaredVars;
                    emitStmtList(fn, selS->caseElse, scope, elseDecls);
                    indentLevel -= 1;
                    emitLine("}");
                } else {
                    emitLine("} else {");
                    indentLevel += 1;
                    auto elseDecls = declaredVars;
                    emitStmtList(fn, selS->caseElse, scope, elseDecls);
                    indentLevel -= 1;
                    emitLine("}");
                }
            } else {
                if (!isFirst) {
                    emitLine("}");
                }
            }
            indentLevel -= 1;
            emitLine("}");
            emitCatchBreakIfPending();
            return;
        }

        emitLine("switch (" + targetCpp + ") {");
        indentLevel += 1;

        for (size_t idx = 0; idx < selS->cases.size(); ++idx) {
            const auto& c = selS->cases[idx];
            for (size_t mIdx = 0; mIdx < c.matchExprs.size(); ++mIdx) {
                const std::string matchCpp = emitExpr(c.matchExprs[mIdx], scope);
                if (mIdx + 1 == c.matchExprs.size()) {
                    emitLine("case " + matchCpp + ": {");
                } else {
                    emitLine("case " + matchCpp + ":");
                }
            }
            indentLevel += 1;
            auto cDecls = declaredVars;
            emitStmtList(fn, c.body, scope, cDecls);
            emitLine("break;");
            indentLevel -= 1;
            emitLine("}");
        }

        if (selS->hasCaseElse) {
            emitLine("default: {");
            indentLevel += 1;
            auto elseDecls = declaredVars;
            emitStmtList(fn, selS->caseElse, scope, elseDecls);
            emitLine("break;");
            indentLevel -= 1;
            emitLine("}");
        }

        indentLevel -= 1;
        emitLine("}");
        emitCatchBreakIfPending();
        return;
    }

    if (const auto* tryCatch = arena.getStmt<TryCatchStmt>(stmtId)) {
        emitLine("{");
        indentLevel += 1;
        const std::string catchErr = nextTemp("_catch_err");
        emitLine("CbError " + catchErr + "{};");
        emitLine("do {");
        indentLevel += 1;
        catchErrStack.push_back(catchErr);
        auto tryScope = scope;
        auto tryDecls = declaredVars;
        emitStmtList(fn, tryCatch->tryBody, tryScope, tryDecls);
        catchErrStack.pop_back();
        indentLevel -= 1;
        emitLine("} while(false);");
        emitLine("if (cbHasError(" + catchErr + ")) {");
        indentLevel += 1;
        emitLine("CbError " + sanitizeCppIdent(tryCatch->catchVarName) + " = " + catchErr + ";");
        auto catchScope = scope;
        TypeId errTid = symbols.typeErr;
        if (errTid == INVALID_ID) {
            errTid = arena.findType("err");
        }
        catchScope[tryCatch->catchVarName] = VarSymbol{tryCatch->catchVarName, errTid, false, false};
        auto catchDecls = declaredVars;
        catchDecls.insert(tryCatch->catchVarName);
        emitStmtList(fn, tryCatch->catchBody, catchScope, catchDecls);
        indentLevel -= 1;
        emitLine("}");
        indentLevel -= 1;
        emitLine("}");
        return;
    }

    if (const auto* defS = arena.getStmt<DeferStmt>(stmtId)) {
        deferCounter += 1;
        const std::string deferVar = "_defer_" + std::to_string(deferCounter);
        emitLine("auto " + deferVar + " = cbDefer([&]() {");
        indentLevel += 1;
        deferDepth += 1;
        auto defDecls = declaredVars;
        const auto savedCatchStack = catchErrStack;
        catchErrStack.clear();
        emitStmtList(fn, defS->body, scope, defDecls);
        catchErrStack = savedCatchStack;
        deferDepth -= 1;
        indentLevel -= 1;
        emitLine("});");
        return;
    }

    if (const auto* retS = arena.getStmt<ReturnStmt>(stmtId)) {
        emitReturn(stmtNode.line, fn, *retS, scope);
        return;
    }

    if (arena.getStmt<BreakStmt>(stmtId) != nullptr) {
        emitLine("break;");
        return;
    }

    if (arena.getStmt<ContinueStmt>(stmtId) != nullptr) {
        emitLine("continue;");
        return;
    }
}

    void emitEnumDecl(const EnumDecl& ed) {
        const std::string cppName = lookupEnumCppName(currentModuleId, ed.name);
    emitLine("enum class " + cppName + " : int64_t {");
    indentLevel += 1;
    for (size_t i = 0; i < ed.members.size(); ++i) {
        const auto& m = ed.members[i];
        std::string line = sanitizeCppIdent(m.name);
        if (m.hasValue) {
            line += " = " + std::to_string(m.value);
        }
        if (i + 1 < ed.members.size()) {
            line += ",";
        }
        emitLine(line);
    }
    indentLevel -= 1;
    emitLine("};");
    emitLine("inline std::string cbToStr(" + cppName + " val) {");
    indentLevel += 1;
    emitLine("switch (val) {");
    indentLevel += 1;
    const std::string q(1, '"');
    for (const auto& m : ed.members) {
        emitLine("case " + cppName + "::" + sanitizeCppIdent(m.name) + ": return std::string(" + q + m.name + q + ");");
    }
    emitLine("default: return std::string();");
    indentLevel -= 1;
    emitLine("}");
    indentLevel -= 1;
    emitLine("}");
}
    void emitStructDecl(const TypeDecl& td) {
        const std::string cppName = lookupTypeCppName(currentModuleId, td.name);
        emitLine("struct " + cppName + " {");
        indentLevel += 1;
        for (const auto& f : td.fields) {
            const std::string fldCppName = sanitizeCppIdent(f.name);
            if (f.hasDefault && f.defaultValue != INVALID_ID) {
                std::unordered_map<std::string, VarSymbol> emptyScope;
                emitLine(mapType(currentModuleId, f.typeId) + " " + fldCppName + " = " + emitExpr(f.defaultValue, emptyScope) + ";");
            } else {
                emitLine(mapType(currentModuleId, f.typeId) + " " + fldCppName + "{};");
            }
        }
        indentLevel -= 1;
        emitLine("};");
    }

    void emitUnionDecl(const UnionDecl& ud) {
        const std::string key = symbolKey(currentModuleId, ud.name);
        if (symbols.unions.count(key) == 0) {
            return;
        }
        const UnionSymbol& usym = symbols.unions.at(key);
        const std::string cppName = usym.cppName;
        const std::string varDataCpp = unionDataVariantCppType(usym);
        emitLine("struct " + cppName + " {");
        indentLevel += 1;
        emitLine(varDataCpp + " _data{};");
        indentLevel -= 1;
        emitLine("};");
        emitLine("inline std::string cbUnionKind(const " + cppName + "& val) {");
        indentLevel += 1;
        emitLine("switch (static_cast<int64_t>(val._data.index())) {");
        indentLevel += 1;
        const std::string q(1, '"');
        for (size_t i = 0; i < usym.variants.size(); ++i) {
            emitLine("case " + std::to_string(i) + ": return std::string(" + q + usym.variants[i].name + q + ");");
        }
        emitLine("default: return std::string();");
        indentLevel -= 1;
        emitLine("}");
        indentLevel -= 1;
        emitLine("}");
    }

    void emitConstDecl(const ConstDecl& cd) {
    const std::string key = symbolKey(currentModuleId, cd.name);
    std::string cppName = makeCppConstName(currentModuleId, cd.name);
    TypeId tid = symbols.typeAuto;
    if (symbols.constants.count(key) > 0) {
        cppName = symbols.constants.at(key).cppName;
        tid = symbols.constants.at(key).typeId;
    } else if (cd.hasDeclaredType) {
        tid = resolveAlias(arena, symbols, currentModuleId, cd.declaredType);
    }
    std::unordered_map<std::string, VarSymbol> emptyScope;
    const std::string valCpp = emitExpr(cd.value, emptyScope);
    std::string cppType = "auto";
    if (tid != INVALID_ID && tid != symbols.typeAuto) {
        cppType = mapType(currentModuleId, tid);
    }
    emitLine("inline const " + cppType + " " + cppName + " = " + valCpp + ";");
}

    void emitFnDecl(const FnDecl& fn) {
        emitLine(fnSignature(currentModuleId, fn) + " {");
        indentLevel += 1;

        // Seed scope strictly from fn.params to avoid cross-overload type collisions in symbols.fnScopes.
        // Local variables will be added to this scope as they are encountered in emitStmtList -> emitVarAssignment.
        std::unordered_map<std::string, VarSymbol> scope;
        std::unordered_set<std::string> declaredVars;
        for (const auto& p : fn.params) {
            const TypeId resolvedT = resolveAlias(arena, symbols, currentModuleId, p.typeId);
            scope[p.name] = VarSymbol{p.name, resolvedT, p.isByref, !p.isByref};
            declaredVars.insert(p.name);
        }

        emitStmtList(fn, fn.body, scope, declaredVars);

        bool endsWithReturn = false;
        if (!fn.body.empty()) {
            const StmtId lastSid = fn.body.back();
            if (lastSid < arena.stmts.size()) {
                if (arena.getStmt<ReturnStmt>(lastSid) != nullptr) {
                    endsWithReturn = true;
                } else if (const auto* es = arena.getStmt<ExprStmt>(lastSid)) {
                    if (const auto* call = arena.getExpr<FuncCall>(es->expr)) {
                        if (cb::isErrorBuiltinCall(arena, *call)) {
                            endsWithReturn = true;
                        }
                    }
                }
            }
        }
        if (!endsWithReturn) {
            const bool isVoidRet = (fn.returnType == INVALID_ID || arena.isVoid(fn.returnType));
            if (fn.isFallible) {
                if (isVoidRet) {
                    emitLine(R"(return CbError{"", 0};)");
                } else {
                    emitLine("return { " + defaultValueForType(currentModuleId, fn.returnType) + R"(, CbError{"", 0} };)");
                }
            } else if (!isVoidRet) {
                emitLine("return " + defaultValueForType(currentModuleId, fn.returnType) + ";");
            }
        }

        indentLevel -= 1;
        emitLine("}");
    }

    void scanReachableExpr(
        ExprId exprId,
        const std::string& modId,
        const std::unordered_map<std::string, VarSymbol>& scope,
        std::unordered_set<std::string>& reachable,
        std::vector<std::string>& worklist
    ) const {
        if (exprId >= arena.exprs.size()) {
            return;
        }

        auto markReachable = [&](const std::string& key) {
            if (key.empty() || reachable.find(key) != reachable.end()) {
                return;
            }
            reachable.insert(key);
            worklist.push_back(key);
        };

        if (arena.getExpr<Identifier>(exprId) != nullptr) {
            const auto bareFnOpt = resolveBareFnRef(arena, exprId, symbols, scope, modId);
            if (bareFnOpt.has_value() && !bareFnOpt->isBuiltinReceiver && !bareFnOpt->isFnValueCall) {
                markReachable(symbolKey(bareFnOpt->moduleId, bareFnOpt->name));
            }
            return;
        }

        if (const auto* uop = arena.getExpr<UnaryOp>(exprId)) {
            scanReachableExpr(uop->operand, modId, scope, reachable, worklist);
            return;
        }

        if (const auto* bop = arena.getExpr<BinaryOp>(exprId)) {
            scanReachableExpr(bop->left, modId, scope, reachable, worklist);
            scanReachableExpr(bop->right, modId, scope, reachable, worklist);
            return;
        }

        if (const auto* faccess = arena.getExpr<FieldAccess>(exprId)) {
            const auto bareFnOpt = resolveBareFnRef(arena, exprId, symbols, scope, modId);
            if (bareFnOpt.has_value() && !bareFnOpt->isBuiltinReceiver && !bareFnOpt->isFnValueCall) {
                markReachable(symbolKey(bareFnOpt->moduleId, bareFnOpt->name));
            }
            scanReachableExpr(faccess->target, modId, scope, reachable, worklist);
            return;
        }

        if (const auto* iaccess = arena.getExpr<IndexAccess>(exprId)) {
            scanReachableExpr(iaccess->target, modId, scope, reachable, worklist);
            scanReachableExpr(iaccess->index, modId, scope, reachable, worklist);
            return;
        }

        if (const auto* llit = arena.getExpr<ListLiteral>(exprId)) {
            for (const ExprId elem : llit->elements) {
                scanReachableExpr(elem, modId, scope, reachable, worklist);
            }
            return;
        }

        if (const auto* dlit = arena.getExpr<DictLiteral>(exprId)) {
            for (const auto& ent : dlit->entries) {
                scanReachableExpr(ent.key, modId, scope, reachable, worklist);
                scanReachableExpr(ent.value, modId, scope, reachable, worklist);
            }
            return;
        }

        if (const auto* call = arena.getExpr<FuncCall>(exprId)) {
            scanReachableExpr(call->callee, modId, scope, reachable, worklist);
            for (const ExprId argId : call->args) {
                scanReachableExpr(argId, modId, scope, reachable, worklist);
            }
            for (const auto& na : call->namedArgs) {
                scanReachableExpr(na.value, modId, scope, reachable, worklist);
            }

            const auto structOpt = resolveStructConstructor(arena, *call, symbols, modId, scope);
            if (structOpt.has_value()) {
                std::unordered_map<std::string, VarSymbol> emptyScope;
                for (const auto& fdecl : structOpt->fieldOrder) {
                    if (fdecl.hasDefault && fdecl.defaultValue != INVALID_ID) {
                        scanReachableExpr(fdecl.defaultValue, structOpt->moduleId, emptyScope, reachable, worklist);
                    }
                }
                return;
            }

            const auto fnOpt = resolveFuncCall(arena, *call, symbols, modId, scope);
            if (fnOpt.has_value() && !fnOpt->isBuiltinReceiver && !fnOpt->isFnValueCall) {
                markReachable(symbolKey(fnOpt->moduleId, fnOpt->name));
                std::unordered_map<std::string, VarSymbol> emptyScope;
                for (const auto& p : fnOpt->params) {
                    if (p.hasDefault && p.defaultValue != INVALID_ID) {
                        scanReachableExpr(p.defaultValue, fnOpt->moduleId, emptyScope, reachable, worklist);
                    }
                }
            }
            return;
        }
    }

    void scanReachableStmtList(
        const std::vector<StmtId>& stmts,
        const std::string& modId,
        std::unordered_map<std::string, VarSymbol>& scope,
        std::unordered_set<std::string>& reachable,
        std::vector<std::string>& worklist
    ) const {
        for (const StmtId stmtId : stmts) {
            if (stmtId >= arena.stmts.size()) {
                continue;
            }

            if (const auto* vassign = arena.getStmt<VarAssignment>(stmtId)) {
                scanReachableExpr(vassign->value, modId, scope, reachable, worklist);
                if (vassign->target != INVALID_ID) {
                    if (const auto* ident = arena.getExpr<Identifier>(vassign->target)) {
                        TypeId varTid = symbols.typeAuto;
                        if (vassign->hasDeclaredType) {
                            varTid = resolveAlias(arena, symbols, modId, vassign->declaredType);
                        } else {
                            varTid = inferExprType(arena, vassign->value, symbols, scope, modId);
                        }
                        if (vassign->isDim || vassign->isConst || scope.find(ident->name) == scope.end()) {
                            scope[ident->name] = VarSymbol{ident->name, varTid, false, vassign->isConst};
                        }
                    } else {
                        scanReachableExpr(vassign->target, modId, scope, reachable, worklist);
                    }
                }
                continue;
            }

            if (const auto* estmt = arena.getStmt<ExprStmt>(stmtId)) {
                scanReachableExpr(estmt->expr, modId, scope, reachable, worklist);
                continue;
            }

            if (const auto* ifS = arena.getStmt<IfStmt>(stmtId)) {
                scanReachableExpr(ifS->condition, modId, scope, reachable, worklist);
                auto thenScope = scope;
                scanReachableStmtList(ifS->thenBody, modId, thenScope, reachable, worklist);
                for (const auto& branch : ifS->elifBranches) {
                    scanReachableExpr(branch.condition, modId, scope, reachable, worklist);
                    auto elifScope = scope;
                    scanReachableStmtList(branch.body, modId, elifScope, reachable, worklist);
                }
                if (ifS->hasElse) {
                    auto elseScope = scope;
                    scanReachableStmtList(ifS->elseBody, modId, elseScope, reachable, worklist);
                }
                continue;
            }

            if (const auto* forS = arena.getStmt<ForStmt>(stmtId)) {
                auto loopScope = scope;
                if (forS->rangeSpec.hasRange) {
                    scanReachableExpr(forS->rangeSpec.start, modId, scope, reachable, worklist);
                    scanReachableExpr(forS->rangeSpec.end, modId, scope, reachable, worklist);
                    if (forS->rangeSpec.hasStep) {
                        scanReachableExpr(forS->rangeSpec.step, modId, scope, reachable, worklist);
                    }
                    if (!forS->loopVars.empty()) {
                        loopScope[forS->loopVars[0]] = VarSymbol{forS->loopVars[0], symbols.typeInt, false, false};
                    }
                } else if (forS->iterable != INVALID_ID) {
                    scanReachableExpr(forS->iterable, modId, scope, reachable, worklist);
                    const TypeId iterTid = inferExprType(arena, forS->iterable, symbols, scope, modId);
                    const TypeNode iterType = resolveAliasNode(arena, symbols, modId, iterTid);
                    if (forS->loopVars.size() == 1) {
                        TypeId elemT = symbols.typeAuto;
                        if (iterType.name == "str") {
                            elemT = symbols.typeStr;
                        } else if (!iterType.typeArgs.empty()) {
                            elemT = iterType.typeArgs[0];
                        } else if (iterType.name == "bytes") {
                            elemT = symbols.typeInt;
                        }
                        loopScope[forS->loopVars[0]] = VarSymbol{forS->loopVars[0], elemT, false, true};
                    } else if (forS->loopVars.size() == 2) {
                        if (iterType.name == "str") {
                            loopScope[forS->loopVars[0]] = VarSymbol{forS->loopVars[0], symbols.typeInt, false, true};
                            loopScope[forS->loopVars[1]] = VarSymbol{forS->loopVars[1], symbols.typeStr, false, true};
                        } else if (iterType.name == "list" || iterType.name == "bytes") {
                            TypeId elemT = symbols.typeAuto;
                            if (!iterType.typeArgs.empty()) {
                                elemT = iterType.typeArgs[0];
                            } else if (iterType.name == "bytes") {
                                elemT = symbols.typeInt;
                            }
                            loopScope[forS->loopVars[0]] = VarSymbol{forS->loopVars[0], symbols.typeInt, false, true};
                            loopScope[forS->loopVars[1]] = VarSymbol{forS->loopVars[1], elemT, false, true};
                        } else if (iterType.typeArgs.size() == 2) {
                            loopScope[forS->loopVars[0]] = VarSymbol{forS->loopVars[0], iterType.typeArgs[0], false, true};
                            loopScope[forS->loopVars[1]] = VarSymbol{forS->loopVars[1], iterType.typeArgs[1], false, true};
                        }
                    }
                }
                scanReachableStmtList(forS->body, modId, loopScope, reachable, worklist);
                continue;
            }

            if (const auto* whileS = arena.getStmt<WhileStmt>(stmtId)) {
                scanReachableExpr(whileS->condition, modId, scope, reachable, worklist);
                auto whileScope = scope;
                scanReachableStmtList(whileS->body, modId, whileScope, reachable, worklist);
                continue;
            }

            if (const auto* selS = arena.getStmt<SelectStmt>(stmtId)) {
                scanReachableExpr(selS->target, modId, scope, reachable, worklist);
                const TypeId targetTid = inferExprType(arena, selS->target, symbols, scope, modId);
                const auto unionTargetOpt = resolveUnionSymbol(arena, symbols, modId, targetTid);
                for (const auto& c : selS->cases) {
                    TypeId lastPayloadTid = INVALID_ID;
                    for (const ExprId mExpr : c.matchExprs) {
                        scanReachableExpr(mExpr, modId, scope, reachable, worklist);
                        if (unionTargetOpt.has_value()) {
                            std::string varName;
                            if (const auto* ident = arena.getExpr<Identifier>(mExpr)) {
                                varName = ident->name;
                            } else if (const auto* fa = arena.getExpr<FieldAccess>(mExpr)) {
                                varName = fa->fieldName;
                            }
                            const auto vTypeIt = unionTargetOpt->variantTypes.find(varName);
                            if (vTypeIt != unionTargetOpt->variantTypes.end()) {
                                lastPayloadTid = vTypeIt->second;
                            }
                        }
                    }
                    auto caseScope = scope;
                    if (!c.bindingName.empty() && lastPayloadTid != INVALID_ID) {
                        caseScope[c.bindingName] = VarSymbol{c.bindingName, lastPayloadTid, false, false};
                    }
                    scanReachableStmtList(c.body, modId, caseScope, reachable, worklist);
                }
                if (selS->hasCaseElse) {
                    auto elseScope = scope;
                    scanReachableStmtList(selS->caseElse, modId, elseScope, reachable, worklist);
                }
                continue;
            }

            if (const auto* tryCatch = arena.getStmt<TryCatchStmt>(stmtId)) {
                scanReachableStmtList(tryCatch->tryBody, modId, scope, reachable, worklist);
                auto errScope = scope;
                errScope[tryCatch->catchVarName] = VarSymbol{tryCatch->catchVarName, symbols.typeErr, false, false};
                scanReachableStmtList(tryCatch->catchBody, modId, errScope, reachable, worklist);
                continue;
            }

            if (const auto* defS = arena.getStmt<DeferStmt>(stmtId)) {
                auto defScope = scope;
                scanReachableStmtList(defS->body, modId, defScope, reachable, worklist);
                continue;
            }

            if (const auto* retS = arena.getStmt<ReturnStmt>(stmtId)) {
                if (retS->hasValue) {
                    scanReachableExpr(retS->value, modId, scope, reachable, worklist);
                }
                continue;
            }
        }
    }

    std::unordered_set<std::string> computeReachableFunctions(const Program& program) const {
        std::unordered_map<std::string, std::vector<std::pair<std::string, FnDecl>>> fnDeclsByKey;
        std::unordered_set<std::string> reachable;
        std::vector<std::string> worklist;

        auto markReachable = [&](const std::string& key) {
            if (key.empty() || reachable.find(key) != reachable.end()) {
                return;
            }
            reachable.insert(key);
            worklist.push_back(key);
        };

        std::unordered_map<std::string, VarSymbol> emptyScope;
        for (const DeclId did : program.declarations) {
            if (did >= arena.decls.size()) {
                continue;
            }
            const auto& declNode = arena.decls[did];
            if (!isModuleEmitted(symbols, declNode.moduleId)) {
                continue;
            }
            if (const auto* td = arena.getDecl<TypeDecl>(did)) {
                if (!td->isExtern) {
                    for (const auto& f : td->fields) {
                        if (f.hasDefault && f.defaultValue != INVALID_ID) {
                            scanReachableExpr(f.defaultValue, declNode.moduleId, emptyScope, reachable, worklist);
                        }
                    }
                }
            } else if (const auto* cd = arena.getDecl<ConstDecl>(did)) {
                scanReachableExpr(cd->value, declNode.moduleId, emptyScope, reachable, worklist);
            } else if (const auto* fd = arena.getDecl<FnDecl>(did)) {
                if (!fd->isExtern) {
                    const std::string key = symbolKey(declNode.moduleId, fd->name);
                    fnDeclsByKey[key].push_back({declNode.moduleId, *fd});
                    if (declNode.moduleId.empty()) {
                        markReachable(key);
                    }
                }
            }
        }

        while (!worklist.empty()) {
            const std::string key = worklist.back();
            worklist.pop_back();
            const auto it = fnDeclsByKey.find(key);
            if (it == fnDeclsByKey.end()) {
                continue;
            }
            for (const auto& [fnModId, fnDecl] : it->second) {
                std::unordered_map<std::string, VarSymbol> scope;
                for (const auto& p : fnDecl.params) {
                    if (p.hasDefault && p.defaultValue != INVALID_ID) {
                        scanReachableExpr(p.defaultValue, fnModId, emptyScope, reachable, worklist);
                    }
                    const TypeId resolvedT = resolveAlias(arena, symbols, fnModId, p.typeId);
                    scope[p.name] = VarSymbol{p.name, resolvedT, p.isByref, !p.isByref};
                }
                scanReachableStmtList(fnDecl.body, fnModId, scope, reachable, worklist);
            }
        }

        return reachable;
    }

    std::string emitProgram(const Program& program) {
    if (!symbols.linkLibs.empty()) {
        std::string flags;
        for (size_t i = 0; i < symbols.linkLibs.size(); ++i) {
            if (i > 0) flags += " ";
            flags += "-l" + symbols.linkLibs[i];
        }
        emitLine("// CleanBasic Linker Flags: " + flags);
    }

    lines.push_back(std::string(CPP_RUNTIME_PREAMBLE));

    if (!symbols.headers.empty()) {
        emitLine();
        for (const auto& hdr : symbols.headers) {
            emitLine(R"(#include ")" + hdr + R"(")");
        }
        emitLine("#undef min");
        emitLine("#undef max");
        emitLine("#undef ERROR");
    }

    std::vector<std::pair<std::string, EnumDecl>> enumDecls;
    std::vector<std::pair<std::string, TypeDecl>> structDecls;
    std::vector<std::pair<std::string, UnionDecl>> unionDecls;
    std::vector<std::pair<std::string, DeclId>> orderedCompositeDecls;
    std::vector<std::pair<std::string, AliasDecl>> aliasDecls;
    std::vector<std::pair<std::string, ConstDecl>> earlyConstDecls;
    std::vector<std::pair<std::string, ConstDecl>> structConstDecls;
    struct EmittedFnEntry {
        std::string moduleId;
        std::string fileName;
        FnDecl fn;
    };
    std::vector<EmittedFnEntry> fnDecls;
    const std::unordered_set<std::string> reachableFns = computeReachableFunctions(program);

    for (const DeclId did : program.declarations) {
        if (did >= arena.decls.size()) continue;
        const auto& declNode = arena.decls[did];
        if (!isModuleEmitted(symbols, declNode.moduleId)) continue;
        if (const auto* td = arena.getDecl<TypeDecl>(did)) {
            if (!td->isExtern) {
                structDecls.push_back({declNode.moduleId, *td});
                orderedCompositeDecls.push_back({declNode.moduleId, did});
            }
        } else if (const auto* ud = arena.getDecl<UnionDecl>(did)) {
            unionDecls.push_back({declNode.moduleId, *ud});
            orderedCompositeDecls.push_back({declNode.moduleId, did});
        } else if (const auto* ed = arena.getDecl<EnumDecl>(did)) {
            enumDecls.push_back({declNode.moduleId, *ed});
        } else if (const auto* ad = arena.getDecl<AliasDecl>(did)) {
            aliasDecls.push_back({declNode.moduleId, *ad});
        } else if (const auto* cd = arena.getDecl<ConstDecl>(did)) {
            const std::string cKey = symbolKey(declNode.moduleId, cd->name);
            bool isStructConst = false;
            if (symbols.constants.count(cKey) > 0) {
                const TypeNode tnode = resolveAliasNode(arena, symbols, declNode.moduleId, symbols.constants.at(cKey).typeId);
                if ((symbols.types.count(tnode.name) > 0 && tnode.name != "err") || symbols.unions.count(tnode.name) > 0) {
                    isStructConst = true;
                }
            }
            if (isStructConst) {
                structConstDecls.push_back({declNode.moduleId, *cd});
            } else {
                earlyConstDecls.push_back({declNode.moduleId, *cd});
            }
        } else if (const auto* fd = arena.getDecl<FnDecl>(did)) {
            if (!fd->isExtern) {
                const std::string fnKey = symbolKey(declNode.moduleId, fd->name);
                if (declNode.moduleId.empty() || reachableFns.find(fnKey) != reachableFns.end()) {
                    fnDecls.push_back(EmittedFnEntry{declNode.moduleId, declNode.fileName, *fd});
                }
            }
        }
    }

    if (!enumDecls.empty()) {
        emitLine();
        for (const auto& pair : enumDecls) {
            currentModuleId = pair.first;
            emitLine("enum class " + lookupEnumCppName(currentModuleId, pair.second.name) + " : int64_t;");
        }
        emitLine();
        for (const auto& pair : enumDecls) {
            currentModuleId = pair.first;
            emitEnumDecl(pair.second);
            emitLine();
        }
    }

    if (!structDecls.empty() || !unionDecls.empty() || !aliasDecls.empty()) {
        emitLine();
        for (const auto& pair : structDecls) {
            currentModuleId = pair.first;
            emitLine("struct " + lookupTypeCppName(currentModuleId, pair.second.name) + ";");
        }
        for (const auto& pair : unionDecls) {
            currentModuleId = pair.first;
            emitLine("struct " + lookupUnionCppName(currentModuleId, pair.second.name) + ";");
        }
        for (const auto& pair : aliasDecls) {
            currentModuleId = pair.first;
            emitLine("using " + lookupAliasCppName(currentModuleId, pair.second.name) + " = " + mapType(currentModuleId, pair.second.targetType) + ";");
        }
    }

    if (!earlyConstDecls.empty()) {
        emitLine();
        for (const auto& pair : earlyConstDecls) {
            currentModuleId = pair.first;
            emitConstDecl(pair.second);
        }
    }

    if (!orderedCompositeDecls.empty()) {
        std::unordered_map<std::string, std::pair<std::string, DeclId>> compositeByKey;
        std::vector<std::string> initialCompositeKeys;
        initialCompositeKeys.reserve(orderedCompositeDecls.size());
        for (const auto& pair : orderedCompositeDecls) {
            const DeclId did = pair.second;
            std::string key;
            if (const auto* td = arena.getDecl<TypeDecl>(did)) {
                key = symbolKey(pair.first, td->name);
            } else if (const auto* ud = arena.getDecl<UnionDecl>(did)) {
                key = symbolKey(pair.first, ud->name);
            }
            if (!key.empty() && compositeByKey.count(key) == 0) {
                compositeByKey[key] = pair;
                initialCompositeKeys.push_back(key);
            }
        }

        std::unordered_set<std::string> visitedCompositeKeys;
        std::vector<std::pair<std::string, DeclId>> sortedCompositeDecls;
        sortedCompositeDecls.reserve(orderedCompositeDecls.size());

        auto visitComposite = [&](auto& self, const std::string& key) -> void {
            if (visitedCompositeKeys.count(key) > 0 || compositeByKey.count(key) == 0) {
                return;
            }
            visitedCompositeKeys.insert(key);
            for (const std::string& depKey : collectDirectCompositeDeps(arena, symbols, key)) {
                if (compositeByKey.count(depKey) > 0) {
                    self(self, depKey);
                }
            }
            sortedCompositeDecls.push_back(compositeByKey.at(key));
        };

        for (const std::string& key : initialCompositeKeys) {
            visitComposite(visitComposite, key);
        }

        emitLine();
        for (const auto& pair : sortedCompositeDecls) {
            currentModuleId = pair.first;
            const DeclId did = pair.second;
            if (const auto* td = arena.getDecl<TypeDecl>(did)) {
                emitStructDecl(*td);
                emitLine();
            } else if (const auto* ud = arena.getDecl<UnionDecl>(did)) {
                emitUnionDecl(*ud);
                emitLine();
            }
        }
    }

    if (!structConstDecls.empty()) {
        emitLine();
        for (const auto& pair : structConstDecls) {
            currentModuleId = pair.first;
            emitConstDecl(pair.second);
        }
    }

    if (!fnDecls.empty()) {
        emitLine("// Forward Declarations");
        for (const auto& entry : fnDecls) {
            currentModuleId = entry.moduleId;
            if (entry.fileName.empty()) {
                currentFileName = "main.cb";
            } else {
                currentFileName = entry.fileName;
            }
            emitLine(fnSignature(currentModuleId, entry.fn) + ";");
        }
        emitLine();
    }

    for (const auto& entry : fnDecls) {
        currentModuleId = entry.moduleId;
        if (entry.fileName.empty()) {
            currentFileName = "main.cb";
        } else {
            currentFileName = entry.fileName;
        }
        emitFnDecl(entry.fn);
        emitLine();
    }

    const auto mainIt = symbols.functions.find(symbolKey("", "main"));
    if (mainIt != symbols.functions.end()) {
        const FnSymbol& mainSym = mainIt->second;
        const bool mainReturnsInt = (mainSym.returnType == symbols.typeInt);
        emitLine("int main(int argc, char** argv) {");
        indentLevel += 1;
        emitLine("auto& _cbArgs = cbRuntimeArgs();");
        emitLine("_cbArgs.clear();");
        emitLine("for (int _i = 0; _i < argc; ++_i) {");
        indentLevel += 1;
        emitLine("_cbArgs.emplace_back(argv[_i]);");
        indentLevel -= 1;
        emitLine("}");
        if (mainSym.isFallible && mainReturnsInt) {
            emitLine("auto [_mainRc, _mainErr] = cbMain();");
            emitLine("if (cbHasError(_mainErr)) {");
            indentLevel += 1;
            emitLine(R"(std::cerr << "Unhandled error (code=" << _mainErr.code << "): " << _mainErr.msg << std::endl;)");
            emitLine("return 1;");
            indentLevel -= 1;
            emitLine("}");
            emitLine("return static_cast<int>(_mainRc);");
        } else if (mainSym.isFallible) {
            emitLine("auto _mainErr = cbMain();");
            emitLine("if (cbHasError(_mainErr)) {");
            indentLevel += 1;
            emitLine(R"(std::cerr << "Unhandled error (code=" << _mainErr.code << "): " << _mainErr.msg << std::endl;)");
            emitLine("return 1;");
            indentLevel -= 1;
            emitLine("}");
            emitLine("return 0;");
        } else if (mainReturnsInt) {
            emitLine("return static_cast<int>(cbMain());");
        } else {
            emitLine("cbMain();");
            emitLine("return 0;");
        }
        indentLevel -= 1;
        emitLine("}");
    }

    std::string out;
    for (const auto& line : lines) {
        out += line;
        out.push_back(static_cast<char>(10));
    }
    return out;
    }
};

inline std::string emitProgram(
    const AstArena& arena,
    const GlobalSymbolTable& symbols,
    const Program& program
) {
    Emitter emitter(arena, symbols);
    return emitter.emitProgram(program);
}

} // namespace cb

#endif // CLEANBASIC_EMITTER_HPP
