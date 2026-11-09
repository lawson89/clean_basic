
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
    bool isBuiltinReceiver = false;
    std::vector<FnSymbol> overloads;
};

struct EnumSymbol {
    std::string name;
    std::string moduleId;
    std::string key;
    std::vector<EnumMember> members;
    std::unordered_map<std::string, int64_t> memberValues;
    bool isPub = false;
    bool hasExplicitIntType = false;
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

struct ConstSymbol {
    std::string name;
    std::string moduleId;
    std::string key;
    TypeId typeId = INVALID_ID;
    ExprId value = INVALID_ID;
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
    std::unordered_map<std::string, EnumSymbol> enums;
    std::unordered_map<std::string, ConstSymbol> constants;
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
    TypeId typeErr = INVALID_ID;
    TypeId typeCptr = INVALID_ID;
    TypeId typeBytes = INVALID_ID;
};

struct AnalysisResult {
    bool ok = true;
    GlobalSymbolTable symbols;
    DiagnosticError error;
};

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
    symbols.typeBytes = arena.findOrAddType("bytes");

    TypeSymbol errType{};
    errType.name = "err";
    errType.moduleId = "";
    errType.key = symbolKey("", "err");
    errType.isPub = true;
    errType.fields["msg"] = symbols.typeStr;
    errType.fields["code"] = symbols.typeInt;
    errType.fieldOrder.push_back(FieldDecl{1, 1, "msg", symbols.typeStr});
    errType.fieldOrder.push_back(FieldDecl{1, 1, "code", symbols.typeInt});
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
}

inline bool isBuiltinTypeName(std::string_view name) {
    return name == "int" || name == "float" || name == "str" || name == "bool" ||
           name == "void" || name == "auto" || name == "null" ||
           name == "err" || name == "list" || name == "dict" || name == "cptr" ||
           name == "bytes";
}

inline bool isBuiltinSymbolName(std::string_view name) {
    return name == "err" || name == "print";
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
    if (symbols.types.count(key) > 0 || symbols.enums.count(key) > 0 || symbols.aliases.count(key) > 0 || symbols.functions.count(key) > 0 || symbols.constants.count(key) > 0) {
        return key;
    }
    if (isBuiltinSymbolName(name)) {
        return symbolKey("", name);
    }
    return key;
}

// Forward declarations for cross-file recursion
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

