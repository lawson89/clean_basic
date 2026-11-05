# CleanBasic Language Blueprint & Transpiler Architecture

CleanBasic is a statically typed, procedural systems programming language. It combines the readable, low-friction syntax of Visual Basic with the predictable, zero-garbage-collection performance of C++ or ANSI C. By enforcing flat data structures, pure value semantics, eliminating object-oriented inheritance, and using Unified Function Call Syntax (UFCS), it provides scripting-language ergonomics with bare-metal speed.

### Transpiler Internals & Architecture Suite
For deep-dive technical documentation on each compiler stage, consult:
* [Transpiler Architecture & Arena Design](transpiler_architecture.md) — 5-stage Unity Build pipeline, flat `AstArena` vector storage, `std::variant` sum types, `DiagnosticError` handling, and CLI usage.
* [Lexical Analysis & Recursive-Descent Parsing](lexer_and_parser.md) — Tokenization, two-word keyword coalescing, forbidden C-style logical operators, and the expression precedence ladder.
* [Multi-File Module Resolution, Scoping & Name Mangling](modules_and_symbols.md) — Relative path normalization, module aliases, diamond deduplication, circular import detection, `pub` visibility, and C++ identifier mangling.
* [Semantic Analyzer: Type Inference, UFCS, Mutability & Fallibility](semantic_analyzer.md) — Two-pass `GlobalSymbolTable` registration, alias cycle detection, UFCS resolution, `const`/`dim`/`byref` mutability enforcement, and static `can error` / `on error` verification.
* [Unity C++17 Emitter, Runtime Preamble & C/C++ Interop](cpp_emitter_and_interop.md) — Runtime preamble, CleanBasic-to-C++17 type mapping table, tuple error propagation, RAII `defer` guards, native `switch` lowering, and C/C++ FFI wrapping with `cptr` and `extern`.
* [C++17 Strict Data-Oriented Subset](cpp_subset.md) — Coding standards for the transpiler implementation.

---

## 1. Core Architecture
* **Unity Build Backend:** The transpiler converts CleanBasic into a single procedural C++17 `build.cpp` file, eliminating headers and linker hell while enabling maximal GCC/Clang optimization.
* **Procedural Purity & Value Semantics:** CleanBasic strictly bans C++ classes, inheritance, exceptions, and raw user pointers. All structs and containers use stack-first value semantics, free functions, explicit `byref` mutation, and Unified Function Call Syntax (UFCS).
* **English-Over-Symbols:** C-style logical operators (`&&`, `||`, `!`) are forbidden at the lexer level and replaced with `and`, `or`, and `not`.

## 2. The CleanBasic EBNF Grammar

