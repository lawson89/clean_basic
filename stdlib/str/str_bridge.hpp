#ifndef CLEANBASIC_STDLIB_STR_BRIDGE_HPP
#define CLEANBASIC_STDLIB_STR_BRIDGE_HPP

#include <cstdint>
#include <limits>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

inline bool cbStrIsAsciiSpaceByte(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

inline bool cbStrIsAsciiDigitByte(char c) {
    return c >= '0' && c <= '9';
}

inline bool cbStrIsAsciiAlphaByte(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

inline unsigned char cbStrAsciiLowerByte(unsigned char c) {
    if (c >= 'A' && c <= 'Z') {
        return static_cast<unsigned char>(c + ('a' - 'A'));
    }
    return c;
}

inline size_t cbStrUtf8SeqLen(const std::string& s, size_t idx) {
    if (idx >= s.size()) {
        return 0;
    }
    const unsigned char b0 = static_cast<unsigned char>(s[idx]);
    if (b0 <= 0x7Fu) {
        return 1;
    }
    if (b0 >= 0xC2u && b0 <= 0xDFu) {
        if (idx + 1 >= s.size()) {
            return 0;
        }
        const unsigned char b1 = static_cast<unsigned char>(s[idx + 1]);
        if ((b1 & 0xC0u) != 0x80u) {
            return 0;
        }
        return 2;
    }
    if (b0 >= 0xE0u && b0 <= 0xEFu) {
        if (idx + 2 >= s.size()) {
            return 0;
        }
        const unsigned char b1 = static_cast<unsigned char>(s[idx + 1]);
        const unsigned char b2 = static_cast<unsigned char>(s[idx + 2]);
        if ((b1 & 0xC0u) != 0x80u || (b2 & 0xC0u) != 0x80u) {
            return 0;
        }
        if (b0 == 0xE0u && b1 < 0xA0u) {
            return 0;
        }
        if (b0 == 0xEDu && b1 >= 0xA0u) {
            return 0;
        }
        return 3;
    }
    if (b0 >= 0xF0u && b0 <= 0xF4u) {
        if (idx + 3 >= s.size()) {
            return 0;
        }
        const unsigned char b1 = static_cast<unsigned char>(s[idx + 1]);
        const unsigned char b2 = static_cast<unsigned char>(s[idx + 2]);
        const unsigned char b3 = static_cast<unsigned char>(s[idx + 3]);
        if ((b1 & 0xC0u) != 0x80u || (b2 & 0xC0u) != 0x80u || (b3 & 0xC0u) != 0x80u) {
            return 0;
        }
        if (b0 == 0xF0u && b1 < 0x90u) {
            return 0;
        }
        if (b0 == 0xF4u && b1 > 0x8Fu) {
            return 0;
        }
        return 4;
    }
    return 0;
}

inline size_t cbStrUtf8Step(const std::string& s, size_t idx) {
    const size_t n = cbStrUtf8SeqLen(s, idx);
    if (n > 0) {
        return n;
    }
    return 1;
}

inline std::vector<size_t> cbStrUtf8Offsets(const std::string& s) {
    std::vector<size_t> offsets;
    offsets.reserve(s.size() + 1);
    size_t i = 0;
    while (i < s.size()) {
        offsets.push_back(i);
        i += cbStrUtf8Step(s, i);
    }
    offsets.push_back(s.size());
    return offsets;
}

inline int64_t cbStrUtf8Len(const std::string& s) {
    size_t i = 0;
    int64_t count = 0;
    while (i < s.size()) {
        i += cbStrUtf8Step(s, i);
        count += 1;
    }
    return count;
}

inline bool cbStrUtf8Valid(const std::string& s) {
    size_t i = 0;
    while (i < s.size()) {
        const size_t n = cbStrUtf8SeqLen(s, i);
        if (n == 0) {
            return false;
        }
        i += n;
    }
    return true;
}

inline std::vector<std::string> cbStrChars(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        const size_t n = cbStrUtf8Step(s, i);
        out.push_back(s.substr(i, n));
        i += n;
    }
    return out;
}

inline bool cbStrAsciiEqualsIgnoreCase(const std::string& s, const std::string& other) {
    if (s.size() != other.size()) {
        return false;
    }
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char a = cbStrAsciiLowerByte(static_cast<unsigned char>(s[i]));
        const unsigned char b = cbStrAsciiLowerByte(static_cast<unsigned char>(other[i]));
        if (a != b) {
            return false;
        }
    }
    return true;
}

