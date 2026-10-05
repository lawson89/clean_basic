import re
import sys
from pathlib import Path
from dataclasses import dataclass
from typing import List, Dict, Tuple, Optional, Any

# --- 1. LEXER WITH LINE TRACKING ---
TOKEN_REGEX = [
    ('KEYWORD', r'\b(import|header|extern|pub|type|record|end record|fn|end fn|mut|ref|weak|on error|end error|else if|if|else|end if|for|in|end for|match|case|end match|spawn|return|break|var|const)\b'),
    ('TYPE', r'\b(int|str|list|dict|chan|err)\b'),
    ('F_STRING', r'f"[^"]*"|f\'[^\']*\''), 
    ('STRING', r'"[^"]*"|\'[^\']*\''),
    ('IDENT', r'[a-zA-Z_][a-zA-Z0-9_]*'),
    ('NUMBER', r'\d+'),
    ('SYMBOL', r'(\.\.|\-\>|==|!=|<=|>=|:=|\+=|\-=|\+|-|\*|/|=|<|>|:|\(|\)|\[|\]|\.|,)'),
    ('NEWLINE', r'\n+'),
    ('SKIP', r'[ \t]+'),
    ('COMMENT', r'#.*'),
]

class Lexer:
    def __init__(self, code: str, file_name: str = "main.cb"):
        self.code = code
        self.file_name = file_name
        self.tokens = []
        self.tokenize()

    def tokenize(self):
        tok_regex = '|'.join('(?P<%s>%s)' % pair for pair in TOKEN_REGEX)
        line_num = 1
        for mo in re.finditer(tok_regex, self.code):
            kind = mo.lastgroup
            value = mo.group()
            if kind == 'NEWLINE':
                self.tokens.append((kind, value, line_num))
                line_num += value.count('\n')
                continue
            if kind == 'SKIP' or kind == 'COMMENT':
                continue
            self.tokens.append((kind, value, line_num))
        self.tokens.append(('EOF', '', line_num))

# --- 2. AST NODES WITH LINE METADATA ---
@dataclass
class TypeNode:
    name: str
    modifier: str = ""

@dataclass
class ImportHeaderDecl:
    path: str
    line_num: int = 1

@dataclass
class Param:
    name: str
    type_node: TypeNode

@dataclass
class RecordDecl:
    name: str
    fields: List[Param]
    file_name: str
    line_num: int
    is_pub: bool = False
    is_extern: bool = False

@dataclass
class FnDecl:
    name: str
    params: List[Param]
    returns: List[TypeNode]
    body: List[Any]
    file_name: str
    line_num: int
    has_err: bool = False
    is_pub: bool = False
    is_extern: bool = False

# --- FORMATTER & EMITTER ---
class CPPFormatter:
    def __init__(self, indent_size=4):
        self.indent_size = indent_size
        self.current_indent = 0
        self.output = []

    def emit(self, line: str):
        line = line.strip()
        if not line:
            self.output.append("")
            return

        # Preprocessor directives stay flush left
        if line.startswith("#"):
            self.output.append(line)
            return

        # Decrease indent level for closing braces
        if line.startswith("}") or line.startswith("};"):
            self.current_indent = max(0, self.current_indent - 1)

        indent_str = " " * (self.current_indent * self.indent_size)
        self.output.append(f"{indent_str}{line}")

        # Increase indent level for opening braces
        if line.endswith("{"):
            self.current_indent += 1

    def get_code(self) -> str:
        return "\n".join(self.output)

# --- HELPER UTILITIES ---
def split_by_comma(tokens: List[Tuple]) -> List[List[Tuple]]:
    res = []
    curr = []
    paren_depth = 0
    bracket_depth = 0
    for tok in tokens:
        k, v = tok[0], tok[1]
        if v in ["(", "{"]: paren_depth += 1
        elif v in [")", "}"]: paren_depth = max(0, paren_depth - 1)
        elif v == "[": bracket_depth += 1
        elif v == "]": bracket_depth = max(0, bracket_depth - 1)
        
        if v == "," and paren_depth == 0 and bracket_depth == 0:
            if curr: res.append(curr)
            curr = []
        else:
            curr.append(tok)
    if curr:
        res.append(curr)
    return res

