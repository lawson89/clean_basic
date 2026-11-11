#ifndef CLEANBASIC_PARSER_HPP
#define CLEANBASIC_PARSER_HPP

#include "src/cleanbasic/ast.hpp"
#include "src/cleanbasic/diagnostics.hpp"
#include "src/cleanbasic/lexer.hpp"
#include "src/cleanbasic/runtime_preamble.hpp"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace cb {

struct ParseResult {
    bool ok = false;
    Program program;
    AstArena arena;
    DiagnosticError error;
};

inline std::string stripQuotes(std::string_view raw) {
    constexpr char singleQuote = static_cast<char>(39);
    constexpr char backslashChar = static_cast<char>(92);
    std::string_view inner = raw;
    if (raw.size() >= 2 && (raw.front() == '"' || raw.front() == singleQuote) && raw.back() == raw.front()) {
        inner = raw.substr(1, raw.size() - 2);
    }
    std::string out;
    out.reserve(inner.size());
    size_t i = 0;
    while (i < inner.size()) {
        if (inner[i] == backslashChar && i + 1 < inner.size()) {
            const char esc = inner[i + 1];
            out.push_back(backslashChar);
            if (esc == 'X') {
                out.push_back('x');
            } else {
                out.push_back(esc);
            }
            i += 2;
        } else {
            out.push_back(inner[i]);
            i += 1;
        }
    }
    return out;
}

inline int64_t parseIntegerLiteral(std::string_view raw, bool* okOut = nullptr) {
    if (okOut != nullptr) {
        *okOut = true;
    }
    const std::string cleaned(raw);
    try {
        size_t idx = 0;
        const long long val = std::stoll(cleaned, &idx, 10);
        if (idx != cleaned.size() && okOut != nullptr) {
            *okOut = false;
        }
        return static_cast<int64_t>(val);
    } catch (const std::exception&) {
        if (okOut != nullptr) {
            *okOut = false;
        }
        return 0;
    }
}

inline bool validateStringLiteralEscapes(std::string_view raw, std::string* errorOut = nullptr) {
    if (raw.size() < 2) {
        return true;
    }
    constexpr char backslashChar = 92;
    constexpr char singleQuote = static_cast<char>(39);
    size_t i = 1;
    const size_t end = raw.size() - 1;
    while (i < end) {
        if (raw[i] == backslashChar) {
            if (i + 1 >= end) {
                if (errorOut != nullptr) {
                    *errorOut = "Invalid escape sequence in string literal: trailing backslash.";
                }
                return false;
            }
            const char esc = raw[i + 1];
            if (esc == 'n' || esc == 'r' || esc == 't' || esc == '0' || esc == backslashChar || esc == '"' || esc == singleQuote) {
                i += 2;
                continue;
            }
            if (esc == 'x' || esc == 'X') {
                if (i + 3 >= end) {
                    if (errorOut != nullptr) {
                        *errorOut = "Invalid hexadecimal escape in string literal.";
                    }
                    return false;
                }
                const int v1 = cbHexDigitValue(raw[i + 2]);
                const int v2 = cbHexDigitValue(raw[i + 3]);
                if (v1 < 0 || v2 < 0) {
                    if (errorOut != nullptr) {
                        *errorOut = "Invalid hexadecimal escape in string literal.";
                    }
                    return false;
                }
                i += 4;
                continue;
            }
            if (errorOut != nullptr) {
                std::string msg = "Unknown escape sequence '";
                msg.push_back(backslashChar);
                msg.push_back(esc);
                msg += "' in string literal.";
                *errorOut = std::move(msg);
            }
            return false;
        }
        i += 1;
    }
    return true;
}


// ==========================================
// Stateful Parser Class
// ==========================================

class Parser {
public:
    const std::vector<Token>& tokens;
    size_t pos = 0;
    std::string fileName = "main.cb";
    AstArena arena;

    Parser(const std::vector<Token>& tokens, AstArena arena, std::string_view fileName = "main.cb")
        : tokens(tokens), pos(0), fileName(fileName), arena(std::move(arena)) {}

    [[noreturn]] void setParseError(const Token& tok, std::string_view msg) const {
        std::string fullMsg = std::string(msg) + " (got " + tok.kind + " '" + tok.value + "')";
        throw makeError("ParseError", fullMsg, fileName, tok.line, tok.col);
    }

