# CleanBasic C++17 Strict Data-Oriented Subset (Emitted Target Code)

> **Important Scope Distinction**:
> The strict procedural, POD-struct-only, no-class, no-raw-pointer, and no-exception restrictions detailed in this document apply **strictly to the generated C++17 target code (`build.cpp`)** produced by CleanBasic. Generated CleanBasic programs compile as pure procedural value-semantics C++17 with zero OOP overhead.
>
> The **CleanBasic bootstrap transpiler implementation itself (`src/cleanbasic/`)** is written in idiomatic C++17 (using stateful pass classes such as `Parser`, `Analyzer`, and `Emitter`, member methods on `AstArena` and AST nodes, non-owning `std::get_if<T>` pointer accessors, and internal exception short-circuiting for diagnostics caught at top-level API boundaries) while adhering to the project naming convention of `PascalCase` types and `camelCase` methods/functions.

---

### Emitted C++17 Guidelines (`build.cpp`)

When generating C++ code from CleanBasic, the emitter strictly enforces this procedural, memory-safe data-oriented subset:

#### 1. State and Behavior (No Classes)
* **Data:** State is emitted using POD `struct` definitions with public data members only and zero constructors/destructors.
* **Logic:** Free functions inside global or namespace scope implement all behavior. Functions take structs by value (`T`) or mutable reference (`T&` for `byref`).

#### 2. Memory and Value Semantics (No Raw Pointers)
* **No Raw Pointers:** Emitted CleanBasic code never emits raw `T*` user pointers, `new`, or `delete` (except for the built-in opaque `cptr` handle mapped to `void*` for C FFI).
* **Stack Value Semantics:** Default to value copying and stack allocation. Dynamically sized collections use `std::vector` and `std::unordered_map`.

#### 3. Error Handling & Cleanup
* **Zero Exceptions:** Emitted code does not use `throw`, `try`, or `catch`.
* **Structured Return Tuples:** Functions declared with `can error` return `CbError` (void functions) or `std::tuple<T, CbError>` (value functions).
* **RAII Defer Guards:** `defer` blocks lower to stack-allocated `cbDefer` RAII lambdas executed deterministically in LIFO order upon scope exit.

#### 4. Control Flow Lowering
* **Native Switch Jump Tables:** `select case` on integral/boolean expressions lowers directly to native C++ `switch` statements with scoped `case <val>: { ... break; }` branches.
