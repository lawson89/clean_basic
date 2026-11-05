#include "src/cleanbasic/ast.hpp"
#include "src/cleanbasic/diagnostics.hpp"
#include "src/cleanbasic/lexer.hpp"
#include "src/cleanbasic/parser.hpp"
#include "src/cleanbasic/analyzer.hpp"
#include "src/cleanbasic/emitter.hpp"
#include "src/cleanbasic/pipeline.hpp"
#include "src/cleanbasic/json.hpp"
#include "src/cleanbasic/lsp.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>
#include <vector>

namespace cb::test_e2e {

struct FixtureExpectations {
    std::vector<std::string> expect_cpp;
    std::vector<std::string> forbid_cpp;
    std::vector<std::string> expect_out;
};

inline std::string read_file_text(const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open()) {
        return "";
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

inline std::vector<std::string> split_lines_normalized(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(line);
    }
    return lines;
}

inline FixtureExpectations parse_fixture_expectations(const std::string& cb_path) {
    FixtureExpectations exp{};
    const std::string content = read_file_text(cb_path);
    const std::vector<std::string> lines = split_lines_normalized(content);

    const std::string cpp_prefix = "' EXPECT_CPP: ";
    const std::string forbid_prefix = "' FORBID_CPP: ";
    const std::string out_prefix = "' EXPECT_OUT: ";

    for (const auto& raw_line : lines) {
        size_t first_non_space = 0;
        while (first_non_space < raw_line.size() &&
               (raw_line[first_non_space] == ' ' || raw_line[first_non_space] == '\t')) {
            first_non_space += 1;
        }
        const std::string line = raw_line.substr(first_non_space);

        if (line.rfind(cpp_prefix, 0) == 0) {
            exp.expect_cpp.push_back(line.substr(cpp_prefix.size()));
        } else if (line.rfind(forbid_prefix, 0) == 0) {
            exp.forbid_cpp.push_back(line.substr(forbid_prefix.size()));
        } else if (line.rfind(out_prefix, 0) == 0) {
            exp.expect_out.push_back(line.substr(out_prefix.size()));
        }
    }

    return exp;
}

inline void run_e2e_fixture(const std::string& case_name, const std::string& cb_path) {
    const std::string artifact_dir = "build/test_artifacts/" + case_name;
    std::error_code ec;
    std::filesystem::create_directories(artifact_dir, ec);
    REQUIRE(!ec);

    const FixtureExpectations exp = parse_fixture_expectations(cb_path);
    REQUIRE(!exp.expect_cpp.empty());
    REQUIRE(!exp.expect_out.empty());

    const std::string out_cpp_path = artifact_dir + "/build.cpp";
    const std::string bin_path = artifact_dir + "/app";
    const std::string stdout_path = artifact_dir + "/stdout.txt";

    const cb::BuildFileResult build_res = cb::build_file(cb_path, out_cpp_path, true, bin_path);
    INFO("Build diagnostic: " << cb::format_diagnostic(build_res.error));
    REQUIRE(build_res.ok);
    CHECK(std::filesystem::exists(out_cpp_path, ec));
    CHECK(std::filesystem::exists(bin_path, ec));

    const std::string cpp_content = read_file_text(out_cpp_path);
    REQUIRE(!cpp_content.empty());

    for (const auto& expected_snippet : exp.expect_cpp) {
        INFO("Expected C++ snippet missing in " << out_cpp_path << ": " << expected_snippet);
        CHECK(cpp_content.find(expected_snippet) != std::string::npos);
    }

    for (const auto& forbidden_snippet : exp.forbid_cpp) {
        INFO("Forbidden C++ snippet present in " << out_cpp_path << ": " << forbidden_snippet);
        CHECK(cpp_content.find(forbidden_snippet) == std::string::npos);
    }

    const std::string run_cmd = "./" + bin_path + " > " + stdout_path + " 2>&1";
    const int exit_code = std::system(run_cmd.c_str());
    REQUIRE(exit_code == 0);
    CHECK(std::filesystem::exists(stdout_path, ec));

    const std::string stdout_content = read_file_text(stdout_path);
    const std::vector<std::string> actual_lines = split_lines_normalized(stdout_content);
    CHECK(actual_lines == exp.expect_out);
}

}  // namespace cb::test_e2e

TEST_CASE("Lexer tokenizes multi-word keywords and rejects C-style operators", "[lexer]") {
    const std::string code = R"(
' VB style comment
import "player.cb" as player
import header "sqlite_wrapper.hpp"
import link "sqlite3"
dim x as int = 1
const y = "hello" & " world"
select case x
case 1
case else
end select
for i = 0 to 10 step 2
end for
if a and b or not c
else if d
end if
fn check() can error
end fn
on error e
end error
defer
end defer
)";

    const auto lex_res = cb::tokenize(code, "lex_test.cb");
    REQUIRE(lex_res.ok);

    std::vector<std::string> kw_values;
    for (const auto& tok : lex_res.tokens) {
        if (tok.kind == "KEYWORD") {
            kw_values.push_back(tok.value);
        }
    }

    auto has_kw = [&](std::string_view expected) {
        for (const auto& val : kw_values) {
            if (val == expected) {
                return true;
            }
        }
        return false;
    };

    CHECK(has_kw("import"));
    CHECK(has_kw("as"));
    CHECK(has_kw("dim"));
    CHECK(has_kw("const"));
    CHECK(has_kw("import header"));
    CHECK(has_kw("import link"));
    CHECK(has_kw("select case"));
    CHECK(has_kw("case"));
    CHECK(has_kw("case else"));
    CHECK(has_kw("end select"));
    CHECK(has_kw("and"));
    CHECK(has_kw("or"));
    CHECK(has_kw("not"));
    CHECK(has_kw("else if"));
    CHECK(has_kw("end if"));
    CHECK(has_kw("can error"));
    CHECK(has_kw("on error"));
    CHECK(has_kw("end error"));
    CHECK(has_kw("for"));
    CHECK(has_kw("to"));
    CHECK(has_kw("step"));
    CHECK(has_kw("end for"));
    CHECK(has_kw("defer"));
    CHECK(has_kw("end defer"));

    SECTION("Rejects forbidden C-style logical operators") {
        const std::vector<std::string> forbidden = {"a && b", "a || b", "!flag"};
        for (const auto& bad_op : forbidden) {
            const auto bad_lex = cb::tokenize(bad_op, "bad_op.cb");
            CHECK_FALSE(bad_lex.ok);
            CHECK(bad_lex.error.kind == "LexerError");
        }
    }
}

TEST_CASE("Parser builds AST for control flow and types and reports syntax errors", "[parser]") {
    const std::string code = R"(
pub alias ScoreMap = dict[str, list[int]]

fn compute(byref items as list[int], tag as str) as int
    dim total = 0
    for i = 0 to 10 step 2
        select case i
        case 2
            total = total + 2
        case else
            total = total + 1
        end select
    end for
    return total
end fn
)";

    const auto lex_res = cb::tokenize(code, "test.cb");
    REQUIRE(lex_res.ok);

    const auto parse_res = cb::parse_tokens(lex_res.tokens, "test.cb");
    REQUIRE(parse_res.ok);

    REQUIRE(parse_res.program.declarations.size() == 2);
    const cb::DeclId fn_decl_id = parse_res.program.declarations[1];
    REQUIRE(fn_decl_id < parse_res.arena.decls.size());
    const auto& decl_node = parse_res.arena.decls[fn_decl_id];
    REQUIRE(std::holds_alternative<cb::FnDecl>(decl_node.payload));
    const auto& fn_decl = std::get<cb::FnDecl>(decl_node.payload);
    CHECK(fn_decl.name == "compute");
    REQUIRE(fn_decl.params.size() == 2);
    CHECK(fn_decl.params[0].is_byref);

    SECTION("Parses classic BASIC 'for i = start to end' with inclusive RangeSpec") {
        const std::string for_to_code = R"(fn loop_test() as int
    dim s = 0
    for i = 1 to 5 step 2
        s = s + i
    end for
    return s
end fn
)";
        const auto lex_ft = cb::tokenize(for_to_code, "for_to.cb");
        REQUIRE(lex_ft.ok);
        const auto parse_ft = cb::parse_tokens(lex_ft.tokens, "for_to.cb");
        REQUIRE(parse_ft.ok);
        REQUIRE(parse_ft.program.declarations.size() == 1);
        const auto& fn_ft = std::get<cb::FnDecl>(parse_ft.arena.decls[parse_ft.program.declarations[0]].payload);
        REQUIRE(fn_ft.body.size() >= 2);
        const auto& for_stmt = std::get<cb::ForStmt>(parse_ft.arena.stmts[fn_ft.body[1]].payload);
        CHECK(for_stmt.range_spec.has_range);
        CHECK(for_stmt.range_spec.inclusive);
        CHECK(for_stmt.range_spec.has_step);
    }

    SECTION("Parses 'can error' on value-returning and void functions") {
        const std::string can_err_code = R"(
fn connect(ip as str) as int can error
    return 1
end fn

fn validate(port as int) can error
    if port <= 0
        error("bad port", code=400)
    end if
end fn
)";
        const auto lex_ce = cb::tokenize(can_err_code, "can_err.cb");
        REQUIRE(lex_ce.ok);
        const auto parse_ce = cb::parse_tokens(lex_ce.tokens, "can_err.cb");
        REQUIRE(parse_ce.ok);
        REQUIRE(parse_ce.program.declarations.size() == 2);
        const auto& fn0 = std::get<cb::FnDecl>(parse_ce.arena.decls[parse_ce.program.declarations[0]].payload);
        const auto& fn1 = std::get<cb::FnDecl>(parse_ce.arena.decls[parse_ce.program.declarations[1]].payload);
        CHECK(fn0.is_fallible);
        CHECK(fn0.return_type != cb::INVALID_ID);
        CHECK(fn1.is_fallible);
        CHECK(fn1.return_type != cb::INVALID_ID);
        CHECK(parse_ce.arena.types[fn1.return_type].name == "void");
        CHECK(parse_ce.arena.types[fn1.return_type].name == "void");
    }

    SECTION("Rejects ':' in named arguments for function calls and struct initialization") {
        const std::string bad_call_colon = R"(
fn main()
    foo(code: 408)
end fn
)";
        const auto lex_c1 = cb::tokenize(bad_call_colon, "bad_call_colon.cb");
        REQUIRE(lex_c1.ok);
        const auto parse_c1 = cb::parse_tokens(lex_c1.tokens, "bad_call_colon.cb");
        CHECK_FALSE(parse_c1.ok);
        CHECK(parse_c1.error.kind == "ParseError");

        const std::string bad_struct_colon = R"(
fn main()
    dim p = new Point(x: 10, y: 20)
end fn
)";
        const auto lex_c2 = cb::tokenize(bad_struct_colon, "bad_struct_colon.cb");
        REQUIRE(lex_c2.ok);
        const auto parse_c2 = cb::parse_tokens(lex_c2.tokens, "bad_struct_colon.cb");
        CHECK_FALSE(parse_c2.ok);
        CHECK(parse_c2.error.kind == "ParseError");
    }

    SECTION("Rejects removed redundant syntax ('for i in a..b', 'Type{...}', 'new Type{...}', and trailing 'as byref')") {
        const std::vector<std::string> removed_syntaxes = {
            R"(fn main()
    for i in 0..10
    end for
end fn
)",
            R"(fn main()
    dim p = Point{x=1, y=2}
end fn
)",
            R"(fn main()
    dim p as shared Point = new Point{x=1, y=2}
end fn
)",
            R"(fn heal(p as byref Player)
end fn
)"
        };
        for (const auto& snippet : removed_syntaxes) {
            const auto lex_rem = cb::tokenize(snippet, "removed.cb");
            REQUIRE(lex_rem.ok);
            const auto parse_rem = cb::parse_tokens(lex_rem.tokens, "removed.cb");
            CHECK_FALSE(parse_rem.ok);
            CHECK(parse_rem.error.kind == "ParseError");
        }
    }

    SECTION("Rejects legacy trailing 'on error' syntax on variable declarations, assignments, and calls") {
        const std::string bad_trailing_dim = R"(fn main()
    dim x = foo() on error e
        return
    end error
end fn
)";
        const auto lex_td = cb::tokenize(bad_trailing_dim, "bad_trailing_dim.cb");
        REQUIRE(lex_td.ok);
        const auto parse_td = cb::parse_tokens(lex_td.tokens, "bad_trailing_dim.cb");
        CHECK_FALSE(parse_td.ok);
        CHECK(parse_td.error.kind == "ParseError");

        const std::string bad_trailing_call = R"(fn main()
    foo() on error e
        return
    end error
end fn
)";
        const auto lex_tc = cb::tokenize(bad_trailing_call, "bad_trailing_call.cb");
        REQUIRE(lex_tc.ok);
        const auto parse_tc = cb::parse_tokens(lex_tc.tokens, "bad_trailing_call.cb");
        CHECK_FALSE(parse_tc.ok);
        CHECK(parse_tc.error.kind == "ParseError");
    }

    SECTION("Reports ParseError on unclosed control flow block") {
        const std::string bad_code = R"(fn broken()
    if true
        dim x = 1
end fn
)";
        const auto bad_lex = cb::tokenize(bad_code, "bad.cb");
        REQUIRE(bad_lex.ok);
        const auto bad_parse = cb::parse_tokens(bad_lex.tokens, "bad.cb");
        CHECK_FALSE(bad_parse.ok);
        CHECK(bad_parse.error.kind == "ParseError");
    }

}

