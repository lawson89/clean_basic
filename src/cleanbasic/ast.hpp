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
    std::vector<TypeId> typeArgs;
};

struct Param {
    uint32_t line = 1;
    uint32_t col = 1;
    std::string name;
    TypeId typeId = INVALID_ID;
    bool isByref = false;
};

struct FieldDecl {
    uint32_t line = 1;
    uint32_t col = 1;
    std::string name;
    TypeId typeId = INVALID_ID;
};

// ==========================================
// Expressions
// ==========================================

struct NumberLiteral {
    std::string raw = "0";
    bool isFloat = false;
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
    std::string fieldName;
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
    std::vector<NamedArg> namedArgs;
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

    template <typename T>
    const T* getIf() const {
        return std::get_if<T>(&payload);
    }

    template <typename T>
    T* getIfMut() {
        return std::get_if<T>(&payload);
    }

    template <typename T>
    bool is() const {
        return std::holds_alternative<T>(payload);
    }
};

// ==========================================
// Statements
// ==========================================

struct ErrorBlock {
    std::string errVar;
    std::vector<StmtId> body;
};

struct VarAssignment {
    ExprId target = INVALID_ID;
    bool isDim = false;
    bool isConst = false;
    bool hasDeclaredType = false;
    TypeId declaredType = INVALID_ID;
    ExprId value = INVALID_ID;
    bool implicitBubble = false;
};

struct ExprStmt {
    ExprId expr = INVALID_ID;
    bool implicitBubble = false;
};

struct ElseIfBranch {
    uint32_t line = 1;
    uint32_t col = 1;
    ExprId condition = INVALID_ID;
    std::vector<StmtId> body;
};

struct IfStmt {
    ExprId condition = INVALID_ID;
    std::vector<StmtId> thenBody;
    std::vector<ElseIfBranch> elifBranches;
    bool hasElse = false;
    std::vector<StmtId> elseBody;
};

struct RangeSpec {
    bool hasRange = false;
    uint32_t line = 1;
    uint32_t col = 1;
    ExprId start = INVALID_ID;
    ExprId end = INVALID_ID;
    bool hasStep = false;
    ExprId step = INVALID_ID;
    bool inclusive = false;
};

struct ForStmt {
    std::vector<std::string> loopVars;
    RangeSpec rangeSpec;
    bool hasIterable = false;
    ExprId iterable = INVALID_ID;
    std::vector<StmtId> body;
};

struct SelectCase {
    uint32_t line = 1;
    uint32_t col = 1;
    ExprId matchExpr = INVALID_ID;
    std::vector<ExprId> matchExprs;
    std::vector<StmtId> body;
};

struct SelectStmt {
    ExprId target = INVALID_ID;
    std::vector<SelectCase> cases;
    bool hasCaseElse = false;
    std::vector<StmtId> caseElse;
};

struct DeferStmt {
    std::vector<StmtId> body;
};

struct WhileStmt {
    ExprId condition = INVALID_ID;
    std::vector<StmtId> body;
};

struct ReturnStmt {
    bool hasValue = false;
    ExprId value = INVALID_ID;
};

struct BreakStmt {
    uint32_t dummy = 0;
};

struct ContinueStmt {
    uint32_t dummy = 0;
};

using StmtVariant = std::variant<
    VarAssignment,
    ExprStmt,
    IfStmt,
    ForStmt,
    WhileStmt,
    SelectStmt,
    ErrorBlock,
    DeferStmt,
    ReturnStmt,
    BreakStmt,
    ContinueStmt
>;

struct StmtNode {
    uint32_t line = 1;
    uint32_t col = 1;
    StmtVariant payload;

    template <typename T>
    const T* getIf() const {
        return std::get_if<T>(&payload);
    }

    template <typename T>
    T* getIfMut() {
        return std::get_if<T>(&payload);
    }

    template <typename T>
    bool is() const {
        return std::holds_alternative<T>(payload);
    }
};

