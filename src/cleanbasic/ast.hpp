#ifndef CLEANBASIC_AST_HPP
#define CLEANBASIC_AST_HPP

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

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
    std::string err_var;
    std::vector<StmtId> body;
};

struct VarAssignment {
    ExprId target = INVALID_ID;
    bool is_dim = false;
    bool is_const = false;
    bool has_declared_type = false;
    TypeId declared_type = INVALID_ID;
    ExprId value = INVALID_ID;
    bool implicit_bubble = false;
};

struct ExprStmt {
    ExprId expr = INVALID_ID;
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
    bool inclusive = false;
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

struct ReturnStmt {
    bool has_value = false;
    ExprId value = INVALID_ID;
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
    ErrorBlock,
    DeferStmt,
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
    std::string resolved_module_id = "";
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
    TypeId return_type = INVALID_ID;
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
    uint32_t end_line = 1;
    uint32_t name_line = 1;
    uint32_t name_col = 1;
    DeclVariant payload;
    std::string module_id = "";
    std::string file_name = "main.cb";
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

inline std::string strip_module_prefix(std::string_view name) {
    const size_t dot_idx = name.rfind('.');
    if (dot_idx != std::string_view::npos) {
        return std::string(name.substr(dot_idx + 1));
    }
    return std::string(name);
}

inline const TypeNode& empty_type_node() {
    static const TypeNode kEmpty{};
    return kEmpty;
}

inline const TypeNode& get_type_node(const AstArena& arena, TypeId tid) {
    if (tid < arena.types.size()) {
        return arena.types[tid];
    }
    return empty_type_node();
}

inline bool is_void_type(const AstArena& arena, TypeId tid) {
    if (tid >= arena.types.size()) {
        return true;
    }
    return arena.types[tid].name == "void";
}

inline TypeId find_type(
    const AstArena& arena,
    std::string_view name,
    const std::vector<TypeId>& type_args = {}
) {
    for (size_t i = 0; i < arena.types.size(); ++i) {
        const TypeNode& existing = arena.types[i];
        if (existing.name == name &&
            existing.type_args == type_args) {
            return static_cast<TypeId>(i);
        }
    }
    return INVALID_ID;
}

inline TypeId add_type(AstArena& arena, TypeNode node) {
    const TypeId existing = find_type(arena, node.name, node.type_args);
    if (existing != INVALID_ID) {
        return existing;
    }
    const TypeId id = static_cast<TypeId>(arena.types.size());
    arena.types.push_back(std::move(node));
    return id;
}

inline TypeId find_or_add_type(
    AstArena& arena,
    std::string_view name,
    const std::vector<TypeId>& type_args = {},
    uint32_t line = 1,
    uint32_t col = 1
) {
    const TypeId existing = find_type(arena, name, type_args);
    if (existing != INVALID_ID) {
        return existing;
    }
    TypeNode node{};
    node.line = line;
    node.col = col;
    node.name = std::string(name);
    node.type_args = type_args;
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

inline std::string normalize_rel_path(std::string_view raw_path) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= raw_path.size()) {
        size_t end = start;
        while (end < raw_path.size() && raw_path[end] != '/' && raw_path[end] != '\0') {
            end += 1;
        }
        const std::string_view seg = raw_path.substr(start, end - start);
        if (!seg.empty() && seg != ".") {
            if (seg == "..") {
                if (!parts.empty() && parts.back() != "..") {
                    parts.pop_back();
                } else {
                    parts.push_back("..");
                }
            } else {
                parts.emplace_back(seg);
            }
        }
        if (end >= raw_path.size()) {
            break;
        }
        start = end + 1;
    }
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) {
            out += "/";
        }
        out += parts[i];
    }
    return out;
}

inline std::string parent_dir_of(std::string_view file_path) {
    const std::string sep_chars = std::string("/") + std::string(1, static_cast<char>(92));
    const size_t slash_idx = file_path.find_last_of(sep_chars);
    if (slash_idx != std::string_view::npos) {
        if (slash_idx == 0) {
            return "/";
        }
        return std::string(file_path.substr(0, slash_idx));
    }
    return ".";
}