TEST_CASE("Analyzer validates fallible calls, const immutability, and implicit bubbling", "[analyzer]") {
    SECTION("Rejects unhandled fallible call in infallible function") {
        const std::string bad_fallible = R"(
fn risky() as int can error
    return 1
end fn

fn caller_without_err()
    dim x = risky()
end fn
)";

        const auto lex_bad = cb::tokenize(bad_fallible, "bad_fallible.cb");
        REQUIRE(lex_bad.ok);
        auto parse_bad = cb::parse_tokens(lex_bad.tokens, "bad_fallible.cb");
        REQUIRE(parse_bad.ok);
        const auto ana_bad = cb::analyze_program(parse_bad.arena, parse_bad.program);
        CHECK_FALSE(ana_bad.ok);
        CHECK(ana_bad.error.kind == "AnalyzerError");
        CHECK(ana_bad.error.message.find(
            "Unhandled error: function returns an error but caller does not declare 'can error' or have an active 'on error' block"
        ) != std::string::npos);
    }

    SECTION("Rejects unhandled fallible call in expression statement") {
        const std::string bad_expr_stmt = R"(
fn fail() as int can error
    error("failed", code=500)
    return 0
end fn

fn call()
    fail()
end fn
)";
        const auto res = cb::transpile_source(bad_expr_stmt, "bad_expr_stmt.cb");
        CHECK_FALSE(res.ok);
        CHECK(res.error.kind == "AnalyzerError");
        CHECK(res.error.message.find(
            "Unhandled error: function returns an error but caller does not declare 'can error' or have an active 'on error' block"
        ) != std::string::npos);
    }

    SECTION("Rejects fallible call occurring before 'on error' block is registered in scope") {
        const std::string bad_order = R"(
fn risky() as int can error
    return 1
end fn

fn main()
    dim first = risky()
    on error e
        return
    end error
    dim second = risky()
end fn
)";
        const auto res = cb::transpile_source(bad_order, "bad_order.cb");
        CHECK_FALSE(res.ok);
        CHECK(res.error.kind == "AnalyzerError");
        CHECK(res.error.message.find(
            "Unhandled error: function returns an error but caller does not declare 'can error' or have an active 'on error' block"
        ) != std::string::npos);
    }

    SECTION("Rejects 'on error' block that does not terminate with return, error(...), or break") {
        const std::string bad_no_exit = R"(
fn risky() as int can error
    return 1
end fn

fn main()
    on error e
        print("Caught error but forgot to exit: " & e.msg)
    end error
    dim val = risky()
end fn
)";
        const auto res = cb::transpile_source(bad_no_exit, "bad_no_exit.cb");
        CHECK_FALSE(res.ok);
        CHECK(res.error.kind == "AnalyzerError");
        CHECK(res.error.message.find(
            "'on error' block must terminate with an explicit control-flow exit"
        ) != std::string::npos);
    }

    SECTION("Rejects calling error(...) inside an infallible function or with invalid named args") {
        const std::string bad_error_in_infallible = R"(
fn not_fallible() as int
    error("boom", code=1)
    return 0
end fn
)";
        const auto res_inf = cb::transpile_source(bad_error_in_infallible, "bad_error_inf.cb");
        CHECK_FALSE(res_inf.ok);
        CHECK(res_inf.error.kind == "AnalyzerError");
        CHECK(res_inf.error.message.find("Cannot call 'error(...)' inside infallible function") != std::string::npos);

        const std::string bad_error_kwarg = R"(
fn fallible_fn() as int can error
    error("boom", status=500)
    return 0
end fn
)";
        const auto res_kw = cb::transpile_source(bad_error_kwarg, "bad_error_kw.cb");
        CHECK_FALSE(res_kw.ok);
        CHECK(res_kw.error.kind == "AnalyzerError");
        CHECK(res_kw.error.message.find("Unknown named argument 'status'") != std::string::npos);
    }

    SECTION("Rejects unhandled fallible call inside variadic print arguments") {
        const std::string bad_print_arg = R"(
fn risky() as int can error
    return 7
end fn

fn main()
    print("value=", risky())
end fn
)";
        const auto res = cb::transpile_source(bad_print_arg, "bad_print_arg.cb");
        CHECK_FALSE(res.ok);
        CHECK(res.error.kind == "AnalyzerError");

    }

    SECTION("Rejects const variable reassignment") {
        const std::string bad_const = R"(
fn main()
    const limit as int = 10
    limit = 20
end fn
)";
        const auto res_bad_const = cb::transpile_source(bad_const, "bad_const.cb");
        CHECK_FALSE(res_bad_const.ok);
        CHECK(res_bad_const.error.kind == "AnalyzerError");
        CHECK(res_bad_const.error.message.find("Cannot reassign const variable") != std::string::npos);
    }

    SECTION("Annotates implicit_bubble on fallible calls inside fallible functions") {
        const std::string good_code = R"(
fn risky() as int can error
    return 1
end fn

fn middle() as int can error
    dim val = risky()
    return val
end fn

fn main()
    on error e
        print(e.code, ": ", e.msg)
        return
    end error
    dim res = middle()
end fn
)";

        const auto lex_good = cb::tokenize(good_code, "good.cb");
        REQUIRE(lex_good.ok);
        auto parse_good = cb::parse_tokens(lex_good.tokens, "good.cb");
        REQUIRE(parse_good.ok);
        const auto ana_good = cb::analyze_program(parse_good.arena, parse_good.program);
        REQUIRE(ana_good.ok);

        const auto it_risky = ana_good.symbols.functions.find("risky");
        REQUIRE(it_risky != ana_good.symbols.functions.end());
        CHECK(it_risky->second.is_fallible);

        const auto it_middle = ana_good.symbols.functions.find("middle");
        REQUIRE(it_middle != ana_good.symbols.functions.end());
        CHECK(it_middle->second.is_fallible);

        const auto it_main = ana_good.symbols.functions.find("main");
        REQUIRE(it_main != ana_good.symbols.functions.end());
        CHECK_FALSE(it_main->second.is_fallible);

        const cb::DeclId middle_decl_id = parse_good.program.declarations[1];
        const auto& middle_fn = std::get<cb::FnDecl>(parse_good.arena.decls[middle_decl_id].payload);
        REQUIRE(!middle_fn.body.empty());
        const cb::StmtId val_stmt_id = middle_fn.body[0];
        const auto& val_assign = std::get<cb::VarAssignment>(parse_good.arena.stmts[val_stmt_id].payload);
        CHECK(val_assign.implicit_bubble);
    }
}

TEST_CASE("Emitter generates C++17 error bubbling and defer for extern declarations", "[emitter]") {
    const std::string code = R"(
pub extern type SQL_DB
pub extern fn connect(path as str) as SQL_DB can error
pub extern fn SQL_close(db as SQL_DB)

fn process_data() as int can error
    ' Implicit bubble (needs if (cb_has_error(_err1)) return ...)
    dim db = connect("data.db")
    
    defer
        SQL_close(db)
    end defer
    
    return 100
end fn

fn main()
    ' Explicit scope-level handling
    on error e
        print("Crash prevented (" & e.code & "): " & e.msg)
        return
    end error

    dim records = process_data()
end fn
)";

    const auto res = cb::transpile_source(code, "section5.cb");
    REQUIRE(res.ok);

    const std::string& cpp = res.cpp_code;
    CHECK(cpp.find("struct cb_error {") != std::string::npos);
    CHECK(cpp.find("template <typename F> struct _DeferTask { F f; ~_DeferTask() { f(); } };") != std::string::npos);
    CHECK(cpp.find("template <typename F> _DeferTask<F> _defer(F f) { return {f}; }") != std::string::npos);
    CHECK(cpp.find("std::tuple<int, cb_error> process_data()") != std::string::npos);
    CHECK(cpp.find(R"raw(auto [_val1, _err1] = connect(std::string("data.db"));)raw") != std::string::npos);
    CHECK(cpp.find("if (cb_has_error(_err1)) return { {}, _err1 };") != std::string::npos);
    CHECK(cpp.find("auto db = _val1;") != std::string::npos);
    CHECK(cpp.find("auto _defer_1 = _defer([&]() {") != std::string::npos);
    CHECK(cpp.find("SQL_close(db);") != std::string::npos);
    CHECK(cpp.find(R"raw(return { 100, cb_error{"", 0} };)raw") != std::string::npos);
    CHECK(cpp.find("auto [_val2, _err2] = process_data();") != std::string::npos);
    CHECK(cpp.find("if (cb_has_error(_err2)) {") != std::string::npos);
    CHECK(cpp.find("cb_error e = _err2;") != std::string::npos);
    CHECK(cpp.find("auto records = _val2;") != std::string::npos);

    SECTION("Emits variadic print, cb_to_str auto-coercion on &, and inclusive for..to loop") {
        const std::string ergonomic_code = R"(
fn main()
    dim score = 95
    dim ok = true
    for i = 0 to 3
        print("i=", i, " ok=", ok)
    end for
    print("Score: " & score)
    print(score & " points")
    print()
end fn
)";
        const auto erg_res = cb::transpile_source(ergonomic_code, "ergonomic.cb");
        REQUIRE(erg_res.ok);
        CHECK(erg_res.cpp_code.find("for (int i = 0; i <= 3; i += 1) {") != std::string::npos);
        CHECK(erg_res.cpp_code.find(R"raw((std::cout << std::string("i=") << i << std::string(" ok=") << cb_to_str(ok) << std::endl);)raw") != std::string::npos);
        CHECK(erg_res.cpp_code.find(R"raw((std::string("Score: ") + cb_to_str(score)))raw") != std::string::npos);
        CHECK(erg_res.cpp_code.find(R"raw((cb_to_str(score) + std::string(" points")))raw") != std::string::npos);
        CHECK(erg_res.cpp_code.find("(std::cout << std::endl);") != std::string::npos);
    }

    SECTION("Deduplicates canonical TypeNodes in AstArena and emits from const AstArena&") {
        const std::string immut_code = R"(
pub type Box
    val as int
end type

fn compute() as int
    dim a = 10
    dim b = 20
    dim c = 30
    dim s1 = "hello"
    dim s2 = "world"
    dim b1 = Box(val=a)
    dim b2 = Box(val=b)
    return b1.val + b2.val + c
end fn
)";
        const auto lex_im = cb::tokenize(immut_code, "immut.cb");
        REQUIRE(lex_im.ok);
        auto parse_im = cb::parse_tokens(lex_im.tokens, "immut.cb");
        REQUIRE(parse_im.ok);
        const auto ana_im = cb::analyze_program(parse_im.arena, parse_im.program);
        REQUIRE(ana_im.ok);

        const size_t types_after_analysis = parse_im.arena.types.size();
        CHECK(cb::find_type(parse_im.arena, "int") == ana_im.symbols.type_int);
        CHECK(cb::find_type(parse_im.arena, "str") == ana_im.symbols.type_str);

        size_t int_count = 0;
        for (const auto& tnode : parse_im.arena.types) {
            if (tnode.name == "int" && tnode.type_args.empty()) {
                int_count += 1;
            }
        }
        CHECK(int_count == 1);

        const cb::AstArena& const_arena = parse_im.arena;
        const std::string emitted = cb::emit_program(const_arena, ana_im.symbols, parse_im.program);
        CHECK(const_arena.types.size() == types_after_analysis);
        CHECK(emitted.find("struct Box {") != std::string::npos);
        CHECK(emitted.find("int compute()") != std::string::npos);
    }
}

TEST_CASE("End-to-end fixture: multi-file modules, mutability, and UFCS", "[e2e]") {
    cb::test_e2e::run_e2e_fixture("modules_ufcs", "tests/cases/modules_ufcs/main.cb");
}

TEST_CASE("End-to-end fixture: nested folder modules, name collision mangling, and relative imports", "[e2e]") {
    cb::test_e2e::run_e2e_fixture("nested_modules", "tests/cases/nested_modules/main.cb");
}

TEST_CASE("Module scoping, pub visibility, duplicate aliases, diamond deduplication, and circular imports", "[modules]") {
    const std::string base_dir = "build/test_artifacts/module_edge_cases";
    std::error_code ec;
    std::filesystem::remove_all(base_dir, ec);
    std::filesystem::create_directories(base_dir + "/sub", ec);
    REQUIRE(!ec);

    SECTION("Rejects circular imports with clear diagnostic") {
        {
            std::ofstream fa(base_dir + "/a.cb");
            fa << R"(import "sub/b.cb"
pub fn fa() as int
    return 1
end fn
)";
        }
        {
            std::ofstream fb(base_dir + "/sub/b.cb");
            fb << R"(import "../a.cb"
pub fn fb() as int
    return 2
end fn
)";
        }
        const auto res = cb::build_file(base_dir + "/a.cb", base_dir + "/out.cpp", false);
        REQUIRE_FALSE(res.ok);
        CHECK(res.error.kind == "ImportError");
        CHECK(res.error.message.find("Circular module import detected") != std::string::npos);
    }

    SECTION("Deduplicates diamond imports and preserves topological order") {
        {
            std::ofstream fcommon(base_dir + "/sub/common.cb");
            fcommon << R"(pub type SharedCfg
    scale as int
end type
)";
        }
        {
            std::ofstream fleft(base_dir + "/left.cb");
            fleft << R"(import "sub/common.cb"
pub fn make_cfg(s as int) as common.SharedCfg
    return common.SharedCfg(scale=s)
end fn
)";
        }
        {
            std::ofstream fmain(base_dir + "/diamond_main.cb");
            fmain << R"(import "sub/common.cb"
import "left.cb"
fn main()
    dim c = left.make_cfg(7)
    print(c.scale)
end fn
)";
        }
        const auto res = cb::build_file(base_dir + "/diamond_main.cb", base_dir + "/diamond_out.cpp", false);
        REQUIRE(res.ok);
        const std::string cpp = cb::test_e2e::read_file_text(base_dir + "/diamond_out.cpp");
        const size_t first_pos = cpp.find("struct cb_sub_common__SharedCfg {");
        REQUIRE(first_pos != std::string::npos);
        CHECK(cpp.find("struct cb_sub_common__SharedCfg {", first_pos + 1) == std::string::npos);
    }

    SECTION("Rejects duplicate module import aliases in the same file") {
        {
            std::ofstream f1(base_dir + "/mod.cb");
            f1 << R"(pub fn f() as int
    return 1
end fn
)";
        }
        {
            std::ofstream f2(base_dir + "/sub/mod.cb");
            f2 << R"(pub fn f() as int
    return 2
end fn
)";
        }
        {
            std::ofstream fmain(base_dir + "/dup_alias.cb");
            fmain << R"(import "mod.cb"
import "sub/mod.cb"
fn main()
    print(1)
end fn
)";
        }
        const auto res = cb::build_file(base_dir + "/dup_alias.cb", base_dir + "/dup_out.cpp", false);
        REQUIRE_FALSE(res.ok);
        CHECK(res.error.kind == "AnalyzerError");
        CHECK(res.error.message.find("Duplicate module import alias 'mod'") != std::string::npos);
    }

    SECTION("Enforces private by default (non-pub functions and types are rejected across modules)") {
        {
            std::ofstream flib(base_dir + "/secret.cb");
            flib << R"(type HiddenType
    v as int
end type

fn hidden_fn() as int
    return 42
end fn

pub fn public_fn() as int
    return hidden_fn()
end fn
)";
        }
        {
            std::ofstream fcall(base_dir + "/call_priv.cb");
            fcall << R"(import "secret.cb"
fn main()
    dim x = secret.hidden_fn()
end fn
)";
        }
        const auto res_fn = cb::build_file(base_dir + "/call_priv.cb", base_dir + "/priv_fn.cpp", false);
        REQUIRE_FALSE(res_fn.ok);
        CHECK(res_fn.error.message.find("is private to module 'secret'") != std::string::npos);

        {
            std::ofstream ftype(base_dir + "/construct_priv.cb");
            ftype << R"(import "secret.cb"
fn main()
    dim h = secret.HiddenType(v=1)
end fn
)";
        }
        const auto res_type = cb::build_file(base_dir + "/construct_priv.cb", base_dir + "/priv_type.cpp", false);
        REQUIRE_FALSE(res_type.ok);
        CHECK(res_type.error.message.find("is private to module 'secret'") != std::string::npos);
    }
}

