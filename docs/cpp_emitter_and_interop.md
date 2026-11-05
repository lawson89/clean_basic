# Unity C++17 Code Generation, Runtime Preamble & C/C++ Interop

This document details CleanBasic's Unity C++17 code emitter (`src/cleanbasic/emitter.hpp`), including the injected C++17 runtime preamble, the CleanBasic-to-C++17 type mapping table, tuple-based error propagation, RAII `defer` guards, native `switch` lowering for `select case`, and C/C++ interoperability patterns.

### Related Documentation
* [CleanBasic Transpiler Architecture & Arena Design](transpiler_architecture.md)
* [Lexical Analysis & Recursive-Descent Parsing](lexer_and_parser.md)
* [Multi-File Module Resolution, Scoping & Name Mangling](modules_and_symbols.md)
* [Semantic Analyzer: Type Inference, UFCS, Mutability & Fallibility](semantic_analyzer.md)

---

## 1. Structure of Emitted Unity `build.cpp` (`cb::emit_program`)

`cb::emit_program(arena, symbols, program)` generates a single self-contained C++17 file in six ordered sections:

1. **Linker Flags Comment Header** (when `import link "..."` is used):
   `// CleanBasic Linker Flags: -l<lib1> -l<lib2>`
2. **C++17 Runtime Preamble (`CPP_RUNTIME_PREAMBLE`)**:
   Standard library headers (`<cstdint>`, `<iostream>`, `<string>`, `<vector>`, `<unordered_map>`, `<memory>`, `<tuple>`, `<utility>`, `<functional>`) plus `cb_error`, `cb_to_str`, `cb_has_error`, and `_DeferTask` / `_defer`.
3. **External C/C++ Headers** (from `import header "..."`):
   Deduplicated `#include "<header>"` directives.
4. **Struct Forward Declarations & Type Aliases**:
   * `struct <cpp_name>;` for every non-`extern` `TypeDecl`.
   * `using <cpp_name> = <mapped_target_type>;` for every `AliasDecl`.
5. **POD Struct Definitions & Function Forward Declarations**:
   * Full `struct <cpp_name> { <Type> <field>{}; ... };` definitions with zero-initialized default member initializers `{}`.
   * Forward declarations `<ret_type> <cpp_fn_name>(<params...>);` for every non-`extern` function, eliminating declaration-order constraints across modules.
6. **Function Definitions & Entry Point Wrapper**:
   * Full function bodies for all non-`extern` functions across all modules.
   * If a root-module `fn main()` exists (which is emitted as `cb_main()`), the emitter appends the C++ entry point:
     ```cpp
     int main() {
         cb_main();
         return 0;
     }
     ```

---

## 2. Emitted C++17 Runtime Preamble (`CPP_RUNTIME_PREAMBLE`)

Every generated `build.cpp` begins with a lightweight, zero-allocation runtime preamble (`emitter.hpp:21-51`):

```cpp
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <tuple>
#include <utility>
#include <functional>

struct cb_error {
    std::string msg = "";
    int64_t code = 0;
};

inline std::string cb_to_str(const std::string& val) { return val; }
inline std::string cb_to_str(int val) { return std::to_string(val); }
inline std::string cb_to_str(int64_t val) { return std::to_string(val); }
inline std::string cb_to_str(double val) { return std::to_string(val); }
inline std::string cb_to_str(bool val) {
    if (val) {
        return "true";
    }
    return "false";
}
inline std::string cb_to_str(const cb_error& val) { return val.msg; }
inline bool cb_has_error(const cb_error& val) { return !val.msg.empty() || val.code != 0; }

template <typename F> struct _DeferTask { F f; ~_DeferTask() { f(); } };
template <typename F> _DeferTask<F> _defer(F f) { return {f}; }
```

* **`cb_error`**: Represents the built-in `err` type. An empty message (`msg == ""`) with `code == 0` denotes success (`cb_has_error(err) == false`).
* **`cb_to_str`**: Overloaded helper used by the `&` string concatenation operator, `print(...)` (for `bool` and `err`), and `error(...)` message formatting.
* **`_DeferTask<F>` & `_defer(F)`**: Zero-overhead RAII scope-exit wrapper that invokes the captured lambda in its destructor `~_DeferTask()` when the enclosing C++ scope exits.

---

## 3. CleanBasic-to-C++17 Type Mapping Table (`map_base_type` & `fn_return_cpp_type`)

The emitter lowers CleanBasic types and function signatures directly to value-oriented C++17 types:

