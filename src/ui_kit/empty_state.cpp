// src/ui_kit/empty_state.cpp — 空状态实现（spec §4.12）。
// 极简文字（wechat.textSecondary）+ 可选就近按钮（W2b button Secondary 级）。
// 模式仿 W2b button.cpp（颜色走 wechat 令牌，禁裸 hex）。防 §4.12 禁止项见 header。
#include <ui_kit/empty_state.h>

#include <ui_kit/button.h>

namespace ui_kit {

namespace {

constexpr float kEmptyStateFontSize = 14.0f;        // §3.3 主文字
constexpr float kEmptyStateButtonW = 100.0f;         // 就近按钮宽度（§4.12 就近原则）
// 文字占上半区 60%，按钮占下半区 40%（视觉重心居中，按钮就近在文字下方）

} // namespace

void emptyState(eui::Ui& ui, const std::string& id, const std::string& message,
                const std::optional<std::pair<std::string, std::function<void()>>>& action,
                float x, float y, float w, float h,
                const core::theme::Tokens& t) {
    const auto& wc = t.colors.wechat;

    // 极简文字（§4.12 防「插画式/情绪化」——本 helper 只画一行极简文字）
    // 文字色 wechat.textSecondary(#666666)，14px，居中对齐
    const float msgH = action.has_value() ? h * 0.6f : h;
    ui.text(id + ".message")
        .position(x, y)
        .size(w, msgH)
        .text(message)
        .fontSize(kEmptyStateFontSize)
        .horizontalAlign(eui::HorizontalAlign::Center)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(wc.textSecondary)
        .build();

    // 可选就近按钮（§4.12 + §5.6 就近原则）：按钮在文字下方居中，
    // 走 W2b button helper Secondary 级（浅灰底 #F2F2F2 + 深灰字 #1A1A1A）。
    // action 由调用方传 {label, onClick}；helper 只负责就近画按钮，
    // 不加任何情绪化装饰。
    if (action.has_value() && !action->first.empty()) {
        const float btnX = x + (w - kEmptyStateButtonW) * 0.5f;
        const float btnY = y + msgH;
        ButtonOptions opts;
        opts.onClick = action->second;
        button(ui, id + ".action", action->first, ButtonLevel::Secondary,
               btnX, btnY, kEmptyStateButtonW, kButtonHeight, t, opts);
    }
}

} // namespace ui_kit
