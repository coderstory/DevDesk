// src/ui_kit/empty_state.cpp — 空状态实现（spec §4.12 + 2026-11-01 统一整改 R3）。
// 模式仿 W2b button.cpp（颜色走 theme token，禁裸 hex）。防 §4.12 禁止项见 header。
#include <ui_kit/empty_state.h>

#include <components/theme.h>

#include <ui_kit/button.h>

#include <algorithm>

namespace ui_kit {

namespace {

constexpr float kEmptyStateFontSize = 14.0f;        // §3.3 主文字
constexpr float kEmptyStateGuideFontSize = 13.0f;   // 引导句小一档
constexpr float kEmptyStateButtonW = 100.0f;         // 就近按钮宽度（§4.12 就近原则）
constexpr float kEmptyStateIconBox = 40.0f;         // 图标底盒
constexpr float kEmptyStateIconGlyph = 18.0f;       // 图标字形
constexpr float kEmptyStateMsgH = 20.0f;
constexpr float kEmptyStateGuideH = 18.0f;
constexpr float kEmptyStateGap = 6.0f;              // 主句↔引导句
constexpr float kEmptyStateIconGap = 12.0f;         // 图标↔主句
constexpr float kEmptyStateActionGap = 16.0f;       // 文字↔按钮

} // namespace

void emptyState(eui::Ui& ui, const std::string& id,
                const EmptyStateOptions& options,
                float x, float y, float w, float h,
                const core::theme::Tokens& t) {
    const bool hasIcon = options.icon.has_value();
    const bool hasGuide = !options.guide.empty();
    const bool hasAction = options.action.has_value() && !options.action->first.empty();

    // 整组高度 → 组在盒内垂直居中（此前各模块自行拼装，agent 页空态内容顶到盒顶，
    // 下方 70% 空白，读起来像布局坏了）。
    float groupH = 0.0f;
    if (hasIcon) groupH += kEmptyStateIconBox + kEmptyStateIconGap;
    groupH += kEmptyStateMsgH;
    if (hasGuide) groupH += kEmptyStateGap + kEmptyStateGuideH;
    if (hasAction) groupH += kEmptyStateActionGap + kButtonHeight;

    // 2026-11-01：整组包一层 stack 根（id = 传入的 id）。两个原因：
    // ① 调用方按 id 定位空态（单测 md.editor.hint 等）——此前只有 .message/.guide
    //    这些派生 id，根 id 不存在。
    // ② 空态可能挂在 column 父级下——column 的流式游标会接管子元素的 x/y，
    //    直接画散件会被竖着摞起来。stack 根把整组收成一件，子元素坐标相对根。
    ui.stack(id)
        .position(x, y)
        .size(std::max(0.0f, w), std::max(0.0f, h))
        .content([&] {
        float cy = std::max(0.0f, (h - groupH) * 0.5f);

        if (hasIcon) {
            ui.rect(id + ".icon.bg")
                .position((w - kEmptyStateIconBox) * 0.5f, cy)
                .size(kEmptyStateIconBox, kEmptyStateIconBox)
                .radius(t.sizes.radius)
                .color(t.colors.primarySoftBg)
                .build();
            ui.text(id + ".icon")
                .position((w - kEmptyStateIconBox) * 0.5f, cy)
                .size(kEmptyStateIconBox, kEmptyStateIconBox)
                .icon(eui::utf8(*options.icon))
                .fontSize(kEmptyStateIconGlyph)
                .color(t.colors.primary)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();
            cy += kEmptyStateIconBox + kEmptyStateIconGap;
        }

        ui.text(id + ".message")
            .position(0.0f, cy)
            .size(w, kEmptyStateMsgH)
            .text(options.message)
            .fontSize(kEmptyStateFontSize)
            .horizontalAlign(eui::HorizontalAlign::Center)
            .verticalAlign(eui::VerticalAlign::Center)
            .color(t.colors.textSecondary)
            .build();
        cy += kEmptyStateMsgH;

        if (hasGuide) {
            cy += kEmptyStateGap;
            ui.text(id + ".guide")
                .position(0.0f, cy)
                .size(w, kEmptyStateGuideH)
                .text(options.guide)
                .fontSize(kEmptyStateGuideFontSize)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center)
                .color(t.colors.textTertiary)
                .build();
            cy += kEmptyStateGuideH;
        }

        if (hasAction) {
            cy += kEmptyStateActionGap;
            const float btnX = (w - kEmptyStateButtonW) * 0.5f;
            ButtonOptions opts;
            opts.onClick = options.action->second;
            button(ui, id + ".action", options.action->first, ButtonLevel::Secondary,
                   btnX, cy, kEmptyStateButtonW, kButtonHeight, t, opts);
        }
        })
        .build();
}

void emptyState(eui::Ui& ui, const std::string& id, const std::string& message,
                const std::optional<std::pair<std::string, std::function<void()>>>& action,
                float x, float y, float w, float h,
                const core::theme::Tokens& t) {
    EmptyStateOptions opts;
    opts.message = message;
    opts.action = action;
    emptyState(ui, id, opts, x, y, w, h, t);
}

} // namespace ui_kit
