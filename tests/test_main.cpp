#include "src/cleanbasic/ast.hpp"
#include "src/cleanbasic/diagnostics.hpp"
#include "src/cleanbasic/lexer.hpp"
#include "src/cleanbasic/parser.hpp"
#include "src/cleanbasic/analyzer.hpp"
#include "src/cleanbasic/emitter.hpp"
#include "src/cleanbasic/pipeline.hpp"
#include "tests/test_harness.hpp"

#include <cstdio>
#include <fstream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace cb::test_suite {

using cb::test::SuiteSummary;
using cb::test::TestCase;
using cb::test::TestCaseSpec;
using cb::test::TestContext;
using cb::test::TranspileOutcome;

inline void test_basic_assertions(TestContext& ctx) {
    const int a = 10 + 11;
    const int b = 21;
    CB_CHECK(ctx, a == 21);
    CB_CHECK_MSG(ctx, b > 0, "b should be positive");
    CB_CHECK_FALSE(ctx, a == 0);
    CB_CHECK_EQ(ctx, a, b);
    CB_CHECK_NE(ctx, a, 99);

    const std::string emitted = "std::vector<int> nums = {1, 2, 3};";
    CB_CHECK_CONTAINS(ctx, emitted, "std::vector<int>");
    CB_CHECK_NOT_CONTAINS(ctx, emitted, "std::list<int>");
    CB_REQUIRE(ctx, !emitted.empty());
}

inline void helper_with_require(TestContext& ctx, bool should_pass, int& post_require_counter) {
    CB_REQUIRE(ctx, should_pass);
    post_require_counter += 1;
}

inline void test_failure_recording_and_require_early_return(TestContext& ctx) {
    TestContext dummy_ctx{};
    dummy_ctx.current_test_name = "dummy::failure_verification";

    CB_CHECK(dummy_ctx, false);
    CB_CHECK_EQ(ctx, dummy_ctx.failures.size(), static_cast<std::size_t>(1));
    CB_CHECK_EQ(ctx, dummy_ctx.assertions_run, static_cast<uint32_t>(1));

    CB_CHECK_FALSE(dummy_ctx, true);
    CB_CHECK_EQ(ctx, dummy_ctx.failures.size(), static_cast<std::size_t>(2));

    CB_CHECK_EQ(dummy_ctx, 42, 43);
    CB_CHECK_EQ(ctx, dummy_ctx.failures.size(), static_cast<std::size_t>(3));

    CB_CHECK_NE(dummy_ctx, 7, 7);
    CB_CHECK_EQ(ctx, dummy_ctx.failures.size(), static_cast<std::size_t>(4));

    CB_CHECK_CONTAINS(dummy_ctx, "hello world", "missing_token");
    CB_CHECK_EQ(ctx, dummy_ctx.failures.size(), static_cast<std::size_t>(5));

    CB_CHECK_NOT_CONTAINS(dummy_ctx, "hello world", "world");
    CB_CHECK_EQ(ctx, dummy_ctx.failures.size(), static_cast<std::size_t>(6));

    int counter = 0;
    helper_with_require(dummy_ctx, false, counter);
    CB_CHECK(ctx, dummy_ctx.abort_current_test);
    CB_CHECK_EQ(ctx, counter, 0);
    CB_CHECK_EQ(ctx, dummy_ctx.failures.size(), static_cast<std::size_t>(7));
}

inline TranspileOutcome mock_transpile_pipeline(std::string_view source) {
    TranspileOutcome outcome{};
    if (source.find("syntax_error") != std::string_view::npos) {
        outcome.ok = false;
        outcome.output_or_error = "ParseError: unexpected token near syntax_error";
        return outcome;
    }
    outcome.ok = true;
    std::string generated = R"(// Generated C++17
)";
    if (source.find("list[int]") != std::string_view::npos) {
        generated += R"(std::vector<int> items;
)";
    }
    if (source.find("fn main()") != std::string_view::npos) {
        generated += R"(int main() { return 0; }
)";
    }
    outcome.output_or_error = generated;
    return outcome;
}

