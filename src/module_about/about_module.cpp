// 关于页独立模块：微信风格关于页（W10 整改）。
// 居中纯白卡片 + 信息列表两列 + Git commit/版本绿色文字 + 复制功能 + 版权 footer。
// 渐变光晕装饰已移除（about_anim.h 保留空壳）。无 onStart/onStop/storage。
// 平台/架构由 CMake 配置期注入宏，本文件只消费。
#include "core/component_theme.h"
#include "core/logging.h"
#include "core/module_interface.h"
#include "core/theme.h"
#include "core/window/window_backend.h"  // core::window::setClipboardText（复制）
#include "core/window_effect_state.h"    // 页底 alpha 按窗口效果档位（UAT2 B 批）

#include <ui_kit/button.h>      // ui_kit::button Secondary（复制全部信息）
#include <ui_kit/light_toast.h>  // showLightToast（复制成功 toast）
#include <ui_kit/logo.h>          // ui_kit::drawDevDeskLogo（品牌几何 logo）

#include <eui/dsl.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>

// 编译期信息宏回退（CMake 注入缺失时的手工编译兜底）
#ifndef DEVDESK_VERSION
#define DEVDESK_VERSION "dev"
#endif
#ifndef DEVDESK_BUILD_TYPE
#define DEVDESK_BUILD_TYPE "?"
#endif
#ifndef DEVDESK_EUI_NEO_REV
#define DEVDESK_EUI_NEO_REV "unknown"
#endif
#ifndef DEVDESK_EUI_NEO_MIN_REV
#define DEVDESK_EUI_NEO_MIN_REV "?"
#endif
#ifndef DEVDESK_GIT_REV
#define DEVDESK_GIT_REV "unknown"
#endif
#ifndef DEVDESK_COMPILER
#define DEVDESK_COMPILER "unknown"
#endif
#ifndef DEVDESK_PLATFORM
#define DEVDESK_PLATFORM "unknown"
#endif
#ifndef DEVDESK_ARCH
#define DEVDESK_ARCH "unknown"
#endif

namespace module::about {

namespace {

// 版本信息行（本模块自给，不在 settings 内维护第二份）。
std::string aboutVersionLine() {
    return std::string("v") + DEVDESK_VERSION + " · " + DEVDESK_BUILD_TYPE;
}

// 构建时刻 = 本 TU 编译时刻（__DATE__/__TIME__）
std::string aboutBuildLine() {
    return std::string("构建 ") + __DATE__ + " " + __TIME__;
}

std::string aboutEuiNeoLine() {
    return std::string("rev ") + DEVDESK_EUI_NEO_REV + " (min " +
           DEVDESK_EUI_NEO_MIN_REV + ")";
}

std::string aboutPlatformLine() {
    return std::string(DEVDESK_PLATFORM) + " · " + DEVDESK_ARCH;
}

std::string aboutCppLine() {
    if (__cplusplus >= 202002L) return "C++20";
    if (__cplusplus >= 201703L) return "C++17";
    if (__cplusplus >= 201402L) return "C++14";
    return "C++11";
}

// 「复制全部信息」文本：拼全部版本/commit/依赖信息到一段文本。
std::string buildAllInfoText() {
    std::string text;
    text += "DevDesk " + aboutVersionLine() + "\n";
    text += "Git commit: " + std::string(DEVDESK_GIT_REV) + "\n";
    text += "构建时间: " + aboutBuildLine() + "\n";
    text += "编译器: " + std::string(DEVDESK_COMPILER) + "\n";
    text += "平台: " + aboutPlatformLine() + "\n";
    text += "C++ 标准: " + aboutCppLine() + "\n";
    text += "GUI 框架: EUI-NEO (OpenGL)\n";
    text += "EUI-NEO: " + aboutEuiNeoLine() + "\n";
    text += "数据库: SQLite 3 + FTS5\n";
    text += "HTTP: libcurl (静态链接)\n";
    text += "TLS: OpenSSL 3.x (静态链接)\n";
    text += "字体: FreeType + FontAwesome 7";
    return text;
}

// 信息行：左标签 + 右内容。highlight=true → 绿色文字 + 可复制（wechat.primary）。
struct AboutRow {
    const char* label;
    std::string value;
    bool highlight = false;
};

} // namespace

class AboutModule : public ::core::Module {
public:
    ::core::PageId pageId() const override { return ::core::PageId::About; }
    std::string displayName() const override { return "关于"; }
    std::string shortName() const override { return "关于"; }
    std::string iconGlyph() const override { return eui::utf8(0xF05A); }