TEST_CASE("End-to-end fixture: network error bubbling and defer cleanup", "[e2e]") {
    cb::test_e2e::run_e2e_fixture("network_error_bubbling_defer", "tests/cases/network_error_bubbling_defer.cb");
}

TEST_CASE("End-to-end fixture: STL containers and select case", "[e2e]") {
    cb::test_e2e::run_e2e_fixture("containers_and_select_case", "tests/cases/containers_and_select_case.cb");
}

TEST_CASE("End-to-end fixture: struct initialization and field access", "[e2e]") {
    cb::test_e2e::run_e2e_fixture("struct_and_field_access", "tests/cases/struct_and_field_access.cb");
}

TEST_CASE("End-to-end fixture: for range loop with step", "[e2e]") {
    cb::test_e2e::run_e2e_fixture("for_range_step", "tests/cases/for_range_step.cb");
}

TEST_CASE("End-to-end fixture: cptr interop", "[e2e]") {
    cb::test_e2e::run_e2e_fixture("cptr_interop", "tests/cases/cptr_interop.cb");
}


TEST_CASE("End-to-end fixture: fibonacci sequence", "[e2e]") {
    cb::test_e2e::run_e2e_fixture("fibonacci", "tests/cases/fibonacci.cb");
}
TEST_CASE("End-to-end CLI build generates persistent C++ artifacts", "[e2e]") {
    const std::string artifact_dir = "build/test_artifacts/cli_build";
    std::error_code ec;
    std::filesystem::create_directories(artifact_dir, ec);
    REQUIRE(!ec);

    const std::string test_cb_path = artifact_dir + "/sample_cli.cb";
    const std::string test_out_path = artifact_dir + "/build.cpp";
    const std::string cli_out_path = artifact_dir + "/cli_exec_build.cpp";

    std::ofstream out_cb(test_cb_path);
    out_cb << R"(import link "m"

fn main()
    dim x = 42
    print("Answer: " & str(x))
end fn
)";
    out_cb.close();

    const auto build_res = cb::build_file(test_cb_path, test_out_path, false);
    REQUIRE(build_res.ok);

    const std::string content = cb::test_e2e::read_file_text(test_out_path);
    CHECK(content.find("// CleanBasic Linker Flags: -lm") != std::string::npos);
    CHECK(content.find("int main()") != std::string::npos);

    std::string cb_bin_path = "./build/cleanbasic";
    std::ifstream cb_bin_root("build/cleanbasic");
    if (!cb_bin_root.good()) {
        std::ifstream cb_bin_local("./cleanbasic");
        if (cb_bin_local.good()) {
            cb_bin_path = "./cleanbasic";
        }
    }

    std::ifstream cb_bin_check(cb_bin_path);
    REQUIRE(cb_bin_check.good());

    const std::string cli_cmd = cb_bin_path + " build " + test_cb_path + " -o " + cli_out_path + " > /dev/null 2>&1";
    const int cli_rc = std::system(cli_cmd.c_str());
    CHECK(cli_rc == 0);
    CHECK(std::filesystem::exists(cli_out_path, ec));
}

TEST_CASE("Data-oriented JSON arena parser, serializer, builders, queries, and JSON-RPC framing", "[json]") {
    SECTION("Round-trip parsing and compact serialization of nested objects, arrays, numbers, bools, and null") {
        const std::string input = R"({
            "jsonrpc": "2.0",
            "id": 42,
            "negative": -100,
            "pi": 3.25,
            "active": true,
            "disabled": false,
            "nothing": null,
            "tags": ["lsp", "cleanbasic"],
            "nested": {"x": 10, "y": 20}
        })";

        const auto parsed = cb::parse_json(input);
        REQUIRE(parsed.ok);
        REQUIRE(parsed.root != cb::INVALID_ID);

        CHECK(cb::json_get_string(parsed.arena, cb::json_get_field(parsed.arena, parsed.root, "jsonrpc")) == "2.0");
        CHECK(cb::json_get_int(parsed.arena, cb::json_get_field(parsed.arena, parsed.root, "id")) == 42);
        CHECK(cb::json_get_int(parsed.arena, cb::json_get_field(parsed.arena, parsed.root, "negative")) == -100);
        CHECK(cb::json_get_float(parsed.arena, cb::json_get_field(parsed.arena, parsed.root, "pi")) == 3.25);
        CHECK(cb::json_get_bool(parsed.arena, cb::json_get_field(parsed.arena, parsed.root, "active")) == true);
        CHECK(cb::json_get_bool(parsed.arena, cb::json_get_field(parsed.arena, parsed.root, "disabled")) == false);
        CHECK(cb::json_is_null(parsed.arena, cb::json_get_field(parsed.arena, parsed.root, "nothing")));

        const cb::JsonId tags_id = cb::json_get_field(parsed.arena, parsed.root, "tags");
        REQUIRE(tags_id != cb::INVALID_ID);
        REQUIRE(std::holds_alternative<cb::JsonArray>(parsed.arena.nodes[tags_id]));
        const auto& tags = std::get<cb::JsonArray>(parsed.arena.nodes[tags_id]).elements;
        REQUIRE(tags.size() == 2);
        CHECK(cb::json_get_string(parsed.arena, tags[0]) == "lsp");
        CHECK(cb::json_get_string(parsed.arena, tags[1]) == "cleanbasic");

        const cb::JsonId nested_id = cb::json_get_field(parsed.arena, parsed.root, "nested");
        REQUIRE(nested_id != cb::INVALID_ID);
        CHECK(cb::json_get_int(parsed.arena, cb::json_get_field(parsed.arena, nested_id, "x")) == 10);
        CHECK(cb::json_get_int(parsed.arena, cb::json_get_field(parsed.arena, nested_id, "y")) == 20);

        const std::string serialized = cb::serialize_json(parsed.arena, parsed.root);
        CHECK(serialized == R"({"jsonrpc":"2.0","id":42,"negative":-100,"pi":3.25,"active":true,"disabled":false,"nothing":null,"tags":["lsp","cleanbasic"],"nested":{"x":10,"y":20}})");

        const auto reparsed = cb::parse_json(serialized);
        REQUIRE(reparsed.ok);
        CHECK(cb::serialize_json(reparsed.arena, reparsed.root) == serialized);
    }

    SECTION("String escape sequences and UTF-8 unicode decoding and re-escaping") {
        const std::string bs(1, cb::JSON_BACKSLASH);
        const std::string dq(1, cb::JSON_DQUOTE);
        const std::string escaped_json =
            "{" + dq + "msg" + dq + ":" + dq + "quote:" + bs + dq +
            " slash:" + bs + bs +
            " fwd:" + bs + "/" +
            " bs:" + bs + "b" +
            " ff:" + bs + "f" +
            " nl:" + bs + "n" +
            " cr:" + bs + "r" +
            " tab:" + bs + "t" +
            " hex:" + bs + "u0041" + bs + "u00e9" + dq + "}";
        const auto parsed = cb::parse_json(escaped_json);
        REQUIRE(parsed.ok);

        const auto msg_opt = cb::json_get_string(parsed.arena, cb::json_get_field(parsed.arena, parsed.root, "msg"));
        REQUIRE(msg_opt.has_value());
        std::string expected_msg = "quote:" + dq + " slash:" + bs + " fwd:/ bs:\b ff:\f nl:\n cr:\r tab:\t hex:A";
        expected_msg.push_back(static_cast<char>(0xC3));
        expected_msg.push_back(static_cast<char>(0xA9));
        CHECK(msg_opt.value() == expected_msg);

        const std::string reserialized = cb::serialize_json(parsed.arena, parsed.root);
        const auto roundtrip = cb::parse_json(reserialized);
        REQUIRE(roundtrip.ok);
        CHECK(cb::json_get_string(roundtrip.arena, cb::json_get_field(roundtrip.arena, roundtrip.root, "msg")) == expected_msg);
    }

    SECTION("Ergonomic builder and query functions for constructing LSP payloads") {
        cb::JsonArena arena;
        const cb::JsonId root = cb::json_add_object(arena);
        cb::json_object_set(arena, root, "jsonrpc", cb::json_add_string(arena, "2.0"));
        cb::json_object_set(arena, root, "id", cb::json_add_int(arena, 1));
        cb::json_object_set(arena, root, "id", cb::json_add_int(arena, 7)); // overwrite existing key

        const cb::JsonId result_obj = cb::json_add_object(arena);
        const cb::JsonId items_arr = cb::json_add_array(arena);
        cb::json_array_push(arena, items_arr, cb::json_add_string(arena, "fn"));
        cb::json_array_push(arena, items_arr, cb::json_add_bool(arena, true));
        cb::json_array_push(arena, items_arr, cb::json_add_null(arena));
        cb::json_array_push(arena, items_arr, cb::json_add_float(arena, 1.5));
        cb::json_object_set(arena, result_obj, "items", items_arr);
        cb::json_object_set(arena, root, "result", result_obj);

        CHECK(cb::json_get_int(arena, cb::json_get_field(arena, root, "id")) == 7);
        CHECK(cb::json_get_field(arena, root, "missing") == cb::JSON_INVALID_ID);
        CHECK_FALSE(cb::json_get_string(arena, cb::JSON_INVALID_ID).has_value());
        CHECK_FALSE(cb::json_get_int(arena, cb::json_get_field(arena, root, "jsonrpc")).has_value());
        CHECK_FALSE(cb::json_get_bool(arena, cb::json_get_field(arena, root, "id")).has_value());

        const std::string out = cb::serialize_json(arena, root);
        CHECK(out == R"({"jsonrpc":"2.0","id":7,"result":{"items":["fn",true,null,1.5]}})");
    }

    SECTION("Rejects malformed JSON inputs without throwing exceptions") {
        const std::string bs(1, cb::JSON_BACKSLASH);
        const std::string dq(1, cb::JSON_DQUOTE);
        const std::vector<std::string> bad_inputs = {
            "",
            "   ",
            "{",
            R"({"a": 1,})",
            "[1, 2,]",
            R"({"a" 1})",
            R"({a: 1})",
            R"("unterminated)",
            dq + bs + "xbad" + dq,
            dq + bs + "u12G4" + dq,
            "01",
            "1.",
            "1e+",
            "tru",
            "fals",
            "nul",
            R"({"a": 1} trailing)"
        };

        for (const auto& bad : bad_inputs) {
            const auto res = cb::parse_json(bad);
            CHECK_FALSE(res.ok);
            CHECK_FALSE(res.error.empty());
        }
    }

    SECTION("JSON-RPC 2.0 Content-Length framing read and write over streams") {
        std::stringstream stream;
        const std::string msg1 = R"({"jsonrpc":"2.0","id":1,"method":"initialize"})";
        const std::string msg2 = R"({"jsonrpc":"2.0","method":"initialized","params":{}})";

        cb::write_jsonrpc_message(stream, msg1);
        stream << "Content-Type: application/vscode-jsonrpc; charset=utf-8\r\n";
        cb::write_jsonrpc_message(stream, msg2);

        const auto read1 = cb::read_jsonrpc_message(stream);
        REQUIRE(read1.has_value());
        CHECK(read1.value() == msg1);

        const auto read2 = cb::read_jsonrpc_message(stream);
        REQUIRE(read2.has_value());
        CHECK(read2.value() == msg2);

        const auto read_eof = cb::read_jsonrpc_message(stream);
        CHECK_FALSE(read_eof.has_value());

        std::stringstream truncated("Content-Length: 50\r\n\r\n" + std::string(R"({"short":true})"));
        CHECK_FALSE(cb::read_jsonrpc_message(truncated).has_value());

        std::stringstream missing_len("Custom-Header: 123\r\n\r\n{}");
        CHECK_FALSE(cb::read_jsonrpc_message(missing_len).has_value());
    }
}

