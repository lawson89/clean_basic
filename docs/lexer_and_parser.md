# Lexical Analysis & Recursive-Descent Parsing

This document details CleanBasic's lexical scanner (`src/cleanbasic/lexer.hpp`) and recursive-descent parser (`src/cleanbasic/parser.hpp`), including token classification, multi-word keyword coalescing, the expression operator precedence ladder, and statement disambiguation.

### Related Documentation
* [CleanBasic Transpiler Architecture & Arena Design](transpilerArchitecture.md)
* [Multi-File Module Resolution, Scoping & Name Mangling](modulesAndSymbols.md)
* [Semantic Analyzer: Type Inference, UFCS, Mutability & Fallibility](semanticAnalyzer.md)
* [Unity C++17 Emitter, Runtime Preamble & C/C++ Interoperability](cppEmitterAndInterop.md)

---

## 1. Lexical Analysis (`src/cleanbasic/lexer.hpp`)

`cb::tokenize(std::string_view code, std::string_view fileName)` scans UTF-8/ASCII source text in a single linear pass, tracking 1-based `lineNum` and `col` coordinates and producing a `cb::LexResult`:

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
| `KEYWORD` | `enum`, `end enum`, `union`, `end union`, `until`, `fn`, `end fn`, `if`, `else if`, `end if`, `select case`, `can error`, `continue`, `break`, `and`, `or`, `not`, `true`, `false`, `null` | Single-word and coalesced two-word language keywords. |
| `TYPE_KW` | `int`, `float`, `bool`, `str`, `bytes`, `list`, `dict`, `err`, `cptr` | Built-in primitive, container, error, and opaque C-pointer type keywords (`isTypeKeyword`). |
| `IDENT` | `player`, `heal`, `maxPotion` | User-defined identifiers (`[a-zA-Z_][a-zA-Z0-9_]*`) not matching a keyword or type keyword. |
| `NUMBER` | `42`, `1_000_000`, `0xFF_00`, `0b1010_0101`, `3.141_592` | Decimal, hexadecimal (`0x...`), or binary (`0b...`) integer literals and decimal floating-point literals (supporting `_` digit separators). |
| `STRING` | `"Alice"`, `"IP cannot be empty"` | Double-quoted string literals (retaining outer quotes until stripped by `stripQuotes` in the parser; supports `\` escape sequences). Interpolated strings (`$"Hello {user.name}!"`) are desugared directly in the lexer into parenthesized `&` concatenation expressions `("Hello " & (user.name) & "!")` with `{{` and `}}` brace escaping. |
| `BYTES` | `b"GIF89a"`, `B"\x7fELF"` | Byte string literals prefixed with `b"` or `B"`. |
| `OP_CMP` | `==`, `!=`, `<`, `>`, `<=`, `>=` | Relational and equality comparison operators. |
| `OP_ARITH` | `+`, `-`, `*`, `/`, `%`, `&` | Arithmetic operators (including integer modulo `%`) and the `&` string concatenation operator. |
| `COMPOUND_ASSIGN` | `+=`, `-=`, `*=`, `/=`, `%=`, `&=` | Compound assignment operators (desugared in the parser to `VarAssignment`). |
| `ASSIGN` | `=` | Variable assignment, default/alias binding, and named argument separator. |
| `COLON` | `:` | Key-value separator inside dictionary literals (`{k: v}`). |
| `COMMA` | `,` | Parameter, argument, list, dictionary, and loop variable separator. |
| `DOT` | `.` | Struct field access, module qualification, and UFCS method call operator. |
| `LPAREN` / `RPAREN` | `(`, `)` | Parenthesized expressions, parameter lists, and function/constructor argument lists. |
| `LBRACKET` / `RBRACKET` | `[`, `]` | Generic type parameter brackets (`list[int]`), index access (`a[i]`), and list literals (`[1, 2]`). |
| `LBRACE` / `RBRACE` | `{`, `}` | Dictionary literals (`{"a": 1}`). |
| `NEWLINE` | `"\n"` | Statement terminator (emitted only when `bracketDepth == 0`). Consecutive `\n` characters (and blank/comment-only lines) collapse into a single `NEWLINE` token while advancing `lineNum` accurately. |
| `EOF` | `""` | Sentinel token appended at the end of every token stream. |

### 1.2 Comments, Whitespace, and Implicit Multiline Continuation
* **Horizontal Whitespace:** Spaces (`' '`), tabs (`'\t'`), and carriage returns (`'\r'`) are skipped silently.
* **Line Comments:** Both classic BASIC single-quote comments (`' comment`) and hash comments (`# comment`) are supported and consume characters until the next `\n`.
* **Implicit Multiline Continuation Inside `()`, `[]`, and `{}`:** `cb::tokenize()` tracks `bracketDepth`, incrementing on `LPAREN` (`(`), `LBRACKET` (`[`), and `LBRACE` (`{`) and decrementing (when `> 0`) on `RPAREN` (`)`), `RBRACKET` (`]`), and `RBRACE` (`}`). While `bracketDepth > 0`, `NEWLINE` tokens are suppressed while `lineNum` and `lineStart` still advance on every newline. This allows multiline function parameter lists, function/struct constructor call arguments, `list[T]` literals, `dict[K, V]` literals, and parenthesized expressions (including trailing commas and inline comments) to span multiple lines naturally with exact diagnostic line and column coordinates.

### 1.3 Multi-Word Keyword Coalescing (`matchSecondWordForMultiKw`)

CleanBasic uses natural multi-word BASIC block terminators and clauses. When the lexer scans an identifier `firstWord`, it peeks past any horizontal spaces/tabs on the **same line**. If another identifier `secondWord` immediately follows and matches one of the pairs below, both words are consumed and emitted as a single `KEYWORD` token with a normalized single space (`"<first> <second>"`):

| First Word | Valid Second Word(s) | Emitted `KEYWORD` Token Value |
| :--- | :--- | :--- |
| `import` | `header`, `link` | `"import header"`, `"import link"` |
| `select` | `case` | `"select case"` |
| `case` | `else` | `"case else"` |
| `else` | `if` | `"else if"` |
| `can` | `error` | `"can error"` |
| `end` | `select`, `if`, `fn`, `type`, `enum`, `union`, `for`, `while`, `defer`, `try` | `"end select"`, `"end if"`, `"end fn"`, `"end type"`, `"end enum"`, `"end union"`, `"end for"`, `"end while"`, `"end defer"`, `"end try"` |

Coalescing these in the lexer keeps the parser LL(1) on block boundaries without requiring arbitrary backtracking.

### 1.4 Enforcement of English Logical Operators

CleanBasic enforces English logical keywords (`and`, `or`, `not`) and explicitly forbids C-style symbol operators at the lexer level:
* `&&` triggers `LexerError: C-style operator '&&' is forbidden in CleanBasic; use 'and' instead.`
* `||` triggers `LexerError: C-style operator '||' is forbidden in CleanBasic; use 'or' instead.`
* `!` (when not immediately followed by `=` in `!=`) triggers `LexerError: C-style operator '!' is forbidden in CleanBasic; use 'not' instead.`

Note that single `&` is a valid `OP_ARITH` token used for automatic string concatenation.

---

## 2. Recursive-Descent Parser (`src/cleanbasic/parser.hpp`)

The parser consumes the `std::vector<Token>` stream using `cb::Parser` (which holds the token cursor `pos`, `fileName`, and `AstArena`) and populates the arena.

### 2.1 Expression Precedence Hierarchy

Expressions are parsed via recursive-descent precedence climbing starting at `parseExpr()`, which delegates to `parseOr()`. Higher levels in the table below have lower binding power (evaluated later); lower levels have higher binding power (evaluated first):

```plantuml
@startuml
skinparam monochrome false
title Recursive-Descent Expression Precedence Ladder (Lowest to Highest)

rectangle "1. parseOr
Binary 'or' (left-associative)" as L1
rectangle "2. parseAnd
Binary 'and' (left-associative)" as L2
rectangle "3. parseNot
Unary prefix 'not' (right-associative)" as L3
rectangle "4. parseComparison
Binary '==', '!=', '<', '>', '<=', '>=' (left-associative)" as L4
rectangle "5. parseAdditive
Binary '+', '-', '&' (left-associative)" as L5
rectangle "6. parseMultiplicative
Binary '*', '/' (left-associative)" as L6
rectangle "7. parseUnary
Unary prefix '+', '-' (right-associative)" as L7
rectangle "8. parsePostfix
'.field', '[index]', '(args...)' (left-associative loop)" as L8
rectangle "9. parsePrimary
Literals (Number, String, Bytes b"...", Bool, Null, List [...], Dict {...}),
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

#### Postfix Chaining & Call Arguments (`parsePostfix` & `parseCallArguments`)
`parsePostfix()` parses a primary expression and then loops while the next token is:
1. **`DOT` (`.`)**: Parses a field name or method name (`matchIdentOrType`) and wraps the current `ExprId` in a `FieldAccess` node.
2. **`LBRACKET` (`[`)**: Parses an index expression and wraps the current `ExprId` in an `IndexAccess` node.
3. **`LPAREN` (`(`)**: Calls `parseCallArguments()` to parse positional arguments (`posArgs`) and named arguments (`namedArgs` using `name=expr`) and wraps the callee in a `FuncCall` node.
   * **Named Argument Syntax Enforcement:** If the parser sees `IDENT` followed by `COLON` (`:`) inside a call argument list, it immediately reports a `ParseError`: `Named arguments must use '=' (e.g., name=value), not ':'`.
   * **Unified AST Representation:** Both function calls (`heal(p, 10)`), UFCS method calls (`p.heal(10)`), and struct constructors (`Player(name="Alice", health=10)` or `player.Player(name="Alice")`) are parsed into `FuncCall` AST nodes. They are disambiguated during semantic analysis using the `GlobalSymbolTable`.

---

## 3. Statement & Declaration Disambiguation

### 3.1 Top-Level Declarations (`parseTopLevelDecl`)
At the top level of a `.cb` file, only four declaration families are legal:
1. **Imports (`ImportDecl`)**:
   * `import "path/to/mod.cb" [as alias]` (`kind = "module"`)
   * `import header "header.hpp"` (`kind = "header"`)
   * `import link "m"` (`kind = "link"`)
2. **Type Aliases (`AliasDecl`)**:
   * `[public] alias Name = Type` (Applying `extern` to an `alias` is rejected with a `ParseError`).
3. **Struct / Extern Types (`TypeDecl`)**:
   * `[public] [extern] type Name ... end type` for struct definitions with `field as Type` declarations.
   * **Single-Line Opaque Extern Type:** When `isExtern` is true and the newline after `type Name` is immediately followed by another top-level starter (`public`, `extern`, `fn`, `type`, `alias`, `import`, or `EOF`) rather than field declarations or `end type`, `parseTypeDecl()` produces a fieldless extern `TypeDecl` without requiring `end type`.
4. **Enums (`EnumDecl`)**:
   * `[public] enum Name [as int] ... end enum` for enumeration types.
   * `[public] [extern] type Name ... end type` for struct definitions with `field as Type` declarations.
   * **Single-Line Opaque Extern Type:** When `isExtern` is true and the newline after `type Name` is immediately followed by another top-level starter (`public`, `extern`, `fn`, `type`, `alias`, `import`, or `EOF`) rather than field declarations or `end type`, `parseTypeDecl()` produces a fieldless extern `TypeDecl` without requiring `end type`.
5. **Module Constants (`ConstDecl`)**:
   * `[public] const name [as Type] = Expr` declares a module-level constant.
6. **Functions (`FnDecl`)**:
   * `[public] [extern] fn name([byref] p as Type [= defaultExpr], ...) [as ReturnType] [can error]`
   * Fallible functions declare `can error` at the end of the signature (the legacy `as err` return shorthand is rejected with a `ParseError`).
   * If `isExtern` is true and `end fn` is not present, `parseFnDecl()` returns a bodyless extern `FnDecl` immediately after the signature line.

### 3.2 Statements (`parseStatement`)

Inside a function body, `parseStatement()` dispatches on leading keywords:

| Leading Token | Parser Function | AST Node & Disambiguation Rules |
| :--- | :--- | :--- |
| `dim` or `const` | `parseAssignmentOrExprStmt` | Parses `(dim\|const) <IDENT> [as <Type>] = <Expr>` into a `VarAssignment` with `isDim = true` or `isConst = true`. |
| Any expression starter | `parseAssignmentOrExprStmt` | Parses lhs `firstExpr = parseExpr()`. If the next token is `ASSIGN` (`=`), parses rhs `valExpr` and emits a `VarAssignment` (covering variable reassignment, struct field mutation `p.health = 10`, and container index assignment `nums[0] = 5`). Otherwise, emits an `ExprStmt` (e.g., `print("hi")` or `hero.heal(10)`). |
| `if` | `parseIfStmt` | Parses `if <Expr>`, zero or more `else if <Expr>` branches (`ElseIfBranch`), optional `else` branch, and terminating `end if` into `IfStmt`. |
| `for` | `parseForStmt` | Parses one or more comma-separated loop variable identifiers (`loopVars`).<br>• **Range Loop:** If followed by `ASSIGN` (`=`), requires `loopVars.size() == 1`, and parses inclusive `<start> to <end> [step <step>]` into `rangeSpec`.<br>• **Collection Loop:** Otherwise, matches `in <iterableExpr>` (supporting `for item in listVal` or `for k, v in dictVal`). Terminated by `end for`. |
| `select case` | `parseSelectStmt` | Parses `select case <target>`, zero or more `case <matchExpr1>[, <matchExpr2>...]` blocks (`SelectCase`), optional `case else` block, and `end select` into `SelectStmt`. |
| `try` | `parseTryCatchStmt` | Parses `try`, try statement list, `catch <errIdent>`, catch statement list, and `end try` into `TryCatchStmt`. |
| `defer` | `parseDeferStmt` | Parses `defer`, statement list body, and `end defer` into `DeferStmt`. |
| `return` | `parseReturnStmt` | Parses `return [<Expr>]` into `ReturnStmt` (`has_value = true` if an expression precedes `NEWLINE`/`EOF`). |
| `break` | `parseBreakStmt` | Parses `break` into `BreakStmt`. |
| `continue` | `parseContinueStmt` | Parses `continue` into `ContinueStmt`. |
