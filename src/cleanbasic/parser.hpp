#ifndef CLEANBASIC_PARSER_HPP
#define CLEANBASIC_PARSER_HPP

#include "src/cleanbasic/ast.hpp"
#include "src/cleanbasic/diagnostics.hpp"
#include "src/cleanbasic/lexer.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace cb {

struct ParserState {
    size_t pos = 0;
    std::string fileName = "main.cb";
    AstArena arena;
    DiagnosticError error;
    bool hasError = false;
};

struct ParseResult {
    bool ok = false;
    Program program;
    AstArena arena;
    DiagnosticError error;
};

// ==========================================
// Error and Token Navigation Helpers
// ==========================================

inline void setParseError(ParserState& state, const Token& tok, std::string_view msg) {
    if (!state.hasError) {
        state.hasError = true;
        std::string fullMsg = std::string(msg) + " (got " + tok.kind + " '" + tok.value + "')";
        state.error = makeError("ParseError", fullMsg, state.fileName, tok.line, tok.col);
    }
}

inline const Token& current(const std::vector<Token>& tokens, const ParserState& state) {
    static const Token emptyEof{"EOF", "", 1, 1};
    if (tokens.empty()) {
        return emptyEof;
    }
    if (state.pos < tokens.size()) {
        return tokens[state.pos];
    }
    return tokens.back();
}

inline const Token& peekOffset(const std::vector<Token>& tokens, const ParserState& state, size_t offset) {
    static const Token emptyEof{"EOF", "", 1, 1};
    if (tokens.empty()) {
        return emptyEof;
    }
    const size_t idx = state.pos + offset;
    if (idx < tokens.size()) {
        return tokens[idx];
    }
    return tokens.back();
}

inline bool check(const std::vector<Token>& tokens, const ParserState& state, std::string_view kind, std::string_view value = "") {
    if (state.hasError) {
        return false;
    }
    const Token& tok = current(tokens, state);
    if (tok.kind != kind) {
        return false;
    }
    if (!value.empty() && tok.value != value) {
        return false;
    }
    return true;
}

inline bool checkAnyIdentOrType(const std::vector<Token>& tokens, const ParserState& state) {
    if (state.hasError) {
        return false;
    }
    const Token& tok = current(tokens, state);
    return tok.kind == "IDENT" || tok.kind == "TYPE_KW" || (tok.kind == "KEYWORD" && tok.value == "error");
}

inline const Token& match(const std::vector<Token>& tokens, ParserState& state, std::string_view kind, std::string_view value = "") {
    static const Token fallbackTok{"EOF", "", 1, 1};
    if (state.hasError) {
        return fallbackTok;
    }
    const Token& tok = current(tokens, state);
    if (!check(tokens, state, kind, value)) {
        std::string expected = "Expected " + std::string(kind);
        if (!value.empty()) {
            expected += " ('" + std::string(value) + "')";
        }
        setParseError(state, tok, expected);
        return tok;
    }
    state.pos += 1;
    return tok;
}

inline const Token& matchIdentOrType(const std::vector<Token>& tokens, ParserState& state) {
    static const Token fallbackTok{"EOF", "", 1, 1};
    if (state.hasError) {
        return fallbackTok;
    }
    const Token& tok = current(tokens, state);
    if (tok.kind == "IDENT" || tok.kind == "TYPE_KW" || (tok.kind == "KEYWORD" && tok.value == "error")) {
        state.pos += 1;
        return tok;
    }
    setParseError(state, tok, "Expected identifier or type name");
    return tok;
}

inline void skipNewlines(const std::vector<Token>& tokens, ParserState& state) {
    while (!state.hasError && check(tokens, state, "NEWLINE")) {
        state.pos += 1;
    }
}

inline void expectNewlineOrEof(const std::vector<Token>& tokens, ParserState& state) {
    if (state.hasError) {
        return;
    }
    if (check(tokens, state, "NEWLINE")) {
        skipNewlines(tokens, state);
    } else if (check(tokens, state, "EOF")) {
        return;
    } else {
        setParseError(state, current(tokens, state), "Expected end of line (NEWLINE)");
    }
}

inline std::string stripQuotes(std::string_view raw) {
    constexpr char singleQuote = static_cast<char>(39);
    if (raw.size() >= 2 && (raw.front() == '"' || raw.front() == singleQuote) && raw.back() == raw.front()) {
        return std::string(raw.substr(1, raw.size() - 2));
    }
    return std::string(raw);
}

// Forward declarations
inline TypeId parseType(const std::vector<Token>& tokens, ParserState& state);
inline ExprId parseExpr(const std::vector<Token>& tokens, ParserState& state);
inline StmtId parseStatement(const std::vector<Token>& tokens, ParserState& state);

// ==========================================
// Argument & Struct Field Parsing Helpers
// ==========================================