inline std::string get_env_var(const std::string& name) {
    if (std::getenv(name.c_str()) != nullptr) {
        return std::string(std::getenv(name.c_str()));
    }
    return "";
}

inline std::string detect_executable_path() {
#if defined(_WIN32)
    std::array<char, 4096> buf{};
    const DWORD len = GetModuleFileNameA(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (len > 0 && len < buf.size()) {
        std::error_code ec;
        const std::filesystem::path p(std::string(buf.data(), static_cast<size_t>(len)));
        return p.generic_string();
    }
    return "";
#elif defined(__APPLE__)
    std::array<char, 4096> buf{};
    uint32_t size = static_cast<uint32_t>(buf.size());
    if (_NSGetExecutablePath(buf.data(), &size) == 0) {
        std::error_code ec;
        const std::filesystem::path p = std::filesystem::canonical(std::filesystem::path(buf.data()), ec);
        if (!ec) {
            return p.generic_string();
        }
        return std::filesystem::path(buf.data()).generic_string();
    }
    return "";
#else
    std::error_code ec;
    const std::filesystem::path exe_p = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec && !exe_p.empty()) {
        return exe_p.generic_string();
    }
    return "";
#endif
}

inline bool is_valid_sdk_root(std::string_view candidate_dir) {
    if (candidate_dir.empty()) {
        return false;
    }
    std::error_code ec1;
    std::error_code ec2;
    const std::filesystem::path base{std::string(candidate_dir)};
    const bool has_stdlib = std::filesystem::is_directory(base / "stdlib", ec1);
    const bool has_vendor = std::filesystem::is_directory(base / "vendor", ec2);
    return !ec1 && !ec2 && has_stdlib && has_vendor;
}

inline std::string strip_trailing_slashes(std::string_view path) {
    const char bslash = static_cast<char>(92);
    size_t end = path.size();
    while (end > 1 && (path[end - 1] == '/' || path[end - 1] == bslash)) {
        end -= 1;
    }
    return std::string(path.substr(0, end));
}

inline std::string compile_time_root_dir() {
#ifdef CLEANBASIC_ROOT_DIR
    return std::string(CLEANBASIC_ROOT_DIR);
#else
    return ".";
#endif
}

inline std::string resolve_sdk_root_dir(
    std::string_view env_home,
    std::string_view exe_path,
    std::string_view compile_time_root
) {
    if (!env_home.empty()) {
        const std::string clean_env = strip_trailing_slashes(env_home);
        if (is_valid_sdk_root(clean_env)) {
            return clean_env;
        }
        std::error_code ec;
        if (std::filesystem::is_directory(std::filesystem::path(clean_env), ec) && !ec) {
            return clean_env;
        }
    }

    if (!exe_path.empty()) {
        const std::string exe_dir = parent_dir_of(exe_path);
        if (is_valid_sdk_root(exe_dir)) {
            return exe_dir;
        }
        const std::string exe_parent = parent_dir_of(exe_dir);
        if (exe_parent != exe_dir && is_valid_sdk_root(exe_parent)) {
            return exe_parent;
        }
        const std::string exe_grandparent = parent_dir_of(exe_parent);
        if (exe_grandparent != exe_parent && is_valid_sdk_root(exe_grandparent)) {
            return exe_grandparent;
        }
    }

    if (!compile_time_root.empty() && is_valid_sdk_root(compile_time_root)) {
        return std::string(compile_time_root);
    }

    if (is_valid_sdk_root(".")) {
        return ".";
    }

    if (!compile_time_root.empty()) {
        return std::string(compile_time_root);
    }
    return ".";
}

inline std::string cleanbasic_root_dir() {
    return resolve_sdk_root_dir(
        get_env_var("CLEANBASIC_HOME"),
        detect_executable_path(),
        compile_time_root_dir()
    );
}

inline bool is_std_import_target(std::string_view import_target) {
    return import_target.size() >= 5 && import_target.substr(0, 4) == "std/";
}

