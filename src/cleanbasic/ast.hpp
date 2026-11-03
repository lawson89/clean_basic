#ifndef CLEANBASIC_AST_HPP
#define CLEANBASIC_AST_HPP

#include <cstdint>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace cb {

using TypeId = uint32_t;
using ExprId = uint32_t;
using StmtId = uint32_t;
using DeclId = uint32_t;

constexpr uint32_t INVALID_ID = UINT32_MAX;

// ==========================================
// Types
// ==========================================

struct TypeNode {
    uint32_t line = 1;
    uint32_t col = 1;
    std::string name;
    std::vector<TypeId> type_args;
    bool is_shared = false;
};

struct Param {
    uint32_t line = 1;
    uint32_t col = 1;
    std::string name;
    TypeId type_id = INVALID_ID;
    bool is_byref = false;
};

struct FieldDecl {
    uint32_t line = 1;
    uint32_t col = 1;
    std::string name;
    TypeId type_id = INVALID_ID;
};

// ==========================================
// Expressions
// ==========================================

struct NumberLiteral {
    std::string raw = "0";
    bool is_float = false;
};

struct StringLiteral {
    std::string value;
    bool is_fstring = false;
};

struct BoolLiteral {
    bool value = false;
};

struct NullLiteral {
    uint32_t dummy = 0;
};

struct Identifier {
    std::string name;
};

struct BinaryOp {
    std::string op;
    ExprId left = INVALID_ID;
    ExprId right = INVALID_ID;
};

struct UnaryOp {
    std::string op;
    ExprId operand = INVALID_ID;
};

struct FieldAccess {
    ExprId target = INVALID_ID;
    std::string field_name;
};

struct IndexAccess {
    ExprId target = INVALID_ID;
    ExprId index = INVALID_ID;
};

struct NamedArg {
    std::string name;
    ExprId value = INVALID_ID;
};

struct FuncCall {
    ExprId callee = INVALID_ID;
    std::vector<ExprId> args;
    std::vector<NamedArg> named_args;
};

struct InitField {
    bool has_name = false;
    std::string name;
    ExprId value = INVALID_ID;
};

struct StructInit {
    std::string type_name;
    std::vector<InitField> fields;
};

struct NewExpr {
    TypeId target_type = INVALID_ID;
    std::vector<InitField> args;
};

struct ListLiteral {
    std::vector<ExprId> elements;
};

struct DictEntry {
    ExprId key = INVALID_ID;
    ExprId value = INVALID_ID;
};

struct DictLiteral {
    std::vector<DictEntry> entries;
};

using ExprVariant = std::variant<
    NumberLiteral,
    StringLiteral,
    BoolLiteral,
    NullLiteral,
    Identifier,
    BinaryOp,
    UnaryOp,
    FieldAccess,
    IndexAccess,
    FuncCall,
    StructInit,
    NewExpr,
    ListLiteral,
    DictLiteral
>;

struct ExprNode {
    uint32_t line = 1;
    uint32_t col = 1;
    ExprVariant payload;
};

// ==========================================
// Statements
// ==========================================

struct ErrorBlock {
    bool has_error_block = false;
    uint32_t line = 1;
    uint32_t col = 1;
    std::string err_var;
    std::vector<StmtId> body;
};

using ErrBlock = ErrorBlock;

struct VarAssignment {
    std::vector<ExprId> targets;
    bool is_dim = false;
    bool is_const = false;
    bool has_declared_type = false;
    TypeId declared_type = INVALID_ID;
    ExprId value = INVALID_ID;
    ErrorBlock error_block;
    bool implicit_bubble = false;
};

using VarDecl = VarAssignment;

struct ExprStmt {
    ExprId expr = INVALID_ID;
    ErrorBlock error_block;
    bool implicit_bubble = false;
};

struct ElseIfBranch {
    uint32_t line = 1;
    uint32_t col = 1;
    ExprId condition = INVALID_ID;
    std::vector<StmtId> body;
};

struct IfStmt {
    ExprId condition = INVALID_ID;
    std::vector<StmtId> then_body;
    std::vector<ElseIfBranch> elif_branches;
    bool has_else = false;
    std::vector<StmtId> else_body;
};

