# CleanBasic Transpiler Architecture & Arena Design

This document describes the end-to-end architecture of the CleanBasic transpiler, its 5-stage Unity Build compilation pipeline, its strict data-oriented C++17 design, and its command-line interface.

### Related Documentation
* [CleanBasic Language Specification](cleanbasic_specification.md)
* [C++17 Strict Data-Oriented Subset](cpp_subset.md)
* [Lexical Analysis & Recursive-Descent Parsing](lexer_and_parser.md)
* [Multi-File Module Resolution, Scoping & Name Mangling](modules_and_symbols.md)
* [Semantic Analyzer: Type Inference, UFCS, Mutability & Fallibility](semantic_analyzer.md)
* [Unity C++17 Emitter, Runtime Preamble & C/C++ Interoperability](cpp_emitter_and_interop.md)

---

## 1. End-to-End 5-Stage Unity Build Pipeline

The CleanBasic transpiler (`src/cleanbasic/pipeline.hpp`) transforms a root `.cb` source file and all transitively imported modules into a single self-contained C++17 translation unit (`build.cpp`). By emitting a Unity Build file, CleanBasic eliminates C++ header management, forward-declaration mismatches, and multi-object linker bottlenecks while giving `g++` / `clang++` whole-program visibility at `-O3`.

```plantuml
@startuml
skinparam componentStyle rectangle
skinparam monochrome false

actor "Developer / CLI" as Dev
file "Root Source (main.cb)" as MainCB
file "Imported Modules (*.cb)" as ModCB

package "CleanBasic Transpiler (namespace cb)" {
    component "1. Lexer
(lexer.hpp::tokenize)" as Lexer
    component "2. Parser
(parser.hpp::parseTokensWithArena)" as Parser
    component "3. Module Loader
(pipeline.hpp::loadModuleDeclarationsRecursive)" as Loader
    database "Shared Flat AstArena
(ast.hpp::AstArena)
types | exprs | stmts | decls" as Arena
    component "4. Semantic Analyzer
(analyzer.hpp, symbols.hpp)" as Analyzer
    database "GlobalSymbolTable
(symbols.hpp)
types | enums | unions | functions | modules" as SymTab
    component "5. Unity C++17 Emitter
(emitter.hpp, runtime_preamble.hpp)" as Emitter
}

file "Unity C++17 Output (build.cpp)" as BuildCPP
component "C++17 Compiler (g++ -O3 -std=c++17)" as Gpp
file "Native Executable (app)" as Bin

Dev --> MainCB : "cleanbasic build main.cb -c"
MainCB --> Lexer : "std::string_view source"
Lexer --> Parser : "std::vector<Token>"
Parser --> Arena : "Appends TypeNode, ExprNode,
StmtNode, DeclNode"
Parser --> Loader : "Program (root DeclIds)"
Loader --> ModCB : "Resolves relative imports"
ModCB --> Lexer : "Tokenizes unseen modules"
Loader --> Arena : "Merges topological DeclIds
into shared AstArena"
Loader --> Analyzer : "Unified Program + AstArena"
Analyzer --> SymTab : "Pass 1: Register symbols
Pass 2: Validate & infer types"
SymTab --> Emitter : "Resolved symbols & scopes"
Arena --> Emitter : "Read-only AST traversal"
Emitter --> BuildCPP : "Single-file C++17 code"
BuildCPP --> Gpp : "Optional (-c / --compile)"
Gpp --> Bin : "Links -pthread & import link libs"
@enduml
```

### Pipeline Stages (`cb::transpileSource` & `cb::buildFile`)