    const Token& current() const {
        static const Token emptyEof{"EOF", "", 1, 1};
        if (tokens.empty()) {
            return emptyEof;
        }
        if (pos < tokens.size()) {
            return tokens[pos];
        }
        return tokens.back();
    }

    const Token& peekOffset(size_t offset) const {
        static const Token emptyEof{"EOF", "", 1, 1};
        if (tokens.empty()) {
            return emptyEof;
        }
        const size_t idx = pos + offset;
        if (idx < tokens.size()) {
            return tokens[idx];
        }
        return tokens.back();
    }

    bool check(std::string_view kind, std::string_view value = "") const {
        const Token& tok = current();
        if (tok.kind != kind) {
            return false;
        }
        if (!value.empty() && tok.value != value) {
            return false;
        }
        return true;
    }

    // Permits TYPE_KW (e.g. 'str', 'bytes', 'list') and 'error' in identifier positions so
    // static/module calls like 'bytes.fromHex(...)', 'str.join(...)', 'error(...)', and struct
    // fields or methods named after built-in types parse seamlessly.
    bool checkAnyIdentOrType() const {
        const Token& tok = current();
        return tok.kind == "IDENT" || tok.kind == "TYPE_KW" || (tok.kind == "KEYWORD" && tok.value == "error");
    }

    const Token& match(std::string_view kind, std::string_view value = "") {
        const Token& tok = current();
        if (!check(kind, value)) {
            std::string expected = "Expected " + std::string(kind);
            if (!value.empty()) {
                expected += " ('" + std::string(value) + "')";
            }
            setParseError(tok, expected);
        }
        pos += 1;
        return tok;
    }

    const Token& matchIdentOrType() {
        const Token& tok = current();
        if (tok.kind == "IDENT" || tok.kind == "TYPE_KW" || (tok.kind == "KEYWORD" && tok.value == "error")) {
            pos += 1;
            return tok;
        }
        setParseError(tok, "Expected identifier or type name");
    }

    template <typename T, typename ParseItemFn>
    std::vector<T> parseCommaSeparated(std::string_view closingTokKind, ParseItemFn parseItem) {
        std::vector<T> items;
        if (!check(closingTokKind)) {
            items.push_back(parseItem());
            while (check("COMMA")) {
                match("COMMA");
                if (check(closingTokKind)) {
                    break;
                }
                items.push_back(parseItem());
            }
        }
        match(closingTokKind);
        return items;
    }

    void skipNewlines() {
        while (check("NEWLINE")) {
            pos += 1;
        }
    }

    void expectNewlineOrEof() {
        if (check("NEWLINE")) {
            skipNewlines();
        } else if (check("EOF")) {
            return;
        } else {
            setParseError(current(), "Expected end of line (NEWLINE)");
        }
    }

    // ==========================================
    // Argument & Expression Parsing
    // ==========================================

    ExprId parsePrimary() {
        const Token& tok = current();

        if (check("NUMBER")) {
            const Token numTok = match("NUMBER");
            const bool isFloat = numTok.value.find('.') != std::string::npos;
            if (!isFloat) {
                bool ok = true;
                (void)parseIntegerLiteral(numTok.value, &ok);
                if (!ok) {
                    setParseError(numTok, "Integer literal '" + numTok.value + "' is out of range for 64-bit integer");
                }
            }
            return arena.addExpr(numTok.line, numTok.col, NumberLiteral{numTok.value, isFloat});
        }
        if (check("STRING")) {
            const Token strTok = match("STRING");
            std::string errStr;
            if (!validateStringLiteralEscapes(strTok.value, &errStr)) {
                setParseError(strTok, errStr);
            }
            return arena.addExpr(strTok.line, strTok.col, StringLiteral{stripQuotes(strTok.value)});
        }
        if (check("KEYWORD", "true") || check("KEYWORD", "false")) {
            const Token bTok = match("KEYWORD");
            return arena.addExpr(bTok.line, bTok.col, BoolLiteral{bTok.value == "true"});
        }

        if (check("LPAREN")) {
            match("LPAREN");
            const ExprId inner = parseExpr();
            match("RPAREN");
            return inner;
        }

        if (check("LBRACKET")) {
            const Token bTok = match("LBRACKET");
            ListLiteral node{};
            node.elements = parseCommaSeparated<ExprId>("RBRACKET", [&]() { return parseExpr(); });
            return arena.addExpr(bTok.line, bTok.col, std::move(node));
        }

        if (check("LBRACE")) {
            const Token bTok = match("LBRACE");
            DictLiteral node{};
            node.entries = parseCommaSeparated<DictEntry>("RBRACE", [&]() {
                DictEntry entry{};
                entry.key = parseExpr();
                match("COLON");
                entry.value = parseExpr();
                return entry;
            });
            return arena.addExpr(bTok.line, bTok.col, std::move(node));
        }

        if (checkAnyIdentOrType()) {
            const Token idTok = matchIdentOrType();
            Identifier node{};
            node.name = idTok.value;
            return arena.addExpr(idTok.line, idTok.col, std::move(node));
        }

        setParseError(tok, "Expected expression");
    }

