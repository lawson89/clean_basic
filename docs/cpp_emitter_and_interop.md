# Unity C++17 Code Generation, Runtime Preamble & C/C++ Interop

This document details CleanBasic's Unity C++17 code emitter (`src/cleanbasic/runtime_preamble.hpp` and `src/cleanbasic/emitter.hpp`), including the injected C++17 runtime preamble, the CleanBasic-to-C++17 type mapping table, tuple-based error propagation, RAII `defer` guards, native `switch` lowering for `select case`, and C/C++ interoperability patterns.

### Related Documentation
* [CleanBasic Transpiler Architecture & Arena Design](transpilerArchitecture.md)
* [Lexical Analysis & Recursive-Descent Parsing](lexerAndParser.md)
* [Multi-File Module Resolution, Scoping & Name Mangling](modulesAndSymbols.md)
* [Semantic Analyzer: Type Inference, UFCS, Mutability & Fallibility](semanticAnalyzer.md)

---

## 1. Structure of Emitted Unity `build.cpp` (`cb::emitProgram`)

`cb::emitProgram(arena, symbols, program)` generates a single self-contained C++17 file in six ordered sections:

1. **Linker Flags Comment Header** (when `import link "..."` is used):
   `// CleanBasic Linker Flags: -l<lib1> -l<lib2>`
2. **C++17 Runtime Preamble (`CPP_RUNTIME_PREAMBLE`)**:
   Standard library headers (`<cstdint>`, `<iostream>`, `<string>`, `<vector>`, `<unordered_map>`, `<memory>`, `<tuple>`, `<utility>`, `<functional>`) plus `CbError`, `cbToStr`, `cbHasError`, `cbRuntimeArgs`, and `CbDeferTask` / `cbDefer`.
3. **External C/C++ Headers** (from `import header "..."`):
   Deduplicated `#include "<header>"` directives.
4. **Struct Forward Declarations, Type Aliases & Enums**:
   * `struct <cppName>;` for every non-`extern` `TypeDecl`.
   * `using <cppName> = <mappedTargetType>;` for every `AliasDecl`.
   * `enum class <cppName> : int64_t { ... };` definitions and stringifier helpers for every `EnumDecl`.
5. **POD Struct Definitions & Function Forward Declarations**:
   * Full `struct <cppName> { <Type> <field>{}; ... };` definitions with zero-initialized default member initializers `{}`.
   * Forward declarations `<retType> <cppFnName>(<params...>);` for every non-`extern` function, eliminating declaration-order constraints across modules.
6. **Function Definitions & Entry Point Wrapper**:
   * Full function bodies for all non-`extern` functions across all modules.
   * If a root-module `fn main()` exists (which is emitted as `cbMain()`), the emitter appends the C++ entry point:
     ```cpp
     int main() {
         cbMain();
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

struct [[nodiscard]] CbError {
    std::string msg = "";
    int64_t code = 0;
    std::string file = "";
    int64_t line = 0;
};

inline std::string cbToStr(const std::string& val) { return val; }
inline std::string cbToStr(int val) { return std::to_string(val); }
inline std::string cbToStr(int64_t val) { return std::to_string(val); }
inline std::string cbToStr(double val) { return std::to_string(val); }
inline std::string cbToStr(bool val) {
    if (val) {
        return "true";
    }
    return "false";
}
inline std::string cbToStr(const CbError& val) { return val.msg; }
inline bool cbHasError(const CbError& val) { return !val.msg.empty() || val.code != 0; }

inline std::vector<std::string>& cbRuntimeArgs() {
    static std::vector<std::string> args;
    return args;
}

template <typename F> struct CbDeferTask { F f; ~CbDeferTask() { f(); } };
template <typename F> CbDeferTask<F> cbDefer(F f) { return {f}; }
```

* **`CbError`**: Represents the built-in `err` type. An empty message (`msg == ""`) with `code == 0` denotes success (`cbHasError(err) == false`).
* **`cbToStr`**: Overloaded helper used by the `&` string concatenation operator, `print(...)` (for `bool` and `err`), and `error(...)` message formatting.
* **`CbDeferTask<F>` & `cbDefer(F)`**: Zero-overhead RAII scope-exit wrapper that invokes the captured lambda in its destructor `~_DeferTask()` when the enclosing C++ scope exits.

---

## 3. CleanBasic-to-C++17 Type Mapping Table (`mapBaseType` & `fnReturnCppType`)

The emitter lowers CleanBasic types and function signatures directly to value-oriented C++17 types:

| CleanBasic Type / Signature | Emitted C++17 Type | Default Initializer (`defaultValueForType`) | Notes |
| :--- | :--- | :--- | :--- |
| `int` | `int64_t` | `0` | Standard 64-bit signed integer value. |
| `float` | `double` | `0.0` | 64-bit IEEE-754 floating-point value. |
| `bool` | `bool` | `false` | Emits `"true"` / `"false"` via `cbToStr`. |
| `str` | `std::string` | `std::string("")` | Value-semantics string; literals emit `std::string("...")`. |
| `bytes` | `std::vector<uint8_t>` | `std::vector<uint8_t>{}` | Raw binary buffer; byte literals emit initializer lists. |
| `cptr` | `void*` | `void*{}` (`nullptr`) | Opaque C pointer for C/C++ FFI interop. |
| `err` | `CbError` | `CbError{}` | Built-in error struct (`.msg`, `.code`, `.file`, `.line`); marked `[[nodiscard]]`. |
| `fn(T1, ...) [as R] [can error]` | `std::function<Ret(Param1, ...)>` | `std::function<...>{}` | Stateless function reference type (supports `byref` parameters and `can error` return lowering). |
| `list[T]` | `std::vector<MappedT>` | `std::vector<MappedT>{}` | Contiguous dynamic array; rvalue reads lower to zero-copy bounds-safe `cbListGet(v, i)` (`const T&`); `.push(x)` lowers to `.push_back(x)`. |
| `dict[K, V]` | `std::unordered_map<MappedK, MappedV>` | `std::unordered_map<...>{}` | Hash table; rvalue reads lower to zero-copy const-safe `cbDictAt(d, k)` (`const V&`) and `.get(k, def)` lowers to `cbDictGet(d, k, def)`. |
| User `enum Foo` | `Foo` / `cb_m_a__Foo` | `static_cast<Foo>(0)` | Emitted as C++17 `enum class : int64_t`. |
| User `union Bar` | `struct Bar` / `struct cb_m_a__Bar` | `Bar{}` | POD wrapper struct around `std::variant<MappedT0, ...> _data{}` constructed via `std::in_place_index<Idx>`. |
| User `type Foo` (root) | `struct Foo` | `Foo{}` | Pure value semantics (stack allocated, passed by const ref or `byref`). |
| User `type Foo` (module `m/a`) | `struct cb_m_a__Foo` | `cb_m_a__Foo{}` | Mangled module-scoped POD struct. |
| `extern type Bar` | `Bar` | `Bar{}` | Unmangled external C/C++ type from `import header`. |
| Parameter `p as T` (scalar) | `MappedT p` | — | Cheap scalar primitives (`int64_t`, `double`, `bool`, `void*`) pass by value. |
| Parameter `p as T` (non-primitive) | `const MappedT& p` | — | Read-only zero-copy reference for `str`, `list`, `dict`, `err`, and structs. |
| Parameter `byref p as T` | `MappedT& p` | — | Pass-by-mutable-reference (`isByref = true`). |
| `fn f()` (infallible void) | `void f()` | — | Standard void function. |
| `fn f() as T` (infallible value) | `MappedT f()` | — | Single return value. |
| `fn f() can error` (fallible void) | `[[nodiscard]] CbError f()` | — | Returns `CbError{"", 0}` on success, `CbError{msg, code}` on error. |
| `fn f() as T can error` (fallible value) | `[[nodiscard]] std::tuple<MappedT, CbError> f()` | — | Unpacked via C++17 structured bindings `auto [_val1, _err1] = f();`. |

---

## 4. Lowering of `can error`, Implicit Bubbling, `on error` & `defer`

### 4.1 Raising Errors (`error("...", code=N)`) & Successful Returns
Inside a `fn connect(ip as str) as Connection can error`:
* Raising an error (`error("IP cannot be empty", code=408)`) lowers via `buildErrorCallValue()` and `buildErrorBubbleReturn()`:
  ```cpp
  return { Connection{}, CbError{std::string("IP cannot be empty"), 408} };
  ```
  (In a void `can error` function, it emits `return CbError{std::string("..."), 408};`).
* Returning a normal value (`return Connection(id=1)`) lowers via `emitReturn()`:
  ```cpp
  return { Connection{1}, CbError{"", 0} };
  ```
* Falling off the end of a void `can error` function automatically appends:
  ```cpp
  return CbError{"", 0};
  ```

### 4.2 Implicit Error Bubbling (`emitFallibleAssignment`, `emitExprStmt` & `emitReturn`)
Fallible calls at the root of a variable assignment (`dim x = f()`), standalone call (`f()`), or direct return (`return f()`) are unpacked into temporary `auto [_val, _err]` bindings and checked via `emitActiveErrorHandlerOrBubble()`.
When a `can error` function calls another fallible function with no active `on error` block in scope:

**CleanBasic Source:**
```vb
pub fn processNetwork() as int can error
    dim conn = connect("192.168.1.1")
    defer
        closeConnection(conn)
    end defer
    return 100
end fn
```

**Emitted C++17:**
```cpp
[[nodiscard]] std::tuple<int64_t, CbError> processNetwork() {
    auto [_val1, _err1] = connect(std::string("192.168.1.1"));
    if (cbHasError(_err1)) return { {}, _err1 };
    auto conn = _val1;
    auto _defer_1 = cbDefer([&]() {
        closeConnection(conn);
    });
    return { 100, CbError{"", 0} };
}
```
If `connect(...)` returns a non-empty `CbError`, `processNetwork()` immediately returns `{ {}, _err1 }` upward. If `connect(...)` succeeds, `_defer_1` is constructed on the stack, and `closeConnection(conn)` is guaranteed to run when `processNetwork()` exits. Multiple `defer` blocks in the same scope declare `_defer_1`, `_defer_2`, etc., which C++ destroys in reverse declaration order (**LIFO**).

