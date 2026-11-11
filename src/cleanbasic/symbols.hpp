
#ifndef CLEANBASIC_SYMBOLS_HPP
#define CLEANBASIC_SYMBOLS_HPP

#include "src/cleanbasic/ast.hpp"
#include "src/cleanbasic/diagnostics.hpp"
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
    std::string docComment;
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
    std::string docComment;
    bool isBuiltinReceiver = false;
    bool isFnValueCall = false;
    TypeId fnTypeId = INVALID_ID;
};

struct EnumSymbol {
    std::string name;
    std::string moduleId;
    std::string key;
    std::vector<EnumMember> members;
    std::unordered_map<std::string, int64_t> memberValues;
    bool isPub = false;
    std::string cppName;
    std::string fileName = "main.cb";
    uint32_t line = 1;
    uint32_t col = 1;
    uint32_t endLine = 1;
    std::string docComment;
};

struct UnionSymbol {
    std::string name;
    std::string moduleId;
    std::string key;
    std::vector<UnionVariant> variants;
    std::unordered_map<std::string, TypeId> variantTypes;
    std::unordered_map<std::string, size_t> variantIndices;
    bool isPub = false;
    std::string cppName;
    std::string fileName = "main.cb";
    uint32_t line = 1;
    uint32_t col = 1;
    uint32_t endLine = 1;
    std::string docComment;
};

struct ConstSymbol {
    std::string name;
    std::string moduleId;
    std::string key;
    TypeId typeId = INVALID_ID;
    bool isPub = false;
    std::string cppName;
    std::string fileName = "main.cb";
    uint32_t line = 1;
    uint32_t col = 1;
    uint32_t endLine = 1;
    std::string docComment;
};

template <typename SymT>
inline void populateSymbolMeta(
    SymT& sym,
    const DeclNode& declNode,
    std::string_view name,
    bool isPub,
    std::string cppName
) {
    sym.name = std::string(name);
    sym.moduleId = declNode.moduleId;
    sym.key = symbolKey(declNode.moduleId, name);
    sym.isPub = isPub;
    sym.cppName = std::move(cppName);
    sym.fileName = declNode.fileName;
    sym.line = declNode.nameLine;
    sym.col = declNode.nameCol;
    sym.endLine = declNode.endLine;
    sym.docComment = declNode.docComment;
}

inline std::vector<std::string_view> splitSourceLinesView(std::string_view sourceText) {
    std::vector<std::string_view> lines;
    size_t start = 0;
    const size_t n = sourceText.size();
    while (start <= n) {
        size_t nlPos = sourceText.find(static_cast<char>(10), start);
        if (nlPos == std::string_view::npos) {
            std::string_view tail = sourceText.substr(start);
            if (!tail.empty() && tail.back() == static_cast<char>(13)) {
                tail.remove_suffix(1);
            }
            lines.push_back(tail);
            break;
        }
        std::string_view seg = sourceText.substr(start, nlPos - start);
        if (!seg.empty() && seg.back() == static_cast<char>(13)) {
            seg.remove_suffix(1);
        }
        lines.push_back(seg);
        start = nlPos + 1;
    }
    return lines;
}

inline std::string extractPrecedingDocCommentFromLines(
    const std::vector<std::string_view>& lines,
    uint32_t declLine1Based
) {
    if (declLine1Based <= 1 || lines.empty()) {
        return "";
    }
    int64_t idx = static_cast<int64_t>(declLine1Based) - 2;
    if (idx >= static_cast<int64_t>(lines.size())) {
        idx = static_cast<int64_t>(lines.size()) - 1;
    }
    std::vector<std::string_view> revDocLines;
    while (idx >= 0) {
        std::string_view line = lines[static_cast<size_t>(idx)];
        size_t pos = 0;
        while (pos < line.size() && (line[pos] == ' ' || line[pos] == static_cast<char>(9))) {
            pos += 1;
        }
        std::string_view trimmed = line.substr(pos);
        if (trimmed.size() >= 2 && trimmed[0] == static_cast<char>(39) && trimmed[1] == static_cast<char>(39)) {
            std::string_view docText = trimmed.substr(2);
            if (!docText.empty() && docText[0] == ' ') {
                docText.remove_prefix(1);
            }
            revDocLines.push_back(docText);
            idx -= 1;
        } else {
            break;
        }
    }
    if (revDocLines.empty()) {
        return "";
    }
    std::string out;
    for (size_t i = revDocLines.size(); i > 0; --i) {
        if (i < revDocLines.size()) {
            out.push_back(static_cast<char>(10));
        }
        out.append(revDocLines[i - 1]);
    }
    return out;
}

inline void attachDocCommentsToDecls(
    AstArena& arena,
    const std::vector<DeclId>& declIds,
    std::string_view sourceText
) {
    if (declIds.empty() || sourceText.empty()) {
        return;
    }
    const auto lines = splitSourceLinesView(sourceText);
    for (const DeclId did : declIds) {
        if (did >= arena.decls.size()) {
            continue;
        }
        arena.decls[did].docComment = extractPrecedingDocCommentFromLines(lines, arena.decls[did].line);
    }
}

struct VarSymbol {
    std::string name;
    TypeId typeId = INVALID_ID;
    bool isByref = false;
    bool isConst = false;
    std::string fileName = "main.cb";
    uint32_t line = 1;
    uint32_t col = 1;
};

struct GlobalSymbolTable {
    std::unordered_map<std::string, TypeSymbol> types;
    std::unordered_map<std::string, EnumSymbol> enums;
    std::unordered_map<std::string, UnionSymbol> unions;
    std::unordered_map<std::string, ConstSymbol> constants;
    std::unordered_map<std::string, FnSymbol> functions;
    std::vector<std::string> headers;
    std::vector<std::string> linkLibs;
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> modules;
    std::unordered_map<std::string, std::unordered_map<std::string, VarSymbol>> fnScopes;
    std::unordered_set<std::string> explicitlyImportedModules;
    std::unordered_set<std::string> referencedModules;
    std::unordered_map<std::string, std::string> headerModules;
    std::unordered_map<std::string, std::vector<FnSymbol>> builtinReceiverCache;
    std::unordered_map<std::string, std::unordered_set<std::string>> callGraphEdges;
    std::unordered_set<std::string> rootReferencedFunctions;
    bool hasStdStr = false;
    TypeId typeInt = INVALID_ID;
    TypeId typeFloat = INVALID_ID;
    TypeId typeStr = INVALID_ID;
    TypeId typeBool = INVALID_ID;
    TypeId typeVoid = INVALID_ID;
    TypeId typeAuto = INVALID_ID;
    TypeId typeNull = INVALID_ID;
    TypeId typeErr = INVALID_ID;
    TypeId typeCptr = INVALID_ID;
};

struct AnalysisResult {
    bool ok = true;
    GlobalSymbolTable symbols;
    DiagnosticError error;
};