    ExprId parsePostfix() {
        ExprId expr = parsePrimary();

        while (true) {
            if (check("DOT")) {
                match("DOT");
                const Token fieldTok = matchIdentOrType();
                FieldAccess node{};
                node.target = expr;
                node.fieldName = fieldTok.value;
                expr = arena.addExpr(fieldTok.line, fieldTok.col, std::move(node));
            } else if (check("LBRACKET")) {
                const Token bTok = match("LBRACKET");
                const ExprId idxExpr = parseExpr();
                match("RBRACKET");
                IndexAccess node{};
                node.target = expr;
                node.index = idxExpr;
                expr = arena.addExpr(bTok.line, bTok.col, std::move(node));
            } else if (check("LPAREN")) {
                const Token pTok = match("LPAREN");
                FuncCall node{};
                node.callee = expr;
                node.args = parseCommaSeparated<ExprId>("RPAREN", [&]() { return parseExpr(); });
                expr = arena.addExpr(pTok.line, pTok.col, std::move(node));
            } else {
                break;
            }
        }
        return expr;
    }

    ExprId parseUnary() {
        if (check("OP_ARITH", "-") || check("OP_ARITH", "+")) {
            const Token opTok = match("OP_ARITH");
            const ExprId operand = parseUnary();
            UnaryOp node{};
            node.op = opTok.value;
            node.operand = operand;
            return arena.addExpr(opTok.line, opTok.col, std::move(node));
        }
        return parsePostfix();
    }

    template <typename ParseOperandFn>
    ExprId parseBinaryOpLoop(
        std::string_view tokenKind,
        std::initializer_list<std::string_view> allowedOps,
        ParseOperandFn parseOperand
    ) {
        ExprId left = (this->*parseOperand)();
        while (true) {
            bool matchedOp = false;
            if (allowedOps.size() == 0) {
                matchedOp = check(tokenKind);
            } else {
                for (const std::string_view op : allowedOps) {
                    if (check(tokenKind, op)) {
                        matchedOp = true;
                        break;
                    }
                }
            }
            if (!matchedOp) {
                break;
            }
            const Token opTok = match(tokenKind);
            const ExprId right = (this->*parseOperand)();
            left = arena.addExpr(opTok.line, opTok.col, BinaryOp{opTok.value, left, right});
        }
        return left;
    }

    ExprId parseMultiplicative() {
        return parseBinaryOpLoop("OP_ARITH", {"*", "/", "%"}, &Parser::parseUnary);
    }

    ExprId parseAdditive() {
        return parseBinaryOpLoop("OP_ARITH", {"+", "-", "&"}, &Parser::parseMultiplicative);
    }

    ExprId parseComparison() {
        return parseBinaryOpLoop("OP_CMP", {}, &Parser::parseAdditive);
    }

    ExprId parseNot() {
        if (check("KEYWORD", "not")) {
            const Token opTok = match("KEYWORD", "not");
            const ExprId operand = parseNot();
            return arena.addExpr(opTok.line, opTok.col, UnaryOp{"not", operand});
        }
        return parseComparison();
    }

    ExprId parseAnd() {
        return parseBinaryOpLoop("KEYWORD", {"and"}, &Parser::parseNot);
    }

