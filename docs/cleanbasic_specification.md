# CleanBasic Language Blueprint & Transpiler Architecture

CleanBasic is a statically typed, procedural systems programming language. It combines the readable, low-friction syntax of Visual Basic with the predictable, zero-garbage-collection performance of C++ or ANSI C. By enforcing flat data structures, pure value semantics, eliminating object-oriented inheritance, and using Unified Function Call Syntax (UFCS), it provides scripting-language ergonomics with bare-metal speed.

### Transpiler Internals & Architecture Suite
For deep-dive technical documentation on each compiler stage, consult:
* [Transpiler Architecture & Arena Design](transpiler_architecture.md) — 5-stage Unity Build pipeline, flat `AstArena` vector storage, `std::variant` sum types, `DiagnosticError` handling, and CLI usage.
* [Lexical Analysis & Recursive-Descent Parsing](lexer_and_parser.md) — Tokenization, two-word keyword coalescing, forbidden C-style logical operators, and the expression precedence ladder.
* [Multi-File Module Resolution, Scoping & Name Mangling](modules_and_symbols.md) — Relative path normalization, module aliases, diamond deduplication, circular import detection, `public` visibility, and C++ identifier mangling.
* [Semantic Analyzer: Type Inference, UFCS, Mutability & Fallibility](semantic_analyzer.md) — Two-pass `GlobalSymbolTable` registration, alias cycle detection, UFCS resolution, `const`/`dim`/`byref` mutability enforcement, and static `can error` / `try / catch` verification.
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

TopLevelDecl    ::= ImportDecl | ConstDecl | AliasDecl | TypeDecl | EnumDecl | UnionDecl | FnDecl

ImportDecl      ::= "import" StringLiteral ["as" Identifier] NEWLINE
                  | "import header" StringLiteral NEWLINE
                  | "import link" StringLiteral NEWLINE

ConstDecl       ::= ["public"] "const" Identifier ["as" Type] "=" Expr NEWLINE

AliasDecl       ::= ["public"] "alias" Identifier "=" Type NEWLINE

EnumDecl        ::= ["public"] "enum" Identifier NEWLINE EnumMember* "end enum" NEWLINE
EnumMember      ::= Identifier ["=" ["+" | "-"] NumberLiteral] NEWLINE

UnionDecl       ::= ["public"] "union" Identifier NEWLINE UnionVariant+ "end union" NEWLINE
UnionVariant    ::= Identifier "as" Type NEWLINE

TypeDecl        ::= ("public" | "extern")* "type" Identifier NEWLINE FieldDecl* "end type" NEWLINE
                  | ["public"] "extern" "type" Identifier NEWLINE
FieldDecl       ::= ["private"] Identifier "as" Type ["=" Expr] NEWLINE

FnDecl          ::= ("public" | "extern")* "fn" Identifier "(" [ParamList] ")" ["as" Type] ["can error"] NEWLINE
                    Statement*
                    "end fn" NEWLINE
                  | ["public"] "extern" "fn" Identifier "(" [ParamList] ")" ["as" Type] ["can error"] NEWLINE
ParamList       ::= Param ("," Param)* [","]
Param           ::= ["byref"] Identifier "as" Type ["=" Expr]

Statement       ::= VarDecl | Assignment | ExprStmt
                  | IfStmt | ForStmt | WhileStmt | SelectStmt
                  | TryCatchStmt | DeferBlock | ReturnStmt | BreakStmt | ContinueStmt

VarDecl         ::= ("dim" | "const") Identifier ["as" Type] "=" Expr NEWLINE
Assignment      ::= Expr ("=" | "+=" | "-=" | "*=" | "/=" | "%=" | "&=") Expr NEWLINE
ExprStmt        ::= Expr NEWLINE

TryCatchStmt    ::= "try" NEWLINE Statement* "catch" Identifier NEWLINE Statement* "end try" NEWLINE
DeferBlock      ::= "defer" NEWLINE Statement* "end defer" NEWLINE

IfStmt          ::= "if" Expr NEWLINE Statement*
                    ("else if" Expr NEWLINE Statement*)*
                    ["else" NEWLINE Statement*]
                    "end if" NEWLINE

ForStmt         ::= "for" Identifier "=" Expr ("to" | "until") Expr ["step" Expr] NEWLINE
                    Statement*
                    "end for" NEWLINE
                  | "for" IdentList "in" Expr NEWLINE
                    Statement*
                    "end for" NEWLINE

WhileStmt       ::= "while" Expr NEWLINE
                    Statement*
                    "end while" NEWLINE

SelectStmt      ::= "select case" Expr NEWLINE
                    ("case" ExprList ["as" Identifier] NEWLINE Statement*)*
                    ["case else" NEWLINE Statement*]
                    "end select" NEWLINE
