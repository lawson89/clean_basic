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
dim x as int = 1
const y = "hello" & " world"
select case x
case 1
case else
end select
for i = 0 to 10
end for
if a and b or not c
else if d
end if
fn check() can error
end fn
try
catch e
end try
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
    CHECK(hasKw("select case"));
    CHECK(hasKw("case"));
    CHECK(hasKw("case else"));
    CHECK(hasKw("end select"));
    CHECK(hasKw("and"));
    CHECK(hasKw("or"));
    CHECK(hasKw("not"));
    CHECK(hasKw("else if"));
    CHECK(hasKw("end if"));
    CHECK(hasKw("try"));
    CHECK(hasKw("catch"));
    CHECK(hasKw("end try"));
    CHECK(hasKw("for"));
    CHECK(hasKw("to"));
    CHECK(hasKw("end for"));

    SECTION("Rejects forbidden C-style logical operators") {
        const std::vector<std::string> forbidden = {"a && b", "a || b", "!flag"};
        for (const auto& badOp : forbidden) {
            const auto badLex = cb::tokenize(badOp, "badOp.cb");
            CHECK_FALSE(badLex.ok);
            CHECK(badLex.error.kind == "LexerError");
        }
    }
}

TEST_CASE("Parser builds AST for control flow, ranges, and fallible signatures", "[parser]") {
    const std::string code = R"(
public const defaultTag = "score"

fn compute(byref items as list[int], tag as str) as int
    dim total = 0
    for i = 0 to 10
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
    const auto& fnDecl = *parseRes.arena.getDecl<cb::FnDecl>(fnDeclId);
    CHECK(fnDecl.name == "compute");
    REQUIRE(fnDecl.params.size() == 2);
    CHECK(fnDecl.params[0].isByref);

    SECTION("Parses classic BASIC 'for i = start to end' with inclusive RangeSpec") {
        const std::string forToCode = R"(fn loopTest() as int
    dim s = 0
    for i = 1 to 5
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
        const auto& fnFt = *parseFt.arena.getDecl<cb::FnDecl>(parseFt.program.declarations[0]);
        REQUIRE(fnFt.body.size() >= 2);
        const auto& forStmt = *parseFt.arena.getStmt<cb::ForStmt>(fnFt.body[1]);
        CHECK(forStmt.rangeSpec.hasRange);
    }

    SECTION("Parses 'can error' on value-returning and void functions and 'try / catch' blocks") {
        const std::string canErrCode = R"(
fn connect(ip as str) as int can error
    return 1
end fn

fn validate(port as int) can error
    if port <= 0
        error("bad port", 400)
    end if
end fn

fn run()
    try
        dim c = connect("127.0.0.1")
    catch e
        print(e.msg)
    end try
end fn
)";
        const auto lexCe = cb::tokenize(canErrCode, "canErr.cb");
        REQUIRE(lexCe.ok);
        const auto parseCe = cb::parseTokens(lexCe.tokens, "canErr.cb");
        REQUIRE(parseCe.ok);
        REQUIRE(parseCe.program.declarations.size() == 3);
        const auto& fn0 = *parseCe.arena.getDecl<cb::FnDecl>(parseCe.program.declarations[0]);
        const auto& fn1 = *parseCe.arena.getDecl<cb::FnDecl>(parseCe.program.declarations[1]);
        const auto& fn2 = *parseCe.arena.getDecl<cb::FnDecl>(parseCe.program.declarations[2]);
        CHECK(fn0.isFallible);
        CHECK(fn0.returnType != cb::INVALID_ID);
        CHECK(fn1.isFallible);
        CHECK(fn1.returnType != cb::INVALID_ID);
        CHECK(parseCe.arena.types[fn1.returnType].name == "void");
        REQUIRE(fn2.body.size() == 1);
        const auto* tryStmt = parseCe.arena.getStmt<cb::TryCatchStmt>(fn2.body[0]);
        REQUIRE(tryStmt != nullptr);
        CHECK(tryStmt->catchVarName == "e");
        CHECK(tryStmt->tryBody.size() == 1);
        CHECK(tryStmt->catchBody.size() == 1);
    }
}

TEST_CASE("Parser rejects invalid named argument syntax", "[parser]") {
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

TEST_CASE("Parser rejects removed redundant syntax and unclosed blocks", "[parser]") {
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

    SECTION("Reports ParseError on invalid string or byte literal escape sequences and overflowing 64-bit integer literals") {
        const std::vector<std::string> badLiterals = {
            R"(fn main()
    dim s = "bad\qescape"
end fn
)",
            R"(fn main()
    dim s = "bad\x1"
end fn
)",
            R"(fn main()
    dim s = $"interp \q {1}"
end fn
)",
            R"(fn main()
    dim b = b"bad\qescape"
end fn
)",
            R"(fn main()
    dim b = b"\x1"
end fn
)",
            R"(fn main()
    dim x = 999999999999999999999
end fn
)",
            R"(enum Huge
    v = 1
end enum
)"
        };
        for (const auto& snippet : badLiterals) {
            const auto lexRes = cb::tokenize(snippet, "bad_lit.cb");
            REQUIRE(lexRes.ok);
            const auto parseRes = cb::parseTokens(lexRes.tokens, "bad_lit.cb");
            CHECK_FALSE(parseRes.ok);
            CHECK(parseRes.error.kind == "ParseError");
        }
    }
}

TEST_CASE("Analyzer validates fallible calls, const immutability, and implicit bubbling", "[analyzer]") {
    SECTION("Rejects unhandled fallible call in assignment, expression statement, return, or print argument") {
        const std::vector<std::string> badSnippets = {
            R"CB(fn risky() as int can error
    return 1
end fn
fn caller()
    dim x = risky()
end fn
)CB",
            R"CB(fn risky() as int can error
    return 1
end fn
fn caller()
    risky()
end fn
)CB",
            R"CB(fn risky() as int can error
    return 1
end fn
fn caller() as int
    return risky()
end fn
)CB",
            R"CB(fn risky() as int can error
    return 1
end fn
fn caller()
    print("v=", risky())
end fn
)CB"
        };
        for (const auto& src : badSnippets) {
            const auto res = cb::transpileSource(src, "badFallible.cb");
            CHECK_FALSE(res.ok);
            CHECK(res.error.kind == "AnalyzerError");
        }
    }

    SECTION("Permits try / catch block that falls through without return, break, or continue") {
        const std::string fallthroughCode = R"(
fn risky() as int can error
    error("failed", 503)
    return 42
end fn

fn main() as int
    dim fallback = 0
    try
        dim v = risky()
        fallback = v
    catch e
        print("Handled error ", e.code, ": ", e.msg)
        fallback = -1
    end try
    return fallback
end fn
)";
        const auto lexFt = cb::tokenize(fallthroughCode, "fallthrough.cb");
        REQUIRE(lexFt.ok);
        auto parseFt = cb::parseTokens(lexFt.tokens, "fallthrough.cb");
        REQUIRE(parseFt.ok);
        const auto anaFt = cb::analyzeProgram(parseFt.arena, parseFt.program);
        INFO("Analyzer diagnostic: " << cb::formatDiagnostic(anaFt.error));
        REQUIRE(anaFt.ok);

        const auto& mainFn = *parseFt.arena.getDecl<cb::FnDecl>(parseFt.program.declarations[1]);
        REQUIRE(mainFn.body.size() >= 2);
        const auto& tryCatch = *parseFt.arena.getStmt<cb::TryCatchStmt>(mainFn.body[1]);
        REQUIRE(!tryCatch.tryBody.empty());
        CHECK(parseFt.arena.getStmt<cb::VarAssignment>(tryCatch.tryBody[0]) != nullptr);
    }

    SECTION("Rejects calling error(...) inside an infallible function or with non-int code argument") {
        const std::string badErrorInInfallible = R"(
fn notFallible() as int
    error("boom", 1)
    return 0
end fn
)";
        const auto resInf = cb::transpileSource(badErrorInInfallible, "badErrorInf.cb");
        CHECK_FALSE(resInf.ok);
        CHECK(resInf.error.kind == "AnalyzerError");
        CHECK(resInf.error.message.find("Cannot call 'error(...)' inside infallible function") != std::string::npos);

        const std::string badErrorCodeType = R"(
fn fallibleFn() as int can error
    error("boom", "notAnInt")
    return 0
end fn
)";
        const auto resCode = cb::transpileSource(badErrorCodeType, "badErrorCode.cb");
        CHECK_FALSE(resCode.ok);
        CHECK(resCode.error.kind == "AnalyzerError");
        CHECK(resCode.error.message.find("Second argument to 'error(...)' (code) must be of type 'int'") != std::string::npos);
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
    try
        dim res = middle()
    catch e
        print(e.code, ": ", e.msg)
    end try
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
        const auto& middleFn = *parseGood.arena.getDecl<cb::FnDecl>(middleDeclId);
        REQUIRE(!middleFn.body.empty());
        const cb::StmtId valStmtId = middleFn.body[0];
        CHECK(parseGood.arena.getStmt<cb::VarAssignment>(valStmtId) != nullptr);
    }
}