    ExprId parseOr() {
        return parseBinaryOpLoop("KEYWORD", {"or"}, &Parser::parseAnd);
    }

    ExprId parseExpr() {
        return parseOr();
    }

    // ==========================================
    // Type Parsing
    // ==========================================

    TypeId parseType() {
        const Token start = current();
        const Token tok = matchIdentOrType();
        std::string typeName = tok.value;

        while (check("DOT")) {
            match("DOT");
            const Token part = match("IDENT");
            typeName += "." + part.value;
        }

        std::vector<TypeId> typeArgs;
        if (typeName == "list" && check("LBRACKET")) {
            match("LBRACKET");
            typeArgs.push_back(parseType());
            match("RBRACKET");
        } else if (typeName == "dict" && check("LBRACKET")) {
            match("LBRACKET");
            const TypeId keyTid = parseType();
            typeArgs.push_back(keyTid);
            match("COMMA");
            typeArgs.push_back(parseType());
            match("RBRACKET");
            // Pre-intern list[K] whenever dict[K, V] is parsed so read-only type inference
            // (inferExprType / getBuiltinReceiverMethods) can always resolve dict.keys() -> list[K].
            arena.findOrAddType("list", {keyTid}, start.line, start.col);
        }

        TypeNode node{};
        node.line = start.line;
        node.col = start.col;
        node.name = std::move(typeName);
        node.typeArgs = std::move(typeArgs);
        return arena.addType(std::move(node));
    }

    // ==========================================
    // Statement Parsing
    // ==========================================

    StmtId parseAssignmentOrExprStmt() {
        const Token start = current();

        if (check("KEYWORD", "dim") || check("KEYWORD", "const")) {
            const Token declKw = match("KEYWORD");
            const bool isDim = (declKw.value == "dim");
            const bool isConst = (declKw.value == "const");

            const Token firstIdent = match("IDENT");
            Identifier firstIdNode{};
            firstIdNode.name = firstIdent.value;
            const ExprId targetExpr = arena.addExpr(firstIdent.line, firstIdent.col, std::move(firstIdNode));

            bool hasDeclaredType = false;
            TypeId declType = INVALID_ID;
            if (check("KEYWORD", "as")) {
                match("KEYWORD", "as");
                hasDeclaredType = true;
                declType = parseType();
            }

            match("ASSIGN");
            const ExprId valExpr = parseExpr();
            expectNewlineOrEof();

            VarAssignment stmt{};
            stmt.target = targetExpr;
            stmt.isDim = isDim;
            stmt.isConst = isConst;
            stmt.hasDeclaredType = hasDeclaredType;
            stmt.declaredType = declType;
            stmt.value = valExpr;
            return arena.addStmt(start.line, start.col, std::move(stmt));
        }

        const ExprId firstExpr = parseExpr();

        if (check("ASSIGN")) {
            match("ASSIGN");
            const ExprId valExpr = parseExpr();
            expectNewlineOrEof();

            VarAssignment stmt{};
            stmt.target = firstExpr;
            stmt.hasDeclaredType = false;
            stmt.declaredType = INVALID_ID;
            stmt.value = valExpr;
            return arena.addStmt(start.line, start.col, std::move(stmt));
        }

        expectNewlineOrEof();

        ExprStmt stmt{};
        stmt.expr = firstExpr;
        return arena.addStmt(start.line, start.col, std::move(stmt));
    }

    std::vector<StmtId> parseStmtBlockUntil(
        std::initializer_list<std::string_view> endKeywords,
        std::string_view unclosedMsg
    ) {
        std::vector<StmtId> body;
        while (true) {
            bool reachedEnd = false;
            for (const std::string_view kw : endKeywords) {
                if (check("KEYWORD", kw)) {
                    reachedEnd = true;
                    break;
                }
            }
            if (reachedEnd) {
                break;
            }
            if (check("EOF")) {
                setParseError(current(), unclosedMsg);
            }
            body.push_back(parseStatement());
            skipNewlines();
        }
        return body;
    }

