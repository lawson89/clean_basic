#ifndef CLEANBASIC_EMITTER_HPP
#define CLEANBASIC_EMITTER_HPP

#include "src/cleanbasic/analyzer.hpp"
#include "src/cleanbasic/ast.hpp"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
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

struct EmitterState {
    std::vector<std::string> lines;
    int indentLevel = 0;
    uint32_t tempCounter = 0;
    uint32_t valCounter = 0;
    uint32_t deferCounter = 0;
    std::optional<ErrorBlock> activeErrorBlock = std::nullopt;
    std::string currentModuleId = "";
};

inline void emitLine(EmitterState& state, std::string_view line = "") {
    if (line.empty()) {
        state.lines.emplace_back("");
    } else {
        std::string res;
        res.reserve(static_cast<size_t>(state.indentLevel * 4) + line.size());
        for (int i = 0; i < state.indentLevel; ++i) {
            res.append("    ");
        }
        res.append(line);
        state.lines.push_back(std::move(res));
    }
}

inline std::string nextTemp(EmitterState& state, std::string_view prefix = "_tmp") {
    state.tempCounter += 1;
    return std::string(prefix) + "_" + std::to_string(state.tempCounter);
}

inline std::string mapType(
    const AstArena& arena,
    const GlobalSymbolTable& symbols,
    std::string_view currentModuleId,
    TypeId tid,
    bool isByref = false
);

inline std::string mapBaseType(
    const AstArena& arena,
    const GlobalSymbolTable& symbols,
    std::string_view currentModuleId,
    const TypeNode& t
) {
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
            elem = mapType(arena, symbols, currentModuleId, t.typeArgs[0]);
        }
        return "std::vector<" + elem + ">";
    }
    if (t.name == "dict") {
        std::string k = "std::string";
        if (t.typeArgs.size() > 0) {
            k = mapType(arena, symbols, currentModuleId, t.typeArgs[0]);
        }
        std::string v = "int";
        if (t.typeArgs.size() > 1) {
            v = mapType(arena, symbols, currentModuleId, t.typeArgs[1]);
        }
        return "std::unordered_map<" + k + ", " + v + ">";
    }

    const std::string key = resolveSymbolKey(symbols, currentModuleId, t.name);
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

inline std::string mapTypeNode(
    const AstArena& arena,
    const GlobalSymbolTable& symbols,
    std::string_view currentModuleId,
    const TypeNode& resolved,
    bool isByref = false
) {
    const std::string baseCpp = mapBaseType(arena, symbols, currentModuleId, resolved);
    std::string cpp = baseCpp;
    if (isByref) {
        cpp += "&";
    }
    return cpp;
}

inline std::string mapType(
    const AstArena& arena,
    const GlobalSymbolTable& symbols,
    std::string_view currentModuleId,
    TypeId tid,
    bool isByref
) {
    const TypeNode resolved = resolveAliasNode(arena, symbols, currentModuleId, tid);
    return mapTypeNode(arena, symbols, currentModuleId, resolved, isByref);
}

inline std::string defaultValueForType(
    const AstArena& arena,
    const GlobalSymbolTable& symbols,
    std::string_view currentModuleId,
    TypeId tid
) {
    const TypeNode resolved = resolveAliasNode(arena, symbols, currentModuleId, tid);
    if (resolved.name == "int") return "0";
    if (resolved.name == "float") return "0.0";
    if (resolved.name == "bool") return "false";
    if (resolved.name == "str") return "std::string(\"\")";
    return mapTypeNode(arena, symbols, currentModuleId, resolved) + "{}";
}

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