inline void test_table_driven_runner(TestContext& ctx) {
    std::vector<TestCaseSpec> specs;

    TestCaseSpec valid_spec{};
    valid_spec.name = "transpile_list_and_main";
    valid_spec.input_source = R"(fn main()
    dim items as list[int] = [1, 2]
end fn
)";
    valid_spec.expected_substrings = {"std::vector<int> items;", "int main()"};
    valid_spec.forbidden_substrings = {"std::list", "throw"};
    valid_spec.expect_error = false;
    specs.push_back(valid_spec);

    TestCaseSpec error_spec{};
    error_spec.name = "reject_syntax_error";
    error_spec.input_source = R"(fn broken()
    syntax_error
end fn
)";
    error_spec.expect_error = true;
    error_spec.expected_error_substring = "ParseError";
    specs.push_back(error_spec);

    cb::test::run_table_tests(ctx, specs, mock_transpile_pipeline);
}

inline void test_suite_filtering(TestContext& ctx) {
    std::vector<TestCase> sample_cases;

    TestCase c1{};
    c1.suite = "lexer";
    c1.name = "tokenize_numbers";
    c1.run = [](TestContext& inner_ctx) {
        CB_CHECK_EQ(inner_ctx, 1, 1);
    };
    sample_cases.push_back(c1);

    TestCase c2{};
    c2.suite = "parser";
    c2.name = "parse_fn";
    c2.run = [](TestContext& inner_ctx) {
        CB_CHECK_EQ(inner_ctx, 2, 2);
    };
    sample_cases.push_back(c2);

    const SuiteSummary filtered = cb::test::run_all_tests(sample_cases, "lexer");
    CB_CHECK_EQ(ctx, filtered.total_tests, static_cast<uint32_t>(1));
    CB_CHECK_EQ(ctx, filtered.passed_tests, static_cast<uint32_t>(1));
    CB_CHECK_EQ(ctx, filtered.failed_tests, static_cast<uint32_t>(0));
    CB_CHECK_EQ(ctx, cb::test::exit_code_from_summary(filtered), 0);
}

inline void test_lexer_multi_word_and_c_operators(TestContext& ctx) {
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
if a and b or not c
else if d
end if
on error e
end error
defer
end defer
)";

    const auto lex_res = cb::tokenize(code, "lex_test.cb");
    CB_CHECK(ctx, lex_res.ok);
    CB_REQUIRE(ctx, lex_res.ok);

    std::vector<std::string> kw_values;
    for (const auto& tok : lex_res.tokens) {
        if (tok.kind == "KEYWORD") {
            kw_values.push_back(tok.value);
        }
    }

    auto has_kw = [&](std::string_view expected) {
        for (const auto& val : kw_values) {
            if (val == expected) return true;
        }
        return false;
    };

    CB_CHECK(ctx, has_kw("import"));
    CB_CHECK(ctx, has_kw("as"));
    CB_CHECK(ctx, has_kw("dim"));
    CB_CHECK(ctx, has_kw("const"));
    CB_CHECK(ctx, has_kw("import header"));
    CB_CHECK(ctx, has_kw("import link"));
    CB_CHECK(ctx, has_kw("select case"));
    CB_CHECK(ctx, has_kw("case"));
    CB_CHECK(ctx, has_kw("case else"));
    CB_CHECK(ctx, has_kw("end select"));
    CB_CHECK(ctx, has_kw("and"));
    CB_CHECK(ctx, has_kw("or"));
    CB_CHECK(ctx, has_kw("not"));
    CB_CHECK(ctx, has_kw("else if"));
    CB_CHECK(ctx, has_kw("end if"));
    CB_CHECK(ctx, has_kw("on error"));
    CB_CHECK(ctx, has_kw("end error"));
    CB_CHECK(ctx, has_kw("defer"));
    CB_CHECK(ctx, has_kw("end defer"));

    const std::vector<std::string> forbidden = {"a && b", "a || b", "!flag"};
    for (const auto& bad_op : forbidden) {
        const auto bad_lex = cb::tokenize(bad_op, "bad_op.cb");
        CB_CHECK_FALSE(ctx, bad_lex.ok);
        CB_CHECK_EQ(ctx, bad_lex.error.kind, "LexerError");
    }
}