    void compose(eui::Ui& ui, const eui::Screen& screen) override {
        const auto& t = core::theme::current();
        const auto& wc = t.colors.wechat;

        // —— 页面底色：wechat.pageBg #F7F7F7（§4.14 面板灰底）——
        // UAT2 B 批：tint 的 alpha 随窗口效果档位（core::window_effect_state 单值，
        // 写口 = settings applyWindowVisuals）——磨砂 0.15 透 Acrylic backdrop、半透 0.5、
        // off 实色 1.0；off 档与原 opaque 铺底视觉等价。
        ui.rect("about.bg")
            .position(0.0f, 0.0f)
            .size(screen.width, screen.height)
            .color(eui::Color{wc.pageBg.r, wc.pageBg.g, wc.pageBg.b,
                              ::core::window_effect_state::surfaceAlpha(
                                  ::core::window_effect_state::current())})
            .build();

        // —— 卡片几何常量（UAT3 放大：卡 580→760 / pad 32→48 / 上下留白 40→60 /
        //    图标 56→72 / 标题高 24→30 / 行高 40→48 / 按钮 160·34→200·40）——
        constexpr float kCardW = 760.0f;  // 名义卡宽；窄窗下按 cardW 公式自适应
        constexpr float kCardPadX = 48.0f;
        constexpr float kCardPadTop = 60.0f;
        constexpr float kCardPadBottom = 60.0f;
        constexpr float kIconSize = 72.0f;
        constexpr float kIconGap = 10.0f;
        constexpr float kTitleH = 30.0f;
        constexpr float kTitleGap = 28.0f;
        constexpr float kRowH = 48.0f;
        constexpr int kRowCount = 6;
        constexpr float kButtonGap = 28.0f;
        constexpr float kButtonH = 40.0f;
        constexpr float kButtonW = 200.0f;
        constexpr float kFooterH = 20.0f;
        constexpr float kFooterGap = 16.0f;  // 卡底→页脚间距
        constexpr float kLabelW = 120.0f;

        // 卡高 = 上留白 + 图标 + 图标间距 + 标题 + 标题间距 + 6 行 + 按钮间距 + 按钮 + 下留白
        const float cardH_nominal = kCardPadTop + kIconSize + kIconGap + kTitleH +
                                    kTitleGap + kRowCount * kRowH + kButtonGap +
                                    kButtonH + kCardPadBottom;

        // 宽度自适应（UAT3：最小窗宽拟降至 1000，content≈712 < 名义 760）：
        // 有效卡宽 = min(kCardW, screen.width − 2×24)，下限 560（两侧至少 24px）
        const float cardW =
            std::max(560.0f, std::min(kCardW, screen.width - 2.0f * 24.0f));

        // 居中：x = (screen.w - cardW) / 2
        const float cardX = std::max(0.0f, (screen.width - cardW) / 2.0f);

        // 页脚贴页面底部（spacingXXLarge padding）
        const float footerY = screen.height - t.sizes.spacingXXLarge - kFooterH;

        // 卡片顶贴：默认 spacingXXLarge；极矮窗下先推 cardY，仍超则 clamp 卡高
        float cardY = t.sizes.spacingXXLarge;
        float cardH = cardH_nominal;
        if (cardY + cardH + kFooterGap > footerY) {
            cardY = std::max(0.0f, footerY - kFooterGap - cardH);
            // UAT3：cardY 已触 0 仍超 footer（窗 640→页高≈600 时 cardH=616 >
            // footerY=540），clamp cardH 使不叠
            if (cardY + cardH + kFooterGap > footerY) {
                cardH = std::max(200.0f, footerY - cardY - kFooterGap);
            }
        }

        // —— 居中纯白卡片：wechat.cardBg #FFFFFF + radiusWeChat 6px + dividerPrimary 边框 ——
        ui.stack("about.card")
            .position(cardX, cardY)
            .size(cardW, cardH)
            .content([&] {
                // 卡片背景
                ui.rect("about.card.bg")
                    .position(0.0f, 0.0f)
                    .size(cardW, cardH)
                    .color(wc.cardBg)
                    .radius(t.sizes.radiusWeChat)
                    .border(1.0f, wc.dividerPrimary)
                    .build();

                // —— 卡片顶部：DevDesk 图标 + 应用名称，居中 ——
                float y = kCardPadTop;
                // C 方案终端窗 >_ 几何 logo 替换 fa-laptop-code 字形（图标批 2026-10-20）
                ui_kit::drawDevDeskLogo(ui, "about.card.icon",
                                        (cardW - kIconSize) / 2.0f, y,
                                        kIconSize, wc.primary, wc.cardBg);
                y += kIconSize + kIconGap;

                ui.text("about.card.title")
                    .position(0.0f, y)
                    .size(cardW, kTitleH)
                    .text("DevDesk")
                    .fontSize(t.sizes.fontSizeTitle)
                    .lineHeight(kTitleH)
                    .fontWeight(700)
                    .color(wc.textPrimary)
                    .horizontalAlign(eui::HorizontalAlign::Center)
                    .build();
                y += kTitleH + kTitleGap;

                // —— 信息列表：两列对齐 + 分割线 ——
                // 左标签定宽 120px wechat.textSecondary；右内容左对齐 wechat.textPrimary
                const AboutRow rows[] = {
                    {"版本", aboutVersionLine(), true},
                    {"Git commit", DEVDESK_GIT_REV, true},
                    {"构建时间", aboutBuildLine(), false},
                    {"编译器", DEVDESK_COMPILER, false},
                    {"平台", aboutPlatformLine(), false},
                    {"C++ 标准", aboutCppLine(), false},
                };

                const float valueX = kCardPadX + kLabelW + t.sizes.spacing;
                // 留 20px 给 highlight 行的复制图标；非 highlight 行右端多 20px 空白（不影响左对齐）
                const float valueW = cardW - valueX - kCardPadX - 20.0f;

                for (int i = 0; i < kRowCount; ++i) {
                    const float rowY = y + static_cast<float>(i) * kRowH;

                    // 分割线（行间，非首行上方；§4.7 二级分割线 #EDEDED）
                    if (i > 0) {
                        ui.rect("about.card.divider." + std::to_string(i))
                            .position(kCardPadX, rowY - 0.5f)
                            .size(cardW - kCardPadX * 2.0f, 1.0f)
                            .color(wc.dividerSecondary)
                            .build();
                    }

                    // 左标签
                    ui.text("about.card.label." + std::to_string(i))
                        .position(kCardPadX, rowY)
                        .size(kLabelW, kRowH)
                        .text(rows[i].label)
                        .fontSize(t.sizes.fontSizeWeChat)  // UAT3 放大：13→16
                        .lineHeight(kRowH)
                        .color(wc.textSecondary)
                        .build();

                    // 右内容（highlight → 绿色 wechat.primary）
                    ui.text("about.card.value." + std::to_string(i))
                        .position(valueX, rowY)
                        .size(valueW, kRowH)
                        .text(rows[i].value)
                        .fontSize(t.sizes.fontSizeWeChat)  // UAT3 放大：13→16
                        .lineHeight(kRowH)
                        .color(rows[i].highlight ? wc.primaryText : wc.textPrimary)
                        .build();

                    // 复制图标（highlight 行；EUI 无 onEnter/onLeave → 始终可见）
                    if (rows[i].highlight) {
                        constexpr float kCopyIconSize = 16.0f;
                        const float iconX = valueX + valueW + 4.0f;
                        const float iconY = rowY + (kRowH - kCopyIconSize) / 2.0f;
                        ui.text("about.card.copy." + std::to_string(i))
                            .position(iconX, iconY)
                            .size(kCopyIconSize, kCopyIconSize)
                            .icon(0xF0C5)  // fa-copy
                            .fontSize(kCopyIconSize)
                            .color(wc.textTertiary)
                            .build();

                        // 透明 hit 区：点击复制该行值
                        std::string copyText = rows[i].value;
                        ui.rect("about.card.hit." + std::to_string(i))
                            .position(kCardPadX, rowY)
                            .size(cardW - kCardPadX * 2.0f, kRowH)
                            .color(eui::Color{0.0f, 0.0f, 0.0f, 0.0f})
                            .cursor(eui::CursorShape::Hand)
                            .onClick([this, copyText] {
                                ::core::window::setClipboardText(copyText);
                                triggerToast("已复制");
                                LOG_INFO("About: 已复制 " + copyText);
                            })
                            .build();
                    }
                }
                y += kRowCount * kRowH + kButtonGap;

                // —— 「复制全部信息」浅灰次按钮（§4.1 Secondary 级），居中 ——
                const float btnX = (cardW - kButtonW) / 2.0f;
                ui_kit::ButtonOptions opts;
                opts.onClick = [this] {
                    ::core::window::setClipboardText(buildAllInfoText());
                    triggerToast("已复制全部信息");
                    LOG_INFO("About: 已复制全部信息");
                };
                ui_kit::button(ui, "about.card.copyall", "复制全部信息",
                               ui_kit::ButtonLevel::Secondary,
                               btnX, y, kButtonW, kButtonH, t, opts);

                // TODO: 开源许可证模块（补充项，签认可划；依赖清单待取全，暂不硬做）
            })
            .build();

        // —— 版权声明：12px 三级灰 wechat.textTertiary #999999，居中页面最底部 ——
        ui.text("about.footer")
            .position(0.0f, footerY)
            .size(screen.width, kFooterH)
            .text("© 2026 DevDesk")
            .fontSize(12.0f)  // spec §3.7.4：版权 12px（theme 无 12px token，用字面量）
            .lineHeight(kFooterH)
            .color(wc.textTertiary)
            .horizontalAlign(eui::HorizontalAlign::Center)
            .build();

        // —— 复制成功 toast：微信式轻提示（§4.10），居中 ——
        // toast 机制：onClick 设 toastActive_=true + toastMsg_；compose 每帧调 showLightToast。
        // toastActive_=true 时传 toastMsg_ 触发/续渲；1.5s 后 elapsed >= duration → toastActive_=false
        // → 下一帧传 "" 重置 state（s.message 变化 → 触发但 message.empty → 不渲染）。
        // EUI 无 onEnter/onLeave，hover 复制图标改为始终可见（W10 已知限制）。
        const std::string& toastMsg = toastActive_ ? toastMsg_ : std::string{};
        ui_kit::showLightToast(ui, "about.toast", toastMsg,
                               screen.width * 0.5f, screen.height * 0.5f, t);
        if (toastActive_) {
            const float elapsed =
                std::chrono::duration<float>(
                    std::chrono::steady_clock::now() - toastStart_).count();
            if (elapsed >= ui_kit::kLightToastDurationSec) {
                toastActive_ = false;
            }
        }
    }

private:
    bool toastActive_ = false;
    std::string toastMsg_;
    std::chrono::steady_clock::time_point toastStart_{};

    void triggerToast(const std::string& msg) {
        toastMsg_ = msg;
        toastActive_ = true;
        toastStart_ = std::chrono::steady_clock::now();
    }
};

} // namespace module::about

extern "C" ::core::Module* create_about_module() {
    return new module::about::AboutModule();
}