ExprList        ::= Expr ("," Expr)*

ReturnStmt      ::= "return" [Expr] NEWLINE
BreakStmt       ::= "break" NEWLINE
ContinueStmt    ::= "continue" NEWLINE

Expr            ::= OrExpr
OrExpr          ::= AndExpr ("or" AndExpr)*
AndExpr         ::= NotExpr ("and" NotExpr)*
NotExpr         ::= "not" NotExpr | CmpExpr
CmpExpr         ::= AddExpr (("==" | "!=" | "<" | ">" | "<=" | ">=") AddExpr)*
AddExpr         ::= MulExpr (("+" | "-" | "&") MulExpr)*
MulExpr         ::= UnaryExpr (("*" | "/" | "%") UnaryExpr)*
UnaryExpr       ::= ("+" | "-") UnaryExpr | PostfixExpr

PostfixExpr     ::= PrimaryExpr
                    ( "." Identifier
                    | "[" Expr "]"
                    | "(" [CallArgList] ")" )*

PrimaryExpr     ::= Literal | QualifiedIdent | "(" Expr ")"

CallArgList     ::= CallArg ("," CallArg)* [","]
CallArg         ::= [Identifier "="] Expr

Literal         ::= NumberLiteral | StringLiteral | InterpString
                  | "true" | "false" | "null"
                  | ListLiteral | DictLiteral
InterpString    ::= '$"' (StringChar | "{{" | "}}" | "{" Expr "}")* '"'
ListLiteral     ::= "[" [Expr ("," Expr)* [","]] "]"
DictLiteral     ::= "{" [DictEntry ("," DictEntry)* [","]] "}"
DictEntry       ::= Expr ":" Expr

Type            ::= "int" | "float" | "str" | "bool" | "cptr" | "err"
                  | "list" "[" Type "]"
                  | "dict" "[" Type "," Type "]"
                  | FnType
                  | QualifiedIdent
FnType          ::= "fn" "(" [FnTypeParam ("," FnTypeParam)* [","]] ")" ["as" Type] ["can error"]
FnTypeParam     ::= ["byref"] [Identifier "as"] Type

IdentList       ::= Identifier ("," Identifier)*
QualifiedIdent  ::= Identifier ("." Identifier)*
```

---

## 3. Code Examples

### 1. Multi-File Modules, Nested Folder Imports & Visibility

Every `.cb` file is implicitly a module. Everything is private by default; the `public` keyword exposes types, aliases, and functions to the importer.
* **Relative Nested Paths:** Module imports (`import "entities/hero.cb"` or `import "../shared/math.cb"`) are resolved relative to the directory of the importing file and normalized to canonical project-relative module IDs (e.g., `entities/hero`).
* **Default & Explicit Aliases (`as`):** `import "entities/hero.cb"` binds the module alias `hero` (the filename stem). When two modules share a stem (e.g., `ui/button.cb` and `input/button.cb`), use `import "input/button.cb" as inputBtn` to disambiguate. Duplicate module aliases in the same file are rejected at compile time.
* **Diamond Deduplication & Circular Import Rejection:** Shared dependencies imported across multiple modules are loaded once in topological order; circular import chains are rejected with a clear diagnostic.
* **Module Scoping & C++ Name Mangling:** Imported non-`extern` declarations are emitted with collision-free mangled C++ identifiers (`cb_<sanitizedModuleId>__<name>`, e.g., `cb_entities_hero__Player`), allowing different modules to define identical struct or function names without C++ ODR or redefinition conflicts.

**player.cb (The Library Module)**

```vb
' Expose the type to other files
public type Player
    name as str
    health as int
end type

' Expose the function
public fn heal(byref p as Player, amount as int)
    p.health = p.health + amount
end fn

' Private function (no 'public')
fn calculateMaxHealth() as int
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
    const maxPotion = 50 
    
    ' MUTABILITY & INFERENCE: 'dim' allows reassignment, type is inferred
    dim usesLeft = 3 
    usesLeft = 2

    ' VALUE SEMANTICS: Stack allocation (flat, fast, zero cleanup needed)
    dim hero = player.Player(name="Alice", health=10)

    ' UFCS (Unified Function Call Syntax): 
    ' The compiler translates hero.heal(maxPotion) into heal(hero, maxPotion)
    ' Because heal() asks for 'byref', the original hero is mutated in place.
    hero.heal(maxPotion)
    return 0
