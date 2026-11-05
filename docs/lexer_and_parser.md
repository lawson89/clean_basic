# Lexical Analysis & Recursive-Descent Parsing

This document details CleanBasic's lexical scanner (`src/cleanbasic/lexer.hpp`) and recursive-descent parser (`src/cleanbasic/parser.hpp`), including token classification, multi-word keyword coalescing, the expression operator precedence ladder, and statement disambiguation.

### Related Documentation
* [CleanBasic Transpiler Architecture & Arena Design](transpiler_architecture.md)
* [Multi-File Module Resolution, Scoping & Name Mangling](modules_and_symbols.md)
* [Semantic Analyzer: Type Inference, UFCS, Mutability & Fallibility](semantic_analyzer.md)
* [Unity C++17 Emitter, Runtime Preamble & C/C++ Interoperability](cpp_emitter_and_interop.md)

---

## 1. Lexical Analysis (`src/cleanbasic/lexer.hpp`)

`cb::tokenize(std::string_view code, std::string_view file_name)` scans UTF-8/ASCII source text in a single linear pass, tracking 1-based `line_num` and `col` coordinates and producing a `cb::LexResult`:

```cpp
struct Token {
    std::string kind;
    std::string value;
    uint32_t line = 1;
    uint32_t col = 1;
};

struct LexResult {
    bool ok = false;
    std::vector<Token> tokens;
    DiagnosticError error;
};
```

### 1.1 Token Classifications (`Token::kind`)