inline void test_parser_control_flow_and_types(TestContext& ctx) {
    const std::string code = R"(
pub alias ScoreMap = dict[str, list[int]]

fn compute(byref items as list[int], tag as shared str) as int
    dim total = 0
    for i in 0..10 step 2
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
    CB_CHECK(ctx, lex_res.ok);
    CB_REQUIRE(ctx, lex_res.ok);

    const auto parse_res = cb::parse_tokens(lex_res.tokens, "test.cb");
    CB_CHECK(ctx, parse_res.ok);
    CB_REQUIRE(ctx, parse_res.ok);

    CB_CHECK_EQ(ctx, parse_res.program.declarations.size(), static_cast<size_t>(2));
    const DeclId fn_decl_id = parse_res.program.declarations[1];
    CB_CHECK(ctx, fn_decl_id < parse_res.arena.decls.size());
    const auto& decl_node = parse_res.arena.decls[fn_decl_id];
    CB_CHECK(ctx, std::holds_alternative<cb::FnDecl>(decl_node.payload));
    const auto& fn_decl = std::get<cb::FnDecl>(decl_node.payload);
    CB_CHECK_EQ(ctx, fn_decl.name, "compute");
    CB_CHECK_EQ(ctx, fn_decl.params.size(), static_cast<size_t>(2));
    CB_CHECK(ctx, fn_decl.params[0].is_byref);
    const TypeId param1_tid = fn_decl.params[1].type_id;
    CB_CHECK(ctx, parse_res.arena.types[param1_tid].is_shared);

    const std::string bad_code = R"(fn broken()
    if true
        dim x = 1
end fn
)";
    const auto bad_lex = cb::tokenize(bad_code, "bad.cb");
    CB_CHECK(ctx, bad_lex.ok);
    const auto bad_parse = cb::parse_tokens(bad_lex.tokens, "bad.cb");
    CB_CHECK_FALSE(ctx, bad_parse.ok);
    CB_CHECK_EQ(ctx, bad_parse.error.kind, "ParseError");
}

inline void test_analyzer(TestContext& ctx) {
    const std::string bad_fallible = R"(
fn risky() as int, err
    return 1, ""
end fn

fn caller_without_err()
    dim x = risky()
end fn
)";

    const auto lex_bad = cb::tokenize(bad_fallible, "bad_fallible.cb");
    CB_CHECK(ctx, lex_bad.ok);
    auto parse_bad = cb::parse_tokens(lex_bad.tokens, "bad_fallible.cb");
    CB_CHECK(ctx, parse_bad.ok);
    const auto ana_bad = cb::analyze_program(parse_bad.arena, parse_bad.program);
    CB_CHECK_FALSE(ctx, ana_bad.ok);
    CB_CHECK_EQ(ctx, ana_bad.error.kind, "AnalyzerError");
    CB_CHECK_CONTAINS(
        ctx,
        ana_bad.error.message,
        "Unhandled error: function returns an error but caller does not declare 'err' or use 'on error'"
    );

    const std::string bad_const = R"(
fn main()
    const limit as int = 10
    limit = 20
end fn
)";
    const auto res_bad_const = cb::transpile_source(bad_const, "bad_const.cb");
    CB_CHECK_FALSE(ctx, res_bad_const.ok);
    CB_CHECK_EQ(ctx, res_bad_const.error.kind, "AnalyzerError");
    CB_CHECK_CONTAINS(ctx, res_bad_const.error.message, "Cannot reassign const variable");

    const std::string good_code = R"(
fn risky() as int, err
    return 1, ""
end fn

fn middle() as int, err
    dim val = risky()
    return val, ""
end fn

fn main()
    dim res = middle() on error e
        print(e)
        return
    end error
