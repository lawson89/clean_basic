
#ifndef CLEANBASIC_LSP_NAVIGATION_HPP
#define CLEANBASIC_LSP_NAVIGATION_HPP

#include "src/cleanbasic/lsp_protocol.hpp"

namespace cb {

inline void handleDocumentSymbolRequest(LspServerState& state, const JsonArena& reqArena, JsonId reqId, JsonId paramsId, std::ostream& out) {
    const LspTextPositionParams doc = parseDocPositionParams(reqArena, paramsId);
    JsonArena outArena;
    const JsonId symbolsArr = jsonAddArray(outArena);
    if (!doc.uri.empty()) {
        const std::string normPath = normalizeRelPath(doc.filePath);
        const LspDocumentSnapshot snap = getOrBuildSnapshot(state, doc.uri);
        if (snap.valid) {
            for (const DeclId did : snap.program.declarations) {
                if (did >= snap.arena.decls.size()) continue;
                const DeclNode& dn = snap.arena.decls[did];
                if (!declBelongsToFile(dn, doc.filePath, normPath)) continue;
                if (const auto* td = snap.arena.getDecl<TypeDecl>(did)) {
                    const JsonId symObj = makeLspDocSymbol(outArena, td->name, "type", 23, dn.line, dn.col, dn.endLine, dn.nameLine, dn.nameCol);
                    const JsonId childrenArr = jsonAddArray(outArena);
                    for (const auto& field : td->fields) {
                        jsonArrayPush(outArena, childrenArr, makeLspDocSymbol(outArena, field.name, formatCleanbasicType(snap.arena, field.typeId), 8, field.line, field.col, field.line, field.line, field.col));
                    }
                    jsonObjectSet(outArena, symObj, "children", childrenArr);
                    jsonArrayPush(outArena, symbolsArr, symObj);
                } else if (const auto* fn = snap.arena.getDecl<FnDecl>(did)) {
                    jsonArrayPush(outArena, symbolsArr, makeLspDocSymbol(outArena, fn->name, formatFnDetailSignature(snap.arena, fn->params, fn->returnType, fn->isFallible), 12, dn.line, dn.col, dn.endLine, dn.nameLine, dn.nameCol));
                } else if (const auto* ed = snap.arena.getDecl<EnumDecl>(did)) {
                    const JsonId symObj = makeLspDocSymbol(outArena, ed->name, "enum", 10, dn.line, dn.col, dn.endLine, dn.nameLine, dn.nameCol);
                    const JsonId childrenArr = jsonAddArray(outArena);
                    for (const auto& m : ed->members) {
                        jsonArrayPush(outArena, childrenArr, makeLspDocSymbol(outArena, m, "member", 22, dn.line, dn.col, dn.endLine, dn.nameLine, dn.nameCol));
                    }
                    jsonObjectSet(outArena, symObj, "children", childrenArr);
                    jsonArrayPush(outArena, symbolsArr, symObj);
                } else if (const auto* ud = snap.arena.getDecl<UnionDecl>(did)) {
                    const JsonId symObj = makeLspDocSymbol(outArena, ud->name, "union", 23, dn.line, dn.col, dn.endLine, dn.nameLine, dn.nameCol);
                    const JsonId childrenArr = jsonAddArray(outArena);
                    for (const auto& uv : ud->variants) {
                        jsonArrayPush(outArena, childrenArr, makeLspDocSymbol(outArena, uv.name, formatCleanbasicType(snap.arena, uv.type), 22, uv.line, uv.col, uv.line, uv.line, uv.col));
                    }
                    jsonObjectSet(outArena, symObj, "children", childrenArr);
                    jsonArrayPush(outArena, symbolsArr, symObj);
                } else if (const auto* cd = snap.arena.getDecl<ConstDecl>(did)) {
                    std::string detail = "const";
                    const std::string cKey = symbolKey(dn.moduleId, cd->name);
                    if (snap.symbols.constants.count(cKey) > 0) {
                        detail = "as " + formatCleanbasicType(snap.arena, snap.symbols.constants.at(cKey).typeId);
                    }
                    jsonArrayPush(outArena, symbolsArr, makeLspDocSymbol(outArena, cd->name, detail, 14, dn.line, dn.col, dn.endLine, dn.nameLine, dn.nameCol));
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

inline void bindSymbolTarget(ResolvedCursorSymbol& res, const SymbolMeta& sym, std::string hoverMarkdown) {
    res.hoverMarkdown = std::move(hoverMarkdown);
    if (!sym.fileName.empty()) {
        res.definition = makeDefTarget(sym.fileName, sym.line, sym.col, sym.name.size());
    }
}

struct CursorTokenContext {
    bool found = false;
    Token token;
    size_t tokenIndex = 0;
    bool hasDotReceiver = false;
    std::string receiverName;
    bool isImportTarget = false;
};

inline CursorTokenContext resolveCursorToken(std::string_view sourceText, std::string_view filePath, uint32_t line1Based, uint32_t col1Based) {
    CursorTokenContext ctx{};
    const LexResult lexRes = tokenize(sourceText, filePath);
    if (lexRes.tokens.empty()) return ctx;
    for (size_t i = 0; i < lexRes.tokens.size(); ++i) {
        const Token& tok = lexRes.tokens[i];
        if (tok.kind == "EOF" || tok.kind == "NEWLINE") continue;
        if (tok.line != line1Based) continue;
        const uint32_t tokStart = tok.col;
        const uint32_t tokEnd = tokStart + static_cast<uint32_t>(tok.value.size());
        if (col1Based >= tokStart && col1Based <= tokEnd) {
            ctx.found = true;
            ctx.token = tok;
            ctx.tokenIndex = i;
            if (i >= 2 && lexRes.tokens[i - 1].kind == "DOT") {
                const Token& prevTok = lexRes.tokens[i - 2];
                if (prevTok.kind == "IDENT" || prevTok.kind == "TYPE_KW") {
                    ctx.hasDotReceiver = true;
                    ctx.receiverName = prevTok.value;
                } else if (prevTok.kind == "STRING") {
                    ctx.hasDotReceiver = true;
                    ctx.receiverName = """";
                }
            }
            if (i >= 1 && lexRes.tokens[i - 1].kind == "KEYWORD" && lexRes.tokens[i - 1].value == "import" && tok.kind == "STRING") ctx.isImportTarget = true;
            return ctx;
        }
    }
    return ctx;
}

inline bool tryBindSymbolByKey(
    const LspDocumentSnapshot& snap,
    const std::string& key,
    std::string_view rawTokenName,
    bool skipBuiltins,
    ResolvedCursorSymbol& res
) {
    const auto fnIt = snap.symbols.functions.find(key);
    if (fnIt != snap.symbols.functions.end() && (!skipBuiltins || !isBuiltinSymbolName(rawTokenName))) {
        bindSymbolTarget(res, fnIt->second, formatFnHoverMarkdown(snap.arena, fnIt->second));
        return true;
    }
    const auto tyIt = snap.symbols.types.find(key);
    if (tyIt != snap.symbols.types.end() && (!skipBuiltins || !isBuiltinTypeName(rawTokenName))) {
        bindSymbolTarget(res, tyIt->second, formatTypeHoverMarkdown(snap.arena, tyIt->second));
        return true;
    }
    const auto enIt = snap.symbols.enums.find(key);
    if (enIt != snap.symbols.enums.end()) {
        bindSymbolTarget(res, enIt->second, formatEnumHoverMarkdown(enIt->second));
        return true;
    }
    const auto unIt = snap.symbols.unions.find(key);
    if (unIt != snap.symbols.unions.end()) {
        bindSymbolTarget(res, unIt->second, formatUnionHoverMarkdown(snap.arena, unIt->second));
        return true;
    }
    const auto coIt = snap.symbols.constants.find(key);
    if (coIt != snap.symbols.constants.end()) {
        bindSymbolTarget(res, coIt->second, formatConstHoverMarkdown(snap.arena, coIt->second));
        return true;
    }
    return false;
}

inline ResolvedCursorSymbol resolveCursorSymbolInfo(const LspDocumentSnapshot& snap, std::string_view filePath, const CursorTokenContext& cursorCtx) {
    ResolvedCursorSymbol res{};
    if (!snap.valid || !cursorCtx.found) return res;
    const Token& tok = cursorCtx.token;
    const std::string currentModId = findModuleIdForFile(snap.arena, snap.program, filePath);
    if (cursorCtx.isImportTarget && tok.kind == "STRING") {
        const std::string rawTarget = stripQuotes(tok.value);
        if (rawTarget.size() > 3 && rawTarget.substr(rawTarget.size() - 3) == ".cb") {
            const std::string targetModId = resolveModuleId(currentModId, rawTarget);
            const auto modFileOpt = findModuleFilePath(snap.arena, snap.program, targetModId, filePath, rawTarget);
            if (modFileOpt.has_value()) res.definition = makeDefTarget(modFileOpt.value(), 1, 1, 1);
        }
        return res;
    }
    if (tok.kind != "IDENT" && tok.kind != "TYPE_KW" && tok.kind != "KEYWORD") return res;
    const auto fnKeyOpt = findEnclosingFnKey(snap.arena, snap.program, filePath, tok.line);
    std::unordered_map<std::string, VarSymbol> activeScope;
    if (fnKeyOpt.has_value()) {
        const auto scopeIt = snap.symbols.fnScopes.find(fnKeyOpt.value());
        if (scopeIt != snap.symbols.fnScopes.end()) activeScope = scopeIt->second;
    }
    const auto bindFn = [&](const FnSymbol& fn) { bindSymbolTarget(res, fn, formatFnHoverMarkdown(snap.arena, fn)); };
    if (cursorCtx.hasDotReceiver) {
        const std::string& recv = cursorCtx.receiverName;
        const std::string& member = tok.value;
        std::string targetModId;
        if (isModuleAliasInScope(snap.symbols, currentModId, recv, activeScope, targetModId)) {
            const std::string qkey = symbolKey(targetModId, member);
            if (tryBindSymbolByKey(snap, qkey, member, false, res)) {
                return res;
            }
        }
        if (activeScope.find(recv) == activeScope.end()) {
            const auto enumOpt = resolveEnumSymbol(snap.arena, snap.symbols, currentModId, recv);
            if (enumOpt.has_value()) {
                if (enumOpt->memberSet.count(member) > 0) {
                    res.hoverMarkdown = wrapVbCodeBlock("(enum member) " + enumOpt->name + "." + member);
                    if (!enumOpt->fileName.empty()) {
                        res.definition = makeDefTarget(enumOpt->fileName, enumOpt->line, enumOpt->col, enumOpt->name.size());
                    }
                    return res;
                }
            }
            const auto unionOpt = resolveUnionSymbolByName(snap.arena, snap.symbols, currentModId, recv);
            if (unionOpt.has_value()) {
                const auto vtIt = unionOpt->variantTypes.find(member);
                if (vtIt != unionOpt->variantTypes.end()) {
                    res.hoverMarkdown = wrapVbCodeBlock("(union variant) " + unionOpt->name + "." + member + "(payload as " + formatCleanbasicType(snap.arena, vtIt->second) + ") as " + unionOpt->name);
                    if (!unionOpt->fileName.empty()) {
                        for (const auto& uv : unionOpt->variants) {
                            if (uv.name == member) {
                                res.definition = makeDefTarget(unionOpt->fileName, uv.line, uv.col, uv.name.size());
                                break;
                            }
                        }
                    }
                    return res;
                }
            }
        }
        std::optional<TypeId> recvTidOpt;
        if (!recv.empty() && recv.front() == '"') {
            recvTidOpt = snap.symbols.typeStr;
        } else {
            const auto varIt = activeScope.find(recv);
            if (varIt != activeScope.end()) {
                recvTidOpt = varIt->second.typeId;
            } else {
                const auto cIt = snap.symbols.constants.find(symbolKey(currentModId, recv));
                if (cIt != snap.symbols.constants.end()) {
                    recvTidOpt = cIt->second.typeId;
                }
            }
        }
        if (recvTidOpt.has_value()) {
            const TypeId recvTid = recvTidOpt.value();
            const auto tsOpt = resolveTypeSymbol(snap.arena, snap.symbols, currentModId, recvTid);
            if (tsOpt.has_value()) {
                const TypeSymbol& ts = tsOpt.value();
                const auto fit = ts.fields.find(member);
                if (fit != ts.fields.end()) {
                    res.hoverMarkdown = wrapVbCodeBlock("(field) " + ts.name + "." + member + " as " + formatCleanbasicType(snap.arena, fit->second));
                    if (!ts.fileName.empty()) {
                        for (const auto& fd : ts.fieldOrder) {
                            if (fd.name == member) { res.definition = makeDefTarget(ts.fileName, fd.line, fd.col, fd.name.size()); break; }
                        }
                    }
                    return res;
                }
            }
            const auto ufcsFn = resolveUfcsFn(snap.arena, snap.symbols, currentModId, recvTid, member);
            if (ufcsFn.has_value()) {
                if (ufcsFn->isBuiltinReceiver) {
                    const auto bMd = formatBuiltinHoverMarkdown(ufcsFn->name);
                    if (bMd.has_value()) {
                        res.hoverMarkdown = bMd;
                        return res;
                    }
                }
                bindFn(ufcsFn.value());
                return res;
            }
        }
    }
    const auto localIt = activeScope.find(tok.value);
    if (localIt != activeScope.end()) {
        res.hoverMarkdown = formatVarHoverMarkdown(snap.arena, localIt->second);
        if (!localIt->second.fileName.empty()) {
            res.definition = makeDefTarget(localIt->second.fileName, localIt->second.line, localIt->second.col, localIt->second.name.size());
        }
        return res;
    }
    const std::string symKey = resolveSymbolKey(snap.symbols, currentModId, tok.value);
    if (tryBindSymbolByKey(snap, symKey, tok.value, true, res)) {
        return res;
    }
    for (const auto& kv : snap.symbols.types) {
        const auto fit = kv.second.fields.find(tok.value);
        if (fit != kv.second.fields.end()) {
            for (const auto& fd : kv.second.fieldOrder) {
                if (fd.name == tok.value && fd.line == tok.line) {
                    res.hoverMarkdown = wrapVbCodeBlock("(field) " + kv.second.name + "." + tok.value + " as " + formatCleanbasicType(snap.arena, fit->second));
                    return res;
                }
            }
        }
    }
    std::string targetModId;
    if (isModuleAliasInScope(snap.symbols, currentModId, tok.value, activeScope, targetModId)) {
        const auto modFileOpt = findModuleFilePath(snap.arena, snap.program, targetModId, filePath);
        if (modFileOpt.has_value()) res.definition = makeDefTarget(modFileOpt.value(), 1, 1, 1);
    }
    const auto builtinMd = formatBuiltinHoverMarkdown(tok.value);
    if (builtinMd.has_value()) res.hoverMarkdown = builtinMd;
    return res;
}

inline void handleHoverRequest(LspServerState& state, const JsonArena& reqArena, JsonId reqId, JsonId paramsId, std::ostream& out) {
    const LspTextPositionParams doc = parseDocPositionParams(reqArena, paramsId);
    JsonArena outArena;
    if (!doc.valid) { writeJsonrpcResult(reqArena, reqId, outArena, jsonAddNull(outArena), out); return; }
    const std::string sourceText = readDocumentText(state, doc.uri);
    const LspDocumentSnapshot snap = getOrBuildSnapshot(state, doc.uri);
    const CursorTokenContext cursorCtx = resolveCursorToken(sourceText, doc.filePath, doc.line1Based, doc.col1Based);
    const ResolvedCursorSymbol info = resolveCursorSymbolInfo(snap, doc.filePath, cursorCtx);
    if (!info.hoverMarkdown.has_value()) { writeJsonrpcResult(reqArena, reqId, outArena, jsonAddNull(outArena), out); return; }
    const JsonId contents = jsonAddObject(outArena);
    jsonObjectSet(outArena, contents, "kind", jsonAddString(outArena, "markdown"));
    jsonObjectSet(outArena, contents, "value", jsonAddString(outArena, info.hoverMarkdown.value()));
    const JsonId hoverObj = jsonAddObject(outArena);
    jsonObjectSet(outArena, hoverObj, "contents", contents);
    if (cursorCtx.found) jsonObjectSet(outArena, hoverObj, "range", makeLspRange(outArena, cursorCtx.token.line, cursorCtx.token.col, cursorCtx.token.line, cursorCtx.token.col + static_cast<uint32_t>(cursorCtx.token.value.size())));
    writeJsonrpcResult(reqArena, reqId, outArena, hoverObj, out);
}

inline void handleDefinitionRequest(LspServerState& state, const JsonArena& reqArena, JsonId reqId, JsonId paramsId, std::ostream& out) {
    const LspTextPositionParams doc = parseDocPositionParams(reqArena, paramsId);
    JsonArena outArena;
    if (!doc.valid) { writeJsonrpcResult(reqArena, reqId, outArena, jsonAddNull(outArena), out); return; }
    const std::string sourceText = readDocumentText(state, doc.uri);
    const LspDocumentSnapshot snap = getOrBuildSnapshot(state, doc.uri);
    const CursorTokenContext cursorCtx = resolveCursorToken(sourceText, doc.filePath, doc.line1Based, doc.col1Based);
    const ResolvedCursorSymbol info = resolveCursorSymbolInfo(snap, doc.filePath, cursorCtx);
    if (!info.definition.has_value()) { writeJsonrpcResult(reqArena, reqId, outArena, jsonAddNull(outArena), out); return; }
    const DefinitionTarget& dt = info.definition.value();
    writeJsonrpcResult(reqArena, reqId, outArena, makeLspLocation(outArena, dt.fileName, dt.line, dt.col, dt.nameLen), out);
}

} // namespace cb

#endif // CLEANBASIC_LSP_NAVIGATION_HPP