| Token `kind` | Examples (`Token::value`) | Description |
| :--- | :--- | :--- |
| `KEYWORD` | `fn`, `end fn`, `if`, `else if`, `end if`, `select case`, `can error`, `and`, `or`, `not`, `true`, `false`, `null` | Single-word and coalesced two-word language keywords. |
| `TYPE_KW` | `int`, `float`, `bool`, `str`, `list`, `dict`, `err`, `cptr` | Built-in primitive, container, error, and opaque C-pointer type keywords (`is_type_keyword`). |
| `IDENT` | `player`, `heal`, `max_potion` | User-defined identifiers (`[a-zA-Z_][a-zA-Z0-9_]*`) not matching a keyword or type keyword. |
| `NUMBER` | `42`, `3.14159`, `0` | Integer or decimal floating-point numeric literals. |
| `STRING` | `"Alice"`, `"IP cannot be empty"` | Double-quoted string literals (retaining outer quotes until stripped by `strip_quotes` in the parser; supports `\` escape sequences). |
| `OP_CMP` | `==`, `!=`, `<`, `>`, `<=`, `>=` | Relational and equality comparison operators. |
| `OP_ARITH` | `+`, `-`, `*`, `/`, `&` | Arithmetic operators and the `&` string concatenation operator. |
| `ASSIGN` | `=` | Variable assignment, default/alias binding, and named argument separator. |
| `COLON` | `:` | Key-value separator inside dictionary literals (`{k: v}`). |
| `COMMA` | `,` | Parameter, argument, list, dictionary, and loop variable separator. |
| `DOT` | `.` | Struct field access, module qualification, and UFCS method call operator. |
| `LPAREN` / `RPAREN` | `(`, `)` | Parenthesized expressions, parameter lists, and function/constructor argument lists. |
| `LBRACKET` / `RBRACKET` | `[`, `]` | Generic type parameter brackets (`list[int]`), index access (`a[i]`), and list literals (`[1, 2]`). |
| `LBRACE` / `RBRACE` | `{`, `}` | Dictionary literals (`{"a": 1}`). |
| `NEWLINE` | `"\n"` | Statement terminator. Consecutive `\n` characters (and blank/comment-only lines) collapse into a single `NEWLINE` token while advancing `line_num` accurately. |
| `EOF` | `""` | Sentinel token appended at the end of every token stream. |

### 1.2 Comments and Whitespace
* **Horizontal Whitespace:** Spaces (`' '`), tabs (`'\t'`), and carriage returns (`'\r'`) are skipped silently.
* **Line Comments:** Both classic BASIC single-quote comments (`' comment`) and hash comments (`# comment`) are supported and consume characters until the next `\n`.

### 1.3 Multi-Word Keyword Coalescing (`match_second_word_for_multi_kw`)

CleanBasic uses natural multi-word BASIC block terminators and clauses. When the lexer scans an identifier `first_word`, it peeks past any horizontal spaces/tabs on the **same line**. If another identifier `second_word` immediately follows and matches one of the pairs below, both words are consumed and emitted as a single `KEYWORD` token with a normalized single space (`"<first> <second>"`):

| First Word | Valid Second Word(s) | Emitted `KEYWORD` Token Value |
| :--- | :--- | :--- |
| `import` | `header`, `link` | `"import header"`, `"import link"` |
| `select` | `case` | `"select case"` |
| `case` | `else` | `"case else"` |
| `else` | `if` | `"else if"` |
| `on` | `error` | `"on error"` |
| `can` | `error` | `"can error"` |
| `end` | `select`, `if`, `fn`, `type`, `for`, `defer`, `error` | `"end select"`, `"end if"`, `"end fn"`, `"end type"`, `"end for"`, `"end defer"`, `"end error"` |

Coalescing these in the lexer keeps the parser LL(1) on block boundaries without requiring arbitrary backtracking.

### 1.4 Enforcement of English Logical Operators

CleanBasic enforces English logical keywords (`and`, `or`, `not`) and explicitly forbids C-style symbol operators at the lexer level:
* `&&` triggers `LexerError: C-style operator '&&' is forbidden in CleanBasic; use 'and' instead.`
* `||` triggers `LexerError: C-style operator '||' is forbidden in CleanBasic; use 'or' instead.`
* `!` (when not immediately followed by `=` in `!=`) triggers `LexerError: C-style operator '!' is forbidden in CleanBasic; use 'not' instead.`

Note that single `&` is a valid `OP_ARITH` token used for automatic string concatenation.

---

## 2. Recursive-Descent Parser (`src/cleanbasic/parser.hpp`)

The parser consumes the `std::vector<Token>` stream using `cb::ParserState` (which holds the token cursor `pos`, `file_name`, `AstArena`, and error flag) and populates the arena.

### 2.1 Expression Precedence Hierarchy

Expressions are parsed via recursive-descent precedence climbing starting at `parse_expr()`, which delegates to `parse_or()`. Higher levels in the table below have lower binding power (evaluated later); lower levels have higher binding power (evaluated first):

```plantuml
@startuml
skinparam monochrome false
title Recursive-Descent Expression Precedence Ladder (Lowest to Highest)

rectangle "1. parse_or
Binary 'or' (left-associative)" as L1
rectangle "2. parse_and
Binary 'and' (left-associative)" as L2
rectangle "3. parse_not
Unary prefix 'not' (right-associative)" as L3
rectangle "4. parse_comparison
Binary '==', '!=', '<', '>', '<=', '>=' (left-associative)" as L4
rectangle "5. parse_additive
Binary '+', '-', '&' (left-associative)" as L5
rectangle "6. parse_multiplicative
Binary '*', '/' (left-associative)" as L6
rectangle "7. parse_unary
Unary prefix '+', '-' (right-associative)" as L7
rectangle "8. parse_postfix
'.field', '[index]', '(args...)' (left-associative loop)" as L8
rectangle "9. parse_primary
Literals (Number, String, Bool, Null, List [...], Dict {...}),
Identifiers, and Parenthesized '( expr )'" as L9

L1 --> L2
L2 --> L3
L3 --> L4
L4 --> L5
L5 --> L6
L6 --> L7
L7 --> L8
L8 --> L9
@enduml
```

#### Postfix Chaining & Call Arguments (`parse_postfix` & `parse_call_arguments`)
`parse_postfix()` parses a primary expression and then loops while the next token is:
1. **`DOT` (`.`)**: Parses a field name or method name (`match_ident_or_type`) and wraps the current `ExprId` in a `FieldAccess` node.
2. **`LBRACKET` (`[`)**: Parses an index expression and wraps the current `ExprId` in an `IndexAccess` node.
3. **`LPAREN` (`(`)**: Calls `parse_call_arguments()` to parse positional arguments (`pos_args`) and named arguments (`named_args` using `name=expr`) and wraps the callee in a `FuncCall` node.
   * **Named Argument Syntax Enforcement:** If the parser sees `IDENT` followed by `COLON` (`:`) inside a call argument list, it immediately reports a `ParseError`: `Named arguments must use '=' (e.g., name=value), not ':'`.
   * **Unified AST Representation:** Both function calls (`heal(p, 10)`), UFCS method calls (`p.heal(10)`), and struct constructors (`Player(name="Alice", health=10)` or `player.Player(name="Alice")`) are parsed into `FuncCall` AST nodes. They are disambiguated during semantic analysis using the `GlobalSymbolTable`.

---

## 3. Statement & Declaration Disambiguation

### 3.1 Top-Level Declarations (`parse_top_level_decl`)
At the top level of a `.cb` file, only four declaration families are legal:
1. **Imports (`ImportDecl`)**:
   * `import "path/to/mod.cb" [as alias]` (`kind = "module"`)
   * `import header "header.hpp"` (`kind = "header"`)
   * `import link "m"` (`kind = "link"`)
2. **Type Aliases (`AliasDecl`)**:
   * `[pub] alias Name = Type` (Applying `extern` to an `alias` is rejected with a `ParseError`).
3. **Struct / Extern Types (`TypeDecl`)**:
   * `[pub] [extern] type Name ... end type` for struct definitions with `field as Type` declarations.
   * **Single-Line Opaque Extern Type:** When `is_extern` is true and the newline after `type Name` is immediately followed by another top-level starter (`pub`, `extern`, `fn`, `type`, `alias`, `import`, or `EOF`) rather than field declarations or `end type`, `parse_type_decl()` produces a fieldless extern `TypeDecl` without requiring `end type`.
4. **Functions (`FnDecl`)**:
   * `[pub] [extern] fn name([byref] p as Type, ...) [as ReturnType] [can error]`
   * Note: `as err` is also accepted as a shorthand for a void fallible function (`is_fallible = true`, `return_type = void`), alongside standard `[as ReturnType] can error`.
   * If `is_extern` is true and `end fn` is not present, `parse_fn_decl()` returns a bodyless extern `FnDecl` immediately after the signature line.

### 3.2 Statements (`parse_statement`)

Inside a function body, `parse_statement()` dispatches on leading keywords:

| Leading Token | Parser Function | AST Node & Disambiguation Rules |
| :--- | :--- | :--- |
| `dim` or `const` | `parse_assignment_or_expr_stmt` | Parses `(dim\|const) <IDENT> [as <Type>] = <Expr>` into a `VarAssignment` with `is_dim = true` or `is_const = true`. |
| Any expression starter | `parse_assignment_or_expr_stmt` | Parses lhs `first_expr = parse_expr()`. If the next token is `ASSIGN` (`=`), parses rhs `val_expr` and emits a `VarAssignment` (covering variable reassignment, struct field mutation `p.health = 10`, and container index assignment `nums[0] = 5`). Otherwise, emits an `ExprStmt` (e.g., `print("hi")` or `hero.heal(10)`). |
| `if` | `parse_if_stmt` | Parses `if <Expr>`, zero or more `else if <Expr>` branches (`ElseIfBranch`), optional `else` branch, and terminating `end if` into `IfStmt`. |
| `for` | `parse_for_stmt` | Parses one or more comma-separated loop variable identifiers (`loop_vars`).<br>• **Range Loop:** If followed by `ASSIGN` (`=`), requires `loop_vars.size() == 1`, parses `<start> to <end> [step <step>]`, and sets `range_spec.inclusive = true`.<br>• **Collection Loop:** Otherwise, matches `in <iterable_expr>` (supporting `for item in list_val` or `for k, v in dict_val`). Terminated by `end for`. |
| `select case` | `parse_select_stmt` | Parses `select case <target>`, zero or more `case <match_expr>` blocks (`SelectCase`), optional `case else` block, and `end select` into `SelectStmt`. |
| `on error` | `parse_on_error_stmt` | Parses `on error <err_ident>`, statement list body, and `end error` into `ErrorBlock`. |
| `defer` | `parse_defer_stmt` | Parses `defer`, statement list body, and `end defer` into `DeferStmt`. |
| `return` | `parse_return_stmt` | Parses `return [<Expr>]` into `ReturnStmt` (`has_value = true` if an expression precedes `NEWLINE`/`EOF`). |
| `break` | `parse_break_stmt` | Parses `break` into `BreakStmt`. |