```ebnf
Program         ::= TopLevelDecl*

TopLevelDecl    ::= ImportDecl | AliasDecl | TypeDecl | FnDecl

ImportDecl      ::= "import" (StringLiteral | QualifiedIdent) ["as" Identifier] NEWLINE
                  | "import header" StringLiteral NEWLINE
                  | "import link" StringLiteral NEWLINE

AliasDecl       ::= ["pub"] "alias" Identifier "=" Type NEWLINE

TypeDecl        ::= ("pub" | "extern")* "type" Identifier NEWLINE FieldDecl* "end type" NEWLINE
                  | ["pub"] "extern" "type" Identifier NEWLINE
FieldDecl       ::= Identifier "as" Type NEWLINE

FnDecl          ::= ("pub" | "extern")* "fn" Identifier "(" [ParamList] ")" ["as" Type] ["can error"] NEWLINE
                    Statement*
                    "end fn" NEWLINE
                  | ["pub"] "extern" "fn" Identifier "(" [ParamList] ")" ["as" Type] ["can error"] NEWLINE
ParamList       ::= Param ("," Param)*
Param           ::= ["byref"] Identifier "as" Type

Statement       ::= VarDecl | Assignment | ExprStmt
                  | IfStmt | ForStmt | SelectStmt
                  | ErrBlock | DeferBlock | ReturnStmt | BreakStmt

VarDecl         ::= ("dim" | "const") Identifier ["as" Type] "=" Expr NEWLINE
Assignment      ::= Expr "=" Expr NEWLINE
ExprStmt        ::= Expr NEWLINE

ErrBlock        ::= "on error" Identifier NEWLINE Statement* "end error" NEWLINE
DeferBlock      ::= "defer" NEWLINE Statement* "end defer" NEWLINE

IfStmt          ::= "if" Expr NEWLINE Statement*
                    ("else if" Expr NEWLINE Statement*)*
                    ["else" NEWLINE Statement*]
                    "end if" NEWLINE

ForStmt         ::= "for" Identifier "=" Expr "to" Expr ["step" Expr] NEWLINE
                    Statement*
                    "end for" NEWLINE
                  | "for" IdentList "in" Expr NEWLINE
                    Statement*
                    "end for" NEWLINE

SelectStmt      ::= "select case" Expr NEWLINE
                    ("case" Expr NEWLINE Statement*)*
                    ["case else" NEWLINE Statement*]
                    "end select" NEWLINE

ReturnStmt      ::= "return" [Expr] NEWLINE
BreakStmt       ::= "break" NEWLINE

Expr            ::= OrExpr
OrExpr          ::= AndExpr ("or" AndExpr)*
AndExpr         ::= NotExpr ("and" NotExpr)*
NotExpr         ::= "not" NotExpr | CmpExpr
CmpExpr         ::= AddExpr (("==" | "!=" | "<" | ">" | "<=" | ">=") AddExpr)*
AddExpr         ::= MulExpr (("+" | "-" | "&") MulExpr)*
MulExpr         ::= UnaryExpr (("*" | "/") UnaryExpr)*
UnaryExpr       ::= ("+" | "-") UnaryExpr | PostfixExpr

PostfixExpr     ::= PrimaryExpr
                    ( "." Identifier
                    | "[" Expr "]"
                    | "(" [CallArgList] ")" )*

PrimaryExpr     ::= Literal | QualifiedIdent | "(" Expr ")"

CallArgList     ::= CallArg ("," CallArg)* [","]
CallArg         ::= [Identifier "="] Expr

Literal         ::= NumberLiteral | StringLiteral
                  | "true" | "false" | "null"
                  | ListLiteral | DictLiteral
ListLiteral     ::= "[" [Expr ("," Expr)* [","]] "]"
DictLiteral     ::= "{" [DictEntry ("," DictEntry)* [","]] "}"
DictEntry       ::= Expr ":" Expr

Type            ::= "int" | "float" | "str" | "bool" | "cptr" | "err"
                  | "list" "[" Type "]"
                  | "dict" "[" Type "," Type "]"
                  | QualifiedIdent

IdentList       ::= Identifier ("," Identifier)*
QualifiedIdent  ::= Identifier ("." Identifier)*
```

---

## 3. Code Examples

### 1. Multi-File Modules, Nested Folder Imports & Visibility

Every `.cb` file is implicitly a module. Everything is private by default; the `pub` keyword exposes types, aliases, and functions to the importer.
* **Relative Nested Paths:** Module imports (`import "entities/hero.cb"` or `import "../shared/math.cb"`) are resolved relative to the directory of the importing file and normalized to canonical project-relative module IDs (e.g., `entities/hero`).
* **Default & Explicit Aliases (`as`):** `import "entities/hero.cb"` binds the module alias `hero` (the filename stem). When two modules share a stem (e.g., `ui/button.cb` and `input/button.cb`), use `import "input/button.cb" as input_btn` to disambiguate. Duplicate module aliases in the same file are rejected at compile time.
* **Diamond Deduplication & Circular Import Rejection:** Shared dependencies imported across multiple modules are loaded once in topological order; circular import chains are rejected with a clear diagnostic.
* **Module Scoping & C++ Name Mangling:** Imported non-`extern` declarations are emitted with collision-free mangled C++ identifiers (`cb_<sanitized_module_id>__<name>`, e.g., `cb_entities_hero__Player`), allowing different modules to define identical struct or function names without C++ ODR or redefinition conflicts.

**player.cb (The Library Module)**

```vb
' Expose the type to other files
pub type Player
    name as str
    health as int
end type

' Expose the function
pub fn heal(byref p as Player, amount as int)
    p.health = p.health + amount
end fn

' Private function (no 'pub')
fn calculate_max_health() as int
    return 100
end fn
```

### 2. Pure Value Semantics, Mutability, and UFCS