inline TypeNode buildFnTypeNode(const AstArena& arena, const FnSymbol& fn) {
    TypeNode node{};
    node.name = "fn";
    node.isFallible = fn.isFallible;
    if (fn.returnType != INVALID_ID && !arena.isVoid(fn.returnType)) {
        node.returnType = fn.returnType;
    }
    for (const auto& p : fn.params) {
        node.typeArgs.push_back(p.typeId);
        node.paramByref.push_back(p.isByref);
    }
    return node;
}

inline TypeId addFnTypeId(AstArena& arena, const FnSymbol& fn) {
    return arena.addType(buildFnTypeNode(arena, fn));
}

inline Param makeBuiltinParam(std::string_view name, TypeId tid, bool isByref = false) {
    Param p{};
    p.name = std::string(name);
    p.typeId = tid;
    p.isByref = isByref;
    return p;
}

inline FnSymbol makeBuiltinReceiverMethod(
    std::string_view name,
    TypeId recvTid,
    bool mutatesRecv,
    TypeId returnType,
    std::vector<Param> extraParams = {},
    bool isFallible = false
) {
    FnSymbol fs{};
    fs.name = std::string(name);
    fs.returnType = returnType;
    fs.isFallible = isFallible;
    fs.isBuiltinReceiver = true;
    fs.params.reserve(1 + extraParams.size());
    fs.params.push_back(makeBuiltinParam("self", recvTid, mutatesRecv));
    for (auto& ep : extraParams) {
        fs.params.push_back(std::move(ep));
    }
    return fs;
}

inline void registerBuiltins(AstArena& arena, GlobalSymbolTable& symbols) {
    symbols.typeInt = arena.findOrAddType("int");
    symbols.typeFloat = arena.findOrAddType("float");
    symbols.typeStr = arena.findOrAddType("str");
    symbols.typeBool = arena.findOrAddType("bool");
    symbols.typeVoid = arena.findOrAddType("void");
    symbols.typeAuto = arena.findOrAddType("auto");
    symbols.typeNull = arena.findOrAddType("null");
    symbols.typeErr = arena.findOrAddType("err");
    symbols.typeCptr = arena.findOrAddType("cptr");

    arena.findOrAddType("list", {symbols.typeInt});
    arena.findOrAddType("list", {symbols.typeFloat});
    arena.findOrAddType("list", {symbols.typeStr});
    arena.findOrAddType("list", {symbols.typeBool});
    arena.findOrAddType("list", {symbols.typeCptr});
    arena.findOrAddType("list", {symbols.typeErr});

    TypeSymbol errType{};
    errType.name = "err";
    errType.moduleId = "";
    errType.key = symbolKey("", "err");
    errType.isPub = true;
    errType.fields["msg"] = symbols.typeStr;
    errType.fields["code"] = symbols.typeInt;
    errType.fields["file"] = symbols.typeStr;
    errType.fields["line"] = symbols.typeInt;
    errType.fieldOrder.push_back(FieldDecl{1, 1, "msg", symbols.typeStr});
    errType.fieldOrder.push_back(FieldDecl{1, 1, "code", symbols.typeInt});
    errType.fieldOrder.push_back(FieldDecl{1, 1, "file", symbols.typeStr});
    errType.fieldOrder.push_back(FieldDecl{1, 1, "line", symbols.typeInt});
    errType.cppName = "CbError";
    symbols.types[errType.key] = std::move(errType);

    FnSymbol printFn{};
    printFn.name = "print";
    printFn.moduleId = "";
    printFn.key = symbolKey("", "print");
    printFn.isPub = true;
    printFn.isFallible = false;
    printFn.cppName = "cbPrint";
    symbols.functions[printFn.key] = std::move(printFn);

    symbols.builtinReceiverCache["int"] = {
        makeBuiltinReceiverMethod("toStr", symbols.typeInt, false, symbols.typeStr),
        makeBuiltinReceiverMethod("toInt", symbols.typeInt, false, symbols.typeInt),
        makeBuiltinReceiverMethod("toFloat", symbols.typeInt, false, symbols.typeFloat),
        makeBuiltinReceiverMethod("bitAnd", symbols.typeInt, false, symbols.typeInt, {makeBuiltinParam("y", symbols.typeInt)}),
        makeBuiltinReceiverMethod("bitOr", symbols.typeInt, false, symbols.typeInt, {makeBuiltinParam("y", symbols.typeInt)}),
        makeBuiltinReceiverMethod("bitXor", symbols.typeInt, false, symbols.typeInt, {makeBuiltinParam("y", symbols.typeInt)}),
        makeBuiltinReceiverMethod("bitNot", symbols.typeInt, false, symbols.typeInt),
        makeBuiltinReceiverMethod("shl", symbols.typeInt, false, symbols.typeInt, {makeBuiltinParam("bits", symbols.typeInt)}),
        makeBuiltinReceiverMethod("shr", symbols.typeInt, false, symbols.typeInt, {makeBuiltinParam("bits", symbols.typeInt)})
    };
    symbols.builtinReceiverCache["float"] = {
        makeBuiltinReceiverMethod("toStr", symbols.typeFloat, false, symbols.typeStr),
        makeBuiltinReceiverMethod("toInt", symbols.typeFloat, false, symbols.typeInt),
        makeBuiltinReceiverMethod("toFloat", symbols.typeFloat, false, symbols.typeFloat)
    };
    symbols.builtinReceiverCache["bool"] = {
        makeBuiltinReceiverMethod("toStr", symbols.typeBool, false, symbols.typeStr),
        makeBuiltinReceiverMethod("toInt", symbols.typeBool, false, symbols.typeInt)
    };
    symbols.builtinReceiverCache["err"] = {
        makeBuiltinReceiverMethod("toStr", symbols.typeErr, false, symbols.typeStr)
    };
    symbols.builtinReceiverCache["str"] = {
        makeBuiltinReceiverMethod("len", symbols.typeStr, false, symbols.typeInt),
        makeBuiltinReceiverMethod("byteAt", symbols.typeStr, false, symbols.typeInt, {makeBuiltinParam("idx", symbols.typeInt)}),
        makeBuiltinReceiverMethod("toStr", symbols.typeStr, false, symbols.typeStr)
    };
}

inline bool isBuiltinTypeName(std::string_view name) {
    return name == "int" || name == "float" || name == "str" || name == "bool" ||
           name == "void" || name == "auto" || name == "null" ||
           name == "err" || name == "list" || name == "dict" || name == "cptr" ||
           name == "fn";
}

inline bool isBuiltinSymbolName(std::string_view name) {
    return name == "err" || name == "print" || name == "error" || name == "assert";
}

inline bool isAssertBuiltinCall(const AstArena& arena, const FuncCall& call) {
    if (const auto* ident = arena.getExpr<Identifier>(call.callee)) {
        return ident->name == "assert";
    }
    return false;
}

