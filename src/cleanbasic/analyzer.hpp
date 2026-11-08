#ifndef CLEANBASIC_ANALYZER_HPP
#define CLEANBASIC_ANALYZER_HPP

#include "src/cleanbasic/ast.hpp"
#include "src/cleanbasic/diagnostics.hpp"

#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace cb {

struct TypeSymbol {
    std::string name;
    std::string moduleId;
    std::string key;
    std::unordered_map<std::string, TypeId> fields;
    std::vector<FieldDecl> fieldOrder;
    bool isPub = false;
    bool isExtern = false;
    std::string cppName;
    std::string fileName = "main.cb";
    uint32_t line = 1;
    uint32_t col = 1;
    uint32_t endLine = 1;
};

struct FnSymbol {
    std::string name;
    std::string moduleId;
    std::string key;
    std::vector<Param> params;
    TypeId returnType = INVALID_ID;
    bool isFallible = false;
    bool isPub = false;
    bool isExtern = false;
    std::string cppName;
    std::string fileName = "main.cb";
    uint32_t line = 1;
    uint32_t col = 1;
    uint32_t endLine = 1;
};

struct AliasSymbol {
    std::string name;
    std::string moduleId;
    std::string key;
    TypeId targetType;
    bool isPub = false;
    std::string cppName;
    std::string fileName = "main.cb";
    uint32_t line = 1;
    uint32_t col = 1;
    uint32_t endLine = 1;
};

struct VarSymbol {
    std::string name;
    TypeId typeId = INVALID_ID;
    bool isByref = false;
    bool isConst = false;
    std::string fileName = "main.cb";
    uint32_t line = 1;
    uint32_t col = 1;
    uint32_t endLine = 1;
};

struct GlobalSymbolTable {
    std::unordered_map<std::string, TypeSymbol> types;
    std::unordered_map<std::string, AliasSymbol> aliases;
    std::unordered_map<std::string, FnSymbol> functions;
    std::vector<std::string> headers;
    std::vector<std::string> linkLibs;
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> modules;
    std::unordered_map<std::string, std::unordered_map<std::string, VarSymbol>> fnScopes;
    TypeId typeInt = INVALID_ID;
    TypeId typeFloat = INVALID_ID;
    TypeId typeStr = INVALID_ID;
    TypeId typeBool = INVALID_ID;
    TypeId typeVoid = INVALID_ID;
    TypeId typeAuto = INVALID_ID;
    TypeId typeNull = INVALID_ID;
    TypeId typeAny = INVALID_ID;
    TypeId typeErr = INVALID_ID;
    TypeId typeCptr = INVALID_ID;
};

struct AnalysisResult {
    bool ok = true;
    GlobalSymbolTable symbols;
    DiagnosticError error;
};

inline void registerBuiltins(AstArena& arena, GlobalSymbolTable& symbols) {
    const TypeId intT = findOrAddType(arena, "int");
    const TypeId floatT = findOrAddType(arena, "float");
    const TypeId strT = findOrAddType(arena, "str");
    const TypeId boolT = findOrAddType(arena, "bool");
    const TypeId voidT = findOrAddType(arena, "void");
    const TypeId autoT = findOrAddType(arena, "auto");
    const TypeId nullT = findOrAddType(arena, "null");
    const TypeId anyT = findOrAddType(arena, "any");
    const TypeId errT = findOrAddType(arena, "err");
    const TypeId cptrT = findOrAddType(arena, "cptr");

    symbols.typeInt = intT;
    symbols.typeFloat = floatT;
    symbols.typeStr = strT;
    symbols.typeBool = boolT;
    symbols.typeVoid = voidT;
    symbols.typeAuto = autoT;
    symbols.typeNull = nullT;
    symbols.typeAny = anyT;
    symbols.typeErr = errT;
    symbols.typeCptr = cptrT;

    TypeSymbol errType{};
    errType.name = "err";
    errType.moduleId = "";
    errType.key = symbolKey("", "err");
    errType.isPub = true;
    errType.fields["msg"] = strT;
    errType.fields["code"] = intT;
    errType.fieldOrder.push_back(FieldDecl{1, 1, "msg", strT});
    errType.fieldOrder.push_back(FieldDecl{1, 1, "code", intT});
    errType.cppName = "cbErr";
    symbols.types[errType.key] = std::move(errType);

    FnSymbol printFn{};
    printFn.name = "print";
    printFn.moduleId = "";
    printFn.key = symbolKey("", "print");
    printFn.isPub = true;
    printFn.isFallible = false;
    printFn.cppName = "cbPrint";
    symbols.functions[printFn.key] = std::move(printFn);

    FnSymbol lenFn{};
    lenFn.name = "len";
    lenFn.moduleId = "";
    lenFn.key = symbolKey("", "len");
    lenFn.isPub = true;
    lenFn.params.push_back(Param{1, 1, "container", anyT, false});
    lenFn.returnType = intT;
    lenFn.cppName = "cbLen";
    symbols.functions[lenFn.key] = std::move(lenFn);

    FnSymbol strFn{};
    strFn.name = "str";
    strFn.moduleId = "";
    strFn.key = symbolKey("", "str");
    strFn.isPub = true;
    strFn.params.push_back(Param{1, 1, "val", anyT, false});
    strFn.returnType = strT;
    strFn.cppName = "cbStr";
    symbols.functions[strFn.key] = std::move(strFn);
}

inline bool isBuiltinTypeName(std::string_view name) {
    return name == "int" || name == "float" || name == "str" || name == "bool" ||
           name == "void" || name == "auto" || name == "null" || name == "any" ||
           name == "err" || name == "list" || name == "dict" || name == "cptr";
}

inline bool isBuiltinSymbolName(std::string_view name) {
    return name == "err" || name == "print" || name == "len" || name == "str";
}

inline bool isModuleAliasInScope(
    const GlobalSymbolTable& symbols,
    std::string_view currentModuleId,
    std::string_view identName,
    const std::unordered_map<std::string, VarSymbol>& scope,
    std::string& outTargetModuleId
) {
    if (scope.find(std::string(identName)) != scope.end()) {
        return false;
    }
    const auto modIt = symbols.modules.find(std::string(currentModuleId));
    if (modIt == symbols.modules.end()) {
        return false;
    }
    const auto aliasIt = modIt->second.find(std::string(identName));
    if (aliasIt == modIt->second.end()) {
        return false;
    }
    outTargetModuleId = aliasIt->second;
    return true;
}