inline bool cbStrAsciiStartsWithIgnoreCase(const std::string& s, const std::string& prefix) {
    if (prefix.size() > s.size()) {
        return false;
    }
    for (size_t i = 0; i < prefix.size(); ++i) {
        const unsigned char a = cbStrAsciiLowerByte(static_cast<unsigned char>(s[i]));
        const unsigned char b = cbStrAsciiLowerByte(static_cast<unsigned char>(prefix[i]));
        if (a != b) {
            return false;
        }
    }
    return true;
}

inline bool cbStrAsciiEndsWithIgnoreCase(const std::string& s, const std::string& suffix) {
    if (suffix.size() > s.size()) {
        return false;
    }
    const size_t offset = s.size() - suffix.size();
    for (size_t i = 0; i < suffix.size(); ++i) {
        const unsigned char a = cbStrAsciiLowerByte(static_cast<unsigned char>(s[offset + i]));
        const unsigned char b = cbStrAsciiLowerByte(static_cast<unsigned char>(suffix[i]));
        if (a != b) {
            return false;
        }
    }
    return true;
}

inline int64_t cbStrAsciiFindIgnoreCase(const std::string& s, const std::string& sub) {
    if (sub.empty()) {
        return 0;
    }
    if (sub.size() > s.size()) {
        return -1;
    }
    const unsigned char firstLower = cbStrAsciiLowerByte(static_cast<unsigned char>(sub[0]));
    const size_t limit = s.size() - sub.size();
    for (size_t i = 0; i <= limit; ++i) {
        if (cbStrAsciiLowerByte(static_cast<unsigned char>(s[i])) != firstLower) {
            continue;
        }
        bool matched = true;
        for (size_t j = 1; j < sub.size(); ++j) {
            const unsigned char a = cbStrAsciiLowerByte(static_cast<unsigned char>(s[i + j]));
            const unsigned char b = cbStrAsciiLowerByte(static_cast<unsigned char>(sub[j]));
            if (a != b) {
                matched = false;
                break;
            }
        }
        if (matched) {
            return static_cast<int64_t>(i);
        }
    }
    return -1;
}

inline int64_t cbStrAsciiRfindIgnoreCase(const std::string& s, const std::string& sub) {
    if (sub.empty()) {
        return static_cast<int64_t>(s.size());
    }
    if (sub.size() > s.size()) {
        return -1;
    }
    const unsigned char firstLower = cbStrAsciiLowerByte(static_cast<unsigned char>(sub[0]));
    size_t i = s.size() - sub.size() + 1;
    while (i > 0) {
        i -= 1;
        if (cbStrAsciiLowerByte(static_cast<unsigned char>(s[i])) != firstLower) {
            continue;
        }
        bool matched = true;
        for (size_t j = 1; j < sub.size(); ++j) {
            const unsigned char a = cbStrAsciiLowerByte(static_cast<unsigned char>(s[i + j]));
            const unsigned char b = cbStrAsciiLowerByte(static_cast<unsigned char>(sub[j]));
            if (a != b) {
                matched = false;
                break;
            }
        }
        if (matched) {
            return static_cast<int64_t>(i);
        }
    }
    return -1;
}

inline bool cbStrAsciiContainsIgnoreCase(const std::string& s, const std::string& sub) {
    return cbStrAsciiFindIgnoreCase(s, sub) >= 0;
}

inline bool cbStrContains(const std::string& s, const std::string& sub) {
    if (sub.empty()) {
        return true;
    }
    return s.find(sub) != std::string::npos;
}

inline int64_t cbStrFind(const std::string& s, const std::string& sub) {
    if (sub.empty()) {
        return 0;
    }
    const size_t pos = s.find(sub);
    if (pos == std::string::npos) {
        return -1;
    }
    return static_cast<int64_t>(pos);
}

inline int64_t cbStrRfind(const std::string& s, const std::string& sub) {
    if (sub.empty()) {
        return static_cast<int64_t>(s.size());
    }
    const size_t pos = s.rfind(sub);
    if (pos == std::string::npos) {
        return -1;
    }
    return static_cast<int64_t>(pos);
}

