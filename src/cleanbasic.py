import re
import sys
from pathlib import Path
from dataclasses import dataclass
from typing import List, Dict, Tuple, Optional, Any

# --- 1. LEXER ---
TOKEN_REGEX = [
    ('KEYWORD', r'\b(import|header|extern|pub|type|struct|end struct|fn|end fn|mut|ref|weak|on error|end error|if|else|end if|for|in|end for|match|case|end match|spawn|return|break)\b'),
    ('TYPE', r'\b(int|str|list|dict|chan|err)\b'),
    ('F_STRING', r'f"[^"]*"|f\'[^\']*\''), 
    ('STRING', r'"[^"]*"|\'[^\']*\''),
    ('IDENT', r'[a-zA-Z_][a-zA-Z0-9_]*'),
    ('NUMBER', r'\d+'),
    ('SYMBOL', r'(\.\.|\-\>|==|!=|<=|>=|:=|\+|-|\*|/|=|<|>|:|\(|\)|\[|\]|\.|,)'),
    ('NEWLINE', r'\n+'),
    ('SKIP', r'[ \t]+'),
    ('COMMENT', r'#.*'),
]

class Lexer:
    def __init__(self, code: str):
        self.code = code
        self.tokens = []
        self.tokenize()

    def tokenize(self):
        tok_regex = '|'.join('(?P<%s>%s)' % pair for pair in TOKEN_REGEX)
        for mo in re.finditer(tok_regex, self.code):
            kind = mo.lastgroup
            value = mo.group()
            if kind == 'SKIP' or kind == 'COMMENT':
                continue
            self.tokens.append((kind, value))
        self.tokens.append(('EOF', ''))

# --- 2. AST NODES ---
@dataclass
class TypeNode:
    name: str
    modifier: str = ""

@dataclass
class ImportHeaderDecl:
    path: str

@dataclass
class Param:
    name: str
    type_node: TypeNode

@dataclass
class StructDecl:
    name: str
    fields: List[Param]
    is_pub: bool = False
    is_extern: bool = False

@dataclass
class FnDecl:
    name: str
    params: List[Param]
    returns: List[TypeNode]
    body: List[Any]
    has_err: bool = False
    is_pub: bool = False
    is_extern: bool = False