inline std::string resolveSymbolKey(
    const GlobalSymbolTable& symbols,
    std::string_view currentModuleId,
    std::string_view name
) {
    if (name.find("::") != std::string_view::npos) {
        return std::string(name);
    }
    const size_t dotIdx = name.find('.');
    if (dotIdx != std::string_view::npos) {
        const std::string prefix = std::string(name.substr(0, dotIdx));
        const std::string sym = std::string(name.substr(dotIdx + 1));
        const auto modIt = symbols.modules.find(std::string(currentModuleId));
        if (modIt != symbols.modules.end()) {
            const auto aliasIt = modIt->second.find(prefix);
            if (aliasIt != modIt->second.end()) {
                return symbolKey(aliasIt->second, sym);
            }
        }
        return symbolKey(prefix, sym);
    }

    const std::string key = symbolKey(currentModuleId, name);
    if (symbols.types.count(key) > 0 || symbols.aliases.count(key) > 0 || symbols.functions.count(key) > 0) {
        return key;
    }
    if (isBuiltinSymbolName(name)) {
        return symbolKey("", name);
    }
    return key;
}

inline TypeId resolveAlias(
    const AstArena& arena,
    const GlobalSymbolTable& symbols,
    std::string_view currentModuleId,
    TypeId tid
);

inline TypeId inferExprType(
    const AstArena& arena,
    ExprId exprId,
    const GlobalSymbolTable& symbols,
    const std::unordered_map<std::string, VarSymbol>& scope,
    std::string_view currentModuleId
);

inline std::optional<TypeSymbol> resolveTypeSymbolByName(
    const AstArena& arena,
    const GlobalSymbolTable& symbols,
    std::string_view currentModuleId,
    std::string_view rawName,
    bool requireFirstHopPub = false
) {
    std::string key = resolveSymbolKey(symbols, currentModuleId, rawName);
    if (symbols.functions.find(key) != symbols.functions.end()) {
        return std::nullopt;
    }
    std::unordered_set<std::string> seen;
    std::string activeMod(currentModuleId);
    bool firstHop = true;
    while (symbols.aliases.find(key) != symbols.aliases.end()) {
        if (seen.find(key) != seen.end()) {
            break;
        }
        seen.insert(key);
        const auto& asym = symbols.aliases.at(key);
        if (firstHop && requireFirstHopPub && !asym.isPub && asym.moduleId != currentModuleId) {
            return std::nullopt;
        }
        firstHop = false;
        activeMod = asym.moduleId;
        if (asym.targetType >= arena.types.size()) {
            break;
        }
        key = resolveSymbolKey(symbols, activeMod, arena.types[asym.targetType].name);
    }
    const auto it = symbols.types.find(key);
    if (it != symbols.types.end()) {
        if (firstHop && requireFirstHopPub && !it->second.isPub && it->second.moduleId != currentModuleId) {
            return std::nullopt;
        }
        return it->second;
    }
    return std::nullopt;
}

inline std::optional<TypeSymbol> resolveTypeSymbol(
    const AstArena& arena,
    const GlobalSymbolTable& symbols,
    std::string_view currentModuleId,
    TypeId tid
) {
    if (tid == INVALID_ID || tid >= arena.types.size()) {
        return std::nullopt;
    }
    const TypeId resolvedTid = resolveAlias(arena, symbols, currentModuleId, tid);
    if (resolvedTid == INVALID_ID || resolvedTid >= arena.types.size()) {
        return std::nullopt;
    }
    const std::string& tname = arena.types[resolvedTid].name;
    const auto directIt = symbols.types.find(tname);
    if (directIt != symbols.types.end()) {
        return directIt->second;
    }
    const std::string key = resolveSymbolKey(symbols, currentModuleId, tname);
    const auto resolvedIt = symbols.types.find(key);
    if (resolvedIt != symbols.types.end()) {
        return resolvedIt->second;
    }
    return std::nullopt;
}

inline std::optional<FnSymbol> resolveUfcsFn(
    const AstArena& arena,
    const GlobalSymbolTable& symbols,
    std::string_view currentModuleId,
    TypeId recvTid,
    std::string_view methodName
) {
    // UFCS Step 1: Current module
    const std::string localKey = symbolKey(currentModuleId, methodName);
    const auto localIt = symbols.functions.find(localKey);
    if (localIt != symbols.functions.end()) {
        return localIt->second;
    }

    // UFCS Step 2: Receiver's defining module
    const TypeId resolvedRecv = resolveAlias(arena, symbols, currentModuleId, recvTid);
    std::string recvTypeKey;
    if (resolvedRecv != INVALID_ID && resolvedRecv < arena.types.size()) {
        recvTypeKey = resolveSymbolKey(symbols, currentModuleId, arena.types[resolvedRecv].name);
        const auto [recvModId, recvBase] = splitSymbolKey(recvTypeKey);
        if (!recvModId.empty()) {
            const std::string recvFnKey = symbolKey(recvModId, methodName);
            const auto recvFnIt = symbols.functions.find(recvFnKey);
            if (recvFnIt != symbols.functions.end()) {
                if (recvFnIt->second.isPub) {
                    return recvFnIt->second;
                }
                return std::nullopt;
            }
        }
    }


    return std::nullopt;
}

