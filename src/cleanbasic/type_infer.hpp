#ifndef CLEANBASIC_TYPE_INFER_HPP
#define CLEANBASIC_TYPE_INFER_HPP

#include "src/cleanbasic/symbols.hpp"

namespace cb {

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

    if (const auto* lit = arena.getExpr<NumberLiteral>(exprId)) {
        if (lit->isFloat) {
            return symbols.typeFloat;
        }
        return symbols.typeInt;
    }

    if (arena.getExpr<StringLiteral>(exprId) != nullptr) {
        return symbols.typeStr;
    }

    if (arena.getExpr<BytesLiteral>(exprId) != nullptr) {
        return symbols.typeBytes;
    }

    if (arena.getExpr<BoolLiteral>(exprId) != nullptr) {
        return symbols.typeBool;
    }

    if (arena.getExpr<NullLiteral>(exprId) != nullptr) {
        return symbols.typeNull;
    }

    if (const auto* ident = arena.getExpr<Identifier>(exprId)) {
        const auto it = scope.find(ident->name);
        if (it != scope.end()) {
            return it->second.typeId;
        }
        return symbols.typeAuto;
    }

    if (const auto* llit = arena.getExpr<ListLiteral>(exprId)) {
        TypeId elemType = symbols.typeInt;
        if (!llit->elements.empty()) {
            elemType = inferExprType(arena, llit->elements[0], symbols, scope, currentModuleId);
        }
        const TypeId listTid = arena.findType("list", {elemType});
        if (listTid != INVALID_ID) {
            return listTid;
        }
        return symbols.typeAuto;
    }

    if (const auto* dlit = arena.getExpr<DictLiteral>(exprId)) {
        TypeId kType = symbols.typeStr;
        TypeId vType = symbols.typeInt;
        if (!dlit->entries.empty()) {
            kType = inferExprType(arena, dlit->entries[0].key, symbols, scope, currentModuleId);
            vType = inferExprType(arena, dlit->entries[0].value, symbols, scope, currentModuleId);
        }
        const TypeId dictTid = arena.findType("dict", {kType, vType});
        if (dictTid != INVALID_ID) {
            return dictTid;
        }
        return symbols.typeAuto;
    }

    if (const auto* faccess = arena.getExpr<FieldAccess>(exprId)) {
        if (const auto* tgtIdent = arena.getExpr<Identifier>(faccess->target)) {
            if (scope.find(tgtIdent->name) == scope.end()) {
                const auto enumOpt = resolveEnumSymbol(arena, symbols, currentModuleId, tgtIdent->name);
                if (enumOpt.has_value() && enumOpt->memberValues.count(faccess->fieldName) > 0) {
                    const TypeId tid = arena.findType(enumOpt->key, {});
                    if (tid != INVALID_ID) return tid;
                }
            }
        }
        if (const auto* innerFa = arena.getExpr<FieldAccess>(faccess->target)) {
            if (const auto* modIdent = arena.getExpr<Identifier>(innerFa->target)) {
                std::string targetModId;
                if (isModuleAliasInScope(symbols, currentModuleId, modIdent->name, scope, targetModId)) {
                    const std::string enumKey = symbolKey(targetModId, innerFa->fieldName);
                    const auto it = symbols.enums.find(enumKey);
                    if (it != symbols.enums.end() && it->second.memberValues.count(faccess->fieldName) > 0) {
                        const TypeId tid = arena.findType(it->second.key, {});
                        if (tid != INVALID_ID) return tid;
                    }
                }
            }
        }
        const TypeId targetTid = inferExprType(arena, faccess->target, symbols, scope, currentModuleId);
        const auto tsOpt = resolveTypeSymbol(arena, symbols, currentModuleId, targetTid);
        if (tsOpt.has_value()) {
            const auto fit = tsOpt->fields.find(faccess->fieldName);
            if (fit != tsOpt->fields.end()) {
                return fit->second;
            }
        }
        return symbols.typeAuto;
    }

    if (const auto* iaccess = arena.getExpr<IndexAccess>(exprId)) {
        const TypeId targetTid = inferExprType(arena, iaccess->target, symbols, scope, currentModuleId);
        if (targetTid < arena.types.size()) {
            const auto& tnode = arena.types[targetTid];
            if (tnode.name == "list" && !tnode.typeArgs.empty()) {
                return tnode.typeArgs[0];
            }
            if (tnode.name == "dict" && tnode.typeArgs.size() == 2) {
                return tnode.typeArgs[1];
            }
            if (tnode.name == "bytes") {
                return symbols.typeInt;
            }
            if (tnode.name == "str") {
                return symbols.typeStr;
            }
        }
        return symbols.typeAuto;
    }

    if (const auto* call = arena.getExpr<FuncCall>(exprId)) {
        const auto fnOpt = resolveFuncCall(arena, *call, symbols, currentModuleId, scope);
        if (fnOpt.has_value() && fnOpt->returnType != INVALID_ID) {
            return resolveAlias(arena, symbols, fnOpt->moduleId, fnOpt->returnType);
        }
        const auto structOpt = resolveStructConstructor(arena, *call, symbols, currentModuleId, scope);
        if (structOpt.has_value()) {
            const TypeId rawTid = arena.findType(structOpt->key, {});
            if (rawTid != INVALID_ID) {
                return rawTid;
            }
        }
        return symbols.typeVoid;
    }

    if (const auto* uop = arena.getExpr<UnaryOp>(exprId)) {
        if (uop->op == "not") {
            return symbols.typeBool;
        }
        return inferExprType(arena, uop->operand, symbols, scope, currentModuleId);
    }

    if (const auto* bop = arena.getExpr<BinaryOp>(exprId)) {
        if (bop->op == "and" || bop->op == "or" ||
            bop->op == "==" || bop->op == "!=" ||
            bop->op == "<" || bop->op == ">" ||
            bop->op == "<=" || bop->op == ">=") {
            return symbols.typeBool;
        }
        if (bop->op == "&") {
            return symbols.typeStr;
        }
        const TypeId leftTid = inferExprType(arena, bop->left, symbols, scope, currentModuleId);
        const TypeId rightTid = inferExprType(arena, bop->right, symbols, scope, currentModuleId);
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

} // namespace cb

#endif // CLEANBASIC_TYPE_INFER_HPP
