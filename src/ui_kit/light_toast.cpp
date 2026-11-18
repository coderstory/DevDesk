// src/ui_kit/light_toast.cpp — 微信式轻提示实现（spec §4.10）。
// need328（2026-11-18 老板验收回调）：改「绿半透底 + 墨绿描边 + 墨绿字」，
// 几何/时序（居中、1.5s、宽估算、36 高、圆角 6）不动。既有通知式 toast
// 保留不动；新反馈点调本 helper。
//
// 色值纪律见 light_toast.h 文件头注释（D-03 语义常量 + 例外说明，两档口径）。
// 模式仿 W2b button.cpp（学其 stack 包 rect.bg + text 的层级模式 + 颜色走 token）。
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

// 单实例 state（function-local static，仿 splash.cpp:32-35 的 splashState() 模式）。
// C++ 标准保证持久（单线程 UI 线程下安全）。不依赖 ui.state<T> 的链接器 COMDAT
// fold 行为（见 light_toast.h 注释）。
LightToastState& lightToastState() {
    static LightToastState state;
    return state;
}

} // namespace

void showLightToast(eui::Ui& ui, const std::string& id, const std::string& message,
                    float cx, float cy, const core::theme::Tokens& t) {
    LightToastState& s = lightToastState();
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

    drawLightToastPill(ui, id, s.message, cx, cy, t);
}

void drawLightToastPill(eui::Ui& ui, const std::string& id,
                        const std::string& message, float cx, float cy,
                        const core::theme::Tokens& t) {
    if (message.empty()) return;

    // 几何：居中于 (cx, cy)——need328 只改配色，几何不动
    const float estW = std::max(kLightToastMinWidth,
                                static_cast<float>(message.size()) * kLightToastCharWidthEstimate
                                + kLightToastPadX * 2.0f);
    const float bgX = cx - estW * 0.5f;
    const float bgY = cy - kLightToastHeight * 0.5f;

    // stack 包 rect.bg + text（仿 button.cpp 模式，保证 text 浮在 bg 之上）
    ui.stack(id)
        .position(bgX, bgY)
        .size(estW, kLightToastHeight)
        .content([&] {
            // 绿半透 bg（need328）：success 令牌压 alpha（两档色值见 .h 文件头）
            eui::Color fill = t.colors.success;
            fill.a = kLightToastGreenAlpha;
            ui.rect(id + ".bg")
                .size(estW, kLightToastHeight)
                .color(fill)
                .border(kLightToastBorderWidth, kLightToastInk)
                .radius(t.sizes.radiusSmall)
                .build();

            // 墨绿字（need328）：绿底白字 ~1.9:1 不达标，沿用 need261 深墨裁决
            ui.text(id + ".text")
                .size(estW, kLightToastHeight)
                .text(message)
                .fontSize(core::theme::scaled(kLightToastFontSize))
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center)
                .color(kLightToastInk)
                .build();
        })
        .build();
}

// —— 测试 seam（见 light_toast.h 注释）——
void resetLightToastForTests() {
    LightToastState& s = lightToastState();
    s.open = false;
    s.message.clear();
    s.startTime = std::chrono::steady_clock::time_point{};
}

LightToastState& lightToastStateForTests() {
    return lightToastState();
}

} // namespace ui_kit