inline std::optional<FnSymbol> resolveFuncCall(
    const AstArena& arena,
    const FuncCall& call,
    const GlobalSymbolTable& symbols,
    std::string_view currentModuleId,
    const std::unordered_map<std::string, VarSymbol>& scope = {}
) {
    if (call.callee >= arena.exprs.size()) {
        return std::nullopt;
    }
    const auto& calleeNode = arena.exprs[call.callee];

    if (std::holds_alternative<Identifier>(calleeNode.payload)) {
        const auto& ident = std::get<Identifier>(calleeNode.payload);
        const std::string key = resolveSymbolKey(symbols, currentModuleId, ident.name);
        const auto it = symbols.functions.find(key);
        if (it != symbols.functions.end()) {
            if (ident.name.find('.') != std::string::npos && !it->second.isPub) {
                return std::nullopt;
            }
            return it->second;
        }
        return std::nullopt;
    }

    if (std::holds_alternative<FieldAccess>(calleeNode.payload)) {
        const auto& access = std::get<FieldAccess>(calleeNode.payload);
        if (access.target < arena.exprs.size() &&
            std::holds_alternative<Identifier>(arena.exprs[access.target].payload)) {
            const auto& tgtIdent = std::get<Identifier>(arena.exprs[access.target].payload);
            std::string targetModId;
            if (isModuleAliasInScope(symbols, currentModuleId, tgtIdent.name, scope, targetModId)) {
                const std::string key = symbolKey(targetModId, access.fieldName);
                const auto it = symbols.functions.find(key);
                if (it != symbols.functions.end() && it->second.isPub) {
                    return it->second;
                }
                return std::nullopt;
            }
        }
        const TypeId recvTid = inferExprType(arena, access.target, symbols, scope, currentModuleId);
        return resolveUfcsFn(arena, symbols, currentModuleId, recvTid, access.fieldName);
    }

    return std::nullopt;
}

inline std::optional<TypeSymbol> resolveStructConstructor(
    const AstArena& arena,
    const FuncCall& call,
    const GlobalSymbolTable& symbols,
    std::string_view currentModuleId,
    const std::unordered_map<std::string, VarSymbol>& scope = {}
) {
    if (call.callee >= arena.exprs.size()) {
        return std::nullopt;
    }
    const auto& calleeNode = arena.exprs[call.callee];

    if (std::holds_alternative<Identifier>(calleeNode.payload)) {
        const auto& ident = std::get<Identifier>(calleeNode.payload);
        const bool requirePub = (ident.name.find('.') != std::string::npos);
        return resolveTypeSymbolByName(arena, symbols, currentModuleId, ident.name, requirePub);
    }

    if (std::holds_alternative<FieldAccess>(calleeNode.payload)) {
        const auto& access = std::get<FieldAccess>(calleeNode.payload);
        if (access.target < arena.exprs.size() &&
            std::holds_alternative<Identifier>(arena.exprs[access.target].payload)) {
            const auto& modIdent = std::get<Identifier>(arena.exprs[access.target].payload);
            std::string targetModId;
            if (!isModuleAliasInScope(symbols, currentModuleId, modIdent.name, scope, targetModId)) {
                return std::nullopt;
            }
            return resolveTypeSymbolByName(arena, symbols, targetModId, access.fieldName, true);
        }
        return std::nullopt;
    }

    return std::nullopt;
}

inline TypeNode resolveAliasNode(
    const AstArena& arena,
    const GlobalSymbolTable& symbols,
    std::string_view currentModuleId,
    TypeId tid
) {
    if (tid >= arena.types.size()) {
        return TypeNode{};
    }

    std::unordered_set<std::string> seen;
    TypeNode curr = arena.types[tid];
    std::string activeModId(currentModuleId);

    while (!curr.name.empty()) {
        const std::string key = resolveSymbolKey(symbols, activeModId, curr.name);
        const auto it = symbols.aliases.find(key);
        if (it == symbols.aliases.end()) {
            break;
        }
        if (curr.name.find('.') != std::string::npos && !it->second.isPub) {
            break;
        }
        if (seen.find(key) != seen.end()) {
            break;
        }
        seen.insert(key);
        activeModId = it->second.moduleId;
        const TypeId aliasedId = it->second.targetType;
        if (aliasedId >= arena.types.size()) {
            break;
        }
        const TypeNode& aliased = arena.types[aliasedId];

        TypeNode nextNode{};
        nextNode.line = curr.line;
        nextNode.col = curr.col;
        nextNode.name = aliased.name;
        nextNode.typeArgs = aliased.typeArgs;
        curr = std::move(nextNode);
    }

    if (!curr.name.empty() && !isBuiltinTypeName(curr.name)) {
        const std::string typeKey = resolveSymbolKey(symbols, activeModId, curr.name);
        const auto tit = symbols.types.find(typeKey);
        if (tit != symbols.types.end()) {
            if (curr.name.find('.') == std::string::npos || tit->second.isPub) {
                curr.name = tit->second.key;
            }
        }
    }

    return curr;
}

inline TypeId resolveAlias(
    const AstArena& arena,
    const GlobalSymbolTable& symbols,
    std::string_view currentModuleId,
    TypeId tid
) {
    if (tid >= arena.types.size()) {
        return tid;
    }
    const TypeNode resolvedNode = resolveAliasNode(arena, symbols, currentModuleId, tid);
    const TypeId found = findType(arena, resolvedNode.name, resolvedNode.typeArgs);
    if (found != INVALID_ID) {
        return found;
    }
    return tid;
}

