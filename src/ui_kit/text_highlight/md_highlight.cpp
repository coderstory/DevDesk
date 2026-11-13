// 行级 Markdown 词法高亮（styled-runs）。
// 边界：行级扫描器非完整 CommonMark——跨行强调等边角会漏标（漏标回默认色，优于错标）；
// 预览侧仍走 md4c 保证渲染正确。
#include "md_highlight.h"

#include "core/theme.h"

#include <algorithm>
#include <functional>
#include <string_view>

namespace module::markdown {

MdHighlightColors mdHighlightColors(const core::theme::Tokens& tokens) {
    return MdHighlightColors{
        tokens.colors.textTertiary,   // marker
        tokens.colors.success,        // code
        tokens.colors.info,           // link / italic
        tokens.colors.primaryText,    // listMarker
        tokens.colors.textSecondary,  // quote
        tokens.colors.info,           // codeKeyword
        tokens.colors.warning,        // codeString
        tokens.colors.textTertiary,   // codeComment
        tokens.colors.primaryText,    // codeNumber
    };
}

namespace {

bool isSpace(char c) { return c == ' ' || c == '\t'; }

// 从 from 找一串恰好 ticks 个的反引号（同长度配对，支持 ``code``）；找不到 npos
size_t findTickRun(std::string_view line, size_t from, size_t ticks) {
    size_t j = from;
    while (j < line.size()) {
        if (line[j] != '`') {
            ++j;
            continue;
        }
        size_t k = j;
        while (k < line.size() && line[k] == '`') ++k;
        if (k - j == ticks) return j;
        j = k;
    }
    return std::string_view::npos;
}

// 找孤立单 *（不与相邻 * 组成 **），斜体闭标记；找不到 npos
size_t findSingleStar(std::string_view line, size_t from) {
    for (size_t j = from; j < line.size(); ++j) {
        if (line[j] != '*') continue;
        const bool nextStar = j + 1 < line.size() && line[j + 1] == '*';
        const bool prevStar = j > 0 && line[j - 1] == '*';
        if (!nextStar && !prevStar) return j;
    }
    return std::string_view::npos;
}

// 行内 token 扫描：`code` / **bold** / *italic* / [text](url) / 表格管道；from = 块级标记之后的扫描起点。
void scanInline(std::string_view line, size_t from,
                const MdHighlightColors& colors,
                const std::function<void(size_t, size_t, const eui::Color&)>& push,
                bool tableLine) {
    const size_t n = line.size();
    size_t i = from;
    while (i < n) {
        const char c = line[i];
        if (tableLine && c == '|') {
            push(i, i + 1, colors.marker);
            ++i;
        } else if (c == '`') {
            size_t ticks = 0;
            while (i + ticks < n && line[i + ticks] == '`') ++ticks;
            const size_t close = findTickRun(line, i + ticks, ticks);
            if (close != std::string_view::npos) {
                // 整段（含反引号）代码色（`std::string` 整段 #788C5D）
                push(i, close + ticks, colors.code);
                i = close + ticks;
            } else {
                i += ticks;  // 未闭合：不标，跳过开标记
            }
        } else if (c == '*') {
            if (i + 1 < n && line[i + 1] == '*') {
                // **bold**：标记弱化；内容在 weight run 前保持正文色
                const size_t close = line.find("**", i + 2);
                if (close != std::string_view::npos) {
                    push(i, i + 2, colors.marker);
                    push(close, close + 2, colors.marker);
                    i = close + 2;
                } else {
                    i += 2;
                }
            } else {
                // *italic*：无 italic 字形，内容用 info 色替代
                const size_t close = findSingleStar(line, i + 1);
                if (close != std::string_view::npos) {
                    push(i, i + 1, colors.marker);
                    push(i + 1, close, colors.link);
                    push(close, close + 1, colors.marker);
                    i = close + 1;
                } else {
                    ++i;
                }
            }
        } else if (c == '[' || (c == '!' && i + 1 < n && line[i + 1] == '[')) {
            const size_t bracket = (c == '[') ? i : i + 1;
            const size_t close = line.find(']', bracket + 1);
            if (close != std::string_view::npos && close + 1 < n &&
                line[close + 1] == '(') {
                const size_t paren = line.find(')', close + 2);
                if (paren != std::string_view::npos) {
                    push(i, i + 1, colors.marker);  // [ 或 !
                    if (c == '!') {
                        push(bracket, bracket + 1, colors.marker);
                    }
                    push(bracket + 1, close, colors.link);  // 链接文字
                    push(close, paren + 1, colors.marker);  // ](url)
                    i = paren + 1;
                    continue;
                }
            }
            ++i;  // 不是链接骨架：按普通字符推进（[ 也可能是无效标记）
        } else {
            ++i;
        }
    }
}

} // namespace

void MdHighlighter::ensureFences(const std::string& fullText) {
    // 全文本等值比较实测 ~1ms 级（见 md_highlight_perf 探针）
    if (fullText == fenceText_) return;
    fenceText_ = fullText;
    fences_ = scanFences(fullText);
}

std::vector<eui::TextRun> MdHighlighter::runsForLine(const std::string& fullText,
                                                     const std::string& lineText,
                                                     int lineStartByte) {
    ensureFences(fullText);
    std::vector<eui::TextRun> runs;
    if (lineStartByte < 0 ||
        static_cast<size_t>(lineStartByte) > fullText.size()) {
        return runs;  // 防御：越界偏移不产 run（框架坐标应恒有效）
    }
    const int base = lineStartByte;
    const auto push = [&](size_t from, size_t to, const eui::Color& color) {
        if (from < to) {
            runs.push_back({base + static_cast<int>(from),
                            base + static_cast<int>(to), color});
        }
    };

    const size_t n = lineText.size();
    const size_t off = static_cast<size_t>(lineStartByte);

    // 1) 围栏行 / 围栏内容行：整行单色，不做行内识别
    for (const MdFenceBlock& f : fences_) {
        if (off == f.openLineStart || off == f.contentEnd) {
            push(0, n, colors_.marker);   // ```lang 开/闭栏行
            return runs;
        }
        if (f.contentStart <= off && off < f.contentEnd) {
            push(0, n, colors_.code);     // 围栏内容（v1 统一 codeText 色）
            return runs;
        }
    }

    // 块级前缀仅硬行首触发（软换行段不识别，避免段中恰以 "# " 开头的段被误判为标题）。
    const bool hardStart =
        lineStartByte == 0 || fullText[lineStartByte - 1] == '\n' ||
        (lineStartByte >= 2 && fullText[lineStartByte - 1] == '\r' &&
         fullText[lineStartByte - 2] == '\n');
    size_t scanFrom = 0;  // 行内扫描起点（跳过块级标记，避免 * 列表符被当斜体开标记）
    if (hardStart && n > 0) {
        // 标题前缀 ^#{1,6}(\s|$)：标记弱化，标题文字保持正文色
        size_t level = 0;
        while (level < n && level < 6 && lineText[level] == '#') ++level;
        if (level > 0 && (level == n || isSpace(lineText[level]))) {
            push(0, level, colors_.marker);
            scanFrom = level;
        } else if (lineText[0] == '>') {
            // 引用 ^>+：标记弱化 + 正文 textSecondary；行内 token 后写覆盖叠加
            size_t marks = 0;
            while (marks < n && lineText[marks] == '>') ++marks;
            push(0, marks, colors_.marker);
            push(marks, n, colors_.quote);
            scanFrom = marks;
        } else {
            // 列表标记 ^\s*([-*+]|\d+[.)])(\s|$)：符号 primary（含前导缩进后定位）
            size_t i = 0;
            while (i < n && isSpace(lineText[i])) ++i;
            size_t markerEnd = 0;
            if (i < n) {
                const char c = lineText[i];
                if ((c == '-' || c == '+' || c == '*') &&
                    (i + 1 == n || isSpace(lineText[i + 1]))) {
                    markerEnd = i + 1;
                } else if (c >= '0' && c <= '9') {
                    size_t d = i;
                    while (d < n && lineText[d] >= '0' && lineText[d] <= '9') ++d;
                    if (d < n && (lineText[d] == '.' || lineText[d] == ')') &&
                        (d + 1 == n || isSpace(lineText[d + 1]))) {
                        markerEnd = d + 1;
                    }
                }
            }
            if (markerEnd > 0) {
                push(i, markerEnd, colors_.listMarker);
                scanFrom = markerEnd;
            }
        }
    }

    // 行内 token（表格行 = 硬行首以 | 开头；无前导管道的 GFM 表头不识别，只认 | 开头）。
    const bool tableLine = hardStart && n > 0 && lineText[0] == '|';
    scanInline(lineText, scanFrom, colors_, push, tableLine);
    return runs;
}

} // namespace module::markdown
