#ifndef CLEANBASIC_STDLIB_OS_BRIDGE_HPP
#define CLEANBASIC_STDLIB_OS_BRIDGE_HPP

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

inline std::string cbOsEnv(const std::string& name) {
    if (name.empty()) {
        return "";
    }
    const char* val = std::getenv(name.c_str());
    if (val != nullptr) {
        return std::string(val);
    }
    return "";
}

inline bool cbOsHasEnv(const std::string& name) {
    if (name.empty()) {
        return false;
    }
    return std::getenv(name.c_str()) != nullptr;
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

inline std::string cbOsExePath() {
#if defined(_WIN32)
    std::array<char, 4096> buf{};
    const DWORD len = GetModuleFileNameA(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (len > 0 && len < buf.size()) {
        std::error_code ec;
        const std::filesystem::path p(std::string(buf.data(), static_cast<size_t>(len)));
        return p.generic_string();
    }
    return "";
#elif defined(__APPLE__)
    std::array<char, 4096> buf{};
    uint32_t size = static_cast<uint32_t>(buf.size());
    if (_NSGetExecutablePath(buf.data(), &size) == 0) {
        std::error_code ec;
        const std::filesystem::path p = std::filesystem::canonical(std::filesystem::path(buf.data()), ec);
        if (!ec) {
            return p.generic_string();
        }
        return std::filesystem::path(buf.data()).generic_string();
    }
    return "";
#else
    std::error_code ec;
    const std::filesystem::path exeP = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec && !exeP.empty()) {
        return exeP.generic_string();
    }
    return "";
#endif
}

#ifdef CLEANBASIC_RUNTIME_PREAMBLE_INCLUDED

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

inline std::tuple<std::string, CbError> cbOsExec(const std::string& cmd) {
    if (cmd.empty()) {
        return {std::string(""), CbError{"Cannot execute empty command", 400}};
    }
#if defined(_WIN32)
    FILE* pipe = _popen(cmd.c_str(), "r");
#else
    FILE* pipe = popen(cmd.c_str(), "r");
#endif
    if (pipe == nullptr) {
        return {std::string(""), CbError{"Failed to start subprocess for command: " + cmd, 1}};
    }
    std::string output;
    char buffer[4096];
    while (true) {
        const size_t bytesRead = std::fread(buffer, 1, sizeof(buffer), pipe);
        if (bytesRead > 0) {
            output.append(buffer, bytesRead);
        }
        if (bytesRead < sizeof(buffer)) {
            break;
        }
    }
#if defined(_WIN32)
    const int rawStatus = _pclose(pipe);
#else
    const int rawStatus = pclose(pipe);
#endif
    if (rawStatus < 0) {
        return {std::string(""), CbError{"Subprocess wait failed for command: " + cmd, 1}};
    }
    int exitCode = rawStatus;
#if !defined(_WIN32)
    if ((rawStatus & 0x7f) == 0) {
        exitCode = (rawStatus >> 8) & 0xff;
    } else {
        exitCode = 128 + (rawStatus & 0x7f);
    }
#endif
    if (exitCode != 0) {
        return {
            std::string(""),
            CbError{
                "Command exited with status " + std::to_string(exitCode) + ": " + cmd,
                static_cast<int64_t>(exitCode)
            }
        };
    }
    return {std::move(output), CbError{"", 0}};
}

inline void cbOsExit(int64_t code) {
    std::exit(static_cast<int>(code));
}

#endif // CLEANBASIC_RUNTIME_PREAMBLE_INCLUDED

#endif // CLEANBASIC_STDLIB_OS_BRIDGE_HPP