### 4.3 Scope-Level `on error` Handling (`emitActiveErrorHandlerOrBubble`)
When `on error e ... end error` is encountered in a block, `emitStmt()` stores the `ErrorBlock` in `state.activeErrorBlock`. Every subsequent fallible call in that scope checks `if (cbHasError(_errN))` and inlines the handler body with `CbError e = _errN;`:

**CleanBasic Source:**
```vb
fn startServer()
    on error e
        print("Failed (code=" & e.code & "): " & e.msg)
        return
    end error
    dim records = processNetwork()
    print("Processed: ", records)
end fn
```

**Emitted C++17:**
```cpp
void startServer() {
    auto [_val2, _err2] = processNetwork();
    if (cbHasError(_err2)) {
        CbError e = _err2;
        (std::cout << (((std::string("Failed (code=") + cbToStr(e.code)) + std::string("): ")) + e.msg) << std::endl);
        return;
    }
    auto records = _val2;
    (std::cout << std::string("Processed: ") << records << std::endl);
}
```

---

## 5. Lowering for `select case` (`switch` Jump Tables & String Equality Chains)

`select case <target>` supports `int`, `bool`, `str`, `enum`, and tagged `union` expressions:
* **Integral, Boolean & Enum Targets (`int`, `bool`, `enum`):** Emitted as a native C++ `switch` statement. Multi-value clauses (`case 1, 2:`) emit stacked case labels (`case 1:
case 2: {`) sharing a scoped compound block with an implicit `break;`.
* **String Targets (`str`):** Emitted inside a scoped compound block by evaluating `target` once into a temporary variable (`const auto _sel_1 = <target>;`) followed by an `if (_sel_1 == <m1> || _sel_1 == <m2>) { ... } else if (...) { ... } else { ... }` equality chain.
* **Tagged Union Targets (`union`):** Evaluates `target` once into `const auto _sel_1 = <target>;` and switches on `static_cast<int64_t>(_sel_1._data.index())`, binding `auto <binding> = std::get<Idx>(_sel_1._data);` when `case <variant> as <binding>` is used.

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

**`cptrInterop.hpp`:**
```cpp
#ifndef CPTR_INTEROP_HPP
#define CPTR_INTEROP_HPP
inline void* getHandle() { return nullptr; }
inline void useHandle(void* h) {}
#endif
```

**`cptrInterop.cb`:**
```vb
import header "cptrInterop.hpp"

extern fn getHandle() as cptr
extern fn useHandle(h as cptr)

fn main()
    dim handle as cptr = getHandle()
    useHandle(handle)
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

   pub extern fn vec2Length(v as Vec2) as float
   ```

2. **Single-Line Opaque External Value Types:**
   If a C++ header defines a copyable/movable C++ value type or type alias (e.g., `using Regex = std::regex;`) whose internal fields should remain opaque to CleanBasic, use the single-line `pub extern type` declaration and pass instances via `byref` to avoid unnecessary copies:
   ```vb
   import header "regexBridge.hpp"

   pub extern type Regex
   pub extern fn compileRegex(pattern as str) as Regex
   pub extern fn regexMatches(byref rx as Regex, text as str) as bool
   ```

### 6.4 Pattern 3: Slot-Table Handles for Non-Copyable C++ RAII Types
CleanBasic enforces pure value semantics and bans raw pointers in user code. When wrapping non-copyable C++ RAII resources (such as `std::fstream`, `std::mutex`, or network sockets), a recommended C++17 data-oriented bridge pattern is to store the resource in a flat vector slot table inside your C++ header and return an `int` handle (or a single-field struct handle) paired with `defer` for deterministic cleanup:

**`fileBridge.hpp`:**
```cpp
#pragma once
#include <fstream>
#include <string>
#include <vector>

inline std::vector<std::fstream>& CbFileTable() {
    static std::vector<std::fstream> table;
    return table;
}

inline int CbFileOpen(const std::string& path) {
    auto& tbl = CbFileTable();
    tbl.emplace_back(path, std::ios::in);
    return static_cast<int>(tbl.size() - 1);
}

inline void CbFileClose(int handle) {
    auto& tbl = CbFileTable();
    if (handle >= 0 && static_cast<size_t>(handle) < tbl.size()) {
        tbl[static_cast<size_t>(handle)].close();
    }
}
```

**`fileDemo.cb`:**
```vb
import header "fileBridge.hpp"

extern fn CbFileOpen(path as str) as int
extern fn CbFileClose(handle as int)

fn main()
    dim f = CbFileOpen("data.txt")
    defer
        CbFileClose(f)
    end defer
end fn
```
