# Multi-File Module Resolution, Scoping & Name Mangling

This document explains how CleanBasic resolves multi-file `.cb` module graphs, normalizes relative paths, handles module aliases, deduplicates diamond imports, detects circular dependencies, enforces `public` visibility, and mangles C++ identifiers for collision-free Unity Builds.

### Related Documentation
* [CleanBasic Transpiler Architecture & Arena Design](transpilerArchitecture.md)
* [Lexical Analysis & Recursive-Descent Parsing](lexerAndParser.md)
* [Semantic Analyzer: Type Inference, UFCS, Mutability & Fallibility](semanticAnalyzer.md)
* [Unity C++17 Emitter, Runtime Preamble & C/C++ Interoperability](cppEmitterAndInterop.md)

---

## 1. Module Identity & Relative Path Resolution (`src/cleanbasic/ast.hpp`)

In CleanBasic, every `.cb` file is a module. Rather than requiring package manifests or C++ header files, modules import other `.cb` files directly using relative paths:

```vb
import "entities/hero.cb"
import "../shared/math.cb" as smath
```

### 1.1 Path Normalization (`cb::normalizeRelPath`)
`normalizeRelPath(std::string_view rawPath)` splits a slash-separated path into segments:
* Empty segments and `"."` segments are discarded.
* `".."` pops the preceding segment from the stack (unless the stack is empty or already starts with `".."`, in which case `".."` is preserved).
* Segments are rejoined with `"/"`.

### 1.2 Canonical Module IDs (`cb::resolveModuleId`)
Every loaded module is assigned a canonical `moduleId` relative to the root entry file's directory:
* The **root entry file** (e.g., `main.cb`) always has an empty module ID: `moduleId = ""`.
* When a module with `importerModuleId` imports `importTarget`:
  1. `resolveModuleId` extracts the parent directory of `importerModuleId`.
  2. Joins `<importerDir>/<importTarget>` and normalizes it with `normalizeRelPath`.
  3. Strips the trailing `".cb"` extension.

**Example (`tests/cases/nestedModules/`):**
* Root `main.cb` (`moduleId = ""`) imports `"combat/battle.cb"` -> `resolvedModuleId = "combat/battle"`.
* `combat/battle.cb` (`moduleId = "combat/battle"`) imports `"../entities/hero.cb"`:
  * Importer dir is `"combat"`.
  * Combined path is `"combat/../entities/hero.cb"`.
  * Normalized path without extension is `resolvedModuleId = "entities/hero"`.

### 1.3 Default vs. Explicit Module Aliases (`cb::defaultImportAlias`)
Each `import` statement binds a local module alias inside the importing module's scope (`GlobalSymbolTable::modules[importerModuleId][alias] = resolvedModuleId`):
* **Default Alias:** If `as <alias>` is omitted, `defaultImportAlias(importTarget)` takes the filename stem after the last `'/'` and strips `".cb"`. For example, `import "entities/hero.cb"` binds the alias `hero`.
* **Explicit Alias:** `import "entities/enemy.cb" as foe` binds the alias `foe` instead of `enemy`. This also allows importing two modules with the same filename from different directories (e.g., `ui/button.cb` and `input/button.cb`).
* **Duplicate Alias Rejection:** If a single module attempts to bind the same import alias twice, `analyzeProgram()` fails in Pass 1a with:
  `AnalyzerError: Duplicate module import alias '<alias>'.`

---

## 2. Recursive Module Graph Loading (`src/cleanbasic/pipeline.hpp`)

After the root file is lexed and parsed into an initial `AstArena` and `Program`, `cb::transpileSource()` runs `cb::loadModuleDeclarationsRecursive()`.

```plantuml
@startuml
skinparam monochrome false
title Recursive Module Loader & Diamond Deduplication

start
:Parse root file (moduleId = "");
:Initialize ModuleLoaderState
(loadingStack, loadingPathStack, loadedModules, mergedDecls);

partition "loadModuleDeclarationsRecursive" {
  while (For each DeclId in current module?) is (next DeclId)
    :Tag DeclNode with currentModuleId & currentFilePath;
    if (DeclNode is ImportDecl (kind == "module", ends with ".cb")?) then (yes)
      :Resolve candidate file path
(1. currentDir/target, 2. rootBaseDir/target, 3. target);
      :Compute canonical targetModuleId;
      if (targetModuleId in loadingStack
OR normCandidate in loadingPathStack?) then (yes)
        #pink:Report ImportError:
"Circular module import detected";
        stop
      endif
      if (targetModuleId NOT in loadedModules?) then (first time seen)
        :Read & tokenize(candidatePath);
        :parseTokensWithArena(tokens, std::move(arena));
        :Push targetModuleId to loadingStack;
        :Recursively call loadModuleDeclarationsRecursive();
        :Pop targetModuleId from loadingStack;
        :Insert targetModuleId into loadedModules;
      else (already loaded)
        :Skip re-parsing (Diamond Deduplication);
      endif
    endif
    :Append DeclId to loader.mergedDecls;
  endwhile (done)
}

:Replace program.declarations with loader.mergedDecls;
stop
@enduml
```