TEST_CASE("CleanBasic Language Server Phase 1: Core Loop, In-Memory Overlay & Live Diagnostics", "[lsp]") {
    SECTION("URI and file path conversion helpers") {
        CHECK(cb::uri_to_file_path("file:///workspace/project/main.cb") == "/workspace/project/main.cb");
        CHECK(cb::uri_to_file_path("file://localhost/workspace/main.cb") == "/workspace/main.cb");
        CHECK(cb::uri_to_file_path("file:///workspace/my%20folder/main.cb") == "/workspace/my folder/main.cb");
        CHECK(cb::uri_to_file_path("file:///C:/Users/dev/main.cb") == "C:/Users/dev/main.cb");
        CHECK(cb::file_path_to_uri("/workspace/project/main.cb") == "file:///workspace/project/main.cb");
        CHECK(cb::file_path_to_uri("relative/main.cb") == "file:///relative/main.cb");
    }

    SECTION("Full JSON-RPC lifecycle: initialize, didOpen, publishDiagnostics, didChange, didClose, shutdown, exit") {
        std::stringstream client_to_server;
        std::stringstream server_to_client;

        // 1. initialize request (id = 1)
        cb::write_jsonrpc_message(
            client_to_server,
            R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":"file:///workspace"}})"
        );

        // 2. initialized notification
        cb::write_jsonrpc_message(
            client_to_server,
            R"({"jsonrpc":"2.0","method":"initialized","params":{}})"
        );

        // 3. textDocument/didOpen with valid CleanBasic code
        {
            cb::JsonArena arena;
            const cb::JsonId msg = cb::json_add_object(arena);
            cb::json_object_set(arena, msg, "jsonrpc", cb::json_add_string(arena, "2.0"));
            cb::json_object_set(arena, msg, "method", cb::json_add_string(arena, "textDocument/didOpen"));
            const cb::JsonId params = cb::json_add_object(arena);
            const cb::JsonId td = cb::json_add_object(arena);
            cb::json_object_set(arena, td, "uri", cb::json_add_string(arena, "file:///workspace/valid.cb"));
            cb::json_object_set(arena, td, "languageId", cb::json_add_string(arena, "cleanbasic"));
            cb::json_object_set(arena, td, "version", cb::json_add_int(arena, 1));
            cb::json_object_set(arena, td, "text", cb::json_add_string(arena, R"(fn main() as int
    dim x = 42
    return x
end fn
)"));
            cb::json_object_set(arena, params, "textDocument", td);
            cb::json_object_set(arena, msg, "params", params);
            cb::write_jsonrpc_message(client_to_server, cb::serialize_json(arena, msg));
        }

        // 4. textDocument/didOpen with invalid CleanBasic code (const reassignment on line 3, 1-based -> line 2, 0-based)
        {
            cb::JsonArena arena;
            const cb::JsonId msg = cb::json_add_object(arena);
            cb::json_object_set(arena, msg, "jsonrpc", cb::json_add_string(arena, "2.0"));
            cb::json_object_set(arena, msg, "method", cb::json_add_string(arena, "textDocument/didOpen"));
            const cb::JsonId params = cb::json_add_object(arena);
            const cb::JsonId td = cb::json_add_object(arena);
            cb::json_object_set(arena, td, "uri", cb::json_add_string(arena, "file:///workspace/broken.cb"));
            cb::json_object_set(arena, td, "languageId", cb::json_add_string(arena, "cleanbasic"));
            cb::json_object_set(arena, td, "version", cb::json_add_int(arena, 1));
            cb::json_object_set(arena, td, "text", cb::json_add_string(arena, R"(fn main() as int
    const x = 10
    x = 20
    return x
end fn
)"));
            cb::json_object_set(arena, params, "textDocument", td);
            cb::json_object_set(arena, msg, "params", params);
            cb::write_jsonrpc_message(client_to_server, cb::serialize_json(arena, msg));
        }

        // 5. textDocument/didChange fixing broken.cb (changing const to dim)
        {
            cb::JsonArena arena;
            const cb::JsonId msg = cb::json_add_object(arena);
            cb::json_object_set(arena, msg, "jsonrpc", cb::json_add_string(arena, "2.0"));
            cb::json_object_set(arena, msg, "method", cb::json_add_string(arena, "textDocument/didChange"));
            const cb::JsonId params = cb::json_add_object(arena);
            const cb::JsonId td = cb::json_add_object(arena);
            cb::json_object_set(arena, td, "uri", cb::json_add_string(arena, "file:///workspace/broken.cb"));
            cb::json_object_set(arena, td, "version", cb::json_add_int(arena, 2));
            cb::json_object_set(arena, params, "textDocument", td);
            const cb::JsonId changes = cb::json_add_array(arena);
            const cb::JsonId change_item = cb::json_add_object(arena);
            cb::json_object_set(arena, change_item, "text", cb::json_add_string(arena, R"(fn main() as int
    dim x = 10
    x = 20
    return x
end fn
)"));
            cb::json_array_push(arena, changes, change_item);
            cb::json_object_set(arena, params, "contentChanges", changes);
            cb::json_object_set(arena, msg, "params", params);
            cb::write_jsonrpc_message(client_to_server, cb::serialize_json(arena, msg));
        }

        // 6. textDocument/didClose on broken.cb
        {
            cb::JsonArena arena;
            const cb::JsonId msg = cb::json_add_object(arena);
            cb::json_object_set(arena, msg, "jsonrpc", cb::json_add_string(arena, "2.0"));
            cb::json_object_set(arena, msg, "method", cb::json_add_string(arena, "textDocument/didClose"));
            const cb::JsonId params = cb::json_add_object(arena);
            const cb::JsonId td = cb::json_add_object(arena);
            cb::json_object_set(arena, td, "uri", cb::json_add_string(arena, "file:///workspace/broken.cb"));
            cb::json_object_set(arena, params, "textDocument", td);
            cb::json_object_set(arena, msg, "params", params);
            cb::write_jsonrpc_message(client_to_server, cb::serialize_json(arena, msg));
        }

        // 7. shutdown request (id = 2) and exit notification
        cb::write_jsonrpc_message(
            client_to_server,
            R"({"jsonrpc":"2.0","id":2,"method":"shutdown","params":null})"
        );
        cb::write_jsonrpc_message(
            client_to_server,
            R"({"jsonrpc":"2.0","method":"exit"})"
        );

        const int exit_code = cb::run_lsp_server(client_to_server, server_to_client);
        CHECK(exit_code == 0);

        // Verify Message 1: initialize response
        const auto resp1_raw = cb::read_jsonrpc_message(server_to_client);
        REQUIRE(resp1_raw.has_value());
        const auto resp1 = cb::parse_json(resp1_raw.value());
        REQUIRE(resp1.ok);
        CHECK(cb::json_get_int(resp1.arena, cb::json_get_field(resp1.arena, resp1.root, "id")) == 1);
        const cb::JsonId res1_obj = cb::json_get_field(resp1.arena, resp1.root, "result");
        const cb::JsonId caps_obj = cb::json_get_field(resp1.arena, res1_obj, "capabilities");
        CHECK(cb::json_get_int(resp1.arena, cb::json_get_field(resp1.arena, caps_obj, "textDocumentSync")) == 1);
        const cb::JsonId srv_info = cb::json_get_field(resp1.arena, res1_obj, "serverInfo");
        CHECK(cb::json_get_string(resp1.arena, cb::json_get_field(resp1.arena, srv_info, "name")) == "cleanbasic-lsp");

        // Verify Message 2: publishDiagnostics for valid.cb (empty diagnostics array)
        const auto diag1_raw = cb::read_jsonrpc_message(server_to_client);
        REQUIRE(diag1_raw.has_value());
        const auto diag1 = cb::parse_json(diag1_raw.value());
        REQUIRE(diag1.ok);
        CHECK(cb::json_get_string(diag1.arena, cb::json_get_field(diag1.arena, diag1.root, "method")) == "textDocument/publishDiagnostics");
        const cb::JsonId diag1_params = cb::json_get_field(diag1.arena, diag1.root, "params");
        CHECK(cb::json_get_string(diag1.arena, cb::json_get_field(diag1.arena, diag1_params, "uri")) == "file:///workspace/valid.cb");
        const cb::JsonId diag1_arr_id = cb::json_get_field(diag1.arena, diag1_params, "diagnostics");
        REQUIRE(std::holds_alternative<cb::JsonArray>(diag1.arena.nodes[diag1_arr_id]));
        CHECK(std::get<cb::JsonArray>(diag1.arena.nodes[diag1_arr_id]).elements.empty());

        // Verify Message 3: publishDiagnostics for broken.cb (1 error diagnostic at 0-based line 2, char 4)
        const auto diag2_raw = cb::read_jsonrpc_message(server_to_client);
        REQUIRE(diag2_raw.has_value());
        const auto diag2 = cb::parse_json(diag2_raw.value());
        REQUIRE(diag2.ok);
        CHECK(cb::json_get_string(diag2.arena, cb::json_get_field(diag2.arena, diag2.root, "method")) == "textDocument/publishDiagnostics");
        const cb::JsonId diag2_params = cb::json_get_field(diag2.arena, diag2.root, "params");
        CHECK(cb::json_get_string(diag2.arena, cb::json_get_field(diag2.arena, diag2_params, "uri")) == "file:///workspace/broken.cb");
        const cb::JsonId diag2_arr_id = cb::json_get_field(diag2.arena, diag2_params, "diagnostics");
        REQUIRE(std::holds_alternative<cb::JsonArray>(diag2.arena.nodes[diag2_arr_id]));
        const auto& diag2_elems = std::get<cb::JsonArray>(diag2.arena.nodes[diag2_arr_id]).elements;
        REQUIRE(diag2_elems.size() == 1);
        const cb::JsonId err_diag = diag2_elems[0];
        CHECK(cb::json_get_int(diag2.arena, cb::json_get_field(diag2.arena, err_diag, "severity")) == 1);
        CHECK(cb::json_get_string(diag2.arena, cb::json_get_field(diag2.arena, err_diag, "source")) == "cleanbasic");
        CHECK(cb::json_get_string(diag2.arena, cb::json_get_field(diag2.arena, err_diag, "code")) == "AnalyzerError");
        const auto err_msg_opt = cb::json_get_string(diag2.arena, cb::json_get_field(diag2.arena, err_diag, "message"));
        REQUIRE(err_msg_opt.has_value());
        CHECK(err_msg_opt.value().find("Cannot reassign const variable 'x'") != std::string::npos);

        const cb::JsonId range_obj = cb::json_get_field(diag2.arena, err_diag, "range");
        const cb::JsonId start_obj = cb::json_get_field(diag2.arena, range_obj, "start");
        CHECK(cb::json_get_int(diag2.arena, cb::json_get_field(diag2.arena, start_obj, "line")) == 2);
        CHECK(cb::json_get_int(diag2.arena, cb::json_get_field(diag2.arena, start_obj, "character")) == 4);

        // Verify Message 4: publishDiagnostics after didChange fixes broken.cb (diagnostics cleared)
        const auto diag3_raw = cb::read_jsonrpc_message(server_to_client);
        REQUIRE(diag3_raw.has_value());
        const auto diag3 = cb::parse_json(diag3_raw.value());
        REQUIRE(diag3.ok);
        const cb::JsonId diag3_params = cb::json_get_field(diag3.arena, diag3.root, "params");
        CHECK(cb::json_get_string(diag3.arena, cb::json_get_field(diag3.arena, diag3_params, "uri")) == "file:///workspace/broken.cb");
        const cb::JsonId diag3_arr_id = cb::json_get_field(diag3.arena, diag3_params, "diagnostics");
        REQUIRE(std::holds_alternative<cb::JsonArray>(diag3.arena.nodes[diag3_arr_id]));
        CHECK(std::get<cb::JsonArray>(diag3.arena.nodes[diag3_arr_id]).elements.empty());

        // Verify Message 5: publishDiagnostics after didClose on broken.cb (diagnostics cleared)
        const auto diag4_raw = cb::read_jsonrpc_message(server_to_client);
        REQUIRE(diag4_raw.has_value());
        const auto diag4 = cb::parse_json(diag4_raw.value());
        REQUIRE(diag4.ok);
        const cb::JsonId diag4_params = cb::json_get_field(diag4.arena, diag4.root, "params");
        const cb::JsonId diag4_arr_id = cb::json_get_field(diag4.arena, diag4_params, "diagnostics");
        REQUIRE(std::holds_alternative<cb::JsonArray>(diag4.arena.nodes[diag4_arr_id]));
        CHECK(std::get<cb::JsonArray>(diag4.arena.nodes[diag4_arr_id]).elements.empty());

        // Verify Message 6: shutdown response
        const auto shutdown_raw = cb::read_jsonrpc_message(server_to_client);
        REQUIRE(shutdown_raw.has_value());
        const auto shutdown_resp = cb::parse_json(shutdown_raw.value());
        REQUIRE(shutdown_resp.ok);
        CHECK(cb::json_get_int(shutdown_resp.arena, cb::json_get_field(shutdown_resp.arena, shutdown_resp.root, "id")) == 2);
        CHECK(cb::json_is_null(shutdown_resp.arena, cb::json_get_field(shutdown_resp.arena, shutdown_resp.root, "result")));
    }

    SECTION("In-memory document overlay resolves unsaved imported modules across open buffers") {
        std::stringstream client_to_server;
        std::stringstream server_to_client;

        // Open an unsaved module in memory (does not exist on disk)
        {
            cb::JsonArena arena;
            const cb::JsonId msg = cb::json_add_object(arena);
            cb::json_object_set(arena, msg, "jsonrpc", cb::json_add_string(arena, "2.0"));
            cb::json_object_set(arena, msg, "method", cb::json_add_string(arena, "textDocument/didOpen"));
            const cb::JsonId params = cb::json_add_object(arena);
            const cb::JsonId td = cb::json_add_object(arena);
            cb::json_object_set(arena, td, "uri", cb::json_add_string(arena, "file:///virtual_ws/math_util.cb"));
            cb::json_object_set(arena, td, "languageId", cb::json_add_string(arena, "cleanbasic"));
            cb::json_object_set(arena, td, "version", cb::json_add_int(arena, 1));
            cb::json_object_set(arena, td, "text", cb::json_add_string(arena, R"(pub fn double_val(x as int) as int
    return x * 2
end fn
)"));
            cb::json_object_set(arena, params, "textDocument", td);
            cb::json_object_set(arena, msg, "params", params);
            cb::write_jsonrpc_message(client_to_server, cb::serialize_json(arena, msg));
        }

        // Open main.cb importing math_util.cb and calling math_util.double_val(21)
        {
            cb::JsonArena arena;
            const cb::JsonId msg = cb::json_add_object(arena);
            cb::json_object_set(arena, msg, "jsonrpc", cb::json_add_string(arena, "2.0"));
            cb::json_object_set(arena, msg, "method", cb::json_add_string(arena, "textDocument/didOpen"));
            const cb::JsonId params = cb::json_add_object(arena);
            const cb::JsonId td = cb::json_add_object(arena);
            cb::json_object_set(arena, td, "uri", cb::json_add_string(arena, "file:///virtual_ws/main.cb"));
            cb::json_object_set(arena, td, "languageId", cb::json_add_string(arena, "cleanbasic"));
            cb::json_object_set(arena, td, "version", cb::json_add_int(arena, 1));
            cb::json_object_set(arena, td, "text", cb::json_add_string(arena, R"(import "math_util.cb"
fn main() as int
    return math_util.double_val(21)
end fn
)"));
            cb::json_object_set(arena, params, "textDocument", td);
            cb::json_object_set(arena, msg, "params", params);
            cb::write_jsonrpc_message(client_to_server, cb::serialize_json(arena, msg));
        }

        // Modify math_util.cb in memory to remove 'pub' from double_val, then re-validate main.cb
        {
            cb::JsonArena arena;
            const cb::JsonId msg = cb::json_add_object(arena);
            cb::json_object_set(arena, msg, "jsonrpc", cb::json_add_string(arena, "2.0"));
            cb::json_object_set(arena, msg, "method", cb::json_add_string(arena, "textDocument/didChange"));
            const cb::JsonId params = cb::json_add_object(arena);
            const cb::JsonId td = cb::json_add_object(arena);
            cb::json_object_set(arena, td, "uri", cb::json_add_string(arena, "file:///virtual_ws/math_util.cb"));
            cb::json_object_set(arena, td, "version", cb::json_add_int(arena, 2));
            cb::json_object_set(arena, params, "textDocument", td);
            const cb::JsonId changes = cb::json_add_array(arena);
            const cb::JsonId change_item = cb::json_add_object(arena);
            cb::json_object_set(arena, change_item, "text", cb::json_add_string(arena, R"(fn double_val(x as int) as int
    return x * 2
end fn
)"));
            cb::json_array_push(arena, changes, change_item);
            cb::json_object_set(arena, params, "contentChanges", changes);
            cb::json_object_set(arena, msg, "params", params);
            cb::write_jsonrpc_message(client_to_server, cb::serialize_json(arena, msg));
        }
        {
            cb::JsonArena arena;
            const cb::JsonId msg = cb::json_add_object(arena);
            cb::json_object_set(arena, msg, "jsonrpc", cb::json_add_string(arena, "2.0"));
            cb::json_object_set(arena, msg, "method", cb::json_add_string(arena, "textDocument/didChange"));
            const cb::JsonId params = cb::json_add_object(arena);
            const cb::JsonId td = cb::json_add_object(arena);
            cb::json_object_set(arena, td, "uri", cb::json_add_string(arena, "file:///virtual_ws/main.cb"));
            cb::json_object_set(arena, td, "version", cb::json_add_int(arena, 2));
            cb::json_object_set(arena, params, "textDocument", td);
            const cb::JsonId changes = cb::json_add_array(arena);
            const cb::JsonId change_item = cb::json_add_object(arena);
            cb::json_object_set(arena, change_item, "text", cb::json_add_string(arena, R"(import "math_util.cb"
fn main() as int
    return math_util.double_val(21)
end fn
)"));
            cb::json_array_push(arena, changes, change_item);
            cb::json_object_set(arena, params, "contentChanges", changes);
            cb::json_object_set(arena, msg, "params", params);
            cb::write_jsonrpc_message(client_to_server, cb::serialize_json(arena, msg));
        }

        // Send exit without shutdown -> should return exit code 1
        cb::write_jsonrpc_message(client_to_server, R"({"jsonrpc":"2.0","method":"exit"})");

        const int exit_code = cb::run_lsp_server(client_to_server, server_to_client);
        CHECK(exit_code == 1);

        // 1st notification: math_util.cb didOpen -> 0 diagnostics
        const auto m1 = cb::parse_json(cb::read_jsonrpc_message(server_to_client).value());
        const cb::JsonId m1_diags = cb::json_get_field(m1.arena, cb::json_get_field(m1.arena, m1.root, "params"), "diagnostics");
        CHECK(std::get<cb::JsonArray>(m1.arena.nodes[m1_diags]).elements.empty());

        // 2nd notification: main.cb didOpen -> 0 diagnostics (resolved math_util.cb from in-memory buffer!)
        const auto m2 = cb::parse_json(cb::read_jsonrpc_message(server_to_client).value());
        const cb::JsonId m2_diags = cb::json_get_field(m2.arena, cb::json_get_field(m2.arena, m2.root, "params"), "diagnostics");
        CHECK(std::get<cb::JsonArray>(m2.arena.nodes[m2_diags]).elements.empty());

        // 3rd notification: math_util.cb didChange -> 0 diagnostics
        const auto m3 = cb::parse_json(cb::read_jsonrpc_message(server_to_client).value());
        const cb::JsonId m3_diags = cb::json_get_field(m3.arena, cb::json_get_field(m3.arena, m3.root, "params"), "diagnostics");
        CHECK(std::get<cb::JsonArray>(m3.arena.nodes[m3_diags]).elements.empty());

        // 4th notification: main.cb didChange -> reports private function AnalyzerError from updated in-memory math_util.cb!
        const auto m4 = cb::parse_json(cb::read_jsonrpc_message(server_to_client).value());
        const cb::JsonId m4_diags = cb::json_get_field(m4.arena, cb::json_get_field(m4.arena, m4.root, "params"), "diagnostics");
        const auto& m4_elems = std::get<cb::JsonArray>(m4.arena.nodes[m4_diags]).elements;
        REQUIRE(m4_elems.size() == 1);
        const auto m4_msg = cb::json_get_string(m4.arena, cb::json_get_field(m4.arena, m4_elems[0], "message"));
        REQUIRE(m4_msg.has_value());
        CHECK(m4_msg.value().find("private to module") != std::string::npos);
    }
}

TEST_CASE("Implicit multiline continuation inside (), [], and {} and trailing parameter commas", "[lexer][parser][multiline]") {
    SECTION("Multiline function params with trailing comma, struct constructors, list/dict literals, and parenthesized expressions transpile") {
        const std::string src = R"(type Config
    host as str
    port as int
    retries as int
end type

fn make_config(
    host as str,
    port as int, # inline hash comment
    retries as int, ' trailing comma + single-quote comment
) as Config
    return Config(
        host = host,
        port = port,
        retries = retries,
    )
end fn

fn main() as int
    dim cfg = make_config(
        "localhost",
        8080,
        3,
    )

    dim ports as list[int] = [
        80,
        443,
        cfg.port,
    ]

    dim labels as dict[str, int] = {
        "http": ports[0],
        "https": ports[1],
        "app": ports[2],
    }

    dim ok = (
        cfg.port == 8080
        and cfg.retries == 3
        and labels["app"] == 8080
    )

    if ok
        return 0
    end if
    return 1
end fn
)";
        const auto res = cb::transpile_source(src, "multiline.cb");
        REQUIRE(res.ok);
        CHECK(res.cpp_code.find("Config make_config(std::string host, int port, int retries)") != std::string::npos);
    }

    SECTION("Line and column numbers remain exact inside and after multiline bracketed blocks") {
        const std::string src = R"(fn compute(
    a as int,
    b as int,
) as int
    dim items as list[int] = [
        a,
        b,
    ]
    undeclared_var = items[0]
    return 0
end fn
)";
        const auto check_res = cb::check_source(src, "diag_multiline.cb");
        REQUIRE_FALSE(check_res.ok);
        CHECK(check_res.error.kind == "AnalyzerError");
        CHECK(check_res.error.line == 9);
        CHECK(check_res.error.col == 5);
        CHECK(check_res.error.message.find("Undeclared variable 'undeclared_var'") != std::string::npos);
    }
}

TEST_CASE("CleanBasic Language Server Phase 2: documentSymbol, hover, definition & snapshot fallback", "[lsp]") {
    SECTION("End-to-end documentSymbol, hover, definition (same-file, cross-module, struct fields), and snapshot resilience") {
        std::stringstream client_to_server;
        std::stringstream server_to_client;

        // 1. initialize (id = 1)
        cb::write_jsonrpc_message(
            client_to_server,
            R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":"file:///ws"}})"
        );

        // 2. didOpen player.cb (in-memory imported module)
        // Line 1: pub alias Score = int
        // Line 2: pub type Player
        // Line 3:     name as str
        // Line 4:     health as int
        // Line 5: end type
        // Line 6: pub fn heal(byref p as Player, amount as int) as int can error
        // Line 7:     p.health = p.health + amount
        // Line 8:     return p.health
        // Line 9: end fn
        {
            cb::JsonArena arena;
            const cb::JsonId msg = cb::json_add_object(arena);
            cb::json_object_set(arena, msg, "jsonrpc", cb::json_add_string(arena, "2.0"));
            cb::json_object_set(arena, msg, "method", cb::json_add_string(arena, "textDocument/didOpen"));
            const cb::JsonId params = cb::json_add_object(arena);
            const cb::JsonId td = cb::json_add_object(arena);
            cb::json_object_set(arena, td, "uri", cb::json_add_string(arena, "file:///ws/player.cb"));
            cb::json_object_set(arena, td, "languageId", cb::json_add_string(arena, "cleanbasic"));
            cb::json_object_set(arena, td, "version", cb::json_add_int(arena, 1));
            cb::json_object_set(arena, td, "text", cb::json_add_string(arena, R"(pub alias Score = int
pub type Player
    name as str
    health as int
end type
pub fn heal(byref p as Player, amount as int) as int can error
    p.health = p.health + amount
    return p.health
end fn
)"));
            cb::json_object_set(arena, params, "textDocument", td);
            cb::json_object_set(arena, msg, "params", params);
            cb::write_jsonrpc_message(client_to_server, cb::serialize_json(arena, msg));
        }

        // 3. didOpen main.cb
        // Line 1 (0-based 0): import "player.cb"
        // Line 2 (0-based 1): fn local_boost(base as int) as int
        // Line 3 (0-based 2):     return base + 10
        // Line 4 (0-based 3): end fn
        // Line 5 (0-based 4): fn main() as int can error
        // Line 6 (0-based 5):     const max_potion = 50
        // Line 7 (0-based 6):     dim hero = player.Player(name="Alice", health=100)
        // Line 8 (0-based 7):     dim boosted = local_boost(max_potion)
        // Line 9 (0-based 8):     dim new_hp = hero.heal(boosted)
        // Line 10 (0-based 9):    print(hero.health, new_hp)
        // Line 11 (0-based 10):   return 0
        // Line 12 (0-based 11): end fn
        {
            cb::JsonArena arena;
            const cb::JsonId msg = cb::json_add_object(arena);
            cb::json_object_set(arena, msg, "jsonrpc", cb::json_add_string(arena, "2.0"));
            cb::json_object_set(arena, msg, "method", cb::json_add_string(arena, "textDocument/didOpen"));
            const cb::JsonId params = cb::json_add_object(arena);
            const cb::JsonId td = cb::json_add_object(arena);
            cb::json_object_set(arena, td, "uri", cb::json_add_string(arena, "file:///ws/main.cb"));
            cb::json_object_set(arena, td, "languageId", cb::json_add_string(arena, "cleanbasic"));
            cb::json_object_set(arena, td, "version", cb::json_add_int(arena, 1));
            cb::json_object_set(arena, td, "text", cb::json_add_string(arena, R"(import "player.cb"
fn local_boost(base as int) as int
    return base + 10
end fn
fn main() as int can error
    const max_potion = 50
    dim hero = player.Player(name="Alice", health=100)
    dim boosted = local_boost(max_potion)
    dim new_hp = hero.heal(boosted)
    print(hero.health, new_hp)
    return 0
end fn
)"));
            cb::json_object_set(arena, params, "textDocument", td);
            cb::json_object_set(arena, msg, "params", params);
            cb::write_jsonrpc_message(client_to_server, cb::serialize_json(arena, msg));
        }

        // 4. textDocument/documentSymbol on player.cb (id = 2)
        cb::write_jsonrpc_message(
            client_to_server,
            R"({"jsonrpc":"2.0","id":2,"method":"textDocument/documentSymbol","params":{"textDocument":{"uri":"file:///ws/player.cb"}}})"
        );

        // 5. textDocument/hover on inferred local variable 'hero' in main.cb (0-based line 6, char 9) (id = 3)
        cb::write_jsonrpc_message(
            client_to_server,
            R"({"jsonrpc":"2.0","id":3,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///ws/main.cb"},"position":{"line":6,"character":9}}})"
        );

        // 6. textDocument/hover on UFCS call 'hero.heal' in main.cb (0-based line 8, char 23 -> 'heal') (id = 4)
        cb::write_jsonrpc_message(
            client_to_server,
            R"({"jsonrpc":"2.0","id":4,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///ws/main.cb"},"position":{"line":8,"character":23}}})"
        );

        // 7. textDocument/hover on struct field 'hero.health' in main.cb (0-based line 9, char 16 -> 'health') (id = 5)
        cb::write_jsonrpc_message(
            client_to_server,
            R"({"jsonrpc":"2.0","id":5,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///ws/main.cb"},"position":{"line":9,"character":16}}})"
        );

        // 8. textDocument/hover on builtin 'print' in main.cb (0-based line 9, char 5) (id = 6)
        cb::write_jsonrpc_message(
            client_to_server,
            R"({"jsonrpc":"2.0","id":6,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///ws/main.cb"},"position":{"line":9,"character":5}}})"
        );

        // 9. textDocument/definition on same-file function call 'local_boost' (0-based line 7, char 20) (id = 7)
        cb::write_jsonrpc_message(
            client_to_server,
            R"({"jsonrpc":"2.0","id":7,"method":"textDocument/definition","params":{"textDocument":{"uri":"file:///ws/main.cb"},"position":{"line":7,"character":20}}})"
        );

        // 10. textDocument/definition on cross-module UFCS call 'hero.heal' (0-based line 8, char 23) (id = 8)
        cb::write_jsonrpc_message(
            client_to_server,
            R"({"jsonrpc":"2.0","id":8,"method":"textDocument/definition","params":{"textDocument":{"uri":"file:///ws/main.cb"},"position":{"line":8,"character":23}}})"
        );

        // 11. textDocument/definition on struct field 'hero.health' (0-based line 9, char 16) (id = 9)
        cb::write_jsonrpc_message(
            client_to_server,
            R"({"jsonrpc":"2.0","id":9,"method":"textDocument/definition","params":{"textDocument":{"uri":"file:///ws/main.cb"},"position":{"line":9,"character":16}}})"
        );

        // 12. textDocument/definition on local const 'max_potion' usage (0-based line 7, char 32) (id = 10)
        cb::write_jsonrpc_message(
            client_to_server,
            R"({"jsonrpc":"2.0","id":10,"method":"textDocument/definition","params":{"textDocument":{"uri":"file:///ws/main.cb"},"position":{"line":7,"character":32}}})"
        );

        // 13. Introduce transient syntax error in main.cb via didChange, then request documentSymbol (id = 11) and hover on 'hero' (id = 12)
        {
            cb::JsonArena arena;
            const cb::JsonId msg = cb::json_add_object(arena);
            cb::json_object_set(arena, msg, "jsonrpc", cb::json_add_string(arena, "2.0"));
            cb::json_object_set(arena, msg, "method", cb::json_add_string(arena, "textDocument/didChange"));
            const cb::JsonId params = cb::json_add_object(arena);
            const cb::JsonId td = cb::json_add_object(arena);
            cb::json_object_set(arena, td, "uri", cb::json_add_string(arena, "file:///ws/main.cb"));
            cb::json_object_set(arena, td, "version", cb::json_add_int(arena, 2));
            cb::json_object_set(arena, params, "textDocument", td);
            const cb::JsonId changes = cb::json_add_array(arena);
            const cb::JsonId change_item = cb::json_add_object(arena);
            cb::json_object_set(arena, change_item, "text", cb::json_add_string(arena, R"(import "player.cb"
fn local_boost(base as int) as int
    return base + 10
end fn
fn main() as int can error
    const max_potion = 50
    dim hero = player.Player(name="Alice", health=100)
    if
)"));
            cb::json_array_push(arena, changes, change_item);
            cb::json_object_set(arena, params, "contentChanges", changes);
            cb::json_object_set(arena, msg, "params", params);
            cb::write_jsonrpc_message(client_to_server, cb::serialize_json(arena, msg));
        }

        cb::write_jsonrpc_message(
            client_to_server,
            R"({"jsonrpc":"2.0","id":11,"method":"textDocument/documentSymbol","params":{"textDocument":{"uri":"file:///ws/main.cb"}}})"
        );
        cb::write_jsonrpc_message(
            client_to_server,
            R"({"jsonrpc":"2.0","id":12,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///ws/main.cb"},"position":{"line":6,"character":9}}})"
        );

        // 14. shutdown & exit
        cb::write_jsonrpc_message(client_to_server, R"({"jsonrpc":"2.0","id":99,"method":"shutdown","params":null})");
        cb::write_jsonrpc_message(client_to_server, R"({"jsonrpc":"2.0","method":"exit"})");

        const int exit_code = cb::run_lsp_server(client_to_server, server_to_client);
        CHECK(exit_code == 0);

        // Msg 1: initialize response capabilities
        const auto init_resp = cb::parse_json(cb::read_jsonrpc_message(server_to_client).value());
        REQUIRE(init_resp.ok);
        const cb::JsonId caps = cb::json_get_field(
            init_resp.arena,
            cb::json_get_field(init_resp.arena, init_resp.root, "result"),
            "capabilities"
        );
        CHECK(cb::json_get_bool(init_resp.arena, cb::json_get_field(init_resp.arena, caps, "documentSymbolProvider")) == true);
        CHECK(cb::json_get_bool(init_resp.arena, cb::json_get_field(init_resp.arena, caps, "definitionProvider")) == true);
        CHECK(cb::json_get_bool(init_resp.arena, cb::json_get_field(init_resp.arena, caps, "hoverProvider")) == true);

        // Msg 2 & 3: publishDiagnostics for player.cb and main.cb (both valid)
        const auto diag_player = cb::parse_json(cb::read_jsonrpc_message(server_to_client).value());
        REQUIRE(diag_player.ok);
        const auto diag_main = cb::parse_json(cb::read_jsonrpc_message(server_to_client).value());
        REQUIRE(diag_main.ok);

        // Msg 4: documentSymbol response for player.cb (id = 2)
        const auto doc_sym_resp = cb::parse_json(cb::read_jsonrpc_message(server_to_client).value());
        REQUIRE(doc_sym_resp.ok);
        CHECK(cb::json_get_int(doc_sym_resp.arena, cb::json_get_field(doc_sym_resp.arena, doc_sym_resp.root, "id")) == 2);
        const cb::JsonId sym_arr_id = cb::json_get_field(doc_sym_resp.arena, doc_sym_resp.root, "result");
        REQUIRE(std::holds_alternative<cb::JsonArray>(doc_sym_resp.arena.nodes[sym_arr_id]));
        const auto& sym_elems = std::get<cb::JsonArray>(doc_sym_resp.arena.nodes[sym_arr_id]).elements;
        REQUIRE(sym_elems.size() == 3);

        // Symbol 0: Alias 'Score' (kind 26)
        CHECK(cb::json_get_string(doc_sym_resp.arena, cb::json_get_field(doc_sym_resp.arena, sym_elems[0], "name")) == "Score");
        CHECK(cb::json_get_int(doc_sym_resp.arena, cb::json_get_field(doc_sym_resp.arena, sym_elems[0], "kind")) == 26);
        CHECK(cb::json_get_string(doc_sym_resp.arena, cb::json_get_field(doc_sym_resp.arena, sym_elems[0], "detail")) == "= int");

        // Symbol 1: Type 'Player' (kind 23) with 2 Field children (kind 8)
        CHECK(cb::json_get_string(doc_sym_resp.arena, cb::json_get_field(doc_sym_resp.arena, sym_elems[1], "name")) == "Player");
        CHECK(cb::json_get_int(doc_sym_resp.arena, cb::json_get_field(doc_sym_resp.arena, sym_elems[1], "kind")) == 23);
        const cb::JsonId children_id = cb::json_get_field(doc_sym_resp.arena, sym_elems[1], "children");
        REQUIRE(std::holds_alternative<cb::JsonArray>(doc_sym_resp.arena.nodes[children_id]));
        const auto& field_children = std::get<cb::JsonArray>(doc_sym_resp.arena.nodes[children_id]).elements;
        REQUIRE(field_children.size() == 2);
        CHECK(cb::json_get_string(doc_sym_resp.arena, cb::json_get_field(doc_sym_resp.arena, field_children[0], "name")) == "name");
        CHECK(cb::json_get_int(doc_sym_resp.arena, cb::json_get_field(doc_sym_resp.arena, field_children[0], "kind")) == 8);
        CHECK(cb::json_get_string(doc_sym_resp.arena, cb::json_get_field(doc_sym_resp.arena, field_children[0], "detail")) == "str");
        CHECK(cb::json_get_string(doc_sym_resp.arena, cb::json_get_field(doc_sym_resp.arena, field_children[1], "name")) == "health");
        CHECK(cb::json_get_string(doc_sym_resp.arena, cb::json_get_field(doc_sym_resp.arena, field_children[1], "detail")) == "int");

        // Symbol 2: Fn 'heal' (kind 12)
        CHECK(cb::json_get_string(doc_sym_resp.arena, cb::json_get_field(doc_sym_resp.arena, sym_elems[2], "name")) == "heal");
        CHECK(cb::json_get_int(doc_sym_resp.arena, cb::json_get_field(doc_sym_resp.arena, sym_elems[2], "kind")) == 12);
        CHECK(cb::json_get_string(doc_sym_resp.arena, cb::json_get_field(doc_sym_resp.arena, sym_elems[2], "detail")) == "(byref p as Player, amount as int) as int can error");

        // Msg 5: hover on inferred variable 'hero' (id = 3)
        const auto hover_hero = cb::parse_json(cb::read_jsonrpc_message(server_to_client).value());
        REQUIRE(hover_hero.ok);
        const cb::JsonId h3_contents = cb::json_get_field(
            hover_hero.arena,
            cb::json_get_field(hover_hero.arena, hover_hero.root, "result"),
            "contents"
        );
        const auto h3_val = cb::json_get_string(hover_hero.arena, cb::json_get_field(hover_hero.arena, h3_contents, "value"));
        REQUIRE(h3_val.has_value());
        CHECK(h3_val.value().find("dim hero as Player") != std::string::npos);

        // Msg 6: hover on UFCS call 'hero.heal' (id = 4)
        const auto hover_heal = cb::parse_json(cb::read_jsonrpc_message(server_to_client).value());
        REQUIRE(hover_heal.ok);
        const cb::JsonId h4_contents = cb::json_get_field(
            hover_heal.arena,
            cb::json_get_field(hover_heal.arena, hover_heal.root, "result"),
            "contents"
        );
        const auto h4_val = cb::json_get_string(hover_heal.arena, cb::json_get_field(hover_heal.arena, h4_contents, "value"));
        REQUIRE(h4_val.has_value());
        CHECK(h4_val.value().find("pub fn heal(byref p as Player, amount as int) as int can error") != std::string::npos);

        // Msg 7: hover on struct field 'hero.health' (id = 5)
        const auto hover_field = cb::parse_json(cb::read_jsonrpc_message(server_to_client).value());
        REQUIRE(hover_field.ok);
        const cb::JsonId h5_contents = cb::json_get_field(
            hover_field.arena,
            cb::json_get_field(hover_field.arena, hover_field.root, "result"),
            "contents"
        );
        const auto h5_val = cb::json_get_string(hover_field.arena, cb::json_get_field(hover_field.arena, h5_contents, "value"));
        REQUIRE(h5_val.has_value());
        CHECK(h5_val.value().find("(field) Player.health as int") != std::string::npos);

        // Msg 8: hover on builtin 'print' (id = 6)
        const auto hover_print = cb::parse_json(cb::read_jsonrpc_message(server_to_client).value());
        REQUIRE(hover_print.ok);
        const cb::JsonId h6_contents = cb::json_get_field(
            hover_print.arena,
            cb::json_get_field(hover_print.arena, hover_print.root, "result"),
            "contents"
        );
        const auto h6_val = cb::json_get_string(hover_print.arena, cb::json_get_field(hover_print.arena, h6_contents, "value"));
        REQUIRE(h6_val.has_value());
        CHECK(h6_val.value().find("fn print(values...)") != std::string::npos);

        // Msg 9: definition on same-file 'local_boost' (id = 7) -> main.cb, 0-based line 1, char 3
        const auto def_local_fn = cb::parse_json(cb::read_jsonrpc_message(server_to_client).value());
        REQUIRE(def_local_fn.ok);
        const cb::JsonId def7_res = cb::json_get_field(def_local_fn.arena, def_local_fn.root, "result");
        CHECK(cb::json_get_string(def_local_fn.arena, cb::json_get_field(def_local_fn.arena, def7_res, "uri")) == "file:///ws/main.cb");
        const cb::JsonId def7_start = cb::json_get_field(
            def_local_fn.arena,
            cb::json_get_field(def_local_fn.arena, def7_res, "range"),
            "start"
        );
        CHECK(cb::json_get_int(def_local_fn.arena, cb::json_get_field(def_local_fn.arena, def7_start, "line")) == 1);
        CHECK(cb::json_get_int(def_local_fn.arena, cb::json_get_field(def_local_fn.arena, def7_start, "character")) == 3);

        // Msg 10: definition on cross-module UFCS 'hero.heal' (id = 8) -> player.cb, 0-based line 5, char 7
        const auto def_ufcs = cb::parse_json(cb::read_jsonrpc_message(server_to_client).value());
        REQUIRE(def_ufcs.ok);
        const cb::JsonId def8_res = cb::json_get_field(def_ufcs.arena, def_ufcs.root, "result");
        CHECK(cb::json_get_string(def_ufcs.arena, cb::json_get_field(def_ufcs.arena, def8_res, "uri")) == "file:///ws/player.cb");
        const cb::JsonId def8_start = cb::json_get_field(
            def_ufcs.arena,
            cb::json_get_field(def_ufcs.arena, def8_res, "range"),
            "start"
        );
        CHECK(cb::json_get_int(def_ufcs.arena, cb::json_get_field(def_ufcs.arena, def8_start, "line")) == 5);
        CHECK(cb::json_get_int(def_ufcs.arena, cb::json_get_field(def_ufcs.arena, def8_start, "character")) == 7);

        // Msg 11: definition on struct field 'hero.health' (id = 9) -> player.cb, 0-based line 3, char 4
        const auto def_field = cb::parse_json(cb::read_jsonrpc_message(server_to_client).value());
        REQUIRE(def_field.ok);
        const cb::JsonId def9_res = cb::json_get_field(def_field.arena, def_field.root, "result");
        CHECK(cb::json_get_string(def_field.arena, cb::json_get_field(def_field.arena, def9_res, "uri")) == "file:///ws/player.cb");
        const cb::JsonId def9_start = cb::json_get_field(
            def_field.arena,
            cb::json_get_field(def_field.arena, def9_res, "range"),
            "start"
        );
        CHECK(cb::json_get_int(def_field.arena, cb::json_get_field(def_field.arena, def9_start, "line")) == 3);
        CHECK(cb::json_get_int(def_field.arena, cb::json_get_field(def_field.arena, def9_start, "character")) == 4);

        // Msg 12: definition on local const 'max_potion' (id = 10) -> main.cb, 0-based line 5, char 10
        const auto def_var = cb::parse_json(cb::read_jsonrpc_message(server_to_client).value());
        REQUIRE(def_var.ok);
        const cb::JsonId def10_res = cb::json_get_field(def_var.arena, def_var.root, "result");
        CHECK(cb::json_get_string(def_var.arena, cb::json_get_field(def_var.arena, def10_res, "uri")) == "file:///ws/main.cb");
        const cb::JsonId def10_start = cb::json_get_field(
            def_var.arena,
            cb::json_get_field(def_var.arena, def10_res, "range"),
            "start"
        );
        CHECK(cb::json_get_int(def_var.arena, cb::json_get_field(def_var.arena, def10_start, "line")) == 5);
        CHECK(cb::json_get_int(def_var.arena, cb::json_get_field(def_var.arena, def10_start, "character")) == 10);

        // Msg 13: publishDiagnostics for broken main.cb after didChange
        const auto diag_broken = cb::parse_json(cb::read_jsonrpc_message(server_to_client).value());
        REQUIRE(diag_broken.ok);

        // Msg 14: documentSymbol on main.cb while broken (id = 11) -> falls back to last-known-good snapshot!
        const auto sym_fallback = cb::parse_json(cb::read_jsonrpc_message(server_to_client).value());
        REQUIRE(sym_fallback.ok);
        const cb::JsonId fb_sym_arr = cb::json_get_field(sym_fallback.arena, sym_fallback.root, "result");
        REQUIRE(std::holds_alternative<cb::JsonArray>(sym_fallback.arena.nodes[fb_sym_arr]));
        CHECK(std::get<cb::JsonArray>(sym_fallback.arena.nodes[fb_sym_arr]).elements.size() == 2);

        // Msg 15: hover on 'hero' while main.cb has syntax error on next line (id = 12) -> falls back to last-known-good snapshot!
        const auto hover_fallback = cb::parse_json(cb::read_jsonrpc_message(server_to_client).value());
        REQUIRE(hover_fallback.ok);
        const cb::JsonId h12_contents = cb::json_get_field(
            hover_fallback.arena,
            cb::json_get_field(hover_fallback.arena, hover_fallback.root, "result"),
            "contents"
        );
        const auto h12_val = cb::json_get_string(hover_fallback.arena, cb::json_get_field(hover_fallback.arena, h12_contents, "value"));
        REQUIRE(h12_val.has_value());
        CHECK(h12_val.value().find("dim hero as Player") != std::string::npos);
    }
}

