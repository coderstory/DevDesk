// src/module_markdown/md_keyboard.cpp
// 编辑器键盘流纯函数实现（接口见 md_keyboard.h）。
// 行号/行首换算复用 md_line_scanner，保证与状态栏「行:列」同一套语义。
#include "md_keyboard.h"

#include "md_line_scanner.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace module::markdown {

namespace {

// 光标所在逻辑行的行首字节偏移（cursor 允许 == text.size()）
size_t lineStartOfCursor(const std::string& text, int cursor) {
    if (cursor <= 0) return 0;
    const auto p = text.rfind('\n', static_cast<size_t>(cursor) - 1);
    return p == std::string::npos ? 0 : p + 1;
}

// ---- 审计 [12] 表格跳格 helpers ----

// 块内一行：[start,end) 不含换行符；pipes = 行内 '|' 绝对偏移（≥2 才成立）；
// separator = 表头分隔行（单元格仅 -/:/空白且至少一个 '-'）
struct TableLine {
    size_t start = 0;
    size_t end = 0;
    std::vector<size_t> pipes;
    bool separator = false;
};

// [start,end) 是表格行（trim 后首尾皆 '|'、≥2 竖线）→ 返回竖线绝对偏移，否则空
std::vector<size_t> tablePipesOf(const std::string& text, size_t start, size_t end) {
    size_t s = start;
    size_t e = end;
    while (s < e && (text[s] == ' ' || text[s] == '\t')) ++s;
    while (e > s && (text[e - 1] == ' ' || text[e - 1] == '\t')) --e;
    if (e - s < 2 || text[s] != '|' || text[e - 1] != '|') return {};
    std::vector<size_t> pipes;
    for (size_t i = s; i < e; ++i) {
        if (text[i] == '|') pipes.push_back(i);
    }
    return pipes.size() >= 2 ? pipes : std::vector<size_t>{};
}

bool isSeparatorLine(const std::string& text, const std::vector<size_t>& pipes) {
    for (size_t k = 0; k + 1 < pipes.size(); ++k) {
        bool anyDash = false;
        for (size_t i = pipes[k] + 1; i < pipes[k + 1]; ++i) {
            const char c = text[i];
            if (c != '-' && c != ':' && c != ' ' && c != '\t') return false;
            if (c == '-') anyDash = true;
        }
        if (!anyDash) return false;  // 全空单元格（|   |）不是分隔行
    }
    return true;
}

// 含 lineStart 行的表格块：上下连续表格行（含分隔行）。lineStart 行非表格行 → 空。
std::vector<TableLine> tableBlockAt(const std::string& text, size_t lineStart) {
    const size_t nl = text.find('\n', lineStart);
    const size_t lineEnd = nl == std::string::npos ? text.size() : nl;
    if (tablePipesOf(text, lineStart, lineEnd).empty()) return {};

    size_t top = lineStart;  // 向上并块
    while (top > 0) {
        if (text[top - 1] != '\n') break;
        size_t prevStart = 0;
        if (top >= 2) {
            const size_t p = text.rfind('\n', top - 2);
            prevStart = p == std::string::npos ? 0 : p + 1;
        }
        if (tablePipesOf(text, prevStart, top - 1).empty()) break;
        top = prevStart;
    }

    std::vector<TableLine> block;  // 自 top 向下收
    size_t cur = top;
    while (true) {
        const size_t e = text.find('\n', cur);
        const size_t endAbs = e == std::string::npos ? text.size() : e;
        auto pipes = tablePipesOf(text, cur, endAbs);
        if (pipes.empty()) break;
        TableLine ln;
        ln.start = cur;
        ln.end = endAbs;
        ln.separator = isSeparatorLine(text, pipes);
        ln.pipes = std::move(pipes);
        block.push_back(std::move(ln));
        if (e == std::string::npos) break;
        cur = e + 1;
    }
    return block;
}

} // namespace

std::string listPrefixOf(const std::string& line) {
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    const char c = i < line.size() ? line[i] : '\0';
    if ((c == '-' || c == '*' || c == '+') && i + 1 < line.size() && line[i + 1] == ' ') {
        return line.substr(0, i + 2);
    }
    size_t d = i;
    while (d < line.size() && line[d] >= '0' && line[d] <= '9') ++d;
    if (d > i && d - i <= 9 && d + 1 < line.size() && line[d] == '.' && line[d + 1] == ' ') {
        return line.substr(0, d + 2);
    }
    return "";
}

