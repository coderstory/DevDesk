// src/module_markdown/md_line_scanner.cpp
#include "md_line_scanner.h"

#include <algorithm>
#include <string_view>

namespace module::markdown {

namespace {

bool isSpace(char c) { return c == ' ' || c == '\t'; }

// 行首是否是 >=3 个 ` 或 ~ 的围栏；返回围栏字符，非围栏返回 0
char fenceMarker(std::string_view line) {
    if (line.size() < 3) return 0;
    const char c = line[0];
    if (c != '`' && c != '~') return 0;
    size_t n = 0;
    while (n < line.size() && line[n] == c) ++n;
    return n >= 3 ? c : 0;
}

} // namespace

std::vector<MdFenceBlock> scanFences(const std::string& text) {
    std::vector<MdFenceBlock> out;
    bool inFence = false;
    char fenceChar = 0;
    size_t openLineStart = 0;
    size_t contentStart = 0;
    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t eol = text.find('\n', pos);
        size_t end = (eol == std::string::npos) ? text.size() : eol;
        if (end > pos && text[end - 1] == '\r') --end;  // CRLF
        const std::string_view line(text.data() + pos, end - pos);

        const char fence = fenceMarker(line);
        if (fence != 0) {
            // ponytail: 只按围栏字符配对开关，不校验 CommonMark 缩进/长度精确匹配
            if (!inFence) {
                inFence = true;
                fenceChar = fence;
                openLineStart = pos;
                contentStart = (eol == std::string::npos) ? text.size() : eol + 1;
            } else if (fence == fenceChar) {
                inFence = false;
                out.push_back({openLineStart, contentStart, pos});
            }
        }

        if (eol == std::string::npos) break;
        pos = eol + 1;
    }
    if (inFence) {
        out.push_back({openLineStart, contentStart, text.size()});
    }
    return out;
}

std::vector<MdHeading> scanHeadings(const std::string& text) {
    std::vector<MdHeading> out;
    // 围栏语义与 scanFences 同源（一鱼三吃：大纲/高亮/分块共用一套状态机）
    const std::vector<MdFenceBlock> fences = scanFences(text);
    size_t nextFence = 0;
    size_t pos = 0;
    int lineNo = 0;
    while (pos <= text.size()) {
        const size_t eol = text.find('\n', pos);
        size_t end = (eol == std::string::npos) ? text.size() : eol;
        if (end > pos && text[end - 1] == '\r') --end;  // CRLF
        const std::string_view line(text.data() + pos, end - pos);

        // 区间按序且互不重叠：先跳过已结束的围栏，再看行首是否落在内容区间
        while (nextFence < fences.size() &&
               fences[nextFence].contentEnd <= pos) {
            ++nextFence;
        }
        const bool inFence = nextFence < fences.size() &&
                             fences[nextFence].contentStart <= pos;
        if (!inFence && !line.empty() && line[0] == '#') {
            size_t level = 0;
            while (level < line.size() && level < 6 && line[level] == '#') ++level;
            if (level == line.size() || isSpace(line[level])) {
                MdHeading h;
                h.level = static_cast<int>(level);
                h.lineNo = lineNo;
                h.byteOffset = pos;
                size_t t = level;
                while (t < line.size() && isSpace(line[t])) ++t;
                size_t te = line.size();
                while (te > t && isSpace(line[te - 1])) --te;
                h.title.assign(line.substr(t, te - t));
                out.push_back(std::move(h));
            }
        }

        if (eol == std::string::npos) break;
        pos = eol + 1;
        ++lineNo;
    }
    return out;
}

int countLines(const std::string& text) {
    const size_t n = static_cast<size_t>(
        std::count(text.begin(), text.end(), '\n'));
    return static_cast<int>(n) + 1;
}

int byteOffsetToLine(const std::string& text, size_t offset) {
    if (offset > text.size()) offset = text.size();
    int line = 0;
    for (size_t i = 0; i < offset; ++i) {
        if (text[i] == '\n') ++line;
    }
    return line;
}

size_t lineStartOffset(const std::string& text, int lineNo) {
    if (lineNo <= 0) return 0;
    int line = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '\n') continue;
        if (++line == lineNo) return i + 1;
    }
    return text.size();
}

size_t utf8Length(const std::string& text) {
    size_t n = 0;
    for (const unsigned char c : text) {
        if ((c & 0xC0) != 0x80) ++n;
    }
    return n;
}

} // namespace module::markdown