end fn
)";

    const auto lex_good = cb::tokenize(good_code, "good.cb");
    CB_CHECK(ctx, lex_good.ok);
    auto parse_good = cb::parse_tokens(lex_good.tokens, "good.cb");
    CB_CHECK(ctx, parse_good.ok);
    const auto ana_good = cb::analyze_program(parse_good.arena, parse_good.program);
    CB_CHECK(ctx, ana_good.ok);
    const auto it_risky = ana_good.symbols.functions.find("risky");
    CB_CHECK(ctx, it_risky != ana_good.symbols.functions.end());
    if (it_risky != ana_good.symbols.functions.end()) {
        CB_CHECK(ctx, it_risky->second.returns_err);
        CB_CHECK(ctx, it_risky->second.is_fallible);
    }
    const auto it_middle = ana_good.symbols.functions.find("middle");
    CB_CHECK(ctx, it_middle != ana_good.symbols.functions.end());
    if (it_middle != ana_good.symbols.functions.end()) {
        CB_CHECK(ctx, it_middle->second.returns_err);
        CB_CHECK(ctx, it_middle->second.is_fallible);
    }
    const auto it_main = ana_good.symbols.functions.find("main");
    CB_CHECK(ctx, it_main != ana_good.symbols.functions.end());
    if (it_main != ana_good.symbols.functions.end()) {
        CB_CHECK_FALSE(ctx, it_main->second.returns_err);
        CB_CHECK_FALSE(ctx, it_main->second.is_fallible);
    }

    // Verify implicit_bubble was set on 'dim val = risky()' inside 'middle'
    const DeclId middle_decl_id = parse_good.program.declarations[1];
    const auto& middle_fn = std::get<cb::FnDecl>(parse_good.arena.decls[middle_decl_id].payload);
    CB_REQUIRE(ctx, !middle_fn.body.empty());
    const StmtId val_stmt_id = middle_fn.body[0];
    const auto& val_assign = std::get<cb::VarAssignment>(parse_good.arena.stmts[val_stmt_id].payload);
    CB_CHECK(ctx, val_assign.implicit_bubble);
}

inline void test_prompt_section_5_error_bubbling_and_defer(TestContext& ctx) {
    const std::string code = R"(
pub extern type SQL_DB
pub extern fn connect(path as str) as SQL_DB, err
pub extern fn SQL_close(db as SQL_DB)

fn process_data() as int, err
    ' Implicit bubble (needs if (!err.empty()) return ...)
    dim db = connect("data.db")
    
    defer
        SQL_close(db)
    end defer
    
    return 100, ""
end fn

fn main()
    ' Explicit handling
    dim records = process_data() on error e
        print("Crash prevented: " & e)
        return
    end error
end fn
)";

    const auto res = cb::transpile_source(code, "section5.cb");
    CB_CHECK(ctx, res.ok);
    CB_REQUIRE(ctx, res.ok);

    const std::string& cpp = res.cpp_code;
    CB_CHECK_CONTAINS(ctx, cpp, "template <typename F> struct _DeferTask { F f; ~_DeferTask() { f(); } };");
    CB_CHECK_CONTAINS(ctx, cpp, "template <typename F> _DeferTask<F> _defer(F f) { return {f}; }");
    CB_CHECK_CONTAINS(ctx, cpp, "std::tuple<int, std::string> process_data()");
    CB_CHECK_CONTAINS(ctx, cpp, R"(auto [_val1, _err1] = connect(std::string("data.db"));)");
    CB_CHECK_CONTAINS(ctx, cpp, "if (!_err1.empty()) return { {}, _err1 };");
    CB_CHECK_CONTAINS(ctx, cpp, "auto db = _val1;");
    CB_CHECK_CONTAINS(ctx, cpp, "auto _defer_1 = _defer([&]() {");
    CB_CHECK_CONTAINS(ctx, cpp, "SQL_close(db);");
    CB_CHECK_CONTAINS(ctx, cpp, R"(return { 100, std::string("") };)");
    CB_CHECK_CONTAINS(ctx, cpp, "auto [_val2, e] = process_data();");
    CB_CHECK_CONTAINS(ctx, cpp, "if (!e.empty()) {");
    CB_CHECK_CONTAINS(ctx, cpp, "auto records = _val2;");
}

