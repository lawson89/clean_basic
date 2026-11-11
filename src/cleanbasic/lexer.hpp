#ifndef CLEANBASIC_LEXER_HPP
#define CLEANBASIC_LEXER_HPP

#include "src/cleanbasic/diagnostics.hpp"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cb {

struct Token {
    std::string kind;
    std::string value;
    uint32_t line = 1;
    uint32_t col = 1;
};

struct LexResult {
    bool ok = false;
    std::vector<Token> tokens;
    DiagnosticError error;
};

inline bool isIdentStart(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

inline bool isIdentChar(char c) {
    return isIdentStart(c) || (c >= '0' && c <= '9');
}

inline bool isDigitChar(char c) {
    return c >= '0' && c <= '9';
}

inline bool isHexDigitChar(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

inline bool isBinDigitChar(char c) {
    return c == '0' || c == '1';
}

inline bool isSingleKeyword(std::string_view word) {
    static const std::vector<std::string_view> keywords = {
        "import", "as", "public", "extern", "alias", "type", "enum", "union", "fn",
        "dim", "const", "byref", "if", "else",
        "for", "in", "to", "step", "while", "case", "defer", "try", "catch", "error",
        "return", "break", "continue", "and", "or", "not",
        "true", "false", "null"
    };
    for (const auto& kw : keywords) {
        if (word == kw) {
            return true;
        }
    }
    return false;
}

inline bool isTypeKeyword(std::string_view word) {
    static const std::vector<std::string_view> typeKws = {
        "int", "float", "bool", "str", "list", "dict", "err", "cptr"
    };
    for (const auto& tk : typeKws) {
        if (word == tk) {
            return true;
        }
    }
    return false;
}

inline bool matchSecondWordForMultiKw(std::string_view first, std::string_view second) {
    if (first == "import") {
        return second == "header" || second == "link";
    }
    if (first == "select") {
        return second == "case";
    }
    if (first == "case") {
        return second == "else";
    }
    if (first == "else") {
        return second == "if";
    }

    if (first == "can") {
        return second == "error";
    }
    if (first == "end") {
        return second == "select" || second == "if" || second == "fn" ||
               second == "type" || second == "enum" || second == "union" || second == "for" || second == "while" ||
               second == "defer" || second == "try";
    }
    return false;
}

inline LexResult tokenize(std::string_view code, std::string_view fileName = "main.cb") {
    LexResult res{};
    res.ok = true;
    const std::string fileStr(fileName);
    constexpr char mulCh = '*';
    constexpr char singleQuote = '\'';
    constexpr char backslashChar = '\\';
    constexpr char nlCh = '\n';
    constexpr char crCh = '\r';
    constexpr char tabCh = '\t';

    uint32_t lineNum = 1;
    std::size_t lineStart = 0;
    uint32_t bracketDepth = 0;
    std::size_t i = 0;
    const std::size_t n = code.size();

    // String interpolation desugaring state machine:
    // Rewrites $"Hello {user.name}!" directly in the token stream into parenthesized
    // '&' concatenation expressions: ("Hello " & (user.name) & "!").
    // Each active '{ ... }' interpolation hole pushes an InterpState recording the
    // bracketDepth at its opening '(' so nested '{' (such as dict literals) inside the
    // interpolated expression do not prematurely close the interpolation hole.
    struct InterpState {
        uint32_t braceDepth = 0;
        uint32_t openLine = 1;
        uint32_t openCol = 1;
        std::size_t openTokenCount = 0;
    };
    std::vector<InterpState> interpBraceDepths;

    auto scanInterpolatedSegment = [&](uint32_t segStartCol) -> bool {
        std::string segment(1, '"');
        while (i < n && code[i] != nlCh) {
            if (code[i] == backslashChar && i + 1 < n) {
                segment.push_back(code[i]);
                segment.push_back(code[i + 1]);
                i += 2;
                continue;
            }
            if (code[i] == '{') {
                if (i + 1 < n && code[i + 1] == '{') {
                    segment.push_back('{');
                    i += 2;
                    continue;
                }
                const uint32_t braceCol = static_cast<uint32_t>(i - lineStart + 1);
                segment.push_back('"');
                res.tokens.push_back(Token{"STRING", std::move(segment), lineNum, segStartCol});
                res.tokens.push_back(Token{"OP_ARITH", "&", lineNum, braceCol});
                bracketDepth += 1;
                res.tokens.push_back(Token{"LPAREN", "(", lineNum, braceCol});
                interpBraceDepths.push_back(InterpState{bracketDepth, lineNum, braceCol, res.tokens.size()});
                i += 1;
                return true;
            }
            if (code[i] == '}') {
                if (i + 1 < n && code[i + 1] == '}') {
                    segment.push_back('}');
                    i += 2;
                    continue;
                }
                const uint32_t braceCol = static_cast<uint32_t>(i - lineStart + 1);
                res.ok = false;
                res.error = makeError(
                    "LexerError",
                    "Unescaped '}' in interpolated string literal; use '}}' to escape a literal brace.",
                    fileStr,
                    lineNum,
                    braceCol
                );
                return false;
            }
            if (code[i] == '"') {
                const uint32_t closeCol = static_cast<uint32_t>(i - lineStart + 1);
                segment.push_back('"');
                res.tokens.push_back(Token{"STRING", std::move(segment), lineNum, segStartCol});
                if (bracketDepth > 0) {
                    bracketDepth -= 1;
                }
                res.tokens.push_back(Token{"RPAREN", ")", lineNum, closeCol});
                i += 1;
                return true;
            }
            segment.push_back(code[i]);
            i += 1;
        }
        res.ok = false;
        res.error = makeError(
            "LexerError",
            "Unterminated interpolated string literal",
            fileStr,
            lineNum,
            segStartCol
        );
        return false;
    };

    while (i < n) {
        const char ch = code[i];
        const uint32_t col = static_cast<uint32_t>(i - lineStart + 1);

        // 1. Horizontal whitespace
        if (ch == ' ' || ch == tabCh || ch == crCh) {
            i += 1;
            continue;
        }

        // 2. Comment (' or # to end of line)
        if (ch == singleQuote || ch == '#') {
            while (i < n && code[i] != nlCh) {
                i += 1;
            }
            continue;
        }

        // 3. Consecutive newlines collapsed into a single NEWLINE token.
        // When bracketDepth > 0 (inside '()', '[]', or '{}'), NEWLINE token emission is suppressed
        // to allow implicit multiline expressions/lists/dicts/parameter lists while still advancing
        // lineNum and lineStart for exact diagnostic coordinates.
        if (ch == nlCh) {
            uint32_t nlCount = 0;
            while (i < n && code[i] == nlCh) {
                nlCount += 1;
                i += 1;
            }
            if (bracketDepth == 0) {
                Token tok{};
                tok.kind = "NEWLINE";
                tok.value = std::string(1, nlCh);
                tok.line = lineNum;
                tok.col = col;
                res.tokens.push_back(std::move(tok));
            }
            lineNum += nlCount;
            lineStart = i;
            continue;
        }
        // 4. Forbidden C-style logical operators: &&, ||, !(not followed by =)
        if (ch == '&' && i + 1 < n && code[i + 1] == '&') {
            return LexResult{false, {}, makeError("LexerError", "C-style operator '&&' is forbidden in CleanBasic; use 'and' instead.", fileStr, lineNum, col)};
        }
        if (ch == '|' && i + 1 < n && code[i + 1] == '|') {
            return LexResult{false, {}, makeError("LexerError", "C-style operator '||' is forbidden in CleanBasic; use 'or' instead.", fileStr, lineNum, col)};
        }
        if (ch == '!' && !(i + 1 < n && code[i + 1] == '=')) {
            return LexResult{false, {}, makeError("LexerError", "C-style operator '!' is forbidden in CleanBasic; use 'not' instead.", fileStr, lineNum, col)};
        }

        auto scanQuotedLiteral = [&](std::size_t prefixLen, std::string_view tokKind, std::string_view errMsg) -> bool {
            const char quote = '"';
            const std::size_t startIdx = i;
            i += prefixLen;
            bool closed = false;
            while (i < n && code[i] != nlCh) {
                if (code[i] == backslashChar && i + 1 < n) {
                    i += 2;
                    continue;
                }
                if (code[i] == quote) {
                    i += 1;
                    closed = true;
                    break;
                }
                i += 1;
            }
            if (!closed) {
                res.ok = false;
                res.error = makeError("LexerError", std::string(errMsg), fileStr, lineNum, col);
                return false;
            }
            res.tokens.push_back(Token{std::string(tokKind), std::string(code.substr(startIdx, i - startIdx)), lineNum, col});
            return true;
        };

        // 5a. Interpolated string literal: $"..."
        if (ch == '$' && i + 1 < n && code[i + 1] == '"') {
            bracketDepth += 1;
            res.tokens.push_back(Token{"LPAREN", "(", lineNum, col});
            i += 2;
            if (!scanInterpolatedSegment(col)) {
                return res;
            }
            continue;
        }

        // 5c. Closing '}' of an active string interpolation hole
        if (ch == '}' && !interpBraceDepths.empty() && bracketDepth == interpBraceDepths.back().braceDepth) {
            if (res.tokens.size() == interpBraceDepths.back().openTokenCount) {
                res.ok = false;
                res.error = makeError(
                    "LexerError",
                    "Empty interpolation hole '{}' in interpolated string literal",
                    fileStr,
                    lineNum,
                    col
                );
                return res;
            }
            interpBraceDepths.pop_back();
            if (bracketDepth > 0) {
                bracketDepth -= 1;
            }
            res.tokens.push_back(Token{"RPAREN", ")", lineNum, col});
            res.tokens.push_back(Token{"OP_ARITH", "&", lineNum, col});
            i += 1;
            if (!scanInterpolatedSegment(col)) {
                return res;
            }
            continue;
        }

        // 5. String literal
        if (ch == '"') {
            if (!scanQuotedLiteral(1, "STRING", "Unterminated string literal")) {
                return res;
            }
            continue;
        }

        // 6. Identifiers, multi-word keywords, single-word keywords, and type keywords
        if (isIdentStart(ch)) {
            const std::size_t startIdx = i;
            while (i < n && isIdentChar(code[i])) {
                i += 1;
            }
            const std::string_view firstWord = code.substr(startIdx, i - startIdx);

            // Check for multi-word keyword: firstWord + [ 	]+ + secondWord
            std::size_t look = i;
            while (look < n && (code[look] == ' ' || code[look] == '	')) {
                look += 1;
            }
            if (look > i && look < n && isIdentStart(code[look])) {
                const std::size_t secStart = look;
                while (look < n && isIdentChar(code[look])) {
                    look += 1;
                }
                const std::string_view secondWord = code.substr(secStart, look - secStart);
                if (matchSecondWordForMultiKw(firstWord, secondWord)) {
                    i = look;
                    Token tok{};
                    tok.kind = "KEYWORD";
                    tok.value = std::string(firstWord) + " " + std::string(secondWord);
                    tok.line = lineNum;
                    tok.col = col;
                    res.tokens.push_back(std::move(tok));
                    continue;
                }
            }

            Token tok{};
            if (isSingleKeyword(firstWord)) {
                tok.kind = "KEYWORD";
            } else if (isTypeKeyword(firstWord)) {
                tok.kind = "TYPE_KW";
            } else {
                tok.kind = "IDENT";
            }
            tok.value = std::string(firstWord);
            tok.line = lineNum;
            tok.col = col;
            res.tokens.push_back(std::move(tok));
            continue;
        }

        // 7. Numbers (Decimal, Hexadecimal 0x..., Binary 0b..., and optional '_' separators)
        if (isDigitChar(ch)) {
            if (ch == '0' && i + 1 < n && (code[i + 1] == 'x' || code[i + 1] == 'X')) {
                i += 2;
                std::string normHex = "0x";
                bool hasHexDigit = false;
                while (i < n && (isHexDigitChar(code[i]) || code[i] == '_')) {
                    if (code[i] != '_') {
                        normHex.push_back(code[i]);
                        hasHexDigit = true;
                    }
                    i += 1;
                }
                if (!hasHexDigit || (i < n && isIdentChar(code[i]))) {
                    return LexResult{false, {}, makeError("LexerError", "Invalid hexadecimal integer literal", fileStr, lineNum, col)};
                }
                res.tokens.push_back(Token{"NUMBER", std::move(normHex), lineNum, col});
                continue;
            }
            if (ch == '0' && i + 1 < n && (code[i + 1] == 'b' || code[i + 1] == 'B')) {
                i += 2;
                std::string normBin = "0b";
                bool hasBinDigit = false;
                while (i < n && (isBinDigitChar(code[i]) || code[i] == '_')) {
                    if (code[i] != '_') {
                        normBin.push_back(code[i]);
                        hasBinDigit = true;
                    }
                    i += 1;
                }
                if (!hasBinDigit || (i < n && isIdentChar(code[i]))) {
                    return LexResult{false, {}, makeError("LexerError", "Invalid binary integer literal", fileStr, lineNum, col)};
                }
                res.tokens.push_back(Token{"NUMBER", std::move(normBin), lineNum, col});
                continue;
            }
            std::string normNum;
            while (i < n && (isDigitChar(code[i]) || code[i] == '_')) {
                if (code[i] != '_') {
                    normNum.push_back(code[i]);
                }
                i += 1;
            }
            if (i + 1 < n && code[i] == '.' && isDigitChar(code[i + 1])) {
                normNum.push_back('.');
                i += 1;
                while (i < n && (isDigitChar(code[i]) || code[i] == '_')) {
                    if (code[i] != '_') {
                        normNum.push_back(code[i]);
                    }
                    i += 1;
                }
            }
            Token tok{};
            tok.kind = "NUMBER";
            tok.value = std::move(normNum);
            tok.line = lineNum;
            tok.col = col;
            res.tokens.push_back(std::move(tok));
            continue;
        }

        // 8. Multi-character operators: ==, !=, <=, >=
        if ((ch == '=' || ch == '!' || ch == '<' || ch == '>') && i + 1 < n && code[i + 1] == '=') {
            res.tokens.push_back(Token{"OP_CMP", std::string(code.substr(i, 2)), lineNum, col});
            i += 2;
            continue;
        }
        if (ch == '<' || ch == '>') {
            res.tokens.push_back(Token{"OP_CMP", std::string(1, ch), lineNum, col});
            i += 1;
            continue;
        }

        // 8b. Compound assignment operators: +=, -=, *=, /=, %=, &=
        if ((ch == '+' || ch == '-' || ch == mulCh || ch == '/' || ch == '%' || ch == '&') &&
            i + 1 < n && code[i + 1] == '=') {
            res.tokens.push_back(Token{"COMPOUND_ASSIGN", std::string(code.substr(i, 2)), lineNum, col});
            i += 2;
            continue;
        }

        // 9. Single-character arithmetic, concatenation, and punctuation
        if (ch == '+' || ch == '-' || ch == mulCh || ch == '/' || ch == '%' || ch == '&') {
            res.tokens.push_back(Token{"OP_ARITH", std::string(1, ch), lineNum, col});
            i += 1;
            continue;
        }
        std::string punctKind;
        if (ch == '=') punctKind = "ASSIGN";
        else if (ch == ':') punctKind = "COLON";
        else if (ch == ',') punctKind = "COMMA";
        else if (ch == '.') punctKind = "DOT";
        else if (ch == '(') { bracketDepth += 1; punctKind = "LPAREN"; }
        else if (ch == ')') { if (bracketDepth > 0) bracketDepth -= 1; punctKind = "RPAREN"; }
        else if (ch == '[') { bracketDepth += 1; punctKind = "LBRACKET"; }
        else if (ch == ']') { if (bracketDepth > 0) bracketDepth -= 1; punctKind = "RBRACKET"; }
        else if (ch == '{') { bracketDepth += 1; punctKind = "LBRACE"; }
        else if (ch == '}') { if (bracketDepth > 0) bracketDepth -= 1; punctKind = "RBRACE"; }

        if (!punctKind.empty()) {
            res.tokens.push_back(Token{std::move(punctKind), std::string(1, ch), lineNum, col});
            i += 1;
            continue;
        }

        // 10. Unknown character
        return LexResult{false, {}, makeError("LexerError", std::string("Unexpected character '") + ch + "'", fileStr, lineNum, col)};
    }

    if (!interpBraceDepths.empty()) {
        const InterpState& unclosed = interpBraceDepths.back();
        res.ok = false;
        res.error = makeError(
            "LexerError",
            "Unterminated interpolation block '{...}' in interpolated string literal",
            fileStr,
            unclosed.openLine,
            unclosed.openCol
        );
        return res;
    }

    uint32_t eofCol = 1u;
    if (n >= lineStart) {
        eofCol = static_cast<uint32_t>(n - lineStart + 1);
    }
    if (eofCol == 0) {
        eofCol = 1u;
    }
    res.tokens.push_back(Token{"EOF", "", lineNum, eofCol});
    return res;
}

}  // namespace cb

#endif  // CLEANBASIC_LEXER_HPP
