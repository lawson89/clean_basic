#include "src/cleanbasic/ast.hpp"
#include "src/cleanbasic/diagnostics.hpp"
#include "src/cleanbasic/lexer.hpp"
#include "src/cleanbasic/parser.hpp"
#include "src/cleanbasic/analyzer.hpp"
#include "src/cleanbasic/emitter.hpp"
#include "src/cleanbasic/pipeline.hpp"
#include "src/cleanbasic/json.hpp"
#include "src/cleanbasic/lsp.hpp"
#define CLEANBASIC_CLI_LIB_ONLY
#include "src/cleanbasic/cli_main.cpp"

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

namespace cb::testE2E {

struct FixtureExpectations {
    std::vector<std::string> expectCpp;
    std::vector<std::string> forbidCpp;
    std::vector<std::string> expectOut;
};

inline std::string readFileText(const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open()) {
        return "";
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

inline std::vector<std::string> splitLinesNormalized(const std::string& text) {
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

inline FixtureExpectations parseFixtureExpectations(const std::string& cbPath) {
    FixtureExpectations exp{};
    const std::string content = readFileText(cbPath);
    const std::vector<std::string> lines = splitLinesNormalized(content);

    const std::string cppPrefix = "' EXPECT_CPP: ";
    const std::string forbidPrefix = "' FORBID_CPP: ";
    const std::string outPrefix = "' EXPECT_OUT: ";

    for (const auto& rawLine : lines) {
        size_t firstNonSpace = 0;
        while (firstNonSpace < rawLine.size() &&
               (rawLine[firstNonSpace] == ' ' || rawLine[firstNonSpace] == '\t')) {
            firstNonSpace += 1;
        }
        const std::string line = rawLine.substr(firstNonSpace);

        if (line.rfind(cppPrefix, 0) == 0) {
            exp.expectCpp.push_back(line.substr(cppPrefix.size()));
        } else if (line.rfind(forbidPrefix, 0) == 0) {
            exp.forbidCpp.push_back(line.substr(forbidPrefix.size()));
        } else if (line.rfind(outPrefix, 0) == 0) {
            exp.expectOut.push_back(line.substr(outPrefix.size()));
        }
    }

    return exp;
}

inline void runE2EFixture(const std::string& caseName, const std::string& cbPath) {
    const std::string artifactDir = "build/testArtifacts/" + caseName;
    std::error_code ec;
    std::filesystem::create_directories(artifactDir, ec);
    REQUIRE(!ec);

    const FixtureExpectations exp = parseFixtureExpectations(cbPath);
    REQUIRE(!exp.expectCpp.empty());
    REQUIRE(!exp.expectOut.empty());

    const std::string outCppPath = artifactDir + "/build.cpp";
    const std::string binPath = artifactDir + "/app";
    const std::string stdoutPath = artifactDir + "/stdout.txt";

    const cb::BuildFileResult buildRes = cb::buildFile(cbPath, outCppPath, true, binPath);
    INFO("Build diagnostic: " << cb::formatDiagnostic(buildRes.error));
    REQUIRE(buildRes.ok);
    CHECK(std::filesystem::exists(outCppPath, ec));
    CHECK(std::filesystem::exists(binPath, ec));

    const std::string cppContent = readFileText(outCppPath);
    REQUIRE(!cppContent.empty());

    for (const auto& expectedSnippet : exp.expectCpp) {
        INFO("Expected C++ snippet missing in " << outCppPath << ": " << expectedSnippet);
        CHECK(cppContent.find(expectedSnippet) != std::string::npos);
    }

    for (const auto& forbiddenSnippet : exp.forbidCpp) {
        INFO("Forbidden C++ snippet present in " << outCppPath << ": " << forbiddenSnippet);
        CHECK(cppContent.find(forbiddenSnippet) == std::string::npos);
    }

    const std::string runCmd = "./" + binPath + " > " + stdoutPath + " 2>&1";
    const int exitCode = std::system(runCmd.c_str());
    REQUIRE(exitCode == 0);
    CHECK(std::filesystem::exists(stdoutPath, ec));

    const std::string stdoutContent = readFileText(stdoutPath);
    const std::vector<std::string> actualLines = splitLinesNormalized(stdoutContent);
    CHECK(actualLines == exp.expectOut);
}

}  // namespace cb::testE2E

TEST_CASE("Lexer tokenizes multi-word keywords and rejects C-style operators", "[lexer]") {
    const std::string code = R"(
' VB style comment
import "player.cb" as player
import header "sqliteWrapper.hpp"
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

    const auto lexRes = cb::tokenize(code, "lexTest.cb");
    REQUIRE(lexRes.ok);

    std::vector<std::string> kwValues;
    for (const auto& tok : lexRes.tokens) {
        if (tok.kind == "KEYWORD") {
            kwValues.push_back(tok.value);
        }
    }

    auto hasKw = [&](std::string_view expected) {
        for (const auto& val : kwValues) {
            if (val == expected) {
                return true;
            }
        }
        return false;
    };

    CHECK(hasKw("import"));
    CHECK(hasKw("as"));
    CHECK(hasKw("dim"));
    CHECK(hasKw("const"));
    CHECK(hasKw("import header"));
    CHECK(hasKw("import link"));
    CHECK(hasKw("select case"));
    CHECK(hasKw("case"));
    CHECK(hasKw("case else"));
    CHECK(hasKw("end select"));
    CHECK(hasKw("and"));
    CHECK(hasKw("or"));
    CHECK(hasKw("not"));
    CHECK(hasKw("else if"));
    CHECK(hasKw("end if"));
    CHECK(hasKw("can error"));
    CHECK(hasKw("on error"));
    CHECK(hasKw("end error"));
    CHECK(hasKw("for"));
    CHECK(hasKw("to"));
    CHECK(hasKw("step"));
    CHECK(hasKw("end for"));
    CHECK(hasKw("defer"));
    CHECK(hasKw("end defer"));

    SECTION("Rejects forbidden C-style logical operators") {
        const std::vector<std::string> forbidden = {"a && b", "a || b", "!flag"};
        for (const auto& badOp : forbidden) {
            const auto badLex = cb::tokenize(badOp, "badOp.cb");
            CHECK_FALSE(badLex.ok);
            CHECK(badLex.error.kind == "LexerError");
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

    const auto lexRes = cb::tokenize(code, "test.cb");
    REQUIRE(lexRes.ok);

    const auto parseRes = cb::parseTokens(lexRes.tokens, "test.cb");
    REQUIRE(parseRes.ok);

    REQUIRE(parseRes.program.declarations.size() == 2);
    const cb::DeclId fnDeclId = parseRes.program.declarations[1];
    REQUIRE(fnDeclId < parseRes.arena.decls.size());

    REQUIRE(parseRes.arena.getDecl<cb::FnDecl>(fnDeclId) != nullptr);
    const auto& fnDecl = cb::asDecl<cb::FnDecl>(parseRes.arena, fnDeclId);
    CHECK(fnDecl.name == "compute");
    REQUIRE(fnDecl.params.size() == 2);
    CHECK(fnDecl.params[0].isByref);

    SECTION("Parses classic BASIC 'for i = start to end' with inclusive RangeSpec") {
        const std::string forToCode = R"(fn loopTest() as int
    dim s = 0
    for i = 1 to 5 step 2
        s = s + i
    end for
    return s
end fn
)";
        const auto lexFt = cb::tokenize(forToCode, "forTo.cb");
        REQUIRE(lexFt.ok);
        const auto parseFt = cb::parseTokens(lexFt.tokens, "forTo.cb");
        REQUIRE(parseFt.ok);
        REQUIRE(parseFt.program.declarations.size() == 1);
        const auto& fnFt = cb::asDecl<cb::FnDecl>(parseFt.arena, parseFt.program.declarations[0]);
        REQUIRE(fnFt.body.size() >= 2);
        const auto& forStmt = cb::asStmt<cb::ForStmt>(parseFt.arena, fnFt.body[1]);
        CHECK(forStmt.rangeSpec.hasRange);
        CHECK(forStmt.rangeSpec.inclusive);
        CHECK(forStmt.rangeSpec.hasStep);
    }

    SECTION("Parses 'can error' on value-returning and void functions") {
        const std::string canErrCode = R"(
fn connect(ip as str) as int can error
    return 1
end fn

fn validate(port as int) can error
    if port <= 0
        error("bad port", code=400)
    end if
end fn
)";
        const auto lexCe = cb::tokenize(canErrCode, "canErr.cb");
        REQUIRE(lexCe.ok);
        const auto parseCe = cb::parseTokens(lexCe.tokens, "canErr.cb");
        REQUIRE(parseCe.ok);
        REQUIRE(parseCe.program.declarations.size() == 2);
        const auto& fn0 = cb::asDecl<cb::FnDecl>(parseCe.arena, parseCe.program.declarations[0]);
        const auto& fn1 = cb::asDecl<cb::FnDecl>(parseCe.arena, parseCe.program.declarations[1]);
        CHECK(fn0.isFallible);
        CHECK(fn0.returnType != cb::INVALID_ID);
        CHECK(fn1.isFallible);
        CHECK(fn1.returnType != cb::INVALID_ID);
        CHECK(parseCe.arena.types[fn1.returnType].name == "void");
        CHECK(parseCe.arena.types[fn1.returnType].name == "void");
    }

    SECTION("Rejects ':' in named arguments for function calls and struct initialization") {
        const std::string badCallColon = R"(
fn main()
    foo(code: 408)
end fn
)";
        const auto lexC1 = cb::tokenize(badCallColon, "badCallColon.cb");
        REQUIRE(lexC1.ok);
        const auto parseC1 = cb::parseTokens(lexC1.tokens, "badCallColon.cb");
        CHECK_FALSE(parseC1.ok);
        CHECK(parseC1.error.kind == "ParseError");

        const std::string badStructColon = R"(
fn main()
    dim p = new Point(x: 10, y: 20)
end fn
)";
        const auto lexC2 = cb::tokenize(badStructColon, "badStructColon.cb");
        REQUIRE(lexC2.ok);
        const auto parseC2 = cb::parseTokens(lexC2.tokens, "badStructColon.cb");
        CHECK_FALSE(parseC2.ok);
        CHECK(parseC2.error.kind == "ParseError");
    }

    SECTION("Rejects removed redundant syntax ('for i in a..b', 'Type{...}', 'new Type{...}', and trailing 'as byref')") {
        const std::vector<std::string> removedSyntaxes = {
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
        for (const auto& snippet : removedSyntaxes) {
            const auto lexRem = cb::tokenize(snippet, "removed.cb");
            REQUIRE(lexRem.ok);
            const auto parseRem = cb::parseTokens(lexRem.tokens, "removed.cb");
            CHECK_FALSE(parseRem.ok);
            CHECK(parseRem.error.kind == "ParseError");
        }
    }

    SECTION("Rejects legacy trailing 'on error' syntax on variable declarations, assignments, and calls") {
        const std::string badTrailingDim = R"(fn main()
    dim x = foo() on error e
        return
    end error
end fn
)";
        const auto lexTd = cb::tokenize(badTrailingDim, "badTrailingDim.cb");
        REQUIRE(lexTd.ok);
        const auto parseTd = cb::parseTokens(lexTd.tokens, "badTrailingDim.cb");
        CHECK_FALSE(parseTd.ok);
        CHECK(parseTd.error.kind == "ParseError");

        const std::string badTrailingCall = R"(fn main()
    foo() on error e
        return
    end error
end fn
)";
        const auto lexTc = cb::tokenize(badTrailingCall, "badTrailingCall.cb");
        REQUIRE(lexTc.ok);
        const auto parseTc = cb::parseTokens(lexTc.tokens, "badTrailingCall.cb");
        CHECK_FALSE(parseTc.ok);
        CHECK(parseTc.error.kind == "ParseError");
    }

    SECTION("Reports ParseError on unclosed control flow block") {
        const std::string badCode = R"(fn broken()
    if true
        dim x = 1
end fn
)";
        const auto badLex = cb::tokenize(badCode, "bad.cb");
        REQUIRE(badLex.ok);
        const auto badParse = cb::parseTokens(badLex.tokens, "bad.cb");
        CHECK_FALSE(badParse.ok);
        CHECK(badParse.error.kind == "ParseError");
    }

}

TEST_CASE("Analyzer validates fallible calls, const immutability, and implicit bubbling", "[analyzer]") {
    SECTION("Rejects unhandled fallible call in infallible function") {
        const std::string badFallible = R"(
fn risky() as int can error
    return 1
end fn

fn callerWithoutErr()
    dim x = risky()
end fn
)";

        const auto lexBad = cb::tokenize(badFallible, "badFallible.cb");
        REQUIRE(lexBad.ok);
        auto parseBad = cb::parseTokens(lexBad.tokens, "badFallible.cb");
        REQUIRE(parseBad.ok);
        const auto anaBad = cb::analyzeProgram(parseBad.arena, parseBad.program);
        CHECK_FALSE(anaBad.ok);
        CHECK(anaBad.error.kind == "AnalyzerError");
        CHECK(anaBad.error.message.find(
            "Unhandled error: function returns an error but caller does not declare 'can error' or have an active 'on error' block"
        ) != std::string::npos);
    }

    SECTION("Rejects unhandled fallible call in expression statement") {
        const std::string badExprStmt = R"(
fn fail() as int can error
    error("failed", code=500)
    return 0
end fn

fn call()
    fail()
end fn
)";
        const auto res = cb::transpileSource(badExprStmt, "badExprStmt.cb");
        CHECK_FALSE(res.ok);
        CHECK(res.error.kind == "AnalyzerError");
        CHECK(res.error.message.find(
            "Unhandled error: function returns an error but caller does not declare 'can error' or have an active 'on error' block"
        ) != std::string::npos);
    }

    SECTION("Rejects fallible call occurring before 'on error' block is registered in scope") {
        const std::string badOrder = R"(
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
        const auto res = cb::transpileSource(badOrder, "badOrder.cb");
        CHECK_FALSE(res.ok);
        CHECK(res.error.kind == "AnalyzerError");
        CHECK(res.error.message.find(
            "Unhandled error: function returns an error but caller does not declare 'can error' or have an active 'on error' block"
        ) != std::string::npos);
    }

    SECTION("Rejects 'on error' block that does not terminate with return, error(...), or break") {
        const std::string badNoExit = R"(
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
        const auto res = cb::transpileSource(badNoExit, "badNoExit.cb");
        CHECK_FALSE(res.ok);
        CHECK(res.error.kind == "AnalyzerError");
        CHECK(res.error.message.find(
            "'on error' block must terminate with an explicit control-flow exit"
        ) != std::string::npos);
    }

    SECTION("Rejects calling error(...) inside an infallible function or with invalid named args") {
        const std::string badErrorInInfallible = R"(
fn notFallible() as int
    error("boom", code=1)
    return 0
end fn
)";
        const auto resInf = cb::transpileSource(badErrorInInfallible, "badErrorInf.cb");
        CHECK_FALSE(resInf.ok);
        CHECK(resInf.error.kind == "AnalyzerError");
        CHECK(resInf.error.message.find("Cannot call 'error(...)' inside infallible function") != std::string::npos);

        const std::string badErrorKwarg = R"(
fn fallibleFn() as int can error
    error("boom", status=500)
    return 0
end fn
)";
        const auto resKw = cb::transpileSource(badErrorKwarg, "badErrorKw.cb");
        CHECK_FALSE(resKw.ok);
        CHECK(resKw.error.kind == "AnalyzerError");
        CHECK(resKw.error.message.find("Unknown named argument 'status'") != std::string::npos);
    }

    SECTION("Rejects unhandled fallible call inside variadic print arguments") {
        const std::string badPrintArg = R"(
fn risky() as int can error
    return 7
end fn

fn main()
    print("value=", risky())
end fn
)";
        const auto res = cb::transpileSource(badPrintArg, "badPrintArg.cb");
        CHECK_FALSE(res.ok);
        CHECK(res.error.kind == "AnalyzerError");

    }

    SECTION("Rejects const variable reassignment") {
        const std::string badConst = R"(
fn main()
    const limit as int = 10
    limit = 20
end fn
)";
        const auto resBadConst = cb::transpileSource(badConst, "badConst.cb");
        CHECK_FALSE(resBadConst.ok);
        CHECK(resBadConst.error.kind == "AnalyzerError");
        CHECK(resBadConst.error.message.find("Cannot reassign const variable") != std::string::npos);
    }

    SECTION("Annotates implicitBubble on fallible calls inside fallible functions") {
        const std::string goodCode = R"(
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

        const auto lexGood = cb::tokenize(goodCode, "good.cb");
        REQUIRE(lexGood.ok);
        auto parseGood = cb::parseTokens(lexGood.tokens, "good.cb");
        REQUIRE(parseGood.ok);
        const auto anaGood = cb::analyzeProgram(parseGood.arena, parseGood.program);
        REQUIRE(anaGood.ok);

        const auto itRisky = anaGood.symbols.functions.find("risky");
        REQUIRE(itRisky != anaGood.symbols.functions.end());
        CHECK(itRisky->second.isFallible);

        const auto itMiddle = anaGood.symbols.functions.find("middle");
        REQUIRE(itMiddle != anaGood.symbols.functions.end());
        CHECK(itMiddle->second.isFallible);

        const auto itMain = anaGood.symbols.functions.find("main");
        REQUIRE(itMain != anaGood.symbols.functions.end());
        CHECK_FALSE(itMain->second.isFallible);

        const cb::DeclId middleDeclId = parseGood.program.declarations[1];
        const auto& middleFn = cb::asDecl<cb::FnDecl>(parseGood.arena, middleDeclId);
        REQUIRE(!middleFn.body.empty());
        const cb::StmtId valStmtId = middleFn.body[0];
        const auto& valAssign = cb::asStmt<cb::VarAssignment>(parseGood.arena, valStmtId);
        CHECK(valAssign.implicitBubble);
    }
}

TEST_CASE("Emitter generates C++17 error bubbling and defer for extern declarations", "[emitter]") {
    const std::string code = R"(
pub extern type SQL_DB
pub extern fn connect(path as str) as SQL_DB can error
pub extern fn SQL_close(db as SQL_DB)

fn processData() as int can error
    ' Implicit bubble (needs if (cbHasError(_err1)) return ...)
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

    dim records = processData()
end fn
)";

    const auto res = cb::transpileSource(code, "section5.cb");
    REQUIRE(res.ok);

    const std::string& cpp = res.cppCode;
    CHECK(cpp.find("struct [[nodiscard]] CbError {") != std::string::npos);
    CHECK(cpp.find("template <typename F> struct CbDeferTask { F f; ~CbDeferTask() { f(); } };") != std::string::npos);
    CHECK(cpp.find("template <typename F> CbDeferTask<F> cbDefer(F f) { return {f}; }") != std::string::npos);
    CHECK(cpp.find("[[nodiscard]] std::tuple<int64_t, CbError> processData()") != std::string::npos);
    CHECK(cpp.find(R"raw(auto [_val1, _err1] = connect(std::string("data.db"));)raw") != std::string::npos);
    CHECK(cpp.find("if (cbHasError(_err1)) {") != std::string::npos);
    CHECK(cpp.find("return { {}, _err1 };") != std::string::npos);
    CHECK(cpp.find("auto db = _val1;") != std::string::npos);
    CHECK(cpp.find("auto _defer_1 = cbDefer([&]() {") != std::string::npos);
    CHECK(cpp.find("SQL_close(db);") != std::string::npos);
    CHECK(cpp.find(R"raw(return { 100, CbError{"", 0} };)raw") != std::string::npos);
    CHECK(cpp.find("auto [_val2, _err2] = processData();") != std::string::npos);
    CHECK(cpp.find("if (cbHasError(_err2)) {") != std::string::npos);
    CHECK(cpp.find("CbError e = _err2;") != std::string::npos);
    CHECK(cpp.find("auto records = _val2;") != std::string::npos);

    SECTION("Emits variadic print, cbToStr auto-coercion on &, and inclusive for..to loop") {
        const std::string ergonomicCode = R"(
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
        const auto ergRes = cb::transpileSource(ergonomicCode, "ergonomic.cb");
        REQUIRE(ergRes.ok);
        CHECK(ergRes.cppCode.find("for (int64_t i = 0; i <= 3; i += 1) {") != std::string::npos);
        CHECK(ergRes.cppCode.find(R"raw((std::cout << std::string("i=") << i << std::string(" ok=") << cbToStr(ok) << std::endl);)raw") != std::string::npos);
        CHECK(ergRes.cppCode.find(R"raw((std::string("Score: ") + cbToStr(score)))raw") != std::string::npos);
        CHECK(ergRes.cppCode.find(R"raw((cbToStr(score) + std::string(" points")))raw") != std::string::npos);
        CHECK(ergRes.cppCode.find("(std::cout << std::endl);") != std::string::npos);
    }

    SECTION("Deduplicates canonical TypeNodes in AstArena and emits from const AstArena&") {
        const std::string immutCode = R"(
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
        const auto lexIm = cb::tokenize(immutCode, "immut.cb");
        REQUIRE(lexIm.ok);
        auto parseIm = cb::parseTokens(lexIm.tokens, "immut.cb");
        REQUIRE(parseIm.ok);
        const auto anaIm = cb::analyzeProgram(parseIm.arena, parseIm.program);
        REQUIRE(anaIm.ok);

        const size_t typesAfterAnalysis = parseIm.arena.types.size();
        CHECK(cb::findType(parseIm.arena, "int") == anaIm.symbols.typeInt);
        CHECK(cb::findType(parseIm.arena, "str") == anaIm.symbols.typeStr);

        size_t intCount = 0;
        for (const auto& tnode : parseIm.arena.types) {
            if (tnode.name == "int" && tnode.typeArgs.empty()) {
                intCount += 1;
            }
        }
        CHECK(intCount == 1);

        const cb::AstArena& constArena = parseIm.arena;
        const std::string emitted = cb::emitProgram(constArena, anaIm.symbols, parseIm.program);
        CHECK(constArena.types.size() == typesAfterAnalysis);
        CHECK(emitted.find("struct Box {") != std::string::npos);
        CHECK(emitted.find("int64_t compute()") != std::string::npos);
    }
}

TEST_CASE("End-to-end fixture: multi-file modules, mutability, and UFCS", "[e2e]") {
    cb::testE2E::runE2EFixture("modules_ufcs", "tests/cases/modules_ufcs/main.cb");
}

TEST_CASE("End-to-end fixture: nested folder modules, name collision mangling, and relative imports", "[e2e]") {
    cb::testE2E::runE2EFixture("nested_modules", "tests/cases/nested_modules/main.cb");
}

TEST_CASE("Module scoping, pub visibility, duplicate aliases, diamond deduplication, and circular imports", "[modules]") {
    const std::string baseDir = "build/testArtifacts/moduleEdgeCases";
    std::error_code ec;
    std::filesystem::remove_all(baseDir, ec);
    std::filesystem::create_directories(baseDir + "/sub", ec);
    REQUIRE(!ec);

    SECTION("Rejects circular imports with clear diagnostic") {
        {
            std::ofstream fa(baseDir + "/a.cb");
            fa << R"(import "sub/b.cb"
pub fn fa() as int
    return 1
end fn
)";
        }
        {
            std::ofstream fb(baseDir + "/sub/b.cb");
            fb << R"(import "../a.cb"
pub fn fb() as int
    return 2
end fn
)";
        }
        const auto res = cb::buildFile(baseDir + "/a.cb", baseDir + "/out.cpp", false);
        REQUIRE_FALSE(res.ok);
        CHECK(res.error.kind == "ImportError");
        CHECK(res.error.message.find("Circular module import detected") != std::string::npos);
    }

    SECTION("Deduplicates diamond imports and preserves topological order") {
        {
            std::ofstream fcommon(baseDir + "/sub/common.cb");
            fcommon << R"(pub type SharedCfg
    scale as int
end type
)";
        }
        {
            std::ofstream fleft(baseDir + "/left.cb");
            fleft << R"(import "sub/common.cb"
pub fn makeCfg(s as int) as common.SharedCfg
    return common.SharedCfg(scale=s)
end fn
)";
        }
        {
            std::ofstream fmain(baseDir + "/diamondMain.cb");
            fmain << R"(import "sub/common.cb"
import "left.cb"
fn main()
    dim c = left.makeCfg(7)
    print(c.scale)
end fn
)";
        }
        const auto res = cb::buildFile(baseDir + "/diamondMain.cb", baseDir + "/diamondOut.cpp", false);
        REQUIRE(res.ok);
        const std::string cpp = cb::testE2E::readFileText(baseDir + "/diamondOut.cpp");
        const size_t firstPos = cpp.find("struct cb_sub_common__SharedCfg {");
        REQUIRE(firstPos != std::string::npos);
        CHECK(cpp.find("struct cb_sub_common__SharedCfg {", firstPos + 1) == std::string::npos);
    }

    SECTION("Rejects duplicate module import aliases in the same file") {
        {
            std::ofstream f1(baseDir + "/mod.cb");
            f1 << R"(pub fn f() as int
    return 1
end fn
)";
        }
        {
            std::ofstream f2(baseDir + "/sub/mod.cb");
            f2 << R"(pub fn f() as int
    return 2
end fn
)";
        }
        {
            std::ofstream fmain(baseDir + "/dupAlias.cb");
            fmain << R"(import "mod.cb"
import "sub/mod.cb"
fn main()
    print(1)
end fn
)";
        }
        const auto res = cb::buildFile(baseDir + "/dupAlias.cb", baseDir + "/dupOut.cpp", false);
        REQUIRE_FALSE(res.ok);
        CHECK(res.error.kind == "AnalyzerError");
        CHECK(res.error.message.find("Duplicate module import alias 'mod'") != std::string::npos);
    }

    SECTION("Enforces private by default (non-pub functions and types are rejected across modules)") {
        {
            std::ofstream flib(baseDir + "/secret.cb");
            flib << R"(type HiddenType
    v as int
end type

fn hiddenFn() as int
    return 42
end fn

pub fn publicFn() as int
    return hiddenFn()
end fn
)";
        }
        {
            std::ofstream fcall(baseDir + "/callPriv.cb");
            fcall << R"(import "secret.cb"
fn main()
    dim x = secret.hiddenFn()
end fn
)";
        }
        const auto resFn = cb::buildFile(baseDir + "/callPriv.cb", baseDir + "/privFn.cpp", false);
        REQUIRE_FALSE(resFn.ok);
        CHECK(resFn.error.message.find("is private to module 'secret'") != std::string::npos);

        {
            std::ofstream ftype(baseDir + "/constructPriv.cb");
            ftype << R"(import "secret.cb"
fn main()
    dim h = secret.HiddenType(v=1)
end fn
)";
        }
        const auto resType = cb::buildFile(baseDir + "/constructPriv.cb", baseDir + "/privType.cpp", false);
        REQUIRE_FALSE(resType.ok);
        CHECK(resType.error.message.find("is private to module 'secret'") != std::string::npos);
    }
}