| Stage | Header & Entry Point | Input | Output | Responsibility |
| :--- | :--- | :--- | :--- | :--- |
| **1. Lexical Analysis** | `src/cleanbasic/lexer.hpp`<br>`cb::tokenize()` | Raw source (`std::string_view`), filename | `LexResult` (`std::vector<Token>` or `DiagnosticError`) | Scans characters into tokens, coalesces two-word keywords (`end if`, `select case`, `can error`, etc.), collapses consecutive newlines, and rejects C-style `&&`, `\|\|`, `!` operators. |
| **2. Syntax Analysis** | `src/cleanbasic/parser.hpp`<br>`cb::parseTokens()` / `cb::parseTokensWithArena()` | `std::vector<Token>`, `AstArena` | `ParseResult` (`Program`, `AstArena`, or `DiagnosticError`) | Recursive-descent parsing of top-level declarations, statements, and precedence-climbed expressions into flat arena vectors. |
| **3. Module Resolution** | `src/cleanbasic/pipeline.hpp`<br>`cb::loadModuleDeclarationsRecursive()` | Root `Program`, `AstArena`, file path | Merged `std::vector<DeclId>` in topological dependency order | Recursively lexes and parses imported `.cb` modules into the **same** `AstArena`, deduplicates diamond dependencies, and detects circular import chains. |
| **4. Semantic Analysis** | `src/cleanbasic/analyzer.hpp`<br>(umbrella for `symbols.hpp`) | Unified `AstArena` & `Program` | `AnalysisResult` (`GlobalSymbolTable` or `DiagnosticError`) | Two-pass registration of types, enums, unions, constants, functions, and module scopes; type inference; UFCS resolution; `const`/`dim` mutability checks; and static `can error` / `try / catch` verification. |
| **5. C++17 Emission** | `src/cleanbasic/emitter.hpp`<br>(with `runtime_preamble.hpp`) | `AstArena`, `GlobalSymbolTable`, `Program` | `std::string` (complete `build.cpp` source) | Emits the C++17 runtime preamble, `#include` headers, forward declarations, POD `struct` definitions, free functions, RAII `cbDefer` guards, tuple error propagation, and `int main()`. |

---

## 2. Data-Oriented C++17 Design (`src/cleanbasic/ast.hpp`)

The CleanBasic compilation pipeline maintains a clear distinction between the **bootstrap transpiler implementation** and the **emitted C++17 target code** (detailed in [`docs/cpp_subset.md`](cpp_subset.md)):
* **Target Output (`build.cpp`):** Strictly procedural POD `struct` definitions, free functions, value semantics, and zero exceptions/classes.
* **Bootstrap Transpiler (`src/cleanbasic/`):** Implemented in idiomatic C++17 using flat `AstArena` contiguous vector storage, stateful pass classes (`Parser`, `Analyzer`, `Emitter`), concise `std::get_if<T>` pointer accessors (`arena.getExpr<T>(id)`, `arena.getStmt<T>(id)`, `arena.getDecl<T>(id)`), and exception-based diagnostic short-circuiting (`throw DiagnosticError`) caught at the top-level API boundary (`parseTokensWithArena`, `analyzeProgram`) so callers always receive value-based `ParseResult` and `AnalysisResult` structs.

### 2.1 Flat `AstArena` Vector Storage & Integer Handles

Instead of heap-allocating individual AST nodes and linking them with pointers or `std::unique_ptr`, CleanBasic stores every node across the entire multi-file program in four contiguous `std::vector` buffers inside `cb::AstArena`:

```cpp
using TypeId = uint32_t;
using ExprId = uint32_t;
using StmtId = uint32_t;
using DeclId = uint32_t;

constexpr uint32_t INVALID_ID = UINT32_MAX;

struct AstArena {
    std::vector<TypeNode> types;
    std::vector<ExprNode> exprs;
    std::vector<StmtNode> stmts;
    std::vector<DeclNode> decls;
};
```

Every parent-to-child edge in the AST is stored as a 32-bit unsigned integer index (`TypeId`, `ExprId`, `StmtId`, or `DeclId`) into the corresponding `AstArena` vector:
* **Cache Locality & Zero Fragmentation:** Traversing expressions or statements reads contiguous memory blocks rather than chasing heap pointers.
* **Trivial Multi-Module Merging:** When `loadModuleDeclarationsRecursive` parses an imported `.cb` file, it passes ownership of the existing `AstArena` into `parseTokensWithArena(tokens, std::move(arena), candidatePath)`. New nodes are simply appended to the existing vectors, and existing `TypeId`/`ExprId`/`StmtId`/`DeclId` handles remain valid even as the vectors grow.
* **Type Interning / Deduplication:** `addType()` checks whether an identical `TypeNode` (including `paramByref`, `returnType`, and `isFallible` for `fn(...)` types) already exists in `arena.types` via `findTypeNode()`, while `findOrAddType()` checks `name` and `typeArgs` via `findType()`, returning the existing `TypeId` so identical types share canonical IDs.

