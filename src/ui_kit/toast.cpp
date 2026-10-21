// src/ui_kit/toast.cpp — toast 绘制接线的唯一实现（边界见 toast.h）。
//
// UAT6 内容驱动尺寸：宽 = min(内容宽+边距, 360 上限)、高 = 行数×行高+边距——
// 长文本不再跑出固定 360×88 框、短文本（todo setStatus 单行）不再配两行高空框。
// 几何镜像 components/toast.h build() 的默认 ThemeMetricTokens 布局
//（componentTokensWeChat 不覆盖 metrics，单测与真机同源）：
//   左图标位 textX = 44+12 = 56；右 chrome = 闭合钮 28 + 右保留 22 → 宽公式 +106
//   标题行 16..40（18px 字/行高 22）；正文行 44 起（14px 字/行高 18）；下留白 16
// 字体测量走 TextPrimitive（与渲染同一台引擎）；字体不可用（测量全零）时整体
// 退回组件默认 360×88——退行退化而不是崩成最小框。
#include <ui_kit/toast.h>

#include <components/toast.h>

#include <core/render/text.h>

#include <algorithm>
#include <cmath>

namespace ui_kit {

namespace {

// 与 components/toast.h 默认 ThemeMetricTokens 同源（见文件头注释）。
constexpr float kToastTextX = 56.0f;        // 左：图标 44 + 图文距 12
constexpr float kToastChromeRight = 50.0f; // 右：闭合钮 28 + 右保留 22
constexpr float kToastTitleFont = 18.0f;   // typography.control
constexpr float kToastMsgFont = 14.0f;      // typography.label
constexpr float kToastMsgLineH = 18.0f;    // label + lineGap(4)
constexpr float kToastMsgTop = 44.0f;      // control.control（正文行顶）
constexpr float kToastPadBottom = 16.0f;   // spacing.section
constexpr float kToastMaxW = 360.0f;       // 宽上限（防巨框；同组件默认宽）
constexpr float kToastMinW = 180.0f;      // 宽下限（极短文本仍成像完整 toast）
constexpr float kToastMinH = 58.0f;        // 高下限（图标 20+22+竖向留白）

// 换行行数：与渲染同一引擎测量（wrap + maxWidth 下 measureTextSize 返回
// {行宽, 行数×lineHeight}，内部带缓存）；字体不可用（高度 0）退 1 行。
int wrappedLineCount(const std::string& text, float maxWidth, float fontSize,
                     float lineHeight) {
    if (text.empty() || maxWidth <= 0.0f) return 1;
    core::TextStyle style;
    style.text = text;
    style.fontSize = fontSize;
    style.lineHeight = lineHeight;
    style.maxWidth = maxWidth;
    style.wrap = true;
    const core::Vec2 size = core::TextPrimitive::measureTextSize(style);
    if (size.y <= 0.0f) return 1;   // 测量不可用 → 按 1 行退行
    return std::max(1, static_cast<int>(std::round(size.y / lineHeight)));
}

}  // namespace

void drawToast(eui::Ui& ui, const eui::Screen& screen, const std::string& id,
               ToastState& state,
               const components::theme::ThemeColorTokens& tokens,
               const ToastOptions& options) {
    auto builder = components::toast(ui, id);
    builder.theme(tokens)
        .screen(screen.width, screen.height)
        .visible(state.open)
        .duration(options.durationSec)
        .icon(state.isError ? options.errorIcon : options.okIcon)
        .title(state.title)
        // 无条件覆盖正文（含空串）：组件默认正文是英文占位串，不覆盖会在无正文站点画出来。
        .message(state.body);

    // —— 内容驱动尺寸（UAT6）——
    const float maxTextW = kToastMaxW - kToastTextX - kToastChromeRight;
    const float titleW = state.title.empty()
        ? 0.0f
        : core::TextPrimitive::measureTextWidth(state.title, {},
                                                kToastTitleFont);
    const float msgOneLineW = state.body.empty()
        ? 0.0f
        : core::TextPrimitive::measureTextWidth(state.body, {}, kToastMsgFont);
    const bool fontsAvailable =
        (state.title.empty() || titleW > 0.0f) &&
        (state.body.empty() || msgOneLineW > 0.0f);
    if (fontsAvailable) {
        // 宽：单行内容宽（正文超上限则封顶）+ 左右 chrome；钳进 [180, 360]。
        const float contentW =
            std::max(titleW, std::min(msgOneLineW, maxTextW));
        const float w = std::clamp(
            contentW + kToastTextX + kToastChromeRight, kToastMinW, kToastMaxW);
        // 高：有正文 = 正文顶 44 + 行数×18 + 下留白 16；仅标题 = 标题底 40 +
        // 16；两者都过图标竖向下限 58。短文本一行就一行高。
        float h;
        if (state.body.empty()) {
            h = 40.0f + kToastPadBottom;
        } else {
            const int lines =
                wrappedLineCount(state.body, maxTextW, kToastMsgFont,
                                 kToastMsgLineH);
            h = kToastMsgTop + static_cast<float>(lines) * kToastMsgLineH +
                kToastPadBottom;
        }
        h = std::max(h, kToastMinH);
        builder.size(w, h);
    }
    // 字体不可用：不调 .size()，落组件默认 360×88（退行退化，不崩成最小框）。

    builder.onAutoDismiss([&state] { state.dismiss(); })
        .onDismiss([&state] { state.dismiss(); })
        .build();
}

} // namespace ui_kit
