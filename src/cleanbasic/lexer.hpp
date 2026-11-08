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

inline bool isSingleKeyword(std::string_view word) {
    static const std::vector<std::string_view> keywords = {
        "import", "as", "pub", "extern", "alias", "type", "fn",
        "dim", "const", "byref", "if", "else",
        "for", "in", "to", "step", "while", "case", "defer", "on", "error",
        "return", "break", "and", "or", "not",
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
    if (first == "on") {
        return second == "error";
    }
    if (first == "can") {
        return second == "error";
    }
    if (first == "end") {
        return second == "select" || second == "if" || second == "fn" ||
               second == "type" || second == "for" || second == "while" ||
               second == "defer" || second == "error";
    }
    return false;
}

inline LexResult tokenize(std::string_view code, std::string_view fileName = "main.cb") {
    LexResult res{};
    res.ok = true;
    const std::string fileStr(fileName);
    const char mulCh = static_cast<char>(42);
    constexpr char singleQuote = static_cast<char>(39);
    constexpr char backslashChar = static_cast<char>(92);
    constexpr char nlCh = static_cast<char>(10);
    constexpr char crCh = static_cast<char>(13);
    constexpr char tabCh = static_cast<char>(9);

    uint32_t lineNum = 1;
    std::size_t lineStart = 0;
    uint32_t bracketDepth = 0;
    std::size_t i = 0;
    const std::size_t n = code.size();

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

        // 3. Consecutive newlines collapsed into a single NEWLINE token (suppressed inside open brackets)
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

        // 5. String literal
        if (ch == '"') {
            const char quote = '"';
            const std::size_t startIdx = i;
            i += 1;
            bool closed = false;
            while (i < n && code[i] != static_cast<char>(10)) {
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
                res.error = makeError(
                    "LexerError",
                    "Unterminated string literal",
                    fileStr,
                    lineNum,
                    col
                );
                return res;
            }
            Token tok{};
            tok.kind = "STRING";
            tok.value = std::string(code.substr(startIdx, i - startIdx));
            tok.line = lineNum;
            tok.col = col;
            res.tokens.push_back(std::move(tok));
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

        // 7. Numbers
        if (isDigitChar(ch)) {
            const std::size_t startIdx = i;
            while (i < n && isDigitChar(code[i])) {
                i += 1;
            }
            if (i + 1 < n && code[i] == '.' && isDigitChar(code[i + 1])) {
                i += 1;
                while (i < n && isDigitChar(code[i])) {
                    i += 1;
                }
            }
            Token tok{};
            tok.kind = "NUMBER";
            tok.value = std::string(code.substr(startIdx, i - startIdx));
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

        // 9. Single-character arithmetic, concatenation, and punctuation
        if (ch == '+' || ch == '-' || ch == mulCh || ch == '/' || ch == '&') {
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