    StmtId parseIfStmt() {
        const Token start = match("KEYWORD", "if");
        const ExprId condition = parseExpr();
        expectNewlineOrEof();

        std::vector<StmtId> thenBody = parseStmtBlockUntil(
            {"else if", "else", "end if"}, "Unclosed 'if' statement, expected 'end if'"
        );

        std::vector<ElseIfBranch> elifBranches;
        while (check("KEYWORD", "else if")) {
            const Token elifTok = match("KEYWORD", "else if");
            const ExprId elifCond = parseExpr();
            expectNewlineOrEof();
            std::vector<StmtId> elifBody = parseStmtBlockUntil(
                {"else if", "else", "end if"}, "Unclosed 'else if' block, expected 'end if'"
            );
            elifBranches.push_back(ElseIfBranch{elifTok.line, elifTok.col, elifCond, std::move(elifBody)});
        }

        bool hasElse = false;
        std::vector<StmtId> elseBody;
        if (check("KEYWORD", "else")) {
            match("KEYWORD", "else");
            expectNewlineOrEof();
            hasElse = true;
            elseBody = parseStmtBlockUntil({"end if"}, "Unclosed 'else' block, expected 'end if'");
        }

        match("KEYWORD", "end if");
        expectNewlineOrEof();
        return arena.addStmt(start.line, start.col, IfStmt{condition, std::move(thenBody), std::move(elifBranches), hasElse, std::move(elseBody)});
    }

    StmtId parseForStmt() {
        const Token start = match("KEYWORD", "for");
        std::vector<std::string> loopVars;
        loopVars.push_back(match("IDENT").value);
        while (check("COMMA")) {
            match("COMMA");
            loopVars.push_back(match("IDENT").value);
        }

        RangeSpec rangeSpec{};
        ExprId iterable = INVALID_ID;

        if (check("ASSIGN")) {
            if (loopVars.size() != 1) {
                setParseError(start, "Range 'for' loop only supports a single loop variable");
            }
            const Token eqTok = match("ASSIGN");
            const ExprId startExpr = parseExpr();
            match("KEYWORD", "to");
            const ExprId endExpr = parseExpr();
            rangeSpec = RangeSpec{true, eqTok.line, eqTok.col, startExpr, endExpr};
        } else {
            match("KEYWORD", "in");
            iterable = parseExpr();
        }

        expectNewlineOrEof();
        std::vector<StmtId> body = parseStmtBlockUntil({"end for"}, "Unclosed 'for' loop, expected 'end for'");
        match("KEYWORD", "end for");
        expectNewlineOrEof();

        return arena.addStmt(start.line, start.col, ForStmt{std::move(loopVars), rangeSpec, iterable, std::move(body)});
    }

    StmtId parseSelectStmt() {
        const Token start = match("KEYWORD", "select case");
        const ExprId target = parseExpr();
        expectNewlineOrEof();

        std::vector<SelectCase> cases;
        while (check("KEYWORD", "case")) {
            const Token caseTok = match("KEYWORD", "case");
            std::vector<ExprId> matchExprs;
            matchExprs.push_back(parseExpr());
            while (check("COMMA")) {
                match("COMMA");
                matchExprs.push_back(parseExpr());
            }
            std::string bindingName;
            if (check("KEYWORD", "as")) {
                if (matchExprs.size() != 1) {
                    setParseError(current(), "Case payload binding ('as <ident>') requires a single variant in 'case'");
                }
                match("KEYWORD", "as");
                bindingName = match("IDENT").value;
            }
            expectNewlineOrEof();
            std::vector<StmtId> caseBody = parseStmtBlockUntil(
                {"case", "case else", "end select"}, "Unclosed 'case' in 'select case', expected 'end select'"
            );
            cases.push_back(SelectCase{caseTok.line, caseTok.col, std::move(matchExprs), std::move(caseBody), std::move(bindingName)});
        }

        bool hasCaseElse = false;
        std::vector<StmtId> caseElse;
        if (check("KEYWORD", "case else")) {
            match("KEYWORD", "case else");
            expectNewlineOrEof();
            hasCaseElse = true;
            caseElse = parseStmtBlockUntil({"end select"}, "Unclosed 'case else', expected 'end select'");
        }

        match("KEYWORD", "end select");
        expectNewlineOrEof();
        return arena.addStmt(start.line, start.col, SelectStmt{target, std::move(cases), hasCaseElse, std::move(caseElse)});
    }