inline std::string strip_cb_ext(std::string_view path) {
    if (path.size() > 3 && path.substr(path.size() - 3) == ".cb") {
        return std::string(path.substr(0, path.size() - 3));
    }
    return std::string(path);
}

inline std::string resolve_module_id(std::string_view importer_module_id, std::string_view import_target) {
    if (is_std_import_target(import_target)) {
        return normalize_rel_path(strip_cb_ext(import_target));
    }
    std::string importer_dir;
    if (!importer_module_id.empty()) {
        const size_t slash_idx = importer_module_id.find_last_of('/');
        if (slash_idx != std::string_view::npos) {
            importer_dir = std::string(importer_module_id.substr(0, slash_idx));
        }
    }
    std::string combined;
    if (!importer_dir.empty()) {
        combined = importer_dir + "/" + std::string(import_target);
    } else {
        combined = std::string(import_target);
    }
    return strip_cb_ext(normalize_rel_path(combined));
}

inline std::string default_import_alias(std::string_view import_target) {
    std::string alias(import_target);
    const size_t slash_idx = alias.find_last_of('/');
    if (slash_idx != std::string::npos) {
        alias = alias.substr(slash_idx + 1);
    }
    if (alias.size() > 3 && alias.substr(alias.size() - 3) == ".cb") {
        alias = alias.substr(0, alias.size() - 3);
    } else {
        alias = strip_module_prefix(alias);
    }
    return alias;
}

inline std::string sanitize_module_id(std::string_view module_id) {
    std::string out;
    out.reserve(module_id.size());
    for (const char c : module_id) {
        const bool is_alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        const bool is_num = (c >= '0' && c <= '9');
        if (is_alpha || is_num || c == '_') {
            out.push_back(c);
        } else {
            out.push_back('_');
        }
    }
    return out;
}

inline std::string symbol_key(std::string_view module_id, std::string_view name) {
    if (module_id.empty()) {
        return std::string(name);
    }
    return std::string(module_id) + "::" + std::string(name);
}

inline std::pair<std::string, std::string> split_symbol_key(std::string_view key) {
    const size_t sep = key.find("::");
    if (sep != std::string_view::npos) {
        return {std::string(key.substr(0, sep)), std::string(key.substr(sep + 2))};
    }
    return {"", std::string(key)};
}

inline std::string make_cpp_type_name(std::string_view module_id, std::string_view name, bool is_extern = false) {
    if (is_extern || module_id.empty()) {
        return std::string(name);
    }
    return "cb_" + sanitize_module_id(module_id) + "__" + std::string(name);
}

inline std::string make_cpp_fn_name(std::string_view module_id, std::string_view name, bool is_extern = false) {
    if (is_extern) {
        return std::string(name);
    }
    if (module_id.empty()) {
        if (name == "main") {
            return "cb_main";
        }
        return std::string(name);
    }
    return "cb_" + sanitize_module_id(module_id) + "__" + std::string(name);
}

inline DeclId add_decl(
    AstArena& arena,
    uint32_t line,
    uint32_t col,
    DeclVariant payload,
    std::string_view module_id = "",
    std::string_view file_name = "main.cb",
    uint32_t end_line = 0,
    uint32_t name_line = 0,
    uint32_t name_col = 0
) {
    const DeclId id = static_cast<DeclId>(arena.decls.size());
    DeclNode node{};
    node.line = line;
    node.col = col;
    if (end_line > 0) {
        node.end_line = end_line;
    } else {
        node.end_line = line;
    }
    if (name_line > 0) {
        node.name_line = name_line;
    } else {
        node.name_line = line;
    }
    if (name_col > 0) {
        node.name_col = name_col;
    } else {
        node.name_col = col;
    }
    node.payload = std::move(payload);
    node.module_id = std::string(module_id);
    node.file_name = std::string(file_name);
    arena.decls.push_back(std::move(node));
    return id;
}

}  // namespace cb

#endif  // CLEANBASIC_AST_HPP
