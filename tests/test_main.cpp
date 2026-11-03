#include "src/cleanbasic/ast.hpp"
#include "src/cleanbasic/diagnostics.hpp"
#include "src/cleanbasic/lexer.hpp"
#include "src/cleanbasic/parser.hpp"
#include "src/cleanbasic/analyzer.hpp"
#include "src/cleanbasic/emitter.hpp"
#include "src/cleanbasic/pipeline.hpp"

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
if a and b or not c
else if d
end if
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
    CHECK(has_kw("on error"));
    CHECK(has_kw("end error"));
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
    const cb::TypeId param1_tid = fn_decl.params[1].type_id;
    CHECK(parse_res.arena.types[param1_tid].is_shared);

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
fn risky() as int, err
    return 1, ""
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
            "Unhandled error: function returns an error but caller does not declare 'err' or use 'on error'"
        ) != std::string::npos);
    }

    SECTION("Rejects unhandled fallible call in expression statement") {
        const std::string bad_expr_stmt = R"(
fn fail() as int, err
    return 0, "error"
end fn

fn call()
    fail()
end fn
)";
        const auto res = cb::transpile_source(bad_expr_stmt, "bad_expr_stmt.cb");
        CHECK_FALSE(res.ok);
        CHECK(res.error.kind == "AnalyzerError");
        CHECK(res.error.message.find(
            "Unhandled error: function returns an error but caller does not declare 'err' or use 'on error'"
        ) != std::string::npos);
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
        REQUIRE(lex_good.ok);
        auto parse_good = cb::parse_tokens(lex_good.tokens, "good.cb");
        REQUIRE(parse_good.ok);
        const auto ana_good = cb::analyze_program(parse_good.arena, parse_good.program);
        REQUIRE(ana_good.ok);

        const auto it_risky = ana_good.symbols.functions.find("risky");
        REQUIRE(it_risky != ana_good.symbols.functions.end());
        CHECK(it_risky->second.returns_err);
        CHECK(it_risky->second.is_fallible);

        const auto it_middle = ana_good.symbols.functions.find("middle");
        REQUIRE(it_middle != ana_good.symbols.functions.end());
        CHECK(it_middle->second.returns_err);
        CHECK(it_middle->second.is_fallible);

        const auto it_main = ana_good.symbols.functions.find("main");
        REQUIRE(it_main != ana_good.symbols.functions.end());
        CHECK_FALSE(it_main->second.returns_err);
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
    REQUIRE(res.ok);

    const std::string& cpp = res.cpp_code;
    CHECK(cpp.find("template <typename F> struct _DeferTask { F f; ~_DeferTask() { f(); } };") != std::string::npos);
    CHECK(cpp.find("template <typename F> _DeferTask<F> _defer(F f) { return {f}; }") != std::string::npos);
    CHECK(cpp.find("std::tuple<int, std::string> process_data()") != std::string::npos);
    CHECK(cpp.find(R"(auto [_val1, _err1] = connect(std::string("data.db"));)") != std::string::npos);
    CHECK(cpp.find("if (!_err1.empty()) return { {}, _err1 };") != std::string::npos);
    CHECK(cpp.find("auto db = _val1;") != std::string::npos);
    CHECK(cpp.find("auto _defer_1 = _defer([&]() {") != std::string::npos);
    CHECK(cpp.find("SQL_close(db);") != std::string::npos);
    CHECK(cpp.find(R"(return { 100, std::string("") };)") != std::string::npos);
    CHECK(cpp.find("auto [_val2, e] = process_data();") != std::string::npos);
    CHECK(cpp.find("if (!e.empty()) {") != std::string::npos);
    CHECK(cpp.find("auto records = _val2;") != std::string::npos);
}

TEST_CASE("End-to-end fixture: multi-file modules, mutability, and UFCS", "[e2e]") {
    cb::test_e2e::run_e2e_fixture("modules_ufcs", "tests/cases/modules_ufcs/main.cb");
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