// ==========================================
// Top-Level Declarations
// ==========================================

struct ImportDecl {
    std::string kind = "module";
    std::string target;
    bool hasAlias = false;
    std::string alias;
    std::string resolvedModuleId = "";
};

struct AliasDecl {
    std::string name;
    TypeId targetType = INVALID_ID;
    bool isPub = false;
};

struct TypeDecl {
    std::string name;
    std::vector<FieldDecl> fields;
    bool isPub = false;
    bool isExtern = false;
};

struct FnDecl {
    std::string name;
    std::vector<Param> params;
    TypeId returnType = INVALID_ID;
    bool isFallible = false;
    std::vector<StmtId> body;
    bool isPub = false;
    bool isExtern = false;
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
    uint32_t endLine = 1;
    uint32_t nameLine = 1;
    uint32_t nameCol = 1;
    DeclVariant payload;
    std::string moduleId = "";
    std::string fileName = "main.cb";

    template <typename T>
    const T* getIf() const {
        return std::get_if<T>(&payload);
    }

    template <typename T>
    T* getIfMut() {
        return std::get_if<T>(&payload);
    }

    template <typename T>
    bool is() const {
        return std::holds_alternative<T>(payload);
    }
};

inline const TypeNode& emptyTypeNode() {
    static const TypeNode kEmpty{};
    return kEmpty;
}

struct AstArena {
    std::vector<TypeNode> types;
    std::vector<ExprNode> exprs;
    std::vector<StmtNode> stmts;
    std::vector<DeclNode> decls;

    template <typename T>
    const T* getExpr(ExprId id) const {
        if (id >= exprs.size()) {
            return nullptr;
        }
        return exprs[id].getIf<T>();
    }

    template <typename T>
    T* getExprMut(ExprId id) {
        if (id >= exprs.size()) {
            return nullptr;
        }
        return exprs[id].getIfMut<T>();
    }

    template <typename T>
    const T* getStmt(StmtId id) const {
        if (id >= stmts.size()) {
            return nullptr;
        }
        return stmts[id].getIf<T>();
    }

    template <typename T>
    T* getStmtMut(StmtId id) {
        if (id >= stmts.size()) {
            return nullptr;
        }
        return stmts[id].getIfMut<T>();
    }

    template <typename T>
    const T* getDecl(DeclId id) const {
        if (id >= decls.size()) {
            return nullptr;
        }
        return decls[id].getIf<T>();
    }

    template <typename T>
    T* getDeclMut(DeclId id) {
        if (id >= decls.size()) {
            return nullptr;
        }
        return decls[id].getIfMut<T>();
    }

    const TypeNode& getType(TypeId tid) const {
        if (tid < types.size()) {
            return types[tid];
        }
        return emptyTypeNode();
    }

    bool isVoid(TypeId tid) const {
        if (tid >= types.size()) {
            return true;
        }
        return types[tid].name == "void";
    }

    TypeId findType(std::string_view name, const std::vector<TypeId>& typeArgs = {}) const {
        for (size_t i = 0; i < types.size(); ++i) {
            const TypeNode& existing = types[i];
            if (existing.name == name && existing.typeArgs == typeArgs) {
                return static_cast<TypeId>(i);
            }
        }
        return INVALID_ID;
    }

    TypeId addType(TypeNode node) {
        const TypeId existing = findType(node.name, node.typeArgs);
        if (existing != INVALID_ID) {
            return existing;
        }
        const TypeId id = static_cast<TypeId>(types.size());
        types.push_back(std::move(node));
        return id;
    }

    TypeId findOrAddType(
        std::string_view name,
        const std::vector<TypeId>& typeArgs = {},
        uint32_t line = 1,
        uint32_t col = 1
    ) {
        const TypeId existing = findType(name, typeArgs);
        if (existing != INVALID_ID) {
            return existing;
        }
        TypeNode node{};
        node.line = line;
        node.col = col;
        node.name = std::string(name);
        node.typeArgs = typeArgs;
        const TypeId id = static_cast<TypeId>(types.size());
        types.push_back(std::move(node));
        return id;
    }