inline std::string fnReturnCppType(
    const AstArena& arena,
    const GlobalSymbolTable& symbols,
    std::string_view currentModuleId,
    const FnDecl& fn
) {
    std::vector<std::string> retCpp;
    if (fn.returnType != INVALID_ID && !isVoidType(arena, fn.returnType)) {
        retCpp.push_back(mapType(arena, symbols, currentModuleId, fn.returnType));
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

inline std::string fnSignature(
    const AstArena& arena,
    const GlobalSymbolTable& symbols,
    std::string_view currentModuleId,
    const FnDecl& fn
) {
    const std::string retStr = fnReturnCppType(arena, symbols, currentModuleId, fn);
    std::vector<std::string> paramItems;
    paramItems.reserve(fn.params.size());
    for (const auto& p : fn.params) {
        paramItems.push_back(mapType(arena, symbols, currentModuleId, p.typeId, p.isByref) + " " + p.name);
    }
    const std::string key = resolveSymbolKey(symbols, currentModuleId, fn.name);
    std::string cppName = makeCppFnName(currentModuleId, fn.name, fn.isExtern);
    if (symbols.functions.count(key) > 0) {
        cppName = symbols.functions.at(key).cppName;
    }
    return retStr + " " + cppName + "(" + joinStrings(paramItems) + ")";
}

inline std::string emitExpr(
    const AstArena& arena,
    const GlobalSymbolTable& symbols,
    ExprId exprId,
    const std::unordered_map<std::string, VarSymbol>& scope,
    EmitterState& state
);

inline std::string emitAsStrExpr(
    const AstArena& arena,
    const GlobalSymbolTable& symbols,
    ExprId exprId,
    const std::unordered_map<std::string, VarSymbol>& scope,
    EmitterState& state
) {
    const TypeId tid = inferExprType(arena, exprId, symbols, scope, state.currentModuleId);
    std::string cpp = emitExpr(arena, symbols, exprId, scope, state);
    if (tid < arena.types.size() && arena.types[tid].name == "str") {
        return cpp;
    }
    return "cbToStr(" + cpp + ")";
}

inline std::vector<std::string> emitExprList(
    const AstArena& arena,
    const GlobalSymbolTable& symbols,
    const std::vector<ExprId>& exprIds,
    const std::unordered_map<std::string, VarSymbol>& scope,
    EmitterState& state
) {
    std::vector<std::string> rendered;
    rendered.reserve(exprIds.size());
    for (const ExprId eid : exprIds) {
        rendered.push_back(emitExpr(arena, symbols, eid, scope, state));
    }
    return rendered;
}

inline std::vector<std::string> orderStructFields(
    const AstArena& arena,
    const GlobalSymbolTable& symbols,
    std::string_view typeName,
    const std::vector<ExprId>& posArgs,
    const std::vector<NamedArg>& namedArgs,
    const std::unordered_map<std::string, VarSymbol>& scope,
    EmitterState& state
) {
    const std::string typeKey = resolveSymbolKey(symbols, state.currentModuleId, typeName);
    const auto it = symbols.types.find(typeKey);
    if (namedArgs.empty() || it == symbols.types.end() || it->second.fieldOrder.empty()) {
        return emitExprList(arena, symbols, posArgs, scope, state);
    }

    std::unordered_map<std::string, ExprId> namedMap;
    for (const auto& na : namedArgs) {
        namedMap[na.name] = na.value;
    }

    std::vector<std::string> ordered;
    ordered.reserve(it->second.fieldOrder.size());
    for (const auto& fdecl : it->second.fieldOrder) {
        const auto fit = namedMap.find(fdecl.name);
        if (fit != namedMap.end()) {
            ordered.push_back(emitExpr(arena, symbols, fit->second, scope, state));
        } else {
            ordered.push_back(defaultValueForType(arena, symbols, it->second.moduleId, fdecl.typeId));
        }
    }
    return ordered;
}

inline std::string emitCallArgsJoined(
    const AstArena& arena,
    const GlobalSymbolTable& symbols,
    const std::vector<ExprId>& posArgs,
    const std::vector<NamedArg>& namedArgs,
    const std::unordered_map<std::string, VarSymbol>& scope,
    EmitterState& state
) {
    std::vector<std::string> rendered = emitExprList(arena, symbols, posArgs, scope, state);
    rendered.reserve(posArgs.size() + namedArgs.size());
    for (const auto& na : namedArgs) {
        rendered.push_back(emitExpr(arena, symbols, na.value, scope, state));
    }
    return joinStrings(rendered);
}

inline std::string emitExpr(
    const AstArena& arena,
    const GlobalSymbolTable& symbols,
    ExprId exprId,
    const std::unordered_map<std::string, VarSymbol>& scope,
    EmitterState& state
) {
    if (exprId >= arena.exprs.size()) {
        return "";
    }

    const auto& exprNode = arena.exprs[exprId];
    const auto& payload = exprNode.payload;

    if (std::holds_alternative<NumberLiteral>(payload)) {
        return std::get<NumberLiteral>(payload).raw;
    }

    if (std::holds_alternative<StringLiteral>(payload)) {
        const std::string q(1, '"');
        return "std::string(" + q + std::get<StringLiteral>(payload).value + q + ")";
    }

    if (std::holds_alternative<BoolLiteral>(payload)) {
        if (std::get<BoolLiteral>(payload).value) {
            return "true";
        }
        return "false";
    }

    if (std::holds_alternative<NullLiteral>(payload)) {
        return "{}";
    }

    if (std::holds_alternative<Identifier>(payload)) {
        return std::get<Identifier>(payload).name;
    }

    if (std::holds_alternative<UnaryOp>(payload)) {
        const auto& uop = std::get<UnaryOp>(payload);
        std::string opStr = uop.op;
        if (uop.op == "not") {
            opStr = "!";
        }
        return "(" + opStr + emitExpr(arena, symbols, uop.operand, scope, state) + ")";
    }

    if (std::holds_alternative<BinaryOp>(payload)) {
        const auto& bop = std::get<BinaryOp>(payload);
        if (bop.op == "&") {
            const std::string leftCpp = emitAsStrExpr(arena, symbols, bop.left, scope, state);
            const std::string rightCpp = emitAsStrExpr(arena, symbols, bop.right, scope, state);
            return "(" + leftCpp + " + " + rightCpp + ")";
        }
        std::string opStr = bop.op;
        if (opStr == "and") {
            opStr = "&&";
        } else if (opStr == "or") {
            opStr = "||";
        }
        const std::string leftCpp = emitExpr(arena, symbols, bop.left, scope, state);
        const std::string rightCpp = emitExpr(arena, symbols, bop.right, scope, state);
        return "(" + leftCpp + " " + opStr + " " + rightCpp + ")";
    }

    if (std::holds_alternative<FieldAccess>(payload)) {
        const auto& faccess = std::get<FieldAccess>(payload);
        return emitExpr(arena, symbols, faccess.target, scope, state) + "." + faccess.fieldName;
    }

    if (std::holds_alternative<IndexAccess>(payload)) {
        const auto& iaccess = std::get<IndexAccess>(payload);
        return emitExpr(arena, symbols, iaccess.target, scope, state) + "[" +
            emitExpr(arena, symbols, iaccess.index, scope, state) + "]";
    }

    if (std::holds_alternative<ListLiteral>(payload)) {
        const auto& llit = std::get<ListLiteral>(payload);
        return "{" + joinStrings(emitExprList(arena, symbols, llit.elements, scope, state)) + "}";
    }

    if (std::holds_alternative<DictLiteral>(payload)) {
        const auto& dlit = std::get<DictLiteral>(payload);
        std::vector<std::string> entries;
        entries.reserve(dlit.entries.size());
        for (const auto& ent : dlit.entries) {
            const std::string k = emitExpr(arena, symbols, ent.key, scope, state);
            const std::string v = emitExpr(arena, symbols, ent.value, scope, state);
            entries.push_back("{" + k + ", " + v + "}");
        }
        return "{" + joinStrings(entries) + "}";
    }

    if (std::holds_alternative<FuncCall>(payload)) {
        const auto& call = std::get<FuncCall>(payload);
        if (call.callee >= arena.exprs.size()) {
            return "";
        }
        const auto structOpt = resolveStructConstructor(arena, call, symbols, state.currentModuleId, scope);
        if (structOpt.has_value()) {
            const auto ordered = orderStructFields(arena, symbols, structOpt->key, call.args, call.namedArgs, scope, state);
            return structOpt->cppName + "{" + joinStrings(ordered) + "}";
        }
        const auto& calleeNode = arena.exprs[call.callee];

        if (std::holds_alternative<Identifier>(calleeNode.payload)) {
            const std::string& fnName = std::get<Identifier>(calleeNode.payload).name;

            if (fnName == "len" && call.args.size() == 1) {
                return "static_cast<int>(" + emitExpr(arena, symbols, call.args[0], scope, state) + ".size())";
            }

            if (fnName == "str" && call.args.size() == 1) {
                const ExprId argExpr = call.args[0];
                const TypeId argTypeId = inferExprType(arena, argExpr, symbols, scope, state.currentModuleId);
                const std::string argCpp = emitExpr(arena, symbols, argExpr, scope, state);
                if (argTypeId < arena.types.size() && arena.types[argTypeId].name == "str") {
                    return argCpp;
                }
                if (argTypeId < arena.types.size() && arena.types[argTypeId].name == "err") {
                    return "cbToStr(" + argCpp + ")";
                }
                return "std::to_string(" + argCpp + ")";
            }

            if (fnName == "print") {
                if (call.args.empty()) {
                    return "(std::cout << std::endl)";
                }
                std::string streamExpr = "(std::cout";
                for (const ExprId argId : call.args) {
                    const TypeId argTid = inferExprType(arena, argId, symbols, scope, state.currentModuleId);
                    std::string argCpp = emitExpr(arena, symbols, argId, scope, state);
                    if (argTid < arena.types.size() &&
                        (arena.types[argTid].name == "bool" || arena.types[argTid].name == "err")) {
                        argCpp = "cbToStr(" + argCpp + ")";
                    }
                    streamExpr += " << " + argCpp;
                }
                streamExpr += " << std::endl)";
                return streamExpr;
            }

            const std::optional<FnSymbol> fnSym = resolveFuncCall(arena, call, symbols, state.currentModuleId, scope);
            std::string targetCppName = makeCppFnName("", fnName);
            if (fnSym.has_value()) {
                targetCppName = fnSym->cppName;
            }
            return targetCppName + "(" + emitCallArgsJoined(arena, symbols, call.args, call.namedArgs, scope, state) + ")";
        }

        if (std::holds_alternative<FieldAccess>(calleeNode.payload)) {
            const auto& faccess = std::get<FieldAccess>(calleeNode.payload);
            const ExprId receiver = faccess.target;
            const std::string& methodName = faccess.fieldName;

            if (receiver < arena.exprs.size() && std::holds_alternative<Identifier>(arena.exprs[receiver].payload)) {
                const auto& tgtIdent = std::get<Identifier>(arena.exprs[receiver].payload);
                std::string targetModId;
                if (isModuleAliasInScope(symbols, state.currentModuleId, tgtIdent.name, scope, targetModId)) {
                    const std::optional<FnSymbol> fnSym = resolveFuncCall(arena, call, symbols, state.currentModuleId, scope);
                    std::string targetCppName = makeCppFnName(targetModId, methodName);
                    if (fnSym.has_value()) {
                        targetCppName = fnSym->cppName;
                    }
                    return targetCppName + "(" + emitCallArgsJoined(arena, symbols, call.args, call.namedArgs, scope, state) + ")";
                }
            }

            const TypeId recvTid = inferExprType(arena, receiver, symbols, scope, state.currentModuleId);
            const TypeNode& recvType = getTypeNode(arena, recvTid);
            const std::string recvCpp = emitExpr(arena, symbols, receiver, scope, state);

            if (recvType.name == "list" && (methodName == "push" || methodName == "append") && !call.args.empty()) {
                return recvCpp + ".push_back(" + emitExpr(arena, symbols, call.args[0], scope, state) + ")";
            }

            const std::optional<FnSymbol> ufcsSym = resolveFuncCall(arena, call, symbols, state.currentModuleId, scope);
            if (ufcsSym.has_value()) {
                std::vector<ExprId> allPos;
                allPos.reserve(1 + call.args.size());
                allPos.push_back(receiver);
                for (const ExprId a : call.args) {
                    allPos.push_back(a);
                }
                return ufcsSym->cppName + "(" + emitCallArgsJoined(arena, symbols, allPos, call.namedArgs, scope, state) + ")";
            }

            return recvCpp + "." + methodName + "(" + joinStrings(emitExprList(arena, symbols, call.args, scope, state)) + ")";
        }

        const std::string calleeCpp = emitExpr(arena, symbols, call.callee, scope, state);
        return calleeCpp + "(" + joinStrings(emitExprList(arena, symbols, call.args, scope, state)) + ")";
    }

    return "";
}

inline std::string buildErrorCallValue(
    const AstArena& arena,
    const GlobalSymbolTable& symbols,
    const FuncCall& call,
    const std::unordered_map<std::string, VarSymbol>& scope,
    EmitterState& state
) {
    std::string msgCpp = R"(std::string(""))";
    if (call.args.size() == 1) {
        msgCpp = emitAsStrExpr(arena, symbols, call.args[0], scope, state);
    } else if (call.args.size() > 1) {
        std::vector<std::string> parts;
        parts.reserve(call.args.size());
        for (const ExprId argId : call.args) {
            parts.push_back(emitAsStrExpr(arena, symbols, argId, scope, state));
        }
        msgCpp = "(" + joinStrings(parts, " + ") + ")";
    }

    std::string codeCpp = "1";
    for (const auto& na : call.namedArgs) {
        if (na.name == "code") {
            codeCpp = emitExpr(arena, symbols, na.value, scope, state);
        }
    }
    return "CbError{" + msgCpp + ", " + codeCpp + "}";
}

inline std::string buildErrorBubbleReturn(
    const AstArena& arena,
    const GlobalSymbolTable& symbols,
    std::string_view currentModuleId,
    const FnDecl& fn,
    std::string_view errVar
) {
    if (fn.returnType == INVALID_ID || isVoidType(arena, fn.returnType)) {
        return "return " + std::string(errVar) + ";";
    }
    std::string args;
    const TypeNode tnode = resolveAliasNode(arena, symbols, currentModuleId, fn.returnType);
    std::string elem = "{}";
    if (tnode.name == "str" && tnode.typeArgs.empty()) {
        elem = "std::string{}";
    } else if (tnode.name == "list" || tnode.name == "dict" || symbols.types.find(tnode.name) != symbols.types.end()) {
        elem = mapTypeNode(arena, symbols, currentModuleId, tnode) + "{}";
    }
    args = elem + ", " + std::string(errVar);
    return "return { " + args + " };";
}

inline void emitStmt(
    const AstArena& arena,
    const GlobalSymbolTable& symbols,
    const FnDecl& fn,
    StmtId stmtId,
    std::unordered_map<std::string, VarSymbol>& scope,
    std::unordered_set<std::string>& declaredVars,
    EmitterState& state
);

inline void emitStmtList(
    const AstArena& arena,
    const GlobalSymbolTable& symbols,
    const FnDecl& fn,
    const std::vector<StmtId>& stmts,
    std::unordered_map<std::string, VarSymbol>& scope,
    std::unordered_set<std::string>& declaredVars,
    EmitterState& state
) {
    const std::optional<ErrorBlock> savedErrorBlock = state.activeErrorBlock;
    for (const StmtId sid : stmts) {
        emitStmt(arena, symbols, fn, sid, scope, declaredVars, state);
    }
    state.activeErrorBlock = savedErrorBlock;
}

inline void emitActiveErrorHandlerOrBubble(
    const AstArena& arena,
    const GlobalSymbolTable& symbols,
    const FnDecl& fn,
    const std::string& errTmp,
    std::unordered_map<std::string, VarSymbol>& scope,
    std::unordered_set<std::string>& declaredVars,
    EmitterState& state
) {
    if (state.activeErrorBlock.has_value()) {
        const ErrorBlock activeEb = state.activeErrorBlock.value();
        emitLine(state, "if (cbHasError(" + errTmp + ")) {");
        state.indentLevel += 1;
        emitLine(state, "CbError " + activeEb.errVar + " = " + errTmp + ";");
        auto errScope = scope;
        TypeId errTid = symbols.typeErr;
        if (errTid == INVALID_ID) {
            errTid = findType(arena, "err");
        }
        errScope[activeEb.errVar] = VarSymbol{activeEb.errVar, errTid, false, false};
        auto errDecls = declaredVars;
        errDecls.insert(activeEb.errVar);
        const std::optional<ErrorBlock> prevEb = state.activeErrorBlock;
        state.activeErrorBlock = std::nullopt;
        emitStmtList(arena, symbols, fn, activeEb.body, errScope, errDecls, state);
        state.activeErrorBlock = prevEb;
        state.indentLevel -= 1;
        emitLine(state, "}");
    } else {
        const std::string earlyRet = buildErrorBubbleReturn(arena, symbols, state.currentModuleId, fn, errTmp);
        emitLine(state, "if (cbHasError(" + errTmp + ")) " + earlyRet);
    }
}

inline void emitFallibleAssignment(
    const AstArena& arena,
    const GlobalSymbolTable& symbols,
    const FnDecl& fn,
    const VarAssignment& stmt,
    const FnSymbol& fnSym,
    std::unordered_map<std::string, VarSymbol>& scope,
    std::unordered_set<std::string>& declaredVars,
    EmitterState& state
) {
    const std::string callCpp = emitExpr(arena, symbols, stmt.value, scope, state);
    state.valCounter += 1;
    const uint32_t cid = state.valCounter;

    const bool hasValue = (fnSym.returnType != INVALID_ID && !isVoidType(arena, fnSym.returnType));
    const std::string valTmp = "_val" + std::to_string(cid);
    const std::string errVar = "_err" + std::to_string(cid);

    if (hasValue) {
        emitLine(state, "auto [" + valTmp + ", " + errVar + "] = " + callCpp + ";");
    } else {
        emitLine(state, "auto " + errVar + " = " + callCpp + ";");
    }

    emitActiveErrorHandlerOrBubble(arena, symbols, fn, errVar, scope, declaredVars, state);

    if (stmt.target != INVALID_ID) {
        const ExprId tid = stmt.target;
        if (tid < arena.exprs.size() && std::holds_alternative<Identifier>(arena.exprs[tid].payload)) {
            const std::string& name = std::get<Identifier>(arena.exprs[tid].payload).name;
            TypeId retT = symbols.typeAuto;
            if (stmt.hasDeclaredType) {
                retT = resolveAlias(arena, symbols, state.currentModuleId, stmt.declaredType);
            } else if (hasValue) {
                retT = resolveAlias(arena, symbols, fnSym.moduleId, fnSym.returnType);
            }
            const bool isNewDecl = stmt.isDim || stmt.isConst || (declaredVars.find(name) == declaredVars.end());
            scope[name] = VarSymbol{name, retT, false, stmt.isConst};
            declaredVars.insert(name);
            std::string rhs = "0";
            if (hasValue) {
                rhs = valTmp;
            }
            if (isNewDecl) {
                std::string prefix = "auto ";
                if (stmt.isConst) {
                    prefix = "const auto ";
                }
                emitLine(state, prefix + name + " = " + rhs + ";");
            } else {
                emitLine(state, name + " = " + rhs + ";");
            }
        } else {
            const std::string lhsCpp = emitExpr(arena, symbols, tid, scope, state);
            std::string rhs = "0";
            if (hasValue) {
                rhs = valTmp;
            }
            emitLine(state, lhsCpp + " = " + rhs + ";");
        }
    }
}

inline void emitVarAssignment(
    const AstArena& arena,
    const GlobalSymbolTable& symbols,
    const FnDecl& fn,
    const VarAssignment& stmt,
    std::unordered_map<std::string, VarSymbol>& scope,
    std::unordered_set<std::string>& declaredVars,
    EmitterState& state
) {
    if (stmt.value < arena.exprs.size() && std::holds_alternative<FuncCall>(arena.exprs[stmt.value].payload)) {
        const auto& call = std::get<FuncCall>(arena.exprs[stmt.value].payload);
        const auto fnSym = resolveFuncCall(arena, call, symbols, state.currentModuleId, scope);
        if (fnSym.has_value() && fnSym->isFallible) {
            emitFallibleAssignment(arena, symbols, fn, stmt, *fnSym, scope, declaredVars, state);
            return;
        }
    }

    std::string valCpp = emitExpr(arena, symbols, stmt.value, scope, state);
    const TypeId valType = inferExprType(arena, stmt.value, symbols, scope, state.currentModuleId);

    if (stmt.hasDeclaredType && stmt.target != INVALID_ID) {
        const ExprId targetId = stmt.target;
        if (targetId < arena.exprs.size() && std::holds_alternative<Identifier>(arena.exprs[targetId].payload)) {
            const std::string& name = std::get<Identifier>(arena.exprs[targetId].payload).name;
            const TypeId resolvedT = resolveAlias(arena, symbols, state.currentModuleId, stmt.declaredType);
            const TypeNode resNode = resolveAliasNode(arena, symbols, state.currentModuleId, stmt.declaredType);
            scope[name] = VarSymbol{name, resolvedT, false};
            declaredVars.insert(name);
            const std::string cppType = mapTypeNode(arena, symbols, state.currentModuleId, resNode);
            std::string prefix;
            if (stmt.isConst) {
                prefix = "const ";
            }
            emitLine(state, prefix + cppType + " " + name + " = " + valCpp + ";");
            return;
        }
    }

    if (stmt.target != INVALID_ID) {
        const ExprId targetId = stmt.target;
        TypeId inferredT = symbols.typeAuto;
        if (valType != INVALID_ID) {
            inferredT = valType;
        }

        if (targetId < arena.exprs.size() && std::holds_alternative<Identifier>(arena.exprs[targetId].payload)) {
            const std::string& name = std::get<Identifier>(arena.exprs[targetId].payload).name;
            if (stmt.isDim || stmt.isConst || declaredVars.find(name) == declaredVars.end()) {
                scope[name] = VarSymbol{name, inferredT, false, stmt.isConst};
                declaredVars.insert(name);
                std::string inferredName = "auto";
                if (inferredT < arena.types.size()) {
                    inferredName = arena.types[inferredT].name;
                }
                std::string cppT = "auto";
                if (inferredName != "auto") {
                    cppT = mapType(arena, symbols, state.currentModuleId, inferredT);
                }
                std::string prefix;
                if (stmt.isConst) {
                    prefix = "const ";
                }
                emitLine(state, prefix + cppT + " " + name + " = " + valCpp + ";");
            } else {
                emitLine(state, name + " = " + valCpp + ";");
            }
        } else {
            const std::string lhsCpp = emitExpr(arena, symbols, targetId, scope, state);
            emitLine(state, lhsCpp + " = " + valCpp + ";");
        }
    }
}

inline void emitExprStmt(
    const AstArena& arena,
    const GlobalSymbolTable& symbols,
    const FnDecl& fn,
    const ExprStmt& stmt,
    std::unordered_map<std::string, VarSymbol>& scope,
    std::unordered_set<std::string>& declaredVars,
    EmitterState& state
) {
    if (stmt.expr < arena.exprs.size() && std::holds_alternative<FuncCall>(arena.exprs[stmt.expr].payload)) {
        const auto& call = std::get<FuncCall>(arena.exprs[stmt.expr].payload);
        if (isErrorBuiltinCall(arena, call)) {
            const std::string errVal = buildErrorCallValue(arena, symbols, call, scope, state);
            emitLine(state, buildErrorBubbleReturn(arena, symbols, state.currentModuleId, fn, errVal));
            return;
        }
        const auto fnSym = resolveFuncCall(arena, call, symbols, state.currentModuleId, scope);
        if (fnSym.has_value() && fnSym->isFallible) {
            const std::string callCpp = emitExpr(arena, symbols, stmt.expr, scope, state);
            state.valCounter += 1;
            const std::string errVar = "_err" + std::to_string(state.valCounter);

            if (fnSym->returnType != INVALID_ID && !isVoidType(arena, fnSym->returnType)) {
                std::string discard = nextTemp(state, "_discard");
                emitLine(state, "auto [" + discard + ", " + errVar + "] = " + callCpp + ";");
            } else {
                emitLine(state, "auto " + errVar + " = " + callCpp + ";");
            }

            emitActiveErrorHandlerOrBubble(arena, symbols, fn, errVar, scope, declaredVars, state);
            return;
        }
    }

    const std::string exprCpp = emitExpr(arena, symbols, stmt.expr, scope, state);
    emitLine(state, exprCpp + ";");
}

inline void emitReturn(
    const AstArena& arena,
    const GlobalSymbolTable& symbols,
    const FnDecl& fn,
    const ReturnStmt& stmt,
    const std::unordered_map<std::string, VarSymbol>& scope,
    EmitterState& state
) {
    if (!stmt.hasValue) {
        if (fn.isFallible && (fn.returnType == INVALID_ID || isVoidType(arena, fn.returnType))) {
            emitLine(state, R"(return CbError{"", 0};)");
        } else {
            emitLine(state, "return;");
        }
        return;
    }

    const ExprId firstId = stmt.value;
    if (firstId < arena.exprs.size() && std::holds_alternative<FuncCall>(arena.exprs[firstId].payload)) {
        const auto& call = std::get<FuncCall>(arena.exprs[firstId].payload);
        if (isErrorBuiltinCall(arena, call)) {
            const std::string errVal = buildErrorCallValue(arena, symbols, call, scope, state);
            emitLine(state, buildErrorBubbleReturn(arena, symbols, state.currentModuleId, fn, errVal));
            return;
        }
    }

    std::string valCpp = emitExpr(arena, symbols, stmt.value, scope, state);
    if (stmt.value < arena.exprs.size() && std::holds_alternative<NullLiteral>(arena.exprs[stmt.value].payload) &&
        fn.returnType != INVALID_ID) {
        const TypeNode tnode = resolveAliasNode(arena, symbols, state.currentModuleId, fn.returnType);
        if (symbols.types.find(tnode.name) != symbols.types.end()) {
            valCpp = mapTypeNode(arena, symbols, state.currentModuleId, tnode) + "{}";
        }
    }

    if (fn.isFallible) {
        emitLine(state, "return { " + valCpp + R"(, CbError{"", 0} };)");
    } else {
        emitLine(state, "return " + valCpp + ";");
    }
}

inline void emitStmt(
    const AstArena& arena,
    const GlobalSymbolTable& symbols,
    const FnDecl& fn,
    StmtId stmtId,
    std::unordered_map<std::string, VarSymbol>& scope,
    std::unordered_set<std::string>& declaredVars,
    EmitterState& state
) {
    if (stmtId >= arena.stmts.size()) {
        return;
    }

    const auto& stmtNode = arena.stmts[stmtId];
    const auto& payload = stmtNode.payload;

    if (std::holds_alternative<VarAssignment>(payload)) {
        emitVarAssignment(arena, symbols, fn, std::get<VarAssignment>(payload), scope, declaredVars, state);
        return;
    }

    if (std::holds_alternative<ExprStmt>(payload)) {
        emitExprStmt(arena, symbols, fn, std::get<ExprStmt>(payload), scope, declaredVars, state);
        return;
    }

    if (std::holds_alternative<IfStmt>(payload)) {
        const auto& ifS = std::get<IfStmt>(payload);
        const std::string condCpp = emitExpr(arena, symbols, ifS.condition, scope, state);
        emitLine(state, "if (" + condCpp + ") {");
        state.indentLevel += 1;
        auto thenDecls = declaredVars;
        emitStmtList(arena, symbols, fn, ifS.thenBody, scope, thenDecls, state);
        state.indentLevel -= 1;

        for (const auto& branch : ifS.elifBranches) {
            const std::string elifCond = emitExpr(arena, symbols, branch.condition, scope, state);
            emitLine(state, "} else if (" + elifCond + ") {");
            state.indentLevel += 1;
            auto elifDecls = declaredVars;
            emitStmtList(arena, symbols, fn, branch.body, scope, elifDecls, state);
            state.indentLevel -= 1;
        }

        if (ifS.hasElse) {
            emitLine(state, "} else {");
            state.indentLevel += 1;
            auto elseDecls = declaredVars;
            emitStmtList(arena, symbols, fn, ifS.elseBody, scope, elseDecls, state);
            state.indentLevel -= 1;
        }

        emitLine(state, "}");
        return;
    }

    if (std::holds_alternative<ForStmt>(payload)) {
        const auto& forS = std::get<ForStmt>(payload);
        if (forS.rangeSpec.hasRange && !forS.loopVars.empty()) {
            const std::string& loopVar = forS.loopVars[0];
            const std::string startCpp = emitExpr(arena, symbols, forS.rangeSpec.start, scope, state);
            const std::string endCpp = emitExpr(arena, symbols, forS.rangeSpec.end, scope, state);
            std::string stepCpp = "1";
            if (forS.rangeSpec.hasStep) {
                stepCpp = emitExpr(arena, symbols, forS.rangeSpec.step, scope, state);
            }
            std::string cmpOp = "<";
            if (forS.rangeSpec.inclusive) {
                cmpOp = "<=";
            }

            auto loopScope = scope;
            TypeId intTid = symbols.typeInt;
            if (intTid == INVALID_ID) {
                intTid = findType(arena, "int");
            }
            loopScope[loopVar] = VarSymbol{loopVar, intTid, false};
            auto loopDecls = declaredVars;
            loopDecls.insert(loopVar);

            emitLine(state, "for (int " + loopVar + " = " + startCpp + "; " + loopVar + " " + cmpOp + " " + endCpp + "; " + loopVar + " += " + stepCpp + ") {");
            state.indentLevel += 1;
            emitStmtList(arena, symbols, fn, forS.body, loopScope, loopDecls, state);
            state.indentLevel -= 1;
            emitLine(state, "}");
            return;
        }

        if (forS.hasIterable) {
            std::string iterCpp = emitExpr(arena, symbols, forS.iterable, scope, state);
            const TypeId iterTid = inferExprType(arena, forS.iterable, symbols, scope, state.currentModuleId);
            const TypeNode& iterType = getTypeNode(arena, iterTid);

            auto loopScope = scope;
            auto loopDecls = declaredVars;
            for (const auto& lv : forS.loopVars) {
                loopDecls.insert(lv);
            }

            if (forS.loopVars.size() == 1) {
                const std::string& lv = forS.loopVars[0];
                TypeId elemT = symbols.typeAuto;
                if (!iterType.typeArgs.empty()) {
                    elemT = iterType.typeArgs[0];
                }
                loopScope[lv] = VarSymbol{lv, elemT, false};
                emitLine(state, "for (auto& " + lv + " : " + iterCpp + ") {");
            } else {
                if (iterType.typeArgs.size() == 2) {
                    loopScope[forS.loopVars[0]] = VarSymbol{forS.loopVars[0], iterType.typeArgs[0], false};
                    loopScope[forS.loopVars[1]] = VarSymbol{forS.loopVars[1], iterType.typeArgs[1], false};
                }
                emitLine(state, "for (auto& [" + joinStrings(forS.loopVars) + "] : " + iterCpp + ") {");
            }

            state.indentLevel += 1;
            emitStmtList(arena, symbols, fn, forS.body, loopScope, loopDecls, state);
            state.indentLevel -= 1;
            emitLine(state, "}");
            return;
        }
    }

    if (std::holds_alternative<WhileStmt>(payload)) {
        const auto& whileS = std::get<WhileStmt>(payload);
        std::optional<FnSymbol> fallibleCondFn = std::nullopt;
        if (whileS.condition < arena.exprs.size() &&
            std::holds_alternative<FuncCall>(arena.exprs[whileS.condition].payload)) {
            const auto& call = std::get<FuncCall>(arena.exprs[whileS.condition].payload);
            const auto fnOpt = resolveFuncCall(arena, call, symbols, state.currentModuleId, scope);
            if (fnOpt.has_value() && fnOpt->isFallible) {
                fallibleCondFn = fnOpt;
            }
        }
        auto whileScope = scope;
        auto whileDecls = declaredVars;
        if (fallibleCondFn.has_value()) {
            emitLine(state, "while (true) {");
            state.indentLevel += 1;
            const std::string callCpp = emitExpr(arena, symbols, whileS.condition, whileScope, state);
            state.valCounter += 1;
            const uint32_t cid = state.valCounter;
            const std::string valTmp = "_val" + std::to_string(cid);
            const std::string errVar = "_err" + std::to_string(cid);
            emitLine(state, "auto [" + valTmp + ", " + errVar + "] = " + callCpp + ";");
            emitActiveErrorHandlerOrBubble(arena, symbols, fn, errVar, whileScope, whileDecls, state);
            emitLine(state, "if (!" + valTmp + ") { break; }");
            emitStmtList(arena, symbols, fn, whileS.body, whileScope, whileDecls, state);
            state.indentLevel -= 1;
            emitLine(state, "}");
        } else {
            const std::string condCpp = emitExpr(arena, symbols, whileS.condition, scope, state);
            emitLine(state, "while (" + condCpp + ") {");
            state.indentLevel += 1;
            emitStmtList(arena, symbols, fn, whileS.body, whileScope, whileDecls, state);
            state.indentLevel -= 1;
            emitLine(state, "}");
        }
        return;
    }

    if (std::holds_alternative<SelectStmt>(payload)) {
        const auto& selS = std::get<SelectStmt>(payload);
        const std::string targetCpp = emitExpr(arena, symbols, selS.target, scope, state);
        emitLine(state, "switch (" + targetCpp + ") {");
        state.indentLevel += 1;

        for (size_t idx = 0; idx < selS.cases.size(); ++idx) {
            const auto& c = selS.cases[idx];
            const std::string matchCpp = emitExpr(arena, symbols, c.matchExpr, scope, state);
            emitLine(state, "case " + matchCpp + ": {");
            state.indentLevel += 1;
            auto cDecls = declaredVars;
            emitStmtList(arena, symbols, fn, c.body, scope, cDecls, state);
            emitLine(state, "break;");
            state.indentLevel -= 1;
            emitLine(state, "}");
        }

        if (selS.hasCaseElse) {
            emitLine(state, "default: {");
            state.indentLevel += 1;
            auto elseDecls = declaredVars;
            emitStmtList(arena, symbols, fn, selS.caseElse, scope, elseDecls, state);
            emitLine(state, "break;");
            state.indentLevel -= 1;
            emitLine(state, "}");
        }

        state.indentLevel -= 1;
        emitLine(state, "}");
        return;
    }

    if (std::holds_alternative<ErrorBlock>(payload)) {
        state.activeErrorBlock = std::get<ErrorBlock>(payload);
        return;
    }

    if (std::holds_alternative<DeferStmt>(payload)) {
        const auto& defS = std::get<DeferStmt>(payload);
        state.deferCounter += 1;
        const std::string deferVar = "_defer_" + std::to_string(state.deferCounter);
        emitLine(state, "auto " + deferVar + " = cbDefer([&]() {");
        state.indentLevel += 1;
        auto defDecls = declaredVars;
        const std::optional<ErrorBlock> prevEb = state.activeErrorBlock;
        state.activeErrorBlock = std::nullopt;
        emitStmtList(arena, symbols, fn, defS.body, scope, defDecls, state);
        state.activeErrorBlock = prevEb;
        state.indentLevel -= 1;
        emitLine(state, "});");
        return;
    }



    if (std::holds_alternative<ReturnStmt>(payload)) {
        emitReturn(arena, symbols, fn, std::get<ReturnStmt>(payload), scope, state);
        return;
    }

    if (std::holds_alternative<BreakStmt>(payload)) {
        emitLine(state, "break;");
        return;
    }
}

inline void emitStructDecl(
    const AstArena& arena,
    const GlobalSymbolTable& symbols,
    const TypeDecl& td,
    EmitterState& state
) {
    const std::string key = resolveSymbolKey(symbols, state.currentModuleId, td.name);
    std::string cppName = td.name;
    if (symbols.types.count(key) > 0) {
        cppName = symbols.types.at(key).cppName;
    }
    emitLine(state, "struct " + cppName + " {");
    state.indentLevel += 1;
    for (const auto& f : td.fields) {
        emitLine(state, mapType(arena, symbols, state.currentModuleId, f.typeId) + " " + f.name + "{};");
    }
    state.indentLevel -= 1;
    emitLine(state, "};");
}

inline void emitFnDecl(
    const AstArena& arena,
    const GlobalSymbolTable& symbols,
    const FnDecl& fn,
    EmitterState& state
) {
    emitLine(state, fnSignature(arena, symbols, state.currentModuleId, fn) + " {");
    state.indentLevel += 1;

    std::unordered_map<std::string, VarSymbol> scope;
    const auto fnScopeIt = symbols.fnScopes.find(symbolKey(state.currentModuleId, fn.name));
    if (fnScopeIt != symbols.fnScopes.end()) {
        scope = fnScopeIt->second;
    }
    std::unordered_set<std::string> declaredVars;
    for (const auto& p : fn.params) {
        if (scope.find(p.name) == scope.end()) {
            const TypeId resolvedT = resolveAlias(arena, symbols, state.currentModuleId, p.typeId);
            scope[p.name] = VarSymbol{p.name, resolvedT, p.isByref};
        }
        declaredVars.insert(p.name);
    }

    emitStmtList(arena, symbols, fn, fn.body, scope, declaredVars, state);

    if (fn.isFallible && (fn.returnType == INVALID_ID || isVoidType(arena, fn.returnType))) {
        bool endsWithReturn = false;
        if (!fn.body.empty()) {
            const StmtId lastSid = fn.body.back();
            if (lastSid < arena.stmts.size()) {
                const auto& lastPayload = arena.stmts[lastSid].payload;
                if (std::holds_alternative<ReturnStmt>(lastPayload)) {
                    endsWithReturn = true;
                } else if (std::holds_alternative<ExprStmt>(lastPayload)) {
                    const auto& es = std::get<ExprStmt>(lastPayload);
                    if (es.expr < arena.exprs.size() && std::holds_alternative<FuncCall>(arena.exprs[es.expr].payload)) {
                        if (isErrorBuiltinCall(arena, std::get<FuncCall>(arena.exprs[es.expr].payload))) {
                            endsWithReturn = true;
                        }
                    }
                }
            }
        }
        if (!endsWithReturn) {
            emitLine(state, R"(return CbError{"", 0};)");
        }
    }

    state.indentLevel -= 1;
    emitLine(state, "}");
}

inline std::string emitProgram(
    const AstArena& arena,
    const GlobalSymbolTable& symbols,
    const Program& program
) {
    EmitterState state{};

    if (!symbols.linkLibs.empty()) {
        std::string flags;
        for (size_t i = 0; i < symbols.linkLibs.size(); ++i) {
            if (i > 0) flags += " ";
            flags += "-l" + symbols.linkLibs[i];
        }
        emitLine(state, "// CleanBasic Linker Flags: " + flags);
    }

    state.lines.push_back(std::string(CPP_RUNTIME_PREAMBLE));

    if (!symbols.headers.empty()) {
        emitLine(state);
        for (const auto& hdr : symbols.headers) {
            emitLine(state, R"(#include ")" + hdr + R"(")");
        }
    }

    std::vector<std::pair<std::string, TypeDecl>> structDecls;
    std::vector<std::pair<std::string, AliasDecl>> aliasDecls;
    std::vector<std::pair<std::string, FnDecl>> fnDecls;
    for (const DeclId did : program.declarations) {
        if (did >= arena.decls.size()) continue;
        const auto& declNode = arena.decls[did];
        if (std::holds_alternative<TypeDecl>(declNode.payload)) {
            const auto& td = std::get<TypeDecl>(declNode.payload);
            if (!td.isExtern) structDecls.push_back({declNode.moduleId, td});
        } else if (std::holds_alternative<AliasDecl>(declNode.payload)) {
            aliasDecls.push_back({declNode.moduleId, std::get<AliasDecl>(declNode.payload)});
        } else if (std::holds_alternative<FnDecl>(declNode.payload)) {
            const auto& fd = std::get<FnDecl>(declNode.payload);
            if (!fd.isExtern) fnDecls.push_back({declNode.moduleId, fd});
        }
    }

    if (!structDecls.empty() || !aliasDecls.empty()) {
        emitLine(state);
        for (const auto& pair : structDecls) {
            state.currentModuleId = pair.first;
            const std::string key = resolveSymbolKey(symbols, state.currentModuleId, pair.second.name);
            std::string cppName = pair.second.name;
            if (symbols.types.count(key) > 0) {
                cppName = symbols.types.at(key).cppName;
            }
            emitLine(state, "struct " + cppName + ";");
        }
        for (const auto& pair : aliasDecls) {
            state.currentModuleId = pair.first;
            const std::string key = resolveSymbolKey(symbols, state.currentModuleId, pair.second.name);
            std::string cppName = pair.second.name;
            if (symbols.aliases.count(key) > 0) {
                cppName = symbols.aliases.at(key).cppName;
            }
            emitLine(state, "using " + cppName + " = " + mapType(arena, symbols, state.currentModuleId, pair.second.targetType) + ";");
        }
    }

    if (!structDecls.empty()) {
        emitLine(state);
        for (const auto& pair : structDecls) {
            state.currentModuleId = pair.first;
            emitStructDecl(arena, symbols, pair.second, state);
            emitLine(state);
        }
    }

    if (!fnDecls.empty()) {
        emitLine(state, "// Forward Declarations");
        for (const auto& pair : fnDecls) {
            state.currentModuleId = pair.first;
            emitLine(state, fnSignature(arena, symbols, state.currentModuleId, pair.second) + ";");
        }
        emitLine(state);
    }

    for (const auto& pair : fnDecls) {
        state.currentModuleId = pair.first;
        emitFnDecl(arena, symbols, pair.second, state);
        emitLine(state);
    }

    const auto mainIt = symbols.functions.find(symbolKey("", "main"));
    if (mainIt != symbols.functions.end()) {
        const FnSymbol& mainSym = mainIt->second;
        const bool mainReturnsInt = (mainSym.returnType == symbols.typeInt);
        emitLine(state, "int main(int argc, char** argv) {");
        state.indentLevel += 1;
        emitLine(state, "auto& _cbArgs = cbRuntimeArgs();");
        emitLine(state, "_cbArgs.clear();");
        emitLine(state, "for (int _i = 0; _i < argc; ++_i) {");
        state.indentLevel += 1;
        emitLine(state, "_cbArgs.emplace_back(argv[_i]);");
        state.indentLevel -= 1;
        emitLine(state, "}");
        if (mainSym.isFallible && mainReturnsInt) {
            emitLine(state, "auto [_mainRc, _mainErr] = cbMain();");
            emitLine(state, "if (cbHasError(_mainErr)) {");
            state.indentLevel += 1;
            emitLine(state, R"(std::cerr << "Unhandled error (code=" << _mainErr.code << "): " << _mainErr.msg << std::endl;)");
            emitLine(state, "return 1;");
            state.indentLevel -= 1;
            emitLine(state, "}");
            emitLine(state, "return _mainRc;");
        } else if (mainSym.isFallible) {
            emitLine(state, "auto _mainErr = cbMain();");
            emitLine(state, "if (cbHasError(_mainErr)) {");
            state.indentLevel += 1;
            emitLine(state, R"(std::cerr << "Unhandled error (code=" << _mainErr.code << "): " << _mainErr.msg << std::endl;)");
            emitLine(state, "return 1;");
            state.indentLevel -= 1;
            emitLine(state, "}");
            emitLine(state, "return 0;");
        } else if (mainReturnsInt) {
            emitLine(state, "return cbMain();");
        } else {
            emitLine(state, "cbMain();");
            emitLine(state, "return 0;");
        }
        state.indentLevel -= 1;
        emitLine(state, "}");
    }

    std::string out;
    for (const auto& line : state.lines) {
        out += line;
        out += "\n";
    }
    return out;
}

} // namespace cb

#endif // CLEANBASIC_EMITTER_HPP