### 2.1 Candidate File Search Order
When resolving `import "path.cb"` from `currentFilePath`, the loader checks three candidate filesystem paths in order:
1. `<parentDirOf(currentFilePath)>/<imp.target>` (relative to the importing file)
2. `<loader.rootBaseDir>/<imp.target>` (relative to the root entry file's directory)
3. `<imp.target>` (relative to the current working directory)

### 2.2 Diamond Dependency Deduplication
`ModuleLoaderState::loadedModules` (`std::unordered_set<std::string>`) records every `targetModuleId` whose declarations have already been loaded and merged.
* In a diamond dependency graph (e.g., `main.cb` imports both `entities/hero.cb` and `combat/battle.cb`, and `combat/battle.cb` also imports `../entities/hero.cb`), `entities/hero` is lexed, parsed, and emitted into `mergedDecls` **exactly once** before its dependents.
* The second `ImportDecl` node is still preserved in `mergedDecls` so Pass 1a of the semantic analyzer registers the module alias in the second importer's scope.

### 2.3 Circular Import Detection
`ModuleLoaderState` maintains two active DFS stacks:
* `loadingStack`: Active canonical `moduleId` strings on the current recursion path.
* `loadingPathStack`: Active normalized file paths (including the root entry file).

If an `import` targets a module ID in `loadingStack` or a file path in `loadingPathStack`, the loader immediately halts with an `ImportError`:
```text
Circular module import detected when importing '<target>' (module '<targetModuleId>').
```

---

## 3. Visibility Rules (`public` vs. Private)

By default, every `type`, `alias`, and `fn` declared in a CleanBasic module is **private** to that module (`isPub = false`). Adding the `public` keyword exports the declaration (`isPub = true`).

| Access Context | Private Symbol (`isPub == false`) | Public Symbol (`isPub == true`) |
| :--- | :--- | :--- |
| **Same Module (`currentModuleId == decl.moduleId`)** | Allowed (direct unqualified access & local UFCS). | Allowed. |
| **Cross-Module Qualified Call/Type (`modAlias.Symbol`)** | **Rejected** by `analyzeProgram()` with `AnalyzerError: Function/Type/Alias '<name>' is private to module '<mod>' (missing 'public').` | Allowed if `modAlias` is imported in the current module. |
| **Cross-Module UFCS Call (`recv.method()`)** | **Rejected** (`resolveFuncCall` requires `isPub == true` for functions in the receiver's defining module or imported modules). | Allowed if the receiver's defining module or a directly imported module defines `public fn method(recv, ...)`. |
| **Transitive (Unimported) Module Access** | **Rejected.** | **Rejected** for direct qualified `unimportedAlias.fn()`, because `isModuleAliasInScope` only checks `symbols.modules[currentModuleId]`. |

---

## 4. Internal Symbol Keys & C++ Identifier Mangling (`src/cleanbasic/ast.hpp`)

Because all modules compile into a single C++17 file (`build.cpp`), two different modules might define private or public types and functions with identical names (e.g., `entities/hero.cb` and `entities/enemy.cb` both defining `type Stats` or `fn reset()`). CleanBasic prevents C++ One Definition Rule (ODR) and redefinition collisions at two levels:

### 4.1 Symbol Table Keys (`cb::symbolKey`)
Inside `GlobalSymbolTable`, declarations are indexed by `symbolKey(moduleId, name)`:
* **Root module (`moduleId == ""`)**: `"<name>"` (e.g., `"main"`, `"Player"`)
* **Imported module (`moduleId != ""`)**: `"<moduleId>::<name>"` (e.g., `"entities/hero::Hero"`, `"combat/battle::engage"`)

`splitSymbolKey(key)` splits a `"::"` key back into `{moduleId, baseName}`.

### 4.2 C++ Identifier Mangling (`makeCppTypeName` & `makeCppFnName`)
`sanitizeModuleId(moduleId)` replaces every non-alphanumeric character (such as `'/'` or `'.'`) with `'_'`. The emitter then uses `makeCppTypeName` and `makeCppFnName` to compute each symbol's emitted C++ identifier:

| Declaration Kind | Root Module (`moduleId == ""`) | Imported Module (`moduleId == "entities/hero"`) | `extern` Declaration (`isExtern == true`) |
| :--- | :--- | :--- | :--- |
| **`type Hero`** | `struct Hero` | `struct cb_entities_hero__Hero` | Unmangled (`Hero`, not re-emitted as a struct if extern) |
| **`alias Id = int`** | `using Id = int;` | `using cb_entities_hero__Id = int;` | N/A (`extern alias` is forbidden) |
| **`fn makeHero()`** | `makeHero(...)` | `cbEntitiesHeroMakeHero(...)` | Unmangled (`makeHero(...)`) |
| **`fn main()`** | `cbMain()` (called by generated `int main()`) | `cbEntitiesHeroMain()` | Unmangled |

Because `extern` types and functions bind to existing C/C++ symbols from `import header` files or linked libraries, their C++ names are **never mangled**, regardless of which `.cb` module declares them.
