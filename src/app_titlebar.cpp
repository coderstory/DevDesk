// 自绘标题栏（UAT3 D-R3-1；自 app_sidebar.cpp 拆出，纯搬移，语义零变更）。
// 公共接口在 app_sidebar.h；drawTitlebar 为本 TU 内部缝（app_sidebar_internal.h）。
#include "app_sidebar.h"
#include "app_sidebar_internal.h"

#include "core/theme.h"

#include <eui/app.h>  // customWindowChromeSupported / setCustomTitlebarSkin / closeMainWindow
#include <eui/types.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>

namespace app {

namespace {

// 自绘标题栏布局开关的测试钉住缝（nullopt = 跟平台能力走，见 customTitlebarActive）
std::optional<bool> g_titlebarModeOverride;

// 关闭钮 hover 态：EUI-NEO .onHover 回调写、compose 读。红底需要图标色随 hover 翻白，
// 而 .states() 只能给 rect 上色——关闭叉是独立 text 元素，色值须在此跨帧传递
bool g_closeButtonHovered = false;

} // namespace

// ---- 自绘标题栏（UAT3 D-R3-1）----

float titlebarHeight() {
    return 40.0f;
}

bool customTitlebarActive() {
    if (g_titlebarModeOverride.has_value()) {
        return *g_titlebarModeOverride;
    }
    return customWindowChromeSupported();
}

void setCustomTitlebarMode(std::optional<bool> mode) {
    g_titlebarModeOverride = std::move(mode);
}

// 窗口钮热区：每钮 46×标题条高。
// Windows：右缘两钮（关/最小）——UAT4 按用户拍板移除最大化钮，最大化/还原只留
// 标题栏双击一条路径。
// macOS：右缘三钮（关/最小/最大化）——need:140 老板 2026-11-06 改口径为
// 「Windows 同款自绘钮」，三钮齐全（红绿灯彩点方案已废止）。
constexpr float kWindowButtonW = 46.0f;

// 标题条右缘钮数量（Windows 2 / macOS 2）——老板 2026-11-06 最终口径：两端都
// 只有关闭+最小化两钮（Windows 侧亦无全屏钮），全屏走双击标题栏。
int titlebarButtonCount() {
    return 2;
}

// eui::Color → 0xRRGGBBAA（macOS 自绘钮皮肤注入用；EUI 覆盖层零硬编码色）
std::uint32_t rgba32(const eui::Color& c) {
    const auto channel = [](float v) {
        return static_cast<std::uint32_t>(
            std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
    };
    return (channel(c.r) << 24) | (channel(c.g) << 16) | (channel(c.b) << 8) |
           channel(c.a);
}

// macOS 自绘钮皮肤（need:140 路线 B；老板 2026-11-06 改口径 = Windows 同款）：
// 常态不铺底（0 = 透明）、hover 灰底（关闭钮 hover 红底）、图标色 textSecondary，
// 关闭钮 hover 时白叉。色值全取 theme token，深浅主题切换后重注入。
core::platform::CustomTitlebarSkin titlebarSkinFromTheme(const ::core::theme::Tokens& t) {
    core::platform::CustomTitlebarSkin skin;
    const auto button = [](const eui::Color& hover, const eui::Color& pressed) {
        core::platform::CustomTitlebarButtonSkin s;
        s.normal = 0;  // Windows 同款：常态透明
        s.hover = rgba32(hover);
        s.pressed = rgba32(pressed);
        s.stroke = 0;  // 无描边
        return s;
    };
    skin.close = button(t.colors.error, t.colors.error);
    skin.minimize = button(t.colors.hover, t.colors.primarySoftBg);
    skin.fullscreen = button(t.colors.hover, t.colors.primarySoftBg);
    skin.closeSymbol = rgba32(eui::Color{1.0f, 1.0f, 1.0f, 1.0f});  // hover 时白叉
    skin.unfocused = rgba32(t.colors.textDisabled);
    skin.iconNormal = rgba32(t.colors.textSecondary);  // 常态图标色（Windows 同款）
    // hover 态非关闭钮图标：hover 底是浅灰（亮）/深灰（暗），图标取高对比前景色
    skin.iconHover = rgba32(t.colors.textPrimary);
    return skin;
}

bool titlebarCaptionHit(const CaptionHitQuery& query) {
    // 拖拽区 = 标题条带内、右缘钮热区左侧之外（品牌文本区可拖）。
    // 标题条贴边处的八向 resize 热区在 EUI-NEO 层优先于本回调，此处不管边缘。
    // mac 侧三钮（Windows 同款，need:140 2026-11-06 改口径）占右缘 3×46，
    // Windows 侧两钮占 2×46——按平台分档由 kTitlebarButtonCount 决定。
    if (!customTitlebarActive()) {
        return false;
    }
    if (query.y < 0.0f || query.y >= titlebarHeight()) {
        return false;
    }
    if (query.x < 0.0f || query.x > query.width) {
        return false;
    }
    const float buttonZone = kWindowButtonW * static_cast<float>(titlebarButtonCount());
    return query.x < query.width - buttonZone;
}

// 自绘标题条：左品牌文本、右两钮（关/最小）；不铺底色——半透/磨砂的 clearColor +
// DWM backdrop 直接透到标题条区（D-R3-1 的落点：undecorated 后窗口效果铺满全窗）。
// wechat 风格：灰字灰 hover；关闭钮 hover 红（theme warningRed #FA5151）+ 白叉
namespace internal {
void drawTitlebar(eui::Ui& ui, const eui::Screen& screen) {
    const auto& t = ::core::theme::current();
    const eui::Color kTransparent{0.0f, 0.0f, 0.0f, 0.0f};
    const float h = titlebarHeight();

    // macOS 自绘壳（need:140 路线 B）：三钮由 EUI NSView 覆盖层渲染与交互，色值
    // 经 setCustomTitlebarSkin 注入（主题 token，EUI 层零硬编码色）；DSL 只画品牌
    // 文本，右缘三钮热区（3×46）让位给系统层。
    // 老板 2026-11-06 改口径：视觉从红绿灯彩点改为 **Windows 同款**（右缘连排、
    // 常态透明底、灰线性图标、关闭 hover 红底白叉、最大化走方框图标）；品牌起点
    // 回到 16 与 Windows 构图一致。Windows 分支（下方）零改动（F-09 门禁）。
    // 主题指纹（深浅）变化才注入，主题切换即时生效
    if (customWindowChromeButtonsNative()) {
        static std::optional<bool> injectedDark;
        if (!injectedDark.has_value() || *injectedDark != t.isDark) {
            setCustomTitlebarSkin(titlebarSkinFromTheme(t));
            injectedDark = t.isDark;
        }
        ui.text("app.titlebar.brand")
            .position(16.0f, 0.0f)
            .size(160.0f, h)
            .text("DevDesk")
            .fontSize(t.sizes.fontSizeBody)
            .fontWeight(700)  // 老板 2026-11-18 裁决豁免：品牌字保留 700
            .verticalAlign(eui::VerticalAlign::Center)
            .color(t.colors.textPrimary)
            .build();
        return;
    }


    // 单钮 = 透明 hit rect（hover/pressed 换底色）+ 居中图标 text
    const auto drawWindowButton = [&](const char* idSuffix, float bx,
                                      unsigned int glyph, bool redHover,
                                      const std::function<void()>& action) {
        const std::string id = std::string("app.titlebar.") + idSuffix;
        if (redHover) {
            ui.rect(id + ".hit")
                .position(bx, 0.0f)
                .size(kWindowButtonW, h)
                .states(kTransparent, t.colors.error, t.colors.error)
                .onHover([](bool hovered) { g_closeButtonHovered = hovered; })
                .onClick(action)
                .build();
        } else {
            ui.rect(id + ".hit")
                .position(bx, 0.0f)
                .size(kWindowButtonW, h)
                .states(kTransparent, t.colors.hover, t.colors.primarySoftBg)
                .onClick(action)
                .build();
        }
        ui.text(id + ".icon")
            .position(bx, 0.0f)
            .size(kWindowButtonW, h)
            .icon(eui::utf8(glyph))
            .fontSize(core::theme::scaled(12.0f))
            .horizontalAlign(eui::HorizontalAlign::Center)
            .verticalAlign(eui::VerticalAlign::Center)
            .color(redHover && g_closeButtonHovered
                       ? eui::Color{1.0f, 1.0f, 1.0f, 1.0f}
                       : t.colors.textSecondary)
            .build();
    };

    ui.stack("app.titlebar")
        .position(0.0f, 0.0f)
        .size(screen.width, h)
        .content([&] {
            // 左：品牌文本（UAT4：标题条 20px logo 按用户拍板移除——品牌图形只留
            // 侧栏一处；起点对齐侧栏 padX=16，拖拽热区相应左扩）
            ui.text("app.titlebar.brand")
                .position(16.0f, 0.0f)
                .size(160.0f, h)
                .text("DevDesk")
                .fontSize(t.sizes.fontSizeBody)
                .fontWeight(700)  // 老板 2026-11-18 裁决豁免：品牌字保留 700
                .verticalAlign(eui::VerticalAlign::Center)
                .color(t.colors.textPrimary)
                .build();

            // 右：两钮从右往左 = 关闭 / 最小化（UAT4：最大化钮移除，标题栏
            // 双击保留最大化/还原；关闭走 closeMainWindow = 与系统 X 同链，
            // 托盘模式下隐藏到托盘——EUI-NEO 侧修复）
            drawWindowButton("close", screen.width - kWindowButtonW * 1.0f,
                             0xF00D,  // fa-xmark
                             /*redHover=*/true, [] { closeMainWindow(); });
            drawWindowButton("minimize", screen.width - kWindowButtonW * 2.0f,
                             0xF2D1,  // fa-window-minimize
                             /*redHover=*/false, [] { iconifyMainWindow(); });
        })
        .build();
}
} // namespace internal

} // namespace app
