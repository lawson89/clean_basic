#ifndef CLEANBASIC_DIAGNOSTICS_HPP
#define CLEANBASIC_DIAGNOSTICS_HPP

#include <cstdint>
#include <sstream>
#include <string>

namespace cb {

struct DiagnosticError {
    std::string kind;
    std::string message;
    std::string file_name = "main.cb";
    uint32_t line = 1;
    uint32_t col = 1;
};

inline DiagnosticError make_error(
    std::string kind,
    std::string message,
    std::string file_name = "main.cb",
    uint32_t line = 1,
    uint32_t col = 1
) {
    DiagnosticError err{};
    err.kind = std::move(kind);
    err.message = std::move(message);
    err.file_name = std::move(file_name);
    err.line = line;
    err.col = col;
    return err;
}

inline std::string format_diagnostic(const DiagnosticError& err) {
    std::ostringstream oss;
    oss << err.file_name << ":" << err.line << ":" << err.col
        << ": " << err.kind << ": " << err.message;
    return oss.str();
}

}  // namespace cb

#endif  // CLEANBASIC_DIAGNOSTICS_HPP