inline TypeId inferExprType(
    const AstArena& arena,
    ExprId exprId,
    const GlobalSymbolTable& symbols,
    const std::unordered_map<std::string, VarSymbol>& scope,
    std::string_view currentModuleId
) {
    if (exprId >= arena.exprs.size()) {
        return symbols.typeAuto;
    }

    const auto& exprNode = arena.exprs[exprId];
    const auto& payload = exprNode.payload;

    if (std::holds_alternative<NumberLiteral>(payload)) {
        const auto& lit = std::get<NumberLiteral>(payload);
        if (lit.isFloat) {
            return symbols.typeFloat;
        }
        return symbols.typeInt;
    }

    if (std::holds_alternative<StringLiteral>(payload)) {
        return symbols.typeStr;
    }

    if (std::holds_alternative<BoolLiteral>(payload)) {
        return symbols.typeBool;
    }

    if (std::holds_alternative<NullLiteral>(payload)) {
        return symbols.typeNull;
    }

    if (std::holds_alternative<Identifier>(payload)) {
        const auto& ident = std::get<Identifier>(payload);
        const auto it = scope.find(ident.name);
        if (it != scope.end()) {
            return it->second.typeId;
        }
        return symbols.typeAuto;
    }


    if (std::holds_alternative<ListLiteral>(payload)) {
        const auto& llit = std::get<ListLiteral>(payload);
        TypeId elemType = symbols.typeInt;
        if (!llit.elements.empty()) {
            elemType = inferExprType(arena, llit.elements[0], symbols, scope, currentModuleId);
        }
        const TypeId listTid = findType(arena, "list", {elemType});
        if (listTid != INVALID_ID) {
            return listTid;
        }
        return symbols.typeAuto;
    }

    if (std::holds_alternative<DictLiteral>(payload)) {
        const auto& dlit = std::get<DictLiteral>(payload);
        TypeId kType = symbols.typeStr;
        TypeId vType = symbols.typeInt;
        if (!dlit.entries.empty()) {
            kType = inferExprType(arena, dlit.entries[0].key, symbols, scope, currentModuleId);
            vType = inferExprType(arena, dlit.entries[0].value, symbols, scope, currentModuleId);
        }
        const TypeId dictTid = findType(arena, "dict", {kType, vType});
        if (dictTid != INVALID_ID) {
            return dictTid;
        }
        return symbols.typeAuto;
    }

    if (std::holds_alternative<FieldAccess>(payload)) {
        const auto& faccess = std::get<FieldAccess>(payload);
        const TypeId targetTid = inferExprType(arena, faccess.target, symbols, scope, currentModuleId);
        const auto tsOpt = resolveTypeSymbol(arena, symbols, currentModuleId, targetTid);
        if (tsOpt.has_value()) {
            const auto fit = tsOpt->fields.find(faccess.fieldName);
            if (fit != tsOpt->fields.end()) {
                return fit->second;
            }
        }
        return symbols.typeAuto;
    }

    if (std::holds_alternative<IndexAccess>(payload)) {
        const auto& iaccess = std::get<IndexAccess>(payload);
        const TypeId targetTid = inferExprType(arena, iaccess.target, symbols, scope, currentModuleId);
        if (targetTid < arena.types.size()) {
            const auto& tnode = arena.types[targetTid];
            if (tnode.name == "list" && !tnode.typeArgs.empty()) {
                return tnode.typeArgs[0];
            }
            if (tnode.name == "dict" && tnode.typeArgs.size() == 2) {
                return tnode.typeArgs[1];
            }
        }
        return symbols.typeAuto;
    }

    if (std::holds_alternative<FuncCall>(payload)) {
        const auto& call = std::get<FuncCall>(payload);
        const auto fnOpt = resolveFuncCall(arena, call, symbols, currentModuleId, scope);
        if (fnOpt.has_value() && fnOpt->returnType != INVALID_ID) {
            return resolveAlias(arena, symbols, fnOpt->moduleId, fnOpt->returnType);
        }
        const auto structOpt = resolveStructConstructor(arena, call, symbols, currentModuleId, scope);
        if (structOpt.has_value()) {
            const TypeId rawTid = findType(arena, structOpt->key, {});
            if (rawTid != INVALID_ID) {
                return rawTid;
            }
        }
        return symbols.typeVoid;
    }

    if (std::holds_alternative<UnaryOp>(payload)) {
        const auto& uop = std::get<UnaryOp>(payload);
        if (uop.op == "not") {
            return symbols.typeBool;
        }
        return inferExprType(arena, uop.operand, symbols, scope, currentModuleId);
    }

    if (std::holds_alternative<BinaryOp>(payload)) {
        const auto& bop = std::get<BinaryOp>(payload);
        if (bop.op == "and" || bop.op == "or" ||
            bop.op == "==" || bop.op == "!=" ||
            bop.op == "<" || bop.op == ">" ||
            bop.op == "<=" || bop.op == ">=") {
            return symbols.typeBool;
        }
        if (bop.op == "&") {
            return symbols.typeStr;
        }
        const TypeId leftTid = inferExprType(arena, bop.left, symbols, scope, currentModuleId);
        const TypeId rightTid = inferExprType(arena, bop.right, symbols, scope, currentModuleId);
        std::string_view leftName = "";
        if (leftTid < arena.types.size()) {
            leftName = arena.types[leftTid].name;
        }
        std::string_view rightName = "";
        if (rightTid < arena.types.size()) {
            rightName = arena.types[rightTid].name;
        }

        if (leftName == "str" || rightName == "str") {
            return symbols.typeStr;
        }
        if (leftName == "float" || rightName == "float") {
            return symbols.typeFloat;
        }
        return leftTid;
    }

    return symbols.typeAuto;
}

struct AnalyzerState {
    std::string fileName = "main.cb";
    std::string moduleId = "";
    DiagnosticError error;
    bool hasError = false;
};

inline void setAnalyzerError(
    AnalyzerState& state,
    uint32_t line,
    uint32_t col,
    std::string_view msg
) {
    if (!state.hasError) {
        state.hasError = true;
        state.error = makeError("AnalyzerError", std::string(msg), state.fileName, line, col);
    }
}

inline void checkFallibleUsage(
    AnalyzerState& state,
    const AstArena& arena,
    const FnDecl& fn,
    ExprId exprId,
    bool hasActiveOnError,
    const GlobalSymbolTable& symbols,
    bool& outImplicitBubble
) {
    outImplicitBubble = false;
    if (exprId >= arena.exprs.size()) {
        return;
    }
    const auto& exprNode = arena.exprs[exprId];
    if (std::holds_alternative<FuncCall>(exprNode.payload)) {
        const auto& call = std::get<FuncCall>(exprNode.payload);
        const auto fnSymOpt = resolveFuncCall(arena, call, symbols, state.moduleId);
        if (fnSymOpt.has_value()) {
            const auto& fnSym = fnSymOpt.value();
            const bool calleeFallible = fnSym.isFallible;
            const bool callerFallible = fn.isFallible;
            if (calleeFallible) {
                if (!hasActiveOnError) {
                    if (callerFallible) {
                        outImplicitBubble = true;
                    } else {
                        std::string msg = "Unhandled error: function returns an error but caller does not declare 'can error' or have an active 'on error' block (function '" +
                            fn.name + "' calls fallible function '" + fnSym.name + "').";
                        setAnalyzerError(state, exprNode.line, exprNode.col, msg);
                        return;
                    }
                }
            }
        }
    }
}

inline bool isErrorBuiltinCall(const AstArena& arena, const FuncCall& call);