inline int64_t cbStrCount(const std::string& s, const std::string& sub) {
    if (sub.empty() || s.empty()) {
        return 0;
    }
    int64_t occurrences = 0;
    size_t pos = 0;
    while (pos <= s.size()) {
        const size_t found = s.find(sub, pos);
        if (found == std::string::npos) {
            break;
        }
        occurrences += 1;
        pos = found + sub.size();
    }
    return occurrences;
}

inline bool cbStrStartsWith(const std::string& s, const std::string& prefix) {
    if (prefix.size() > s.size()) {
        return false;
    }
    return s.compare(0, prefix.size(), prefix) == 0;
}

inline bool cbStrEndsWith(const std::string& s, const std::string& suffix) {
    if (suffix.size() > s.size()) {
        return false;
    }
    return s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

inline std::vector<std::string> cbStrSplit(const std::string& s, const std::string& sep) {
    if (sep.empty()) {
        return cbStrChars(s);
    }
    std::vector<std::string> out;
    size_t start = 0;
    while (true) {
        const size_t pos = s.find(sep, start);
        if (pos == std::string::npos) {
            out.push_back(s.substr(start));
            break;
        }
        out.push_back(s.substr(start, pos - start));
        start = pos + sep.size();
    }
    return out;
}

inline std::vector<std::string> cbStrFields(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && cbStrIsAsciiSpaceByte(s[i])) {
            i += 1;
        }
        if (i >= s.size()) {
            break;
        }
        const size_t start = i;
        while (i < s.size() && !cbStrIsAsciiSpaceByte(s[i])) {
            i += 1;
        }
        out.push_back(s.substr(start, i - start));
    }
    return out;
}

inline std::vector<std::string> cbStrLines(const std::string& s) {
    std::vector<std::string> out;
    if (s.empty()) {
        return out;
    }
    size_t start = 0;
    while (start <= s.size()) {
        const size_t nl = s.find('\n', start);
        if (nl == std::string::npos) {
            if (start < s.size()) {
                std::string tail = s.substr(start);
                if (!tail.empty() && tail.back() == '\r') {
                    tail.pop_back();
                }
                out.push_back(std::move(tail));
            }
            break;
        }
        std::string line = s.substr(start, nl - start);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        out.push_back(std::move(line));
        start = nl + 1;
        if (start == s.size()) {
            break;
        }
    }
    return out;
}

inline std::string cbStrJoin(const std::vector<std::string>& parts, const std::string& sep) {
    if (parts.empty()) {
        return "";
    }
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) {
            out += sep;
        }
        out += parts[i];
    }
    return out;
}

inline std::string cbStrLtrim(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && cbStrIsAsciiSpaceByte(s[start])) {
        start += 1;
    }
    return s.substr(start);
}

inline std::string cbStrRtrim(const std::string& s) {
    size_t end = s.size();
    while (end > 0 && cbStrIsAsciiSpaceByte(s[end - 1])) {
        end -= 1;
    }
    return s.substr(0, end);
}

inline std::string cbStrTrim(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && cbStrIsAsciiSpaceByte(s[start])) {
        start += 1;
    }
    size_t end = s.size();
    while (end > start && cbStrIsAsciiSpaceByte(s[end - 1])) {
        end -= 1;
    }
    return s.substr(start, end - start);
}

inline bool cbStrUtf8SpanInSet(
    const std::string& s,
    size_t spanStart,
    size_t spanLen,
    const std::vector<std::string>& charSet
) {
    for (size_t i = 0; i < charSet.size(); ++i) {
        const std::string& item = charSet[i];
        if (item.size() == spanLen && s.compare(spanStart, spanLen, item) == 0) {
            return true;
        }
    }
    return false;
}

inline std::string cbStrTrimChars(const std::string& s, const std::string& chars) {
    if (chars.empty() || s.empty()) {
        return s;
    }
    const std::vector<std::string> charSet = cbStrChars(chars);
    const std::vector<size_t> offsets = cbStrUtf8Offsets(s);
    const size_t cpCount = offsets.size() - 1;
    size_t startCp = 0;
    while (startCp < cpCount) {
        const size_t bytePos = offsets[startCp];
        const size_t byteLen = offsets[startCp + 1] - bytePos;
        if (!cbStrUtf8SpanInSet(s, bytePos, byteLen, charSet)) {
            break;
        }
        startCp += 1;
    }
    size_t endCp = cpCount;
    while (endCp > startCp) {
        const size_t bytePos = offsets[endCp - 1];
        const size_t byteLen = offsets[endCp] - bytePos;
        if (!cbStrUtf8SpanInSet(s, bytePos, byteLen, charSet)) {
            break;
        }
        endCp -= 1;
    }
    return s.substr(offsets[startCp], offsets[endCp] - offsets[startCp]);
}