TEST_CASE("Emitter generates C++17 error bubbling for extern declarations", "[emitter]") {
    const std::string code = R"(
public extern type SQL_DB
end type
public extern fn connect(path as str) as SQL_DB can error
public extern fn SQL_close(db as SQL_DB)

fn processData() as int can error
    ' Implicit bubble (needs if (cbHasError(_err1)) return ...)
    dim db = connect("data.db")
    SQL_close(db)
    return 100
end fn

fn main()
    ' Explicit try / catch handling
    try
        dim records = processData()
    catch e
        print("Crash prevented (" & e.code & "): " & e.msg)
    end try
end fn
)";

    const auto res = cb::transpileSource(code, "section5.cb");
    REQUIRE(res.ok);

    const std::string& cpp = res.cppCode;
    CHECK(cpp.find("struct [[nodiscard]] CbError {") != std::string::npos);
    CHECK(cpp.find("[[nodiscard]] std::tuple<int64_t, CbError> processData()") != std::string::npos);
    CHECK(cpp.find(R"raw(auto [_val1, _err1] = connect(std::string("data.db"));)raw") != std::string::npos);
    CHECK(cpp.find("if (cbHasError(_err1)) {") != std::string::npos);
    CHECK(cpp.find("return { {}, _err1 };") != std::string::npos);
    CHECK(cpp.find("auto db = _val1;") != std::string::npos);
    CHECK(cpp.find("SQL_close(db);") != std::string::npos);
    CHECK(cpp.find(R"raw(return { 100, CbError{"", 0} };)raw") != std::string::npos);
    CHECK(cpp.find("CbError _catch_err_2{};") != std::string::npos);
    CHECK(cpp.find("do {") != std::string::npos);
    CHECK(cpp.find("auto [_val3, _err3] = processData();") != std::string::npos);
    CHECK(cpp.find("if (cbHasError(_err3)) {") != std::string::npos);
    CHECK(cpp.find("_catch_err_2 = _err3;") != std::string::npos);
    CHECK(cpp.find("break;") != std::string::npos);
    CHECK(cpp.find("} while(false);") != std::string::npos);
    CHECK(cpp.find("CbError e = _catch_err_2;") != std::string::npos);
    CHECK(cpp.find("auto records = _val3;") != std::string::npos);

    SECTION("Deduplicates canonical TypeNodes in AstArena and emits from const AstArena&") {
        const std::string immutCode = R"(
public type Box
    val as int
end type

fn compute() as int
    dim a = 10
    dim b = 20
    dim c = 30
    dim s1 = "hello"
    dim s2 = "world"
    dim b1 = Box(a)
    dim b2 = Box(b)
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
        CHECK(parseIm.arena.findType("int") == anaIm.symbols.typeInt);
        CHECK(parseIm.arena.findType("str") == anaIm.symbols.typeStr);

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

TEST_CASE("Module scoping, public visibility, duplicate aliases, diamond deduplication, and circular imports", "[modules]") {
    const std::string baseDir = "build/testArtifacts/moduleEdgeCases";
    std::error_code ec;
    std::filesystem::remove_all(baseDir, ec);
    std::filesystem::create_directories(baseDir + "/sub", ec);
    REQUIRE(!ec);

    SECTION("Rejects circular imports with clear diagnostic") {
        {
            std::ofstream fa(baseDir + "/a.cb");
            fa << R"(import "sub/b.cb"
public fn fa() as int
    return 1
end fn
)";
        }
        {
            std::ofstream fb(baseDir + "/sub/b.cb");
            fb << R"(import "../a.cb"
public fn fb() as int
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
        const auto res = cb::buildFile("tests/cases/cross_module_ufcs/main.cb", baseDir + "/diamondOut.cpp", false);
        REQUIRE(res.ok);
        const std::string cpp = cb::testE2E::readFileText(baseDir + "/diamondOut.cpp");
        const size_t firstPos = cpp.find("struct cb_ast__SharedCfg {");
        REQUIRE(firstPos != std::string::npos);
        CHECK(cpp.find("struct cb_ast__SharedCfg {", firstPos + 1) == std::string::npos);
    }

    SECTION("Rejects duplicate module import aliases in the same file") {
        {
            std::ofstream f1(baseDir + "/mod.cb");
            f1 << R"(public fn f() as int
    return 1
end fn
)";
        }
        {
            std::ofstream f2(baseDir + "/sub/mod.cb");
            f2 << R"(public fn f() as int
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

    SECTION("Enforces private by default (non-public functions and types are rejected across modules)") {
        {
            std::ofstream flib(baseDir + "/secret.cb");
            flib << R"(type HiddenType
    v as int
end type

fn hiddenFn() as int
    return 42
end fn

public fn publicFn() as int
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
    dim h = secret.HiddenType(1)
end fn
)";
        }
        const auto resType = cb::buildFile(baseDir + "/constructPriv.cb", baseDir + "/privType.cpp", false);
        REQUIRE_FALSE(resType.ok);
        CHECK(resType.error.message.find("is private to module 'secret'") != std::string::npos);
    }
}

TEST_CASE("End-to-end fixture: cross-module UFCS, module constants, and diamond imports", "[e2e]") {
    cb::testE2E::runE2EFixture("crossModuleUfcs", "tests/cases/cross_module_ufcs/main.cb");
}

TEST_CASE("End-to-end fixture: call-graph tree-shaking across modules and stdlib", "[e2e]") {
    cb::testE2E::runE2EFixture("treeShaking", "tests/cases/tree_shaking/main.cb");
}

TEST_CASE("End-to-end fixture: network error bubbling", "[e2e]") {
    cb::testE2E::runE2EFixture("networkErrorBubblingDefer", "tests/cases/network_error_bubbling_defer.cb");
}

TEST_CASE("End-to-end fixture: STL containers and select case", "[e2e]") {
    cb::testE2E::runE2EFixture("containersAndSelectCase", "tests/cases/containers_and_select_case.cb");
}

TEST_CASE("End-to-end fixture: struct initialization and field access", "[e2e]") {
    cb::testE2E::runE2EFixture("structAndFieldAccess", "tests/cases/struct_and_field_access.cb");
}

TEST_CASE("End-to-end fixture: for range loop", "[e2e]") {
    cb::testE2E::runE2EFixture("forRangeStep", "tests/cases/for_range_step.cb");
}

TEST_CASE("End-to-end fixture: fibonacci sequence", "[e2e]") {
    cb::testE2E::runE2EFixture("fibonacci", "tests/cases/fibonacci.cb");
}

TEST_CASE("End-to-end fixture: enums and bytes", "[e2e]") {
    cb::testE2E::runE2EFixture("enumsAndBytes", "tests/cases/enums_and_bytes.cb");
}

TEST_CASE("End-to-end fixture: defaults, zero-copy containers, and prelude", "[e2e]") {
    cb::testE2E::runE2EFixture("defaultsAndErgonomics", "tests/cases/defaults_and_ergonomics.cb");
}

TEST_CASE("End-to-end fixture: tagged unions and containers", "[e2e]") {
    cb::testE2E::runE2EFixture("unionsAndBitwise", "tests/cases/unions_and_bitwise.cb");
}

TEST_CASE("End-to-end fixture: unused std/str prelude dead-code elimination", "[e2e]") {
    cb::testE2E::runE2EFixture("preludeDeadCode", "tests/cases/prelude_dead_code.cb");
}

TEST_CASE("End-to-end fixture: self-hosting compiler constructs", "[e2e]") {
    cb::testE2E::runE2EFixture("selfhostConstructs", "tests/cases/selfhost_constructs.cb");
}

TEST_CASE("End-to-end fixture: regression fixes", "[e2e]") {
    cb::testE2E::runE2EFixture("regressionFixes", "tests/cases/regression_fixes.cb");
}

TEST_CASE("End-to-end fixture: std/str standard library", "[e2e]") {
    cb::testE2E::runE2EFixture("stdlibStr", "tests/cases/stdlib_str.cb");
}

TEST_CASE("End-to-end fixture: std/fs standard library", "[e2e]") {
    cb::testE2E::runE2EFixture("stdlibFs", "tests/cases/stdlib_fs.cb");
}

TEST_CASE("End-to-end fixture: std/os standard library", "[e2e]") {
    cb::testE2E::runE2EFixture("stdlibOs", "tests/cases/stdlib_os.cb");
}

TEST_CASE("End-to-end fixture: std/json standard library", "[e2e]") {
    cb::testE2E::runE2EFixture("stdlibJson", "tests/cases/stdlib_json.cb");
}

TEST_CASE("End-to-end fixture: std/cli standard library", "[e2e]") {
    cb::testE2E::runE2EFixture("stdlibCli", "tests/cases/stdlib_cli.cb");
}

TEST_CASE("End-to-end fixture: std/test standard library", "[e2e]") {
    cb::testE2E::runE2EFixture("stdlibTest", "tests/cases/stdlib_test.cb");
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
    outCb << R"(fn main()
    dim x = 42
    print("Answer: " & x.toStr())
end fn
)";
    outCb.close();

    const auto buildRes = cb::buildFile(testCbPath, testOutPath, false);
    REQUIRE(buildRes.ok);

    const std::string content = cb::testE2E::readFileText(testOutPath);
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

TEST_CASE("LSP URI and file path conversion helpers", "[lsp][uri]") {
    CHECK(cb::uriToFilePath("file:///workspace/project/main.cb") == "/workspace/project/main.cb");
    CHECK(cb::uriToFilePath("file://localhost/workspace/main.cb") == "/workspace/main.cb");
    CHECK(cb::uriToFilePath("file:///workspace/my%20folder/main.cb") == "/workspace/my folder/main.cb");
    CHECK(cb::uriToFilePath("file:///C:/Users/dev/main.cb") == "C:/Users/dev/main.cb");
    CHECK(cb::filePathToUri("/workspace/project/main.cb") == "file:///workspace/project/main.cb");
    CHECK(cb::filePathToUri("relative/main.cb") == "file:///relative/main.cb");
}

TEST_CASE("CleanBasic Language Server Phase 1: Core Loop, In-Memory Overlay & Live Diagnostics", "[lsp]") {

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
            cb::jsonObjectSet(arena, td, "text", cb::jsonAddString(arena, R"(public fn doubleVal(x as int) as int
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

        // Modify mathUtil.cb in memory to remove 'public' from doubleVal, then re-validate main.cb
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

TEST_CASE("Implicit multiline continuation inside (), [], and {} preserves diagnostic coordinates", "[lexer][parser][multiline]") {
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

namespace cb::testLsp {

inline void sendDidOpen(std::ostream& out, std::string_view uri, std::string_view text, int64_t version = 1) {
    cb::JsonArena arena;
    const cb::JsonId msg = cb::jsonAddObject(arena);
    cb::jsonObjectSet(arena, msg, "jsonrpc", cb::jsonAddString(arena, "2.0"));
    cb::jsonObjectSet(arena, msg, "method", cb::jsonAddString(arena, "textDocument/didOpen"));
    const cb::JsonId params = cb::jsonAddObject(arena);
    const cb::JsonId td = cb::jsonAddObject(arena);
    cb::jsonObjectSet(arena, td, "uri", cb::jsonAddString(arena, uri));
    cb::jsonObjectSet(arena, td, "languageId", cb::jsonAddString(arena, "cleanbasic"));
    cb::jsonObjectSet(arena, td, "version", cb::jsonAddInt(arena, version));
    cb::jsonObjectSet(arena, td, "text", cb::jsonAddString(arena, text));
    cb::jsonObjectSet(arena, params, "textDocument", td);
    cb::jsonObjectSet(arena, msg, "params", params);
    cb::writeJsonrpcMessage(out, cb::serializeJson(arena, msg));
}

inline void sendDidChange(std::ostream& out, std::string_view uri, int64_t version, std::string_view text) {
    cb::JsonArena arena;
    const cb::JsonId msg = cb::jsonAddObject(arena);
    cb::jsonObjectSet(arena, msg, "jsonrpc", cb::jsonAddString(arena, "2.0"));
    cb::jsonObjectSet(arena, msg, "method", cb::jsonAddString(arena, "textDocument/didChange"));
    const cb::JsonId params = cb::jsonAddObject(arena);
    const cb::JsonId td = cb::jsonAddObject(arena);
    cb::jsonObjectSet(arena, td, "uri", cb::jsonAddString(arena, uri));
    cb::jsonObjectSet(arena, td, "version", cb::jsonAddInt(arena, version));
    cb::jsonObjectSet(arena, params, "textDocument", td);
    const cb::JsonId changes = cb::jsonAddArray(arena);
    const cb::JsonId changeItem = cb::jsonAddObject(arena);
    cb::jsonObjectSet(arena, changeItem, "text", cb::jsonAddString(arena, text));
    cb::jsonArrayPush(arena, changes, changeItem);
    cb::jsonObjectSet(arena, params, "contentChanges", changes);
    cb::jsonObjectSet(arena, msg, "params", params);
    cb::writeJsonrpcMessage(out, cb::serializeJson(arena, msg));
}

inline cb::JsonParseResult readResponseForId(std::istream& in, int64_t targetId) {
    while (true) {
        const auto rawOpt = cb::readJsonrpcMessage(in);
        REQUIRE(rawOpt.has_value());
        auto parsed = cb::parseJson(rawOpt.value());
        REQUIRE(parsed.ok);
        const auto idOpt = cb::jsonGetInt(parsed.arena, cb::jsonGetField(parsed.arena, parsed.root, "id"));
        if (idOpt.has_value() && idOpt.value() == targetId) {
            return parsed;
        }
    }
}

inline bool hasCompletionItem(
    const cb::JsonArena& arena,
    cb::JsonId arrId,
    std::string_view label,
    int64_t expectedKind = -1,
    std::string_view detailSubstr = ""
) {
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
}

}  // namespace cb::testLsp

TEST_CASE("LSP DocumentSymbol Provider outlines types, fields, constants, and functions", "[lsp][documentSymbol]") {
    std::stringstream clientToServer;
    std::stringstream serverToClient;

    cb::writeJsonrpcMessage(
        clientToServer,
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":"file:///ws"}})"
    );

    cb::testLsp::sendDidOpen(clientToServer, "file:///ws/player.cb", R"(public const maxScore = 100
public type Player
    name as str
    health as int
end type
public fn heal(byref p as Player, amount as int) as int can error
    p.health = p.health + amount
    return p.health
end fn
)");

    cb::writeJsonrpcMessage(
        clientToServer,
        R"({"jsonrpc":"2.0","id":2,"method":"textDocument/documentSymbol","params":{"textDocument":{"uri":"file:///ws/player.cb"}}})"
    );
    cb::writeJsonrpcMessage(clientToServer, R"({"jsonrpc":"2.0","id":99,"method":"shutdown","params":null})");
    cb::writeJsonrpcMessage(clientToServer, R"({"jsonrpc":"2.0","method":"exit"})");

    CHECK(cb::runLspServer(clientToServer, serverToClient) == 0);

    const auto initResp = cb::testLsp::readResponseForId(serverToClient, 1);
    const cb::JsonId caps = cb::jsonGetField(
        initResp.arena,
        cb::jsonGetField(initResp.arena, initResp.root, "result"),
        "capabilities"
    );
    CHECK(cb::jsonGetBool(initResp.arena, cb::jsonGetField(initResp.arena, caps, "documentSymbolProvider")) == true);

    const auto docSymResp = cb::testLsp::readResponseForId(serverToClient, 2);
    const cb::JsonId symArrId = cb::jsonGetField(docSymResp.arena, docSymResp.root, "result");
    REQUIRE(std::holds_alternative<cb::JsonArray>(docSymResp.arena.nodes[symArrId]));
    const auto& symElems = std::get<cb::JsonArray>(docSymResp.arena.nodes[symArrId]).elements;
    REQUIRE(symElems.size() == 3);

    CHECK(cb::jsonGetString(docSymResp.arena, cb::jsonGetField(docSymResp.arena, symElems[0], "name")) == "maxScore");
    CHECK(cb::jsonGetInt(docSymResp.arena, cb::jsonGetField(docSymResp.arena, symElems[0], "kind")) == 14);
    CHECK(cb::jsonGetString(docSymResp.arena, cb::jsonGetField(docSymResp.arena, symElems[0], "detail")) == "as int");

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

    CHECK(cb::jsonGetString(docSymResp.arena, cb::jsonGetField(docSymResp.arena, symElems[2], "name")) == "heal");
    CHECK(cb::jsonGetInt(docSymResp.arena, cb::jsonGetField(docSymResp.arena, symElems[2], "kind")) == 12);
    CHECK(cb::jsonGetString(docSymResp.arena, cb::jsonGetField(docSymResp.arena, symElems[2], "detail")) == "(byref p as Player, amount as int) as int can error");
}

TEST_CASE("LSP Hover Provider resolves variables, UFCS methods, struct fields, and builtins", "[lsp][hover]") {
    std::stringstream clientToServer;
    std::stringstream serverToClient;

    cb::writeJsonrpcMessage(
        clientToServer,
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":"file:///ws"}})"
    );

    cb::testLsp::sendDidOpen(clientToServer, "file:///ws/player.cb", R"(public const maxScore = 100
public type Player
    name as str
    health as int
end type
public fn heal(byref p as Player, amount as int) as int can error
    p.health = p.health + amount
    return p.health
end fn
)");

    cb::testLsp::sendDidOpen(clientToServer, "file:///ws/main.cb", R"(import "player.cb"
fn localBoost(base as int) as int
    return base + 10
end fn
fn main() as int can error
    const maxPotion = 50
    dim hero = player.Player("Alice", 100)
    dim boosted = localBoost(maxPotion)
    dim newHp = hero.heal(boosted)
    print(hero.health, newHp)
    return 0
end fn
)");

    // Hover on inferred local variable 'hero' (0-based line 6, char 9) (id = 3)
    cb::writeJsonrpcMessage(
        clientToServer,
        R"({"jsonrpc":"2.0","id":3,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///ws/main.cb"},"position":{"line":6,"character":9}}})"
    );
    // Hover on UFCS call 'hero.heal' (0-based line 8, char 23 -> 'heal') (id = 4)
    cb::writeJsonrpcMessage(
        clientToServer,
        R"({"jsonrpc":"2.0","id":4,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///ws/main.cb"},"position":{"line":8,"character":23}}})"
    );
    // Hover on struct field 'hero.health' (0-based line 9, char 16 -> 'health') (id = 5)
    cb::writeJsonrpcMessage(
        clientToServer,
        R"({"jsonrpc":"2.0","id":5,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///ws/main.cb"},"position":{"line":9,"character":16}}})"
    );
    // Hover on builtin 'print' (0-based line 9, char 5) (id = 6)
    cb::writeJsonrpcMessage(
        clientToServer,
        R"({"jsonrpc":"2.0","id":6,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///ws/main.cb"},"position":{"line":9,"character":5}}})"
    );
    cb::writeJsonrpcMessage(clientToServer, R"({"jsonrpc":"2.0","id":99,"method":"shutdown","params":null})");
    cb::writeJsonrpcMessage(clientToServer, R"({"jsonrpc":"2.0","method":"exit"})");

    CHECK(cb::runLspServer(clientToServer, serverToClient) == 0);

    const auto initResp = cb::testLsp::readResponseForId(serverToClient, 1);
    const cb::JsonId caps = cb::jsonGetField(
        initResp.arena,
        cb::jsonGetField(initResp.arena, initResp.root, "result"),
        "capabilities"
    );
    CHECK(cb::jsonGetBool(initResp.arena, cb::jsonGetField(initResp.arena, caps, "hoverProvider")) == true);

    auto getHoverValue = [&](int64_t reqId) -> std::string {
        const auto resp = cb::testLsp::readResponseForId(serverToClient, reqId);
        const cb::JsonId contents = cb::jsonGetField(
            resp.arena,
            cb::jsonGetField(resp.arena, resp.root, "result"),
            "contents"
        );
        const auto val = cb::jsonGetString(resp.arena, cb::jsonGetField(resp.arena, contents, "value"));
        REQUIRE(val.has_value());
        return val.value();
    };

    CHECK(getHoverValue(3).find("dim hero as Player") != std::string::npos);
    CHECK(getHoverValue(4).find("public fn heal(byref p as Player, amount as int) as int can error") != std::string::npos);
    CHECK(getHoverValue(5).find("(field) Player.health as int") != std::string::npos);
    CHECK(getHoverValue(6).find("fn print(values...)") != std::string::npos);

    const auto lenHover = cb::formatBuiltinHoverMarkdown("len");
    REQUIRE(lenHover.has_value());
    CHECK(lenHover->find("list, dict, or str") != std::string::npos);
    const auto toIntHover = cb::formatBuiltinHoverMarkdown("toInt");
    REQUIRE(toIntHover.has_value());
    CHECK(toIntHover->find("numeric, boolean, or enum value") != std::string::npos);
}