inline void test_emitter_spec_modules_mutability_and_ufcs(TestContext& ctx) {
    const std::string code = R"(
' Expose the type to other files
pub type Player
    name as str
    health as int
end type

' Expose the function
pub fn heal(byref p as Player, amount as int)
    p.health = p.health + amount
end fn

' Private function (no 'pub')
fn calculate_max_health() as int
    return 100
end fn

import "player.cb"

fn main()
    ' IMMUTABILITY: Cannot be changed after declaration
    const max_potion as int = 50 
    
    ' MUTABILITY & INFERENCE: 'dim' allows reassignment, type is inferred
    dim uses_left = 3 
    uses_left = 2

    ' MEMORY: Stack allocation (flat, fast, zero cleanup needed)
    dim stack_hero = player.Player(name="Alice", health=10)
    
    ' MEMORY: Heap allocation (requires 'shared' and 'new')
    dim heap_hero as shared player.Player = new player.Player(name="Bob", health=10)

    ' UFCS (Unified Function Call Syntax): 
    stack_hero.heal(max_potion)
    
    ' Auto-dereference shared pointer on UFCS byref call:
    heap_hero.heal(max_potion)
end fn
)";

    const auto res = cb::transpile_source(code, "ufcs_spec.cb");
    CB_CHECK(ctx, res.ok);
    CB_REQUIRE(ctx, res.ok);

    const std::string& cpp = res.cpp_code;
    CB_CHECK_CONTAINS(ctx, cpp, "struct Player {");
    CB_CHECK_CONTAINS(ctx, cpp, "void heal(Player& p, int amount)");
    CB_CHECK_CONTAINS(ctx, cpp, "const int max_potion = 50;");
    CB_CHECK_CONTAINS(ctx, cpp, "int uses_left = 3;");
    CB_CHECK_CONTAINS(ctx, cpp, "uses_left = 2;");
    CB_CHECK_CONTAINS(ctx, cpp, R"(Player stack_hero = Player{std::string("Alice"), 10};)");
    CB_CHECK_CONTAINS(ctx, cpp, R"(std::shared_ptr<Player> heap_hero = std::make_shared<Player>(Player{std::string("Bob"), 10});)");
    CB_CHECK_CONTAINS(ctx, cpp, "heal(stack_hero, max_potion);");
    CB_CHECK_CONTAINS(ctx, cpp, "heal((*heap_hero), max_potion);");
}

inline void test_emitter_spec_network_error_bubbling_and_defer(TestContext& ctx) {
    const std::string code = R"(
pub type Connection
    id as int
end type

' Function can return a Connection OR an error
pub fn connect(ip as str) as Connection, err
    if ip == ""
        return null, "IP cannot be empty"
    end if
    
    return Connection(id=1), ""
end fn

pub fn close_connection(conn as Connection)
    ' Cleanup logic goes here
end fn

' Declares 'err', enabling implicit error bubbling
pub fn process_network() as int, err
    ' IMPLICIT BUBBLING: If connect fails, process_network immediately returns the error.
    dim conn = connect("192.168.1.1")
    
    ' DETERMINISTIC CLEANUP: Runs right before the function exits, no matter how.
    defer
        close_connection(conn)
    end defer
    
    return 100, ""
end fn

fn start_server()
    ' start_server() cannot fail. The developer MUST handle the error here.
    ' The 'on error' block keeps the logic perfectly flat against the margin.
    dim records = process_network() on error e
        print("Failed to start server: " & e)
        return
    end error
    
    print("Connected and processed " & str(records) & " records.")
end fn
)";

    const auto res = cb::transpile_source(code, "network.cb");
    CB_CHECK(ctx, res.ok);
    CB_REQUIRE(ctx, res.ok);

    const std::string& cpp = res.cpp_code;
    CB_CHECK_CONTAINS(ctx, cpp, "struct Connection {");
    CB_CHECK_CONTAINS(ctx, cpp, "std::tuple<Connection, std::string> connect(std::string ip)");
    CB_CHECK_CONTAINS(ctx, cpp, R"(return { {}, std::string("IP cannot be empty") };)");
    CB_CHECK_CONTAINS(ctx, cpp, R"(return { Connection{1}, std::string("") };)");
    CB_CHECK_CONTAINS(ctx, cpp, R"(auto [_val1, _err1] = connect(std::string("192.168.1.1"));)");
    CB_CHECK_CONTAINS(ctx, cpp, "if (!_err1.empty()) return { {}, _err1 };");
    CB_CHECK_CONTAINS(ctx, cpp, "auto conn = _val1;");
    CB_CHECK_CONTAINS(ctx, cpp, "auto _defer_1 = _defer([&]() {");
    CB_CHECK_CONTAINS(ctx, cpp, "close_connection(conn);");
    CB_CHECK_CONTAINS(ctx, cpp, "auto [_val2, e] = process_network();");
    CB_CHECK_CONTAINS(ctx, cpp, "if (!e.empty()) {");
    CB_CHECK_CONTAINS(ctx, cpp, "auto records = _val2;");
}