TEST_CASE("End-to-end fixture: network error bubbling and defer cleanup", "[e2e]") {
    cb::testE2E::runE2EFixture("networkErrorBubblingDefer", "tests/cases/network_error_bubbling_defer.cb");
}

TEST_CASE("End-to-end fixture: STL containers and select case", "[e2e]") {
    cb::testE2E::runE2EFixture("containersAndSelectCase", "tests/cases/containers_and_select_case.cb");
}

TEST_CASE("End-to-end fixture: struct initialization and field access", "[e2e]") {
    cb::testE2E::runE2EFixture("structAndFieldAccess", "tests/cases/struct_and_field_access.cb");
}

TEST_CASE("End-to-end fixture: for range loop with step", "[e2e]") {
    cb::testE2E::runE2EFixture("forRangeStep", "tests/cases/for_range_step.cb");
}

TEST_CASE("End-to-end fixture: cptr interop", "[e2e]") {
    cb::testE2E::runE2EFixture("cptrInterop", "tests/cases/cptr_interop.cb");
}


TEST_CASE("End-to-end fixture: fibonacci sequence", "[e2e]") {
    cb::testE2E::runE2EFixture("fibonacci", "tests/cases/fibonacci.cb");
}

TEST_CASE("End-to-end fixture: enums and bytes", "[e2e]") {
    cb::testE2E::runE2EFixture("enumsAndBytes", "tests/cases/enums_and_bytes.cb");
}
TEST_CASE("End-to-end CLI build generates persistent C++ artifacts", "[e2e]") {
    const std::string artifactDir = "build/testArtifacts/cliBuild";
    std::error_code ec;
    std::filesystem::create_directories(artifactDir, ec);
    REQUIRE(!ec);

    const std::string testCbPath = artifactDir + "/sampleCli.cb";
    const std::string testOutPath = artifactDir + "/build.cpp";
    const std::string cliOutPath = artifactDir + "/cliExecBuild.cpp";

    std::ofstream outCb(testCbPath);
    outCb << R"(import link "m"

fn main()
    dim x = 42
    print("Answer: " & x.toStr())
end fn
)";
    outCb.close();

    const auto buildRes = cb::buildFile(testCbPath, testOutPath, false);
    REQUIRE(buildRes.ok);

    const std::string content = cb::testE2E::readFileText(testOutPath);
    CHECK(content.find("// CleanBasic Linker Flags: -lm") != std::string::npos);
    CHECK(content.find("int main(int argc, char** argv)") != std::string::npos);

    std::string cbBinPath = "./build/cleanbasic";
    std::ifstream cbBinRoot("build/cleanbasic");
    if (!cbBinRoot.good()) {
        std::ifstream cbBinLocal("./cleanbasic");
        if (cbBinLocal.good()) {
            cbBinPath = "./cleanbasic";
        }
    }

    std::ifstream cbBinCheck(cbBinPath);
    REQUIRE(cbBinCheck.good());

    const std::string cliCmd = cbBinPath + " build " + testCbPath + " -o " + cliOutPath + " > /dev/null 2>&1";
    const int cliRc = std::system(cliCmd.c_str());
    CHECK(cliRc == 0);
    CHECK(std::filesystem::exists(cliOutPath, ec));
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

        const auto parsed = cb::parseJson(input);
        REQUIRE(parsed.ok);
        REQUIRE(parsed.root != cb::INVALID_ID);

        CHECK(cb::jsonGetString(parsed.arena, cb::jsonGetField(parsed.arena, parsed.root, "jsonrpc")) == "2.0");
        CHECK(cb::jsonGetInt(parsed.arena, cb::jsonGetField(parsed.arena, parsed.root, "id")) == 42);
        CHECK(cb::jsonGetInt(parsed.arena, cb::jsonGetField(parsed.arena, parsed.root, "negative")) == -100);
        CHECK(cb::jsonGetFloat(parsed.arena, cb::jsonGetField(parsed.arena, parsed.root, "pi")) == 3.25);
        CHECK(cb::jsonGetBool(parsed.arena, cb::jsonGetField(parsed.arena, parsed.root, "active")) == true);
        CHECK(cb::jsonGetBool(parsed.arena, cb::jsonGetField(parsed.arena, parsed.root, "disabled")) == false);
        CHECK(cb::jsonIsNull(parsed.arena, cb::jsonGetField(parsed.arena, parsed.root, "nothing")));

        const cb::JsonId tagsId = cb::jsonGetField(parsed.arena, parsed.root, "tags");
        REQUIRE(tagsId != cb::INVALID_ID);
        REQUIRE(std::holds_alternative<cb::JsonArray>(parsed.arena.nodes[tagsId]));
        const auto& tags = std::get<cb::JsonArray>(parsed.arena.nodes[tagsId]).elements;
        REQUIRE(tags.size() == 2);
        CHECK(cb::jsonGetString(parsed.arena, tags[0]) == "lsp");
        CHECK(cb::jsonGetString(parsed.arena, tags[1]) == "cleanbasic");

        const cb::JsonId nestedId = cb::jsonGetField(parsed.arena, parsed.root, "nested");
        REQUIRE(nestedId != cb::INVALID_ID);
        CHECK(cb::jsonGetInt(parsed.arena, cb::jsonGetField(parsed.arena, nestedId, "x")) == 10);
        CHECK(cb::jsonGetInt(parsed.arena, cb::jsonGetField(parsed.arena, nestedId, "y")) == 20);

        const std::string serialized = cb::serializeJson(parsed.arena, parsed.root);
        CHECK(serialized == R"({"jsonrpc":"2.0","id":42,"negative":-100,"pi":3.25,"active":true,"disabled":false,"nothing":null,"tags":["lsp","cleanbasic"],"nested":{"x":10,"y":20}})");

        const auto reparsed = cb::parseJson(serialized);
        REQUIRE(reparsed.ok);
        CHECK(cb::serializeJson(reparsed.arena, reparsed.root) == serialized);
    }

    SECTION("String escape sequences and UTF-8 unicode decoding and re-escaping") {
        const std::string bs(1, cb::JSON_BACKSLASH);
        const std::string dq(1, cb::JSON_DQUOTE);
        const std::string escapedJson =
            "{" + dq + "msg" + dq + ":" + dq + "quote:" + bs + dq +
            " slash:" + bs + bs +
            " fwd:" + bs + "/" +
            " bs:" + bs + "b" +
            " ff:" + bs + "f" +
            " nl:" + bs + "n" +
            " cr:" + bs + "r" +
            " tab:" + bs + "t" +
            " hex:" + bs + "u0041" + bs + "u00e9" + dq + "}";
        const auto parsed = cb::parseJson(escapedJson);
        REQUIRE(parsed.ok);

        const auto msgOpt = cb::jsonGetString(parsed.arena, cb::jsonGetField(parsed.arena, parsed.root, "msg"));
        REQUIRE(msgOpt.has_value());
        std::string expectedMsg = "quote:" + dq + " slash:" + bs + " fwd:/ bs:\b ff:\f nl:\n cr:\r tab:\t hex:A";
        expectedMsg.push_back(static_cast<char>(0xC3));
        expectedMsg.push_back(static_cast<char>(0xA9));
        CHECK(msgOpt.value() == expectedMsg);

        const std::string reserialized = cb::serializeJson(parsed.arena, parsed.root);
        const auto roundtrip = cb::parseJson(reserialized);
        REQUIRE(roundtrip.ok);
        CHECK(cb::jsonGetString(roundtrip.arena, cb::jsonGetField(roundtrip.arena, roundtrip.root, "msg")) == expectedMsg);
    }

    SECTION("Ergonomic builder and query functions for constructing LSP payloads") {
        cb::JsonArena arena;
        const cb::JsonId root = cb::jsonAddObject(arena);
        cb::jsonObjectSet(arena, root, "jsonrpc", cb::jsonAddString(arena, "2.0"));
        cb::jsonObjectSet(arena, root, "id", cb::jsonAddInt(arena, 1));
        cb::jsonObjectSet(arena, root, "id", cb::jsonAddInt(arena, 7)); // overwrite existing key

        const cb::JsonId resultObj = cb::jsonAddObject(arena);
        const cb::JsonId itemsArr = cb::jsonAddArray(arena);
        cb::jsonArrayPush(arena, itemsArr, cb::jsonAddString(arena, "fn"));
        cb::jsonArrayPush(arena, itemsArr, cb::jsonAddBool(arena, true));
        cb::jsonArrayPush(arena, itemsArr, cb::jsonAddNull(arena));
        cb::jsonArrayPush(arena, itemsArr, cb::jsonAddFloat(arena, 1.5));
        cb::jsonObjectSet(arena, resultObj, "items", itemsArr);
        cb::jsonObjectSet(arena, root, "result", resultObj);

        CHECK(cb::jsonGetInt(arena, cb::jsonGetField(arena, root, "id")) == 7);
        CHECK(cb::jsonGetField(arena, root, "missing") == cb::JSON_INVALID_ID);
        CHECK_FALSE(cb::jsonGetString(arena, cb::JSON_INVALID_ID).has_value());
        CHECK_FALSE(cb::jsonGetInt(arena, cb::jsonGetField(arena, root, "jsonrpc")).has_value());
        CHECK_FALSE(cb::jsonGetBool(arena, cb::jsonGetField(arena, root, "id")).has_value());

        const std::string out = cb::serializeJson(arena, root);
        CHECK(out == R"({"jsonrpc":"2.0","id":7,"result":{"items":["fn",true,null,1.5]}})");
    }

    SECTION("Rejects malformed JSON inputs without throwing exceptions") {
        const std::string bs(1, cb::JSON_BACKSLASH);
        const std::string dq(1, cb::JSON_DQUOTE);
        const std::vector<std::string> badInputs = {
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

        for (const auto& bad : badInputs) {
            const auto res = cb::parseJson(bad);
            CHECK_FALSE(res.ok);
            CHECK_FALSE(res.error.empty());
        }
    }

    SECTION("JSON-RPC 2.0 Content-Length framing read and write over streams") {
        std::stringstream stream;
        const std::string msg1 = R"({"jsonrpc":"2.0","id":1,"method":"initialize"})";
        const std::string msg2 = R"({"jsonrpc":"2.0","method":"initialized","params":{}})";

        cb::writeJsonrpcMessage(stream, msg1);
        stream << "Content-Type: application/vscode-jsonrpc; charset=utf-8\r\n";
        cb::writeJsonrpcMessage(stream, msg2);

        const auto read1 = cb::readJsonrpcMessage(stream);
        REQUIRE(read1.has_value());
        CHECK(read1.value() == msg1);

        const auto read2 = cb::readJsonrpcMessage(stream);
        REQUIRE(read2.has_value());
        CHECK(read2.value() == msg2);

        const auto readEof = cb::readJsonrpcMessage(stream);
        CHECK_FALSE(readEof.has_value());

        std::stringstream truncated("Content-Length: 50\r\n\r\n" + std::string(R"({"short":true})"));
        CHECK_FALSE(cb::readJsonrpcMessage(truncated).has_value());

        std::stringstream missingLen("Custom-Header: 123\r\n\r\n{}");
        CHECK_FALSE(cb::readJsonrpcMessage(missingLen).has_value());
    }
}