TEST_CASE("LSP Definition Provider resolves same-file, cross-module UFCS, struct field, and local variable targets", "[lsp][definition]") {
    std::stringstream clientToServer;
    std::stringstream serverToClient;

    cb::writeJsonrpcMessage(
        clientToServer,
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":"file:///ws"}})"
    );

    cb::testLsp::sendDidOpen(clientToServer, "file:///ws/player.cb", R"(public const maxScore = 100
public type Player
    name as str
    health as int
end type
public fn heal(byref p as Player, amount as int) as int can error
    p.health = p.health + amount
    return p.health
end fn
)");

    cb::testLsp::sendDidOpen(clientToServer, "file:///ws/main.cb", R"(import "player.cb"
fn localBoost(base as int) as int
    return base + 10
end fn
fn main() as int can error
    const maxPotion = 50
    dim hero = player.Player("Alice", 100)
    dim boosted = localBoost(maxPotion)
    dim newHp = hero.heal(boosted)
    print(hero.health, newHp)
    return 0
end fn
)");

    // Definition on same-file function call 'localBoost' (0-based line 7, char 20) (id = 7)
    cb::writeJsonrpcMessage(
        clientToServer,
        R"({"jsonrpc":"2.0","id":7,"method":"textDocument/definition","params":{"textDocument":{"uri":"file:///ws/main.cb"},"position":{"line":7,"character":20}}})"
    );
    // Definition on cross-module UFCS call 'hero.heal' (0-based line 8, char 23) (id = 8)
    cb::writeJsonrpcMessage(
        clientToServer,
        R"({"jsonrpc":"2.0","id":8,"method":"textDocument/definition","params":{"textDocument":{"uri":"file:///ws/main.cb"},"position":{"line":8,"character":23}}})"
    );
    // Definition on struct field 'hero.health' (0-based line 9, char 16) (id = 9)
    cb::writeJsonrpcMessage(
        clientToServer,
        R"({"jsonrpc":"2.0","id":9,"method":"textDocument/definition","params":{"textDocument":{"uri":"file:///ws/main.cb"},"position":{"line":9,"character":16}}})"
    );
    // Definition on local const 'maxPotion' usage (0-based line 7, char 32) (id = 10)
    cb::writeJsonrpcMessage(
        clientToServer,
        R"({"jsonrpc":"2.0","id":10,"method":"textDocument/definition","params":{"textDocument":{"uri":"file:///ws/main.cb"},"position":{"line":7,"character":32}}})"
    );
    cb::writeJsonrpcMessage(clientToServer, R"({"jsonrpc":"2.0","id":99,"method":"shutdown","params":null})");
    cb::writeJsonrpcMessage(clientToServer, R"({"jsonrpc":"2.0","method":"exit"})");

    CHECK(cb::runLspServer(clientToServer, serverToClient) == 0);

    const auto initResp = cb::testLsp::readResponseForId(serverToClient, 1);
    const cb::JsonId caps = cb::jsonGetField(
        initResp.arena,
        cb::jsonGetField(initResp.arena, initResp.root, "result"),
        "capabilities"
    );
    CHECK(cb::jsonGetBool(initResp.arena, cb::jsonGetField(initResp.arena, caps, "definitionProvider")) == true);

    auto checkDefLocation = [&](int64_t reqId, std::string_view expectedUri, int64_t expectedLine, int64_t expectedChar) {
        const auto resp = cb::testLsp::readResponseForId(serverToClient, reqId);
        const cb::JsonId res = cb::jsonGetField(resp.arena, resp.root, "result");
        CHECK(cb::jsonGetString(resp.arena, cb::jsonGetField(resp.arena, res, "uri")) == expectedUri);
        const cb::JsonId start = cb::jsonGetField(resp.arena, cb::jsonGetField(resp.arena, res, "range"), "start");
        CHECK(cb::jsonGetInt(resp.arena, cb::jsonGetField(resp.arena, start, "line")) == expectedLine);
        CHECK(cb::jsonGetInt(resp.arena, cb::jsonGetField(resp.arena, start, "character")) == expectedChar);
    };

    checkDefLocation(7, "file:///ws/main.cb", 1, 3);
    checkDefLocation(8, "file:///ws/player.cb", 5, 10);
    checkDefLocation(9, "file:///ws/player.cb", 3, 4);
    checkDefLocation(10, "file:///ws/main.cb", 5, 10);
}

TEST_CASE("LSP Snapshot Fallback preserves symbols and hover during transient syntax errors", "[lsp][snapshot]") {
    std::stringstream clientToServer;
    std::stringstream serverToClient;

    cb::writeJsonrpcMessage(
        clientToServer,
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":"file:///ws"}})"
    );

    cb::testLsp::sendDidOpen(clientToServer, "file:///ws/player.cb", R"(public type Player
    name as str
    health as int
end type
)");

    cb::testLsp::sendDidOpen(clientToServer, "file:///ws/main.cb", R"(import "player.cb"
fn localBoost(base as int) as int
    return base + 10
end fn
fn main() as int can error
    const maxPotion = 50
    dim hero = player.Player("Alice", 100)
    return 0
end fn
)");

    // Introduce transient syntax error in main.cb via didChange, then request documentSymbol (id = 11) and hover on 'hero' (id = 12)
    cb::testLsp::sendDidChange(clientToServer, "file:///ws/main.cb", 2, R"(import "player.cb"
fn localBoost(base as int) as int
    return base + 10
end fn
fn main() as int can error
    const maxPotion = 50
    dim hero = player.Player("Alice", 100)
    if
)");

    cb::writeJsonrpcMessage(
        clientToServer,
        R"({"jsonrpc":"2.0","id":11,"method":"textDocument/documentSymbol","params":{"textDocument":{"uri":"file:///ws/main.cb"}}})"
    );
    cb::writeJsonrpcMessage(
        clientToServer,
        R"({"jsonrpc":"2.0","id":12,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///ws/main.cb"},"position":{"line":6,"character":9}}})"
    );
    cb::writeJsonrpcMessage(clientToServer, R"({"jsonrpc":"2.0","id":99,"method":"shutdown","params":null})");
    cb::writeJsonrpcMessage(clientToServer, R"({"jsonrpc":"2.0","method":"exit"})");

    CHECK(cb::runLspServer(clientToServer, serverToClient) == 0);

    const auto symFallback = cb::testLsp::readResponseForId(serverToClient, 11);
    const cb::JsonId fbSymArr = cb::jsonGetField(symFallback.arena, symFallback.root, "result");
    REQUIRE(std::holds_alternative<cb::JsonArray>(symFallback.arena.nodes[fbSymArr]));
    CHECK(std::get<cb::JsonArray>(symFallback.arena.nodes[fbSymArr]).elements.size() == 2);

    const auto hoverFallback = cb::testLsp::readResponseForId(serverToClient, 12);
    const cb::JsonId h12Contents = cb::jsonGetField(
        hoverFallback.arena,
        cb::jsonGetField(hoverFallback.arena, hoverFallback.root, "result"),
        "contents"
    );
    const auto h12Val = cb::jsonGetString(hoverFallback.arena, cb::jsonGetField(hoverFallback.arena, h12Contents, "value"));
    REQUIRE(h12Val.has_value());
    CHECK(h12Val.value().find("dim hero as Player") != std::string::npos);
}

