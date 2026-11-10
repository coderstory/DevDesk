// md_viewer_search.h — 只读查看器纯逻辑助手（F-05 页内搜索状态机 / F-04 标题截断 /
// 空内容判定）。零 UI / 零窗口依赖：md_viewer_window.cpp 消费，devdesk_tests 直驱。
// 契约总纲：
//   - matches 区间口径 = 块源码字节偏移（与渲染核「区间→rect」查询缝同口径，
//     mdResolveBlockRangeRect 直消费，不经换算）。
//   - 状态机 Idle ⇄ Active{query, matches, cur}；Idle 保留 query（再唤起回填，
//     D-2「保留上次关键词」）；清除/Esc 只清 matches 与 cur。
#pragma once

#include <cctype>
#include <cstddef>
#include <string>
#include <vector>

#include "md_block_model.h"

namespace module::markdown {

// 「无命中」哨兵（0/0 时 Enter/Shift+Enter 的返回值：调用方据此无操作——PRD 硬边界）
inline constexpr size_t kViewerSearchNoMatch = static_cast<size_t>(-1);

// 单条命中：block = 块序号（blocks 下标）；begin/end = 块源码内字节偏移
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
inline std::vector<ViewerSearchMatch> viewerSearchFindMatches(
        const std::vector<MdBlock>& blocks, const std::string& docText,
        const std::string& query) {
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
    for (size_t k = 0; k < blocks.size(); ++k) {
        const std::string blockText = docText.substr(
            blocks[k].start, blocks[k].end - blocks[k].start);
        const std::string hay = fold(blockText);
        for (size_t pos = hay.find(needle); pos != std::string::npos;
             pos = hay.find(needle, pos + 1)) {
            out.push_back({k, pos, pos + needle.size()});
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
                                   const std::string& docText) {
    s.query = q;
    s.matches = viewerSearchFindMatches(blocks, docText, q);
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