inline void parseCallArguments(
    const std::vector<Token>& tokens,
    ParserState& state,
    std::string_view closingKind,
    std::vector<ExprId>& posArgs,
    std::vector<NamedArg>& namedArgs
) {
    if (check(tokens, state, closingKind)) {
        return;
    }

    while (!state.hasError) {
        const Token& peekTok = peekOffset(tokens, state, 1);
        if (check(tokens, state, "IDENT") && peekTok.kind == "COLON") {
            setParseError(state, peekTok, "Named arguments must use '=' (e.g., name=value), not ':'");
            return;
        }
        if (check(tokens, state, "IDENT") && peekTok.kind == "ASSIGN") {
            const Token nameTok = match(tokens, state, "IDENT");
            state.pos += 1;  // consume '='
            const ExprId val = parseExpr(tokens, state);
            NamedArg arg{};
            arg.name = nameTok.value;
            arg.value = val;
            namedArgs.push_back(std::move(arg));
        } else {
            posArgs.push_back(parseExpr(tokens, state));
        }

        if (check(tokens, state, "COMMA")) {
            match(tokens, state, "COMMA");
            if (check(tokens, state, closingKind)) {
                break;
            }
        } else {
            break;
        }
    }
}


// ==========================================
// Expression Precedence Climbing
// ==========================================

inline ExprId parseExpr(const std::vector<Token>& tokens, ParserState& state);

inline ExprId parsePrimary(const std::vector<Token>& tokens, ParserState& state) {
    if (state.hasError) {
        return INVALID_ID;
    }
    const Token& tok = current(tokens, state);


    if (check(tokens, state, "NUMBER")) {
        const Token numTok = match(tokens, state, "NUMBER");
        return addExpr(state.arena, numTok.line, numTok.col, NumberLiteral{numTok.value, numTok.value.find('.') != std::string::npos});
    }
    if (check(tokens, state, "STRING")) {
        const Token strTok = match(tokens, state, "STRING");
        return addExpr(state.arena, strTok.line, strTok.col, StringLiteral{stripQuotes(strTok.value)});
    }
    if (check(tokens, state, "KEYWORD", "true") || check(tokens, state, "KEYWORD", "false")) {
        const Token bTok = match(tokens, state, "KEYWORD");
        return addExpr(state.arena, bTok.line, bTok.col, BoolLiteral{bTok.value == "true"});
    }
    if (check(tokens, state, "KEYWORD", "null")) {
        const Token nTok = match(tokens, state, "KEYWORD", "null");
        return addExpr(state.arena, nTok.line, nTok.col, NullLiteral{});
    }

    if (check(tokens, state, "LPAREN")) {
        match(tokens, state, "LPAREN");
        const ExprId inner = parseExpr(tokens, state);
        match(tokens, state, "RPAREN");
        return inner;
    }

    if (check(tokens, state, "LBRACKET")) {
        const Token bTok = match(tokens, state, "LBRACKET");
        std::vector<ExprId> elements;
        if (!check(tokens, state, "RBRACKET")) {
            elements.push_back(parseExpr(tokens, state));
            while (check(tokens, state, "COMMA")) {
                match(tokens, state, "COMMA");
                if (check(tokens, state, "RBRACKET")) {
                    break;
                }
                elements.push_back(parseExpr(tokens, state));
            }
        }
        match(tokens, state, "RBRACKET");
        ListLiteral node{};
        node.elements = std::move(elements);
        return addExpr(state.arena, bTok.line, bTok.col, std::move(node));
    }

    if (check(tokens, state, "LBRACE")) {
        const Token bTok = match(tokens, state, "LBRACE");
        std::vector<DictEntry> entries;
        if (!check(tokens, state, "RBRACE")) {
            DictEntry first{};
            first.key = parseExpr(tokens, state);
            match(tokens, state, "COLON");
            first.value = parseExpr(tokens, state);
            entries.push_back(std::move(first));

            while (check(tokens, state, "COMMA")) {
                match(tokens, state, "COMMA");
                if (check(tokens, state, "RBRACE")) {
                    break;
                }
                DictEntry entry{};
                entry.key = parseExpr(tokens, state);
                match(tokens, state, "COLON");
                entry.value = parseExpr(tokens, state);
                entries.push_back(std::move(entry));
            }
        }
        match(tokens, state, "RBRACE");
        DictLiteral node{};
        node.entries = std::move(entries);
        return addExpr(state.arena, bTok.line, bTok.col, std::move(node));
    }

    if (checkAnyIdentOrType(tokens, state)) {
        const Token idTok = matchIdentOrType(tokens, state);
        Identifier node{};
        node.name = idTok.value;
        return addExpr(state.arena, idTok.line, idTok.col, std::move(node));
    }

    setParseError(state, tok, "Expected expression");
    return INVALID_ID;
}