```plantuml
@startuml
skinparam monochrome false
title Flat AstArena Indexing Example: "dim x as int = 10 + 20"

rectangle "arena.types (std::vector<TypeNode>)" as Types {
    card "TypeId 0: TypeNode{name="int", typeArgs={}}" as T0
}

rectangle "arena.exprs (std::vector<ExprNode>)" as Exprs {
    card "ExprId 0: Identifier{name="x"}" as E0
    card "ExprId 1: NumberLiteral{raw="10", isFloat=false}" as E1
    card "ExprId 2: NumberLiteral{raw="20", isFloat=false}" as E2
    card "ExprId 3: BinaryOp{op="+", left=1, right=2}" as E3
}

rectangle "arena.stmts (std::vector<StmtNode>)" as Stmts {
    card "StmtId 0: VarAssignment{
  target = ExprId(0),
  isDim = true,
  hasDeclaredType = true,
  declaredType = TypeId(0),
  value = ExprId(3)
}" as S0
}

S0 --> E0 : "target (ExprId 0)"
S0 --> T0 : "declaredType (TypeId 0)"
S0 --> E3 : "value (ExprId 3)"
E3 --> E1 : "left (ExprId 1)"
E3 --> E2 : "right (ExprId 2)"
@enduml
```

### 2.2 Sum-Type Polymorphism via `std::variant`

Each arena wrapper node (`ExprNode`, `StmtNode`, `DeclNode`) pairs source location metadata (`line`, `col`, and module provenance on `DeclNode`) with a `std::variant` payload:

1. **`ExprVariant` (`ExprNode::payload`)**
   ```cpp
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
   ```

2. **`StmtVariant` (`StmtNode::payload`)**
   ```cpp
   using StmtVariant = std::variant<
       VarAssignment,
       ExprStmt,
       IfStmt,
       ForStmt,
       WhileStmt,
       SelectStmt,
       TryCatchStmt,
       DeferStmt,
       ReturnStmt,
       BreakStmt,
       ContinueStmt
   >;
   ```

3. **`DeclVariant` (`DeclNode::payload`)**
   ```cpp
   using DeclVariant = std::variant<
       ImportDecl,
       TypeDecl,
       EnumDecl,
       UnionDecl,
       ConstDecl,
       FnDecl
   >;
   ```

The parser, analyzer, and emitter inspect arena variants ergonomically using non-owning `std::get_if<T>` helper methods on `AstArena` (`arena.getExpr<T>(id)`, `arena.getExprMut<T>(id)`, `arena.getStmt<T>(id)`, `arena.getDecl<T>(id)`). Each `ExprNode` also caches its inferred `TypeId resolvedType = INVALID_ID` during semantic analysis so downstream passes can read expression types in $O(1)$.

---

## 3. Structured Diagnostic Reporting (`src/cleanbasic/diagnostics.hpp`)

Every compiler phase reports errors at its public API boundary through the POD `cb::DiagnosticError` structure (using internal exception short-circuiting inside `Parser` and `Analyzer` caught by `parseTokensWithArena` and `analyzeProgram`):

```cpp
struct DiagnosticError {
    std::string kind;
    std::string message;
    std::string fileName = "main.cb";
    uint32_t line = 1;
    uint32_t col = 1;
};
```

### Diagnostic Categories
* `LexerError` — Emitted by `cb::tokenize()` on unterminated string literals, forbidden C-style logical operators (`&&`, `||`, `!`), or unexpected characters.
* `ParseError` — Emitted by `cb::setParseError()` when recursive-descent expectations fail (e.g., missing `end fn`, using `:` instead of `=` in named arguments, unclosed blocks).
* `ImportError` — Emitted by `cb::loadModuleDeclarationsRecursive()` when a circular module import chain is detected.
* `AnalyzerError` — Emitted by `cb::setAnalyzerError()` or `cb::analyzeProgram()` on duplicate module aliases, undeclared variables, `const` reassignment, private cross-module symbol access, invalid `select case` types, or unhandled fallible calls.
* `FileNotFoundError` / `IOError` / `CompilationError` — Emitted by `cb::buildFile()` during file I/O or native `g++` invocation.

`cb::formatDiagnostic(err)` formats diagnostics in standard compiler `<file>:<line>:<col>: <kind>: <message>` format:
```text
network.cb:14:9: AnalyzerError: Unhandled error: function returns an error but caller does not declare 'can error' or have an active 'try / catch' block (function 'main' calls fallible function 'connect').
```

