// 关于页独立模块：纯 compose 页，只读静态信息 + 构建期注入常量，
// 无 onStart/onStop/storage，不依赖任何 module_*。平台/架构由 CMake 配置期注入宏，本文件只消费。
#include "core/component_theme.h"
#include "core/logging.h"
#include "core/module_interface.h"
#include "core/paths.h"
#include "core/theme.h"

#include "platform/file_open.h"

#include "about_anim.h"

#include <ui_kit/card.h>

#include <eui/dsl.h>

#include <components/button.h>

#include <algorithm>
#include <array>
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

struct AboutRow {
    const char* label;   // 13px textTertiary 左
    std::string value;   // 13px 右对齐
    bool hash = false;   // true → 短 hash / rev 等标识符用主色
};

// 卡片底板走 ui_kit::cardFrame（与设置页同一视觉语言）。行高 = 卡高算式的一项。
void aboutCard(eui::Ui& ui, const std::string& id, const core::theme::Tokens& t,
               float x, float y, float w, float h, unsigned int iconCode,
               const char* title, const AboutRow* rows, int rowCount) {
    constexpr float padX = 28.0f;
    constexpr float kPadY = 24.0f;   // 上下留白（卡高算式的一项，勿单改）
    constexpr float kTitleRowH = 28.0f;
    constexpr float kRowH = 28.0f;   // 信息行行高（卡高算式的一项，勿单改）
    ui.stack(id)
        .position(x, y)
        .size(w, h)
        .content([&] {
            ui_kit::cardFrame(ui, id, w, h, t);
            ui.text(id + ".icon")
                .position(padX, kPadY + 2.0f)   // 图标与标题行同基线（+2 居中）
                .size(20.0f, 20.0f)
                .icon(iconCode)
                .fontSize(18.0f)
                .color(t.colors.textSecondary)
                .build();
            ui.text(id + ".title")
                .position(padX + 24.0f, kPadY)
                .size(w - padX * 2.0f - 24.0f, 22.0f)
                .text(title)
                .fontSize(t.sizes.fontSizeLarge)
                .lineHeight(22.0f)
                .fontWeight(600)
                .color(t.colors.textPrimary)
                .build();
            const float rowTop = kPadY + kTitleRowH;
            for (int i = 0; i < rowCount; ++i) {
                const float rowY = rowTop + static_cast<float>(i) * kRowH;
                ui.text(id + ".label." + std::to_string(i))
                    .position(padX, rowY)
                    .size(110.0f, kRowH)
                    .text(rows[i].label)
                    .fontSize(t.sizes.fontSizeSmall)
                    .lineHeight(kRowH)
                    .color(t.colors.textTertiary)
                    .build();
                ui.text(id + ".value." + std::to_string(i))
                    .position(padX, rowY)
                    .size(w - padX * 2.0f, kRowH)
                    .text(rows[i].value)
                    .fontSize(t.sizes.fontSizeSmall)
                    .lineHeight(kRowH)
                    .horizontalAlign(eui::HorizontalAlign::Right)
                    .color(rows[i].hash ? t.colors.primary
                                        : t.colors.textPrimary)
                    .build();
            }
        })
        .build();
}

// 备用「打开」按钮；[[maybe_unused]] 防 -Werror=unused-function（GCC 对匿名命名空间未用函数告警）
[[maybe_unused]] void openDirectoryButton(eui::Ui& ui, const std::string& id,
                         const core::theme::Tokens& t, float x, float y,
                         const std::string& path) {
    const auto compTheme = core::theme::componentTokens(t);
    components::button(ui, id)
        .position(x, y)
        .size(72.0f, 30.0f)
        .text("打开")
        .fontSize(t.sizes.fontSizeSmall)
        .theme(compTheme, /*primary=*/false)
        .colors(t.colors.surfaceElevated, t.colors.hover, t.colors.active)
        .textColor(t.colors.textPrimary)
        .radius(t.sizes.radiusSmall)
        .border(1.0f, t.colors.borderDefault)
        .onClick([path] {
            auto r = ::platform::file_open::openDirectory(path);
            if (!r.isOk()) {
                LOG_ERROR("About: openDirectory 失败 " + path + ": " +
                          r.errorMessage());
            }
        })
        .build();
}

// 动态光晕背景：光斑 + 利萨如漂移 + 呼吸 + 聚光扫过带 + 霓虹边；休眠由 awake() 单点开关控制。
// 驱动 onTimer 30fps（光晕要一直动，但不必每帧重绘）。

// 主题两色线性插值（theme 运行期可切换，故在 compose 时现算）
eui::Color mixThemeColor(const eui::Color& a, const eui::Color& b, float t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
            a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}

} // namespace

