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
    std::string file_name = "main.cb";
    AstArena arena;
    DiagnosticError error;
    bool has_error = false;
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

inline void set_parse_error(ParserState& state, const Token& tok, std::string_view msg) {
    if (!state.has_error) {
        state.has_error = true;
        std::string full_msg = std::string(msg) + " (got " + tok.kind + " '" + tok.value + "')";
        state.error = make_error("ParseError", full_msg, state.file_name, tok.line, tok.col);
    }
}

inline const Token& current(const std::vector<Token>& tokens, const ParserState& state) {
    static const Token empty_eof{"EOF", "", 1, 1};
    if (tokens.empty()) {
        return empty_eof;
    }
    if (state.pos < tokens.size()) {
        return tokens[state.pos];
    }
    return tokens.back();
}

inline const Token& peek_offset(const std::vector<Token>& tokens, const ParserState& state, size_t offset) {
    static const Token empty_eof{"EOF", "", 1, 1};
    if (tokens.empty()) {
        return empty_eof;
    }
    const size_t idx = state.pos + offset;
    if (idx < tokens.size()) {
        return tokens[idx];
    }
    return tokens.back();
}

inline bool check(const std::vector<Token>& tokens, const ParserState& state, std::string_view kind, std::string_view value = "") {
    if (state.has_error) {
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

inline bool check_any_ident_or_type(const std::vector<Token>& tokens, const ParserState& state) {
    if (state.has_error) {
        return false;
    }
    const Token& tok = current(tokens, state);
    return tok.kind == "IDENT" || tok.kind == "TYPE_KW" || (tok.kind == "KEYWORD" && tok.value == "error");
}

inline const Token& match(const std::vector<Token>& tokens, ParserState& state, std::string_view kind, std::string_view value = "") {
    static const Token fallback_tok{"EOF", "", 1, 1};
    if (state.has_error) {
        return fallback_tok;
    }
    const Token& tok = current(tokens, state);
    if (!check(tokens, state, kind, value)) {
        std::string expected = "Expected " + std::string(kind);
        if (!value.empty()) {
            expected += " ('" + std::string(value) + "')";
        }
        set_parse_error(state, tok, expected);
        return tok;
    }
    state.pos += 1;
    return tok;
}

inline const Token& match_ident_or_type(const std::vector<Token>& tokens, ParserState& state) {
    static const Token fallback_tok{"EOF", "", 1, 1};
    if (state.has_error) {
        return fallback_tok;
    }
    const Token& tok = current(tokens, state);
    if (tok.kind == "IDENT" || tok.kind == "TYPE_KW" || (tok.kind == "KEYWORD" && tok.value == "error")) {
        state.pos += 1;
        return tok;
    }
    set_parse_error(state, tok, "Expected identifier or type name");
    return tok;
}

inline void skip_newlines(const std::vector<Token>& tokens, ParserState& state) {
    while (!state.has_error && check(tokens, state, "NEWLINE")) {
        state.pos += 1;
    }
}

inline void expect_newline_or_eof(const std::vector<Token>& tokens, ParserState& state) {
    if (state.has_error) {
        return;
    }
    if (check(tokens, state, "NEWLINE")) {
        skip_newlines(tokens, state);
    } else if (check(tokens, state, "EOF")) {
        return;
    } else {
        set_parse_error(state, current(tokens, state), "Expected end of line (NEWLINE)");
    }
}

inline std::string strip_quotes(std::string_view raw) {
    constexpr char single_quote = static_cast<char>(39);
    if (raw.size() >= 2 && (raw.front() == '"' || raw.front() == single_quote) && raw.back() == raw.front()) {
        return std::string(raw.substr(1, raw.size() - 2));
    }
    return std::string(raw);
}

// Forward declarations
inline TypeId parse_type(const std::vector<Token>& tokens, ParserState& state);
inline ExprId parse_expr(const std::vector<Token>& tokens, ParserState& state);
inline StmtId parse_statement(const std::vector<Token>& tokens, ParserState& state);

// ==========================================
// Argument & Struct Field Parsing Helpers
// ==========================================

inline void parse_call_arguments(
    const std::vector<Token>& tokens,
    ParserState& state,
    std::string_view closing_kind,
    std::vector<ExprId>& pos_args,
    std::vector<NamedArg>& named_args
) {
    if (check(tokens, state, closing_kind)) {
        return;
    }

    while (!state.has_error) {
        const Token& peek_tok = peek_offset(tokens, state, 1);
        if (check(tokens, state, "IDENT") && peek_tok.kind == "COLON") {
            set_parse_error(state, peek_tok, "Named arguments must use '=' (e.g., name=value), not ':'");
            return;
        }
        if (check(tokens, state, "IDENT") && peek_tok.kind == "ASSIGN") {
            const Token name_tok = match(tokens, state, "IDENT");
            state.pos += 1;  // consume '='
            const ExprId val = parse_expr(tokens, state);
            NamedArg arg{};
            arg.name = name_tok.value;
            arg.value = val;
            named_args.push_back(std::move(arg));
        } else {
            pos_args.push_back(parse_expr(tokens, state));
        }

        if (check(tokens, state, "COMMA")) {
            match(tokens, state, "COMMA");
            if (check(tokens, state, closing_kind)) {
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

inline ExprId parse_expr(const std::vector<Token>& tokens, ParserState& state);

inline ExprId parse_primary(const std::vector<Token>& tokens, ParserState& state) {
    if (state.has_error) {
        return INVALID_ID;
    }
    const Token& tok = current(tokens, state);


    if (check(tokens, state, "NUMBER")) {
        const Token num_tok = match(tokens, state, "NUMBER");
        NumberLiteral node{};
        node.raw = num_tok.value;
        node.is_float = (num_tok.value.find('.') != std::string::npos);
        return add_expr(state.arena, num_tok.line, num_tok.col, std::move(node));
    }

    if (check(tokens, state, "STRING")) {
        const Token str_tok = match(tokens, state, "STRING");
        StringLiteral node{};
        node.value = strip_quotes(str_tok.value);
        return add_expr(state.arena, str_tok.line, str_tok.col, std::move(node));
    }
    if (check(tokens, state, "KEYWORD", "true") || check(tokens, state, "KEYWORD", "false")) {
        const Token b_tok = match(tokens, state, "KEYWORD");
        BoolLiteral node{};
        node.value = (b_tok.value == "true");
        return add_expr(state.arena, b_tok.line, b_tok.col, std::move(node));
    }

    if (check(tokens, state, "KEYWORD", "null")) {
        const Token n_tok = match(tokens, state, "KEYWORD", "null");
        NullLiteral node{};
        return add_expr(state.arena, n_tok.line, n_tok.col, std::move(node));
    }

    if (check(tokens, state, "LPAREN")) {
        match(tokens, state, "LPAREN");
        const ExprId inner = parse_expr(tokens, state);
        match(tokens, state, "RPAREN");
        return inner;
    }

    if (check(tokens, state, "LBRACKET")) {
        const Token b_tok = match(tokens, state, "LBRACKET");
        std::vector<ExprId> elements;
        if (!check(tokens, state, "RBRACKET")) {
            elements.push_back(parse_expr(tokens, state));
            while (check(tokens, state, "COMMA")) {
                match(tokens, state, "COMMA");
                if (check(tokens, state, "RBRACKET")) {
                    break;
                }
                elements.push_back(parse_expr(tokens, state));
            }
        }
        match(tokens, state, "RBRACKET");
        ListLiteral node{};
        node.elements = std::move(elements);
        return add_expr(state.arena, b_tok.line, b_tok.col, std::move(node));
    }

    if (check(tokens, state, "LBRACE")) {
        const Token b_tok = match(tokens, state, "LBRACE");
        std::vector<DictEntry> entries;
        if (!check(tokens, state, "RBRACE")) {
            DictEntry first{};
            first.key = parse_expr(tokens, state);
            match(tokens, state, "COLON");
            first.value = parse_expr(tokens, state);
            entries.push_back(std::move(first));

            while (check(tokens, state, "COMMA")) {
                match(tokens, state, "COMMA");
                if (check(tokens, state, "RBRACE")) {
                    break;
                }
                DictEntry entry{};
                entry.key = parse_expr(tokens, state);
                match(tokens, state, "COLON");
                entry.value = parse_expr(tokens, state);
                entries.push_back(std::move(entry));
            }
        }
        match(tokens, state, "RBRACE");
        DictLiteral node{};
        node.entries = std::move(entries);
        return add_expr(state.arena, b_tok.line, b_tok.col, std::move(node));
    }

    if (check_any_ident_or_type(tokens, state)) {
        const Token id_tok = match_ident_or_type(tokens, state);
        Identifier node{};
        node.name = id_tok.value;
        return add_expr(state.arena, id_tok.line, id_tok.col, std::move(node));
    }

    set_parse_error(state, tok, "Expected expression");
    return INVALID_ID;
}

inline ExprId parse_postfix(const std::vector<Token>& tokens, ParserState& state) {
    ExprId expr = parse_primary(tokens, state);

    while (!state.has_error) {
        if (check(tokens, state, "DOT")) {
            match(tokens, state, "DOT");
            const Token field_tok = match_ident_or_type(tokens, state);
            FieldAccess node{};
            node.target = expr;
            node.field_name = field_tok.value;
            expr = add_expr(state.arena, field_tok.line, field_tok.col, std::move(node));
        } else if (check(tokens, state, "LBRACKET")) {
            const Token b_tok = match(tokens, state, "LBRACKET");
            const ExprId idx_expr = parse_expr(tokens, state);
            match(tokens, state, "RBRACKET");
            IndexAccess node{};
            node.target = expr;
            node.index = idx_expr;
            expr = add_expr(state.arena, b_tok.line, b_tok.col, std::move(node));
        } else if (check(tokens, state, "LPAREN")) {
            const Token p_tok = match(tokens, state, "LPAREN");
            std::vector<ExprId> pos_args;
            std::vector<NamedArg> named_args;
            parse_call_arguments(tokens, state, "RPAREN", pos_args, named_args);
            match(tokens, state, "RPAREN");
            FuncCall node{};
            node.callee = expr;
            node.args = std::move(pos_args);
            node.named_args = std::move(named_args);
            expr = add_expr(state.arena, p_tok.line, p_tok.col, std::move(node));
        } else {
            break;
        }
    }
    return expr;
}

inline ExprId parse_unary(const std::vector<Token>& tokens, ParserState& state) {
    if (state.has_error) {
        return INVALID_ID;
    }
    if (check(tokens, state, "OP_ARITH", "-") || check(tokens, state, "OP_ARITH", "+")) {
        const Token op_tok = match(tokens, state, "OP_ARITH");
        const ExprId operand = parse_unary(tokens, state);
        UnaryOp node{};
        node.op = op_tok.value;
        node.operand = operand;
        return add_expr(state.arena, op_tok.line, op_tok.col, std::move(node));
    }
    return parse_postfix(tokens, state);
}

inline ExprId parse_multiplicative(const std::vector<Token>& tokens, ParserState& state) {
    ExprId left = parse_unary(tokens, state);
    while (!state.has_error && (check(tokens, state, "OP_ARITH", "*") || check(tokens, state, "OP_ARITH", "/"))) {
        const Token op_tok = match(tokens, state, "OP_ARITH");
        const ExprId right = parse_unary(tokens, state);
        BinaryOp node{};
        node.op = op_tok.value;
        node.left = left;
        node.right = right;
        left = add_expr(state.arena, op_tok.line, op_tok.col, std::move(node));
    }
    return left;
}

inline ExprId parse_additive(const std::vector<Token>& tokens, ParserState& state) {
    ExprId left = parse_multiplicative(tokens, state);
    while (!state.has_error && (check(tokens, state, "OP_ARITH", "+") || check(tokens, state, "OP_ARITH", "-") || check(tokens, state, "OP_ARITH", "&"))) {
        const Token op_tok = match(tokens, state, "OP_ARITH");
        const ExprId right = parse_multiplicative(tokens, state);
        BinaryOp node{};
        node.op = op_tok.value;
        node.left = left;
        node.right = right;
        left = add_expr(state.arena, op_tok.line, op_tok.col, std::move(node));
    }
    return left;
}

inline ExprId parse_comparison(const std::vector<Token>& tokens, ParserState& state) {
    ExprId left = parse_additive(tokens, state);
    while (!state.has_error && check(tokens, state, "OP_CMP")) {
        const Token op_tok = match(tokens, state, "OP_CMP");
        const ExprId right = parse_additive(tokens, state);
        BinaryOp node{};
        node.op = op_tok.value;
        node.left = left;
        node.right = right;
        left = add_expr(state.arena, op_tok.line, op_tok.col, std::move(node));
    }
    return left;
}

inline ExprId parse_not(const std::vector<Token>& tokens, ParserState& state) {
    if (state.has_error) {
        return INVALID_ID;
    }
    if (check(tokens, state, "KEYWORD", "not")) {
        const Token op_tok = match(tokens, state, "KEYWORD", "not");
        const ExprId operand = parse_not(tokens, state);
        UnaryOp node{};
        node.op = "not";
        node.operand = operand;
        return add_expr(state.arena, op_tok.line, op_tok.col, std::move(node));
    }
    return parse_comparison(tokens, state);
}

inline ExprId parse_and(const std::vector<Token>& tokens, ParserState& state) {
    ExprId left = parse_not(tokens, state);
    while (!state.has_error && check(tokens, state, "KEYWORD", "and")) {
        const Token op_tok = match(tokens, state, "KEYWORD", "and");
        const ExprId right = parse_not(tokens, state);
        BinaryOp node{};
        node.op = "and";
        node.left = left;
        node.right = right;
        left = add_expr(state.arena, op_tok.line, op_tok.col, std::move(node));
    }
    return left;
}

inline ExprId parse_or(const std::vector<Token>& tokens, ParserState& state) {
    ExprId left = parse_and(tokens, state);
    while (!state.has_error && check(tokens, state, "KEYWORD", "or")) {
        const Token op_tok = match(tokens, state, "KEYWORD", "or");
        const ExprId right = parse_and(tokens, state);
        BinaryOp node{};
        node.op = "or";
        node.left = left;
        node.right = right;
        left = add_expr(state.arena, op_tok.line, op_tok.col, std::move(node));
    }
    return left;
}

inline ExprId parse_expr(const std::vector<Token>& tokens, ParserState& state) {
    return parse_or(tokens, state);
}

// ==========================================
// Types
// ==========================================

inline TypeId parse_type(const std::vector<Token>& tokens, ParserState& state) {
    if (state.has_error) {
        return INVALID_ID;
    }
    const Token start = current(tokens, state);
    const Token tok = match_ident_or_type(tokens, state);
    std::string type_name = tok.value;

    while (check(tokens, state, "DOT")) {
        match(tokens, state, "DOT");
        const Token part = match(tokens, state, "IDENT");
        type_name += "." + part.value;
    }

    std::vector<TypeId> type_args;
    if (type_name == "list" && check(tokens, state, "LBRACKET")) {
        match(tokens, state, "LBRACKET");
        type_args.push_back(parse_type(tokens, state));
        match(tokens, state, "RBRACKET");
    } else if (type_name == "dict" && check(tokens, state, "LBRACKET")) {
        match(tokens, state, "LBRACKET");
        type_args.push_back(parse_type(tokens, state));
        match(tokens, state, "COMMA");
        type_args.push_back(parse_type(tokens, state));
        match(tokens, state, "RBRACKET");
    }

    TypeNode node{};
    node.line = start.line;
    node.col = start.col;
    node.name = std::move(type_name);
    node.type_args = std::move(type_args);
    return add_type(state.arena, std::move(node));
}

// ==========================================
// Statements
// ==========================================

inline StmtId parse_assignment_or_expr_stmt(const std::vector<Token>& tokens, ParserState& state) {
    if (state.has_error) {
        return INVALID_ID;
    }
    const Token start = current(tokens, state);

    if (check(tokens, state, "KEYWORD", "dim") || check(tokens, state, "KEYWORD", "const")) {
        const Token decl_kw = match(tokens, state, "KEYWORD");
        const bool is_dim = (decl_kw.value == "dim");
        const bool is_const = (decl_kw.value == "const");

        const Token first_ident = match(tokens, state, "IDENT");
        Identifier first_id_node{};
        first_id_node.name = first_ident.value;
        const ExprId target_expr = add_expr(state.arena, first_ident.line, first_ident.col, std::move(first_id_node));

        bool has_declared_type = false;
        TypeId decl_type = INVALID_ID;
        if (check(tokens, state, "KEYWORD", "as")) {
            match(tokens, state, "KEYWORD", "as");
            has_declared_type = true;
            decl_type = parse_type(tokens, state);
        }

        match(tokens, state, "ASSIGN");
        const ExprId val_expr = parse_expr(tokens, state);
        expect_newline_or_eof(tokens, state);

        VarAssignment stmt{};
        stmt.target = target_expr;
        stmt.is_dim = is_dim;
        stmt.is_const = is_const;
        stmt.has_declared_type = has_declared_type;
        stmt.declared_type = decl_type;
        stmt.value = val_expr;
        return add_stmt(state.arena, start.line, start.col, std::move(stmt));
    }

    const ExprId first_expr = parse_expr(tokens, state);

    if (check(tokens, state, "ASSIGN")) {
        match(tokens, state, "ASSIGN");
        const ExprId val_expr = parse_expr(tokens, state);
        expect_newline_or_eof(tokens, state);

        VarAssignment stmt{};
        stmt.target = first_expr;
        stmt.has_declared_type = false;
        stmt.declared_type = INVALID_ID;
        stmt.value = val_expr;
        return add_stmt(state.arena, start.line, start.col, std::move(stmt));
    }

    expect_newline_or_eof(tokens, state);

    ExprStmt stmt{};
    stmt.expr = first_expr;
    return add_stmt(state.arena, start.line, start.col, std::move(stmt));
}

inline StmtId parse_if_stmt(const std::vector<Token>& tokens, ParserState& state) {
    const Token start = match(tokens, state, "KEYWORD", "if");
    const ExprId condition = parse_expr(tokens, state);
    expect_newline_or_eof(tokens, state);

    std::vector<StmtId> then_body;
    while (!state.has_error &&
           !check(tokens, state, "KEYWORD", "else if") &&
           !check(tokens, state, "KEYWORD", "else") &&
           !check(tokens, state, "KEYWORD", "end if")) {
        if (check(tokens, state, "EOF")) {
            set_parse_error(state, current(tokens, state), "Unclosed 'if' statement, expected 'end if'");
            break;
        }
        then_body.push_back(parse_statement(tokens, state));
        skip_newlines(tokens, state);
    }

    std::vector<ElseIfBranch> elif_branches;
    while (!state.has_error && check(tokens, state, "KEYWORD", "else if")) {
        const Token elif_tok = match(tokens, state, "KEYWORD", "else if");
        const ExprId elif_cond = parse_expr(tokens, state);
        expect_newline_or_eof(tokens, state);

        std::vector<StmtId> elif_body;
        while (!state.has_error &&
               !check(tokens, state, "KEYWORD", "else if") &&
               !check(tokens, state, "KEYWORD", "else") &&
               !check(tokens, state, "KEYWORD", "end if")) {
            if (check(tokens, state, "EOF")) {
                set_parse_error(state, current(tokens, state), "Unclosed 'else if' block, expected 'end if'");
                break;
            }
            elif_body.push_back(parse_statement(tokens, state));
            skip_newlines(tokens, state);
        }
        ElseIfBranch branch{};
        branch.line = elif_tok.line;
        branch.col = elif_tok.col;
        branch.condition = elif_cond;
        branch.body = std::move(elif_body);
        elif_branches.push_back(std::move(branch));
    }

    bool has_else = false;
    std::vector<StmtId> else_body;
    if (check(tokens, state, "KEYWORD", "else")) {
        match(tokens, state, "KEYWORD", "else");
        expect_newline_or_eof(tokens, state);
        has_else = true;

        while (!state.has_error && !check(tokens, state, "KEYWORD", "end if")) {
            if (check(tokens, state, "EOF")) {
                set_parse_error(state, current(tokens, state), "Unclosed 'else' block, expected 'end if'");
                break;
            }
            else_body.push_back(parse_statement(tokens, state));
            skip_newlines(tokens, state);
        }
    }

    match(tokens, state, "KEYWORD", "end if");
    expect_newline_or_eof(tokens, state);

    IfStmt stmt{};
    stmt.condition = condition;
    stmt.then_body = std::move(then_body);
    stmt.elif_branches = std::move(elif_branches);
    stmt.has_else = has_else;
    stmt.else_body = std::move(else_body);
    return add_stmt(state.arena, start.line, start.col, std::move(stmt));
}

inline StmtId parse_for_stmt(const std::vector<Token>& tokens, ParserState& state) {
    const Token start = match(tokens, state, "KEYWORD", "for");
    std::vector<std::string> loop_vars;
    loop_vars.push_back(match(tokens, state, "IDENT").value);
    while (check(tokens, state, "COMMA")) {
        match(tokens, state, "COMMA");
        loop_vars.push_back(match(tokens, state, "IDENT").value);
    }

    RangeSpec range_spec{};
    bool has_iterable = false;
    ExprId iterable = INVALID_ID;

    if (check(tokens, state, "ASSIGN")) {
        if (loop_vars.size() != 1) {
            set_parse_error(state, start, "Range 'for' loop only supports a single loop variable");
            return INVALID_ID;
        }
        const Token eq_tok = match(tokens, state, "ASSIGN");
        const ExprId start_expr = parse_expr(tokens, state);
        match(tokens, state, "KEYWORD", "to");
        const ExprId end_expr = parse_expr(tokens, state);
        bool has_step = false;
        ExprId step_expr = INVALID_ID;
        if (check(tokens, state, "KEYWORD", "step")) {
            match(tokens, state, "KEYWORD", "step");
            has_step = true;
            step_expr = parse_expr(tokens, state);
        }
        range_spec.has_range = true;
        range_spec.line = eq_tok.line;
        range_spec.col = eq_tok.col;
        range_spec.start = start_expr;
        range_spec.end = end_expr;
        range_spec.has_step = has_step;
        range_spec.step = step_expr;
        range_spec.inclusive = true;
    } else {
        match(tokens, state, "KEYWORD", "in");
        has_iterable = true;
        iterable = parse_expr(tokens, state);
    }

    expect_newline_or_eof(tokens, state);

    std::vector<StmtId> body;
    while (!state.has_error && !check(tokens, state, "KEYWORD", "end for")) {
        if (check(tokens, state, "EOF")) {
            set_parse_error(state, current(tokens, state), "Unclosed 'for' loop, expected 'end for'");
            break;
        }
        body.push_back(parse_statement(tokens, state));
        skip_newlines(tokens, state);
    }

    match(tokens, state, "KEYWORD", "end for");
    expect_newline_or_eof(tokens, state);

    ForStmt stmt{};
    stmt.loop_vars = std::move(loop_vars);
    stmt.range_spec = range_spec;
    stmt.has_iterable = has_iterable;
    stmt.iterable = iterable;
    stmt.body = std::move(body);
    return add_stmt(state.arena, start.line, start.col, std::move(stmt));
}

inline StmtId parse_select_stmt(const std::vector<Token>& tokens, ParserState& state) {
    const Token start = match(tokens, state, "KEYWORD", "select case");
    const ExprId target = parse_expr(tokens, state);
    expect_newline_or_eof(tokens, state);

    std::vector<SelectCase> cases;
    while (!state.has_error && check(tokens, state, "KEYWORD", "case")) {
        const Token case_tok = match(tokens, state, "KEYWORD", "case");
        const ExprId match_expr = parse_expr(tokens, state);
        expect_newline_or_eof(tokens, state);

        std::vector<StmtId> case_body;
        while (!state.has_error &&
               !check(tokens, state, "KEYWORD", "case") &&
               !check(tokens, state, "KEYWORD", "case else") &&
               !check(tokens, state, "KEYWORD", "end select")) {
            if (check(tokens, state, "EOF")) {
                set_parse_error(state, current(tokens, state), "Unclosed 'case' in 'select case', expected 'end select'");
                break;
            }
            case_body.push_back(parse_statement(tokens, state));
            skip_newlines(tokens, state);
        }
        SelectCase sc{};
        sc.line = case_tok.line;
        sc.col = case_tok.col;
        sc.match_expr = match_expr;
        sc.body = std::move(case_body);
        cases.push_back(std::move(sc));
    }

    bool has_case_else = false;
    std::vector<StmtId> case_else;
    if (check(tokens, state, "KEYWORD", "case else")) {
        match(tokens, state, "KEYWORD", "case else");
        expect_newline_or_eof(tokens, state);
        has_case_else = true;

        while (!state.has_error && !check(tokens, state, "KEYWORD", "end select")) {
            if (check(tokens, state, "EOF")) {
                set_parse_error(state, current(tokens, state), "Unclosed 'case else', expected 'end select'");
                break;
            }
            case_else.push_back(parse_statement(tokens, state));
            skip_newlines(tokens, state);
        }
    }

    match(tokens, state, "KEYWORD", "end select");
    expect_newline_or_eof(tokens, state);

    SelectStmt stmt{};
    stmt.target = target;
    stmt.cases = std::move(cases);
    stmt.has_case_else = has_case_else;
    stmt.case_else = std::move(case_else);
    return add_stmt(state.arena, start.line, start.col, std::move(stmt));
}

inline StmtId parse_on_error_stmt(const std::vector<Token>& tokens, ParserState& state) {
    const Token start = match(tokens, state, "KEYWORD", "on error");
    const Token err_ident = match(tokens, state, "IDENT");
    expect_newline_or_eof(tokens, state);

    std::vector<StmtId> body;
    while (!state.has_error && !check(tokens, state, "KEYWORD", "end error")) {
        if (check(tokens, state, "EOF")) {
            set_parse_error(state, current(tokens, state), "Unclosed 'on error' block, expected 'end error'");
            break;
        }
        body.push_back(parse_statement(tokens, state));
        skip_newlines(tokens, state);
    }

    match(tokens, state, "KEYWORD", "end error");
    expect_newline_or_eof(tokens, state);

    ErrorBlock stmt{};
    stmt.err_var = err_ident.value;
    stmt.body = std::move(body);
    return add_stmt(state.arena, start.line, start.col, std::move(stmt));
}

inline StmtId parse_defer_stmt(const std::vector<Token>& tokens, ParserState& state) {
    const Token start = match(tokens, state, "KEYWORD", "defer");
    expect_newline_or_eof(tokens, state);

    std::vector<StmtId> body;
    while (!state.has_error && !check(tokens, state, "KEYWORD", "end defer")) {
        if (check(tokens, state, "EOF")) {
            set_parse_error(state, current(tokens, state), "Unclosed 'defer' block, expected 'end defer'");
            break;
        }
        body.push_back(parse_statement(tokens, state));
        skip_newlines(tokens, state);
    }

    match(tokens, state, "KEYWORD", "end defer");
    expect_newline_or_eof(tokens, state);

    DeferStmt stmt{};
    stmt.body = std::move(body);
    return add_stmt(state.arena, start.line, start.col, std::move(stmt));
}

inline StmtId parse_return_stmt(const std::vector<Token>& tokens, ParserState& state) {
    const Token start = match(tokens, state, "KEYWORD", "return");
    ReturnStmt stmt{};
    if (!check(tokens, state, "NEWLINE") && !check(tokens, state, "EOF")) {
        stmt.has_value = true;
        stmt.value = parse_expr(tokens, state);
    }
    expect_newline_or_eof(tokens, state);

    return add_stmt(state.arena, start.line, start.col, std::move(stmt));
}

inline StmtId parse_break_stmt(const std::vector<Token>& tokens, ParserState& state) {
    const Token start = match(tokens, state, "KEYWORD", "break");
    expect_newline_or_eof(tokens, state);

    BreakStmt stmt{};
    return add_stmt(state.arena, start.line, start.col, std::move(stmt));
}

inline StmtId parse_statement(const std::vector<Token>& tokens, ParserState& state) {
    skip_newlines(tokens, state);
    if (state.has_error) {
        return INVALID_ID;
    }

    if (check(tokens, state, "KEYWORD", "if")) {
        return parse_if_stmt(tokens, state);
    }
    if (check(tokens, state, "KEYWORD", "for")) {
        return parse_for_stmt(tokens, state);
    }
    if (check(tokens, state, "KEYWORD", "select case")) {
        return parse_select_stmt(tokens, state);
    }
    if (check(tokens, state, "KEYWORD", "on error")) {
        return parse_on_error_stmt(tokens, state);
    }
    if (check(tokens, state, "KEYWORD", "defer")) {
        return parse_defer_stmt(tokens, state);
    }
    if (check(tokens, state, "KEYWORD", "return")) {
        return parse_return_stmt(tokens, state);
    }
    if (check(tokens, state, "KEYWORD", "break")) {
        return parse_break_stmt(tokens, state);
    }

    return parse_assignment_or_expr_stmt(tokens, state);
}

// ==========================================
// Top-Level Declarations
// ==========================================

inline DeclId parse_import_decl(const std::vector<Token>& tokens, ParserState& state) {
    const Token start = match(tokens, state, "KEYWORD", "import");
    std::string module_path;
    if (check(tokens, state, "STRING")) {
        const Token str_tok = match(tokens, state, "STRING");
        module_path = strip_quotes(str_tok.value);
    } else {
        module_path = match(tokens, state, "IDENT").value;
        while (check(tokens, state, "DOT")) {
            match(tokens, state, "DOT");
            module_path += "." + match(tokens, state, "IDENT").value;
        }
    }

    bool has_alias = false;
    std::string alias;
    if (check(tokens, state, "KEYWORD", "as")) {
        match(tokens, state, "KEYWORD", "as");
        has_alias = true;
        alias = match(tokens, state, "IDENT").value;
    }
    expect_newline_or_eof(tokens, state);

    ImportDecl decl{};
    decl.kind = "module";
    decl.target = std::move(module_path);
    decl.has_alias = has_alias;
    decl.alias = std::move(alias);
    return add_decl(state.arena, start.line, start.col, std::move(decl));
}

inline DeclId parse_import_header_decl(const std::vector<Token>& tokens, ParserState& state) {
    const Token start = match(tokens, state, "KEYWORD", "import header");
    const Token str_tok = match(tokens, state, "STRING");
    expect_newline_or_eof(tokens, state);

    ImportDecl decl{};
    decl.kind = "header";
    decl.target = strip_quotes(str_tok.value);
    return add_decl(state.arena, start.line, start.col, std::move(decl));
}

inline DeclId parse_import_link_decl(const std::vector<Token>& tokens, ParserState& state) {
    const Token start = match(tokens, state, "KEYWORD", "import link");
    const Token str_tok = match(tokens, state, "STRING");
    expect_newline_or_eof(tokens, state);

    ImportDecl decl{};
    decl.kind = "link";
    decl.target = strip_quotes(str_tok.value);
    return add_decl(state.arena, start.line, start.col, std::move(decl));
}

inline DeclId parse_alias_decl(const std::vector<Token>& tokens, ParserState& state, bool is_pub, const Token& start_tok) {
    match(tokens, state, "KEYWORD", "alias");
    const Token name_tok = match(tokens, state, "IDENT");
    match(tokens, state, "ASSIGN");
    const TypeId target_type = parse_type(tokens, state);
    expect_newline_or_eof(tokens, state);

    AliasDecl decl{};
    decl.name = name_tok.value;
    decl.target_type = target_type;
    decl.is_pub = is_pub;
    return add_decl(state.arena, start_tok.line, start_tok.col, std::move(decl), "", state.file_name, name_tok.line, name_tok.line, name_tok.col);
}

inline DeclId parse_type_decl(const std::vector<Token>& tokens, ParserState& state, bool is_pub, bool is_extern, const Token& start_tok) {
    match(tokens, state, "KEYWORD", "type");
    const Token name_tok = match(tokens, state, "IDENT");
    expect_newline_or_eof(tokens, state);

    std::vector<FieldDecl> fields;
    if (is_extern && !check(tokens, state, "KEYWORD", "end type")) {
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
            decl.name = name_tok.value;
            decl.fields = std::move(fields);
            decl.is_pub = is_pub;
            decl.is_extern = is_extern;
            return add_decl(state.arena, start_tok.line, start_tok.col, std::move(decl), "", state.file_name, name_tok.line, name_tok.line, name_tok.col);
        }
    }

    while (!state.has_error && !check(tokens, state, "KEYWORD", "end type")) {
        if (check(tokens, state, "EOF")) {
            set_parse_error(state, current(tokens, state), "Unclosed 'type " + name_tok.value + "', expected 'end type'");
            break;
        }
        const Token field_tok = match(tokens, state, "IDENT");
        match(tokens, state, "KEYWORD", "as");
        const TypeId field_type = parse_type(tokens, state);
        FieldDecl fd{};
        fd.line = field_tok.line;
        fd.col = field_tok.col;
        fd.name = field_tok.value;
        fd.type_id = field_type;
        fields.push_back(std::move(fd));
        expect_newline_or_eof(tokens, state);
    }

    const Token end_tok = match(tokens, state, "KEYWORD", "end type");
    expect_newline_or_eof(tokens, state);

    TypeDecl decl{};
    decl.name = name_tok.value;
    decl.fields = std::move(fields);
    decl.is_pub = is_pub;
    decl.is_extern = is_extern;
    return add_decl(state.arena, start_tok.line, start_tok.col, std::move(decl), "", state.file_name, end_tok.line, name_tok.line, name_tok.col);
}

inline Param parse_param(const std::vector<Token>& tokens, ParserState& state) {
    bool is_byref = false;
    if (check(tokens, state, "KEYWORD", "byref")) {
        match(tokens, state, "KEYWORD", "byref");
        is_byref = true;
    }
    const Token name_tok = match(tokens, state, "IDENT");
    match(tokens, state, "KEYWORD", "as");
    const TypeId t_node = parse_type(tokens, state);
    Param p{};
    p.line = name_tok.line;
    p.col = name_tok.col;
    p.name = name_tok.value;
    p.type_id = t_node;
    p.is_byref = is_byref;
    return p;
}

inline DeclId parse_fn_decl(const std::vector<Token>& tokens, ParserState& state, bool is_pub, bool is_extern, const Token& start_tok) {
    match(tokens, state, "KEYWORD", "fn");
    const Token name_tok = match(tokens, state, "IDENT");
    match(tokens, state, "LPAREN");

    std::vector<Param> params;
    if (!check(tokens, state, "RPAREN")) {
        params.push_back(parse_param(tokens, state));
        while (check(tokens, state, "COMMA")) {
            match(tokens, state, "COMMA");
            if (check(tokens, state, "RPAREN")) {
                break;
            }
            params.push_back(parse_param(tokens, state));
        }
    }
    match(tokens, state, "RPAREN");

    TypeId return_type = find_or_add_type(state.arena, "void", {}, start_tok.line, start_tok.col);
    bool is_fallible = false;
    if (check(tokens, state, "KEYWORD", "as")) {
        match(tokens, state, "KEYWORD", "as");
        if (check(tokens, state, "TYPE_KW", "err")) {
            match(tokens, state, "TYPE_KW", "err");
            is_fallible = true;
        } else {
            return_type = parse_type(tokens, state);
        }
    }
    if (check(tokens, state, "KEYWORD", "can error")) {
        match(tokens, state, "KEYWORD", "can error");
        is_fallible = true;
    }
    expect_newline_or_eof(tokens, state);

    std::vector<StmtId> body;
    if (is_extern && !check(tokens, state, "KEYWORD", "end fn")) {
        FnDecl decl{};
        decl.name = name_tok.value;
        decl.params = std::move(params);
        decl.return_type = return_type;
        decl.is_fallible = is_fallible;
        decl.body = std::move(body);
        decl.is_pub = is_pub;
        decl.is_extern = true;
        return add_decl(state.arena, start_tok.line, start_tok.col, std::move(decl), "", state.file_name, name_tok.line, name_tok.line, name_tok.col);
    }

    while (!state.has_error && !check(tokens, state, "KEYWORD", "end fn")) {
        if (check(tokens, state, "EOF")) {
            set_parse_error(state, current(tokens, state), "Unclosed 'fn " + name_tok.value + "', expected 'end fn'");
            break;
        }
        body.push_back(parse_statement(tokens, state));
        skip_newlines(tokens, state);
    }

    const Token end_tok = match(tokens, state, "KEYWORD", "end fn");
    expect_newline_or_eof(tokens, state);

    FnDecl decl{};
    decl.name = name_tok.value;
    decl.params = std::move(params);
    decl.return_type = return_type;
    decl.is_fallible = is_fallible;
    decl.body = std::move(body);
    decl.is_pub = is_pub;
    decl.is_extern = is_extern;
    return add_decl(state.arena, start_tok.line, start_tok.col, std::move(decl), "", state.file_name, end_tok.line, name_tok.line, name_tok.col);
}

inline DeclId parse_top_level_decl(const std::vector<Token>& tokens, ParserState& state) {
    if (state.has_error) {
        return INVALID_ID;
    }
    const Token tok = current(tokens, state);

    if (check(tokens, state, "KEYWORD", "import")) {
        return parse_import_decl(tokens, state);
    }
    if (check(tokens, state, "KEYWORD", "import header")) {
        return parse_import_header_decl(tokens, state);
    }
    if (check(tokens, state, "KEYWORD", "import link")) {
        return parse_import_link_decl(tokens, state);
    }

    bool is_pub = false;
    bool is_extern = false;
    while (check(tokens, state, "KEYWORD", "pub") || check(tokens, state, "KEYWORD", "extern")) {
        if (check(tokens, state, "KEYWORD", "pub")) {
            match(tokens, state, "KEYWORD", "pub");
            is_pub = true;
        } else if (check(tokens, state, "KEYWORD", "extern")) {
            match(tokens, state, "KEYWORD", "extern");
            is_extern = true;
        }
    }

    if (check(tokens, state, "KEYWORD", "alias")) {
        if (is_extern) {
            set_parse_error(state, current(tokens, state), "'extern' cannot be applied to 'alias'");
            return INVALID_ID;
        }
        return parse_alias_decl(tokens, state, is_pub, tok);
    }

    if (check(tokens, state, "KEYWORD", "type")) {
        return parse_type_decl(tokens, state, is_pub, is_extern, tok);
    }

    if (check(tokens, state, "KEYWORD", "fn")) {
        return parse_fn_decl(tokens, state, is_pub, is_extern, tok);
    }

    set_parse_error(state, current(tokens, state), "Expected top-level declaration ('import', 'alias', 'type', or 'fn')");
    return INVALID_ID;
}

// ==========================================
// Main Parse Entry Point
// ==========================================

inline ParseResult parse_tokens_with_arena(
    const std::vector<Token>& tokens,
    AstArena arena,
    std::string_view file_name = "main.cb"
) {
    ParserState state{};
    state.file_name = std::string(file_name);
    state.arena = std::move(arena);

    Program prog{};
    prog.file_name = state.file_name;

    skip_newlines(tokens, state);
    while (!state.has_error && !check(tokens, state, "EOF")) {
        const DeclId decl_id = parse_top_level_decl(tokens, state);
        if (state.has_error) {
            break;
        }
        prog.declarations.push_back(decl_id);
        skip_newlines(tokens, state);
    }

    ParseResult result{};
    if (state.has_error) {
        result.ok = false;
        result.error = std::move(state.error);
    } else {
        result.ok = true;
        result.program = std::move(prog);
        result.arena = std::move(state.arena);
    }
    return result;
}

inline ParseResult parse_tokens(const std::vector<Token>& tokens, std::string_view file_name = "main.cb") {
    return parse_tokens_with_arena(tokens, AstArena{}, file_name);
}

}  // namespace cb

#endif  // CLEANBASIC_PARSER_HPP
