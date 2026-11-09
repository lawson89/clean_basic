#ifndef CLEANBASIC_STDLIB_OS_BRIDGE_HPP
#define CLEANBASIC_STDLIB_OS_BRIDGE_HPP

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <vector>

inline std::vector<std::string> cbOsArgs() {
    return cbRuntimeArgs();
}

inline void cbOsPrintRaw(const std::string& text) {
    std::cout << text << std::flush;
}

inline void cbOsEprint(const std::string& msg) {
    std::cerr << msg << std::endl;
}

inline void cbOsEprintRaw(const std::string& text) {
    std::cerr << text << std::flush;
}

inline bool cbOsReadLine(std::string& outLine) {
    outLine.clear();
    if (!std::getline(std::cin, outLine)) {
        return false;
    }
    if (!outLine.empty() && outLine.back() == static_cast<char>(13)) {
        outLine.pop_back();
    }
    return true;
}

inline std::string cbOsReadExact(int64_t count) {
    if (count <= 0) {
        return "";
    }
    std::string buf(static_cast<size_t>(count), static_cast<char>(0));
    std::cin.read(&buf[0], static_cast<std::streamsize>(count));
    const std::streamsize got = std::cin.gcount();
    if (got <= 0) {
        return "";
    }
    buf.resize(static_cast<size_t>(got));
    return buf;
}

inline std::string cbOsReadAll() {
    std::ostringstream ss;
    ss << std::cin.rdbuf();
    return ss.str();
}

inline std::string cbOsEnv(const std::string& name) {
    if (name.empty()) {
        return "";
    }
    if (std::getenv(name.c_str()) != nullptr) {
        return std::string(std::getenv(name.c_str()));
    }
    return "";
}

inline bool cbOsHasEnv(const std::string& name) {
    if (name.empty()) {
        return false;
    }
    return std::getenv(name.c_str()) != nullptr;
}

inline std::tuple<std::string, CbError> cbOsCwd() {
    std::error_code ec;
    const std::filesystem::path p = std::filesystem::current_path(ec);
    if (ec) {
        return {std::string(""), CbError{"Failed to get current working directory: " + ec.message(), 500}};
    }
    return {p.generic_string(), CbError{"", 0}};
}

inline CbError cbOsChdir(const std::string& path) {
    if (path.empty()) {
        return CbError{"Cannot change directory to empty path", 400};
    }
    std::error_code ec;
    std::filesystem::current_path(path, ec);
    if (ec) {
        return CbError{"Failed to change directory to '" + path + "': " + ec.message(), 500};
    }
    return CbError{"", 0};
}

inline std::string cbOsPlatform() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}

inline int64_t cbOsClock() {
    static const auto startTime = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime).count();
    return static_cast<int64_t>(elapsedMs);
}

inline void cbOsSleep(int64_t ms) {
    if (ms <= 0) {
        return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

inline int64_t cbOsRun(const std::string& cmd) {
    const int status = std::system(cmd.c_str());
    if (status < 0) {
        return status;
    }
#if !defined(_WIN32)
    if ((status & 0x7f) == 0) {
        return (status >> 8) & 0xff;
    }
#endif
    return status;
}

inline void cbOsExit(int64_t code) {
    std::exit(static_cast<int>(code));
}

#endif // CLEANBASIC_STDLIB_OS_BRIDGE_HPP