inline ExprId parsePostfix(const std::vector<Token>& tokens, ParserState& state) {
    ExprId expr = parsePrimary(tokens, state);

    while (!state.hasError) {
        if (check(tokens, state, "DOT")) {
            match(tokens, state, "DOT");
            const Token fieldTok = matchIdentOrType(tokens, state);
            FieldAccess node{};
            node.target = expr;
            node.fieldName = fieldTok.value;
            expr = addExpr(state.arena, fieldTok.line, fieldTok.col, std::move(node));
        } else if (check(tokens, state, "LBRACKET")) {
            const Token bTok = match(tokens, state, "LBRACKET");
            const ExprId idxExpr = parseExpr(tokens, state);
            match(tokens, state, "RBRACKET");
            IndexAccess node{};
            node.target = expr;
            node.index = idxExpr;
            expr = addExpr(state.arena, bTok.line, bTok.col, std::move(node));
        } else if (check(tokens, state, "LPAREN")) {
            const Token pTok = match(tokens, state, "LPAREN");
            std::vector<ExprId> posArgs;
            std::vector<NamedArg> namedArgs;
            parseCallArguments(tokens, state, "RPAREN", posArgs, namedArgs);
            match(tokens, state, "RPAREN");
            FuncCall node{};
            node.callee = expr;
            node.args = std::move(posArgs);
            node.namedArgs = std::move(namedArgs);
            expr = addExpr(state.arena, pTok.line, pTok.col, std::move(node));
        } else {
            break;
        }
    }
    return expr;
}

inline ExprId parseUnary(const std::vector<Token>& tokens, ParserState& state) {
    if (state.hasError) {
        return INVALID_ID;
    }
    if (check(tokens, state, "OP_ARITH", "-") || check(tokens, state, "OP_ARITH", "+")) {
        const Token opTok = match(tokens, state, "OP_ARITH");
        const ExprId operand = parseUnary(tokens, state);
        UnaryOp node{};
        node.op = opTok.value;
        node.operand = operand;
        return addExpr(state.arena, opTok.line, opTok.col, std::move(node));
    }
    return parsePostfix(tokens, state);
}

template <typename ParseOperandFn>
inline ExprId parseBinaryOpLoop(
    const std::vector<Token>& tokens,
    ParserState& state,
    std::string_view tokenKind,
    std::initializer_list<std::string_view> allowedOps,
    ParseOperandFn parseOperand
) {
    ExprId left = parseOperand(tokens, state);
    while (!state.hasError) {
        bool matchedOp = false;
        if (allowedOps.size() == 0) {
            matchedOp = check(tokens, state, tokenKind);
        } else {
            for (const std::string_view op : allowedOps) {
                if (check(tokens, state, tokenKind, op)) {
                    matchedOp = true;
                    break;
                }
            }
        }
        if (!matchedOp) {
            break;
        }
        const Token opTok = match(tokens, state, tokenKind);
        const ExprId right = parseOperand(tokens, state);
        left = addExpr(state.arena, opTok.line, opTok.col, BinaryOp{opTok.value, left, right});
    }
    return left;
}

inline ExprId parseMultiplicative(const std::vector<Token>& tokens, ParserState& state) {
    return parseBinaryOpLoop(tokens, state, "OP_ARITH", {"*", "/"}, parseUnary);
}

inline ExprId parseAdditive(const std::vector<Token>& tokens, ParserState& state) {
    return parseBinaryOpLoop(tokens, state, "OP_ARITH", {"+", "-", "&"}, parseMultiplicative);
}

inline ExprId parseComparison(const std::vector<Token>& tokens, ParserState& state) {
    return parseBinaryOpLoop(tokens, state, "OP_CMP", {}, parseAdditive);
}

inline ExprId parseNot(const std::vector<Token>& tokens, ParserState& state) {
    if (state.hasError) {
        return INVALID_ID;
    }
    if (check(tokens, state, "KEYWORD", "not")) {
        const Token opTok = match(tokens, state, "KEYWORD", "not");
        const ExprId operand = parseNot(tokens, state);
        return addExpr(state.arena, opTok.line, opTok.col, UnaryOp{"not", operand});
    }
    return parseComparison(tokens, state);
}

inline ExprId parseAnd(const std::vector<Token>& tokens, ParserState& state) {
    return parseBinaryOpLoop(tokens, state, "KEYWORD", {"and"}, parseNot);
}

inline ExprId parseOr(const std::vector<Token>& tokens, ParserState& state) {
    return parseBinaryOpLoop(tokens, state, "KEYWORD", {"or"}, parseAnd);
}

inline ExprId parseExpr(const std::vector<Token>& tokens, ParserState& state) {
    return parseOr(tokens, state);
}

// ==========================================
// Types
// ==========================================