CleanBasic uses stack-first value semantics (safe clones) by default. Mutability is strictly controlled at the declaration site (`const` vs `dim`) and the parameter boundary (`byref`). Named arguments and struct fields strictly use `=` (`Type(field=val)`). For C/C++ FFI handles, the built-in `cptr` type maps directly to `void*`.

**main.cb (The Executable Module)**

```vb
import "player.cb"

fn main() as int
    ' IMMUTABILITY: Cannot be changed after declaration (supports optional or inferred type)
    const max_potion = 50 
    
    ' MUTABILITY & INFERENCE: 'dim' allows reassignment, type is inferred
    dim uses_left = 3 
    uses_left = 2

    ' VALUE SEMANTICS: Stack allocation (flat, fast, zero cleanup needed)
    dim hero = player.Player(name="Alice", health=10)

    ' UFCS (Unified Function Call Syntax): 
    ' The compiler translates hero.heal(max_potion) into heal(hero, max_potion)
    ' Because heal() asks for 'byref', the original hero is mutated in place.
    hero.heal(max_potion)
    return 0
end fn
```

### 3. Error Handling & Cleanup (Static Bubbling & Defer)

CleanBasic provides systems-level safety without `try/catch` performance penalties. Functions declare fallibility by adding `can error` to their signature (`fn connect(ip as str) as Connection can error` or void `fn validate_port(port as int) can error`), raise errors with `error("...", [code=N])`, and use tuple-free `return` statements.

1. **Implicit Bubbling:** If a fallible function calls another fallible function (with no active `on error` handler in scope), the transpiler automatically injects an invisible check that returns the error upward. The developer writes zero error boilerplate.
2. **Deterministic Cleanup (`defer`):** The `defer ... end defer` block guarantees execution when the function exits (via a successful return, an `on error` return, or an error bubble), mapping cleanly to C++ RAII scope-exit destructors in LIFO order.
3. **Scope-Level Explicit Handling (`on error`):** An `on error <ident> ... end error` statement block registers an active error handler for all subsequent fallible statements in the enclosing scope (mirroring `defer ... end defer`). It binds `<ident>` as a structured `err` value with `.msg` (`str`) and `.code` (`int`) fields, and its body *must* terminate with an explicit control-flow exit (`return`, `error(...)`, or `break`). Pairing `defer` with `on error` provides clean `try / catch / finally` semantics without C++ exception overhead.

**network.cb (Error Bubbling & Cleanup)**

```vb
pub type Connection
    id as int
end type

' Function can return a Connection OR raise a structured error
pub fn connect(ip as str) as Connection can error
    if ip == ""
        error("IP cannot be empty", code=408)
    end if
    
    return Connection(id=1)
end fn

pub fn close_connection(conn as Connection)
    ' Cleanup logic goes here
end fn

' Declares 'can error', enabling implicit error bubbling
pub fn process_network() as int can error
    ' IMPLICIT BUBBLING: If connect fails, process_network immediately returns the error.
    dim conn = connect("192.168.1.1")
    
    ' DETERMINISTIC CLEANUP: Runs right before the function exits, no matter how.
    defer
        close_connection(conn)
    end defer
    
    return 100
end fn

fn start_server()
    ' start_server() cannot fail. Register a scope-level 'on error' handler
    ' before calling any fallible functions in this scope.
    on error e
        print("Failed to start server (code=" & e.code & "): " & e.msg)
        return
    end error

    dim records = process_network()
    print("Connected and processed ", records, " records.")
end fn
```

### 4. Control Flow, Ranges, and Built-in Containers

CleanBasic provides built-in generic containers (`list[T]`, `dict[K, V]`), auto-coercing `&` string concatenation, variadic `print(...)`, classic BASIC `for i = start to end [step s]` inclusive range loops alongside `for item in collection` loops, and integral/boolean `select case` blocks lowered directly to native C++ `switch` statements.

```vb
fn demo_containers() as int
    dim nums as list[int] = [10, 20, 30]
    dim scores as dict[str, int] = {"alice": 100, "bob": 85}

    dim total = 0
    for i = 0 to 2 step 1
        total = total + nums[i]
    end for

    for x in nums
        print("Item: ", x)
    end for

    select case scores["alice"]
        case 100
            print("Perfect score! Total: " & total)
        case else
            print("Keep trying")
    end select
    return 0
end fn
```