    StmtId parseTryCatchStmt() {
        const Token start = match("KEYWORD", "try");
        expectNewlineOrEof();
        std::vector<StmtId> tryBody = parseStmtBlockUntil({"catch"}, "Unclosed 'try' block, expected 'catch'");
        match("KEYWORD", "catch");
        const Token catchIdent = match("IDENT");
        expectNewlineOrEof();
        std::vector<StmtId> catchBody = parseStmtBlockUntil({"end try"}, "Unclosed 'catch' block, expected 'end try'");
        match("KEYWORD", "end try");
        expectNewlineOrEof();
        return arena.addStmt(start.line, start.col, TryCatchStmt{std::move(tryBody), catchIdent.value, std::move(catchBody)});
    }

    StmtId parseWhileStmt() {
        const Token start = match("KEYWORD", "while");
        const ExprId cond = parseExpr();
        expectNewlineOrEof();
        std::vector<StmtId> body = parseStmtBlockUntil({"end while"}, "Unclosed 'while' block, expected 'end while'");
        match("KEYWORD", "end while");
        expectNewlineOrEof();
        return arena.addStmt(start.line, start.col, WhileStmt{cond, std::move(body)});
    }

    StmtId parseReturnStmt() {
        const Token start = match("KEYWORD", "return");
        ReturnStmt stmt{};
        if (!check("NEWLINE") && !check("EOF")) {
            stmt.hasValue = true;
            stmt.value = parseExpr();
        }
        expectNewlineOrEof();

        return arena.addStmt(start.line, start.col, std::move(stmt));
    }

    StmtId parseBreakStmt() {
        const Token start = match("KEYWORD", "break");
        expectNewlineOrEof();

        BreakStmt stmt{};
        return arena.addStmt(start.line, start.col, std::move(stmt));
    }

    StmtId parseContinueStmt() {
        const Token start = match("KEYWORD", "continue");
        expectNewlineOrEof();

        ContinueStmt stmt{};
        return arena.addStmt(start.line, start.col, std::move(stmt));
    }

    StmtId parseStatement() {
        skipNewlines();

        if (check("KEYWORD", "if")) {
            return parseIfStmt();
        }
        if (check("KEYWORD", "for")) {
            return parseForStmt();
        }
        if (check("KEYWORD", "while")) {
            return parseWhileStmt();
        }
        if (check("KEYWORD", "select case")) {
            return parseSelectStmt();
        }
        if (check("KEYWORD", "try")) {
            return parseTryCatchStmt();
        }
        if (check("KEYWORD", "return")) {
            return parseReturnStmt();
        }
        if (check("KEYWORD", "break")) {
            return parseBreakStmt();
        }
        if (check("KEYWORD", "continue")) {
            return parseContinueStmt();
        }

        return parseAssignmentOrExprStmt();
    }

    // ==========================================
    // Top-Level Declarations
    // ==========================================

    DeclId parseImportDecl() {
        const Token start = match("KEYWORD");
        std::string kind = "module";
        if (start.value == "import header") {
            kind = "header";
        }
        const Token strTok = match("STRING");
        ImportDecl decl{};
        decl.kind = std::move(kind);
        decl.target = stripQuotes(strTok.value);
        if (decl.kind == "module" && check("KEYWORD", "as")) {
            match("KEYWORD", "as");
            decl.hasAlias = true;
            decl.alias = match("IDENT").value;
        }
        expectNewlineOrEof();
        return arena.addDecl(start.line, start.col, std::move(decl));
    }

    DeclId parseTypeDecl(bool isPub, bool isExtern, const Token& startTok) {
        match("KEYWORD", "type");
        const Token nameTok = match("IDENT");
        expectNewlineOrEof();

        std::vector<FieldDecl> fields;
        while (!check("KEYWORD", "end type")) {
            if (check("EOF")) {
                setParseError(current(), "Unclosed 'type " + nameTok.value + "', expected 'end type'");
            }
            const Token fieldTok = match("IDENT");
            match("KEYWORD", "as");
            const TypeId fieldType = parseType();
            FieldDecl fd{};
            fd.line = fieldTok.line;
            fd.col = fieldTok.col;
            fd.name = fieldTok.value;
            fd.typeId = fieldType;
            fields.push_back(std::move(fd));
            expectNewlineOrEof();
        }

        const Token endTok = match("KEYWORD", "end type");
        expectNewlineOrEof();

        TypeDecl decl{};
        decl.name = nameTok.value;
        decl.fields = std::move(fields);
        decl.isPub = isPub;
        decl.isExtern = isExtern;
        return arena.addDecl(startTok.line, startTok.col, std::move(decl), "", fileName, endTok.line, nameTok.line, nameTok.col);
    }