TEST_CASE("LSP Completion Provider resolves UFCS and fields", "[lsp][completion]") {
    std::stringstream clientToServer;
    std::stringstream serverToClient;

    cb::writeJsonrpcMessage(
        clientToServer,
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":"file:///ws"}})"
    );

    cb::testLsp::sendDidOpen(clientToServer, "file:///ws/player.cb", R"(public const maxScore = 100
public type Player
    name as str
    health as int
end type
public fn heal(byref p as Player, amount as int, bonus as int) as int can error
    p.health = p.health + amount + bonus
    return p.health
end fn
fn secretHelper(p as Player) as int
    return p.health
end fn
)");

    // Open main.cb directly with a brand-new variable 'hero' and an incomplete trailing dot 'hero.' on the very next line (two-stage scrub recovery)
    cb::testLsp::sendDidOpen(clientToServer, "file:///ws/main.cb", R"(import "player.cb"
fn localBuff(byref p as player.Player, shield as int)
    p.health = p.health + shield
end fn
fn main() as int
    try
    catch e
    end try
    dim nums as list[int] = [1, 2]
    dim hero = player.Player("Alice", 100)
    hero.
    return 0
end fn
)");

    // Req id=2: textDocument/completion at 'hero.' (line 10, character 9)
    cb::writeJsonrpcMessage(
        clientToServer,
        R"({"jsonrpc":"2.0","id":2,"method":"textDocument/completion","params":{"textDocument":{"uri":"file:///ws/main.cb"},"position":{"line":10,"character":9}}})"
    );

    auto sendMainLine10Change = [&](int version, std::string_view newLine10) {
        std::string fullSrc = R"(import "player.cb"
fn localBuff(byref p as player.Player, shield as int)
    p.health = p.health + shield
end fn
fn main() as int
    try
    catch e
    end try
    dim nums as list[int] = [1, 2]
    dim hero = player.Player("Alice", 100)
)";
        fullSrc += std::string(newLine10) + R"(
    return 0
end fn
)";
        cb::testLsp::sendDidChange(clientToServer, "file:///ws/main.cb", version, fullSrc);
    };

    // Change v2: '    player.' (line 10, char 11) -> Req id=3 (module alias completion)
    sendMainLine10Change(2, "    player.");
    cb::writeJsonrpcMessage(
        clientToServer,
        R"({"jsonrpc":"2.0","id":3,"method":"textDocument/completion","params":{"textDocument":{"uri":"file:///ws/main.cb"},"position":{"line":10,"character":11}}})"
    );

    // Change v3: '    e.' (line 10, char 6) -> Req id=4 (built-in err fields .msg and .code)
    sendMainLine10Change(3, "    e.");
    cb::writeJsonrpcMessage(
        clientToServer,
        R"({"jsonrpc":"2.0","id":4,"method":"textDocument/completion","params":{"textDocument":{"uri":"file:///ws/main.cb"},"position":{"line":10,"character":6}}})"
    );

    // Change v4: '    nums.' (line 10, char 9) -> Req id=5 (built-in list[int] methods .push and .len)
    sendMainLine10Change(4, "    nums.");
    cb::writeJsonrpcMessage(
        clientToServer,
        R"({"jsonrpc":"2.0","id":5,"method":"textDocument/completion","params":{"textDocument":{"uri":"file:///ws/main.cb"},"position":{"line":10,"character":9}}})"
    );

    cb::writeJsonrpcMessage(clientToServer, R"({"jsonrpc":"2.0","id":99,"method":"shutdown","params":null})");
    cb::writeJsonrpcMessage(clientToServer, R"({"jsonrpc":"2.0","method":"exit"})");

    CHECK(cb::runLspServer(clientToServer, serverToClient) == 0);

    const auto initResp = cb::testLsp::readResponseForId(serverToClient, 1);
    const cb::JsonId initRes = cb::jsonGetField(initResp.arena, initResp.root, "result");
    const cb::JsonId caps = cb::jsonGetField(initResp.arena, initRes, "capabilities");
    CHECK(cb::jsonGetField(initResp.arena, caps, "completionProvider") != cb::JSON_INVALID_ID);
    const cb::JsonId srvInfo = cb::jsonGetField(initResp.arena, initRes, "serverInfo");
    CHECK(cb::jsonGetString(initResp.arena, cb::jsonGetField(initResp.arena, srvInfo, "version")) == "0.3.0");

    const auto compHero = cb::testLsp::readResponseForId(serverToClient, 2);
    const cb::JsonId heroItems = cb::jsonGetField(compHero.arena, compHero.root, "result");
    CHECK(cb::testLsp::hasCompletionItem(compHero.arena, heroItems, "name", 5, "as str"));
    CHECK(cb::testLsp::hasCompletionItem(compHero.arena, heroItems, "health", 5, "as int"));
    CHECK(cb::testLsp::hasCompletionItem(compHero.arena, heroItems, "heal", 2, "ufcs fn heal(amount as int, bonus as int) as int can error"));
    CHECK(cb::testLsp::hasCompletionItem(compHero.arena, heroItems, "localBuff", 2, "ufcs fn localBuff(shield as int)"));
    CHECK_FALSE(cb::testLsp::hasCompletionItem(compHero.arena, heroItems, "secretHelper"));

    const auto compMod = cb::testLsp::readResponseForId(serverToClient, 3);
    const cb::JsonId modItems = cb::jsonGetField(compMod.arena, compMod.root, "result");
    CHECK(cb::testLsp::hasCompletionItem(compMod.arena, modItems, "Player", 22, "type Player"));
    CHECK(cb::testLsp::hasCompletionItem(compMod.arena, modItems, "maxScore", 21, "const as int"));
    CHECK(cb::testLsp::hasCompletionItem(compMod.arena, modItems, "heal", 3));
    CHECK_FALSE(cb::testLsp::hasCompletionItem(compMod.arena, modItems, "secretHelper"));

    const auto compErr = cb::testLsp::readResponseForId(serverToClient, 4);
    const cb::JsonId errItems = cb::jsonGetField(compErr.arena, compErr.root, "result");
    CHECK(cb::testLsp::hasCompletionItem(compErr.arena, errItems, "msg", 5, "as str"));
    CHECK(cb::testLsp::hasCompletionItem(compErr.arena, errItems, "code", 5, "as int"));

    const auto compList = cb::testLsp::readResponseForId(serverToClient, 5);
    const cb::JsonId listItems = cb::jsonGetField(compList.arena, compList.root, "result");
    CHECK(cb::testLsp::hasCompletionItem(compList.arena, listItems, "push", 2, "fn(item as int)"));
    CHECK(cb::testLsp::hasCompletionItem(compList.arena, listItems, "len", 2, "fn() as int"));
    CHECK_FALSE(cb::testLsp::hasCompletionItem(compList.arena, listItems, "append"));
}

TEST_CASE("LSP Completion Provider resolves type annotations and lexical scope", "[lsp][completion]") {
    std::stringstream clientToServer;
    std::stringstream serverToClient;

    cb::writeJsonrpcMessage(
        clientToServer,
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":"file:///ws"}})"
    );

    cb::testLsp::sendDidOpen(clientToServer, "file:///ws/player.cb", R"(public type Player
    name as str
    health as int
end type
)");

    cb::testLsp::sendDidOpen(clientToServer, "file:///ws/main.cb", R"(import "player.cb"
fn main() as int
    dim hero = player.Player("Alice", 100)
    dim x as 
    return 0
end fn
)");

    // Req id=9: type annotation completion after 'as ' (line 3, char 13)
    cb::writeJsonrpcMessage(
        clientToServer,
        R"({"jsonrpc":"2.0","id":9,"method":"textDocument/completion","params":{"textDocument":{"uri":"file:///ws/main.cb"},"position":{"line":3,"character":13}}})"
    );

    // Change v2: '    he' (line 3, char 6) -> Req id=10 (general scope completion filtered by prefix 'he' -> finds 'hero')
    cb::testLsp::sendDidChange(clientToServer, "file:///ws/main.cb", 2, R"(import "player.cb"
fn main() as int
    dim hero = player.Player("Alice", 100)
    he
    return 0
end fn
)");
    cb::writeJsonrpcMessage(
        clientToServer,
        R"({"jsonrpc":"2.0","id":10,"method":"textDocument/completion","params":{"textDocument":{"uri":"file:///ws/main.cb"},"position":{"line":3,"character":6}}})"
    );

    cb::writeJsonrpcMessage(clientToServer, R"({"jsonrpc":"2.0","id":99,"method":"shutdown","params":null})");
    cb::writeJsonrpcMessage(clientToServer, R"({"jsonrpc":"2.0","method":"exit"})");

    CHECK(cb::runLspServer(clientToServer, serverToClient) == 0);

    const auto compTypes = cb::testLsp::readResponseForId(serverToClient, 9);
    const cb::JsonId typeItems = cb::jsonGetField(compTypes.arena, compTypes.root, "result");
    CHECK(cb::testLsp::hasCompletionItem(compTypes.arena, typeItems, "int", 14));
    CHECK(cb::testLsp::hasCompletionItem(compTypes.arena, typeItems, "str", 14));
    CHECK(cb::testLsp::hasCompletionItem(compTypes.arena, typeItems, "list", 14));
    CHECK(cb::testLsp::hasCompletionItem(compTypes.arena, typeItems, "player", 9));

    const auto compScope = cb::testLsp::readResponseForId(serverToClient, 10);
    const cb::JsonId scopeItems = cb::jsonGetField(compScope.arena, compScope.root, "result");
    CHECK(cb::testLsp::hasCompletionItem(compScope.arena, scopeItems, "hero", 6, "as Player"));
}