inline void test_emitter_stl_containers_and_select_case(TestContext& ctx) {
    const std::string code = R"(
fn test_containers(nums as list[int], lookup as dict[str, int]) as int
    select case len(nums)
    case 0
        return 0
    case else
        return nums[0]
    end select
end fn
)";

    const auto res = cb::transpile_source(code, "containers.cb");
    CB_CHECK(ctx, res.ok);
    CB_REQUIRE(ctx, res.ok);

    const std::string& cpp = res.cpp_code;
    CB_CHECK_CONTAINS(ctx, cpp, "int test_containers(std::vector<int> nums, std::unordered_map<std::string, int> lookup)");
    CB_CHECK_CONTAINS(ctx, cpp, "static_cast<int>(nums.size())");
}

inline void test_cli_build_generates_build_cpp(TestContext& ctx) {
    const std::string test_cb_path = "sample_test_cli.cb";
    const std::string test_out_path = "sample_test_build.cpp";

    std::ofstream out_cb(test_cb_path);
    out_cb << R"(import link "m"

fn main()
    dim x = 42
    print("Answer: " & str(x))
end fn
)";
    out_cb.close();

    const auto build_res = cb::build_file(test_cb_path, test_out_path, false);
    CB_CHECK(ctx, build_res.ok);
    CB_REQUIRE(ctx, build_res.ok);

    std::ifstream in_cpp(test_out_path);
    CB_CHECK(ctx, in_cpp.is_open());
    std::string content;
    if (in_cpp.is_open()) {
        char c;
        while (in_cpp.get(c)) {
            content += c;
        }
        in_cpp.close();
    }

    CB_CHECK_CONTAINS(ctx, content, "// CleanBasic Linker Flags: -lm");
    CB_CHECK_CONTAINS(ctx, content, "int main()");

    std::string cb_bin_path = "./build/cleanbasic";
    std::ifstream cb_bin_root("build/cleanbasic");
    if (!cb_bin_root.good()) {
        std::ifstream cb_bin_local("./cleanbasic");
        if (cb_bin_local.good()) {
            cb_bin_path = "./cleanbasic";
        }
    }

    std::ifstream cb_bin_check(cb_bin_path);
    CB_CHECK(ctx, cb_bin_check.good());

    const std::string cli_out_path = "sample_cli_exec_build.cpp";
    const std::string cli_cmd = cb_bin_path + " build " + test_cb_path + " -o " + cli_out_path + " > /dev/null 2>&1";
    const int cli_rc = std::system(cli_cmd.c_str());
    CB_CHECK_EQ(ctx, cli_rc, 0);

    std::ifstream cli_cpp(cli_out_path);
    CB_CHECK(ctx, cli_cpp.good());
    cli_cpp.close();

    std::remove(test_cb_path.c_str());
    std::remove(test_out_path.c_str());
    std::remove(cli_out_path.c_str());
}

inline void test_table_driven_pipeline(TestContext& ctx) {
    std::vector<TestCaseSpec> specs;

    TestCaseSpec spec_struct{};
    spec_struct.name = "struct_and_field_access";
    spec_struct.input_source = R"(
type Point
    x as int
    y as int
end type
fn main()
    dim pt = Point(x=1, y=2)
    print(str(pt.x))
end fn
)";
    spec_struct.expected_substrings = {"struct Point {", "int x", "int y", "pt.x"};
    spec_struct.expect_error = false;
    specs.push_back(spec_struct);

    TestCaseSpec spec_for{};
    spec_for.name = "for_range_step";
    spec_for.input_source = R"(
fn sum_evens() as int
    dim total = 0
    for i in 0..10 step 2
        total = total + i
    end for
    return total
