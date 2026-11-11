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

    TypeId inferExpr(ExprId exprId, const std::unordered_map<std::string, VarSymbol>& scope) {
        return inferAndCacheExprType(arena, exprId, symbols, scope, moduleId);
    }

    void recordCalleeEdge(const FnSymbol& fnSym) {
        if (fnSym.isBuiltinReceiver || fnSym.isFnValueCall) {
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
        const std::unordered_map<std::string, VarSymbol>& scope
    ) const {
        if (const auto* call = arena.getExpr<FuncCall>(exprId)) {
            bool isFallible = false;
            std::string calleeName = "";
            if (isAssertBuiltinCall(arena, *call)) {
                isFallible = true;
                calleeName = "assert";
            } else if (isErrorBuiltinCall(arena, *call) && activeTryDepth > 0) {
                return;
            } else {
                const auto fnSymOpt = resolveFuncCall(arena, *call, symbols, moduleId, scope);
                if (fnSymOpt.has_value()) {
                    isFallible = fnSymOpt->isFallible;
                    calleeName = fnSymOpt->name;
                }
            }

            if (isFallible && activeTryDepth <= 0 && !fn.isFallible) {
                const auto& exprNode = arena.exprs[exprId];
                std::string msg = "Unhandled error: function returns an error but caller does not declare 'can error' or have an active 'try / catch' block (function '" +
                    fn.name + "' calls fallible function '" + calleeName + "').";
                setAnalyzerError(exprNode.line, exprNode.col, msg);
            }
        }
    }

    template <typename Sym>
    void checkCrossModuleAccess(
        uint32_t line,
        uint32_t col,
        std::string_view kindLabel,
        const Sym& sym,
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

        inferExpr(exprId, scope);
        const auto& exprNode = arena.exprs[exprId];

        if (arena.getExpr<Identifier>(exprId) != nullptr) {
            const auto bareFnOpt = resolveBareFnRef(arena, exprId, symbols, scope, moduleId);
            if (bareFnOpt.has_value()) {
                recordCalleeEdge(*bareFnOpt);
                checkCrossModuleAccess(exprNode.line, exprNode.col, "Function", *bareFnOpt);
            }
            return;
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
                if (!call->namedArgs.empty()) {
                    setAnalyzerError(exprNode.line, exprNode.col, "Named arguments are not supported in 'assert(...)'.");
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
                        validateNonNullTarget(exprNode.line, exprNode.col, expectedParamTid, call->args[aIdx]);
                        const TypeId actualArgTid = resolveAlias(arena, symbols, moduleId, inferExpr(call->args[aIdx], scope));
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
                            validateByrefArg(exprNode.line, exprNode.col, call->args[aIdx], scope, fnSym, fnSym.params[pIdx].name);
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
                                validateByrefArg(exprNode.line, exprNode.col, namedArg.value, scope, fnSym, p.name);
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
                            checkCrossModuleAccess(exprNode.line, exprNode.col, "Function", fnSym, calleeFa->fieldName);
                        }
                    }
                }
            } else {
                const auto structOpt = resolveStructConstructor(arena, *call, symbols, moduleId, scope);
                if (structOpt.has_value()) {
                    checkCrossModuleAccess(exprNode.line, exprNode.col, "Type", *structOpt);
                    if (structOpt->isExtern && structOpt->fieldOrder.empty()) {
                        setAnalyzerError(exprNode.line, exprNode.col, "Cannot directly construct opaque extern type '" + structOpt->name + "'.");
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
                        checkCrossModuleAccess(exprNode.line, exprNode.col, "Union", *uOpt);
                        const auto vTypeIt = uOpt->variantTypes.find(fa->fieldName);
                        if (vTypeIt == uOpt->variantTypes.end()) {
                            setAnalyzerError(exprNode.line, exprNode.col, "Union '" + uOpt->name + "' has no variant '" + fa->fieldName + "'.");
                        }
                        if (!call->namedArgs.empty() || call->args.size() != 1) {
                            setAnalyzerError(exprNode.line, exprNode.col, "Union variant constructor '" + uOpt->name + "." + fa->fieldName + "(...)' requires exactly 1 positional argument.");
                        }
                        const TypeId expectedPayloadTid = resolveAlias(arena, symbols, uOpt->moduleId, vTypeIt->second);
                        validateNonNullTarget(exprNode.line, exprNode.col, expectedPayloadTid, call->args[0]);
                        const TypeId actualArgTid = resolveAlias(arena, symbols, moduleId, inferExpr(call->args[0], scope));
                        if (expectedPayloadTid != symbols.typeAuto && actualArgTid != symbols.typeAuto && expectedPayloadTid != actualArgTid) {
                            setAnalyzerError(exprNode.line, exprNode.col, "Argument type does not match payload type for union variant '" + uOpt->name + "." + fa->fieldName + "'.");
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
            const TypeId lt = inferExpr(bop->left, scope);
            const TypeId rt = inferExpr(bop->right, scope);
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
                checkCrossModuleAccess(exprNode.line, exprNode.col, "Enum", *enumOpt);
                if (enumOpt->memberValues.count(faccess->fieldName) == 0) {
                    setAnalyzerError(exprNode.line, exprNode.col, "Enum '" + enumOpt->name + "' has no member '" + faccess->fieldName + "'.");
                }
                return;
            }
            const auto cOpt = resolveModuleConstAccess(arena, symbols, moduleId, scope, *faccess);
            if (cOpt.has_value()) {
                checkCrossModuleAccess(exprNode.line, exprNode.col, "Constant", *cOpt);
                return;
            }
            const auto bareFnOpt = resolveBareFnRef(arena, exprId, symbols, scope, moduleId);
            if (bareFnOpt.has_value()) {
                recordCalleeEdge(*bareFnOpt);
                checkCrossModuleAccess(exprNode.line, exprNode.col, "Function", *bareFnOpt);
                return;
            }
            const TypeId targetTid = inferExpr(faccess->target, scope);
            const auto tsOpt = resolveTypeSymbol(arena, symbols, moduleId, targetTid);
            if (tsOpt.has_value()) {
                if (tsOpt->fields.find(faccess->fieldName) == tsOpt->fields.end()) {
                    setAnalyzerError(exprNode.line, exprNode.col, "Type '" + tsOpt->name + "' has no field '" + faccess->fieldName + "'.");
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
        const TypeId valTid = inferExpr(defaultExprId, emptyScope);
        validateNonNullTarget(line, col, resolvedTid, defaultExprId);
        validateEnumAssignment(line, col, itemName, resolvedTid, valTid);
        validateExprCalls(dummyFn, defaultExprId, emptyScope, false);
    }

    void validateDeclDefaults(DeclId did, const DeclNode& declNode) {
        std::unordered_map<std::string, VarSymbol> emptyScope;
        if (const auto* cdecl = arena.getDecl<ConstDecl>(did)) {
            const std::string key = symbolKey(declNode.moduleId, cdecl->name);
            const TypeId valTid = inferExpr(cdecl->value, emptyScope);
            TypeId targetTid = valTid;
            if (symbols.constants.count(key) > 0) {
                targetTid = symbols.constants.at(key).typeId;
            }
            validateEnumAssignment(declNode.line, declNode.col, cdecl->name, targetTid, valTid);
            FnDecl dummyFn{};
            dummyFn.name = cdecl->name;
            dummyFn.isFallible = false;
            currentFnKey = "__top__" + declNode.moduleId;
            validateExprCalls(dummyFn, cdecl->value, emptyScope, false);
            currentFnKey.clear();
        } else if (const auto* fdecl = arena.getDecl<FnDecl>(did)) {
            FnDecl dummyFn{};
            dummyFn.name = fdecl->name;
            dummyFn.isFallible = false;
            currentFnKey = symbolKey(declNode.moduleId, fdecl->name);
            for (const auto& p : fdecl->params) {
                if (p.hasDefault && p.defaultValue != INVALID_ID) {
                    validateDefaultValueExpr(dummyFn, p.line, p.col, p.name, p.typeId, p.defaultValue, emptyScope);
                }
            }
            currentFnKey.clear();
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

        if (const auto* vassign = arena.getStmt<VarAssignment>(stmtId)) {
            checkFallibleUsage(fn, vassign->value, scope);

            const TypeId valType = inferExpr(vassign->value, scope);
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
                validateNonNullTarget(tgtNode.line, tgtNode.col, lhsTid, vassign->value);
                std::string targetLabel = root.name;
                if (const auto* fa = arena.getExpr<FieldAccess>(vassign->target)) {
                    targetLabel = fa->fieldName;
                }
                validateEnumAssignment(tgtNode.line, tgtNode.col, targetLabel, lhsTid, valType);
                validateExprCalls(fn, vassign->target, scope, false);
            }

            validateExprCalls(fn, vassign->value, scope, true);
            return;
        }

        if (const auto* estmt = arena.getStmt<ExprStmt>(stmtId)) {
            checkFallibleUsage(fn, estmt->expr, scope);
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

        if (const auto* forStmt = arena.getStmt<ForStmt>(stmtId)) {
            loopDepth += 1;
            auto loopScope = scope;
            if (forStmt->rangeSpec.hasRange) {
                validateExprCalls(fn, forStmt->rangeSpec.start, scope, false);
                validateExprCalls(fn, forStmt->rangeSpec.end, scope, false);
                if (forStmt->rangeSpec.hasStep) {
                    validateExprCalls(fn, forStmt->rangeSpec.step, scope, false);
                }
                if (!forStmt->loopVars.empty()) {
                    bindLocalVar(loopScope, forStmt->loopVars[0], symbols.typeInt, false, false, stmtNode.line, stmtNode.col);
                }
            } else if (forStmt->iterable != INVALID_ID) {
                validateExprCalls(fn, forStmt->iterable, scope, false);
                std::string loopErr;
                const auto bindings = resolveForInLoopVars(arena, symbols, moduleId, *forStmt, scope, &loopErr);
                if (!loopErr.empty()) {
                    setAnalyzerError(stmtNode.line, stmtNode.col, loopErr);
                }
                for (const auto& kv : bindings) {
                    bindLocalVar(loopScope, kv.first, kv.second, false, true, stmtNode.line, stmtNode.col);
                }
            }
            analyzeBlock(fn, forStmt->body, loopScope);
            loopDepth -= 1;
            return;
        }

        if (const auto* whileStmt = arena.getStmt<WhileStmt>(stmtId)) {
            loopDepth += 1;
            validateExprCalls(fn, whileStmt->condition, scope, false);
            analyzeBlock(fn, whileStmt->body, scope);
            loopDepth -= 1;
            return;
        }

        if (const auto* selStmt = arena.getStmt<SelectStmt>(stmtId)) {
            validateExprCalls(fn, selStmt->target, scope, false);

            const TypeId targetTid = inferExpr(selStmt->target, scope);
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
                        boundPayloadTid = varOpt->payloadType;
                    }
                    auto caseScope = scope;
                    if (!scase.bindingName.empty() && boundPayloadTid != INVALID_ID) {
                        bindLocalVar(caseScope, scase.bindingName, boundPayloadTid, false, true, scase.line, scase.col);
                    }
                    analyzeBlock(fn, scase.body, caseScope);
                } else {
                    if (!scase.bindingName.empty()) {
                        setAnalyzerError(scase.line, scase.col, "Case payload binding ('as <ident>') is only supported when 'select case' target is a union");
                    }
                    for (const auto& mExpr : scase.matchExprs) {
                        validateExprCalls(fn, mExpr, scope, false);
                        const TypeId mExprTid = inferExpr(mExpr, scope);
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
            FnDecl deferFn{};
            deferFn.name = "defer";
            deferFn.returnType = symbols.typeVoid;
            deferFn.isFallible = false;
            analyzeBlock(deferFn, dstmt->body, scope);
            activeTryDepth = savedTryDepth;
            return;
        }

        if (const auto* retStmt = arena.getStmt<ReturnStmt>(stmtId)) {
            bool isReturnErrorCall = false;
            bool isReturnVoidFallibleCall = false;
            if (retStmt->hasValue) {
                if (const auto* call = arena.getExpr<FuncCall>(retStmt->value)) {
                    if (isErrorBuiltinCall(arena, *call) || isAssertBuiltinCall(arena, *call)) {
                        isReturnErrorCall = true;
                    } else {
                        const auto fnSymOpt = resolveFuncCall(arena, *call, symbols, moduleId, scope);
                        if (fnSymOpt.has_value() && fnSymOpt->isFallible &&
                            (fnSymOpt->returnType == INVALID_ID || arena.isVoid(fnSymOpt->returnType))) {
                            isReturnVoidFallibleCall = true;
                        }
                    }
                }
                if (!isReturnErrorCall) {
                    checkFallibleUsage(fn, retStmt->value, scope);
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
        std::unordered_map<std::string, VarSymbol> scope
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
                if (arena.getExpr<NullLiteral>(cdecl->value) != nullptr && constTid != symbols.typeCptr) {
                    throw makeError("AnalyzerError", "CleanBasic types use non-nullable value semantics; only 'cptr' can be 'null'.", declNode.fileName, declNode.line, declNode.col);
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
                fsym.fnTypeId = addFnTypeId(arena, fsym);
                if (symbols.functions.find(fsym.key) != symbols.functions.end()) {
                    setAnalyzerError(declNode.line, declNode.col, "Duplicate function definition '" + fdecl->name + "'.");
                }
                symbols.functions[fsym.key] = std::move(fsym);
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
                    if (declNode.moduleId.empty()) {
                        symbols.rootReferencedFunctions.insert(symbolKey(declNode.moduleId, fdecl->name));
                    }
                    analyzeFn(*fdecl);
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
        return result;
    } catch (const DiagnosticError& err) {
        result.ok = false;
        result.error = err;
        return result;
    }
}

} // namespace cb

#endif // CLEANBASIC_ANALYZER_HPP