TEST_CASE("CleanBasic Language Server Phase 1: Core Loop, In-Memory Overlay & Live Diagnostics", "[lsp]") {
    SECTION("URI and file path conversion helpers") {
        CHECK(cb::uriToFilePath("file:///workspace/project/main.cb") == "/workspace/project/main.cb");
        CHECK(cb::uriToFilePath("file://localhost/workspace/main.cb") == "/workspace/main.cb");
        CHECK(cb::uriToFilePath("file:///workspace/my%20folder/main.cb") == "/workspace/my folder/main.cb");
        CHECK(cb::uriToFilePath("file:///C:/Users/dev/main.cb") == "C:/Users/dev/main.cb");
        CHECK(cb::filePathToUri("/workspace/project/main.cb") == "file:///workspace/project/main.cb");
        CHECK(cb::filePathToUri("relative/main.cb") == "file:///relative/main.cb");
    }

    SECTION("Full JSON-RPC lifecycle: initialize, didOpen, publishDiagnostics, didChange, didClose, shutdown, exit") {
        std::stringstream clientToServer;
        std::stringstream serverToClient;

        // 1. initialize request (id = 1)
        cb::writeJsonrpcMessage(
            clientToServer,
            R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":"file:///workspace"}})"
        );

        // 2. initialized notification
        cb::writeJsonrpcMessage(
            clientToServer,
            R"({"jsonrpc":"2.0","method":"initialized","params":{}})"
        );

        // 3. textDocument/didOpen with valid CleanBasic code
        {
            cb::JsonArena arena;
            const cb::JsonId msg = cb::jsonAddObject(arena);
            cb::jsonObjectSet(arena, msg, "jsonrpc", cb::jsonAddString(arena, "2.0"));
            cb::jsonObjectSet(arena, msg, "method", cb::jsonAddString(arena, "textDocument/didOpen"));
            const cb::JsonId params = cb::jsonAddObject(arena);
            const cb::JsonId td = cb::jsonAddObject(arena);
            cb::jsonObjectSet(arena, td, "uri", cb::jsonAddString(arena, "file:///workspace/valid.cb"));
            cb::jsonObjectSet(arena, td, "languageId", cb::jsonAddString(arena, "cleanbasic"));
            cb::jsonObjectSet(arena, td, "version", cb::jsonAddInt(arena, 1));
            cb::jsonObjectSet(arena, td, "text", cb::jsonAddString(arena, R"(fn main() as int
    dim x = 42
    return x
end fn
)"));
            cb::jsonObjectSet(arena, params, "textDocument", td);
            cb::jsonObjectSet(arena, msg, "params", params);
            cb::writeJsonrpcMessage(clientToServer, cb::serializeJson(arena, msg));
        }

        // 4. textDocument/didOpen with invalid CleanBasic code (const reassignment on line 3, 1-based -> line 2, 0-based)
        {
            cb::JsonArena arena;
            const cb::JsonId msg = cb::jsonAddObject(arena);
            cb::jsonObjectSet(arena, msg, "jsonrpc", cb::jsonAddString(arena, "2.0"));
            cb::jsonObjectSet(arena, msg, "method", cb::jsonAddString(arena, "textDocument/didOpen"));
            const cb::JsonId params = cb::jsonAddObject(arena);
            const cb::JsonId td = cb::jsonAddObject(arena);
            cb::jsonObjectSet(arena, td, "uri", cb::jsonAddString(arena, "file:///workspace/broken.cb"));
            cb::jsonObjectSet(arena, td, "languageId", cb::jsonAddString(arena, "cleanbasic"));
            cb::jsonObjectSet(arena, td, "version", cb::jsonAddInt(arena, 1));
            cb::jsonObjectSet(arena, td, "text", cb::jsonAddString(arena, R"(fn main() as int
    const x = 10
    x = 20
    return x
end fn
)"));
            cb::jsonObjectSet(arena, params, "textDocument", td);
            cb::jsonObjectSet(arena, msg, "params", params);
            cb::writeJsonrpcMessage(clientToServer, cb::serializeJson(arena, msg));
        }

        // 5. textDocument/didChange fixing broken.cb (changing const to dim)
        {
            cb::JsonArena arena;
            const cb::JsonId msg = cb::jsonAddObject(arena);
            cb::jsonObjectSet(arena, msg, "jsonrpc", cb::jsonAddString(arena, "2.0"));
            cb::jsonObjectSet(arena, msg, "method", cb::jsonAddString(arena, "textDocument/didChange"));
            const cb::JsonId params = cb::jsonAddObject(arena);
            const cb::JsonId td = cb::jsonAddObject(arena);
            cb::jsonObjectSet(arena, td, "uri", cb::jsonAddString(arena, "file:///workspace/broken.cb"));
            cb::jsonObjectSet(arena, td, "version", cb::jsonAddInt(arena, 2));
            cb::jsonObjectSet(arena, params, "textDocument", td);
            const cb::JsonId changes = cb::jsonAddArray(arena);
            const cb::JsonId changeItem = cb::jsonAddObject(arena);
            cb::jsonObjectSet(arena, changeItem, "text", cb::jsonAddString(arena, R"(fn main() as int
    dim x = 10
    x = 20
    return x
end fn
)"));
            cb::jsonArrayPush(arena, changes, changeItem);
            cb::jsonObjectSet(arena, params, "contentChanges", changes);
            cb::jsonObjectSet(arena, msg, "params", params);
            cb::writeJsonrpcMessage(clientToServer, cb::serializeJson(arena, msg));
        }

        // 6. textDocument/didClose on broken.cb
        {
            cb::JsonArena arena;
            const cb::JsonId msg = cb::jsonAddObject(arena);
            cb::jsonObjectSet(arena, msg, "jsonrpc", cb::jsonAddString(arena, "2.0"));
            cb::jsonObjectSet(arena, msg, "method", cb::jsonAddString(arena, "textDocument/didClose"));
            const cb::JsonId params = cb::jsonAddObject(arena);
            const cb::JsonId td = cb::jsonAddObject(arena);
            cb::jsonObjectSet(arena, td, "uri", cb::jsonAddString(arena, "file:///workspace/broken.cb"));
            cb::jsonObjectSet(arena, params, "textDocument", td);
            cb::jsonObjectSet(arena, msg, "params", params);
            cb::writeJsonrpcMessage(clientToServer, cb::serializeJson(arena, msg));
        }

        // 7. shutdown request (id = 2) and exit notification
        cb::writeJsonrpcMessage(
            clientToServer,
            R"({"jsonrpc":"2.0","id":2,"method":"shutdown","params":null})"
        );
        cb::writeJsonrpcMessage(
            clientToServer,
            R"({"jsonrpc":"2.0","method":"exit"})"
        );

        const int exitCode = cb::runLspServer(clientToServer, serverToClient);
        CHECK(exitCode == 0);

        // Verify Message 1: initialize response
        const auto resp1Raw = cb::readJsonrpcMessage(serverToClient);
        REQUIRE(resp1Raw.has_value());
        const auto resp1 = cb::parseJson(resp1Raw.value());
        REQUIRE(resp1.ok);
        CHECK(cb::jsonGetInt(resp1.arena, cb::jsonGetField(resp1.arena, resp1.root, "id")) == 1);
        const cb::JsonId res1Obj = cb::jsonGetField(resp1.arena, resp1.root, "result");
        const cb::JsonId capsObj = cb::jsonGetField(resp1.arena, res1Obj, "capabilities");
        CHECK(cb::jsonGetInt(resp1.arena, cb::jsonGetField(resp1.arena, capsObj, "textDocumentSync")) == 1);
        const cb::JsonId srvInfo = cb::jsonGetField(resp1.arena, res1Obj, "serverInfo");
        CHECK(cb::jsonGetString(resp1.arena, cb::jsonGetField(resp1.arena, srvInfo, "name")) == "cleanbasic-lsp");

        // Verify Message 2: publishDiagnostics for valid.cb (empty diagnostics array)
        const auto diag1Raw = cb::readJsonrpcMessage(serverToClient);
        REQUIRE(diag1Raw.has_value());
        const auto diag1 = cb::parseJson(diag1Raw.value());
        REQUIRE(diag1.ok);
        CHECK(cb::jsonGetString(diag1.arena, cb::jsonGetField(diag1.arena, diag1.root, "method")) == "textDocument/publishDiagnostics");
        const cb::JsonId diag1Params = cb::jsonGetField(diag1.arena, diag1.root, "params");
        CHECK(cb::jsonGetString(diag1.arena, cb::jsonGetField(diag1.arena, diag1Params, "uri")) == "file:///workspace/valid.cb");
        const cb::JsonId diag1ArrId = cb::jsonGetField(diag1.arena, diag1Params, "diagnostics");
        REQUIRE(std::holds_alternative<cb::JsonArray>(diag1.arena.nodes[diag1ArrId]));
        CHECK(std::get<cb::JsonArray>(diag1.arena.nodes[diag1ArrId]).elements.empty());

        // Verify Message 3: publishDiagnostics for broken.cb (1 error diagnostic at 0-based line 2, char 4)
        const auto diag2Raw = cb::readJsonrpcMessage(serverToClient);
        REQUIRE(diag2Raw.has_value());
        const auto diag2 = cb::parseJson(diag2Raw.value());
        REQUIRE(diag2.ok);
        CHECK(cb::jsonGetString(diag2.arena, cb::jsonGetField(diag2.arena, diag2.root, "method")) == "textDocument/publishDiagnostics");
        const cb::JsonId diag2Params = cb::jsonGetField(diag2.arena, diag2.root, "params");
        CHECK(cb::jsonGetString(diag2.arena, cb::jsonGetField(diag2.arena, diag2Params, "uri")) == "file:///workspace/broken.cb");
        const cb::JsonId diag2ArrId = cb::jsonGetField(diag2.arena, diag2Params, "diagnostics");
        REQUIRE(std::holds_alternative<cb::JsonArray>(diag2.arena.nodes[diag2ArrId]));
        const auto& diag2Elems = std::get<cb::JsonArray>(diag2.arena.nodes[diag2ArrId]).elements;
        REQUIRE(diag2Elems.size() == 1);
        const cb::JsonId errDiag = diag2Elems[0];
        CHECK(cb::jsonGetInt(diag2.arena, cb::jsonGetField(diag2.arena, errDiag, "severity")) == 1);
        CHECK(cb::jsonGetString(diag2.arena, cb::jsonGetField(diag2.arena, errDiag, "source")) == "cleanbasic");
        CHECK(cb::jsonGetString(diag2.arena, cb::jsonGetField(diag2.arena, errDiag, "code")) == "AnalyzerError");
        const auto errMsgOpt = cb::jsonGetString(diag2.arena, cb::jsonGetField(diag2.arena, errDiag, "message"));
        REQUIRE(errMsgOpt.has_value());
        CHECK(errMsgOpt.value().find("Cannot reassign const variable 'x'") != std::string::npos);

        const cb::JsonId rangeObj = cb::jsonGetField(diag2.arena, errDiag, "range");
        const cb::JsonId startObj = cb::jsonGetField(diag2.arena, rangeObj, "start");
        CHECK(cb::jsonGetInt(diag2.arena, cb::jsonGetField(diag2.arena, startObj, "line")) == 2);
        CHECK(cb::jsonGetInt(diag2.arena, cb::jsonGetField(diag2.arena, startObj, "character")) == 4);

        // Verify Message 4: publishDiagnostics after didChange fixes broken.cb (diagnostics cleared)
        const auto diag3Raw = cb::readJsonrpcMessage(serverToClient);
        REQUIRE(diag3Raw.has_value());
        const auto diag3 = cb::parseJson(diag3Raw.value());
        REQUIRE(diag3.ok);
        const cb::JsonId diag3Params = cb::jsonGetField(diag3.arena, diag3.root, "params");
        CHECK(cb::jsonGetString(diag3.arena, cb::jsonGetField(diag3.arena, diag3Params, "uri")) == "file:///workspace/broken.cb");
        const cb::JsonId diag3ArrId = cb::jsonGetField(diag3.arena, diag3Params, "diagnostics");
        REQUIRE(std::holds_alternative<cb::JsonArray>(diag3.arena.nodes[diag3ArrId]));
        CHECK(std::get<cb::JsonArray>(diag3.arena.nodes[diag3ArrId]).elements.empty());

        // Verify Message 5: publishDiagnostics after didClose on broken.cb (diagnostics cleared)
        const auto diag4Raw = cb::readJsonrpcMessage(serverToClient);
        REQUIRE(diag4Raw.has_value());
        const auto diag4 = cb::parseJson(diag4Raw.value());
        REQUIRE(diag4.ok);
        const cb::JsonId diag4Params = cb::jsonGetField(diag4.arena, diag4.root, "params");
        const cb::JsonId diag4ArrId = cb::jsonGetField(diag4.arena, diag4Params, "diagnostics");
        REQUIRE(std::holds_alternative<cb::JsonArray>(diag4.arena.nodes[diag4ArrId]));
        CHECK(std::get<cb::JsonArray>(diag4.arena.nodes[diag4ArrId]).elements.empty());

        // Verify Message 6: shutdown response
        const auto shutdownRaw = cb::readJsonrpcMessage(serverToClient);
        REQUIRE(shutdownRaw.has_value());
        const auto shutdownResp = cb::parseJson(shutdownRaw.value());
        REQUIRE(shutdownResp.ok);
        CHECK(cb::jsonGetInt(shutdownResp.arena, cb::jsonGetField(shutdownResp.arena, shutdownResp.root, "id")) == 2);
        CHECK(cb::jsonIsNull(shutdownResp.arena, cb::jsonGetField(shutdownResp.arena, shutdownResp.root, "result")));
    }

    SECTION("In-memory document overlay resolves unsaved imported modules across open buffers") {
        std::stringstream clientToServer;
        std::stringstream serverToClient;

        // Open an unsaved module in memory (does not exist on disk)
        {
            cb::JsonArena arena;
            const cb::JsonId msg = cb::jsonAddObject(arena);
            cb::jsonObjectSet(arena, msg, "jsonrpc", cb::jsonAddString(arena, "2.0"));
            cb::jsonObjectSet(arena, msg, "method", cb::jsonAddString(arena, "textDocument/didOpen"));
            const cb::JsonId params = cb::jsonAddObject(arena);
            const cb::JsonId td = cb::jsonAddObject(arena);
            cb::jsonObjectSet(arena, td, "uri", cb::jsonAddString(arena, "file:///virtualWs/mathUtil.cb"));
            cb::jsonObjectSet(arena, td, "languageId", cb::jsonAddString(arena, "cleanbasic"));
            cb::jsonObjectSet(arena, td, "version", cb::jsonAddInt(arena, 1));
            cb::jsonObjectSet(arena, td, "text", cb::jsonAddString(arena, R"(pub fn doubleVal(x as int) as int
    return x * 2
end fn
)"));
            cb::jsonObjectSet(arena, params, "textDocument", td);
            cb::jsonObjectSet(arena, msg, "params", params);
            cb::writeJsonrpcMessage(clientToServer, cb::serializeJson(arena, msg));
        }

        // Open main.cb importing mathUtil.cb and calling mathUtil.doubleVal(21)
        {
            cb::JsonArena arena;
            const cb::JsonId msg = cb::jsonAddObject(arena);
            cb::jsonObjectSet(arena, msg, "jsonrpc", cb::jsonAddString(arena, "2.0"));
            cb::jsonObjectSet(arena, msg, "method", cb::jsonAddString(arena, "textDocument/didOpen"));
            const cb::JsonId params = cb::jsonAddObject(arena);
            const cb::JsonId td = cb::jsonAddObject(arena);
            cb::jsonObjectSet(arena, td, "uri", cb::jsonAddString(arena, "file:///virtualWs/main.cb"));
            cb::jsonObjectSet(arena, td, "languageId", cb::jsonAddString(arena, "cleanbasic"));
            cb::jsonObjectSet(arena, td, "version", cb::jsonAddInt(arena, 1));
            cb::jsonObjectSet(arena, td, "text", cb::jsonAddString(arena, R"(import "mathUtil.cb"
fn main() as int
    return mathUtil.doubleVal(21)
end fn
)"));
            cb::jsonObjectSet(arena, params, "textDocument", td);
            cb::jsonObjectSet(arena, msg, "params", params);
            cb::writeJsonrpcMessage(clientToServer, cb::serializeJson(arena, msg));
        }

        // Modify mathUtil.cb in memory to remove 'pub' from doubleVal, then re-validate main.cb
        {
            cb::JsonArena arena;
            const cb::JsonId msg = cb::jsonAddObject(arena);
            cb::jsonObjectSet(arena, msg, "jsonrpc", cb::jsonAddString(arena, "2.0"));
            cb::jsonObjectSet(arena, msg, "method", cb::jsonAddString(arena, "textDocument/didChange"));
            const cb::JsonId params = cb::jsonAddObject(arena);
            const cb::JsonId td = cb::jsonAddObject(arena);
            cb::jsonObjectSet(arena, td, "uri", cb::jsonAddString(arena, "file:///virtualWs/mathUtil.cb"));
            cb::jsonObjectSet(arena, td, "version", cb::jsonAddInt(arena, 2));
            cb::jsonObjectSet(arena, params, "textDocument", td);
            const cb::JsonId changes = cb::jsonAddArray(arena);
            const cb::JsonId changeItem = cb::jsonAddObject(arena);
            cb::jsonObjectSet(arena, changeItem, "text", cb::jsonAddString(arena, R"(fn doubleVal(x as int) as int
    return x * 2
end fn
)"));
            cb::jsonArrayPush(arena, changes, changeItem);
            cb::jsonObjectSet(arena, params, "contentChanges", changes);
            cb::jsonObjectSet(arena, msg, "params", params);
            cb::writeJsonrpcMessage(clientToServer, cb::serializeJson(arena, msg));
        }
        {
            cb::JsonArena arena;
            const cb::JsonId msg = cb::jsonAddObject(arena);
            cb::jsonObjectSet(arena, msg, "jsonrpc", cb::jsonAddString(arena, "2.0"));
            cb::jsonObjectSet(arena, msg, "method", cb::jsonAddString(arena, "textDocument/didChange"));
            const cb::JsonId params = cb::jsonAddObject(arena);
            const cb::JsonId td = cb::jsonAddObject(arena);
            cb::jsonObjectSet(arena, td, "uri", cb::jsonAddString(arena, "file:///virtualWs/main.cb"));
            cb::jsonObjectSet(arena, td, "version", cb::jsonAddInt(arena, 2));
            cb::jsonObjectSet(arena, params, "textDocument", td);
            const cb::JsonId changes = cb::jsonAddArray(arena);
            const cb::JsonId changeItem = cb::jsonAddObject(arena);
            cb::jsonObjectSet(arena, changeItem, "text", cb::jsonAddString(arena, R"(import "mathUtil.cb"
fn main() as int
    return mathUtil.doubleVal(21)
end fn
)"));
            cb::jsonArrayPush(arena, changes, changeItem);
            cb::jsonObjectSet(arena, params, "contentChanges", changes);
            cb::jsonObjectSet(arena, msg, "params", params);
            cb::writeJsonrpcMessage(clientToServer, cb::serializeJson(arena, msg));
        }

        // Send exit without shutdown -> should return exit code 1
        cb::writeJsonrpcMessage(clientToServer, R"({"jsonrpc":"2.0","method":"exit"})");

        const int exitCode = cb::runLspServer(clientToServer, serverToClient);
        CHECK(exitCode == 1);

        // 1st notification: mathUtil.cb didOpen -> 0 diagnostics
        const auto m1 = cb::parseJson(cb::readJsonrpcMessage(serverToClient).value());
        const cb::JsonId m1Diags = cb::jsonGetField(m1.arena, cb::jsonGetField(m1.arena, m1.root, "params"), "diagnostics");
        CHECK(std::get<cb::JsonArray>(m1.arena.nodes[m1Diags]).elements.empty());

        // 2nd notification: main.cb didOpen -> 0 diagnostics (resolved mathUtil.cb from in-memory buffer!)
        const auto m2 = cb::parseJson(cb::readJsonrpcMessage(serverToClient).value());
        const cb::JsonId m2Diags = cb::jsonGetField(m2.arena, cb::jsonGetField(m2.arena, m2.root, "params"), "diagnostics");
        CHECK(std::get<cb::JsonArray>(m2.arena.nodes[m2Diags]).elements.empty());

        // 3rd notification: mathUtil.cb didChange -> 0 diagnostics
        const auto m3 = cb::parseJson(cb::readJsonrpcMessage(serverToClient).value());
        const cb::JsonId m3Diags = cb::jsonGetField(m3.arena, cb::jsonGetField(m3.arena, m3.root, "params"), "diagnostics");
        CHECK(std::get<cb::JsonArray>(m3.arena.nodes[m3Diags]).elements.empty());

        // 4th notification: main.cb didChange -> reports private function AnalyzerError from updated in-memory mathUtil.cb!
        const auto m4 = cb::parseJson(cb::readJsonrpcMessage(serverToClient).value());
        const cb::JsonId m4Diags = cb::jsonGetField(m4.arena, cb::jsonGetField(m4.arena, m4.root, "params"), "diagnostics");
        const auto& m4Elems = std::get<cb::JsonArray>(m4.arena.nodes[m4Diags]).elements;
        REQUIRE(m4Elems.size() == 1);
        const auto m4Msg = cb::jsonGetString(m4.arena, cb::jsonGetField(m4.arena, m4Elems[0], "message"));
        REQUIRE(m4Msg.has_value());
        CHECK(m4Msg.value().find("private to module") != std::string::npos);
    }
}

TEST_CASE("Implicit multiline continuation inside (), [], and {} and trailing parameter commas", "[lexer][parser][multiline]") {
    SECTION("Multiline function params with trailing comma, struct constructors, list/dict literals, and parenthesized expressions transpile") {
        const std::string src = R"(type Config
    host as str
    port as int
    retries as int
end type

fn makeConfig(
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
    dim cfg = makeConfig(
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
        const auto res = cb::transpileSource(src, "multiline.cb");
        REQUIRE(res.ok);
        CHECK(res.cppCode.find("Config makeConfig(const std::string& host, int64_t port, int64_t retries)") != std::string::npos);
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
    undeclaredVar = items[0]
    return 0
end fn
)";
        const auto checkRes = cb::checkSource(src, "diagMultiline.cb");
        REQUIRE_FALSE(checkRes.ok);
        CHECK(checkRes.error.kind == "AnalyzerError");
        CHECK(checkRes.error.line == 9);
        CHECK(checkRes.error.col == 5);
        CHECK(checkRes.error.message.find("Undeclared variable 'undeclaredVar'") != std::string::npos);
    }
}

TEST_CASE("CleanBasic Language Server Phase 2: documentSymbol, hover, definition & snapshot fallback", "[lsp]") {
    SECTION("End-to-end documentSymbol, hover, definition (same-file, cross-module, struct fields), and snapshot resilience") {
        std::stringstream clientToServer;
        std::stringstream serverToClient;

        // 1. initialize (id = 1)
        cb::writeJsonrpcMessage(
            clientToServer,
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
            const cb::JsonId msg = cb::jsonAddObject(arena);
            cb::jsonObjectSet(arena, msg, "jsonrpc", cb::jsonAddString(arena, "2.0"));
            cb::jsonObjectSet(arena, msg, "method", cb::jsonAddString(arena, "textDocument/didOpen"));
            const cb::JsonId params = cb::jsonAddObject(arena);
            const cb::JsonId td = cb::jsonAddObject(arena);
            cb::jsonObjectSet(arena, td, "uri", cb::jsonAddString(arena, "file:///ws/player.cb"));
            cb::jsonObjectSet(arena, td, "languageId", cb::jsonAddString(arena, "cleanbasic"));
            cb::jsonObjectSet(arena, td, "version", cb::jsonAddInt(arena, 1));
            cb::jsonObjectSet(arena, td, "text", cb::jsonAddString(arena, R"(pub alias Score = int
pub type Player
    name as str
    health as int
end type
pub fn heal(byref p as Player, amount as int) as int can error
    p.health = p.health + amount
    return p.health
end fn
)"));
            cb::jsonObjectSet(arena, params, "textDocument", td);
            cb::jsonObjectSet(arena, msg, "params", params);
            cb::writeJsonrpcMessage(clientToServer, cb::serializeJson(arena, msg));
        }

        // 3. didOpen main.cb
        // Line 1 (0-based 0): import "player.cb"
        // Line 2 (0-based 1): fn localBoost(base as int) as int
        // Line 3 (0-based 2):     return base + 10
        // Line 4 (0-based 3): end fn
        // Line 5 (0-based 4): fn main() as int can error
        // Line 6 (0-based 5):     const maxPotion = 50
        // Line 7 (0-based 6):     dim hero = player.Player(name="Alice", health=100)
        // Line 8 (0-based 7):     dim boosted = localBoost(maxPotion)
        // Line 9 (0-based 8):     dim newHp = hero.heal(boosted)
        // Line 10 (0-based 9):    print(hero.health, newHp)
        // Line 11 (0-based 10):   return 0
        // Line 12 (0-based 11): end fn
        {
            cb::JsonArena arena;
            const cb::JsonId msg = cb::jsonAddObject(arena);
            cb::jsonObjectSet(arena, msg, "jsonrpc", cb::jsonAddString(arena, "2.0"));
            cb::jsonObjectSet(arena, msg, "method", cb::jsonAddString(arena, "textDocument/didOpen"));
            const cb::JsonId params = cb::jsonAddObject(arena);
            const cb::JsonId td = cb::jsonAddObject(arena);
            cb::jsonObjectSet(arena, td, "uri", cb::jsonAddString(arena, "file:///ws/main.cb"));
            cb::jsonObjectSet(arena, td, "languageId", cb::jsonAddString(arena, "cleanbasic"));
            cb::jsonObjectSet(arena, td, "version", cb::jsonAddInt(arena, 1));
            cb::jsonObjectSet(arena, td, "text", cb::jsonAddString(arena, R"(import "player.cb"
fn localBoost(base as int) as int
    return base + 10
end fn
fn main() as int can error
    const maxPotion = 50
    dim hero = player.Player(name="Alice", health=100)
    dim boosted = localBoost(maxPotion)
    dim newHp = hero.heal(boosted)
    print(hero.health, newHp)
    return 0
end fn
)"));
            cb::jsonObjectSet(arena, params, "textDocument", td);
            cb::jsonObjectSet(arena, msg, "params", params);
            cb::writeJsonrpcMessage(clientToServer, cb::serializeJson(arena, msg));
        }

        // 4. textDocument/documentSymbol on player.cb (id = 2)
        cb::writeJsonrpcMessage(
            clientToServer,
            R"({"jsonrpc":"2.0","id":2,"method":"textDocument/documentSymbol","params":{"textDocument":{"uri":"file:///ws/player.cb"}}})"
        );

        // 5. textDocument/hover on inferred local variable 'hero' in main.cb (0-based line 6, char 9) (id = 3)
        cb::writeJsonrpcMessage(
            clientToServer,
            R"({"jsonrpc":"2.0","id":3,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///ws/main.cb"},"position":{"line":6,"character":9}}})"
        );

        // 6. textDocument/hover on UFCS call 'hero.heal' in main.cb (0-based line 8, char 23 -> 'heal') (id = 4)
        cb::writeJsonrpcMessage(
            clientToServer,
            R"({"jsonrpc":"2.0","id":4,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///ws/main.cb"},"position":{"line":8,"character":23}}})"
        );

        // 7. textDocument/hover on struct field 'hero.health' in main.cb (0-based line 9, char 16 -> 'health') (id = 5)
        cb::writeJsonrpcMessage(
            clientToServer,
            R"({"jsonrpc":"2.0","id":5,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///ws/main.cb"},"position":{"line":9,"character":16}}})"
        );

        // 8. textDocument/hover on builtin 'print' in main.cb (0-based line 9, char 5) (id = 6)
        cb::writeJsonrpcMessage(
            clientToServer,
            R"({"jsonrpc":"2.0","id":6,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///ws/main.cb"},"position":{"line":9,"character":5}}})"
        );

        // 9. textDocument/definition on same-file function call 'localBoost' (0-based line 7, char 20) (id = 7)
        cb::writeJsonrpcMessage(
            clientToServer,
            R"({"jsonrpc":"2.0","id":7,"method":"textDocument/definition","params":{"textDocument":{"uri":"file:///ws/main.cb"},"position":{"line":7,"character":20}}})"
        );

        // 10. textDocument/definition on cross-module UFCS call 'hero.heal' (0-based line 8, char 23) (id = 8)
        cb::writeJsonrpcMessage(
            clientToServer,
            R"({"jsonrpc":"2.0","id":8,"method":"textDocument/definition","params":{"textDocument":{"uri":"file:///ws/main.cb"},"position":{"line":8,"character":23}}})"
        );

        // 11. textDocument/definition on struct field 'hero.health' (0-based line 9, char 16) (id = 9)
        cb::writeJsonrpcMessage(
            clientToServer,
            R"({"jsonrpc":"2.0","id":9,"method":"textDocument/definition","params":{"textDocument":{"uri":"file:///ws/main.cb"},"position":{"line":9,"character":16}}})"
        );

        // 12. textDocument/definition on local const 'maxPotion' usage (0-based line 7, char 32) (id = 10)
        cb::writeJsonrpcMessage(
            clientToServer,
            R"({"jsonrpc":"2.0","id":10,"method":"textDocument/definition","params":{"textDocument":{"uri":"file:///ws/main.cb"},"position":{"line":7,"character":32}}})"
        );

        // 13. Introduce transient syntax error in main.cb via didChange, then request documentSymbol (id = 11) and hover on 'hero' (id = 12)
        {
            cb::JsonArena arena;
            const cb::JsonId msg = cb::jsonAddObject(arena);
            cb::jsonObjectSet(arena, msg, "jsonrpc", cb::jsonAddString(arena, "2.0"));
            cb::jsonObjectSet(arena, msg, "method", cb::jsonAddString(arena, "textDocument/didChange"));
            const cb::JsonId params = cb::jsonAddObject(arena);
            const cb::JsonId td = cb::jsonAddObject(arena);
            cb::jsonObjectSet(arena, td, "uri", cb::jsonAddString(arena, "file:///ws/main.cb"));
            cb::jsonObjectSet(arena, td, "version", cb::jsonAddInt(arena, 2));
            cb::jsonObjectSet(arena, params, "textDocument", td);
            const cb::JsonId changes = cb::jsonAddArray(arena);
            const cb::JsonId changeItem = cb::jsonAddObject(arena);
            cb::jsonObjectSet(arena, changeItem, "text", cb::jsonAddString(arena, R"(import "player.cb"
fn localBoost(base as int) as int
    return base + 10
end fn
fn main() as int can error
    const maxPotion = 50
    dim hero = player.Player(name="Alice", health=100)
    if
)"));
            cb::jsonArrayPush(arena, changes, changeItem);
            cb::jsonObjectSet(arena, params, "contentChanges", changes);
            cb::jsonObjectSet(arena, msg, "params", params);
            cb::writeJsonrpcMessage(clientToServer, cb::serializeJson(arena, msg));
        }

        cb::writeJsonrpcMessage(
            clientToServer,
            R"({"jsonrpc":"2.0","id":11,"method":"textDocument/documentSymbol","params":{"textDocument":{"uri":"file:///ws/main.cb"}}})"
        );
        cb::writeJsonrpcMessage(
            clientToServer,
            R"({"jsonrpc":"2.0","id":12,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///ws/main.cb"},"position":{"line":6,"character":9}}})"
        );

        // 14. shutdown & exit
        cb::writeJsonrpcMessage(clientToServer, R"({"jsonrpc":"2.0","id":99,"method":"shutdown","params":null})");
        cb::writeJsonrpcMessage(clientToServer, R"({"jsonrpc":"2.0","method":"exit"})");

        const int exitCode = cb::runLspServer(clientToServer, serverToClient);
        CHECK(exitCode == 0);

        // Msg 1: initialize response capabilities
        const auto initResp = cb::parseJson(cb::readJsonrpcMessage(serverToClient).value());
        REQUIRE(initResp.ok);
        const cb::JsonId caps = cb::jsonGetField(
            initResp.arena,
            cb::jsonGetField(initResp.arena, initResp.root, "result"),
            "capabilities"
        );
        CHECK(cb::jsonGetBool(initResp.arena, cb::jsonGetField(initResp.arena, caps, "documentSymbolProvider")) == true);
        CHECK(cb::jsonGetBool(initResp.arena, cb::jsonGetField(initResp.arena, caps, "definitionProvider")) == true);
        CHECK(cb::jsonGetBool(initResp.arena, cb::jsonGetField(initResp.arena, caps, "hoverProvider")) == true);

        // Msg 2 & 3: publishDiagnostics for player.cb and main.cb (both valid)
        const auto diagPlayer = cb::parseJson(cb::readJsonrpcMessage(serverToClient).value());
        REQUIRE(diagPlayer.ok);
        const auto diagMain = cb::parseJson(cb::readJsonrpcMessage(serverToClient).value());
        REQUIRE(diagMain.ok);

        // Msg 4: documentSymbol response for player.cb (id = 2)
        const auto docSymResp = cb::parseJson(cb::readJsonrpcMessage(serverToClient).value());
        REQUIRE(docSymResp.ok);
        CHECK(cb::jsonGetInt(docSymResp.arena, cb::jsonGetField(docSymResp.arena, docSymResp.root, "id")) == 2);
        const cb::JsonId symArrId = cb::jsonGetField(docSymResp.arena, docSymResp.root, "result");
        REQUIRE(std::holds_alternative<cb::JsonArray>(docSymResp.arena.nodes[symArrId]));
        const auto& symElems = std::get<cb::JsonArray>(docSymResp.arena.nodes[symArrId]).elements;
        REQUIRE(symElems.size() == 3);

        // Symbol 0: Alias 'Score' (kind 26)
        CHECK(cb::jsonGetString(docSymResp.arena, cb::jsonGetField(docSymResp.arena, symElems[0], "name")) == "Score");
        CHECK(cb::jsonGetInt(docSymResp.arena, cb::jsonGetField(docSymResp.arena, symElems[0], "kind")) == 26);
        CHECK(cb::jsonGetString(docSymResp.arena, cb::jsonGetField(docSymResp.arena, symElems[0], "detail")) == "= int");

        // Symbol 1: Type 'Player' (kind 23) with 2 Field children (kind 8)
        CHECK(cb::jsonGetString(docSymResp.arena, cb::jsonGetField(docSymResp.arena, symElems[1], "name")) == "Player");
        CHECK(cb::jsonGetInt(docSymResp.arena, cb::jsonGetField(docSymResp.arena, symElems[1], "kind")) == 23);
        const cb::JsonId childrenId = cb::jsonGetField(docSymResp.arena, symElems[1], "children");
        REQUIRE(std::holds_alternative<cb::JsonArray>(docSymResp.arena.nodes[childrenId]));
        const auto& fieldChildren = std::get<cb::JsonArray>(docSymResp.arena.nodes[childrenId]).elements;
        REQUIRE(fieldChildren.size() == 2);
        CHECK(cb::jsonGetString(docSymResp.arena, cb::jsonGetField(docSymResp.arena, fieldChildren[0], "name")) == "name");
        CHECK(cb::jsonGetInt(docSymResp.arena, cb::jsonGetField(docSymResp.arena, fieldChildren[0], "kind")) == 8);
        CHECK(cb::jsonGetString(docSymResp.arena, cb::jsonGetField(docSymResp.arena, fieldChildren[0], "detail")) == "str");
        CHECK(cb::jsonGetString(docSymResp.arena, cb::jsonGetField(docSymResp.arena, fieldChildren[1], "name")) == "health");
        CHECK(cb::jsonGetString(docSymResp.arena, cb::jsonGetField(docSymResp.arena, fieldChildren[1], "detail")) == "int");

        // Symbol 2: Fn 'heal' (kind 12)
        CHECK(cb::jsonGetString(docSymResp.arena, cb::jsonGetField(docSymResp.arena, symElems[2], "name")) == "heal");
        CHECK(cb::jsonGetInt(docSymResp.arena, cb::jsonGetField(docSymResp.arena, symElems[2], "kind")) == 12);
        CHECK(cb::jsonGetString(docSymResp.arena, cb::jsonGetField(docSymResp.arena, symElems[2], "detail")) == "(byref p as Player, amount as int) as int can error");

        // Msg 5: hover on inferred variable 'hero' (id = 3)
        const auto hoverHero = cb::parseJson(cb::readJsonrpcMessage(serverToClient).value());
        REQUIRE(hoverHero.ok);
        const cb::JsonId h3Contents = cb::jsonGetField(
            hoverHero.arena,
            cb::jsonGetField(hoverHero.arena, hoverHero.root, "result"),
            "contents"
        );
        const auto h3Val = cb::jsonGetString(hoverHero.arena, cb::jsonGetField(hoverHero.arena, h3Contents, "value"));
        REQUIRE(h3Val.has_value());
        CHECK(h3Val.value().find("dim hero as Player") != std::string::npos);

        // Msg 6: hover on UFCS call 'hero.heal' (id = 4)
        const auto hoverHeal = cb::parseJson(cb::readJsonrpcMessage(serverToClient).value());
        REQUIRE(hoverHeal.ok);
        const cb::JsonId h4Contents = cb::jsonGetField(
            hoverHeal.arena,
            cb::jsonGetField(hoverHeal.arena, hoverHeal.root, "result"),
            "contents"
        );
        const auto h4Val = cb::jsonGetString(hoverHeal.arena, cb::jsonGetField(hoverHeal.arena, h4Contents, "value"));
        REQUIRE(h4Val.has_value());
        CHECK(h4Val.value().find("pub fn heal(byref p as Player, amount as int) as int can error") != std::string::npos);

        // Msg 7: hover on struct field 'hero.health' (id = 5)
        const auto hoverField = cb::parseJson(cb::readJsonrpcMessage(serverToClient).value());
        REQUIRE(hoverField.ok);
        const cb::JsonId h5Contents = cb::jsonGetField(
            hoverField.arena,
            cb::jsonGetField(hoverField.arena, hoverField.root, "result"),
            "contents"
        );
        const auto h5Val = cb::jsonGetString(hoverField.arena, cb::jsonGetField(hoverField.arena, h5Contents, "value"));
        REQUIRE(h5Val.has_value());
        CHECK(h5Val.value().find("(field) Player.health as int") != std::string::npos);

        // Msg 8: hover on builtin 'print' (id = 6)
        const auto hoverPrint = cb::parseJson(cb::readJsonrpcMessage(serverToClient).value());
        REQUIRE(hoverPrint.ok);
        const cb::JsonId h6Contents = cb::jsonGetField(
            hoverPrint.arena,
            cb::jsonGetField(hoverPrint.arena, hoverPrint.root, "result"),
            "contents"
        );
        const auto h6Val = cb::jsonGetString(hoverPrint.arena, cb::jsonGetField(hoverPrint.arena, h6Contents, "value"));
        REQUIRE(h6Val.has_value());
        CHECK(h6Val.value().find("fn print(values...)") != std::string::npos);

        // Msg 9: definition on same-file 'localBoost' (id = 7) -> main.cb, 0-based line 1, char 3
        const auto defLocalFn = cb::parseJson(cb::readJsonrpcMessage(serverToClient).value());
        REQUIRE(defLocalFn.ok);
        const cb::JsonId def7Res = cb::jsonGetField(defLocalFn.arena, defLocalFn.root, "result");
        CHECK(cb::jsonGetString(defLocalFn.arena, cb::jsonGetField(defLocalFn.arena, def7Res, "uri")) == "file:///ws/main.cb");
        const cb::JsonId def7Start = cb::jsonGetField(
            defLocalFn.arena,
            cb::jsonGetField(defLocalFn.arena, def7Res, "range"),
            "start"
        );
        CHECK(cb::jsonGetInt(defLocalFn.arena, cb::jsonGetField(defLocalFn.arena, def7Start, "line")) == 1);
        CHECK(cb::jsonGetInt(defLocalFn.arena, cb::jsonGetField(defLocalFn.arena, def7Start, "character")) == 3);

        // Msg 10: definition on cross-module UFCS 'hero.heal' (id = 8) -> player.cb, 0-based line 5, char 7
        const auto defUfcs = cb::parseJson(cb::readJsonrpcMessage(serverToClient).value());
        REQUIRE(defUfcs.ok);
        const cb::JsonId def8Res = cb::jsonGetField(defUfcs.arena, defUfcs.root, "result");
        CHECK(cb::jsonGetString(defUfcs.arena, cb::jsonGetField(defUfcs.arena, def8Res, "uri")) == "file:///ws/player.cb");
        const cb::JsonId def8Start = cb::jsonGetField(
            defUfcs.arena,
            cb::jsonGetField(defUfcs.arena, def8Res, "range"),
            "start"
        );
        CHECK(cb::jsonGetInt(defUfcs.arena, cb::jsonGetField(defUfcs.arena, def8Start, "line")) == 5);
        CHECK(cb::jsonGetInt(defUfcs.arena, cb::jsonGetField(defUfcs.arena, def8Start, "character")) == 7);

        // Msg 11: definition on struct field 'hero.health' (id = 9) -> player.cb, 0-based line 3, char 4
        const auto defField = cb::parseJson(cb::readJsonrpcMessage(serverToClient).value());
        REQUIRE(defField.ok);
        const cb::JsonId def9Res = cb::jsonGetField(defField.arena, defField.root, "result");
        CHECK(cb::jsonGetString(defField.arena, cb::jsonGetField(defField.arena, def9Res, "uri")) == "file:///ws/player.cb");
        const cb::JsonId def9Start = cb::jsonGetField(
            defField.arena,
            cb::jsonGetField(defField.arena, def9Res, "range"),
            "start"
        );
        CHECK(cb::jsonGetInt(defField.arena, cb::jsonGetField(defField.arena, def9Start, "line")) == 3);
        CHECK(cb::jsonGetInt(defField.arena, cb::jsonGetField(defField.arena, def9Start, "character")) == 4);

        // Msg 12: definition on local const 'maxPotion' (id = 10) -> main.cb, 0-based line 5, char 10
        const auto defVar = cb::parseJson(cb::readJsonrpcMessage(serverToClient).value());
        REQUIRE(defVar.ok);
        const cb::JsonId def10Res = cb::jsonGetField(defVar.arena, defVar.root, "result");
        CHECK(cb::jsonGetString(defVar.arena, cb::jsonGetField(defVar.arena, def10Res, "uri")) == "file:///ws/main.cb");
        const cb::JsonId def10Start = cb::jsonGetField(
            defVar.arena,
            cb::jsonGetField(defVar.arena, def10Res, "range"),
            "start"
        );
        CHECK(cb::jsonGetInt(defVar.arena, cb::jsonGetField(defVar.arena, def10Start, "line")) == 5);
        CHECK(cb::jsonGetInt(defVar.arena, cb::jsonGetField(defVar.arena, def10Start, "character")) == 10);

        // Msg 13: publishDiagnostics for broken main.cb after didChange
        const auto diagBroken = cb::parseJson(cb::readJsonrpcMessage(serverToClient).value());
        REQUIRE(diagBroken.ok);

        // Msg 14: documentSymbol on main.cb while broken (id = 11) -> falls back to last-known-good snapshot!
        const auto symFallback = cb::parseJson(cb::readJsonrpcMessage(serverToClient).value());
        REQUIRE(symFallback.ok);
        const cb::JsonId fbSymArr = cb::jsonGetField(symFallback.arena, symFallback.root, "result");
        REQUIRE(std::holds_alternative<cb::JsonArray>(symFallback.arena.nodes[fbSymArr]));
        CHECK(std::get<cb::JsonArray>(symFallback.arena.nodes[fbSymArr]).elements.size() == 2);

        // Msg 15: hover on 'hero' while main.cb has syntax error on next line (id = 12) -> falls back to last-known-good snapshot!
        const auto hoverFallback = cb::parseJson(cb::readJsonrpcMessage(serverToClient).value());
        REQUIRE(hoverFallback.ok);
        const cb::JsonId h12Contents = cb::jsonGetField(
            hoverFallback.arena,
            cb::jsonGetField(hoverFallback.arena, hoverFallback.root, "result"),
            "contents"
        );
        const auto h12Val = cb::jsonGetString(hoverFallback.arena, cb::jsonGetField(hoverFallback.arena, h12Contents, "value"));
        REQUIRE(h12Val.has_value());
        CHECK(h12Val.value().find("dim hero as Player") != std::string::npos);
    }
}