# --- 3. PARSER ---
class Parser:
    def __init__(self, tokens, file_name: str = "main.cb"):
        self.tokens = tokens
        self.file_name = file_name
        self.pos = 0

    def match(self, expected_kind, expected_val=None):
        if self.pos < len(self.tokens):
            kind, val, line = self.tokens[self.pos]
            if kind == expected_kind and (expected_val is None or val == expected_val):
                self.pos += 1
                return val
        return None

    def peek(self, kind, val=None):
        if self.pos < len(self.tokens):
            k, v, _ = self.tokens[self.pos]
            return k == kind and (val is None or v == val)
        return False
        
    def peek_next(self, kind, val=None):
        if self.pos + 1 < len(self.tokens):
            k, v, _ = self.tokens[self.pos + 1]
            return k == kind and (val is None or v == val)
        return False

    def get_current_line(self) -> int:
        if self.pos < len(self.tokens):
            return self.tokens[self.pos][2]
        return 1

    def parse_type(self) -> TypeNode:
        modifier = ""
        if self.peek("KEYWORD", "mut") or self.peek("KEYWORD", "ref") or self.peek("KEYWORD", "weak"):
            modifier = self.tokens[self.pos][1]
            self.pos += 1
            
        type_tokens = []
        bracket_depth = 0
        while self.pos < len(self.tokens):
            kind, val, _ = self.tokens[self.pos]
            if bracket_depth == 0 and (kind in ['NEWLINE', 'EOF'] or val in [')', '->', '=', ':', ',']):
                break
            if val == '[': bracket_depth += 1
            if val == ']': bracket_depth -= 1
            type_tokens.append(val)
            self.pos += 1
            if bracket_depth == 0 and type_tokens and type_tokens[-1] == ']':
                break
                
        name = "".join(type_tokens) if type_tokens else "unknown_type"
        return TypeNode(name=name, modifier=modifier)

    def parse_record(self, is_pub: bool, is_extern: bool) -> RecordDecl:
        start_line = self.get_current_line()
        self.match("KEYWORD", "record")
        name = self.match("IDENT") or "unknown_record"
        
        if is_extern:
            self.match("NEWLINE")
            return RecordDecl(name, [], self.file_name, start_line, is_pub, is_extern)
            
        self.match("NEWLINE")
        fields = []
        
        while self.pos < len(self.tokens) and not self.peek("KEYWORD", "end record") and not self.peek("EOF"):
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
                
        self.match("KEYWORD", "end record")
        return RecordDecl(name, fields, self.file_name, start_line, is_pub, is_extern)

    def parse_fn(self, is_pub: bool, is_extern: bool) -> FnDecl:
        start_line = self.get_current_line()
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
            return FnDecl(name, params, returns, [], self.file_name, start_line, has_err, is_pub, is_extern)
                
        body = []
        while self.pos < len(self.tokens) and not self.peek("KEYWORD", "end fn") and not self.peek("EOF"):
            body.append(self.tokens[self.pos])
            self.pos += 1
        self.match("KEYWORD", "end fn")
        
        return FnDecl(name, params, returns, body, self.file_name, start_line, has_err, is_pub, is_extern)

    def parse_program(self):
        decls = []
        while self.pos < len(self.tokens):
            if self.peek("EOF"):
                break
                
            if self.peek("KEYWORD", "import") and self.peek_next("KEYWORD", "header"):
                line = self.get_current_line()
                self.match("KEYWORD", "import")
                self.match("KEYWORD", "header")
                path = self.match("STRING")
                if path:
                    decls.append(ImportHeaderDecl(path, line))
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
                    
            if self.peek("KEYWORD", "record"):
                decls.append(self.parse_record(is_pub, is_extern))
            elif self.peek("KEYWORD", "fn"):
                decls.append(self.parse_fn(is_pub, is_extern))
            else:
                self.pos += 1 
        return decls

