#ifndef CLEANBASIC_TEST_HARNESS_HPP
#define CLEANBASIC_TEST_HARNESS_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace cb::test {

struct AssertionFailure {
    std::string file;
    uint32_t line = 0;
    std::string expression;
    std::string message;
};

struct TestContext {
    std::string current_test_name;
    uint32_t assertions_run = 0;
    std::vector<AssertionFailure> failures;
    bool abort_current_test = false;
};

struct TestCase {
    std::string name;
    std::string suite;
    std::function<void(TestContext&)> run;
};

struct TestCaseSpec {
    std::string name;
    std::string input_source;
    std::vector<std::string> expected_substrings;
    std::vector<std::string> forbidden_substrings;
    bool expect_error = false;
    std::string expected_error_substring;
};

struct TranspileOutcome {
    bool ok = false;
    std::string output_or_error;
};

struct SuiteSummary {
    uint32_t total_tests = 0;
    uint32_t passed_tests = 0;
    uint32_t failed_tests = 0;
    uint32_t total_assertions = 0;
};

inline void record_failure(
    TestContext& ctx,
    std::string_view file,
    uint32_t line,
    std::string_view expr_str,
    std::string_view msg
) {
    AssertionFailure failure{};
    failure.file = std::string(file);
    failure.line = line;
    failure.expression = std::string(expr_str);
    failure.message = std::string(msg);
    ctx.failures.push_back(failure);
}

inline void check_true(
    TestContext& ctx,
    bool condition,
    std::string_view expr_str,
    std::string_view msg,
    std::string_view file,
    uint32_t line
) {
    ctx.assertions_run += 1;
    if (!condition) {
        std::ostringstream oss;
        oss << "Expected condition to be true";
        if (!msg.empty()) {
            oss << " (" << msg << ")";
        }
        record_failure(ctx, file, line, expr_str, oss.str());
    }
}

inline void check_false(
    TestContext& ctx,
    bool condition,
    std::string_view expr_str,
    std::string_view msg,
    std::string_view file,
    uint32_t line
) {
    ctx.assertions_run += 1;
    if (condition) {
        std::ostringstream oss;
        oss << "Expected condition to be false";
        if (!msg.empty()) {
            oss << " (" << msg << ")";
        }
        record_failure(ctx, file, line, expr_str, oss.str());
    }
}

template <typename A, typename B>
inline void check_eq(
    TestContext& ctx,
    const A& actual,
    const B& expected,
    std::string_view actual_expr,
    std::string_view expected_expr,
    std::string_view file,
    uint32_t line
) {
    ctx.assertions_run += 1;
    if (!(actual == expected)) {
        std::ostringstream expr_oss;
        expr_oss << actual_expr << " == " << expected_expr;
        std::ostringstream msg_oss;
        msg_oss << "Equality check failed: actual [" << actual << "] != expected [" << expected << "]";
        record_failure(ctx, file, line, expr_oss.str(), msg_oss.str());
    }
}

template <typename A, typename B>
inline void check_ne(
    TestContext& ctx,
    const A& actual,
    const B& expected,
    std::string_view actual_expr,
    std::string_view expected_expr,
    std::string_view file,
    uint32_t line
) {
    ctx.assertions_run += 1;
    if (!(actual != expected)) {
        std::ostringstream expr_oss;
        expr_oss << actual_expr << " != " << expected_expr;
        std::ostringstream msg_oss;
        msg_oss << "Inequality check failed: both values equal [" << actual << "]";
        record_failure(ctx, file, line, expr_oss.str(), msg_oss.str());
    }
}

inline void check_contains(
    TestContext& ctx,
    std::string_view haystack,
    std::string_view needle,
    std::string_view file,
    uint32_t line
) {
    ctx.assertions_run += 1;
    if (haystack.find(needle) == std::string_view::npos) {
        std::ostringstream msg_oss;
        msg_oss << "Expected output to contain substring [" << needle << "], but got [" << haystack << "]";
        record_failure(ctx, file, line, "check_contains", msg_oss.str());
    }
}

inline void check_not_contains(
    TestContext& ctx,
    std::string_view haystack,
    std::string_view needle,
    std::string_view file,
    uint32_t line
) {
    ctx.assertions_run += 1;
    if (haystack.find(needle) != std::string_view::npos) {
        std::ostringstream msg_oss;
        msg_oss << "Expected output NOT to contain substring [" << needle << "], but found it in [" << haystack << "]";
        record_failure(ctx, file, line, "check_not_contains", msg_oss.str());
    }
}