inline bool stmtListHasExit(const AstArena& arena, const std::vector<StmtId>& stmts) {
    if (stmts.empty()) {
        return false;
    }
    const StmtId lastSid = stmts.back();
    if (lastSid >= arena.stmts.size()) {
        return false;
    }
    const auto& lastPayload = arena.stmts[lastSid].payload;
    if (std::holds_alternative<ReturnStmt>(lastPayload)) {
        return true;
    }
    if (std::holds_alternative<BreakStmt>(lastPayload)) {
        return true;
    }
    if (std::holds_alternative<ExprStmt>(lastPayload)) {
        const auto& estmt = std::get<ExprStmt>(lastPayload);
        if (estmt.expr < arena.exprs.size() && std::holds_alternative<FuncCall>(arena.exprs[estmt.expr].payload)) {
            const auto& call = std::get<FuncCall>(arena.exprs[estmt.expr].payload);
            if (isErrorBuiltinCall(arena, call)) {
                return true;
            }
        }
    }
    return false;
}

inline bool isErrorBuiltinCall(const AstArena& arena, const FuncCall& call) {
    if (call.callee >= arena.exprs.size()) {
        return false;
    }
    const auto& calleeNode = arena.exprs[call.callee];
    if (std::holds_alternative<Identifier>(calleeNode.payload)) {
        return std::get<Identifier>(calleeNode.payload).name == "error";
    }
    return false;
}

inline void validateExprCalls(
    AnalyzerState& state,
    const AstArena& arena,
    const FnDecl& fn,
    ExprId exprId,
    const GlobalSymbolTable& symbols,
    const std::unordered_map<std::string, VarSymbol>& scope,
    bool allowTopLevelFallible
) {
    if (exprId >= arena.exprs.size() || state.hasError) {
        return;
    }

    const auto& exprNode = arena.exprs[exprId];
    const auto& payload = exprNode.payload;

    if (std::holds_alternative<FuncCall>(payload)) {
        const auto& call = std::get<FuncCall>(payload);
        if (isErrorBuiltinCall(arena, call)) {
            const bool callerFallible = fn.isFallible;
            if (!callerFallible) {
                std::string msg = "Cannot call 'error(...)' inside infallible function '" + fn.name +
                    "'; add 'can error' to the function signature.";
                setAnalyzerError(state, exprNode.line, exprNode.col, msg);
                return;
            }
            if (call.args.empty()) {
                setAnalyzerError(state, exprNode.line, exprNode.col, "'error(...)' requires at least one message argument.");
                return;
            }
            for (const auto& namedArg : call.namedArgs) {
                if (namedArg.name != "code") {
                    std::string msg = "Unknown named argument '" + namedArg.name +
                        "' in 'error(...)'; only 'code=' is supported.";
                    setAnalyzerError(state, exprNode.line, exprNode.col, msg);
                    return;
                }
            }
        }
        const auto fnSymOpt = resolveFuncCall(arena, call, symbols, state.moduleId, scope);
        if (fnSymOpt.has_value() && fnSymOpt->isFallible && !allowTopLevelFallible) {
            if (!fn.isFallible) {
                std::string msg = "Function '" + fn.name + "' calls fallible function '" + fnSymOpt->name +
                    "' without an 'on error' block, but '" + fn.name + "' does not declare 'can error' in its signature.";
                setAnalyzerError(state, exprNode.line, exprNode.col, msg);
                return;
            }
        }
        if (call.callee < arena.exprs.size() && std::holds_alternative<FieldAccess>(arena.exprs[call.callee].payload)) {
            const auto& calleeFa = std::get<FieldAccess>(arena.exprs[call.callee].payload);
            if (calleeFa.target < arena.exprs.size()) {
                const auto& tgtNode = arena.exprs[calleeFa.target];
                if (std::holds_alternative<Identifier>(tgtNode.payload)) {
                    const auto& tgtIdent = std::get<Identifier>(tgtNode.payload);
                    std::string targetModId;
                    if (isModuleAliasInScope(symbols, state.moduleId, tgtIdent.name, scope, targetModId)) {
                        const std::string symKey = symbolKey(targetModId, calleeFa.fieldName);
                        const auto fit = symbols.functions.find(symKey);
                        if (fit != symbols.functions.end() && !fit->second.isPub) {
                            std::string msg = "Function '" + calleeFa.fieldName + "' is private to module '" + targetModId + "' (missing 'pub').";
                            setAnalyzerError(state, exprNode.line, exprNode.col, msg);
                            return;
                        }
                        const auto tit = symbols.types.find(symKey);
                        if (tit != symbols.types.end() && !tit->second.isPub) {
                            std::string msg = "Type '" + calleeFa.fieldName + "' is private to module '" + targetModId + "' (missing 'pub').";
                            setAnalyzerError(state, exprNode.line, exprNode.col, msg);
                            return;
                        }
                        const auto ait = symbols.aliases.find(symKey);
                        if (ait != symbols.aliases.end() && !ait->second.isPub) {
                            std::string msg = "Alias '" + calleeFa.fieldName + "' is private to module '" + targetModId + "' (missing 'pub').";
                            setAnalyzerError(state, exprNode.line, exprNode.col, msg);
                            return;
                        }
                    }
                } else {
                    validateExprCalls(state, arena, fn, calleeFa.target, symbols, scope, false);
                    if (state.hasError) {
                        return;
                    }
                }
            }
        }
        for (const ExprId argId : call.args) {
            validateExprCalls(state, arena, fn, argId, symbols, scope, false);
        }
        for (const auto& namedArg : call.namedArgs) {
            validateExprCalls(state, arena, fn, namedArg.value, symbols, scope, false);
        }
        return;
    }

    if (std::holds_alternative<BinaryOp>(payload)) {
        const auto& bop = std::get<BinaryOp>(payload);
        validateExprCalls(state, arena, fn, bop.left, symbols, scope, false);
        validateExprCalls(state, arena, fn, bop.right, symbols, scope, false);
        return;
    }

    if (std::holds_alternative<UnaryOp>(payload)) {
        const auto& uop = std::get<UnaryOp>(payload);
        validateExprCalls(state, arena, fn, uop.operand, symbols, scope, false);
        return;
    }

    if (std::holds_alternative<FieldAccess>(payload)) {
        const auto& faccess = std::get<FieldAccess>(payload);
        validateExprCalls(state, arena, fn, faccess.target, symbols, scope, false);
        return;
    }

    if (std::holds_alternative<IndexAccess>(payload)) {
        const auto& iaccess = std::get<IndexAccess>(payload);
        validateExprCalls(state, arena, fn, iaccess.target, symbols, scope, false);
        validateExprCalls(state, arena, fn, iaccess.index, symbols, scope, false);
        return;
    }


    if (std::holds_alternative<ListLiteral>(payload)) {
        const auto& llit = std::get<ListLiteral>(payload);
        for (const ExprId elId : llit.elements) {
            validateExprCalls(state, arena, fn, elId, symbols, scope, false);
        }
        return;
    }

    if (std::holds_alternative<DictLiteral>(payload)) {
        const auto& dlit = std::get<DictLiteral>(payload);
        for (const auto& ent : dlit.entries) {
            validateExprCalls(state, arena, fn, ent.key, symbols, scope, false);
            validateExprCalls(state, arena, fn, ent.value, symbols, scope, false);
        }
        return;
    }
}

