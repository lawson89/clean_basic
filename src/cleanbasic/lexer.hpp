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

inline bool is_ident_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

inline bool is_ident_char(char c) {
    return is_ident_start(c) || (c >= '0' && c <= '9');
}

inline bool is_digit_char(char c) {
    return c >= '0' && c <= '9';
}

inline bool is_single_keyword(std::string_view word) {
    static const std::vector<std::string_view> keywords = {
        "import", "as", "pub", "extern", "alias", "type", "fn",
        "dim", "const", "byref", "shared", "new", "if", "else",
        "for", "in", "to", "step", "case", "defer", "on", "error",
        "spawn", "return", "break", "and", "or", "not",
        "true", "false", "null"
    };
    for (const auto& kw : keywords) {
        if (word == kw) {
            return true;
        }
    }
    return false;
}

inline bool is_type_keyword(std::string_view word) {
    static const std::vector<std::string_view> type_kws = {
        "int", "float", "bool", "str", "list", "dict", "chan", "err"
    };
    for (const auto& tk : type_kws) {
        if (word == tk) {
            return true;
        }
    }
    return false;
}

inline bool match_second_word_for_multi_kw(std::string_view first, std::string_view second) {
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
               second == "type" || second == "for" || second == "defer" ||
               second == "error";
    }
    return false;
}

