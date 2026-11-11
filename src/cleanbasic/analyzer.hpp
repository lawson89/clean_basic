#ifndef CLEANBASIC_ANALYZER_HPP
#define CLEANBASIC_ANALYZER_HPP

#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "src/cleanbasic/runtime_preamble.hpp"
#include "src/cleanbasic/symbols.hpp"

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
 * CleanBasic Fused Semantic Analyzer & Unity C++17 Emitter
 *
 * Implements a Two-Pass Symbol-Header + Fused Typecheck/Codegen pipeline:
 * - Pass 1a: Registers module imports, type aliases, and type/enum/union shell declarations.
 * - Pass 1b: Canonicalizes struct fields, union variants, enum values, module constants, and
 *   function signatures, then verifies and topologically sorts composite value types.
 * - Pass 2 (Fused Typecheck & C++17 Codegen): Validates and emits top-level constants and function
 *   bodies in a single AST walk (checkAndEmitStmt / checkAndEmitExpr) into per-constant and
 *   per-function buffers while recording callGraphEdges and referencedModules, then computes
 *   reachable functions (computeReachableFunctions) and assembles the Unity C++17 build.cpp output.
 */
class Analyzer {
    std::string escapeFileName(std::string_view path) const {
        const char q = '"';
        const char bs = static_cast<char>(92);
        std::string out(1, q);
        for (char c : path) {
            if (c == bs) {
                out += bs;
                out += bs;
            } else if (c == q) {
                out += bs;
                out += q;
            } else {
                out += c;
            }
        }
        out += q;
        return out;
    }

public:
    AstArena& arena;
    GlobalSymbolTable& symbols;
    std::string fileName = "main.cb";
    std::string moduleId = "";
    std::string currentFnKey = "";
    int loopDepth = 0;
    std::vector<std::string> lines;
    int indentLevel = 0;
    uint32_t tempCounter = 0;
    std::vector<std::string> catchErrStack;

    Analyzer(AstArena& arena, GlobalSymbolTable& symbols, std::string_view fileName = "main.cb")
        : arena(arena), symbols(symbols), fileName(fileName), moduleId(""), currentFnKey(""), loopDepth(0) {}

    [[noreturn]] void setAnalyzerError(uint32_t line, uint32_t col, std::string_view msg) const {
        throw makeError("AnalyzerError", std::string(msg), fileName, line, col);
    }

    TypeId inferExpr(ExprId exprId, const std::unordered_map<std::string, VarSymbol>& scope) {
        return inferAndCacheExprType(arena, exprId, symbols, scope, moduleId);
    }

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

    std::string mapBaseType(std::string_view modId, const TypeNode& t) const {
        if (t.name == "int") return "int64_t";
        if (t.name == "float") return "double";
        if (t.name == "bool") return "bool";
        if (t.name == "str") return "std::string";
        if (t.name == "err") return "CbError";
        if (t.name == "void") return "void";
        if (t.name == "auto") return "auto";
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
        if (resolved.name == "str") return R"(std::string(""))";
        if (isEnumType(symbols, tid, arena, modId)) {
            return "static_cast<" + mapTypeNode(modId, resolved) + ">(0)";
        }
        return mapTypeNode(modId, resolved) + "{}";
    }

    std::string wrapFallibleReturnCppType(std::string_view modId, TypeId returnType, bool isFallible) const {
        std::string ret = "void";
        if (returnType != INVALID_ID && !arena.isVoid(returnType)) {
            ret = mapType(modId, returnType);
        }
        if (isFallible) {
            if (ret == "void") {
                return "CbError";
            }
            return "std::tuple<" + ret + ", CbError>";
        }
        return ret;
    }

    std::string fnReturnCppType(std::string_view modId, const FnDecl& fn) const {
        return wrapFallibleReturnCppType(modId, fn.returnType, fn.isFallible);
    }