# --- 3. PARSER (Robust Recursive Descent) ---
class Parser:
    def __init__(self, tokens):
        self.tokens = tokens
        self.pos = 0

    def match(self, expected_kind, expected_val=None):
        if self.pos < len(self.tokens):
            kind, val = self.tokens[self.pos]
            if kind == expected_kind and (expected_val is None or val == expected_val):
                self.pos += 1
                return val
        return None

    def peek(self, kind, val=None):
        if self.pos < len(self.tokens):
            k, v = self.tokens[self.pos]
            return k == kind and (val is None or v == val)
        return False
        
    def peek_next(self, kind, val=None):
        if self.pos + 1 < len(self.tokens):
            k, v = self.tokens[self.pos + 1]
            return k == kind and (val is None or v == val)
        return False

    def parse_type(self) -> TypeNode:
        modifier = ""
        if self.peek("KEYWORD", "mut") or self.peek("KEYWORD", "ref") or self.peek("KEYWORD", "weak"):
            modifier = self.tokens[self.pos][1]
            self.pos += 1
            
        name = "unknown_type"
        if self.pos < len(self.tokens) and self.tokens[self.pos][0] != 'EOF':
            name = self.tokens[self.pos][1]
            self.pos += 1 
            
        return TypeNode(name=name, modifier=modifier)

    def parse_struct(self, is_pub: bool, is_extern: bool) -> StructDecl:
        self.match("KEYWORD", "struct")
        name = self.match("IDENT") or "unknown_struct"
        
        if is_extern:
            self.match("NEWLINE")
            return StructDecl(name, [], is_pub, is_extern)
            
        self.match("NEWLINE")
        fields = []
        
        while self.pos < len(self.tokens) and not self.peek("KEYWORD", "end struct") and not self.peek("EOF"):
            start_pos = self.pos
            if self.peek("NEWLINE"): 
                self.pos += 1
                continue
            
            fname = self.match("IDENT")
            if fname:
                self.match("SYMBOL", ":")
                ftype = self.parse_type()
                fields.append(Param(fname, ftype))
                
            if self.pos == start_pos:
                self.pos += 1
                
        self.match("KEYWORD", "end struct")
        return StructDecl(name, fields, is_pub, is_extern)

    def parse_fn(self, is_pub: bool, is_extern: bool) -> FnDecl:
        self.match("KEYWORD", "fn")
        name = self.match("IDENT") or "unknown_fn"
        params = []
        
        if self.match("SYMBOL", "("):
            while self.pos < len(self.tokens) and not self.peek("SYMBOL", ")") and not self.peek("EOF"):
                start_pos = self.pos
                pname = self.match("IDENT")
                if pname:
                    self.match("SYMBOL", ":")
                    ptype = self.parse_type()
                    params.append(Param(pname, ptype))
                self.match("SYMBOL", ",")
                
                if self.pos == start_pos:
                    self.pos += 1
                    
            self.match("SYMBOL", ")")
        
        returns = []
        has_err = False
        if self.match("SYMBOL", "->"):
            while self.pos < len(self.tokens) and not self.peek("NEWLINE") and not self.peek("EOF"):
                start_pos = self.pos
                if self.peek("TYPE", "err"):
                    has_err = True
                    self.pos += 1
                else:
                    returns.append(self.parse_type())
                self.match("SYMBOL", ",")
                if self.pos == start_pos:
                    self.pos += 1
        
        if is_extern:
            self.match("NEWLINE")
            return FnDecl(name, params, returns, [], has_err, is_pub, is_extern)
                
        body = []
        while self.pos < len(self.tokens) and not self.peek("KEYWORD", "end fn") and not self.peek("EOF"):
            body.append(self.tokens[self.pos])
            self.pos += 1
        self.match("KEYWORD", "end fn")
        
        return FnDecl(name, params, returns, body, has_err, is_pub, is_extern)

    def parse_program(self):
        decls = []
        while self.pos < len(self.tokens):
            if self.peek("EOF"):
                break
                
            if self.peek("KEYWORD", "import") and self.peek_next("KEYWORD", "header"):
                self.match("KEYWORD", "import")
                self.match("KEYWORD", "header")
                path = self.match("STRING")
                if path:
                    decls.append(ImportHeaderDecl(path))
                continue
                
            is_pub = False
            is_extern = False
            
            while self.peek("KEYWORD", "pub") or self.peek("KEYWORD", "extern"):
                if self.peek("KEYWORD", "pub"):
                    is_pub = True
                    self.pos += 1
                if self.peek("KEYWORD", "extern"):
                    is_extern = True
                    self.pos += 1
                    
            if self.peek("KEYWORD", "struct"):
                decls.append(self.parse_struct(is_pub, is_extern))
            elif self.peek("KEYWORD", "fn"):
                decls.append(self.parse_fn(is_pub, is_extern))
            else:
                self.pos += 1 
        return decls