TEST_CASE("CleanBasic Language Server Phase 3: completion (dot UFCS/fields/modules/err/list, named args, types, scope) & signatureHelp", "[lsp]") {
    auto hasCompletionItem = [](
        const cb::JsonArena& arena,
        cb::JsonId arrId,
        std::string_view label,
        int64_t expectedKind = -1,
        std::string_view detailSubstr = ""
    ) -> bool {
        if (arrId >= arena.nodes.size() || !std::holds_alternative<cb::JsonArray>(arena.nodes[arrId])) {
            return false;
        }
        const auto& elems = std::get<cb::JsonArray>(arena.nodes[arrId]).elements;
        for (const cb::JsonId itemId : elems) {
            const auto lbl = cb::jsonGetString(arena, cb::jsonGetField(arena, itemId, "label"));
            if (!lbl.has_value() || lbl.value() != label) {
                continue;
            }
            if (expectedKind >= 0) {
                const auto k = cb::jsonGetInt(arena, cb::jsonGetField(arena, itemId, "kind"));
                if (!k.has_value() || k.value() != expectedKind) {
                    continue;
                }
            }
            if (!detailSubstr.empty()) {
                const auto det = cb::jsonGetString(arena, cb::jsonGetField(arena, itemId, "detail"));
                if (!det.has_value() || det.value().find(detailSubstr) == std::string::npos) {
                    continue;
                }
            }
            return true;
        }
        return false;
    };

    SECTION("Two-stage dot-scrub recovery on brand-new variable, UFCS & struct fields, module exports, err/list, named args, types, and signatureHelp") {
        std::stringstream clientToServer;
        std::stringstream serverToClient;

        // 1. initialize (id = 1)
        cb::writeJsonrpcMessage(
            clientToServer,
            R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":"file:///ws"}})"
        );

        // 2. didOpen player.cb
        {
            cb::JsonArena arena;
            const cb::JsonId msg = cb::jsonAddObject(arena);
            cb::jsonObjectSet(arena, msg, "jsonrpc", cb::jsonAddString(arena, "2.0"));
            cb::jsonObjectSet(arena, msg, "method", cb::jsonAddString(arena, "textDocument/didOpen"));
            const cb::JsonId params = cb::jsonAddObject(arena);
            const cb::JsonId td = cb::jsonAddObject(arena);
            cb::jsonObjectSet(arena, td, "uri", cb::jsonAddString(arena, "file:///ws/player.cb"));
            cb::jsonObjectSet(arena, td, "languageId", cb::jsonAddString(arena, "cleanbasic"));
            cb::jsonObjectSet(arena, td, "version", cb::jsonAddInt(arena, 1));
            cb::jsonObjectSet(arena, td, "text", cb::jsonAddString(arena, R"(pub alias Score = int
pub type Player
    name as str
    health as int
end type
pub fn heal(byref p as Player, amount as int, bonus as int) as int can error
    p.health = p.health + amount + bonus
    return p.health
end fn
fn secretHelper(p as Player) as int
    return p.health
end fn
)"));
            cb::jsonObjectSet(arena, params, "textDocument", td);
            cb::jsonObjectSet(arena, msg, "params", params);
            cb::writeJsonrpcMessage(clientToServer, cb::serializeJson(arena, msg));
        }

        // 3. didOpen main.cb directly with a brand-new variable 'hero' and an incomplete trailing dot 'hero.' on the very next line!
        // Note: main.cb has NO prior cached snapshot when 'hero.' is typed!
        // Line 0: import "player.cb"
        // Line 1: fn localBuff(byref p as player.Player, shield as int)
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
            const cb::JsonId msg = cb::jsonAddObject(arena);
            cb::jsonObjectSet(arena, msg, "jsonrpc", cb::jsonAddString(arena, "2.0"));
            cb::jsonObjectSet(arena, msg, "method", cb::jsonAddString(arena, "textDocument/didOpen"));
            const cb::JsonId params = cb::jsonAddObject(arena);
            const cb::JsonId td = cb::jsonAddObject(arena);
            cb::jsonObjectSet(arena, td, "uri", cb::jsonAddString(arena, "file:///ws/main.cb"));
            cb::jsonObjectSet(arena, td, "languageId", cb::jsonAddString(arena, "cleanbasic"));
            cb::jsonObjectSet(arena, td, "version", cb::jsonAddInt(arena, 1));
            cb::jsonObjectSet(arena, td, "text", cb::jsonAddString(arena, R"(import "player.cb"
fn localBuff(byref p as player.Player, shield as int)
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
            cb::jsonObjectSet(arena, params, "textDocument", td);
            cb::jsonObjectSet(arena, msg, "params", params);
            cb::writeJsonrpcMessage(clientToServer, cb::serializeJson(arena, msg));
        }

        // Req id=2: textDocument/completion at 'hero.' (line 10, character 9)
        cb::writeJsonrpcMessage(
            clientToServer,
            R"({"jsonrpc":"2.0","id":2,"method":"textDocument/completion","params":{"textDocument":{"uri":"file:///ws/main.cb"},"position":{"line":10,"character":9}}})"
        );

        // 4. didChange main.cb to test module alias completion 'player.' (line 10, char 11)
        auto sendMainChange = [&](int version, std::string_view newLine10) {
            cb::JsonArena arena;
            const cb::JsonId msg = cb::jsonAddObject(arena);
            cb::jsonObjectSet(arena, msg, "jsonrpc", cb::jsonAddString(arena, "2.0"));
            cb::jsonObjectSet(arena, msg, "method", cb::jsonAddString(arena, "textDocument/didChange"));
            const cb::JsonId params = cb::jsonAddObject(arena);
            const cb::JsonId td = cb::jsonAddObject(arena);
            cb::jsonObjectSet(arena, td, "uri", cb::jsonAddString(arena, "file:///ws/main.cb"));
            cb::jsonObjectSet(arena, td, "version", cb::jsonAddInt(arena, version));
            cb::jsonObjectSet(arena, params, "textDocument", td);
            const cb::JsonId changes = cb::jsonAddArray(arena);
            const cb::JsonId changeItem = cb::jsonAddObject(arena);
            std::string fullSrc = R"(import "player.cb"
fn localBuff(byref p as player.Player, shield as int)
    p.health = p.health + shield
end fn
fn main() as int
    on error e
        return 1
    end error
    dim nums as list[int] = [1, 2]
    dim hero = player.Player(name="Alice", health=100)
)";
            fullSrc += std::string(newLine10) + "\n    return 0\nend fn\n";
            cb::jsonObjectSet(arena, changeItem, "text", cb::jsonAddString(arena, fullSrc));
            cb::jsonArrayPush(arena, changes, changeItem);
            cb::jsonObjectSet(arena, params, "contentChanges", changes);
            cb::jsonObjectSet(arena, msg, "params", params);
            cb::writeJsonrpcMessage(clientToServer, cb::serializeJson(arena, msg));
        };

        // Change v2: '    player.' (line 10, char 11) -> Req id=3
        sendMainChange(2, "    player.");
        cb::writeJsonrpcMessage(
            clientToServer,
            R"({"jsonrpc":"2.0","id":3,"method":"textDocument/completion","params":{"textDocument":{"uri":"file:///ws/main.cb"},"position":{"line":10,"character":11}}})"
        );

        // Change v3: '    e.' (line 10, char 6) -> Req id=4 (built-in err fields .msg and .code)
        sendMainChange(3, "    e.");
        cb::writeJsonrpcMessage(
            clientToServer,
            R"({"jsonrpc":"2.0","id":4,"method":"textDocument/completion","params":{"textDocument":{"uri":"file:///ws/main.cb"},"position":{"line":10,"character":6}}})"
        );

        // Change v4: '    nums.' (line 10, char 9) -> Req id=5 (built-in list[int] methods .push and .append)
        sendMainChange(4, "    nums.");
        cb::writeJsonrpcMessage(
            clientToServer,
            R"({"jsonrpc":"2.0","id":5,"method":"textDocument/completion","params":{"textDocument":{"uri":"file:///ws/main.cb"},"position":{"line":10,"character":9}}})"
        );

        // Change v5: '    dim p2 = player.Player(' (line 10, char 27) -> Req id=6 (named field= completion) & Req id=7 (signatureHelp on struct constructor)
        sendMainChange(5, "    dim p2 = player.Player(");
        cb::writeJsonrpcMessage(
            clientToServer,
            R"({"jsonrpc":"2.0","id":6,"method":"textDocument/completion","params":{"textDocument":{"uri":"file:///ws/main.cb"},"position":{"line":10,"character":27}}})"
        );
        cb::writeJsonrpcMessage(
            clientToServer,
            R"({"jsonrpc":"2.0","id":7,"method":"textDocument/signatureHelp","params":{"textDocument":{"uri":"file:///ws/main.cb"},"position":{"line":10,"character":27}}})"
        );

        // Change v6: '    hero.heal(25, ' (line 10, char 18) -> Req id=8 (UFCS signatureHelp at 2nd visible parameter: activeParameter == 1)
        sendMainChange(6, "    hero.heal(25, ");
        cb::writeJsonrpcMessage(
            clientToServer,
            R"({"jsonrpc":"2.0","id":8,"method":"textDocument/signatureHelp","params":{"textDocument":{"uri":"file:///ws/main.cb"},"position":{"line":10,"character":18}}})"
        );

        // Change v7: '    dim x as ' (line 10, char 13) -> Req id=9 (type annotation completion after 'as ')
        sendMainChange(7, "    dim x as ");
        cb::writeJsonrpcMessage(
            clientToServer,
            R"({"jsonrpc":"2.0","id":9,"method":"textDocument/completion","params":{"textDocument":{"uri":"file:///ws/main.cb"},"position":{"line":10,"character":13}}})"
        );

        // Change v8: '    he' (line 10, char 6) -> Req id=10 (general scope completion filtered by prefix 'he' -> finds 'hero')
        sendMainChange(8, "    he");
        cb::writeJsonrpcMessage(
            clientToServer,
            R"({"jsonrpc":"2.0","id":10,"method":"textDocument/completion","params":{"textDocument":{"uri":"file:///ws/main.cb"},"position":{"line":10,"character":6}}})"
        );

        // shutdown & exit
        cb::writeJsonrpcMessage(clientToServer, R"({"jsonrpc":"2.0","id":99,"method":"shutdown","params":null})");
        cb::writeJsonrpcMessage(clientToServer, R"({"jsonrpc":"2.0","method":"exit"})");

        const int exitCode = cb::runLspServer(clientToServer, serverToClient);
        CHECK(exitCode == 0);

        // Helper to read next response with a specific id (skipping publishDiagnostics notifications)
        auto readResponseForId = [&](int64_t targetId) -> cb::JsonParseResult {
            while (true) {
                const auto rawOpt = cb::readJsonrpcMessage(serverToClient);
                REQUIRE(rawOpt.has_value());
                auto parsed = cb::parseJson(rawOpt.value());
                REQUIRE(parsed.ok);
                const auto idOpt = cb::jsonGetInt(parsed.arena, cb::jsonGetField(parsed.arena, parsed.root, "id"));
                if (idOpt.has_value() && idOpt.value() == targetId) {
                    return parsed;
                }
            }
        };

        // 1. Verify initialize capabilities (id = 1)
        const auto initResp = readResponseForId(1);
        const cb::JsonId initRes = cb::jsonGetField(initResp.arena, initResp.root, "result");
        const cb::JsonId caps = cb::jsonGetField(initResp.arena, initRes, "capabilities");
        const cb::JsonId compProv = cb::jsonGetField(initResp.arena, caps, "completionProvider");
        REQUIRE(compProv != cb::JSON_INVALID_ID);
        const cb::JsonId sigProv = cb::jsonGetField(initResp.arena, caps, "signatureHelpProvider");
        REQUIRE(sigProv != cb::JSON_INVALID_ID);
        const cb::JsonId srvInfo = cb::jsonGetField(initResp.arena, initRes, "serverInfo");
        CHECK(cb::jsonGetString(initResp.arena, cb::jsonGetField(initResp.arena, srvInfo, "version")) == "0.3.0");

        // 2. Verify dot-completion on brand-new variable 'hero.' (id = 2)
        // Must include struct fields ('name', 'health'), cross-module UFCS ('heal'), and same-module UFCS ('localBuff'),
        // while excluding private cross-module 'secretHelper'.
        const auto compHero = readResponseForId(2);
        const cb::JsonId heroItems = cb::jsonGetField(compHero.arena, compHero.root, "result");
        CHECK(hasCompletionItem(compHero.arena, heroItems, "name", 5, "as str"));
        CHECK(hasCompletionItem(compHero.arena, heroItems, "health", 5, "as int"));
        CHECK(hasCompletionItem(compHero.arena, heroItems, "heal", 2, "ufcs fn heal(amount as int, bonus as int) as int can error"));
        CHECK(hasCompletionItem(compHero.arena, heroItems, "localBuff", 2, "ufcs fn localBuff(shield as int)"));
        CHECK_FALSE(hasCompletionItem(compHero.arena, heroItems, "secretHelper"));

        // 3. Verify module alias dot-completion 'player.' (id = 3)
        const auto compMod = readResponseForId(3);
        const cb::JsonId modItems = cb::jsonGetField(compMod.arena, compMod.root, "result");
        CHECK(hasCompletionItem(compMod.arena, modItems, "Player", 22, "type Player"));
        CHECK(hasCompletionItem(compMod.arena, modItems, "Score", 25, "alias Score = int"));
        CHECK(hasCompletionItem(compMod.arena, modItems, "heal", 3));
        CHECK_FALSE(hasCompletionItem(compMod.arena, modItems, "secretHelper"));

        // 4. Verify built-in err dot-completion 'e.' (id = 4)
        const auto compErr = readResponseForId(4);
        const cb::JsonId errItems = cb::jsonGetField(compErr.arena, compErr.root, "result");
        CHECK(hasCompletionItem(compErr.arena, errItems, "msg", 5, "as str"));
        CHECK(hasCompletionItem(compErr.arena, errItems, "code", 5, "as int"));

        // 5. Verify built-in list[int] dot-completion 'nums.' (id = 5)
        const auto compList = readResponseForId(5);
        const cb::JsonId listItems = cb::jsonGetField(compList.arena, compList.root, "result");
        CHECK(hasCompletionItem(compList.arena, listItems, "push", 2, "fn(item as int)"));
        CHECK(hasCompletionItem(compList.arena, listItems, "len", 2, "fn() as int"));
        CHECK_FALSE(hasCompletionItem(compList.arena, listItems, "append"));

        // 6. Verify named field= completion inside 'player.Player(' (id = 6)
        const auto compNamed = readResponseForId(6);
        const cb::JsonId namedItems = cb::jsonGetField(compNamed.arena, compNamed.root, "result");
        CHECK(hasCompletionItem(compNamed.arena, namedItems, "name=", 5, "as str"));
        CHECK(hasCompletionItem(compNamed.arena, namedItems, "health=", 5, "as int"));

        // 7. Verify signatureHelp inside 'player.Player(' (id = 7)
        const auto sigCtor = readResponseForId(7);
        const cb::JsonId sigCtorRes = cb::jsonGetField(sigCtor.arena, sigCtor.root, "result");
        CHECK(cb::jsonGetInt(sigCtor.arena, cb::jsonGetField(sigCtor.arena, sigCtorRes, "activeParameter")) == 0);
        const cb::JsonId ctorSigs = cb::jsonGetField(sigCtor.arena, sigCtorRes, "signatures");
        REQUIRE(std::holds_alternative<cb::JsonArray>(sigCtor.arena.nodes[ctorSigs]));
        const cb::JsonId ctorSig0 = std::get<cb::JsonArray>(sigCtor.arena.nodes[ctorSigs]).elements[0];
        CHECK(cb::jsonGetString(sigCtor.arena, cb::jsonGetField(sigCtor.arena, ctorSig0, "label")) ==
              "Player(name as str, health as int) as Player");

        // 8. Verify UFCS signatureHelp inside 'hero.heal(25, ' (id = 8)
        // Receiver 'byref p as Player' is omitted from visible UFCS parameters, and activeParameter is 1 ('bonus as int')
        const auto sigUfcs = readResponseForId(8);
        const cb::JsonId sigUfcsRes = cb::jsonGetField(sigUfcs.arena, sigUfcs.root, "result");
        CHECK(cb::jsonGetInt(sigUfcs.arena, cb::jsonGetField(sigUfcs.arena, sigUfcsRes, "activeParameter")) == 1);
        const cb::JsonId ufcsSigs = cb::jsonGetField(sigUfcs.arena, sigUfcsRes, "signatures");
        REQUIRE(std::holds_alternative<cb::JsonArray>(sigUfcs.arena.nodes[ufcsSigs]));
        const cb::JsonId ufcsSig0 = std::get<cb::JsonArray>(sigUfcs.arena.nodes[ufcsSigs]).elements[0];
        CHECK(cb::jsonGetString(sigUfcs.arena, cb::jsonGetField(sigUfcs.arena, ufcsSig0, "label")) ==
              "fn heal(amount as int, bonus as int) as int can error");
        const cb::JsonId ufcsParams = cb::jsonGetField(sigUfcs.arena, ufcsSig0, "parameters");
        REQUIRE(std::holds_alternative<cb::JsonArray>(sigUfcs.arena.nodes[ufcsParams]));
        const auto& ufcsParamElems = std::get<cb::JsonArray>(sigUfcs.arena.nodes[ufcsParams]).elements;
        REQUIRE(ufcsParamElems.size() == 2);
        CHECK(cb::jsonGetString(sigUfcs.arena, cb::jsonGetField(sigUfcs.arena, ufcsParamElems[0], "label")) == "amount as int");
        CHECK(cb::jsonGetString(sigUfcs.arena, cb::jsonGetField(sigUfcs.arena, ufcsParamElems[1], "label")) == "bonus as int");

        // 9. Verify type annotation completion after 'dim x as ' (id = 9)
        const auto compTypes = readResponseForId(9);
        const cb::JsonId typeItems = cb::jsonGetField(compTypes.arena, compTypes.root, "result");
        CHECK(hasCompletionItem(compTypes.arena, typeItems, "int", 14));
        CHECK(hasCompletionItem(compTypes.arena, typeItems, "str", 14));
        CHECK(hasCompletionItem(compTypes.arena, typeItems, "list", 14));
        CHECK(hasCompletionItem(compTypes.arena, typeItems, "player", 9));

        // 10. Verify general scope completion with prefix 'he' (id = 10)
        const auto compScope = readResponseForId(10);
        const cb::JsonId scopeItems = cb::jsonGetField(compScope.arena, compScope.root, "result");
        CHECK(hasCompletionItem(compScope.arena, scopeItems, "hero", 6, "as Player"));
    }
}

TEST_CASE("VS Code extension JSON manifests, TextMate grammar, and snippets are valid", "[vscode][lsp]") {
    SECTION("editors/vscode/package.json manifest and contributes schema") {
        const std::string raw = cb::testE2E::readFileText("editors/vscode/package.json");
        REQUIRE(!raw.empty());
        const cb::JsonParseResult parsed = cb::parseJson(raw);
        REQUIRE(parsed.ok);
        CHECK(cb::jsonGetString(parsed.arena, cb::jsonGetField(parsed.arena, parsed.root, "name")) == "cleanbasic");
        CHECK(cb::jsonGetString(parsed.arena, cb::jsonGetField(parsed.arena, parsed.root, "main")) == "./extension.js");

        const cb::JsonId contributes = cb::jsonGetField(parsed.arena, parsed.root, "contributes");
        REQUIRE(contributes != cb::INVALID_ID);

        const cb::JsonId langs = cb::jsonGetField(parsed.arena, contributes, "languages");
        REQUIRE(std::holds_alternative<cb::JsonArray>(parsed.arena.nodes[langs]));
        const cb::JsonId lang0 = std::get<cb::JsonArray>(parsed.arena.nodes[langs]).elements[0];
        CHECK(cb::jsonGetString(parsed.arena, cb::jsonGetField(parsed.arena, lang0, "id")) == "cleanbasic");

        const cb::JsonId grammars = cb::jsonGetField(parsed.arena, contributes, "grammars");
        REQUIRE(std::holds_alternative<cb::JsonArray>(parsed.arena.nodes[grammars]));
        const cb::JsonId gram0 = std::get<cb::JsonArray>(parsed.arena.nodes[grammars]).elements[0];
        CHECK(cb::jsonGetString(parsed.arena, cb::jsonGetField(parsed.arena, gram0, "scopeName")) == "source.cleanbasic");

        const cb::JsonId config = cb::jsonGetField(parsed.arena, contributes, "configuration");
        const cb::JsonId props = cb::jsonGetField(parsed.arena, config, "properties");
        CHECK(cb::jsonGetField(parsed.arena, props, "cleanbasic.executablePath") != cb::INVALID_ID);
        CHECK(cb::jsonGetField(parsed.arena, props, "cleanbasic.trace.server") != cb::INVALID_ID);
    }

    SECTION("editors/vscode/language-configuration.json, TextMate grammar, snippets, and .vscode/launch.json") {
        const cb::JsonParseResult langCfg = cb::parseJson(cb::testE2E::readFileText("editors/vscode/language-configuration.json"));
        REQUIRE(langCfg.ok);
        const cb::JsonId comments = cb::jsonGetField(langCfg.arena, langCfg.root, "comments");
        CHECK(cb::jsonGetString(langCfg.arena, cb::jsonGetField(langCfg.arena, comments, "lineComment")) == "'");
        CHECK(cb::jsonGetField(langCfg.arena, langCfg.root, "folding") != cb::INVALID_ID);
        CHECK(cb::jsonGetField(langCfg.arena, langCfg.root, "indentationRules") != cb::INVALID_ID);

        const cb::JsonParseResult tmGram = cb::parseJson(cb::testE2E::readFileText("editors/vscode/syntaxes/cleanbasic.tmLanguage.json"));
        REQUIRE(tmGram.ok);
        CHECK(cb::jsonGetString(tmGram.arena, cb::jsonGetField(tmGram.arena, tmGram.root, "scopeName")) == "source.cleanbasic");
        const cb::JsonId repo = cb::jsonGetField(tmGram.arena, tmGram.root, "repository");
        CHECK(cb::jsonGetField(tmGram.arena, repo, "comments") != cb::INVALID_ID);
        CHECK(cb::jsonGetField(tmGram.arena, repo, "strings") != cb::INVALID_ID);
        CHECK(cb::jsonGetField(tmGram.arena, repo, "keywords") != cb::INVALID_ID);
        CHECK(cb::jsonGetField(tmGram.arena, repo, "types") != cb::INVALID_ID);
        CHECK(cb::jsonGetField(tmGram.arena, repo, "builtins") != cb::INVALID_ID);

        const cb::JsonParseResult snippets = cb::parseJson(cb::testE2E::readFileText("editors/vscode/snippets/cleanbasic.code-snippets"));
        REQUIRE(snippets.ok);
        CHECK(cb::jsonGetField(snippets.arena, snippets.root, "Function Declaration") != cb::INVALID_ID);
        CHECK(cb::jsonGetField(snippets.arena, snippets.root, "Select Case Block") != cb::INVALID_ID);
        CHECK(cb::jsonGetField(snippets.arena, snippets.root, "On Error Handler") != cb::INVALID_ID);

        const cb::JsonParseResult launch = cb::parseJson(cb::testE2E::readFileText(".vscode/launch.json"));
        REQUIRE(launch.ok);
        const cb::JsonId configs = cb::jsonGetField(launch.arena, launch.root, "configurations");
        REQUIRE(std::holds_alternative<cb::JsonArray>(launch.arena.nodes[configs]));
        const cb::JsonId cfg0 = std::get<cb::JsonArray>(launch.arena.nodes[configs]).elements[0];
        CHECK(cb::jsonGetString(launch.arena, cb::jsonGetField(launch.arena, cfg0, "type")) == "extensionHost");

        const std::string extJs = cb::testE2E::readFileText("editors/vscode/extension.js");
        CHECK(extJs.find("LanguageClient") != std::string::npos);
        CHECK(extJs.find("cleanbasic.restartServer") != std::string::npos);
        CHECK(extJs.find("path.resolve(__dirname, '..', '..', 'build', exeName)") != std::string::npos);
        CHECK(extJs.find("TransportKind.stdio") == std::string::npos);
    }

    SECTION("cleanbasic lsp --stdio subprocess invocation exits cleanly with code 0") {
        std::string cbBinPath = "./build/cleanbasic";
        std::ifstream cbBinRoot("build/cleanbasic");
        if (!cbBinRoot.good()) {
            std::ifstream cbBinLocal("./cleanbasic");
            if (cbBinLocal.good()) {
                cbBinPath = "./cleanbasic";
            }
        }

        std::ostringstream lspInputSs;
        cb::writeJsonrpcMessage(
            lspInputSs,
            R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})"
        );
        cb::writeJsonrpcMessage(
            lspInputSs,
            R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})"
        );
        cb::writeJsonrpcMessage(
            lspInputSs,
            R"({"jsonrpc":"2.0","method":"exit"})"
        );

        const std::string tmpIn = "/tmp/cbLspStdioIn.txt";
        const std::string tmpOut = "/tmp/cbLspStdioOut.txt";
        {
            std::ofstream outF(tmpIn, std::ios::binary);
            outF << lspInputSs.str();
        }

        const std::string cmd = cbBinPath + " lsp --stdio < " + tmpIn + " > " + tmpOut + " 2>&1";
        const int rc = std::system(cmd.c_str());
        CHECK(rc == 0);

        const std::string outText = cb::testE2E::readFileText(tmpOut);
        CHECK(outText.find("cleanbasic-lsp") != std::string::npos);

        std::error_code ec;
        std::filesystem::remove(tmpIn, ec);
        std::filesystem::remove(tmpOut, ec);
    }
}

