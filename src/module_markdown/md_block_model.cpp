// src/module_markdown/md_block_model.cpp
// 块级切分器 + 增量对齐（设计说明见 md_block_model.h 头注释）。
//
// 「宁可多并，不可错切」总原则：逐块渲染与整篇渲染的等价性只会被【错切】
// 破坏（切在 md4c 不分块的地方 → 两段各自解析结果 ≠ 整体解析）；【多并】
// 永远安全（md4c 对同一字节区间自己分块，结果与整篇一致）。因此：
//   - 切点白名单 = md4c（GitHub 方言）必然开新块的位置：列 0 围栏起点、
//     空行后（同型/缩进延续的松散列表除外）、ATX 标题行、列 < 内容缩进的
//     bullet / "1." 列表标记、"> " 引用行、空行后的分隔线、空行后的表格段
//   - 歧义位置全部并入当前块：setext 下划线、段落内表格行（各家对表格能否
//     打断段落有分歧）、非 "1." 有序标记的 lazy 延续、列表内容缩进 ≥ 的
//     任何行（嵌套列表/代码块都在列表块内，交给 md4c）
// 围栏配对完全复用 scanFences（与大纲/编辑器高亮同一状态机，一鱼三吃）。
#include "md_block_model.h"

#include "md_line_scanner.h"

#include <algorithm>
#include <string_view>

