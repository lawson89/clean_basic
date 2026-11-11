#ifndef CLEANBASIC_ANALYZER_HPP
#define CLEANBASIC_ANALYZER_HPP

#include <fstream>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "src/cleanbasic/symbols.hpp"

namespace cb {

class Analyzer {
public:
    AstArena& arena;
    GlobalSymbolTable& symbols;
    std::string fileName = "main.cb";
    std::string moduleId = "";
    std::string currentFnKey = "";
    int loopDepth = 0;
    int activeTryDepth = 0;

    Analyzer(AstArena& arena, GlobalSymbolTable& symbols, std::string_view fileName = "main.cb")
        : arena(arena), symbols(symbols), fileName(fileName), moduleId(""), currentFnKey(""), loopDepth(0), activeTryDepth(0) {}

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
        bool& outImplicitBubble,
        bool& outBreaksToCatch
    ) {
        outImplicitBubble = false;
        outBreaksToCatch = false;
        if (const auto* call = arena.getExpr<FuncCall>(exprId)) {
            bool isFallible = false;
            std::string calleeName = "";
            if (isAssertBuiltinCall(arena, *call)) {
                isFallible = true;
                calleeName = "assert";
            } else if (isErrorBuiltinCall(arena, *call) && activeTryDepth > 0) {
                outBreaksToCatch = true;
                return;
            } else {
                const auto fnSymOpt = resolveFuncCall(arena, *call, symbols, moduleId, scope);
                if (fnSymOpt.has_value()) {
                    isFallible = fnSymOpt->isFallible;
                    calleeName = fnSymOpt->name;
                }
            }

            if (isFallible) {
                if (activeTryDepth > 0) {
                    outBreaksToCatch = true;
                } else {
                    if (fn.isFallible) {
                        outImplicitBubble = true;
                    } else {
                        const auto& exprNode = arena.exprs[exprId];
                        std::string msg = "Unhandled error: function returns an error but caller does not declare 'can error' or have an active 'try / catch' block (function '" +
                            fn.name + "' calls fallible function '" + calleeName + "').";
                        setAnalyzerError(exprNode.line, exprNode.col, msg);
                    }
                }
            }
        }
    }

    // Validates function/constructor/method calls within an expression tree.
    // allowTopLevelFallible is true ONLY at the root expression of a VarAssignment ('x = f()'),
    // ExprStmt ('f()'), or ReturnStmt ('return f()'). Any nested sub-expression recursive call passes
    // allowTopLevelFallible = false to enforce CleanBasic's statement-root error-bubbling invariant.
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

