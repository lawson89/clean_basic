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

    Analyzer(AstArena& arena, GlobalSymbolTable& symbols, std::string_view fileName = "main.cb")
        : arena(arena), symbols(symbols), fileName(fileName), moduleId("") {}

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
            return RootVarInfo{ident->name, false, false};
        }
        if (const auto* fa = arena.getExpr<FieldAccess>(exprId)) {
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
        bool hasActiveOnError,
        bool& outImplicitBubble
    ) {
        outImplicitBubble = false;
        if (const auto* call = arena.getExpr<FuncCall>(exprId)) {
            const auto fnSymOpt = resolveFuncCall(arena, *call, symbols, moduleId);
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
                    } else {
                        // len, toStr, toInt, toFloat
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

                    const size_t paramOffset = isUfcs ? 1 : 0;
                    for (size_t aIdx = 0; aIdx < call->args.size(); ++aIdx) {
                        const size_t pIdx = paramOffset + aIdx;
                        if (pIdx < fnSym.params.size()) {
                            if (fnSym.params[pIdx].isByref) {
                                const auto root = findRootVar(call->args[aIdx], scope);
                                if (root.found && root.isConst) {
                                    setAnalyzerError(exprNode.line, exprNode.col, "Cannot pass const variable '" + root.name + "' to byref parameter '" + fnSym.params[pIdx].name + "'.");
                                }
                            }
                        }
                    }

                    for (const auto& namedArg : call->namedArgs) {
                        for (const auto& p : fnSym.params) {
                            if (p.name == namedArg.name && p.isByref) {
                                const auto root = findRootVar(namedArg.value, scope);
                                if (root.found && root.isConst) {
                                    setAnalyzerError(exprNode.line, exprNode.col, "Cannot pass const variable '" + root.name + "' to byref parameter '" + p.name + "'.");
                                }
                                break;
                            }
                        }
                    }

                    if (fnSym.isFallible && !allowTopLevelFallible) {
                        if (!fn.isFallible) {
                            std::string msg = "Function '" + fn.name + "' calls fallible function '" + fnSym.name +
                                "' without an 'on error' block, but '" + fn.name + "' does not declare 'can error' in its signature.";
                            setAnalyzerError(exprNode.line, exprNode.col, msg);
                        }
                    }
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
                    if (!structOpt->isPub && structOpt->moduleId != moduleId) {
                        std::string msg = "Type '" + structOpt->name + "' is private to module '" + structOpt->moduleId + "' (missing 'pub').";
                        setAnalyzerError(exprNode.line, exprNode.col, msg);
                    }
                }

                // Check for removed .append()
                if (const auto* fa = arena.getExpr<FieldAccess>(call->callee)) {
                    if (fa->fieldName == "append") {
                        const TypeId recvTid = inferExprType(arena, fa->target, symbols, scope, moduleId);
                        const TypeNode recvT = resolveAliasFromNode(arena, symbols, moduleId, arena.types[recvTid]);
                        if (recvT.name == "list") {
                            setAnalyzerError(exprNode.line, exprNode.col, "'.append()' is removed; use '.push()'.");
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
            return;
        }

        if (const auto* uop = arena.getExpr<UnaryOp>(exprId)) {
            validateExprCalls(fn, uop->operand, scope, false);
            return;
        }

        if (const auto* faccess = arena.getExpr<FieldAccess>(exprId)) {
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
            checkFallibleUsage(fn, vassign->value, hasActiveOnError, vassign->implicitBubble);

            const TypeId valType = inferExprType(arena, vassign->value, symbols, scope, moduleId);
            if (const auto* ident = arena.getExpr<Identifier>(vassign->target)) {
                const auto& tgtNode = arena.exprs[vassign->target];
                if (vassign->isDim || vassign->isConst) {
                    TypeId varTid = symbols.typeInt;
                    if (vassign->hasDeclaredType) {
                        varTid = resolveAlias(arena, symbols, moduleId, vassign->declaredType);
                    } else if (valType != INVALID_ID) {
                        varTid = valType;
                    }
                    bindLocalVar(scope, ident->name, varTid, false, vassign->isConst, tgtNode.line, tgtNode.col);
                } else {
                    const auto existing = scope.find(ident->name);
                    if (existing == scope.end()) {
                        std::string msg = "Undeclared variable '" + ident->name + "'. Use 'dim' or 'const' to declare variables.";
                        setAnalyzerError(tgtNode.line, tgtNode.col, msg);
                    }
                    if (existing->second.isConst) {
                        std::string msg = "Cannot reassign const variable '" + ident->name + "'.";
                        setAnalyzerError(tgtNode.line, tgtNode.col, msg);
                    }
                }
            } else {
                const auto& tgtNode = arena.exprs[vassign->target];
                const auto root = findRootVar(vassign->target, scope);
                if (root.found && root.isConst) {
                    std::string msg = "Cannot mutate const variable '" + root.name + "'.";
                    setAnalyzerError(tgtNode.line, tgtNode.col, msg);
                }
                validateExprCalls(fn, vassign->target, scope, false);
            }

            validateExprCalls(fn, vassign->value, scope, true);
            return;
        }

        if (auto* estmt = arena.getStmtMut<ExprStmt>(stmtId)) {
            checkFallibleUsage(fn, estmt->expr, hasActiveOnError, estmt->implicitBubble);
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
                    const auto& iterT = arena.types[iterTid];
                    if (iterT.name == "list" && !iterT.typeArgs.empty() && !forStmt->loopVars.empty()) {
                        bindLocalVar(scope, forStmt->loopVars[0], iterT.typeArgs[0], false, false, stmtNode.line, stmtNode.col);
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
            return;
        }

        if (const auto* whileStmt = arena.getStmt<WhileStmt>(stmtId)) {
            bool isFallibleCond = false;
            if (const auto* call = arena.getExpr<FuncCall>(whileStmt->condition)) {
                const auto fnOpt = resolveFuncCall(arena, *call, symbols, moduleId, scope);
                if (fnOpt.has_value() && fnOpt->isFallible) {
                    isFallibleCond = true;
                }
            }
            if (isFallibleCond) {
                bool dummyBubble = false;
                checkFallibleUsage(fn, whileStmt->condition, hasActiveOnError, dummyBubble);
                validateExprCalls(fn, whileStmt->condition, scope, true);
            } else {
                validateExprCalls(fn, whileStmt->condition, scope, false);
            }
            analyzeBlock(fn, whileStmt->body, scope, hasActiveOnError);
            return;
        }

        if (const auto* selStmt = arena.getStmt<SelectStmt>(stmtId)) {
            validateExprCalls(fn, selStmt->target, scope, false);

            const TypeId targetTid = inferExprType(arena, selStmt->target, symbols, scope, moduleId);
            if (targetTid != symbols.typeInt && targetTid != symbols.typeBool) {
                setAnalyzerError(stmtNode.line, stmtNode.col, "select case expression must be of type 'int' or 'bool'");
            }

            for (const auto& scase : selStmt->cases) {
                validateExprCalls(fn, scase.matchExpr, scope, false);
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
            if (retStmt->hasValue) {
                validateExprCalls(fn, retStmt->value, scope, false);
            }
            bool isReturnErrorCall = false;
            if (retStmt->hasValue) {
                if (const auto* call = arena.getExpr<FuncCall>(retStmt->value)) {
                    if (isErrorBuiltinCall(arena, *call)) {
                        isReturnErrorCall = true;
                    }
                }
            }
            if (!isReturnErrorCall) {
                const bool expectedHasValue = (fn.returnType != INVALID_ID && !arena.isVoid(fn.returnType));
                if (retStmt->hasValue && !expectedHasValue) {
                    std::string msg = "Function '" + fn.name + "' returns void, but a value was returned.";
                    setAnalyzerError(stmtNode.line, stmtNode.col, msg);
                } else if (!retStmt->hasValue && expectedHasValue) {
                    std::string msg = "Function '" + fn.name + "' expects a return value, but none was provided.";
                    setAnalyzerError(stmtNode.line, stmtNode.col, msg);
                }
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

        // Pass 1a: Register module imports, aliases, and type declarations
        for (const DeclId did : program.declarations) {
            if (did >= arena.decls.size()) continue;
            const auto& declNode = arena.decls[did];

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
            }
        }

        // Pass 1b: Resolve struct field types and register function signatures
        for (const DeclId did : program.declarations) {
            if (did >= arena.decls.size()) continue;
            const auto& declNode = arena.decls[did];

            if (const auto* tdecl = arena.getDecl<TypeDecl>(did)) {
                const std::string key = symbolKey(declNode.moduleId, tdecl->name);
                auto& tsym = symbols.types[key];
                for (const auto& fld : tdecl->fields) {
                    tsym.fields[fld.name] = resolveAlias(arena, symbols, declNode.moduleId, fld.typeId);
                }
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
                symbols.functions[fsym.key] = std::move(fsym);
            }
        }

        // Pass 2: Validate function bodies
        for (const DeclId did : program.declarations) {
            if (did >= arena.decls.size()) continue;
            const auto& declNode = arena.decls[did];
            if (const auto* fdecl = arena.getDecl<FnDecl>(did)) {
                if (!fdecl->isExtern) {
                    fileName = declNode.fileName;
                    moduleId = declNode.moduleId;
                    analyzeFn(*fdecl);
                }
            }
        }
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
