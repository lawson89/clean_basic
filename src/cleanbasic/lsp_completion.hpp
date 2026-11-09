
#ifndef CLEANBASIC_LSP_COMPLETION_HPP
#define CLEANBASIC_LSP_COMPLETION_HPP

#include "src/cleanbasic/lsp_navigation.hpp"

namespace cb {

struct CursorLineSlice {
    std::string fullLine;
    std::string prefixToCursor;
    size_t lineStartOffset = 0;
    size_t lineEndOffset = 0;
};

inline CursorLineSlice extractCursorLineSlice(std::string_view sourceText, uint32_t line0Based, uint32_t char0Based) {
    CursorLineSlice slice{};
    uint32_t currLine = 0;
    size_t i = 0;
    while (i < sourceText.size() && currLine < line0Based) {
        if (sourceText[i] == '\n') currLine += 1;
        i += 1;
    }
    slice.lineStartOffset = i;
    size_t endI = i;
    while (endI < sourceText.size() && sourceText[endI] != '\n') endI += 1;
    slice.lineEndOffset = endI;
    std::string_view rawLine = sourceText.substr(i, endI - i);
    if (!rawLine.empty() && rawLine.back() == '\r') rawLine.remove_suffix(1);
    slice.fullLine = std::string(rawLine);
    size_t prefixLen = static_cast<size_t>(char0Based);
    if (prefixLen > slice.fullLine.size()) prefixLen = slice.fullLine.size();
    slice.prefixToCursor = slice.fullLine.substr(0, prefixLen);
    return slice;
}

inline std::string scrubIncompleteLineInSource(std::string_view sourceText, const CursorLineSlice& slice) {
    std::string scrubbed(sourceText.substr(0, slice.lineStartOffset));
    const std::string trimmed = trimAsciiWs(slice.prefixToCursor);
    if (startsWithIgnoreCase(trimmed, "dim ") || startsWithIgnoreCase(trimmed, "const ")) {
        const size_t eqPos = slice.prefixToCursor.find('=');
        if (eqPos != std::string::npos) scrubbed += slice.prefixToCursor.substr(0, eqPos + 1) + " 0";
        else scrubbed += "' scrubbed";
    } else if (startsWithIgnoreCase(trimmed, "if ") || startsWithIgnoreCase(trimmed, "else if ")) {
        if (startsWithIgnoreCase(trimmed, "else if ")) scrubbed += "else if true";
        else scrubbed += "if true";
    } else if (startsWithIgnoreCase(trimmed, "return ")) scrubbed += "return 0";
    else scrubbed += "' scrubbed";
    scrubbed += std::string(sourceText.substr(slice.lineEndOffset));
    return scrubbed;
}

inline LspDocumentSnapshot getOrBuildScrubbedSnapshot(LspServerState& state, std::string_view uri, std::string_view sourceText, uint32_t line0Based, uint32_t char0Based) {
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
        const CheckSourceResult scrubbedRes = checkSource(scrubbedText, filePath, scrubbedBuffers);
        if (scrubbedRes.ok) return LspDocumentSnapshot{true, scrubbedRes.arena, scrubbedRes.program, scrubbedRes.symbols};
    }
    const auto snapIt = state.snapshots.find(uriKey);
    if (snapIt != state.snapshots.end() && snapIt->second.valid) return snapIt->second;
    return LspDocumentSnapshot{};
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
    const LexResult lexRes = tokenize(std::string(linePrefix), "line.cb");
    std::vector<Token> toks;
    for (const auto& t : lexRes.tokens) if (t.kind != "EOF" && t.kind != "NEWLINE") toks.push_back(t);
    if (toks.empty()) return ctx;
    int parenDepth = 0, bracketDepth = 0, braceDepth = 0;
    size_t openParenIdx = toks.size();
    for (size_t i = toks.size(); i > 0; --i) {
        const Token& t = toks[i - 1];
        if (t.kind == "RPAREN") parenDepth++;
        else if (t.kind == "LPAREN") { if (parenDepth > 0) parenDepth--; else { openParenIdx = i - 1; break; } }
        else if (t.kind == "RBRACKET") bracketDepth++;
        else if (t.kind == "LBRACKET") { if (bracketDepth > 0) bracketDepth--; }
        else if (t.kind == "RBRACE") braceDepth++;
        else if (t.kind == "LBRACE") { if (braceDepth > 0) braceDepth--; }
    }
    if (openParenIdx == toks.size() || openParenIdx == 0) return ctx;
    const Token& calleeTok = toks[openParenIdx - 1];
    if (calleeTok.kind != "IDENT" && calleeTok.kind != "TYPE_KW" && !(calleeTok.kind == "KEYWORD" && calleeTok.value == "error")) return ctx;
    ctx.insideCall = true;
    ctx.calleeName = calleeTok.value;
    if (openParenIdx >= 3 && toks[openParenIdx - 2].kind == "DOT") {
        const Token& recvTok = toks[openParenIdx - 3];
        if (recvTok.kind == "IDENT" || recvTok.kind == "TYPE_KW") { ctx.hasReceiver = true; ctx.receiverName = recvTok.value; }
    }
    int innerParen = 0, innerBracket = 0, innerBrace = 0;
    uint32_t commaCount = 0;
    size_t lastDelimiterIdx = openParenIdx;
    for (size_t k = openParenIdx + 1; k < toks.size(); ++k) {
        const Token& t = toks[k];
        if (t.kind == "LPAREN") innerParen++;
        else if (t.kind == "RPAREN") { if (innerParen > 0) innerParen--; }
        else if (t.kind == "LBRACKET") innerBracket++;
        else if (t.kind == "RBRACKET") { if (innerBracket > 0) innerBracket--; }
        else if (t.kind == "LBRACE") innerBrace++;
        else if (t.kind == "RBRACE") { if (innerBrace > 0) innerBrace--; }
        else if (t.kind == "COMMA" && innerParen == 0 && innerBracket == 0 && innerBrace == 0) { commaCount++; lastDelimiterIdx = k; }
    }
    ctx.activeParam = commaCount;
    const size_t remTokens = toks.size() - (lastDelimiterIdx + 1);
    if (remTokens == 0) ctx.atArgNamePosition = true;
    else if (remTokens == 1) {
        const Token& lastTok = toks.back();
        if (!linePrefix.empty() && isIdentChar(linePrefix.back()) && (lastTok.kind == "IDENT" || lastTok.kind == "TYPE_KW")) { ctx.atArgNamePosition = true; ctx.argPrefix = lastTok.value; }
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
    size_t wordStart = linePrefix.size();
    while (wordStart > 0 && isIdentChar(linePrefix[wordStart - 1])) wordStart--;
    res.prefix = std::string(linePrefix.substr(wordStart));
    size_t beforeWord = wordStart;
    while (beforeWord > 0 && (linePrefix[beforeWord - 1] == ' ' || linePrefix[beforeWord - 1] == '\t')) beforeWord--;
    if (wordStart > 0 && linePrefix[wordStart - 1] == '.') {
        size_t recvEnd = wordStart - 1;
        while (recvEnd > 0 && (linePrefix[recvEnd - 1] == ' ' || linePrefix[recvEnd - 1] == '\t')) recvEnd--;
        size_t recvStart = recvEnd;
        while (recvStart > 0 && isIdentChar(linePrefix[recvStart - 1])) recvStart--;
        if (recvEnd > recvStart) { res.isDot = true; res.dotReceiver = std::string(linePrefix.substr(recvStart, recvEnd - recvStart)); }
    } else if (beforeWord >= 2) {
        size_t kwEnd = beforeWord;
        size_t kwStart = kwEnd;
        while (kwStart > 0 && isIdentChar(linePrefix[kwStart - 1])) kwStart--;
        if (linePrefix.substr(kwStart, kwEnd - kwStart) == "as" && beforeWord < wordStart) res.isAfterAs = true;
    }
    return res;
}

inline bool matchesCompletionPrefix(std::string_view candidate, std::string_view prefix) {
    return prefix.empty() || (candidate.size() >= prefix.size() && candidate.substr(0, prefix.size()) == prefix);
}

inline void addUniqueCompletionItem(std::vector<CompletionItemEntry>& items, std::unordered_map<std::string, bool>& seen, const CompletionItemEntry& entry) {
    const std::string key = std::to_string(entry.kind) + ":" + entry.label + ":" + entry.insertText;
    if (seen.find(key) == seen.end()) { seen[key] = true; items.push_back(entry); }
}

inline void addCompletionEntry(std::vector<CompletionItemEntry>& items, std::unordered_map<std::string, bool>& seen, std::string_view label, int64_t kind, std::string_view detail, std::string_view insertText = "", std::string_view documentation = "") {
    std::string finalInsertText(insertText);
    if (finalInsertText.empty()) {
        finalInsertText = std::string(label);
    }
    addUniqueCompletionItem(items, seen, CompletionItemEntry{std::string(label), kind, std::string(detail), finalInsertText, std::string(documentation)});
}

inline void collectModuleSymbolCompletions(const LspDocumentSnapshot& snap, std::string_view targetModuleId, std::string_view prefix, bool requirePub, bool includeFunctions, bool includeModuleAliases, std::vector<CompletionItemEntry>& items, std::unordered_map<std::string, bool>& seen) {
    if (includeFunctions) {
        for (const auto& kv : snap.symbols.functions) {
            const FnSymbol& fn = kv.second;
            if (fn.moduleId != targetModuleId || (requirePub && !fn.isPub) || (!requirePub && isBuiltinSymbolName(fn.name))) continue;
            if (matchesCompletionPrefix(fn.name, prefix)) addCompletionEntry(items, seen, fn.name, 3, "fn" + formatFnDetailSignature(snap.arena, fn.params, fn.returnType, fn.isFallible), fn.name, formatFnHoverMarkdown(snap.arena, fn));
        }
    }
    for (const auto& kv : snap.symbols.types) {
        const TypeSymbol& ts = kv.second;
        if (ts.moduleId != targetModuleId || (requirePub && !ts.isPub) || (!requirePub && isBuiltinTypeName(ts.name))) continue;
        if (matchesCompletionPrefix(ts.name, prefix)) addCompletionEntry(items, seen, ts.name, 22, "type " + ts.name, ts.name, formatTypeHoverMarkdown(snap.arena, ts));
    }
    for (const auto& kv : snap.symbols.enums) {
        const EnumSymbol& es = kv.second;
        if (es.moduleId != targetModuleId || (requirePub && !es.isPub)) continue;
        if (matchesCompletionPrefix(es.name, prefix)) addCompletionEntry(items, seen, es.name, 13, "enum " + es.name, es.name, formatEnumHoverMarkdown(es));
    }
    for (const auto& kv : snap.symbols.aliases) {
        const AliasSymbol& al = kv.second;
        if (al.moduleId != targetModuleId || (requirePub && !al.isPub)) continue;
        if (matchesCompletionPrefix(al.name, prefix)) addCompletionEntry(items, seen, al.name, 25, "alias " + al.name + " = " + formatCleanbasicType(snap.arena, al.targetType), al.name, formatAliasHoverMarkdown(snap.arena, snap.symbols, al));
    }
    if (includeFunctions) {
        for (const auto& kv : snap.symbols.constants) {
            const ConstSymbol& cs = kv.second;
            if (cs.moduleId != targetModuleId || (requirePub && !cs.isPub)) continue;
            if (matchesCompletionPrefix(cs.name, prefix)) addCompletionEntry(items, seen, cs.name, 21, "const as " + formatCleanbasicType(snap.arena, cs.typeId), cs.name, formatConstHoverMarkdown(snap.arena, cs));
        }
    }
    if (includeModuleAliases) {
        const auto modMapIt = snap.symbols.modules.find(std::string(targetModuleId));
        if (modMapIt != snap.symbols.modules.end()) for (const auto& kv : modMapIt->second) if (matchesCompletionPrefix(kv.first, prefix)) addCompletionEntry(items, seen, kv.first, 9, "module " + kv.second, kv.first);
    }
}

inline std::optional<TypeId> resolveVariableTypeInFn(const LspDocumentSnapshot& snap, const std::optional<std::string>& enclosingFnKey, std::string_view varName) {
    if (!enclosingFnKey.has_value()) return std::nullopt;
    const auto scopeIt = snap.symbols.fnScopes.find(enclosingFnKey.value());
    if (scopeIt == snap.symbols.fnScopes.end()) return std::nullopt;
    const auto varIt = scopeIt->second.find(std::string(varName));
    if (varIt == scopeIt->second.end()) return std::nullopt;
    return varIt->second.typeId;
}

inline std::string formatUfcsMethodDetail(const AstArena& arena, const FnSymbol& fn) {
    std::vector<Param> remainingParams;
    for (size_t i = 1; i < fn.params.size(); ++i) remainingParams.push_back(fn.params[i]);
    return "ufcs fn " + fn.name + formatFnDetailSignature(arena, remainingParams, fn.returnType, fn.isFallible);
}

inline void collectUfcsCompletionsForType(const LspDocumentSnapshot& snap, std::string_view currentModuleId, TypeId recvTid, std::string_view prefix, std::vector<CompletionItemEntry>& items, std::unordered_map<std::string, bool>& seen) {
    if (recvTid == INVALID_ID || recvTid >= snap.arena.types.size()) return;
    const TypeNode resolvedRecvNode = resolveAliasNode(snap.arena, snap.symbols, currentModuleId, recvTid);
    std::string recvTypeKey = resolvedRecvNode.name;
    if (!recvTypeKey.empty() && !isBuiltinTypeName(recvTypeKey)) recvTypeKey = resolveSymbolKey(snap.symbols, currentModuleId, recvTypeKey);
    const std::string recvModId = resolveUfcsDefiningModule(snap.symbols, currentModuleId, recvTypeKey);
    for (const auto& kv : snap.symbols.functions) {
        const FnSymbol& fn = kv.second;
        if (fn.params.empty() || !matchesCompletionPrefix(fn.name, prefix)) continue;
        if (fn.moduleId != currentModuleId && (recvModId.empty() || fn.moduleId != recvModId || !fn.isPub)) continue;
        if (ufcsParamMatchesReceiver(snap.arena, snap.symbols, fn, recvTypeKey)) addCompletionEntry(items, seen, fn.name, 2, formatUfcsMethodDetail(snap.arena, fn), fn.name, formatFnHoverMarkdown(snap.arena, fn));
    }
}

inline void collectDotCompletions(const LspDocumentSnapshot& snap, std::string_view currentModuleId, const std::optional<std::string>& enclosingFnKey, std::string_view receiverName, std::string_view prefix, std::vector<CompletionItemEntry>& items, std::unordered_map<std::string, bool>& seen) {
    auto varTidOpt = resolveVariableTypeInFn(snap, enclosingFnKey, receiverName);
    if (!varTidOpt.has_value()) {
        const auto cIt = snap.symbols.constants.find(symbolKey(currentModuleId, receiverName));
        if (cIt != snap.symbols.constants.end()) {
            varTidOpt = cIt->second.typeId;
        }
    }
    if (!varTidOpt.has_value()) {
        if (receiverName == "bytes") {
            if (matchesCompletionPrefix("fromHex", prefix)) {
                addCompletionEntry(items, seen, "fromHex", 2, "fn(hex as str) as bytes can error");
            }
            return;
        }
        const auto enumOpt = resolveEnumSymbol(snap.arena, snap.symbols, currentModuleId, receiverName);
        if (enumOpt.has_value() && (enumOpt->moduleId == currentModuleId || enumOpt->isPub)) {
            for (const auto& m : enumOpt->members) {
                if (!matchesCompletionPrefix(m.name, prefix)) continue;
                std::string detail = "enum member";
                const auto vit = enumOpt->memberValues.find(m.name);
                if (vit != enumOpt->memberValues.end()) {
                    detail = "= " + std::to_string(vit->second);
                }
                addCompletionEntry(items, seen, m.name, 20, detail);
            }
            return;
        }
        const auto modMapIt = snap.symbols.modules.find(std::string(currentModuleId));
        if (modMapIt != snap.symbols.modules.end()) {
            const auto aliasIt = modMapIt->second.find(std::string(receiverName));
            if (aliasIt != modMapIt->second.end()) collectModuleSymbolCompletions(snap, aliasIt->second, prefix, true, true, false, items, seen);
        }
        return;
    }
    const TypeId recvTid = resolveAlias(snap.arena, snap.symbols, currentModuleId, varTidOpt.value());
    if (recvTid == INVALID_ID || recvTid >= snap.arena.types.size()) return;
    const TypeNode& recvNode = snap.arena.types[recvTid];
    const bool recvIsEnum = isEnumType(snap.symbols, recvTid, snap.arena, currentModuleId);

    // Built-in receiver methods
    if (recvNode.name == "list" || recvNode.name == "dict" || recvNode.name == "str" || recvNode.name == "bytes") {
        if (matchesCompletionPrefix("len", prefix)) addCompletionEntry(items, seen, "len", 2, "fn() as int");
    }
    if (recvNode.name == "list") {
        std::string elemType = "T";
        if (!recvNode.typeArgs.empty()) elemType = formatCleanbasicType(snap.arena, recvNode.typeArgs[0]);
        if (matchesCompletionPrefix("push", prefix)) addCompletionEntry(items, seen, "push", 2, "fn(item as " + elemType + ")");
        if (matchesCompletionPrefix("pop", prefix)) addCompletionEntry(items, seen, "pop", 2, "fn() as " + elemType);
        if (matchesCompletionPrefix("clear", prefix)) addCompletionEntry(items, seen, "clear", 2, "fn()");
    }
    if (recvNode.name == "dict") {
        std::string keyType = "K";
        if (!recvNode.typeArgs.empty()) keyType = formatCleanbasicType(snap.arena, recvNode.typeArgs[0]);
        if (matchesCompletionPrefix("has", prefix)) addCompletionEntry(items, seen, "has", 2, "fn(key as " + keyType + ") as bool");
        if (matchesCompletionPrefix("remove", prefix)) addCompletionEntry(items, seen, "remove", 2, "fn(key as " + keyType + ") as bool");
        if (matchesCompletionPrefix("clear", prefix)) addCompletionEntry(items, seen, "clear", 2, "fn()");
    }
    if (recvNode.name == "bytes") {
        if (matchesCompletionPrefix("push", prefix)) addCompletionEntry(items, seen, "push", 2, "fn(byteVal as int)");
        if (matchesCompletionPrefix("pop", prefix)) addCompletionEntry(items, seen, "pop", 2, "fn() as int");
        if (matchesCompletionPrefix("clear", prefix)) addCompletionEntry(items, seen, "clear", 2, "fn()");
        if (matchesCompletionPrefix("slice", prefix)) addCompletionEntry(items, seen, "slice", 2, "fn(start as int, stop as int) as bytes");
        if (matchesCompletionPrefix("sliceFrom", prefix)) addCompletionEntry(items, seen, "sliceFrom", 2, "fn(start as int) as bytes");
        if (matchesCompletionPrefix("toUtf8", prefix)) addCompletionEntry(items, seen, "toUtf8", 2, "fn() as str can error");
        if (matchesCompletionPrefix("toHex", prefix)) addCompletionEntry(items, seen, "toHex", 2, "fn() as str");
        const std::vector<std::string> readMethods = {"readU16Le", "readU16Be", "readU32Le", "readU32Be", "readU64Le", "readU64Be"};
        for (const auto& rm : readMethods) {
            if (matchesCompletionPrefix(rm, prefix)) addCompletionEntry(items, seen, rm, 2, "fn(offset as int) as int can error");
        }
        const std::vector<std::string> writeMethods = {"writeU16Le", "writeU16Be", "writeU32Le", "writeU32Be", "writeU64Le", "writeU64Be"};
        for (const auto& wm : writeMethods) {
            if (matchesCompletionPrefix(wm, prefix)) addCompletionEntry(items, seen, wm, 2, "fn(offset as int, value as int) can error");
        }
    }
    if (recvNode.name == "str") {
        if (matchesCompletionPrefix("toBytes", prefix)) addCompletionEntry(items, seen, "toBytes", 2, "fn() as bytes");
    }
    if (recvNode.name == "int" || recvNode.name == "float" || recvNode.name == "bool" || recvNode.name == "str" || recvNode.name == "err" || recvIsEnum) {
        if (matchesCompletionPrefix("toStr", prefix)) addCompletionEntry(items, seen, "toStr", 2, "fn() as str");
    }
    if (recvNode.name == "int" || recvNode.name == "float" || recvNode.name == "bool" || recvIsEnum) {
        if (matchesCompletionPrefix("toInt", prefix)) addCompletionEntry(items, seen, "toInt", 2, "fn() as int");
    }
    if (recvNode.name == "int" || recvNode.name == "float") {
        if (matchesCompletionPrefix("toFloat", prefix)) addCompletionEntry(items, seen, "toFloat", 2, "fn() as float");
    }

    if (recvTid == snap.symbols.typeErr || recvNode.name == "err") {
        if (matchesCompletionPrefix("msg", prefix)) addCompletionEntry(items, seen, "msg", 5, "as str");
        if (matchesCompletionPrefix("code", prefix)) addCompletionEntry(items, seen, "code", 5, "as int");
    }
    const auto tsOpt = resolveTypeSymbol(snap.arena, snap.symbols, currentModuleId, recvTid);
    if (tsOpt.has_value()) {
        for (const auto& f : tsOpt->fieldOrder) {
            if (!matchesCompletionPrefix(f.name, prefix)) continue;
            TypeId ftid = f.typeId;
            const auto fit = tsOpt->fields.find(f.name);
            if (fit != tsOpt->fields.end()) ftid = fit->second;
            addCompletionEntry(items, seen, f.name, 5, "as " + formatCleanbasicType(snap.arena, ftid));
        }
    }
    collectUfcsCompletionsForType(snap, currentModuleId, recvTid, prefix, items, seen);
}

inline std::optional<TypeSymbol> resolveCallStructSymbol(const LspDocumentSnapshot& snap, std::string_view currentModuleId, const std::optional<std::string>& enclosingFnKey, const CallSiteContext& callCtx) {
    std::string lookupName = callCtx.calleeName;
    if (callCtx.hasReceiver) {
        if (resolveVariableTypeInFn(snap, enclosingFnKey, callCtx.receiverName).has_value()) return std::nullopt;
        lookupName = callCtx.receiverName + "." + callCtx.calleeName;
    }
    return resolveTypeSymbolByName(snap.arena, snap.symbols, currentModuleId, lookupName, callCtx.hasReceiver);
}

struct ResolvedCallFunction { FnSymbol fn; bool isUfcs = false; };

inline std::optional<ResolvedCallFunction> resolveCallFnSymbol(const LspDocumentSnapshot& snap, std::string_view currentModuleId, const std::optional<std::string>& enclosingFnKey, const CallSiteContext& callCtx) {
    if (callCtx.hasReceiver) {
        const auto varTidOpt = resolveVariableTypeInFn(snap, enclosingFnKey, callCtx.receiverName);
        if (!varTidOpt.has_value()) {
            const auto modMapIt = snap.symbols.modules.find(std::string(currentModuleId));
            if (modMapIt != snap.symbols.modules.end()) {
                const auto aliasIt = modMapIt->second.find(callCtx.receiverName);
                if (aliasIt != modMapIt->second.end()) {
                    const std::string fnKey = symbolKey(aliasIt->second, callCtx.calleeName);
                    const auto fnIt = snap.symbols.functions.find(fnKey);
                    if (fnIt != snap.symbols.functions.end() && fnIt->second.isPub) return ResolvedCallFunction{fnIt->second, false};
                }
            }
        } else {
            const auto ufcsOpt = resolveUfcsFn(snap.arena, snap.symbols, currentModuleId, varTidOpt.value(), callCtx.calleeName);
            if (ufcsOpt.has_value()) return ResolvedCallFunction{ufcsOpt.value(), true};
        }
        return std::nullopt;
    }
    const std::string fnKey = resolveSymbolKey(snap.symbols, currentModuleId, callCtx.calleeName);
    const auto fnIt = snap.symbols.functions.find(fnKey);
    if (fnIt != snap.symbols.functions.end()) return ResolvedCallFunction{fnIt->second, false};
    return std::nullopt;
}

inline void collectNamedArgumentCompletions(const LspDocumentSnapshot& snap, std::string_view currentModuleId, const std::optional<std::string>& enclosingFnKey, const CallSiteContext& callCtx, std::vector<CompletionItemEntry>& items, std::unordered_map<std::string, bool>& seen) {
    if (!callCtx.insideCall || !callCtx.atArgNamePosition) return;
    if (!callCtx.hasReceiver && callCtx.calleeName == "error") {
        if (matchesCompletionPrefix("code", callCtx.argPrefix)) addCompletionEntry(items, seen, "code=", 6, "as int");
        return;
    }
    const auto tsOpt = resolveCallStructSymbol(snap, currentModuleId, enclosingFnKey, callCtx);
    if (tsOpt.has_value()) {
        for (const auto& f : tsOpt->fieldOrder) {
            if (!matchesCompletionPrefix(f.name, callCtx.argPrefix)) continue;
            TypeId ftid = f.typeId;
            const auto fit = tsOpt->fields.find(f.name);
            if (fit != tsOpt->fields.end()) ftid = fit->second;
            addCompletionEntry(items, seen, f.name + "=", 5, "as " + formatCleanbasicType(snap.arena, ftid));
        }
        return;
    }
    const auto fnOpt = resolveCallFnSymbol(snap, currentModuleId, enclosingFnKey, callCtx);
    if (fnOpt.has_value()) {
        const FnSymbol& fn = fnOpt.value().fn;
        size_t startIdx = 0;
        if (fnOpt.value().isUfcs) {
            startIdx = 1;
        }
        for (size_t i = startIdx; i < fn.params.size(); ++i) {
            if (matchesCompletionPrefix(fn.params[i].name, callCtx.argPrefix)) addCompletionEntry(items, seen, fn.params[i].name + "=", 6, "as " + formatCleanbasicType(snap.arena, fn.params[i].typeId));
        }
    }
}

inline void collectTypeAnnotationCompletions(const LspDocumentSnapshot& snap, std::string_view currentModuleId, std::string_view prefix, std::vector<CompletionItemEntry>& items, std::unordered_map<std::string, bool>& seen) {
    const std::vector<std::string> builtinTypes = {"int", "float", "str", "bytes", "bool", "cptr", "err", "list", "dict"};
    for (const auto& bt : builtinTypes) if (matchesCompletionPrefix(bt, prefix)) addCompletionEntry(items, seen, bt, 14, "built-in type");
    if (snap.valid) collectModuleSymbolCompletions(snap, currentModuleId, prefix, false, false, true, items, seen);
}

inline void collectGeneralScopeCompletions(const LspDocumentSnapshot& snap, std::string_view currentModuleId, const std::optional<std::string>& enclosingFnKey, std::string_view prefix, std::vector<CompletionItemEntry>& items, std::unordered_map<std::string, bool>& seen) {
    if (snap.valid) {
        if (enclosingFnKey.has_value()) {
            const auto scopeIt = snap.symbols.fnScopes.find(enclosingFnKey.value());
            if (scopeIt != snap.symbols.fnScopes.end()) {
                for (const auto& kv : scopeIt->second) if (matchesCompletionPrefix(kv.second.name, prefix)) addCompletionEntry(items, seen, kv.second.name, 6, "as " + formatCleanbasicType(snap.arena, kv.second.typeId), kv.second.name, formatVarHoverMarkdown(snap.arena, kv.second));
            }
        }
        collectModuleSymbolCompletions(snap, currentModuleId, prefix, false, true, true, items, seen);
    }
    const std::vector<std::pair<std::string, std::string>> builtins = {{"print", "fn print(values...)"}, {"error", "fn error(msg as str, [code=1]) as err"}};
    for (const auto& b : builtins) if (matchesCompletionPrefix(b.first, prefix)) {
        std::string doc;
        const auto docOpt = formatBuiltinHoverMarkdown(b.first);
        if (docOpt.has_value()) doc = docOpt.value();
        addCompletionEntry(items, seen, b.first, 3, b.second, b.first, doc);
    }
    const std::vector<std::string> keywords = {"fn", "end fn", "pub", "extern", "type", "end type", "enum", "end enum", "alias", "import", "import header", "import link", "dim", "const", "byref", "as", "if", "else if", "else", "end if", "for", "to", "step", "in", "end for", "while", "end while", "select case", "case", "case else", "end select", "on error", "end error", "defer", "end defer", "return", "break", "continue", "can error", "and", "or", "not", "true", "false", "null"};
    for (const auto& kw : keywords) if (matchesCompletionPrefix(kw, prefix)) addCompletionEntry(items, seen, kw, 14, "keyword");
}

inline void handleCompletionRequest(LspServerState& state, const JsonArena& reqArena, JsonId reqId, JsonId paramsId, std::ostream& out) {
    const LspTextPositionParams doc = parseDocPositionParams(reqArena, paramsId);
    JsonArena outArena;
    const JsonId itemsArr = jsonAddArray(outArena);
    if (!doc.valid) { writeJsonrpcResult(reqArena, reqId, outArena, itemsArr, out); return; }
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
    if (cursorInfo.isDot) { if (snap.valid) collectDotCompletions(snap, currentModId, enclosingFnKey, cursorInfo.dotReceiver, cursorInfo.prefix, entries, seen); }
    else if (cursorInfo.isAfterAs) collectTypeAnnotationCompletions(snap, currentModId, cursorInfo.prefix, entries, seen);
    else {
        if (snap.valid && cursorInfo.callCtx.insideCall && cursorInfo.callCtx.atArgNamePosition) collectNamedArgumentCompletions(snap, currentModId, enclosingFnKey, cursorInfo.callCtx, entries, seen);
        collectGeneralScopeCompletions(snap, currentModId, enclosingFnKey, cursorInfo.prefix, entries, seen);
    }
    for (const auto& entry : entries) {
        const JsonId itemObj = jsonAddObject(outArena);
        jsonObjectSet(outArena, itemObj, "label", jsonAddString(outArena, entry.label));
        jsonObjectSet(outArena, itemObj, "kind", jsonAddInt(outArena, entry.kind));
        if (!entry.detail.empty()) jsonObjectSet(outArena, itemObj, "detail", jsonAddString(outArena, entry.detail));
        if (!entry.insertText.empty()) jsonObjectSet(outArena, itemObj, "insertText", jsonAddString(outArena, entry.insertText));
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

struct SignatureHelpData { std::string label; std::string documentation; std::vector<std::string> paramLabels; uint32_t activeParameter = 0; };

inline std::optional<SignatureHelpData> resolveSignatureHelp(const LspDocumentSnapshot& snap, std::string_view currentModuleId, const std::optional<std::string>& enclosingFnKey, const CallSiteContext& callCtx) {
    if (!callCtx.insideCall) return std::nullopt;
    if (!callCtx.hasReceiver) {
        if (callCtx.calleeName == "error") {
            std::string doc;
            const auto docOpt = formatBuiltinHoverMarkdown("error");
            if (docOpt.has_value()) doc = docOpt.value();
            uint32_t activeIdx = callCtx.activeParam;
            if (activeIdx > 1) {
                activeIdx = 1;
            }
            return SignatureHelpData{"fn error(msg as str, code as int) as err", doc, {"msg as str", "code as int"}, activeIdx};
        }
        if (callCtx.calleeName == "print") return SignatureHelpData{"fn print(values...)", "", {"values..."}, 0};
    } else {
        // Built-in receiver methods
        if (callCtx.calleeName == "len") return SignatureHelpData{"fn .len() as int", "", {}, 0};
        if (callCtx.calleeName == "toStr") return SignatureHelpData{"fn .toStr() as str", "", {}, 0};
        if (callCtx.calleeName == "toInt") return SignatureHelpData{"fn .toInt() as int", "", {}, 0};
        if (callCtx.calleeName == "toFloat") return SignatureHelpData{"fn .toFloat() as float", "", {}, 0};
        if (callCtx.calleeName == "push") return SignatureHelpData{"fn .push(item as T)", "", {"item as T"}, 0};
    }
    if (!snap.valid) return std::nullopt;
    const auto tsOpt = resolveCallStructSymbol(snap, currentModuleId, enclosingFnKey, callCtx);
    if (tsOpt.has_value()) {
        SignatureHelpData sig{};
        sig.label = tsOpt->name + "(";
        for (size_t i = 0; i < tsOpt->fieldOrder.size(); ++i) {
            if (i > 0) sig.label += ", ";
            TypeId ftid = tsOpt->fieldOrder[i].typeId;
            const auto fit = tsOpt->fields.find(tsOpt->fieldOrder[i].name);
            if (fit != tsOpt->fields.end()) ftid = fit->second;
            const std::string pLabel = tsOpt->fieldOrder[i].name + " as " + formatCleanbasicType(snap.arena, ftid);
            sig.label += pLabel;
            sig.paramLabels.push_back(pLabel);
        }
        sig.label += ") as " + tsOpt->name;
        sig.documentation = formatTypeHoverMarkdown(snap.arena, tsOpt.value());
        sig.activeParameter = callCtx.activeParam;
        if (!sig.paramLabels.empty() && sig.activeParameter >= sig.paramLabels.size()) sig.activeParameter = static_cast<uint32_t>(sig.paramLabels.size() - 1);
        return sig;
    }
    const auto fnOpt = resolveCallFnSymbol(snap, currentModuleId, enclosingFnKey, callCtx);
    if (fnOpt.has_value()) {
        const FnSymbol& fn = fnOpt.value().fn;
        std::vector<Param> visibleParams;
        size_t startIdx = 0;
        if (fnOpt.value().isUfcs) {
            startIdx = 1;
        }
        for (size_t i = startIdx; i < fn.params.size(); ++i) {
            visibleParams.push_back(fn.params[i]);
        }
        SignatureHelpData sig{};
        sig.label = "fn " + fn.name + formatFnDetailSignature(snap.arena, visibleParams, fn.returnType, fn.isFallible);
        sig.documentation = formatFnHoverMarkdown(snap.arena, fn);
        for (const auto& p : visibleParams) {
            std::string byrefPrefix;
            if (p.isByref) {
                byrefPrefix = "byref ";
            }
            sig.paramLabels.push_back(byrefPrefix + p.name + " as " + formatCleanbasicType(snap.arena, p.typeId));
        }
        sig.activeParameter = callCtx.activeParam;
        if (!sig.paramLabels.empty() && sig.activeParameter >= sig.paramLabels.size()) sig.activeParameter = static_cast<uint32_t>(sig.paramLabels.size() - 1);
        return sig;
    }
    return std::nullopt;
}

inline void handleSignatureHelpRequest(LspServerState& state, const JsonArena& reqArena, JsonId reqId, JsonId paramsId, std::ostream& out) {
    const LspTextPositionParams doc = parseDocPositionParams(reqArena, paramsId);
    JsonArena outArena;
    if (!doc.valid) { writeJsonrpcResult(reqArena, reqId, outArena, jsonAddNull(outArena), out); return; }
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
    if (!sigOpt.has_value()) { writeJsonrpcResult(reqArena, reqId, outArena, jsonAddNull(outArena), out); return; }
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

} // namespace cb

#endif // CLEANBASIC_LSP_COMPLETION_HPP