TEST_CASE("LSP Signature Help Provider resolves struct constructors and UFCS active parameters", "[lsp][signatureHelp]") {
    std::stringstream clientToServer;
    std::stringstream serverToClient;

    cb::writeJsonrpcMessage(
        clientToServer,
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":"file:///ws"}})"
    );

    cb::testLsp::sendDidOpen(clientToServer, "file:///ws/player.cb", R"(public type Player
    name as str
    health as int
end type
public fn heal(byref p as Player, amount as int, bonus as int) as int can error
    p.health = p.health + amount + bonus
    return p.health
end fn
)");

    cb::testLsp::sendDidOpen(clientToServer, "file:///ws/main.cb", R"(import "player.cb"
fn main() as int
    dim hero = player.Player("Alice", 100)
    dim p2 = player.Player(
    return 0
end fn
)");

    // Req id=7: signatureHelp inside 'player.Player(' (line 3, char 27)
    cb::writeJsonrpcMessage(
        clientToServer,
        R"({"jsonrpc":"2.0","id":7,"method":"textDocument/signatureHelp","params":{"textDocument":{"uri":"file:///ws/main.cb"},"position":{"line":3,"character":27}}})"
    );

    // Change v2: '    hero.heal(25, ' (line 3, char 18) -> Req id=8 (UFCS signatureHelp at 2nd visible parameter: activeParameter == 1)
    cb::testLsp::sendDidChange(clientToServer, "file:///ws/main.cb", 2, R"(import "player.cb"
fn main() as int
    dim hero = player.Player("Alice", 100)
    hero.heal(25, 
    return 0
end fn
)");
    cb::writeJsonrpcMessage(
        clientToServer,
        R"({"jsonrpc":"2.0","id":8,"method":"textDocument/signatureHelp","params":{"textDocument":{"uri":"file:///ws/main.cb"},"position":{"line":3,"character":18}}})"
    );
    cb::writeJsonrpcMessage(clientToServer, R"({"jsonrpc":"2.0","id":99,"method":"shutdown","params":null})");
    cb::writeJsonrpcMessage(clientToServer, R"({"jsonrpc":"2.0","method":"exit"})");
    CHECK(cb::runLspServer(clientToServer, serverToClient) == 0);

    const auto initResp = cb::testLsp::readResponseForId(serverToClient, 1);
    const cb::JsonId caps = cb::jsonGetField(
        initResp.arena,
        cb::jsonGetField(initResp.arena, initResp.root, "result"),
        "capabilities"
    );
    CHECK(cb::jsonGetField(initResp.arena, caps, "signatureHelpProvider") != cb::JSON_INVALID_ID);

    const auto sigCtor = cb::testLsp::readResponseForId(serverToClient, 7);
    const cb::JsonId sigCtorRes = cb::jsonGetField(sigCtor.arena, sigCtor.root, "result");
    CHECK(cb::jsonGetInt(sigCtor.arena, cb::jsonGetField(sigCtor.arena, sigCtorRes, "activeParameter")) == 0);
    const cb::JsonId ctorSigs = cb::jsonGetField(sigCtor.arena, sigCtorRes, "signatures");
    REQUIRE(std::holds_alternative<cb::JsonArray>(sigCtor.arena.nodes[ctorSigs]));
    const cb::JsonId ctorSig0 = std::get<cb::JsonArray>(sigCtor.arena.nodes[ctorSigs]).elements[0];
    CHECK(cb::jsonGetString(sigCtor.arena, cb::jsonGetField(sigCtor.arena, ctorSig0, "label")) ==
          "Player(name as str, health as int) as Player");

    const auto sigUfcs = cb::testLsp::readResponseForId(serverToClient, 8);
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

    // Verify signature help for built-in receiver method (.push on list[int])
    const auto checkList = cb::checkSource(R"CB(fn main()
  dim l = [1]
  l.push(0)
end fn)CB", "main.cb");
    REQUIRE(checkList.ok);
    cb::LspDocumentSnapshot snapList{};
    snapList.valid = true;
    snapList.arena = std::move(checkList.arena);
    snapList.symbols = std::move(checkList.symbols);
    snapList.program = std::move(checkList.program);
    cb::CallSiteContext pushCtx{};
    pushCtx.insideCall = true;
    pushCtx.hasReceiver = true;
    pushCtx.receiverName = "l";
    pushCtx.calleeName = "push";
    pushCtx.activeParam = 0;
    const auto pushHelp = cb::resolveSignatureHelp(snapList, "", std::string("main"), pushCtx);
    REQUIRE(pushHelp.has_value());
    CHECK(pushHelp->label.find(".push(item as int)") != std::string::npos);
    REQUIRE(pushHelp->paramLabels.size() == 1);
    CHECK(pushHelp->paramLabels[0] == "item as int");
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
        CHECK(cb::jsonGetField(parsed.arena, props, "cleanbasic.formatterPath") != cb::INVALID_ID);
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
        CHECK(cb::jsonGetField(snippets.arena, snippets.root, "Try Catch Block") != cb::INVALID_ID);

        const cb::JsonParseResult launch = cb::parseJson(cb::testE2E::readFileText(".vscode/launch.json"));
        REQUIRE(launch.ok);
        const cb::JsonId configs = cb::jsonGetField(launch.arena, launch.root, "configurations");
        REQUIRE(std::holds_alternative<cb::JsonArray>(launch.arena.nodes[configs]));
        const cb::JsonId cfg0 = std::get<cb::JsonArray>(launch.arena.nodes[configs]).elements[0];
        CHECK(cb::jsonGetString(launch.arena, cb::jsonGetField(launch.arena, cfg0, "type")) == "extensionHost");

        const std::string extJs = cb::testE2E::readFileText("editors/vscode/extension.js");
        CHECK(extJs.find("LanguageClient") != std::string::npos);
        CHECK(extJs.find("cleanbasic.restartServer") != std::string::npos);
        CHECK(extJs.find("registerDocumentFormattingEditProvider") != std::string::npos);
        CHECK(extJs.find("resolveCbFmtExecutable") != std::string::npos);
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

        std::error_code dirEc;
        std::filesystem::create_directories("build/testArtifacts", dirEc);
        const std::string tmpIn = "build/testArtifacts/cbLspStdioIn.txt";
        const std::string tmpOut = "build/testArtifacts/cbLspStdioOut.txt";
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

TEST_CASE("LSP go-to-definition and hover resolve std/json symbols into stdlib/json/json.cb", "[stdlib][json][lsp]") {
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
        const std::string tmpBase = "build/testArtifacts/cbPortableSdkTest";
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
            "projects/demo/app.exe"
        );
        CHECK(cmd.find("zig c++ -target x86_64-windows-gnu -O0 -std=c++17 -pthread") != std::string::npos);
        CHECK(cmd.find(R"(-I"/opt/cleanbasic-sdk/vendor")") != std::string::npos);
        CHECK(cmd.find(R"(-I"/opt/cleanbasic-sdk/stdlib")") != std::string::npos);
        CHECK(cmd.find(R"(-I"projects/demo")") != std::string::npos);
        CHECK(cmd.find(R"("projects/demo/build.cpp" -o "projects/demo/app.exe")") != std::string::npos);

        const std::string releaseCmd = cb::buildCompileCommand(
            crossCc,
            "/opt/cleanbasic-sdk",
            "projects/demo",
            "projects/demo/build.cpp",
            "projects/demo/app.exe",
            "3"
        );
        CHECK(releaseCmd.find("zig c++ -target x86_64-windows-gnu -O3 -std=c++17 -pthread") != std::string::npos);

        const std::string pchCmd = cb::buildCompileCommand(
            "g++",
            "/opt/cleanbasic-sdk",
            "projects/demo",
            "projects/demo/build.cpp",
            "projects/demo/app",
            "0",
            "/opt/cleanbasic-sdk/build/pch/cb_pch.hpp"
        );
        CHECK(pchCmd.find(R"(-Winvalid-pch -include "/opt/cleanbasic-sdk/build/pch/cb_pch.hpp")") != std::string::npos);

        // Verify Tier 1 default hybrid static C++ runtime flags and Tier 3 --static flags across targets
        const cb::TargetSpec linuxSpec = cb::resolveTargetSpec("linux");
        CHECK(cb::resolvePortableRuntimeFlags("g++", linuxSpec, false) == "-static-libstdc++ -static-libgcc");
        CHECK(cb::resolvePortableRuntimeFlags("g++", linuxSpec, true) == "-static");
        CHECK(cb::resolvePortableRuntimeFlags("zig c++ -target x86_64-linux-musl", linuxSpec, false).empty());
        CHECK(cb::resolvePortableRuntimeFlags("zig c++ -target x86_64-linux-musl", linuxSpec, true) == "-static");

        CHECK(cb::resolvePortableRuntimeFlags("g++", winSpec, false) ==
              "-static-libstdc++ -static-libgcc -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic");
        CHECK(cb::resolvePortableRuntimeFlags("g++", winSpec, true) == "-static");

        const cb::TargetSpec macSpec = cb::resolveTargetSpec("macos");
        CHECK(cb::resolvePortableRuntimeFlags("clang++", macSpec, false).empty());
        CHECK(cb::resolvePortableRuntimeFlags("clang++", macSpec, true).empty());

        const std::string winHybridCmd = cb::buildCompileCommand(
            "g++",
            "/opt/cleanbasic-sdk",
            "projects/demo",
            "projects/demo/build.cpp",
            "projects/demo/app.exe",
            "0",
            "",
            false,
            winSpec
        );
        const size_t bdPos = winHybridCmd.find("-Wl,-Bstatic -lwinpthread -Wl,-Bdynamic");
        const size_t srcPos = winHybridCmd.find(R"("projects/demo/build.cpp")");
        CHECK(bdPos != std::string::npos);
        CHECK(srcPos != std::string::npos);
        CHECK(bdPos < srcPos);

        const std::string staticLinuxCmd = cb::buildCompileCommand(
            "g++",
            "/opt/cleanbasic-sdk",
            "projects/demo",
            "projects/demo/build.cpp",
            "projects/demo/app",
            "3",
            "",
            true,
            linuxSpec
        );
        CHECK(staticLinuxCmd.find(" -static ") != std::string::npos);

        const std::string staticMacCmd = cb::buildCompileCommand(
            "clang++",
            "/opt/cleanbasic-sdk",
            "projects/demo",
            "projects/demo/build.cpp",
            "projects/demo/app",
            "3",
            "",
            true,
            macSpec
        );
        CHECK(staticMacCmd.find("-static") == std::string::npos);
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
        CHECK_FALSE(rebuildPchArgs.staticLink);

        const cb::CliArgs staticArgs = cb::parseCliArgs({"build", "main.cb", "--static"});
        CHECK(staticArgs.ok);
        CHECK(staticArgs.compileBinary);
        CHECK(staticArgs.staticLink);

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

        // Verify PCH rebuild (--rebuild-pch) followed by PCH cache hit, --no-pch, and staticLink = true
        const std::string outCpp = "build/testArtifacts/cliOptsFib.cpp";
        const std::string outBin = "build/testArtifacts/cliOptsFibBin";
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
        INFO("Build diagnostic: " << cb::formatDiagnostic(rebuildRes.error));
        REQUIRE(rebuildRes.ok);
        CHECK(rebuildRes.outputBinaryPath == outBin);
        CHECK(rebuildRes.compileCommand.find(" -O0 ") != std::string::npos);
        CHECK(rebuildRes.compileCommand.find(R"(-I")") != std::string::npos);
        CHECK(rebuildRes.timings.transpileTotalMs > 0.0);
        CHECK(rebuildRes.timings.cppCompileMs > 0.0);
        CHECK(rebuildRes.timings.totalMs >= rebuildRes.timings.transpileTotalMs);
        CHECK(rebuildRes.timings.pchUsed);
        CHECK_FALSE(rebuildRes.timings.pchCacheHit);
        CHECK(rebuildRes.timings.pchBuildMs > 0.0);
        CHECK(rebuildRes.compileCommand.find("-Winvalid-pch -include") != std::string::npos);
        std::error_code ec;
        CHECK(std::filesystem::exists(outBin, ec));

        const std::string staticBin = "build/testArtifacts/cliOptsFibStaticBin";
        cb::BuildOptions staticOpts{};
        staticOpts.outputCppPath = outCpp;
        staticOpts.compileBinary = true;
        staticOpts.binaryOutput = staticBin;
        staticOpts.target = "native";
        staticOpts.compiler = "g++";
        staticOpts.staticLink = true;
        staticOpts.usePch = true;
        staticOpts.rebuildPch = false;
        const cb::BuildFileResult staticRes = cb::buildFileWithOptions("tests/cases/fibonacci.cb", staticOpts);
        INFO("Static build diagnostic: " << cb::formatDiagnostic(staticRes.error));
        REQUIRE(staticRes.ok);
        CHECK(staticRes.timings.pchUsed);
        CHECK(staticRes.timings.pchCacheHit);
        CHECK(staticRes.timings.pchBuildMs == 0.0);
        CHECK(std::filesystem::exists(staticBin, ec));

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

TEST_CASE("examples/cbgrep.cb and piped standard streams E2E verification", "[stdlib][fs][os][str][cbgrep][e2e]") {
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

        const int rcStdinExplicit = std::system(("./" + outBin + " -in cleanbasic - < " + sampleFile + " > " + stdoutFile + " 2>&1").c_str());
        CHECK(rcStdinExplicit == 0);
        const std::string stdinExplicitOut = cb::testE2E::readFileText(stdoutFile);
        CHECK(stdinExplicitOut.find("{stdin}:1:CleanBasic Alpha") != std::string::npos);
        CHECK(stdinExplicitOut.find("{stdin}:3:cleanbasic beta") != std::string::npos);

        const int rcStdinImplicit = std::system(("./" + outBin + " -ic cleanbasic < " + sampleFile + " > " + stdoutFile + " 2>&1").c_str());
        CHECK(rcStdinImplicit == 0);
        const std::string stdinImplicitOut = cb::testE2E::readFileText(stdoutFile);
        CHECK(stdinImplicitOut.find("{stdin}:2") != std::string::npos);

        const int rcHelp = std::system(("./" + outBin + " --help > " + stdoutFile + " 2>&1").c_str());
        CHECK(rcHelp == 0);
        const std::string helpOut = cb::testE2E::readFileText(stdoutFile);
        CHECK(helpOut.find("Usage: cbgrep [options] <pattern>") != std::string::npos);
        CHECK(helpOut.find("-i, --ignore-case") != std::string::npos);

        const int rcVersion = std::system(("./" + outBin + " --version > " + stdoutFile + " 2>&1").c_str());
        CHECK(rcVersion == 0);
        const std::string versionOut = cb::testE2E::readFileText(stdoutFile);
        CHECK(versionOut.find("cbgrep 1.0") != std::string::npos);
    }
}





TEST_CASE("Analyzer enforces non-byref parameter and const mutability rules", "[analyzer]") {
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
TEST_CASE("Analyzer validates loop depth for break/continue and select case target/case types", "[control_flow][select_case][continue]") {
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

TEST_CASE("Fallible UFCS scope forwarding, enum aliases, and LSP enum parity", "[analyzer][emitter][lsp]") {
    SECTION("Rejects unhandled fallible UFCS receiver calls") {
        const std::string badUfcs = R"CB(
fn decodeUnhandled()
    dim raw = "42"
    dim n = raw.toInt()
    print(n)
end fn
)CB";
        const auto badRes = cb::transpileSource(badUfcs, "badUfcs.cb");
        CHECK_FALSE(badRes.ok);
        CHECK(badRes.error.kind == "AnalyzerError");
        CHECK(badRes.error.message.find("Unhandled error") != std::string::npos);
    }

    SECTION("LSP supports enum symbols/hover/completions") {
        cb::LspServerState state{};
        const std::string uri = "file:///ws/enum_lsp.cb";
        const std::string text = R"CB(public enum Color
    red
    blue
end enum
fn demo()
    dim c = Color.red
end fn
)CB";
        state.openDocuments[uri] = text;
        std::ostringstream dummyOut;
        cb::publishDocumentDiagnostics(state, uri, text, dummyOut);
        const cb::LspDocumentSnapshot snap = cb::getOrBuildSnapshot(state, uri);
        REQUIRE(snap.valid);

        const cb::CursorTokenContext enumMemberCtx = cb::resolveCursorToken(text, "/ws/enum_lsp.cb", 6, 20);
        REQUIRE(enumMemberCtx.found);
        CHECK(enumMemberCtx.hasDotReceiver);
        CHECK(enumMemberCtx.receiverName == "Color");
        const cb::ResolvedCursorSymbol hoverInfo = cb::resolveCursorSymbolInfo(snap, "/ws/enum_lsp.cb", enumMemberCtx);
        REQUIRE(hoverInfo.hoverMarkdown.has_value());
        CHECK(hoverInfo.hoverMarkdown.value().find("(enum member) Color.red") != std::string::npos);

        std::vector<cb::CompletionItemEntry> dotEnumItems;
        std::unordered_map<std::string, bool> seenEnum;
        cb::collectDotCompletions(snap, "", std::string("demo"), "Color", "", dotEnumItems, seenEnum);
        CHECK(dotEnumItems.size() == 2);
    }
}

TEST_CASE("Analyzer guardrails for module constants, arity, const containers, untyped empty literals, and LSP parity", "[ergonomics][constants][containers][guardrails]") {
    SECTION("Rejects private cross-module constant access and constant reassignment") {
        std::unordered_map<std::string, std::string> memFiles;
        memFiles["cfg.cb"] = R"CB(
public const defaultPort = 8080
const secretKey = "hidden"
)CB";

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
    maxRetries = 4
end fn
)CB";
        const auto reassignRes = cb::transpileSource(badReassignConst, "reassignConst.cb");
        CHECK_FALSE(reassignRes.ok);
        CHECK(reassignRes.error.message.find("Cannot reassign const variable 'maxRetries'") != std::string::npos);
    }

    SECTION("Rejects missing required arguments and wrong struct constructor arity") {
        const std::string missingReqArg = R"CB(
fn greet(name as str, suffix as str)
    print(name & suffix)
end fn
fn main()
    greet("Alice")
end fn
)CB";
        const auto missingRes = cb::transpileSource(missingReqArg, "missingArg.cb");
        CHECK_FALSE(missingRes.ok);
        CHECK(missingRes.error.message.find("Missing required argument for parameter 'suffix'") != std::string::npos);

        const std::string badStructArity = R"CB(
type Pair
    a as int
    b as int
end type
fn main()
    dim p = Pair(1)
end fn
)CB";
        const auto arityRes = cb::transpileSource(badStructArity, "badStructArity.cb");
        CHECK_FALSE(arityRes.ok);
        CHECK(arityRes.error.message.find("Constructor for 'Pair' expects 0 or 2 positional arguments") != std::string::npos);
    }

    SECTION("Rejects mutating container receiver methods (pop, remove) on const variables") {
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

    SECTION("Guardrails: untyped empty []/{}") {
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
    }

    SECTION("LSP surfaces top-level constants and new container receiver methods") {
        cb::LspServerState state{};
        const std::string uri = "file:///ws/const_and_containers.cb";
        const std::string text = R"CB(public const maxConnections = 128
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
        CHECK(constHover.hoverMarkdown.value().find("public const maxConnections as int") != std::string::npos);
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

TEST_CASE("String indexing immutability, .byteAt(i) arity validation, and LSP parity", "[str_index][byteAt][stdlib]") {
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

TEST_CASE("Top-level fallible statement guardrails and unhandled fallible call rejection", "[analyzer][fallible]") {
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

    }

    SECTION("Rejects fallible calls inside for-in iterables, while conditions, and struct field defaults") {
        const std::string fallibleFor = R"CB(
fn loadItems() as list[str] can error
    return ["a"]
end fn

fn badCaller() can error
    for item in loadItems()
        print(item)
    end for
end fn
)CB";
        const auto badForRes = cb::transpileSource(fallibleFor, "fallibleFor.cb");
        CHECK_FALSE(badForRes.ok);
        CHECK(badForRes.error.kind == "AnalyzerError");
        CHECK(badForRes.error.message.find("Fallible call to 'loadItems' must be assigned to a variable, returned directly, or called as a standalone statement.") != std::string::npos);

        const std::string fallibleWhile = R"CB(
fn nextStep() as bool can error
    return false
end fn

fn badCaller() can error
    while nextStep()
        print("step")
    end while
end fn
)CB";
        const auto badWhileRes = cb::transpileSource(fallibleWhile, "fallibleWhile.cb");
        CHECK_FALSE(badWhileRes.ok);
        CHECK(badWhileRes.error.kind == "AnalyzerError");
        CHECK(badWhileRes.error.message.find("Fallible call to 'nextStep' must be assigned to a variable, returned directly, or called as a standalone statement.") != std::string::npos);

        const std::string structFieldDefault = R"CB(
type BadConfig
    port as int = 8080
end type
)CB";
        const auto badStructRes = cb::transpileSource(structFieldDefault, "badStruct.cb");
        CHECK_FALSE(badStructRes.ok);
        CHECK(badStructRes.error.kind == "ParseError");
    }
}

