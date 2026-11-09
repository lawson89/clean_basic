#ifndef CLEANBASIC_ANALYZER_HPP
#define CLEANBASIC_ANALYZER_HPP

#include <fstream>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "src/cleanbasic/type_infer.hpp"

namespace cb {

inline bool isErrorBuiltinCall(const AstArena& arena, const FuncCall& call) {
    if (const auto* ident = arena.getExpr<Identifier>(call.callee)) {
        return ident->name == "error";
    }
    return false;
}

inline bool stmtListHasExit(const AstArena& arena, const std::vector<StmtId>& stmts) {
    if (stmts.empty()) {
        return false;
    }
    const StmtId lastSid = stmts.back();
    if (arena.getStmt<ReturnStmt>(lastSid) != nullptr) {
        return true;
    }
    if (arena.getStmt<BreakStmt>(lastSid) != nullptr) {
        return true;
    }
    if (arena.getStmt<ContinueStmt>(lastSid) != nullptr) {
        return true;
    }
    if (const auto* estmt = arena.getStmt<ExprStmt>(lastSid)) {
        if (const auto* call = arena.getExpr<FuncCall>(estmt->expr)) {
            if (isErrorBuiltinCall(arena, *call)) {
                return true;
            }
        }
    }
    return false;
}

class Analyzer {
public:
    AstArena& arena;
    GlobalSymbolTable& symbols;
    std::string fileName = "main.cb";
    std::string moduleId = "";
    int loopDepth = 0;

    Analyzer(AstArena& arena, GlobalSymbolTable& symbols, std::string_view fileName = "main.cb")
        : arena(arena), symbols(symbols), fileName(fileName), moduleId(""), loopDepth(0) {}