TEST_CASE("Standard library std/json module: vendor/ + stdlib/ resolution, UFCS queries, error handling, and E2E compilation", "[stdlib][json][e2e]") {
    SECTION("E2E fixture tests/cases/stdlib_json.cb compiles and runs natively") {
        cb::testE2E::runE2EFixture("stdlibJson", "tests/cases/stdlib_json.cb");
    }

    SECTION("import 'std/json' and 'std/json.cb' resolve module ID 'std/json', default alias 'json', and module-relative json_bridge.hpp") {
        const std::string src = R"(import "std/json"

fn main() as int
    on error e
        return 1
    end error
    dim doc = json.parse("3.5")
    dim root = doc.root()
    dim pi = doc.getFloat(root)
    if pi > 3.0
        return 0
    end if
    return 1
end fn
)";
        const auto res = cb::transpileSource(src, "appWithStdJson.cb");
        REQUIRE(res.ok);
        REQUIRE(!res.symbols.headers.empty());
        CHECK(res.cppCode.find("json/json_bridge.hpp") != std::string::npos);
        CHECK(res.cppCode.find("cb_std_json__parse(") != std::string::npos);
        CHECK(res.cppCode.find("cb_std_json__getFloat(") != std::string::npos);
    }

    SECTION("LSP go-to-definition and hover resolve std/json symbols into stdlib/json/json.cb") {
        std::stringstream clientToServer;
        std::stringstream serverToClient;

        cb::writeJsonrpcMessage(
            clientToServer,
            R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})"
        );

        {
            cb::JsonArena arena;
            const cb::JsonId msg = cb::jsonAddObject(arena);
            cb::jsonObjectSet(arena, msg, "jsonrpc", cb::jsonAddString(arena, "2.0"));
            cb::jsonObjectSet(arena, msg, "method", cb::jsonAddString(arena, "textDocument/didOpen"));
            const cb::JsonId params = cb::jsonAddObject(arena);
            const cb::JsonId td = cb::jsonAddObject(arena);
            cb::jsonObjectSet(arena, td, "uri", cb::jsonAddString(arena, "file:///virtualWs/stdJsonLsp.cb"));
            cb::jsonObjectSet(arena, td, "languageId", cb::jsonAddString(arena, "cleanbasic"));
            cb::jsonObjectSet(arena, td, "version", cb::jsonAddInt(arena, 1));
            cb::jsonObjectSet(arena, td, "text", cb::jsonAddString(arena, R"(import "std/json"
fn run() can error
    dim doc = json.parse("{}")
end fn
)"));
            cb::jsonObjectSet(arena, params, "textDocument", td);
            cb::jsonObjectSet(arena, msg, "params", params);
            cb::writeJsonrpcMessage(clientToServer, cb::serializeJson(arena, msg));
        }

        // Request definition on 'parse' in 'json.parse("{}")' (line 2, character 20)
        cb::writeJsonrpcMessage(
            clientToServer,
            R"({"jsonrpc":"2.0","id":2,"method":"textDocument/definition","params":{"textDocument":{"uri":"file:///virtualWs/stdJsonLsp.cb"},"position":{"line":2,"character":20}}})"
        );
        cb::writeJsonrpcMessage(clientToServer, R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");
        cb::writeJsonrpcMessage(clientToServer, R"({"jsonrpc":"2.0","method":"exit"})");

        const int rc = cb::runLspServer(clientToServer, serverToClient);
        CHECK(rc == 0);

        // Read initialize response & didOpen diagnostics
        (void)cb::readJsonrpcMessage(serverToClient);
        const auto diagMsg = cb::parseJson(cb::readJsonrpcMessage(serverToClient).value());
        REQUIRE(diagMsg.ok);
        const cb::JsonId diags = cb::jsonGetField(diagMsg.arena, cb::jsonGetField(diagMsg.arena, diagMsg.root, "params"), "diagnostics");
        CHECK(std::get<cb::JsonArray>(diagMsg.arena.nodes[diags]).elements.empty());

        // Read definition response -> points to stdlib/json/json.cb
        const auto defMsg = cb::parseJson(cb::readJsonrpcMessage(serverToClient).value());
        REQUIRE(defMsg.ok);
        const cb::JsonId defRes = cb::jsonGetField(defMsg.arena, defMsg.root, "result");
        const auto defUri = cb::jsonGetString(defMsg.arena, cb::jsonGetField(defMsg.arena, defRes, "uri"));
        REQUIRE(defUri.has_value());
        CHECK(defUri.value().find("stdlib/json/json.cb") != std::string::npos);
    }
}

TEST_CASE("Portable SDK root resolution, cross-platform target specs, CLI flags, and compiler command generation", "[sdk][cross][cli]") {
    SECTION("resolveSdkRootDir locates portable SDK layout via CLEANBASIC_HOME, exe dir, exe parent, and fallback") {
        const std::string tmpBase = "/tmp/cbPortableSdkTest";
        std::error_code ec;
        std::filesystem::remove_all(tmpBase, ec);

        const std::string sdkRoot = tmpBase + "/cleanbasic-sdk";
        const std::string sdkBinDir = sdkRoot + "/bin";
        const std::string sdkMultiCfgDir = sdkRoot + "/build/Release";
        const std::string envSdk = tmpBase + "/custom-env-sdk";

        std::filesystem::create_directories(sdkRoot + "/stdlib/json", ec);
        std::filesystem::create_directories(sdkRoot + "/vendor", ec);
        std::filesystem::create_directories(sdkBinDir, ec);
        std::filesystem::create_directories(sdkMultiCfgDir, ec);
        std::filesystem::create_directories(envSdk + "/stdlib", ec);
        std::filesystem::create_directories(envSdk + "/vendor", ec);

        CHECK(cb::isValidSdkRoot(sdkRoot));
        CHECK(cb::isValidSdkRoot(envSdk));
        CHECK_FALSE(cb::isValidSdkRoot(sdkBinDir));

        // 1. Executable directly inside SDK root: <sdk>/cleanbasic
        CHECK(cb::resolveSdkRootDir("", sdkRoot + "/cleanbasic", "/nonexistent") == sdkRoot);

        // 2. Executable inside <sdk>/bin/cleanbasic
        CHECK(cb::resolveSdkRootDir("", sdkBinDir + "/cleanbasic.exe", "/nonexistent") == sdkRoot);

        // 3. Executable inside <sdk>/build/Release/cleanbasic
        CHECK(cb::resolveSdkRootDir("", sdkMultiCfgDir + "/cleanbasic", "/nonexistent") == sdkRoot);

        // 4. CLEANBASIC_HOME takes highest priority
        CHECK(cb::resolveSdkRootDir(envSdk + "/", sdkRoot + "/cleanbasic", sdkRoot) == envSdk);

        // 5. Fallback to compile-time root when exe path is outside any SDK layout
        CHECK(cb::resolveSdkRootDir("", "/usr/local/bin/cleanbasic", sdkRoot) == sdkRoot);

        // 6. Live cleanbasicRootDir() resolves a valid SDK root containing stdlib/ and vendor/
        CHECK(cb::isValidSdkRoot(cb::cleanbasicRootDir()));

        std::filesystem::remove_all(tmpBase, ec);
    }

    SECTION("resolveTargetSpec maps Linux, Windows, and macOS targets to Zig triples and extensions") {
        const cb::TargetSpec nativeSpec = cb::resolveTargetSpec("");
        CHECK(nativeSpec.ok);
        CHECK_FALSE(nativeSpec.isCross);
        CHECK(nativeSpec.canonicalName == "native");

        const cb::TargetSpec linuxSpec = cb::resolveTargetSpec("linux");
        CHECK(linuxSpec.ok);
        CHECK(linuxSpec.isCross);
        CHECK_FALSE(linuxSpec.isWindows);
        CHECK(linuxSpec.zigTriple == "x86_64-linux-musl");
        CHECK(linuxSpec.binaryExt == "");

        const cb::TargetSpec linuxArmSpec = cb::resolveTargetSpec("linux-arm64");
        CHECK(linuxArmSpec.ok);
        CHECK(linuxArmSpec.isCross);
        CHECK(linuxArmSpec.zigTriple == "aarch64-linux-musl");

        const cb::TargetSpec winSpec = cb::resolveTargetSpec("windows");
        CHECK(winSpec.ok);
        CHECK(winSpec.isCross);
        CHECK(winSpec.isWindows);
        CHECK(winSpec.zigTriple == "x86_64-windows-gnu");
        CHECK(winSpec.binaryExt == ".exe");
        CHECK(cb::resolveBinaryOutputPath("dist", "", winSpec) == "dist/app.exe");
        CHECK(cb::resolveBinaryOutputPath("dist", "dist/myGame", winSpec) == "dist/myGame.exe");
        CHECK(cb::resolveBinaryOutputPath("dist", "dist/myGame.exe", winSpec) == "dist/myGame.exe");

        const cb::TargetSpec winArmSpec = cb::resolveTargetSpec("windows-arm64");
        CHECK(winArmSpec.ok);
        CHECK(winArmSpec.isWindows);
        CHECK(winArmSpec.zigTriple == "aarch64-windows-gnu");

        const cb::TargetSpec macSpec = cb::resolveTargetSpec("macos");
        CHECK(macSpec.ok);
        CHECK(macSpec.isCross);
        CHECK_FALSE(macSpec.isWindows);
        CHECK(macSpec.zigTriple == "x86_64-macos");

        const cb::TargetSpec macArmSpec = cb::resolveTargetSpec("macos-arm64");
        CHECK(macArmSpec.ok);
        CHECK(macArmSpec.zigTriple == "aarch64-macos");

        const cb::TargetSpec badSpec = cb::resolveTargetSpec("solaris-sparc");
        CHECK_FALSE(badSpec.ok);
        CHECK(badSpec.errorMessage.find("Unsupported target") != std::string::npos);
    }

    SECTION("resolveCompilerCommand and buildCompileCommand generate portable C++17 invocation") {
        const cb::TargetSpec winSpec = cb::resolveTargetSpec("windows");
        const std::string crossCc = cb::resolveCompilerCommand(winSpec, "");
        CHECK(crossCc == "zig c++ -target x86_64-windows-gnu");

        const std::string cmd = cb::buildCompileCommand(
            crossCc,
            "/opt/cleanbasic-sdk",
            "projects/demo",
            "projects/demo/build.cpp",
            "projects/demo/app.exe",
            {"-lm"}
        );
        CHECK(cmd.find("zig c++ -target x86_64-windows-gnu -O0 -std=c++17 -pthread") != std::string::npos);
        CHECK(cmd.find(R"(-I"/opt/cleanbasic-sdk/vendor")") != std::string::npos);
        CHECK(cmd.find(R"(-I"/opt/cleanbasic-sdk/stdlib")") != std::string::npos);
        CHECK(cmd.find(R"(-I"projects/demo")") != std::string::npos);
        CHECK(cmd.find(R"("projects/demo/build.cpp" -o "projects/demo/app.exe" -lm)") != std::string::npos);

        const std::string releaseCmd = cb::buildCompileCommand(
            crossCc,
            "/opt/cleanbasic-sdk",
            "projects/demo",
            "projects/demo/build.cpp",
            "projects/demo/app.exe",
            {"-lm"},
            "3"
        );
        CHECK(releaseCmd.find("zig c++ -target x86_64-windows-gnu -O3 -std=c++17 -pthread") != std::string::npos);

        const std::string pchCmd = cb::buildCompileCommand(
            "g++",
            "/opt/cleanbasic-sdk",
            "projects/demo",
            "projects/demo/build.cpp",
            "projects/demo/app",
            {},
            "0",
            "/opt/cleanbasic-sdk/build/pch/cb_pch.hpp"
        );
        CHECK(pchCmd.find(R"(-Winvalid-pch -include "/opt/cleanbasic-sdk/build/pch/cb_pch.hpp")") != std::string::npos);
    }

    SECTION("parseCliArgs parses --target, --bin, and --cc and buildFile compiles with custom options") {
        const cb::CliArgs args1 = cb::parseCliArgs({"build", "main.cb", "--target", "windows", "-b", "dist/game", "--cc", "zig c++"});
        CHECK(args1.ok);
        CHECK(args1.inputFile == "main.cb");
        CHECK(args1.compileBinary);
        CHECK(args1.target == "windows");
        CHECK(args1.binaryOutput == "dist/game");
        CHECK(args1.compiler == "zig c++");

        const cb::CliArgs args2 = cb::parseCliArgs({"main.cb", "-b", "outBin"});
        CHECK(args2.ok);
        CHECK(args2.compileBinary);
        CHECK(args2.binaryOutput == "outBin");

        const cb::CliArgs badArgs = cb::parseCliArgs({"build", "main.cb", "--target"});
        CHECK_FALSE(badArgs.ok);

        const cb::CliArgs defaultOptArgs = cb::parseCliArgs({"main.cb", "-c"});
        CHECK(defaultOptArgs.ok);
        CHECK(defaultOptArgs.optLevel == "0");
        CHECK_FALSE(defaultOptArgs.isRelease);
        CHECK_FALSE(defaultOptArgs.showTimings);

        const cb::CliArgs releaseLongArgs = cb::parseCliArgs({"main.cb", "-c", "--release"});
        CHECK(releaseLongArgs.ok);
        CHECK(releaseLongArgs.optLevel == "3");
        CHECK(releaseLongArgs.isRelease);

        const cb::CliArgs releaseShortArgs = cb::parseCliArgs({"main.cb", "-c", "-r"});
        CHECK(releaseShortArgs.ok);
        CHECK(releaseShortArgs.optLevel == "3");
        CHECK(releaseShortArgs.isRelease);

        const cb::CliArgs debugExplicitArgs = cb::parseCliArgs({"main.cb", "-c", "--release", "--debug"});
        CHECK(debugExplicitArgs.ok);
        CHECK(debugExplicitArgs.optLevel == "0");
        CHECK_FALSE(debugExplicitArgs.isRelease);

        const cb::CliArgs opt2Args = cb::parseCliArgs({"main.cb", "-c", "--opt", "2"});
        CHECK(opt2Args.ok);
        CHECK(opt2Args.optLevel == "2");
        CHECK_FALSE(opt2Args.isRelease);

        const cb::CliArgs optShortInlineArgs = cb::parseCliArgs({"main.cb", "-c", "-O1"});
        CHECK(optShortInlineArgs.ok);
        CHECK(optShortInlineArgs.optLevel == "1");

        const cb::CliArgs optShortSepArgs = cb::parseCliArgs({"main.cb", "-c", "-O", "1"});
        CHECK(optShortSepArgs.ok);
        CHECK(optShortSepArgs.optLevel == "1");

        const cb::CliArgs timeArgs = cb::parseCliArgs({"main.cb", "-c", "--time"});
        CHECK(timeArgs.ok);
        CHECK(timeArgs.showTimings);
        CHECK(timeArgs.usePch);
        CHECK_FALSE(timeArgs.rebuildPch);

        const cb::CliArgs noPchArgs = cb::parseCliArgs({"main.cb", "-c", "--no-pch"});
        CHECK(noPchArgs.ok);
        CHECK_FALSE(noPchArgs.usePch);
        CHECK_FALSE(noPchArgs.rebuildPch);

        const cb::CliArgs rebuildPchArgs = cb::parseCliArgs({"main.cb", "-c", "--rebuild-pch"});
        CHECK(rebuildPchArgs.ok);
        CHECK(rebuildPchArgs.usePch);
        CHECK(rebuildPchArgs.rebuildPch);

        const cb::CliArgs badOptArgs = cb::parseCliArgs({"main.cb", "-c", "--opt", "9"});
        CHECK_FALSE(badOptArgs.ok);
        CHECK(badOptArgs.errorMessage.find("Invalid optimization level") != std::string::npos);

        // Verify buildFile rejects unknown target with TargetError
        const cb::BuildFileResult badTargetRes = cb::buildFile(
            "tests/cases/fibonacci.cb",
            "build/testArtifacts/badTarget.cpp",
            true,
            "",
            "invalid-os"
        );
        CHECK_FALSE(badTargetRes.ok);
        CHECK(badTargetRes.error.kind == "TargetError");

        // Verify buildFile compiles native binary with explicit --bin and --cc g++
        const std::string outCpp = "build/testArtifacts/cliOptsFib.cpp";
        const std::string outBin = "build/testArtifacts/cliOptsFibBin";
        const cb::BuildFileResult okRes = cb::buildFile(
            "tests/cases/fibonacci.cb",
            outCpp,
            true,
            outBin,
            "native",
            "g++"
        );
        INFO("Build diagnostic: " << cb::formatDiagnostic(okRes.error));
        REQUIRE(okRes.ok);
        CHECK(okRes.outputBinaryPath == outBin);
        CHECK(okRes.compileCommand.find(" -O0 ") != std::string::npos);
        CHECK(okRes.compileCommand.find(R"(-I")") != std::string::npos);
        CHECK(okRes.timings.transpileTotalMs > 0.0);
        CHECK(okRes.timings.cppCompileMs > 0.0);
        CHECK(okRes.timings.totalMs >= okRes.timings.transpileTotalMs);
        std::error_code ec;
        CHECK(std::filesystem::exists(outBin, ec));

        // Verify PCH rebuild (--rebuild-pch) followed by PCH cache hit and --no-pch fallback
        const cb::BuildFileResult rebuildRes = cb::buildFile(
            "tests/cases/fibonacci.cb",
            outCpp,
            true,
            outBin,
            "native",
            "g++",
            "0",
            false,
            true,
            true
        );
        REQUIRE(rebuildRes.ok);
        CHECK(rebuildRes.timings.pchUsed);
        CHECK_FALSE(rebuildRes.timings.pchCacheHit);
        CHECK(rebuildRes.timings.pchBuildMs > 0.0);
        CHECK(rebuildRes.compileCommand.find("-Winvalid-pch -include") != std::string::npos);

        const cb::BuildFileResult cachedRes = cb::buildFile(
            "tests/cases/fibonacci.cb",
            outCpp,
            true,
            outBin,
            "native",
            "g++",
            "0",
            false,
            true,
            false
        );
        REQUIRE(cachedRes.ok);
        CHECK(cachedRes.timings.pchUsed);
        CHECK(cachedRes.timings.pchCacheHit);
        CHECK(cachedRes.timings.pchBuildMs == 0.0);

        const cb::BuildFileResult noPchRes = cb::buildFile(
            "tests/cases/fibonacci.cb",
            outCpp,
            true,
            outBin,
            "native",
            "g++",
            "0",
            false,
            false,
            false
        );
        REQUIRE(noPchRes.ok);
        CHECK_FALSE(noPchRes.timings.pchUsed);
        CHECK(noPchRes.compileCommand.find("-include") == std::string::npos);

        // Verify runCli with --release and --time outputs timing breakdown and compiles with -O3
        std::ostringstream capturedCout;
        std::streambuf* origCout = std::cout.rdbuf(capturedCout.rdbuf());
        const int cliRc = cb::runCli(
            "cleanbasic",
            {"build", "tests/cases/fibonacci.cb", "-o", outCpp, "-b", outBin, "--cc", "g++", "--release", "--time"}
        );
        std::cout.rdbuf(origCout);
        CHECK(cliRc == 0);
        const std::string cliOut = capturedCout.str();
        CHECK(cliOut.find("Build Timing Breakdown:") != std::string::npos);
        CHECK(cliOut.find("Total Transpilation:") != std::string::npos);
        CHECK(cliOut.find("Precompiled Header:") != std::string::npos);
        CHECK(cliOut.find("C++ Compile & Link:") != std::string::npos);
        CHECK(cliOut.find("Total Build Time:") != std::string::npos);
    }
}

TEST_CASE("Standard library std/fs, std/os, std/str, while loops, and examples/cbgrep.cb E2E verification", "[stdlib][fs][os][str][cbgrep][e2e]") {
    SECTION("E2E fixture tests/cases/stdlib_fs.cb compiles and runs natively") {
        cb::testE2E::runE2EFixture("stdlib_fs", "tests/cases/stdlib_fs.cb");
    }

    SECTION("E2E fixture tests/cases/stdlib_os.cb compiles and runs natively") {
        cb::testE2E::runE2EFixture("stdlib_os", "tests/cases/stdlib_os.cb");
    }

    SECTION("E2E fixture tests/cases/stdlib_str.cb compiles and runs natively with deterministic std/str UFCS") {
        cb::testE2E::runE2EFixture("stdlib_str", "tests/cases/stdlib_str.cb");
    }

    SECTION("examples/cbgrep.cb compiles and runs natively with CLI flags and exit codes") {
        const std::string outCpp = "build/testArtifacts/cbgrepBuild.cpp";
        const std::string outBin = "build/testArtifacts/cbgrepBin";
        const cb::BuildFileResult buildRes = cb::buildFile(
            "examples/cbgrep.cb",
            outCpp,
            true,
            outBin,
            "native",
            "g++"
        );
        INFO("cbgrep build diagnostic: " << cb::formatDiagnostic(buildRes.error));
        REQUIRE(buildRes.ok);

        const std::string sampleFile = "build/testArtifacts/cbgrepSample.txt";
        const std::string stdoutFile = "build/testArtifacts/cbgrepStdout.txt";
        {
            std::ofstream out(sampleFile, std::ios::out | std::ios::binary | std::ios::trunc);
            out << "CleanBasic Alpha" << std::endl
                << "Second line" << std::endl
                << "cleanbasic beta" << std::endl;
        }

        const int rcMatch = std::system(("./" + outBin + " -in cleanbasic " + sampleFile + " > " + stdoutFile + " 2>&1").c_str());
        CHECK(rcMatch == 0);
        const std::string matchOut = cb::testE2E::readFileText(stdoutFile);
        CHECK(matchOut.find("1:CleanBasic Alpha") != std::string::npos);
        CHECK(matchOut.find("3:cleanbasic beta") != std::string::npos);

        const int rcCount = std::system(("./" + outBin + " -ic cleanbasic " + sampleFile + " > " + stdoutFile + " 2>&1").c_str());
        CHECK(rcCount == 0);
        const std::string countOut = cb::testE2E::readFileText(stdoutFile);
        CHECK(countOut.find("2") != std::string::npos);

        const int rcInvert = std::system(("./" + outBin + " -iv cleanbasic " + sampleFile + " > " + stdoutFile + " 2>&1").c_str());
        CHECK(rcInvert == 0);
        const std::string invertOut = cb::testE2E::readFileText(stdoutFile);
        CHECK(invertOut.find("Second line") != std::string::npos);
        CHECK(invertOut.find("CleanBasic Alpha") == std::string::npos);

        const int rcBadFlag = std::system(("./" + outBin + " -z cleanbasic " + sampleFile + " > " + stdoutFile + " 2>&1").c_str());
        CHECK(rcBadFlag != 0);
        const std::string badFlagOut = cb::testE2E::readFileText(stdoutFile);
        CHECK(badFlagOut.find("Unknown option '-z'") != std::string::npos);

        const int rcNoMatch = std::system(("./" + outBin + " nonexistentPatternXYZ " + sampleFile + " > " + stdoutFile + " 2>&1").c_str());
        CHECK(rcNoMatch != 0);
    }
}



TEST_CASE("Analyzer rejects removed global len() and str() functions", "[analyzer]") {
    SECTION("Rejects global len()") {
        const std::string code = R"(
fn main()
    dim x = [1, 2]
    print(len(x))
end fn
)";
        const auto res = cb::transpileSource(code, "badLen.cb");
        CHECK_FALSE(res.ok);
        CHECK(res.error.message.find("Global 'len()' is removed; use 'x.len()'") != std::string::npos);
    }
    SECTION("Rejects global str()") {
        const std::string code = R"(
fn main()
    dim x = 42
    print(str(x))
end fn
)";
        const auto res = cb::transpileSource(code, "badStr.cb");
        CHECK_FALSE(res.ok);
        CHECK(res.error.message.find("Global 'str()' is removed; use 'x.toStr()'") != std::string::npos);
    }
    SECTION("Rejects list.append()") {
        const std::string code = R"(
fn main()
    dim x as list[int] = []
    x.append(1)
end fn
)";
        const auto res = cb::transpileSource(code, "badAppend.cb");
        CHECK_FALSE(res.ok);
        CHECK(res.error.message.find("'.append()' is removed; use '.push()'") != std::string::npos);
    }
}