inline std::string cbStrBuildUtf8Pad(int64_t neededCp, const std::string& pad) {
    if (neededCp <= 0) {
        return "";
    }
    std::string padUnit = pad;
    if (padUnit.empty()) {
        padUnit = " ";
    }
    const std::vector<std::string> padChars = cbStrChars(padUnit);
    if (padChars.empty()) {
        return std::string(static_cast<size_t>(neededCp), ' ');
    }
    std::string out;
    int64_t added = 0;
    size_t idx = 0;
    while (added < neededCp) {
        out += padChars[idx];
        added += 1;
        idx += 1;
        if (idx >= padChars.size()) {
            idx = 0;
        }
    }
    return out;
}

inline std::string cbStrPadLeft(const std::string& s, int64_t width, const std::string& pad) {
    const int64_t currentCp = cbStrUtf8Len(s);
    if (width <= 0 || width <= currentCp) {
        return s;
    }
    return cbStrBuildUtf8Pad(width - currentCp, pad) + s;
}

inline std::string cbStrPadRight(const std::string& s, int64_t width, const std::string& pad) {
    const int64_t currentCp = cbStrUtf8Len(s);
    if (width <= 0 || width <= currentCp) {
        return s;
    }
    return s + cbStrBuildUtf8Pad(width - currentCp, pad);
}

inline std::string cbStrSlice(const std::string& s, int64_t start, int64_t stop) {
    const int64_t len = static_cast<int64_t>(s.size());
    int64_t clampedStart = start;
    if (clampedStart < 0) {
        clampedStart = 0;
    }
    if (clampedStart > len) {
        clampedStart = len;
    }
    int64_t clampedStop = stop;
    if (clampedStop < clampedStart) {
        clampedStop = clampedStart;
    }
    if (clampedStop > len) {
        clampedStop = len;
    }
    return s.substr(
        static_cast<size_t>(clampedStart),
        static_cast<size_t>(clampedStop - clampedStart)
    );
}

inline std::string cbStrSliceFrom(const std::string& s, int64_t start) {
    return cbStrSlice(s, start, static_cast<int64_t>(s.size()));
}

inline std::string cbStrLeft(const std::string& s, int64_t n) {
    if (n <= 0 || s.empty()) {
        return "";
    }
    size_t bytePos = 0;
    int64_t count = 0;
    while (bytePos < s.size() && count < n) {
        bytePos += cbStrUtf8Step(s, bytePos);
        count += 1;
    }
    return s.substr(0, bytePos);
}

inline std::string cbStrRight(const std::string& s, int64_t n) {
    if (n <= 0 || s.empty()) {
        return "";
    }
    const std::vector<size_t> offsets = cbStrUtf8Offsets(s);
    const int64_t cpLen = static_cast<int64_t>(offsets.size() - 1);
    if (n >= cpLen) {
        return s;
    }
    const size_t startByte = offsets[static_cast<size_t>(cpLen - n)];
    return s.substr(startByte);
}

inline std::string cbStrMid(const std::string& s, int64_t start, int64_t count) {
    if (count <= 0 || s.empty()) {
        return "";
    }
    int64_t clampedStart = start;
    if (clampedStart < 0) {
        clampedStart = 0;
    }
    size_t bytePos = 0;
    int64_t cpIdx = 0;
    while (bytePos < s.size() && cpIdx < clampedStart) {
        bytePos += cbStrUtf8Step(s, bytePos);
        cpIdx += 1;
    }
    if (bytePos >= s.size()) {
        return "";
    }
    const size_t startByte = bytePos;
    int64_t taken = 0;
    while (bytePos < s.size() && taken < count) {
        bytePos += cbStrUtf8Step(s, bytePos);
        taken += 1;
    }
    return s.substr(startByte, bytePos - startByte);
}

