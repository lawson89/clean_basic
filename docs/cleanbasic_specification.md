# CleanBasic Language Blueprint & Transpiler Architecture

CleanBasic is a statically typed, procedural systems programming language. It combines the readable, low-friction syntax of Visual Basic with the predictable, zero-garbage-collection performance of C++ or ANSI C. By enforcing flat data structures, eliminating object-oriented inheritance, and using Unified Function Call Syntax (UFCS), it provides scripting-language ergonomics with bare-metal speed.

## 1. Core Architecture
* **Unity Build Backend:** The transpiler converts CleanBasic into a single procedural C++17 `build.cpp` file, eliminating headers and linker hell while enabling maximal GCC/Clang optimization.
* **Procedural Purity:** CleanBasic strictly bans C++ classes, inheritance, exceptions, and raw pointers. It relies entirely on flat data structures, free functions, and Unified Function Call Syntax (UFCS).
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

FnDecl          ::= ("pub" | "extern")* "fn" Identifier "(" [ParamList] ")" ["as" ReturnList] ["can error"] NEWLINE
                    Statement*
                    "end fn" NEWLINE
                  | ["pub"] "extern" "fn" Identifier "(" [ParamList] ")" ["as" ReturnList] ["can error"] NEWLINE
ParamList       ::= Param ("," Param)*
Param           ::= ["byref"] Identifier "as" ["byref"] Type
ReturnList      ::= Type ("," Type)*

Statement       ::= VarDecl | Assignment | ExprStmt
                  | IfStmt | ForStmt | SelectStmt
                  | ErrBlock | DeferBlock | SpawnStmt | ReturnStmt | BreakStmt

VarDecl         ::= ("dim" | "const") IdentList ["as" Type] "=" Expr NEWLINE
Assignment      ::= ExprList "=" Expr NEWLINE
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
                  | "for" Identifier "in" Expr ".." Expr ["step" Expr] NEWLINE
                    Statement*
                    "end for" NEWLINE
                  | "for" IdentList "in" Expr NEWLINE
                    Statement*
                    "end for" NEWLINE

SelectStmt      ::= "select case" Expr NEWLINE
                    ("case" Expr NEWLINE Statement*)*
                    ["case else" NEWLINE Statement*]
                    "end select" NEWLINE

SpawnStmt       ::= "spawn" FnCall NEWLINE
ReturnStmt      ::= "return" [ExprList] NEWLINE
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
                    | "(" [CallArgList] ")"
                    | "{" [InitFieldList] "}" )*

PrimaryExpr     ::= Allocation | Literal | QualifiedIdent | "(" Expr ")"
Allocation      ::= "new" Type [ "(" [InitFieldList] ")" | "{" [InitFieldList] "}" ]

CallArgList     ::= CallArg ("," CallArg)* [","]
CallArg         ::= [Identifier "="] Expr

InitFieldList   ::= InitField ("," InitField)* [","]
InitField       ::= [Identifier "="] Expr

Literal         ::= NumberLiteral | StringLiteral | FStringLiteral
                  | "true" | "false" | "null"
                  | ListLiteral | DictLiteral
ListLiteral     ::= "[" [Expr ("," Expr)* [","]] "]"
DictLiteral     ::= "{" [DictEntry ("," DictEntry)* [","]] "}"
DictEntry       ::= Expr ":" Expr

Type            ::= ["shared"] BaseType
BaseType        ::= "int" | "float" | "str" | "bool"
                  | "list" "[" Type "]"
                  | "dict" "[" Type "," Type "]"
                  | "chan" "[" Type "]"
                  | QualifiedIdent

IdentList       ::= Identifier ("," Identifier)*
ExprList        ::= Expr ("," Expr)*
QualifiedIdent  ::= Identifier ("." Identifier)*
```

---

## 3. Code Examples

### 1. Multi-File Modules & Imports

Every `.cb` file is implicitly a module. Everything is private by default; the `pub` keyword exposes types, aliases, and functions to the importer.

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

### 2. Memory, Mutability, and UFCS

CleanBasic assumes pass-by-value (safe clones) by default. Mutability is strictly controlled at the declaration site (`const` vs `dim`) and the parameter boundary (`byref`). Named arguments and struct fields strictly use `=` (`Type(field=val)`).

**main.cb (The Executable Module)**

```vb
import "player.cb"

fn main() as int
    ' IMMUTABILITY: Cannot be changed after declaration
    const max_potion as int = 50 
    
    ' MUTABILITY & INFERENCE: 'dim' allows reassignment, type is inferred
    dim uses_left = 3 
    uses_left = 2

    ' MEMORY: Stack allocation (flat, fast, zero cleanup needed)
    dim stack_hero = player.Player(name="Alice", health=10)
    
    ' MEMORY: Heap allocation (requires 'shared' and 'new')
    dim heap_hero as shared player.Player = new player.Player(name="Bob", health=10)

    ' UFCS (Unified Function Call Syntax): 
    ' The compiler translates stack_hero.heal() into heal(stack_hero, max_potion)
    ' Because heal() asks for 'byref', the original stack_hero is mutated.
    stack_hero.heal(max_potion)
    
    ' Because heap_hero is 'shared' (a pointer), passing it to a 'byref' function
    ' automatically dereferences it under the hood. No C-style '*' or '&' needed.
    heap_hero.heal(max_potion)
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

CleanBasic provides built-in generic containers (`list[T]`, `dict[K, V]`, `chan[T]`), interpolated strings (`f"..."`), auto-coercing `&` string concatenation, variadic `print(...)`, classic BASIC `for i = start to end [step s]` inclusive range loops alongside `for item in collection` loops, and `select case` pattern blocks.

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
            print("Perfect score, total=" & total)
        case else
            print("Keep trying")
    end select
    return 0
end fn
```
