// Markdown 围栏块扫描（实现自 module_markdown/md_line_scanner.cpp 原样平移，
// need:10 批次三；语义回归锚 = md_block_model_render_equivalence / md_scanner_*）。
#include "md_fence_scan.h"

#include <string_view>

namespace module::markdown {

namespace {

bool isSpace(char c) { return c == ' ' || c == '\t'; }

// 围栏候选行：≤3 前导空格 + ≥3 个同 ` / ~ 字符；ch=0 = 非候选行（≥4 空格属缩进代码）。
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

// 闭栏判定（CommonMark/md4c 口径）：同字符、长度 ≥ 开栏、行尾仅空白。尾随信息串
//（```closer）不是闭栏——与 md4c 不一致会让大纲/高亮/分块三处同时漂移（回归锚）。
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
            // 开栏合法性（md4c 口径）：` 围栏的 info 串不得含 `；~ 围栏 info 可含 ~。
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

} // namespace module::markdown
