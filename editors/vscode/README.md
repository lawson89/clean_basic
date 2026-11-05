# CleanBasic for VS Code

This extension provides full IDE support for the **CleanBasic** (`.cb`) systems programming language, powered by the native `cleanbasic lsp` Language Server.

## Features

- **TextMate Syntax Highlighting**: Multi-word keywords (`select case`, `on error`, `can error`, `end fn`, etc.), built-in primitive and generic container types (`int`, `float`, `str`, `bool`, `cptr`, `err`, `list[T]`, `dict[K, V]`), logical operators (`and`, `or`, `not`), and comments (`'` and `#`).
- **Block Folding & Auto-Indentation**: Folding and smart indentation for `fn ... end fn`, `type ... end type`, `if ... end if`, `for ... end for`, `select case ... end select`, `on error ... end error`, and `defer ... end defer`.
- **Ergonomic Code Snippets**: Quick tab-completion templates for `fn`, `pub fn`, `fnerr`, `type`, `pub type`, `if`, `for`, `forin`, `select`, `onerror`, and `defer`.
- **Live Diagnostics (`textDocument/publishDiagnostics`)**: Real-time lexer, parser, import, and semantic analyzer error underlines on open, edit, and save.
- **Document Outline (`textDocument/documentSymbol`)**: Hierarchical outline of structs (with nested fields), functions (with full signatures), and type aliases.
- **Hover Type Signatures (`textDocument/hover`)**: Rich Markdown hover tooltips for local variables, parameters (`byref`), struct fields, UFCS methods, functions (`can error`), aliases, and built-ins.
- **Go to Definition (`textDocument/definition`)**: Jump to local variables, parameters, functions, UFCS methods, structs, fields, aliases, and imported `.cb` module files across nested directories.
- **Context-Aware Auto-Completion & Signature Help**:
  - UFCS and struct field dot-completion (`hero.`, `e.msg`, `e.code`, `nums.push`, `nums.append`)
  - Cross-module alias completion (`player.`) respecting `pub` visibility
  - Named argument / constructor field completion (`Player(name=, health=)`)
  - Active parameter signature help (`textDocument/signatureHelp`) on `(` and `,`

## Quick Start (Development Host)

1. Build the `cleanbasic` binary from the repository root:
   ```bash
   cmake -B build && cmake --build build
   ```
2. Install the VS Code Language Client dependency:
   ```bash
   cd editors/vscode
   npm install
   ```
3. Open the repository root in VS Code and press **`F5`** (*Launch CleanBasic VS Code Extension*).
   - The extension automatically detects `<workspaceFolder>/build/cleanbasic` (or you can set `"cleanbasic.executablePath"` in VS Code Settings).