    ExprId addExpr(uint32_t line, uint32_t col, ExprVariant payload) {
        const ExprId id = static_cast<ExprId>(exprs.size());
        ExprNode node{};
        node.line = line;
        node.col = col;
        node.payload = std::move(payload);
        exprs.push_back(std::move(node));
        return id;
    }

    StmtId addStmt(uint32_t line, uint32_t col, StmtVariant payload) {
        const StmtId id = static_cast<StmtId>(stmts.size());
        StmtNode node{};
        node.line = line;
        node.col = col;
        node.payload = std::move(payload);
        stmts.push_back(std::move(node));
        return id;
    }

    DeclId addDecl(
        uint32_t line,
        uint32_t col,
        DeclVariant payload,
        std::string_view moduleId = "",
        std::string_view fileName = "main.cb",
        uint32_t endLine = 0,
        uint32_t nameLine = 0,
        uint32_t nameCol = 0
    ) {
        const DeclId id = static_cast<DeclId>(decls.size());
        DeclNode node{};
        node.line = line;
        node.col = col;
        if (endLine > 0) {
            node.endLine = endLine;
        } else {
            node.endLine = line;
        }
        if (nameLine > 0) {
            node.nameLine = nameLine;
        } else {
            node.nameLine = line;
        }
        if (nameCol > 0) {
            node.nameCol = nameCol;
        } else {
            node.nameCol = col;
        }
        node.payload = std::move(payload);
        node.moduleId = std::string(moduleId);
        node.fileName = std::string(fileName);
        decls.push_back(std::move(node));
        return id;
    }
};

struct Program {
    uint32_t line = 1;
    uint32_t col = 1;
    std::vector<DeclId> declarations;
    std::string fileName = "main.cb";
};

inline std::string stripModulePrefix(std::string_view name) {
    const size_t dotIdx = name.rfind('.');
    if (dotIdx != std::string_view::npos) {
        return std::string(name.substr(dotIdx + 1));
    }
    return std::string(name);
}

inline const TypeNode& getTypeNode(const AstArena& arena, TypeId tid) {
    return arena.getType(tid);
}

inline bool isVoidType(const AstArena& arena, TypeId tid) {
    return arena.isVoid(tid);
}

inline TypeId findType(
    const AstArena& arena,
    std::string_view name,
    const std::vector<TypeId>& typeArgs = {}
) {
    return arena.findType(name, typeArgs);
}

inline TypeId addType(AstArena& arena, TypeNode node) {
    return arena.addType(std::move(node));
}

inline TypeId findOrAddType(
    AstArena& arena,
    std::string_view name,
    const std::vector<TypeId>& typeArgs = {},
    uint32_t line = 1,
    uint32_t col = 1
) {
    return arena.findOrAddType(name, typeArgs, line, col);
}

inline ExprId addExpr(AstArena& arena, uint32_t line, uint32_t col, ExprVariant payload) {
    return arena.addExpr(line, col, std::move(payload));
}

inline StmtId addStmt(AstArena& arena, uint32_t line, uint32_t col, StmtVariant payload) {
    return arena.addStmt(line, col, std::move(payload));
}