# --- 4. UNITY BUILD TRANSPILER ---
class ProjectTranspiler:
    def __init__(self):
        self.global_symbol_table = {}
        self.modules_ast = []
        
        self.cpp_headers = [
            "#include <iostream>", "#include <string>", "#include <vector>", 
            "#include <memory>", "#include <variant>", "#include <tuple>", 
            "#include <sstream>", "\n"
        ]
        self.cpp_structs = []
        self.cpp_fn_declarations = []
        self.cpp_fn_bodies = []

    def add_module(self, module_name: str, ast_decls: List[Any]):
        self.modules_ast.append((module_name, ast_decls))

    def map_type(self, t: TypeNode, current_module: str) -> str:
        if t.name in ["int", "str"]:
            base = {"int": "int", "str": "std::string"}[t.name]
        else:
            if t.name in self.global_symbol_table and self.global_symbol_table[t.name].get("is_extern"):
                base = f"{t.name}*"
            else:
                base = f"{current_module}_{t.name}"
                
        if t.modifier == "mut": return f"{base}&" if not base.endswith("*") else base
        if t.modifier == "ref": return f"std::shared_ptr<{base}>"
        if t.modifier == "weak": return f"std::weak_ptr<{base}>"
        return base

    def pass_1_scrape(self):
        for module_name, decls in self.modules_ast:
            for decl in decls:
                if isinstance(decl, ImportHeaderDecl):
                    continue
                    
                is_extern = getattr(decl, 'is_extern', False)
                mangled_name = decl.name if is_extern else f"{module_name}_{decl.name}"
                
                if isinstance(decl, StructDecl):
                    self.global_symbol_table[mangled_name] = {"type": "struct", "is_extern": is_extern, "module": module_name}
                elif isinstance(decl, FnDecl):
                    self.global_symbol_table[mangled_name] = {"type": "fn", "is_extern": is_extern, "module": module_name}

    def transpile_fstring(self, token_val: str) -> str:
        content = token_val[2:-1] 
        parts = re.split(r'\{([^}]+)\}', content)
        
        stream_parts = []
        for i, part in enumerate(parts):
            if i % 2 == 0:
                if part: stream_parts.append(f'"{part}"')
            else:
                stream_parts.append(part)
                
        if not stream_parts: return '""'
        chain = " << ".join(stream_parts)
        return f"(std::ostringstream{{}} << {chain}).str()"

    def transpile_expr_tokens(self, current_module: str, tokens: List[Tuple[str, str]]) -> str:
        out = []
        i = 0
        while i < len(tokens):
            kind, val = tokens[i]
            
            if kind == "IDENT" and i + 2 < len(tokens) and tokens[i+1][1] == "." and tokens[i+2][0] == "IDENT":
                module_guess = val
                target_guess = tokens[i+2][1]
                mangled_local = f"{module_guess}_{target_guess}"
                
                if mangled_local in self.global_symbol_table:
                    out.append(mangled_local)
                    i += 3
                    continue
                elif target_guess in self.global_symbol_table and self.global_symbol_table[target_guess].get("is_extern"):
                    out.append(target_guess)
                    i += 3
                    continue

            if kind == "F_STRING":
                out.append(self.transpile_fstring(val))
            elif val == "ref" and i + 1 < len(tokens) and tokens[i+1][0] == "IDENT":
                type_name = tokens[i+1][1]
                out.append(f"std::make_shared<{current_module}_{type_name}>")
                i += 1 
            else:
                out.append(val)
            i += 1
            
        expr = " ".join(out)
        return expr.replace(" . ", ".").replace("( ", "(").replace(" )", ")").replace(" ,", ",")

    def transpile_statement(self, decl: FnDecl, current_module: str, tokens: List[Tuple[str, str]], local_vars: set) -> str:
        
        # 1. EXPLICIT TYPE ASSIGNMENT (e.g., msg: str = "Hello")
        if len(tokens) >= 3 and tokens[0][0] == "IDENT" and tokens[1][1] == ":":
            var_name = tokens[0][1]
            mod = ""
            type_idx = 2
            
            if tokens[2][1] in ["mut", "ref", "weak"]:
                mod = tokens[2][1]
                type_idx = 3
                
            if len(tokens) > type_idx:
                type_name = tokens[type_idx][1]
                
                if len(tokens) > type_idx + 1 and tokens[type_idx + 1][1] == "=":
                    cpp_type = self.map_type(TypeNode(name=type_name, modifier=mod), current_module)
                    local_vars.add(var_name)
                    stripped_tokens = [tokens[0]] + tokens[type_idx + 1:]
                    expr = self.transpile_expr_tokens(current_module, stripped_tokens)
                    return f"{cpp_type} {expr};"
                    
                elif len(tokens) == type_idx + 1:
                    cpp_type = self.map_type(TypeNode(name=type_name, modifier=mod), current_module)
                    local_vars.add(var_name)
                    return f"{cpp_type} {var_name};"

        # 2. STRICT PRINT INTERCEPT
        if len(tokens) >= 3 and tokens[0][1] == "print" and tokens[1][1] == "(":
            arg_tokens = tokens[2:-1] 
            args = []
            curr = []
            for k, v in arg_tokens:
                if v == ",":
                    if curr:
                        args.append(self.transpile_expr_tokens(current_module, curr))
                        curr = []
                else:
                    curr.append((k, v))
            if curr:
                args.append(self.transpile_expr_tokens(current_module, curr))
                
            if not args:
                return 'std::cout << "\\n";'
                
            cout_chain = ' << " " << '.join(args)
            return f'std::cout << {cout_chain} << "\\n";'

        # 3. STANDARD STATEMENT PROCESSING
        is_return = tokens and tokens[0][1] == "return"
        expr = self.transpile_expr_tokens(current_module, tokens)
        
        # INJECT AUTO FOR NEW VARIABLES (Implicit assignment)
        if len(tokens) >= 3 and tokens[0][0] == "IDENT" and tokens[1][1] == "=":
            var_name = tokens[0][1]
            if var_name not in local_vars:
                local_vars.add(var_name)
                expr = f"auto {expr}"
        
        if is_return and decl.has_err:
            parts = expr.split(" ", 1)
            if len(parts) == 2:
                expr = f"return std::make_tuple({parts[1].strip()})"
                
        return f"{expr};"

    def pass_2_generate(self):
        for module_name, decls in self.modules_ast:
            for decl in decls:
                if isinstance(decl, ImportHeaderDecl):
                    self.cpp_headers.append(f"#include {decl.path}")
                elif isinstance(decl, StructDecl):
                    if decl.is_extern:
                        self.cpp_structs.append(f"struct {decl.name};")
                    else:
                        self.cpp_structs.append(f"struct {module_name}_{decl.name} {{")
                        for f in decl.fields:
                            self.cpp_structs.append(f"    {self.map_type(f.type_node, module_name)} {f.name};")
                        self.cpp_structs.append("};\n")

        for module_name, decls in self.modules_ast:
            for decl in decls:
                if isinstance(decl, FnDecl) and not decl.is_extern:
                    sig = self._generate_fn_signature(decl, module_name)
                    self.cpp_fn_declarations.append(f"{sig};")

        for module_name, decls in self.modules_ast:
            for decl in decls:
                if isinstance(decl, FnDecl) and not decl.is_extern:
                    sig = self._generate_fn_signature(decl, module_name)
                    self.cpp_fn_bodies.append(f"{sig} {{")
                    
                    local_vars = set(p.name for p in decl.params)
                    current_line = []
                    
                    for kind, val in decl.body:
                        if kind == "NEWLINE":
                            if current_line:
                                self.cpp_fn_bodies.append(f"    {self.transpile_statement(decl, module_name, current_line, local_vars)}")
                                current_line = []
                        else:
                            current_line.append((kind, val))
                    if current_line:
                        self.cpp_fn_bodies.append(f"    {self.transpile_statement(decl, module_name, current_line, local_vars)}")
                        
                    self.cpp_fn_bodies.append("}\n")

    def _generate_fn_signature(self, decl: FnDecl, module_name: str) -> str:
        params_str = ", ".join(f"{self.map_type(p.type_node, module_name)} {p.name}" for p in decl.params)
        ret_types = [self.map_type(r, module_name) for r in decl.returns]
        if decl.has_err: ret_types.append("std::string")
            
        ret_str = "void" if not ret_types else (ret_types[0] if len(ret_types) == 1 else f"std::tuple<{', '.join(ret_types)}>")
        fn_name = decl.name if decl.is_extern else f"{module_name}_{decl.name}"
        return f"{ret_str} {fn_name}({params_str})"

    def build(self) -> str:
        self.pass_1_scrape()
        self.pass_2_generate()
        
        final_code = []
        final_code.extend(self.cpp_headers)
        final_code.append("// --- STRUCTS ---")
        final_code.extend(self.cpp_structs)
        final_code.append("\n// --- FORWARD DECLARATIONS ---")
        final_code.extend(self.cpp_fn_declarations)
        final_code.append("\n// --- FUNCTION IMPLEMENTATIONS ---")
        final_code.extend(self.cpp_fn_bodies)
        
        final_code.append("\nint main() {")
        if "main_main" in self.global_symbol_table:
            final_code.append("    main_main();")
        final_code.append("    return 0;\n}")
        
        return "\n".join(final_code)