class AboutModule : public ::core::Module {
public:
    ::core::PageId pageId() const override { return ::core::PageId::About; }
    std::string displayName() const override { return "关于"; }
    std::string shortName() const override { return "关于"; }
    std::string iconGlyph() const override { return eui::utf8(0xF05A); }  // fa-circle-info

    void compose(eui::Ui& ui, const eui::Screen& screen) override {
        const auto& t = core::theme::current();

        // 页面不铺底色（主窗 clearColor 透出）；本层透明承载，作背景装饰不拦交互。
        AboutAnimState& anim = ui.state<AboutAnimState>("about.anim");
        if (!anim.initialized) {
            initGlowLayer(anim);
        }
        auto bg = ui.rect("about.bg");
        bg.position(0.0f, 0.0f)
            .size(screen.width, screen.height)
            .color(eui::Color{0.0f, 0.0f, 0.0f, 0.0f})
            .cursor(eui::CursorShape::Arrow)
            .onMove([&anim](const eui::PointerEvent&, const eui::Rect&) {
                anim.wakeHold = 0.0f;  // 指针移动唤醒 / 续期
            });
        if (anim.awake()) {
            // 30fps 节拍推进；空闲超时后不再挂此回调 → 运行时零合成零重绘
            bg.onTimer(AboutAnimState::kFrameSeconds,
                       [&anim] { anim.step(AboutAnimState::kFrameSeconds); });
        }
        bg.build();

        // 光斑：同心圆叠层（半径递增 / alpha 递减）伪造径向渐变，色只用 primary + 暖琥珀。
        const eui::Color amber = t.colors.warning;
        for (int i = 0; i < AboutAnimState::kBlobCount; ++i) {
            const GlowBlob& blob = anim.blobs[static_cast<std::size_t>(i)];
            float cx = 0.0f, cy = 0.0f;
            blobCenter(anim, i, screen.width, screen.height, cx, cy);
            const float breath = blobBreath(anim, i);  // 1 ± 0.35
            const eui::Color base = mixThemeColor(t.colors.primary, amber,
                                                  blob.colorMix);
            for (int k = 0; k < AboutAnimState::kLayersPerBlob; ++k) {
                const float radius = blobLayerRadius(blob.radius, k);
                eui::Color layer = base;
                layer.a = blobLayerAlpha(k) * breath;
                auto element = ui.rect("about.glow." + std::to_string(i) + "." +
                                       std::to_string(k));
                element.position(cx - radius, cy - radius)
                    .size(radius * 2.0f, radius * 2.0f)
                    .radius(radius)
                    .color(layer);
                if (k == AboutAnimState::kLayersPerBlob - 1) {
                    // 最外圈霓虹边：primary@0.35 的 shadow 提亮轮廓（不影响本层填充 alpha）。
                    eui::Color halo = base;
                    halo.a = AboutAnimState::kNeonShadowAlpha;
                    element.shadow(AboutAnimState::kNeonShadowBlur, 0.0f, 0.0f,
                                   halo);
                } else {
                    // 内层柔边：同色 shadow（offset 0）抹平离散叠层的同心圆台阶。
                    eui::Color halo = base;
                    halo.a = layer.a * AboutAnimState::kLayerSoftAlphaScale;
                    element.shadow(AboutAnimState::kLayerSoftBlur, 0.0f, 0.0f,
                                   halo);
                }
                element.build();
            }
        }
        // 聚光扫过带：两半拼出对称梯形 alpha 剖面（无硬边），14s 从左扫到右一轮。
        {
            const float left = sweepLeft(anim, screen.width);
            constexpr float halfW = AboutAnimState::kSweepWidth * 0.5f;
            const float bandH = screen.height * AboutAnimState::kSweepHeightFactor;
            const float bandY = (screen.height - bandH) * 0.5f;
            eui::Color fade = t.colors.primary;
            fade.a = 0.0f;
            eui::Color peak = t.colors.primary;
            peak.a = AboutAnimState::kSweepAlpha;
            eui::Color halo = t.colors.primary;
            halo.a = AboutAnimState::kSweepShadowAlpha;
            for (int half = 0; half < AboutAnimState::kSweepHalves; ++half) {
                const bool leading = half == 1;  // 后半：峰值 → 0（行进前缘渐隐）
                ui.rect("about.sweep." + std::to_string(half))
                    .position(left + halfW * static_cast<float>(half), bandY)
                    .size(halfW, bandH)
                    .radius(halfW)
                    .gradient(leading ? peak : fade, leading ? fade : peak,
                              core::GradientDirection::Horizontal)
                    .rotate(AboutAnimState::kSweepTiltRad)
                    .shadow(AboutAnimState::kSweepShadowBlur, 0.0f, 0.0f, halo)
                    .build();
            }
        }

        // 居中 900px 主内容区（窄窗不滚动）：按最小窗宽 1330 减侧边栏 208 推得页面宽 1122，居中不溢出
        constexpr float kAboutContentW = 900.0f;
        const float x = std::max(0.0f, (screen.width - kAboutContentW) / 2.0f);
        constexpr float cardGap = 8.0f;
        // 单张卡高 = 上留白 24 + 标题行 28 + 6 行 × 28 + 下留白 24 = 244。
        constexpr float cardH = 24.0f + 28.0f + 6.0f * 28.0f + 24.0f;
        // hero 间距与标题行高：提为常量仅为把内容块总高写成算式
        constexpr float heroGap = 8.0f;
        constexpr float heroTitleH = 40.0f;
        constexpr float footerH = 20.0f;

        // topPad = 极矮窗兜底起点（下方 max 护栏）；正常窗口起笔点是反推值、恒大于它
        const float topPad = t.sizes.spacingXXLarge;
        // hero 图标 = 56（装饰尺寸，不在字号规范内；再大 em 盒余量小、有裁切风险）
        const float heroIcon = t.sizes.spacingXXLarge + t.sizes.spacingLarge;
        // 内容块整体贴页脚上方，起点反推 y = 页脚顶 − 一档间距 − 内容块总高；
        // max(topPad, …) 护栏：极矮窗装不下时退回顶部对齐
        const float contentH = heroIcon + heroGap + heroTitleH +
                               t.sizes.spacingXLarge + cardH + cardGap + cardH;
        const float footerTop = screen.height - t.sizes.spacingXXLarge - footerH;
        float y = std::max(topPad, footerTop - t.sizes.spacingXLarge - contentH);
        ui.text("about.hero.icon")
            .position(x + (kAboutContentW - heroIcon) / 2.0f, y)
            .size(heroIcon, heroIcon)
            .icon(0xF5FC)  // fa-laptop-code（侧边栏 logo 同款）
            .fontSize(heroIcon)
            .color(t.colors.primary)
            .build();
        y += heroIcon + heroGap;
        ui.text("about.hero.title")
            .position(x, y)
            .size(kAboutContentW, heroTitleH)
            .text("DevDesk")
            .fontSize(t.sizes.fontSizePage)
            .lineHeight(heroTitleH)
            .fontWeight(700)
            .color(t.colors.textPrimary)
            .horizontalAlign(eui::HorizontalAlign::Center)
            .build();
        y += heroTitleH + t.sizes.spacingXLarge;

        // 两卡占满内容区宽、等高 cardH，上下堆叠，起始 y 由 contentH 反推（贴底对齐）。
        const float cardW = kAboutContentW;

        // 卡 1：应用信息
        const AboutRow infoRows[] = {
            {"版本", aboutVersionLine(), false},
            {"Git commit", DEVDESK_GIT_REV, true},
            {"构建时间", aboutBuildLine(), false},
            {"编译器", DEVDESK_COMPILER, false},
            {"平台", aboutPlatformLine(), false},
            {"C++ 标准", aboutCppLine(), false},
        };
        aboutCard(ui, "about.card.app", t, x, y, cardW, cardH,
                  0xF05A /* fa-circle-info */, "应用信息", infoRows, 6);

        // 卡 2：依赖与框架
        const AboutRow depsRows[] = {
            {"GUI 框架", "EUI-NEO (OpenGL)", false},
            {"EUI-NEO", aboutEuiNeoLine(), true},
            {"数据库", "SQLite 3 + FTS5", false},
            {"HTTP", "libcurl (静态链接)", false},
            {"TLS", "OpenSSL 3.x (静态链接)", false},
            {"字体", "FreeType + FontAwesome 7", false},
        };
        aboutCard(ui, "about.card.deps", t, x, y + cardH + cardGap, cardW, cardH,
                  0xF5FD /* fa-layer-group */, "依赖与框架", depsRows, 6);

        // 页脚贴页面底部；max() 护栏：极矮窗下 footer 不与卡片重叠。
        const float cardsBottom = y + cardH + cardGap + cardH;
        const float footerY = std::max(cardsBottom + cardGap, footerTop);
        ui.text("about.footer")
            .position(x, footerY)
            .size(kAboutContentW, footerH)
            .text("© 2026 DevDesk")
            .fontSize(t.sizes.fontSizeSmall)
            .lineHeight(footerH)
            .color(t.colors.textTertiary)
            .horizontalAlign(eui::HorizontalAlign::Center)
            .build();
    }
};

} // namespace module::about

extern "C" ::core::Module* create_about_module() {
    return new module::about::AboutModule();
}