    [[noreturn]] void setAnalyzerError(uint32_t line, uint32_t col, std::string_view msg) const {
        throw makeError("AnalyzerError", std::string(msg), fileName, line, col);
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

    void validateNonNullTarget(
        uint32_t line,
        uint32_t col,
        TypeId targetTid,
        ExprId valExprId
    ) const {
        if (arena.getExpr<NullLiteral>(valExprId) == nullptr) {
            return;
        }
        const TypeId resolvedTarget = resolveAlias(arena, symbols, moduleId, targetTid);
        if (resolvedTarget != symbols.typeCptr) {
            setAnalyzerError(line, col, "CleanBasic types use non-nullable value semantics; only 'cptr' can be 'null'.");
        }
    }

    void checkFallibleUsage(
        const FnDecl& fn,
        ExprId exprId,
        const std::unordered_map<std::string, VarSymbol>& scope,
        bool hasActiveOnError,
        bool& outImplicitBubble
    ) {
        outImplicitBubble = false;
        if (const auto* call = arena.getExpr<FuncCall>(exprId)) {
            const auto fnSymOpt = resolveFuncCall(arena, *call, symbols, moduleId, scope);
            if (fnSymOpt.has_value()) {
                const auto& fnSym = fnSymOpt.value();
                if (fnSym.isFallible) {
                    if (!hasActiveOnError) {
                        if (fn.isFallible) {
                            outImplicitBubble = true;
                        } else {
                            const auto& exprNode = arena.exprs[exprId];
                            std::string msg = "Unhandled error: function returns an error but caller does not declare 'can error' or have an active 'on error' block (function '" +
                                fn.name + "' calls fallible function '" + fnSym.name + "').";
                            setAnalyzerError(exprNode.line, exprNode.col, msg);
                        }
                    }
                }
            }
        }
    }

    void validateExprCalls(
        const FnDecl& fn,
        ExprId exprId,
        const std::unordered_map<std::string, VarSymbol>& scope,
        bool allowTopLevelFallible
    ) {
        if (exprId >= arena.exprs.size()) {
            return;
        }

        const auto& exprNode = arena.exprs[exprId];

        if (const auto* call = arena.getExpr<FuncCall>(exprId)) {
            // Help users migrate from removed globals
            if (const auto* ident = arena.getExpr<Identifier>(call->callee)) {
                if (ident->name == "len") {
                    setAnalyzerError(exprNode.line, exprNode.col, "Global 'len()' is removed; use 'x.len()'.");
                }
                if (ident->name == "str") {
                    setAnalyzerError(exprNode.line, exprNode.col, "Global 'str()' is removed; use 'x.toStr()'.");
                }
            }

            if (isErrorBuiltinCall(arena, *call)) {
                if (!fn.isFallible) {
                    std::string msg = "Cannot call 'error(...)' inside infallible function '" + fn.name +
                        "'; add 'can error' to the function signature.";
                    setAnalyzerError(exprNode.line, exprNode.col, msg);
                }
                if (call->args.empty()) {
                    setAnalyzerError(exprNode.line, exprNode.col, "'error(...)' requires at least one message argument.");
                }
                for (const auto& namedArg : call->namedArgs) {
                    if (namedArg.name != "code") {
                        std::string msg = "Unknown named argument '" + namedArg.name +
                            "' in 'error(...)'; only 'code=' is supported.";
                        setAnalyzerError(exprNode.line, exprNode.col, msg);
                    }
                }
            }

            const auto fnSymOpt = resolveFuncCall(arena, *call, symbols, moduleId, scope);
            if (fnSymOpt.has_value()) {
                const auto& fnSym = fnSymOpt.value();

                // Built-in receiver validation
                if (fnSym.isBuiltinReceiver) {
                    if (fnSym.name == "push") {
                        if (call->args.size() != 1) {
                            setAnalyzerError(exprNode.line, exprNode.col, "'.push()' requires exactly 1 argument.");
                        }
                        if (const auto* calleeFa = arena.getExpr<FieldAccess>(call->callee)) {
                            const auto root = findRootVar(calleeFa->target, scope);
                            if (root.found && root.isConst) {
                                setAnalyzerError(exprNode.line, exprNode.col, "Cannot call '.push()' on const variable '" + root.name + "'.");
                            }
                        }
                    } else if (fnSym.name == "pop" || fnSym.name == "clear") {
                        if (!call->args.empty()) {
                            setAnalyzerError(exprNode.line, exprNode.col, "'." + fnSym.name + "()' takes 0 arguments.");
                        }
                        if (const auto* calleeFa = arena.getExpr<FieldAccess>(call->callee)) {
                            const auto root = findRootVar(calleeFa->target, scope);
                            if (root.found && root.isConst) {
                                setAnalyzerError(exprNode.line, exprNode.col, "Cannot call '." + fnSym.name + "()' on const variable '" + root.name + "'.");
                            }
                        }
                    } else if (fnSym.name == "get") {
                        if (call->args.size() != 2) {
                            setAnalyzerError(exprNode.line, exprNode.col, "'.get()' requires 2 arguments (key, defaultValue).");
                        }
                    } else if (fnSym.name == "has") {
                        if (call->args.size() != 1) {
                            setAnalyzerError(exprNode.line, exprNode.col, "'.has()' requires exactly 1 argument (key).");
                        }
                    } else if (fnSym.name == "remove") {
                        if (call->args.size() != 1) {
                            setAnalyzerError(exprNode.line, exprNode.col, "'.remove()' requires exactly 1 argument (key).");
                        }
                        if (const auto* calleeFa = arena.getExpr<FieldAccess>(call->callee)) {
                            const auto root = findRootVar(calleeFa->target, scope);
                            if (root.found && root.isConst) {
                                setAnalyzerError(exprNode.line, exprNode.col, "Cannot call '.remove()' on const variable '" + root.name + "'.");
                            }
                        }
                    } else if (fnSym.name == "slice") {
                        if (call->args.size() != 2) {
                            setAnalyzerError(exprNode.line, exprNode.col, "'.slice()' requires exactly 2 arguments (start, end).");
                        }
                    } else if (fnSym.name == "sliceFrom") {
                        if (call->args.size() != 1) {
                            setAnalyzerError(exprNode.line, exprNode.col, "'.sliceFrom()' requires exactly 1 argument (start).");
                        }
                    } else if (fnSym.name == "byteAt") {
                        if (call->args.size() != 1) {
                            setAnalyzerError(exprNode.line, exprNode.col, "'.byteAt()' requires exactly 1 argument (idx).");
                        }
                    } else if (fnSym.name == "fromHex") {
                        bool isStaticBytesCtor = false;
                        if (const auto* calleeFa = arena.getExpr<FieldAccess>(call->callee)) {
                            if (const auto* tgtIdent = arena.getExpr<Identifier>(calleeFa->target)) {
                                if (tgtIdent->name == "bytes" && scope.find("bytes") == scope.end()) {
                                    isStaticBytesCtor = true;
                                }
                            }
                        }
                        if (isStaticBytesCtor) {
                            if (call->args.size() != 1) {
                                setAnalyzerError(exprNode.line, exprNode.col, "'bytes.fromHex()' requires exactly 1 argument (hex string).");
                            }
                            const TypeId argTid = resolveAlias(arena, symbols, moduleId, inferExprType(arena, call->args[0], symbols, scope, moduleId));
                            if (argTid != symbols.typeStr && argTid != symbols.typeAuto) {
                                setAnalyzerError(exprNode.line, exprNode.col, "'bytes.fromHex()' argument must be of type 'str'.");
                            }
                        } else {
                            if (!call->args.empty()) {
                                setAnalyzerError(exprNode.line, exprNode.col, "'.fromHex()' on 'str' takes 0 arguments.");
                            }
                        }
                    } else if (fnSym.name.size() > 5 && fnSym.name.substr(0, 5) == "readU") {
                        if (call->args.size() != 1) {
                            setAnalyzerError(exprNode.line, exprNode.col, "'." + fnSym.name + "()' requires exactly 1 argument (offset).");
                        }
                    } else if (fnSym.name.size() > 6 && fnSym.name.substr(0, 6) == "writeU") {
                        if (call->args.size() != 2) {
                            setAnalyzerError(exprNode.line, exprNode.col, "'." + fnSym.name + "()' requires exactly 2 arguments (value, offset).");
                        }
                        if (const auto* calleeFa = arena.getExpr<FieldAccess>(call->callee)) {
                            const auto root = findRootVar(calleeFa->target, scope);
                            if (root.found && root.isConst) {
                                setAnalyzerError(exprNode.line, exprNode.col, "Cannot call '." + fnSym.name + "()' on const variable '" + root.name + "'.");
                            }
                        }
                    } else {
                        // len, toStr, toInt, toFloat, toUtf8, toBytes, toHex
                        if (!call->args.empty()) {
                            setAnalyzerError(exprNode.line, exprNode.col, "'." + fnSym.name + "()' takes 0 arguments.");
                        }
                    }
                } else {
                    bool isUfcs = false;
                    ExprId ufcsRecv = INVALID_ID;
                    if (const auto* calleeFa = arena.getExpr<FieldAccess>(call->callee)) {
                        std::string targetModId;
                        bool isModCall = false;
                        if (const auto* tgtIdent = arena.getExpr<Identifier>(calleeFa->target)) {
                            if (isModuleAliasInScope(symbols, moduleId, tgtIdent->name, scope, targetModId)) {
                                isModCall = true;
                            }
                        }
                        if (!isModCall) {
                            isUfcs = true;
                            ufcsRecv = calleeFa->target;
                        }
                    }

                    if (isUfcs && !fnSym.params.empty()) {
                        if (fnSym.params[0].isByref) {
                            const auto root = findRootVar(ufcsRecv, scope);
                            if (root.found && root.isConst) {
                                setAnalyzerError(exprNode.line, exprNode.col, "Cannot pass const variable '" + root.name + "' to byref parameter '" + fnSym.params[0].name + "'.");
                            }
                        }
                    }

                    size_t paramOffset = 0;
                    if (isUfcs) {
                        paramOffset = 1;
                    }
                    for (size_t aIdx = 0; aIdx < call->args.size(); ++aIdx) {
                        const size_t pIdx = paramOffset + aIdx;
                        if (pIdx < fnSym.params.size()) {
                            validateNonNullTarget(exprNode.line, exprNode.col, fnSym.params[pIdx].typeId, call->args[aIdx]);
                            if (fnSym.params[pIdx].isByref) {
                                const auto root = findRootVar(call->args[aIdx], scope);
                                if (root.found && root.isConst) {
                                    setAnalyzerError(exprNode.line, exprNode.col, "Cannot pass const variable '" + root.name + "' to byref parameter '" + fnSym.params[pIdx].name + "'.");
                                }
                            }
                        }
                    }

                    const bool isBuiltinPrint = (fnSym.name == "print" && fnSym.moduleId.empty());
                    for (const auto& namedArg : call->namedArgs) {
                        bool foundParam = false;
                        for (const auto& p : fnSym.params) {
                            if (p.name == namedArg.name) {
                                foundParam = true;
                                validateNonNullTarget(exprNode.line, exprNode.col, p.typeId, namedArg.value);
                                if (p.isByref) {
                                    const auto root = findRootVar(namedArg.value, scope);
                                    if (root.found && root.isConst) {
                                        setAnalyzerError(exprNode.line, exprNode.col, "Cannot pass const variable '" + root.name + "' to byref parameter '" + p.name + "'.");
                                    }
                                }
                                break;
                            }
                        }
                        if (!foundParam && !isBuiltinPrint) {
                            setAnalyzerError(exprNode.line, exprNode.col, "Unknown named argument '" + namedArg.name + "' in call to '" + fnSym.name + "'.");
                        }
                    }

                    if (!isBuiltinPrint && fnSym.overloads.empty()) {
                        const size_t totalProvided = paramOffset + call->args.size() + call->namedArgs.size();
                        if (totalProvided > fnSym.params.size()) {
                            setAnalyzerError(exprNode.line, exprNode.col, "Too many arguments in call to '" + fnSym.name + "'.");
                        }
                        for (size_t pIdx = paramOffset + call->args.size(); pIdx < fnSym.params.size(); ++pIdx) {
                            const auto& p = fnSym.params[pIdx];
                            bool providedByName = false;
                            for (const auto& na : call->namedArgs) {
                                if (na.name == p.name) {
                                    providedByName = true;
                                    break;
                                }
                            }
                            if (!providedByName && !p.hasDefault) {
                                setAnalyzerError(exprNode.line, exprNode.col, "Missing required argument for parameter '" + p.name + "' in call to '" + fnSym.name + "'.");
                            }
                        }
                    }

                }

                if (!fnSym.moduleId.empty() && fnSym.moduleId != moduleId) {
                    symbols.referencedModules.insert(fnSym.moduleId);
                }

                if (fnSym.isFallible && !allowTopLevelFallible) {
                    std::string msg = "Fallible call to '" + fnSym.name +
                        "' must be assigned to a variable, returned directly, or called as a standalone statement.";
                    setAnalyzerError(exprNode.line, exprNode.col, msg);
                }

                // Visibility check for module-qualified calls
                if (const auto* calleeFa = arena.getExpr<FieldAccess>(call->callee)) {
                    if (const auto* tgtIdent = arena.getExpr<Identifier>(calleeFa->target)) {
                        std::string targetModId;
                        if (isModuleAliasInScope(symbols, moduleId, tgtIdent->name, scope, targetModId)) {
                            if (!fnSym.isPub && fnSym.moduleId != moduleId) {
                                std::string msg = "Function '" + calleeFa->fieldName + "' is private to module '" + targetModId + "' (missing 'pub').";
                                setAnalyzerError(exprNode.line, exprNode.col, msg);
                            }
                        }
                    }
                }
            } else {
                const auto structOpt = resolveStructConstructor(arena, *call, symbols, moduleId, scope);
                if (structOpt.has_value()) {
                    if (!structOpt->moduleId.empty() && structOpt->moduleId != moduleId) {
                        symbols.referencedModules.insert(structOpt->moduleId);
                    }
                    if (!structOpt->isPub && structOpt->moduleId != moduleId) {
                        std::string msg = "Type '" + structOpt->name + "' is private to module '" + structOpt->moduleId + "' (missing 'pub').";
                        setAnalyzerError(exprNode.line, exprNode.col, msg);
                    }
                    for (size_t aIdx = 0; aIdx < call->args.size() && aIdx < structOpt->fieldOrder.size(); ++aIdx) {
                        validateNonNullTarget(exprNode.line, exprNode.col, structOpt->fieldOrder[aIdx].typeId, call->args[aIdx]);
                    }
                    for (const auto& na : call->namedArgs) {
                        const auto fit = structOpt->fields.find(na.name);
                        if (fit != structOpt->fields.end()) {
                            validateNonNullTarget(exprNode.line, exprNode.col, fit->second, na.value);
                        } else {
                            setAnalyzerError(exprNode.line, exprNode.col, "Unknown field '" + na.name + "' in constructor for '" + structOpt->name + "'.");
                        }
                    }
                }

                // Check for removed .append() and invalid bytes.fromHex instance calls
                if (const auto* fa = arena.getExpr<FieldAccess>(call->callee)) {
                    if (fa->fieldName == "append") {
                        const TypeId recvTid = inferExprType(arena, fa->target, symbols, scope, moduleId);
                        const TypeNode recvT = resolveAliasFromNode(arena, symbols, moduleId, arena.types[recvTid]);
                        if (recvT.name == "list") {
                            setAnalyzerError(exprNode.line, exprNode.col, "'.append()' is removed; use '.push()'.");
                        }
                    }
                    if (fa->fieldName == "fromHex") {
                        const TypeId recvTid = inferExprType(arena, fa->target, symbols, scope, moduleId);
                        if (recvTid < arena.types.size()) {
                            const TypeNode recvT = resolveAliasFromNode(arena, symbols, moduleId, arena.types[recvTid]);
                            if (recvT.name == "bytes") {
                                setAnalyzerError(exprNode.line, exprNode.col, "'.fromHex()' cannot be called on a 'bytes' instance; use 'bytes.fromHex(hex)' or 'hex.fromHex()'.");
                            }
                        }
                    }
                }
            }

            if (const auto* calleeFa = arena.getExpr<FieldAccess>(call->callee)) {
                if (!arena.getExpr<Identifier>(calleeFa->target)) {
                    validateExprCalls(fn, calleeFa->target, scope, false);
                }
            }

            for (const ExprId argId : call->args) {
                validateExprCalls(fn, argId, scope, false);
            }
            for (const auto& namedArg : call->namedArgs) {
                validateExprCalls(fn, namedArg.value, scope, false);
            }
            return;
        }

        if (const auto* bop = arena.getExpr<BinaryOp>(exprId)) {
            validateExprCalls(fn, bop->left, scope, false);
            validateExprCalls(fn, bop->right, scope, false);
            const TypeId lt = inferExprType(arena, bop->left, symbols, scope, moduleId);
            const TypeId rt = inferExprType(arena, bop->right, symbols, scope, moduleId);
            if (bop->op == "==" || bop->op == "!=") {
                if (arena.getExpr<NullLiteral>(bop->left) != nullptr && rt != symbols.typeAuto) {
                    validateNonNullTarget(exprNode.line, exprNode.col, rt, bop->left);
                }
                if (arena.getExpr<NullLiteral>(bop->right) != nullptr && lt != symbols.typeAuto) {
                    validateNonNullTarget(exprNode.line, exprNode.col, lt, bop->right);
                }
            }
            const bool lEnum = isEnumType(symbols, lt, arena, moduleId);
            const bool rEnum = isEnumType(symbols, rt, arena, moduleId);
            if (bop->op != "&" && (lEnum || rEnum)) {
                if (lt != rt && lt != symbols.typeAuto && rt != symbols.typeAuto) {
                    setAnalyzerError(exprNode.line, exprNode.col, "Cannot compare enum with mismatched type (use .toInt() for integer comparison).");
                }
            }
            return;
        }

        if (const auto* uop = arena.getExpr<UnaryOp>(exprId)) {
            validateExprCalls(fn, uop->operand, scope, false);
            return;
        }

        if (const auto* faccess = arena.getExpr<FieldAccess>(exprId)) {
            const auto enumOpt = resolveEnumMemberAccess(arena, symbols, moduleId, scope, *faccess);
            if (enumOpt.has_value()) {
                if (!enumOpt->moduleId.empty() && enumOpt->moduleId != moduleId) {
                    symbols.referencedModules.insert(enumOpt->moduleId);
                }
                if (enumOpt->moduleId != moduleId && !enumOpt->isPub) {
                    setAnalyzerError(exprNode.line, exprNode.col, "Enum '" + enumOpt->name + "' is private to module '" + enumOpt->moduleId + "' (missing 'pub').");
                }
                if (enumOpt->memberValues.count(faccess->fieldName) == 0) {
                    setAnalyzerError(exprNode.line, exprNode.col, "Enum '" + enumOpt->name + "' has no member '" + faccess->fieldName + "'.");
                }
                return;
            }
            const auto cOpt = resolveModuleConstAccess(arena, symbols, moduleId, scope, *faccess);
            if (cOpt.has_value()) {
                if (!cOpt->moduleId.empty() && cOpt->moduleId != moduleId) {
                    symbols.referencedModules.insert(cOpt->moduleId);
                }
                if (cOpt->moduleId != moduleId && !cOpt->isPub) {
                    setAnalyzerError(exprNode.line, exprNode.col, "Constant '" + cOpt->name + "' is private to module '" + cOpt->moduleId + "' (missing 'pub').");
                }
                return;
            }
            validateExprCalls(fn, faccess->target, scope, false);
            return;
        }

        if (const auto* iaccess = arena.getExpr<IndexAccess>(exprId)) {
            validateExprCalls(fn, iaccess->target, scope, false);
            validateExprCalls(fn, iaccess->index, scope, false);
            return;
        }

        if (const auto* llit = arena.getExpr<ListLiteral>(exprId)) {
            for (const ExprId elId : llit->elements) {
                validateExprCalls(fn, elId, scope, false);
            }
            return;
        }

        if (const auto* dlit = arena.getExpr<DictLiteral>(exprId)) {
            for (const auto& ent : dlit->entries) {
                validateExprCalls(fn, ent.key, scope, false);
                validateExprCalls(fn, ent.value, scope, false);
            }
            return;
        }
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
        scope[name] = VarSymbol{name, tid, isByref, isConst, fileName, line, col, line};
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
        if (varIsEnum && resolvedVal != symbols.typeAuto && resolvedVal != resolvedTarget) {
            setAnalyzerError(line, col, "Cannot assign mismatched type to enum variable '" + std::string(varName) + "'.");
        } else if (!varIsEnum && valIsEnum && resolvedTarget == symbols.typeInt) {
            setAnalyzerError(line, col, "Cannot assign enum value to 'int' without explicit conversion (use .toInt()).");
        }
    }

    void analyzeStmt(
        const FnDecl& fn,
        StmtId stmtId,
        std::unordered_map<std::string, VarSymbol>& scope,
        bool& hasActiveOnError
    ) {
        if (stmtId >= arena.stmts.size()) {
            return;
        }

        const auto& stmtNode = arena.stmts[stmtId];

        if (auto* vassign = arena.getStmtMut<VarAssignment>(stmtId)) {
            checkFallibleUsage(fn, vassign->value, scope, hasActiveOnError, vassign->implicitBubble);

            const TypeId valType = inferExprType(arena, vassign->value, symbols, scope, moduleId);
            if (const auto* ident = arena.getExpr<Identifier>(vassign->target)) {
                const auto& tgtNode = arena.exprs[vassign->target];
                if (vassign->isDim || vassign->isConst) {
                    if (!vassign->hasDeclaredType) {
                        if (const auto* llit = arena.getExpr<ListLiteral>(vassign->value)) {
                            if (llit->elements.empty()) {
                                setAnalyzerError(stmtNode.line, stmtNode.col, "Cannot infer element type of empty list '[]'; specify an explicit type such as 'dim " + ident->name + " as list[int] = []'.");
                            }
                        }
                        if (const auto* dlit = arena.getExpr<DictLiteral>(vassign->value)) {
                            if (dlit->entries.empty()) {
                                setAnalyzerError(stmtNode.line, stmtNode.col, "Cannot infer key/value types of empty dict '{}'; specify an explicit type such as 'dim " + ident->name + " as dict[str, int] = {}'.");
                            }
                        }
                    }
                    TypeId varTid = symbols.typeInt;
                    if (vassign->hasDeclaredType) {
                        varTid = resolveAlias(arena, symbols, moduleId, vassign->declaredType);
                    } else if (valType != INVALID_ID) {
                        varTid = valType;
                    }
                    validateNonNullTarget(tgtNode.line, tgtNode.col, varTid, vassign->value);
                    validateEnumAssignment(tgtNode.line, tgtNode.col, ident->name, varTid, valType);
                    bindLocalVar(scope, ident->name, varTid, false, vassign->isConst, tgtNode.line, tgtNode.col);
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
                    validateNonNullTarget(tgtNode.line, tgtNode.col, existing->second.typeId, vassign->value);
                    validateEnumAssignment(tgtNode.line, tgtNode.col, ident->name, existing->second.typeId, valType);
                }
            } else {
                const auto& tgtNode = arena.exprs[vassign->target];
                if (const auto* iaccess = arena.getExpr<IndexAccess>(vassign->target)) {
                    const TypeId idxTgtTid = inferExprType(arena, iaccess->target, symbols, scope, moduleId);
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
                const TypeId lhsTid = inferExprType(arena, vassign->target, symbols, scope, moduleId);
                validateNonNullTarget(tgtNode.line, tgtNode.col, lhsTid, vassign->value);
                validateExprCalls(fn, vassign->target, scope, false);
            }

            validateExprCalls(fn, vassign->value, scope, true);
            return;
        }

        if (auto* estmt = arena.getStmtMut<ExprStmt>(stmtId)) {
            checkFallibleUsage(fn, estmt->expr, scope, hasActiveOnError, estmt->implicitBubble);
            validateExprCalls(fn, estmt->expr, scope, true);
            return;
        }

        if (const auto* errBlock = arena.getStmt<ErrorBlock>(stmtId)) {
            bindLocalVar(scope, errBlock->errVar, symbols.typeErr, false, false, stmtNode.line, stmtNode.col);
            auto errScope = scope;
            analyzeBlock(fn, errBlock->body, errScope, false);

            if (!stmtListHasExit(arena, errBlock->body)) {
                std::string msg = "'on error' block must terminate with an explicit control-flow exit ('return', 'error(...)', or 'break').";
                setAnalyzerError(stmtNode.line, stmtNode.col, msg);
            }

            hasActiveOnError = true;
            return;
        }

        if (const auto* ifStmt = arena.getStmt<IfStmt>(stmtId)) {
            validateExprCalls(fn, ifStmt->condition, scope, false);
            analyzeBlock(fn, ifStmt->thenBody, scope, hasActiveOnError);

            for (const auto& branch : ifStmt->elifBranches) {
                validateExprCalls(fn, branch.condition, scope, false);
                analyzeBlock(fn, branch.body, scope, hasActiveOnError);
            }

            if (ifStmt->hasElse) {
                analyzeBlock(fn, ifStmt->elseBody, scope, hasActiveOnError);
            }
            return;
        }

        if (const auto* forStmt = arena.getStmt<ForStmt>(stmtId)) {
            loopDepth += 1;
            if (forStmt->rangeSpec.hasRange) {
                validateExprCalls(fn, forStmt->rangeSpec.start, scope, false);
                validateExprCalls(fn, forStmt->rangeSpec.end, scope, false);
                if (forStmt->rangeSpec.hasStep) {
                    validateExprCalls(fn, forStmt->rangeSpec.step, scope, false);
                }
                if (!forStmt->loopVars.empty()) {
                    bindLocalVar(scope, forStmt->loopVars[0], symbols.typeInt, false, false, stmtNode.line, stmtNode.col);
                }
            } else if (forStmt->hasIterable) {
                validateExprCalls(fn, forStmt->iterable, scope, false);
                const TypeId iterTid = inferExprType(arena, forStmt->iterable, symbols, scope, moduleId);
                if (iterTid < arena.types.size()) {
                    const TypeNode iterT = resolveAliasNode(arena, symbols, moduleId, iterTid);
                    if (iterT.name == "list" && !iterT.typeArgs.empty() && !forStmt->loopVars.empty()) {
                        if (forStmt->loopVars.size() == 1) {
                            bindLocalVar(scope, forStmt->loopVars[0], iterT.typeArgs[0], false, false, stmtNode.line, stmtNode.col);
                        } else if (forStmt->loopVars.size() == 2) {
                            bindLocalVar(scope, forStmt->loopVars[0], symbols.typeInt, false, false, stmtNode.line, stmtNode.col);
                            bindLocalVar(scope, forStmt->loopVars[1], iterT.typeArgs[0], false, false, stmtNode.line, stmtNode.col);
                        } else {
                            setAnalyzerError(stmtNode.line, stmtNode.col, "List iteration supports at most 2 loop variables ('for item in list' or 'for i, item in list').");
                        }
                    } else if (iterT.name == "bytes" && !forStmt->loopVars.empty()) {
                        if (forStmt->loopVars.size() == 1) {
                            bindLocalVar(scope, forStmt->loopVars[0], symbols.typeInt, false, false, stmtNode.line, stmtNode.col);
                        } else if (forStmt->loopVars.size() == 2) {
                            bindLocalVar(scope, forStmt->loopVars[0], symbols.typeInt, false, false, stmtNode.line, stmtNode.col);
                            bindLocalVar(scope, forStmt->loopVars[1], symbols.typeInt, false, false, stmtNode.line, stmtNode.col);
                        } else {
                            setAnalyzerError(stmtNode.line, stmtNode.col, "Bytes iteration supports at most 2 loop variables ('for b in data' or 'for i, b in data').");
                        }
                    } else if (iterT.name == "dict" && iterT.typeArgs.size() == 2) {
                        if (forStmt->loopVars.size() >= 1) {
                            bindLocalVar(scope, forStmt->loopVars[0], iterT.typeArgs[0], false, false, stmtNode.line, stmtNode.col);
                        }
                        if (forStmt->loopVars.size() >= 2) {
                            bindLocalVar(scope, forStmt->loopVars[1], iterT.typeArgs[1], false, false, stmtNode.line, stmtNode.col);
                        }
                    } else {
                        for (const auto& lv : forStmt->loopVars) {
                            bindLocalVar(scope, lv, symbols.typeAuto, false, false, stmtNode.line, stmtNode.col);
                        }
                    }
                }
            }
            analyzeBlock(fn, forStmt->body, scope, hasActiveOnError);
            loopDepth -= 1;
            return;
        }

        if (const auto* whileStmt = arena.getStmt<WhileStmt>(stmtId)) {
            loopDepth += 1;
            bool isFallibleCond = false;
            if (const auto* call = arena.getExpr<FuncCall>(whileStmt->condition)) {
                const auto fnOpt = resolveFuncCall(arena, *call, symbols, moduleId, scope);
                if (fnOpt.has_value() && fnOpt->isFallible) {
                    isFallibleCond = true;
                }
            }
            if (isFallibleCond) {
                bool dummyBubble = false;
                checkFallibleUsage(fn, whileStmt->condition, scope, hasActiveOnError, dummyBubble);
                validateExprCalls(fn, whileStmt->condition, scope, true);
            } else {
                validateExprCalls(fn, whileStmt->condition, scope, false);
            }
            analyzeBlock(fn, whileStmt->body, scope, hasActiveOnError);
            loopDepth -= 1;
            return;
        }

        if (const auto* selStmt = arena.getStmt<SelectStmt>(stmtId)) {
            validateExprCalls(fn, selStmt->target, scope, false);

            const TypeId targetTid = inferExprType(arena, selStmt->target, symbols, scope, moduleId);
            const bool isEnumTarget = isEnumType(symbols, targetTid, arena, moduleId);
            if (targetTid != symbols.typeInt && targetTid != symbols.typeBool && targetTid != symbols.typeStr && !isEnumTarget) {
                setAnalyzerError(stmtNode.line, stmtNode.col, "select case expression must be of type 'int', 'bool', or 'str' (or enum)");
            }

            for (const auto& scase : selStmt->cases) {
                for (const auto& mExpr : scase.matchExprs) {
                    validateExprCalls(fn, mExpr, scope, false);
                    const TypeId mExprTid = inferExprType(arena, mExpr, symbols, scope, moduleId);
                    if (mExprTid != targetTid && mExprTid != symbols.typeAuto) {
                        setAnalyzerError(scase.line, scase.col, "case expression type does not match select case target type");
                    }
                }
                analyzeBlock(fn, scase.body, scope, hasActiveOnError);
            }

            if (selStmt->hasCaseElse) {
                analyzeBlock(fn, selStmt->caseElse, scope, hasActiveOnError);
            }
            return;
        }

        if (const auto* dstmt = arena.getStmt<DeferStmt>(stmtId)) {
            analyzeBlock(fn, dstmt->body, scope, false);
            return;
        }

        if (const auto* retStmt = arena.getStmt<ReturnStmt>(stmtId)) {
            bool isReturnErrorCall = false;
            if (retStmt->hasValue) {
                if (const auto* call = arena.getExpr<FuncCall>(retStmt->value)) {
                    if (isErrorBuiltinCall(arena, *call)) {
                        isReturnErrorCall = true;
                    }
                }
                if (!isReturnErrorCall) {
                    bool implicitBubble = false;
                    checkFallibleUsage(fn, retStmt->value, scope, hasActiveOnError, implicitBubble);
                }
                validateExprCalls(fn, retStmt->value, scope, true);
            }
            if (!isReturnErrorCall) {
                const bool expectedHasValue = (fn.returnType != INVALID_ID && !arena.isVoid(fn.returnType));
                if (retStmt->hasValue && !expectedHasValue) {
                    std::string msg = "Function '" + fn.name + "' returns void, but a value was returned.";
                    setAnalyzerError(stmtNode.line, stmtNode.col, msg);
                } else if (!retStmt->hasValue && expectedHasValue) {
                    std::string msg = "Function '" + fn.name + "' expects a return value, but none was provided.";
                    setAnalyzerError(stmtNode.line, stmtNode.col, msg);
                } else if (retStmt->hasValue && expectedHasValue) {
                    validateNonNullTarget(stmtNode.line, stmtNode.col, fn.returnType, retStmt->value);
                }
            }
            return;
        }

        if (arena.getStmt<BreakStmt>(stmtId) != nullptr) {
            if (loopDepth <= 0) {
                setAnalyzerError(stmtNode.line, stmtNode.col, "'break' cannot be used outside of a loop");
            }
            return;
        }

        if (arena.getStmt<ContinueStmt>(stmtId) != nullptr) {
            if (loopDepth <= 0) {
                setAnalyzerError(stmtNode.line, stmtNode.col, "'continue' cannot be used outside of a loop");
            }
            return;
        }
    }

    void analyzeBlock(
        const FnDecl& fn,
        const std::vector<StmtId>& stmts,
        std::unordered_map<std::string, VarSymbol>& scope,
        bool inheritedOnError
    ) {
        bool activeOnError = inheritedOnError;
        for (const StmtId sid : stmts) {
            analyzeStmt(fn, sid, scope, activeOnError);
        }
    }

    void analyzeFn(const FnDecl& fn) {
        loopDepth = 0;
        std::unordered_map<std::string, VarSymbol> scope;
        for (const auto& param : fn.params) {
            const TypeId resolvedTid = resolveAlias(arena, symbols, moduleId, param.typeId);
            const bool isConst = !param.isByref;
            bindLocalVar(scope, param.name, resolvedTid, param.isByref, isConst, param.line, param.col);
        }

        analyzeBlock(fn, fn.body, scope, false);
        symbols.fnScopes[symbolKey(moduleId, fn.name)] = std::move(scope);
    }

    void analyzeProgram(const Program& program) {
        fileName = program.fileName;

        std::unordered_set<std::string> knownModules;
        knownModules.insert("");

        // Pass 1a: Register module imports, aliases, and type declarations
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
                } else if (imp->kind == "link") {
                    bool found = false;
                    for (const auto& l : symbols.linkLibs) {
                        if (l == imp->target) { found = true; break; }
                    }
                    if (!found) {
                        symbols.linkLibs.push_back(imp->target);
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
                }
            } else if (const auto* adecl = arena.getDecl<AliasDecl>(did)) {
                AliasSymbol asym{};
                asym.name = adecl->name;
                asym.moduleId = declNode.moduleId;
                asym.key = symbolKey(declNode.moduleId, adecl->name);
                asym.targetType = adecl->targetType;
                asym.isPub = adecl->isPub;
                asym.cppName = makeCppTypeName(declNode.moduleId, adecl->name, false);
                asym.fileName = declNode.fileName;
                asym.line = declNode.nameLine;
                asym.col = declNode.nameCol;
                asym.endLine = declNode.endLine;
                symbols.aliases[asym.key] = std::move(asym);
            } else if (const auto* tdecl = arena.getDecl<TypeDecl>(did)) {
                TypeSymbol tsym{};
                tsym.name = tdecl->name;
                tsym.moduleId = declNode.moduleId;
                tsym.key = symbolKey(declNode.moduleId, tdecl->name);
                tsym.isPub = tdecl->isPub;
                tsym.isExtern = tdecl->isExtern;
                tsym.cppName = makeCppTypeName(declNode.moduleId, tdecl->name, tdecl->isExtern);
                tsym.fieldOrder = tdecl->fields;
                tsym.fileName = declNode.fileName;
                tsym.line = declNode.nameLine;
                tsym.col = declNode.nameCol;
                tsym.endLine = declNode.endLine;
                const std::string keyCopy = tsym.key;
                symbols.types[keyCopy] = std::move(tsym);
                arena.findOrAddType(keyCopy, {});
            } else if (const auto* edecl = arena.getDecl<EnumDecl>(did)) {
                EnumSymbol esym{};
                esym.name = edecl->name;
                esym.moduleId = declNode.moduleId;
                esym.key = symbolKey(declNode.moduleId, edecl->name);
                esym.isPub = edecl->isPub;
                esym.hasExplicitIntType = edecl->hasExplicitIntType;
                esym.cppName = makeCppTypeName(declNode.moduleId, edecl->name, false);
                esym.fileName = declNode.fileName;
                esym.line = declNode.nameLine;
                esym.col = declNode.nameCol;
                esym.endLine = declNode.endLine;
                std::unordered_set<std::string> seen;
                for (const auto& mem : edecl->members) {
                    if (seen.count(mem.name) > 0) {
                        throw makeError("AnalyzerError", "Duplicate enum member '" + mem.name + "' in enum '" + edecl->name + "'.", declNode.fileName, declNode.line, declNode.col);
                    }
                    seen.insert(mem.name);
                    esym.members.push_back(mem);
                    esym.memberValues[mem.name] = mem.value;
                }
                const std::string keyCopy = esym.key;
                symbols.enums[keyCopy] = std::move(esym);
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

        // Pass 1b: Resolve struct field types, constants, and function signatures
        for (const DeclId did : program.declarations) {
            if (did >= arena.decls.size()) continue;
            const auto& declNode = arena.decls[did];

            if (const auto* tdecl = arena.getDecl<TypeDecl>(did)) {
                const std::string key = symbolKey(declNode.moduleId, tdecl->name);
                auto& tsym = symbols.types[key];
                tsym.fieldOrder.clear();
                tsym.fieldOrder.reserve(tdecl->fields.size());
                for (const auto& fld : tdecl->fields) {
                    const TypeId resolvedFid = resolveAlias(arena, symbols, declNode.moduleId, fld.typeId);
                    tsym.fields[fld.name] = resolvedFid;
                    FieldDecl resolvedFld = fld;
                    resolvedFld.typeId = resolvedFid;
                    tsym.fieldOrder.push_back(std::move(resolvedFld));
                }
            } else if (const auto* cdecl = arena.getDecl<ConstDecl>(did)) {
                const std::string key = symbolKey(declNode.moduleId, cdecl->name);
                if (symbols.constants.count(key) > 0) {
                    throw makeError("AnalyzerError", "Duplicate constant declaration '" + cdecl->name + "'.", declNode.fileName, declNode.line, declNode.col);
                }
                if (!cdecl->hasDeclaredType) {
                    if (const auto* llit = arena.getExpr<ListLiteral>(cdecl->value)) {
                        if (llit->elements.empty()) {
                            throw makeError("AnalyzerError", "Cannot infer element type of empty list '[]'; specify an explicit type such as 'const " + cdecl->name + " as list[int] = []'.", declNode.fileName, declNode.line, declNode.col);
                        }
                    }
                    if (const auto* dlit = arena.getExpr<DictLiteral>(cdecl->value)) {
                        if (dlit->entries.empty()) {
                            throw makeError("AnalyzerError", "Cannot infer key/value types of empty dict '{}'; specify an explicit type such as 'const " + cdecl->name + " as dict[str, int] = {}'.", declNode.fileName, declNode.line, declNode.col);
                        }
                    }
                }
                std::unordered_map<std::string, VarSymbol> emptyScope;
                const TypeId inferredTid = inferExprType(arena, cdecl->value, symbols, emptyScope, declNode.moduleId);
                TypeId constTid = inferredTid;
                if (cdecl->hasDeclaredType) {
                    constTid = resolveAlias(arena, symbols, declNode.moduleId, cdecl->declaredType);
                }
                if (arena.getExpr<NullLiteral>(cdecl->value) != nullptr && constTid != symbols.typeCptr) {
                    throw makeError("AnalyzerError", "CleanBasic types use non-nullable value semantics; only 'cptr' can be 'null'.", declNode.fileName, declNode.line, declNode.col);
                }
                ConstSymbol csym{};
                csym.name = cdecl->name;
                csym.moduleId = declNode.moduleId;
                csym.key = key;
                csym.typeId = constTid;
                csym.value = cdecl->value;
                csym.isPub = cdecl->isPub;
                csym.cppName = makeCppConstName(declNode.moduleId, cdecl->name);
                csym.fileName = declNode.fileName;
                csym.line = declNode.nameLine;
                csym.col = declNode.nameCol;
                csym.endLine = declNode.endLine;
                symbols.constants[key] = std::move(csym);
            } else if (const auto* fdecl = arena.getDecl<FnDecl>(did)) {
                FnSymbol fsym{};
                fsym.name = fdecl->name;
                fsym.moduleId = declNode.moduleId;
                fsym.key = symbolKey(declNode.moduleId, fdecl->name);
                fsym.isPub = fdecl->isPub;
                fsym.isExtern = fdecl->isExtern;
                fsym.isFallible = fdecl->isFallible;
                fsym.cppName = makeCppFnName(declNode.moduleId, fdecl->name, fdecl->isExtern);
                fsym.fileName = declNode.fileName;
                fsym.line = declNode.nameLine;
                fsym.col = declNode.nameCol;
                fsym.endLine = declNode.endLine;
                fsym.params.reserve(fdecl->params.size());
                for (const auto& p : fdecl->params) {
                    Param resolvedP = p;
                    resolvedP.typeId = resolveAlias(arena, symbols, declNode.moduleId, p.typeId);
                    fsym.params.push_back(std::move(resolvedP));
                }
                if (fdecl->returnType != INVALID_ID) {
                    fsym.returnType = resolveAlias(arena, symbols, declNode.moduleId, fdecl->returnType);
                }
                const auto existing = symbols.functions.find(fsym.key);
                if (existing != symbols.functions.end()) {
                    existing->second.overloads.push_back(std::move(fsym));
                } else {
                    symbols.functions[fsym.key] = std::move(fsym);
                }
            }
        }

        // Pass 2: Validate top-level constants, defaults, and function bodies
        for (const DeclId did : program.declarations) {
            if (did >= arena.decls.size()) continue;
            const auto& declNode = arena.decls[did];
            fileName = declNode.fileName;
            moduleId = declNode.moduleId;
            if (const auto* cdecl = arena.getDecl<ConstDecl>(did)) {
                std::unordered_map<std::string, VarSymbol> emptyScope;
                const std::string key = symbolKey(declNode.moduleId, cdecl->name);
                const TypeId valTid = inferExprType(arena, cdecl->value, symbols, emptyScope, moduleId);
                TypeId targetTid = valTid;
                if (symbols.constants.count(key) > 0) {
                    targetTid = symbols.constants.at(key).typeId;
                }
                validateEnumAssignment(declNode.line, declNode.col, cdecl->name, targetTid, valTid);
                FnDecl dummyFn{};
                dummyFn.name = cdecl->name;
                dummyFn.isFallible = false;
                validateExprCalls(dummyFn, cdecl->value, emptyScope, false);
            } else if (const auto* tdecl = arena.getDecl<TypeDecl>(did)) {
                std::unordered_map<std::string, VarSymbol> emptyScope;
                FnDecl dummyFn{};
                dummyFn.name = tdecl->name;
                dummyFn.isFallible = false;
                for (const auto& fld : tdecl->fields) {
                    if (fld.hasDefault && fld.defaultValue != INVALID_ID) {
                        const TypeId resolvedFid = resolveAlias(arena, symbols, moduleId, fld.typeId);
                        const TypeId valTid = inferExprType(arena, fld.defaultValue, symbols, emptyScope, moduleId);
                        validateNonNullTarget(fld.line, fld.col, resolvedFid, fld.defaultValue);
                        validateEnumAssignment(fld.line, fld.col, fld.name, resolvedFid, valTid);
                        validateExprCalls(dummyFn, fld.defaultValue, emptyScope, false);
                    }
                }
            } else if (const auto* fdecl = arena.getDecl<FnDecl>(did)) {
                std::unordered_map<std::string, VarSymbol> emptyScope;
                FnDecl dummyFn{};
                dummyFn.name = fdecl->name;
                dummyFn.isFallible = false;
                for (const auto& p : fdecl->params) {
                    if (p.hasDefault && p.defaultValue != INVALID_ID) {
                        const TypeId resolvedPid = resolveAlias(arena, symbols, moduleId, p.typeId);
                        const TypeId valTid = inferExprType(arena, p.defaultValue, symbols, emptyScope, moduleId);
                        validateNonNullTarget(p.line, p.col, resolvedPid, p.defaultValue);
                        validateEnumAssignment(p.line, p.col, p.name, resolvedPid, valTid);
                        validateExprCalls(dummyFn, p.defaultValue, emptyScope, false);
                    }
                }
                if (!fdecl->isExtern) {
                    analyzeFn(*fdecl);
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
    }
};

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

    try {
        Analyzer analyzer(arena, result.symbols, program.fileName);
        analyzer.analyzeProgram(program);
        return result;
    } catch (const DiagnosticError& err) {
        result.ok = false;
        result.error = err;
        return result;
    }
}

} // namespace cb

#endif // CLEANBASIC_ANALYZER_HPP