TEST_CASE("CleanBasic Language Server Phase 3: completion (dot UFCS/fields/modules/err/list, named args, types, scope) & signatureHelp", "[lsp]") {
    auto has_completion_item = [](
        const cb::JsonArena& arena,
        cb::JsonId arr_id,
        std::string_view label,
        int64_t expected_kind = -1,
        std::string_view detail_substr = ""
    ) -> bool {
        if (arr_id >= arena.nodes.size() || !std::holds_alternative<cb::JsonArray>(arena.nodes[arr_id])) {
            return false;
        }
        const auto& elems = std::get<cb::JsonArray>(arena.nodes[arr_id]).elements;
        for (const cb::JsonId item_id : elems) {
            const auto lbl = cb::json_get_string(arena, cb::json_get_field(arena, item_id, "label"));
            if (!lbl.has_value() || lbl.value() != label) {
                continue;
            }
            if (expected_kind >= 0) {
                const auto k = cb::json_get_int(arena, cb::json_get_field(arena, item_id, "kind"));
                if (!k.has_value() || k.value() != expected_kind) {
                    continue;
                }
            }
            if (!detail_substr.empty()) {
                const auto det = cb::json_get_string(arena, cb::json_get_field(arena, item_id, "detail"));
                if (!det.has_value() || det.value().find(detail_substr) == std::string::npos) {
                    continue;
                }
            }
            return true;
        }
        return false;
    };

    SECTION("Two-stage dot-scrub recovery on brand-new variable, UFCS & struct fields, module exports, err/list, named args, types, and signatureHelp") {
        std::stringstream client_to_server;
        std::stringstream server_to_client;

        // 1. initialize (id = 1)
        cb::write_jsonrpc_message(
            client_to_server,
            R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":"file:///ws"}})"
        );

        // 2. didOpen player.cb
        {
            cb::JsonArena arena;
            const cb::JsonId msg = cb::json_add_object(arena);
            cb::json_object_set(arena, msg, "jsonrpc", cb::json_add_string(arena, "2.0"));
            cb::json_object_set(arena, msg, "method", cb::json_add_string(arena, "textDocument/didOpen"));
            const cb::JsonId params = cb::json_add_object(arena);
            const cb::JsonId td = cb::json_add_object(arena);
            cb::json_object_set(arena, td, "uri", cb::json_add_string(arena, "file:///ws/player.cb"));
            cb::json_object_set(arena, td, "languageId", cb::json_add_string(arena, "cleanbasic"));
            cb::json_object_set(arena, td, "version", cb::json_add_int(arena, 1));
            cb::json_object_set(arena, td, "text", cb::json_add_string(arena, R"(pub alias Score = int
pub type Player
    name as str
    health as int
end type
pub fn heal(byref p as Player, amount as int, bonus as int) as int can error
    p.health = p.health + amount + bonus
    return p.health
end fn
fn secret_helper(p as Player) as int
    return p.health
end fn
)"));
            cb::json_object_set(arena, params, "textDocument", td);
            cb::json_object_set(arena, msg, "params", params);
            cb::write_jsonrpc_message(client_to_server, cb::serialize_json(arena, msg));
        }

        // 3. didOpen main.cb directly with a brand-new variable 'hero' and an incomplete trailing dot 'hero.' on the very next line!
        // Note: main.cb has NO prior cached snapshot when 'hero.' is typed!
        // Line 0: import "player.cb"
        // Line 1: fn local_buff(byref p as player.Player, shield as int)
        // Line 2:     p.health = p.health + shield
        // Line 3: end fn
        // Line 4: fn main() as int
        // Line 5:     on error e
        // Line 6:         return 1
        // Line 7:     end error
        // Line 8:     dim nums as list[int] = [1, 2]
        // Line 9:     dim hero = player.Player(name="Alice", health=100)
        // Line 10:    hero.
        // Line 11:    return 0
        // Line 12: end fn
        {
            cb::JsonArena arena;
            const cb::JsonId msg = cb::json_add_object(arena);
            cb::json_object_set(arena, msg, "jsonrpc", cb::json_add_string(arena, "2.0"));
            cb::json_object_set(arena, msg, "method", cb::json_add_string(arena, "textDocument/didOpen"));
            const cb::JsonId params = cb::json_add_object(arena);
            const cb::JsonId td = cb::json_add_object(arena);
            cb::json_object_set(arena, td, "uri", cb::json_add_string(arena, "file:///ws/main.cb"));
            cb::json_object_set(arena, td, "languageId", cb::json_add_string(arena, "cleanbasic"));
            cb::json_object_set(arena, td, "version", cb::json_add_int(arena, 1));
            cb::json_object_set(arena, td, "text", cb::json_add_string(arena, R"(import "player.cb"
fn local_buff(byref p as player.Player, shield as int)
    p.health = p.health + shield
end fn
fn main() as int
    on error e
        return 1
    end error
    dim nums as list[int] = [1, 2]
    dim hero = player.Player(name="Alice", health=100)
    hero.
    return 0
end fn
)"));
            cb::json_object_set(arena, params, "textDocument", td);
            cb::json_object_set(arena, msg, "params", params);
            cb::write_jsonrpc_message(client_to_server, cb::serialize_json(arena, msg));
        }

        // Req id=2: textDocument/completion at 'hero.' (line 10, character 9)
        cb::write_jsonrpc_message(
            client_to_server,
            R"({"jsonrpc":"2.0","id":2,"method":"textDocument/completion","params":{"textDocument":{"uri":"file:///ws/main.cb"},"position":{"line":10,"character":9}}})"
        );

        // 4. didChange main.cb to test module alias completion 'player.' (line 10, char 11)
        auto send_main_change = [&](int version, std::string_view new_line_10) {
            cb::JsonArena arena;
            const cb::JsonId msg = cb::json_add_object(arena);
            cb::json_object_set(arena, msg, "jsonrpc", cb::json_add_string(arena, "2.0"));
            cb::json_object_set(arena, msg, "method", cb::json_add_string(arena, "textDocument/didChange"));
            const cb::JsonId params = cb::json_add_object(arena);
            const cb::JsonId td = cb::json_add_object(arena);
            cb::json_object_set(arena, td, "uri", cb::json_add_string(arena, "file:///ws/main.cb"));
            cb::json_object_set(arena, td, "version", cb::json_add_int(arena, version));
            cb::json_object_set(arena, params, "textDocument", td);
            const cb::JsonId changes = cb::json_add_array(arena);
            const cb::JsonId change_item = cb::json_add_object(arena);
            std::string full_src = R"(import "player.cb"
fn local_buff(byref p as player.Player, shield as int)
    p.health = p.health + shield
end fn
fn main() as int
    on error e
        return 1
    end error
    dim nums as list[int] = [1, 2]
    dim hero = player.Player(name="Alice", health=100)
)";
            full_src += std::string(new_line_10) + "\n    return 0\nend fn\n";
            cb::json_object_set(arena, change_item, "text", cb::json_add_string(arena, full_src));
            cb::json_array_push(arena, changes, change_item);
            cb::json_object_set(arena, params, "contentChanges", changes);
            cb::json_object_set(arena, msg, "params", params);
            cb::write_jsonrpc_message(client_to_server, cb::serialize_json(arena, msg));
        };

        // Change v2: '    player.' (line 10, char 11) -> Req id=3
        send_main_change(2, "    player.");
        cb::write_jsonrpc_message(
            client_to_server,
            R"({"jsonrpc":"2.0","id":3,"method":"textDocument/completion","params":{"textDocument":{"uri":"file:///ws/main.cb"},"position":{"line":10,"character":11}}})"
        );

        // Change v3: '    e.' (line 10, char 6) -> Req id=4 (built-in err fields .msg and .code)
        send_main_change(3, "    e.");
        cb::write_jsonrpc_message(
            client_to_server,
            R"({"jsonrpc":"2.0","id":4,"method":"textDocument/completion","params":{"textDocument":{"uri":"file:///ws/main.cb"},"position":{"line":10,"character":6}}})"
        );

        // Change v4: '    nums.' (line 10, char 9) -> Req id=5 (built-in list[int] methods .push and .append)
        send_main_change(4, "    nums.");
        cb::write_jsonrpc_message(
            client_to_server,
            R"({"jsonrpc":"2.0","id":5,"method":"textDocument/completion","params":{"textDocument":{"uri":"file:///ws/main.cb"},"position":{"line":10,"character":9}}})"
        );

        // Change v5: '    dim p2 = player.Player(' (line 10, char 27) -> Req id=6 (named field= completion) & Req id=7 (signatureHelp on struct constructor)
        send_main_change(5, "    dim p2 = player.Player(");
        cb::write_jsonrpc_message(
            client_to_server,
            R"({"jsonrpc":"2.0","id":6,"method":"textDocument/completion","params":{"textDocument":{"uri":"file:///ws/main.cb"},"position":{"line":10,"character":27}}})"
        );
        cb::write_jsonrpc_message(
            client_to_server,
            R"({"jsonrpc":"2.0","id":7,"method":"textDocument/signatureHelp","params":{"textDocument":{"uri":"file:///ws/main.cb"},"position":{"line":10,"character":27}}})"
        );

        // Change v6: '    hero.heal(25, ' (line 10, char 18) -> Req id=8 (UFCS signatureHelp at 2nd visible parameter: activeParameter == 1)
        send_main_change(6, "    hero.heal(25, ");
        cb::write_jsonrpc_message(
            client_to_server,
            R"({"jsonrpc":"2.0","id":8,"method":"textDocument/signatureHelp","params":{"textDocument":{"uri":"file:///ws/main.cb"},"position":{"line":10,"character":18}}})"
        );

        // Change v7: '    dim x as ' (line 10, char 13) -> Req id=9 (type annotation completion after 'as ')
        send_main_change(7, "    dim x as ");
        cb::write_jsonrpc_message(
            client_to_server,
            R"({"jsonrpc":"2.0","id":9,"method":"textDocument/completion","params":{"textDocument":{"uri":"file:///ws/main.cb"},"position":{"line":10,"character":13}}})"
        );

        // Change v8: '    he' (line 10, char 6) -> Req id=10 (general scope completion filtered by prefix 'he' -> finds 'hero')
        send_main_change(8, "    he");
        cb::write_jsonrpc_message(
            client_to_server,
            R"({"jsonrpc":"2.0","id":10,"method":"textDocument/completion","params":{"textDocument":{"uri":"file:///ws/main.cb"},"position":{"line":10,"character":6}}})"
        );

        // shutdown & exit
        cb::write_jsonrpc_message(client_to_server, R"({"jsonrpc":"2.0","id":99,"method":"shutdown","params":null})");
        cb::write_jsonrpc_message(client_to_server, R"({"jsonrpc":"2.0","method":"exit"})");

        const int exit_code = cb::run_lsp_server(client_to_server, server_to_client);
        CHECK(exit_code == 0);

        // Helper to read next response with a specific id (skipping publishDiagnostics notifications)
        auto read_response_for_id = [&](int64_t target_id) -> cb::JsonParseResult {
            while (true) {
                const auto raw_opt = cb::read_jsonrpc_message(server_to_client);
                REQUIRE(raw_opt.has_value());
                auto parsed = cb::parse_json(raw_opt.value());
                REQUIRE(parsed.ok);
                const auto id_opt = cb::json_get_int(parsed.arena, cb::json_get_field(parsed.arena, parsed.root, "id"));
                if (id_opt.has_value() && id_opt.value() == target_id) {
                    return parsed;
                }
            }
        };

        // 1. Verify initialize capabilities (id = 1)
        const auto init_resp = read_response_for_id(1);
        const cb::JsonId init_res = cb::json_get_field(init_resp.arena, init_resp.root, "result");
        const cb::JsonId caps = cb::json_get_field(init_resp.arena, init_res, "capabilities");
        const cb::JsonId comp_prov = cb::json_get_field(init_resp.arena, caps, "completionProvider");
        REQUIRE(comp_prov != cb::JSON_INVALID_ID);
        const cb::JsonId sig_prov = cb::json_get_field(init_resp.arena, caps, "signatureHelpProvider");
        REQUIRE(sig_prov != cb::JSON_INVALID_ID);
        const cb::JsonId srv_info = cb::json_get_field(init_resp.arena, init_res, "serverInfo");
        CHECK(cb::json_get_string(init_resp.arena, cb::json_get_field(init_resp.arena, srv_info, "version")) == "0.3.0");

        // 2. Verify dot-completion on brand-new variable 'hero.' (id = 2)
        // Must include struct fields ('name', 'health'), cross-module UFCS ('heal'), and same-module UFCS ('local_buff'),
        // while excluding private cross-module 'secret_helper'.
        const auto comp_hero = read_response_for_id(2);
        const cb::JsonId hero_items = cb::json_get_field(comp_hero.arena, comp_hero.root, "result");
        CHECK(has_completion_item(comp_hero.arena, hero_items, "name", 5, "as str"));
        CHECK(has_completion_item(comp_hero.arena, hero_items, "health", 5, "as int"));
        CHECK(has_completion_item(comp_hero.arena, hero_items, "heal", 2, "ufcs fn heal(amount as int, bonus as int) as int can error"));
        CHECK(has_completion_item(comp_hero.arena, hero_items, "local_buff", 2, "ufcs fn local_buff(shield as int)"));
        CHECK_FALSE(has_completion_item(comp_hero.arena, hero_items, "secret_helper"));

        // 3. Verify module alias dot-completion 'player.' (id = 3)
        const auto comp_mod = read_response_for_id(3);
        const cb::JsonId mod_items = cb::json_get_field(comp_mod.arena, comp_mod.root, "result");
        CHECK(has_completion_item(comp_mod.arena, mod_items, "Player", 22, "type Player"));
        CHECK(has_completion_item(comp_mod.arena, mod_items, "Score", 25, "alias Score = int"));
        CHECK(has_completion_item(comp_mod.arena, mod_items, "heal", 3));
        CHECK_FALSE(has_completion_item(comp_mod.arena, mod_items, "secret_helper"));

        // 4. Verify built-in err dot-completion 'e.' (id = 4)
        const auto comp_err = read_response_for_id(4);
        const cb::JsonId err_items = cb::json_get_field(comp_err.arena, comp_err.root, "result");
        CHECK(has_completion_item(comp_err.arena, err_items, "msg", 5, "as str"));
        CHECK(has_completion_item(comp_err.arena, err_items, "code", 5, "as int"));

        // 5. Verify built-in list[int] dot-completion 'nums.' (id = 5)
        const auto comp_list = read_response_for_id(5);
        const cb::JsonId list_items = cb::json_get_field(comp_list.arena, comp_list.root, "result");
        CHECK(has_completion_item(comp_list.arena, list_items, "push", 2, "fn(item as int)"));
        CHECK(has_completion_item(comp_list.arena, list_items, "append", 2, "fn(item as int)"));

        // 6. Verify named field= completion inside 'player.Player(' (id = 6)
        const auto comp_named = read_response_for_id(6);
        const cb::JsonId named_items = cb::json_get_field(comp_named.arena, comp_named.root, "result");
        CHECK(has_completion_item(comp_named.arena, named_items, "name=", 5, "as str"));
        CHECK(has_completion_item(comp_named.arena, named_items, "health=", 5, "as int"));

        // 7. Verify signatureHelp inside 'player.Player(' (id = 7)
        const auto sig_ctor = read_response_for_id(7);
        const cb::JsonId sig_ctor_res = cb::json_get_field(sig_ctor.arena, sig_ctor.root, "result");
        CHECK(cb::json_get_int(sig_ctor.arena, cb::json_get_field(sig_ctor.arena, sig_ctor_res, "activeParameter")) == 0);
        const cb::JsonId ctor_sigs = cb::json_get_field(sig_ctor.arena, sig_ctor_res, "signatures");
        REQUIRE(std::holds_alternative<cb::JsonArray>(sig_ctor.arena.nodes[ctor_sigs]));
        const cb::JsonId ctor_sig0 = std::get<cb::JsonArray>(sig_ctor.arena.nodes[ctor_sigs]).elements[0];
        CHECK(cb::json_get_string(sig_ctor.arena, cb::json_get_field(sig_ctor.arena, ctor_sig0, "label")) ==
              "Player(name as str, health as int) as Player");

        // 8. Verify UFCS signatureHelp inside 'hero.heal(25, ' (id = 8)
        // Receiver 'byref p as Player' is omitted from visible UFCS parameters, and activeParameter is 1 ('bonus as int')
        const auto sig_ufcs = read_response_for_id(8);
        const cb::JsonId sig_ufcs_res = cb::json_get_field(sig_ufcs.arena, sig_ufcs.root, "result");
        CHECK(cb::json_get_int(sig_ufcs.arena, cb::json_get_field(sig_ufcs.arena, sig_ufcs_res, "activeParameter")) == 1);
        const cb::JsonId ufcs_sigs = cb::json_get_field(sig_ufcs.arena, sig_ufcs_res, "signatures");
        REQUIRE(std::holds_alternative<cb::JsonArray>(sig_ufcs.arena.nodes[ufcs_sigs]));
        const cb::JsonId ufcs_sig0 = std::get<cb::JsonArray>(sig_ufcs.arena.nodes[ufcs_sigs]).elements[0];
        CHECK(cb::json_get_string(sig_ufcs.arena, cb::json_get_field(sig_ufcs.arena, ufcs_sig0, "label")) ==
              "fn heal(amount as int, bonus as int) as int can error");
        const cb::JsonId ufcs_params = cb::json_get_field(sig_ufcs.arena, ufcs_sig0, "parameters");
        REQUIRE(std::holds_alternative<cb::JsonArray>(sig_ufcs.arena.nodes[ufcs_params]));
        const auto& ufcs_param_elems = std::get<cb::JsonArray>(sig_ufcs.arena.nodes[ufcs_params]).elements;
        REQUIRE(ufcs_param_elems.size() == 2);
        CHECK(cb::json_get_string(sig_ufcs.arena, cb::json_get_field(sig_ufcs.arena, ufcs_param_elems[0], "label")) == "amount as int");
        CHECK(cb::json_get_string(sig_ufcs.arena, cb::json_get_field(sig_ufcs.arena, ufcs_param_elems[1], "label")) == "bonus as int");

        // 9. Verify type annotation completion after 'dim x as ' (id = 9)
        const auto comp_types = read_response_for_id(9);
        const cb::JsonId type_items = cb::json_get_field(comp_types.arena, comp_types.root, "result");
        CHECK(has_completion_item(comp_types.arena, type_items, "int", 14));
        CHECK(has_completion_item(comp_types.arena, type_items, "str", 14));
        CHECK(has_completion_item(comp_types.arena, type_items, "list", 14));
        CHECK(has_completion_item(comp_types.arena, type_items, "player", 9));

        // 10. Verify general scope completion with prefix 'he' (id = 10)
        const auto comp_scope = read_response_for_id(10);
        const cb::JsonId scope_items = cb::json_get_field(comp_scope.arena, comp_scope.root, "result");
        CHECK(has_completion_item(comp_scope.arena, scope_items, "hero", 6, "as Player"));
    }
}

TEST_CASE("VS Code extension JSON manifests, TextMate grammar, and snippets are valid", "[vscode][lsp]") {
    SECTION("editors/vscode/package.json manifest and contributes schema") {
        const std::string raw = cb::test_e2e::read_file_text("editors/vscode/package.json");
        REQUIRE(!raw.empty());
        const cb::JsonParseResult parsed = cb::parse_json(raw);
        REQUIRE(parsed.ok);
        CHECK(cb::json_get_string(parsed.arena, cb::json_get_field(parsed.arena, parsed.root, "name")) == "cleanbasic");
        CHECK(cb::json_get_string(parsed.arena, cb::json_get_field(parsed.arena, parsed.root, "main")) == "./extension.js");

        const cb::JsonId contributes = cb::json_get_field(parsed.arena, parsed.root, "contributes");
        REQUIRE(contributes != cb::INVALID_ID);

        const cb::JsonId langs = cb::json_get_field(parsed.arena, contributes, "languages");
        REQUIRE(std::holds_alternative<cb::JsonArray>(parsed.arena.nodes[langs]));
        const cb::JsonId lang0 = std::get<cb::JsonArray>(parsed.arena.nodes[langs]).elements[0];
        CHECK(cb::json_get_string(parsed.arena, cb::json_get_field(parsed.arena, lang0, "id")) == "cleanbasic");

        const cb::JsonId grammars = cb::json_get_field(parsed.arena, contributes, "grammars");
        REQUIRE(std::holds_alternative<cb::JsonArray>(parsed.arena.nodes[grammars]));
        const cb::JsonId gram0 = std::get<cb::JsonArray>(parsed.arena.nodes[grammars]).elements[0];
        CHECK(cb::json_get_string(parsed.arena, cb::json_get_field(parsed.arena, gram0, "scopeName")) == "source.cleanbasic");

        const cb::JsonId config = cb::json_get_field(parsed.arena, contributes, "configuration");
        const cb::JsonId props = cb::json_get_field(parsed.arena, config, "properties");
        CHECK(cb::json_get_field(parsed.arena, props, "cleanbasic.executablePath") != cb::INVALID_ID);
        CHECK(cb::json_get_field(parsed.arena, props, "cleanbasic.trace.server") != cb::INVALID_ID);
    }

    SECTION("editors/vscode/language-configuration.json, TextMate grammar, snippets, and .vscode/launch.json") {
        const cb::JsonParseResult lang_cfg = cb::parse_json(cb::test_e2e::read_file_text("editors/vscode/language-configuration.json"));
        REQUIRE(lang_cfg.ok);
        const cb::JsonId comments = cb::json_get_field(lang_cfg.arena, lang_cfg.root, "comments");
        CHECK(cb::json_get_string(lang_cfg.arena, cb::json_get_field(lang_cfg.arena, comments, "lineComment")) == "'");
        CHECK(cb::json_get_field(lang_cfg.arena, lang_cfg.root, "folding") != cb::INVALID_ID);
        CHECK(cb::json_get_field(lang_cfg.arena, lang_cfg.root, "indentationRules") != cb::INVALID_ID);

        const cb::JsonParseResult tm_gram = cb::parse_json(cb::test_e2e::read_file_text("editors/vscode/syntaxes/cleanbasic.tmLanguage.json"));
        REQUIRE(tm_gram.ok);
        CHECK(cb::json_get_string(tm_gram.arena, cb::json_get_field(tm_gram.arena, tm_gram.root, "scopeName")) == "source.cleanbasic");
        const cb::JsonId repo = cb::json_get_field(tm_gram.arena, tm_gram.root, "repository");
        CHECK(cb::json_get_field(tm_gram.arena, repo, "comments") != cb::INVALID_ID);
        CHECK(cb::json_get_field(tm_gram.arena, repo, "strings") != cb::INVALID_ID);
        CHECK(cb::json_get_field(tm_gram.arena, repo, "keywords") != cb::INVALID_ID);
        CHECK(cb::json_get_field(tm_gram.arena, repo, "types") != cb::INVALID_ID);
        CHECK(cb::json_get_field(tm_gram.arena, repo, "builtins") != cb::INVALID_ID);

        const cb::JsonParseResult snippets = cb::parse_json(cb::test_e2e::read_file_text("editors/vscode/snippets/cleanbasic.code-snippets"));
        REQUIRE(snippets.ok);
        CHECK(cb::json_get_field(snippets.arena, snippets.root, "Function Declaration") != cb::INVALID_ID);
        CHECK(cb::json_get_field(snippets.arena, snippets.root, "Select Case Block") != cb::INVALID_ID);
        CHECK(cb::json_get_field(snippets.arena, snippets.root, "On Error Handler") != cb::INVALID_ID);

        const cb::JsonParseResult launch = cb::parse_json(cb::test_e2e::read_file_text(".vscode/launch.json"));
        REQUIRE(launch.ok);
        const cb::JsonId configs = cb::json_get_field(launch.arena, launch.root, "configurations");
        REQUIRE(std::holds_alternative<cb::JsonArray>(launch.arena.nodes[configs]));
        const cb::JsonId cfg0 = std::get<cb::JsonArray>(launch.arena.nodes[configs]).elements[0];
        CHECK(cb::json_get_string(launch.arena, cb::json_get_field(launch.arena, cfg0, "type")) == "extensionHost");

        const std::string ext_js = cb::test_e2e::read_file_text("editors/vscode/extension.js");
        CHECK(ext_js.find("LanguageClient") != std::string::npos);
        CHECK(ext_js.find("cleanbasic.restartServer") != std::string::npos);
        CHECK(ext_js.find("path.resolve(__dirname, '..', '..', 'build', exeName)") != std::string::npos);
        CHECK(ext_js.find("TransportKind.stdio") == std::string::npos);
    }

    SECTION("cleanbasic lsp --stdio subprocess invocation exits cleanly with code 0") {
        std::string cb_bin_path = "./build/cleanbasic";
        std::ifstream cb_bin_root("build/cleanbasic");
        if (!cb_bin_root.good()) {
            std::ifstream cb_bin_local("./cleanbasic");
            if (cb_bin_local.good()) {
                cb_bin_path = "./cleanbasic";
            }
        }

        std::ostringstream lsp_input_ss;
        cb::write_jsonrpc_message(
            lsp_input_ss,
            R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})"
        );
        cb::write_jsonrpc_message(
            lsp_input_ss,
            R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})"
        );
        cb::write_jsonrpc_message(
            lsp_input_ss,
            R"({"jsonrpc":"2.0","method":"exit"})"
        );

        const std::string tmp_in = "/tmp/cb_lsp_stdio_in.txt";
        const std::string tmp_out = "/tmp/cb_lsp_stdio_out.txt";
        {
            std::ofstream out_f(tmp_in, std::ios::binary);
            out_f << lsp_input_ss.str();
        }

        const std::string cmd = cb_bin_path + " lsp --stdio < " + tmp_in + " > " + tmp_out + " 2>&1";
        const int rc = std::system(cmd.c_str());
        CHECK(rc == 0);

        const std::string out_text = cb::test_e2e::read_file_text(tmp_out);
        CHECK(out_text.find("cleanbasic-lsp") != std::string::npos);

        std::error_code ec;
        std::filesystem::remove(tmp_in, ec);
        std::filesystem::remove(tmp_out, ec);
    }
}