end fn
```

### 3. Error Handling & Cleanup (Static Bubbling & Defer)

CleanBasic provides systems-level safety without `try/catch` performance penalties. Functions declare fallibility by adding `can error` to their signature (`fn connect(ip as str) as Connection can error` or void `fn validatePort(port as int) can error`), raise errors with `error("...", [code=N])`, and use tuple-free `return` statements.

1. **Implicit Bubbling:** If a fallible function calls another fallible function (outside of a `try ... catch` block), the transpiler automatically injects an invisible check that returns the error upward. The developer writes zero error boilerplate.
2. **Deterministic Cleanup (`defer`):** The `defer ... end defer` block guarantees execution when the function exits (via a successful return, a `catch` return, or an error bubble), mapping cleanly to C++ RAII scope-exit destructors in LIFO order.
3. **Exception-less `try / catch` Blocks:** A `try ... catch <ident> ... end try` statement block wraps fallible calls and lowers to a zero-exception `do { ... break; } while(false);` construct. It binds `<ident>` as a structured `err` value with `.msg` (`str`), `.code` (`int`), `.file` (`str`), and `.line` (`int`) fields, and its `catch` body can either return, break/continue, or fall through.

**network.cb (Error Bubbling & Cleanup)**

```vb
public type Connection
    id as int
end type

' Function can return a Connection OR raise a structured error
public fn connect(ip as str) as Connection can error
    if ip == ""
        error("IP cannot be empty", code=408)
    end if
    
    return Connection(id=1)
end fn

public fn closeConnection(conn as Connection)
    ' Cleanup logic goes here
end fn

' Declares 'can error', enabling implicit error bubbling
public fn processNetwork() as int can error
    ' IMPLICIT BUBBLING: If connect fails, processNetwork immediately returns the error.
    dim conn = connect("192.168.1.1")
    
    ' DETERMINISTIC CLEANUP: Runs right before the function exits, no matter how.
    defer
        closeConnection(conn)
    end defer
    
    return 100
end fn

fn startServer()
    ' startServer() cannot fail. Handle fallible calls in a try / catch block.
    try
        dim records = processNetwork()
        print("Connected and processed ", records, " records.")
    catch e
        print("Failed to start server (code=" & e.code.toStr() & "): " & e.msg)
    end try
end fn
```

### 4. Control Flow, Ranges, and Built-in Containers

CleanBasic provides an auto-included `std/str` prelude (so `str` UFCS methods like `s.trim()`, `s.split(",")`, `s.toInt()`, and `str.join(...)` work out of the box with zero imports and zero dead-code emission when unused), built-in generic containers (`list[T]`, `dict[K, V]` with bounds-safe indexing and `dict.get(key, defaultVal)`), auto-coercing `&` string concatenation, variadic `print(...)`, classic BASIC `for i = start to end [step s]` loops alongside `while` loops, `break` and `continue` control statements, and `select case` blocks on `int`, `bool`, `str`, `enum`, and tagged `union` (lowered to native C++ `switch` jump tables with stacked cases for integral/boolean/enum/union types, and single-evaluation `if / else if / else` chains for strings, supporting comma-separated multi-value `case expr1, expr2` clauses and `case Variant as binding` union payload extraction).

```vb
fn demoContainers() as int
    dim nums as list[int] = [10, 20, 30]
    dim scores as dict[str, int] = {"alice": 100, "bob": 85}

    dim total = 0
    for i = 0 to nums.len() - 1
        total = total + nums[i]
    end for

    for x in nums
        print("Item: ", x)
    end for

    select case scores["alice"]
        case 100
            print("Perfect score! Total: " & total.toStr())
        case else
            print("Keep trying")
    end select
    return 0
end fn
```

### 5. Declarative CLI Parsing (`std/cli`) and `''` Doc-Comments (LSP & `cbdoc`)

CleanBasic includes a pure-CleanBasic declarative command-line parser (`import "std/cli"`) supporting boolean flags (`addFlag`), bundled short flags (`-icnv`), key-value options (`addOption` with `--opt val`, `--opt=val`, `-o val`, and default values), positional arguments (`addArg`), trailing `rest` arguments, and formatted `usage()` / `printUsage()` output.

Declarations preceded by contiguous `''` (double-single-quote) doc-comment lines automatically surface their documentation in the CleanBasic Language Server (**Hover**, **Completion documentation**, and **Signature Help**) and can be extracted into Markdown API reference docs using `tools/cbdoc.cb` (alongside the `tools/cbfmt.cb` source formatter).

```vb
import "std/cli"

'' Searches files for a matching pattern.
public fn runSearch() as int
    dim p = cli.newParser(name="cbgrep", version="1.0", description="Search for pattern in files")
    p.addFlag("ignore-case", "i", "Case-insensitive search")
    p.addOption("output", "o", "Output file path", defaultVal="out.txt")
    p.addArg("pattern", "Substring pattern to search for")

    try
        dim args = p.parse()
        if args.helpRequested
            p.printUsage()
            return 0
        end if
        return 0
    catch e
        print(e.msg)
        p.printUsage()
        return 2
    end try
end fn
```
