# CleanBasic

CleanBasic is a compiled, statically typed language combining the readable, English-like syntax of 90s Basic with the memory-safe, bare-metal execution of modern C++17.

The transpiler, language server, and test suite are implemented in a strict, data-oriented subset of C++17 (`-std=c++17 -fno-exceptions -fno-rtti -Wall -Wextra -Werror`).

## Building with CMake

Configure and build both the `cleanbasic` transpiler/LSP CLI and the `cleanbasic_tests` test executor:

```bash
cmake -B build
cmake --build build
```

This produces two binaries in the `build/` directory:
- `build/cleanbasic`: The CleanBasic transpiler and Language Server (`cleanbasic lsp`) CLI executable.
- `build/cleanbasic_tests`: The CleanBasic data-oriented test executor.

## Running the Transpiler

Transpile a `.cb` source file to a C++17 unity build file (`build.cpp`):

```bash
./build/cleanbasic build path/to/file.cb -o build.cpp
```

Or transpile and compile directly into a native binary:

```bash
./build/cleanbasic build path/to/file.cb -o build.cpp -c
```

## Language Server (`cleanbasic lsp`) & VS Code Extension

`cleanbasic` includes a built-in Language Server Protocol (LSP) server over `stdio`:

```bash
./build/cleanbasic lsp
```

To launch the included VS Code extension (`editors/vscode/`) in an Extension Development Host window:

1. Build the compiler/LSP binary (`cmake -B build && cmake --build build`).
2. Install the extension's Language Client runtime dependency:
   ```bash
   cd editors/vscode
   npm install
   ```
3. Open the repository root in VS Code and press **`F5`** (*Launch CleanBasic VS Code Extension*).

## Running Tests

Run the test executor directly or via CTest:

```bash
./build/cleanbasic_tests
# or
ctest --test-dir build --output-on-failure
```