        if (arena.getExpr<Identifier>(exprId) != nullptr) {
            const auto bareFnOpt = resolveBareFnRef(arena, exprId, symbols, scope, moduleId);
            if (bareFnOpt.has_value()) {
                if (!bareFnOpt->moduleId.empty() && bareFnOpt->moduleId != moduleId) {
                    symbols.referencedModules.insert(bareFnOpt->moduleId);
                }
                if (bareFnOpt->moduleId != moduleId && !bareFnOpt->isPub) {
                    setAnalyzerError(exprNode.line, exprNode.col, "Function '" + bareFnOpt->name + "' is private to module '" + bareFnOpt->moduleId + "' (missing 'public').");
                }
            }
            return;
        }

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
                if (!call->namedArgs.empty()) {
                    setAnalyzerError(exprNode.line, exprNode.col, "Named arguments are not supported in 'assert(...)'.");
                }
                const TypeId condTid = resolveAlias(arena, symbols, moduleId, inferExprType(arena, call->args[0], symbols, scope, moduleId));
                if (condTid != symbols.typeBool && condTid != symbols.typeAuto) {
                    setAnalyzerError(exprNode.line, exprNode.col, "First argument to 'assert(...)' must be of type 'bool'.");
                }
                if (call->args.size() == 2) {
                    const TypeId msgTid = resolveAlias(arena, symbols, moduleId, inferExprType(arena, call->args[1], symbols, scope, moduleId));
                    if (msgTid != symbols.typeStr && msgTid != symbols.typeAuto) {
                        setAnalyzerError(exprNode.line, exprNode.col, "Second argument to 'assert(...)' must be of type 'str'.");
                    }
                }
                for (const ExprId argId : call->args) {
                    validateExprCalls(fn, argId, scope, false);
                }
                return;
            }

            if (isErrorBuiltinCall(arena, *call)) {
                if (!fn.isFallible && activeTryDepth <= 0) {
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

                ExprId ufcsRecv = INVALID_ID;
                const bool isUfcs = isUfcsCallExpr(arena, *call, symbols, moduleId, scope, &ufcsRecv);

                if (isUfcs && !fnSym.params.empty() && fnSym.params[0].isByref) {
                    const auto root = findRootVar(ufcsRecv, scope);
                    if (root.found && root.isConst) {
                        if (fnSym.isBuiltinReceiver) {
                            setAnalyzerError(exprNode.line, exprNode.col, "Cannot call '." + fnSym.name + "()' on const variable '" + root.name + "'.");
                        }
                        setAnalyzerError(exprNode.line, exprNode.col, "Cannot pass const variable '" + root.name + "' to byref parameter '" + fnSym.params[0].name + "'.");
                    }
                }

                size_t paramOffset = 0;
                if (isUfcs) {
                    paramOffset = 1;
                }
                for (size_t aIdx = 0; aIdx < call->args.size(); ++aIdx) {
                    const size_t pIdx = paramOffset + aIdx;
                    if (pIdx < fnSym.params.size()) {
                        const TypeId expectedParamTid = resolveAlias(arena, symbols, moduleId, fnSym.params[pIdx].typeId);
                        validateNonNullTarget(exprNode.line, exprNode.col, expectedParamTid, call->args[aIdx]);
                        const TypeId actualArgTid = resolveAlias(arena, symbols, moduleId, inferExprType(arena, call->args[aIdx], symbols, scope, moduleId));
                        if (fnSym.isBuiltinReceiver) {
                            if (expectedParamTid != symbols.typeAuto && actualArgTid != symbols.typeAuto && expectedParamTid != actualArgTid) {
                                setAnalyzerError(exprNode.line, exprNode.col, "Type mismatch for parameter '" + fnSym.params[pIdx].name + "' in call to '." + fnSym.name + "()'.");
                            }
                        } else if (expectedParamTid < arena.types.size() && arena.types[expectedParamTid].name == "fn") {
                            if (actualArgTid != symbols.typeAuto && actualArgTid != expectedParamTid) {
                                setAnalyzerError(exprNode.line, exprNode.col, "Function reference type mismatch for parameter '" + fnSym.params[pIdx].name + "' in call to '" + fnSym.name + "'.");
                            }
                        }
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

                if (!isBuiltinPrint) {
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
                                std::string msg = "Function '" + calleeFa->fieldName + "' is private to module '" + targetModId + "' (missing 'public').";
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
                        std::string msg = "Type '" + structOpt->name + "' is private to module '" + structOpt->moduleId + "' (missing 'public').";
                        setAnalyzerError(exprNode.line, exprNode.col, msg);
                    }
                    if (structOpt->moduleId != moduleId) {
                        for (const auto& fld : structOpt->fieldOrder) {
                            if (fld.isPrivate) {
                                setAnalyzerError(exprNode.line, exprNode.col, "Cannot construct type '" + structOpt->name + "' outside module '" + structOpt->moduleId + "' because field '" + fld.name + "' is private.");
                            }
                        }
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

                if (const auto* fa = arena.getExpr<FieldAccess>(call->callee)) {
                    const auto uOpt = resolveUnionVariantAccess(arena, symbols, moduleId, scope, *fa);
                    if (uOpt.has_value()) {
                        if (!uOpt->moduleId.empty() && uOpt->moduleId != moduleId) {
                            symbols.referencedModules.insert(uOpt->moduleId);
                        }
                        if (!uOpt->isPub && uOpt->moduleId != moduleId) {
                            setAnalyzerError(exprNode.line, exprNode.col, "Union '" + uOpt->name + "' is private to module '" + uOpt->moduleId + "' (missing 'public').");
                        }
                        const auto vTypeIt = uOpt->variantTypes.find(fa->fieldName);
                        if (vTypeIt == uOpt->variantTypes.end()) {
                            setAnalyzerError(exprNode.line, exprNode.col, "Union '" + uOpt->name + "' has no variant '" + fa->fieldName + "'.");
                        }
                        if (!call->namedArgs.empty() || call->args.size() != 1) {
                            setAnalyzerError(exprNode.line, exprNode.col, "Union variant constructor '" + uOpt->name + "." + fa->fieldName + "(...)' requires exactly 1 positional argument.");
                        }
                        const TypeId expectedPayloadTid = resolveAlias(arena, symbols, uOpt->moduleId, vTypeIt->second);
                        validateNonNullTarget(exprNode.line, exprNode.col, expectedPayloadTid, call->args[0]);
                        const TypeId actualArgTid = resolveAlias(arena, symbols, moduleId, inferExprType(arena, call->args[0], symbols, scope, moduleId));
                        if (expectedPayloadTid != symbols.typeAuto && actualArgTid != symbols.typeAuto && expectedPayloadTid != actualArgTid) {
                            setAnalyzerError(exprNode.line, exprNode.col, "Argument type does not match payload type for union variant '" + uOpt->name + "." + fa->fieldName + "'.");
                        }
                    }
                    if (fa->fieldName == "append") {
                        const TypeId recvTid = inferExprType(arena, fa->target, symbols, scope, moduleId);
                        if (recvTid < arena.types.size()) {
                            const TypeNode recvT = resolveAliasFromNode(arena, symbols, moduleId, arena.types[recvTid]);
                            if (recvT.name == "list") {
                                setAnalyzerError(exprNode.line, exprNode.col, "'.append()' is removed; use '.push()'.");
                            }
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
                if (!arena.getExpr<Identifier>(calleeFa->target) &&
                    !resolveUnionVariantAccess(arena, symbols, moduleId, scope, *calleeFa).has_value()) {
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
                    setAnalyzerError(exprNode.line, exprNode.col, "Enum '" + enumOpt->name + "' is private to module '" + enumOpt->moduleId + "' (missing 'public').");
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
                    setAnalyzerError(exprNode.line, exprNode.col, "Constant '" + cOpt->name + "' is private to module '" + cOpt->moduleId + "' (missing 'public').");
                }
                return;
            }
            const auto bareFnOpt = resolveBareFnRef(arena, exprId, symbols, scope, moduleId);
            if (bareFnOpt.has_value()) {
                if (!bareFnOpt->moduleId.empty() && bareFnOpt->moduleId != moduleId) {
                    symbols.referencedModules.insert(bareFnOpt->moduleId);
                }
                if (bareFnOpt->moduleId != moduleId && !bareFnOpt->isPub) {
                    setAnalyzerError(exprNode.line, exprNode.col, "Function '" + bareFnOpt->name + "' is private to module '" + bareFnOpt->moduleId + "' (missing 'public').");
                }
                return;
            }
            const TypeId targetTid = inferExprType(arena, faccess->target, symbols, scope, moduleId);
            const auto tsOpt = resolveTypeSymbol(arena, symbols, moduleId, targetTid);
            if (tsOpt.has_value()) {
                if (tsOpt->moduleId != moduleId && tsOpt->privateFields.count(faccess->fieldName) > 0) {
                    setAnalyzerError(exprNode.line, exprNode.col, "Field '" + faccess->fieldName + "' of type '" + tsOpt->name + "' is private to module '" + tsOpt->moduleId + "'.");
                }
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

    void ensureContainerTypesInExpr(
        ExprId exprId,
        const std::unordered_map<std::string, VarSymbol>& scope
    ) {
        if (exprId >= arena.exprs.size()) return;
        if (const auto* llit = arena.getExpr<ListLiteral>(exprId)) {
            for (const ExprId elId : llit->elements) {
                ensureContainerTypesInExpr(elId, scope);
            }
            if (!llit->elements.empty()) {
                const TypeId elTid = resolveAlias(arena, symbols, moduleId, inferExprType(arena, llit->elements[0], symbols, scope, moduleId));
                arena.findOrAddType("list", {elTid});
            }
        } else if (const auto* dlit = arena.getExpr<DictLiteral>(exprId)) {
            for (const auto& ent : dlit->entries) {
                ensureContainerTypesInExpr(ent.key, scope);
                ensureContainerTypesInExpr(ent.value, scope);
            }
            if (!dlit->entries.empty()) {
                const TypeId kTid = resolveAlias(arena, symbols, moduleId, inferExprType(arena, dlit->entries[0].key, symbols, scope, moduleId));
                const TypeId vTid = resolveAlias(arena, symbols, moduleId, inferExprType(arena, dlit->entries[0].value, symbols, scope, moduleId));
                arena.findOrAddType("dict", {kTid, vTid});
                arena.findOrAddType("list", {kTid});
            }
        } else if (const auto* call = arena.getExpr<FuncCall>(exprId)) {
            for (const ExprId argId : call->args) {
                ensureContainerTypesInExpr(argId, scope);
            }
            for (const auto& na : call->namedArgs) {
                ensureContainerTypesInExpr(na.value, scope);
            }
        } else if (const auto* bop = arena.getExpr<BinaryOp>(exprId)) {
            ensureContainerTypesInExpr(bop->left, scope);
            ensureContainerTypesInExpr(bop->right, scope);
        } else if (const auto* uop = arena.getExpr<UnaryOp>(exprId)) {
            ensureContainerTypesInExpr(uop->operand, scope);
        } else if (const auto* fa = arena.getExpr<FieldAccess>(exprId)) {
            ensureContainerTypesInExpr(fa->target, scope);
        } else if (const auto* ia = arena.getExpr<IndexAccess>(exprId)) {
            ensureContainerTypesInExpr(ia->target, scope);
            ensureContainerTypesInExpr(ia->index, scope);
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
        VarSymbol sym{name, tid, isByref, isConst, fileName, line, col, line};
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
        if (resolvedTarget < arena.types.size() && arena.types[resolvedTarget].name == "fn") {
            if (resolvedVal != symbols.typeAuto && resolvedVal != resolvedTarget) {
                setAnalyzerError(line, col, "Cannot assign mismatched function signature to '" + std::string(varName) + "'.");
            }
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

    void validateDefaultValueExpr(
        const FnDecl& dummyFn,
        uint32_t line,
        uint32_t col,
        std::string_view itemName,
        TypeId rawTypeId,
        ExprId defaultExprId,
        const std::unordered_map<std::string, VarSymbol>& emptyScope
    ) {
        const TypeId resolvedTid = resolveAlias(arena, symbols, moduleId, rawTypeId);
        const TypeId valTid = inferExprType(arena, defaultExprId, symbols, emptyScope, moduleId);
        validateNonNullTarget(line, col, resolvedTid, defaultExprId);
        validateEnumAssignment(line, col, itemName, resolvedTid, valTid);
        validateExprCalls(dummyFn, defaultExprId, emptyScope, false);
    }

    void validateDeclDefaults(DeclId did, const DeclNode& declNode) {
        std::unordered_map<std::string, VarSymbol> emptyScope;
        if (const auto* cdecl = arena.getDecl<ConstDecl>(did)) {
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
            FnDecl dummyFn{};
            dummyFn.name = tdecl->name;
            dummyFn.isFallible = false;
            for (const auto& fld : tdecl->fields) {
                if (fld.hasDefault && fld.defaultValue != INVALID_ID) {
                    validateDefaultValueExpr(dummyFn, fld.line, fld.col, fld.name, fld.typeId, fld.defaultValue, emptyScope);
                }
            }
        } else if (const auto* fdecl = arena.getDecl<FnDecl>(did)) {
            FnDecl dummyFn{};
            dummyFn.name = fdecl->name;
            dummyFn.isFallible = false;
            for (const auto& p : fdecl->params) {
                if (p.hasDefault && p.defaultValue != INVALID_ID) {
                    validateDefaultValueExpr(dummyFn, p.line, p.col, p.name, p.typeId, p.defaultValue, emptyScope);
                }
            }
        }
    }

    void analyzeStmt(
        const FnDecl& fn,
        StmtId stmtId,
        std::unordered_map<std::string, VarSymbol>& scope
    ) {
        if (stmtId >= arena.stmts.size()) {
            return;
        }

        const auto& stmtNode = arena.stmts[stmtId];

        if (auto* vassign = arena.getStmtMut<VarAssignment>(stmtId)) {
            ensureContainerTypesInExpr(vassign->value, scope);
            ExprId fallibleCheckExpr = vassign->value;
            if (vassign->compoundRhs != INVALID_ID) {
                fallibleCheckExpr = vassign->compoundRhs;
            }
            checkFallibleUsage(fn, fallibleCheckExpr, scope, vassign->implicitBubble, vassign->breaksToCatch);

            const TypeId valType = inferExprType(arena, vassign->value, symbols, scope, moduleId);
            if (const auto* ident = arena.getExpr<Identifier>(vassign->target)) {
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
                std::string targetLabel = root.name;
                if (const auto* fa = arena.getExpr<FieldAccess>(vassign->target)) {
                    targetLabel = fa->fieldName;
                }
                validateEnumAssignment(tgtNode.line, tgtNode.col, targetLabel, lhsTid, valType);
                validateExprCalls(fn, vassign->target, scope, false);
            }

            if (vassign->compoundRhs != INVALID_ID) {
                validateExprCalls(fn, vassign->target, scope, false);
                validateExprCalls(fn, vassign->compoundRhs, scope, true);
            } else {
                validateExprCalls(fn, vassign->value, scope, true);
            }
            return;
        }

        if (auto* estmt = arena.getStmtMut<ExprStmt>(stmtId)) {
            checkFallibleUsage(fn, estmt->expr, scope, estmt->implicitBubble, estmt->breaksToCatch);
            validateExprCalls(fn, estmt->expr, scope, true);
            return;
        }

        if (const auto* tryCatch = arena.getStmt<TryCatchStmt>(stmtId)) {
            auto tryScope = scope;
            activeTryDepth += 1;
            analyzeBlock(fn, tryCatch->tryBody, tryScope);
            activeTryDepth -= 1;

            auto catchScope = scope;
            bindLocalVar(catchScope, tryCatch->catchVarName, symbols.typeErr, false, false, stmtNode.line, stmtNode.col);
            analyzeBlock(fn, tryCatch->catchBody, catchScope);
            return;
        }

        if (const auto* ifStmt = arena.getStmt<IfStmt>(stmtId)) {
            validateExprCalls(fn, ifStmt->condition, scope, false);
            analyzeBlock(fn, ifStmt->thenBody, scope);

            for (const auto& branch : ifStmt->elifBranches) {
                validateExprCalls(fn, branch.condition, scope, false);
                analyzeBlock(fn, branch.body, scope);
            }

            if (ifStmt->hasElse) {
                analyzeBlock(fn, ifStmt->elseBody, scope);
            }
            return;
        }

        if (auto* forStmt = arena.getStmtMut<ForStmt>(stmtId)) {
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
            } else if (forStmt->iterable != INVALID_ID) {
                checkFallibleUsage(fn, forStmt->iterable, scope, forStmt->implicitBubble, forStmt->breaksToCatch);
                validateExprCalls(fn, forStmt->iterable, scope, true);
                const TypeId iterTid = inferExprType(arena, forStmt->iterable, symbols, scope, moduleId);
                if (iterTid < arena.types.size()) {
                    const TypeNode iterT = resolveAliasNode(arena, symbols, moduleId, iterTid);
                    if (iterT.name == "list" && !iterT.typeArgs.empty() && !forStmt->loopVars.empty()) {
                        if (forStmt->loopVars.size() == 1) {
                            bindLocalVar(scope, forStmt->loopVars[0], iterT.typeArgs[0], false, true, stmtNode.line, stmtNode.col);
                        } else if (forStmt->loopVars.size() == 2) {
                            bindLocalVar(scope, forStmt->loopVars[0], symbols.typeInt, false, true, stmtNode.line, stmtNode.col);
                            bindLocalVar(scope, forStmt->loopVars[1], iterT.typeArgs[0], false, true, stmtNode.line, stmtNode.col);
                        } else {
                            setAnalyzerError(stmtNode.line, stmtNode.col, "List iteration supports at most 2 loop variables ('for item in list' or 'for i, item in list').");
                        }
                    } else if (iterT.name == "bytes" && !forStmt->loopVars.empty()) {
                        if (forStmt->loopVars.size() == 1) {
                            bindLocalVar(scope, forStmt->loopVars[0], symbols.typeInt, false, true, stmtNode.line, stmtNode.col);
                        } else if (forStmt->loopVars.size() == 2) {
                            bindLocalVar(scope, forStmt->loopVars[0], symbols.typeInt, false, true, stmtNode.line, stmtNode.col);
                            bindLocalVar(scope, forStmt->loopVars[1], symbols.typeInt, false, true, stmtNode.line, stmtNode.col);
                        } else {
                            setAnalyzerError(stmtNode.line, stmtNode.col, "Bytes iteration supports at most 2 loop variables ('for b in data' or 'for i, b in data').");
                        }
                    } else if (iterT.name == "str" && !forStmt->loopVars.empty()) {
                        if (forStmt->loopVars.size() == 1) {
                            bindLocalVar(scope, forStmt->loopVars[0], symbols.typeStr, false, true, stmtNode.line, stmtNode.col);
                        } else if (forStmt->loopVars.size() == 2) {
                            bindLocalVar(scope, forStmt->loopVars[0], symbols.typeInt, false, true, stmtNode.line, stmtNode.col);
                            bindLocalVar(scope, forStmt->loopVars[1], symbols.typeStr, false, true, stmtNode.line, stmtNode.col);
                        } else {
                            setAnalyzerError(stmtNode.line, stmtNode.col, "String iteration supports at most 2 loop variables ('for ch in s' or 'for i, ch in s').");
                        }
                    } else if (iterT.name == "dict" && iterT.typeArgs.size() == 2) {
                        if (forStmt->loopVars.size() >= 1) {
                            bindLocalVar(scope, forStmt->loopVars[0], iterT.typeArgs[0], false, true, stmtNode.line, stmtNode.col);
                        }
                        if (forStmt->loopVars.size() >= 2) {
                            bindLocalVar(scope, forStmt->loopVars[1], iterT.typeArgs[1], false, true, stmtNode.line, stmtNode.col);
                        }
                    } else {
                        for (const auto& lv : forStmt->loopVars) {
                            bindLocalVar(scope, lv, symbols.typeAuto, false, true, stmtNode.line, stmtNode.col);
                        }
                    }
                }
            }
            analyzeBlock(fn, forStmt->body, scope);
            loopDepth -= 1;
            return;
        }

        if (auto* whileStmt = arena.getStmtMut<WhileStmt>(stmtId)) {
            loopDepth += 1;
            bool isFallibleCond = false;
            if (const auto* call = arena.getExpr<FuncCall>(whileStmt->condition)) {
                const auto fnOpt = resolveFuncCall(arena, *call, symbols, moduleId, scope);
                if (fnOpt.has_value() && fnOpt->isFallible) {
                    isFallibleCond = true;
                }
            }
            if (isFallibleCond) {
                checkFallibleUsage(fn, whileStmt->condition, scope, whileStmt->implicitBubble, whileStmt->breaksToCatch);
                validateExprCalls(fn, whileStmt->condition, scope, true);
            } else {
                validateExprCalls(fn, whileStmt->condition, scope, false);
            }
            analyzeBlock(fn, whileStmt->body, scope);
            loopDepth -= 1;
            return;
        }

        if (const auto* selStmt = arena.getStmt<SelectStmt>(stmtId)) {
            validateExprCalls(fn, selStmt->target, scope, false);

            const TypeId targetTid = inferExprType(arena, selStmt->target, symbols, scope, moduleId);
            const bool isEnumTarget = isEnumType(symbols, targetTid, arena, moduleId);
            const auto unionOpt = resolveUnionSymbol(arena, symbols, moduleId, targetTid);
            const bool isUnionTarget = unionOpt.has_value();
            if (targetTid != symbols.typeInt && targetTid != symbols.typeBool && targetTid != symbols.typeStr && !isEnumTarget && !isUnionTarget) {
                setAnalyzerError(stmtNode.line, stmtNode.col, "select case expression must be of type 'int', 'bool', or 'str' (or enum or union)");
            }

            for (const auto& scase : selStmt->cases) {
                if (isUnionTarget) {
                    TypeId boundPayloadTid = INVALID_ID;
                    for (const auto& mExpr : scase.matchExprs) {
                        std::string varName;
                        if (const auto* ident = arena.getExpr<Identifier>(mExpr)) {
                            varName = ident->name;
                        } else if (const auto* fa = arena.getExpr<FieldAccess>(mExpr)) {
                            const auto uAccOpt = resolveUnionVariantAccess(arena, symbols, moduleId, scope, *fa);
                            if (!uAccOpt.has_value() || uAccOpt->key != unionOpt->key) {
                                setAnalyzerError(scase.line, scase.col, "case variant does not match target union '" + unionOpt->name + "'");
                            }
                            varName = fa->fieldName;
                        } else {
                            setAnalyzerError(scase.line, scase.col, "Expected variant name in 'select case' on union '" + unionOpt->name + "'");
                        }
                        const auto vtIt = unionOpt->variantTypes.find(varName);
                        if (vtIt == unionOpt->variantTypes.end()) {
                            setAnalyzerError(scase.line, scase.col, "Union '" + unionOpt->name + "' has no variant '" + varName + "'");
                        }
                        boundPayloadTid = vtIt->second;
                    }
                    auto caseScope = scope;
                    if (!scase.bindingName.empty() && boundPayloadTid != INVALID_ID) {
                        bindLocalVar(caseScope, scase.bindingName, boundPayloadTid, false, false, scase.line, scase.col);
                    }
                    analyzeBlock(fn, scase.body, caseScope);
                    for (const auto& kv : caseScope) {
                        if (kv.first != scase.bindingName) {
                            scope[kv.first] = kv.second;
                        }
                    }
                } else {
                    if (!scase.bindingName.empty()) {
                        setAnalyzerError(scase.line, scase.col, "Case payload binding ('as <ident>') is only supported when 'select case' target is a union");
                    }
                    for (const auto& mExpr : scase.matchExprs) {
                        validateExprCalls(fn, mExpr, scope, false);
                        const TypeId mExprTid = inferExprType(arena, mExpr, symbols, scope, moduleId);
                        if (mExprTid != targetTid && mExprTid != symbols.typeAuto) {
                            setAnalyzerError(scase.line, scase.col, "case expression type does not match select case target type");
                        }
                    }
                    analyzeBlock(fn, scase.body, scope);
                }
            }

            if (selStmt->hasCaseElse) {
                analyzeBlock(fn, selStmt->caseElse, scope);
            }
            return;
        }

        if (const auto* dstmt = arena.getStmt<DeferStmt>(stmtId)) {
            const int savedTryDepth = activeTryDepth;
            activeTryDepth = 0;
            analyzeBlock(fn, dstmt->body, scope);
            activeTryDepth = savedTryDepth;
            return;
        }

        if (auto* retStmt = arena.getStmtMut<ReturnStmt>(stmtId)) {
            bool isReturnErrorCall = false;
            bool isReturnVoidFallibleCall = false;
            if (retStmt->hasValue) {
                if (const auto* call = arena.getExpr<FuncCall>(retStmt->value)) {
                    if (isErrorBuiltinCall(arena, *call) || isAssertBuiltinCall(arena, *call)) {
                        isReturnErrorCall = true;
                        if (activeTryDepth > 0) {
                            retStmt->breaksToCatch = true;
                        }
                    } else {
                        const auto fnSymOpt = resolveFuncCall(arena, *call, symbols, moduleId, scope);
                        if (fnSymOpt.has_value() && fnSymOpt->isFallible &&
                            (fnSymOpt->returnType == INVALID_ID || arena.isVoid(fnSymOpt->returnType))) {
                            isReturnVoidFallibleCall = true;
                        }
                    }
                }
                if (!isReturnErrorCall) {
                    checkFallibleUsage(fn, retStmt->value, scope, retStmt->implicitBubble, retStmt->breaksToCatch);
                }
                validateExprCalls(fn, retStmt->value, scope, true);
            }
            if (!isReturnErrorCall) {
                const bool expectedHasValue = (fn.returnType != INVALID_ID && !arena.isVoid(fn.returnType));
                if (isReturnVoidFallibleCall) {
                    if (expectedHasValue) {
                        std::string msg = "Function '" + fn.name + "' expects a return value, but none was provided.";
                        setAnalyzerError(stmtNode.line, stmtNode.col, msg);
                    }
                } else if (retStmt->hasValue && !expectedHasValue) {
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
        std::unordered_map<std::string, VarSymbol>& scope
    ) {
        for (const StmtId sid : stmts) {
            analyzeStmt(fn, sid, scope);
        }
    }

    void analyzeFn(const FnDecl& fn) {
        loopDepth = 0;
        activeTryDepth = 0;
        currentFnKey = symbolKey(moduleId, fn.name);
        std::unordered_map<std::string, VarSymbol> scope;
        for (const auto& param : fn.params) {
            const TypeId resolvedTid = canonicalizeType(arena, symbols, moduleId, param.typeId);
            const bool isConst = !param.isByref;
            bindLocalVar(scope, param.name, resolvedTid, param.isByref, isConst, param.line, param.col);
        }

        analyzeBlock(fn, fn.body, scope);
        auto& savedScope = symbols.fnScopes[currentFnKey];
        for (auto& kv : scope) {
            if (savedScope.find(kv.first) == savedScope.end()) {
                savedScope.emplace(kv.first, std::move(kv.second));
            }
        }
        currentFnKey.clear();
    }

    void analyzeProgram(const Program& program) {
        fileName = program.fileName;

        std::unordered_set<std::string> knownModules;
        knownModules.insert("");

        // Two-Pass Semantic Analysis:
        // - Pass 1a: Register module imports, type aliases, and type/enum/union shell declarations so
        //   out-of-order and cross-module type references are visible before inspecting fields/signatures.
        // - Pass 1b: Canonicalize struct fields, union variants, enum values, module constants, and
        //   function signatures (including overload sets).
        // - Pass 2: Type-check function bodies, infer local variable types, enforce const/byref mutability,
        //   and verify static 'can error' / 'try / catch' handling.
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
                    if (resolvedMod == "std/str") {
                        symbols.hasStdStr = true;
                    }
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
                asym.docComment = declNode.docComment;
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
                tsym.docComment = declNode.docComment;
                const std::string keyCopy = tsym.key;
                symbols.types[keyCopy] = std::move(tsym);
                arena.findOrAddType(keyCopy, {});
            } else if (const auto* edecl = arena.getDecl<EnumDecl>(did)) {
                EnumSymbol esym{};
                esym.name = edecl->name;
                esym.moduleId = declNode.moduleId;
                esym.key = symbolKey(declNode.moduleId, edecl->name);
                esym.isPub = edecl->isPub;
                esym.cppName = makeCppTypeName(declNode.moduleId, edecl->name, false);
                esym.fileName = declNode.fileName;
                esym.line = declNode.nameLine;
                esym.col = declNode.nameCol;
                esym.endLine = declNode.endLine;
                esym.docComment = declNode.docComment;
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
            } else if (const auto* udecl = arena.getDecl<UnionDecl>(did)) {
                UnionSymbol usym{};
                usym.name = udecl->name;
                usym.moduleId = declNode.moduleId;
                usym.key = symbolKey(declNode.moduleId, udecl->name);
                usym.isPub = udecl->isPub;
                usym.cppName = makeCppTypeName(declNode.moduleId, udecl->name, false);
                usym.fileName = declNode.fileName;
                usym.line = declNode.nameLine;
                usym.col = declNode.nameCol;
                usym.endLine = declNode.endLine;
                usym.docComment = declNode.docComment;
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

        for (auto& kv : symbols.aliases) {
            kv.second.targetType = canonicalizeType(arena, symbols, kv.second.moduleId, kv.second.targetType);
        }

        // Pass 1b: Resolve struct field types, union variant types, constants, and function signatures
        for (const DeclId did : program.declarations) {
            if (did >= arena.decls.size()) continue;
            const auto& declNode = arena.decls[did];

            if (const auto* tdecl = arena.getDecl<TypeDecl>(did)) {
                const std::string key = symbolKey(declNode.moduleId, tdecl->name);
                auto& tsym = symbols.types[key];
                tsym.fieldOrder.clear();
                tsym.privateFields.clear();
                tsym.fieldOrder.reserve(tdecl->fields.size());
                for (const auto& fld : tdecl->fields) {
                    const TypeId resolvedFid = canonicalizeType(arena, symbols, declNode.moduleId, fld.typeId);
                    tsym.fields[fld.name] = resolvedFid;
                    if (fld.isPrivate) {
                        tsym.privateFields.insert(fld.name);
                    }
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
                const TypeId inferredTid = inferExprType(arena, cdecl->value, symbols, emptyScope, declNode.moduleId);
                TypeId constTid = inferredTid;
                if (cdecl->hasDeclaredType) {
                    constTid = canonicalizeType(arena, symbols, declNode.moduleId, cdecl->declaredType);
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
                csym.docComment = declNode.docComment;
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
                fsym.docComment = declNode.docComment;
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
                fsym.fnTypeId = addFnTypeId(arena, fsym);
                const auto existing = symbols.functions.find(fsym.key);
                if (existing != symbols.functions.end()) {
                    existing->second.overloads.push_back(std::move(fsym));
                } else {
                    symbols.functions[fsym.key] = std::move(fsym);
                }
            }
        }

        // Validate that composite value types (structs and unions) have no direct by-value recursive cycles
        {
            std::unordered_map<std::string, int> visitState;

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
                    if (st == 0) {
                        self(self, depKey);
                    }
                }
                visitState[currKey] = 2;
            };

            for (const DeclId did : program.declarations) {
                if (did >= arena.decls.size()) continue;
                const auto& declNode = arena.decls[did];
                if (const auto* tdecl = arena.getDecl<TypeDecl>(did)) {
                    if (!tdecl->isExtern) {
                        const std::string key = symbolKey(declNode.moduleId, tdecl->name);
                        if (visitState[key] == 0) {
                            dfsCycleCheck(dfsCycleCheck, key);
                        }
                    }
                } else if (const auto* udecl = arena.getDecl<UnionDecl>(did)) {
                    const std::string key = symbolKey(declNode.moduleId, udecl->name);
                    if (visitState[key] == 0) {
                        dfsCycleCheck(dfsCycleCheck, key);
                    }
                }
            }
        }

        // Pass 2: Validate top-level constants, defaults, and function bodies
        for (const DeclId did : program.declarations) {
            if (did >= arena.decls.size()) continue;
            const auto& declNode = arena.decls[did];
            fileName = declNode.fileName;
            moduleId = declNode.moduleId;
            validateDeclDefaults(did, declNode);
            if (const auto* fdecl = arena.getDecl<FnDecl>(did)) {
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
