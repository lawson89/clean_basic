#ifndef CLEANBASIC_STDLIB_FS_BRIDGE_HPP
#define CLEANBASIC_STDLIB_FS_BRIDGE_HPP

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <systemError>
#include <tuple>
#include <utility>
#include <vector>

struct CbFile {
    int handle = -1;
};

struct CbFileSlot {
    std::fstream stream;
    bool inUse = false;
};

inline std::vector<CbFileSlot>& cbFsSlotTable() {
    static std::vector<CbFileSlot> table;
    return table;
}

inline std::mutex& cbFsSlotMutex() {
    static std::mutex mtx;
    return mtx;
}

inline bool cbFsValidSlot(int handle) {
    if (handle < 0) {
        return false;
    }
    auto& tbl = cbFsSlotTable();
    const size_t idx = static_cast<size_t>(handle);
    if (idx >= tbl.size()) {
        return false;
    }
    return tbl[idx].inUse;
}

inline void cbFsStripCr(std::string& line) {
    if (!line.empty() && line.back() == '\r') {
        line.popBack();
    }
}

inline std::tuple<std::string, CbError> cbFsRead(const std::string& path) {
    if (path.empty()) {
        return {std::string(""), CbError{"Cannot read empty file path", 400}};
    }
    std::errorCode ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        return {std::string(""), CbError{"File not found: " + path, 404}};
    }
    if (std::filesystem::isDirectory(path, ec)) {
        return {std::string(""), CbError{"Path is a directory, not a file: " + path, 400}};
    }
    std::ifstream in(path, std::ios::in | std::ios::binary);
    if (!in.isOpen()) {
        return {std::string(""), CbError{"Failed to open file for reading: " + path, 500}};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    if (in.bad()) {
        return {std::string(""), CbError{"I/O error while reading file: " + path, 500}};
    }
    return {ss.str(), CbError{"", 0}};
}

inline std::tuple<std::vector<std::string>, CbError> cbFsLines(const std::string& path) {
    if (path.empty()) {
        return {std::vector<std::string>{}, CbError{"Cannot read lines from empty file path", 400}};
    }
    std::errorCode ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        return {std::vector<std::string>{}, CbError{"File not found: " + path, 404}};
    }
    if (std::filesystem::isDirectory(path, ec)) {
        return {std::vector<std::string>{}, CbError{"Path is a directory, not a file: " + path, 400}};
    }
    std::ifstream in(path, std::ios::in | std::ios::binary);
    if (!in.isOpen()) {
        return {std::vector<std::string>{}, CbError{"Failed to open file for reading lines: " + path, 500}};
    }
    std::vector<std::string> result;
    std::string line;
    while (std::getline(in, line)) {
        cbFsStripCr(line);
        result.push_back(line);
    }
    if (in.bad()) {
        return {std::vector<std::string>{}, CbError{"I/O error while reading lines from file: " + path, 500}};
    }
    return {std::move(result), CbError{"", 0}};
}

inline CbError cbFsWrite(const std::string& path, const std::string& content) {
    if (path.empty()) {
        return CbError{"Cannot write to empty file path", 400};
    }
    std::ofstream out(path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!out.isOpen()) {
        return CbError{"Failed to open file for writing: " + path, 500};
    }
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    out.flush();
    if (!out.good()) {
        return CbError{"Failed to write content to file: " + path, 500};
    }
    return CbError{"", 0};
}

inline CbError cbFsAppend(const std::string& path, const std::string& content) {
    if (path.empty()) {
        return CbError{"Cannot append to empty file path", 400};
    }
    std::ofstream out(path, std::ios::out | std::ios::binary | std::ios::app);
    if (!out.isOpen()) {
        return CbError{"Failed to open file for appending: " + path, 500};
    }
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    out.flush();
    if (!out.good()) {
        return CbError{"Failed to append content to file: " + path, 500};
    }
    return CbError{"", 0};
}

inline CbError cbFsCopy(const std::string& src, const std::string& dst) {
    if (src.empty() || dst.empty()) {
        return CbError{"Source and destination paths cannot be empty", 400};
    }
    std::errorCode ec;
    const bool ok = std::filesystem::copyFile(
        src,
        dst,
        std::filesystem::copyOptions::overwriteExisting,
        ec
    );
    if (!ok || ec) {
        return CbError{"Failed to copy '" + src + "' to '" + dst + "': " + ec.message(), 500};
    }
    return CbError{"", 0};
}