struct RangeSpec {
    bool has_range = false;
    uint32_t line = 1;
    uint32_t col = 1;
    ExprId start = INVALID_ID;
    ExprId end = INVALID_ID;
    bool has_step = false;
    ExprId step = INVALID_ID;
};

struct ForStmt {
    std::vector<std::string> loop_vars;
    RangeSpec range_spec;
    bool has_iterable = false;
    ExprId iterable = INVALID_ID;
    std::vector<StmtId> body;
};

struct SelectCase {
    uint32_t line = 1;
    uint32_t col = 1;
    ExprId match_expr = INVALID_ID;
    std::vector<StmtId> body;
};

struct SelectStmt {
    ExprId target = INVALID_ID;
    std::vector<SelectCase> cases;
    bool has_case_else = false;
    std::vector<StmtId> case_else;
};

struct DeferStmt {
    std::vector<StmtId> body;
};

using DeferBlock = DeferStmt;

struct SpawnStmt {
    ExprId call_expr = INVALID_ID;
};

struct ReturnStmt {
    std::vector<ExprId> values;
};

struct BreakStmt {
    uint32_t dummy = 0;
};

using StmtVariant = std::variant<
    VarAssignment,
    ExprStmt,
    IfStmt,
    ForStmt,
    SelectStmt,
    DeferStmt,
    SpawnStmt,
    ReturnStmt,
    BreakStmt
>;

struct StmtNode {
    uint32_t line = 1;
    uint32_t col = 1;
    StmtVariant payload;
};

// ==========================================
// Top-Level Declarations
// ==========================================

struct ImportDecl {
    std::string kind = "module";
    std::string target;
    bool has_alias = false;
    std::string alias;
};

struct AliasDecl {
    std::string name;
    TypeId target_type = INVALID_ID;
    bool is_pub = false;
};

struct TypeDecl {
    std::string name;
    std::vector<FieldDecl> fields;
    bool is_pub = false;
    bool is_extern = false;
};

struct FnDecl {
    std::string name;
    std::vector<Param> params;
    std::vector<TypeId> return_types;
    bool returns_err = false;
    bool is_fallible = false;
    std::vector<StmtId> body;
    bool is_pub = false;
    bool is_extern = false;
};

using DeclVariant = std::variant<
    ImportDecl,
    AliasDecl,
    TypeDecl,
    FnDecl
>;

struct DeclNode {
    uint32_t line = 1;
    uint32_t col = 1;
    DeclVariant payload;
};

struct AstArena {
    std::vector<TypeNode> types;
    std::vector<ExprNode> exprs;
    std::vector<StmtNode> stmts;
    std::vector<DeclNode> decls;
};

struct Program {
    uint32_t line = 1;
    uint32_t col = 1;
    std::vector<DeclId> declarations;
    std::string file_name = "main.cb";
};

inline TypeId add_type(AstArena& arena, TypeNode node) {
    const TypeId id = static_cast<TypeId>(arena.types.size());
    arena.types.push_back(std::move(node));
    return id;
}

inline ExprId add_expr(AstArena& arena, uint32_t line, uint32_t col, ExprVariant payload) {
    const ExprId id = static_cast<ExprId>(arena.exprs.size());
    ExprNode node{};
    node.line = line;
    node.col = col;
    node.payload = std::move(payload);
    arena.exprs.push_back(std::move(node));
    return id;
}

inline StmtId add_stmt(AstArena& arena, uint32_t line, uint32_t col, StmtVariant payload) {
    const StmtId id = static_cast<StmtId>(arena.stmts.size());
    StmtNode node{};
    node.line = line;
    node.col = col;
    node.payload = std::move(payload);
    arena.stmts.push_back(std::move(node));
    return id;
}

inline DeclId add_decl(AstArena& arena, uint32_t line, uint32_t col, DeclVariant payload) {
    const DeclId id = static_cast<DeclId>(arena.decls.size());
    DeclNode node{};
    node.line = line;
    node.col = col;
    node.payload = std::move(payload);
    arena.decls.push_back(std::move(node));
    return id;
}

}  // namespace cb

#endif  // CLEANBASIC_AST_HPP