# --- 4. PROJECT TRANSPILER ---
class ProjectTranspiler:
    def __init__(self):
        self.global_symbol_table = {}
        self.modules_ast = []
        
    def add_module(self, module_name: str, ast_decls: List[Any]):
        self.modules_ast.append((module_name, ast_decls))

    def map_type(self, t: TypeNode, current_module: str) -> str:
        name = t.name

        # Support qualified module types like "math_utils.Vec2"
        if "." in name and not name.startswith("std::"):
            mod, tname = name.split(".", 1)
            mangled = f"{mod}_{tname}"
            if mangled in self.global_symbol_table:
                base = mangled
            else:
                base = f"{mod}_{tname}"
        elif "[" in name and name.endswith("]"):
            base_type = name[:name.index("[")].strip()
            inner = name[name.index("[")+1:-1].strip()
            if base_type == "list":
                inner_cpp = self.map_type(TypeNode(name=inner), current_module)
                base = f"std::vector<{inner_cpp}>"
            elif base_type == "dict":
                parts = [p.strip() for p in inner.split(",")]
                k_cpp = self.map_type(TypeNode(name=parts[0]), current_module)
                v_cpp = self.map_type(TypeNode(name=parts[1]), current_module)
                base = f"std::unordered_map<{k_cpp}, {v_cpp}>"
            elif base_type == "chan":
                inner_cpp = self.map_type(TypeNode(name=inner), current_module)
                base = f"Channel<{inner_cpp}>"
            else:
                base = name
        elif name in ["int", "str"]:
            base = {"int": "int", "str": "std::string"}[name]
        else:
            if name in self.global_symbol_table and self.global_symbol_table[name].get("is_extern"):
                base = f"{name}*"
            else:
                base = f"{current_module}_{name}" if name not in ["void", "auto"] else name

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
                
                if isinstance(decl, RecordDecl):
                    self.global_symbol_table[mangled_name] = {"type": "record", "is_extern": is_extern, "module": module_name}
                elif isinstance(decl, FnDecl):
                    self.global_symbol_table[mangled_name] = {"type": "fn", "is_extern": is_extern, "module": module_name}

    def transpile_fstring(self, current_module: str, token_val: str, ref_vars: set) -> str:
        content = token_val[2:-1] 
        parts = re.split(r'\{([^}]+)\}', content)
        
        stream_parts = []
        for i, part in enumerate(parts):
            if i % 2 == 0:
                if part: stream_parts.append(f'"{part}"')
            else:
                inner_lexer = Lexer(part)
                inner_toks = [t for t in inner_lexer.tokens if t[0] != 'EOF']
                transpiled_inner = self.transpile_expr_tokens(current_module, inner_toks, ref_vars)
                stream_parts.append(transpiled_inner)
                
        if not stream_parts: return '""'
        chain = " << ".join(stream_parts)
        return f"(std::ostringstream{{}} << {chain}).str()"

    def transpile_expr_tokens(self, current_module: str, tokens: List[Tuple], ref_vars: set) -> str:
        out = []
        i = 0
        in_parens = 0
        while i < len(tokens):
            kind, val = tokens[i][0], tokens[i][1]
            prev_val = tokens[i-1][1] if i > 0 else ""
            next_val = tokens[i+1][1] if i + 1 < len(tokens) else ""
            
            if val == "(": in_parens += 1
            elif val == ")": in_parens = max(0, in_parens - 1)
                
            if in_parens > 0 and kind == "IDENT" and next_val == "=":
                i += 2
                continue

            if kind == "IDENT" and val in ref_vars and next_val == ".":
                out.append(f"{val}->")
                i += 2
                continue

            if kind == "IDENT" and i + 2 < len(tokens) and next_val == "." and tokens[i+2][0] == "IDENT":
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
                out.append(self.transpile_fstring(current_module, val, ref_vars))
            elif val == "ref" and i + 1 < len(tokens) and tokens[i+1][0] == "IDENT":
                type_name = tokens[i+1][1]
                mangled_type = f"{current_module}_{type_name}" if f"{current_module}_{type_name}" in self.global_symbol_table else type_name
                out.append(f"std::make_shared<{mangled_type}>")
                i += 1 
            elif kind == "IDENT" and prev_val not in [".", "->"]:
                mangled_local = f"{current_module}_{val}"
                if mangled_local in self.global_symbol_table:
                    out.append(mangled_local)
                else:
                    out.append(val)
            else:
                out.append(val)
            i += 1
            
        expr = " ".join(out)
        return expr.replace(" . ", ".").replace("( ", "(").replace(" )", ")").replace(" ,", ",").replace("-> ", "->")

    def transpile_statement(self, decl: FnDecl, current_module: str, tokens: List[Tuple], local_vars: set, ref_vars: set) -> str:
        if not tokens: return ""

        tok0 = tokens[0][1]
        tok1 = tokens[1][1] if len(tokens) > 1 else ""

        if tok0 == "if":
            cond = self.transpile_expr_tokens(current_module, tokens[1:], ref_vars)
            return f"if ({cond}) {{"
        elif tok0 == "else if" or (tok0 == "else" and tok1 == "if"):
            rest = tokens[1:] if tok0 == "else if" else tokens[2:]
            cond = self.transpile_expr_tokens(current_module, rest, ref_vars)
            return f"}} else if ({cond}) {{"
        elif tok0 == "else":
            return "} else {"
        elif tok0 in ["end if", "end"] and (tok0 == "end if" or tok1 == "if"):
            return "}"

        elif tok0 == "for":
            var_name = tokens[1][1]
            local_vars.add(var_name)
            in_idx = [idx for idx, t in enumerate(tokens) if t[1] == "in"]
            if in_idx:
                rhs_tokens = tokens[in_idx[0]+1:]
                has_range = any(t[1] == ".." for t in rhs_tokens)
                if has_range:
                    dotdot_idx = [idx for idx, t in enumerate(rhs_tokens) if t[1] == ".."][0]
                    start_expr = self.transpile_expr_tokens(current_module, rhs_tokens[:dotdot_idx], ref_vars)
                    end_expr = self.transpile_expr_tokens(current_module, rhs_tokens[dotdot_idx+1:], ref_vars)
                    return f"for (int {var_name} = {start_expr}; {var_name} < {end_expr}; ++{var_name}) {{"
                else:
                    iter_expr = self.transpile_expr_tokens(current_module, rhs_tokens, ref_vars)
                    return f"for (auto&& {var_name} : {iter_expr}) {{"
        elif tok0 in ["end for", "end"] and (tok0 == "end for" or tok1 == "for"):
            return "}"

        elif tok0 == "match":
            match_expr = self.transpile_expr_tokens(current_module, tokens[1:], ref_vars)
            return f"switch ({match_expr}) {{"
        elif tok0 == "case":
            case_expr = self.transpile_expr_tokens(current_module, tokens[1:], ref_vars)
            if case_expr.endswith(":"): case_expr = case_expr[:-1]
            return f"case {case_expr}:"
        elif tok0 in ["end match", "end"] and (tok0 == "end match" or tok1 == "match"):
            return "}"

        elif tok0 in ["end error", "end"] and (tok0 == "end error" or tok1 == "error"):
            return "}"

        elif tok0 == "spawn":
            spawn_expr = self.transpile_expr_tokens(current_module, tokens[1:], ref_vars)
            return f"std::async(std::launch::async, [=]() {{ {spawn_expr}; }});"

        elif tok0 == "break":
            return "break;"

        on_err_idx = -1
        for idx, t in enumerate(tokens):
            if t[1] == "on error" or (t[1] == "on" and idx + 1 < len(tokens) and tokens[idx+1][1] == "error"):
                on_err_idx = idx
                break

        if on_err_idx != -1:
            main_tokens = tokens[:on_err_idx]
            if tokens[on_err_idx][1] == "on error":
                err_var = tokens[on_err_idx + 1][1] if len(tokens) > on_err_idx + 1 else "err"
            else:
                err_var = tokens[on_err_idx + 2][1] if len(tokens) > on_err_idx + 2 else "err"

            is_const = main_tokens[0][1] == "const"
            prefix = "const " if is_const else ""

            if main_tokens[0][1] in ["var", "const"]:
                var_name = main_tokens[1][1]
                local_vars.add(var_name)
                assign_idx = [i for i, t in enumerate(main_tokens) if t[1] == "="][0]
                call_expr = self.transpile_expr_tokens(current_module, main_tokens[assign_idx+1:], ref_vars)
                return f"{prefix}auto [{var_name}, {err_var}] = {call_expr};\nif (!{err_var}.empty()) {{"
            else:
                assign_idx = [i for i, t in enumerate(main_tokens) if t[1] == "="][0]
                var_name = self.transpile_expr_tokens(current_module, main_tokens[:assign_idx], ref_vars)
                call_expr = self.transpile_expr_tokens(current_module, main_tokens[assign_idx+1:], ref_vars)
                return f"auto [{var_name}_res, {err_var}] = {call_expr};\n{var_name} = {var_name}_res;\nif (!{err_var}.empty()) {{"

        if tok0 in ["var", "const"] and len(tokens) >= 2 and tokens[1][0] == "IDENT":
            is_const = tok0 == "const"
            prefix = "const " if is_const else ""
            var_name = tokens[1][1]
            local_vars.add(var_name)
            
            if any(t[1] in ["ref", "weak"] for t in tokens):
                ref_vars.add(var_name)
            
            if len(tokens) >= 4 and tokens[2][1] == ":":
                type_tokens = []
                idx = 3
                mod = ""
                if tokens[3][1] in ["mut", "ref", "weak"]:
                    mod = tokens[3][1]
                    idx = 4
                
                assign_idx = -1
                for i in range(idx, len(tokens)):
                    if tokens[i][1] == "=":
                        assign_idx = i
                        break
                    type_tokens.append(tokens[i][1])
                
                full_type_str = "".join(type_tokens)
                cpp_type = self.map_type(TypeNode(name=full_type_str, modifier=mod), current_module)
                
                if assign_idx != -1:
                    expr = self.transpile_expr_tokens(current_module, tokens[assign_idx + 1:], ref_vars)
                    return f"{prefix}{cpp_type} {var_name} = {expr};"
                else:
                    return f"{cpp_type} {var_name};"
                        
            elif len(tokens) >= 4 and tokens[2][1] == "=":
                expr = self.transpile_expr_tokens(current_module, tokens[3:], ref_vars)
                return f"{prefix}auto {var_name} = {expr};"

        if tok0 == "print" and len(tokens) >= 3 and tokens[1][1] == "(":
            arg_tokens = tokens[2:-1] 
            args_raw = split_by_comma(arg_tokens)
            args = [self.transpile_expr_tokens(current_module, arg, ref_vars) for arg in args_raw if arg]
                
            if not args: return 'std::cout << "\\n";'
            cout_chain = ' << " " << '.join(args)
            return f'std::cout << {cout_chain} << "\\n";'

        is_return = tok0 == "return"
        if is_return:
            ret_tokens = tokens[1:]
            if not ret_tokens:
                return "return;"
            
            if decl.has_err:
                args = split_by_comma(ret_tokens)
                expected_success = len(decl.returns)
                
                if len(args) == expected_success + 1:
                    exprs = [self.transpile_expr_tokens(current_module, arg, ref_vars) for arg in args]
                    return f"return std::make_tuple({', '.join(exprs)});"
                elif len(args) == expected_success:
                    exprs = [self.transpile_expr_tokens(current_module, arg, ref_vars) for arg in args]
                    exprs.append('std::string("")')
                    return f"return std::make_tuple({', '.join(exprs)});"
                else:
                    exprs = [self.transpile_expr_tokens(current_module, arg, ref_vars) for arg in args]
                    return f"return std::make_tuple({', '.join(exprs)});"
            else:
                args = split_by_comma(ret_tokens)
                if len(args) > 1:
                    exprs = [self.transpile_expr_tokens(current_module, arg, ref_vars) for arg in args]
                    return f"return std::make_tuple({', '.join(exprs)});"
                else:
                    expr = self.transpile_expr_tokens(current_module, ret_tokens, ref_vars)
                    return f"return {expr};"

        expr = self.transpile_expr_tokens(current_module, tokens, ref_vars)
        return f"{expr};"

    def _generate_fn_signature(self, decl: FnDecl, module_name: str) -> str:
        params_str = ", ".join(f"{self.map_type(p.type_node, module_name)} {p.name}" for p in decl.params)
        ret_types = [self.map_type(r, module_name) for r in decl.returns]
        if decl.has_err: ret_types.append("std::string")
            
        ret_str = "void" if not ret_types else (ret_types[0] if len(ret_types) == 1 else f"std::tuple<{', '.join(ret_types)}>")
        fn_name = decl.name if decl.is_extern else f"{module_name}_{decl.name}"
        return f"{ret_str} {fn_name}({params_str})"

    def build(self) -> str:
        self.pass_1_scrape()
        fmt = CPPFormatter()

        # 1. Headers & Built-in runtime
        headers = [
            "#include <iostream>", "#include <string>", "#include <vector>", 
            "#include <unordered_map>", "#include <memory>", "#include <variant>", 
            "#include <tuple>", "#include <sstream>", "#include <future>",
            "#include <mutex>", "#include <queue>", "#include <condition_variable>",
        ]
        for h in headers: fmt.emit(h)

        # Include custom imported headers
        for module_name, decls in self.modules_ast:
            for decl in decls:
                if isinstance(decl, ImportHeaderDecl):
                    fmt.emit(f"#include {decl.path}")

        fmt.emit("")
        fmt.emit("template<typename T>")
        fmt.emit("class Channel {")
        fmt.emit("    std::queue<T> queue_; std::mutex mutex_; std::condition_variable cv_;")
        fmt.emit("public:")
        fmt.emit("    void send(T val) { std::unique_lock<std::mutex> lock(mutex_); queue_.push(val); cv_.notify_one(); }")
        fmt.emit("    T recv() { std::unique_lock<std::mutex> lock(mutex_); cv_.wait(lock, [this]{ return !queue_.empty(); }); T val = queue_.front(); queue_.pop(); return val; }")
        fmt.emit("};")
        fmt.emit("")

        # 2. Structs
        fmt.emit("// --- STRUCTS ---")
        for module_name, decls in self.modules_ast:
            for decl in decls:
                if isinstance(decl, RecordDecl):
                    fmt.emit(f'#line {decl.line_num} "{decl.file_name}"')
                    if decl.is_extern:
                        fmt.emit(f"struct {decl.name};")
                    else:
                        fmt.emit(f"struct {module_name}_{decl.name} {{")
                        for f in decl.fields:
                            fmt.emit(f"{self.map_type(f.type_node, module_name)} {f.name};")
                        fmt.emit("};")
                    fmt.emit("")

        # 3. Forward Declarations
        fmt.emit("// --- FORWARD DECLARATIONS ---")
        for module_name, decls in self.modules_ast:
            for decl in decls:
                if isinstance(decl, FnDecl) and not decl.is_extern:
                    sig = self._generate_fn_signature(decl, module_name)
                    fmt.emit(f"{sig};")
        fmt.emit("")

        # 4. Function Implementations
        fmt.emit("// --- FUNCTION IMPLEMENTATIONS ---")
        for module_name, decls in self.modules_ast:
            for decl in decls:
                if isinstance(decl, FnDecl) and not decl.is_extern:
                    fmt.emit(f'#line {decl.line_num} "{decl.file_name}"')
                    sig = self._generate_fn_signature(decl, module_name)
                    fmt.emit(f"{sig} {{")
                    
                    local_vars = set(p.name for p in decl.params)
                    ref_vars = set(p.name for p in decl.params if p.type_node.modifier in ["ref", "weak"])
                    
                    current_line = []
                    last_emitted_line = -1

                    for tok in decl.body:
                        kind, val, line_num = tok
                        if kind == "NEWLINE":
                            if current_line:
                                stmt_line = current_line[0][2]
                                stmt = self.transpile_statement(decl, module_name, current_line, local_vars, ref_vars)
                                if stmt:
                                    if stmt_line != last_emitted_line:
                                        fmt.emit(f'#line {stmt_line} "{decl.file_name}"')
                                        last_emitted_line = stmt_line
                                    for sub_stmt in stmt.split('\n'):
                                        fmt.emit(sub_stmt)
                                current_line = []
                        else:
                            current_line.append(tok)

                    if current_line:
                        stmt_line = current_line[0][2]
                        stmt = self.transpile_statement(decl, module_name, current_line, local_vars, ref_vars)
                        if stmt:
                            if stmt_line != last_emitted_line:
                                fmt.emit(f'#line {stmt_line} "{decl.file_name}"')
                            for sub_stmt in stmt.split('\n'):
                                fmt.emit(sub_stmt)

                    fmt.emit("}")
                    fmt.emit("")

        # Entry point
        fmt.emit("int main() {")
        if "main_main" in self.global_symbol_table:
            fmt.emit("    main_main();")
        fmt.emit("    return 0;")
        fmt.emit("}")

        return fmt.get_code()

