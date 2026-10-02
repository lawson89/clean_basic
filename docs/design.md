# **Language Blueprint: The Pragmatic C++17 Transpiler**

## **1\. Language Philosophy & Architecture**

This language is designed to offer the developer ergonomics of Python and Go, the predictability of C, and the performance of C++, without the cognitive overhead of C++'s legacy features.

> * **Compilation Strategy:** Python-based Two-Pass Transpiler.  
  * *Pass 1 (Signature Scraper):* Scans top-level definitions to build a Global Symbol Table, enabling order-independent function declarations without header files.  
  * *Pass 2 (Generation):* Performs type-checking, UFCS resolution, and emits C++ code.  
> * **Target Backend:** Transpiles to a single "Unity Build" .cpp file targeting **C++17**. This eliminates C++ headers, circular dependencies, and linker hell while allowing aggressive whole-program optimization by GCC/Clang.  
> * **Modules & Visibility:** Directory-based (like Go). Everything is private by default. The pub keyword exports a symbol. C++ name mangling (e.g., module\_function) prevents collisions.

## **2\. The "Flat C++17" Target Subset**

To ensure the generated C++ is heavily optimized by compilers but remains highly readable for human audits and LLM analysis, the transpiler emits a strict, linear subset of C++.

### **Allowed C++ Features:**

> * **Plain Structs:** Only struct with public fields. No C++ classes, constructors, or methods.  
> * **Free Functions:** All logic is transpiled into flat, globally scoped C++ functions.  
> * **STL Containers:** std::vector (list\[T\]), std::unordered\_map (dict\[K,V\]).  
> * **Smart Pointers:** std::shared\_ptr (ref) and std::weak\_ptr (weak).  
> * **Structured Bindings:** Used to cleanly unpack multiple return values (auto \[val, err\]).  
> * **Standard Types:** std::string, std::tuple, std::variant (for union types).

### **Banned C++ Features (The Transpiler Will Never Emit These):**

> * class, virtual, override, or inheritance (no v-tables).  
> * Exceptions (throw, try, catch). The output is compiled with \-fno-exceptions.  
> * Custom template \<typename T\> declarations (STL monomorphic usage only).  
> * Nested namespace blocks (flat snake\_case naming is used instead).  
> * Raw pointers (\*), pointer arithmetic, and manual new/delete.  
> * \#define macros (outside of standard \#includes).

## **3\. Memory Model & Ownership**

Memory management is completely deterministic, avoiding unpredictable garbage collection pauses while remaining entirely safe.

> 1. **Stack-by-Default:** Variables, including structs, are allocated on the stack. Passing them to functions implies a copy. *(Optimization: The transpiler silently emits const T& in C++ for read-only struct parameters to avoid expensive copies).*  
> 2. **Explicit Mutability (mut):** To modify a stack variable in a function, it must be passed as mut. This transpiles to a C++ lvalue reference (T&).  
> 3. **Opt-in ARC (ref):** For heap allocation and shared ownership, the ref keyword wraps the type in a std::shared\_ptr\<T\>.  
> 4. **Cycle Breaking (weak):** To prevent cyclic memory leaks, weak wraps the type in a std::weak\_ptr\<T\>.

*Note on Leak Detection:* Because the transpiler dictates the generated code, memory leak testing is handled at build time. The transpiler can seamlessly swap std::make\_shared with an instrumented debug\_make\_ref in testing modes without changing a single line of the user's source code.

## **4\. Control Flow & Error Handling**