    DeclId parseEnumDecl(bool isPub, const Token& startTok) {
        match("KEYWORD", "enum");
        const Token nameTok = match("IDENT");
        expectNewlineOrEof();

        std::vector<std::string> members;
        while (!check("KEYWORD", "end enum")) {
            if (check("EOF")) {
                setParseError(current(), "Unclosed 'enum " + nameTok.value + "', expected 'end enum'");
            }
            const Token memberTok = match("IDENT");
            members.push_back(memberTok.value);
            expectNewlineOrEof();
        }

        const Token endTok = match("KEYWORD", "end enum");
        expectNewlineOrEof();

        EnumDecl decl{};
        decl.name = nameTok.value;
        decl.members = std::move(members);
        decl.isPub = isPub;
        return arena.addDecl(startTok.line, startTok.col, std::move(decl), "", fileName, endTok.line, nameTok.line, nameTok.col);
    }

    DeclId parseUnionDecl(bool isPub, const Token& startTok) {
        match("KEYWORD", "union");
        const Token nameTok = match("IDENT");
        expectNewlineOrEof();

        std::vector<UnionVariant> variants;
        while (!check("KEYWORD", "end union")) {
            if (check("EOF")) {
                setParseError(current(), "Unclosed 'union " + nameTok.value + "', expected 'end union'");
            }
            const Token varTok = match("IDENT");
            match("KEYWORD", "as");
            const TypeId varType = parseType();
            UnionVariant uv{};
            uv.name = varTok.value;
            uv.type = varType;
            uv.line = varTok.line;
            uv.col = varTok.col;
            variants.push_back(std::move(uv));
            expectNewlineOrEof();
        }
        if (variants.empty()) {
            setParseError(nameTok, "Union '" + nameTok.value + "' must declare at least one variant");
        }

        const Token endTok = match("KEYWORD", "end union");
        expectNewlineOrEof();

        UnionDecl decl{};
        decl.name = nameTok.value;
        decl.variants = std::move(variants);
        decl.isPub = isPub;
        return arena.addDecl(startTok.line, startTok.col, std::move(decl), "", fileName, endTok.line, nameTok.line, nameTok.col);
    }

    Param parseParam() {
        bool isByref = false;
        if (check("KEYWORD", "byref")) {
            match("KEYWORD", "byref");
            isByref = true;
        }
        const Token nameTok = match("IDENT");
        match("KEYWORD", "as");
        const TypeId tNode = parseType();
        Param p{};
        p.line = nameTok.line;
        p.col = nameTok.col;
        p.name = nameTok.value;
        p.typeId = tNode;
        p.isByref = isByref;
        return p;
    }

    DeclId parseConstDecl(bool isPub, const Token& startTok) {
        match("KEYWORD", "const");
        const Token nameTok = match("IDENT");
        bool hasDeclaredType = false;
        TypeId declaredType = INVALID_ID;
        if (check("KEYWORD", "as")) {
            match("KEYWORD", "as");
            hasDeclaredType = true;
            declaredType = parseType();
        }
        match("ASSIGN");
        const ExprId valExpr = parseExpr();
        expectNewlineOrEof();

        ConstDecl decl{};
        decl.name = nameTok.value;
        decl.hasDeclaredType = hasDeclaredType;
        decl.declaredType = declaredType;
        decl.value = valExpr;
        decl.isPub = isPub;
        return arena.addDecl(startTok.line, startTok.col, std::move(decl), "", fileName, nameTok.line, nameTok.line, nameTok.col);
    }

