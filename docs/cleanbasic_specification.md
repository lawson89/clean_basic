# CleanBasic Language Blueprint & Transpiler Architecture

CleanBasic is a statically typed, procedural systems programming language. It combines the readable, low-friction syntax of Visual Basic with the predictable, zero-garbage-collection performance of C++ or ANSI C. By enforcing flat data structures, eliminating object-oriented inheritance, and using Unified Function Call Syntax (UFCS), it provides scripting-language ergonomics with bare-metal speed.

## 1. Core Architecture
* **Unity Build Backend:** The transpiler converts CleanBasic into a single procedural C++17 `build.cpp` file, eliminating headers and linker hell while enabling maximal GCC/Clang optimization.
* **Procedural Purity:** CleanBasic strictly bans C++ classes, inheritance, exceptions, and raw pointers. It relies entirely on flat data structures, free functions, and Unified Function Call Syntax (UFCS).
* **English-Over-Symbols:** C-style operators (`&&`, `||`, `!`) are replaced with `and`, `or`, and `not`.

## 2. The CleanBasic EBNF Grammar

```ebnf
Program         ::= Statement*

Statement       ::= ImportStmt | TypeDecl | FnDecl | VarDecl | Assignment 
                  | ExprStmt | IfStmt | ForStmt | SelectStmt | DeferBlock

ImportStmt      ::= "import" StringLiteral ["as" Identifier] NEWLINE
TypeDecl        ::= ["pub"] "type" Identifier NEWLINE (FieldDecl)+ "end type" NEWLINE
FieldDecl       ::= Identifier "as" Type NEWLINE

FnDecl          ::= ["pub" | "extern"] "fn" Identifier "(" [ParamList] ")" ["as" ReturnList] NEWLINE 
                    Statement* 
                    "end fn" NEWLINE
ParamList       ::= Param ("," Param)*
Param           ::= ["byref"] Identifier "as" Type
ReturnList      ::= Type ("," Type)* ["," "err"]

VarDecl         ::= ("dim" | "const") Identifier ["as" Type] "=" Expr [ErrBlock] NEWLINE
Assignment      ::= Expr "=" Expr [ErrBlock] NEWLINE
ErrBlock        ::= "on error" Identifier NEWLINE Statement* "end error"
DeferBlock      ::= "defer" NEWLINE Statement* "end defer"

ExprStmt        ::= Expr NEWLINE
Expr            ::= Expr ("+" | "-" | "*" | "/" | "&" | "==" | "!=") Expr 
                  | "not" Expr | Expr ("and" | "or") Expr
                  | MethodCall | FnCall | Literal | Identifier | Allocation
                  
MethodCall      ::= Expr "." Identifier "(" [ArgList] ")"
FnCall          ::= Identifier "(" [ArgList] ")"
ArgList         ::= Expr ("," Expr)*
Allocation      ::= "new" Type "(" [ArgList] ")"

Type            ::= "int" | "float" | "str" | "bool" 
                  | "shared" Type 
                  | "list[" Type "]" | "dict[" Type "," Type "]" 
                  | Identifier
```

---

## 3. Code Examples

### 1. Multi-File Modules & Imports

Every `.cb` file is implicitly a module. Everything is private by default; the `pub` keyword exposes types and functions to the importer.

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

CleanBasic assumes pass-by-value (safe clones) by default. Mutability is strictly controlled at the declaration site (`const` vs `dim`) and the parameter boundary (`byref`).

**main.cb (The Executable Module)**

```vb
import "player.cb"

fn main()
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
end fn
```

### 3. Error Handling & Cleanup (Static Bubbling & Defer)

CleanBasic provides systems-level safety without `try/catch` performance penalties. Functions declare fallibility by adding `err` to their return signature. The transpiler handles the rest.

1. **Implicit Bubbling:** If a fallible function calls another fallible function, the transpiler automatically injects an invisible check that returns the error upward. The developer writes zero error boilerplate.
2. **Deterministic Cleanup:** The `defer` block guarantees execution when the function exits (via a successful return, an error bubble, or a panic), mapping cleanly to C++ RAII destructors.
3. **Explicit Handling:** If an infallible function calls a fallible one, it *must* handle the error using the `on error` block, binding the error string to a local variable.

**network.cb (Error Bubbling & Cleanup)**

```vb
pub type Connection
    id as int
end type

' Function can return a Connection OR an error
pub fn connect(ip as str) as Connection, err
    if ip == ""
        return null, "IP cannot be empty"
    end if
    
    return Connection(id=1), ""
end fn

pub fn close_connection(conn as Connection)
    ' Cleanup logic goes here
end fn

' Declares 'err', enabling implicit error bubbling
pub fn process_network() as int, err
    ' IMPLICIT BUBBLING: If connect fails, process_network immediately returns the error.
    dim conn = connect("192.168.1.1")
    
    ' DETERMINISTIC CLEANUP: Runs right before the function exits, no matter how.
    defer
        close_connection(conn)
    end defer
    
    return 100, ""
end fn

fn start_server()
    ' start_server() cannot fail. The developer MUST handle the error here.
    ' The 'on error' block keeps the logic perfectly flat against the margin.
    dim records = process_network() on error e
        print("Failed to start server: " & e)
        return
    end error
    
    print("Connected and processed " & str(records) & " records.")
end fn
```
