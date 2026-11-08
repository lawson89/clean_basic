#ifndef CLEANBASIC_STDLIB_OS_BRIDGE_HPP
#define CLEANBASIC_STDLIB_OS_BRIDGE_HPP

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <systemError>
#include <thread>
#include <tuple>
#include <vector>

inline std::vector<std::string> cbOsArgs() {
    return cbRuntimeArgs();
}

inline void cbOsEprint(const std::string& msg) {
    std::cerr << msg << std::endl;
}

inline bool cbOsReadLine(std::string& outLine) {
    outLine.clear();
    if (!std::getline(std::cin, outLine)) {
        return false;
    }
    if (!outLine.empty() && outLine.back() == '\r') {
        outLine.popBack();
    }
    return true;
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
    std::errorCode ec;
    const std::filesystem::path p = std::filesystem::currentPath(ec);
    if (ec) {
        return {std::string(""), CbError{"Failed to get current working directory: " + ec.message(), 500}};
    }
    return {p.genericString(), CbError{"", 0}};
}

inline CbError cbOsChdir(const std::string& path) {
    if (path.empty()) {
        return CbError{"Cannot change directory to empty path", 400};
    }
    std::errorCode ec;
    std::filesystem::currentPath(path, ec);
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

inline int cbOsClock() {
    static const auto startTime = std::chrono::steadyClock::now();
    const auto now = std::chrono::steadyClock::now();
    const auto elapsedMs = std::chrono::durationCast<std::chrono::milliseconds>(now - startTime).count();
    return static_cast<int>(elapsedMs);
}

inline void cbOsSleep(int ms) {
    if (ms <= 0) {
        return;
    }
    std::thisThread::sleepFor(std::chrono::milliseconds(ms));
}

inline int cbOsRun(const std::string& cmd) {
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

inline void cbOsExit(int code) {
    std::exit(code);
}

#endif // CLEANBASIC_STDLIB_OS_BRIDGE_HPP