TEST_CASE("Emitted C++17 safest subset: const T&, [[nodiscard]], int64_t, and mutability enforcement", "[emitter][analyzer]") {
    SECTION("Read-only parameters emit const T&, primitives emit value, byref emits T&") {
        const std::string code = R"(
type Point
    x as int
    y as int
end type

fn inspect(
    s as str,
    nums as list[int],
    lookup as dict[str, int],
    pt as Point,
    n as int,
    f as float,
    flag as bool,
    byref outPt as Point
) as int
    return n
end fn

fn main()
end fn
)";
        const auto res = cb::transpileSource(code, "paramPassing.cb");
        REQUIRE(res.ok);
        CHECK(res.cppCode.find("int64_t inspect(const std::string& s, const std::vector<int64_t>& nums, const std::unordered_map<std::string, int64_t>& lookup, const Point& pt, int64_t n, double f, bool flag, Point& outPt)") != std::string::npos);
    }

    SECTION("Fallible functions emit [[nodiscard]] and CbError is [[nodiscard]]") {
        const std::string code = R"(
fn failVoid() can error
    error("fail")
end fn

fn failVal() as int can error
    return 42
end fn

fn main()
    on error e
        return
    end error
    failVoid()
    dim x = failVal()
end fn
)";
        const auto res = cb::transpileSource(code, "fallibleNodiscard.cb");
        REQUIRE(res.ok);
        CHECK(res.cppCode.find("struct [[nodiscard]] CbError {") != std::string::npos);
        CHECK(res.cppCode.find("[[nodiscard]] CbError failVoid()") != std::string::npos);
        CHECK(res.cppCode.find("[[nodiscard]] std::tuple<int64_t, CbError> failVal()") != std::string::npos);
    }

    SECTION("Analyzer rejects reassigning non-byref parameter") {
        const std::string code = R"(
fn test(p as int)
    p = 10
end fn
)";
        const auto res = cb::transpileSource(code, "badParamReassign.cb");
        CHECK_FALSE(res.ok);
        CHECK(res.error.message.find("Cannot reassign const variable 'p'") != std::string::npos);
    }

    SECTION("Analyzer rejects field mutation on non-byref parameter or const struct") {
        const std::string code = R"(
type Point
    x as int
end type
fn test(p as Point)
    p.x = 10
end fn
)";
        const auto res = cb::transpileSource(code, "badFieldMut.cb");
        CHECK_FALSE(res.ok);
        CHECK(res.error.message.find("Cannot mutate const variable 'p'") != std::string::npos);
    }

    SECTION("Analyzer rejects calling .push() on non-byref list parameter") {
        const std::string code = R"(
fn test(nums as list[int])
    nums.push(1)
end fn
)";
        const auto res = cb::transpileSource(code, "badPushMut.cb");
        CHECK_FALSE(res.ok);
        CHECK(res.error.message.find("Cannot call '.push()' on const variable 'nums'") != std::string::npos);
    }

    SECTION("Analyzer rejects passing non-byref parameter to byref parameter") {
        const std::string code = R"(
type Point
    x as int
end type
fn mutate(byref p as Point)
    p.x = 10
end fn
fn test(p as Point)
    mutate(p)
end fn
)";
        const auto res = cb::transpileSource(code, "badByrefPass.cb");
        CHECK_FALSE(res.ok);
        CHECK(res.error.message.find("Cannot pass const variable 'p' to byref parameter 'p'") != std::string::npos);
    }
}
TEST_CASE("Language ergonomics: continue, loop depth validation, and multi-value/string select case", "[control_flow][select_case][continue]") {
    SECTION("continue statement in for and while loops transpiles to C++ continue;") {
        const std::string code = R"(
fn main()
    for i = 1 to 5
        if i == 3
            continue
        end if
    end for

    dim n = 0
    while n < 5
        n = n + 1
        if n == 2
            continue
        end if
    end while
end fn
)";
        const auto res = cb::transpileSource(code, "testContinue.cb");
        REQUIRE(res.ok);
        CHECK(res.cppCode.find("continue;") != std::string::npos);
    }

    SECTION("Analyzer rejects break and continue outside loops") {
        const std::string badBreak = R"(
fn main()
    break
end fn
)";
        const auto resBreak = cb::transpileSource(badBreak, "badBreak.cb");
        CHECK_FALSE(resBreak.ok);
        CHECK(resBreak.error.message.find("'break' cannot be used outside of a loop") != std::string::npos);

        const std::string badContinue = R"(
fn main()
    continue
end fn
)";
        const auto resCont = cb::transpileSource(badContinue, "badContinue.cb");
        CHECK_FALSE(resCont.ok);
        CHECK(resCont.error.message.find("'continue' cannot be used outside of a loop") != std::string::npos);

        const std::string badSelectBreak = R"(
fn main()
    select case 1
        case 1
            break
    end select
end fn
)";
        const auto resSelBreak = cb::transpileSource(badSelectBreak, "badSelBreak.cb");
        CHECK_FALSE(resSelBreak.ok);
        CHECK(resSelBreak.error.message.find("'break' cannot be used outside of a loop") != std::string::npos);
    }

    SECTION("Multi-value case clauses for int emit stacked case labels") {
        const std::string code = R"(
fn classify(n as int) as str
    select case n
        case 1, 2, 3
            return "small"
        case 4, 5
            return "medium"
        case else
            return "large"
    end select
end fn

fn main()
end fn
)";
        const auto res = cb::transpileSource(code, "multiCaseInt.cb");
        REQUIRE(res.ok);
        CHECK(res.cppCode.find("case 1:") != std::string::npos);
        CHECK(res.cppCode.find("case 2:") != std::string::npos);
        CHECK(res.cppCode.find("case 3: {") != std::string::npos);
        CHECK(res.cppCode.find("case 4:") != std::string::npos);
        CHECK(res.cppCode.find("case 5: {") != std::string::npos);
        CHECK(res.cppCode.find("default: {") != std::string::npos);
    }

    SECTION("select case on str lowers to single-eval temporary variable and if/else-if equality chain") {
        const std::string code = R"(
fn classifyStr(s as str) as int
    select case s
        case "apple", "banana"
            return 1
        case "cherry"
            return 2
        case else
            return 0
    end select
end fn

fn main()
end fn
)";
        const auto res = cb::transpileSource(code, "strSelect.cb");
        REQUIRE(res.ok);
        CHECK(res.cppCode.find("const auto _sel_") != std::string::npos);
        CHECK(res.cppCode.find(R"(if (_sel_1 == std::string("apple") || _sel_1 == std::string("banana")) {)") != std::string::npos);
        CHECK(res.cppCode.find(R"(} else if (_sel_1 == std::string("cherry")) {)") != std::string::npos);
        CHECK(res.cppCode.find("} else {") != std::string::npos);
    }

    SECTION("Analyzer type-checks case expressions against target type and rejects invalid select case targets") {
        const std::string mismatchType = R"(
fn main()
    select case 10
        case "hello"
            print("no")
    end select
end fn
)";
        const auto resMismatch = cb::transpileSource(mismatchType, "mismatchType.cb");
        CHECK_FALSE(resMismatch.ok);
        CHECK(resMismatch.error.message.find("case expression type does not match select case target type") != std::string::npos);

        const std::string badTarget = R"(
fn main()
    dim f = 3.14
    select case f
        case 3.14
            print("no")
    end select
end fn
)";
        const auto resBadTarget = cb::transpileSource(badTarget, "badTarget.cb");
        CHECK_FALSE(resBadTarget.ok);
        CHECK(resBadTarget.error.message.find("select case expression must be of type 'int', 'bool', or 'str'") != std::string::npos);
    }

    SECTION("continue is accepted as valid scope exit terminator for on error blocks inside loops") {
        const std::string code = R"(
fn fallibleTask() as int can error
    error("failed")
    return 0
end fn

fn main()
    for i = 1 to 5
        on error e
            continue
        end error
        dim x = fallibleTask()
    end for
end fn
)";
        const auto res = cb::transpileSource(code, "continueOnError.cb");
        REQUIRE(res.ok);
        CHECK(res.cppCode.find("continue;") != std::string::npos);
    }
}


TEST_CASE("Enums and bytes semantic analysis and code generation", "[enums][bytes]") {
    SECTION("Analyzer rejects implicit conversion between enum and int") {
        const std::string assignToInt = R"(
enum Color
    red
    green
end enum

fn main()
    dim c = Color.red
    dim x as int = c
end fn
)";
        const auto res1 = cb::transpileSource(assignToInt, "enumToInt.cb");
        CHECK_FALSE(res1.ok);
        CHECK(res1.error.message.find("Cannot assign enum value to 'int'") != std::string::npos);

        const std::string assignFromInt = R"(
enum Color
    red
    green
end enum

fn main()
    dim c as Color = 1
end fn
)";
        const auto res2 = cb::transpileSource(assignFromInt, "intToEnum.cb");
        CHECK_FALSE(res2.ok);
        CHECK(res2.error.message.find("Cannot assign mismatched type to enum variable") != std::string::npos);
    }

    SECTION("Analyzer rejects unknown enum member") {
        const std::string badMember = R"(
enum Color
    red
    green
end enum

fn main()
    dim c = Color.blue
end fn
)";
        const auto res = cb::transpileSource(badMember, "badMember.cb");
        CHECK_FALSE(res.ok);
        CHECK(res.error.message.find("has no member 'blue'") != std::string::npos);
    }

    SECTION("Analyzer rejects mutating bytes methods on const variables") {
        const std::string constPush = R"(
fn main()
    const b = b"hello"
    b.push(65)
end fn
)";
        const auto resPush = cb::transpileSource(constPush, "constPush.cb");
        CHECK_FALSE(resPush.ok);
        CHECK(resPush.error.message.find("Cannot call '.push()' on const variable 'b'") != std::string::npos);

        const std::string constWrite = R"(
fn main()
    const b = b"hello"
    b.writeU16Le(1234, 0)
end fn
)";
        const auto resWrite = cb::transpileSource(constWrite, "constWrite.cb");
        CHECK_FALSE(resWrite.ok);
        CHECK(resWrite.error.message.find("Cannot call '.writeU16Le()' on const variable 'b'") != std::string::npos);
    }

    SECTION("Analyzer validates select case matching on enum types") {
        const std::string mismatchedEnumCase = R"(
enum Color
    red
end enum
enum Direction
    north
end enum

fn main()
    dim c = Color.red
    select case c
        case Direction.north
            print("no")
    end select
end fn
)";
        const auto res = cb::transpileSource(mismatchedEnumCase, "mismatchedEnumCase.cb");
        CHECK_FALSE(res.ok);
        CHECK(res.error.message.find("case expression type does not match select case target type") != std::string::npos);
    }

    SECTION("Lexer rejects invalid byte literal hex escape") {
        const std::string badHex = R"(
fn main()
    dim b = b"\xZZ"
end fn
)";
        const auto res = cb::transpileSource(badHex, "badHex.cb");
        CHECK_FALSE(res.ok);
        CHECK(res.error.message.find("Invalid hexadecimal escape") != std::string::npos);
    }
}

TEST_CASE("String interpolation zero-AST lexer desugaring and diagnostics", "[lexer][interpolation]") {
    SECTION("Desugars interpolated string into exact parenthesized concatenation token stream") {
        const auto lexRes = cb::tokenize(R"CB($"Hello {user.name}!")CB", "interp.cb");
        REQUIRE(lexRes.ok);
        std::vector<std::pair<std::string, std::string>> actual;
        for (const auto& t : lexRes.tokens) {
            if (t.kind != "EOF") {
                actual.emplace_back(t.kind, t.value);
            }
        }
        const std::vector<std::pair<std::string, std::string>> expected = {
            {"LPAREN", "("},
            {"STRING", R"CB("Hello ")CB"},
            {"OP_ARITH", "&"},
            {"LPAREN", "("},
            {"IDENT", "user"},
            {"DOT", "."},
            {"IDENT", "name"},
            {"RPAREN", ")"},
            {"OP_ARITH", "&"},
            {"STRING", R"CB("!")CB"},
            {"RPAREN", ")"}
        };
        CHECK(actual == expected);
    }

    SECTION("Supports expressions, precedence, {{ / }} escaping, nested interpolation, and dict literals") {
        const std::string code = R"CB(
enum Status
    ready
    busy
end enum

type User
    name as str
    score as int
end type

fn greet(u as User) as str
    return $"User {u.name} has {u.score + 5} pts (valid={u.score >= 10 and not false})"
end fn

fn main()
    dim u = User(name="Alice", score=95)
    dim st = Status.ready
    dim msg = $"{greet(u)} | status={st} | braces={{literal}} | nested={$"inner({1 + 2})"} | dict={({"x": 7})["x"]}"
    print(msg)
end fn
)CB";
        const auto res = cb::transpileSource(code, "interpFull.cb");
        INFO("Transpile diagnostic: " << cb::formatDiagnostic(res.error));
        REQUIRE(res.ok);
        CHECK(res.cppCode.find("cbToStr((u.score + 5))") != std::string::npos);
        CHECK(res.cppCode.find("cbToStr(((u.score >= 10) && (!false)))") != std::string::npos);
        CHECK(res.cppCode.find(R"CB(std::string(" | braces={literal} | nested="))CB") != std::string::npos);
    }

    SECTION("Reports clear LexerError diagnostics for invalid interpolated strings") {
        const auto emptyHole = cb::tokenize(R"CB(dim s = $"Hello {}")CB", "emptyHole.cb");
        CHECK_FALSE(emptyHole.ok);
        CHECK(emptyHole.error.kind == "LexerError");
        CHECK(emptyHole.error.message.find("Empty interpolation hole '{}'") != std::string::npos);

        const auto wsHole = cb::tokenize(R"CB(dim s = $"Hello {   }")CB", "wsHole.cb");
        CHECK_FALSE(wsHole.ok);
        CHECK(wsHole.error.kind == "LexerError");
        CHECK(wsHole.error.message.find("Empty interpolation hole '{}'") != std::string::npos);

        const auto loneCloseBrace = cb::tokenize(R"CB(dim s = $"Hello } world")CB", "loneBrace.cb");
        CHECK_FALSE(loneCloseBrace.ok);
        CHECK(loneCloseBrace.error.kind == "LexerError");
        CHECK(loneCloseBrace.error.message.find("Unescaped '}'") != std::string::npos);

        const auto untermStr = cb::tokenize(R"CB(dim s = $"Hello world
)CB", "untermInterp.cb");
        CHECK_FALSE(untermStr.ok);
        CHECK(untermStr.error.kind == "LexerError");
        CHECK(untermStr.error.message.find("Unterminated interpolated string literal") != std::string::npos);

        const auto untermHole = cb::tokenize(R"CB(dim s = $"Hello {name)CB", "untermHole.cb");
        CHECK_FALSE(untermHole.ok);
        CHECK(untermHole.error.kind == "LexerError");
        CHECK(untermHole.error.message.find("Unterminated interpolation block '{...}'") != std::string::npos);
    }
}

TEST_CASE("Fallible UFCS scope forwarding, enum aliases, for-in over bytes/aliases, and LSP enum/bytes parity", "[analyzer][emitter][lsp]") {
    SECTION("Rejects unhandled fallible UFCS receiver calls and bubbles them in can error functions") {
        const std::string badUfcs = R"CB(
fn decodeUnhandled()
    dim raw = b"hello"
    dim s = raw.toUtf8()
    print(s)
end fn
)CB";
        const auto badRes = cb::transpileSource(badUfcs, "badUfcs.cb");
        CHECK_FALSE(badRes.ok);
        CHECK(badRes.error.kind == "AnalyzerError");
        CHECK(badRes.error.message.find("Unhandled error") != std::string::npos);

        const std::string goodUfcs = R"CB(
enum Mode
    fast
    safe
end enum

alias ModeAlias = Mode
alias IntList = list[int]

fn parseAndSum(raw as bytes, nums as IntList) as ModeAlias can error
    dim decoded = raw.toUtf8()
    dim total = 0
    for b in raw
        total = total + b
    end for
    for n in nums
        total = total + n
    end for
    if total > 0 and decoded.len() > 0
        return Mode.fast
    end if
    return Mode.safe
end fn
)CB";
        const auto goodRes = cb::transpileSource(goodUfcs, "goodUfcs.cb");
        INFO("Transpile diagnostic: " << cb::formatDiagnostic(goodRes.error));
        REQUIRE(goodRes.ok);
        CHECK(goodRes.cppCode.find("auto [_val1, _err1] = cbBytesToUtf8(raw);") != std::string::npos);
        CHECK(goodRes.cppCode.find("if (cbHasError(_err1)) {") != std::string::npos);
        CHECK(goodRes.cppCode.find("return { static_cast<Mode>(0), _err1 };") != std::string::npos);
    }

    SECTION("LSP supports enum symbols/hover/completions and bytes receiver completions") {
        cb::LspServerState state{};
        const std::string uri = "file:///ws/enum_bytes_lsp.cb";
        const std::string text = R"CB(pub enum Color
    red = 1
    blue = 2
end enum
fn demo()
    dim buf = b"abc"
    dim c = Color.red
end fn
)CB";
        state.openDocuments[uri] = text;
        std::ostringstream dummyOut;
        cb::publishDocumentDiagnostics(state, uri, text, dummyOut);
        const cb::LspDocumentSnapshot snap = cb::getOrBuildSnapshot(state, uri);
        REQUIRE(snap.valid);

        const cb::CursorTokenContext enumMemberCtx = cb::resolveCursorToken(text, "/ws/enum_bytes_lsp.cb", 7, 20);
        REQUIRE(enumMemberCtx.found);
        CHECK(enumMemberCtx.hasDotReceiver);
        CHECK(enumMemberCtx.receiverName == "Color");
        const cb::ResolvedCursorSymbol hoverInfo = cb::resolveCursorSymbolInfo(snap, "/ws/enum_bytes_lsp.cb", enumMemberCtx);
        REQUIRE(hoverInfo.hoverMarkdown.has_value());
        CHECK(hoverInfo.hoverMarkdown.value().find("(enum member) Color.red = 1") != std::string::npos);

        std::vector<cb::CompletionItemEntry> dotEnumItems;
        std::unordered_map<std::string, bool> seenEnum;
        cb::collectDotCompletions(snap, "", std::string("demo"), "Color", "", dotEnumItems, seenEnum);
        CHECK(dotEnumItems.size() == 2);

        std::vector<cb::CompletionItemEntry> dotBytesItems;
        std::unordered_map<std::string, bool> seenBytes;
        cb::collectDotCompletions(snap, "", std::string("demo"), "buf", "toU", dotBytesItems, seenBytes);
        REQUIRE(dotBytesItems.size() == 1);
        CHECK(dotBytesItems[0].label == "toUtf8");

        std::vector<cb::CompletionItemEntry> typeItems;
        std::unordered_map<std::string, bool> seenTypes;
        cb::collectTypeAnnotationCompletions(snap, "", "by", typeItems, seenTypes);
        REQUIRE(!typeItems.empty());
        CHECK(typeItems[0].label == "bytes");
    }
}

TEST_CASE("Language clarity and utility polish: compound assignments, modulo, module constants, defaults, container methods, indexed loops, and guardrails", "[ergonomics][constants][defaults][containers][guardrails]") {
    SECTION("Compound assignments (+=, -=, *=, /=, %=, &=) and modulo (%) desugar cleanly") {
        const std::string code = R"CB(
fn main()
    dim x = 10
    x += 5
    x -= 3
    x *= 4
    x /= 6
    x %= 5
    dim msg = "count="
    msg &= x
    print(msg)
end fn
)CB";
        const auto res = cb::transpileSource(code, "compound.cb");
        INFO("Diagnostic: " << cb::formatDiagnostic(res.error));
        REQUIRE(res.ok);
        CHECK(res.cppCode.find("x = (x + 5);") != std::string::npos);
        CHECK(res.cppCode.find("x = (x - 3);") != std::string::npos);
        CHECK(res.cppCode.find("x = (x * 4);") != std::string::npos);
        CHECK(res.cppCode.find("x = (x / 6);") != std::string::npos);
        CHECK(res.cppCode.find("x = (x % 5);") != std::string::npos);
        CHECK(res.cppCode.find("msg = (msg + cbToStr(x));") != std::string::npos);
    }

    SECTION("Top-level [pub] const declarations, module qualification, visibility, and immutability") {
        const std::string modSource = R"CB(
pub const defaultPort = 8080
const secretKey = "hidden"
)CB";
        const std::string mainSource = R"CB(
import "cfg.cb"

const localLimit as int = 100

fn main()
    dim p = cfg.defaultPort + localLimit
    print(p)
end fn
)CB";
        std::unordered_map<std::string, std::string> memFiles;
        memFiles["cfg.cb"] = modSource;
        memFiles["main.cb"] = mainSource;
        const auto res = cb::transpileSource(mainSource, "main.cb", memFiles);
        INFO("Diagnostic: " << cb::formatDiagnostic(res.error));
        REQUIRE(res.ok);
        CHECK(res.cppCode.find("inline const int64_t cb_cfg__defaultPort = 8080;") != std::string::npos);
        CHECK(res.cppCode.find("inline const int64_t localLimit = 100;") != std::string::npos);
        CHECK(res.cppCode.find("(cb_cfg__defaultPort + localLimit)") != std::string::npos);

        const std::string badPrivConst = R"CB(
import "cfg.cb"
fn main()
    print(cfg.secretKey)
end fn
)CB";
        const auto privRes = cb::transpileSource(badPrivConst, "main.cb", memFiles);
        CHECK_FALSE(privRes.ok);
        CHECK(privRes.error.message.find("Constant 'secretKey' is private to module 'cfg'") != std::string::npos);

        const std::string badReassignConst = R"CB(
const maxRetries = 3
fn main()
    maxRetries += 1
end fn
)CB";
        const auto reassignRes = cb::transpileSource(badReassignConst, "reassignConst.cb");
        CHECK_FALSE(reassignRes.ok);
        CHECK(reassignRes.error.message.find("Cannot reassign const variable 'maxRetries'") != std::string::npos);
    }

    SECTION("Struct field defaults and function parameter defaults") {
        const std::string code = R"CB(
const defaultHost = "127.0.0.1"

type ServerConfig
    host as str = defaultHost
    port as int = 8080
    tls as bool = false
end type

fn connect(host as str = "localhost", port as int = 8080, timeoutMs as int = 5000) as str
    return $"{host}:{port} (t={timeoutMs})"
end fn

fn main()
    dim cfg1 = ServerConfig()
    dim cfg2 = ServerConfig(port=9090)
    dim s1 = connect()
    dim s2 = connect("10.0.0.1", timeoutMs=1500)
    print(cfg1.host, cfg2.port, s1, s2)
end fn
)CB";
        const auto res = cb::transpileSource(code, "defaults.cb");
        INFO("Diagnostic: " << cb::formatDiagnostic(res.error));
        REQUIRE(res.ok);
        CHECK(res.cppCode.find("std::string host = defaultHost;") != std::string::npos);
        CHECK(res.cppCode.find("int64_t port = 8080;") != std::string::npos);
        CHECK(res.cppCode.find(R"CB(ServerConfig{defaultHost, 9090, false})CB") != std::string::npos);
        CHECK(res.cppCode.find(R"CB(connect(std::string("localhost"), 8080, 5000))CB") != std::string::npos);
        CHECK(res.cppCode.find(R"CB(connect(std::string("10.0.0.1"), 8080, 1500))CB") != std::string::npos);

        const std::string badByrefDefault = R"CB(
fn bump(byref x as int = 0)
    x += 1
end fn
)CB";
        const auto badByrefRes = cb::transpileSource(badByrefDefault, "badByref.cb");
        CHECK_FALSE(badByrefRes.ok);
        CHECK(badByrefRes.error.message.find("'byref' parameter 'x' cannot have a default value") != std::string::npos);

        const std::string missingReqArg = R"CB(
fn greet(name as str, suffix as str = "!")
    print(name & suffix)
end fn
fn main()
    greet()
end fn
)CB";
        const auto missingRes = cb::transpileSource(missingReqArg, "missingArg.cb");
        CHECK_FALSE(missingRes.ok);
        CHECK(missingRes.error.message.find("Missing required argument for parameter 'name'") != std::string::npos);
    }

    SECTION("Container receiver methods (has, remove, clear, pop) and const receiver checks") {
        const std::string code = R"CB(
fn main()
    dim d as dict[str, int] = {"a": 1, "b": 2}
    if d.has("a")
        dim removed = d.remove("a")
    end if
    d.clear()

    dim items as list[str] = ["x", "y"]
    dim lastItem = items.pop()
    items.clear()

    dim buf = b"abc"
    dim lastByte = buf.pop()
    buf.clear()
end fn
)CB";
        const auto res = cb::transpileSource(code, "containers.cb");
        INFO("Diagnostic: " << cb::formatDiagnostic(res.error));
        REQUIRE(res.ok);
        CHECK(res.cppCode.find(R"CB((d.count(std::string("a")) > 0))CB") != std::string::npos);
        CHECK(res.cppCode.find(R"CB((d.erase(std::string("a")) > 0))CB") != std::string::npos);
        CHECK(res.cppCode.find("d.clear();") != std::string::npos);
        CHECK(res.cppCode.find("cbListPop(items)") != std::string::npos);
        CHECK(res.cppCode.find("cbBytesPop(buf)") != std::string::npos);

        const std::string badConstPop = R"CB(
fn main()
    const items as list[int] = [1, 2, 3]
    dim x = items.pop()
end fn
)CB";
        const auto badPopRes = cb::transpileSource(badConstPop, "badPop.cb");
        CHECK_FALSE(badPopRes.ok);
        CHECK(badPopRes.error.message.find("Cannot call '.pop()' on const variable 'items'") != std::string::npos);

        const std::string badConstRemove = R"CB(
fn main()
    const d as dict[str, int] = {"a": 1}
    dim ok = d.remove("a")
end fn
)CB";
        const auto badRemRes = cb::transpileSource(badConstRemove, "badRemove.cb");
        CHECK_FALSE(badRemRes.ok);
        CHECK(badRemRes.error.message.find("Cannot call '.remove()' on const variable 'd'") != std::string::npos);
    }

    SECTION("Indexed for i, item in list/bytes and negative step countdown loops") {
        const std::string code = R"CB(
fn main()
    dim names as list[str] = ["alice", "bob"]
    for i, name in names
        print($"{i}: {name}")
    end for

    dim raw = b"AB"
    for idx, b in raw
        print(idx + b)
    end for

    for n = 10 to 0 step -2
        print(n)
    end for
end fn
)CB";
        const auto res = cb::transpileSource(code, "indexedFor.cb");
        INFO("Diagnostic: " << cb::formatDiagnostic(res.error));
        REQUIRE(res.ok);
        CHECK(res.cppCode.find("for (int64_t i = 0; i < static_cast<int64_t>(_iter_1.size()); ++i)") != std::string::npos);
        CHECK(res.cppCode.find("for (int64_t n = 10; n >= 0; n += (-2))") != std::string::npos);
    }

    SECTION("Guardrails: retired 'as err' return shorthand, untyped empty []/{}, and non-cptr null") {
        const std::string legacyAsErr = R"CB(
fn checkVal(x as int) as err
    return
end fn
)CB";
        const auto errRes = cb::transpileSource(legacyAsErr, "legacyAsErr.cb");
        CHECK_FALSE(errRes.ok);
        CHECK(errRes.error.kind == "ParseError");
        CHECK(errRes.error.message.find("Return type 'as err' is removed; use 'can error'") != std::string::npos);

        const std::string untypedEmptyList = R"CB(
fn main()
    dim items = []
end fn
)CB";
        const auto emptyListRes = cb::transpileSource(untypedEmptyList, "emptyList.cb");
        CHECK_FALSE(emptyListRes.ok);
        CHECK(emptyListRes.error.message.find("Cannot infer element type of empty list '[]'") != std::string::npos);

        const std::string untypedEmptyDict = R"CB(
fn main()
    dim lookup = {}
end fn
)CB";
        const auto emptyDictRes = cb::transpileSource(untypedEmptyDict, "emptyDict.cb");
        CHECK_FALSE(emptyDictRes.ok);
        CHECK(emptyDictRes.error.message.find("Cannot infer key/value types of empty dict '{}'") != std::string::npos);

        const std::string badNullStr = R"CB(
fn main()
    dim s as str = null
end fn
)CB";
        const auto nullStrRes = cb::transpileSource(badNullStr, "nullStr.cb");
        CHECK_FALSE(nullStrRes.ok);
        CHECK(nullStrRes.error.message.find("only 'cptr' can be 'null'") != std::string::npos);

        const std::string validNullCptr = R"CB(