    bool isPassByConstRefType(const TypeNode& resolved) const {
        if (resolved.name == "int" || resolved.name == "float" ||
            resolved.name == "bool" ||
            resolved.name == "void" || resolved.name == "auto") {
            return false;
        }
        if (symbols.enums.count(resolved.name) > 0) {
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
        if (isPassByConstRefType(resolved)) {
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

    void recordCalleeEdge(const FnSymbol& fnSym) {
        if (fnSym.isBuiltinReceiver) {
            return;
        }
        const std::string calleeKey = symbolKey(fnSym.moduleId, fnSym.name);
        if (calleeKey.empty()) {
            return;
        }
        if (!currentFnKey.empty()) {
            symbols.callGraphEdges[currentFnKey].insert(calleeKey);
        }
    }

    struct RootVarInfo {
        std::string name;
        bool isConst = false;
        bool found = false;
    };

    RootVarInfo findRootVar(ExprId exprId, const std::unordered_map<std::string, VarSymbol>& scope) const {
        if (exprId >= arena.exprs.size()) return {};
        if (const auto* ident = arena.getExpr<Identifier>(exprId)) {
            const auto it = scope.find(ident->name);
            if (it != scope.end()) {
                return RootVarInfo{it->second.name, it->second.isConst, true};
            }
            const auto cOpt = resolveConstSymbol(symbols, moduleId, scope, ident->name);
            if (cOpt.has_value()) {
                return RootVarInfo{cOpt->name, true, true};
            }
            return RootVarInfo{ident->name, false, false};
        }
        if (const auto* fa = arena.getExpr<FieldAccess>(exprId)) {
            const auto mcOpt = resolveModuleConstAccess(arena, symbols, moduleId, scope, *fa);
            if (mcOpt.has_value()) {
                return RootVarInfo{mcOpt->name, true, true};
            }
            return findRootVar(fa->target, scope);
        }
        if (const auto* ia = arena.getExpr<IndexAccess>(exprId)) {
            return findRootVar(ia->target, scope);
        }
        return {};
    }

    void checkFallibleUsage(
        const FnDecl& fn,
        ExprId exprId,
        const std::unordered_map<std::string, VarSymbol>& scope
    ) const {
        if (const auto* call = arena.getExpr<FuncCall>(exprId)) {
            bool isFallible = false;
            std::string calleeName = "";
            if (isAssertBuiltinCall(arena, *call)) {
                isFallible = true;
                calleeName = "assert";
            } else if (isErrorBuiltinCall(arena, *call) && !catchErrStack.empty()) {
                return;
            } else {
                const auto fnSymOpt = resolveFuncCall(arena, *call, symbols, moduleId, scope);
                if (fnSymOpt.has_value()) {
                    isFallible = fnSymOpt->isFallible;
                    calleeName = fnSymOpt->name;
                }
            }

            if (isFallible && catchErrStack.empty() && !fn.isFallible) {
                const auto& exprNode = arena.exprs[exprId];
                std::string msg = "Unhandled error: function returns an error but caller does not declare 'can error' or have an active 'try / catch' block (function '" +
                    fn.name + "' calls fallible function '" + calleeName + "').";
                setAnalyzerError(exprNode.line, exprNode.col, msg);
            }
        }
    }

    void checkCrossModuleAccess(
        uint32_t line,
        uint32_t col,
        std::string_view kindLabel,
        const SymbolMeta& sym,
        std::string_view displayName = ""
    ) {
        if (!sym.moduleId.empty() && sym.moduleId != moduleId) {
            symbols.referencedModules.insert(sym.moduleId);
        }
        if (sym.moduleId != moduleId && !sym.isPub) {
            std::string_view label = sym.name;
            if (!displayName.empty()) {
                label = displayName;
            }
            setAnalyzerError(line, col, std::string(kindLabel) + " '" + std::string(label) + "' is private to module '" + sym.moduleId + "' (missing 'public').");
        }
    }

    void validateByrefArg(
        uint32_t line,
        uint32_t col,
        ExprId argExprId,
        const std::unordered_map<std::string, VarSymbol>& scope,
        const FnSymbol& fnSym,
        std::string_view paramName,
        bool isUfcsRecv = false
    ) const {
        const auto root = findRootVar(argExprId, scope);
        if (root.found && root.isConst) {
            if (isUfcsRecv && fnSym.isBuiltinReceiver) {
                setAnalyzerError(line, col, "Cannot call '." + fnSym.name + "()' on const variable '" + root.name + "'.");
            }
            setAnalyzerError(line, col, "Cannot pass const variable '" + root.name + "' to byref parameter '" + std::string(paramName) + "'.");
        }
    }

    std::string emitAsStrExpr(
        const FnDecl& fn,
        ExprId exprId,
        const std::unordered_map<std::string, VarSymbol>& scope
    ) {
        const TypeId tid = inferExpr(exprId, scope);
        const std::string cpp = checkAndEmitExpr(fn, exprId, scope, false, false);
        if (tid < arena.types.size() && arena.types[tid].name == "str") {
            return cpp;
        }
        return "cbToStr(" + cpp + ")";
    }

    std::vector<std::string> checkAndEmitExprList(
        const FnDecl& fn,
        const std::vector<ExprId>& exprIds,
        const std::unordered_map<std::string, VarSymbol>& scope
    ) {
        std::vector<std::string> rendered;
        rendered.reserve(exprIds.size());
        for (const ExprId eid : exprIds) {
            rendered.push_back(checkAndEmitExpr(fn, eid, scope, false, false));
        }
        return rendered;
    }

    std::string buildErrorCallValue(
        const FnDecl& fn,
        const FuncCall& call,
        const std::unordered_map<std::string, VarSymbol>& scope
    ) {
        std::string msgCpp = R"(std::string(""))";
        if (!call.args.empty()) {
            msgCpp = emitAsStrExpr(fn, call.args[0], scope);
        }
        std::string codeCpp = "1";
        if (call.args.size() >= 2) {
            codeCpp = checkAndEmitExpr(fn, call.args[1], scope, false, false);
        }
        const auto& calleeNode = arena.exprs[call.callee];
        return "CbError{" + msgCpp + ", " + codeCpp + ", std::string(" + escapeFileName(fileName) + "), " + std::to_string(calleeNode.line) + "}";
    }

    std::string buildAssertCallValue(
        const FnDecl& fn,
        const FuncCall& call,
        const std::unordered_map<std::string, VarSymbol>& scope
    ) {
        const auto& calleeNode = arena.exprs[call.callee];
        const std::string lineStr = std::to_string(calleeNode.line);
        const std::string q(1, '"');
        const std::string condCpp = checkAndEmitExpr(fn, call.args[0], scope, false, false);
        std::string msgCpp = "std::string(" + q + "Assertion failed" + q + ")";
        if (call.args.size() > 1) {
            msgCpp = emitAsStrExpr(fn, call.args[1], scope);
        }
        return "cbAssertCond(" + condCpp + ", " + msgCpp + ", " + escapeFileName(fileName) + ", " + lineStr + ")";
    }

    std::string buildErrorBubbleReturn(
        const FnDecl& fn,
        std::string_view errVar
    ) const {
        if (fn.returnType == INVALID_ID || arena.isVoid(fn.returnType)) {
            return "return " + std::string(errVar) + ";";
        }
        const TypeNode tnode = resolveAliasNode(arena, symbols, moduleId, fn.returnType);
        std::string elem = "{}";
        if (tnode.name != "int" && tnode.name != "float" && tnode.name != "bool") {
            elem = defaultValueForType(moduleId, fn.returnType);
        }
        return "return { " + elem + ", " + std::string(errVar) + " };";
    }

    // Fused expression type-checker, call validator, and C++17 expression emitter.
    // allowTopLevelFallible is true ONLY at the root expression of a VarAssignment ('x = f()'),
    // ExprStmt ('f()'), or ReturnStmt ('return f()'). Any nested sub-expression recursive call passes
    // allowTopLevelFallible = false to enforce CleanBasic's statement-root error-bubbling invariant.
    std::string checkAndEmitExpr(
        const FnDecl& fn,
        ExprId exprId,
        const std::unordered_map<std::string, VarSymbol>& scope,
        bool allowTopLevelFallible,
        bool asLvalue = false,
        std::optional<FnSymbol>* outFallibleCallSym = nullptr
    ) {
        if (outFallibleCallSym != nullptr) {
            *outFallibleCallSym = std::nullopt;
        }
        if (exprId >= arena.exprs.size()) {
            return "";
        }

        inferExpr(exprId, scope);
        const auto& exprNode = arena.exprs[exprId];

        if (const auto* num = arena.getExpr<NumberLiteral>(exprId)) {
            return num->raw;
        }

        if (const auto* str = arena.getExpr<StringLiteral>(exprId)) {
            const std::string q(1, '"');
            return "std::string(" + q + str->value + q + ")";
        }

        if (const auto* b = arena.getExpr<BoolLiteral>(exprId)) {
            if (b->value) {
                return "true";
            }
            return "false";
        }

        if (const auto* ident = arena.getExpr<Identifier>(exprId)) {
            const auto cOpt = resolveConstSymbol(symbols, moduleId, scope, ident->name);
            if (cOpt.has_value()) {
                return cOpt->cppName;
            }
            return sanitizeCppIdent(ident->name);
        }

        if (const auto* uop = arena.getExpr<UnaryOp>(exprId)) {
            std::string opStr = uop->op;
            if (uop->op == "not") {
                opStr = "!";
            }
            return "(" + opStr + checkAndEmitExpr(fn, uop->operand, scope, false, false) + ")";
        }

        if (const auto* bop = arena.getExpr<BinaryOp>(exprId)) {
            const TypeId lt = inferExpr(bop->left, scope);
            const TypeId rt = inferExpr(bop->right, scope);
            const bool lEnum = isEnumType(symbols, lt, arena, moduleId);
            const bool rEnum = isEnumType(symbols, rt, arena, moduleId);
            if (bop->op == "&") {
                const std::string leftCpp = emitAsStrExpr(fn, bop->left, scope);
                const std::string rightCpp = emitAsStrExpr(fn, bop->right, scope);
                return "(" + leftCpp + " + " + rightCpp + ")";
            }
            const std::string leftCpp = checkAndEmitExpr(fn, bop->left, scope, false, false);
            const std::string rightCpp = checkAndEmitExpr(fn, bop->right, scope, false, false);
            if (lEnum || rEnum) {
                if (lt != rt && lt != symbols.typeAuto && rt != symbols.typeAuto) {
                    setAnalyzerError(exprNode.line, exprNode.col, "Cannot compare enum with mismatched type (use .toInt() for integer comparison).");
                }
            }
            std::string opStr = bop->op;
            if (opStr == "and") {
                opStr = "&&";
            } else if (opStr == "or") {
                opStr = "||";
            }
            return "(" + leftCpp + " " + opStr + " " + rightCpp + ")";
        }

        if (const auto* faccess = arena.getExpr<FieldAccess>(exprId)) {
            const auto enumOpt = resolveEnumMemberAccess(arena, symbols, moduleId, scope, *faccess);
            if (enumOpt.has_value()) {
                checkCrossModuleAccess(exprNode.line, exprNode.col, "Enum", *enumOpt);
                if (enumOpt->memberSet.count(faccess->fieldName) == 0) {
                    setAnalyzerError(exprNode.line, exprNode.col, "Enum '" + enumOpt->name + "' has no member '" + faccess->fieldName + "'.");
                }
                return enumOpt->cppName + "::" + sanitizeCppIdent(faccess->fieldName);
            }
            const auto cOpt = resolveModuleConstAccess(arena, symbols, moduleId, scope, *faccess);
            if (cOpt.has_value()) {
                checkCrossModuleAccess(exprNode.line, exprNode.col, "Constant", *cOpt);
                return cOpt->cppName;
            }
            const TypeId targetTid = inferExpr(faccess->target, scope);
            const auto tsOpt = resolveTypeSymbol(arena, symbols, moduleId, targetTid);
            if (tsOpt.has_value()) {
                if (tsOpt->fields.find(faccess->fieldName) == tsOpt->fields.end()) {
                    setAnalyzerError(exprNode.line, exprNode.col, "Type '" + tsOpt->name + "' has no field '" + faccess->fieldName + "'.");
                }
            }
            const std::string tgtCpp = checkAndEmitExpr(fn, faccess->target, scope, false, asLvalue);
            std::string emittedFieldName = faccess->fieldName;
            if (!tsOpt.has_value() || !tsOpt->isExtern) {
                emittedFieldName = sanitizeCppIdent(faccess->fieldName);
            }
            return tgtCpp + "." + emittedFieldName;
        }

        if (const auto* iaccess = arena.getExpr<IndexAccess>(exprId)) {
            const TypeId tgtTid = inferExpr(iaccess->target, scope);
            const TypeNode resolvedTgt = resolveAliasNode(arena, symbols, moduleId, tgtTid);
            const std::string tgtCpp = checkAndEmitExpr(fn, iaccess->target, scope, false, asLvalue);
            const std::string idxCpp = checkAndEmitExpr(fn, iaccess->index, scope, false, false);
            if (asLvalue) {
                return tgtCpp + "[" + idxCpp + "]";
            }
            if (resolvedTgt.name == "str") {
                return "cbStrIndex(" + tgtCpp + ", " + idxCpp + ")";
            }
            if (resolvedTgt.name == "list") {
                return "cbListGet(" + tgtCpp + ", " + idxCpp + ")";
            }
            if (resolvedTgt.name == "dict") {
                return "cbDictAt(" + tgtCpp + ", " + idxCpp + ")";
            }
            return tgtCpp + "[" + idxCpp + "]";
        }

        if (const auto* llit = arena.getExpr<ListLiteral>(exprId)) {
            return "{" + joinStrings(checkAndEmitExprList(fn, llit->elements, scope)) + "}";
        }

        if (const auto* dlit = arena.getExpr<DictLiteral>(exprId)) {
            std::vector<std::string> entries;
            entries.reserve(dlit->entries.size());
            for (const auto& ent : dlit->entries) {
                const std::string k = checkAndEmitExpr(fn, ent.key, scope, false, false);
                const std::string v = checkAndEmitExpr(fn, ent.value, scope, false, false);
                entries.push_back("{" + k + ", " + v + "}");
            }
            return "{" + joinStrings(entries) + "}";
        }

        if (const auto* call = arena.getExpr<FuncCall>(exprId)) {
            if (isAssertBuiltinCall(arena, *call)) {
                if (!allowTopLevelFallible) {
                    setAnalyzerError(exprNode.line, exprNode.col, "Fallible call to 'assert' must be assigned to a variable, returned directly, or called as a standalone statement.");
                }
                if (call->args.empty()) {
                    setAnalyzerError(exprNode.line, exprNode.col, "'assert(...)' requires at least a condition argument.");
                }
                if (call->args.size() > 2) {
                    setAnalyzerError(exprNode.line, exprNode.col, "'assert(...)' accepts at most 2 arguments (condition, [message]).");
                }
                const TypeId condTid = resolveAlias(arena, symbols, moduleId, inferExpr(call->args[0], scope));
                if (condTid != symbols.typeBool && condTid != symbols.typeAuto) {
                    setAnalyzerError(exprNode.line, exprNode.col, "First argument to 'assert(...)' must be of type 'bool'.");
                }
                if (call->args.size() == 2) {
                    const TypeId msgTid = resolveAlias(arena, symbols, moduleId, inferExpr(call->args[1], scope));
                    if (msgTid != symbols.typeStr && msgTid != symbols.typeAuto) {
                        setAnalyzerError(exprNode.line, exprNode.col, "Second argument to 'assert(...)' must be of type 'str'.");
                    }
                }
                return buildAssertCallValue(fn, *call, scope);
            }

            if (isErrorBuiltinCall(arena, *call)) {
                if (!fn.isFallible && catchErrStack.empty()) {
                    std::string msg = "Cannot call 'error(...)' inside infallible function '" + fn.name +
                        "'; add 'can error' to the function signature.";
                    setAnalyzerError(exprNode.line, exprNode.col, msg);
                }
                if (call->args.empty()) {
                    setAnalyzerError(exprNode.line, exprNode.col, "'error(...)' requires at least one message argument.");
                }
                if (call->args.size() > 2) {
                    setAnalyzerError(exprNode.line, exprNode.col, "'error(...)' accepts at most 2 arguments (message, [code]).");
                }
                if (call->args.size() == 2) {
                    const TypeId codeTid = resolveAlias(arena, symbols, moduleId, inferExpr(call->args[1], scope));
                    if (codeTid != symbols.typeInt && codeTid != symbols.typeAuto) {
                        setAnalyzerError(exprNode.line, exprNode.col, "Second argument to 'error(...)' (code) must be of type 'int'.");
                    }
                }
                return buildErrorCallValue(fn, *call, scope);
            }

            const auto fnSymOpt = resolveFuncCall(arena, *call, symbols, moduleId, scope);
            if (fnSymOpt.has_value()) {
                const auto& fnSym = fnSymOpt.value();
                recordCalleeEdge(fnSym);

                ExprId ufcsRecv = INVALID_ID;
                const bool isUfcs = isUfcsCallExpr(arena, *call, symbols, moduleId, scope, &ufcsRecv);

                if (isUfcs && !fnSym.params.empty() && fnSym.params[0].isByref) {
                    validateByrefArg(exprNode.line, exprNode.col, ufcsRecv, scope, fnSym, fnSym.params[0].name, true);
                }

                size_t paramOffset = 0;
                if (isUfcs) {
                    paramOffset = 1;
                }
                for (size_t aIdx = 0; aIdx < call->args.size(); ++aIdx) {
                    const size_t pIdx = paramOffset + aIdx;
                    if (pIdx < fnSym.params.size()) {
                        const TypeId expectedParamTid = resolveAlias(arena, symbols, moduleId, fnSym.params[pIdx].typeId);
                        const TypeId actualArgTid = resolveAlias(arena, symbols, moduleId, inferExpr(call->args[aIdx], scope));
                        if (fnSym.isBuiltinReceiver) {
                            if (expectedParamTid != symbols.typeAuto && actualArgTid != symbols.typeAuto && expectedParamTid != actualArgTid) {
                                setAnalyzerError(exprNode.line, exprNode.col, "Type mismatch for parameter '" + fnSym.params[pIdx].name + "' in call to '." + fnSym.name + "()'.");
                            }
                        }
                        if (fnSym.params[pIdx].isByref) {
                            validateByrefArg(exprNode.line, exprNode.col, call->args[aIdx], scope, fnSym, fnSym.params[pIdx].name);
                        }
                    }
                }

                const bool isBuiltinPrint = (fnSym.name == "print" && fnSym.moduleId.empty());
                if (!isBuiltinPrint) {
                    const size_t totalProvided = paramOffset + call->args.size();
                    if (totalProvided > fnSym.params.size()) {
                        setAnalyzerError(exprNode.line, exprNode.col, "Too many arguments in call to '" + fnSym.name + "'.");
                    }
                    if (totalProvided < fnSym.params.size()) {
                        const auto& p = fnSym.params[totalProvided];
                        setAnalyzerError(exprNode.line, exprNode.col, "Missing required argument for parameter '" + p.name + "' in call to '" + fnSym.name + "'.");
                    }
                }

                if (!fnSym.moduleId.empty() && fnSym.moduleId != moduleId) {
                    symbols.referencedModules.insert(fnSym.moduleId);
                }

                if (fnSym.isFallible) {
                    if (!allowTopLevelFallible) {
                        std::string msg = "Fallible call to '" + fnSym.name +
                            "' must be assigned to a variable, returned directly, or called as a standalone statement.";
                        setAnalyzerError(exprNode.line, exprNode.col, msg);
                    }
                    if (outFallibleCallSym != nullptr) {
                        *outFallibleCallSym = fnSym;
                    }
                }

                if (const auto* calleeFa = arena.getExpr<FieldAccess>(call->callee)) {
                    if (const auto* tgtIdent = arena.getExpr<Identifier>(calleeFa->target)) {
                        std::string targetModId;
                        if (isModuleAliasInScope(symbols, moduleId, tgtIdent->name, scope, targetModId)) {
                            checkCrossModuleAccess(exprNode.line, exprNode.col, "Function", fnSym, calleeFa->fieldName);
                        }
                    }
                }

                if (fnSym.isBuiltinReceiver) {
                    const auto* access = arena.getExpr<FieldAccess>(call->callee);
                    const bool recvMutated = (!fnSym.params.empty() && fnSym.params[0].isByref);
                    const std::string recvCpp = checkAndEmitExpr(fn, access->target, scope, false, recvMutated);
                    const std::vector<std::string> argCpps = checkAndEmitExprList(fn, call->args, scope);
                    if (fnSym.name == "len") {
                        return "static_cast<int64_t>(" + recvCpp + ".size())";
                    } else if (fnSym.name == "push") {
                        return recvCpp + ".push_back(" + argCpps[0] + ")";
                    } else if (fnSym.name == "pop") {
                        return "cbListPop(" + recvCpp + ")";
                    } else if (fnSym.name == "clear") {
                        return recvCpp + ".clear()";
                    } else if (fnSym.name == "get") {
                        return "cbDictGet(" + recvCpp + ", " + argCpps[0] + ", " + argCpps[1] + ")";
                    } else if (fnSym.name == "has") {
                        return "(" + recvCpp + ".count(" + argCpps[0] + ") > 0)";
                    } else if (fnSym.name == "remove") {
                        return "(" + recvCpp + ".erase(" + argCpps[0] + ") > 0)";
                    } else if (fnSym.name == "toStr") {
                        return "cbToStr(" + recvCpp + ")";
                    } else if (fnSym.name == "toInt") {
                        return "static_cast<int64_t>(" + recvCpp + ")";
                    } else if (fnSym.name == "toFloat") {
                        return "static_cast<double>(" + recvCpp + ")";
                    } else if (fnSym.name == "slice") {
                        return "cbListSlice(" + recvCpp + ", " + argCpps[0] + ", " + argCpps[1] + ")";
                    } else if (fnSym.name == "sliceFrom") {
                        return "cbListSliceFrom(" + recvCpp + ", " + argCpps[0] + ")";
                    } else if (fnSym.name == "contains") {
                        return "cbListContains(" + recvCpp + ", " + argCpps[0] + ")";
                    } else if (fnSym.name == "sort") {
                        return "cbListSort(" + recvCpp + ")";
                    } else if (fnSym.name == "removeAt") {
                        return "cbListRemoveAt(" + recvCpp + ", " + argCpps[0] + ")";
                    } else if (fnSym.name == "keys") {
                        return "cbDictKeys(" + recvCpp + ")";
                    } else if (fnSym.name == "kind") {
                        return "cbUnionKind(" + recvCpp + ")";
                    } else if (fnSym.name == "byteAt") {
                        return "cbStrByteAtFast(" + recvCpp + ", " + argCpps[0] + ")";
                    }
                    return "";
                }

                if (isBuiltinPrint) {
                    if (call->args.empty()) {
                        return "(std::cout << std::endl)";
                    }
                    std::string streamExpr = "(std::cout";
                    for (const ExprId argId : call->args) {
                        const TypeId argTid = inferExpr(argId, scope);
                        std::string argCpp = checkAndEmitExpr(fn, argId, scope, false, false);
                        if (argTid < arena.types.size() &&
                            (arena.types[argTid].name == "bool" || arena.types[argTid].name == "err")) {
                            argCpp = "cbToStr(" + argCpp + ")";
                        }
                        streamExpr += " << " + argCpp;
                    }
                    streamExpr += " << std::endl)";
                    return streamExpr;
                }

                std::vector<ExprId> allPos;
                const std::vector<ExprId>* posArgsPtr = &call->args;
                if (isUfcs) {
                    allPos.reserve(1 + call->args.size());
                    allPos.push_back(ufcsRecv);
                    for (const ExprId a : call->args) {
                        allPos.push_back(a);
                    }
                    posArgsPtr = &allPos;
                }
                std::vector<std::string> rendered;
                rendered.reserve(posArgsPtr->size());
                for (size_t i = 0; i < posArgsPtr->size(); ++i) {
                    bool isByrefParam = false;
                    if (i < fnSym.params.size() && fnSym.params[i].isByref) {
                        isByrefParam = true;
                    }
                    rendered.push_back(checkAndEmitExpr(fn, (*posArgsPtr)[i], scope, false, isByrefParam));
                }
                return fnSym.cppName + "(" + joinStrings(rendered) + ")";
            }

            const auto structOpt = resolveStructConstructor(arena, *call, symbols, moduleId, scope);
            if (structOpt.has_value()) {
                checkCrossModuleAccess(exprNode.line, exprNode.col, "Type", *structOpt);
                if (structOpt->isExtern && structOpt->fieldOrder.empty()) {
                    setAnalyzerError(exprNode.line, exprNode.col, "Cannot directly construct opaque extern type '" + structOpt->name + "'.");
                }
                if (!call->args.empty() && call->args.size() != structOpt->fieldOrder.size()) {
                    setAnalyzerError(exprNode.line, exprNode.col, "Constructor for '" + structOpt->name + "' expects 0 or " + std::to_string(structOpt->fieldOrder.size()) + " positional arguments (got " + std::to_string(call->args.size()) + ").");
                }
                return structOpt->cppName + "{" + joinStrings(checkAndEmitExprList(fn, call->args, scope)) + "}";
            }

            if (const auto* fa = arena.getExpr<FieldAccess>(call->callee)) {
                const auto uOpt = resolveUnionVariantAccess(arena, symbols, moduleId, scope, *fa);
                if (uOpt.has_value()) {
                    checkCrossModuleAccess(exprNode.line, exprNode.col, "Union", *uOpt);
                    const auto vTypeIt = uOpt->variantTypes.find(fa->fieldName);
                    if (vTypeIt == uOpt->variantTypes.end()) {
                        setAnalyzerError(exprNode.line, exprNode.col, "Union '" + uOpt->name + "' has no variant '" + fa->fieldName + "'.");
                    }
                    if (call->args.size() != 1) {
                        setAnalyzerError(exprNode.line, exprNode.col, "Union variant constructor '" + uOpt->name + "." + fa->fieldName + "(...)' requires exactly 1 positional argument.");
                    }
                    const TypeId expectedPayloadTid = resolveAlias(arena, symbols, uOpt->moduleId, vTypeIt->second);
                    const TypeId actualArgTid = resolveAlias(arena, symbols, moduleId, inferExpr(call->args[0], scope));
                    if (expectedPayloadTid != symbols.typeAuto && actualArgTid != symbols.typeAuto && expectedPayloadTid != actualArgTid) {
                        setAnalyzerError(exprNode.line, exprNode.col, "Argument type does not match payload type for union variant '" + uOpt->name + "." + fa->fieldName + "'.");
                    }
                    const std::string argCpp = checkAndEmitExpr(fn, call->args[0], scope, false, false);
                    const size_t varIndex = uOpt->variantIndices.at(fa->fieldName);
                    const std::string varDataCpp = unionDataVariantCppType(*uOpt);
                    return uOpt->cppName + "{" + varDataCpp + "(std::in_place_index<" + std::to_string(varIndex) + ">, " + argCpp + ")}";
                }
            }

            const std::string calleeCpp = checkAndEmitExpr(fn, call->callee, scope, false, false);
            return calleeCpp + "(" + joinStrings(checkAndEmitExprList(fn, call->args, scope)) + ")";
        }

        return "";
    }

    void bindLocalVar(
        std::unordered_map<std::string, VarSymbol>& scope,
        const std::string& name,
        TypeId tid,
        bool isByref,
        bool isConst,
        uint32_t line,
        uint32_t col
    ) {
        VarSymbol sym{name, tid, isByref, isConst, fileName, line, col};
        scope[name] = sym;
        if (!currentFnKey.empty()) {
            auto& fnScope = symbols.fnScopes[currentFnKey];
            if (fnScope.find(name) == fnScope.end()) {
                fnScope.emplace(name, std::move(sym));
            }
        }
    }

    void validateEnumAssignment(
        uint32_t line,
        uint32_t col,
        std::string_view varName,
        TypeId targetTid,
        TypeId valTid
    ) const {
        const TypeId resolvedTarget = resolveAlias(arena, symbols, moduleId, targetTid);
        const TypeId resolvedVal = resolveAlias(arena, symbols, moduleId, valTid);
        const bool varIsEnum = isEnumType(symbols, resolvedTarget, arena, moduleId);
        const bool valIsEnum = isEnumType(symbols, resolvedVal, arena, moduleId);
        if (varIsEnum && resolvedTarget != resolvedVal && resolvedVal != symbols.typeAuto) {
            setAnalyzerError(line, col, "Cannot assign mismatched type to enum variable '" + std::string(varName) + "'.");
        }
        if (!varIsEnum && valIsEnum && resolvedTarget == symbols.typeInt) {
            setAnalyzerError(line, col, "Cannot assign enum value to 'int' without explicit conversion (use .toInt()).");
        }
    }

    void validateEmptyContainerInference(
        uint32_t line,
        uint32_t col,
        std::string_view declKeyword,
        std::string_view varName,
        bool hasDeclaredType,
        ExprId valExprId
    ) const {
        if (hasDeclaredType) return;
        if (const auto* llit = arena.getExpr<ListLiteral>(valExprId)) {
            if (llit->elements.empty()) {
                setAnalyzerError(line, col, "Cannot infer element type of empty list '[]'; specify an explicit type such as '" + std::string(declKeyword) + " " + std::string(varName) + " as list[int] = []'.");
            }
        }
        if (const auto* dlit = arena.getExpr<DictLiteral>(valExprId)) {
            if (dlit->entries.empty()) {
                setAnalyzerError(line, col, "Cannot infer key/value types of empty dict '{}'; specify an explicit type such as '" + std::string(declKeyword) + " " + std::string(varName) + " as dict[str, int] = {}'.");
            }
        }
    }

    std::string checkAndEmitConstDecl(DeclId did, const DeclNode& declNode) {
        const auto* cdecl = arena.getDecl<ConstDecl>(did);
        if (cdecl == nullptr) {
            return "";
        }
        std::unordered_map<std::string, VarSymbol> emptyScope;
        const std::string key = symbolKey(declNode.moduleId, cdecl->name);
        const TypeId valTid = inferExpr(cdecl->value, emptyScope);
        TypeId targetTid = valTid;
        std::string cppName = makeCppConstName(declNode.moduleId, cdecl->name);
        if (symbols.constants.count(key) > 0) {
            targetTid = symbols.constants.at(key).typeId;
            cppName = symbols.constants.at(key).cppName;
        }
        validateEnumAssignment(declNode.line, declNode.col, cdecl->name, targetTid, valTid);
        FnDecl dummyFn{};
        dummyFn.name = cdecl->name;
        dummyFn.isFallible = false;
        currentFnKey = "__top__" + declNode.moduleId;
        const std::string valCpp = checkAndEmitExpr(dummyFn, cdecl->value, emptyScope, false, false);
        currentFnKey.clear();

        std::string cppType = "auto";
        if (targetTid != INVALID_ID && targetTid != symbols.typeAuto) {
            cppType = mapType(declNode.moduleId, targetTid);
        }
        return "inline const " + cppType + " " + cppName + " = " + valCpp + ";";
    }

    void emitErrorCheck(uint32_t line, const FnDecl& fn, const std::string& errTmp) {
        if (!catchErrStack.empty()) {
            const std::string& catchVar = catchErrStack.back();
            emitLine("if (cbHasError(" + errTmp + ")) {");
            indentLevel += 1;
            emitLine("cbStampError(" + errTmp + ", " + escapeFileName(fileName) + ", " + std::to_string(line) + ");");
            emitLine(catchVar + " = " + errTmp + ";");
            emitLine("break;");
            indentLevel -= 1;
            emitLine("}");
        } else {
            const std::string earlyRet = buildErrorBubbleReturn(fn, errTmp);
            emitLine("if (cbHasError(" + errTmp + ")) {");
            indentLevel += 1;
            emitLine("cbStampError(" + errTmp + ", " + escapeFileName(fileName) + ", " + std::to_string(line) + ");");
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

    std::string emitFallibleCallUnpack(
        uint32_t line,
        const FnDecl& fn,
        const std::string& callCpp,
        bool hasReturnValue
    ) {
        const auto [valTmp, errVar] = nextValErrTemps();
        if (hasReturnValue) {
            emitLine("auto [" + valTmp + ", " + errVar + "] = " + callCpp + ";");
        } else {
            emitLine("auto " + errVar + " = " + callCpp + ";");
        }
        emitErrorCheck(line, fn, errVar);
        return valTmp;
    }

    bool emitIntrinsicFallibleOrErrorCall(
        uint32_t line,
        const FnDecl& fn,
        ExprId exprId,
        const FuncCall& call,
        std::unordered_map<std::string, VarSymbol>& scope
    ) {
        if (isAssertBuiltinCall(arena, call)) {
            const std::string assertVal = checkAndEmitExpr(fn, exprId, scope, true, false);
            const auto [discardTmp, errVar] = nextValErrTemps();
            emitLine("auto " + errVar + " = " + assertVal + ";");
            emitErrorCheck(line, fn, errVar);
            return true;
        }
        if (isErrorBuiltinCall(arena, call)) {
            const std::string errVal = checkAndEmitExpr(fn, exprId, scope, true, false);
            if (!catchErrStack.empty()) {
                const auto [discardTmp, errVar] = nextValErrTemps();
                emitLine("auto " + errVar + " = " + errVal + ";");
                emitErrorCheck(line, fn, errVar);
            } else {
                emitLine(buildErrorBubbleReturn(fn, errVal));
            }
            return true;
        }
        return false;
    }

    void checkAndEmitBlock(
        const FnDecl& fn,
        const std::vector<StmtId>& stmts,
        std::unordered_map<std::string, VarSymbol> scope
    ) {
        for (const StmtId sid : stmts) {
            checkAndEmitStmt(fn, sid, scope);
        }
    }

    void checkAndEmitStmt(
        const FnDecl& fn,
        StmtId stmtId,
        std::unordered_map<std::string, VarSymbol>& scope
    ) {
        if (stmtId >= arena.stmts.size()) {
            return;
        }

        const auto& stmtNode = arena.stmts[stmtId];

        if (const auto* vassign = arena.getStmt<VarAssignment>(stmtId)) {
            checkFallibleUsage(fn, vassign->value, scope);

            const TypeId valType = inferExpr(vassign->value, scope);
            const Identifier* ident = arena.getExpr<Identifier>(vassign->target);
            TypeId boundTid = symbols.typeAuto;
            std::string lhsCpp;

            if (ident != nullptr) {
                const auto& tgtNode = arena.exprs[vassign->target];
                if (vassign->isDim || vassign->isConst) {
                    std::string_view declKw = "dim";
                    if (vassign->isConst) {
                        declKw = "const";
                    }
                    validateEmptyContainerInference(stmtNode.line, stmtNode.col, declKw, ident->name, vassign->hasDeclaredType, vassign->value);
                    TypeId varTid = symbols.typeInt;
                    if (vassign->hasDeclaredType) {
                        varTid = resolveAlias(arena, symbols, moduleId, vassign->declaredType);
                    } else if (valType != INVALID_ID) {
                        varTid = valType;
                    }
                    validateEnumAssignment(tgtNode.line, tgtNode.col, ident->name, varTid, valType);
                    boundTid = varTid;
                } else {
                    const auto existing = scope.find(ident->name);
                    if (existing == scope.end()) {
                        const auto cOpt = resolveConstSymbol(symbols, moduleId, scope, ident->name);
                        if (cOpt.has_value()) {
                            setAnalyzerError(tgtNode.line, tgtNode.col, "Cannot reassign const variable '" + ident->name + "'.");
                        }
                        std::string msg = "Undeclared variable '" + ident->name + "'. Use 'dim' or 'const' to declare variables.";
                        setAnalyzerError(tgtNode.line, tgtNode.col, msg);
                    }
                    if (existing->second.isConst) {
                        std::string msg = "Cannot reassign const variable '" + ident->name + "'.";
                        setAnalyzerError(tgtNode.line, tgtNode.col, msg);
                    }
                    validateEnumAssignment(tgtNode.line, tgtNode.col, ident->name, existing->second.typeId, valType);
                }
            } else if (vassign->target != INVALID_ID) {
                const auto& tgtNode = arena.exprs[vassign->target];
                if (const auto* iaccess = arena.getExpr<IndexAccess>(vassign->target)) {
                    const TypeId idxTgtTid = inferExpr(iaccess->target, scope);
                    if (idxTgtTid < arena.types.size()) {
                        const TypeNode idxTgtNode = resolveAliasNode(arena, symbols, moduleId, idxTgtTid);
                        if (idxTgtNode.name == "str") {
                            setAnalyzerError(tgtNode.line, tgtNode.col, "Cannot mutate string index in place; strings are immutable by index in CleanBasic.");
                        }
                    }
                }
                const auto root = findRootVar(vassign->target, scope);
                if (root.found && root.isConst) {
                    std::string msg = "Cannot mutate const variable '" + root.name + "'.";
                    setAnalyzerError(tgtNode.line, tgtNode.col, msg);
                }
                const TypeId lhsTid = inferExpr(vassign->target, scope);
                std::string targetLabel = root.name;
                if (const auto* fa = arena.getExpr<FieldAccess>(vassign->target)) {
                    targetLabel = fa->fieldName;
                }
                validateEnumAssignment(tgtNode.line, tgtNode.col, targetLabel, lhsTid, valType);
                lhsCpp = checkAndEmitExpr(fn, vassign->target, scope, false, true);
            }

            std::optional<FnSymbol> fallibleSym;
            const std::string rawValCpp = checkAndEmitExpr(fn, vassign->value, scope, true, false, &fallibleSym);
            std::string valCpp = rawValCpp;
            bool isFallibleCall = false;
            if (fallibleSym.has_value()) {
                isFallibleCall = true;
                const bool hasValue = (fallibleSym->returnType != INVALID_ID && !arena.isVoid(fallibleSym->returnType));
                const std::string valTmp = emitFallibleCallUnpack(stmtNode.line, fn, rawValCpp, hasValue);
                valCpp = "0";
                if (hasValue) {
                    valCpp = valTmp;
                }
            }

            if (vassign->target == INVALID_ID) {
                return;
            }

            if (ident != nullptr) {
                const auto& tgtNode = arena.exprs[vassign->target];
                const std::string& name = ident->name;
                const std::string cppVarName = sanitizeCppIdent(name);
                if (vassign->isDim || vassign->isConst) {
                    bindLocalVar(scope, name, boundTid, false, vassign->isConst, tgtNode.line, tgtNode.col);
                    std::string cppT = "auto";
                    if (!isFallibleCall) {
                        if (vassign->hasDeclaredType) {
                            cppT = mapTypeNode(moduleId, arena.getType(boundTid));
                        } else if (boundTid < arena.types.size() && arena.types[boundTid].name != "auto") {
                            cppT = mapType(moduleId, boundTid);
                        }
                    }
                    std::string prefix;
                    if (vassign->isConst) {
                        prefix = "const ";
                    }
                    emitLine(prefix + cppT + " " + cppVarName + " = " + valCpp + ";");
                } else {
                    emitLine(cppVarName + " = " + valCpp + ";");
                }
            } else {
                emitLine(lhsCpp + " = " + valCpp + ";");
            }
            return;
        }

        if (const auto* estmt = arena.getStmt<ExprStmt>(stmtId)) {
            checkFallibleUsage(fn, estmt->expr, scope);
            if (const auto* call = arena.getExpr<FuncCall>(estmt->expr)) {
                if (emitIntrinsicFallibleOrErrorCall(stmtNode.line, fn, estmt->expr, *call, scope)) {
                    return;
                }
            }
            std::optional<FnSymbol> fallibleSym;
            const std::string exprCpp = checkAndEmitExpr(fn, estmt->expr, scope, true, false, &fallibleSym);
            if (fallibleSym.has_value()) {
                const bool hasValue = (fallibleSym->returnType != INVALID_ID && !arena.isVoid(fallibleSym->returnType));
                emitFallibleCallUnpack(stmtNode.line, fn, exprCpp, hasValue);
                return;
            }
            emitLine(exprCpp + ";");
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
            checkAndEmitBlock(fn, tryCatch->tryBody, scope);
            catchErrStack.pop_back();
            indentLevel -= 1;
            emitLine("} while(false);");
            emitLine("if (cbHasError(" + catchErr + ")) {");
            indentLevel += 1;
            emitLine("CbError " + sanitizeCppIdent(tryCatch->catchVarName) + " = " + catchErr + ";");
            auto catchScope = scope;
            bindLocalVar(catchScope, tryCatch->catchVarName, symbols.typeErr, false, false, stmtNode.line, stmtNode.col);
            checkAndEmitBlock(fn, tryCatch->catchBody, catchScope);
            indentLevel -= 1;
            emitLine("}");
            indentLevel -= 1;
            emitLine("}");
            return;
        }

        if (const auto* ifStmt = arena.getStmt<IfStmt>(stmtId)) {
            const std::string condCpp = checkAndEmitExpr(fn, ifStmt->condition, scope, false, false);
            emitLine("if (" + condCpp + ") {");
            indentLevel += 1;
            checkAndEmitBlock(fn, ifStmt->thenBody, scope);
            indentLevel -= 1;

            for (const auto& branch : ifStmt->elifBranches) {
                const std::string elifCond = checkAndEmitExpr(fn, branch.condition, scope, false, false);
                emitLine("} else if (" + elifCond + ") {");
                indentLevel += 1;
                checkAndEmitBlock(fn, branch.body, scope);
                indentLevel -= 1;
            }

            if (ifStmt->hasElse) {
                emitLine("} else {");
                indentLevel += 1;
                checkAndEmitBlock(fn, ifStmt->elseBody, scope);
                indentLevel -= 1;
            }

            emitLine("}");
            emitCatchBreakIfPending();
            return;
        }

        if (const auto* forStmt = arena.getStmt<ForStmt>(stmtId)) {
            loopDepth += 1;
            auto loopScope = scope;
            if (forStmt->rangeSpec.hasRange) {
                const std::string startCpp = checkAndEmitExpr(fn, forStmt->rangeSpec.start, scope, false, false);
                const std::string endCpp = checkAndEmitExpr(fn, forStmt->rangeSpec.end, scope, false, false);
                if (!forStmt->loopVars.empty()) {
                    const std::string& loopVar = forStmt->loopVars[0];
                    const std::string cppLoopVar = sanitizeCppIdent(loopVar);
                    bindLocalVar(loopScope, loopVar, symbols.typeInt, false, false, stmtNode.line, stmtNode.col);
                    emitLine("for (int64_t " + cppLoopVar + " = " + startCpp + "; " + cppLoopVar + " <= " + endCpp + "; " + cppLoopVar + " += 1) {");
                    indentLevel += 1;
                    checkAndEmitBlock(fn, forStmt->body, loopScope);
                    indentLevel -= 1;
                    emitLine("}");
                    emitCatchBreakIfPending();
                }
            } else if (forStmt->iterable != INVALID_ID) {
                const std::string iterCpp = checkAndEmitExpr(fn, forStmt->iterable, scope, false, false);
                const TypeId iterTid = inferExpr(forStmt->iterable, scope);
                const TypeNode iterType = resolveAliasNode(arena, symbols, moduleId, iterTid);
                std::string loopErr;
                const auto bindings = resolveForInLoopVars(arena, symbols, moduleId, *forStmt, scope, &loopErr);
                if (!loopErr.empty()) {
                    setAnalyzerError(stmtNode.line, stmtNode.col, loopErr);
                }
                for (const auto& kv : bindings) {
                    bindLocalVar(loopScope, kv.first, kv.second, false, true, stmtNode.line, stmtNode.col);
                }
                if (forStmt->loopVars.size() == 1) {
                    const std::string& lv = forStmt->loopVars[0];
                    if (iterType.name == "str") {
                        emitLine("for (const auto& " + lv + " : cbStrUtf8Chars(" + iterCpp + ")) {");
                    } else {
                        emitLine("for (const auto& " + lv + " : " + iterCpp + ") {");
                    }
                } else {
                    emitLine("for (const auto& [" + joinStrings(forStmt->loopVars) + "] : " + iterCpp + ") {");
                }
                indentLevel += 1;
                checkAndEmitBlock(fn, forStmt->body, loopScope);
                indentLevel -= 1;
                emitLine("}");
                emitCatchBreakIfPending();
            }
            loopDepth -= 1;
            return;
        }

        if (const auto* whileStmt = arena.getStmt<WhileStmt>(stmtId)) {
            loopDepth += 1;
            const std::string condCpp = checkAndEmitExpr(fn, whileStmt->condition, scope, false, false);
            emitLine("while (" + condCpp + ") {");
            indentLevel += 1;
            checkAndEmitBlock(fn, whileStmt->body, scope);
            indentLevel -= 1;
            emitLine("}");
            emitCatchBreakIfPending();
            loopDepth -= 1;
            return;
        }

        if (const auto* selStmt = arena.getStmt<SelectStmt>(stmtId)) {
            const std::string targetCpp = checkAndEmitExpr(fn, selStmt->target, scope, false, false);
            const TypeId targetTid = inferExpr(selStmt->target, scope);
            const bool isEnumTarget = isEnumType(symbols, targetTid, arena, moduleId);
            const auto unionOpt = resolveUnionSymbol(arena, symbols, moduleId, targetTid);
            const bool isUnionTarget = unionOpt.has_value();
            if (targetTid != symbols.typeInt && targetTid != symbols.typeBool && targetTid != symbols.typeStr && !isEnumTarget && !isUnionTarget) {
                setAnalyzerError(stmtNode.line, stmtNode.col, "select case expression must be of type 'int', 'bool', or 'str' (or enum or union)");
            }

            if (isUnionTarget) {
                emitLine("{");
                indentLevel += 1;
                const std::string selVar = nextTemp("_sel");
                emitLine("const auto& " + selVar + " = " + targetCpp + ";");
                emitLine("switch (static_cast<int64_t>(" + selVar + "._data.index())) {");
                indentLevel += 1;
                for (const auto& scase : selStmt->cases) {
                    size_t lastVarIndex = 0;
                    TypeId boundPayloadTid = INVALID_ID;
                    for (size_t mIdx = 0; mIdx < scase.matchExprs.size(); ++mIdx) {
                        const ExprId mExpr = scase.matchExprs[mIdx];
                        if (const auto* fa = arena.getExpr<FieldAccess>(mExpr)) {
                            const auto uAccOpt = resolveUnionVariantAccess(arena, symbols, moduleId, scope, *fa);
                            if (!uAccOpt.has_value() || uAccOpt->key != unionOpt->key) {
                                setAnalyzerError(scase.line, scase.col, "case variant does not match target union '" + unionOpt->name + "'");
                            }
                        } else if (arena.getExpr<Identifier>(mExpr) == nullptr) {
                            setAnalyzerError(scase.line, scase.col, "Expected variant name in 'select case' on union '" + unionOpt->name + "'");
                        }
                        const auto varOpt = resolveUnionSelectCaseVariant(arena, *unionOpt, mExpr);
                        if (!varOpt.has_value()) {
                            std::string varName;
                            if (const auto* ident = arena.getExpr<Identifier>(mExpr)) {
                                varName = ident->name;
                            } else if (const auto* fa = arena.getExpr<FieldAccess>(mExpr)) {
                                varName = fa->fieldName;
                            }
                            setAnalyzerError(scase.line, scase.col, "Union '" + unionOpt->name + "' has no variant '" + varName + "'");
                        }
                        lastVarIndex = varOpt->variantIndex;
                        boundPayloadTid = varOpt->payloadType;
                        if (mIdx + 1 == scase.matchExprs.size()) {
                            emitLine("case " + std::to_string(lastVarIndex) + ": {");
                        } else {
                            emitLine("case " + std::to_string(lastVarIndex) + ":");
                        }
                    }
                    indentLevel += 1;
                    auto caseScope = scope;
                    if (!scase.bindingName.empty() && boundPayloadTid != INVALID_ID) {
                        bindLocalVar(caseScope, scase.bindingName, boundPayloadTid, false, true, scase.line, scase.col);
                        emitLine("const auto& " + sanitizeCppIdent(scase.bindingName) + " = std::get<" + std::to_string(lastVarIndex) + ">(" + selVar + "._data);");
                    }
                    checkAndEmitBlock(fn, scase.body, caseScope);
                    emitLine("break;");
                    indentLevel -= 1;
                    emitLine("}");
                }
                if (selStmt->hasCaseElse) {
                    emitLine("default: {");
                    indentLevel += 1;
                    checkAndEmitBlock(fn, selStmt->caseElse, scope);
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
                for (const auto& scase : selStmt->cases) {
                    if (!scase.bindingName.empty()) {
                        setAnalyzerError(scase.line, scase.col, "Case payload binding ('as <ident>') is only supported when 'select case' target is a union");
                    }
                    std::string cond;
                    for (size_t mIdx = 0; mIdx < scase.matchExprs.size(); ++mIdx) {
                        const ExprId mExpr = scase.matchExprs[mIdx];
                        const std::string matchCpp = checkAndEmitExpr(fn, mExpr, scope, false, false);
                        const TypeId mExprTid = inferExpr(mExpr, scope);
                        if (mExprTid != targetTid && mExprTid != symbols.typeAuto) {
                            setAnalyzerError(scase.line, scase.col, "case expression type does not match select case target type");
                        }
                        if (mIdx > 0) {
                            cond += " || ";
                        }
                        cond += selVar + " == " + matchCpp;
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
                    checkAndEmitBlock(fn, scase.body, scope);
                    indentLevel -= 1;
                }
                if (selStmt->hasCaseElse) {
                    if (isFirst) {
                        emitLine("{");
                        indentLevel += 1;
                        checkAndEmitBlock(fn, selStmt->caseElse, scope);
                        indentLevel -= 1;
                        emitLine("}");
                    } else {
                        emitLine("} else {");
                        indentLevel += 1;
                        checkAndEmitBlock(fn, selStmt->caseElse, scope);
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
            for (const auto& scase : selStmt->cases) {
                if (!scase.bindingName.empty()) {
                    setAnalyzerError(scase.line, scase.col, "Case payload binding ('as <ident>') is only supported when 'select case' target is a union");
                }
                for (size_t mIdx = 0; mIdx < scase.matchExprs.size(); ++mIdx) {
                    const ExprId mExpr = scase.matchExprs[mIdx];
                    const std::string matchCpp = checkAndEmitExpr(fn, mExpr, scope, false, false);
                    const TypeId mExprTid = inferExpr(mExpr, scope);
                    if (mExprTid != targetTid && mExprTid != symbols.typeAuto) {
                        setAnalyzerError(scase.line, scase.col, "case expression type does not match select case target type");
                    }
                    if (mIdx + 1 == scase.matchExprs.size()) {
                        emitLine("case " + matchCpp + ": {");
                    } else {
                        emitLine("case " + matchCpp + ":");
                    }
                }
                indentLevel += 1;
                checkAndEmitBlock(fn, scase.body, scope);
                emitLine("break;");
                indentLevel -= 1;
                emitLine("}");
            }
            if (selStmt->hasCaseElse) {
                emitLine("default: {");
                indentLevel += 1;
                checkAndEmitBlock(fn, selStmt->caseElse, scope);
                emitLine("break;");
                indentLevel -= 1;
                emitLine("}");
            }
            indentLevel -= 1;
            emitLine("}");
            emitCatchBreakIfPending();
            return;
        }

        if (const auto* retStmt = arena.getStmt<ReturnStmt>(stmtId)) {
            const bool expectedHasValue = (fn.returnType != INVALID_ID && !arena.isVoid(fn.returnType));
            if (!retStmt->hasValue) {
                if (expectedHasValue) {
                    std::string msg = "Function '" + fn.name + "' expects a return value, but none was provided.";
                    setAnalyzerError(stmtNode.line, stmtNode.col, msg);
                }
                if (fn.isFallible && (fn.returnType == INVALID_ID || arena.isVoid(fn.returnType))) {
                    emitLine(R"(return CbError{"", 0};)");
                } else {
                    emitLine("return;");
                }
                return;
            }

            if (const auto* call = arena.getExpr<FuncCall>(retStmt->value)) {
                if (emitIntrinsicFallibleOrErrorCall(stmtNode.line, fn, retStmt->value, *call, scope)) {
                    return;
                }
            }

            checkFallibleUsage(fn, retStmt->value, scope);
            std::optional<FnSymbol> fallibleSym;
            std::string valCpp = checkAndEmitExpr(fn, retStmt->value, scope, true, false, &fallibleSym);

            if (fallibleSym.has_value()) {
                const bool calleeHasValue = (fallibleSym->returnType != INVALID_ID && !arena.isVoid(fallibleSym->returnType));
                if (!calleeHasValue && expectedHasValue) {
                    std::string msg = "Function '" + fn.name + "' expects a return value, but none was provided.";
                    setAnalyzerError(stmtNode.line, stmtNode.col, msg);
                } else if (calleeHasValue && !expectedHasValue) {
                    std::string msg = "Function '" + fn.name + "' returns void, but a value was returned.";
                    setAnalyzerError(stmtNode.line, stmtNode.col, msg);
                }
                const std::string valTmp = emitFallibleCallUnpack(stmtNode.line, fn, valCpp, calleeHasValue);
                if (!calleeHasValue) {
                    if (fn.isFallible) {
                        emitLine(R"(return CbError{"", 0};)");
                    } else {
                        emitLine("return;");
                    }
                } else {
                    if (fn.isFallible) {
                        emitLine("return { " + valTmp + R"(, CbError{"", 0} };)");
                    } else {
                        emitLine("return " + valTmp + ";");
                    }
                }
                return;
            }

            if (!expectedHasValue) {
                std::string msg = "Function '" + fn.name + "' returns void, but a value was returned.";
                setAnalyzerError(stmtNode.line, stmtNode.col, msg);
            }
            if (fn.isFallible) {
                if (valCpp == "{}") {
                    valCpp = defaultValueForType(moduleId, fn.returnType);
                }
                emitLine("return { " + valCpp + R"(, CbError{"", 0} };)");
            } else {
                emitLine("return " + valCpp + ";");
            }
            return;
        }

        if (arena.getStmt<BreakStmt>(stmtId) != nullptr) {
            if (loopDepth <= 0) {
                setAnalyzerError(stmtNode.line, stmtNode.col, "'break' cannot be used outside of a loop");
            }
            emitLine("break;");
            return;
        }

        if (arena.getStmt<ContinueStmt>(stmtId) != nullptr) {
            if (loopDepth <= 0) {
                setAnalyzerError(stmtNode.line, stmtNode.col, "'continue' cannot be used outside of a loop");
            }
            emitLine("continue;");
            return;
        }
    }

    std::vector<std::string> checkAndEmitFn(const FnDecl& fn) {
        loopDepth = 0;
        catchErrStack.clear();
        lines.clear();
        indentLevel = 0;
        currentFnKey = symbolKey(moduleId, fn.name);

        emitLine(fnSignature(moduleId, fn) + " {");
        indentLevel += 1;

        std::unordered_map<std::string, VarSymbol> scope;
        for (const auto& param : fn.params) {
            const TypeId resolvedTid = canonicalizeType(arena, symbols, moduleId, param.typeId);
            const bool isConst = !param.isByref;
            bindLocalVar(scope, param.name, resolvedTid, param.isByref, isConst, param.line, param.col);
        }

        checkAndEmitBlock(fn, fn.body, scope);

        bool endsWithReturn = false;
        if (!fn.body.empty()) {
            const StmtId lastSid = fn.body.back();
            if (lastSid < arena.stmts.size()) {
                if (arena.getStmt<ReturnStmt>(lastSid) != nullptr) {
                    endsWithReturn = true;
                } else if (const auto* es = arena.getStmt<ExprStmt>(lastSid)) {
                    if (const auto* call = arena.getExpr<FuncCall>(es->expr)) {
                        if (isErrorBuiltinCall(arena, *call)) {
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
                    emitLine("return { " + defaultValueForType(moduleId, fn.returnType) + R"(, CbError{"", 0} };)");
                }
            } else if (!isVoidRet) {
                emitLine("return " + defaultValueForType(moduleId, fn.returnType) + ";");
            }
        }

        indentLevel -= 1;
        emitLine("}");

        auto& savedScope = symbols.fnScopes[currentFnKey];
        for (auto& kv : scope) {
            if (savedScope.find(kv.first) == savedScope.end()) {
                savedScope.emplace(kv.first, std::move(kv.second));
            }
        }
        currentFnKey.clear();
        return std::move(lines);
    }

    void emitEnumDecl(const EnumDecl& ed) {
        const std::string cppName = lookupSymbolCppName(symbols.enums, moduleId, ed.name);
        emitLine("enum class " + cppName + " : int64_t {");
        indentLevel += 1;
        for (size_t i = 0; i < ed.members.size(); ++i) {
            std::string line = sanitizeCppIdent(ed.members[i]);
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
            emitLine("case " + cppName + "::" + sanitizeCppIdent(m) + ": return std::string(" + q + m + q + ");");
        }
        emitLine("default: return std::string();");
        indentLevel -= 1;
        emitLine("}");
        indentLevel -= 1;
        emitLine("}");
    }

    void emitStructDecl(const TypeDecl& td) {
        const std::string cppName = lookupSymbolCppName(symbols.types, moduleId, td.name);
        emitLine("struct " + cppName + " {");
        indentLevel += 1;
        for (const auto& f : td.fields) {
            const std::string fldCppName = sanitizeCppIdent(f.name);
            emitLine(mapType(moduleId, f.typeId) + " " + fldCppName + "{};");
        }
        indentLevel -= 1;
        emitLine("};");
    }

    void emitUnionDecl(const UnionDecl& ud) {
        const std::string key = symbolKey(moduleId, ud.name);
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

    std::unordered_set<std::string> computeReachableFunctions() const {
        std::unordered_set<std::string> reachable;
        std::vector<std::string> worklist;

        auto markReachable = [&](const std::string& key) {
            if (key.empty() || reachable.find(key) != reachable.end()) {
                return;
            }
            reachable.insert(key);
            worklist.push_back(key);
        };

        for (const auto& rootKey : symbols.rootReferencedFunctions) {
            markReachable(rootKey);
        }

        while (!worklist.empty()) {
            const std::string key = worklist.back();
            worklist.pop_back();
            const auto edgeIt = symbols.callGraphEdges.find(key);
            if (edgeIt == symbols.callGraphEdges.end()) {
                continue;
            }
            for (const auto& calleeKey : edgeIt->second) {
                markReachable(calleeKey);
            }
        }

        return reachable;
    }

    void analyzeProgram(const Program& program) {
        fileName = program.fileName;

        std::unordered_set<std::string> knownModules;
        knownModules.insert("");

        // Pass 1a: Register module imports and type/enum/union shell declarations so
        // out-of-order and cross-module type references are visible before inspecting fields/signatures.
        for (const DeclId did : program.declarations) {
            if (did >= arena.decls.size()) continue;
            const auto& declNode = arena.decls[did];
            knownModules.insert(declNode.moduleId);
            if (declNode.moduleId == "std/str") {
                symbols.hasStdStr = true;
            }

            if (const auto* imp = arena.getDecl<ImportDecl>(did)) {
                if (imp->kind == "header") {
                    std::string headerPath = imp->target;
                    if (!declNode.fileName.empty()) {
                        const std::string modDir = parentDirOf(declNode.fileName);
                        if (modDir != "." && !headerPath.empty() && headerPath[0] != '/') {
                            const std::string relCandidate = modDir + "/" + headerPath;
                            std::ifstream hIn(relCandidate);
                            if (hIn.is_open()) {
                                hIn.close();
                                headerPath = relCandidate;
                            }
                        }
                    }
                    bool found = false;
                    for (const auto& h : symbols.headers) {
                        if (h == headerPath) { found = true; break; }
                    }
                    if (!found) {
                        symbols.headers.push_back(headerPath);
                        symbols.headerModules[headerPath] = declNode.moduleId;
                    } else if (declNode.moduleId != "std/str") {
                        symbols.headerModules[headerPath] = declNode.moduleId;
                    }
                } else if (imp->kind == "module") {
                    std::string alias = defaultImportAlias(imp->target);
                    if (imp->hasAlias) {
                        alias = imp->alias;
                    }
                    if (symbols.modules[declNode.moduleId].count(alias) > 0) {
                        throw makeError("AnalyzerError", "Duplicate module import alias '" + alias + "'.", declNode.fileName, declNode.line, declNode.col);
                    }
                    std::string resolvedMod = imp->resolvedModuleId;
                    if (resolvedMod.empty()) {
                        resolvedMod = resolveModuleId(declNode.moduleId, imp->target);
                    }
                    symbols.modules[declNode.moduleId][alias] = resolvedMod;
                    symbols.explicitlyImportedModules.insert(resolvedMod);
                    if (resolvedMod == "std/str") {
                        symbols.hasStdStr = true;
                    }
                }
            } else if (const auto* tdecl = arena.getDecl<TypeDecl>(did)) {
                TypeSymbol tsym{};
                populateSymbolMeta(tsym, declNode, tdecl->name, tdecl->isPub, makeCppTypeName(declNode.moduleId, tdecl->name, tdecl->isExtern));
                tsym.isExtern = tdecl->isExtern;
                tsym.fieldOrder = tdecl->fields;
                const std::string keyCopy = tsym.key;
                symbols.types[keyCopy] = std::move(tsym);
                arena.findOrAddType(keyCopy, {});
            } else if (const auto* edecl = arena.getDecl<EnumDecl>(did)) {
                EnumSymbol esym{};
                populateSymbolMeta(esym, declNode, edecl->name, edecl->isPub, makeCppTypeName(declNode.moduleId, edecl->name, false));
                for (const auto& memName : edecl->members) {
                    if (esym.memberSet.count(memName) > 0) {
                        throw makeError("AnalyzerError", "Duplicate enum member '" + memName + "' in enum '" + edecl->name + "'.", declNode.fileName, declNode.line, declNode.col);
                    }
                    esym.memberSet.insert(memName);
                    esym.members.push_back(memName);
                }
                const std::string keyCopy = esym.key;
                symbols.enums[keyCopy] = std::move(esym);
                arena.findOrAddType(keyCopy, {});
            } else if (const auto* udecl = arena.getDecl<UnionDecl>(did)) {
                UnionSymbol usym{};
                populateSymbolMeta(usym, declNode, udecl->name, udecl->isPub, makeCppTypeName(declNode.moduleId, udecl->name, false));
                std::unordered_set<std::string> seen;
                for (size_t i = 0; i < udecl->variants.size(); ++i) {
                    const auto& uv = udecl->variants[i];
                    if (seen.count(uv.name) > 0) {
                        throw makeError("AnalyzerError", "Duplicate union variant '" + uv.name + "' in union '" + udecl->name + "'.", declNode.fileName, uv.line, uv.col);
                    }
                    seen.insert(uv.name);
                    usym.variants.push_back(uv);
                    usym.variantIndices[uv.name] = i;
                }
                const std::string keyCopy = usym.key;
                symbols.unions[keyCopy] = std::move(usym);
                arena.findOrAddType(keyCopy, {});
            }
        }

        if (symbols.hasStdStr) {
            for (const auto& modId : knownModules) {
                if (modId == "std/str") {
                    continue;
                }
                auto& modMap = symbols.modules[modId];
                if (modMap.count("str") > 0) {
                    continue;
                }
                bool alreadyImportedStdStr = false;
                for (const auto& kv : modMap) {
                    if (kv.second == "std/str") {
                        alreadyImportedStdStr = true;
                        break;
                    }
                }
                if (!alreadyImportedStdStr) {
                    modMap["str"] = "std/str";
                }
            }
        }

        // Pass 1b: Resolve struct field types, union variant types, constants, and function signatures
        for (const DeclId did : program.declarations) {
            if (did >= arena.decls.size()) continue;
            const auto& declNode = arena.decls[did];

            if (const auto* tdecl = arena.getDecl<TypeDecl>(did)) {
                const std::string key = symbolKey(declNode.moduleId, tdecl->name);
                auto& tsym = symbols.types[key];
                tsym.fieldOrder.clear();
                tsym.fieldOrder.reserve(tdecl->fields.size());
                for (const auto& fld : tdecl->fields) {
                    const TypeId resolvedFid = canonicalizeType(arena, symbols, declNode.moduleId, fld.typeId);
                    tsym.fields[fld.name] = resolvedFid;
                    FieldDecl resolvedFld = fld;
                    resolvedFld.typeId = resolvedFid;
                    tsym.fieldOrder.push_back(std::move(resolvedFld));
                }
            } else if (const auto* udecl = arena.getDecl<UnionDecl>(did)) {
                const std::string key = symbolKey(declNode.moduleId, udecl->name);
                auto& usym = symbols.unions[key];
                usym.variants.clear();
                for (const auto& uv : udecl->variants) {
                    const TypeId resolvedVid = canonicalizeType(arena, symbols, declNode.moduleId, uv.type);
                    usym.variantTypes[uv.name] = resolvedVid;
                    UnionVariant resolvedUv = uv;
                    resolvedUv.type = resolvedVid;
                    usym.variants.push_back(std::move(resolvedUv));
                }
            } else if (const auto* cdecl = arena.getDecl<ConstDecl>(did)) {
                const std::string key = symbolKey(declNode.moduleId, cdecl->name);
                if (symbols.constants.count(key) > 0) {
                    throw makeError("AnalyzerError", "Duplicate constant declaration '" + cdecl->name + "'.", declNode.fileName, declNode.line, declNode.col);
                }
                fileName = declNode.fileName;
                validateEmptyContainerInference(declNode.line, declNode.col, "const", cdecl->name, cdecl->hasDeclaredType, cdecl->value);
                std::unordered_map<std::string, VarSymbol> emptyScope;
                const TypeId inferredTid = inferAndCacheExprType(arena, cdecl->value, symbols, emptyScope, declNode.moduleId);
                TypeId constTid = inferredTid;
                if (cdecl->hasDeclaredType) {
                    constTid = canonicalizeType(arena, symbols, declNode.moduleId, cdecl->declaredType);
                }
                ConstSymbol csym{};
                populateSymbolMeta(csym, declNode, cdecl->name, cdecl->isPub, makeCppConstName(declNode.moduleId, cdecl->name));
                csym.typeId = constTid;
                symbols.constants[key] = std::move(csym);
            } else if (const auto* fdecl = arena.getDecl<FnDecl>(did)) {
                FnSymbol fsym{};
                populateSymbolMeta(fsym, declNode, fdecl->name, fdecl->isPub, makeCppFnName(declNode.moduleId, fdecl->name, fdecl->isExtern));
                fsym.isExtern = fdecl->isExtern;
                fsym.isFallible = fdecl->isFallible;
                fsym.params.reserve(fdecl->params.size());
                for (const auto& p : fdecl->params) {
                    Param resolvedP = p;
                    resolvedP.typeId = canonicalizeType(arena, symbols, declNode.moduleId, p.typeId);
                    fsym.params.push_back(std::move(resolvedP));
                }
                if (fdecl->returnType != INVALID_ID) {
                    fsym.returnType = canonicalizeType(arena, symbols, declNode.moduleId, fdecl->returnType);
                } else {
                    fsym.returnType = symbols.typeVoid;
                }
                if (symbols.functions.find(fsym.key) != symbols.functions.end()) {
                    setAnalyzerError(declNode.line, declNode.col, "Duplicate function definition '" + fdecl->name + "'.");
                }
                symbols.functions[fsym.key] = std::move(fsym);
            }
        }

        // Validate that composite value types (structs and unions) have no direct by-value recursive cycles
        // and record their topological declaration order in symbols.sortedCompositeDecls.
        {
            std::unordered_map<std::string, std::pair<std::string, DeclId>> compositeByKey;
            std::vector<std::string> compositeKeys;
            for (const DeclId did : program.declarations) {
                if (did >= arena.decls.size()) continue;
                const auto& declNode = arena.decls[did];
                std::string key;
                if (const auto* tdecl = arena.getDecl<TypeDecl>(did)) {
                    if (!tdecl->isExtern) {
                        key = symbolKey(declNode.moduleId, tdecl->name);
                    }
                } else if (const auto* udecl = arena.getDecl<UnionDecl>(did)) {
                    key = symbolKey(declNode.moduleId, udecl->name);
                }
                if (!key.empty() && compositeByKey.count(key) == 0) {
                    compositeByKey[key] = {declNode.moduleId, did};
                    compositeKeys.push_back(key);
                }
            }

            std::unordered_map<std::string, int> visitState;
            symbols.sortedCompositeDecls.clear();
            symbols.sortedCompositeDecls.reserve(compositeKeys.size());

            auto dfsCycleCheck = [&](auto& self, const std::string& currKey) -> void {
                visitState[currKey] = 1;
                for (const std::string& depKey : collectDirectCompositeDeps(arena, symbols, currKey)) {
                    const int st = visitState[depKey];
                    if (st == 1) {
                        std::string errFile = fileName;
                        uint32_t errLine = 1;
                        uint32_t errCol = 1;
                        std::string errName = stripModulePrefix(depKey);
                        const auto tit = symbols.types.find(depKey);
                        if (tit != symbols.types.end()) {
                            errName = tit->second.name;
                            errFile = tit->second.fileName;
                            errLine = tit->second.line;
                            errCol = tit->second.col;
                        } else {
                            const auto uit = symbols.unions.find(depKey);
                            if (uit != symbols.unions.end()) {
                                errName = uit->second.name;
                                errFile = uit->second.fileName;
                                errLine = uit->second.line;
                                errCol = uit->second.col;
                            }
                        }
                        throw makeError("AnalyzerError", "Recursive value type cycle detected involving '" + errName + "'.", errFile, errLine, errCol);
                    }
                    if (st == 0 && compositeByKey.count(depKey) > 0) {
                        self(self, depKey);
                    }
                }
                visitState[currKey] = 2;
                const auto compIt = compositeByKey.find(currKey);
                if (compIt != compositeByKey.end()) {
                    symbols.sortedCompositeDecls.push_back(compIt->second);
                }
            };

            for (const std::string& key : compositeKeys) {
                if (visitState[key] == 0) {
                    dfsCycleCheck(dfsCycleCheck, key);
                }
            }
        }

        // Pass 2 (Fused Typecheck & C++17 Codegen):
        // Validate and emit top-level constants and function bodies into buffers while recording
        // callGraphEdges and referencedModules.
        struct EmittedConstEntry {
            std::string moduleId;
            std::string cppLine;
        };
        struct EmittedFnEntry {
            std::string moduleId;
            std::string fnKey;
            std::string signatureLine;
            std::vector<std::string> bodyLines;
        };
        std::vector<EmittedConstEntry> emittedConsts;
        std::vector<EmittedFnEntry> emittedFns;

        for (const DeclId did : program.declarations) {
            if (did >= arena.decls.size()) continue;
            const auto& declNode = arena.decls[did];
            if (declNode.fileName.empty()) {
                fileName = "main.cb";
            } else {
                fileName = declNode.fileName;
            }
            moduleId = declNode.moduleId;
            if (arena.getDecl<ConstDecl>(did) != nullptr) {
                std::string constLine = checkAndEmitConstDecl(did, declNode);
                emittedConsts.push_back(EmittedConstEntry{declNode.moduleId, std::move(constLine)});
            } else if (const auto* fdecl = arena.getDecl<FnDecl>(did)) {
                if (!fdecl->isExtern) {
                    const std::string fnKey = symbolKey(declNode.moduleId, fdecl->name);
                    if (declNode.moduleId.empty()) {
                        symbols.rootReferencedFunctions.insert(fnKey);
                    }
                    const uint32_t savedTempCounter = tempCounter;
                    std::string sigLine = fnSignature(declNode.moduleId, *fdecl) + ";";
                    std::vector<std::string> fnLines = checkAndEmitFn(*fdecl);
                    if (declNode.moduleId == "std/str" &&
                        symbols.explicitlyImportedModules.find("std/str") == symbols.explicitlyImportedModules.end()) {
                        tempCounter = savedTempCounter;
                    }
                    emittedFns.push_back(EmittedFnEntry{
                        declNode.moduleId,
                        fnKey,
                        std::move(sigLine),
                        std::move(fnLines)
                    });
                }
            }
        }

        for (const DeclId did : program.declarations) {
            if (did >= arena.decls.size()) continue;
            const auto& declNode = arena.decls[did];
            if (!isModuleEmitted(symbols, declNode.moduleId)) {
                continue;
            }
            const auto topIt = symbols.callGraphEdges.find("__top__" + declNode.moduleId);
            if (topIt != symbols.callGraphEdges.end()) {
                for (const auto& calleeKey : topIt->second) {
                    symbols.rootReferencedFunctions.insert(calleeKey);
                }
            }
        }

        std::vector<std::string> activeHeaders;
        activeHeaders.reserve(symbols.headers.size());
        for (const auto& hdr : symbols.headers) {
            const auto hmIt = symbols.headerModules.find(hdr);
            if (hmIt != symbols.headerModules.end() && !isModuleEmitted(symbols, hmIt->second)) {
                continue;
            }
            activeHeaders.push_back(hdr);
        }
        symbols.headers = std::move(activeHeaders);

        // Assemble final Unity C++17 output (build.cpp)
        lines.clear();
        indentLevel = 0;
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
        for (const DeclId did : program.declarations) {
            if (did >= arena.decls.size()) continue;
            const auto& declNode = arena.decls[did];
            if (!isModuleEmitted(symbols, declNode.moduleId)) continue;
            if (const auto* td = arena.getDecl<TypeDecl>(did)) {
                if (!td->isExtern) {
                    structDecls.push_back({declNode.moduleId, *td});
                }
            } else if (const auto* ud = arena.getDecl<UnionDecl>(did)) {
                unionDecls.push_back({declNode.moduleId, *ud});
            } else if (const auto* ed = arena.getDecl<EnumDecl>(did)) {
                enumDecls.push_back({declNode.moduleId, *ed});
            }
        }

        if (!enumDecls.empty()) {
            emitLine();
            for (const auto& pair : enumDecls) {
                moduleId = pair.first;
                emitLine("enum class " + lookupSymbolCppName(symbols.enums, moduleId, pair.second.name) + " : int64_t;");
            }
            emitLine();
            for (const auto& pair : enumDecls) {
                moduleId = pair.first;
                emitEnumDecl(pair.second);
                emitLine();
            }
        }

        if (!structDecls.empty() || !unionDecls.empty()) {
            emitLine();
            for (const auto& pair : structDecls) {
                moduleId = pair.first;
                emitLine("struct " + lookupSymbolCppName(symbols.types, moduleId, pair.second.name) + ";");
            }
            for (const auto& pair : unionDecls) {
                moduleId = pair.first;
                emitLine("struct " + lookupSymbolCppName(symbols.unions, moduleId, pair.second.name) + ";");
            }

            emitLine();
            for (const auto& pair : symbols.sortedCompositeDecls) {
                if (!isModuleEmitted(symbols, pair.first)) {
                    continue;
                }
                moduleId = pair.first;
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

        std::vector<const EmittedConstEntry*> reachableConsts;
        for (const auto& entry : emittedConsts) {
            if (isModuleEmitted(symbols, entry.moduleId)) {
                reachableConsts.push_back(&entry);
            }
        }
        if (!reachableConsts.empty()) {
            emitLine();
            for (const auto* entry : reachableConsts) {
                emitLine(entry->cppLine);
            }
        }

        const std::unordered_set<std::string> reachableFns = computeReachableFunctions();
        std::vector<const EmittedFnEntry*> activeFns;
        for (const auto& entry : emittedFns) {
            if (!isModuleEmitted(symbols, entry.moduleId)) {
                continue;
            }
            if (entry.moduleId.empty() || reachableFns.find(entry.fnKey) != reachableFns.end()) {
                activeFns.push_back(&entry);
            }
        }

        if (!activeFns.empty()) {
            emitLine("// Forward Declarations");
            for (const auto* entry : activeFns) {
                emitLine(entry->signatureLine);
            }
            emitLine();
        }

        for (const auto* entry : activeFns) {
            for (const auto& fnLine : entry->bodyLines) {
                lines.push_back(fnLine);
            }
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
        symbols.emittedCpp = std::move(out);
    }
};

inline AnalysisResult analyzeProgram(
    AstArena& arena,
    const Program& program
) {
    AnalysisResult result{};
    result.ok = true;
    registerBuiltins(arena, result.symbols);

    try {
        Analyzer analyzer(arena, result.symbols, program.fileName);
        analyzer.analyzeProgram(program);
        result.cppCode = result.symbols.emittedCpp;
        return result;
    } catch (const DiagnosticError& err) {
        result.ok = false;
        result.error = err;
        return result;
    }
}

} // namespace cb

#endif // CLEANBASIC_ANALYZER_HPP