TEST_CASE("Auto-included std/str prelude LSP parity", "[prelude][lsp]") {
    SECTION("LSP zero-import str completions/hover and dict.get") {
        cb::LspServerState state;
        const std::string uri = "file:///workspace/lspPrelude.cb";
        const std::string src = R"CB(fn main()
    dim s = "hello"
    dim d as dict[str, int] = {"a": 1}
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
        for (const auto& item : strItems) {
            if (item.label == "trim") foundTrim = true;
        }
        CHECK(foundTrim);

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

        const auto cursorCtx = cb::resolveCursorToken(src, "/workspace/lspPrelude.cb", 4, 16);
        const auto symInfo = cb::resolveCursorSymbolInfo(snap, "/workspace/lspPrelude.cb", cursorCtx);
        REQUIRE(symInfo.hoverMarkdown.has_value());
        CHECK(symInfo.hoverMarkdown.value().find("public fn trim(s as str) as str") != std::string::npos);
        REQUIRE(symInfo.definition.has_value());
        CHECK(symInfo.definition.value().fileName.find("stdlib/str/str.cb") != std::string::npos);
    }
}

TEST_CASE("Analyzer validates assert() arguments and fallibility rules", "[stdlib][assert]") {
    SECTION("Analyzer validates assert() arguments and fallibility rules") {
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
    }
}

TEST_CASE("Tools cbdoc and cbfmt, LSP '' doc-comments, and collection loop const enforcement", "[cbdoc][cbfmt][lsp][e2e]") {
    SECTION("tools/cbdoc.cb extracts '' doc-comments and public signatures into Markdown") {
        const std::string workDir = "build/testArtifacts/tool_cbdoc";
        std::error_code ec;
        std::filesystem::create_directories(workDir, ec);
        REQUIRE(!ec);

        const std::string outCpp = workDir + "/build.cpp";
        const std::string outBin = workDir + "/cbdoc_bin";
        const std::string outMd = workDir + "/cli_ref.md";
        const std::string stdoutTxt = workDir + "/stdout.txt";

        const auto buildRes = cb::buildFile("tools/cbdoc.cb", outCpp, true, outBin);
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
        CHECK(mdText.find("public fn newParser(") != std::string::npos);
        CHECK(mdText.find("Creates a new declarative command-line Parser") != std::string::npos);

        // Verify cbdoc handles fieldless and multi-line extern types (including extern public type)
        const std::string extCb = workDir + "/extern_types.cb";
        const std::string extMd = workDir + "/extern_ref.md";
        {
            std::ofstream f(extCb, std::ios::binary | std::ios::trunc);
            f << R"CB('' Opaque regex handle.
extern public type Regex
end type

'' Vector with fields.
public extern type Vec2
    x as float
    y as float
end type

'' Computes length.
public extern fn vec2Len(v as Vec2) as float
)CB";
        }
        const int extDocRc = std::system(
            ("./" + outBin + " -o " + extMd + " " + extCb + " > " + stdoutTxt + " 2>&1").c_str()
        );
        CHECK(extDocRc == 0);
        const std::string extMdText = cb::testE2E::readFileText(extMd);
        CHECK(extMdText.find("Regex") != std::string::npos);
        CHECK(extMdText.find("extern public type Regex") != std::string::npos);
        CHECK(extMdText.find("Vec2") != std::string::npos);
        CHECK(extMdText.find("x as float") != std::string::npos);
        CHECK(extMdText.find("vec2Len") != std::string::npos);
    }

    SECTION("tools/cbfmt.cb formats CleanBasic source via stdin, --check, and --write idempotently") {
        const std::string workDir = "build/testArtifacts/tool_cbfmt";
        std::error_code ec;
        std::filesystem::create_directories(workDir, ec);
        REQUIRE(!ec);

        const std::string outCpp = workDir + "/build.cpp";
        const std::string outBin = workDir + "/cbfmt_bin";
        const std::string messyFile = workDir + "/messy.cb";
        const std::string stdoutTxt = workDir + "/stdout.txt";

        const auto buildRes = cb::buildFile("tools/cbfmt.cb", outCpp, true, outBin);
        INFO("buildFile error: " << cb::formatDiagnostic(buildRes.error));
        REQUIRE(buildRes.ok);

        const std::string messySource = R"CB(

'unspaced comment
''doc comment
public type Item
id as int
name as str
end type


fn compute(x as int) as int
if x > 0
select case x
case 1
return 10
case else
return 20
end select
else
dim it = Item(
0,
"none"
)
return it.id
end if
end fn

)CB";

        const std::string expectedFormatted = R"CB(' unspaced comment
'' doc comment
public type Item
    id as int
    name as str
end type

fn compute(x as int) as int
    if x > 0
        select case x
            case 1
                return 10
            case else
                return 20
        end select
    else
        dim it = Item(
            0,
            "none"
        )
        return it.id
    end if
end fn
)CB";

        {
            std::ofstream f(messyFile, std::ios::binary | std::ios::trunc);
            f << messySource;
        }

        // 1. --check on unformatted file exits with non-zero
        const int checkFailRc = std::system(
            ("./" + outBin + " --check " + messyFile + " > " + stdoutTxt + " 2>&1").c_str()
        );
        CHECK(checkFailRc != 0);
        CHECK(cb::testE2E::readFileText(stdoutTxt).find("needs formatting") != std::string::npos);

        // 2. Stdin formatting produces exact formatted output without modifying the file
        const int stdinRc = std::system(
            ("./" + outBin + " < " + messyFile + " > " + stdoutTxt).c_str()
        );
        CHECK(stdinRc == 0);
        CHECK(cb::testE2E::readFileText(stdoutTxt) == expectedFormatted);
        CHECK(cb::testE2E::readFileText(messyFile) == messySource);

        // 3. --write formats the file in place, and subsequent --check succeeds (idempotent)
        const int writeRc = std::system(
            ("./" + outBin + " -w " + messyFile + " > " + stdoutTxt + " 2>&1").c_str()
        );
        CHECK(writeRc == 0);
        CHECK(cb::testE2E::readFileText(messyFile) == expectedFormatted);

        const int checkPassRc = std::system(
            ("./" + outBin + " --check " + messyFile + " > " + stdoutTxt + " 2>&1").c_str()
        );
        CHECK(checkPassRc == 0);

        // 4. Verify the formatted code passes semantic analysis
        const auto checkFormatted = cb::checkSource(expectedFormatted, "formatted.cb");
        REQUIRE(checkFormatted.ok);

        // 4b. Verify cbfmt preserves nested quotes and apostrophes inside interpolated strings
        const std::string interpFile = workDir + "/interp.cb";
        const std::string interpSource = R"CB(fn demo() as str
dim s = $"hello {"world ' not a comment"}!" 'trailing comment
return s
end fn
)CB";
        const std::string expectedInterp = R"CB(fn demo() as str
    dim s = $"hello {"world ' not a comment"}!" ' trailing comment
    return s
end fn
)CB";
        {
            std::ofstream f(interpFile, std::ios::binary | std::ios::trunc);
            f << interpSource;
        }
        const int interpStdinRc = std::system(
            ("./" + outBin + " < " + interpFile + " > " + stdoutTxt).c_str()
        );
        CHECK(interpStdinRc == 0);
        CHECK(cb::testE2E::readFileText(stdoutTxt) == expectedInterp);

        // 5. Verify stdlib, examples, tools, and tests/cases pass cbfmt --check
        const int repoCheckRc = std::system(
            ("./" + outBin + " --check stdlib examples tools tests/cases > " + stdoutTxt + " 2>&1").c_str()
        );
        INFO("cbfmt --check stdlib examples tools tests/cases output: " << cb::testE2E::readFileText(stdoutTxt));
        CHECK(repoCheckRc == 0);
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
public type Endpoint
    host as str
    port as int