fn main()
    dim h as cptr = null
    if h == null
        print("null handle")
    end if
end fn
)CB";
        const auto okCptrRes = cb::transpileSource(validNullCptr, "okCptr.cb");
        INFO("Diagnostic: " << cb::formatDiagnostic(okCptrRes.error));
        REQUIRE(okCptrRes.ok);
        CHECK(okCptrRes.cppCode.find("void* h = nullptr;") != std::string::npos);
        CHECK(okCptrRes.cppCode.find("(h == nullptr)") != std::string::npos);
    }

    SECTION("LSP surfaces top-level constants and new container receiver methods") {
        cb::LspServerState state{};
        const std::string uri = "file:///ws/const_and_containers.cb";
        const std::string text = R"CB(pub const maxConnections = 128
fn demo()
    dim table as dict[str, int] = {"k": 1}
    dim n = maxConnections
end fn
)CB";
        state.openDocuments[uri] = text;
        std::ostringstream dummyOut;
        cb::publishDocumentDiagnostics(state, uri, text, dummyOut);
        const cb::LspDocumentSnapshot snap = cb::getOrBuildSnapshot(state, uri);
        REQUIRE(snap.valid);

        const cb::CursorTokenContext constRefCtx = cb::resolveCursorToken(text, "/ws/const_and_containers.cb", 4, 15);
        REQUIRE(constRefCtx.found);
        const cb::ResolvedCursorSymbol constHover = cb::resolveCursorSymbolInfo(snap, "/ws/const_and_containers.cb", constRefCtx);
        REQUIRE(constHover.hoverMarkdown.has_value());
        CHECK(constHover.hoverMarkdown.value().find("pub const maxConnections as int") != std::string::npos);
        REQUIRE(constHover.definition.has_value());
        CHECK(constHover.definition->line == 1);

        std::vector<cb::CompletionItemEntry> dictDotItems;
        std::unordered_map<std::string, bool> seenDict;
        cb::collectDotCompletions(snap, "", std::string("demo"), "table", "", dictDotItems, seenDict);
        bool foundHas = false;
        bool foundRemove = false;
        bool foundClear = false;
        for (const auto& item : dictDotItems) {
            if (item.label == "has") foundHas = true;
            if (item.label == "remove") foundRemove = true;
            if (item.label == "clear") foundClear = true;
        }
        CHECK(foundHas);
        CHECK(foundRemove);
        CHECK(foundClear);
    }
}

TEST_CASE("String indexing s[i], built-in s.byteAt(i), std/str compiler helpers, and std/os raw stdio", "[str_index][byteAt][stdlib]") {
    SECTION("s[i] emits cbStrIndex and built-in s.byteAt(i) works without importing std/str") {
        const std::string code = R"CB(
fn main() as int
    dim s = "CleanBasic"
    dim ch = s[0]
    if s[1] == "l" and s[-1] == "" and s[999] == ""
        print(ch & ":" & s.byteAt(0) & ":" & s.byteAt(-1))
    end if
    return 0
end fn
)CB";
        const auto res = cb::transpileSource(code, "strIndexAndByteAt.cb");
        INFO("Diagnostic: " << cb::formatDiagnostic(res.error));
        REQUIRE(res.ok);
        CHECK(res.cppCode.find("cbStrIndex(s, 0)") != std::string::npos);
        CHECK(res.cppCode.find("cbStrByteAtFast(s, 0)") != std::string::npos);
    }

    SECTION("Analyzer rejects mutating string index s[i] = val in place and validates .byteAt(idx) arity") {
        const std::string badMutCode = R"CB(
fn main()
    dim s = "hello"
    s[0] = "H"
end fn
)CB";
        const auto mutRes = cb::transpileSource(badMutCode, "badStrIndexMut.cb");
        CHECK_FALSE(mutRes.ok);
        CHECK(mutRes.error.kind == "AnalyzerError");
        CHECK(mutRes.error.message.find("Cannot mutate string index in place; strings are immutable by index in CleanBasic.") != std::string::npos);

        const std::string badByteAtArity = R"CB(
fn main()
    dim s = "hello"
    dim b = s.byteAt()
end fn
)CB";
        const auto arityRes = cb::transpileSource(badByteAtArity, "badByteAtArity.cb");
        CHECK_FALSE(arityRes.ok);
        CHECK(arityRes.error.message.find("Missing required argument for parameter 'idx'") != std::string::npos);
    }

    SECTION("std/str escapeCpp, unescape, asciiIsAlnum, and asciiIsHexDigit") {
        const std::string unescapeCode = R"CB(
import "std/str"
fn main() can error
    dim raw = str.fromChar(92) & "x41"
    dim decoded = str.unescape(raw)
    dim esc = decoded.escapeCpp()
    dim alnum = decoded.asciiIsAlnum()
    dim hex = decoded.asciiIsHexDigit()
end fn
)CB";
        const auto res = cb::transpileSource(unescapeCode, "unescapeCheck.cb");
        INFO("Diagnostic: " << cb::formatDiagnostic(res.error));
        REQUIRE(res.ok);
        CHECK(res.cppCode.find("cb_std_str__unescape(") != std::string::npos);
        CHECK(res.cppCode.find("cb_std_str__escapeCpp(") != std::string::npos);
        CHECK(res.cppCode.find("cb_std_str__asciiIsAlnum(") != std::string::npos);
        CHECK(res.cppCode.find("cb_std_str__asciiIsHexDigit(") != std::string::npos);
    }

    SECTION("LSP completes and hovers built-in .byteAt on str") {
        cb::LspServerState state{};
        const std::string uri = "file:///ws/str_byteat.cb";
        const std::string text = R"CB(fn demo()
    dim tokenText = "ident"
    dim b = tokenText.byteAt(0)
end fn
)CB";
        state.openDocuments[uri] = text;
        std::ostringstream dummyOut;
        cb::publishDocumentDiagnostics(state, uri, text, dummyOut);
        const cb::LspDocumentSnapshot snap = cb::getOrBuildSnapshot(state, uri);
        REQUIRE(snap.valid);

        std::vector<cb::CompletionItemEntry> strDotItems;
        std::unordered_map<std::string, bool> seenStr;
        cb::collectDotCompletions(snap, "", std::string("demo"), "tokenText", "", strDotItems, seenStr);
        bool foundByteAt = false;
        for (const auto& item : strDotItems) {
            if (item.label == "byteAt") {
                foundByteAt = true;
            }
        }
        CHECK(foundByteAt);

        const auto hoverOpt = cb::formatBuiltinHoverMarkdown("byteAt");
        REQUIRE(hoverOpt.has_value());
        CHECK(hoverOpt.value().find("fn .byteAt(idx as int) as int") != std::string::npos);
    }
}

TEST_CASE("Direct return fallibleCall() and top-level fallible statement guardrail", "[analyzer][emitter][fallible]") {
    SECTION("Supports return fallibleCall() in can error functions, on error blocks, and bytes.toUtf8()") {
        const std::string code = R"CB(
fn parseNum(s as str) as int can error
    if s == ""
        error("empty input", code=400)
    end if
    return 42
end fn

fn delegateParse(s as str) as int can error
    return parseNum(s)
end fn

fn decodeBytes(raw as bytes) as str can error
    return raw.toUtf8()
end fn

fn safeParse(s as str) as int
    on error e
        return -1
    end error
    return delegateParse(s)
end fn

fn main()
    print(safeParse("ok"))
    print(safeParse(""))
end fn
)CB";
        const auto res = cb::transpileSource(code, "returnFallible.cb");
        INFO("Diagnostic: " << cb::formatDiagnostic(res.error));
        REQUIRE(res.ok);
        CHECK(res.cppCode.find("auto [_val1, _err1] = parseNum(s);") != std::string::npos);
        CHECK(res.cppCode.find("if (cbHasError(_err1)) {") != std::string::npos);
        CHECK(res.cppCode.find("return { {}, _err1 };") != std::string::npos);
        CHECK(res.cppCode.find(R"CB(return { _val1, CbError{"", 0} };)CB") != std::string::npos);
        CHECK(res.cppCode.find("auto [_val2, _err2] = cbBytesToUtf8(raw);") != std::string::npos);
        CHECK(res.cppCode.find("auto [_val3, _err3] = delegateParse(s);") != std::string::npos);
        CHECK(res.cppCode.find("return _val3;") != std::string::npos);
    }

    SECTION("Rejects unhandled return fallibleCall() inside infallible function without on error") {
        const std::string badReturn = R"CB(
fn risky() as int can error
    return 1
end fn

fn caller() as int
    return risky()
end fn
)CB";
        const auto res = cb::transpileSource(badReturn, "badReturnFallible.cb");
        CHECK_FALSE(res.ok);
        CHECK(res.error.kind == "AnalyzerError");
        CHECK(res.error.message.find("Unhandled error: function returns an error but caller does not declare 'can error' or have an active 'on error' block") != std::string::npos);
    }

    SECTION("Rejects nested fallible calls inside sub-expressions even in can error functions") {
        const std::string nestedInArg = R"CB(
fn stepOne() as int can error
    return 10
end fn

fn caller() can error
    dim items as list[int] = [1]
    items.push(stepOne())
end fn
)CB";
        const auto resArg = cb::transpileSource(nestedInArg, "nestedInArg.cb");
        CHECK_FALSE(resArg.ok);
        CHECK(resArg.error.kind == "AnalyzerError");
        CHECK(resArg.error.message.find("Fallible call to 'stepOne' must be assigned to a variable, returned directly, or called as a standalone statement.") != std::string::npos);

        const std::string nestedInBinOp = R"CB(
fn stepOne() as int can error
    return 10
end fn

fn caller() as int can error
    return 1 + stepOne()
end fn
)CB";
        const auto resBin = cb::transpileSource(nestedInBinOp, "nestedInBinOp.cb");
        CHECK_FALSE(resBin.ok);
        CHECK(resBin.error.kind == "AnalyzerError");
        CHECK(resBin.error.message.find("Fallible call to 'stepOne' must be assigned to a variable, returned directly, or called as a standalone statement.") != std::string::npos);

        const std::string nestedInCompound = R"CB(
fn stepOne() as int can error
    return 10
end fn

fn caller() can error
    dim total = 0
    total += stepOne()
end fn
)CB";
        const auto resComp = cb::transpileSource(nestedInCompound, "nestedInCompound.cb");
        CHECK_FALSE(resComp.ok);
        CHECK(resComp.error.kind == "AnalyzerError");
        CHECK(resComp.error.message.find("Fallible call to 'stepOne' must be assigned to a variable, returned directly, or called as a standalone statement.") != std::string::npos);

        const std::string nestedToUtf8 = R"CB(
fn caller(raw as bytes) can error
    if raw.toUtf8() == "ok"
        print("ok")
    end if
end fn
)CB";
        const auto resUtf8 = cb::transpileSource(nestedToUtf8, "nestedToUtf8.cb");
        CHECK_FALSE(resUtf8.ok);
        CHECK(resUtf8.error.kind == "AnalyzerError");
        CHECK(resUtf8.error.message.find("Fallible call to 'toUtf8' must be assigned to a variable, returned directly, or called as a standalone statement.") != std::string::npos);
    }
}

TEST_CASE("Auto-included std/str prelude, bytes.fromHex vs str.fromHex cleanup, and safe container/buffer indexing", "[prelude][fromHex][containers]") {
    SECTION("std/str methods and str.* calls work without import 'std/str' and compile E2E") {
        const std::string src = R"CB(
fn main() as int can error
    dim raw = "  alpha,beta,42  "
    dim trimmed = raw.trim()
    dim parts = trimmed.split(",")
    dim num = parts[2].toInt()
    dim joined = str.join([parts[0], parts[1]], "-")
    if trimmed.contains("beta") and joined == "alpha-beta" and num == 42
        return 0
    end if
    return 1
end fn
)CB";
        const auto res = cb::transpileSource(src, "zeroImportStr.cb");
        REQUIRE(res.ok);
        CHECK(res.cppCode.find("str_bridge.hpp") != std::string::npos);
        CHECK(res.cppCode.find("cb_std_str__trim(") != std::string::npos);
        CHECK(res.cppCode.find("cb_std_str__split(") != std::string::npos);
        CHECK(res.cppCode.find("cb_std_str__toInt(") != std::string::npos);
        CHECK(res.cppCode.find("cb_std_str__join(") != std::string::npos);
    }

    SECTION("Programs that do not reference std/str omit str_bridge.hpp and cb_std_str__* definitions") {
        const std::string src = R"CB(
fn main() as int
    dim x = 20 + 22
    print("answer=" & x.toStr())
    return 0
end fn
)CB";
        const auto res = cb::transpileSource(src, "unusedPrelude.cb");
        REQUIRE(res.ok);
        CHECK(res.cppCode.find("str_bridge.hpp") == std::string::npos);
        CHECK(res.cppCode.find("cb_std_str__") == std::string::npos);
        CHECK(res.symbols.headers.empty());
    }

    SECTION("Explicit import 'std/str' remains idempotent without duplicate alias errors") {
        const std::string src = R"CB(
import "std/str"

fn main() as int
    dim s = "  ok  ".trim()
    print(s)
    return 0
end fn
)CB";
        const auto res = cb::transpileSource(src, "explicitStdStr.cb");
        REQUIRE(res.ok);
        CHECK(res.cppCode.find("str_bridge.hpp") != std::string::npos);
        CHECK(res.cppCode.find("cb_std_str__trim(") != std::string::npos);
    }

    SECTION("bytes.fromHex(hex) and hex.fromHex() succeed while bytes instance b.fromHex(...) is rejected") {
        const std::string validHex = R"CB(
fn main() as int can error
    dim b1 = bytes.fromHex("deadbeef")
    dim hexStr = "cafebabe"
    dim b2 = hexStr.fromHex()
    dim b3 = "0102".fromHex()
    print(b1.toHex() & ":" & b2.toHex() & ":" & b3.toHex())
    return 0
end fn
)CB";
        const auto okRes = cb::transpileSource(validHex, "validHex.cb");
        REQUIRE(okRes.ok);
        CHECK(okRes.cppCode.find(R"CPP(cbBytesFromHex(std::string("deadbeef")))CPP") != std::string::npos);
        CHECK(okRes.cppCode.find("cbBytesFromHex(hexStr)") != std::string::npos);
        CHECK(okRes.cppCode.find(R"CPP(cbBytesFromHex(std::string("0102")))CPP") != std::string::npos);

        const std::string badInstanceHex = R"CB(
fn main() as int can error
    dim b = b""
    dim out = b.fromHex("deadbeef")
    return out.len()
end fn
)CB";
        const auto badRes = cb::transpileSource(badInstanceHex, "badInstanceHex.cb");
        CHECK_FALSE(badRes.ok);
        CHECK(badRes.error.kind == "AnalyzerError");
        CHECK(badRes.error.message.find("'.fromHex()' cannot be called on a 'bytes' instance") != std::string::npos);
    }

    SECTION("dict.get(key, defaultVal), read-only dict parameter indexing, and bounds-safe list/bytes indexing") {
        const std::string src = R"CB(
fn sumReadOnly(nums as list[int], raw as bytes, d as dict[str, int]) as int
    return nums[0] + nums[99] + raw[0] + raw[99] + d["present"] + d.get("missing", 10)
end fn

fn mutateContainers(byref nums as list[int], byref d as dict[str, int])
    nums[0] = 50
    d["present"] = 100
end fn

fn main() as int
    dim nums as list[int] = [1, 2]
    dim raw = b"\x05"
    dim d as dict[str, int] = {"present": 7}
    mutateContainers(nums, d)
    return sumReadOnly(nums, raw, d)
end fn
)CB";
        const auto res = cb::transpileSource(src, "safeContainers.cb");
        REQUIRE(res.ok);
        CHECK(res.cppCode.find("cbListGet(nums, 0)") != std::string::npos);
        CHECK(res.cppCode.find("cbBytesGet(raw, 0)") != std::string::npos);
        CHECK(res.cppCode.find(R"CPP(cbDictAt(d, std::string("present")))CPP") != std::string::npos);
        CHECK(res.cppCode.find(R"CPP(cbDictGet(d, std::string("missing"), 10))CPP") != std::string::npos);
        CHECK(res.cppCode.find("nums[0] = 50;") != std::string::npos);
        CHECK(res.cppCode.find(R"CPP(d[std::string("present")] = 100;)CPP") != std::string::npos);
    }

    SECTION("LSP zero-import str completions/hover, dict.get, str.fromHex, and aligned infallible bytes endian signatures") {
        cb::LspServerState state;
        const std::string uri = "file:///workspace/lspPrelude.cb";
        const std::string src = R"CB(fn main()
    dim s = "hello"
    dim d as dict[str, int] = {"a": 1}
    dim b = b"\x01\x02"
    dim x = s.trim()
end fn
)CB";
        state.openDocuments[uri] = src;
        const auto snap = cb::getOrBuildSnapshot(state, uri);
        REQUIRE(snap.valid);
        const std::optional<std::string> fnKey = std::string("main");

        std::vector<cb::CompletionItemEntry> strItems;
        std::unordered_map<std::string, bool> seenStr;
        cb::collectDotCompletions(snap, "", fnKey, "s", "", strItems, seenStr);
        bool foundTrim = false;
        bool foundFromHex = false;
        for (const auto& item : strItems) {
            if (item.label == "trim") foundTrim = true;
            if (item.label == "fromHex" && item.detail == "fn() as bytes can error") foundFromHex = true;
        }
        CHECK(foundTrim);
        CHECK(foundFromHex);

        std::vector<cb::CompletionItemEntry> litItems;
        std::unordered_map<std::string, bool> seenLit;
        cb::collectDotCompletions(snap, "", fnKey, R"CPP("")CPP", "sp", litItems, seenLit);
        bool foundSplitOnLit = false;
        for (const auto& item : litItems) {
            if (item.label == "split") foundSplitOnLit = true;
        }
        CHECK(foundSplitOnLit);
        std::vector<cb::CompletionItemEntry> dictItems;
        std::unordered_map<std::string, bool> seenDict;
        cb::collectDotCompletions(snap, "", fnKey, "d", "g", dictItems, seenDict);
        REQUIRE(!dictItems.empty());
        CHECK(dictItems[0].label == "get");
        CHECK(dictItems[0].detail == "fn(key as str, defaultValue as int) as int");

        std::vector<cb::CompletionItemEntry> bytesItems;
        std::unordered_map<std::string, bool> seenBytes;
        cb::collectDotCompletions(snap, "", fnKey, "b", "", bytesItems, seenBytes);
        bool foundReadU16Le = false;
        bool foundWriteU16Le = false;
        bool foundBytesFromHex = false;
        for (const auto& item : bytesItems) {
            if (item.label == "readU16Le" && item.detail == "fn(offset as int) as int") foundReadU16Le = true;
            if (item.label == "writeU16Le" && item.detail == "fn(value as int, offset as int)") foundWriteU16Le = true;
            if (item.label == "fromHex") foundBytesFromHex = true;
        }
        CHECK(foundReadU16Le);
        CHECK(foundWriteU16Le);
        CHECK_FALSE(foundBytesFromHex);

        const auto cursorCtx = cb::resolveCursorToken(src, "/workspace/lspPrelude.cb", 5, 16);
        const auto symInfo = cb::resolveCursorSymbolInfo(snap, "/workspace/lspPrelude.cb", cursorCtx);
        REQUIRE(symInfo.hoverMarkdown.has_value());
        CHECK(symInfo.hoverMarkdown.value().find("pub fn trim(s as str) as str") != std::string::npos);
        REQUIRE(symInfo.definition.has_value());
        CHECK(symInfo.definition.value().fileName.find("stdlib/str/str.cb") != std::string::npos);
    }
}
TEST_CASE("Standard library std/test module, built-in assert(), and stateless fn(...) types", "[stdlib][test][assert][fn][e2e]") {
    SECTION("End-to-end execution of std/test, assert(), err.file/err.line, and stateless fn(...) references") {
        cb::testE2E::runE2EFixture("stdlib_test", "tests/cases/stdlib_test.cb");
    }

    SECTION("Analyzer validates assert() arguments, fallibility rules, and fn(...) signature matching") {
        const std::string unhandledAssert = R"(
fn bad()
    assert(1 == 1)
end fn
)";
        const auto res1 = cb::transpileSource(unhandledAssert, "bad_assert.cb");
        CHECK_FALSE(res1.ok);
        CHECK(res1.error.message.find("calls fallible function 'assert'") != std::string::npos);

        const std::string nonBoolAssert = R"(
fn bad() can error
    assert(123)
end fn
)";
        const auto res2 = cb::transpileSource(nonBoolAssert, "bad_assert_type.cb");
        CHECK_FALSE(res2.ok);
        CHECK(res2.error.message.find("First argument to 'assert(...)' must be of type 'bool'") != std::string::npos);

        const std::string mismatchedFn = R"(
alias UnaryOp = fn(x as int) as int

fn add(a as int, b as int) as int
    return a + b
end fn

fn main()
    dim f as UnaryOp = add
end fn
)";
        const auto res3 = cb::transpileSource(mismatchedFn, "bad_fn_sig.cb");
        CHECK_FALSE(res3.ok);
        CHECK(res3.error.message.find("Cannot assign mismatched function signature") != std::string::npos);
    }
}

TEST_CASE("Declarative CLI parser (std/cli), refactored cbgrep, cbdoc, and LSP '' doc-comments", "[stdlib][cli][cbdoc][lsp][e2e]") {
    SECTION("End-to-end execution of std/cli and std/str test suites (including str.unescape)") {
        cb::testE2E::runE2EFixture("stdlib_cli", "tests/cases/stdlib_cli.cb");
        cb::testE2E::runE2EFixture("stdlib_str", "tests/cases/stdlib_str.cb");
    }

    SECTION("Refactored examples/cbgrep.cb compiles and executes with std/cli") {
        const std::string workDir = "build/testArtifacts/example_cbgrep_cli";
        std::error_code ec;
        std::filesystem::create_directories(workDir, ec);
        REQUIRE(!ec);

        const std::string outCpp = workDir + "/build.cpp";
        const std::string outBin = workDir + "/cbgrep_bin";
        const std::string sampleTxt = workDir + "/sample.txt";
        const std::string stdoutTxt = workDir + "/stdout.txt";

        {
            std::ofstream f(sampleTxt, std::ios::binary | std::ios::trunc);
            f << "Alpha line\nBeta MATCH line\ngamma match two\n";
        }

        const auto buildRes = cb::buildFile("examples/cbgrep.cb", outCpp, true, outBin);
        INFO("buildFile error: " << cb::formatDiagnostic(buildRes.error));
        REQUIRE(buildRes.ok);

        const int helpRc = std::system(("./" + outBin + " --help > " + stdoutTxt + " 2>&1").c_str());
        CHECK(helpRc == 0);
        const std::string helpOut = cb::testE2E::readFileText(stdoutTxt);
        CHECK(helpOut.find("Usage: cbgrep [options] <pattern>") != std::string::npos);
        CHECK(helpOut.find("-i, --ignore-case") != std::string::npos);

        const int searchRc = std::system(("./" + outBin + " -in match " + sampleTxt + " > " + stdoutTxt + " 2>&1").c_str());
        CHECK(searchRc == 0);
        const std::string searchOut = cb::testE2E::readFileText(stdoutTxt);
        CHECK(searchOut.find("2:Beta MATCH line") != std::string::npos);
        CHECK(searchOut.find("3:gamma match two") != std::string::npos);

        const int countRc = std::system(("./" + outBin + " -ic match " + sampleTxt + " > " + stdoutTxt + " 2>&1").c_str());
        CHECK(countRc == 0);
        const std::string countOut = cb::testE2E::readFileText(stdoutTxt);
        CHECK(countOut.find("2") != std::string::npos);
    }

    SECTION("examples/cbdoc.cb extracts '' doc-comments and pub signatures into Markdown") {
        const std::string workDir = "build/testArtifacts/example_cbdoc";
        std::error_code ec;
        std::filesystem::create_directories(workDir, ec);
        REQUIRE(!ec);

        const std::string outCpp = workDir + "/build.cpp";
        const std::string outBin = workDir + "/cbdoc_bin";
        const std::string outMd = workDir + "/cli_ref.md";
        const std::string stdoutTxt = workDir + "/stdout.txt";

        const auto buildRes = cb::buildFile("examples/cbdoc.cb", outCpp, true, outBin);
        INFO("buildFile error: " << cb::formatDiagnostic(buildRes.error));
        REQUIRE(buildRes.ok);

        const int docRc = std::system(
            ("./" + outBin + " -t CleanBasicCliDocs -o " + outMd + " stdlib/cli/cli.cb > " + stdoutTxt + " 2>&1").c_str()
        );
        INFO("cbdoc output: " << cb::testE2E::readFileText(stdoutTxt));
        CHECK(docRc == 0);

        const std::string mdText = cb::testE2E::readFileText(outMd);
        CHECK(mdText.find("# CleanBasicCliDocs") != std::string::npos);
        CHECK(mdText.find("stdlib/cli/cli.cb") != std::string::npos);
        CHECK(mdText.find("(type)") != std::string::npos);
        CHECK(mdText.find("Declarative command-line parser definition") != std::string::npos);
        CHECK(mdText.find("newParser") != std::string::npos);
        CHECK(mdText.find("pub fn newParser(") != std::string::npos);
        CHECK(mdText.find("Creates a new declarative command-line Parser") != std::string::npos);
    }

    SECTION("LSP Hover, Completion documentation, and Signature Help include '' doc-comments") {
        std::stringstream clientToServer;
        std::stringstream serverToClient;

        cb::writeJsonrpcMessage(
            clientToServer,
            R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})"
        );

        const std::string docSource = R"(import "std/cli"

'' Represents a documented server endpoint.
pub type Endpoint
    host as str
    port as int
end type

'' Connects to the target Endpoint.
'' Raises an error if the port is invalid.
pub fn connectEndpoint(ep as Endpoint, timeoutMs as int = 1000) as bool can error
    return ep.port > 0 and timeoutMs > 0
end fn

fn main() as int
    on error e
        return 1
    end error
    dim p = cli.newParser("app")
    dim ep = Endpoint(host="localhost", port=8080)
    dim ok = connectEndpoint(ep, 500)
    return 0