| CleanBasic Type / Signature | Emitted C++17 Type | Default Initializer (`default_value_for_type`) | Notes |
| :--- | :--- | :--- | :--- |
| `int` | `int` | `0` | Standard signed integer value. |
| `float` | `double` | `0.0` | 64-bit IEEE-754 floating-point value. |
| `bool` | `bool` | `false` | Emits `"true"` / `"false"` via `cb_to_str`. |
| `str` | `std::string` | `std::string("")` | Value-semantics string; literals emit `std::string("...")`. |
| `cptr` | `void*` | `void*{}` (`nullptr`) | Opaque C pointer for C/C++ FFI interop. |
| `err` | `cb_error` | `cb_error{}` | Built-in error struct (`.msg`, `.code`). |
| `list[T]` | `std::vector<MappedT>` | `std::vector<MappedT>{}` | Contiguous dynamic array; `.push(x)` / `.append(x)` lowers to `.push_back(x)`. |
| `dict[K, V]` | `std::unordered_map<MappedK, MappedV>` | `std::unordered_map<...>{}` | Hash table; supports `for k, v in d` structured binding loops. |
| User `type Foo` (root) | `struct Foo` | `Foo{}` | Pure value semantics (stack allocated, passed by value unless `byref`). |
| User `type Foo` (module `m/a`) | `struct cb_m_a__Foo` | `cb_m_a__Foo{}` | Mangled module-scoped POD struct. |
| `extern type Bar` | `Bar` | `Bar{}` | Unmangled external C/C++ type from `import header`. |
| Parameter `p as T` | `MappedT p` | — | Pass-by-value (safe clone). |
| Parameter `byref p as T` | `MappedT& p` | — | Pass-by-mutable-reference (`is_byref = true`). |
| `fn f()` (infallible void) | `void f()` | — | Standard void function. |
| `fn f() as T` (infallible value) | `MappedT f()` | — | Single return value. |
| `fn f() can error` (fallible void) | `cb_error f()` | — | Returns `cb_error{"", 0}` on success, `cb_error{msg, code}` on error. |
| `fn f() as T can error` (fallible value) | `std::tuple<MappedT, cb_error> f()` | — | Unpacked via C++17 structured bindings `auto [_val1, _err1] = f();`. |

---

## 4. Lowering of `can error`, Implicit Bubbling, `on error` & `defer`

### 4.1 Raising Errors (`error("...", code=N)`) & Successful Returns
Inside a `fn connect(ip as str) as Connection can error`:
* Raising an error (`error("IP cannot be empty", code=408)`) lowers via `build_error_call_value()` and `build_error_bubble_return()`:
  ```cpp
  return { Connection{}, cb_error{std::string("IP cannot be empty"), 408} };
  ```
  (In a void `can error` function, it emits `return cb_error{std::string("..."), 408};`).
* Returning a normal value (`return Connection(id=1)`) lowers via `emit_return()`:
  ```cpp
  return { Connection{1}, cb_error{"", 0} };
  ```
* Falling off the end of a void `can error` function automatically appends:
  ```cpp
  return cb_error{"", 0};
  ```

### 4.2 Implicit Error Bubbling (`emit_fallible_assignment` & `emit_expr_stmt`)
When a `can error` function calls another fallible function with no active `on error` block in scope:

**CleanBasic Source:**
```vb
pub fn process_network() as int can error
    dim conn = connect("192.168.1.1")
    defer
        close_connection(conn)
    end defer
    return 100
end fn
```

**Emitted C++17:**
```cpp
std::tuple<int, cb_error> process_network() {
    auto [_val1, _err1] = connect(std::string("192.168.1.1"));
    if (cb_has_error(_err1)) return { {}, _err1 };
    auto conn = _val1;
    auto _defer_1 = _defer([&]() {
        close_connection(conn);
    });
    return { 100, cb_error{"", 0} };
}
```
If `connect(...)` returns a non-empty `cb_error`, `process_network()` immediately returns `{ {}, _err1 }` upward. If `connect(...)` succeeds, `_defer_1` is constructed on the stack, and `close_connection(conn)` is guaranteed to run when `process_network()` exits. Multiple `defer` blocks in the same scope declare `_defer_1`, `_defer_2`, etc., which C++ destroys in reverse declaration order (**LIFO**).

### 4.3 Scope-Level `on error` Handling (`emit_active_error_handler_or_bubble`)
When `on error e ... end error` is encountered in a block, `emit_stmt()` stores the `ErrorBlock` in `state.active_error_block`. Every subsequent fallible call in that scope checks `if (cb_has_error(_errN))` and inlines the handler body with `cb_error e = _errN;`:

**CleanBasic Source:**
```vb
fn start_server()
    on error e
        print("Failed (code=" & e.code & "): " & e.msg)
        return
    end error
    dim records = process_network()
    print("Processed: ", records)
end fn
```

**Emitted C++17:**
```cpp
void start_server() {
    auto [_val2, _err2] = process_network();
    if (cb_has_error(_err2)) {
        cb_error e = _err2;
        (std::cout << (((std::string("Failed (code=") + cb_to_str(e.code)) + std::string("): ")) + e.msg) << std::endl);
        return;
    }
    auto records = _val2;
    (std::cout << std::string("Processed: ") << records << std::endl);
}
```

---

## 5. Native `switch` Lowering for `select case`

Because the semantic analyzer restricts `select case <target>` to `int` or `bool` expressions, `emit_stmt()` lowers `SelectStmt` directly to a native C++ `switch` statement with scoped `case <expr>: { ... break; }` blocks and `default: { ... break; }`:

**CleanBasic Source:**
```vb
select case scores["alice"]
    case 100
        print("Perfect score!")
    case else
        print("Keep trying")
end select
```

**Emitted C++17:**
```cpp
switch (scores[std::string("alice")]) {
    case 100: {
        (std::cout << std::string("Perfect score!") << std::endl);
        break;
    }
    default: {
        (std::cout << std::string("Keep trying") << std::endl);
        break;
    }
}
```
Wrapping each `case` body in its own `{ ... }` compound block allows CleanBasic `case` branches to declare local `dim` / `const` variables without triggering C++ `crosses initialization of variable` compiler errors, while automatically inserting `break;` to prevent accidental C-style switch fallthrough.

---

## 6. C/C++ Interoperability & Standard Library Wrapping Guide

CleanBasic is designed to wrap C libraries and C++17 headers with zero FFI marshaling overhead. Because the output is a standard C++17 translation unit, external C/C++ functions and types are called directly at native speed.

### 6.1 Injecting Headers and Linker Flags (`import header` & `import link`)
* **`import header "path/to/header.hpp"`**: Emits `#include "path/to/header.hpp"` immediately after the runtime preamble in `build.cpp`. Relative paths are resolved by `g++` relative to the location of `build.cpp`.
* **`import link "sqlite3"`**: Appends `-lsqlite3` to the `// CleanBasic Linker Flags:` comment header and passes `-lsqlite3` to `g++` when compiling with `-c` / `--compile`.

### 6.2 Pattern 1: Opaque C Pointers with `cptr` (`void*`)
For C APIs that return or accept opaque pointers/handles (such as `void*`, `sqlite3*`, `FILE*`, or `SDL_Window*` wrapped in thin inline C++ helpers), use the built-in `cptr` type, which maps directly to `void*`:

**`cptr_interop.hpp`:**
```cpp
#ifndef CPTR_INTEROP_HPP
#define CPTR_INTEROP_HPP
inline void* get_handle() { return nullptr; }
inline void use_handle(void* h) {}
#endif
```

**`cptr_interop.cb`:**
```vb
import header "cptr_interop.hpp"

extern fn get_handle() as cptr
extern fn use_handle(h as cptr)

fn main()
    dim handle as cptr = get_handle()
    use_handle(handle)
end fn
```

### 6.3 Pattern 2: Wrapping External C/C++ Structs & Value Classes (`pub extern type`)
CleanBasic supports two forms of `extern type`:

1. **External Structs with Known Fields:**
   If a C/C++ header defines a POD struct `struct Vec2 { double x; double y; };`, declare its fields inside `pub extern type Vec2 ... end type`. CleanBasic will **not** re-emit `struct Vec2` in `build.cpp` (avoiding redefinition errors), but the semantic analyzer will type-check field accesses (`v.x`, `v.y`) and aggregate constructors (`Vec2(x=1.0, y=2.0)`).
   ```vb
   import header "vec2.hpp"

   pub extern type Vec2
       x as float
       y as float
   end type

   pub extern fn vec2_length(v as Vec2) as float
   ```

2. **Single-Line Opaque External Value Types:**
   If a C++ header defines a copyable/movable C++ value type or type alias (e.g., `using Regex = std::regex;`) whose internal fields should remain opaque to CleanBasic, use the single-line `pub extern type` declaration and pass instances via `byref` to avoid unnecessary copies:
   ```vb
   import header "regex_bridge.hpp"

   pub extern type Regex
   pub extern fn compile_regex(pattern as str) as Regex
   pub extern fn regex_matches(byref rx as Regex, text as str) as bool
   ```

### 6.4 Pattern 3: Slot-Table Handles for Non-Copyable C++ RAII Types
CleanBasic enforces pure value semantics and bans raw pointers in user code. When wrapping non-copyable C++ RAII resources (such as `std::fstream`, `std::mutex`, or network sockets), a recommended C++17 data-oriented bridge pattern is to store the resource in a flat vector slot table inside your C++ header and return an `int` handle (or a single-field struct handle) paired with `defer` for deterministic cleanup:

**`file_bridge.hpp`:**
```cpp
#pragma once
#include <fstream>
#include <string>
#include <vector>

inline std::vector<std::fstream>& cb_file_table() {
    static std::vector<std::fstream> table;
    return table;
}

inline int cb_file_open(const std::string& path) {
    auto& tbl = cb_file_table();
    tbl.emplace_back(path, std::ios::in);
    return static_cast<int>(tbl.size() - 1);
}

inline void cb_file_close(int handle) {
    auto& tbl = cb_file_table();
    if (handle >= 0 && static_cast<size_t>(handle) < tbl.size()) {
        tbl[static_cast<size_t>(handle)].close();
    }
}
```

**`file_demo.cb`:**
```vb
import header "file_bridge.hpp"

extern fn cb_file_open(path as str) as int
extern fn cb_file_close(handle as int)

fn main()
    dim f = cb_file_open("data.txt")
    defer
        cb_file_close(f)
    end defer
end fn
```