inline TypeNode resolveAliasFromNode(
    const AstArena& arena,
    const GlobalSymbolTable& symbols,
    std::string_view currentModuleId,
    TypeNode curr
) {
    std::unordered_set<std::string> seen;
    std::string activeModId(currentModuleId);

    while (!curr.name.empty()) {
        const std::string key = resolveSymbolKey(symbols, activeModId, curr.name);
        const auto it = symbols.aliases.find(key);
        if (it == symbols.aliases.end()) {
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
        curr.name = aliased.name;
        curr.typeArgs = aliased.typeArgs;
    }

    if (!curr.name.empty() && !isBuiltinTypeName(curr.name)) {
        const std::string typeKey = resolveSymbolKey(symbols, activeModId, curr.name);
        const auto tit = symbols.types.find(typeKey);
        if (tit != symbols.types.end()) {
            curr.name = tit->second.key;
        } else {
            const auto eit = symbols.enums.find(typeKey);
            if (eit != symbols.enums.end()) {
                curr.name = eit->second.key;
            }
        }
    }

    return curr;
}

inline std::optional<TypeSymbol> resolveTypeSymbolByName(
    const AstArena& arena,
    const GlobalSymbolTable& symbols,
    std::string_view currentModuleId,
    std::string_view rawName,
    bool requireFirstHopPub = false
) {
    const std::string firstKey = resolveSymbolKey(symbols, currentModuleId, rawName);
    if (symbols.functions.find(firstKey) != symbols.functions.end()) {
        return std::nullopt;
    }
    if (requireFirstHopPub) {
        const auto ait = symbols.aliases.find(firstKey);
        if (ait != symbols.aliases.end() && !ait->second.isPub && ait->second.moduleId != currentModuleId) {
            return std::nullopt;
        }
        const auto tit = symbols.types.find(firstKey);
        if (tit != symbols.types.end() && !tit->second.isPub && tit->second.moduleId != currentModuleId) {
            return std::nullopt;
        }
    }
    const TypeNode resolved = resolveAliasFromNode(arena, symbols, currentModuleId, TypeNode{1, 1, std::string(rawName), {}});
    const auto it = symbols.types.find(resolved.name);
    if (it != symbols.types.end()) {
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

inline std::string resolveUfcsDefiningModule(
    const GlobalSymbolTable& symbols,
    std::string_view currentModuleId,
    std::string_view recvTypeKey
) {
    if (recvTypeKey == "str") {
        if (currentModuleId == "std/str") {
            return "std/str";
        }
        const auto modIt = symbols.modules.find(std::string(currentModuleId));
        if (modIt != symbols.modules.end()) {
            for (const auto& kv : modIt->second) {
                if (kv.second == "std/str") {
                    return "std/str";
                }
            }
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
    const TypeNode p0Resolved = resolveAliasFromNode(arena, symbols, fn.moduleId, arena.types[p0Tid]);
    return p0Resolved.name == recvTypeKey;
}

inline std::optional<FnSymbol> resolveUfcsFn(
    const AstArena& arena,
    const GlobalSymbolTable& symbols,
    std::string_view currentModuleId,
    TypeId recvTid,
    std::string_view methodName
) {
    TypeNode resolvedRecvNode{};
    if (recvTid < arena.types.size()) {
        resolvedRecvNode = resolveAliasFromNode(arena, symbols, currentModuleId, arena.types[recvTid]);
    }
    std::string recvTypeKey = resolvedRecvNode.name;
    if (!recvTypeKey.empty() && !isBuiltinTypeName(recvTypeKey)) {
        recvTypeKey = resolveSymbolKey(symbols, currentModuleId, recvTypeKey);
    }

    const bool isEnumRecv = (symbols.enums.count(recvTypeKey) > 0 ||
                             symbols.enums.count(resolveSymbolKey(symbols, currentModuleId, recvTypeKey)) > 0);

    // Built-in receiver methods
    if (methodName == "len") {
        if (recvTypeKey == "str" || recvTypeKey == "list" || recvTypeKey == "dict" || recvTypeKey == "bytes") {
            FnSymbol fs{};
            fs.name = "len";
            fs.returnType = symbols.typeInt;
            fs.isBuiltinReceiver = true;
            return fs;
        }
    } else if (methodName == "push") {
        if (recvTypeKey == "list" || recvTypeKey == "bytes") {
            FnSymbol fs{};
            fs.name = "push";
            fs.returnType = symbols.typeVoid;
            fs.isBuiltinReceiver = true;
            return fs;
        }
    } else if (methodName == "pop") {
        if (recvTypeKey == "list") {
            TypeId elemType = symbols.typeInt;
            if (!resolvedRecvNode.typeArgs.empty()) {
                elemType = resolvedRecvNode.typeArgs[0];
            }
            FnSymbol fs{};
            fs.name = "pop";
            fs.returnType = elemType;
            fs.isBuiltinReceiver = true;
            return fs;
        }
        if (recvTypeKey == "bytes") {
            FnSymbol fs{};
            fs.name = "pop";
            fs.returnType = symbols.typeInt;
            fs.isBuiltinReceiver = true;
            return fs;
        }
    } else if (methodName == "clear") {
        if (recvTypeKey == "list" || recvTypeKey == "dict" || recvTypeKey == "bytes") {
            FnSymbol fs{};
            fs.name = "clear";
            fs.returnType = symbols.typeVoid;
            fs.isBuiltinReceiver = true;
            return fs;
        }
    } else if (methodName == "has") {
        if (recvTypeKey == "dict") {
            FnSymbol fs{};
            fs.name = "has";
            fs.returnType = symbols.typeBool;
            fs.isBuiltinReceiver = true;
            return fs;
        }
    } else if (methodName == "remove") {
        if (recvTypeKey == "dict") {
            FnSymbol fs{};
            fs.name = "remove";
            fs.returnType = symbols.typeBool;
            fs.isBuiltinReceiver = true;
            return fs;
        }
    } else if (methodName == "toStr") {
        if (recvTypeKey == "int" || recvTypeKey == "float" || recvTypeKey == "bool" || recvTypeKey == "str" || recvTypeKey == "err" || isEnumRecv) {
            FnSymbol fs{};
            fs.name = "toStr";
            fs.returnType = symbols.typeStr;
            fs.isBuiltinReceiver = true;
            return fs;
        }
    } else if (methodName == "toInt") {
        if (recvTypeKey == "int" || recvTypeKey == "float" || recvTypeKey == "bool" || isEnumRecv) {
            FnSymbol fs{};
            fs.name = "toInt";
            fs.returnType = symbols.typeInt;
            fs.isBuiltinReceiver = true;
            return fs;
        }
    } else if (methodName == "toFloat") {
        if (recvTypeKey == "int" || recvTypeKey == "float") {
            FnSymbol fs{};
            fs.name = "toFloat";
            fs.returnType = symbols.typeFloat;
            fs.isBuiltinReceiver = true;
            return fs;
        }
    } else if (methodName == "slice") {
        if (recvTypeKey == "bytes") {
            FnSymbol fs{};
            fs.name = "slice";
            fs.returnType = symbols.typeBytes;
            fs.isBuiltinReceiver = true;
            return fs;
        }
    } else if (methodName == "sliceFrom") {
        if (recvTypeKey == "bytes") {
            FnSymbol fs{};
            fs.name = "sliceFrom";
            fs.returnType = symbols.typeBytes;
            fs.isBuiltinReceiver = true;
            return fs;
        }
    } else if (methodName == "toUtf8") {
        if (recvTypeKey == "bytes") {
            FnSymbol fs{};
            fs.name = "toUtf8";
            fs.returnType = symbols.typeStr;
            fs.isFallible = true;
            fs.isBuiltinReceiver = true;
            return fs;
        }
    } else if (methodName == "toBytes") {
        if (recvTypeKey == "str") {
            FnSymbol fs{};
            fs.name = "toBytes";
            fs.returnType = symbols.typeBytes;
            fs.isBuiltinReceiver = true;
            return fs;
        }
    } else if (methodName == "toHex") {
        if (recvTypeKey == "bytes") {
            FnSymbol fs{};
            fs.name = "toHex";
            fs.returnType = symbols.typeStr;
            fs.isBuiltinReceiver = true;
            return fs;
        }
    } else if (methodName == "fromHex") {
        if (recvTypeKey == "bytes") {
            FnSymbol fs{};
            fs.name = "fromHex";
            fs.returnType = symbols.typeBytes;
            fs.isFallible = true;
            fs.isBuiltinReceiver = true;
            return fs;
        }
    } else if (methodName == "readU16Le" || methodName == "readU16Be" ||
               methodName == "readU32Le" || methodName == "readU32Be" ||
               methodName == "readU64Le" || methodName == "readU64Be") {
        if (recvTypeKey == "bytes") {
            FnSymbol fs{};
            fs.name = std::string(methodName);
            fs.returnType = symbols.typeInt;
            fs.isBuiltinReceiver = true;
            return fs;
        }
    } else if (methodName == "writeU16Le" || methodName == "writeU16Be" ||
               methodName == "writeU32Le" || methodName == "writeU32Be" ||
               methodName == "writeU64Le" || methodName == "writeU64Be") {
        if (recvTypeKey == "bytes") {
            FnSymbol fs{};
            fs.name = std::string(methodName);
            fs.returnType = symbols.typeVoid;
            fs.isBuiltinReceiver = true;
            return fs;
        }
    }

    // UFCS Step 1: Current module
    const std::string localKey = symbolKey(currentModuleId, methodName);
    const auto localIt = symbols.functions.find(localKey);
    if (localIt != symbols.functions.end()) {
        if (ufcsParamMatchesReceiver(arena, symbols, localIt->second, recvTypeKey)) {
            return localIt->second;
        }
        for (const auto& ov : localIt->second.overloads) {
            if (ufcsParamMatchesReceiver(arena, symbols, ov, recvTypeKey)) {
                return ov;
            }
        }
    }

    // UFCS Step 2: Receiver's defining module (including 'std/str' for 'str' when imported)
    const std::string recvModId = resolveUfcsDefiningModule(symbols, currentModuleId, recvTypeKey);
    if (!recvModId.empty()) {
        const std::string recvFnKey = symbolKey(recvModId, methodName);
        const auto recvFnIt = symbols.functions.find(recvFnKey);
        if (recvFnIt != symbols.functions.end()) {
            if ((recvModId == currentModuleId || recvFnIt->second.isPub) &&
                ufcsParamMatchesReceiver(arena, symbols, recvFnIt->second, recvTypeKey)) {
                return recvFnIt->second;
            }
            for (const auto& ov : recvFnIt->second.overloads) {
                if ((recvModId == currentModuleId || ov.isPub) &&
                    ufcsParamMatchesReceiver(arena, symbols, ov, recvTypeKey)) {
                    return ov;
                }
            }
            return std::nullopt;
        }
    }

    return std::nullopt;
}

inline std::optional<EnumSymbol> resolveEnumSymbol(
    const AstArena& /*arena*/,
    const GlobalSymbolTable& symbols,
    std::string_view currentModuleId,
    std::string_view typeName
) {
    const std::string localKey = symbolKey(currentModuleId, typeName);
    const auto it = symbols.enums.find(localKey);
    if (it != symbols.enums.end()) {
        return it->second;
    }
    const std::string rootKey = symbolKey("", typeName);
    const auto it2 = symbols.enums.find(rootKey);
    if (it2 != symbols.enums.end()) {
        return it2->second;
    }
    const std::string normKey = resolveSymbolKey(symbols, currentModuleId, typeName);
    const auto it3 = symbols.enums.find(normKey);
    if (it3 != symbols.enums.end()) {
        return it3->second;
    }
    return std::nullopt;
}

inline std::optional<EnumSymbol> resolveEnumMemberAccess(
    const AstArena& arena,
    const GlobalSymbolTable& symbols,
    std::string_view currentModuleId,
    const std::unordered_map<std::string, VarSymbol>& scope,
    const FieldAccess& faccess
) {
    if (const auto* tgtIdent = arena.getExpr<Identifier>(faccess.target)) {
        if (scope.find(tgtIdent->name) == scope.end()) {
            const auto enumOpt = resolveEnumSymbol(arena, symbols, currentModuleId, tgtIdent->name);
            if (enumOpt.has_value()) {
                return enumOpt;
            }
        }
    }
    if (const auto* innerFa = arena.getExpr<FieldAccess>(faccess.target)) {
        if (const auto* modIdent = arena.getExpr<Identifier>(innerFa->target)) {
            std::string targetModId;
            if (isModuleAliasInScope(symbols, currentModuleId, modIdent->name, scope, targetModId)) {
                const std::string enumKey = symbolKey(targetModId, innerFa->fieldName);
                const auto it = symbols.enums.find(enumKey);
                if (it != symbols.enums.end()) {
                    return it->second;
                }
            }
        }
    }
    return std::nullopt;
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

inline bool isEnumType(const GlobalSymbolTable& symbols, TypeId tid, const AstArena& arena, std::string_view currentModuleId) {
    if (tid >= arena.types.size()) return false;
    const TypeNode resolved = resolveAliasFromNode(arena, symbols, currentModuleId, arena.types[tid]);
    if (symbols.enums.find(resolved.name) != symbols.enums.end()) return true;
    const std::string k = resolveSymbolKey(symbols, currentModuleId, resolved.name);
    return symbols.enums.find(k) != symbols.enums.end();
}

inline bool fnMatchesArgCount(const FnSymbol& fn, size_t totalArgs, bool isUfcs = false) {
    size_t startIdx = 0;
    if (isUfcs) {
        startIdx = 1;
    }
    if (startIdx > fn.params.size()) {
        return false;
    }
    const size_t maxArgs = fn.params.size() - startIdx;
    if (totalArgs > maxArgs) {
        return false;
    }
    size_t minArgs = 0;
    for (size_t i = startIdx; i < fn.params.size(); ++i) {
        if (!fn.params[i].hasDefault) {
            minArgs += 1;
        }
    }
    return totalArgs >= minArgs;
}

inline std::optional<FnSymbol> resolveFuncCall(
    const AstArena& arena,
    const FuncCall& call,
    const GlobalSymbolTable& symbols,
    std::string_view currentModuleId,
    const std::unordered_map<std::string, VarSymbol>& scope = {}
) {
    const size_t totalArgs = call.args.size() + call.namedArgs.size();
    if (const auto* ident = arena.getExpr<Identifier>(call.callee)) {
        const std::string key = resolveSymbolKey(symbols, currentModuleId, ident->name);
        const auto it = symbols.functions.find(key);
        if (it != symbols.functions.end()) {
            if (!it->second.overloads.empty()) {
                if (it->second.params.size() == totalArgs) {
                    return it->second;
                }
                for (const auto& ov : it->second.overloads) {
                    if (ov.params.size() == totalArgs) {
                        return ov;
                    }
                }
                if (fnMatchesArgCount(it->second, totalArgs, false)) {
                    return it->second;
                }
                for (const auto& ov : it->second.overloads) {
                    if (fnMatchesArgCount(ov, totalArgs, false)) {
                        return ov;
                    }
                }
            }
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
                    if (!it->second.overloads.empty()) {
                        if (it->second.params.size() == totalArgs) {
                            return it->second;
                        }
                        for (const auto& ov : it->second.overloads) {
                            if (ov.params.size() == totalArgs) {
                                return ov;
                            }
                        }
                        if (fnMatchesArgCount(it->second, totalArgs, false)) {
                            return it->second;
                        }
                        for (const auto& ov : it->second.overloads) {
                            if (fnMatchesArgCount(ov, totalArgs, false)) {
                                return ov;
                            }
                        }
                    }
                    return it->second;
                }
                return std::nullopt;
            }
            if (tgtIdent->name == "bytes" && access->fieldName == "fromHex") {
                FnSymbol fs{};
                fs.name = "fromHex";
                fs.returnType = symbols.typeBytes;
                fs.isFallible = true;
                fs.isBuiltinReceiver = true;
                return fs;
            }
        }
        const TypeId recvTid = inferExprType(arena, access->target, symbols, scope, currentModuleId);
        return resolveUfcsFn(arena, symbols, currentModuleId, recvTid, access->fieldName);
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

inline TypeNode resolveAliasNode(
    const AstArena& arena,
    const GlobalSymbolTable& symbols,
    std::string_view currentModuleId,
    TypeId tid
) {
    if (tid >= arena.types.size()) {
        return TypeNode{};
    }
    return resolveAliasFromNode(arena, symbols, currentModuleId, arena.types[tid]);
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

} // namespace cb

#endif // CLEANBASIC_SYMBOLS_HPP