namespace module::markdown {

namespace {

bool isSpace(char c) { return c == ' ' || c == '\t'; }

// 行体（去掉行尾 \r；[ls, le) 为不含换行的行内容）
std::string_view lineBody(const std::string& text, size_t ls, size_t le) {
    while (le > ls && text[le - 1] == '\r') --le;
    return std::string_view(text).substr(ls, le - ls);
}

size_t skipSpaces(std::string_view s, size_t i) {
    while (i < s.size() && isSpace(s[i])) ++i;
    return i;
}

size_t leadingSpaces(std::string_view s) {
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return i;
}

bool isBlankLine(std::string_view body) {
    for (char c : body) {
        if (!isSpace(c)) return false;
    }
    return true;
}

// ATX 标题：列 0 的 #{1,6} + 空白或行尾（与 scanHeadings / md_highlight 同源）
bool isHeadingLine(std::string_view body) {
    if (body.empty() || body[0] != '#') return false;
    size_t i = 0;
    while (i < body.size() && i < 7 && body[i] == '#') ++i;
    if (i == 0 || i > 6) return false;
    return i == body.size() || isSpace(body[i]);
}

// 引用行：^ {0,3}>
bool isQuoteLine(std::string_view body) {
    const size_t i = skipSpaces(body, 0);
    return i <= 3 && i < body.size() && body[i] == '>';
}

// 列表标记行：^ {0,3}([-*+]|\d{1,9}[.)])(\s|$)。
// ordered = 数字项；marker = bullet 字符（有序记 '.'）；
// markerEnd = 标记结束偏移（用于内容缩进 = leading + marker + 空格数）
bool isListMarker(std::string_view body, bool* ordered, char* marker,
                  size_t* markerEnd) {
    const size_t indent = leadingSpaces(body);
    size_t i = indent;
    if (i > 3) return false;
    if (i < body.size() && (body[i] == '-' || body[i] == '+' || body[i] == '*')) {
        const size_t j = i + 1;
        if (j == body.size() || isSpace(body[j])) {
            if (ordered) *ordered = false;
            if (marker) *marker = body[i];
            size_t k = j;
            while (k < body.size() && isSpace(body[k])) ++k;
            if (markerEnd) *markerEnd = k;
            return true;
        }
        return false;
    }
    const size_t d0 = i;
    while (i < body.size() && i - d0 < 9 && body[i] >= '0' && body[i] <= '9') ++i;
    if (i == d0 || i >= body.size()) return false;
    if (body[i] != '.' && body[i] != ')') return false;
    const size_t j = i + 1;
    if (j == body.size() || isSpace(body[j])) {
        if (ordered) *ordered = true;
        if (marker) *marker = '.';
        size_t k = j;
        while (k < body.size() && isSpace(body[k])) ++k;
        if (markerEnd) *markerEnd = k;
        return true;
    }
    return false;
}

// 有序标记能否【打断】段落/列表外上下文：仅 "1."/"1)"（CommonMark）
bool orderedCanInterrupt(std::string_view body) {
    const size_t i = skipSpaces(body, 0);
    return i < body.size() && body[i] == '1';
}

// 分隔线：^ {0,3}([*_\-])( *\1){2,} *$（同字符 ≥3，允许间隔空格）
bool isDividerLine(std::string_view body) {
    size_t i = skipSpaces(body, 0);
    if (i >= body.size() || i > 3) return false;
    const char mark = body[i];
    if (mark != '*' && mark != '_' && mark != '-') return false;
    size_t count = 0;
    for (; i < body.size(); ++i) {
        if (body[i] == mark) ++count;
        else if (!isSpace(body[i])) return false;
    }
    return count >= 3;
}

// GFM 分隔行：只含 | : - 与空格，且至少一个 - 一个 |
bool isTableDelimiterLine(std::string_view body) {
    bool hasDash = false;
    bool hasPipe = false;
    for (char c : body) {
        if (c == '-') hasDash = true;
        else if (c == '|') hasPipe = true;
        else if (c != ':' && !isSpace(c)) return false;
    }
    return hasDash && hasPipe;
}

// 表体行（保守口径：列 0 硬起 |，与审计[12] 生成语法 / md_highlight 一致）
bool isTableRowLine(std::string_view body) {
    return !body.empty() && body[0] == '|';
}

// ls 行的下一行行首（越过行尾换行；无下一行返回 limit 哨兵）
size_t lineStartAfter(const std::string& text, size_t le, size_t n) {
    if (le >= n) return n;
    return le + 1;
}

// 链接引用定义行：^ {0,3}\[label\]:\s （label 非空；保守口径：结构像即算）。
// refdef 是 md4c 的【全文档上下文】——定义段本身不产出可见块，且跨块引用
// （[foo] 链接在别的块）只有整篇一起解析才能解析。逐块解析会让未定义块
// 把 [foo] 渲染成字面文本 → 与整篇不等价。含 refdef 的文档整篇退化为单块
// （单块 = 原样整篇，恒等价），增量收益归零但渲染正确。宁可漏增量不可错切。
bool isRefdefLine(std::string_view body) {
    const size_t indent = leadingSpaces(body);
    if (indent > 3 || indent >= body.size() || body[indent] != '[') return false;
    const size_t close = body.find(']', indent + 1);
    if (close == std::string_view::npos || close == indent + 1) return false;
    if (close + 1 >= body.size() || body[close + 1] != ':') return false;
    const size_t after = close + 2;
    return after == body.size() || isSpace(body[after]);
}

// 表格起点：| 行 + 下一行是合法分隔行
bool startsTable(const std::string& text, size_t ls, size_t le, size_t n) {
    if (!isTableRowLine(lineBody(text, ls, le))) return false;
    const size_t nl = lineStartAfter(text, le, n);
    if (nl >= n) return false;
    const size_t ne = std::min(text.find('\n', nl), n);
    return isTableDelimiterLine(lineBody(text, nl, ne));
}

// 围栏块末字节：闭栏行行尾（含换行）；未闭合 = 文末。
// （scanFences：contentEnd = 闭栏行行首 或 size()）
size_t fenceBlockEnd(const std::string& text, const MdFenceBlock& f) {
    if (f.contentEnd >= text.size()) return text.size();
    const size_t le = text.find('\n', f.contentEnd);
    return le == std::string::npos ? text.size() : le + 1;
}

struct Run {
    bool active = false;
    MdBlockKind kind = MdBlockKind::Paragraph;
    size_t start = 0;          // 含前导空行归属的起点
    bool listOrdered = false;
    char listMarker = 0;       // bullet 字符（有序存 '.'）
    size_t listContentIndent = 2;  // 列表项内容列（打断判定分界）
};

} // namespace

uint64_t mdBlockHash(std::string_view text) {
    uint64_t h = 1469598103934665603ull;  // FNV-1a 64 offset basis
    for (const char c : text) {
        h ^= static_cast<uint8_t>(c);
        h *= 1099511628211ull;
    }
    return h;
}

const char* mdBlockKindName(MdBlockKind kind) {
    switch (kind) {
        case MdBlockKind::Paragraph: return "段落";
        case MdBlockKind::Heading: return "标题";
        case MdBlockKind::CodeFence: return "代码块";
        case MdBlockKind::Table: return "表格";
        case MdBlockKind::List: return "列表";
        case MdBlockKind::Quote: return "引用";
        case MdBlockKind::Divider: return "分隔线";
    }
    return "?";
}

std::vector<MdBlock> segmentMarkdownBlocks(const std::string& text) {
    std::vector<MdBlock> out;
    const size_t n = text.size();
    if (n == 0) return out;

    // 围栏状态机与大纲/编辑器高亮同源（一鱼三吃第三吃）
    const std::vector<MdFenceBlock> fences = scanFences(text);

    // refdef 检测（围栏外）：命中 → 全文档单块退化（见 isRefdefLine 注释）
    {
        size_t fi = 0;
        for (size_t i = 0; i < n;) {
            const size_t rawLe = text.find('\n', i);
            const size_t le = rawLe == std::string_view::npos ? n : rawLe;
            while (fi < fences.size() && fences[fi].contentEnd <= i) ++fi;
            const bool inFence = fi < fences.size() && fences[fi].contentStart <= i;
            if (!inFence && isRefdefLine(lineBody(text, i, le))) {
                out.push_back(MdBlock{MdBlockKind::Paragraph, 0, n,
                                      mdBlockHash(std::string_view(text)), -1});
                return out;
            }
            if (rawLe == std::string_view::npos) break;
            i = rawLe + 1;
        }
    }

    auto hashOf = [&](size_t start, size_t end) {
        return mdBlockHash(std::string_view(text).substr(start, end - start));
    };
    auto emit = [&](MdBlockKind kind, size_t start, size_t end) {
        if (start >= end) return;
        out.push_back(MdBlock{kind, start, end, hashOf(start, end), -1});
    };
    auto fenceEndAt = [&](size_t ls) -> size_t {
        const auto it = std::find_if(fences.begin(), fences.end(),
                                     [ls](const MdFenceBlock& f) {
                                         return f.openLineStart == ls;
                                     });
        return it == fences.end() ? 0 : fenceBlockEnd(text, *it);
    };

    Run run;
    // 前导空行归属：未终结 run 时挂 firstBlank（仅 List 延迟裁决），
    // run 已闭合时空行段记在 leadBlank（并入下一块头部，md4c 忽略块前
    // 空行 → 渲染无差）。leadBlank==0 用 leadValid 区分。
    size_t firstBlank = 0;
    bool sawBlank = false;      // List 挂起态
    size_t leadBlank = 0;
    bool leadValid = false;
    size_t i = 0;

    auto takeLead = [&](size_t at) {
        const size_t start = leadValid ? leadBlank : at;
        leadValid = false;
        return start;
    };
    auto setLead = [&](size_t at) {
        if (!leadValid) {
            leadBlank = at;
            leadValid = true;
        }
    };
    auto closeRun = [&](size_t end) {
        if (!run.active) return;
        emit(run.kind, run.start, end);
        run.active = false;
    };

    while (i < n) {
        const size_t rawLe = text.find('\n', i);
        const size_t le = rawLe == std::string::npos ? n : rawLe;  // 行体末
        const size_t next = le == n ? n : le + 1;                   // 下一行首
        const std::string_view body = lineBody(text, i, le);

        // 1) 列 0 围栏起点：无条件开块（fence 必打断段落/列表/引用；内部
        //    行不参与分类，i 直接跳过闭栏）
        if (const size_t fe = fenceEndAt(i)) {
            const size_t start = sawBlank ? firstBlank : takeLead(i);
            closeRun(start);
            emit(MdBlockKind::CodeFence, start, fe);
            sawBlank = false;
            i = fe;
            continue;
        }

        // 2) 空行
        if (isBlankLine(body)) {
            if (run.active && run.kind != MdBlockKind::List) {
                // 段落/引用/表格：空行必终结；空行段并入下一块头部
                closeRun(sawBlank ? firstBlank : i);
                setLead(sawBlank ? firstBlank : i);
                sawBlank = false;
            } else if (run.active) {
                if (!sawBlank) {
                    firstBlank = i;
                    sawBlank = true;
                }
            } else {
                setLead(i);
            }
            i = next;
            continue;
        }

        // 3) 内容行
        bool orderedFlag = false;
        char markerChar = 0;
        size_t markerEnd = 0;
        const bool listMarker =
            isListMarker(body, &orderedFlag, &markerChar, &markerEnd);
        const bool heading = isHeadingLine(body);
        const bool quote = isQuoteLine(body);
        const size_t indent = leadingSpaces(body);

        if (run.active) {
            bool absorb = false;
            if (sawBlank) {
                // List 挂起裁决：同型标记 / 缩进 ≥ 内容列的行延续松散列表
                const bool sameKindMarker =
                    listMarker && orderedFlag == run.listOrdered &&
                    (run.listOrdered || markerChar == run.listMarker);
                if (run.kind == MdBlockKind::List &&
                    (sameKindMarker || indent >= run.listContentIndent)) {
                    sawBlank = false;
                    absorb = true;
                } else {
                    emit(run.kind, run.start, firstBlank);
                    run.active = false;
                    setLead(firstBlank);
                    sawBlank = false;
                }
            } else {
                // 紧邻行（无空行）：只有 md4c 必然开新块的构造才打断
                switch (run.kind) {
                    case MdBlockKind::Paragraph:
                        absorb = !(heading || quote ||
                                   (listMarker && (!orderedFlag ||
                                                   orderedCanInterrupt(body))));
                        break;
                    case MdBlockKind::Quote:
                        // 引用延续：> 行与 lazy 文本；标题/可打断列表标记终结
                        absorb = !heading &&
                                 !(listMarker && (!orderedFlag ||
                                                  orderedCanInterrupt(body)));
                        break;
                    case MdBlockKind::List:
                        // 列表项内容缩进以上的任何行属于本列表（嵌套列表/
                        // 代码块，交给 md4c）；以下才看打断：异型且可打断
                        // 标记、标题、引用。同型标记 = 下一项，吸收。
                        absorb = indent >= run.listContentIndent ||
                                 (!heading && !quote &&
                                  !(listMarker && orderedFlag != run.listOrdered &&
                                    (!orderedFlag ||
                                     orderedCanInterrupt(body))));
                        if (absorb && listMarker) {
                            run.listContentIndent = std::max(
                                run.listContentIndent, indent + markerEnd);
                        }
                        break;
                    case MdBlockKind::Table:
                        absorb = isTableRowLine(body);
                        break;
                    default:
                        absorb = false;
                }
            }
            if (absorb) {
                i = next;
                continue;
            }
            closeRun(i);
        }

        // 4) 新块分类（前导空行并入块头）
        const size_t start = takeLead(i);
        if (heading) {
            emit(MdBlockKind::Heading, start, next);  // 自足单行
            i = next;
            continue;
        }
        if (isDividerLine(body)) {
            // 能走到这里说明前一行是空行或自足块（setext 情形已被上游
            // run 吸收）
            emit(MdBlockKind::Divider, start, next);
            i = next;
            continue;
        }
        run.active = true;
        run.start = start;
        if (listMarker) {
            run.kind = MdBlockKind::List;
            run.listOrdered = orderedFlag;
            run.listMarker = markerChar;
            run.listContentIndent = indent + markerEnd;
        } else if (quote) {
            run.kind = MdBlockKind::Quote;
        } else if (startsTable(text, i, le, n)) {
            // 表格仅在独立起点（空行后/文首/自足块后）识别——段落内 | 行
            // 是否开表各实现有分歧，一律按 lazy 文本并入段落保等价
            run.kind = MdBlockKind::Table;
        } else {
            run.kind = MdBlockKind::Paragraph;
        }
        i = next;
    }

    // 收尾：活动 run 吃到文末（尾随空行并入末块保覆盖连续）
    if (run.active) {
        emit(run.kind, run.start, n);
    } else if (!out.empty() && leadValid) {
        out.back().end = n;
        out.back().hash = hashOf(out.back().start, n);
    } else if (out.empty()) {
        emit(MdBlockKind::Paragraph, 0, n);
    }
    return out;
}

MdBlockUpdate MdBlockModel::update(const std::string& text) {
    MdBlockUpdate r;
    if (text == text_) {
        // 零重算。空脏区间统一约定：dirtyFirst > dirtyLast（1,0 哨兵）
        r.dirtyFirst = 1;
        r.dirtyLast = 0;
        return r;
    }
    r.changed = true;

    std::vector<MdBlock> next = segmentMarkdownBlocks(text);

    // (kind, hash) 双端对齐：未跨越变化窗口的块字节相同 → hash 相同 →
    // 前/后缀命中即继承 stableId（块身份跨编辑稳定，供渲染缓存键控；
    // 上游 cachedMarkdownDocument 按 source 串键 LRU，未变块解析零成本）。
    const size_t m = blocks_.size();
    const size_t k = next.size();
    size_t p = 0;
    while (p < m && p < k && blocks_[p].kind == next[p].kind &&
           blocks_[p].hash == next[p].hash) {
        ++p;
    }
    size_t s = 0;
    while (s + p < m && s + p < k &&
           blocks_[m - 1 - s].kind == next[k - 1 - s].kind &&
           blocks_[m - 1 - s].hash == next[k - 1 - s].hash) {
        ++s;
    }
    for (size_t x = 0; x < p; ++x) next[x].stableId = blocks_[x].stableId;
    for (size_t x = 0; x < s; ++x) {
        next[k - 1 - x].stableId = blocks_[m - 1 - x].stableId;
    }
    for (size_t x = p; x + s < k; ++x) {
        next[x].stableId = nextStableId_++;
    }

    r.reused = p + s;
    if (p + s >= k) {
        r.dirtyFirst = 1;  // 无脏块（防御路径），同 1,0 哨兵约定
        r.dirtyLast = 0;
    } else {
        r.dirtyFirst = p;
        r.dirtyLast = k - 1 - s;
    }
    // 增量口径：切块本身恒为 O(n) 行扫描（与既有 scanFences 同量级，
    // 万行 ms 级——md_highlight 性能探针先例）；「编辑只重算受影响块」
    // 的收益在【解析/布局层】：逐块渲染时脏区间外块命中上游缓存、
    // 不重析。连切块也 O(dirty) 的区段重扫留给实测超预算再做。
    r.incremental = false;

    blocks_ = std::move(next);
    text_ = text;
    return r;
}

size_t MdBlockModel::blockIndexAt(size_t offset) const {
    if (blocks_.empty()) return 0;
    size_t lo = 0, hi = blocks_.size() - 1;
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (offset < blocks_[mid].end) hi = mid;
        else lo = mid + 1;
    }
    return lo;
}

} // namespace module::markdown