inline std::string cbStrCharAt(const std::string& s, int64_t idx) {
    if (idx < 0 || s.empty()) {
        return "";
    }
    size_t bytePos = 0;
    int64_t cpIdx = 0;
    while (bytePos < s.size() && cpIdx < idx) {
        bytePos += cbStrUtf8Step(s, bytePos);
        cpIdx += 1;
    }
    if (bytePos >= s.size()) {
        return "";
    }
    const size_t step = cbStrUtf8Step(s, bytePos);
    return s.substr(bytePos, step);
}

inline std::string cbStrFromChar(int64_t code) {
    if (code < 0 || code > 0x10FFFF) {
        return "";
    }
    if (code >= 0xD800 && code <= 0xDFFF) {
        return "";
    }
    const uint32_t u = static_cast<uint32_t>(code);
    std::string out;
    if (u <= 0x7Fu) {
        out.push_back(static_cast<char>(u));
        return out;
    }
    if (u <= 0x7FFu) {
        out.push_back(static_cast<char>(0xC0u | ((u >> 6) & 0x1Fu)));
        out.push_back(static_cast<char>(0x80u | (u & 0x3Fu)));
        return out;
    }
    if (u <= 0xFFFFu) {
        out.push_back(static_cast<char>(0xE0u | ((u >> 12) & 0x0Fu)));
        out.push_back(static_cast<char>(0x80u | ((u >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (u & 0x3Fu)));
        return out;
    }
    out.push_back(static_cast<char>(0xF0u | ((u >> 18) & 0x07u)));
    out.push_back(static_cast<char>(0x80u | ((u >> 12) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | ((u >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (u & 0x3Fu)));
    return out;
}

inline std::string cbStrReplace(const std::string& s, const std::string& oldSub, const std::string& newSub) {
    if (oldSub.empty()) {
        return s;
    }
    std::string out;
    size_t start = 0;
    while (true) {
        const size_t pos = s.find(oldSub, start);
        if (pos == std::string::npos) {
            out.append(s, start, std::string::npos);
            break;
        }
        out.append(s, start, pos - start);
        out += newSub;
        start = pos + oldSub.size();
    }
    return out;
}

inline std::string cbStrReplaceFirst(const std::string& s, const std::string& oldSub, const std::string& newSub) {
    if (oldSub.empty()) {
        return s;
    }
    const size_t pos = s.find(oldSub);
    if (pos == std::string::npos) {
        return s;
    }
    std::string out = s.substr(0, pos);
    out += newSub;
    out += s.substr(pos + oldSub.size());
    return out;
}

inline std::string cbStrAsciiLower(const std::string& s) {
    std::string out = s;
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i] >= 'A' && out[i] <= 'Z') {
            out[i] = static_cast<char>(out[i] - 'A' + 'a');
        }
    }
    return out;
}

inline std::string cbStrAsciiUpper(const std::string& s) {
    std::string out = s;
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i] >= 'a' && out[i] <= 'z') {
            out[i] = static_cast<char>(out[i] - 'a' + 'A');
        }
    }
    return out;
}

inline std::string cbStrRepeat(const std::string& s, int64_t count) {
    if (count <= 0 || s.empty()) {
        return "";
    }
    std::string out;
    out.reserve(s.size() * static_cast<size_t>(count));
    for (int64_t i = 0; i < count; ++i) {
        out += s;
    }
    return out;
}

inline std::string cbStrReverse(const std::string& s) {
    if (s.empty()) {
        return "";
    }
    const std::vector<size_t> offsets = cbStrUtf8Offsets(s);
    const size_t cpCount = offsets.size() - 1;
    std::string out;
    out.reserve(s.size());
    for (size_t i = cpCount; i > 0; --i) {
        const size_t start = offsets[i - 1];
        const size_t len = offsets[i] - start;
        out.append(s, start, len);
    }
    return out;
}

