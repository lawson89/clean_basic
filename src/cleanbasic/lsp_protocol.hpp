
#ifndef CLEANBASIC_LSP_PROTOCOL_HPP
#define CLEANBASIC_LSP_PROTOCOL_HPP

#include "src/cleanbasic/json.hpp"
#include "src/cleanbasic/pipeline.hpp"
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace cb {

struct LspDocumentSnapshot {
    bool valid = false;
    AstArena arena;
    Program program;
    GlobalSymbolTable symbols;
};

struct LspServerState {
    bool initialized = false;
    bool shutdownRequested = false;
    bool shouldExit = false;
    int exitCode = 0;
    std::unordered_map<std::string, std::string> openDocuments;
    std::unordered_map<std::string, LspDocumentSnapshot> snapshots;
};

inline std::string wrapVbCodeBlock(std::string_view code) {
    const std::string fence(3, static_cast<char>(96));
    return fence + "vb\n" + std::string(code) + "\n" + fence;
}

inline std::string wrapInlineCode(std::string_view text) {
    const std::string tick(1, static_cast<char>(96));
    return tick + std::string(text) + tick;
}

inline bool isAsciiAlpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

inline std::string percentDecodeUriPath(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    size_t i = 0;
    while (i < raw.size()) {
        if (raw[i] == '%' && i + 2 < raw.size()) {
            const auto hi = jsonHexDigitValue(raw[i + 1]);
            const auto lo = jsonHexDigitValue(raw[i + 2]);
            if (hi.has_value() && lo.has_value()) {
                const uint32_t byteVal = (hi.value() << 4) | lo.value();
                out.push_back(static_cast<char>(byteVal));
                i += 3;
                continue;
            }
        }
        out.push_back(raw[i]);
        i += 1;
    }
    return out;
}

inline std::string uriToFilePath(std::string_view uri) {
    std::string_view rest = uri;
    const std::string_view scheme = "file://";
    if (rest.size() >= scheme.size() && rest.substr(0, scheme.size()) == scheme) {
        rest = rest.substr(scheme.size());
        const std::string_view localhost = "localhost/";
        if (rest.size() >= localhost.size() && rest.substr(0, localhost.size()) == localhost) {
            rest = rest.substr(localhost.size() - 1);
        }
        std::string decoded = percentDecodeUriPath(rest);
        if (decoded.size() >= 3 && decoded[0] == '/' && isAsciiAlpha(decoded[1]) && decoded[2] == ':') {
            decoded = decoded.substr(1);
        }
        return decoded;
    }
    return std::string(uri);
}

inline std::string filePathToUri(std::string_view path) {
    const std::string_view scheme = "file://";
    if (path.size() >= scheme.size() && path.substr(0, scheme.size()) == scheme) {
        return std::string(path);
    }
    if (!path.empty() && path[0] == '/') {
        return "file://" + std::string(path);
    }
    return "file:///" + std::string(path);
}

inline std::unordered_map<std::string, std::string> buildOpenFileBuffers(const LspServerState& state) {
    std::unordered_map<std::string, std::string> buffers;
    for (const auto& kv : state.openDocuments) {
        const std::string filePath = uriToFilePath(kv.first);
        buffers[filePath] = kv.second;
        const std::string normPath = normalizeRelPath(filePath);
        if (!normPath.empty()) {
            buffers[normPath] = kv.second;
        }
    }
    return buffers;
}

inline std::string readDocumentText(const LspServerState& state, std::string_view uri) {
    const auto it = state.openDocuments.find(std::string(uri));
    if (it != state.openDocuments.end()) {
        return it->second;
    }
    const std::string filePath = uriToFilePath(uri);
    const auto openBuffers = buildOpenFileBuffers(state);
    const auto memOpt = findInMemorySource(openBuffers, filePath);
    if (memOpt.has_value()) {
        return memOpt.value();
    }
    std::ifstream in(filePath);
    if (!in.is_open()) {
        return "";
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

inline LspDocumentSnapshot getOrBuildSnapshot(LspServerState& state, std::string_view uri) {
    const std::string uriKey(uri);
    const std::string filePath = uriToFilePath(uri);
    const std::string text = readDocumentText(state, uri);
    if (!text.empty()) {
        const auto openBuffers = buildOpenFileBuffers(state);
        const CheckSourceResult checkRes = checkSource(text, filePath, openBuffers);
        if (checkRes.ok) {
            LspDocumentSnapshot snap{true, checkRes.arena, checkRes.program, checkRes.symbols};
            state.snapshots[uriKey] = snap;
            return snap;
        }
    }
    const auto snapIt = state.snapshots.find(uriKey);
    if (snapIt != state.snapshots.end() && snapIt->second.valid) {
        return snapIt->second;
    }
    return LspDocumentSnapshot{};
}

inline JsonId cloneJsonScalarOrNull(const JsonArena& srcArena, JsonId srcId, JsonArena& dstArena) {
    if (srcId >= srcArena.nodes.size()) {
        return jsonAddNull(dstArena);
    }
    const auto& var = srcArena.nodes[srcId];
    if (std::holds_alternative<JsonNull>(var)) return jsonAddNull(dstArena);
    if (std::holds_alternative<JsonBool>(var)) return jsonAddBool(dstArena, std::get<JsonBool>(var).value);
    if (std::holds_alternative<JsonNumber>(var)) {
        const auto& num = std::get<JsonNumber>(var);
        if (num.isInt) return jsonAddInt(dstArena, num.intVal);
        return jsonAddFloat(dstArena, num.floatVal);
    }
    if (std::holds_alternative<JsonString>(var)) return jsonAddString(dstArena, std::get<JsonString>(var).value);
    return jsonAddNull(dstArena);
}

inline JsonId makeLspRange(
    JsonArena& arena,
    uint32_t startLine1Based,
    uint32_t startCol1Based,
    uint32_t endLine1Based,
    uint32_t endCol1Based
) {
    int64_t sLine = 0;
    if (startLine1Based > 0) {
        sLine = static_cast<int64_t>(startLine1Based - 1);
    }
    int64_t sCol = 0;
    if (startCol1Based > 0) {
        sCol = static_cast<int64_t>(startCol1Based - 1);
    }
    int64_t eLine = sLine;
    if (endLine1Based > 0) {
        eLine = static_cast<int64_t>(endLine1Based - 1);
    }
    if (eLine < sLine) eLine = sLine;
    int64_t eCol = sCol;
    if (endCol1Based > 0) {
        eCol = static_cast<int64_t>(endCol1Based - 1);
    }

    const JsonId startPos = jsonAddObject(arena);
    jsonObjectSet(arena, startPos, "line", jsonAddInt(arena, sLine));
    jsonObjectSet(arena, startPos, "character", jsonAddInt(arena, sCol));

    const JsonId endPos = jsonAddObject(arena);
    jsonObjectSet(arena, endPos, "line", jsonAddInt(arena, eLine));
    jsonObjectSet(arena, endPos, "character", jsonAddInt(arena, eCol));

    const JsonId rangeObj = jsonAddObject(arena);
    jsonObjectSet(arena, rangeObj, "start", startPos);
    jsonObjectSet(arena, rangeObj, "end", endPos);
    return rangeObj;
}

inline JsonId makeLspLocation(
    JsonArena& arena,
    std::string_view filePath,
    uint32_t line1Based,
    uint32_t col1Based,
    size_t nameLen
) {
    uint32_t spanLen = 1;
    if (nameLen > 0) {
        spanLen = static_cast<uint32_t>(nameLen);
    }
    const JsonId loc = jsonAddObject(arena);
    jsonObjectSet(arena, loc, "uri", jsonAddString(arena, filePathToUri(filePath)));
    jsonObjectSet(arena, loc, "range", makeLspRange(arena, line1Based, col1Based, line1Based, col1Based + spanLen));
    return loc;
}

inline std::string formatCleanbasicType(const AstArena& arena, TypeId tid) {
    if (tid == INVALID_ID || tid >= arena.types.size()) return "void";
    const TypeNode& tn = arena.types[tid];
    if (tn.name.empty() || tn.name == "void") return "void";
    const auto [modId, baseName] = splitSymbolKey(tn.name);
    std::string out = baseName;
    if (!tn.typeArgs.empty()) {
        out += "[";
        for (size_t i = 0; i < tn.typeArgs.size(); ++i) {
            if (i > 0) out += ", ";
            out += formatCleanbasicType(arena, tn.typeArgs[i]);
        }
        out += "]";
    }
    return out;
}

inline std::string formatFnDetailSignature(
    const AstArena& arena,
    const std::vector<Param>& params,
    TypeId returnType,
    bool isFallible
) {
    std::string out = "(";
    for (size_t i = 0; i < params.size(); ++i) {
        if (i > 0) out += ", ";
        if (params[i].isByref) out += "byref ";
        out += params[i].name + " as " + formatCleanbasicType(arena, params[i].typeId);
    }
    out += ")";
    if (!arena.isVoid(returnType)) {
        out += " as " + formatCleanbasicType(arena, returnType);
    }
    if (isFallible) out += " can error";
    return out;
}

inline std::string formatFnHoverMarkdown(const AstArena& arena, const FnSymbol& fn) {
    std::string sig;
    if (fn.isPub) sig += "pub ";
    if (fn.isExtern) sig += "extern ";
    sig += "fn " + fn.name + formatFnDetailSignature(arena, fn.params, fn.returnType, fn.isFallible);
    return wrapVbCodeBlock(sig);
}

inline std::string formatTypeHoverMarkdown(const AstArena& arena, const TypeSymbol& ts) {
    std::string code;
    if (ts.isPub) code += "pub ";
    if (ts.isExtern) code += "extern ";
    code += "type " + ts.name;
    if (ts.isExtern && ts.fieldOrder.empty()) return wrapVbCodeBlock(code);
    code += "\n";
    for (const auto& f : ts.fieldOrder) {
        TypeId ftid = f.typeId;
        const auto fit = ts.fields.find(f.name);
        if (fit != ts.fields.end()) ftid = fit->second;
        code += "    " + f.name + " as " + formatCleanbasicType(arena, ftid) + "\n";
    }
    code += "end type";
    return wrapVbCodeBlock(code);
}

inline std::string formatEnumHoverMarkdown(const EnumSymbol& es) {
    std::string code;
    if (es.isPub) {
        code += "pub ";
    }
    code += "enum " + es.name + "\n";
    for (const auto& m : es.members) {
        code += "    " + m.name;
        const auto vit = es.memberValues.find(m.name);
        if (vit != es.memberValues.end()) {
            code += " = " + std::to_string(vit->second);
        }
        code += "\n";
    }
    code += "end enum";
    return wrapVbCodeBlock(code);
}

inline std::string formatAliasHoverMarkdown(const AstArena& arena, const GlobalSymbolTable& symbols, const AliasSymbol& al) {
    std::string code;
    if (al.isPub) code += "pub ";
    const std::string rawTarget = formatCleanbasicType(arena, al.targetType);
    const TypeId resolvedTid = resolveAlias(arena, symbols, al.moduleId, al.targetType);
    const std::string resolvedTarget = formatCleanbasicType(arena, resolvedTid);
    code += "alias " + al.name + " = " + rawTarget;
    std::string md = wrapVbCodeBlock(code);
    if (resolvedTarget != rawTarget) {
        md += "\nResolved type: " + wrapInlineCode(resolvedTarget);
    }
    return md;
}

inline std::string formatConstHoverMarkdown(const AstArena& arena, const ConstSymbol& cs) {
    std::string code;
    if (cs.isPub) code += "pub ";
    code += "const " + cs.name + " as " + formatCleanbasicType(arena, cs.typeId);
    return wrapVbCodeBlock(code);
}

inline std::string formatVarHoverMarkdown(const AstArena& arena, const VarSymbol& vs) {
    std::string prefix = "dim ";
    if (vs.isByref) prefix = "(param) byref ";
    else if (vs.isConst) prefix = "const ";
    return wrapVbCodeBlock(prefix + vs.name + " as " + formatCleanbasicType(arena, vs.typeId));
}

inline std::optional<std::string> formatBuiltinHoverMarkdown(std::string_view name) {
    if (name == "print") {
        return wrapVbCodeBlock("fn print(values...)") + "\nWrites one or more values to standard output followed by a newline.";
    }
    if (name == "error") {
        return wrapVbCodeBlock("fn error(msg as str, [code=1]) as err") + "\nRaises and returns a structured err value from a can error function.";
    }
    if (name == "len") {
        return wrapVbCodeBlock("fn .len() as int") + "\nReturns the number of elements in a list, dict, or str.";
    }
    if (name == "toStr") {
        return wrapVbCodeBlock("fn .toStr() as str") + "\nConverts a numeric, boolean, or err value to a str.";
    }
    if (name == "toInt") {
        return wrapVbCodeBlock("fn .toInt() as int") + "\nConverts a numeric or boolean value to an int.";
    }
    if (name == "toFloat") {
        return wrapVbCodeBlock("fn .toFloat() as float") + "\nConverts a numeric value to a float.";
    }
    if (name == "push") {
        return wrapVbCodeBlock("fn .push(item as T)") + "\nAppends an item to the end of a list or bytes buffer.";
    }
    if (name == "pop") {
        return wrapVbCodeBlock("fn .pop() as T") + "\nRemoves and returns the last element from a list or bytes buffer.";
    }
    if (name == "clear") {
        return wrapVbCodeBlock("fn .clear()") + "\nRemoves all elements from a list, dict, or bytes buffer.";
    }
    if (name == "has") {
        return wrapVbCodeBlock("fn .has(key as K) as bool") + "\nReturns true if the dict contains the specified key.";
    }
    if (name == "remove") {
        return wrapVbCodeBlock("fn .remove(key as K) as bool") + "\nRemoves the entry for key from a dict and returns true if it existed.";
    }
    return std::nullopt;
}

inline bool declBelongsToFile(const DeclNode& declNode, std::string_view targetFilePath, std::string_view targetNormPath) {
    if (declNode.fileName == targetFilePath) return true;
    const std::string declNorm = normalizeRelPath(declNode.fileName);
    if (!declNorm.empty() && !targetNormPath.empty() && declNorm == targetNormPath) return true;
    return declNode.moduleId.empty() && declNode.fileName.empty();
}

inline std::string findModuleIdForFile(const AstArena& arena, const Program& program, std::string_view filePath) {
    const std::string normTarget = normalizeRelPath(filePath);
    for (const DeclId did : program.declarations) {
        if (did >= arena.decls.size()) continue;
        const DeclNode& dn = arena.decls[did];
        if (declBelongsToFile(dn, filePath, normTarget)) return dn.moduleId;
    }
    return "";
}

inline std::optional<std::string> findEnclosingFnKey(const AstArena& arena, const Program& program, std::string_view filePath, uint32_t cursorLine1Based) {
    const std::string normTarget = normalizeRelPath(filePath);
    for (const DeclId did : program.declarations) {
        if (did >= arena.decls.size()) continue;
        const DeclNode& dn = arena.decls[did];
        if (!declBelongsToFile(dn, filePath, normTarget)) continue;
        if (const auto* fn = arena.getDecl<FnDecl>(did)) {
            if (cursorLine1Based >= dn.line && cursorLine1Based <= dn.endLine) return symbolKey(dn.moduleId, fn->name);
        }
    }
    return std::nullopt;
}

inline std::optional<std::string> findModuleFilePath(const AstArena& arena, const Program& program, std::string_view targetModuleId, std::string_view currentFilePath, std::string_view importTarget = "") {
    for (const DeclId did : program.declarations) {
        if (did >= arena.decls.size()) continue;
        const DeclNode& dn = arena.decls[did];
        if (dn.moduleId == targetModuleId && !dn.fileName.empty()) return dn.fileName;
    }
    if (!importTarget.empty()) {
        if (isStdImportTarget(importTarget)) {
            const std::string stdSub = stripCbExt(importTarget.substr(4));
            return cleanbasicRootDir() + "/stdlib/" + stdSub + "/" + stdSub + ".cb";
        }
        const std::string dir = parentDirOf(currentFilePath);
        std::string prefix;
        if (dir != ".") {
            prefix = dir + "/";
        }
        return normalizeRelPath(prefix + std::string(importTarget));
    }
    return std::nullopt;
}

struct LspTextPositionParams {
    bool valid = false;
    std::string uri;
    std::string filePath;
    uint32_t line0Based = 0;
    uint32_t char0Based = 0;
    uint32_t line1Based = 1;
    uint32_t col1Based = 1;
};

inline LspTextPositionParams parseDocPositionParams(const JsonArena& reqArena, JsonId paramsId) {
    LspTextPositionParams res{};
    const JsonId textDoc = jsonGetField(reqArena, paramsId, "textDocument");
    const auto uriOpt = jsonGetString(reqArena, jsonGetField(reqArena, textDoc, "uri"));
    const JsonId posObj = jsonGetField(reqArena, paramsId, "position");
    const auto line0Opt = jsonGetInt(reqArena, jsonGetField(reqArena, posObj, "line"));
    const auto char0Opt = jsonGetInt(reqArena, jsonGetField(reqArena, posObj, "character"));
    if (uriOpt.has_value()) {
        res.uri = uriOpt.value();
        res.filePath = uriToFilePath(res.uri);
    }
    if (uriOpt.has_value() && line0Opt.has_value() && char0Opt.has_value()) {
        res.valid = true;
        res.line0Based = static_cast<uint32_t>(line0Opt.value());
        res.char0Based = static_cast<uint32_t>(char0Opt.value());
        res.line1Based = res.line0Based + 1;
        res.col1Based = res.char0Based + 1;
    }
    return res;
}

inline void writeJsonrpcResult(const JsonArena& reqArena, JsonId reqId, JsonArena& outArena, JsonId resultId, std::ostream& out) {
    const JsonId root = jsonAddObject(outArena);
    jsonObjectSet(outArena, root, "jsonrpc", jsonAddString(outArena, "2.0"));
    jsonObjectSet(outArena, root, "id", cloneJsonScalarOrNull(reqArena, reqId, outArena));
    jsonObjectSet(outArena, root, "result", resultId);
    writeJsonrpcMessage(out, serializeJson(outArena, root));
}

inline JsonId makeLspDocSymbol(JsonArena& arena, std::string_view name, std::string_view detail, int64_t kind, uint32_t startLine, uint32_t startCol, uint32_t endLine, uint32_t nameLine, uint32_t nameCol) {
    const uint32_t nameLen = static_cast<uint32_t>(name.size());
    const JsonId symObj = jsonAddObject(arena);
    jsonObjectSet(arena, symObj, "name", jsonAddString(arena, name));
    jsonObjectSet(arena, symObj, "detail", jsonAddString(arena, detail));
    jsonObjectSet(arena, symObj, "kind", jsonAddInt(arena, kind));
    jsonObjectSet(arena, symObj, "range", makeLspRange(arena, startLine, startCol, endLine, nameCol + nameLen));
    jsonObjectSet(arena, symObj, "selectionRange", makeLspRange(arena, nameLine, nameCol, nameLine, nameCol + nameLen));
    return symObj;
}

inline void publishEmptyDiagnostics(std::string_view uri, std::ostream& out) {
    JsonArena arena;
    const JsonId root = jsonAddObject(arena);
    jsonObjectSet(arena, root, "jsonrpc", jsonAddString(arena, "2.0"));
    jsonObjectSet(arena, root, "method", jsonAddString(arena, "textDocument/publishDiagnostics"));
    const JsonId params = jsonAddObject(arena);
    jsonObjectSet(arena, params, "uri", jsonAddString(arena, uri));
    jsonObjectSet(arena, params, "diagnostics", jsonAddArray(arena));
    jsonObjectSet(arena, root, "params", params);
    writeJsonrpcMessage(out, serializeJson(arena, root));
}

inline void publishDocumentDiagnostics(LspServerState& state, std::string_view uri, std::string_view text, std::ostream& out) {
    const std::string filePath = uriToFilePath(uri);
    const auto openBuffers = buildOpenFileBuffers(state);
    const CheckSourceResult checkRes = checkSource(text, filePath, openBuffers);
    if (checkRes.ok) state.snapshots[std::string(uri)] = LspDocumentSnapshot{true, checkRes.arena, checkRes.program, checkRes.symbols};
    JsonArena arena;
    const JsonId root = jsonAddObject(arena);
    jsonObjectSet(arena, root, "jsonrpc", jsonAddString(arena, "2.0"));
    jsonObjectSet(arena, root, "method", jsonAddString(arena, "textDocument/publishDiagnostics"));
    const JsonId params = jsonAddObject(arena);
    jsonObjectSet(arena, params, "uri", jsonAddString(arena, uri));
    const JsonId diagArr = jsonAddArray(arena);
    if (!checkRes.ok) {
        int64_t line0 = 0;
        if (checkRes.error.line > 0) {
            line0 = static_cast<int64_t>(checkRes.error.line - 1);
        }
        int64_t col0 = 0;
        if (checkRes.error.col > 0) {
            col0 = static_cast<int64_t>(checkRes.error.col - 1);
        }
        const JsonId startPos = jsonAddObject(arena);
        jsonObjectSet(arena, startPos, "line", jsonAddInt(arena, line0));
        jsonObjectSet(arena, startPos, "character", jsonAddInt(arena, col0));
        const JsonId endPos = jsonAddObject(arena);
        jsonObjectSet(arena, endPos, "line", jsonAddInt(arena, line0));
        jsonObjectSet(arena, endPos, "character", jsonAddInt(arena, col0 + 1));
        const JsonId rangeObj = jsonAddObject(arena);
        jsonObjectSet(arena, rangeObj, "start", startPos);
        jsonObjectSet(arena, rangeObj, "end", endPos);
        const JsonId diagObj = jsonAddObject(arena);
        jsonObjectSet(arena, diagObj, "range", rangeObj);
        jsonObjectSet(arena, diagObj, "severity", jsonAddInt(arena, 1));
        jsonObjectSet(arena, diagObj, "source", jsonAddString(arena, "cleanbasic"));
        jsonObjectSet(arena, diagObj, "code", jsonAddString(arena, checkRes.error.kind));
        jsonObjectSet(arena, diagObj, "message", jsonAddString(arena, checkRes.error.message));
        jsonArrayPush(arena, diagArr, diagObj);
    }
    jsonObjectSet(arena, params, "diagnostics", diagArr);
    jsonObjectSet(arena, root, "params", params);
    writeJsonrpcMessage(out, serializeJson(arena, root));
}

} // namespace cb

#endif // CLEANBASIC_LSP_PROTOCOL_HPP
