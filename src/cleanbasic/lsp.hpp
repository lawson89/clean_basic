#ifndef CLEANBASIC_LSP_HPP
#define CLEANBASIC_LSP_HPP

#include "src/cleanbasic/json.hpp"
#include "src/cleanbasic/pipeline.hpp"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
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
    if (c >= 'a' && c <= 'z') {
        return true;
    }
    if (c >= 'A' && c <= 'Z') {
        return true;
    }
    return false;
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
    LspDocumentSnapshot emptySnap{};
    emptySnap.valid = false;
    return emptySnap;
}

inline JsonId cloneJsonScalarOrNull(const JsonArena& srcArena, JsonId srcId, JsonArena& dstArena) {
    if (srcId >= srcArena.nodes.size()) {
        return jsonAddNull(dstArena);
    }
    const auto& var = srcArena.nodes[srcId];
    if (std::holds_alternative<JsonNull>(var)) {
        return jsonAddNull(dstArena);
    }
    if (std::holds_alternative<JsonBool>(var)) {
        return jsonAddBool(dstArena, std::get<JsonBool>(var).value);
    }
    if (std::holds_alternative<JsonNumber>(var)) {
        const auto& num = std::get<JsonNumber>(var);
        if (num.isInt) {
            return jsonAddInt(dstArena, num.intVal);
        }
        return jsonAddFloat(dstArena, num.floatVal);
    }
    if (std::holds_alternative<JsonString>(var)) {
        return jsonAddString(dstArena, std::get<JsonString>(var).value);
    }
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
    if (eLine < sLine) {
        eLine = sLine;
    }
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
    jsonObjectSet(
        arena,
        loc,
        "range",
        makeLspRange(arena, line1Based, col1Based, line1Based, col1Based + spanLen)
    );
    return loc;
}