end fn
)";
    spec_for.expected_substrings = {"for (int i = 0; i < 10; i += 2) {"};
    spec_for.expect_error = false;
    specs.push_back(spec_for);

    TestCaseSpec spec_err{};
    spec_err.name = "reject_unhandled_error";
    spec_err.input_source = R"(
fn fail() as int, err
    return 0, "error"
end fn
fn call()
    dim x = fail()
end fn
)";
    spec_err.expect_error = true;
    spec_err.expected_error_substring = "Unhandled error: function returns an error but caller does not declare 'err' or use 'on error'";
    specs.push_back(spec_err);

    auto trans_runner = [](std::string_view src) {
        TranspileOutcome outcome{};
        const auto res = cb::transpile_source(src);
        outcome.ok = res.ok;
        if (res.ok) {
            outcome.output_or_error = res.cpp_code;
        } else {
            outcome.output_or_error = cb::format_diagnostic(res.error);
        }
        return outcome;
    };

    cb::test::run_table_tests(ctx, specs, trans_runner);
}

}  // namespace cb::test_suite

int main() {
    std::vector<cb::test::TestCase> tests;

    cb::test::TestCase t1{};
    t1.suite = "harness";
    t1.name = "basic_assertions";
    t1.run = cb::test_suite::test_basic_assertions;
    tests.push_back(t1);

    cb::test::TestCase t2{};
    t2.suite = "harness";
    t2.name = "failure_recording_and_require";
    t2.run = cb::test_suite::test_failure_recording_and_require_early_return;
    tests.push_back(t2);

    cb::test::TestCase t3{};
    t3.suite = "harness";
    t3.name = "table_driven_runner";
    t3.run = cb::test_suite::test_table_driven_runner;
    tests.push_back(t3);

    cb::test::TestCase t4{};
    t4.suite = "harness";
    t4.name = "suite_filtering";
    t4.run = cb::test_suite::test_suite_filtering;
    tests.push_back(t4);

    cb::test::TestCase t5{};
    t5.suite = "lexer";
    t5.name = "multi_word_and_c_operators";
    t5.run = cb::test_suite::test_lexer_multi_word_and_c_operators;
    tests.push_back(t5);

    cb::test::TestCase t6{};
    t6.suite = "parser";
    t6.name = "control_flow_and_types";
    t6.run = cb::test_suite::test_parser_control_flow_and_types;
    tests.push_back(t6);

    cb::test::TestCase t7{};
    t7.suite = "semantic";
    t7.name = "analyzer_validation";
    t7.run = cb::test_suite::test_analyzer;
    tests.push_back(t7);

    cb::test::TestCase t8{};
    t8.suite = "emitter";
    t8.name = "prompt_section_5_error_bubbling_and_defer";
    t8.run = cb::test_suite::test_prompt_section_5_error_bubbling_and_defer;
    tests.push_back(t8);

    cb::test::TestCase t9{};
    t9.suite = "emitter";
    t9.name = "spec_modules_mutability_and_ufcs";
    t9.run = cb::test_suite::test_emitter_spec_modules_mutability_and_ufcs;
    tests.push_back(t9);

    cb::test::TestCase t10{};
    t10.suite = "emitter";
    t10.name = "spec_network_error_bubbling_and_defer";
    t10.run = cb::test_suite::test_emitter_spec_network_error_bubbling_and_defer;
    tests.push_back(t10);

    cb::test::TestCase t11{};
    t11.suite = "emitter";
    t11.name = "stl_containers_and_select_case";
    t11.run = cb::test_suite::test_emitter_stl_containers_and_select_case;
    tests.push_back(t11);

    cb::test::TestCase t12{};
    t12.suite = "pipeline";
    t12.name = "cli_build_generates_build_cpp";
    t12.run = cb::test_suite::test_cli_build_generates_build_cpp;
    tests.push_back(t12);

    cb::test::TestCase t13{};
    t13.suite = "pipeline";
    t13.name = "table_driven_transpile";
    t13.run = cb::test_suite::test_table_driven_pipeline;
    tests.push_back(t13);

    const cb::test::SuiteSummary summary = cb::test::run_all_tests(tests);
    return cb::test::exit_code_from_summary(summary);
}
