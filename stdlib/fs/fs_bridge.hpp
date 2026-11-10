#ifndef CLEANBASIC_STDLIB_FS_BRIDGE_HPP
#define CLEANBASIC_STDLIB_FS_BRIDGE_HPP

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

struct CbFile {
    int64_t handle = -1;
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

inline bool cbFsValidSlot(int64_t handle) {
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
        line.pop_back();
    }
}

inline std::tuple<std::vector<uint8_t>, CbError> cbFsReadBytes(const std::string& path) {
    if (path.empty()) {
        return {std::vector<uint8_t>{}, CbError{"Cannot read empty file path", 400}};
    }
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        return {std::vector<uint8_t>{}, CbError{"File not found: " + path, 404}};
    }
    if (std::filesystem::is_directory(path, ec)) {
        return {std::vector<uint8_t>{}, CbError{"Path is a directory, not a file: " + path, 400}};
    }
    std::ifstream in(path, std::ios::in | std::ios::binary);
    if (!in.is_open()) {
        return {std::vector<uint8_t>{}, CbError{"Failed to open file for reading: " + path, 500}};
    }
    in.seekg(0, std::ios::end);
    const std::streamsize size = in.tellg();
    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> buffer;
    if (size > 0) {
        buffer.resize(static_cast<size_t>(size));
        if (!in.read(reinterpret_cast<char*>(buffer.data()), size)) {
            return {std::vector<uint8_t>{}, CbError{"I/O error while reading binary file: " + path, 500}};
        }
    }
    return {buffer, CbError{"", 0}};
}

inline CbError cbFsWriteBytes(const std::string& path, const std::vector<uint8_t>& data) {
    if (path.empty()) {
        return CbError{"Cannot write to empty file path", 400};
    }
    std::ofstream out(path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return CbError{"Failed to open file for writing: " + path, 500};
    }
    if (!data.empty()) {
        out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    }
    out.flush();
    if (!out.good()) {
        return CbError{"Failed to write binary content to file: " + path, 500};
    }
    return CbError{"", 0};
}