inline TypeId parseType(const std::vector<Token>& tokens, ParserState& state) {
    if (state.hasError) {
        return INVALID_ID;
    }
    const Token start = current(tokens, state);
    const Token tok = matchIdentOrType(tokens, state);
    std::string typeName = tok.value;

    while (check(tokens, state, "DOT")) {
        match(tokens, state, "DOT");
        const Token part = match(tokens, state, "IDENT");
        typeName += "." + part.value;
    }

    std::vector<TypeId> typeArgs;
    if (typeName == "list" && check(tokens, state, "LBRACKET")) {
        match(tokens, state, "LBRACKET");
        typeArgs.push_back(parseType(tokens, state));
        match(tokens, state, "RBRACKET");
    } else if (typeName == "dict" && check(tokens, state, "LBRACKET")) {
        match(tokens, state, "LBRACKET");
        typeArgs.push_back(parseType(tokens, state));
        match(tokens, state, "COMMA");
        typeArgs.push_back(parseType(tokens, state));
        match(tokens, state, "RBRACKET");
    }

    TypeNode node{};
    node.line = start.line;
    node.col = start.col;
    node.name = std::move(typeName);
    node.typeArgs = std::move(typeArgs);
    return addType(state.arena, std::move(node));
}

// ==========================================
// Statements
// ==========================================

inline StmtId parseAssignmentOrExprStmt(const std::vector<Token>& tokens, ParserState& state) {
    if (state.hasError) {
        return INVALID_ID;
    }
    const Token start = current(tokens, state);

    if (check(tokens, state, "KEYWORD", "dim") || check(tokens, state, "KEYWORD", "const")) {
        const Token declKw = match(tokens, state, "KEYWORD");
        const bool isDim = (declKw.value == "dim");
        const bool isConst = (declKw.value == "const");

        const Token firstIdent = match(tokens, state, "IDENT");
        Identifier firstIdNode{};
        firstIdNode.name = firstIdent.value;
        const ExprId targetExpr = addExpr(state.arena, firstIdent.line, firstIdent.col, std::move(firstIdNode));

        bool hasDeclaredType = false;
        TypeId declType = INVALID_ID;
        if (check(tokens, state, "KEYWORD", "as")) {
            match(tokens, state, "KEYWORD", "as");
            hasDeclaredType = true;
            declType = parseType(tokens, state);
        }

        match(tokens, state, "ASSIGN");
        const ExprId valExpr = parseExpr(tokens, state);
        expectNewlineOrEof(tokens, state);

        VarAssignment stmt{};
        stmt.target = targetExpr;
        stmt.isDim = isDim;
        stmt.isConst = isConst;
        stmt.hasDeclaredType = hasDeclaredType;
        stmt.declaredType = declType;
        stmt.value = valExpr;
        return addStmt(state.arena, start.line, start.col, std::move(stmt));
    }

    const ExprId firstExpr = parseExpr(tokens, state);

    if (check(tokens, state, "ASSIGN")) {
        match(tokens, state, "ASSIGN");
        const ExprId valExpr = parseExpr(tokens, state);
        expectNewlineOrEof(tokens, state);

        VarAssignment stmt{};
        stmt.target = firstExpr;
        stmt.hasDeclaredType = false;
        stmt.declaredType = INVALID_ID;
        stmt.value = valExpr;
        return addStmt(state.arena, start.line, start.col, std::move(stmt));
    }

    expectNewlineOrEof(tokens, state);

    ExprStmt stmt{};
    stmt.expr = firstExpr;
    return addStmt(state.arena, start.line, start.col, std::move(stmt));
}

inline std::vector<StmtId> parseStmtBlockUntil(
    const std::vector<Token>& tokens,
    ParserState& state,
    std::initializer_list<std::string_view> endKeywords,
    std::string_view unclosedMsg
) {
    std::vector<StmtId> body;
    while (!state.hasError) {
        bool reachedEnd = false;
        for (const std::string_view kw : endKeywords) {
            if (check(tokens, state, "KEYWORD", kw)) {
                reachedEnd = true;
                break;
            }
        }
        if (reachedEnd) {
            break;
        }
        if (check(tokens, state, "EOF")) {
            setParseError(state, current(tokens, state), unclosedMsg);
            break;
        }
        body.push_back(parseStatement(tokens, state));
        skipNewlines(tokens, state);
    }
    return body;
}

inline StmtId parseIfStmt(const std::vector<Token>& tokens, ParserState& state) {
    const Token start = match(tokens, state, "KEYWORD", "if");
    const ExprId condition = parseExpr(tokens, state);
    expectNewlineOrEof(tokens, state);

    std::vector<StmtId> thenBody = parseStmtBlockUntil(
        tokens, state, {"else if", "else", "end if"}, "Unclosed 'if' statement, expected 'end if'"
    );

    std::vector<ElseIfBranch> elifBranches;
    while (!state.hasError && check(tokens, state, "KEYWORD", "else if")) {
        const Token elifTok = match(tokens, state, "KEYWORD", "else if");
        const ExprId elifCond = parseExpr(tokens, state);
        expectNewlineOrEof(tokens, state);
        std::vector<StmtId> elifBody = parseStmtBlockUntil(
            tokens, state, {"else if", "else", "end if"}, "Unclosed 'else if' block, expected 'end if'"
        );
        elifBranches.push_back(ElseIfBranch{elifTok.line, elifTok.col, elifCond, std::move(elifBody)});
    }

    bool hasElse = false;
    std::vector<StmtId> elseBody;
    if (check(tokens, state, "KEYWORD", "else")) {
        match(tokens, state, "KEYWORD", "else");
        expectNewlineOrEof(tokens, state);
        hasElse = true;
        elseBody = parseStmtBlockUntil(tokens, state, {"end if"}, "Unclosed 'else' block, expected 'end if'");
    }

    match(tokens, state, "KEYWORD", "end if");
    expectNewlineOrEof(tokens, state);
    return addStmt(state.arena, start.line, start.col, IfStmt{condition, std::move(thenBody), std::move(elifBranches), hasElse, std::move(elseBody)});
}