inline std::string normalizeRelPath(std::string_view rawPath) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= rawPath.size()) {
        size_t end = start;
        while (end < rawPath.size() && rawPath[end] != '/' && rawPath[end] != static_cast<char>(92)) {
            end += 1;
        }
        const std::string_view seg = rawPath.substr(start, end - start);
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
        if (end >= rawPath.size()) {
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

inline std::string parentDirOf(std::string_view filePath) {
    const std::string sepChars = std::string("/") + std::string(1, static_cast<char>(92));
    const size_t slashIdx = filePath.find_last_of(sepChars);
    if (slashIdx != std::string_view::npos) {
        if (slashIdx == 0) {
            return "/";
        }
        return std::string(filePath.substr(0, slashIdx));
    }
    return ".";
}

inline std::string getEnvVar(const std::string& name) {
    if (std::getenv(name.c_str()) != nullptr) {
        return std::string(std::getenv(name.c_str()));
    }
    return "";
}

inline std::string detectExecutablePath() {
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
    const std::filesystem::path exeP = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec && !exeP.empty()) {
        return exeP.generic_string();
    }
    return "";
#endif
}

inline bool isValidSdkRoot(std::string_view candidateDir) {
    if (candidateDir.empty()) {
        return false;
    }
    std::error_code ec1;
    std::error_code ec2;
    const std::filesystem::path base{std::string(candidateDir)};
    const bool hasStdlib = std::filesystem::is_directory(base / "stdlib", ec1);
    const bool hasVendor = std::filesystem::is_directory(base / "vendor", ec2);
    return !ec1 && !ec2 && hasStdlib && hasVendor;
}

inline std::string stripTrailingSlashes(std::string_view path) {
    const char bslash = static_cast<char>(92);
    size_t end = path.size();
    while (end > 1 && (path[end - 1] == '/' || path[end - 1] == bslash)) {
        end -= 1;
    }
    return std::string(path.substr(0, end));
}

inline std::string compileTimeRootDir() {
#ifdef CLEANBASIC_ROOT_DIR
    return std::string(CLEANBASIC_ROOT_DIR);
#else
    return ".";
#endif
}

inline std::string resolveSdkRootDir(
    std::string_view envHome,
    std::string_view exePath,
    std::string_view compileTimeRoot
) {
    if (!envHome.empty()) {
        const std::string cleanEnv = stripTrailingSlashes(envHome);
        if (isValidSdkRoot(cleanEnv)) {
            return cleanEnv;
        }
        std::error_code ec;
        if (std::filesystem::is_directory(std::filesystem::path(cleanEnv), ec) && !ec) {
            return cleanEnv;
        }
    }

    if (!exePath.empty()) {
        const std::string exeDir = parentDirOf(exePath);
        if (isValidSdkRoot(exeDir)) {
            return exeDir;
        }
        const std::string exeParent = parentDirOf(exeDir);
        if (exeParent != exeDir && isValidSdkRoot(exeParent)) {
            return exeParent;
        }
        const std::string exeGrandparent = parentDirOf(exeParent);
        if (exeGrandparent != exeParent && isValidSdkRoot(exeGrandparent)) {
            return exeGrandparent;
        }
    }

    if (!compileTimeRoot.empty() && isValidSdkRoot(compileTimeRoot)) {
        return std::string(compileTimeRoot);
    }

    if (isValidSdkRoot(".")) {
        return ".";
    }

    if (!compileTimeRoot.empty()) {
        return std::string(compileTimeRoot);
    }
    return ".";
}

inline std::string cleanbasicRootDir() {
    return resolveSdkRootDir(
        getEnvVar("CLEANBASIC_HOME"),
        detectExecutablePath(),
        compileTimeRootDir()
    );
}

inline bool isStdImportTarget(std::string_view importTarget) {
    return importTarget.size() >= 5 && importTarget.substr(0, 4) == "std/";
}

inline std::string stripCbExt(std::string_view path) {
    if (path.size() > 3 && path.substr(path.size() - 3) == ".cb") {
        return std::string(path.substr(0, path.size() - 3));
    }
    return std::string(path);
}

inline std::string resolveModuleId(std::string_view importerModuleId, std::string_view importTarget) {
    if (isStdImportTarget(importTarget)) {
        return normalizeRelPath(stripCbExt(importTarget));
    }
    std::string importerDir;
    if (!importerModuleId.empty()) {
        const size_t slashIdx = importerModuleId.find_last_of('/');
        if (slashIdx != std::string_view::npos) {
            importerDir = std::string(importerModuleId.substr(0, slashIdx));
        }
    }
    std::string combined;
    if (!importerDir.empty()) {
        combined = importerDir + "/" + std::string(importTarget);
    } else {
        combined = std::string(importTarget);
    }
    return stripCbExt(normalizeRelPath(combined));
}

inline std::string defaultImportAlias(std::string_view importTarget) {
    std::string alias(importTarget);
    const size_t slashIdx = alias.find_last_of('/');
    if (slashIdx != std::string::npos) {
        alias = alias.substr(slashIdx + 1);
    }
    if (alias.size() > 3 && alias.substr(alias.size() - 3) == ".cb") {
        alias = alias.substr(0, alias.size() - 3);
    } else {
        alias = stripModulePrefix(alias);
    }
    return alias;
}

inline std::string sanitizeModuleId(std::string_view moduleId) {
    std::string out;
    out.reserve(moduleId.size());
    for (const char c : moduleId) {
        const bool isAlpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        const bool isNum = (c >= '0' && c <= '9');
        if (isAlpha || isNum || c == '_') {
            out.push_back(c);
        } else {
            out.push_back('_');
        }
    }
    return out;
}

inline std::string symbolKey(std::string_view moduleId, std::string_view name) {
    if (moduleId.empty()) {
        return std::string(name);
    }
    return std::string(moduleId) + "::" + std::string(name);
}

inline std::pair<std::string, std::string> splitSymbolKey(std::string_view key) {
    const size_t sep = key.find("::");
    if (sep != std::string_view::npos) {
        return {std::string(key.substr(0, sep)), std::string(key.substr(sep + 2))};
    }
    return {"", std::string(key)};
}

inline std::string makeCppTypeName(std::string_view moduleId, std::string_view name, bool isExtern = false) {
    if (isExtern || moduleId.empty()) {
        return std::string(name);
    }
    return "cb_" + sanitizeModuleId(moduleId) + "__" + std::string(name);
}

inline std::string makeCppFnName(std::string_view moduleId, std::string_view name, bool isExtern = false) {
    if (isExtern) {
        return std::string(name);
    }
    if (moduleId.empty()) {
        if (name == "main") {
            return "cbMain";
        }
        return std::string(name);
    }
    return "cb_" + sanitizeModuleId(moduleId) + "__" + std::string(name);
}

inline DeclId addDecl(
    AstArena& arena,
    uint32_t line,
    uint32_t col,
    DeclVariant payload,
    std::string_view moduleId = "",
    std::string_view fileName = "main.cb",
    uint32_t endLine = 0,
    uint32_t nameLine = 0,
    uint32_t nameCol = 0
) {
    return arena.addDecl(line, col, std::move(payload), moduleId, fileName, endLine, nameLine, nameCol);
}

// ==========================================
// AST Helpers
// ==========================================

template <typename T>
inline bool isExpr(const AstArena& arena, ExprId id) {
    return id != INVALID_ID && id < arena.exprs.size() && std::holds_alternative<T>(arena.exprs[id].payload);
}

template <typename T>
inline bool isStmt(const AstArena& arena, StmtId id) {
    return id != INVALID_ID && id < arena.stmts.size() && std::holds_alternative<T>(arena.stmts[id].payload);
}

template <typename T>
inline bool isDecl(const AstArena& arena, DeclId id) {
    return id != INVALID_ID && id < arena.decls.size() && std::holds_alternative<T>(arena.decls[id].payload);
}

template <typename T>
inline const T& asExpr(const AstArena& arena, ExprId id) {
    return std::get<T>(arena.exprs[id].payload);
}

template <typename T>
inline const T& asStmt(const AstArena& arena, StmtId id) {
    return std::get<T>(arena.stmts[id].payload);
}

template <typename T>
inline T& asStmtMut(AstArena& arena, StmtId id) {
    return std::get<T>(arena.stmts[id].payload);
}

template <typename T>
inline const T& asDecl(const AstArena& arena, DeclId id) {
    return std::get<T>(arena.decls[id].payload);
}

}  // namespace cb

#endif  // CLEANBASIC_AST_HPP
