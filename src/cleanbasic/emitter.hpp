#ifndef CLEANBASIC_EMITTER_HPP
#define CLEANBASIC_EMITTER_HPP

#include "src/cleanbasic/emitter_types.hpp"
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cb {

inline std::string Emitter::emitAsStrExpr(
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

inline std::vector<std::string> Emitter::emitExprList(
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

inline std::vector<std::string> Emitter::orderStructFields(
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

inline std::string Emitter::emitCallArgsJoined(
    const FnSymbol& fnSym,
    const std::vector<ExprId>& posArgs,
    const std::vector<NamedArg>& namedArgs,
    const std::unordered_map<std::string, VarSymbol>& scope
) {
    if (fnSym.params.empty() || (namedArgs.empty() && posArgs.size() >= fnSym.params.size())) {
        std::vector<std::string> rendered = emitExprList(posArgs, scope);
        rendered.reserve(posArgs.size() + namedArgs.size());
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
            rendered.push_back(emitExpr(posArgs[i], scope));
        } else {
            const auto nit = namedMap.find(p.name);
            if (nit != namedMap.end()) {
                rendered.push_back(emitExpr(nit->second, scope));
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

inline std::string Emitter::emitExpr(
    ExprId exprId,
    const std::unordered_map<std::string, VarSymbol>& scope
) {
    if (exprId >= arena.exprs.size()) {
        return "";
    }

    if (const auto* num = arena.getExpr<NumberLiteral>(exprId)) {
        return num->raw;
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
        return ident->name;
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
        return emitExpr(faccess->target, scope) + "." + faccess->fieldName;
    }

    if (const auto* iaccess = arena.getExpr<IndexAccess>(exprId)) {
        const TypeId tgtTid = inferExprType(arena, iaccess->target, symbols, scope, currentModuleId);
        const TypeNode resolvedTgt = resolveAliasNode(arena, symbols, currentModuleId, tgtTid);
        const std::string tgtCpp = emitExpr(iaccess->target, scope);
        const std::string idxCpp = emitExpr(iaccess->index, scope);
        if (resolvedTgt.name == "str") {
            return "cbStrIndex(" + tgtCpp + ", " + idxCpp + ")";
        }
        if (resolvedTgt.name == "dict") {
            bool isConstDict = false;
            if (const auto* ident = arena.getExpr<Identifier>(iaccess->target)) {
                const auto it = scope.find(ident->name);
                if (it != scope.end() && it->second.isConst) {
                    isConstDict = true;
                } else if (resolveConstSymbol(symbols, currentModuleId, scope, ident->name).has_value()) {
                    isConstDict = true;
                }
            } else if (const auto* fa = arena.getExpr<FieldAccess>(iaccess->target)) {
                if (resolveModuleConstAccess(arena, symbols, currentModuleId, scope, *fa).has_value()) {
                    isConstDict = true;
                }
            }
            if (isConstDict) {
                return tgtCpp + ".at(" + idxCpp + ")";
            }
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
        if (call->callee >= arena.exprs.size()) {
            return "";
        }
        const auto structOpt = resolveStructConstructor(arena, *call, symbols, currentModuleId, scope);
        if (structOpt.has_value()) {
            const auto ordered = orderStructFields(structOpt->key, call->args, call->namedArgs, scope);
            return structOpt->cppName + "{" + joinStrings(ordered) + "}";
        }

        const auto fnSym = resolveFuncCall(arena, *call, symbols, currentModuleId, scope);
        if (fnSym.has_value()) {
            if (fnSym->isBuiltinReceiver) {
                const auto* access = arena.getExpr<FieldAccess>(call->callee);
                const std::string recvCpp = emitExpr(access->target, scope);
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
                    return "cbBytesSlice(" + recvCpp + ", " + emitExpr(call->args[0], scope) + ", " + emitExpr(call->args[1], scope) + ")";
                } else if (fnSym->name == "sliceFrom") {
                    return "cbBytesSliceFrom(" + recvCpp + ", " + emitExpr(call->args[0], scope) + ")";
                } else if (fnSym->name == "toUtf8") {
                    return "cbBytesToUtf8(" + recvCpp + ")";
                } else if (fnSym->name == "toBytes") {
                    return "cbStrToBytes(" + recvCpp + ")";
                } else if (fnSym->name == "byteAt") {
                    return "cbStrByteAtFast(" + recvCpp + ", " + emitExpr(call->args[0], scope) + ")";
                } else if (fnSym->name == "toHex") {
                    return "cbBytesToHex(" + recvCpp + ")";
                } else if (fnSym->name == "fromHex") {
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
            if (const auto* access = arena.getExpr<FieldAccess>(call->callee)) {
                std::string targetModId;
                bool isModCall = false;
                if (const auto* tgtIdent = arena.getExpr<Identifier>(access->target)) {
                    if (isModuleAliasInScope(symbols, currentModuleId, tgtIdent->name, scope, targetModId)) {
                        isModCall = true;
                    }
                }

                if (!isModCall) {
                    // UFCS
                    std::vector<ExprId> allPos;
                    allPos.reserve(1 + call->args.size());
                    allPos.push_back(access->target);
                    for (const ExprId a : call->args) {
                        allPos.push_back(a);
                    }
                    return fnSym->cppName + "(" + emitCallArgsJoined(*fnSym, allPos, call->namedArgs, scope) + ")";
                }
            }

            return fnSym->cppName + "(" + emitCallArgsJoined(*fnSym, call->args, call->namedArgs, scope) + ")";
        }

        const std::string calleeCpp = emitExpr(call->callee, scope);
        return calleeCpp + "(" + joinStrings(emitExprList(call->args, scope)) + ")";
    }

    return "";
}

inline std::string Emitter::buildErrorCallValue(
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
    return "CbError{" + msgCpp + ", " + codeCpp + "}";
}

inline std::string Emitter::buildErrorBubbleReturn(
    const FnDecl& fn,
    std::string_view errVar
) {
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

inline void Emitter::emitStmtList(
    const FnDecl& fn,
    const std::vector<StmtId>& stmts,
    std::unordered_map<std::string, VarSymbol>& scope,
    std::unordered_set<std::string>& declaredVars
) {
    const std::optional<ErrorBlock> savedErrorBlock = activeErrorBlock;
    for (const StmtId sid : stmts) {
        emitStmt(fn, sid, scope, declaredVars);
    }
    activeErrorBlock = savedErrorBlock;
}

inline void Emitter::emitActiveErrorHandlerOrBubble(
    const FnDecl& fn,
    const std::string& errTmp,
    std::unordered_map<std::string, VarSymbol>& scope,
    std::unordered_set<std::string>& declaredVars
) {
    if (activeErrorBlock.has_value()) {
        const ErrorBlock activeEb = activeErrorBlock.value();
        emitLine("if (cbHasError(" + errTmp + ")) {");
        indentLevel += 1;
        emitLine("CbError " + activeEb.errVar + " = " + errTmp + ";");
        auto errScope = scope;
        TypeId errTid = symbols.typeErr;
        if (errTid == INVALID_ID) {
            errTid = arena.findType("err");
        }
        errScope[activeEb.errVar] = VarSymbol{activeEb.errVar, errTid, false, false};
        auto errDecls = declaredVars;
        errDecls.insert(activeEb.errVar);
        const std::optional<ErrorBlock> prevEb = activeErrorBlock;
        activeErrorBlock = std::nullopt;
        emitStmtList(fn, activeEb.body, errScope, errDecls);
        activeErrorBlock = prevEb;
        indentLevel -= 1;
        emitLine("}");
    } else {
        const std::string earlyRet = buildErrorBubbleReturn(fn, errTmp);
        emitLine("if (cbHasError(" + errTmp + ")) " + earlyRet);
    }
}

inline void Emitter::emitFallibleAssignment(
    const FnDecl& fn,
    const VarAssignment& stmt,
    const FnSymbol& fnSym,
    std::unordered_map<std::string, VarSymbol>& scope,
    std::unordered_set<std::string>& declaredVars
) {
    const std::string callCpp = emitExpr(stmt.value, scope);
    valCounter += 1;
    const uint32_t cid = valCounter;

    const bool hasValue = (fnSym.returnType != INVALID_ID && !arena.isVoid(fnSym.returnType));
    const std::string valTmp = "_val" + std::to_string(cid);
    const std::string errVar = "_err" + std::to_string(cid);

    if (hasValue) {
        emitLine("auto [" + valTmp + ", " + errVar + "] = " + callCpp + ";");
    } else {
        emitLine("auto " + errVar + " = " + callCpp + ";");
    }

    emitActiveErrorHandlerOrBubble(fn, errVar, scope, declaredVars);

    if (stmt.target != INVALID_ID) {
        const ExprId tid = stmt.target;
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
            std::string rhs = "0";
            if (hasValue) {
                rhs = valTmp;
            }
            if (isNewDecl) {
                std::string prefix = "auto ";
                if (stmt.isConst) {
                    prefix = "const auto ";
                }
                emitLine(prefix + name + " = " + rhs + ";");
            } else {
                emitLine(name + " = " + rhs + ";");
            }
        } else {
            const std::string lhsCpp = emitExpr(tid, scope);
            std::string rhs = "0";
            if (hasValue) {
                rhs = valTmp;
            }
            emitLine(lhsCpp + " = " + rhs + ";");
        }
    }
}

inline void Emitter::emitVarAssignment(
    const FnDecl& fn,
    const VarAssignment& stmt,
    std::unordered_map<std::string, VarSymbol>& scope,
    std::unordered_set<std::string>& declaredVars
) {
    if (const auto* call = arena.getExpr<FuncCall>(stmt.value)) {
        const auto fnSym = resolveFuncCall(arena, *call, symbols, currentModuleId, scope);
        if (fnSym.has_value() && fnSym->isFallible) {
            emitFallibleAssignment(fn, stmt, *fnSym, scope, declaredVars);
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
            scope[name] = VarSymbol{name, resolvedT, false};
            declaredVars.insert(name);
            const std::string cppType = mapTypeNode(currentModuleId, resNode);
            std::string prefix;
            if (stmt.isConst) {
                prefix = "const ";
            }
            emitLine(prefix + cppType + " " + name + " = " + valCpp + ";");
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
                emitLine(prefix + cppT + " " + name + " = " + valCpp + ";");
            } else {
                emitLine(name + " = " + valCpp + ";");
            }
        } else {
            const std::string lhsCpp = emitExpr(targetId, scope);
            emitLine(lhsCpp + " = " + valCpp + ";");
        }
    }
}

inline void Emitter::emitExprStmt(
    const FnDecl& fn,
    const ExprStmt& stmt,
    std::unordered_map<std::string, VarSymbol>& scope,
    std::unordered_set<std::string>& declaredVars
) {
    if (const auto* call = arena.getExpr<FuncCall>(stmt.expr)) {
        if (isErrorBuiltinCall(arena, *call)) {
            const std::string errVal = buildErrorCallValue(*call, scope);
            emitLine(buildErrorBubbleReturn(fn, errVal));
            return;
        }
        const auto fnSym = resolveFuncCall(arena, *call, symbols, currentModuleId, scope);
        if (fnSym.has_value() && fnSym->isFallible) {
            const std::string callCpp = emitExpr(stmt.expr, scope);
            valCounter += 1;
            const std::string errVar = "_err" + std::to_string(valCounter);

            if (fnSym->returnType != INVALID_ID && !arena.isVoid(fnSym->returnType)) {
                std::string discard = nextTemp("_discard");
                emitLine("auto [" + discard + ", " + errVar + "] = " + callCpp + ";");
            } else {
                emitLine("auto " + errVar + " = " + callCpp + ";");
            }

            emitActiveErrorHandlerOrBubble(fn, errVar, scope, declaredVars);
            return;
        }
    }

    const std::string exprCpp = emitExpr(stmt.expr, scope);
    emitLine(exprCpp + ";");
}

inline void Emitter::emitReturn(
    const FnDecl& fn,
    const ReturnStmt& stmt,
    std::unordered_map<std::string, VarSymbol>& scope,
    std::unordered_set<std::string>& declaredVars
) {
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
        if (isErrorBuiltinCall(arena, *call)) {
            const std::string errVal = buildErrorCallValue(*call, scope);
            emitLine(buildErrorBubbleReturn(fn, errVal));
            return;
        }
        const auto fnSym = resolveFuncCall(arena, *call, symbols, currentModuleId, scope);
        if (fnSym.has_value() && fnSym->isFallible) {
            const std::string valTmp = nextTemp("_val");
            const std::string errTmp = nextTemp("_err");
            emitLine("auto [" + valTmp + ", " + errTmp + "] = " + emitExpr(stmt.value, scope) + ";");
            emitActiveErrorHandlerOrBubble(fn, errTmp, scope, declaredVars);
            if (fn.isFallible) {
                emitLine("return { " + valTmp + R"(, CbError{"", 0} };)");
            } else {
                emitLine("return " + valTmp + ";");
            }
            return;
        }
    }

    std::string valCpp = emitExpr(stmt.value, scope);
    if (arena.getExpr<NullLiteral>(stmt.value) != nullptr && fn.returnType != INVALID_ID) {
        const TypeNode tnode = resolveAliasNode(arena, symbols, currentModuleId, fn.returnType);
        if (symbols.types.find(tnode.name) != symbols.types.end()) {
            valCpp = mapTypeNode(currentModuleId, tnode) + "{}";
        }
    }

    if (fn.isFallible) {
        emitLine("return { " + valCpp + R"(, CbError{"", 0} };)");
    } else {
        emitLine("return " + valCpp + ";");
    }
}

inline void Emitter::emitStmt(
    const FnDecl& fn,
    StmtId stmtId,
    std::unordered_map<std::string, VarSymbol>& scope,
    std::unordered_set<std::string>& declaredVars
) {
    if (stmtId >= arena.stmts.size()) {
        return;
    }

    if (const auto* vassign = arena.getStmt<VarAssignment>(stmtId)) {
        emitVarAssignment(fn, *vassign, scope, declaredVars);
        return;
    }

    if (const auto* estmt = arena.getStmt<ExprStmt>(stmtId)) {
        emitExprStmt(fn, *estmt, scope, declaredVars);
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
        return;
    }

    if (const auto* forS = arena.getStmt<ForStmt>(stmtId)) {
        if (forS->rangeSpec.hasRange && !forS->loopVars.empty()) {
            const std::string& loopVar = forS->loopVars[0];
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
                std::string cmpOp = "<";
                if (forS->rangeSpec.inclusive) {
                    cmpOp = "<=";
                }
                condCpp = loopVar + " " + cmpOp + " " + endCpp;
            } else if (isKnownNegativeStep) {
                std::string cmpOp = ">";
                if (forS->rangeSpec.inclusive) {
                    cmpOp = ">=";
                }
                condCpp = loopVar + " " + cmpOp + " " + endCpp;
            } else {
                condCpp = "cbRangeCond(" + loopVar + ", " + endCpp + ", " + stepCpp + ")";
            }

            auto loopScope = scope;
            TypeId intTid = symbols.typeInt;
            if (intTid == INVALID_ID) {
                intTid = arena.findType("int");
            }
            loopScope[loopVar] = VarSymbol{loopVar, intTid, false};
            auto loopDecls = declaredVars;
            loopDecls.insert(loopVar);

            emitLine("for (int64_t " + loopVar + " = " + startCpp + "; " + condCpp + "; " + loopVar + " += " + stepCpp + ") {");
            indentLevel += 1;
            emitStmtList(fn, forS->body, loopScope, loopDecls);
            indentLevel -= 1;
            emitLine("}");
            return;
        }

        if (forS->hasIterable) {
            std::string iterCpp = emitExpr(forS->iterable, scope);
            const TypeId iterTid = inferExprType(arena, forS->iterable, symbols, scope, currentModuleId);
            const TypeNode iterType = resolveAliasNode(arena, symbols, currentModuleId, iterTid);

            auto loopScope = scope;
            auto loopDecls = declaredVars;
            for (const auto& lv : forS->loopVars) {
                loopDecls.insert(lv);
            }

            if (forS->loopVars.size() == 1) {
                const std::string& lv = forS->loopVars[0];
                TypeId elemT = symbols.typeAuto;
                if (!iterType.typeArgs.empty()) {
                    elemT = iterType.typeArgs[0];
                } else if (iterType.name == "bytes") {
                    elemT = symbols.typeInt;
                }
                loopScope[lv] = VarSymbol{lv, elemT, false};
                emitLine("for (auto& " + lv + " : " + iterCpp + ") {");
                indentLevel += 1;
                emitStmtList(fn, forS->body, loopScope, loopDecls);
                indentLevel -= 1;
                emitLine("}");
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
                loopScope[idxVar] = VarSymbol{idxVar, symbols.typeInt, false};
                loopScope[elemVar] = VarSymbol{elemVar, elemT, false};
                const std::string iterRef = nextTemp("_iter");
                emitLine("{");
                indentLevel += 1;
                emitLine("auto&& " + iterRef + " = " + iterCpp + ";");
                emitLine("for (int64_t " + idxVar + " = 0; " + idxVar + " < static_cast<int64_t>(" + iterRef + ".size()); ++" + idxVar + ") {");
                indentLevel += 1;
                if (iterType.name == "bytes") {
                    emitLine("int64_t " + elemVar + " = static_cast<int64_t>(" + iterRef + "[static_cast<size_t>(" + idxVar + ")]);");
                } else {
                    emitLine("auto& " + elemVar + " = " + iterRef + "[static_cast<size_t>(" + idxVar + ")];");
                }
                emitStmtList(fn, forS->body, loopScope, loopDecls);
                indentLevel -= 1;
                emitLine("}");
                indentLevel -= 1;
                emitLine("}");
                return;
            }
            if (iterType.typeArgs.size() == 2) {
                loopScope[forS->loopVars[0]] = VarSymbol{forS->loopVars[0], iterType.typeArgs[0], false};
                loopScope[forS->loopVars[1]] = VarSymbol{forS->loopVars[1], iterType.typeArgs[1], false};
            }
            emitLine("for (auto& [" + joinStrings(forS->loopVars) + "] : " + iterCpp + ") {");
            indentLevel += 1;
            emitStmtList(fn, forS->body, loopScope, loopDecls);
            indentLevel -= 1;
            emitLine("}");
            return;
        }
    }

    if (const auto* whileS = arena.getStmt<WhileStmt>(stmtId)) {
        std::optional<FnSymbol> fallibleCondFn = std::nullopt;
        if (const auto* call = arena.getExpr<FuncCall>(whileS->condition)) {
            const auto fnOpt = resolveFuncCall(arena, *call, symbols, currentModuleId, scope);
            if (fnOpt.has_value() && fnOpt->isFallible) {
                fallibleCondFn = fnOpt;
            }
        }
        auto whileScope = scope;
        auto whileDecls = declaredVars;
        if (fallibleCondFn.has_value()) {
            emitLine("while (true) {");
            indentLevel += 1;
            const std::string callCpp = emitExpr(whileS->condition, whileScope);
            valCounter += 1;
            const uint32_t cid = valCounter;
            const std::string valTmp = "_val" + std::to_string(cid);
            const std::string errVar = "_err" + std::to_string(cid);
            emitLine("auto [" + valTmp + ", " + errVar + "] = " + callCpp + ";");
            emitActiveErrorHandlerOrBubble(fn, errVar, whileScope, whileDecls);
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
        return;
    }

    if (const auto* selS = arena.getStmt<SelectStmt>(stmtId)) {
        const TypeId targetTid = inferExprType(arena, selS->target, symbols, scope, currentModuleId);
        const std::string targetCpp = emitExpr(selS->target, scope);

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
        return;
    }

    if (const auto* eb = arena.getStmt<ErrorBlock>(stmtId)) {
        activeErrorBlock = *eb;
        return;
    }

    if (const auto* defS = arena.getStmt<DeferStmt>(stmtId)) {
        deferCounter += 1;
        const std::string deferVar = "_defer_" + std::to_string(deferCounter);
        emitLine("auto " + deferVar + " = cbDefer([&]() {");
        indentLevel += 1;
        auto defDecls = declaredVars;
        const std::optional<ErrorBlock> prevEb = activeErrorBlock;
        activeErrorBlock = std::nullopt;
        emitStmtList(fn, defS->body, scope, defDecls);
        activeErrorBlock = prevEb;
        indentLevel -= 1;
        emitLine("});");
        return;
    }

    if (const auto* retS = arena.getStmt<ReturnStmt>(stmtId)) {
        emitReturn(fn, *retS, scope, declaredVars);
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

inline void Emitter::emitEnumDecl(const EnumDecl& ed) {
    const std::string key = resolveSymbolKey(symbols, currentModuleId, ed.name);
    std::string cppName = ed.name;
    if (symbols.enums.count(key) > 0) {
        cppName = symbols.enums.at(key).cppName;
    }
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
inline void Emitter::emitStructDecl(const TypeDecl& td) {
    const std::string key = resolveSymbolKey(symbols, currentModuleId, td.name);
    std::string cppName = td.name;
    if (symbols.types.count(key) > 0) {
        cppName = symbols.types.at(key).cppName;
    }
    emitLine("struct " + cppName + " {");
    indentLevel += 1;
    for (const auto& f : td.fields) {
        if (f.hasDefault && f.defaultValue != INVALID_ID) {
            std::unordered_map<std::string, VarSymbol> emptyScope;
            emitLine(mapType(currentModuleId, f.typeId) + " " + f.name + " = " + emitExpr(f.defaultValue, emptyScope) + ";");
        } else {
            emitLine(mapType(currentModuleId, f.typeId) + " " + f.name + "{};");
        }
    }
    indentLevel -= 1;
    emitLine("};");
}

inline void Emitter::emitConstDecl(const ConstDecl& cd) {
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

inline void Emitter::emitFnDecl(const FnDecl& fn) {
    emitLine(fnSignature(currentModuleId, fn) + " {");
    indentLevel += 1;

    std::unordered_map<std::string, VarSymbol> scope;
    const auto fnScopeIt = symbols.fnScopes.find(symbolKey(currentModuleId, fn.name));
    if (fnScopeIt != symbols.fnScopes.end()) {
        scope = fnScopeIt->second;
    }
    std::unordered_set<std::string> declaredVars;
    for (const auto& p : fn.params) {
        if (scope.find(p.name) == scope.end()) {
            const TypeId resolvedT = resolveAlias(arena, symbols, currentModuleId, p.typeId);
            scope[p.name] = VarSymbol{p.name, resolvedT, p.isByref, !p.isByref};
        }
        declaredVars.insert(p.name);
    }

    emitStmtList(fn, fn.body, scope, declaredVars);

    if (fn.isFallible && (fn.returnType == INVALID_ID || arena.isVoid(fn.returnType))) {
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
            emitLine(R"(return CbError{"", 0};)");
        }
    }

    indentLevel -= 1;
    emitLine("}");
}

inline std::string Emitter::emitProgram(const Program& program) {
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
    }

    std::vector<std::pair<std::string, EnumDecl>> enumDecls;
    std::vector<std::pair<std::string, TypeDecl>> structDecls;
    std::vector<std::pair<std::string, AliasDecl>> aliasDecls;
    std::vector<std::pair<std::string, ConstDecl>> earlyConstDecls;
    std::vector<std::pair<std::string, ConstDecl>> structConstDecls;
    std::vector<std::pair<std::string, FnDecl>> fnDecls;

    for (const DeclId did : program.declarations) {
        if (did >= arena.decls.size()) continue;
        const auto& declNode = arena.decls[did];
        if (const auto* td = arena.getDecl<TypeDecl>(did)) {
            if (!td->isExtern) structDecls.push_back({declNode.moduleId, *td});
        } else if (const auto* ed = arena.getDecl<EnumDecl>(did)) {
            enumDecls.push_back({declNode.moduleId, *ed});
        } else if (const auto* ad = arena.getDecl<AliasDecl>(did)) {
            aliasDecls.push_back({declNode.moduleId, *ad});
        } else if (const auto* cd = arena.getDecl<ConstDecl>(did)) {
            const std::string cKey = symbolKey(declNode.moduleId, cd->name);
            bool isStructConst = false;
            if (symbols.constants.count(cKey) > 0) {
                const TypeNode tnode = resolveAliasNode(arena, symbols, declNode.moduleId, symbols.constants.at(cKey).typeId);
                if (symbols.types.count(tnode.name) > 0 && tnode.name != "err") {
                    isStructConst = true;
                }
            }
            if (isStructConst) {
                structConstDecls.push_back({declNode.moduleId, *cd});
            } else {
                earlyConstDecls.push_back({declNode.moduleId, *cd});
            }
        } else if (const auto* fd = arena.getDecl<FnDecl>(did)) {
            if (!fd->isExtern) fnDecls.push_back({declNode.moduleId, *fd});
        }
    }

    if (!enumDecls.empty()) {
        emitLine();
        for (const auto& pair : enumDecls) {
            currentModuleId = pair.first;
            const std::string key = resolveSymbolKey(symbols, currentModuleId, pair.second.name);
            std::string cppName = pair.second.name;
            if (symbols.enums.count(key) > 0) {
                cppName = symbols.enums.at(key).cppName;
            }
            emitLine("enum class " + cppName + " : int64_t;");
        }
    }

    if (!enumDecls.empty()) {
        emitLine();
        for (const auto& pair : enumDecls) {
            currentModuleId = pair.first;
            emitEnumDecl(pair.second);
            emitLine();
        }
    }

    if (!structDecls.empty() || !aliasDecls.empty()) {
        emitLine();
        for (const auto& pair : structDecls) {
            currentModuleId = pair.first;
            const std::string key = resolveSymbolKey(symbols, currentModuleId, pair.second.name);
            std::string cppName = pair.second.name;
            if (symbols.types.count(key) > 0) {
                cppName = symbols.types.at(key).cppName;
            }
            emitLine("struct " + cppName + ";");
        }
        for (const auto& pair : aliasDecls) {
            currentModuleId = pair.first;
            const std::string key = resolveSymbolKey(symbols, currentModuleId, pair.second.name);
            std::string cppName = pair.second.name;
            if (symbols.aliases.count(key) > 0) {
                cppName = symbols.aliases.at(key).cppName;
            }
            emitLine("using " + cppName + " = " + mapType(currentModuleId, pair.second.targetType) + ";");
        }
    }

    if (!earlyConstDecls.empty()) {
        emitLine();
        for (const auto& pair : earlyConstDecls) {
            currentModuleId = pair.first;
            emitConstDecl(pair.second);
        }
    }

    if (!structDecls.empty()) {
        emitLine();
        for (const auto& pair : structDecls) {
            currentModuleId = pair.first;
            emitStructDecl(pair.second);
            emitLine();
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
        for (const auto& pair : fnDecls) {
            currentModuleId = pair.first;
            emitLine(fnSignature(currentModuleId, pair.second) + ";");
        }
        emitLine();
    }

    for (const auto& pair : fnDecls) {
        currentModuleId = pair.first;
        emitFnDecl(pair.second);
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
        out.push_back('\n');
    }
    return out;
}

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
