// src/module_markdown/md_preview_highlight.cpp
// 预览代码块高亮纯函数 + 脏块缓存（Phase 3 ③，接口语义见头文件）。
#include "md_preview_highlight.h"

#include "md_keyword_highlight.h"

#include <algorithm>
#include <unordered_set>

namespace module::markdown {

namespace {

bool isFenceSpace(char c) { return c == ' ' || c == '\t'; }

bool colorsEqual(const MdHighlightColors& a, const MdHighlightColors& b) {
    const auto same = [](const eui::Color& x, const eui::Color& y) {
        return x.r == y.r && x.g == y.g && x.b == y.b && x.a == y.a;
    };
    return same(a.marker, b.marker) && same(a.code, b.code) &&
           same(a.link, b.link) && same(a.listMarker, b.listMarker) &&
           same(a.quote, b.quote) && same(a.codeKeyword, b.codeKeyword) &&
           same(a.codeString, b.codeString) &&
           same(a.codeComment, b.codeComment) &&
           same(a.codeNumber, b.codeNumber);
}

// 行尾 \r 剔除，返回剔除后的视图（CRLF 文档：显示与 md4c 的换行语义一致，
// \r 不进 glyph；参数按值传视图，调用方接收返回值才生效）
std::string_view trimTrailingCr(std::string_view line) {
    while (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    return line;
}

} // namespace

MdPreviewCodeHighlight mdPreviewCodeHighlight(std::string_view blockSource,
                                              const MdHighlightColors& colors) {
    MdPreviewCodeHighlight out;
    const std::string src(blockSource);
    const std::vector<MdFenceBlock> fences = scanFences(src);
    // CodeFence 块 = 恰好一个围栏（块切分器与 scanFences 同源保证）；解析不出
    // 单围栏 → 交给调用方回退整块 components::markdown，绝不错画
    if (fences.size() != 1) return out;
    const MdFenceBlock& f = fences[0];
    if (f.openLineStart > f.contentStart || f.contentStart > f.contentEnd ||
        f.contentEnd > src.size()) {
        return out;  // 防御：区间不自洽（理论不可达）
    }

    // 开栏行 → lang：跳过 ≤3 前导空格与 ≥3 围栏字符，info 取首词
    //（md4c MD_LANG 语义；lang 只供词法器选语言，不进显示串——2026-09-24
    //  用户要求代码块渲染不显示语言名标签）
    const size_t openLineEnd = src.find('\n', f.openLineStart);
    std::string_view openLine(src.data() + f.openLineStart,
                              (openLineEnd == std::string::npos
                                   ? src.size()
                                   : openLineEnd) - f.openLineStart);
    openLine = trimTrailingCr(openLine);
    size_t i = 0;
    while (i < openLine.size() && isFenceSpace(openLine[i])) ++i;
    if (i >= openLine.size()) return out;  // 空行不可能开栏（防御）
    const char fenceChar = openLine[i];
    if (fenceChar != '`' && fenceChar != '~') return out;  // 防御
    size_t ticks = 0;
    while (i + ticks < openLine.size() && openLine[i + ticks] == fenceChar) {
        ++ticks;
    }
    std::string_view info = openLine.substr(std::min(i + ticks, openLine.size()));
    size_t s = 0;
    while (s < info.size() && isFenceSpace(info[s])) ++s;
    size_t e = s;
    while (e < info.size() && !isFenceSpace(info[e])) ++e;
    out.lang.assign(info.substr(s, e - s));

    // 显示串：内容行以 \n 连接（无尾换行；lang 标签不显示，见开栏行注释）
    // 内容行先入显示串（不产 run）：受支持语言交给关键字词法器，
    // 未覆盖/空 lang 保持 ③ 的逐行整行 code 单色（与编辑器 token 等价）
    struct LineSpan {
        int start;
        int len;
    };
    std::vector<LineSpan> lineSpans;
    const int contentDispStart = static_cast<int>(out.displayText.size());
    size_t pos = f.contentStart;
    while (pos < f.contentEnd) {
        size_t nl = src.find('\n', pos);
        if (nl == std::string::npos || nl > f.contentEnd) nl = f.contentEnd;
        std::string_view line(src.data() + pos, nl - pos);
        const size_t len = trimTrailingCr(line).size();
        const int lineStart = static_cast<int>(out.displayText.size());
        out.displayText.append(src, pos, len);
        if (len > 0) lineSpans.push_back({lineStart, static_cast<int>(len)});
        pos = (nl < f.contentEnd) ? nl + 1 : f.contentEnd;
        if (pos < f.contentEnd) out.displayText += '\n';
    }
    if (!lineSpans.empty()) {
        const std::string_view content(out.displayText.data() +
                                           contentDispStart,
                                       out.displayText.size() -
                                           static_cast<size_t>(contentDispStart));
        const MdKeywordHighlight kw =
            mdKeywordHighlight(content, out.lang, colors);
        if (kw.supported) {
            // 词法器 runs 坐标 = content 内偏移 → 平移进 displayText 坐标系；
            // 升序不重叠由词法器保证
            out.runs.reserve(out.runs.size() + kw.runs.size());
            for (const eui::TextRun& r : kw.runs) {
                out.runs.push_back({r.byteStart + contentDispStart,
                                    r.byteEnd + contentDispStart,
                                    r.color});
            }
        } else {
            for (const LineSpan& ls : lineSpans) {
                // 内容行整行 code 色（编辑器围栏分支同源语义：整行单色、
                // 无行内识别）
                out.runs.push_back(
                    {ls.start, ls.start + ls.len, colors.code});
            }
        }
    }
    out.ok = true;
    return out;
}

void MdPreviewCodeCache::setColors(const MdHighlightColors& colors) {
    if (colorsSet_ && colorsEqual(colors_, colors)) return;
    if (colorsSet_) entries_.clear();  // 主题切换：旧色条目全部作废
    colors_ = colors;
    colorsSet_ = true;
}

const MdPreviewCodeHighlight* MdPreviewCodeCache::get(int stableId,
                                                      uint64_t hash) const {
    auto it = entries_.find(stableId);
    if (it != entries_.end() && it->second.hash == hash) {
        ++hits_;
        return &it->second.highlight;
    }
    ++misses_;
    return nullptr;
}

void MdPreviewCodeCache::put(int stableId,
                             uint64_t hash,
                             MdPreviewCodeHighlight highlight) {
    entries_[stableId] = Entry{hash, std::move(highlight)};
}

void MdPreviewCodeCache::pruneActive(const std::vector<int>& activeStableIds) {
    std::unordered_set<int> active(activeStableIds.begin(),
                                   activeStableIds.end());
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (active.find(it->first) == active.end()) {
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace module::markdown
