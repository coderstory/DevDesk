// src/module_markdown/md_line_scanner.cpp
#include "md_line_scanner.h"

#include <algorithm>
#include <string_view>

namespace module::markdown {

namespace {

bool isSpace(char c) { return c == ' ' || c == '\t'; }

// 围栏候选行：≤3 前导空格 + ≥3 个同 ` / ~ 字符。返回围栏字符、连串长度与
// 连串后的剩余文本（ch=0 = 不是候选行；≥4 空格缩进属缩进代码，非围栏）。
struct FenceInfo {
    char ch = 0;
    size_t run = 0;
    std::string_view rest;
};

FenceInfo fenceLine(std::string_view line) {
    FenceInfo f;
    size_t i = 0;
    while (i < line.size() && line[i] == ' ' && i < 4) ++i;
    if (i >= 4 || i >= line.size()) return f;  // 4+ 空格 = 缩进代码
    const char c = line[i];
    if (c != '`' && c != '~') return f;
    size_t j = i;
    while (j < line.size() && line[j] == c) ++j;
    if (j - i < 3) return f;
    f.ch = c;
    f.run = j - i;
    f.rest = line.substr(j);
    return f;
}

// 闭栏判定（CommonMark/md4c 口径）：同字符、长度 ≥ 开栏、行尾仅空格/制表符。
// 尾随信息串（```closer / ~~~~ x）不是闭栏——md4c 会把整段视作围栏内容，
// 本扫描器与它不一致会让大纲/高亮/分块三处同时漂移（等价测试
// md_block_model_render_equivalence 的回归锚）。
bool closesFence(const FenceInfo& f, char openChar, size_t openRun) {
    if (f.ch != openChar || f.run < openRun) return false;
    for (const char c : f.rest) {
        if (!isSpace(c)) return false;
    }
    return true;
}

} // namespace

std::vector<MdFenceBlock> scanFences(const std::string& text) {
    std::vector<MdFenceBlock> out;
    bool inFence = false;
    char fenceChar = 0;
    size_t openRun = 0;
    size_t openLineStart = 0;
    size_t contentStart = 0;
    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t eol = text.find('\n', pos);
        size_t end = (eol == std::string::npos) ? text.size() : eol;
        if (end > pos && text[end - 1] == '\r') --end;  // CRLF
        const std::string_view line(text.data() + pos, end - pos);

        const FenceInfo f = fenceLine(line);
        if (!inFence) {
            // 开栏合法性（md4c 口径）：` 围栏的 info 串不得含 `（```a`b` 整行
            // 是段落文本）；~ 围栏 info 可含 ~。缩进 ≤3 已在 fenceLine 校验。
            const bool validOpen = f.ch != 0 &&
                                   !(f.ch == '`' && f.rest.find('`') != std::string_view::npos);
            if (validOpen) {
                inFence = true;
                fenceChar = f.ch;
                openRun = f.run;
                openLineStart = pos;
                contentStart = (eol == std::string::npos) ? text.size() : eol + 1;
            }
        } else if (closesFence(f, fenceChar, openRun)) {
            inFence = false;
            out.push_back({openLineStart, contentStart, pos});
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