std::string nextListPrefix(const std::string& prefix) {
    const size_t dot = prefix.find('.');
    if (dot == std::string::npos || dot + 1 >= prefix.size() || prefix[dot + 1] != ' ') {
        return prefix;  // 无序列表（"- " / "* " / 缩进 + 无序）
    }
    size_t s = 0;
    while (s < dot && (prefix[s] == ' ' || prefix[s] == '\t')) ++s;
    if (s == dot || dot - s > 9) return prefix;  // 非数字前缀
    unsigned long long n = 0;
    for (size_t i = s; i < dot; ++i) n = n * 10 + static_cast<unsigned>(prefix[i] - '0');
    return prefix.substr(0, s) + std::to_string(n + 1) + ". ";
}

bool applyEnterList(EditBuffer& buf) {
    if (buf.selStart != buf.selEnd) return false;  // 选区 Enter 交回内置（删选区+换行）
    const size_t ls = lineStartOfCursor(buf.text, buf.cursor);
    const auto nl = buf.text.find('\n', ls);
    const size_t le = nl == std::string::npos ? buf.text.size() : nl;
    const std::string line = buf.text.substr(ls, le - ls);
    const std::string prefix = listPrefixOf(line);
    if (prefix.empty()) return false;

    const bool emptyItem = line.size() <= prefix.size() ||
                           line.find_first_not_of(" \t", prefix.size()) == std::string::npos;
    if (emptyItem) {
        // 空列表项回车 = 取消前缀退出列表：清掉整行（只剩前缀+空白），不换行
        buf.text.erase(ls, line.size());
        buf.cursor = static_cast<int>(ls);
        buf.selStart = buf.selEnd = buf.cursor;
        return true;
    }
    const std::string insert = "\n" + nextListPrefix(prefix);
    buf.text.insert(static_cast<size_t>(buf.cursor), insert);
    buf.cursor += static_cast<int>(insert.size());
    buf.selStart = buf.selEnd = buf.cursor;
    return true;
}

void applyTab(EditBuffer& buf, bool shift) {
    // spec §5.2：缩进单位 = 4 空格（反缩进每行去 1 个 Tab 或至多 4 个前导空格）
    constexpr int kIndentUnit = 4;
    const int s = std::min(buf.selStart, buf.selEnd);
    const int e = std::max(buf.selStart, buf.selEnd);

    if (!shift && s == e) {
        // 无选区 Tab：插入 4 空格（同打字）
        buf.text.insert(static_cast<size_t>(s), "    ");
        buf.cursor = s + kIndentUnit;
        buf.selStart = buf.selEnd = buf.cursor;
        return;
    }

    // 行级缩进/反缩进：光标（无选区 Shift+Tab）或选区覆盖的所有行
    const int first = byteOffsetToLine(buf.text, static_cast<size_t>(s));
    const int last = (e > s) ? byteOffsetToLine(buf.text, static_cast<size_t>(e - 1)) : first;

    struct Edit {
        size_t pos;  // 原始坐标里的行首
        int len;     // >0 插入字节数；<0 删除 -len 字节
    };
    std::vector<Edit> edits;
    for (int ln = first; ln <= last; ++ln) {
        const size_t ls = lineStartOffset(buf.text, ln);
        if (shift) {
            size_t remove = 0;
            if (ls < buf.text.size() && buf.text[ls] == '\t') {
                remove = 1;
            } else {
                // 至多 kIndentUnit 个前导空格（不足 4 个有几个去几个）
                while (remove < static_cast<size_t>(kIndentUnit) &&
                       ls + remove < buf.text.size() &&
                       buf.text[ls + remove] == ' ') {
                    ++remove;
                }
            }
            if (remove > 0) edits.push_back({ls, -static_cast<int>(remove)});
        } else {
            edits.push_back({ls, kIndentUnit});
        }
    }

    // 从后往前应用（行首原始坐标保持有效）
    for (auto it = edits.rbegin(); it != edits.rend(); ++it) {
        if (it->len > 0) {
            buf.text.insert(it->pos, static_cast<size_t>(it->len), ' ');
        } else {
            buf.text.erase(it->pos, static_cast<size_t>(-it->len));
        }
    }

    // 偏移修正：off 始终按原始坐标比较，shift 累计位移最后一次性应用
    //（终验复审：原实现边比边移，删减钳回行首时会拿已位移的 off 与原始
    // pos 比较，多行反缩进选区端点偏 1）
    //   插入 pos/len：off >= pos → shift+len；删除 pos/n：off >= pos+n →
    //   shift-n，落进被删区间（pos < off < pos+n）→ 钳回行首 pos
    const auto adjust = [&edits](int& offRef) {
        int off = offRef;
        int shift = 0;
        for (const Edit& ed : edits) {
            const int pos = static_cast<int>(ed.pos);
            if (ed.len > 0) {
                if (off >= pos) shift += ed.len;
            } else {
                const int n = -ed.len;
                if (off >= pos + n) {
                    shift -= n;
                } else if (off > pos) {
                    off = pos;
                }
            }
        }
        offRef = off + shift;
    };
    adjust(buf.selStart);
    adjust(buf.selEnd);
    adjust(buf.cursor);
}