inline std::tuple<int64_t, CbError> cbStrToInt(const std::string& s) {
    const std::string trimmed = cbStrTrim(s);
    if (trimmed.empty()) {
        return {0, CbError{"Cannot parse empty string as int", 400}};
    }
    bool isNeg = false;
    size_t i = 0;
    if (trimmed[0] == '+' || trimmed[0] == '-') {
        isNeg = (trimmed[0] == '-');
        i = 1;
    }
    uint64_t base = 10;
    if (i + 2 <= trimmed.size() && trimmed[i] == '0') {
        if (trimmed[i + 1] == 'x' || trimmed[i + 1] == 'X') {
            base = 16;
            i += 2;
        } else if (trimmed[i + 1] == 'b' || trimmed[i + 1] == 'B') {
            base = 2;
            i += 2;
        }
    }
    if (i >= trimmed.size()) {
        return {0, CbError{"Invalid integer literal: '" + s + "'", 400}};
    }
    uint64_t maxMag = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    if (isNeg) {
        maxMag += 1ULL;
    }
    uint64_t acc = 0;
    while (i < trimmed.size()) {
        const char c = trimmed[i];
        uint64_t digit = base;
        if (c >= '0' && c <= '9') {
            digit = static_cast<uint64_t>(c - '0');
        } else if (base == 16 && c >= 'a' && c <= 'f') {
            digit = static_cast<uint64_t>(10 + (c - 'a'));
        } else if (base == 16 && c >= 'A' && c <= 'F') {
            digit = static_cast<uint64_t>(10 + (c - 'A'));
        }
        if (digit >= base) {
            return {0, CbError{"Invalid integer literal: '" + s + "'", 400}};
        }
        if (acc > (maxMag - digit) / base) {
            return {0, CbError{"Integer out of range: '" + s + "'", 400}};
        }
        acc = acc * base + digit;
        i += 1;
    }
    if (isNeg) {
        if (acc == maxMag) {
            return {std::numeric_limits<int64_t>::min(), CbError{"", 0}};
        }
        return {-static_cast<int64_t>(acc), CbError{"", 0}};
    }
    return {static_cast<int64_t>(acc), CbError{"", 0}};
}

inline std::tuple<double, CbError> cbStrToFloat(const std::string& s) {
    const std::string trimmed = cbStrTrim(s);
    if (trimmed.empty()) {
        return {0.0, CbError{"Cannot parse empty string as float", 400}};
    }
    size_t i = 0;
    if (trimmed[0] == '+' || trimmed[0] == '-') {
        i = 1;
    }
    bool sawDigit = false;
    while (i < trimmed.size() && cbStrIsAsciiDigitByte(trimmed[i])) {
        sawDigit = true;
        i += 1;
    }
    if (i < trimmed.size() && trimmed[i] == '.') {
        i += 1;
        while (i < trimmed.size() && cbStrIsAsciiDigitByte(trimmed[i])) {
            sawDigit = true;
            i += 1;
        }
    }
    if (!sawDigit) {
        return {0.0, CbError{"Invalid float literal: '" + s + "'", 400}};
    }
    if (i < trimmed.size() && (trimmed[i] == 'e' || trimmed[i] == 'E')) {
        i += 1;
        if (i < trimmed.size() && (trimmed[i] == '+' || trimmed[i] == '-')) {
            i += 1;
        }
        bool sawExpDigit = false;
        while (i < trimmed.size() && cbStrIsAsciiDigitByte(trimmed[i])) {
            sawExpDigit = true;
            i += 1;
        }
        if (!sawExpDigit) {
            return {0.0, CbError{"Invalid float literal: '" + s + "'", 400}};
        }
    }
    if (i != trimmed.size()) {
        return {0.0, CbError{"Invalid float literal: '" + s + "'", 400}};
    }
    std::istringstream iss(trimmed);
    double val = 0.0;
    iss >> val;
    if (iss.fail() || !iss.eof()) {
        return {0.0, CbError{"Float out of range: '" + s + "'", 400}};
    }
    return {val, CbError{"", 0}};
}

inline std::tuple<bool, CbError> cbStrToBool(const std::string& s) {
    const std::string norm = cbStrAsciiLower(cbStrTrim(s));
    if (norm == "true" || norm == "1") {
        return {true, CbError{"", 0}};
    }
    if (norm == "false" || norm == "0") {
        return {false, CbError{"", 0}};
    }
    return {false, CbError{"Invalid boolean literal: '" + s + "'", 400}};
}

inline bool cbStrAsciiIsDigit(const std::string& s) {
    if (s.empty()) {
        return false;
    }
    for (size_t i = 0; i < s.size(); ++i) {
        if (!cbStrIsAsciiDigitByte(s[i])) {
            return false;
        }
    }
    return true;
}

inline bool cbStrAsciiIsAlpha(const std::string& s) {
    if (s.empty()) {
        return false;
    }
    for (size_t i = 0; i < s.size(); ++i) {
        if (!cbStrIsAsciiAlphaByte(s[i])) {
            return false;
        }
    }
    return true;
}

