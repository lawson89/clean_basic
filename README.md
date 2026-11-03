# CleanBasic

CleanBasic is a compiled, statically typed language combining the readable, English-like syntax of 90s Basic with the memory-safe, bare-metal execution of modern C++17.

The transpiler and test suite are implemented in a strict, data-oriented subset of C++17 (`-std=c++17 -fno-exceptions -fno-rtti -Wall -Wextra -Werror`).

## Building with CMake

Configure and build both the `cleanbasic` transpiler CLI and the `cleanbasic_tests` test executor:

```bash
cmake -B build
cmake --build build
```

This produces two binaries in the `build/` directory:
- `build/cleanbasic`: The CleanBasic transpiler CLI executable.
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

## Running Tests

Run the test executor directly or via CTest:

```bash
./build/cleanbasic_tests
# or
ctest --test-dir build --output-on-failure
```