inline void analyzeBlock(
    AnalyzerState& state,
    AstArena& arena,
    const FnDecl& fn,
    const std::vector<StmtId>& stmts,
    GlobalSymbolTable& symbols,
    std::unordered_map<std::string, VarSymbol>& scope,
    bool inheritedOnError = false
);

inline void bindLocalVar(
    std::unordered_map<std::string, VarSymbol>& scope,
    const AnalyzerState& state,
    const std::string& name,
    TypeId tid,
    bool isByref,
    bool isConst,
    uint32_t line,
    uint32_t col
) {
    scope[name] = VarSymbol{name, tid, isByref, isConst, state.fileName, line, col, line};
}

inline void analyzeStmt(
    AnalyzerState& state,
    AstArena& arena,
    const FnDecl& fn,
    StmtId stmtId,
    GlobalSymbolTable& symbols,
    std::unordered_map<std::string, VarSymbol>& scope,
    bool& hasActiveOnError
) {
    if (stmtId >= arena.stmts.size() || state.hasError) {
        return;
    }

    const auto& stmtNode = arena.stmts[stmtId];
    const auto& payload = stmtNode.payload;

    if (std::holds_alternative<VarAssignment>(payload)) {
        auto& vassign = std::get<VarAssignment>(arena.stmts[stmtId].payload);
        checkFallibleUsage(state, arena, fn, vassign.value, hasActiveOnError, symbols, vassign.implicitBubble);
        if (state.hasError) return;

        const TypeId valType = inferExprType(arena, vassign.value, symbols, scope, state.moduleId);
        if (vassign.target < arena.exprs.size() && std::holds_alternative<Identifier>(arena.exprs[vassign.target].payload)) {
            const auto& tgtNode = arena.exprs[vassign.target];
            const auto& ident = std::get<Identifier>(tgtNode.payload);
            if (vassign.isDim || vassign.isConst) {
                TypeId varTid = symbols.typeInt;
                if (vassign.hasDeclaredType) {
                    varTid = resolveAlias(arena, symbols, state.moduleId, vassign.declaredType);
                } else if (valType != INVALID_ID) {
                    varTid = valType;
                }
                bindLocalVar(scope, state, ident.name, varTid, false, vassign.isConst, tgtNode.line, tgtNode.col);
            } else {
                const auto existing = scope.find(ident.name);
                if (existing == scope.end()) {
                    std::string msg = "Undeclared variable '" + ident.name + "'. Use 'dim' or 'const' to declare variables.";
                    setAnalyzerError(state, tgtNode.line, tgtNode.col, msg);
                    return;
                }
                if (existing->second.isConst) {
                    std::string msg = "Cannot reassign const variable '" + ident.name + "'.";
                    setAnalyzerError(state, tgtNode.line, tgtNode.col, msg);
                    return;
                }
            }
        }

        validateExprCalls(state, arena, fn, vassign.value, symbols, scope, true);
        return;
    }

    if (std::holds_alternative<ExprStmt>(payload)) {
        auto& estmt = std::get<ExprStmt>(arena.stmts[stmtId].payload);
        checkFallibleUsage(state, arena, fn, estmt.expr, hasActiveOnError, symbols, estmt.implicitBubble);
        if (state.hasError) return;

        validateExprCalls(state, arena, fn, estmt.expr, symbols, scope, true);
        return;
    }

    if (std::holds_alternative<ErrorBlock>(payload)) {
        const auto& errBlock = std::get<ErrorBlock>(payload);
        TypeId errT = symbols.typeErr;
        if (errT == INVALID_ID) {
            errT = addType(arena, TypeNode{stmtNode.line, stmtNode.col, "err", {}});
        }
        bindLocalVar(scope, state, errBlock.errVar, errT, false, false, stmtNode.line, stmtNode.col);
        auto errScope = scope;
        analyzeBlock(state, arena, fn, errBlock.body, symbols, errScope, false);
        if (state.hasError) return;

        if (!stmtListHasExit(arena, errBlock.body)) {
            std::string msg = "'on error' block must terminate with an explicit control-flow exit ('return', 'error(...)', or 'break').";
            setAnalyzerError(state, stmtNode.line, stmtNode.col, msg);
            return;
        }

        hasActiveOnError = true;
        return;
    }

    if (std::holds_alternative<IfStmt>(payload)) {
        const auto& ifStmt = std::get<IfStmt>(payload);
        validateExprCalls(state, arena, fn, ifStmt.condition, symbols, scope, false);
        if (state.hasError) return;
        analyzeBlock(state, arena, fn, ifStmt.thenBody, symbols, scope, hasActiveOnError);
        if (state.hasError) return;

        for (const auto& branch : ifStmt.elifBranches) {
            validateExprCalls(state, arena, fn, branch.condition, symbols, scope, false);
            if (state.hasError) return;
            analyzeBlock(state, arena, fn, branch.body, symbols, scope, hasActiveOnError);
            if (state.hasError) return;
        }

        if (ifStmt.hasElse) {
            analyzeBlock(state, arena, fn, ifStmt.elseBody, symbols, scope, hasActiveOnError);
        }
        return;
    }

    if (std::holds_alternative<ForStmt>(payload)) {
        const auto& forStmt = std::get<ForStmt>(payload);
        if (forStmt.rangeSpec.hasRange) {
            validateExprCalls(state, arena, fn, forStmt.rangeSpec.start, symbols, scope, false);
            validateExprCalls(state, arena, fn, forStmt.rangeSpec.end, symbols, scope, false);
            if (forStmt.rangeSpec.hasStep) {
                validateExprCalls(state, arena, fn, forStmt.rangeSpec.step, symbols, scope, false);
            }
            if (!forStmt.loopVars.empty()) {
                const TypeId intT = addType(arena, TypeNode{stmtNode.line, stmtNode.col, "int", {}});
                bindLocalVar(scope, state, forStmt.loopVars[0], intT, false, false, stmtNode.line, stmtNode.col);
            }
        } else if (forStmt.hasIterable) {
            validateExprCalls(state, arena, fn, forStmt.iterable, symbols, scope, false);
            const TypeId iterTid = inferExprType(arena, forStmt.iterable, symbols, scope, state.moduleId);
            if (iterTid < arena.types.size()) {
                const auto& iterT = arena.types[iterTid];
                if (iterT.name == "list" && !iterT.typeArgs.empty() && !forStmt.loopVars.empty()) {
                    bindLocalVar(scope, state, forStmt.loopVars[0], iterT.typeArgs[0], false, false, stmtNode.line, stmtNode.col);
                } else if (iterT.name == "dict" && iterT.typeArgs.size() == 2) {
                    if (forStmt.loopVars.size() >= 1) {
                        bindLocalVar(scope, state, forStmt.loopVars[0], iterT.typeArgs[0], false, false, stmtNode.line, stmtNode.col);
                    }
                    if (forStmt.loopVars.size() >= 2) {
                        bindLocalVar(scope, state, forStmt.loopVars[1], iterT.typeArgs[1], false, false, stmtNode.line, stmtNode.col);
                    }
                } else {
                    const TypeId autoT = addType(arena, TypeNode{stmtNode.line, stmtNode.col, "auto", {}});
                    for (const auto& lv : forStmt.loopVars) {
                        bindLocalVar(scope, state, lv, autoT, false, false, stmtNode.line, stmtNode.col);
                    }
                }
            }
        }
        if (state.hasError) return;
        analyzeBlock(state, arena, fn, forStmt.body, symbols, scope, hasActiveOnError);
        return;
    }

    if (std::holds_alternative<SelectStmt>(payload)) {
        const auto& selStmt = std::get<SelectStmt>(payload);
        validateExprCalls(state, arena, fn, selStmt.target, symbols, scope, false);
        if (state.hasError) return;

        const TypeId targetTid = inferExprType(arena, selStmt.target, symbols, scope, state.moduleId);
        if (targetTid != symbols.typeInt && targetTid != symbols.typeBool) {
            setAnalyzerError(state, stmtNode.line, stmtNode.col, "select case expression must be of type 'int' or 'bool'");
            return;
        }

        for (const auto& scase : selStmt.cases) {
            validateExprCalls(state, arena, fn, scase.matchExpr, symbols, scope, false);
            if (state.hasError) return;
            analyzeBlock(state, arena, fn, scase.body, symbols, scope, hasActiveOnError);
            if (state.hasError) return;
        }

        if (selStmt.hasCaseElse) {
            analyzeBlock(state, arena, fn, selStmt.caseElse, symbols, scope, hasActiveOnError);
        }
        return;
    }

    if (std::holds_alternative<DeferStmt>(payload)) {
        const auto& dstmt = std::get<DeferStmt>(payload);
        analyzeBlock(state, arena, fn, dstmt.body, symbols, scope, false);
        return;
    }



    if (std::holds_alternative<ReturnStmt>(payload)) {
        const auto& retStmt = std::get<ReturnStmt>(payload);
        if (retStmt.has_value) {
            validateExprCalls(state, arena, fn, retStmt.value, symbols, scope, false);
            if (state.hasError) return;
        }
        bool isReturnErrorCall = false;
        if (retStmt.has_value) {
            const ExprId firstId = retStmt.value;
            if (firstId < arena.exprs.size() && std::holds_alternative<FuncCall>(arena.exprs[firstId].payload)) {
                const auto& call = std::get<FuncCall>(arena.exprs[firstId].payload);
                if (isErrorBuiltinCall(arena, call)) {
                    isReturnErrorCall = true;
                }
            }
        }
        if (!isReturnErrorCall) {
            const bool expectedHasValue = (fn.returnType != INVALID_ID && !isVoidType(arena, fn.returnType));
            if (retStmt.has_value && !expectedHasValue) {
                std::string msg = "Function '" + fn.name + "' returns void, but a value was returned.";
                setAnalyzerError(state, stmtNode.line, stmtNode.col, msg);
                return;
            } else if (!retStmt.has_value && expectedHasValue) {
                std::string msg = "Function '" + fn.name + "' expects a return value, but none was provided.";
                setAnalyzerError(state, stmtNode.line, stmtNode.col, msg);
                return;
            }
        }
        return;
    }
}