# --- RUNNER ---
def discover_and_compile(project_path: str):
    transpiler = ProjectTranspiler()
    target_dir = Path(project_path)
    
    if target_dir.is_file():
        module_name = target_dir.parent.name
        if not module_name or module_name in [".", "src"]:
            module_name = "main"
            
        with open(target_dir, 'r') as f:
            lexer = Lexer(f.read())
            parser = Parser(lexer.tokens)
            transpiler.add_module(module_name, parser.parse_program())
    else:
        print(f"Scanning directory: {target_dir}")
        cb_files = list(target_dir.rglob("*.cb"))
        
        if not cb_files:
            print(f"No .cb files found in {target_dir}")
            sys.exit(1)
            
        for file_path in cb_files:
            module_name = file_path.parent.name
            if module_name == target_dir.name or module_name in ["src", "."]:
                module_name = "main"
                
            print(f"  -> Parsing {file_path} (Module: {module_name})")
            with open(file_path, 'r') as f:
                lexer = Lexer(f.read())
                parser = Parser(lexer.tokens)
                transpiler.add_module(module_name, parser.parse_program())
                
    cpp_output = transpiler.build()
    out_file = target_dir / "build.cpp" if target_dir.is_dir() else target_dir.with_suffix(".cpp")
    
    with open(out_file, "w") as f:
        f.write(cpp_output)
        
    print(f"\nSuccess! Unity build written to: {out_file}")
    print(f"Compile it with: g++ -std=c++17 {out_file}")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python3 cleanbasic.py <path_to_project_dir_or_file>")
        sys.exit(1)
    discover_and_compile(sys.argv[1])