inline LexResult tokenize(std::string_view code, std::string_view file_name = "main.cb") {
    LexResult res{};
    res.ok = true;
    const std::string file_str(file_name);
    const char mul_ch = static_cast<char>(42);
    constexpr char single_quote = static_cast<char>(39);
    constexpr char backslash_char = static_cast<char>(92);
    constexpr char nl_ch = static_cast<char>(10);
    constexpr char cr_ch = static_cast<char>(13);
    constexpr char tab_ch = static_cast<char>(9);

    uint32_t line_num = 1;
    std::size_t line_start = 0;
    std::size_t i = 0;
    const std::size_t n = code.size();

    while (i < n) {
        const char ch = code[i];
        const uint32_t col = static_cast<uint32_t>(i - line_start + 1);

        // 1. Horizontal whitespace
        if (ch == ' ' || ch == tab_ch || ch == cr_ch) {
            i += 1;
            continue;
        }

        // 2. Comment (' or # to end of line)
        if (ch == single_quote || ch == '#') {
            while (i < n && code[i] != nl_ch) {
                i += 1;
            }
            continue;
        }

        // 3. Consecutive newlines collapsed into a single NEWLINE token
        if (ch == nl_ch) {
            uint32_t nl_count = 0;
            while (i < n && code[i] == nl_ch) {
                nl_count += 1;
                i += 1;
            }
            Token tok{};
            tok.kind = "NEWLINE";
            tok.value = std::string(1, nl_ch);
            tok.line = line_num;
            tok.col = col;
            res.tokens.push_back(std::move(tok));
            line_num += nl_count;
            line_start = i;
            continue;
        }
        // 4. Forbidden C-style logical operators: &&, ||, !(not followed by =)
        if (ch == '&' && i + 1 < n && code[i + 1] == '&') {
            res.ok = false;
            res.error = make_error(
                "LexerError",
                "C-style operator '&&' is forbidden in CleanBasic; use 'and' instead.",
                file_str,
                line_num,
                col
            );
            return res;
        }
        if (ch == '|' && i + 1 < n && code[i + 1] == '|') {
            res.ok = false;
            res.error = make_error(
                "LexerError",
                "C-style operator '||' is forbidden in CleanBasic; use 'or' instead.",
                file_str,
                line_num,
                col
            );
            return res;
        }
        if (ch == '!' && !(i + 1 < n && code[i + 1] == '=')) {
            res.ok = false;
            res.error = make_error(
                "LexerError",
                "C-style operator '!' is forbidden in CleanBasic; use 'not' instead.",
                file_str,
                line_num,
                col
            );
            return res;
        }

        // 5. F-String or String literal
        if ((ch == 'f' && i + 1 < n && code[i + 1] == '"') || ch == '"') {
            const bool is_fstr = (ch == 'f');
            const char quote = '"';
            const std::size_t start_idx = i;
            if (is_fstr) {
                i += 2;
            } else {
                i += 1;
            }
            bool closed = false;
            while (i < n && code[i] != static_cast<char>(10)) {
                if (code[i] == backslash_char && i + 1 < n) {
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
                res.error = make_error(
                    "LexerError",
                    "Unterminated string literal",
                    file_str,
                    line_num,
                    col
                );
                return res;
            }
            Token tok{};
            if (is_fstr) {
                tok.kind = "F_STRING";
            } else {
                tok.kind = "STRING";
            }
            tok.value = std::string(code.substr(start_idx, i - start_idx));
            tok.line = line_num;
            tok.col = col;
            res.tokens.push_back(std::move(tok));
            continue;
        }

        // 6. Identifiers, multi-word keywords, single-word keywords, and type keywords
        if (is_ident_start(ch)) {
            const std::size_t start_idx = i;
            while (i < n && is_ident_char(code[i])) {
                i += 1;
            }
            const std::string_view first_word = code.substr(start_idx, i - start_idx);

            // Check for multi-word keyword: first_word + [ 	]+ + second_word
            std::size_t look = i;
            while (look < n && (code[look] == ' ' || code[look] == '	')) {
                look += 1;
            }
            if (look > i && look < n && is_ident_start(code[look])) {
                const std::size_t sec_start = look;
                while (look < n && is_ident_char(code[look])) {
                    look += 1;
                }
                const std::string_view second_word = code.substr(sec_start, look - sec_start);
                if (match_second_word_for_multi_kw(first_word, second_word)) {
                    i = look;
                    Token tok{};
                    tok.kind = "KEYWORD";
                    tok.value = std::string(first_word) + " " + std::string(second_word);
                    tok.line = line_num;
                    tok.col = col;
                    res.tokens.push_back(std::move(tok));
                    continue;
                }
            }

            Token tok{};
            if (is_single_keyword(first_word)) {
                tok.kind = "KEYWORD";
            } else if (is_type_keyword(first_word)) {
                tok.kind = "TYPE_KW";
            } else {
                tok.kind = "IDENT";
            }
            tok.value = std::string(first_word);
            tok.line = line_num;
            tok.col = col;
            res.tokens.push_back(std::move(tok));
            continue;
        }

        // 7. Numbers (taking care not to consume '..' range operator)
        if (is_digit_char(ch)) {
            const std::size_t start_idx = i;
            while (i < n && is_digit_char(code[i])) {
                i += 1;
            }
            if (i + 1 < n && code[i] == '.' && is_digit_char(code[i + 1])) {
                std::size_t frac_end = i + 1;
                while (frac_end < n && is_digit_char(code[frac_end])) {
                    frac_end += 1;
                }
                if (!(frac_end < n && code[frac_end] == '.')) {
                    i = frac_end;
                }
            }
            Token tok{};
            tok.kind = "NUMBER";
            tok.value = std::string(code.substr(start_idx, i - start_idx));
            tok.line = line_num;
            tok.col = col;
            res.tokens.push_back(std::move(tok));
            continue;
        }

        // 8. Multi-character operators: .., ==, !=, <=, >=
        if (ch == '.' && i + 1 < n && code[i + 1] == '.') {
            res.tokens.push_back(Token{"RANGE", "..", line_num, col});
            i += 2;
            continue;
        }
        if ((ch == '=' || ch == '!' || ch == '<' || ch == '>') && i + 1 < n && code[i + 1] == '=') {
            res.tokens.push_back(Token{"OP_CMP", std::string(code.substr(i, 2)), line_num, col});
            i += 2;
            continue;
        }
        if (ch == '<' || ch == '>') {
            res.tokens.push_back(Token{"OP_CMP", std::string(1, ch), line_num, col});
            i += 1;
            continue;
        }

        // 9. Single-character arithmetic, concatenation, and punctuation
        if (ch == '+' || ch == '-' || ch == mul_ch || ch == '/' || ch == '&') {
            res.tokens.push_back(Token{"OP_ARITH", std::string(1, ch), line_num, col});
            i += 1;
            continue;
        }
        if (ch == '=') {
            res.tokens.push_back(Token{"ASSIGN", "=", line_num, col});
            i += 1;
            continue;
        }
        if (ch == ':') {
            res.tokens.push_back(Token{"COLON", ":", line_num, col});
            i += 1;
            continue;
        }
        if (ch == ',') {
            res.tokens.push_back(Token{"COMMA", ",", line_num, col});
            i += 1;
            continue;
        }
        if (ch == '.') {
            res.tokens.push_back(Token{"DOT", ".", line_num, col});
            i += 1;
            continue;
        }
        if (ch == '(') {
            res.tokens.push_back(Token{"LPAREN", "(", line_num, col});
            i += 1;
            continue;
        }
        if (ch == ')') {
            res.tokens.push_back(Token{"RPAREN", ")", line_num, col});
            i += 1;
            continue;
        }
        if (ch == '[') {
            res.tokens.push_back(Token{"LBRACKET", "[", line_num, col});
            i += 1;
            continue;
        }
        if (ch == ']') {
            res.tokens.push_back(Token{"RBRACKET", "]", line_num, col});
            i += 1;
            continue;
        }
        if (ch == '{') {
            res.tokens.push_back(Token{"LBRACE", "{", line_num, col});
            i += 1;
            continue;
        }
        if (ch == '}') {
            res.tokens.push_back(Token{"RBRACE", "}", line_num, col});
            i += 1;
            continue;
        }

        // 10. Unknown character
        res.ok = false;
        res.error = make_error(
            "LexerError",
            std::string("Unexpected character '") + ch + "'",
            file_str,
            line_num,
            col
        );
        return res;
    }

    uint32_t eof_col = 1u;
    if (n >= line_start) {
        eof_col = static_cast<uint32_t>(n - line_start + 1);
    }
    if (eof_col == 0) {
        eof_col = 1u;
    }
    res.tokens.push_back(Token{"EOF", "", line_num, eof_col});
    return res;
}

}  // namespace cb

#endif  // CLEANBASIC_LEXER_HPP