inline bool cbStrAsciiIsSpace(const std::string& s) {
    if (s.empty()) {
        return false;
    }
    for (size_t i = 0; i < s.size(); ++i) {
        if (!cbStrIsAsciiSpaceByte(s[i])) {
            return false;
        }
    }
    return true;
}

inline bool cbStrAsciiIsAlnum(const std::string& s) {
    if (s.empty()) {
        return false;
    }
    for (size_t i = 0; i < s.size(); ++i) {
        if (!cbStrIsAsciiAlphaByte(s[i]) && !cbStrIsAsciiDigitByte(s[i])) {
            return false;
        }
    }
    return true;
}

inline bool cbStrIsAsciiHexDigitByte(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

inline bool cbStrAsciiIsHexDigit(const std::string& s) {
    if (s.empty()) {
        return false;
    }
    for (size_t i = 0; i < s.size(); ++i) {
        if (!cbStrIsAsciiHexDigitByte(s[i])) {
            return false;
        }
    }
    return true;
}

inline std::string cbStrEscapeCpp(const std::string& s) {
    static const char hexChars[] = "0123456789abcdef";
    const char bs = static_cast<char>(92);
    const char dq = static_cast<char>(34);
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char b = static_cast<unsigned char>(s[i]);
        if (b == 92) {
            out.push_back(bs);
            out.push_back(bs);
        } else if (b == 34) {
            out.push_back(bs);
            out.push_back(dq);
        } else if (b == 10) {
            out.push_back(bs);
            out.push_back('n');
        } else if (b == 13) {
            out.push_back(bs);
            out.push_back('r');
        } else if (b == 9) {
            out.push_back(bs);
            out.push_back('t');
        } else if (b == 0) {
            out.push_back(bs);
            out.push_back('0');
        } else if (b < 0x20 || b == 0x7F) {
            out.push_back(bs);
            out.push_back('x');
            out.push_back(hexChars[(b >> 4) & 0x0F]);
            out.push_back(hexChars[b & 0x0F]);
        } else {
            out.push_back(static_cast<char>(b));
        }
    }
    return out;
}

inline int cbStrHexDigitValue(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }
    if (c >= 'A' && c <= 'F') {
        return 10 + (c - 'A');
    }
    return -1;
}

inline std::tuple<std::string, CbError> cbStrUnescape(const std::string& s) {
    const char bs = static_cast<char>(92);
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] != bs) {
            out.push_back(s[i]);
            i += 1;
            continue;
        }
        if (i + 1 >= s.size()) {
            return {std::string(""), CbError{"Invalid escape sequence: trailing backslash", 400}};
        }
        const char esc = s[i + 1];
        if (esc == bs) {
            out.push_back(bs);
            i += 2;
        } else if (esc == '"') {
            out.push_back('"');
            i += 2;
        } else if (esc == static_cast<char>(39)) {
            out.push_back(static_cast<char>(39));
            i += 2;
        } else if (esc == 'n') {
            out.push_back(static_cast<char>(10));
            i += 2;
        } else if (esc == 'r') {
            out.push_back(static_cast<char>(13));
            i += 2;
        } else if (esc == 't') {
            out.push_back(static_cast<char>(9));
            i += 2;
        } else if (esc == '0') {
            out.push_back(static_cast<char>(0));
            i += 2;
        } else if (esc == 'x') {
            if (i + 3 >= s.size()) {
                return {std::string(""), CbError{"Invalid hex escape sequence: expected two hex digits", 400}};
            }
            const int hi = cbStrHexDigitValue(s[i + 2]);
            const int lo = cbStrHexDigitValue(s[i + 3]);
            if (hi < 0 || lo < 0) {
                return {std::string(""), CbError{"Invalid hex escape sequence: non-hex digit", 400}};
            }
            out.push_back(static_cast<char>((hi << 4) | lo));
            i += 4;
        } else {
            std::string errMsg = "Unknown escape sequence: ";
            errMsg.push_back(bs);
            errMsg.push_back(esc);
            return {std::string(""), CbError{errMsg, 400}};
        }
    }
    return {out, CbError{"", 0}};
}

#endif // CLEANBASIC_STDLIB_STR_BRIDGE_HPP
