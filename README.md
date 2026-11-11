# CleanBasic

CleanBasic is a compiled, statically typed language combining the readable, English-like syntax of 90s Basic with the memory-safe, bare-metal execution of modern C++17.

The transpiler, language server, and test suite are implemented in a strict, data-oriented subset of C++17 (`-std=c++17 -fno-exceptions -fno-rtti -Wall -Wextra -Werror`).

## Building with CMake

Configure and build both the `cleanbasic` transpiler/LSP CLI and the `cleanbasicTests` test executor:

```bash
cmake -B build
cmake --build build
```

This produces two binaries in the `build/` directory:
- `build/cleanbasic`: The CleanBasic transpiler and Language Server (`cleanbasic lsp`) CLI executable.
- `build/cleanbasicTests`: The CleanBasic data-oriented test executor.

## Running the Transpiler

Transpile a `.cb` source file to a C++17 unity build file (`build.cpp`):

```bash
./build/cleanbasic build path/to/file.cb -o build.cpp
```

Or transpile and compile directly into a native binary (defaults to fast `-O0` debug mode with automatic C++17 Precompiled Header caching and portable static C++ runtime linking on Linux/Windows; pass `-r` / `--release` for `-O3` release builds, `--static` for a fully static Linux/Windows executable, or `--time` for a detailed stage timing breakdown):

```bash
./build/cleanbasic build path/to/file.cb -o build.cpp -c
./build/cleanbasic build path/to/file.cb -c --release --time
./build/cleanbasic build path/to/file.cb --release --static
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

## Developer Tools (`tools/`)

CleanBasic includes first-party command-line tools written in pure CleanBasic under `tools/`:
- `tools/cbfmt.cb`: CleanBasic source code formatter supporting `stdin`/`stdout` piping, in-place formatting (`-w` / `--write`), formatting verification (`-c` / `--check`), and configurable indentation (`-i <n>`).
- `tools/cbdoc.cb`: Markdown API reference generator that extracts `''` doc-comments and `public` declarations from `.cb` files and directories.

Compile them into `build/` with:

```bash
./build/cleanbasic build tools/cbfmt.cb -b build/cbfmt --release
./build/cleanbasic build tools/cbdoc.cb -b build/cbdoc --release
```

## Running Tests

Run the test executor directly or via CTest:

```bash
./build/cleanbasicTests
# or
ctest --test-dir build --output-on-failure
```