end type

'' Connects to the target Endpoint.
'' Raises an error if the port is invalid.
public fn connectEndpoint(ep as Endpoint, timeoutMs as int) as bool can error
    return ep.port > 0 and timeoutMs > 0
end fn

fn main() as int
    ' setup
    ' parser
    try
    dim p = cli.newParser("app", "1.0", "App")
    dim ep = Endpoint("localhost", 8080)
    dim ok = connectEndpoint(ep, 500)
    catch e
    end try
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

        const auto hoverFn = cb::testLsp::readResponseForId(serverToClient, 2);
        const cb::JsonId hoverFnRes = cb::jsonGetField(hoverFn.arena, hoverFn.root, "result");
        const cb::JsonId hoverFnContents = cb::jsonGetField(hoverFn.arena, hoverFnRes, "contents");
        const std::string hoverFnVal = cb::jsonGetString(hoverFn.arena, cb::jsonGetField(hoverFn.arena, hoverFnContents, "value")).value_or("");
        CHECK(hoverFnVal.find("public fn connectEndpoint(ep as Endpoint, timeoutMs as int) as bool can error") != std::string::npos);
        CHECK(hoverFnVal.find("Connects to the target Endpoint.\nRaises an error if the port is invalid.") != std::string::npos);

        const auto hoverStdCli = cb::testLsp::readResponseForId(serverToClient, 3);
        const cb::JsonId hoverStdRes = cb::jsonGetField(hoverStdCli.arena, hoverStdCli.root, "result");
        const cb::JsonId hoverStdContents = cb::jsonGetField(hoverStdCli.arena, hoverStdRes, "contents");
        const std::string hoverStdVal = cb::jsonGetString(hoverStdCli.arena, cb::jsonGetField(hoverStdCli.arena, hoverStdContents, "value")).value_or("");
        CHECK(hoverStdVal.find("public fn newParser(name as str, version as str, description as str) as Parser") != std::string::npos);
        CHECK(hoverStdVal.find("Creates a new declarative command-line Parser") != std::string::npos);

        const auto sigResp = cb::testLsp::readResponseForId(serverToClient, 4);
        const cb::JsonId sigRes = cb::jsonGetField(sigResp.arena, sigResp.root, "result");
        const cb::JsonId sigs = cb::jsonGetField(sigResp.arena, sigRes, "signatures");
        REQUIRE(std::holds_alternative<cb::JsonArray>(sigResp.arena.nodes[sigs]));
        const cb::JsonId sig0 = std::get<cb::JsonArray>(sigResp.arena.nodes[sigs]).elements[0];
        const cb::JsonId sigDoc = cb::jsonGetField(sigResp.arena, sig0, "documentation");
        const std::string sigDocVal = cb::jsonGetString(sigResp.arena, cb::jsonGetField(sigResp.arena, sigDoc, "value")).value_or("");
        CHECK(sigDocVal.find("Connects to the target Endpoint.") != std::string::npos);

        // Verify built-in assert(...) signature help via resolveSignatureHelp
        const cb::LspDocumentSnapshot assertSnap = cb::makeSnapshotFromCheckResult(cb::checkSource(docSource, "/ws/doc_test.cb"));
        const cb::CallSiteContext assertCallCtx = cb::parseEnclosingCallSite("    assert(ok, ");
        const auto assertSigOpt = cb::resolveSignatureHelp(assertSnap, "", std::string("main"), assertCallCtx);
        REQUIRE(assertSigOpt.has_value());
        CHECK(assertSigOpt->label == "fn assert(cond as bool, msg as str) can error");
        CHECK(assertSigOpt->activeParameter == 1);
        CHECK(assertSigOpt->documentation.find("Asserts that cond is true") != std::string::npos);
    }

    SECTION("Analyzer rejects mutating collection for-in loop variables or chained non-byref parameters") {
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

TEST_CASE("Tagged union guardrails, const list sort/removeAt checks, overload arity errors, and LSP union/UFCS parity", "[union][containers][lsp]") {
    SECTION("Analyzer rejects private union access, unknown union variants, and invalid case payload bindings") {
        std::unordered_map<std::string, std::string> memFiles;
        memFiles["ast.cb"] = R"CB(
public type Span
    line as int
    col as int
end type

public union AstNode
    intVal as int
    textVal as str
    spanVal as Span
end union

union PrivateUnion
    secret as int
end union
)CB";
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

    SECTION("Analyzer rejects .sort() and .removeAt() on const list variables") {
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
        const std::string text = R"CB(public union TokenVal
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

    SECTION("Analyzer rejects duplicate function definitions in the same module and LSP discovers cross-module UFCS") {
        const std::string badDupCode = R"CB(
type TypeRegistry
    items as list[str]
end type

fn findType(reg as TypeRegistry, target as str) as int
    return target.len()
end fn

fn findType(byref reg as TypeRegistry, target as list[int], tag as str) as int
    return target.len()
end fn
)CB";
        const auto badDup = cb::checkSource(badDupCode, "bad_dup.cb");
        CHECK_FALSE(badDup.ok);
        CHECK(badDup.error.message.find("Duplicate function definition 'findType'.") != std::string::npos);

        // Verify LSP dot-completion on 'arena' in analyzer.cb discovers 'resolveAlias' from imported symbols.cb
        cb::LspServerState lspState{};
        lspState.openDocuments["file:///ws/ast.cb"] = R"CB(public type AstArena
    types as list[str]
end type
)CB";
        lspState.openDocuments["file:///ws/symbols.cb"] = R"CB(import "ast.cb"
public fn resolveAlias(arena as ast.AstArena, tid as int) as str
    return arena.types[tid]
end fn
)CB";
        lspState.openDocuments["file:///ws/analyzer.cb"] = R"CB(import "ast.cb"
import "symbols.cb"
fn main() as int
    dim arena = ast.AstArena(["int"])
    return 0
end fn
)CB";
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
}

TEST_CASE("Audit remediation: missing module ImportError, dict loop variable count, and uppercase hex string escapes", "[audit][pipeline][analyzer][emitter]") {
    SECTION("Reports ImportError when importing a nonexistent .cb module file") {
        const std::string badImport = R"CB(import "does_not_exist_anywhere.cb"
fn main() as int
    return 0
end fn
)CB";
        const auto res = cb::transpileSource(badImport, "missing_import_test.cb");
        CHECK_FALSE(res.ok);
        CHECK(res.error.kind == "ImportError");
        CHECK(res.error.message.find("Module file not found: 'does_not_exist_anywhere.cb'") != std::string::npos);
    }

    SECTION("Rejects invalid loop variable counts in list, str, and dict 'for ... in' loops") {
        const std::string badListLoop2 = R"CB(
fn main() as int
    dim xs as list[int] = [10, 20]
    for i, x in xs
        print(i, x)
    end for
    return 0
end fn
)CB";
        const auto resList = cb::transpileSource(badListLoop2, "bad_list_loop2.cb");
        CHECK_FALSE(resList.ok);
        CHECK(resList.error.kind == "AnalyzerError");
        CHECK(resList.error.message.find("List 'for ... in' loop expects 1 variable (for item in listVal).") != std::string::npos);

        const std::string badStrLoop2 = R"CB(
fn main() as int
    dim s = "hi"
    for i, ch in s
        print(i, ch)
    end for
    return 0
end fn
)CB";
        const auto resStr = cb::transpileSource(badStrLoop2, "bad_str_loop2.cb");
        CHECK_FALSE(resStr.ok);
        CHECK(resStr.error.kind == "AnalyzerError");
        CHECK(resStr.error.message.find("String 'for ... in' loop expects 1 variable (for ch in strVal).") != std::string::npos);

        const std::string badDictLoop1 = R"CB(
fn main() as int
    dim d as dict[str, int] = {"a": 1}
    for k in d
        print(k)
    end for
    return 0
end fn
)CB";
        const auto res1 = cb::transpileSource(badDictLoop1, "bad_dict_loop1.cb");
        CHECK_FALSE(res1.ok);
        CHECK(res1.error.kind == "AnalyzerError");
        CHECK(res1.error.message.find("Dictionary 'for ... in' loop requires key and value variables (for k, v in dictVal).") != std::string::npos);

        const std::string badDictLoop3 = R"CB(
fn main() as int
    dim d as dict[str, int] = {"a": 1}
    for k, v, z in d
        print(k)
    end for
    return 0
end fn
)CB";
        const auto res3 = cb::transpileSource(badDictLoop3, "bad_dict_loop3.cb");
        CHECK_FALSE(res3.ok);
        CHECK(res3.error.kind == "AnalyzerError");
        CHECK(res3.error.message.find("Dictionary 'for ... in' loop requires key and value variables (for k, v in dictVal).") != std::string::npos);
    }

    SECTION("Normalizes uppercase hex escapes in string literals for valid C++17 emission") {
        const std::string bs(1, static_cast<char>(92));
        const std::string dq(1, static_cast<char>(34));
        const std::string nl(1, static_cast<char>(10));
        const std::string src = "fn main() as int" + nl +
            "    dim s = " + dq + "hello" + bs + "X41" + bs + "x42" + dq + nl +
            "    print(s)" + nl +
            "    return 0" + nl +
            "end fn" + nl;
        const auto res = cb::transpileSource(src, "hex_esc.cb");
        REQUIRE(res.ok);
        CHECK(res.cppCode.find("std::string(" + dq + "hello" + bs + "x41" + bs + "x42" + dq + ")") != std::string::npos);
        CHECK(res.cppCode.find(bs + "X41") == std::string::npos);
    }
}

TEST_CASE("Fast linker detection, built-in type alias fast path, and PCH hash stability", "[pipeline][symbols][optimization]") {
    SECTION("Built-in type normalization fast path preserves primitive TypeIds and normalizes user types") {
        const std::string src = R"CB(public type Box
    v as int
end type

fn main() as int
    dim a as int = 10
    dim b as list[Box] = [Box(a)]
    return b[0].v
end fn
)CB";
        const auto checkRes = cb::checkSource(src, "type_fastpath.cb");
        REQUIRE(checkRes.ok);

        CHECK(cb::resolveAlias(checkRes.arena, checkRes.symbols, "", checkRes.symbols.typeInt) == checkRes.symbols.typeInt);
        CHECK(cb::resolveAlias(checkRes.arena, checkRes.symbols, "", checkRes.symbols.typeStr) == checkRes.symbols.typeStr);
        CHECK(cb::resolveAlias(checkRes.arena, checkRes.symbols, "", checkRes.symbols.typeBool) == checkRes.symbols.typeBool);
        const cb::TypeId boxTid = checkRes.arena.findType("Box");
        REQUIRE(boxTid != cb::INVALID_ID);
        CHECK(cb::resolveAlias(checkRes.arena, checkRes.symbols, "", boxTid) == boxTid);
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

TEST_CASE("fnScopes preserves outer variable types when inner try/catch scopes shadow variable names", "[analyzer][symbols]") {
    SECTION("fnScopes preserves outer variable types when inner try/catch scopes declare same-named variables") {
        const std::string src = R"CB(
fn demo() as int
    dim val = 42
    try
        dim innerOnly = "hello"
    catch val
        print(val.msg)
    end try
    return val
end fn
)CB";
        const auto checkRes = cb::checkSource(src, "scope_preserve.cb");
        REQUIRE(checkRes.ok);
        const auto fnScopeIt = checkRes.symbols.fnScopes.find("demo");
        REQUIRE(fnScopeIt != checkRes.symbols.fnScopes.end());
        const auto valIt = fnScopeIt->second.find("val");
        REQUIRE(valIt != fnScopeIt->second.end());
        CHECK(valIt->second.typeId == checkRes.symbols.typeInt);
    }
}

TEST_CASE("Opaque extern types, lexical block scoping, recursive value type cycle detection, topological struct ordering, 1-to-1 module mangling, and macro hygiene", "[access][analyzer][emitter][mangling]") {
    SECTION("Enforces true lexical block scoping across if, for, while, select case, try/catch, and defer blocks") {
        const std::string leakedIfVar = R"CB(
fn main() as int
    if true
        dim innerVal = 42
    end if
    innerVal = 99
    return 0
end fn
)CB";
        const auto resIf = cb::checkSource(leakedIfVar, "leaked_if.cb");
        REQUIRE_FALSE(resIf.ok);
        CHECK(resIf.error.message.find("Undeclared variable 'innerVal'") != std::string::npos);

        const std::string leakedForVar = R"CB(
fn main() as int
    for i = 0 to 3
        dim stepVal = i
    end for
    i = 10
    return 0
end fn
)CB";
        const auto resFor = cb::checkSource(leakedForVar, "leaked_for.cb");
        REQUIRE_FALSE(resFor.ok);
        CHECK(resFor.error.message.find("Undeclared variable 'i'") != std::string::npos);

        // Verify sibling branches can independently declare same-named variables with different types
        const std::string siblingShadow = R"CB(