inline std::tuple<std::string, CbError> cbFsRead(const std::string& path) {
    if (path.empty()) {
        return {std::string(""), CbError{"Cannot read empty file path", 400}};
    }
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        return {std::string(""), CbError{"File not found: " + path, 404}};
    }
    if (std::filesystem::is_directory(path, ec)) {
        return {std::string(""), CbError{"Path is a directory, not a file: " + path, 400}};
    }
    std::ifstream in(path, std::ios::in | std::ios::binary);
    if (!in.is_open()) {
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
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        return {std::vector<std::string>{}, CbError{"File not found: " + path, 404}};
    }
    if (std::filesystem::is_directory(path, ec)) {
        return {std::vector<std::string>{}, CbError{"Path is a directory, not a file: " + path, 400}};
    }
    std::ifstream in(path, std::ios::in | std::ios::binary);
    if (!in.is_open()) {
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
    if (!out.is_open()) {
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
    if (!out.is_open()) {
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
    std::error_code ec;
    const bool ok = std::filesystem::copy_file(
        src,
        dst,
        std::filesystem::copy_options::overwrite_existing,
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
    if (mode == "r" || mode == "rb") {
        std::error_code ec;
        if (!std::filesystem::exists(path, ec) || ec) {
            return {CbFile{-1}, CbError{"File not found: " + path, 404}};
        }
        if (std::filesystem::is_directory(path, ec)) {
            return {CbFile{-1}, CbError{"Path is a directory, not a file: " + path, 400}};
        }
        openFlags = std::ios::in | std::ios::binary;
    } else if (mode == "w" || mode == "wb") {
        openFlags = std::ios::out | std::ios::binary | std::ios::trunc;
    } else if (mode == "a" || mode == "ab") {
        openFlags = std::ios::out | std::ios::binary | std::ios::app;
    } else {
        return {CbFile{-1}, CbError{"Invalid file open mode '" + mode + "'; expected 'r', 'w', 'a', 'rb', 'wb', or 'ab'", 400}};
    }

    std::lock_guard<std::mutex> lock(cbFsSlotMutex());
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
    if (slot.stream.is_open()) {
        slot.stream.close();
    }
    slot.stream.clear();
    slot.stream.open(path, openFlags);
    if (!slot.stream.is_open()) {
        slot.inUse = false;
        return {CbFile{-1}, CbError{"Failed to open file: " + path, 500}};
    }
    slot.inUse = true;
    return {CbFile{static_cast<int64_t>(slotIdx)}, CbError{"", 0}};
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
    if (!s.is_open()) {
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
    if (!s.is_open()) {
        return true;
    }
    if (s.eof()) {
        return true;
    }
    return s.peek() == std::char_traits<char>::eof();
}

inline CbError cbFsWriteStream(CbFile f, const std::string& text) {
    if (!cbFsValidSlot(f.handle)) {
        return CbError{"Invalid or closed file handle", 400};
    }
    auto& s = cbFsSlotTable()[static_cast<size_t>(f.handle)].stream;
    if (!s.is_open()) {
        return CbError{"File stream is not open", 400};
    }
    s.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (s.fail() || s.bad()) {
        return CbError{"Failed to write to file stream", 500};
    }
    return CbError{"", 0};
}

inline CbError cbFsWriteLine(CbFile f, const std::string& text) {
    return cbFsWriteStream(f, text + std::string(1, static_cast<char>(10)));
}

inline std::tuple<std::vector<uint8_t>, CbError> cbFsReadFileBytes(CbFile f, int64_t count) {
    if (!cbFsValidSlot(f.handle)) {
        return {std::vector<uint8_t>{}, CbError{"Invalid or closed file handle", 400}};
    }
    if (count <= 0) {
        return {std::vector<uint8_t>{}, CbError{"", 0}};
    }
    auto& s = cbFsSlotTable()[static_cast<size_t>(f.handle)].stream;
    if (!s.is_open()) {
        return {std::vector<uint8_t>{}, CbError{"File stream is not open", 400}};
    }
    std::vector<uint8_t> buf(static_cast<size_t>(count));
    s.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(count));
    const std::streamsize bytesRead = s.gcount();
    buf.resize(static_cast<size_t>(bytesRead));
    if (s.bad()) {
        return {std::vector<uint8_t>{}, CbError{"I/O error while reading bytes from stream", 500}};
    }
    return {buf, CbError{"", 0}};
}

inline CbError cbFsWriteFileBytes(CbFile f, const std::vector<uint8_t>& data) {
    if (!cbFsValidSlot(f.handle)) {
        return CbError{"Invalid or closed file handle", 400};
    }
    auto& s = cbFsSlotTable()[static_cast<size_t>(f.handle)].stream;
    if (!s.is_open()) {
        return CbError{"File stream is not open", 400};
    }
    if (!data.empty()) {
        s.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    }
    if (s.fail() || s.bad()) {
        return CbError{"Failed to write binary data to file stream", 500};
    }
    return CbError{"", 0};
}

inline CbError cbFsFlush(CbFile f) {
    if (!cbFsValidSlot(f.handle)) {
        return CbError{"Invalid or closed file handle", 400};
    }
    auto& s = cbFsSlotTable()[static_cast<size_t>(f.handle)].stream;
    if (!s.is_open()) {
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
    return cbFsSlotTable()[static_cast<size_t>(f.handle)].stream.is_open();
}

inline void cbFsClose(CbFile& f) {
    std::lock_guard<std::mutex> lock(cbFsSlotMutex());
    if (cbFsValidSlot(f.handle)) {
        auto& slot = cbFsSlotTable()[static_cast<size_t>(f.handle)];
        if (slot.stream.is_open()) {
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
    std::error_code ec;
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
    std::error_code ec;
    const bool res = std::filesystem::is_regular_file(path, ec);
    if (ec) {
        return false;
    }
    return res;
}

inline bool cbFsIsDir(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    std::error_code ec;
    const bool res = std::filesystem::is_directory(path, ec);
    if (ec) {
        return false;
    }
    return res;
}

inline std::tuple<int64_t, CbError> cbFsSize(const std::string& path) {
    if (path.empty()) {
        return {0, CbError{"Cannot get size of empty path", 400}};
    }
    std::error_code ec;
    const auto sz = std::filesystem::file_size(path, ec);
    if (ec) {
        return {0, CbError{"Failed to get file size for '" + path + "': " + ec.message(), 404}};
    }
    return {static_cast<int64_t>(sz), CbError{"", 0}};
}

inline std::tuple<std::vector<std::string>, CbError> cbFsDir(const std::string& path) {
    if (path.empty()) {
        return {std::vector<std::string>{}, CbError{"Cannot list empty directory path", 400}};
    }
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        return {std::vector<std::string>{}, CbError{"Directory not found: " + path, 404}};
    }
    if (!std::filesystem::is_directory(path, ec) || ec) {
        return {std::vector<std::string>{}, CbError{"Path is not a directory: " + path, 400}};
    }
    std::vector<std::string> entries;
    std::filesystem::directory_iterator it(path, ec);
    if (ec) {
        return {std::vector<std::string>{}, CbError{"Failed to iterate directory '" + path + "': " + ec.message(), 500}};
    }
    const std::filesystem::directory_iterator endIt;
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

inline std::tuple<std::vector<std::string>, CbError> cbFsWalkFiles(const std::string& path) {
    if (path.empty()) {
        return {std::vector<std::string>{}, CbError{"Cannot walk empty path", 400}};
    }
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        return {std::vector<std::string>{}, CbError{"Path not found: " + path, 404}};
    }
    std::vector<std::string> entries;
    for (auto it = std::filesystem::recursive_directory_iterator(path, std::filesystem::directory_options::skip_permission_denied, ec);
         it != std::filesystem::recursive_directory_iterator();
         it.increment(ec)) {
        if (ec) break;
        const auto& entry = *it;
        std::error_code isFileEc;
        if (entry.is_regular_file(isFileEc) && !isFileEc) {
            entries.push_back(entry.path().generic_string());
        }
    }
    if (ec) {
        return {std::vector<std::string>{}, CbError{"Error while walking path '" + path + "': " + ec.message(), 500}};
    }
    std::sort(entries.begin(), entries.end());
    return {std::move(entries), CbError{"", 0}};
}

inline CbError cbFsMkdir(const std::string& path) {
    if (path.empty()) {
        return CbError{"Cannot create directory with empty path", 400};
    }
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec) {
        return CbError{"Failed to create directory '" + path + "': " + ec.message(), 500};
    }
    return CbError{"", 0};
}

inline CbError cbFsRename(const std::string& src, const std::string& dst) {
    if (src.empty() || dst.empty()) {
        return CbError{"Cannot rename with empty source or destination path", 400};
    }
    std::error_code ec;
    if (!std::filesystem::exists(src, ec)) {
        return CbError{"Source path not found: " + src, 404};
    }
    std::filesystem::rename(src, dst, ec);
    if (ec) {
        return CbError{"Failed to rename '" + src + "' to '" + dst + "': " + ec.message(), 500};
    }
    return CbError{"", 0};
}

inline CbError cbFsRemove(const std::string& path) {
    if (path.empty()) {
        return CbError{"Cannot remove empty path", 400};
    }
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    if (ec) {
        return CbError{"Failed to remove path '" + path + "': " + ec.message(), 500};
    }
    return CbError{"", 0};
}

inline std::tuple<int64_t, CbError> cbFsRemoveAll(const std::string& path) {
    if (path.empty()) {
        return {0, CbError{"Cannot remove empty path", 400}};
    }
    std::error_code ec;
    const std::uintmax_t removedCount = std::filesystem::remove_all(path, ec);
    if (ec) {
        return {0, CbError{"Failed to remove path '" + path + "': " + ec.message(), 500}};
    }
    return {static_cast<int64_t>(removedCount), CbError{"", 0}};
}

inline std::tuple<std::string, CbError> cbFsAbsPath(const std::string& path) {
    if (path.empty()) {
        return {std::string(""), CbError{"Cannot resolve absolute path for empty path", 400}};
    }
    std::error_code ec;
    const std::filesystem::path absP = std::filesystem::absolute(path, ec);
    if (ec) {
        return {std::string(""), CbError{"Failed to resolve absolute path for '" + path + "': " + ec.message(), 500}};
    }
    return {absP.lexically_normal().generic_string(), CbError{"", 0}};
}

inline std::tuple<std::string, CbError> cbFsTempDir() {
    std::error_code ec;
    const std::filesystem::path tmpP = std::filesystem::temp_directory_path(ec);
    if (ec) {
        return {std::string(""), CbError{"Failed to get temporary directory path: " + ec.message(), 500}};
    }
    return {tmpP.generic_string(), CbError{"", 0}};
}

inline std::string cbFsJoinPath(const std::string& a, const std::string& b) {
    if (a.empty()) {
        return b;
    }
    if (b.empty()) {
        return a;
    }
    const std::filesystem::path joined = std::filesystem::path(a) / std::filesystem::path(b);
    return joined.generic_string();
}

inline std::string cbFsParentDir(const std::string& path) {
    return std::filesystem::path(path).parent_path().generic_string();
}

inline std::string cbFsFileName(const std::string& path) {
    return std::filesystem::path(path).filename().generic_string();
}

inline std::string cbFsExt(const std::string& path) {
    return std::filesystem::path(path).extension().generic_string();
}

#endif // CLEANBASIC_STDLIB_FS_BRIDGE_HPP