---

## 4. CLI Workflow & Flags (`src/cleanbasic/cli_main.cpp`)

The `cleanbasic` command-line executable (`cli_main.cpp`) parses arguments into `cb::CliArgs` via `cb::parseCliArgs()` and invokes `cb::buildFile()`.

### Usage Synopsis
```bash
cleanbasic build <file.cb> [-o output.cpp] [-c] [-b binary] [-t target] [--cc compiler] [--static] [-r|--release] [--opt level] [--no-pch] [--rebuild-pch] [--time]
cleanbasic <file.cb> [-o output.cpp] [-c]
cleanbasic lsp
```

### Supported CLI Options
| Flag / Argument | Description | Default Behavior |
| :--- | :--- | :--- |
| `build <file.cb>` or `<file.cb>` | Path to the root CleanBasic source file to transpile. | Required (unless `lsp` or `-h`/`--help` is passed). |
| `lsp, --lsp` | Starts the CleanBasic Language Server over stdio. | Off |
| `-o, --output <file>` | Output path for the generated Unity C++17 source file. | `<inputParentDir>/build.cpp` |
| `-c, --compile` | Compiles the generated `build.cpp` into a binary executable. | Off (unless `-b` or `-t` is passed). |
| `-b, --bin <file>` | Output binary executable path (implies `-c`; appends `.exe` on Windows targets). | `<inputParentDir>/app` (or `app.exe`). |
| `-t, --target <target>` | Target platform (`native`, `linux`, `linux-arm64`, `windows`, `windows-arm64`, `macos`, `macos-arm64`). Implies `-c` and uses `zig c++ -target <triple>` when cross-compiling. | `native` |
| `--cc <compiler>` | Explicit C++17 compiler override (e.g. `g++`, `clang++`, `zig c++`). | `zig c++` (cross) or `$CXX` / `g++` / `clang++` (native). |
| `--static` | Links a fully static binary executable on Linux/Windows (implies `-c`; falls back to static C++ runtime if host `libc.a` is unavailable). | Off (defaults to hybrid static C++ runtime `-static-libstdc++ -static-libgcc` on Linux/Windows). |
| `-r, --release` | Compiles the generated C++ with full optimizations (`-O3`). | Off (defaults to debug mode `-O0`). |
| `--debug` | Explicitly compiles in debug mode (`-O0`). | On (`-O0`). |
| `--opt, -O <0\|1\|2\|3\|s\|z>` | Sets the C++ optimization level explicitly. | `0` (`-O0`). |
| `--no-pch` | Disables precompiled headers during C++ backend compilation. | Off (PCH is enabled by default). |
| `--rebuild-pch` | Forces regeneration of the cached C++ precompiled header before compiling. | Off |
| `--time` | Prints a detailed stopwatch timing breakdown of transpilation, PCH, and C++ compilation stages. | Off |
| `-h, --help` | Prints CLI usage instructions and exits with code `0`. | Off |

### Portable SDK Root Discovery & Compilation Command (`cb::buildFile`)
CleanBasic supports zero-configuration portable SDK zip distributions (`cleanbasic` binary alongside `stdlib/` and `vendor/`). `cb::cleanbasicRootDir()` dynamically resolves the SDK root in priority order:
1. `CLEANBASIC_HOME` environment variable (if set).
2. Relative to the running `cleanbasic` executable (`<exeDir>`, `<exeDir>/..`, or `<exeDir>/../..` containing `stdlib/` and `vendor/`).
3. Compile-time `CLEANBASIC_ROOT_DIR` or current working directory (`.`) fallback.

When binary compilation (`-c`, `-b`, or `-t`) is enabled, `cb::buildFile()` collects all deduplicated `import link "<lib>"` declarations from `GlobalSymbolTable::linkLibs`, transforms each into `-l<lib>`, and runs (defaulting to `-O0` debug mode, or `-O3` with `--release`):

```bash
<compiler> -O<optLevel> -std=c++17 -pthread -I"<sdkRoot>/vendor" -I"<sdkRoot>/stdlib" -I"<inputDir>" "<outCpp>" -o "<binPath>" [-l<lib1> -l<lib2> ...]
```
* On success, the CLI prints:
  ```text
  Generated build.cpp (linker flags: -lsqlite3 -lm)
  Compiled binary app
  ```