void applyWrapAround(EditBuffer& buf, const std::string& marker) {
    const int s = std::min(buf.selStart, buf.selEnd);
    const int e = std::max(buf.selStart, buf.selEnd);
    const int m = static_cast<int>(marker.size());
    // 再按 = 去包裹（unwrap toggle，spec §5.2）：选区两端恰为 marker → 移除
    if (s != e && s - m >= 0 && e + m <= static_cast<int>(buf.text.size()) &&
        buf.text.compare(static_cast<size_t>(s - m), static_cast<size_t>(m),
                         marker) == 0 &&
        buf.text.compare(static_cast<size_t>(e), static_cast<size_t>(m),
                         marker) == 0) {
        buf.text.erase(static_cast<size_t>(e), static_cast<size_t>(m));
        buf.text.erase(static_cast<size_t>(s - m), static_cast<size_t>(m));
        const int ns = s - m;  // 去包裹后原文的新区间 [ns, ne)
        const int ne = e - m;
        if (buf.selStart <= buf.selEnd) {
            buf.selStart = ns;
            buf.selEnd = ne;
        } else {
            buf.selEnd = ns;
            buf.selStart = ne;
        }
        buf.cursor = (buf.cursor == e) ? ne : ns;
        return;
    }
    if (s == e) {
        buf.text.insert(static_cast<size_t>(s), marker + marker);
        buf.cursor = s + m;  // 光标放中间
        buf.selStart = buf.selEnd = buf.cursor;
        return;
    }
    buf.text.insert(static_cast<size_t>(e), marker);
    buf.text.insert(static_cast<size_t>(s), marker);
    const int ns = s + m;  // 被包裹原文的新区间 [ns, ne)
    const int ne = e + m;
    if (buf.selStart <= buf.selEnd) {
        buf.selStart = ns;
        buf.selEnd = ne;
    } else {
        buf.selEnd = ns;
        buf.selStart = ne;
    }
    buf.cursor = (buf.cursor == e) ? ne : ns;
}

void applyLinkSkeleton(EditBuffer& buf) {
    static const std::string kUrl = "链接";
    const int s = std::min(buf.selStart, buf.selEnd);
    const int e = std::max(buf.selStart, buf.selEnd);
    if (s == e) {
        static const std::string kLabel = "文本";
        buf.text.insert(static_cast<size_t>(s), "[" + kLabel + "](" + kUrl + ")");
        buf.selStart = s + 1;  // 选中「文本」占位
        buf.selEnd = s + 1 + static_cast<int>(kLabel.size());
        buf.cursor = buf.selEnd;
        return;
    }
    const std::string label = buf.text.substr(static_cast<size_t>(s), e - s);
    buf.text.replace(static_cast<size_t>(s), static_cast<size_t>(e - s),
                     "[" + label + "](" + kUrl + ")");
    const int urlStart = s + 1 + static_cast<int>(label.size()) + 2;
    buf.selStart = urlStart;  // 选中「链接」占位
    buf.selEnd = urlStart + static_cast<int>(kUrl.size());
    buf.cursor = buf.selEnd;
}

// ==================== 审计 [12]：表格 MVP ====================