template <typename PipelineFn>
inline void run_table_tests(
    TestContext& ctx,
    const std::vector<TestCaseSpec>& specs,
    PipelineFn pipeline_fn
) {
    for (const auto& spec : specs) {
        const TranspileOutcome outcome = pipeline_fn(spec.input_source);
        if (spec.expect_error) {
            ctx.assertions_run += 1;
            if (outcome.ok) {
                std::ostringstream oss;
                oss << "[" << spec.name << "] Expected error, but pipeline succeeded with output: " << outcome.output_or_error;
                record_failure(ctx, __FILE__, static_cast<uint32_t>(__LINE__), spec.name, oss.str());
                continue;
            }
            if (!spec.expected_error_substring.empty()) {
                ctx.assertions_run += 1;
                if (outcome.output_or_error.find(spec.expected_error_substring) == std::string::npos) {
                    std::ostringstream oss;
                    oss << "[" << spec.name << "] Expected error containing [" << spec.expected_error_substring
                        << "], got [" << outcome.output_or_error << "]";
                    record_failure(ctx, __FILE__, static_cast<uint32_t>(__LINE__), spec.name, oss.str());
                }
            }
        } else {
            ctx.assertions_run += 1;
            if (!outcome.ok) {
                std::ostringstream oss;
                oss << "[" << spec.name << "] Expected success, but failed with error: " << outcome.output_or_error;
                record_failure(ctx, __FILE__, static_cast<uint32_t>(__LINE__), spec.name, oss.str());
                continue;
            }
            for (const auto& expected_sub : spec.expected_substrings) {
                ctx.assertions_run += 1;
                if (outcome.output_or_error.find(expected_sub) == std::string::npos) {
                    std::ostringstream oss;
                    oss << "[" << spec.name << "] Missing expected substring [" << expected_sub
                        << "] in output [" << outcome.output_or_error << "]";
                    record_failure(ctx, __FILE__, static_cast<uint32_t>(__LINE__), spec.name, oss.str());
                }
            }
            for (const auto& forbidden_sub : spec.forbidden_substrings) {
                ctx.assertions_run += 1;
                if (outcome.output_or_error.find(forbidden_sub) != std::string::npos) {
                    std::ostringstream oss;
                    oss << "[" << spec.name << "] Found forbidden substring [" << forbidden_sub
                        << "] in output [" << outcome.output_or_error << "]";
                    record_failure(ctx, __FILE__, static_cast<uint32_t>(__LINE__), spec.name, oss.str());
                }
            }
        }
    }
}

inline bool matches_filter(const TestCase& tc, std::string_view suite_filter) {
    if (suite_filter.empty()) {
        return true;
    }
    if (tc.suite.find(suite_filter) != std::string::npos) {
        return true;
    }
    if (tc.name.find(suite_filter) != std::string::npos) {
        return true;
    }
    const std::string full_name = tc.suite + "::" + tc.name;
    return full_name.find(suite_filter) != std::string::npos;
}

inline SuiteSummary run_all_tests(
    const std::vector<TestCase>& tests,
    std::string_view suite_filter = ""
) {
    SuiteSummary summary{};
    for (const auto& tc : tests) {
        if (!matches_filter(tc, suite_filter)) {
            continue;
        }
        summary.total_tests += 1;
        TestContext ctx{};
        ctx.current_test_name = tc.suite + "::" + tc.name;
        tc.run(ctx);
        summary.total_assertions += ctx.assertions_run;

        if (ctx.failures.empty()) {
            summary.passed_tests += 1;
            std::cout << "[ PASS ] " << tc.suite << "::" << tc.name << "\n";
        } else {
            summary.failed_tests += 1;
            std::cout << "[ FAIL ] " << tc.suite << "::" << tc.name << "\n";
            for (const auto& f : ctx.failures) {
                std::cout << "  " << f.file << ":" << f.line << ": " << f.expression << " -> " << f.message << "\n";
            }
        }
    }

    std::cout << "Passed " << summary.passed_tests << "/" << summary.total_tests
              << " tests (" << summary.total_assertions << " assertions)\n";
    return summary;
}

inline int exit_code_from_summary(const SuiteSummary& summary) {
    return summary.failed_tests == 0 ? 0 : 1;
}

}  // namespace cb::test

#define CB_CHECK(ctx, cond) ::cb::test::check_true((ctx), static_cast<bool>(cond), #cond, "", __FILE__, static_cast<uint32_t>(__LINE__))
#define CB_CHECK_MSG(ctx, cond, msg) ::cb::test::check_true((ctx), static_cast<bool>(cond), #cond, (msg), __FILE__, static_cast<uint32_t>(__LINE__))
#define CB_CHECK_FALSE(ctx, cond) ::cb::test::check_false((ctx), static_cast<bool>(cond), #cond, "", __FILE__, static_cast<uint32_t>(__LINE__))
#define CB_CHECK_EQ(ctx, actual, expected) ::cb::test::check_eq((ctx), (actual), (expected), #actual, #expected, __FILE__, static_cast<uint32_t>(__LINE__))
#define CB_CHECK_NE(ctx, actual, expected) ::cb::test::check_ne((ctx), (actual), (expected), #actual, #expected, __FILE__, static_cast<uint32_t>(__LINE__))
#define CB_CHECK_CONTAINS(ctx, haystack, needle) ::cb::test::check_contains((ctx), (haystack), (needle), __FILE__, static_cast<uint32_t>(__LINE__))
#define CB_CHECK_NOT_CONTAINS(ctx, haystack, needle) ::cb::test::check_not_contains((ctx), (haystack), (needle), __FILE__, static_cast<uint32_t>(__LINE__))
#define CB_REQUIRE(ctx, cond) do { const std::size_t _cb_prev_failures = (ctx).failures.size(); ::cb::test::check_true((ctx), static_cast<bool>(cond), #cond, "fatal requirement failed", __FILE__, static_cast<uint32_t>(__LINE__)); if ((ctx).failures.size() > _cb_prev_failures) { (ctx).abort_current_test = true; return; } } while (false)

#endif  // CLEANBASIC_TEST_HARNESS_HPP