    DeclId parseFnDecl(bool isPub, bool isExtern, const Token& startTok) {
        match("KEYWORD", "fn");
        const Token nameTok = match("IDENT");
        match("LPAREN");

        std::vector<Param> params = parseCommaSeparated<Param>("RPAREN", [&]() { return parseParam(); });

        TypeId returnType = arena.findOrAddType("void", {}, startTok.line, startTok.col);
        bool isFallible = false;
        if (check("KEYWORD", "as")) {
            match("KEYWORD", "as");
            returnType = parseType();
        }
        if (check("KEYWORD", "can error")) {
            match("KEYWORD", "can error");
            isFallible = true;
        }
        expectNewlineOrEof();

        std::vector<StmtId> body;
        if (isExtern && !check("KEYWORD", "end fn")) {
            FnDecl decl{};
            decl.name = nameTok.value;
            decl.params = std::move(params);
            decl.returnType = returnType;
            decl.isFallible = isFallible;
            decl.body = std::move(body);
            decl.isPub = isPub;
            decl.isExtern = true;
            return arena.addDecl(startTok.line, startTok.col, std::move(decl), "", fileName, nameTok.line, nameTok.line, nameTok.col);
        }

        body = parseStmtBlockUntil({"end fn"}, "Unclosed 'fn " + nameTok.value + "', expected 'end fn'");

        const Token endTok = match("KEYWORD", "end fn");
        expectNewlineOrEof();

        FnDecl decl{};
        decl.name = nameTok.value;
        decl.params = std::move(params);
        decl.returnType = returnType;
        decl.isFallible = isFallible;
        decl.body = std::move(body);
        decl.isPub = isPub;
        decl.isExtern = isExtern;
        return arena.addDecl(startTok.line, startTok.col, std::move(decl), "", fileName, endTok.line, nameTok.line, nameTok.col);
    }

    DeclId parseTopLevelDecl() {
        const Token tok = current();

        if (check("KEYWORD", "import") || check("KEYWORD", "import header")) {
            return parseImportDecl();
        }

        bool isPub = false;
        bool isExtern = false;
        while (check("KEYWORD", "public") || check("KEYWORD", "extern")) {
            if (check("KEYWORD", "public")) {
                match("KEYWORD", "public");
                isPub = true;
            } else if (check("KEYWORD", "extern")) {
                match("KEYWORD", "extern");
                isExtern = true;
            }
        }

        if (check("KEYWORD", "type")) {
            return parseTypeDecl(isPub, isExtern, tok);
        }

        if (check("KEYWORD", "enum")) {
            if (isExtern) {
                setParseError(current(), "'extern' cannot be applied to 'enum'");
            }
            return parseEnumDecl(isPub, tok);
        }

        if (check("KEYWORD", "union")) {
            if (isExtern) {
                setParseError(current(), "'extern' cannot be applied to 'union'");
            }
            return parseUnionDecl(isPub, tok);
        }

        if (check("KEYWORD", "const")) {
            if (isExtern) {
                setParseError(current(), "'extern' cannot be applied to 'const'");
            }
            return parseConstDecl(isPub, tok);
        }

        if (check("KEYWORD", "fn")) {
            return parseFnDecl(isPub, isExtern, tok);
        }

        setParseError(current(), "Expected top-level declaration ('import', 'const', 'type', 'enum', 'union', or 'fn')");
    }

    Program parseProgram() {
        Program prog{};
        prog.fileName = fileName;

        skipNewlines();
        while (!check("EOF")) {
            const DeclId declId = parseTopLevelDecl();
            prog.declarations.push_back(declId);
            skipNewlines();
        }
        return prog;
    }
};

// ==========================================
// Main Parse Entry Point
// ==========================================

inline ParseResult parseTokensWithArena(
    const std::vector<Token>& tokens,
    AstArena arena,
    std::string_view fileName = "main.cb"
) {
    try {
        Parser parser(tokens, std::move(arena), fileName);
        Program prog = parser.parseProgram();
        ParseResult result{};
        result.ok = true;
        result.program = std::move(prog);
        result.arena = std::move(parser.arena);
        return result;
    } catch (const DiagnosticError& err) {
        ParseResult result{};
        result.ok = false;
        result.error = err;
        return result;
    }
}

inline ParseResult parseTokens(const std::vector<Token>& tokens, std::string_view fileName = "main.cb") {
    return parseTokensWithArena(tokens, AstArena{}, fileName);
}

}  // namespace cb

#endif  // CLEANBASIC_PARSER_HPP
