// md_viewer_search.h — 只读查看器纯逻辑助手（F-05 页内搜索状态机 / F-04 标题截断 /
// 空内容判定）。零 UI / 零窗口依赖：md_viewer_window.cpp 消费，devdesk_tests 直驱。
// 契约总纲：
//   - matches 区间口径 = 块【显示文本】码点偏移（bug259-G：渲染核查询缝改在真实
//     inline 布局上反解矩形，域 = MarkdownBuilder::displayTextOf 的显示文本）。
//     显示文本由注入的 displayTextOf 缝供给（查看器传 EUI 真实现；单测传恒等 =
//     块源码本身），本头文件保持零 EUI 依赖。
//   - 状态机 Idle ⇄ Active{query, matches, cur}；Idle 保留 query（再唤起回填，
//     D-2「保留上次关键词」）；清除/Esc 只清 matches 与 cur。
#pragma once

#include <cctype>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "md_block_model.h"

namespace module::markdown {

// 「无命中」哨兵（0/0 时 Enter/Shift+Enter 的返回值：调用方据此无操作——PRD 硬边界）
inline constexpr size_t kViewerSearchNoMatch = static_cast<size_t>(-1);

// 显示文本缝：块 → 渲染显示文本（查看器 = MarkdownBuilder::displayTextOf；
// 单测缺省恒等映射 = 块源码）。
using ViewerDisplayTextFn =
    std::function<std::string(const std::string& blockText)>;

// 单条命中：block = 块序号（blocks 下标）；begin/end = 块显示文本内码点偏移
//（非全文档偏移——pendingRangeQuery 直返、渲染核同口径解析）。
struct ViewerSearchMatch {
    size_t block = 0;
    size_t begin = 0;
    size_t end = 0;
};

enum class ViewerSearchPhase { Idle, Active };

// 搜索状态机本体（ViewerSession 持有；纯值语义，无句柄无回调）。
struct ViewerSearchState {
    ViewerSearchPhase phase = ViewerSearchPhase::Idle;
    std::string query;            // 当前词；Idle 仍保留（再唤起回填，D-2）
    bool everActivated = false;   // 曾开过（C1「保留上次关键词」的判据面）
    std::vector<ViewerSearchMatch> matches;
    size_t cur = 0;               // 当前命中下标（matches 空时无意义）
    bool hasFocusPending = false; // 唤起后请求输入框聚焦（compose 侧转 ui.requestFocus）
};

// 大小写不敏感子串全匹配（纯文本，无正则无网络，PRD §八）。大小写折叠仅对
// ASCII 字节（<0x80）生效：UTF-8 多字节序列按原样字节比对——中文/全角无大小写，
// 命中语义与浏览器页内查找一致。空 query → 空结果（唤起即有焦点无词 = 0/0）。
// 匹配域 = displayTextOf(blockText)（bug259-G 显示域；缺省恒等 = 源码域，单测用）。
inline std::vector<ViewerSearchMatch> viewerSearchFindMatches(
        const std::vector<MdBlock>& blocks, const std::string& docText,
        const std::string& query,
        const ViewerDisplayTextFn& displayTextOf = {}) {
    std::vector<ViewerSearchMatch> out;
    if (query.empty()) return out;
    const auto fold = [](const std::string& s) {
        std::string r = s;
        for (char& c : r) {
            const unsigned char u = static_cast<unsigned char>(c);
            if (u < 0x80) c = static_cast<char>(std::tolower(u));
        }
        return r;
    };
    const std::string needle = fold(query);
    if (needle.empty()) return out;
    // std::string::find 返回【UTF-8 字节偏移】，而渲染缝 hitRectsForDisplayRange
    // 按【码点】解析区间——bug259-G/H 根因：CJK 文档字节偏移 = 3×码点偏移，字节
    // 直当码点用会让区间整段越过 run → 矩形为空（高亮消失）或错位。fold 只折叠
    // ASCII（1 字节↔1 字节），折叠前后码点序不变，故字节→码点换算在折叠串上做
    // 一次即可。步进语义与 EUI countCodepoints 一致（复核 P3：lead byte 步进
    // 不钳位，截断/非法尾序列同为 1 码点，两侧计数逐位可比）。
    const auto byteToCp = [](const std::string& s, size_t bytePos) {
        size_t cp = 0;
        size_t i = 0;
        while (i < bytePos && i < s.size()) {
            const unsigned char c = static_cast<unsigned char>(s[i]);
            const size_t step =
                c >= 0xF0U ? 4 : c >= 0xE0U ? 3 : c >= 0xC0U ? 2 : 1;
            i += step;
            ++cp;
        }
        return cp;
    };
    const size_t needleCps = byteToCp(needle, needle.size());
    for (size_t k = 0; k < blocks.size(); ++k) {
        const std::string blockText = docText.substr(
            blocks[k].start, blocks[k].end - blocks[k].start);
        const std::string hay = fold(displayTextOf ? displayTextOf(blockText)
                                                   : blockText);
        for (size_t pos = hay.find(needle); pos != std::string::npos;
             pos = hay.find(needle, pos + 1)) {
            const size_t cpBegin = byteToCp(hay, pos);
            out.push_back({k, cpBegin, cpBegin + needleCps});
        }
    }
    return out;
}

// 唤起（Ctrl/Cmd+F）：Idle→Active；已 Active 再按 = 保留词重聚焦（C1 重跑语义）。
// 不动 matches/cur：由调用方以 viewerSearchApplyQuery 配当前词刷新。
inline void viewerSearchActivate(ViewerSearchState& s) {
    s.phase = ViewerSearchPhase::Active;
    s.everActivated = true;
    s.hasFocusPending = true;  // C1：光标已在输入框
}

// 输入（输入框 onChange / 探针注入缝同一通路）：重算全部命中 + 计数归 1（cur=0）。
inline void viewerSearchApplyQuery(ViewerSearchState& s, const std::string& q,
                                   const std::vector<MdBlock>& blocks,
                                   const std::string& docText,
                                   const ViewerDisplayTextFn& displayTextOf = {}) {
    s.query = q;
    s.matches = viewerSearchFindMatches(blocks, docText, q, displayTextOf);
    s.cur = 0;
}

// Enter 下一处；首尾回绕按浏览器惯例（末处 Enter 回到第一处）。0/0 → 无操作
//（返回 kViewerSearchNoMatch，cur 不动——PRD F-05 硬边界）。
inline size_t viewerSearchNext(ViewerSearchState& s) {
    if (s.matches.empty()) return kViewerSearchNoMatch;
    s.cur = (s.cur + 1) % s.matches.size();
    return s.cur;
}

// Shift+Enter 上一处；首尾回绕同上。0/0 → 无操作。
inline size_t viewerSearchPrev(ViewerSearchState& s) {
    if (s.matches.empty()) return kViewerSearchNoMatch;
    s.cur = (s.cur + s.matches.size() - 1) % s.matches.size();
    return s.cur;
}

// 清除（Esc / 点 ×）：高亮消失、搜索条收起（phase=Idle），回到阅读态。
// query / everActivated 刻意保留——再唤起回填关键词（D-2）。
inline void viewerSearchClear(ViewerSearchState& s) {
    s.phase = ViewerSearchPhase::Idle;
    s.matches.clear();
    s.cur = 0;
}

// ── F-04 标题与内容纯助手 ─────────────────────────────────────────────

// 窗口标题 = 文件名含扩展名（D-3：路径永不出现在标题）；超长中段省略号截断
//（保头尾，头 2/3 尾 1/3），截断点回退到 UTF-8 码点边界（不切续字节序列）。
inline std::string mdViewerWindowTitle(const std::string& path,
                                       size_t maxBytes = 96) {
    const size_t sep = path.find_last_of("/\\");
    const std::string name =
        sep == std::string::npos ? path : path.substr(sep + 1);
    if (name.size() <= maxBytes) return name;
    const std::string ellipsis = "…";  // 3 字节
    const size_t keep = maxBytes > ellipsis.size() + 1
                            ? maxBytes - ellipsis.size()
                            : 1;
    const size_t head = keep - keep / 3;
    const size_t tail = keep - head;
    const auto snapToCodepoint = [&name](size_t pos) {
        while (pos > 0 &&
               (static_cast<unsigned char>(name[pos]) & 0xC0) == 0x80) {
            --pos;
        }
        return pos;
    };
    const size_t headEnd = snapToCodepoint(head);
    const size_t tailBegin =
        snapToCodepoint(name.size() > tail ? name.size() - tail : 0);
    if (headEnd == 0 || tailBegin <= headEnd) return name.substr(0, maxBytes);
    return name.substr(0, headEnd) + ellipsis + name.substr(tailBegin);
}

// 空文件判定（PRD §八渲染区「空」态）：0 字节或全空白（ASCII 空白折叠；
// ≥0x80 视为内容字节——CJK 全角空格等交由渲染层自然呈现空排版）。
inline bool mdViewerContentBlank(const std::string& text) {
    for (const char c : text) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u >= 0x80 || std::isspace(u) == 0) return false;
    }
    return true;
}

} // namespace module::markdown