inline std::tuple<CbFile, CbError> cbFsOpenMode(const std::string& path, const std::string& mode) {
    if (path.empty()) {
        return {CbFile{-1}, CbError{"Cannot open empty file path", 400}};
    }
    std::ios_base::openmode openFlags = std::ios::binary;
    if (mode == "r") {
        std::errorCode ec;
        if (!std::filesystem::exists(path, ec) || ec) {
            return {CbFile{-1}, CbError{"File not found: " + path, 404}};
        }
        if (std::filesystem::isDirectory(path, ec)) {
            return {CbFile{-1}, CbError{"Path is a directory, not a file: " + path, 400}};
        }
        openFlags = std::ios::in | std::ios::binary;
    } else if (mode == "w") {
        openFlags = std::ios::out | std::ios::binary | std::ios::trunc;
    } else if (mode == "a") {
        openFlags = std::ios::out | std::ios::binary | std::ios::app;
    } else {
        return {CbFile{-1}, CbError{"Invalid file open mode '" + mode + "'; expected 'r', 'w', or 'a'", 400}};
    }

    std::lockGuard<std::mutex> lock(cbFsSlotMutex());
    auto& tbl = cbFsSlotTable();
    size_t slotIdx = tbl.size();
    for (size_t i = 0; i < tbl.size(); ++i) {
        if (!tbl[i].inUse) {
            slotIdx = i;
            break;
        }
    }
    if (slotIdx == tbl.size()) {
        tbl.emplace_back();
    }

    auto& slot = tbl[slotIdx];
    if (slot.stream.isOpen()) {
        slot.stream.close();
    }
    slot.stream.clear();
    slot.stream.open(path, openFlags);
    if (!slot.stream.isOpen()) {
        slot.inUse = false;
        return {CbFile{-1}, CbError{"Failed to open file: " + path, 500}};
    }
    slot.inUse = true;
    return {CbFile{static_cast<int>(slotIdx)}, CbError{"", 0}};
}

inline std::tuple<CbFile, CbError> cbFsOpen(const std::string& path) {
    return cbFsOpenMode(path, "r");
}

inline std::tuple<bool, CbError> cbFsReadLine(CbFile f, std::string& outLine) {
    if (!cbFsValidSlot(f.handle)) {
        outLine.clear();
        return {false, CbError{"Invalid or closed file handle", 400}};
    }
    auto& s = cbFsSlotTable()[static_cast<size_t>(f.handle)].stream;
    if (!s.isOpen()) {
        outLine.clear();
        return {false, CbError{"File stream is not open", 400}};
    }
    std::string line;
    if (std::getline(s, line)) {
        cbFsStripCr(line);
        outLine = std::move(line);
        return {true, CbError{"", 0}};
    }
    if (s.bad()) {
        outLine.clear();
        return {false, CbError{"I/O error while reading line from file", 500}};
    }
    outLine.clear();
    return {false, CbError{"", 0}};
}

inline bool cbFsEof(CbFile f) {
    if (!cbFsValidSlot(f.handle)) {
        return true;
    }
    auto& s = cbFsSlotTable()[static_cast<size_t>(f.handle)].stream;
    if (!s.isOpen()) {
        return true;
    }
    if (s.eof()) {
        return true;
    }
    return s.peek() == std::charTraits<char>::eof();
}

inline CbError cbFsWriteStream(CbFile f, const std::string& text) {
    if (!cbFsValidSlot(f.handle)) {
        return CbError{"Invalid or closed file handle", 400};
    }
    auto& s = cbFsSlotTable()[static_cast<size_t>(f.handle)].stream;
    if (!s.isOpen()) {
        return CbError{"File stream is not open", 400};
    }
    s.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (s.fail() || s.bad()) {
        return CbError{"Failed to write to file stream", 500};
    }
    return CbError{"", 0};
}

inline CbError cbFsWriteLine(CbFile f, const std::string& text) {
    return cbFsWriteStream(f, text + "\n");
}

inline CbError cbFsFlush(CbFile f) {
    if (!cbFsValidSlot(f.handle)) {
        return CbError{"Invalid or closed file handle", 400};
    }
    auto& s = cbFsSlotTable()[static_cast<size_t>(f.handle)].stream;
    if (!s.isOpen()) {
        return CbError{"File stream is not open", 400};
    }
    s.flush();
    if (s.fail() || s.bad()) {
        return CbError{"Failed to flush file stream", 500};
    }
    return CbError{"", 0};
}

