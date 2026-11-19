// src/module_terminal/terminal_search.cpp
// 池#365：终端历史搜索实现。纯逻辑（collectMatchLines / scrollOffsetToCenter）
// 与搜索条 UI（drawTerminalSearchBar）同居本 TU；屏上行拼串复用
// terminal_grid_widget 的 rowSliceText（同口径：cell 序列 → UTF-8）。
#include "terminal_search.h"

#include <algorithm>
#include <cstdint>

#include "terminal_grid_widget.h"
#include "ui_kit/card.h"

#include "core/component_theme.h"

#include <components/input.h>

namespace terminal {

namespace {

// ASCII 大小写折算（locale 无关；非 ASCII 原样保留按位比较）
char asciiLower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool containsCaseInsensitive(const std::string& haystack, const std::string& needle) {
    if (needle.empty() || haystack.size() < needle.size()) return false;
    const size_t n = haystack.size() - needle.size();
    for (size_t i = 0; i <= n; ++i) {
        bool hit = true;
        for (size_t k = 0; k < needle.size(); ++k) {
            if (asciiLower(haystack[i + k]) != asciiLower(needle[k])) {
                hit = false;
                break;
            }
        }
        if (hit) return true;
    }
    return false;
}

} // namespace

std::vector<int> collectMatchLines(
    const std::string& needle, int totalLines, int scrollbackCount,
    const std::function<bool(int, std::string&)>& scrollbackLine,
    const platform::TerminalFrame& frame) {
    std::vector<int> out;
    if (needle.empty() || totalLines <= 0) return out;
    std::string text;
    for (int line = 0; line < totalLines; ++line) {
        text.clear();
        if (line < scrollbackCount) {
            // 历史行：老→新序，取不到（裁剪/压缩）按空串——不中断整表扫描
            if (scrollbackLine) {
                (void)scrollbackLine(line, text);
            }
        } else if (!frame.cells.empty() && frame.cols > 0) {
            const int row = line - scrollbackCount;
            if (row < 0 || row >= frame.rows) continue;
            text = rowSliceText(
                &frame.cells[static_cast<size_t>(row) * frame.cols],
                frame.cols, 0, frame.cols);
        }
        if (containsCaseInsensitive(text, needle)) {
            out.push_back(line);
        }
    }
    return out;
}

int scrollOffsetToCenter(int matchLine, int totalLines, int viewRows, int maxOffset) {
    if (viewRows <= 0 || maxOffset < 0) return 0;
    // 目标顶行 = 命中行上移半个视口（钳到内容顶）；偏移口径与网格换算同式：
    // startLine = max(0, totalLines - viewRows - scrollOffset)
    const int topLine = std::max(0, matchLine - viewRows / 2);
    return std::clamp(totalLines - viewRows - topLine, 0, maxOffset);
}

void drawTerminalSearchBar(eui::Ui& ui, float pageW, float gridTop,
                           const core::theme::Tokens& t,
                           TerminalSearchState& st,
                           const std::function<void()>& onClose) {
    if (!st.open) return;
    constexpr float kBarH = 36.0f;
    constexpr float kInputW = 200.0f;
    constexpr float kCountW = 72.0f;
    constexpr float kCloseW = 28.0f;
    constexpr float kPad = 8.0f;
    const float barW = kPad + kInputW + kPad + kCountW + kCloseW + kPad;
    const float x = std::max(12.0f, pageW - barW - 12.0f);
    const float y = gridTop + 8.0f;

    ui.stack("term.search.bar")
        .position(x, y)
        .size(barW, kBarH)
        .content([&] {
            ui.rect("term.search.bar.bg")
                .size(barW, kBarH)
                .color(ui_kit::cardSurface(t))
                .radius(t.sizes.radius)
                .border(1.0f, t.colors.borderDefault)
                .build();
            components::input(ui, "term.search.input")
                .position(kPad, (kBarH - 28.0f) * 0.5f)
                .size(kInputW, 28.0f)
                .value(st.query)
                .placeholder("搜索历史，Enter 下一处")
                .fontSize(core::theme::scaled(13.0f))
                .theme(core::theme::componentTokens(t))
                .onChange([&st](const std::string& v) {
                    st.query = v;
                    // 查询变 → 命中态作废（下次 Enter 从视口顶行重扫）
                    st.matchLine = -1;
                    st.matchOrdinal = 0;
                    st.matchCount = 0;
                    st.searched = false;
                })
                .onEnter([&st] { st.pendingStep = true; })
                .onCancel(onClose)
                .build();
            // 命中计数：未搜过留空；无匹配/第几处。右对齐省字面拼接宽度重算。
            std::string count;
            if (st.searched && !st.query.empty()) {
                count = st.matchCount > 0
                    ? std::to_string(st.matchOrdinal) + "/" +
                          std::to_string(st.matchCount)
                    : std::string("无匹配");
            }
            ui.text("term.search.count")
                .position(kPad + kInputW + kPad, 0.0f)
                .size(kCountW, kBarH)
                .text(count)
                .fontSize(core::theme::scaled(12.0f))
                .lineHeight(kBarH)
                .horizontalAlign(eui::HorizontalAlign::Right)
                .verticalAlign(eui::VerticalAlign::Center)
                .color(st.searched && st.matchCount == 0 && !st.query.empty()
                           ? t.colors.errorText
                           : t.colors.textTertiary)
                .build();
            // 关闭钮（Esc 同效，走 input onCancel）
            ui.rect("term.search.close.bg")
                .position(kPad + kInputW + kPad + kCountW, (kBarH - kCloseW) * 0.5f)
                .size(kCloseW, kCloseW)
                .radius(6.0f)
                .states(core::Color{0.0f, 0.0f, 0.0f, 0.0f}, t.colors.hover,
                        t.colors.primarySoftBg)
                .onClick(onClose)
                .build();
            ui.text("term.search.close.icon")
                .position(kPad + kInputW + kPad + kCountW, (kBarH - kCloseW) * 0.5f)
                .size(kCloseW, kCloseW)
                .icon(0xF00D)  // fa-xmark
                .fontSize(core::theme::scaled(13.0f))
                .lineHeight(kCloseW)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center)
                .color(t.colors.textTertiary)
                .build();
        })
        .build();
}

} // namespace terminal