std::string buildMarkdownTable(int rows, int cols) {
    if (rows < 1) rows = 1;
    if (cols < 1) cols = 1;
    std::string header = "|";
    std::string sep = "|";
    for (int c = 0; c < cols; ++c) {
        header += "   |";
        sep += "---|";
    }
    std::string out = header + "\n" + sep;  // 表头 + 分隔行
    for (int r = 1; r < rows; ++r) out += "\n" + header;  // rows 含表头行
    return out;
}

void applyInsertTable(EditBuffer& buf, int rows, int cols) {
    const std::string table = buildMarkdownTable(rows, cols);
    const size_t ls = lineStartOfCursor(buf.text, buf.cursor);
    const size_t nl = buf.text.find('\n', ls);
    const size_t le = nl == std::string::npos ? buf.text.size() : nl;
    const std::string line = buf.text.substr(ls, le - ls);
    size_t at = 0;
    if (line.find_first_not_of(" \t") == std::string::npos) {
        buf.text.replace(ls, le - ls, table);   // 空白行：整行替换为表格
        at = ls;
    } else {
        buf.text.insert(le, "\n\n" + table);    // 非空行：行尾插，前置空行隔开段落
        at = le + 2;
    }
    const int cell = static_cast<int>(at) + 2;  // 进表头首格（"| " 之后）
    buf.cursor = cell;
    buf.selStart = buf.selEnd = cell;
}

bool applyTabTable(EditBuffer& buf, bool shift) {
    const size_t ls = lineStartOfCursor(buf.text, buf.cursor);
    const size_t nl = buf.text.find('\n', ls);
    const size_t le = nl == std::string::npos ? buf.text.size() : nl;
    if (tablePipesOf(buf.text, ls, le).empty()) return false;  // 非表格行：交回缩进
    const auto block = tableBlockAt(buf.text, ls);
    if (block.empty()) return false;

    int ri = 0;  // 光标行在块内的行号
    for (int i = 0; i < static_cast<int>(block.size()); ++i) {
        if (static_cast<size_t>(buf.cursor) >= block[i].start &&
            static_cast<size_t>(buf.cursor) <= block[i].end) {
            ri = i;
            break;
        }
    }
    const TableLine& cur = block[ri];
    const int cellCount = static_cast<int>(cur.pipes.size()) - 1;
    // 光标单元格 = 「最后一根严格在其左侧的竖线」右邻格（压在竖线上算左格；
    // 跳格落点 ce 在格内，Shift+Tab 才能反向走回去）；第一格前钳 0、末格后钳末
    int k = -1;
    for (size_t i = 0; i < cur.pipes.size() &&
                      static_cast<size_t>(buf.cursor) > cur.pipes[i]; ++i) {
        k = static_cast<int>(i);
    }
    if (k < 0) k = 0;
    if (k > cellCount - 1) k = cellCount - 1;

    // 块内下一个/上一个非分隔行（环形查找；块内除自身外全分隔行 → 自身回绕）
    const int m = static_cast<int>(block.size());
    auto nextNonSep = [&](int from, bool forward) {
        for (int step = 1; step <= m; ++step) {
            const int i = ((forward ? from + step : from - step) % m + m) % m;
            if (!block[i].separator) return i;
        }
        return -1;
    };

    int nr = ri;
    int nk = k;
    if (!shift) {
        if (!cur.separator && k + 1 < cellCount) {
            nk = k + 1;  // 同行下一格
        } else {
            const int r2 = nextNonSep(ri, true);
            nr = r2 >= 0 ? r2 : ri;
            nk = 0;
        }
    } else {
        if (!cur.separator && k > 0) {
            nk = k - 1;  // 同行上一格
        } else {
            const int r2 = nextNonSep(ri, false);
            nr = r2 >= 0 ? r2 : ri;
            nk = block[nr].pipes.size() >= 2
                     ? static_cast<int>(block[nr].pipes.size()) - 2
                     : 0;
        }
    }

    // 落点：目标格内容成选区（去贴边各一个空格，覆写友好）
    const TableLine& dst = block[nr];
    size_t cs = dst.pipes[nk] + 1;
    size_t ce = dst.pipes[nk + 1];
    if (cs < ce && buf.text[cs] == ' ') ++cs;
    if (ce > cs && buf.text[ce - 1] == ' ') --ce;
    buf.selStart = static_cast<int>(cs);
    buf.selEnd = static_cast<int>(ce);
    buf.cursor = static_cast<int>(ce);
    return true;
}

} // namespace module::markdown
