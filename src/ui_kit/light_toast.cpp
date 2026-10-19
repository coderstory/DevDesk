// src/ui_kit/light_toast.cpp — 微信式轻提示实现（spec §4.10）。
// 黑半透明 bg（语义常量，wechat 无槽位）+ 白字（wechat.onPrimary）+ 圆角 6 +
// 居中 + 1.5s 自动消失。既有通知式 toast 保留不动；新反馈点调本 helper。
//
// 模式仿 W2b button.cpp（学其 stack 包 rect.bg + text 的层级模式 + 颜色走
// wechat 令牌）。色值纪律见 light_toast.h 文件头注释。
#include <ui_kit/light_toast.h>

#include <algorithm>
#include <chrono>

namespace ui_kit {

namespace {

// 估算轻提示宽度（防 layout 收缩）：
// 14px 字号下 ASCII 字符宽约 8px、CJK 约 14px，取中位 10px × message.size()。
// 最小宽 80px（短文案如「已复制」不至过窄）；左右内边距 16px。
constexpr float kLightToastMinWidth = 80.0f;
constexpr float kLightToastHeight = 36.0f;             // §4.1 D-12 统一按钮高 32-36 取中
constexpr float kLightToastPadX = 16.0f;
constexpr float kLightToastCharWidthEstimate = 10.0f;
constexpr float kLightToastFontSize = 14.0f;          // §3.3 主文字

} // namespace

void showLightToast(eui::Ui& ui, const std::string& id, const std::string& message,
                    float cx, float cy, const core::theme::Tokens& t) {
    LightToastState& s = ui.state<LightToastState>(id + ".state");
    const auto now = std::chrono::steady_clock::now();

    // 新 message 触发：覆盖前一条，重新计时（单实例无队列，符合「轻提示」语义）
    if (s.message != message) {
        s.message = message;
        s.open = true;
        s.startTime = now;
    }

    // 自动消失：elapsed >= duration → dismiss（§4.10 1.5s）
    if (s.open) {
        const float elapsed = std::chrono::duration<float>(now - s.startTime).count();
        if (elapsed >= kLightToastDurationSec) {
            s.open = false;
        }
    }

    if (!s.open || s.message.empty()) return;

    const auto& wc = t.colors.wechat;

    // 几何：居中于 (cx, cy)
    const float estW = std::max(kLightToastMinWidth,
                                static_cast<float>(s.message.size()) * kLightToastCharWidthEstimate
                                + kLightToastPadX * 2.0f);
    const float bgX = cx - estW * 0.5f;
    const float bgY = cy - kLightToastHeight * 0.5f;

    // stack 包 rect.bg + text（仿 button.cpp 模式，保证 text 浮在 bg 之上）
    ui.stack(id)
        .position(bgX, bgY)
        .size(estW, kLightToastHeight)
        .content([&] {
            // 黑半透明 bg（§4.10）：wechat 无对应槽位，用语义常量（见 header 注释）
            ui.rect(id + ".bg")
                .size(estW, kLightToastHeight)
                .color(eui::Color{0.0f, 0.0f, 0.0f, kLightToastBgAlpha})
                .radius(t.sizes.radiusWeChat)
                .build();

            // 白字（§4.10）：wechat.onPrimary=#FFFFFF（主色上的前景色，黑底白字同义）
            ui.text(id + ".text")
                .size(estW, kLightToastHeight)
                .text(s.message)
                .fontSize(kLightToastFontSize)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center)
                .color(wc.onPrimary)
                .build();
        })
        .build();
}

} // namespace ui_kit
