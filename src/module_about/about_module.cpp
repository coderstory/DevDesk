// src/module_about/about_module.cpp
// 关于页独立模块（2026-09-28 about 页设计 D-01/D-02）。
//
// 纯 compose 页：只读静态信息 + 构建期注入常量，无 onStart/onStop/storage
//（D-01）。版本/构建/平台信息自给（D-02）——编译期宏全部挂在
// devdesk_module_about target（见本目录 CMakeLists.txt），**不 include
// settings_config.h、不依赖任何 module_***（模块隔离铁律）。
//
// 平台/架构不读 #ifdef _WIN32 / __APPLE__ / __x86_64__（项目核心约束 #5），
// 由 CMake 在配置期注入 DEVDESK_PLATFORM / DEVDESK_ARCH，本文件只消费宏。
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

// ---- 编译期信息宏回退（CMake 注入缺失时的手工编译兜底；与
// settings_config.cpp 同款 #ifndef 模式）----
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

// ---- 版本信息行（本模块自给，几行等价逻辑在 settings 侧删旧 About tab 后
// 成死代码，不属本需求范围——D-02 明确不在 settings 内维护第二份）----

// 应用版本行："v0.1.0 · Debug"
std::string aboutVersionLine() {
    return std::string("v") + DEVDESK_VERSION + " · " + DEVDESK_BUILD_TYPE;
}

// 构建时刻行：__DATE__/__TIME__ 为本 TU 编译时刻（C 标准宏直接拼字符串，
// Debug 增量构建后随之刷新）
std::string aboutBuildLine() {
    return std::string("构建 ") + __DATE__ + " " + __TIME__;
}

// Git commit 短 hash（configure 期 git rev-parse --short=7；非 git 环境 = "unknown"）
// 消费点：卡片行「Git commit」（DEVDESK_GIT_REV，主色）与「编译器」
//（DEVDESK_COMPILER）——原 hero 副标题的 commit 行 2026-09-28 删除
//（与卡片逐项重复，用户真机反馈）。

// EUI-NEO rev 行："rev 7164d55 (min 7164d55)"
std::string aboutEuiNeoLine() {
    return std::string("rev ") + DEVDESK_EUI_NEO_REV + " (min " +
           DEVDESK_EUI_NEO_MIN_REV + ")";
}

// 平台行："Windows · x64"（CMake 注入，无平台宏）
std::string aboutPlatformLine() {
    return std::string(DEVDESK_PLATFORM) + " · " + DEVDESK_ARCH;
}

// C++ 标准行：__cplusplus 判档
std::string aboutCppLine() {
    if (__cplusplus >= 202002L) return "C++20";
    if (__cplusplus >= 201703L) return "C++17";
    if (__cplusplus >= 201402L) return "C++14";
    return "C++11";
}

// ---- 信息卡（应用信息 / 依赖与框架）：key-value 行 ----
struct AboutRow {
    const char* label;   // 13px textTertiary 左
    std::string value;   // 13px 右对齐
    bool hash = false;   // true → 短 hash / rev 等标识符用主色（spec §卡片组件）
};

// 卡片底板走 ui_kit::cardFrame（surface 底色 + panelShadow + 圆角 + 1px 描边，
// 与设置页同一套视觉语言，非自绘 rect）。行高 28px（2026-09-29 由 24 上调：
// 卡高 204→244 是「顶部留白太多」的解，见 compose 内 topPad 注释）、行内左
// label 110px 区 + 右侧值右对齐，标题行 28px（icon 18px + 标题 18px/600），
// 上下留白各 24px（= spacingXLarge，与页面其余 padding 同档）。
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

// ---- 数据卡「打开」按钮（outlined+small）：系统文件管理器打开该路径 ----
// 失败用 LOG_ERROR 记录（openDirectory 返回 Result<void>，不抛异常）。
// 数据卡已移除（2026-09-28 布局调整），helper 保留备用（GDP 数据卡如恢复直接
// 复用）；[[maybe_unused]] 防 -Werror=unused-function（GCC 对匿名命名空间未用
// 函数同样告警）。
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

// ---- 动态光晕背景（2026-09-29 方案 A，替掉旧版 7 个 1px 浮环）----
// 需求原话：「又简陋又不明显 几乎看不到」「类似于聚光灯 霓虹灯的效果」。
// 旧版两条根因：① 1px 描边 + 0.18-0.28 不透明度合成后只有个位数 ΔRGB；
// ② 2s 无鼠标移动即休眠 —— 人看页面时几乎总是静止态。
// 现在：多个小光斑（数量 / 直径区间 / 层数见 about_anim.h 的 kBlobCount /
// kDiameterMin-Max / kLayersPerBlob，2026-09-29 二次真机反馈「光团缩小一点，
// 然后多生成几个」后为 6 球 / 240-400 / 3 层）+ 利萨如漂移 +
// 透明度呼吸 ±35% + 14s 一轮的聚光扫过带 + 最外圈 shadow 霓虹边。
// 休眠机制沿用（awake() 单点开关 + 空闲 120s 休眠 + 指针移动唤醒），
// 驱动由 onFrame(60fps 每帧重绘) 换成 onTimer(30fps 节拍)：光晕要一直动，
// 但不是每帧重绘。状态机本体（GlowBlob / AboutAnimState / initGlowLayer）
// 在 about_anim.h —— 独立成头是为了单测能直接驱动（数量 / 直径区间 /
// 呼吸 alpha 区间 / 扫带周期 / 漂移 / 休眠均可断言，回归不再无人守）。