inline bool isErrorBuiltinCall(const AstArena& arena, const FuncCall& call) {
    if (const auto* ident = arena.getExpr<Identifier>(call.callee)) {
        return ident->name == "error";
    }
    return false;
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
    if (symbols.types.count(key) > 0 || symbols.enums.count(key) > 0 || symbols.unions.count(key) > 0 || symbols.functions.count(key) > 0 || symbols.constants.count(key) > 0) {
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

inline TypeNode resolveAliasNode(
    const AstArena& arena,
    const GlobalSymbolTable& symbols,
    std::string_view currentModuleId,
    TypeId tid
) {
    if (tid >= arena.types.size()) {
        return TypeNode{};
    }
    TypeNode curr = arena.types[tid];
    if (curr.typeArgs.empty() && curr.returnType == INVALID_ID && isBuiltinTypeName(curr.name)) {
        return curr;
    }

    if (!curr.name.empty() && !isBuiltinTypeName(curr.name)) {
        const std::string typeKey = resolveSymbolKey(symbols, currentModuleId, curr.name);
        const auto tit = symbols.types.find(typeKey);
        if (tit != symbols.types.end()) {
            curr.name = tit->second.key;
        } else {
            const auto eit = symbols.enums.find(typeKey);
            if (eit != symbols.enums.end()) {
                curr.name = eit->second.key;
            } else {
                const auto uit = symbols.unions.find(typeKey);
                if (uit != symbols.unions.end()) {
                    curr.name = uit->second.key;
                }
            }
        }
    }

    for (TypeId& argTid : curr.typeArgs) {
        argTid = resolveAlias(arena, symbols, currentModuleId, argTid);
    }
    if (curr.returnType != INVALID_ID) {
        curr.returnType = resolveAlias(arena, symbols, currentModuleId, curr.returnType);
        if (arena.isVoid(curr.returnType)) {
            curr.returnType = INVALID_ID;
        }
    }

    return curr;
}

// Read-only alias resolution used during Pass 2, Emission, and LSP queries where AstArena is const.
// Looks up the already-interned canonical TypeId without mutating arena.types.
inline TypeId resolveAlias(
    const AstArena& arena,
    const GlobalSymbolTable& symbols,
    std::string_view currentModuleId,
    TypeId tid
) {
    if (tid >= arena.types.size()) {
        return tid;
    }
    const TypeNode& initialNode = arena.types[tid];
    if (initialNode.typeArgs.empty() && initialNode.returnType == INVALID_ID && isBuiltinTypeName(initialNode.name)) {
        return tid;
    }
    const TypeNode resolvedNode = resolveAliasNode(arena, symbols, currentModuleId, tid);
    const TypeId found = arena.findTypeNode(resolvedNode);
    if (found != INVALID_ID) {
        return found;
    }
    return tid;
}

// Mutating type canonicalization used during Pass 1b declaration registration.
// Recursively unwraps aliases in type arguments/return types and interns the canonical TypeNode
// (plus list[T]) into arena.types so subsequent read-only resolveAlias() lookups always succeed.
inline TypeId canonicalizeType(
    AstArena& arena,
    const GlobalSymbolTable& symbols,
    std::string_view currentModuleId,
    TypeId tid
) {
    if (tid >= arena.types.size()) {
        return tid;
    }
    TypeNode resolvedNode = resolveAliasNode(arena, symbols, currentModuleId, tid);
    for (TypeId& argTid : resolvedNode.typeArgs) {
        argTid = canonicalizeType(arena, symbols, currentModuleId, argTid);
    }
    if (resolvedNode.returnType != INVALID_ID) {
        resolvedNode.returnType = canonicalizeType(arena, symbols, currentModuleId, resolvedNode.returnType);
        if (arena.isVoid(resolvedNode.returnType)) {
            resolvedNode.returnType = INVALID_ID;
        }
    }
    const TypeId canonicalTid = arena.addType(std::move(resolvedNode));
    arena.findOrAddType("list", {canonicalTid});
    return canonicalTid;
}

template <typename TableMap>
inline bool isTypeInTable(
    const GlobalSymbolTable& symbols,
    const TableMap& table,
    TypeId tid,
    const AstArena& arena,
    std::string_view currentModuleId
) {
    if (tid >= arena.types.size()) {
        return false;
    }
    const TypeNode resolved = resolveAliasNode(arena, symbols, currentModuleId, tid);
    return table.count(resolved.name) > 0;
}

inline bool isEnumType(
    const GlobalSymbolTable& symbols,
    TypeId tid,
    const AstArena& arena,
    std::string_view currentModuleId
) {
    return isTypeInTable(symbols, symbols.enums, tid, arena, currentModuleId);
}

inline bool isUnionType(
    const GlobalSymbolTable& symbols,
    TypeId tid,
    const AstArena& arena,
    std::string_view currentModuleId
) {
    return isTypeInTable(symbols, symbols.unions, tid, arena, currentModuleId);
}

inline std::optional<TypeSymbol> resolveTypeSymbolByName(
    const AstArena& arena,
    const GlobalSymbolTable& symbols,
    std::string_view currentModuleId,
    std::string_view rawName,
    bool requireFirstHopPub = false
) {
    (void)arena;
    const std::string firstKey = resolveSymbolKey(symbols, currentModuleId, rawName);
    if (symbols.functions.find(firstKey) != symbols.functions.end()) {
        return std::nullopt;
    }
    const auto tit = symbols.types.find(firstKey);
    if (tit != symbols.types.end()) {
        if (requireFirstHopPub && !tit->second.isPub && tit->second.moduleId != currentModuleId) {
            return std::nullopt;
        }
        return tit->second;
    }
    return std::nullopt;
}

template <typename SymbolT>
inline std::optional<SymbolT> resolveTypeByIdInTable(
    const AstArena& arena,
    const GlobalSymbolTable& symbols,
    const std::unordered_map<std::string, SymbolT>& table,
    std::string_view currentModuleId,
    TypeId tid
) {
    if (tid == INVALID_ID || tid >= arena.types.size()) {
        return std::nullopt;
    }
    const TypeNode resolved = resolveAliasNode(arena, symbols, currentModuleId, tid);
    const auto directIt = table.find(resolved.name);
    if (directIt != table.end()) {
        return directIt->second;
    }
    const std::string key = resolveSymbolKey(symbols, currentModuleId, resolved.name);
    const auto resolvedIt = table.find(key);
    if (resolvedIt != table.end()) {
        return resolvedIt->second;
    }
    return std::nullopt;
}

inline std::optional<TypeSymbol> resolveTypeSymbol(
    const AstArena& arena,
    const GlobalSymbolTable& symbols,
    std::string_view currentModuleId,
    TypeId tid
) {
    return resolveTypeByIdInTable(arena, symbols, symbols.types, currentModuleId, tid);
}

inline bool isModuleEmitted(
    const GlobalSymbolTable& symbols,
    std::string_view modId
) {
    if (modId != "std/str") {
        return true;
    }
    if (symbols.explicitlyImportedModules.count("std/str") > 0) {
        return true;
    }
    if (symbols.referencedModules.count("std/str") > 0) {
        return true;
    }
    return false;
}

inline std::string resolveUfcsDefiningModule(
    const GlobalSymbolTable& symbols,
    std::string_view currentModuleId,
    std::string_view recvTypeKey
) {
    if (recvTypeKey == "str") {
        if (currentModuleId == "std/str" || symbols.hasStdStr) {
            return "std/str";
        }
        return "";
    }
    const auto [recvModId, recvBase] = splitSymbolKey(recvTypeKey);
    return recvModId;
}

inline bool ufcsParamMatchesReceiver(
    const AstArena& arena,
    const GlobalSymbolTable& symbols,
    const FnSymbol& fn,
    std::string_view recvTypeKey
) {
    if (fn.params.empty()) {
        return false;
    }
    if (recvTypeKey.empty() || recvTypeKey == "auto") {
        return true;
    }
    const TypeId p0Tid = fn.params[0].typeId;
    if (p0Tid == INVALID_ID || p0Tid >= arena.types.size()) {
        return false;
    }
    const TypeNode p0Resolved = resolveAliasNode(arena, symbols, fn.moduleId, p0Tid);
    return p0Resolved.name == recvTypeKey;
}

inline std::vector<FnSymbol> getBuiltinReceiverMethods(
    const AstArena& arena,
    const GlobalSymbolTable& symbols,
    std::string_view currentModuleId,
    TypeId recvTid
) {
    if (recvTid >= arena.types.size()) {
        return {};
    }
    const TypeNode resolvedRecvNode = resolveAliasNode(arena, symbols, currentModuleId, recvTid);
    const std::string& recvTypeKey = resolvedRecvNode.name;

    const auto cachedIt = symbols.builtinReceiverCache.find(recvTypeKey);
    if (cachedIt != symbols.builtinReceiverCache.end()) {
        std::vector<FnSymbol> methods = cachedIt->second;
        for (auto& m : methods) {
            if (!m.params.empty() && m.params[0].typeId != recvTid) {
                m.params[0].typeId = recvTid;
            }
        }
        return methods;
    }

    std::vector<FnSymbol> methods;
    if (recvTypeKey == "list") {
        TypeId elemType = symbols.typeInt;
        if (!resolvedRecvNode.typeArgs.empty()) {
            elemType = resolvedRecvNode.typeArgs[0];
        }
        methods.push_back(makeBuiltinReceiverMethod("len", recvTid, false, symbols.typeInt));
        methods.push_back(makeBuiltinReceiverMethod("push", recvTid, true, symbols.typeVoid, {makeBuiltinParam("item", elemType)}));
        methods.push_back(makeBuiltinReceiverMethod("pop", recvTid, true, elemType));
        methods.push_back(makeBuiltinReceiverMethod("clear", recvTid, true, symbols.typeVoid));
        methods.push_back(makeBuiltinReceiverMethod("slice", recvTid, false, recvTid, {makeBuiltinParam("start", symbols.typeInt), makeBuiltinParam("stop", symbols.typeInt)}));
        methods.push_back(makeBuiltinReceiverMethod("sliceFrom", recvTid, false, recvTid, {makeBuiltinParam("start", symbols.typeInt)}));
        methods.push_back(makeBuiltinReceiverMethod("contains", recvTid, false, symbols.typeBool, {makeBuiltinParam("val", elemType)}));
        methods.push_back(makeBuiltinReceiverMethod("sort", recvTid, true, symbols.typeVoid));
        methods.push_back(makeBuiltinReceiverMethod("removeAt", recvTid, true, elemType, {makeBuiltinParam("idx", symbols.typeInt)}));
    } else if (recvTypeKey == "dict") {
        TypeId keyType = symbols.typeStr;
        TypeId valType = symbols.typeInt;
        if (!resolvedRecvNode.typeArgs.empty()) {
            keyType = resolvedRecvNode.typeArgs[0];
        }
        if (resolvedRecvNode.typeArgs.size() >= 2) {
            valType = resolvedRecvNode.typeArgs[1];
        }
        TypeId keyListTid = arena.findType("list", {keyType});
        if (keyListTid == INVALID_ID) {
            keyListTid = symbols.typeAuto;
        }
        methods.push_back(makeBuiltinReceiverMethod("len", recvTid, false, symbols.typeInt));
        methods.push_back(makeBuiltinReceiverMethod("get", recvTid, false, valType, {makeBuiltinParam("key", keyType), makeBuiltinParam("defaultValue", valType)}));
        methods.push_back(makeBuiltinReceiverMethod("has", recvTid, false, symbols.typeBool, {makeBuiltinParam("key", keyType)}));
        methods.push_back(makeBuiltinReceiverMethod("remove", recvTid, true, symbols.typeBool, {makeBuiltinParam("key", keyType)}));
        methods.push_back(makeBuiltinReceiverMethod("clear", recvTid, true, symbols.typeVoid));
        methods.push_back(makeBuiltinReceiverMethod("keys", recvTid, false, keyListTid));
    } else if (isEnumType(symbols, recvTid, arena, currentModuleId)) {
        methods.push_back(makeBuiltinReceiverMethod("toStr", recvTid, false, symbols.typeStr));
        methods.push_back(makeBuiltinReceiverMethod("toInt", recvTid, false, symbols.typeInt));
    } else if (isUnionType(symbols, recvTid, arena, currentModuleId)) {
        methods.push_back(makeBuiltinReceiverMethod("kind", recvTid, false, symbols.typeStr));
    }
    return methods;
}

// Resolves a UFCS method call 'recv.method(args...)' across 4 priority tiers:
// 0. Built-in receiver methods (list/dict/str/bytes/int/union intrinsics).
// 1. Free functions in the caller's current module (public or private).
// 2. Public free functions in the receiver type's defining module (or 'std/str' for 'str').
// 3. Public free functions in any module directly imported by currentModuleId.
inline std::optional<FnSymbol> resolveUfcsFn(
    const AstArena& arena,
    const GlobalSymbolTable& symbols,
    std::string_view currentModuleId,
    TypeId recvTid,
    std::string_view methodName
) {
    const auto builtinMethods = getBuiltinReceiverMethods(arena, symbols, currentModuleId, recvTid);
    for (const auto& bm : builtinMethods) {
        if (bm.name == methodName) {
            return bm;
        }
    }

    TypeNode resolvedRecvNode{};
    if (recvTid < arena.types.size()) {
        resolvedRecvNode = resolveAliasNode(arena, symbols, currentModuleId, recvTid);
    }
    const std::string& recvTypeKey = resolvedRecvNode.name;

    // UFCS Step 1: Current module
    const std::string localKey = symbolKey(currentModuleId, methodName);
    const auto localIt = symbols.functions.find(localKey);
    if (localIt != symbols.functions.end() && ufcsParamMatchesReceiver(arena, symbols, localIt->second, recvTypeKey)) {
        return localIt->second;
    }

    // UFCS Step 2: Receiver's defining module (including 'std/str' for 'str' when imported)
    const std::string recvModId = resolveUfcsDefiningModule(symbols, currentModuleId, recvTypeKey);
    if (!recvModId.empty()) {
        const std::string recvFnKey = symbolKey(recvModId, methodName);
        const auto recvFnIt = symbols.functions.find(recvFnKey);
        if (recvFnIt != symbols.functions.end()) {
            const bool requirePub = (recvModId != currentModuleId);
            if ((!requirePub || recvFnIt->second.isPub) && ufcsParamMatchesReceiver(arena, symbols, recvFnIt->second, recvTypeKey)) {
                return recvFnIt->second;
            }
        }
    }

    // UFCS Step 3: Directly imported modules in currentModuleId
    const auto modMapIt = symbols.modules.find(std::string(currentModuleId));
    if (modMapIt != symbols.modules.end()) {
        for (const auto& aliasEntry : modMapIt->second) {
            const std::string& importedModId = aliasEntry.second;
            if (importedModId.empty() || importedModId == currentModuleId || importedModId == recvModId) {
                continue;
            }
            const std::string impFnKey = symbolKey(importedModId, methodName);
            const auto impFnIt = symbols.functions.find(impFnKey);
            if (impFnIt != symbols.functions.end() && impFnIt->second.isPub && ufcsParamMatchesReceiver(arena, symbols, impFnIt->second, recvTypeKey)) {
                return impFnIt->second;
            }
        }
    }

    return std::nullopt;
}

template <typename SymbolT>
inline std::optional<SymbolT> resolveSymbolInTable(
    const AstArena& arena,
    const GlobalSymbolTable& symbols,
    const std::unordered_map<std::string, SymbolT>& table,
    std::string_view currentModuleId,
    std::string_view typeName
) {
    (void)arena;
    const std::string localKey = symbolKey(currentModuleId, typeName);
    const auto it = table.find(localKey);
    if (it != table.end()) {
        return it->second;
    }
    const std::string rootKey = symbolKey("", typeName);
    const auto it2 = table.find(rootKey);
    if (it2 != table.end()) {
        return it2->second;
    }
    const std::string normKey = resolveSymbolKey(symbols, currentModuleId, typeName);
    const auto it3 = table.find(normKey);
    if (it3 != table.end()) {
        return it3->second;
    }
    return std::nullopt;
}

inline std::optional<EnumSymbol> resolveEnumSymbol(
    const AstArena& arena,
    const GlobalSymbolTable& symbols,
    std::string_view currentModuleId,
    std::string_view typeName
) {
    return resolveSymbolInTable(arena, symbols, symbols.enums, currentModuleId, typeName);
}

inline std::optional<UnionSymbol> resolveUnionSymbolByName(
    const AstArena& arena,
    const GlobalSymbolTable& symbols,
    std::string_view currentModuleId,
    std::string_view typeName
) {
    return resolveSymbolInTable(arena, symbols, symbols.unions, currentModuleId, typeName);
}

inline std::optional<UnionSymbol> resolveUnionSymbol(
    const AstArena& arena,
    const GlobalSymbolTable& symbols,
    std::string_view currentModuleId,
    TypeId tid
) {
    return resolveTypeByIdInTable(arena, symbols, symbols.unions, currentModuleId, tid);
}

template <typename SymbolT>
inline std::optional<SymbolT> resolveScopedContainerAccess(
    const AstArena& arena,
    const GlobalSymbolTable& symbols,
    const std::unordered_map<std::string, SymbolT>& table,
    std::string_view currentModuleId,
    const std::unordered_map<std::string, VarSymbol>& scope,
    const FieldAccess& faccess
) {
    if (const auto* tgtIdent = arena.getExpr<Identifier>(faccess.target)) {
        if (scope.find(tgtIdent->name) == scope.end()) {
            const auto symOpt = resolveSymbolInTable(arena, symbols, table, currentModuleId, tgtIdent->name);
            if (symOpt.has_value()) {
                return symOpt;
            }
        }
    }
    if (const auto* innerFa = arena.getExpr<FieldAccess>(faccess.target)) {
        if (const auto* modIdent = arena.getExpr<Identifier>(innerFa->target)) {
            std::string targetModId;
            if (isModuleAliasInScope(symbols, currentModuleId, modIdent->name, scope, targetModId)) {
                return resolveSymbolInTable(arena, symbols, table, targetModId, innerFa->fieldName);
            }
        }
    }
    return std::nullopt;
}

inline std::optional<UnionSymbol> resolveUnionVariantAccess(
    const AstArena& arena,
    const GlobalSymbolTable& symbols,
    std::string_view currentModuleId,
    const std::unordered_map<std::string, VarSymbol>& scope,
    const FieldAccess& faccess
) {
    return resolveScopedContainerAccess(arena, symbols, symbols.unions, currentModuleId, scope, faccess);
}

struct ResolvedUnionVariantCall {
    UnionSymbol unionSym;
    std::string variantName;
    size_t variantIndex = 0;
    TypeId payloadType = INVALID_ID;
};

inline std::optional<ResolvedUnionVariantCall> resolveUnionVariantConstructor(
    const AstArena& arena,
    const FuncCall& call,
    const GlobalSymbolTable& symbols,
    std::string_view currentModuleId,
    const std::unordered_map<std::string, VarSymbol>& scope = {}
) {
    const auto* access = arena.getExpr<FieldAccess>(call.callee);
    if (access == nullptr) {
        return std::nullopt;
    }
    const auto uOpt = resolveUnionVariantAccess(arena, symbols, currentModuleId, scope, *access);
    if (!uOpt.has_value()) {
        return std::nullopt;
    }
    const auto idxIt = uOpt->variantIndices.find(access->fieldName);
    const auto typeIt = uOpt->variantTypes.find(access->fieldName);
    if (idxIt == uOpt->variantIndices.end() || typeIt == uOpt->variantTypes.end()) {
        return std::nullopt;
    }
    return ResolvedUnionVariantCall{*uOpt, access->fieldName, idxIt->second, typeIt->second};
}

inline std::optional<EnumSymbol> resolveEnumMemberAccess(
    const AstArena& arena,
    const GlobalSymbolTable& symbols,
    std::string_view currentModuleId,
    const std::unordered_map<std::string, VarSymbol>& scope,
    const FieldAccess& faccess
) {
    return resolveScopedContainerAccess(arena, symbols, symbols.enums, currentModuleId, scope, faccess);
}

inline std::optional<ConstSymbol> resolveConstSymbol(
    const GlobalSymbolTable& symbols,
    std::string_view currentModuleId,
    const std::unordered_map<std::string, VarSymbol>& scope,
    std::string_view identName
) {
    if (scope.find(std::string(identName)) != scope.end()) {
        return std::nullopt;
    }
    const std::string key = symbolKey(currentModuleId, identName);
    const auto it = symbols.constants.find(key);
    if (it != symbols.constants.end()) {
        return it->second;
    }
    return std::nullopt;
}

inline std::optional<ConstSymbol> resolveModuleConstAccess(
    const AstArena& arena,
    const GlobalSymbolTable& symbols,
    std::string_view currentModuleId,
    const std::unordered_map<std::string, VarSymbol>& scope,
    const FieldAccess& faccess
) {
    if (const auto* tgtIdent = arena.getExpr<Identifier>(faccess.target)) {
        std::string targetModId;
        if (isModuleAliasInScope(symbols, currentModuleId, tgtIdent->name, scope, targetModId)) {
            const std::string constKey = symbolKey(targetModId, faccess.fieldName);
            const auto it = symbols.constants.find(constKey);
            if (it != symbols.constants.end()) {
                return it->second;
            }
        }
    }
    return std::nullopt;
}

inline TypeId inferExprType(
    const AstArena& arena,
    ExprId exprId,
    const GlobalSymbolTable& symbols,
    const std::unordered_map<std::string, VarSymbol>& scope,
    std::string_view currentModuleId
);

inline std::optional<FnSymbol> resolveBareFnRef(
    const AstArena& arena,
    ExprId exprId,
    const GlobalSymbolTable& symbols,
    const std::unordered_map<std::string, VarSymbol>& scope,
    std::string_view currentModuleId
) {
    if (exprId >= arena.exprs.size()) {
        return std::nullopt;
    }
    if (const auto* ident = arena.getExpr<Identifier>(exprId)) {
        if (scope.find(ident->name) != scope.end()) {
            return std::nullopt;
        }
        if (resolveConstSymbol(symbols, currentModuleId, scope, ident->name).has_value()) {
            return std::nullopt;
        }
        if (isBuiltinSymbolName(ident->name)) {
            return std::nullopt;
        }
        const std::string key = symbolKey(currentModuleId, ident->name);
        const auto it = symbols.functions.find(key);
        if (it != symbols.functions.end()) {
            return it->second;
        }
        return std::nullopt;
    }
    if (const auto* fa = arena.getExpr<FieldAccess>(exprId)) {
        if (const auto* modIdent = arena.getExpr<Identifier>(fa->target)) {
            std::string targetModId;
            if (isModuleAliasInScope(symbols, currentModuleId, modIdent->name, scope, targetModId)) {
                const std::string key = symbolKey(targetModId, fa->fieldName);
                const auto it = symbols.functions.find(key);
                if (it != symbols.functions.end()) {
                    return it->second;
                }
            }
        }
    }
    return std::nullopt;
}

inline FnSymbol makeFnSymbolFromTypeNode(
    std::string_view name,
    std::string_view moduleId,
    const TypeNode& fnNode,
    TypeId voidTid
) {
    FnSymbol fs{};
    fs.name = std::string(name);
    fs.moduleId = std::string(moduleId);
    fs.isFallible = fnNode.isFallible;
    fs.isFnValueCall = true;
    if (fnNode.returnType != INVALID_ID) {
        fs.returnType = fnNode.returnType;
    } else {
        fs.returnType = voidTid;
    }
    fs.params.reserve(fnNode.typeArgs.size());
    for (size_t i = 0; i < fnNode.typeArgs.size(); ++i) {
        Param p{};
        p.name = "arg" + std::to_string(i + 1);
        p.typeId = fnNode.typeArgs[i];
        if (i < fnNode.paramByref.size()) {
            p.isByref = fnNode.paramByref[i];
        }
        fs.params.push_back(std::move(p));
    }
    return fs;
}

// Disambiguates whether 'target.name(args...)' is a UFCS receiver call vs:
// - a module-qualified call ('modAlias.fn(...)'),
// - a static built-in constructor ('bytes.fromHex(...)'),
// - a tagged union variant constructor ('Shape.Circle(5.0)'), or
// - a call through a struct field of function type ('holder.callback(...)').
inline bool isUfcsCallExpr(
    const AstArena& arena,
    const FuncCall& call,
    const GlobalSymbolTable& symbols,
    std::string_view currentModuleId,
    const std::unordered_map<std::string, VarSymbol>& scope,
    ExprId* outRecvExprId = nullptr
) {
    const auto* calleeFa = arena.getExpr<FieldAccess>(call.callee);
    if (calleeFa == nullptr) {
        return false;
    }
    if (const auto* tgtIdent = arena.getExpr<Identifier>(calleeFa->target)) {
        std::string targetModId;
        if (isModuleAliasInScope(symbols, currentModuleId, tgtIdent->name, scope, targetModId)) {
            return false;
        }
    }
    if (resolveUnionVariantAccess(arena, symbols, currentModuleId, scope, *calleeFa).has_value()) {
        return false;
    }
    const TypeId recvTid = inferExprType(arena, calleeFa->target, symbols, scope, currentModuleId);
    const auto tsOpt = resolveTypeSymbol(arena, symbols, currentModuleId, recvTid);
    if (tsOpt.has_value()) {
        const auto fit = tsOpt->fields.find(calleeFa->fieldName);
        if (fit != tsOpt->fields.end()) {
            const TypeNode fieldResolved = resolveAliasNode(arena, symbols, tsOpt->moduleId, fit->second);
            if (fieldResolved.name == "fn") {
                return false;
            }
        }
    }
    if (outRecvExprId != nullptr) {
        *outRecvExprId = calleeFa->target;
    }
    return true;
}

inline std::optional<TypeSymbol> resolveStructConstructor(
    const AstArena& arena,
    const FuncCall& call,
    const GlobalSymbolTable& symbols,
    std::string_view currentModuleId,
    const std::unordered_map<std::string, VarSymbol>& scope = {}
) {
    if (const auto* ident = arena.getExpr<Identifier>(call.callee)) {
        const bool requirePub = (ident->name.find('.') != std::string::npos);
        return resolveTypeSymbolByName(arena, symbols, currentModuleId, ident->name, requirePub);
    }

    if (const auto* access = arena.getExpr<FieldAccess>(call.callee)) {
        if (const auto* modIdent = arena.getExpr<Identifier>(access->target)) {
            std::string targetModId;
            if (!isModuleAliasInScope(symbols, currentModuleId, modIdent->name, scope, targetModId)) {
                return std::nullopt;
            }
            return resolveTypeSymbolByName(arena, symbols, targetModId, access->fieldName, false);
        }
        return std::nullopt;
    }

    return std::nullopt;
}

inline std::optional<FnSymbol> resolveFuncCall(
    const AstArena& arena,
    const FuncCall& call,
    const GlobalSymbolTable& symbols,
    std::string_view currentModuleId,
    const std::unordered_map<std::string, VarSymbol>& scope = {}
);

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
    const TypeId cachedTid = arena.exprs[exprId].resolvedType;
    if (cachedTid != INVALID_ID && cachedTid != symbols.typeAuto) {
        return cachedTid;
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
        const auto cOpt = resolveConstSymbol(symbols, currentModuleId, scope, ident->name);
        if (cOpt.has_value() && cOpt->typeId != INVALID_ID) {
            return cOpt->typeId;
        }
        const auto bareFnOpt = resolveBareFnRef(arena, exprId, symbols, scope, currentModuleId);
        if (bareFnOpt.has_value() && bareFnOpt->fnTypeId != INVALID_ID) {
            return bareFnOpt->fnTypeId;
        }
        return symbols.typeAuto;
    }

    if (const auto* llit = arena.getExpr<ListLiteral>(exprId)) {
        TypeId elemType = symbols.typeInt;
        if (!llit->elements.empty()) {
            elemType = resolveAlias(arena, symbols, currentModuleId, inferExprType(arena, llit->elements[0], symbols, scope, currentModuleId));
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
            kType = resolveAlias(arena, symbols, currentModuleId, inferExprType(arena, dlit->entries[0].key, symbols, scope, currentModuleId));
            vType = resolveAlias(arena, symbols, currentModuleId, inferExprType(arena, dlit->entries[0].value, symbols, scope, currentModuleId));
        }
        const TypeId dictTid = arena.findType("dict", {kType, vType});
        if (dictTid != INVALID_ID) {
            return dictTid;
        }
        return symbols.typeAuto;
    }

    if (const auto* faccess = arena.getExpr<FieldAccess>(exprId)) {
        const auto enumOpt = resolveEnumMemberAccess(arena, symbols, currentModuleId, scope, *faccess);
        if (enumOpt.has_value() && enumOpt->memberValues.count(faccess->fieldName) > 0) {
            const TypeId tid = arena.findType(enumOpt->key, {});
            if (tid != INVALID_ID) {
                return tid;
            }
        }
        const auto cOpt = resolveModuleConstAccess(arena, symbols, currentModuleId, scope, *faccess);
        if (cOpt.has_value() && cOpt->typeId != INVALID_ID) {
            return cOpt->typeId;
        }
        const auto bareFnOpt = resolveBareFnRef(arena, exprId, symbols, scope, currentModuleId);
        if (bareFnOpt.has_value() && bareFnOpt->fnTypeId != INVALID_ID) {
            return bareFnOpt->fnTypeId;
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
            const TypeNode tnode = resolveAliasNode(arena, symbols, currentModuleId, targetTid);
            if (tnode.name == "list" && !tnode.typeArgs.empty()) {
                return tnode.typeArgs[0];
            }
            if (tnode.name == "dict" && tnode.typeArgs.size() == 2) {
                return tnode.typeArgs[1];
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
        const auto unionVarOpt = resolveUnionVariantConstructor(arena, *call, symbols, currentModuleId, scope);
        if (unionVarOpt.has_value()) {
            const TypeId rawTid = arena.findType(unionVarOpt->unionSym.key, {});
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

inline TypeId inferAndCacheExprType(
    AstArena& arena,
    ExprId exprId,
    const GlobalSymbolTable& symbols,
    const std::unordered_map<std::string, VarSymbol>& scope,
    std::string_view currentModuleId
) {
    if (exprId >= arena.exprs.size()) {
        return symbols.typeAuto;
    }
    if (const auto* llit = arena.getExpr<ListLiteral>(exprId)) {
        for (const ExprId elId : llit->elements) {
            inferAndCacheExprType(arena, elId, symbols, scope, currentModuleId);
        }
        if (!llit->elements.empty()) {
            const TypeId elTid = resolveAlias(arena, symbols, currentModuleId, inferExprType(arena, llit->elements[0], symbols, scope, currentModuleId));
            arena.findOrAddType("list", {elTid});
        }
    } else if (const auto* dlit = arena.getExpr<DictLiteral>(exprId)) {
        for (const auto& ent : dlit->entries) {
            inferAndCacheExprType(arena, ent.key, symbols, scope, currentModuleId);
            inferAndCacheExprType(arena, ent.value, symbols, scope, currentModuleId);
        }
        if (!dlit->entries.empty()) {
            const TypeId kTid = resolveAlias(arena, symbols, currentModuleId, inferExprType(arena, dlit->entries[0].key, symbols, scope, currentModuleId));
            const TypeId vTid = resolveAlias(arena, symbols, currentModuleId, inferExprType(arena, dlit->entries[0].value, symbols, scope, currentModuleId));
            arena.findOrAddType("dict", {kTid, vTid});
            arena.findOrAddType("list", {kTid});
        }
    } else if (const auto* call = arena.getExpr<FuncCall>(exprId)) {
        inferAndCacheExprType(arena, call->callee, symbols, scope, currentModuleId);
        for (const ExprId argId : call->args) {
            inferAndCacheExprType(arena, argId, symbols, scope, currentModuleId);
        }
        for (const auto& na : call->namedArgs) {
            inferAndCacheExprType(arena, na.value, symbols, scope, currentModuleId);
        }
    } else if (const auto* bop = arena.getExpr<BinaryOp>(exprId)) {
        inferAndCacheExprType(arena, bop->left, symbols, scope, currentModuleId);
        inferAndCacheExprType(arena, bop->right, symbols, scope, currentModuleId);
    } else if (const auto* uop = arena.getExpr<UnaryOp>(exprId)) {
        inferAndCacheExprType(arena, uop->operand, symbols, scope, currentModuleId);
    } else if (const auto* fa = arena.getExpr<FieldAccess>(exprId)) {
        inferAndCacheExprType(arena, fa->target, symbols, scope, currentModuleId);
    } else if (const auto* ia = arena.getExpr<IndexAccess>(exprId)) {
        inferAndCacheExprType(arena, ia->target, symbols, scope, currentModuleId);
        inferAndCacheExprType(arena, ia->index, symbols, scope, currentModuleId);
    }

    const TypeId tid = inferExprType(arena, exprId, symbols, scope, currentModuleId);
    if (tid != INVALID_ID && tid != symbols.typeAuto) {
        arena.exprs[exprId].resolvedType = tid;
    }
    return tid;
}

inline std::optional<FnSymbol> resolveFuncCall(
    const AstArena& arena,
    const FuncCall& call,
    const GlobalSymbolTable& symbols,
    std::string_view currentModuleId,
    const std::unordered_map<std::string, VarSymbol>& scope
) {
    if (const auto* ident = arena.getExpr<Identifier>(call.callee)) {
        const auto varIt = scope.find(ident->name);
        if (varIt != scope.end()) {
            const TypeNode resolved = resolveAliasNode(arena, symbols, currentModuleId, varIt->second.typeId);
            if (resolved.name == "fn") {
                return makeFnSymbolFromTypeNode(ident->name, currentModuleId, resolved, symbols.typeVoid);
            }
        }
        const auto cOpt = resolveConstSymbol(symbols, currentModuleId, scope, ident->name);
        if (cOpt.has_value() && cOpt->typeId != INVALID_ID) {
            const TypeNode resolved = resolveAliasNode(arena, symbols, currentModuleId, cOpt->typeId);
            if (resolved.name == "fn") {
                return makeFnSymbolFromTypeNode(ident->name, currentModuleId, resolved, symbols.typeVoid);
            }
        }
        const std::string key = resolveSymbolKey(symbols, currentModuleId, ident->name);
        const auto it = symbols.functions.find(key);
        if (it != symbols.functions.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    if (const auto* access = arena.getExpr<FieldAccess>(call.callee)) {
        if (const auto* tgtIdent = arena.getExpr<Identifier>(access->target)) {
            std::string targetModId;
            if (isModuleAliasInScope(symbols, currentModuleId, tgtIdent->name, scope, targetModId)) {
                const std::string key = symbolKey(targetModId, access->fieldName);
                const auto it = symbols.functions.find(key);
                if (it != symbols.functions.end()) {
                    return it->second;
                }
                return std::nullopt;
            }
        }
        if (resolveUnionVariantAccess(arena, symbols, currentModuleId, scope, *access).has_value()) {
            return std::nullopt;
        }
        const TypeId recvTid = inferExprType(arena, access->target, symbols, scope, currentModuleId);
        const auto tsOpt = resolveTypeSymbol(arena, symbols, currentModuleId, recvTid);
        if (tsOpt.has_value()) {
            const auto fit = tsOpt->fields.find(access->fieldName);
            if (fit != tsOpt->fields.end()) {
                const TypeNode fieldResolved = resolveAliasNode(arena, symbols, tsOpt->moduleId, fit->second);
                if (fieldResolved.name == "fn") {
                    return makeFnSymbolFromTypeNode(access->fieldName, currentModuleId, fieldResolved, symbols.typeVoid);
                }
            }
        }
        return resolveUfcsFn(arena, symbols, currentModuleId, recvTid, access->fieldName);
    }

    const TypeId calleeTid = inferExprType(arena, call.callee, symbols, scope, currentModuleId);
    if (calleeTid < arena.types.size()) {
        const TypeNode resolved = resolveAliasNode(arena, symbols, currentModuleId, calleeTid);
        if (resolved.name == "fn") {
            return makeFnSymbolFromTypeNode("fn", currentModuleId, resolved, symbols.typeVoid);
        }
    }

    return std::nullopt;
}

struct ResolvedUnionCaseVariant {
    std::string variantName;
    size_t variantIndex = 0;
    TypeId payloadType = INVALID_ID;
};

inline std::optional<ResolvedUnionCaseVariant> resolveUnionSelectCaseVariant(
    const AstArena& arena,
    const UnionSymbol& unionSym,
    ExprId matchExpr
) {
    std::string varName;
    if (const auto* ident = arena.getExpr<Identifier>(matchExpr)) {
        varName = ident->name;
    } else if (const auto* fa = arena.getExpr<FieldAccess>(matchExpr)) {
        varName = fa->fieldName;
    } else {
        return std::nullopt;
    }
    const auto vTypeIt = unionSym.variantTypes.find(varName);
    if (vTypeIt == unionSym.variantTypes.end()) {
        return std::nullopt;
    }
    size_t idx = 0;
    const auto vIdxIt = unionSym.variantIndices.find(varName);
    if (vIdxIt != unionSym.variantIndices.end()) {
        idx = vIdxIt->second;
    }
    return ResolvedUnionCaseVariant{varName, idx, vTypeIt->second};
}

inline std::vector<std::pair<std::string, TypeId>> resolveForInLoopVars(
    const AstArena& arena,
    const GlobalSymbolTable& symbols,
    std::string_view modId,
    const ForStmt& forStmt,
    const std::unordered_map<std::string, VarSymbol>& scope,
    std::string* errorOut = nullptr
) {
    std::vector<std::pair<std::string, TypeId>> bindings;
    if (forStmt.iterable == INVALID_ID || forStmt.loopVars.empty()) {
        return bindings;
    }
    const TypeId iterTid = inferExprType(arena, forStmt.iterable, symbols, scope, modId);
    if (iterTid >= arena.types.size()) {
        return bindings;
    }
    const TypeNode iterT = resolveAliasNode(arena, symbols, modId, iterTid);
    const size_t nVars = forStmt.loopVars.size();
    if (iterT.name == "list" && !iterT.typeArgs.empty()) {
        if (nVars == 1) {
            bindings.emplace_back(forStmt.loopVars[0], iterT.typeArgs[0]);
        } else if (errorOut != nullptr) {
            *errorOut = "List 'for ... in' loop expects 1 variable (for item in listVal).";
        }
    } else if (iterT.name == "str") {
        if (nVars == 1) {
            bindings.emplace_back(forStmt.loopVars[0], symbols.typeStr);
        } else if (errorOut != nullptr) {
            *errorOut = "String 'for ... in' loop expects 1 variable (for ch in strVal).";
        }
    } else if (iterT.name == "dict" && iterT.typeArgs.size() >= 2) {
        if (nVars == 2) {
            bindings.emplace_back(forStmt.loopVars[0], iterT.typeArgs[0]);
            bindings.emplace_back(forStmt.loopVars[1], iterT.typeArgs[1]);
        } else if (errorOut != nullptr) {
            *errorOut = "Dictionary 'for ... in' loop requires key and value variables (for k, v in dictVal).";
        }
    } else {
        for (const auto& lv : forStmt.loopVars) {
            bindings.emplace_back(lv, symbols.typeAuto);
        }
    }
    return bindings;
}

inline void bindForInLoopVars(
    const AstArena& arena,
    const GlobalSymbolTable& symbols,
    std::string_view modId,
    const ForStmt& forStmt,
    std::unordered_map<std::string, VarSymbol>& loopScope
) {
    const auto bindings = resolveForInLoopVars(arena, symbols, modId, forStmt, loopScope, nullptr);
    for (const auto& kv : bindings) {
        loopScope[kv.first] = VarSymbol{kv.first, kv.second, false, true};
    }
}

inline std::vector<std::string> collectDirectCompositeDeps(
    const AstArena& arena,
    const GlobalSymbolTable& symbols,
    const std::string& key
) {
    std::vector<std::string> deps;
    const auto tit = symbols.types.find(key);
    if (tit != symbols.types.end() && !tit->second.isExtern) {
        for (const auto& fld : tit->second.fieldOrder) {
            const TypeNode& tn = arena.getType(fld.typeId);
            if (tn.typeArgs.empty() && tn.returnType == INVALID_ID) {
                if (symbols.types.count(tn.name) > 0 || symbols.unions.count(tn.name) > 0) {
                    deps.push_back(tn.name);
                }
            }
        }
    } else {
        const auto uit = symbols.unions.find(key);
        if (uit != symbols.unions.end()) {
            for (const auto& uv : uit->second.variants) {
                const TypeNode& tn = arena.getType(uv.type);
                if (tn.typeArgs.empty() && tn.returnType == INVALID_ID) {
                    if (symbols.types.count(tn.name) > 0 || symbols.unions.count(tn.name) > 0) {
                        deps.push_back(tn.name);
                    }
                }
            }
        }
    }
    return deps;
}

} // namespace cb

#endif // CLEANBASIC_SYMBOLS_HPP