end fn
)";

        {
            cb::JsonArena arena;
            const cb::JsonId msg = cb::jsonAddObject(arena);
            cb::jsonObjectSet(arena, msg, "jsonrpc", cb::jsonAddString(arena, "2.0"));
            cb::jsonObjectSet(arena, msg, "method", cb::jsonAddString(arena, "textDocument/didOpen"));
            const cb::JsonId params = cb::jsonAddObject(arena);
            const cb::JsonId td = cb::jsonAddObject(arena);
            cb::jsonObjectSet(arena, td, "uri", cb::jsonAddString(arena, "file:///ws/doc_test.cb"));
            cb::jsonObjectSet(arena, td, "languageId", cb::jsonAddString(arena, "cleanbasic"));
            cb::jsonObjectSet(arena, td, "version", cb::jsonAddInt(arena, 1));
            cb::jsonObjectSet(arena, td, "text", cb::jsonAddString(arena, docSource));
            cb::jsonObjectSet(arena, params, "textDocument", td);
            cb::jsonObjectSet(arena, msg, "params", params);
            cb::writeJsonrpcMessage(clientToServer, cb::serializeJson(arena, msg));
        }

        // Hover on 'connectEndpoint' call at line 20 (0-based), character 15 -> id=2
        cb::writeJsonrpcMessage(
            clientToServer,
            R"({"jsonrpc":"2.0","id":2,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///ws/doc_test.cb"},"position":{"line":20,"character":15}}})"
        );

        // Hover on 'cli.newParser' call at line 18 (0-based), character 18 -> id=3
        cb::writeJsonrpcMessage(
            clientToServer,
            R"({"jsonrpc":"2.0","id":3,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///ws/doc_test.cb"},"position":{"line":18,"character":18}}})"
        );

        // SignatureHelp inside 'connectEndpoint(ep, ' at line 20 (0-based), character 33 -> id=4
        cb::writeJsonrpcMessage(
            clientToServer,
            R"({"jsonrpc":"2.0","id":4,"method":"textDocument/signatureHelp","params":{"textDocument":{"uri":"file:///ws/doc_test.cb"},"position":{"line":20,"character":33}}})"
        );

        cb::writeJsonrpcMessage(clientToServer, R"({"jsonrpc":"2.0","id":99,"method":"shutdown","params":null})");
        cb::writeJsonrpcMessage(clientToServer, R"({"jsonrpc":"2.0","method":"exit"})");

        const int exitCode = cb::runLspServer(clientToServer, serverToClient);
        CHECK(exitCode == 0);

        auto readResponseForId = [&](int64_t targetId) -> cb::JsonParseResult {
            while (true) {
                const auto rawOpt = cb::readJsonrpcMessage(serverToClient);
                REQUIRE(rawOpt.has_value());
                auto parsed = cb::parseJson(rawOpt.value());
                REQUIRE(parsed.ok);
                const auto idOpt = cb::jsonGetInt(parsed.arena, cb::jsonGetField(parsed.arena, parsed.root, "id"));
                if (idOpt.has_value() && idOpt.value() == targetId) {
                    return parsed;
                }
            }
        };

        const auto hoverFn = readResponseForId(2);
        const cb::JsonId hoverFnRes = cb::jsonGetField(hoverFn.arena, hoverFn.root, "result");
        const cb::JsonId hoverFnContents = cb::jsonGetField(hoverFn.arena, hoverFnRes, "contents");
        const std::string hoverFnVal = cb::jsonGetString(hoverFn.arena, cb::jsonGetField(hoverFn.arena, hoverFnContents, "value")).value_or("");
        CHECK(hoverFnVal.find("pub fn connectEndpoint(ep as Endpoint, timeoutMs as int) as bool can error") != std::string::npos);
        CHECK(hoverFnVal.find("Connects to the target Endpoint.\nRaises an error if the port is invalid.") != std::string::npos);

        const auto hoverStdCli = readResponseForId(3);
        const cb::JsonId hoverStdRes = cb::jsonGetField(hoverStdCli.arena, hoverStdCli.root, "result");
        const cb::JsonId hoverStdContents = cb::jsonGetField(hoverStdCli.arena, hoverStdRes, "contents");
        const std::string hoverStdVal = cb::jsonGetString(hoverStdCli.arena, cb::jsonGetField(hoverStdCli.arena, hoverStdContents, "value")).value_or("");
        CHECK(hoverStdVal.find("pub fn newParser(name as str, version as str, description as str) as Parser") != std::string::npos);
        CHECK(hoverStdVal.find("Creates a new declarative command-line Parser") != std::string::npos);

        const auto sigResp = readResponseForId(4);
        const cb::JsonId sigRes = cb::jsonGetField(sigResp.arena, sigResp.root, "result");
        const cb::JsonId sigs = cb::jsonGetField(sigResp.arena, sigRes, "signatures");
        REQUIRE(std::holds_alternative<cb::JsonArray>(sigResp.arena.nodes[sigs]));
        const cb::JsonId sig0 = std::get<cb::JsonArray>(sigResp.arena.nodes[sigs]).elements[0];
        const cb::JsonId sigDoc = cb::jsonGetField(sigResp.arena, sig0, "documentation");
        const std::string sigDocVal = cb::jsonGetString(sigResp.arena, cb::jsonGetField(sigResp.arena, sigDoc, "value")).value_or("");
        CHECK(sigDocVal.find("Connects to the target Endpoint.") != std::string::npos);
    }

    SECTION("Self-hosting readiness: zero-copy cbListGet/cbDictAt and collection loop/param const enforcement") {
        // Verify runtime preamble cbListGet and cbDictAt return const references
        const auto res = cb::transpileSource(
            R"(type Item
    id as int
end type
fn readItem(items as list[Item], table as dict[str, list[Item]]) as int
    dim total = items[0].id + table["a"][0].id
    for it in items
        total += it.id
    end for
    for k, v in table
        total += v.len()
    end for
    return total
end fn
)",
            "self_host_read.cb"
        );
        REQUIRE(res.ok);
        CHECK(res.cppCode.find("inline const T& cbListGet(const std::vector<T>& v, int64_t idx)") != std::string::npos);
        CHECK(res.cppCode.find("inline const V& cbDictAt(const std::unordered_map<K, V>& d, const KArg& k)") != std::string::npos);
        CHECK(res.cppCode.find("for (const auto& it : items)") != std::string::npos);
        CHECK(res.cppCode.find("for (const auto& [k, v] : table)") != std::string::npos);

        // Verify mutating a collection for-in loop variable is rejected by the analyzer
        const auto badLoopMut = cb::checkSource(
            R"(fn badLoop(items as list[int])
    for x in items
        x = 10
    end for
end fn
)",
            "bad_loop.cb"
        );
        REQUIRE_FALSE(badLoopMut.ok);
        CHECK(badLoopMut.error.message.find("Cannot reassign const variable 'x'") != std::string::npos);

        // Verify mutating a non-byref parameter via chained index/field is rejected by the analyzer
        const auto badParamMut = cb::checkSource(
            R"(type Node
    vals as list[int]
end type
fn badParam(nodes as list[Node])
    nodes[0].vals.push(1)
end fn
)",
            "bad_param.cb"
        );
        REQUIRE_FALSE(badParamMut.ok);
        CHECK(badParamMut.error.message.find("const") != std::string::npos);
    }
}

TEST_CASE("Tagged union sum types, until loops, hex/binary/underscore numerics, bitwise methods, and list/dict helpers", "[union][loops][numerics][bitwise][containers][lsp][e2e]") {
    SECTION("Cross-module pub/private union visibility, variant constructors, .kind(), and select case payload bindings") {
        const std::string modAst = R"CB(
pub type Span
    line as int
    col as int
end type

pub union AstNode
    intVal as int
    textVal as str
    spanVal as Span
end union

union PrivateUnion
    secret as int
end union
)CB";
        const std::string mainSrc = R"CB(
import "ast.cb"

fn describe(n as ast.AstNode) as str
    select case n
        case intVal as v
            return n.kind() & ":" & v.toStr()
        case ast.AstNode.textVal as s
            return "text:" & s
        case spanVal as sp
            return "span:" & sp.line.toStr() & ":" & sp.col.toStr()
    end select
    return ""
end fn

fn main() as int
    dim a = ast.AstNode.intVal(0x10_00)
    dim b = ast.AstNode.textVal("ok")
    dim c = ast.AstNode.spanVal(ast.Span(line=0b1010, col=4))
    print(describe(a) & "|" & describe(b) & "|" & describe(c))
    return 0
end fn
)CB";
        std::unordered_map<std::string, std::string> memFiles;
        memFiles["ast.cb"] = modAst;
        memFiles["main.cb"] = mainSrc;
        const auto okRes = cb::transpileSource(mainSrc, "main.cb", memFiles);
        INFO("Diagnostic: " << cb::formatDiagnostic(okRes.error));
        REQUIRE(okRes.ok);
        CHECK(okRes.cppCode.find("struct cb_ast__AstNode {") != std::string::npos);
        CHECK(okRes.cppCode.find("std::in_place_index<0>") != std::string::npos);
        CHECK(okRes.cppCode.find("cbUnionKind(n)") != std::string::npos);
        CHECK(okRes.cppCode.find("std::get<0>(") != std::string::npos);

        const std::string badPrivUnion = R"CB(
import "ast.cb"
fn main()
    dim x = ast.PrivateUnion.secret(1)
end fn
)CB";
        const auto privRes = cb::transpileSource(badPrivUnion, "main.cb", memFiles);
        CHECK_FALSE(privRes.ok);
        CHECK(privRes.error.message.find("Union 'PrivateUnion' is private to module 'ast'") != std::string::npos);

        const std::string badVariant = R"CB(
import "ast.cb"
fn main()
    dim x = ast.AstNode.nonexistent(1)
end fn
)CB";
        const auto varRes = cb::transpileSource(badVariant, "main.cb", memFiles);
        CHECK_FALSE(varRes.ok);
        CHECK(varRes.error.message.find("Union 'AstNode' has no variant 'nonexistent'") != std::string::npos);

        const std::string badMultiBind = R"CB(
import "ast.cb"
fn main()
    dim n = ast.AstNode.intVal(1)
    select case n
        case intVal, textVal as v
            print(v)
    end select
end fn
)CB";
        const auto multiBindRes = cb::transpileSource(badMultiBind, "main.cb", memFiles);
        CHECK_FALSE(multiBindRes.ok);
        CHECK(multiBindRes.error.message.find("Case payload binding ('as <ident>') requires a single variant in 'case'") != std::string::npos);

        const std::string badIntBind = R"CB(
fn main()
    select case 10
        case 10 as v
            print(v)
    end select
end fn
)CB";
        const auto intBindRes = cb::transpileSource(badIntBind, "badIntBind.cb");
        CHECK_FALSE(intBindRes.ok);
        CHECK(intBindRes.error.message.find("Case payload binding ('as <ident>') is only supported when 'select case' target is a union") != std::string::npos);
    }

    SECTION("Unannotated list/dict literals, dict.keys(), list.slice/sort/contains/removeAt, until loops, and bitwise methods") {
        const std::string code = R"CB(
fn main() as int
    dim nums = [30, 10, 20]
    nums.sort()
    dim sub = nums.slice(0, 2)
    dim tail = nums.sliceFrom(1)
    dim has20 = nums.contains(20)
    dim removed = nums.removeAt(0)

    dim table = {"b": 2, "a": 1}
    dim ks = table.keys()

    dim mask = 0xFF_00.bitAnd(0x0F_00).bitOr(0b0000_0011).bitXor(1).shl(2).shr(1).bitNot()
    dim sum = 0
    for i = 0 until ks.len()
        sum += table[ks[i]]
    end for
    return sum + sub.len() + tail.len() + removed + mask
end fn
)CB";
        const auto res = cb::transpileSource(code, "unannotatedContainers.cb");
        INFO("Diagnostic: " << cb::formatDiagnostic(res.error));
        REQUIRE(res.ok);
        CHECK(res.cppCode.find("cbListSort(nums);") != std::string::npos);
        CHECK(res.cppCode.find("cbListSlice(nums, 0, 2)") != std::string::npos);
        CHECK(res.cppCode.find("cbListSliceFrom(nums, 1)") != std::string::npos);
        CHECK(res.cppCode.find("cbListContains(nums, 20)") != std::string::npos);
        CHECK(res.cppCode.find("cbListRemoveAt(nums, 0)") != std::string::npos);
        CHECK(res.cppCode.find("cbDictKeys(table)") != std::string::npos);
        CHECK(res.cppCode.find("cbBitShl(") != std::string::npos);
        CHECK(res.cppCode.find("cbBitShr(") != std::string::npos);

        const auto badSort = cb::checkSource(R"CB(
fn f()
    const xs = [3, 1, 2]
    xs.sort()
end fn
)CB", "badSort.cb");
        CHECK_FALSE(badSort.ok);
        CHECK(badSort.error.message.find("Cannot call '.sort()' on const variable 'xs'") != std::string::npos);

        const auto badRemAt = cb::checkSource(R"CB(
fn f()
    const xs = [3, 1, 2]
    dim v = xs.removeAt(0)
end fn
)CB", "badRemAt.cb");
        CHECK_FALSE(badRemAt.ok);
        CHECK(badRemAt.error.message.find("Cannot call '.removeAt()' on const variable 'xs'") != std::string::npos);
    }

    SECTION("LSP hover, definition, dot-completion, and signatureHelp for union types and variants") {
        cb::LspServerState state{};
        const std::string uri = "file:///ws/union_lsp.cb";
        const std::string text = R"CB(pub union TokenVal
    intTok as int
    strTok as str
end union
fn demo()
    dim tv = TokenVal.intTok(42)
    dim k = tv.kind()
end fn
)CB";
        state.openDocuments[uri] = text;
        std::ostringstream dummyOut;
        cb::publishDocumentDiagnostics(state, uri, text, dummyOut);
        const cb::LspDocumentSnapshot snap = cb::getOrBuildSnapshot(state, uri);
        REQUIRE(snap.valid);

        std::vector<cb::CompletionItemEntry> unionDotItems;
        std::unordered_map<std::string, bool> seenUnion;
        cb::collectDotCompletions(snap, "", std::string("demo"), "TokenVal", "", unionDotItems, seenUnion);
        CHECK(unionDotItems.size() == 2);

        std::vector<cb::CompletionItemEntry> instDotItems;
        std::unordered_map<std::string, bool> seenInst;
        cb::collectDotCompletions(snap, "", std::string("demo"), "tv", "", instDotItems, seenInst);
        bool foundKind = false;
        for (const auto& item : instDotItems) {
            if (item.label == "kind") {
                foundKind = true;
            }
        }
        CHECK(foundKind);

        const cb::CursorTokenContext varCtx = cb::resolveCursorToken(text, "/ws/union_lsp.cb", 6, 25);
        REQUIRE(varCtx.found);
        const cb::ResolvedCursorSymbol varHover = cb::resolveCursorSymbolInfo(snap, "/ws/union_lsp.cb", varCtx);
        REQUIRE(varHover.hoverMarkdown.has_value());
        CHECK(varHover.hoverMarkdown.value().find("(union variant) TokenVal.intTok(payload as int) as TokenVal") != std::string::npos);
        REQUIRE(varHover.definition.has_value());
        CHECK(varHover.definition->line == 2);
    }

    SECTION("Overloaded functions with shared parameter names, UFCS arity overloads, and cross-module UFCS Step 3") {
        // 1. Overloaded functions sharing parameter name 'target' with different types (str vs list[int]) and UFCS arity overloads
        const std::string overloadCode = R"CB(
type TypeRegistry
    items as list[str]
end type

fn findType(reg as TypeRegistry, target as str) as int
    return target.len() + reg.items.len()
end fn

fn findType(byref reg as TypeRegistry, target as list[int], tag as str) as int
    reg.items.push(tag)
    return target.len() + reg.items.len()
end fn

fn main() as int
    dim reg = TypeRegistry(items=["int", "str"])
    dim a = reg.findType("float")
    dim b = reg.findType([10, 20], "list")
    return a + b
end fn
)CB";
        const auto ovRes = cb::transpileSource(overloadCode, "overload_ufcs.cb");
        INFO("Overload UFCS diagnostic: " << cb::formatDiagnostic(ovRes.error));
        REQUIRE(ovRes.ok);
        CHECK(ovRes.cppCode.find(R"CB(findType(reg, std::string("float")))CB") != std::string::npos);
        CHECK(ovRes.cppCode.find(R"CB(findType(reg, {10, 20}, std::string("list")))CB") != std::string::npos);

        // Reject invalid argument count on overloaded function
        const std::string badOvSuffix = R"CB(
fn bad(r as TypeRegistry) as int
    return r.findType("a", "b", "c")
end fn
)CB";
        const auto badOv = cb::checkSource(overloadCode + badOvSuffix, "bad_ov.cb");
        CHECK_FALSE(badOv.ok);
        CHECK(badOv.error.message.find("Too many arguments in call to 'findType'") != std::string::npos);

        // 2. Cross-module UFCS Step 3 across imported modules (ast.cb defines AstArena, symbols.cb defines pub fn resolveAlias(arena as ast.AstArena, ...))
        std::unordered_map<std::string, std::string> virtualFiles;
        virtualFiles["/ws/ast.cb"] = R"CB(pub type AstArena
    types as list[str]
end type
)CB";
        virtualFiles["/ws/symbols.cb"] = R"CB(import "ast.cb"

pub fn resolveAlias(arena as ast.AstArena, tid as int) as str
    return arena.types[tid]
end fn

pub fn resolveAlias(arena as ast.AstArena, tid as int, fallback as str) as str
    if tid < 0 or tid >= arena.types.len()
        return fallback
    end if
    return arena.types[tid]
end fn
)CB";
        const std::string analyzerSrc = R"CB(import "ast.cb"
import "symbols.cb"

fn main() as int
    dim arena = ast.AstArena(types=["int", "str"])
    dim t0 = arena.resolveAlias(0)
    dim tFallback = arena.resolveAlias(99, "void")
    return t0.len() + tFallback.len()
end fn
)CB";
        virtualFiles["/ws/analyzer.cb"] = analyzerSrc;

        const auto crossUfcsCheck = cb::checkSource(analyzerSrc, "/ws/analyzer.cb", virtualFiles);
        INFO("Cross-module UFCS diagnostic: " << cb::formatDiagnostic(crossUfcsCheck.error));
        REQUIRE(crossUfcsCheck.ok);

        // Verify LSP dot-completion on 'arena' in analyzer.cb discovers 'resolveAlias' from imported symbols.cb
        cb::LspServerState lspState{};
        lspState.openDocuments["file:///ws/ast.cb"] = virtualFiles["/ws/ast.cb"];
        lspState.openDocuments["file:///ws/symbols.cb"] = virtualFiles["/ws/symbols.cb"];
        lspState.openDocuments["file:///ws/analyzer.cb"] = analyzerSrc;
        const cb::LspDocumentSnapshot snap = cb::getOrBuildSnapshot(lspState, "file:///ws/analyzer.cb");
        REQUIRE(snap.valid);

        std::vector<cb::CompletionItemEntry> dotItems;
        std::unordered_map<std::string, bool> seenDot;
        cb::collectDotCompletions(snap, "", std::string("main"), "arena", "resolve", dotItems, seenDot);
        bool foundResolveAlias = false;
        for (const auto& item : dotItems) {
            if (item.label == "resolveAlias") {
                foundResolveAlias = true;
            }
        }
        CHECK(foundResolveAlias);
    }

    SECTION("Self-hosting compiler construct conformance E2E fixture (tests/cases/selfhost_constructs.cb)") {
        cb::testE2E::runE2EFixture("selfhost_constructs", "tests/cases/selfhost_constructs.cb");
    }
}

TEST_CASE("Transpiler and LSP regression fixes for edge-case defects", "[regression][lsp][emitter][analyzer]") {
    SECTION("LSP Built-in Hover Fixes") {
        auto assertHover = cb::formatBuiltinHoverMarkdown("assert");
        REQUIRE(assertHover.has_value());
        CHECK(assertHover->find("fn assert") != std::string::npos);
        CHECK(assertHover->find("Asserts that cond is true") != std::string::npos);
        // Check newline after code block
        CHECK(assertHover->find("```\n") != std::string::npos);

        auto toStrHover = cb::formatBuiltinHoverMarkdown("toStr");
        REQUIRE(toStrHover.has_value());
        CHECK(toStrHover->find("fn .toStr()") != std::string::npos);

        auto lenHover = cb::formatBuiltinHoverMarkdown("len");
        REQUIRE(lenHover.has_value());
        CHECK(lenHover->find("```\n") != std::string::npos);
    }

    SECTION("LSP Signature Help for Built-in Receiver Methods") {
        std::string source = "fn main()\n  dim l = [1]\n  l.push(0)\nend fn";
        auto checkRes = cb::checkSource(source, "main.cb");
        REQUIRE(checkRes.ok);
        cb::LspDocumentSnapshot snap{};
        snap.valid = true;
        snap.arena = std::move(checkRes.arena);
        snap.symbols = std::move(checkRes.symbols);
        snap.program = std::move(checkRes.program);
        
        cb::CallSiteContext ctx;
        ctx.insideCall = true;
        ctx.hasReceiver = true;
        ctx.receiverName = "l";
        ctx.calleeName = "push";
        ctx.activeParam = 0;

        auto help = cb::resolveSignatureHelp(snap, "", std::string("main"), ctx);
        REQUIRE(help.has_value());
        CHECK(help->label.find(".push(item as int)") != std::string::npos);
        CHECK(help->paramLabels.size() == 1);
        CHECK(help->paramLabels[0] == "item as int");
    }

    SECTION("E2E Regression: Fallible void return, overloaded scope, and enum assignments") {
        cb::testE2E::runE2EFixture("regression_fixes", "tests/cases/regression_fixes.cb");
    }
}

TEST_CASE("Optimizations 1-3: Call-graph tree-shaking, fast linker detection, and alias/PCH fast paths", "[emitter][pipeline][symbols][optimization]") {
    SECTION("Emitter call-graph tree-shaking strips unreachable imported module and stdlib functions") {
        std::unordered_map<std::string, std::string> virtualFiles;
        virtualFiles["/ws/helper.cb"] = R"CB(fn transitiveInternal(x as int) as int
    return x * 2
end fn

fn defaultParamHelper() as int
    return 7
end fn

pub fn usedPublic(a as int, b as int = defaultParamHelper()) as int
    return transitiveInternal(a) + b
end fn

pub fn bareRefTarget(v as int) as int
    return v + 1
end fn

pub fn unusedPublic(x as int) as int
    return x + 999
end fn

fn unusedPrivate() as int
    return 42
end fn
)CB";
        const std::string mainSrc = R"CB(import "helper.cb"
import "std/fs"
import "std/os"

fn applyFn(f as fn(int) as int, x as int) as int
    return f(x)
end fn

fn localUnreferencedHelper() as int
    return 123
end fn

fn main() as int
    dim r1 = helper.usedPublic(10)
    dim r2 = applyFn(helper.bareRefTarget, r1)
    if fs.exists("README.md") and os.platform().len() > 0
        return r2
    end if
    return 0
end fn
)CB";
        virtualFiles["/ws/main.cb"] = mainSrc;

        const auto checkRes = cb::checkSource(mainSrc, "/ws/main.cb", virtualFiles);
        INFO("Tree-shaking check diagnostic: " << cb::formatDiagnostic(checkRes.error));
        REQUIRE(checkRes.ok);

        const std::string cpp = cb::emitProgram(checkRes.arena, checkRes.symbols, checkRes.program);

        // Root module functions (including unreferenced root helpers) are preserved
        CHECK(cpp.find("localUnreferencedHelper(") != std::string::npos);

        // Reachable imported functions (direct, transitive internal, default param, bare fn ref) are emitted
        CHECK(cpp.find("cb_helper__usedPublic(") != std::string::npos);
        CHECK(cpp.find("cb_helper__transitiveInternal(") != std::string::npos);
        CHECK(cpp.find("cb_helper__defaultParamHelper(") != std::string::npos);
        CHECK(cpp.find("cb_helper__bareRefTarget(") != std::string::npos);

        // Unreachable imported module functions are stripped
        CHECK(cpp.find("cb_helper__unusedPublic") == std::string::npos);
        CHECK(cpp.find("cb_helper__unusedPrivate") == std::string::npos);

        // Referenced stdlib functions are emitted while unused stdlib functions are stripped
        CHECK(cpp.find("cb_std_fs__exists(") != std::string::npos);
        CHECK(cpp.find("cb_std_os__platform(") != std::string::npos);
        CHECK(cpp.find("cb_std_fs__walkFiles") == std::string::npos);
        CHECK(cpp.find("cb_std_fs__removeAll") == std::string::npos);
        CHECK(cpp.find("cb_std_os__exec") == std::string::npos);
    }

    SECTION("Built-in type alias resolution fast path preserves primitive TypeIds and unwraps user aliases") {
        const std::string src = R"CB(alias MyInt = int
alias IntList = list[MyInt]

fn main() as int
    dim a as MyInt = 10
    dim b as IntList = [a, 20]
    return b[0]
end fn
)CB";
        const auto checkRes = cb::checkSource(src, "alias_fastpath.cb");
        REQUIRE(checkRes.ok);

        CHECK(cb::resolveAlias(checkRes.arena, checkRes.symbols, "", checkRes.symbols.typeInt) == checkRes.symbols.typeInt);
        CHECK(cb::resolveAlias(checkRes.arena, checkRes.symbols, "", checkRes.symbols.typeStr) == checkRes.symbols.typeStr);
        CHECK(cb::resolveAlias(checkRes.arena, checkRes.symbols, "", checkRes.symbols.typeBool) == checkRes.symbols.typeBool);

        const cb::TypeId myIntTid = checkRes.arena.findType("MyInt");
        REQUIRE(myIntTid != cb::INVALID_ID);
        CHECK(cb::resolveAlias(checkRes.arena, checkRes.symbols, "", myIntTid) == checkRes.symbols.typeInt);
    }

    SECTION("findExecutableInPath, detectFastLinkerFlag, and stat-based computeBackendPchHash") {
        const std::filesystem::path tmpDir = std::filesystem::temp_directory_path() / "cb_test_fast_ld_path";
        std::error_code ec;
        std::filesystem::create_directories(tmpDir, ec);
        const std::filesystem::path fakeMold = tmpDir / "mold";
        {
            std::ofstream out(fakeMold);
            out << "#!/bin/sh" << std::endl;
        }

        CHECK(cb::findExecutableInPath("mold", tmpDir.string()));
        CHECK_FALSE(cb::findExecutableInPath("nonexistent_linker_xyz", tmpDir.string()));
        CHECK(cb::detectFastLinkerFlag("zig c++ -target x86_64-linux-gnu").empty());

        std::filesystem::remove_all(tmpDir, ec);

        const cb::TargetSpec nativeSpec = cb::resolveTargetSpec("native");
        const std::string rootDir = cb::cleanbasicRootDir();
        const std::string h1 = cb::computeBackendPchHash("g++", nativeSpec, "0", rootDir);
        const std::string h2 = cb::computeBackendPchHash("g++", nativeSpec, "0", rootDir);
        const std::string hOpt3 = cb::computeBackendPchHash("g++", nativeSpec, "3", rootDir);
        CHECK(h1.size() == 16);
        CHECK(h1 == h2);
        CHECK(h1 != hOpt3);
    }
}