inline void analyzeBlock(
    AnalyzerState& state,
    AstArena& arena,
    const FnDecl& fn,
    const std::vector<StmtId>& stmts,
    GlobalSymbolTable& symbols,
    std::unordered_map<std::string, VarSymbol>& scope,
    bool inheritedOnError
) {
    bool activeOnError = inheritedOnError;
    for (const StmtId sid : stmts) {
        if (state.hasError) {
            return;
        }
        analyzeStmt(state, arena, fn, sid, symbols, scope, activeOnError);
    }
}

inline void analyzeFn(
    AnalyzerState& state,
    AstArena& arena,
    const FnDecl& fn,
    GlobalSymbolTable& symbols
) {
    std::unordered_map<std::string, VarSymbol> scope;
    for (const auto& param : fn.params) {
        const TypeId resolvedTid = resolveAlias(arena, symbols, state.moduleId, param.typeId);
        bindLocalVar(scope, state, param.name, resolvedTid, param.isByref, false, param.line, param.col);
    }

    analyzeBlock(state, arena, fn, fn.body, symbols, scope, false);
    symbols.fnScopes[symbolKey(state.moduleId, fn.name)] = std::move(scope);
}

inline AnalysisResult analyzeProgram(
    AstArena& arena,
    const Program& program,
    std::optional<GlobalSymbolTable> initialSymbols = std::nullopt
) {
    AnalysisResult result{};
    result.ok = true;
    if (initialSymbols.has_value()) {
        result.symbols = std::move(initialSymbols.value());
    } else {
        registerBuiltins(arena, result.symbols);
    }

    AnalyzerState state{};
    state.fileName = program.fileName;

    // Pass 1a: Register module imports, aliases, and type declarations
    for (const DeclId did : program.declarations) {
        if (did >= arena.decls.size()) continue;
        const auto& declNode = arena.decls[did];
        const auto& payload = declNode.payload;

        if (std::holds_alternative<ImportDecl>(payload)) {
            const auto& imp = std::get<ImportDecl>(payload);
            if (imp.kind == "header") {
                std::string headerPath = imp.target;
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
                for (const auto& h : result.symbols.headers) {
                    if (h == headerPath) { found = true; break; }
                }
                if (!found) {
                    result.symbols.headers.push_back(headerPath);
                }
            } else if (imp.kind == "link") {
                bool found = false;
                for (const auto& l : result.symbols.linkLibs) {
                    if (l == imp.target) { found = true; break; }
                }
                if (!found) {
                    result.symbols.linkLibs.push_back(imp.target);
                }
            } else if (imp.kind == "module") {
                std::string alias = defaultImportAlias(imp.target);
                if (imp.hasAlias) {
                    alias = imp.alias;
                }
                if (result.symbols.modules[declNode.moduleId].count(alias) > 0) {
                    result.ok = false;
                    result.error = makeError("AnalyzerError", "Duplicate module import alias '" + alias + "'.", declNode.fileName, declNode.line, declNode.col);
                    return result;
                }
                std::string resolvedMod = imp.resolvedModuleId;
                if (resolvedMod.empty()) {
                    resolvedMod = resolveModuleId(declNode.moduleId, imp.target);
                }
                result.symbols.modules[declNode.moduleId][alias] = resolvedMod;
            }
        } else if (std::holds_alternative<AliasDecl>(payload)) {
            const auto& adecl = std::get<AliasDecl>(payload);
            AliasSymbol asym{};
            asym.name = adecl.name;
            asym.moduleId = declNode.moduleId;
            asym.key = symbolKey(declNode.moduleId, adecl.name);
            asym.targetType = adecl.targetType;
            asym.isPub = adecl.isPub;
            asym.cppName = makeCppTypeName(declNode.moduleId, adecl.name, false);
            asym.fileName = declNode.fileName;
            asym.line = declNode.nameLine;
            asym.col = declNode.nameCol;
            asym.endLine = declNode.endLine;
            result.symbols.aliases[asym.key] = std::move(asym);
        } else if (std::holds_alternative<TypeDecl>(payload)) {
            const auto& tdecl = std::get<TypeDecl>(payload);
            TypeSymbol tsym{};
            tsym.name = tdecl.name;
            tsym.moduleId = declNode.moduleId;
            tsym.key = symbolKey(declNode.moduleId, tdecl.name);
            tsym.isPub = tdecl.isPub;
            tsym.isExtern = tdecl.isExtern;
            tsym.cppName = makeCppTypeName(declNode.moduleId, tdecl.name, tdecl.isExtern);
            tsym.fieldOrder = tdecl.fields;
            tsym.fileName = declNode.fileName;
            tsym.line = declNode.nameLine;
            tsym.col = declNode.nameCol;
            tsym.endLine = declNode.endLine;
            const std::string keyCopy = tsym.key;
            result.symbols.types[keyCopy] = std::move(tsym);
            // Pre-register canonical struct types in the arena
            findOrAddType(arena, keyCopy, {});
        }
    }

    // Pass 1b: Resolve struct field types and register function signatures now that all types and aliases are registered
    for (const DeclId did : program.declarations) {
        if (did >= arena.decls.size()) continue;
        const auto& declNode = arena.decls[did];
        const auto& payload = declNode.payload;

        if (std::holds_alternative<TypeDecl>(payload)) {
            const auto& tdecl = std::get<TypeDecl>(payload);
            const std::string key = symbolKey(declNode.moduleId, tdecl.name);
            auto& tsym = result.symbols.types[key];
            for (const auto& fld : tdecl.fields) {
                tsym.fields[fld.name] = resolveAlias(arena, result.symbols, declNode.moduleId, fld.typeId);
            }
        } else if (std::holds_alternative<FnDecl>(payload)) {
            const auto& fdecl = std::get<FnDecl>(payload);
            FnSymbol fsym{};
            fsym.name = fdecl.name;
            fsym.moduleId = declNode.moduleId;
            fsym.key = symbolKey(declNode.moduleId, fdecl.name);
            fsym.isPub = fdecl.isPub;
            fsym.isExtern = fdecl.isExtern;
            fsym.isFallible = fdecl.isFallible;
            fsym.cppName = makeCppFnName(declNode.moduleId, fdecl.name, fdecl.isExtern);
            fsym.fileName = declNode.fileName;
            fsym.line = declNode.nameLine;
            fsym.col = declNode.nameCol;
            fsym.endLine = declNode.endLine;
            fsym.params.reserve(fdecl.params.size());
            for (const auto& p : fdecl.params) {
                Param resolvedP = p;
                resolvedP.typeId = resolveAlias(arena, result.symbols, declNode.moduleId, p.typeId);
                fsym.params.push_back(std::move(resolvedP));
            }
            if (fdecl.returnType != INVALID_ID) {
                fsym.returnType = resolveAlias(arena, result.symbols, declNode.moduleId, fdecl.returnType);
            }
            result.symbols.functions[fsym.key] = std::move(fsym);
        }
    }

    // Step 2: Validate function bodies, infer local variable types, and enforce static error rules
    for (const DeclId did : program.declarations) {
        if (did >= arena.decls.size()) continue;
        const auto& declNode = arena.decls[did];
        if (std::holds_alternative<FnDecl>(declNode.payload)) {
            const auto& fdecl = std::get<FnDecl>(declNode.payload);
            if (!fdecl.isExtern) {
                state.fileName = declNode.fileName;
                state.moduleId = declNode.moduleId;
                analyzeFn(state, arena, fdecl, result.symbols);
                if (state.hasError) {
                    result.ok = false;
                    result.error = state.error;
                    return result;
                }
            }
        }
    }
    return result;
}

}  // namespace cb

#endif  // CLEANBASIC_ANALYZER_HPP