inline StmtId parseForStmt(const std::vector<Token>& tokens, ParserState& state) {
    const Token start = match(tokens, state, "KEYWORD", "for");
    std::vector<std::string> loopVars;
    loopVars.push_back(match(tokens, state, "IDENT").value);
    while (check(tokens, state, "COMMA")) {
        match(tokens, state, "COMMA");
        loopVars.push_back(match(tokens, state, "IDENT").value);
    }

    RangeSpec rangeSpec{};
    bool hasIterable = false;
    ExprId iterable = INVALID_ID;

    if (check(tokens, state, "ASSIGN")) {
        if (loopVars.size() != 1) {
            setParseError(state, start, "Range 'for' loop only supports a single loop variable");
            return INVALID_ID;
        }
        const Token eqTok = match(tokens, state, "ASSIGN");
        const ExprId startExpr = parseExpr(tokens, state);
        match(tokens, state, "KEYWORD", "to");
        const ExprId endExpr = parseExpr(tokens, state);
        bool hasStep = false;
        ExprId stepExpr = INVALID_ID;
        if (check(tokens, state, "KEYWORD", "step")) {
            match(tokens, state, "KEYWORD", "step");
            hasStep = true;
            stepExpr = parseExpr(tokens, state);
        }
        rangeSpec = RangeSpec{true, eqTok.line, eqTok.col, startExpr, endExpr, hasStep, stepExpr, true};
    } else {
        match(tokens, state, "KEYWORD", "in");
        hasIterable = true;
        iterable = parseExpr(tokens, state);
    }

    expectNewlineOrEof(tokens, state);
    std::vector<StmtId> body = parseStmtBlockUntil(tokens, state, {"end for"}, "Unclosed 'for' loop, expected 'end for'");
    match(tokens, state, "KEYWORD", "end for");
    expectNewlineOrEof(tokens, state);

    return addStmt(state.arena, start.line, start.col, ForStmt{std::move(loopVars), rangeSpec, hasIterable, iterable, std::move(body)});
}

inline StmtId parseSelectStmt(const std::vector<Token>& tokens, ParserState& state) {
    const Token start = match(tokens, state, "KEYWORD", "select case");
    const ExprId target = parseExpr(tokens, state);
    expectNewlineOrEof(tokens, state);

    std::vector<SelectCase> cases;
    while (!state.hasError && check(tokens, state, "KEYWORD", "case")) {
        const Token caseTok = match(tokens, state, "KEYWORD", "case");
        const ExprId matchExpr = parseExpr(tokens, state);
        expectNewlineOrEof(tokens, state);
        std::vector<StmtId> caseBody = parseStmtBlockUntil(
            tokens, state, {"case", "case else", "end select"}, "Unclosed 'case' in 'select case', expected 'end select'"
        );
        cases.push_back(SelectCase{caseTok.line, caseTok.col, matchExpr, std::move(caseBody)});
    }

    bool hasCaseElse = false;
    std::vector<StmtId> caseElse;
    if (check(tokens, state, "KEYWORD", "case else")) {
        match(tokens, state, "KEYWORD", "case else");
        expectNewlineOrEof(tokens, state);
        hasCaseElse = true;
        caseElse = parseStmtBlockUntil(tokens, state, {"end select"}, "Unclosed 'case else', expected 'end select'");
    }

    match(tokens, state, "KEYWORD", "end select");
    expectNewlineOrEof(tokens, state);
    return addStmt(state.arena, start.line, start.col, SelectStmt{target, std::move(cases), hasCaseElse, std::move(caseElse)});
}

inline StmtId parseOnErrorStmt(const std::vector<Token>& tokens, ParserState& state) {
    const Token start = match(tokens, state, "KEYWORD", "on error");
    const Token errIdent = match(tokens, state, "IDENT");
    expectNewlineOrEof(tokens, state);
    std::vector<StmtId> body = parseStmtBlockUntil(tokens, state, {"end error"}, "Unclosed 'on error' block, expected 'end error'");
    match(tokens, state, "KEYWORD", "end error");
    expectNewlineOrEof(tokens, state);
    return addStmt(state.arena, start.line, start.col, ErrorBlock{errIdent.value, std::move(body)});
}