fn main() as int
    if true
        dim temp = 10
    else
        dim temp = "hello"
    end if
    for i = 0 to 2
        dim item = i
    end for
    for i = 0 to 2
        dim item = "str"
    end for
    return 0
end fn
)CB";
        const auto okSibling = cb::transpileSource(siblingShadow, "sibling.cb");
        REQUIRE(okSibling.ok);
    }

    SECTION("Standard library handles in std/fs and std/json are opaque fieldless extern types") {
        const std::string badFsHandle = R"CB(import "std/fs"
fn main() as int
    dim f = fs.stdin()
    return f.handle
end fn
)CB";
        const auto resFs = cb::checkSource(badFsHandle, "bad_fs.cb");
        REQUIRE_FALSE(resFs.ok);
        CHECK(resFs.error.message.find("Type 'File' has no field 'handle'.") != std::string::npos);

        const std::string badJsonConstruct = R"CB(import "std/json"
fn main() as int
    dim d = json.JsonDoc()
    return 0
end fn
)CB";
        const auto resJson = cb::checkSource(badJsonConstruct, "bad_json.cb");
        REQUIRE_FALSE(resJson.ok);
        CHECK(resJson.error.message.find("Cannot directly construct opaque extern type 'JsonDoc'.") != std::string::npos);
    }

    SECTION("Detects direct by-value recursive cycles in structs and unions while allowing list[T] and dict[K, V] self-references") {
        const std::string directCycle = R"CB(
type A
    b as B
end type

type B
    a as A
end type

fn main() as int
    return 0
end fn
)CB";
        const auto cycleRes = cb::checkSource(directCycle, "cycle.cb");
        REQUIRE_FALSE(cycleRes.ok);
        CHECK(cycleRes.error.message.find("Recursive value type cycle detected involving 'A'") != std::string::npos);

        const std::string unionCycle = R"CB(
type ExprBox
    node as ExprNode
end type

union ExprNode
    lit as int
    boxed as ExprBox
end union

fn main() as int
    return 0
end fn
)CB";
        const auto uCycleRes = cb::checkSource(unionCycle, "ucycle.cb");
        REQUIRE_FALSE(uCycleRes.ok);
        CHECK(uCycleRes.error.message.find("Recursive value type cycle detected involving 'ExprBox'") != std::string::npos);

        const std::string heapRecursive = R"CB(
type TreeNode
    label as str
    children as list[TreeNode]
    lookup as dict[str, TreeNode]
end type

fn main() as int
    dim root = TreeNode("root", [], {})
    return root.children.len()
end fn
)CB";
        const auto heapRes = cb::checkSource(heapRecursive, "tree.cb");
        REQUIRE(heapRes.ok);
    }

    SECTION("Topologically sorts emitted by-value structs and unions regardless of source declaration order") {
        const std::string outOfOrderSrc = R"CB(
type Car
    engine as Engine
end type

type Engine
    cylinders as CylinderSpec
end type

type CylinderSpec
    count as int
end type

fn main() as int
    dim c = Car(Engine(CylinderSpec(6)))
    return c.engine.cylinders.count
end fn
)CB";
        const auto tpRes = cb::transpileSource(outOfOrderSrc, "topo.cb");
        REQUIRE(tpRes.ok);
        const size_t posCyl = tpRes.cppCode.find("struct CylinderSpec {");
        const size_t posEng = tpRes.cppCode.find("struct Engine {");
        const size_t posCar = tpRes.cppCode.find("struct Car {");
        REQUIRE(posCyl != std::string::npos);
        REQUIRE(posEng != std::string::npos);
        REQUIRE(posCar != std::string::npos);
        CHECK(posCyl < posEng);
        CHECK(posEng < posCar);
    }

    SECTION("1-to-1 collision-free module mangling and C/POSIX macro identifier sanitization") {
        CHECK(cb::sanitizeModuleId("foo/bar") == "foo_bar");
        CHECK(cb::sanitizeModuleId("foo_bar") == "foo__bar");
        CHECK(cb::sanitizeModuleId("foo/bar") != cb::sanitizeModuleId("foo_bar"));
        CHECK(cb::makeCppFnName("foo/bar", "run") == "cb_foo_bar__run");
        CHECK(cb::makeCppFnName("foo_bar", "run") == "cb_foo__bar__run");

        CHECK(cb::sanitizeCppIdent("errno") == "errno_cb");
        CHECK(cb::sanitizeCppIdent("stdin") == "stdin_cb");
        CHECK(cb::sanitizeCppIdent("stdout") == "stdout_cb");
        CHECK(cb::sanitizeCppIdent("stderr") == "stderr_cb");
        CHECK(cb::sanitizeCppIdent("EOF") == "EOF_cb");
        CHECK(cb::sanitizeCppIdent("NULL") == "NULL_cb");
        CHECK(cb::sanitizeCppIdent("unix") == "unix_cb");
        CHECK(cb::sanitizeCppIdent("linux") == "linux_cb");

        const std::string macroSrc = R"CB(
fn testMacros(errno as int) as int
    dim EOF = errno + 1
    dim stdout = EOF + 1
    return stdout
end fn
)CB";
        const auto macroTp = cb::transpileSource(macroSrc, "macros.cb");
        REQUIRE(macroTp.ok);
        CHECK(macroTp.cppCode.find("#define NOMINMAX") != std::string::npos);
        CHECK(macroTp.cppCode.find("#undef min") != std::string::npos);
        CHECK(macroTp.cppCode.find("#undef max") != std::string::npos);
        CHECK(macroTp.cppCode.find("#undef ERROR") != std::string::npos);
        CHECK(macroTp.cppCode.find("int64_t errno_cb") != std::string::npos);
        CHECK(macroTp.cppCode.find("EOF_cb = (errno_cb + 1)") != std::string::npos);
    }

    SECTION("Sanitizes non-extern struct field names that collide with C++ keywords or macros") {
        const std::string structKwSrc = R"CB(
type Config
    default as int
    class as str
    template as int
    delete as bool
end type

fn main() as int
    dim c = Config(10, "admin", 3, true)
    c.default = c.default + c.template
    if c.delete and c.class == "admin"
        return c.default
    end if
    return 0
end fn
)CB";
        const auto tp = cb::transpileSource(structKwSrc, "struct_kw.cb");
        REQUIRE(tp.ok);
        CHECK(tp.cppCode.find("int64_t default_{};") != std::string::npos);
        CHECK(tp.cppCode.find("std::string class_{};") != std::string::npos);
        CHECK(tp.cppCode.find("int64_t template_{};") != std::string::npos);
        CHECK(tp.cppCode.find("bool delete_{};") != std::string::npos);
        CHECK(tp.cppCode.find("c.default_") != std::string::npos);
        CHECK(tp.cppCode.find("c.template_") != std::string::npos);
    }

    SECTION("Emits RFC 3629 UTF-8 validator in runtime preamble and single-entry std::filesystem::remove in cbFsRemove") {
        const std::string utf8AndFsSrc = R"CB(
import "std/fs"

fn main() as int
    try
        fs.remove("nonexistent_file.txt")
    catch e
        print(e.msg)
    end try
    return 0
end fn
)CB";
        const auto tp = cb::transpileSource(utf8AndFsSrc, "utf8_fs.cb");
        REQUIRE(tp.ok);
        CHECK(tp.cppCode.find("b0 == 0xE0u && b1 < 0xA0u") != std::string::npos);
        CHECK(tp.cppCode.find("b0 == 0xEDu && b1 >= 0xA0u") != std::string::npos);
        CHECK(tp.cppCode.find("b0 == 0xF0u && b1 < 0x90u") != std::string::npos);
        CHECK(tp.cppCode.find("b0 == 0xF4u && b1 > 0x8Fu") != std::string::npos);

        const std::string fsBridgeContent = cb::testE2E::readFileText("stdlib/fs/fs_bridge.hpp");
        CHECK(fsBridgeContent.find("const bool removed = std::filesystem::remove(path, ec);") != std::string::npos);
    }

    SECTION("CLI parseCliArgs populates clear errorMessage for missing flag values and unknown flags") {
        const auto missingOut = cb::parseCliArgs({"main.cb", "-o"});
        CHECK(!missingOut.ok);
        CHECK(missingOut.errorMessage.find("Missing output file path") != std::string::npos);

        const auto missingBin = cb::parseCliArgs({"main.cb", "-b"});
        CHECK(!missingBin.ok);
        CHECK(missingBin.errorMessage.find("Missing binary output path") != std::string::npos);

        const auto missingTarget = cb::parseCliArgs({"main.cb", "-t"});
        CHECK(!missingTarget.ok);
        CHECK(missingTarget.errorMessage.find("Missing target platform") != std::string::npos);

        const auto missingCc = cb::parseCliArgs({"main.cb", "--cc"});
        CHECK(!missingCc.ok);
        CHECK(missingCc.errorMessage.find("Missing compiler command") != std::string::npos);

        const auto unknownFlag = cb::parseCliArgs({"main.cb", "--unknown-flag"});
        CHECK(!unknownFlag.ok);
        CHECK(unknownFlag.errorMessage.find("Unknown option") != std::string::npos);
    }

    SECTION("Zero-copy const auto& union select case binding, const enforcement, pre-cached built-in receiver methods, and Analyzer call-graph edges") {
        const std::string unionSelectSrc = R"CB(
type Payload
    count as int
end type

union Event
    readOnly as Payload
    mutated as Payload
end union

fn bump(byref p as Payload)
    p.count = p.count + 1
end fn

fn inspectEvent(ev as Event) as int
    select case ev
        case Event.readOnly as ro
            return ro.count
        case Event.mutated as m
            dim copy = m
            bump(copy)
            return copy.count
    end select
    return 0
end fn

fn main() as int
    dim e1 = Event.readOnly(Payload(10))
    dim e2 = Event.mutated(Payload(20))
    return inspectEvent(e1) + inspectEvent(e2)
end fn
)CB";
        const auto checkRes = cb::checkSource(unionSelectSrc, "union_zero_copy.cb");
        REQUIRE(checkRes.ok);
        CHECK(checkRes.symbols.builtinReceiverCache.count("int") == 1);
        CHECK(checkRes.symbols.builtinReceiverCache.count("str") == 1);
        CHECK(checkRes.symbols.rootReferencedFunctions.count("main") == 1);
        CHECK(checkRes.symbols.callGraphEdges.at("main").count("inspectEvent") == 1);
        CHECK(checkRes.symbols.callGraphEdges.at("inspectEvent").count("bump") == 1);

        CHECK(!checkRes.cppCode.empty());
        CHECK(checkRes.cppCode == checkRes.symbols.emittedCpp);

        const auto tpRes = cb::transpileSource(unionSelectSrc, "union_zero_copy.cb");
        REQUIRE(tpRes.ok);
        CHECK(tpRes.cppCode == checkRes.cppCode);
        CHECK(tpRes.cppCode.find("const auto& _sel_1 = ev;") != std::string::npos);
        CHECK(tpRes.cppCode.find("const auto& ro = std::get<0>(_sel_1._data);") != std::string::npos);
        CHECK(tpRes.cppCode.find("const auto& m = std::get<1>(_sel_1._data);") != std::string::npos);

        const std::string badReassignUnionBinding = R"CB(
union Val
    num as int
end union

fn bad(v as Val) as int
    select case v
        case Val.num as n
            n = 10
            return n
    end select
    return 0
end fn
)CB";
        const auto badReassignCheck = cb::checkSource(badReassignUnionBinding, "bad_union_reassign.cb");
        CHECK_FALSE(badReassignCheck.ok);
        CHECK(badReassignCheck.error.message.find("Cannot reassign const variable 'n'") != std::string::npos);

        const std::string badByrefUnionBinding = R"CB(
type Payload
    count as int
end type

union Event
    item as Payload
end union

fn bump(byref p as Payload)
    p.count = p.count + 1
end fn

fn bad(ev as Event)
    select case ev
        case Event.item as p
            bump(p)
    end select
end fn
)CB";
        const auto badByrefCheck = cb::checkSource(badByrefUnionBinding, "bad_union_byref.cb");
        CHECK_FALSE(badByrefCheck.ok);
        CHECK(badByrefCheck.error.message.find("Cannot pass const variable 'p' to byref parameter 'p'.") != std::string::npos);
    }
}