inline std::string formatCleanbasicType(const AstArena& arena, TypeId tid) {
    if (tid == INVALID_ID || tid >= arena.types.size()) {
        return "void";
    }
    const TypeNode& tn = arena.types[tid];
    if (tn.name.empty() || tn.name == "void") {
        return "void";
    }
    const auto [modId, baseName] = splitSymbolKey(tn.name);
    std::string out = baseName;
    if (!tn.typeArgs.empty()) {
        out += "[";
        for (size_t i = 0; i < tn.typeArgs.size(); ++i) {
            if (i > 0) {
                out += ", ";
            }
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
        if (i > 0) {
            out += ", ";
        }
        if (params[i].isByref) {
            out += "byref ";
        }
        out += params[i].name + " as " + formatCleanbasicType(arena, params[i].typeId);
    }
    out += ")";
    if (!isVoidType(arena, returnType)) {
        out += " as " + formatCleanbasicType(arena, returnType);
    }
    if (isFallible) {
        out += " can error";
    }
    return out;
}

inline std::string formatFnHoverMarkdown(const AstArena& arena, const FnSymbol& fn) {
    std::string sig;
    if (fn.isPub) {
        sig += "pub ";
    }
    if (fn.isExtern) {
        sig += "extern ";
    }
    sig += "fn " + fn.name + formatFnDetailSignature(arena, fn.params, fn.returnType, fn.isFallible);
    return wrapVbCodeBlock(sig);
}

inline std::string formatTypeHoverMarkdown(const AstArena& arena, const TypeSymbol& ts) {
    std::string code;
    if (ts.isPub) {
        code += "pub ";
    }
    if (ts.isExtern) {
        code += "extern ";
    }
    code += "type " + ts.name;
    if (ts.isExtern && ts.fieldOrder.empty()) {
        return wrapVbCodeBlock(code);
    }
    code += "\n";
    for (const auto& f : ts.fieldOrder) {
        TypeId ftid = f.typeId;
        const auto fit = ts.fields.find(f.name);
        if (fit != ts.fields.end()) {
            ftid = fit->second;
        }
        code += "    " + f.name + " as " + formatCleanbasicType(arena, ftid) + "\n";
    }
    code += "end type";
    return wrapVbCodeBlock(code);
}

inline std::string formatAliasHoverMarkdown(
    const AstArena& arena,
    const GlobalSymbolTable& symbols,
    const AliasSymbol& al
) {
    std::string code;
    if (al.isPub) {
        code += "pub ";
    }
    const std::string rawTarget = formatCleanbasicType(arena, al.targetType);
    const TypeId resolvedTid = resolveAlias(arena, symbols, al.moduleId, al.targetType);
    const std::string resolvedTarget = formatCleanbasicType(arena, resolvedTid);
    code += "alias " + al.name + " = " + rawTarget;
    std::string md = wrapVbCodeBlock(code);
    if (resolvedTarget != rawTarget) {
        md.push_back(static_cast<char>(10));
        md += "Resolved type: " + wrapInlineCode(resolvedTarget);
    }
    return md;
}

inline std::string formatVarHoverMarkdown(const AstArena& arena, const VarSymbol& vs) {
    std::string prefix = "dim ";
    if (vs.isByref) {
        prefix = "(param) byref ";
    } else if (vs.isConst) {
        prefix = "const ";
    }
    const std::string tname = formatCleanbasicType(arena, vs.typeId);
    return wrapVbCodeBlock(prefix + vs.name + " as " + tname);
}

inline std::optional<std::string> formatBuiltinHoverMarkdown(std::string_view name) {
    if (name == "print") {
        return wrapVbCodeBlock("fn print(values...)") +
            "\nWrites one or more values to standard output followed by a newline.";
    }
    if (name == "len") {
        return wrapVbCodeBlock("fn len(container as any) as int") +
            "\nReturns the number of elements in a list, dict, or str.";
    }
    if (name == "str") {
        return wrapVbCodeBlock("fn str(val as any) as str") +
            "\nConverts a numeric, boolean, or err value to a str.";
    }
    if (name == "error") {
        return wrapVbCodeBlock("fn error(msg as str, [code=1]) as err") +
            "\nRaises and returns a structured err value from a can error function.";
    }
    return std::nullopt;
}

inline bool declBelongsToFile(
    const DeclNode& declNode,
    std::string_view targetFilePath,
    std::string_view targetNormPath
) {
    if (declNode.fileName == targetFilePath) {
        return true;
    }
    const std::string declNorm = normalizeRelPath(declNode.fileName);
    if (!declNorm.empty() && !targetNormPath.empty() && declNorm == targetNormPath) {
        return true;
    }
    if (declNode.moduleId.empty() && declNode.fileName.empty()) {
        return true;
    }
    return false;
}

inline std::string findModuleIdForFile(
    const AstArena& arena,
    const Program& program,
    std::string_view filePath
) {
    const std::string normTarget = normalizeRelPath(filePath);
    for (const DeclId did : program.declarations) {
        if (did >= arena.decls.size()) {
            continue;
        }
        const DeclNode& dn = arena.decls[did];
        if (declBelongsToFile(dn, filePath, normTarget)) {
            return dn.moduleId;
        }
    }
    return "";
}

inline std::optional<std::string> findEnclosingFnKey(
    const AstArena& arena,
    const Program& program,
    std::string_view filePath,
    uint32_t cursorLine1Based
) {
    const std::string normTarget = normalizeRelPath(filePath);
    for (const DeclId did : program.declarations) {
        if (did >= arena.decls.size()) {
            continue;
        }
        const DeclNode& dn = arena.decls[did];
        if (!declBelongsToFile(dn, filePath, normTarget)) {
            continue;
        }
        if (std::holds_alternative<FnDecl>(dn.payload)) {
            const auto& fn = std::get<FnDecl>(dn.payload);
            if (cursorLine1Based >= dn.line && cursorLine1Based <= dn.endLine) {
                return symbolKey(dn.moduleId, fn.name);
            }
        }
    }
    return std::nullopt;
}

struct CursorTokenContext {
    bool found = false;
    Token token;
    size_t tokenIndex = 0;
    bool hasDotReceiver = false;
    std::string receiverName;
    bool isImportTarget = false;
};

inline CursorTokenContext resolveCursorToken(
    std::string_view sourceText,
    std::string_view filePath,
    uint32_t line1Based,
    uint32_t col1Based
) {
    CursorTokenContext ctx{};
    const LexResult lexRes = tokenize(sourceText, filePath);
    if (lexRes.tokens.empty()) {
        return ctx;
    }

    for (size_t i = 0; i < lexRes.tokens.size(); ++i) {
        const Token& tok = lexRes.tokens[i];
        if (tok.kind == "EOF" || tok.kind == "NEWLINE") {
            continue;
        }
        if (tok.line != line1Based) {
            continue;
        }
        const uint32_t tokStart = tok.col;
        uint32_t tokLen = 1;
        if (!tok.value.empty()) {
            tokLen = static_cast<uint32_t>(tok.value.size());
        }
        const uint32_t tokEnd = tokStart + tokLen;
        if (col1Based >= tokStart && col1Based <= tokEnd) {
            ctx.found = true;
            ctx.token = tok;
            ctx.tokenIndex = i;
            if (i >= 2 && lexRes.tokens[i - 1].kind == "DOT") {
                const Token& prevTok = lexRes.tokens[i - 2];
                if (prevTok.kind == "IDENT" || prevTok.kind == "TYPE_KW") {
                    ctx.hasDotReceiver = true;
                    ctx.receiverName = prevTok.value;
                }
            }
            if (i >= 1 && lexRes.tokens[i - 1].kind == "KEYWORD" &&
                lexRes.tokens[i - 1].value == "import" && tok.kind == "STRING") {
                ctx.isImportTarget = true;
            }
            return ctx;
        }
    }
    return ctx;
}

inline std::optional<std::string> findModuleFilePath(
    const AstArena& arena,
    const Program& program,
    std::string_view targetModuleId,
    std::string_view currentFilePath,
    std::string_view importTarget = ""
) {
    for (const DeclId did : program.declarations) {
        if (did >= arena.decls.size()) {
            continue;
        }
        const DeclNode& dn = arena.decls[did];
        if (dn.moduleId == targetModuleId && !dn.fileName.empty()) {
            return dn.fileName;
        }
    }
    if (!importTarget.empty()) {
        if (isStdImportTarget(importTarget)) {
            const std::string stdSub = stripCbExt(importTarget.substr(4));
            return cleanbasicRootDir() + "/stdlib/" + stdSub + "/" + stdSub + ".cb";
        }
        const std::string dir = parentDirOf(currentFilePath);
        if (dir == ".") {
            return std::string(importTarget);
        }
        return normalizeRelPath(dir + "/" + std::string(importTarget));
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

inline void writeJsonrpcResult(
    const JsonArena& reqArena,
    JsonId reqId,
    JsonArena& outArena,
    JsonId resultId,
    std::ostream& out
) {
    const JsonId root = jsonAddObject(outArena);
    jsonObjectSet(outArena, root, "jsonrpc", jsonAddString(outArena, "2.0"));
    jsonObjectSet(outArena, root, "id", cloneJsonScalarOrNull(reqArena, reqId, outArena));
    jsonObjectSet(outArena, root, "result", resultId);
    writeJsonrpcMessage(out, serializeJson(outArena, root));
}

inline JsonId makeLspDocSymbol(
    JsonArena& arena,
    std::string_view name,
    std::string_view detail,
    int64_t kind,
    uint32_t startLine,
    uint32_t startCol,
    uint32_t endLine,
    uint32_t nameLine,
    uint32_t nameCol
) {
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

inline void publishDocumentDiagnostics(
    LspServerState& state,
    std::string_view uri,
    std::string_view text,
    std::ostream& out
) {
    const std::string filePath = uriToFilePath(uri);
    const auto openBuffers = buildOpenFileBuffers(state);
    const CheckSourceResult checkRes = checkSource(text, filePath, openBuffers);

    if (checkRes.ok) {
        state.snapshots[std::string(uri)] = LspDocumentSnapshot{true, checkRes.arena, checkRes.program, checkRes.symbols};
    }

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

inline void handleDocumentSymbolRequest(
    LspServerState& state,
    const JsonArena& reqArena,
    JsonId reqId,
    JsonId paramsId,
    std::ostream& out
) {
    const LspTextPositionParams doc = parseDocPositionParams(reqArena, paramsId);
    JsonArena outArena;
    const JsonId symbolsArr = jsonAddArray(outArena);

    if (!doc.uri.empty()) {
        const std::string normPath = normalizeRelPath(doc.filePath);
        const LspDocumentSnapshot snap = getOrBuildSnapshot(state, doc.uri);
        if (snap.valid) {
            for (const DeclId did : snap.program.declarations) {
                if (did >= snap.arena.decls.size()) {
                    continue;
                }
                const DeclNode& dn = snap.arena.decls[did];
                if (!declBelongsToFile(dn, doc.filePath, normPath)) {
                    continue;
                }
                if (std::holds_alternative<TypeDecl>(dn.payload)) {
                    const auto& td = std::get<TypeDecl>(dn.payload);
                    const JsonId symObj = makeLspDocSymbol(
                        outArena, td.name, "type", 23, dn.line, dn.col, dn.endLine, dn.nameLine, dn.nameCol
                    );
                    const JsonId childrenArr = jsonAddArray(outArena);
                    for (const auto& field : td.fields) {
                        jsonArrayPush(
                            outArena,
                            childrenArr,
                            makeLspDocSymbol(
                                outArena,
                                field.name,
                                formatCleanbasicType(snap.arena, field.typeId),
                                8,
                                field.line,
                                field.col,
                                field.line,
                                field.line,
                                field.col
                            )
                        );
                    }
                    jsonObjectSet(outArena, symObj, "children", childrenArr);
                    jsonArrayPush(outArena, symbolsArr, symObj);
                } else if (std::holds_alternative<FnDecl>(dn.payload)) {
                    const auto& fn = std::get<FnDecl>(dn.payload);
                    jsonArrayPush(
                        outArena,
                        symbolsArr,
                        makeLspDocSymbol(
                            outArena,
                            fn.name,
                            formatFnDetailSignature(snap.arena, fn.params, fn.returnType, fn.isFallible),
                            12,
                            dn.line,
                            dn.col,
                            dn.endLine,
                            dn.nameLine,
                            dn.nameCol
                        )
                    );
                } else if (std::holds_alternative<AliasDecl>(dn.payload)) {
                    const auto& al = std::get<AliasDecl>(dn.payload);
                    jsonArrayPush(
                        outArena,
                        symbolsArr,
                        makeLspDocSymbol(
                            outArena,
                            al.name,
                            "= " + formatCleanbasicType(snap.arena, al.targetType),
                            26,
                            dn.line,
                            dn.col,
                            dn.endLine,
                            dn.nameLine,
                            dn.nameCol
                        )
                    );
                }
            }
        }
    }

    writeJsonrpcResult(reqArena, reqId, outArena, symbolsArr, out);
}

struct DefinitionTarget {
    std::string fileName;
    uint32_t line = 1;
    uint32_t col = 1;
    size_t nameLen = 1;
};

struct ResolvedCursorSymbol {
    std::optional<std::string> hoverMarkdown;
    std::optional<DefinitionTarget> definition;
};

inline DefinitionTarget makeDefTarget(std::string_view fileName, uint32_t line, uint32_t col, size_t len) {
    return DefinitionTarget{std::string(fileName), line, col, len};
}

inline ResolvedCursorSymbol resolveCursorSymbolInfo(
    const LspDocumentSnapshot& snap,
    std::string_view filePath,
    const CursorTokenContext& cursorCtx
) {
    ResolvedCursorSymbol res{};
    if (!snap.valid || !cursorCtx.found) {
        return res;
    }

    const Token& tok = cursorCtx.token;
    const std::string currentModId = findModuleIdForFile(snap.arena, snap.program, filePath);

    if (cursorCtx.isImportTarget && tok.kind == "STRING") {
        const std::string rawTarget = stripQuotes(tok.value);
        if (rawTarget.size() > 3 && rawTarget.substr(rawTarget.size() - 3) == ".cb") {
            const std::string targetModId = resolveModuleId(currentModId, rawTarget);
            const auto modFileOpt = findModuleFilePath(snap.arena, snap.program, targetModId, filePath, rawTarget);
            if (modFileOpt.has_value()) {
                res.definition = makeDefTarget(modFileOpt.value(), 1, 1, 1);
            }
        }
        return res;
    }

    if (tok.kind != "IDENT" && tok.kind != "TYPE_KW" && tok.kind != "KEYWORD") {
        return res;
    }

    const auto fnKeyOpt = findEnclosingFnKey(snap.arena, snap.program, filePath, tok.line);
    std::unordered_map<std::string, VarSymbol> activeScope;
    if (fnKeyOpt.has_value()) {
        const auto scopeIt = snap.symbols.fnScopes.find(fnKeyOpt.value());
        if (scopeIt != snap.symbols.fnScopes.end()) {
            activeScope = scopeIt->second;
        }
    }

    const auto bindFn = [&](const FnSymbol& fn) {
        res.hoverMarkdown = formatFnHoverMarkdown(snap.arena, fn);
        if (!fn.fileName.empty()) {
            res.definition = makeDefTarget(fn.fileName, fn.line, fn.col, fn.name.size());
        }
    };
    const auto bindType = [&](const TypeSymbol& ts) {
        res.hoverMarkdown = formatTypeHoverMarkdown(snap.arena, ts);
        if (!ts.fileName.empty()) {
            res.definition = makeDefTarget(ts.fileName, ts.line, ts.col, ts.name.size());
        }
    };
    const auto bindAlias = [&](const AliasSymbol& al) {
        res.hoverMarkdown = formatAliasHoverMarkdown(snap.arena, snap.symbols, al);
        if (!al.fileName.empty()) {
            res.definition = makeDefTarget(al.fileName, al.line, al.col, al.name.size());
        }
    };

    if (cursorCtx.hasDotReceiver) {
        const std::string& recv = cursorCtx.receiverName;
        const std::string& member = tok.value;

        std::string targetModId;
        if (isModuleAliasInScope(snap.symbols, currentModId, recv, activeScope, targetModId)) {
            const std::string qkey = symbolKey(targetModId, member);
            const auto fnIt = snap.symbols.functions.find(qkey);
            if (fnIt != snap.symbols.functions.end()) {
                bindFn(fnIt->second);
                return res;
            }
            const auto tyIt = snap.symbols.types.find(qkey);
            if (tyIt != snap.symbols.types.end()) {
                bindType(tyIt->second);
                return res;
            }
            const auto alIt = snap.symbols.aliases.find(qkey);
            if (alIt != snap.symbols.aliases.end()) {
                bindAlias(alIt->second);
                return res;
            }
        }

        const auto varIt = activeScope.find(recv);
        if (varIt != activeScope.end()) {
            const TypeId recvTid = varIt->second.typeId;
            const auto tsOpt = resolveTypeSymbol(snap.arena, snap.symbols, currentModId, recvTid);
            if (tsOpt.has_value()) {
                const TypeSymbol& ts = tsOpt.value();
                const auto fit = ts.fields.find(member);
                if (fit != ts.fields.end()) {
                    const std::string ftype = formatCleanbasicType(snap.arena, fit->second);
                    res.hoverMarkdown = wrapVbCodeBlock("(field) " + ts.name + "." + member + " as " + ftype);
                    if (!ts.fileName.empty()) {
                        for (const auto& fd : ts.fieldOrder) {
                            if (fd.name == member) {
                                res.definition = makeDefTarget(ts.fileName, fd.line, fd.col, fd.name.size());
                                break;
                            }
                        }
                    }
                    return res;
                }
            }
            const auto ufcsFn = resolveUfcsFn(snap.arena, snap.symbols, currentModId, recvTid, member);
            if (ufcsFn.has_value()) {
                bindFn(ufcsFn.value());
                return res;
            }
        }
    }

    const auto localIt = activeScope.find(tok.value);
    if (localIt != activeScope.end()) {
        res.hoverMarkdown = formatVarHoverMarkdown(snap.arena, localIt->second);
        if (!localIt->second.fileName.empty()) {
            res.definition = makeDefTarget(
                localIt->second.fileName, localIt->second.line, localIt->second.col, localIt->second.name.size()
            );
        }
        return res;
    }

    const std::string symKey = resolveSymbolKey(snap.symbols, currentModId, tok.value);
    const auto fnIt = snap.symbols.functions.find(symKey);
    if (fnIt != snap.symbols.functions.end() && !isBuiltinSymbolName(tok.value)) {
        bindFn(fnIt->second);
        return res;
    }

    const auto alIt = snap.symbols.aliases.find(symKey);
    if (alIt != snap.symbols.aliases.end()) {
        bindAlias(alIt->second);
        return res;
    }

    const auto tyIt = snap.symbols.types.find(symKey);
    if (tyIt != snap.symbols.types.end() && !isBuiltinTypeName(tok.value)) {
        bindType(tyIt->second);
        return res;
    }

    for (const auto& kv : snap.symbols.types) {
        const auto fit = kv.second.fields.find(tok.value);
        if (fit != kv.second.fields.end()) {
            for (const auto& fd : kv.second.fieldOrder) {
                if (fd.name == tok.value && fd.line == tok.line) {
                    const std::string ftype = formatCleanbasicType(snap.arena, fit->second);
                    res.hoverMarkdown = wrapVbCodeBlock("(field) " + kv.second.name + "." + tok.value + " as " + ftype);
                    return res;
                }
            }
        }
    }

    std::string targetModId;
    if (isModuleAliasInScope(snap.symbols, currentModId, tok.value, activeScope, targetModId)) {
        const auto modFileOpt = findModuleFilePath(snap.arena, snap.program, targetModId, filePath);
        if (modFileOpt.has_value()) {
            res.definition = makeDefTarget(modFileOpt.value(), 1, 1, 1);
        }
    }

    const auto builtinMd = formatBuiltinHoverMarkdown(tok.value);
    if (builtinMd.has_value()) {
        res.hoverMarkdown = builtinMd;
    }

    return res;
}

inline void handleHoverRequest(
    LspServerState& state,
    const JsonArena& reqArena,
    JsonId reqId,
    JsonId paramsId,
    std::ostream& out
) {
    const LspTextPositionParams doc = parseDocPositionParams(reqArena, paramsId);
    JsonArena outArena;
    if (!doc.valid) {
        writeJsonrpcResult(reqArena, reqId, outArena, jsonAddNull(outArena), out);
        return;
    }

    const std::string sourceText = readDocumentText(state, doc.uri);
    const LspDocumentSnapshot snap = getOrBuildSnapshot(state, doc.uri);
    const CursorTokenContext cursorCtx = resolveCursorToken(sourceText, doc.filePath, doc.line1Based, doc.col1Based);
    const ResolvedCursorSymbol info = resolveCursorSymbolInfo(snap, doc.filePath, cursorCtx);
    if (!info.hoverMarkdown.has_value()) {
        writeJsonrpcResult(reqArena, reqId, outArena, jsonAddNull(outArena), out);
        return;
    }

    const JsonId contents = jsonAddObject(outArena);
    jsonObjectSet(outArena, contents, "kind", jsonAddString(outArena, "markdown"));
    jsonObjectSet(outArena, contents, "value", jsonAddString(outArena, info.hoverMarkdown.value()));

    const JsonId hoverObj = jsonAddObject(outArena);
    jsonObjectSet(outArena, hoverObj, "contents", contents);
    if (cursorCtx.found) {
        const uint32_t tlen = static_cast<uint32_t>(cursorCtx.token.value.size());
        jsonObjectSet(
            outArena,
            hoverObj,
            "range",
            makeLspRange(outArena, cursorCtx.token.line, cursorCtx.token.col, cursorCtx.token.line, cursorCtx.token.col + tlen)
        );
    }

    writeJsonrpcResult(reqArena, reqId, outArena, hoverObj, out);
}

inline void handleDefinitionRequest(
    LspServerState& state,
    const JsonArena& reqArena,
    JsonId reqId,
    JsonId paramsId,
    std::ostream& out
) {
    const LspTextPositionParams doc = parseDocPositionParams(reqArena, paramsId);
    JsonArena outArena;
    if (!doc.valid) {
        writeJsonrpcResult(reqArena, reqId, outArena, jsonAddNull(outArena), out);
        return;
    }

    const std::string sourceText = readDocumentText(state, doc.uri);
    const LspDocumentSnapshot snap = getOrBuildSnapshot(state, doc.uri);
    const CursorTokenContext cursorCtx = resolveCursorToken(sourceText, doc.filePath, doc.line1Based, doc.col1Based);
    const ResolvedCursorSymbol info = resolveCursorSymbolInfo(snap, doc.filePath, cursorCtx);
    if (!info.definition.has_value()) {
        writeJsonrpcResult(reqArena, reqId, outArena, jsonAddNull(outArena), out);
        return;
    }

    const DefinitionTarget& dt = info.definition.value();
    const JsonId locObj = makeLspLocation(outArena, dt.fileName, dt.line, dt.col, dt.nameLen);
    writeJsonrpcResult(reqArena, reqId, outArena, locObj, out);
}

struct CursorLineSlice {
    std::string fullLine;
    std::string prefixToCursor;
    size_t lineStartOffset = 0;
    size_t lineEndOffset = 0;
};

inline CursorLineSlice extractCursorLineSlice(
    std::string_view sourceText,
    uint32_t line0Based,
    uint32_t char0Based
) {
    CursorLineSlice slice{};
    uint32_t currLine = 0;
    size_t i = 0;
    while (i < sourceText.size() && currLine < line0Based) {
        if (sourceText[i] == '\n') {
            currLine += 1;
        }
        i += 1;
    }
    slice.lineStartOffset = i;
    size_t endI = i;
    while (endI < sourceText.size() && sourceText[endI] != '\n') {
        endI += 1;
    }
    slice.lineEndOffset = endI;
    std::string_view rawLine = sourceText.substr(i, endI - i);
    if (!rawLine.empty() && rawLine.back() == '\r') {
        rawLine.remove_suffix(1);
    }
    slice.fullLine = std::string(rawLine);
    size_t prefixLen = static_cast<size_t>(char0Based);
    if (prefixLen > slice.fullLine.size()) {
        prefixLen = slice.fullLine.size();
    }
    slice.prefixToCursor = slice.fullLine.substr(0, prefixLen);
    return slice;
}

inline std::string scrubIncompleteLineInSource(
    std::string_view sourceText,
    const CursorLineSlice& slice
) {
    std::string scrubbed(sourceText.substr(0, slice.lineStartOffset));
    const std::string trimmed = trimAsciiWs(slice.prefixToCursor);
    if (startsWithIgnoreCase(trimmed, "dim ") || startsWithIgnoreCase(trimmed, "const ")) {
        const size_t eqPos = slice.prefixToCursor.find('=');
        if (eqPos != std::string::npos) {
            scrubbed += slice.prefixToCursor.substr(0, eqPos + 1) + " 0";
        } else {
            scrubbed += "' scrubbed";
        }
    } else if (startsWithIgnoreCase(trimmed, "if ") || startsWithIgnoreCase(trimmed, "else if ")) {
        if (startsWithIgnoreCase(trimmed, "else if ")) {
            scrubbed += "else if true";
        } else {
            scrubbed += "if true";
        }
    } else if (startsWithIgnoreCase(trimmed, "return ")) {
        scrubbed += "return 0";
    } else {
        scrubbed += "' scrubbed";
    }
    scrubbed += std::string(sourceText.substr(slice.lineEndOffset));
    return scrubbed;
}

inline LspDocumentSnapshot getOrBuildScrubbedSnapshot(
    LspServerState& state,
    std::string_view uri,
    std::string_view sourceText,
    uint32_t line0Based,
    uint32_t char0Based
) {
    const std::string uriKey(uri);
    const std::string filePath = uriToFilePath(uri);
    const auto openBuffers = buildOpenFileBuffers(state);

    if (!sourceText.empty()) {
        const CheckSourceResult directRes = checkSource(sourceText, filePath, openBuffers);
        if (directRes.ok) {
            LspDocumentSnapshot snap{true, directRes.arena, directRes.program, directRes.symbols};
            state.snapshots[uriKey] = snap;
            return snap;
        }

        const CursorLineSlice slice = extractCursorLineSlice(sourceText, line0Based, char0Based);
        const std::string scrubbedText = scrubIncompleteLineInSource(sourceText, slice);
        auto scrubbedBuffers = openBuffers;
        scrubbedBuffers[filePath] = scrubbedText;
        const std::string normPath = normalizeRelPath(filePath);
        if (!normPath.empty()) {
            scrubbedBuffers[normPath] = scrubbedText;
        }

        const CheckSourceResult scrubbedRes = checkSource(scrubbedText, filePath, scrubbedBuffers);
        if (scrubbedRes.ok) {
            return LspDocumentSnapshot{true, scrubbedRes.arena, scrubbedRes.program, scrubbedRes.symbols};
        }

        if (startsWithIgnoreCase(trimAsciiWs(slice.prefixToCursor), "return ")) {
            std::string voidRetScrubbed(sourceText.substr(0, slice.lineStartOffset));
            voidRetScrubbed += "return";
            voidRetScrubbed += std::string(sourceText.substr(slice.lineEndOffset));
            scrubbedBuffers[filePath] = voidRetScrubbed;
            if (!normPath.empty()) {
                scrubbedBuffers[normPath] = voidRetScrubbed;
            }
            const CheckSourceResult voidRes = checkSource(voidRetScrubbed, filePath, scrubbedBuffers);
            if (voidRes.ok) {
                return LspDocumentSnapshot{true, voidRes.arena, voidRes.program, voidRes.symbols};
            }
        }
    }

    const auto snapIt = state.snapshots.find(uriKey);
    if (snapIt != state.snapshots.end() && snapIt->second.valid) {
        return snapIt->second;
    }
    LspDocumentSnapshot emptySnap{};
    emptySnap.valid = false;
    return emptySnap;
}

struct CompletionItemEntry {
    std::string label;
    int64_t kind = 1;
    std::string detail;
    std::string insertText;
    std::string documentation;
};

struct CallSiteContext {
    bool insideCall = false;
    bool hasReceiver = false;
    std::string receiverName;
    std::string calleeName;
    uint32_t activeParam = 0;
    bool atArgNamePosition = false;
    std::string argPrefix;
};

inline CallSiteContext parseEnclosingCallSite(std::string_view linePrefix) {
    CallSiteContext ctx{};
    std::string prefixStr(linePrefix);
    const LexResult lexRes = tokenize(prefixStr, "line.cb");
    std::vector<Token> toks;
    for (const auto& t : lexRes.tokens) {
        if (t.kind != "EOF" && t.kind != "NEWLINE") {
            toks.push_back(t);
        }
    }
    if (toks.empty()) {
        return ctx;
    }

    int parenDepth = 0;
    int bracketDepth = 0;
    int braceDepth = 0;
    size_t openParenIdx = toks.size();

    size_t idx = toks.size();
    while (idx > 0) {
        idx -= 1;
        const Token& t = toks[idx];
        if (t.kind == "RPAREN") {
            parenDepth += 1;
        } else if (t.kind == "LPAREN") {
            if (parenDepth > 0) {
                parenDepth -= 1;
            } else {
                openParenIdx = idx;
                break;
            }
        } else if (t.kind == "RBRACKET") {
            bracketDepth += 1;
        } else if (t.kind == "LBRACKET") {
            if (bracketDepth > 0) {
                bracketDepth -= 1;
            }
        } else if (t.kind == "RBRACE") {
            braceDepth += 1;
        } else if (t.kind == "LBRACE") {
            if (braceDepth > 0) {
                braceDepth -= 1;
            }
        }
    }

    if (openParenIdx == toks.size() || openParenIdx == 0) {
        return ctx;
    }

    const Token& calleeTok = toks[openParenIdx - 1];
    if (calleeTok.kind != "IDENT" && calleeTok.kind != "TYPE_KW" &&
        !(calleeTok.kind == "KEYWORD" && calleeTok.value == "error")) {
        return ctx;
    }

    ctx.insideCall = true;
    ctx.calleeName = calleeTok.value;
    if (openParenIdx >= 3 && toks[openParenIdx - 2].kind == "DOT") {
        const Token& recvTok = toks[openParenIdx - 3];
        if (recvTok.kind == "IDENT" || recvTok.kind == "TYPE_KW") {
            ctx.hasReceiver = true;
            ctx.receiverName = recvTok.value;
        }
    }

    int innerParen = 0;
    int innerBracket = 0;
    int innerBrace = 0;
    uint32_t commaCount = 0;
    size_t lastDelimiterIdx = openParenIdx;

    for (size_t k = openParenIdx + 1; k < toks.size(); ++k) {
        const Token& t = toks[k];
        if (t.kind == "LPAREN") {
            innerParen += 1;
        } else if (t.kind == "RPAREN") {
            if (innerParen > 0) {
                innerParen -= 1;
            }
        } else if (t.kind == "LBRACKET") {
            innerBracket += 1;
        } else if (t.kind == "RBRACKET") {
            if (innerBracket > 0) {
                innerBracket -= 1;
            }
        } else if (t.kind == "LBRACE") {
            innerBrace += 1;
        } else if (t.kind == "RBRACE") {
            if (innerBrace > 0) {
                innerBrace -= 1;
            }
        } else if (t.kind == "COMMA" && innerParen == 0 && innerBracket == 0 && innerBrace == 0) {
            commaCount += 1;
            lastDelimiterIdx = k;
        }
    }

    ctx.activeParam = commaCount;

    const size_t remTokens = toks.size() - (lastDelimiterIdx + 1);
    if (remTokens == 0) {
        ctx.atArgNamePosition = true;
        ctx.argPrefix = "";
    } else if (remTokens == 1) {
        const Token& lastTok = toks.back();
        const bool endsWithIdentChar = !linePrefix.empty() && isIdentChar(linePrefix.back());
        if (endsWithIdentChar && (lastTok.kind == "IDENT" || lastTok.kind == "TYPE_KW")) {
            ctx.atArgNamePosition = true;
            ctx.argPrefix = lastTok.value;
        }
    }

    return ctx;
}

struct CompletionCursorAnalysis {
    bool isDot = false;
    std::string dotReceiver;
    std::string prefix;
    bool isAfterAs = false;
    CallSiteContext callCtx;
};

inline CompletionCursorAnalysis analyzeCompletionCursor(std::string_view linePrefix) {
    CompletionCursorAnalysis res{};
    res.callCtx = parseEnclosingCallSite(linePrefix);

    size_t cursorIdx = linePrefix.size();
    size_t wordStart = cursorIdx;
    while (wordStart > 0 && isIdentChar(linePrefix[wordStart - 1])) {
        wordStart -= 1;
    }
    res.prefix = std::string(linePrefix.substr(wordStart, cursorIdx - wordStart));

    size_t beforeWord = wordStart;
    while (beforeWord > 0 && (linePrefix[beforeWord - 1] == ' ' || linePrefix[beforeWord - 1] == '\t')) {
        beforeWord -= 1;
    }

    if (wordStart > 0 && linePrefix[wordStart - 1] == '.') {
        size_t dotPos = wordStart - 1;
        size_t recvEnd = dotPos;
        while (recvEnd > 0 && (linePrefix[recvEnd - 1] == ' ' || linePrefix[recvEnd - 1] == '\t')) {
            recvEnd -= 1;
        }
        size_t recvStart = recvEnd;
        while (recvStart > 0 && isIdentChar(linePrefix[recvStart - 1])) {
            recvStart -= 1;
        }
        if (recvEnd > recvStart) {
            res.isDot = true;
            res.dotReceiver = std::string(linePrefix.substr(recvStart, recvEnd - recvStart));
            return res;
        }
    }

    if (beforeWord >= 2) {
        size_t kwEnd = beforeWord;
        size_t kwStart = kwEnd;
        while (kwStart > 0 && isIdentChar(linePrefix[kwStart - 1])) {
            kwStart -= 1;
        }
        const std::string_view prevWord = linePrefix.substr(kwStart, kwEnd - kwStart);
        if (prevWord == "as" && beforeWord < wordStart) {
            res.isAfterAs = true;
            return res;
        }
    }

    return res;
}

inline bool matchesCompletionPrefix(std::string_view candidate, std::string_view prefix) {
    if (prefix.empty()) {
        return true;
    }
    if (candidate.size() < prefix.size()) {
        return false;
    }
    return candidate.substr(0, prefix.size()) == prefix;
}

inline void addUniqueCompletionItem(
    std::vector<CompletionItemEntry>& items,
    std::unordered_map<std::string, bool>& seen,
    const CompletionItemEntry& entry
) {
    const std::string key = std::to_string(entry.kind) + ":" + entry.label + ":" + entry.insertText;
    if (seen.find(key) != seen.end()) {
        return;
    }
    seen[key] = true;
    items.push_back(entry);
}

inline void addCompletionEntry(
    std::vector<CompletionItemEntry>& items,
    std::unordered_map<std::string, bool>& seen,
    std::string_view label,
    int64_t kind,
    std::string_view detail,
    std::string_view insertText = "",
    std::string_view documentation = ""
) {
    std::string ins(label);
    if (!insertText.empty()) {
        ins = std::string(insertText);
    }
    addUniqueCompletionItem(
        items,
        seen,
        CompletionItemEntry{std::string(label), kind, std::string(detail), ins, std::string(documentation)}
    );
}

inline void collectModuleSymbolCompletions(
    const LspDocumentSnapshot& snap,
    std::string_view targetModuleId,
    std::string_view prefix,
    bool requirePub,
    bool includeFunctions,
    bool includeModuleAliases,
    std::vector<CompletionItemEntry>& items,
    std::unordered_map<std::string, bool>& seen
) {
    if (includeFunctions) {
        for (const auto& kv : snap.symbols.functions) {
            const FnSymbol& fn = kv.second;
            if (fn.moduleId != targetModuleId || (requirePub && !fn.isPub)) {
                continue;
            }
            if (!requirePub && isBuiltinSymbolName(fn.name)) {
                continue;
            }
            if (matchesCompletionPrefix(fn.name, prefix)) {
                addCompletionEntry(
                    items,
                    seen,
                    fn.name,
                    3,
                    "fn" + formatFnDetailSignature(snap.arena, fn.params, fn.returnType, fn.isFallible),
                    fn.name,
                    formatFnHoverMarkdown(snap.arena, fn)
                );
            }
        }
    }
    for (const auto& kv : snap.symbols.types) {
        const TypeSymbol& ts = kv.second;
        if (ts.moduleId != targetModuleId || (requirePub && !ts.isPub)) {
            continue;
        }
        if (!requirePub && isBuiltinTypeName(ts.name)) {
            continue;
        }
        if (matchesCompletionPrefix(ts.name, prefix)) {
            addCompletionEntry(items, seen, ts.name, 22, "type " + ts.name, ts.name, formatTypeHoverMarkdown(snap.arena, ts));
        }
    }
    for (const auto& kv : snap.symbols.aliases) {
        const AliasSymbol& al = kv.second;
        if (al.moduleId != targetModuleId || (requirePub && !al.isPub)) {
            continue;
        }
        if (matchesCompletionPrefix(al.name, prefix)) {
            addCompletionEntry(
                items,
                seen,
                al.name,
                25,
                "alias " + al.name + " = " + formatCleanbasicType(snap.arena, al.targetType),
                al.name,
                formatAliasHoverMarkdown(snap.arena, snap.symbols, al)
            );
        }
    }
    if (includeModuleAliases) {
        const auto modMapIt = snap.symbols.modules.find(std::string(targetModuleId));
        if (modMapIt != snap.symbols.modules.end()) {
            for (const auto& kv : modMapIt->second) {
                if (matchesCompletionPrefix(kv.first, prefix)) {
                    addCompletionEntry(items, seen, kv.first, 9, "module " + kv.second, kv.first);
                }
            }
        }
    }
}

inline std::optional<TypeId> resolveVariableTypeInFn(
    const LspDocumentSnapshot& snap,
    const std::optional<std::string>& enclosingFnKey,
    std::string_view varName
) {
    if (!enclosingFnKey.has_value()) {
        return std::nullopt;
    }
    const auto scopeIt = snap.symbols.fnScopes.find(enclosingFnKey.value());
    if (scopeIt == snap.symbols.fnScopes.end()) {
        return std::nullopt;
    }
    const auto varIt = scopeIt->second.find(std::string(varName));
    if (varIt == scopeIt->second.end()) {
        return std::nullopt;
    }
    return varIt->second.typeId;
}

inline std::string formatUfcsMethodDetail(
    const AstArena& arena,
    const FnSymbol& fn
) {
    std::vector<Param> remainingParams;
    for (size_t i = 1; i < fn.params.size(); ++i) {
        remainingParams.push_back(fn.params[i]);
    }
    return "ufcs fn " + fn.name + formatFnDetailSignature(arena, remainingParams, fn.returnType, fn.isFallible);
}

inline void collectUfcsCompletionsForType(
    const LspDocumentSnapshot& snap,
    std::string_view currentModuleId,
    TypeId recvTid,
    std::string_view prefix,
    std::vector<CompletionItemEntry>& items,
    std::unordered_map<std::string, bool>& seen
) {
    if (recvTid == INVALID_ID || recvTid >= snap.arena.types.size()) {
        return;
    }
    const TypeId resolvedRecv = resolveAlias(snap.arena, snap.symbols, currentModuleId, recvTid);
    if (resolvedRecv == INVALID_ID || resolvedRecv >= snap.arena.types.size()) {
        return;
    }
    const std::string& recvTypeKey = snap.arena.types[resolvedRecv].name;
    const auto [recvModId, recvBase] = splitSymbolKey(recvTypeKey);

    std::unordered_map<std::string, bool> allowedImportedMods;
    const auto modMapIt = snap.symbols.modules.find(std::string(currentModuleId));
    if (modMapIt != snap.symbols.modules.end()) {
        for (const auto& kv : modMapIt->second) {
            allowedImportedMods[kv.second] = true;
        }
    }

    for (const auto& kv : snap.symbols.functions) {
        const FnSymbol& fn = kv.second;
        if (fn.params.empty() || !matchesCompletionPrefix(fn.name, prefix)) {
            continue;
        }
        const bool inCurrentMod = (fn.moduleId == currentModuleId);
        const bool inDefiningMod = (!recvModId.empty() && fn.moduleId == recvModId && fn.isPub);
        const bool inImportedMod = (allowedImportedMods.find(fn.moduleId) != allowedImportedMods.end() && fn.isPub);
        if (!inCurrentMod && !inDefiningMod && !inImportedMod) {
            continue;
        }

        const TypeId p0Resolved = resolveAlias(snap.arena, snap.symbols, fn.moduleId, fn.params[0].typeId);
        if (p0Resolved == INVALID_ID || p0Resolved >= snap.arena.types.size()) {
            continue;
        }
        if (snap.arena.types[p0Resolved].name != recvTypeKey) {
            continue;
        }

        addCompletionEntry(
            items,
            seen,
            fn.name,
            2,
            formatUfcsMethodDetail(snap.arena, fn),
            fn.name,
            formatFnHoverMarkdown(snap.arena, fn)
        );
    }
}

inline void collectDotCompletions(
    const LspDocumentSnapshot& snap,
    std::string_view currentModuleId,
    const std::optional<std::string>& enclosingFnKey,
    std::string_view receiverName,
    std::string_view prefix,
    std::vector<CompletionItemEntry>& items,
    std::unordered_map<std::string, bool>& seen
) {
    const auto varTidOpt = resolveVariableTypeInFn(snap, enclosingFnKey, receiverName);

    // 1. Module alias completion (if not shadowed by local variable)
    if (!varTidOpt.has_value()) {
        const auto modMapIt = snap.symbols.modules.find(std::string(currentModuleId));
        if (modMapIt != snap.symbols.modules.end()) {
            const auto aliasIt = modMapIt->second.find(std::string(receiverName));
            if (aliasIt != modMapIt->second.end()) {
                collectModuleSymbolCompletions(snap, aliasIt->second, prefix, true, true, false, items, seen);
                return;
            }
        }
        return;
    }

    const TypeId recvTid = resolveAlias(snap.arena, snap.symbols, currentModuleId, varTidOpt.value());
    if (recvTid == INVALID_ID || recvTid >= snap.arena.types.size()) {
        return;
    }

    const TypeNode& recvNode = snap.arena.types[recvTid];

    // 2. Built-in 'err' fields (.msg, .code)
    if (recvTid == snap.symbols.typeErr || recvNode.name == "err") {
        if (matchesCompletionPrefix("msg", prefix)) {
            addCompletionEntry(items, seen, "msg", 5, "as str");
        }
        if (matchesCompletionPrefix("code", prefix)) {
            addCompletionEntry(items, seen, "code", 5, "as int");
        }
        collectUfcsCompletionsForType(snap, currentModuleId, recvTid, prefix, items, seen);
        return;
    }

    // 3. Built-in 'list[T]' methods (.push, .append)
    if (recvNode.name == "list") {
        std::string elemType = "any";
        if (!recvNode.typeArgs.empty()) {
            elemType = formatCleanbasicType(snap.arena, recvNode.typeArgs[0]);
        }
        if (matchesCompletionPrefix("push", prefix)) {
            addCompletionEntry(items, seen, "push", 2, "fn(item as " + elemType + ")");
        }
        if (matchesCompletionPrefix("append", prefix)) {
            addCompletionEntry(items, seen, "append", 2, "fn(item as " + elemType + ")");
        }
        collectUfcsCompletionsForType(snap, currentModuleId, recvTid, prefix, items, seen);
        return;
    }

    // 4. User Struct fields + UFCS methods
    const auto tsOpt = resolveTypeSymbol(snap.arena, snap.symbols, currentModuleId, recvTid);
    if (tsOpt.has_value()) {
        const TypeSymbol& ts = tsOpt.value();
        for (const auto& f : ts.fieldOrder) {
            if (!matchesCompletionPrefix(f.name, prefix)) {
                continue;
            }
            TypeId ftid = f.typeId;
            const auto fit = ts.fields.find(f.name);
            if (fit != ts.fields.end()) {
                ftid = fit->second;
            }
            addCompletionEntry(items, seen, f.name, 5, "as " + formatCleanbasicType(snap.arena, ftid));
        }
    }

    collectUfcsCompletionsForType(snap, currentModuleId, recvTid, prefix, items, seen);
}

inline std::optional<TypeSymbol> resolveCallStructSymbol(
    const LspDocumentSnapshot& snap,
    std::string_view currentModuleId,
    const std::optional<std::string>& enclosingFnKey,
    const CallSiteContext& callCtx
) {
    std::string lookupName = callCtx.calleeName;
    if (callCtx.hasReceiver) {
        if (resolveVariableTypeInFn(snap, enclosingFnKey, callCtx.receiverName).has_value()) {
            return std::nullopt;
        }
        lookupName = callCtx.receiverName + "." + callCtx.calleeName;
    }
    return resolveTypeSymbolByName(snap.arena, snap.symbols, currentModuleId, lookupName, callCtx.hasReceiver);
}

struct ResolvedCallFunction {
    FnSymbol fn;
    bool isUfcs = false;
};

inline std::optional<ResolvedCallFunction> resolveCallFnSymbol(
    const LspDocumentSnapshot& snap,
    std::string_view currentModuleId,
    const std::optional<std::string>& enclosingFnKey,
    const CallSiteContext& callCtx
) {
    if (callCtx.hasReceiver) {
        const auto varTidOpt = resolveVariableTypeInFn(snap, enclosingFnKey, callCtx.receiverName);
        if (!varTidOpt.has_value()) {
            const auto modMapIt = snap.symbols.modules.find(std::string(currentModuleId));
            if (modMapIt != snap.symbols.modules.end()) {
                const auto aliasIt = modMapIt->second.find(callCtx.receiverName);
                if (aliasIt != modMapIt->second.end()) {
                    const std::string fnKey = symbolKey(aliasIt->second, callCtx.calleeName);
                    const auto fnIt = snap.symbols.functions.find(fnKey);
                    if (fnIt != snap.symbols.functions.end() && fnIt->second.isPub) {
                        return ResolvedCallFunction{fnIt->second, false};
                    }
                }
            }
        } else {
            const auto ufcsOpt = resolveUfcsFn(snap.arena, snap.symbols, currentModuleId, varTidOpt.value(), callCtx.calleeName);
            if (ufcsOpt.has_value()) {
                return ResolvedCallFunction{ufcsOpt.value(), true};
            }
        }
        return std::nullopt;
    }

    const std::string fnKey = resolveSymbolKey(snap.symbols, currentModuleId, callCtx.calleeName);
    const auto fnIt = snap.symbols.functions.find(fnKey);
    if (fnIt != snap.symbols.functions.end()) {
        return ResolvedCallFunction{fnIt->second, false};
    }
    return std::nullopt;
}

inline void collectNamedArgumentCompletions(
    const LspDocumentSnapshot& snap,
    std::string_view currentModuleId,
    const std::optional<std::string>& enclosingFnKey,
    const CallSiteContext& callCtx,
    std::vector<CompletionItemEntry>& items,
    std::unordered_map<std::string, bool>& seen
) {
    if (!callCtx.insideCall || !callCtx.atArgNamePosition) {
        return;
    }

    if (!callCtx.hasReceiver && callCtx.calleeName == "error") {
        if (matchesCompletionPrefix("code", callCtx.argPrefix)) {
            addCompletionEntry(items, seen, "code=", 6, "as int");
        }
        return;
    }

    const auto tsOpt = resolveCallStructSymbol(snap, currentModuleId, enclosingFnKey, callCtx);
    if (tsOpt.has_value()) {
        const TypeSymbol& ts = tsOpt.value();
        for (const auto& f : ts.fieldOrder) {
            if (!matchesCompletionPrefix(f.name, callCtx.argPrefix)) {
                continue;
            }
            TypeId ftid = f.typeId;
            const auto fit = ts.fields.find(f.name);
            if (fit != ts.fields.end()) {
                ftid = fit->second;
            }
            addCompletionEntry(items, seen, f.name + "=", 5, "as " + formatCleanbasicType(snap.arena, ftid));
        }
        return;
    }

    const auto fnOpt = resolveCallFnSymbol(snap, currentModuleId, enclosingFnKey, callCtx);
    if (fnOpt.has_value()) {
        const FnSymbol& fn = fnOpt.value().fn;
        size_t startIdx = 0;
        if (fnOpt.value().isUfcs && !fn.params.empty()) {
            startIdx = 1;
        }
        for (size_t i = startIdx; i < fn.params.size(); ++i) {
            const Param& p = fn.params[i];
            if (matchesCompletionPrefix(p.name, callCtx.argPrefix)) {
                addCompletionEntry(items, seen, p.name + "=", 6, "as " + formatCleanbasicType(snap.arena, p.typeId));
            }
        }
    }
}

inline void collectTypeAnnotationCompletions(
    const LspDocumentSnapshot& snap,
    std::string_view currentModuleId,
    std::string_view prefix,
    std::vector<CompletionItemEntry>& items,
    std::unordered_map<std::string, bool>& seen
) {
    const std::vector<std::string> builtinTypes = {
        "int", "float", "str", "bool", "cptr", "err", "list", "dict"
    };
    for (const auto& bt : builtinTypes) {
        if (matchesCompletionPrefix(bt, prefix)) {
            addCompletionEntry(items, seen, bt, 14, "built-in type");
        }
    }
    if (snap.valid) {
        collectModuleSymbolCompletions(snap, currentModuleId, prefix, false, false, true, items, seen);
    }
}

inline void collectGeneralScopeCompletions(
    const LspDocumentSnapshot& snap,
    std::string_view currentModuleId,
    const std::optional<std::string>& enclosingFnKey,
    std::string_view prefix,
    std::vector<CompletionItemEntry>& items,
    std::unordered_map<std::string, bool>& seen
) {
    if (snap.valid) {
        if (enclosingFnKey.has_value()) {
            const auto scopeIt = snap.symbols.fnScopes.find(enclosingFnKey.value());
            if (scopeIt != snap.symbols.fnScopes.end()) {
                for (const auto& kv : scopeIt->second) {
                    const VarSymbol& vs = kv.second;
                    if (matchesCompletionPrefix(vs.name, prefix)) {
                        addCompletionEntry(
                            items,
                            seen,
                            vs.name,
                            6,
                            "as " + formatCleanbasicType(snap.arena, vs.typeId),
                            vs.name,
                            formatVarHoverMarkdown(snap.arena, vs)
                        );
                    }
                }
            }
        }
        collectModuleSymbolCompletions(snap, currentModuleId, prefix, false, true, true, items, seen);
    }

    const std::vector<std::pair<std::string, std::string>> builtins = {
        {"print", "fn print(values...)"},
        {"len", "fn len(container as any) as int"},
        {"str", "fn str(val as any) as str"},
        {"error", "fn error(msg as str, [code=1]) as err"}
    };
    for (const auto& b : builtins) {
        if (matchesCompletionPrefix(b.first, prefix)) {
            std::string doc;
            const auto docOpt = formatBuiltinHoverMarkdown(b.first);
            if (docOpt.has_value()) {
                doc = docOpt.value();
            }
            addCompletionEntry(items, seen, b.first, 3, b.second, b.first, doc);
        }
    }

    const std::vector<std::string> keywords = {
        "fn", "end fn", "pub", "extern", "type", "end type", "alias", "import",
        "import header", "import link", "dim", "const", "byref", "as",
        "if", "else if", "else", "end if", "for", "to", "step", "in", "end for",
        "select case", "case", "case else", "end select", "on error", "end error",
        "defer", "end defer", "return", "break", "can error", "and", "or", "not",
        "true", "false", "null"
    };
    for (const auto& kw : keywords) {
        if (matchesCompletionPrefix(kw, prefix)) {
            addCompletionEntry(items, seen, kw, 14, "keyword");
        }
    }
}

inline void handleCompletionRequest(
    LspServerState& state,
    const JsonArena& reqArena,
    JsonId reqId,
    JsonId paramsId,
    std::ostream& out
) {
    const LspTextPositionParams doc = parseDocPositionParams(reqArena, paramsId);
    JsonArena outArena;
    const JsonId itemsArr = jsonAddArray(outArena);

    if (!doc.valid) {
        writeJsonrpcResult(reqArena, reqId, outArena, itemsArr, out);
        return;
    }

    const std::string sourceText = readDocumentText(state, doc.uri);
    const LspDocumentSnapshot snap = getOrBuildScrubbedSnapshot(state, doc.uri, sourceText, doc.line0Based, doc.char0Based);
    const CursorLineSlice slice = extractCursorLineSlice(sourceText, doc.line0Based, doc.char0Based);
    const CompletionCursorAnalysis cursorInfo = analyzeCompletionCursor(slice.prefixToCursor);

    std::string currentModId;
    std::optional<std::string> enclosingFnKey;
    if (snap.valid) {
        currentModId = findModuleIdForFile(snap.arena, snap.program, doc.filePath);
        enclosingFnKey = findEnclosingFnKey(snap.arena, snap.program, doc.filePath, doc.line1Based);
    }

    std::vector<CompletionItemEntry> entries;
    std::unordered_map<std::string, bool> seen;

    if (cursorInfo.isDot) {
        if (snap.valid) {
            collectDotCompletions(
                snap, currentModId, enclosingFnKey, cursorInfo.dotReceiver, cursorInfo.prefix, entries, seen
            );
        }
    } else if (cursorInfo.isAfterAs) {
        collectTypeAnnotationCompletions(snap, currentModId, cursorInfo.prefix, entries, seen);
    } else {
        if (snap.valid && cursorInfo.callCtx.insideCall && cursorInfo.callCtx.atArgNamePosition) {
            collectNamedArgumentCompletions(snap, currentModId, enclosingFnKey, cursorInfo.callCtx, entries, seen);
        }
        collectGeneralScopeCompletions(snap, currentModId, enclosingFnKey, cursorInfo.prefix, entries, seen);
    }

    for (const auto& entry : entries) {
        const JsonId itemObj = jsonAddObject(outArena);
        jsonObjectSet(outArena, itemObj, "label", jsonAddString(outArena, entry.label));
        jsonObjectSet(outArena, itemObj, "kind", jsonAddInt(outArena, entry.kind));
        if (!entry.detail.empty()) {
            jsonObjectSet(outArena, itemObj, "detail", jsonAddString(outArena, entry.detail));
        }
        if (!entry.insertText.empty()) {
            jsonObjectSet(outArena, itemObj, "insertText", jsonAddString(outArena, entry.insertText));
        }
        if (!entry.documentation.empty()) {
            const JsonId docObj = jsonAddObject(outArena);
            jsonObjectSet(outArena, docObj, "kind", jsonAddString(outArena, "markdown"));
            jsonObjectSet(outArena, docObj, "value", jsonAddString(outArena, entry.documentation));
            jsonObjectSet(outArena, itemObj, "documentation", docObj);
        }
        jsonArrayPush(outArena, itemsArr, itemObj);
    }

    writeJsonrpcResult(reqArena, reqId, outArena, itemsArr, out);
}

struct SignatureHelpData {
    std::string label;
    std::string documentation;
    std::vector<std::string> paramLabels;
    uint32_t activeParameter = 0;
};

inline std::optional<SignatureHelpData> resolveSignatureHelp(
    const LspDocumentSnapshot& snap,
    std::string_view currentModuleId,
    const std::optional<std::string>& enclosingFnKey,
    const CallSiteContext& callCtx
) {
    if (!callCtx.insideCall) {
        return std::nullopt;
    }

    if (!callCtx.hasReceiver) {
        if (callCtx.calleeName == "error") {
            uint32_t act = callCtx.activeParam;
            if (act > 1) {
                act = 1;
            }
            std::string doc;
            const auto docOpt = formatBuiltinHoverMarkdown("error");
            if (docOpt.has_value()) {
                doc = docOpt.value();
            }
            return SignatureHelpData{"fn error(msg as str, code as int) as err", doc, {"msg as str", "code as int"}, act};
        }
        if (callCtx.calleeName == "len") {
            return SignatureHelpData{"fn len(container as any) as int", "", {"container as any"}, 0};
        }
        if (callCtx.calleeName == "str") {
            return SignatureHelpData{"fn str(val as any) as str", "", {"val as any"}, 0};
        }
        if (callCtx.calleeName == "print") {
            return SignatureHelpData{"fn print(values...)", "", {"values..."}, 0};
        }
    }

    if (!snap.valid) {
        return std::nullopt;
    }

    // 3. Struct constructor
    const auto tsOpt = resolveCallStructSymbol(snap, currentModuleId, enclosingFnKey, callCtx);
    if (tsOpt.has_value()) {
        const TypeSymbol& ts = tsOpt.value();
        SignatureHelpData sig{};
        sig.label = ts.name + "(";
        for (size_t i = 0; i < ts.fieldOrder.size(); ++i) {
            if (i > 0) {
                sig.label += ", ";
            }
            const FieldDecl& f = ts.fieldOrder[i];
            TypeId ftid = f.typeId;
            const auto fit = ts.fields.find(f.name);
            if (fit != ts.fields.end()) {
                ftid = fit->second;
            }
            const std::string pLabel = f.name + " as " + formatCleanbasicType(snap.arena, ftid);
            sig.label += pLabel;
            sig.paramLabels.push_back(pLabel);
        }
        sig.label += ") as " + ts.name;
        sig.documentation = formatTypeHoverMarkdown(snap.arena, ts);
        sig.activeParameter = callCtx.activeParam;
        if (!sig.paramLabels.empty() && sig.activeParameter >= sig.paramLabels.size()) {
            sig.activeParameter = static_cast<uint32_t>(sig.paramLabels.size() - 1);
        }
        return sig;
    }

    // 4. Regular function or UFCS function call
    const auto fnOpt = resolveCallFnSymbol(snap, currentModuleId, enclosingFnKey, callCtx);
    if (fnOpt.has_value()) {
        const FnSymbol& fn = fnOpt.value().fn;
        const bool isUfcs = fnOpt.value().isUfcs;

        std::vector<Param> visibleParams;
        size_t startIdx = 0;
        if (isUfcs && !fn.params.empty()) {
            startIdx = 1;
        }
        for (size_t i = startIdx; i < fn.params.size(); ++i) {
            visibleParams.push_back(fn.params[i]);
        }

        SignatureHelpData sig{};
        sig.label = "fn " + fn.name + formatFnDetailSignature(snap.arena, visibleParams, fn.returnType, fn.isFallible);
        sig.documentation = formatFnHoverMarkdown(snap.arena, fn);
        for (const auto& p : visibleParams) {
            std::string pLabel;
            if (p.isByref) {
                pLabel += "byref ";
            }
            pLabel += p.name + " as " + formatCleanbasicType(snap.arena, p.typeId);
            sig.paramLabels.push_back(pLabel);
        }
        sig.activeParameter = callCtx.activeParam;
        if (!sig.paramLabels.empty() && sig.activeParameter >= sig.paramLabels.size()) {
            sig.activeParameter = static_cast<uint32_t>(sig.paramLabels.size() - 1);
        }
        return sig;
    }

    return std::nullopt;
}

inline void handleSignatureHelpRequest(
    LspServerState& state,
    const JsonArena& reqArena,
    JsonId reqId,
    JsonId paramsId,
    std::ostream& out
) {
    const LspTextPositionParams doc = parseDocPositionParams(reqArena, paramsId);
    JsonArena outArena;
    if (!doc.valid) {
        writeJsonrpcResult(reqArena, reqId, outArena, jsonAddNull(outArena), out);
        return;
    }

    const std::string sourceText = readDocumentText(state, doc.uri);
    const LspDocumentSnapshot snap = getOrBuildScrubbedSnapshot(state, doc.uri, sourceText, doc.line0Based, doc.char0Based);
    const CursorLineSlice slice = extractCursorLineSlice(sourceText, doc.line0Based, doc.char0Based);
    const CallSiteContext callCtx = parseEnclosingCallSite(slice.prefixToCursor);

    std::string currentModId;
    std::optional<std::string> enclosingFnKey;
    if (snap.valid) {
        currentModId = findModuleIdForFile(snap.arena, snap.program, doc.filePath);
        enclosingFnKey = findEnclosingFnKey(snap.arena, snap.program, doc.filePath, doc.line1Based);
    }

    const auto sigOpt = resolveSignatureHelp(snap, currentModId, enclosingFnKey, callCtx);
    if (!sigOpt.has_value()) {
        writeJsonrpcResult(reqArena, reqId, outArena, jsonAddNull(outArena), out);
        return;
    }

    const SignatureHelpData& sig = sigOpt.value();
    const JsonId sigInfo = jsonAddObject(outArena);
    jsonObjectSet(outArena, sigInfo, "label", jsonAddString(outArena, sig.label));
    if (!sig.documentation.empty()) {
        const JsonId docObj = jsonAddObject(outArena);
        jsonObjectSet(outArena, docObj, "kind", jsonAddString(outArena, "markdown"));
        jsonObjectSet(outArena, docObj, "value", jsonAddString(outArena, sig.documentation));
        jsonObjectSet(outArena, sigInfo, "documentation", docObj);
    }

    const JsonId paramsArr = jsonAddArray(outArena);
    for (const auto& pLabel : sig.paramLabels) {
        const JsonId pObj = jsonAddObject(outArena);
        jsonObjectSet(outArena, pObj, "label", jsonAddString(outArena, pLabel));
        jsonArrayPush(outArena, paramsArr, pObj);
    }
    jsonObjectSet(outArena, sigInfo, "parameters", paramsArr);

    const JsonId sigsArr = jsonAddArray(outArena);
    jsonArrayPush(outArena, sigsArr, sigInfo);

    const JsonId resultObj = jsonAddObject(outArena);
    jsonObjectSet(outArena, resultObj, "signatures", sigsArr);
    jsonObjectSet(outArena, resultObj, "activeSignature", jsonAddInt(outArena, 0));
    jsonObjectSet(outArena, resultObj, "activeParameter", jsonAddInt(outArena, static_cast<int64_t>(sig.activeParameter)));

    writeJsonrpcResult(reqArena, reqId, outArena, resultObj, out);
}

inline void handleLspMessage(LspServerState& state, std::string_view payload, std::ostream& out) {
    const JsonParseResult parsed = parseJson(payload);
    if (!parsed.ok) {
        return;
    }

    const JsonId methodId = jsonGetField(parsed.arena, parsed.root, "method");
    const auto methodOpt = jsonGetString(parsed.arena, methodId);
    if (!methodOpt.has_value()) {
        return;
    }

    const std::string& method = methodOpt.value();
    const JsonId reqId = jsonGetField(parsed.arena, parsed.root, "id");
    const JsonId paramsId = jsonGetField(parsed.arena, parsed.root, "params");

    if (method == "initialize") {
        state.initialized = true;
        JsonArena outArena;

        const JsonId caps = jsonAddObject(outArena);
        jsonObjectSet(outArena, caps, "textDocumentSync", jsonAddInt(outArena, 1));
        jsonObjectSet(outArena, caps, "documentSymbolProvider", jsonAddBool(outArena, true));
        jsonObjectSet(outArena, caps, "definitionProvider", jsonAddBool(outArena, true));
        jsonObjectSet(outArena, caps, "hoverProvider", jsonAddBool(outArena, true));

        const JsonId compProvider = jsonAddObject(outArena);
        const JsonId compTriggers = jsonAddArray(outArena);
        jsonArrayPush(outArena, compTriggers, jsonAddString(outArena, "."));
        jsonObjectSet(outArena, compProvider, "triggerCharacters", compTriggers);
        jsonObjectSet(outArena, caps, "completionProvider", compProvider);

        const JsonId sigProvider = jsonAddObject(outArena);
        const JsonId sigTriggers = jsonAddArray(outArena);
        jsonArrayPush(outArena, sigTriggers, jsonAddString(outArena, "("));
        jsonArrayPush(outArena, sigTriggers, jsonAddString(outArena, ","));
        jsonObjectSet(outArena, sigProvider, "triggerCharacters", sigTriggers);
        jsonObjectSet(outArena, caps, "signatureHelpProvider", sigProvider);

        const JsonId serverInfo = jsonAddObject(outArena);
        jsonObjectSet(outArena, serverInfo, "name", jsonAddString(outArena, "cleanbasic-lsp"));
        jsonObjectSet(outArena, serverInfo, "version", jsonAddString(outArena, "0.3.0"));

        const JsonId result = jsonAddObject(outArena);
        jsonObjectSet(outArena, result, "capabilities", caps);
        jsonObjectSet(outArena, result, "serverInfo", serverInfo);

        writeJsonrpcResult(parsed.arena, reqId, outArena, result, out);
        return;
    }

    if (method == "initialized") {
        state.initialized = true;
        return;
    }

    if (method == "shutdown") {
        state.shutdownRequested = true;
        JsonArena outArena;
        writeJsonrpcResult(parsed.arena, reqId, outArena, jsonAddNull(outArena), out);
        return;
    }

    if (method == "exit") {
        state.shouldExit = true;
        if (state.shutdownRequested) {
            state.exitCode = 0;
        } else {
            state.exitCode = 1;
        }
        return;
    }

    if (method == "textDocument/didOpen") {
        const JsonId textDoc = jsonGetField(parsed.arena, paramsId, "textDocument");
        const auto uriOpt = jsonGetString(parsed.arena, jsonGetField(parsed.arena, textDoc, "uri"));
        const auto textOpt = jsonGetString(parsed.arena, jsonGetField(parsed.arena, textDoc, "text"));
        if (uriOpt.has_value() && textOpt.has_value()) {
            state.openDocuments[uriOpt.value()] = textOpt.value();
            publishDocumentDiagnostics(state, uriOpt.value(), textOpt.value(), out);
        }
        return;
    }

    if (method == "textDocument/didChange") {
        const JsonId textDoc = jsonGetField(parsed.arena, paramsId, "textDocument");
        const auto uriOpt = jsonGetString(parsed.arena, jsonGetField(parsed.arena, textDoc, "uri"));
        const JsonId changesId = jsonGetField(parsed.arena, paramsId, "contentChanges");
        if (uriOpt.has_value() && changesId < parsed.arena.nodes.size() &&
            std::holds_alternative<JsonArray>(parsed.arena.nodes[changesId])) {
            const auto& arr = std::get<JsonArray>(parsed.arena.nodes[changesId]);
            bool updated = false;
            std::string latestText;
            for (const JsonId elemId : arr.elements) {
                const auto textOpt = jsonGetString(parsed.arena, jsonGetField(parsed.arena, elemId, "text"));
                if (textOpt.has_value()) {
                    latestText = textOpt.value();
                    updated = true;
                }
            }
            if (updated) {
                state.openDocuments[uriOpt.value()] = latestText;
                publishDocumentDiagnostics(state, uriOpt.value(), latestText, out);
            }
        }
        return;
    }

    if (method == "textDocument/didClose") {
        const JsonId textDoc = jsonGetField(parsed.arena, paramsId, "textDocument");
        const auto uriOpt = jsonGetString(parsed.arena, jsonGetField(parsed.arena, textDoc, "uri"));
        if (uriOpt.has_value()) {
            state.openDocuments.erase(uriOpt.value());
            state.snapshots.erase(uriOpt.value());
            publishEmptyDiagnostics(uriOpt.value(), out);
        }
        return;
    }

    if (method == "textDocument/documentSymbol") {
        handleDocumentSymbolRequest(state, parsed.arena, reqId, paramsId, out);
        return;
    }

    if (method == "textDocument/hover") {
        handleHoverRequest(state, parsed.arena, reqId, paramsId, out);
        return;
    }

    if (method == "textDocument/definition") {
        handleDefinitionRequest(state, parsed.arena, reqId, paramsId, out);
        return;
    }

    if (method == "textDocument/completion") {
        handleCompletionRequest(state, parsed.arena, reqId, paramsId, out);
        return;
    }

    if (method == "textDocument/signatureHelp") {
        handleSignatureHelpRequest(state, parsed.arena, reqId, paramsId, out);
        return;
    }

    if (reqId != JSON_INVALID_ID) {
        JsonArena outArena;
        const JsonId root = jsonAddObject(outArena);
        jsonObjectSet(outArena, root, "jsonrpc", jsonAddString(outArena, "2.0"));
        jsonObjectSet(outArena, root, "id", cloneJsonScalarOrNull(parsed.arena, reqId, outArena));

        const JsonId errObj = jsonAddObject(outArena);
        jsonObjectSet(outArena, errObj, "code", jsonAddInt(outArena, -32601));
        jsonObjectSet(outArena, errObj, "message", jsonAddString(outArena, "Method not found: " + method));
        jsonObjectSet(outArena, root, "error", errObj);

        writeJsonrpcMessage(out, serializeJson(outArena, root));
    }
}

inline int runLspServer(std::istream& in, std::ostream& out) {
    LspServerState state{};
    while (!state.shouldExit) {
        const auto msgOpt = readJsonrpcMessage(in);
        if (!msgOpt.has_value()) {
            break;
        }
        handleLspMessage(state, msgOpt.value(), out);
    }
    return state.exitCode;
}

}  // namespace cb

#endif  // CLEANBASIC_LSP_HPP