inline StmtId parseDeferStmt(const std::vector<Token>& tokens, ParserState& state) {
    const Token start = match(tokens, state, "KEYWORD", "defer");
    expectNewlineOrEof(tokens, state);
    std::vector<StmtId> body = parseStmtBlockUntil(tokens, state, {"end defer"}, "Unclosed 'defer' block, expected 'end defer'");
    match(tokens, state, "KEYWORD", "end defer");
    expectNewlineOrEof(tokens, state);
    return addStmt(state.arena, start.line, start.col, DeferStmt{std::move(body)});
}

inline StmtId parseWhileStmt(const std::vector<Token>& tokens, ParserState& state) {
    const Token start = match(tokens, state, "KEYWORD", "while");
    const ExprId cond = parseExpr(tokens, state);
    expectNewlineOrEof(tokens, state);
    std::vector<StmtId> body = parseStmtBlockUntil(tokens, state, {"end while"}, "Unclosed 'while' block, expected 'end while'");
    match(tokens, state, "KEYWORD", "end while");
    expectNewlineOrEof(tokens, state);
    return addStmt(state.arena, start.line, start.col, WhileStmt{cond, std::move(body)});
}

inline StmtId parseReturnStmt(const std::vector<Token>& tokens, ParserState& state) {
    const Token start = match(tokens, state, "KEYWORD", "return");
    ReturnStmt stmt{};
    if (!check(tokens, state, "NEWLINE") && !check(tokens, state, "EOF")) {
        stmt.hasValue = true;
        stmt.value = parseExpr(tokens, state);
    }
    expectNewlineOrEof(tokens, state);

    return addStmt(state.arena, start.line, start.col, std::move(stmt));
}

inline StmtId parseBreakStmt(const std::vector<Token>& tokens, ParserState& state) {
    const Token start = match(tokens, state, "KEYWORD", "break");
    expectNewlineOrEof(tokens, state);

    BreakStmt stmt{};
    return addStmt(state.arena, start.line, start.col, std::move(stmt));
}

inline StmtId parseStatement(const std::vector<Token>& tokens, ParserState& state) {
    skipNewlines(tokens, state);
    if (state.hasError) {
        return INVALID_ID;
    }

    if (check(tokens, state, "KEYWORD", "if")) {
        return parseIfStmt(tokens, state);
    }
    if (check(tokens, state, "KEYWORD", "for")) {
        return parseForStmt(tokens, state);
    }
    if (check(tokens, state, "KEYWORD", "while")) {
        return parseWhileStmt(tokens, state);
    }
    if (check(tokens, state, "KEYWORD", "select case")) {
        return parseSelectStmt(tokens, state);
    }
    if (check(tokens, state, "KEYWORD", "on error")) {
        return parseOnErrorStmt(tokens, state);
    }
    if (check(tokens, state, "KEYWORD", "defer")) {
        return parseDeferStmt(tokens, state);
    }
    if (check(tokens, state, "KEYWORD", "return")) {
        return parseReturnStmt(tokens, state);
    }
    if (check(tokens, state, "KEYWORD", "break")) {
        return parseBreakStmt(tokens, state);
    }

    return parseAssignmentOrExprStmt(tokens, state);
}

// ==========================================
// Top-Level Declarations
// ==========================================

inline DeclId parseImportDecl(const std::vector<Token>& tokens, ParserState& state) {
    const Token start = match(tokens, state, "KEYWORD", "import");
    std::string modulePath;
    if (check(tokens, state, "STRING")) {
        const Token strTok = match(tokens, state, "STRING");
        modulePath = stripQuotes(strTok.value);
    } else {
        modulePath = match(tokens, state, "IDENT").value;
        while (check(tokens, state, "DOT")) {
            match(tokens, state, "DOT");
            modulePath += "." + match(tokens, state, "IDENT").value;
        }
    }

    bool hasAlias = false;
    std::string alias;
    if (check(tokens, state, "KEYWORD", "as")) {
        match(tokens, state, "KEYWORD", "as");
        hasAlias = true;
        alias = match(tokens, state, "IDENT").value;
    }
    expectNewlineOrEof(tokens, state);

    ImportDecl decl{};
    decl.kind = "module";
    decl.target = std::move(modulePath);
    decl.hasAlias = hasAlias;
    decl.alias = std::move(alias);
    return addDecl(state.arena, start.line, start.col, std::move(decl));
}

inline DeclId parseImportHeaderDecl(const std::vector<Token>& tokens, ParserState& state) {
    const Token start = match(tokens, state, "KEYWORD", "import header");
    const Token strTok = match(tokens, state, "STRING");
    expectNewlineOrEof(tokens, state);

    ImportDecl decl{};
    decl.kind = "header";
    decl.target = stripQuotes(strTok.value);
    return addDecl(state.arena, start.line, start.col, std::move(decl));
}