# --- RUNNER ---
def discover_and_compile(project_path: str):
    transpiler = ProjectTranspiler()
    target_dir = Path(project_path)
    
    if target_dir.is_file():
        file_name = target_dir.name
        module_name = target_dir.parent.name
        if not module_name or module_name in [".", "src"]:
            module_name = "main"
            
        with open(target_dir, 'r') as f:
            lexer = Lexer(f.read(), file_name=file_name)
            parser = Parser(lexer.tokens, file_name=file_name)
            transpiler.add_module(module_name, parser.parse_program())
    else:
        print(f"Scanning directory: {target_dir}")
        cb_files = list(target_dir.rglob("*.cb"))
        
        if not cb_files:
            print(f"No .cb files found in {target_dir}")
            sys.exit(1)
            
        for file_path in cb_files:
            file_name = file_path.name
            module_name = file_path.parent.name
            if module_name == target_dir.name or module_name in ["src", "."]:
                module_name = "main"
                
            print(f"  -> Parsing {file_path} (Module: {module_name})")
            with open(file_path, 'r') as f:
                lexer = Lexer(f.read(), file_name=file_name)
                parser = Parser(lexer.tokens, file_name=file_name)
                transpiler.add_module(module_name, parser.parse_program())
                
    cpp_output = transpiler.build()
    out_file = target_dir / "build.cpp" if target_dir.is_dir() else target_dir.with_suffix(".cpp")
    
    with open(out_file, "w") as f:
        f.write(cpp_output)
        
    print(f"\nSuccess! Unity build written to: {out_file}")
    print(f"Compile with: g++ -std=c++17 -pthread {out_file}")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python3 cleanbasic.py <path_to_project_dir_or_file>")
        sys.exit(1)
    discover_and_compile(sys.argv[1])