// 主题两色线性插值（theme 运行期可切换，混色在 compose 时现算）
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

        // ========== 背景层：动态光晕（3 光斑 + 聚光扫过，awake 驱动）==========
        // 页面自身不铺底色（主窗 clearColor 透出，磨砂档主体 alpha 恒 1 才可见，
        // 见 home_module 同款注释）；本层仅作透明承载体，z 序最底、无 onClick，
        // 不拦截英雄/卡片。光晕元素在本层之上、内容之下，作为背景装饰。
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
            // 30fps 节拍推进；空闲超时后 compose 不再挂此回调
            // → 运行时 animating 归零 → 零合成零重绘（休眠，非每帧重绘）
            bg.onTimer(AboutAnimState::kFrameSeconds,
                       [&anim] { anim.step(AboutAnimState::kFrameSeconds); });
        }
        bg.build();

        // 光斑绘制：同心圆叠层（半径递增 / alpha 递减）伪造径向渐变 → 柔光球。
        // 颜色只用主题 token：primary 与暖琥珀（warning）按 blob.colorMix 混色，
        // 各球按序号均匀铺满 [0,1]（kBlobCount=6 → 0/0.2/…/1.0）→ 整体各半；
        // 不引入新色。
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
                    // 最外圈霓虹边：shadow 用 primary@0.35 提亮轮廓
                    //（shadow pass 取 shadow 自己的颜色，不影响本层填充 alpha）
                    eui::Color halo = base;
                    halo.a = AboutAnimState::kNeonShadowAlpha;
                    element.shadow(AboutAnimState::kNeonShadowBlur, 0.0f, 0.0f,
                                   halo);
                } else {
                    // 内层柔边：同色 shadow（offset 0）把硬边圆盘化成软边圆盘，
                    // 抹平 6 层离散叠层的同心圆台阶（浅色档最容易看出来）
                    eui::Color halo = base;
                    halo.a = layer.a * AboutAnimState::kLayerSoftAlphaScale;
                    element.shadow(AboutAnimState::kLayerSoftBlur, 0.0f, 0.0f,
                                   halo);
                }
                element.build();
            }
        }
        // 聚光扫过带：两半各 110px 宽、倾斜 18°，alpha 剖面 0 → 0.05 → 0
        //（两半拼出对称梯形，四边都不留硬边；两端圆角 = 半宽 → 半圆帽），
        // 14s 从左扫到右一轮（sweepLeft 是时间的纯函数）
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

        // 居中 900px 主内容区（spec §布局概览；窄窗不滚动，D-04 目标 =
        // 默认窗口内完整可见）。沿革：720（初版）→ 1040（2026-09-28「内容太窄」）
        // → 900（2026-09-29「两卡宽度缩一点」）。页面宽 = 窗口宽 − 侧边栏
        //（展开 208，见 app_sidebar.cpp pageContentWidth）；最小窗宽 1330 →
        // 页面宽 1122，居中后左右各余 111px，不溢出（页面宽只依赖最小窗**宽**；
        // 最小窗高由 core 层按工作区推导，core::window_layout，本机 805）
        constexpr float kAboutContentW = 900.0f;
        const float x = std::max(0.0f, (screen.width - kAboutContentW) / 2.0f);
        constexpr float cardGap = 8.0f;
        // 单张卡高（两卡同构：上留白 24 + 标题行 28 + 6 行 × 28 + 下留白 24 = 244）。
        // 2026-09-29「顶部留白太多」的落地口径见下方 topPad 注释：**靠把卡加高**
        //（行高 24→28、上下留白 16→24）让内容块变高、贴底后顶部自然少空一片——
        // 而不是把内容块往上抬（那样就不再是「贴底」了）。
        // 提到内容块起点之前算：贴底起点要按内容块总高反推（见下）
        constexpr float cardH = 24.0f + 28.0f + 6.0f * 28.0f + 24.0f;
        // hero 图标 → 标题的 8 与标题行高 40：沿用既有字面量（非间距语义，
        // 不取自 spacing token），提为常量只为把内容块总高写成一项可复核的算式
        constexpr float heroGap = 8.0f;
        constexpr float heroTitleH = 40.0f;
        constexpr float footerH = 20.0f;

        // ========== hero 段（居中；图标 + 标题两件）==========
        // topPad 现在的**唯一**作用是「极矮窗兜底起点」（下方 y 的 max 护栏）：
        // 2026-09-29 起内容块贴底（底边 = 页脚顶 − 一档间距），正常窗口下起笔点是
        // 反推值、恒大于 topPad——本机页面高 801 时反推 93、最小窗（页面高 805）时
        // 反推 97，都远在 topPad 之上，故**减小 topPad 对正常窗口观感零影响**
        //（如实说明：用户原话「适当减小 topPad」这条对本机窗口不产生像素变化）。
        // 顶部那片空白真正的解法是「把内容块变高」：卡高 204→244（见上），内容块
        // 544→624 → 贴底起点 173→93，顶部空白从 21.6% 降到 11.6%，底部 24px 的贴底
        // 关系不变（真机截图与像素对账见交付说明）。
        // 取值仍用一档 XXLarge(40)（原为 XXLarge + XLarge 叠加 64，那是「hero 顶部
        // 留白」时代的语义，现已不承担观感）：兜底场景本就是内容放不进页面的退化
        // 形态，取与页面底 padding 同档的值，不再叠加。
        const float topPad = t.sizes.spacingXXLarge;
        // hero 图标 48→56（= XXLarge 40 + Large 16，同属 token 叠加）：用户要
        // 「hero 更有存在感」，但字号受规范约束（页面标题 32 / 正文 15，见
        // CLAUDE.md「视觉规范」）不可动——空间感由加宽 + 顶部留白承担；图标是
        // 装饰尺寸、不在字号规范内，故适度上调一档（56 = 1.75× 页面标题）。
        // 不再往上（64 = 2×）的理由：图标盒高 = 字号，em 盒内上下余量更小，
        // 而本机 screencapture 被 TCC 拦、无截图手段验证字形不裁切
        const float heroIcon = t.sizes.spacingXXLarge + t.sizes.spacingLarge;
        // ========== 内容块贴底对齐（2026-09-29）==========
        // 用户原话（2026-09-28）：「关于页面的 2 个卡片缩在水平中间和顶部对齐，
        // 不大气，2026 devdesk 要放底部」——上下堆叠已落地，贴底这一条此前漏做
        //（真机截图：内容起于 topPad=64、卡底 608，页脚 745，下方一片空白）。
        // 口径：hero + 两卡的**内容块整体**贴向页脚上方（现代 about 页常见形态），
        // 页脚仍贴页面底（见文末 footerY 的 max 护栏）。
        // 起点反推：内容块底边 = 页脚顶 − 一档间距（XLarge），故
        //   y = 页脚顶 − 间距 − 内容块总高
        // 内容块总高 = hero 图标 56 + 图标/标题间距 8 + 标题 40 + 标题到卡一档
        // XLarge 24 + 卡1 244 + 卡距 8 + 卡2 244 = 624（卡高 204→244 见上；
        // 本机页面高 801：页脚顶 741 → 起点 93、卡底 717，距页脚 24px）。
        // 护栏 max(topPad, …)：本页无滚动，极矮窗下内容块放不进「页脚上方」这块
        // 高度时退回原来的顶部对齐（此时页脚侧另有 max 护栏，见下，保证不重叠）
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
        // 原 hero 副标题三行（about.hero.sub / .build / .commit：版本行 /
        // 构建时刻 / commit+编译器）与下方「应用信息」卡逐项重复（用户真机
        // 反馈「卡片里已经有版本号之类的东西」）→ 2026-09-28 删除；标题到
        // 卡片用一档 XLarge(24) 间距（原三行文本流共 78px 的替代，标题与
        // 卡片的分组关系由间距本身表达）
        y += heroTitleH + t.sizes.spacingXLarge;

        // ========== 卡 1 + 卡 2：上下堆叠（2026-09-28 用户真机反馈；原左右并排）==========
        // 每张卡占满内容区宽 900（原并排宽 (1040−8)/2 = 516）；两卡各 6 行、
        // 等高 cardH=244（24 + 28 + 6×28 + 24），堆叠总高 244×2 + 卡距 8 = 496。
        // 起始 y 由 contentH 反推（贴底对齐，见上）——不再是固定的顶部起点；
        // 页面高 801 → 页脚顶 741、卡底 717（最小窗页面高 805 档，core 层按
        // 工作区推导，core::window_layout），页面更高只会更宽松
        const float cardW = kAboutContentW;

        // 卡 1：应用信息（上）
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

        // 卡 2：依赖与框架（下，y = y + cardH + cardGap）
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

        // ========== 页脚（贴页面底部）==========
        // 2026-09-28 用户真机反馈：footer 不再跟卡底流式排（原来紧跟卡底，页面
        // 下方一大片空），改贴页面底部——卡片与页脚之间的空白即呼吸区（2026-09-29
        // 起被内容块贴底对齐占掉固定一档 XLarge，剩余空白仍随窗口高度伸缩）。
        // 底边留白 = 页面 padding token（spacingXXLarge 40，与设置页 pad 同口径）。
        // max() 护栏：本页无滚动，极矮窗下（内容块被 clamp 回 topPad、卡底越过
        // 页脚顶）footer 不回退到与卡片重叠
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