inline DeclId parseImportLinkDecl(const std::vector<Token>& tokens, ParserState& state) {
    const Token start = match(tokens, state, "KEYWORD", "import link");
    const Token strTok = match(tokens, state, "STRING");
    expectNewlineOrEof(tokens, state);

    ImportDecl decl{};
    decl.kind = "link";
    decl.target = stripQuotes(strTok.value);
    return addDecl(state.arena, start.line, start.col, std::move(decl));
}

inline DeclId parseAliasDecl(const std::vector<Token>& tokens, ParserState& state, bool isPub, const Token& startTok) {
    match(tokens, state, "KEYWORD", "alias");
    const Token nameTok = match(tokens, state, "IDENT");
    match(tokens, state, "ASSIGN");
    const TypeId targetType = parseType(tokens, state);
    expectNewlineOrEof(tokens, state);

    AliasDecl decl{};
    decl.name = nameTok.value;
    decl.targetType = targetType;
    decl.isPub = isPub;
    return addDecl(state.arena, startTok.line, startTok.col, std::move(decl), "", state.fileName, nameTok.line, nameTok.line, nameTok.col);
}

inline DeclId parseTypeDecl(const std::vector<Token>& tokens, ParserState& state, bool isPub, bool isExtern, const Token& startTok) {
    match(tokens, state, "KEYWORD", "type");
    const Token nameTok = match(tokens, state, "IDENT");
    expectNewlineOrEof(tokens, state);

    std::vector<FieldDecl> fields;
    if (isExtern && !check(tokens, state, "KEYWORD", "end type")) {
        if (check(tokens, state, "EOF") ||
            check(tokens, state, "KEYWORD", "pub") ||
            check(tokens, state, "KEYWORD", "extern") ||
            check(tokens, state, "KEYWORD", "fn") ||
            check(tokens, state, "KEYWORD", "type") ||
            check(tokens, state, "KEYWORD", "alias") ||
            check(tokens, state, "KEYWORD", "import") ||
            check(tokens, state, "KEYWORD", "import header") ||
            check(tokens, state, "KEYWORD", "import link")) {
            TypeDecl decl{};
            decl.name = nameTok.value;
            decl.fields = std::move(fields);
            decl.isPub = isPub;
            decl.isExtern = isExtern;
            return addDecl(state.arena, startTok.line, startTok.col, std::move(decl), "", state.fileName, nameTok.line, nameTok.line, nameTok.col);
        }
    }

    while (!state.hasError && !check(tokens, state, "KEYWORD", "end type")) {
        if (check(tokens, state, "EOF")) {
            setParseError(state, current(tokens, state), "Unclosed 'type " + nameTok.value + "', expected 'end type'");
            break;
        }
        const Token fieldTok = match(tokens, state, "IDENT");
        match(tokens, state, "KEYWORD", "as");
        const TypeId fieldType = parseType(tokens, state);
        FieldDecl fd{};
        fd.line = fieldTok.line;
        fd.col = fieldTok.col;
        fd.name = fieldTok.value;
        fd.typeId = fieldType;
        fields.push_back(std::move(fd));
        expectNewlineOrEof(tokens, state);
    }

    const Token endTok = match(tokens, state, "KEYWORD", "end type");
    expectNewlineOrEof(tokens, state);

    TypeDecl decl{};
    decl.name = nameTok.value;
    decl.fields = std::move(fields);
    decl.isPub = isPub;
    decl.isExtern = isExtern;
    return addDecl(state.arena, startTok.line, startTok.col, std::move(decl), "", state.fileName, endTok.line, nameTok.line, nameTok.col);
}

inline Param parseParam(const std::vector<Token>& tokens, ParserState& state) {
    bool isByref = false;
    if (check(tokens, state, "KEYWORD", "byref")) {
        match(tokens, state, "KEYWORD", "byref");
        isByref = true;
    }
    const Token nameTok = match(tokens, state, "IDENT");
    match(tokens, state, "KEYWORD", "as");
    const TypeId tNode = parseType(tokens, state);
    Param p{};
    p.line = nameTok.line;
    p.col = nameTok.col;
    p.name = nameTok.value;
    p.typeId = tNode;
    p.isByref = isByref;
    return p;
}

