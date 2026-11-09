
#ifndef CLEANBASIC_LSP_HPP
#define CLEANBASIC_LSP_HPP

#include "src/cleanbasic/lsp_completion.hpp"

namespace cb {

inline void handleLspMessage(LspServerState& state, std::string_view payload, std::ostream& out) {
    const JsonParseResult parsed = parseJson(payload);
    if (!parsed.ok) return;
    const JsonId methodId = jsonGetField(parsed.arena, parsed.root, "method");
    const auto methodOpt = jsonGetString(parsed.arena, methodId);
    if (!methodOpt.has_value()) return;
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
    if (method == "initialized") { state.initialized = true; return; }
    if (method == "shutdown") { state.shutdownRequested = true; JsonArena outArena; writeJsonrpcResult(parsed.arena, reqId, outArena, jsonAddNull(outArena), out); return; }
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
        if (uriOpt.has_value() && textOpt.has_value()) { state.openDocuments[uriOpt.value()] = textOpt.value(); publishDocumentDiagnostics(state, uriOpt.value(), textOpt.value(), out); }
        return;
    }
    if (method == "textDocument/didChange") {
        const JsonId textDoc = jsonGetField(parsed.arena, paramsId, "textDocument");
        const auto uriOpt = jsonGetString(parsed.arena, jsonGetField(parsed.arena, textDoc, "uri"));
        const JsonId changesId = jsonGetField(parsed.arena, paramsId, "contentChanges");
        if (uriOpt.has_value() && changesId < parsed.arena.nodes.size() && std::holds_alternative<JsonArray>(parsed.arena.nodes[changesId])) {
            const auto& arr = std::get<JsonArray>(parsed.arena.nodes[changesId]);
            bool updated = false;
            std::string latestText;
            for (const JsonId elemId : arr.elements) {
                const auto textOpt = jsonGetString(parsed.arena, jsonGetField(parsed.arena, elemId, "text"));
                if (textOpt.has_value()) { latestText = textOpt.value(); updated = true; }
            }
            if (updated) { state.openDocuments[uriOpt.value()] = latestText; publishDocumentDiagnostics(state, uriOpt.value(), latestText, out); }
        }
        return;
    }
    if (method == "textDocument/didClose") {
        const auto uriOpt = jsonGetString(parsed.arena, jsonGetField(parsed.arena, jsonGetField(parsed.arena, paramsId, "textDocument"), "uri"));
        if (uriOpt.has_value()) { state.openDocuments.erase(uriOpt.value()); state.snapshots.erase(uriOpt.value()); publishEmptyDiagnostics(uriOpt.value(), out); }
        return;
    }
    if (method == "textDocument/documentSymbol") { handleDocumentSymbolRequest(state, parsed.arena, reqId, paramsId, out); return; }
    if (method == "textDocument/hover") { handleHoverRequest(state, parsed.arena, reqId, paramsId, out); return; }
    if (method == "textDocument/definition") { handleDefinitionRequest(state, parsed.arena, reqId, paramsId, out); return; }
    if (method == "textDocument/completion") { handleCompletionRequest(state, parsed.arena, reqId, paramsId, out); return; }
    if (method == "textDocument/signatureHelp") { handleSignatureHelpRequest(state, parsed.arena, reqId, paramsId, out); return; }

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
        if (!msgOpt.has_value()) break;
        handleLspMessage(state, msgOpt.value(), out);
    }
    return state.exitCode;
}

} // namespace cb

#endif // CLEANBASIC_LSP_HPP