inline bool cbFsIsOpen(CbFile f) {
    if (!cbFsValidSlot(f.handle)) {
        return false;
    }
    return cbFsSlotTable()[static_cast<size_t>(f.handle)].stream.isOpen();
}

inline void cbFsClose(CbFile& f) {
    std::lockGuard<std::mutex> lock(cbFsSlotMutex());
    if (cbFsValidSlot(f.handle)) {
        auto& slot = cbFsSlotTable()[static_cast<size_t>(f.handle)];
        if (slot.stream.isOpen()) {
            slot.stream.close();
        }
        slot.stream.clear();
        slot.inUse = false;
    }
    f.handle = -1;
}

inline bool cbFsExists(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    std::errorCode ec;
    const bool res = std::filesystem::exists(path, ec);
    if (ec) {
        return false;
    }
    return res;
}

inline bool cbFsIsFile(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    std::errorCode ec;
    const bool res = std::filesystem::isRegularFile(path, ec);
    if (ec) {
        return false;
    }
    return res;
}

inline bool cbFsIsDir(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    std::errorCode ec;
    const bool res = std::filesystem::isDirectory(path, ec);
    if (ec) {
        return false;
    }
    return res;
}

inline std::tuple<int, CbError> cbFsSize(const std::string& path) {
    if (path.empty()) {
        return {0, CbError{"Cannot get size of empty path", 400}};
    }
    std::errorCode ec;
    const auto sz = std::filesystem::fileSize(path, ec);
    if (ec) {
        return {0, CbError{"Failed to get file size for '" + path + "': " + ec.message(), 404}};
    }
    return {static_cast<int>(sz), CbError{"", 0}};
}

inline std::tuple<std::vector<std::string>, CbError> cbFsDir(const std::string& path) {
    if (path.empty()) {
        return {std::vector<std::string>{}, CbError{"Cannot list empty directory path", 400}};
    }
    std::errorCode ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        return {std::vector<std::string>{}, CbError{"Directory not found: " + path, 404}};
    }
    if (!std::filesystem::isDirectory(path, ec) || ec) {
        return {std::vector<std::string>{}, CbError{"Path is not a directory: " + path, 400}};
    }
    std::vector<std::string> entries;
    std::filesystem::directoryIterator it(path, ec);
    if (ec) {
        return {std::vector<std::string>{}, CbError{"Failed to iterate directory '" + path + "': " + ec.message(), 500}};
    }
    const std::filesystem::directoryIterator endIt;
    while (it != endIt) {
        entries.push_back(it->path().filename().string());
        it.increment(ec);
        if (ec) {
            return {std::vector<std::string>{}, CbError{"Error while reading directory '" + path + "': " + ec.message(), 500}};
        }
    }
    std::sort(entries.begin(), entries.end());
    return {std::move(entries), CbError{"", 0}};
}

inline CbError cbFsMkdir(const std::string& path) {
    if (path.empty()) {
        return CbError{"Cannot create directory with empty path", 400};
    }
    std::errorCode ec;
    std::filesystem::createDirectories(path, ec);
    if (ec) {
        return CbError{"Failed to create directory '" + path + "': " + ec.message(), 500};
    }
    return CbError{"", 0};
}

inline CbError cbFsRemove(const std::string& path) {
    if (path.empty()) {
        return CbError{"Cannot remove empty path", 400};
    }
    std::errorCode ec;
    std::filesystem::removeAll(path, ec);
    if (ec) {
        return CbError{"Failed to remove path '" + path + "': " + ec.message(), 500};
    }
    return CbError{"", 0};
}

inline std::string cbFsJoinPath(const std::string& a, const std::string& b) {
    if (a.empty()) {
        return b;
    }
    if (b.empty()) {
        return a;
    }
    const std::filesystem::path joined = std::filesystem::path(a) / std::filesystem::path(b);
    return joined.genericString();
}

inline std::string cbFsParentDir(const std::string& path) {
    return std::filesystem::path(path).parentPath().genericString();
}

inline std::string cbFsFileName(const std::string& path) {
    return std::filesystem::path(path).filename().genericString();
}

inline std::string cbFsExt(const std::string& path) {
    return std::filesystem::path(path).extension().genericString();
}

#endif // CLEANBASIC_STDLIB_FS_BRIDGE_HPP