inline DeclId parseFnDecl(const std::vector<Token>& tokens, ParserState& state, bool isPub, bool isExtern, const Token& startTok) {
    match(tokens, state, "KEYWORD", "fn");
    const Token nameTok = match(tokens, state, "IDENT");
    match(tokens, state, "LPAREN");

    std::vector<Param> params;
    if (!check(tokens, state, "RPAREN")) {
        params.push_back(parseParam(tokens, state));
        while (check(tokens, state, "COMMA")) {
            match(tokens, state, "COMMA");
            if (check(tokens, state, "RPAREN")) {
                break;
            }
            params.push_back(parseParam(tokens, state));
        }
    }
    match(tokens, state, "RPAREN");

    TypeId returnType = findOrAddType(state.arena, "void", {}, startTok.line, startTok.col);
    bool isFallible = false;
    if (check(tokens, state, "KEYWORD", "as")) {
        match(tokens, state, "KEYWORD", "as");
        if (check(tokens, state, "TYPE_KW", "err")) {
            match(tokens, state, "TYPE_KW", "err");
            isFallible = true;
        } else {
            returnType = parseType(tokens, state);
        }
    }
    if (check(tokens, state, "KEYWORD", "can error")) {
        match(tokens, state, "KEYWORD", "can error");
        isFallible = true;
    }
    expectNewlineOrEof(tokens, state);

    std::vector<StmtId> body;
    if (isExtern && !check(tokens, state, "KEYWORD", "end fn")) {
        FnDecl decl{};
        decl.name = nameTok.value;
        decl.params = std::move(params);
        decl.returnType = returnType;
        decl.isFallible = isFallible;
        decl.body = std::move(body);
        decl.isPub = isPub;
        decl.isExtern = true;
        return addDecl(state.arena, startTok.line, startTok.col, std::move(decl), "", state.fileName, nameTok.line, nameTok.line, nameTok.col);
    }

    body = parseStmtBlockUntil(tokens, state, {"end fn"}, "Unclosed 'fn " + nameTok.value + "', expected 'end fn'");

    const Token endTok = match(tokens, state, "KEYWORD", "end fn");
    expectNewlineOrEof(tokens, state);

    FnDecl decl{};
    decl.name = nameTok.value;
    decl.params = std::move(params);
    decl.returnType = returnType;
    decl.isFallible = isFallible;
    decl.body = std::move(body);
    decl.isPub = isPub;
    decl.isExtern = isExtern;
    return addDecl(state.arena, startTok.line, startTok.col, std::move(decl), "", state.fileName, endTok.line, nameTok.line, nameTok.col);
}

inline DeclId parseTopLevelDecl(const std::vector<Token>& tokens, ParserState& state) {
    if (state.hasError) {
        return INVALID_ID;
    }
    const Token tok = current(tokens, state);

    if (check(tokens, state, "KEYWORD", "import")) {
        return parseImportDecl(tokens, state);
    }
    if (check(tokens, state, "KEYWORD", "import header")) {
        return parseImportHeaderDecl(tokens, state);
    }
    if (check(tokens, state, "KEYWORD", "import link")) {
        return parseImportLinkDecl(tokens, state);
    }

    bool isPub = false;
    bool isExtern = false;
    while (check(tokens, state, "KEYWORD", "pub") || check(tokens, state, "KEYWORD", "extern")) {
        if (check(tokens, state, "KEYWORD", "pub")) {
            match(tokens, state, "KEYWORD", "pub");
            isPub = true;
        } else if (check(tokens, state, "KEYWORD", "extern")) {
            match(tokens, state, "KEYWORD", "extern");
            isExtern = true;
        }
    }

    if (check(tokens, state, "KEYWORD", "alias")) {
        if (isExtern) {
            setParseError(state, current(tokens, state), "'extern' cannot be applied to 'alias'");
            return INVALID_ID;
        }
        return parseAliasDecl(tokens, state, isPub, tok);
    }

    if (check(tokens, state, "KEYWORD", "type")) {
        return parseTypeDecl(tokens, state, isPub, isExtern, tok);
    }

    if (check(tokens, state, "KEYWORD", "fn")) {
        return parseFnDecl(tokens, state, isPub, isExtern, tok);
    }

    setParseError(state, current(tokens, state), "Expected top-level declaration ('import', 'alias', 'type', or 'fn')");
    return INVALID_ID;
}

// ==========================================
// Main Parse Entry Point
// ==========================================

inline ParseResult parseTokensWithArena(
    const std::vector<Token>& tokens,
    AstArena arena,
    std::string_view fileName = "main.cb"
) {
    ParserState state{};
    state.fileName = std::string(fileName);
    state.arena = std::move(arena);

    Program prog{};
    prog.fileName = state.fileName;

    skipNewlines(tokens, state);
    while (!state.hasError && !check(tokens, state, "EOF")) {
        const DeclId declId = parseTopLevelDecl(tokens, state);
        if (state.hasError) {
            break;
        }
        prog.declarations.push_back(declId);
        skipNewlines(tokens, state);
    }

    ParseResult result{};
    if (state.hasError) {
        result.ok = false;
        result.error = std::move(state.error);
    } else {
        result.ok = true;
        result.program = std::move(prog);
        result.arena = std::move(state.arena);
    }
    return result;
}

inline ParseResult parseTokens(const std::vector<Token>& tokens, std::string_view fileName = "main.cb") {
    return parseTokensWithArena(tokens, AstArena{}, fileName);
}

}  // namespace cb

#endif  // CLEANBASIC_PARSER_HPP