> * **Keyword Terminators:** The language rejects curly braces and Pythonic significant whitespace in favor of highly explicit, two-word terminators (end fn, end if, end struct, end match, end error). This guarantees precise compiler error messages for unbalanced blocks.  
> * **The Unified for Loop:** A single loop construct replaces while, do-while, and traditional for loops. It handles collection iteration, structured dictionary bindings, numeric ranges (0..10), conditionals, and infinite loops.  
> * **Explicit Error Handling (on error):**  
  * Functions that can fail append an err type to their return signature.  
  * The caller is *forced* by the compiler to use an on error block to capture and handle the failure.  
  * This eliminates uninitialized variable bugs (like Go's if err \!= nil traps) and avoids the invisible control-flow of C++ exceptions.

## **5\. EBNF Grammar Specification**

EBNF  
Program         ::= TopLevelDecl\* EOF  
TopLevelDecl    ::= ImportDecl | TypeAliasDecl | StructDecl | FnDecl  
ImportDecl      ::= "import" Identifier NEWLINE

TypeAliasDecl   ::= \["pub"\] "type" Identifier "=" Type ("|" Type)\* NEWLINE

StructDecl      ::= \["pub"\] "struct" Identifier NEWLINE   
                    FieldDecl\*   
                    "end struct" NEWLINE  
FieldDecl       ::= Identifier ":" \["mut" | "ref" | "weak"\] Type NEWLINE

FnDecl          ::= \["pub"\] "fn" Identifier "(" \[ParamList\] ")" \["-\>" ReturnList\] NEWLINE   
                    Statement\*   
                    "end fn" NEWLINE  
ParamList       ::= Param ("," Param)\*  
Param           ::= Identifier ":" \["mut"\] Type  
ReturnList      ::= Type ("," Type)\* \["," "err"\]

Statement       ::= Assignment | ExprStmt | IfStmt | ForStmt | MatchStmt   
                  | SpawnStmt | ReturnStmt | BreakStmt

Assignment      ::= IdentList "=" Expr \[ErrorBlock\] NEWLINE  
IdentList       ::= Identifier ("," Identifier)\*  
ErrorBlock      ::= "on error" \[Identifier\] NEWLINE Statement\* "end error"

IfStmt          ::= "if" Expr NEWLINE   
                    Statement\*   
                    \["else" NEWLINE Statement\*\]   
                    "end if" NEWLINE

ForStmt         ::= "for" \[ForCondition\] NEWLINE   
                    Statement\*   
                    "end for" NEWLINE  
ForCondition    ::= (IdentList "in" Expr) | (Identifier "in" Range) | Expr  
Range           ::= Expr ".." Expr

MatchStmt       ::= "match" Expr NEWLINE   
                    MatchCase\*   
                    "end match" NEWLINE  
MatchCase       ::= "case" Identifier ":" Type NEWLINE Statement\*

SpawnStmt       ::= "spawn" FuncCall NEWLINE  
FuncCall        ::= Identifier "(" \[ExprList\] ")" | Expr "." Identifier "(" \[ExprList\] ")"

Type            ::= Identifier | "list\[" Type "\]" | "dict\[" Type "," Type "\]" | "chan\[" Type "\]" | "err"

## **6\. Example Code Mapping**

### **Source: MyLang**

Ruby  
import network

\# 1\. Type Aliases & Structs  
pub type EntityID \= int | str

pub struct Player  
    id: EntityID  
    name: str  
    score: int  
    parent: weak Player  
end struct

\# 2\. Infallible Function (No 'err') \+ Mutability  
pub fn grant\_points(p: mut Player, points: int)  
    p.score \= p.score \+ points  
end fn

\# 3\. Fallible Function (Returns 'err') \+ ARC Allocation  
fn load\_player(id: int) \-\> ref Player, err  
    if id \< 0  
        return ref Player(), "Invalid ID"  
    end if  
      
    p \= ref Player(id=id, name="Alice", score=0)  
    return p, ""  
end fn

\# 4\. Main Execution & Concurrency  
fn main()  
    \# 'on error' captures fallible returns cleanly  
    player \= load\_player(42) on error e  
        print("Failed:", e)  
        return  
    end error

    \# Unified Function Call Syntax (UFCS)  
    player.grant\_points(100)

    \# Union Type Matching  
    match player.id  
        case i: int  
            print("Numeric ID:", i)  
        case s: str  
            print("String ID:", s)  
    end match

    \# Unified For Loop  
    for i in 1..5  
        player.grant\_points(i \* 10\)  
    end for  
      
    \# Concurrency via OS threads and Channels  
    jobs: chan\[int\] \= Channel()  
    spawn telemetry\_worker(jobs)  
end fn

### **Generated Output: Flat C++17**

C++  
\#include \<iostream\>  
\#include \<string\>  
\#include \<vector\>  
\#include \<memory\>  
\#include \<variant\>  
\#include \<tuple\>  
\#include \<thread\>  
// ... standard channel implementation included here ...

// 1\. Type Aliases & Structs  
using network\_EntityID \= std::variant\<int, std::string\>;

struct network\_Player {  
    network\_EntityID id;  
    std::string name;  
    int score;  
    std::weak\_ptr\<network\_Player\> parent; // Cycle breaking  
};

// 2\. Infallible Function  
// 'mut' transpiles directly to C++ reference '&'  
void network\_grant\_points(network\_Player& p, int points) {  
    p.score \= p.score \+ points;  
}

// 3\. Fallible Function  
// Uses std::tuple for multiple returns  
std::tuple\<std::shared\_ptr\<network\_Player\>, std::string\> main\_load\_player(int id) {  
    if (id \< 0\) {  
        return std::make\_tuple(std::make\_shared\<network\_Player\>(), "Invalid ID");  
    }  
      
    auto p \= std::make\_shared\<network\_Player\>();  
    p-\>id \= id;  
    p-\>name \= "Alice";  
    p-\>score \= 0;  
      
    return std::make\_tuple(p, "");  
}

// 4\. Main Execution  
int main() {  
    // Structured bindings \+ if check replaces the 'on error' block  
    auto \[player, e\] \= main\_load\_player(42);  
    if (e \!= "") {  
        std::cout \<\< "Failed: " \<\< e \<\< "\\n";  
        return 0;  
    }

    // UFCS resolves to flat function call  
    network\_grant\_points(\*player, 100);

    // Match statement transpiles to std::holds\_alternative/std::get  
    if (std::holds\_alternative\<int\>(player-\>id)) {  
        std::cout \<\< "Numeric ID: " \<\< std::get\<int\>(player-\>id) \<\< "\\n";  
    } else if (std::holds\_alternative\<std::string\>(player-\>id)) {  
        std::cout \<\< "String ID: " \<\< std::get\<std::string\>(player-\>id) \<\< "\\n";  
    }

    // Unified for loop (range)  
    for (int i \= 1; i \< 5; \++i) {  
        network\_grant\_points(\*player, i \* 10);  
    }  
      
    // Concurrency  
    Channel\<int\> jobs \= Channel\<int\>();  
    std::thread(main\_telemetry\_worker, jobs).detach();

    return 0;  
}  
